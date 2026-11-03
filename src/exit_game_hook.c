#include "exit_game_hook.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "exit_game_text.h"

enum {
    SHEET_DATA_OFFSET = 0xf8,
    SHEET_SIZE_OFFSET = 4,
    SHEET_HEADER = 0x20,
    WIDGET_SIZE = 0x5c0,
    WIDGET_ROW_WIDTH_OFFSET = 0x350,
    WIDGET_ROW_HEIGHT_OFFSET = 0x354,
    WIDGET_SCALE_OFFSET = 0x320,
    WIDGET_ROW_WIDTH = 0x500,
    WIDGET_ROW_HEIGHT = 0x32,
    TITLE_MENU_OFFSET = 0x1350,
    TITLE_CURSOR_OFFSET = 0x1ca8,
    SELECTION_FRESH_MS = 300,
    MENU_ITEM_LIST_OFFSET = 0xc70,
    MENU_WIDGETS_OFFSET = 0x68,
    MENU_ITEM_SIZE = 0x30,
    HELP_LIMIT_OLD = 5,
    HELP_LIMIT_NEW = 6
};
#define SHEET_FILL_LENGTH 10
#define TICK_LENGTH 6

static const uint8_t kSheetFillBytes[SHEET_FILL_LENGTH] = {0x48, 0x8b, 0x87, 0xf8, 0x00, 0x00, 0x00, 0x8a, 0x48, 0x18};
static const uint8_t kTickBytes[TICK_LENGTH] = {0x83, 0xc0, 0x02, 0x41, 0xb4, 0x01};

/* Layout part per menu position and animation phase (entries 3..37 of the table; the first three are the title's own).
 * The PS4 variants lay out five rows; the base variants, two part indexes lower, lay out six and have no store panel. */
#define PART_TABLE_FIRST 3
#define PART_TABLE_LAST 37
#define PART_TABLE_VARIANT_DELTA 2
static const int32_t kPartTable[PART_TABLE_LAST + 1 - PART_TABLE_FIRST] = {
    15, 43, 71, 99, 136, 168, 18, 46, 74, 102, 139, 171, 24, 30, 36, 27, 33, 39, 52, 58, 64, 55, 61, 67, 80, 86, 92, 83, 89, 95,
    21, 49, 77, 105, 142};

static uint64_t default_clock_ms(void);
static ExitGameHandler handler;
static ExitGameClock now_ms = default_clock_ms;
static uint64_t selected_at;
static ExitGameNewFn new_widget;
static ExitGameCtorFn construct_widget;
static ExitGameAddFn add_widget;
static int requested;
static int text_logged;
static int row_logged;
static int debug_row_logged;    /* DEBUG_SAULO */
static uint64_t title_menu;

void exit_game_set_handler(ExitGameHandler value) {
    handler = value;
}

void exit_game_set_row_functions(ExitGameNewFn new_fn, ExitGameCtorFn ctor, ExitGameAddFn add) {
    new_widget = new_fn;
    construct_widget = ctor;
    add_widget = add;
}

void exit_game_reset(void) {
    row_logged = 0;
    debug_row_logged = 0;
    title_menu = 0;
    handler = NULL;
    now_ms = default_clock_ms;
    selected_at = 0;
    new_widget = NULL;
    construct_widget = NULL;
    add_widget = NULL;
    requested = 0;
    text_logged = 0;
}

int exit_game_requested(void) {
    return __atomic_load_n(&requested, __ATOMIC_ACQUIRE);
}

/* Entry of the sheet loader's string-table fill (rdi = the sheet object, its data at +0xf8): the loaded SystemMsg is
 * rewritten before the fill walks it. */
void GUEST_HOOK_ABI exit_game_sheet_hook(GuestRegs *regs) {
    uint8_t *data;
    if (!regs->rdi) return;
    memcpy(&data, (const void *)(uintptr_t)(regs->rdi + SHEET_DATA_OFFSET), sizeof(data));
    if (!data) return;
    uint32_t length;
    memcpy(&length, data + SHEET_SIZE_OFFSET, sizeof(length));
    if (memcmp(data, "FMSB", 4)) return;
    const ExitGameLanguage *language = NULL;
    const ExitGameTextStatus status = exit_game_text_patch(data, (size_t)length + SHEET_HEADER + 4, &language);
    if (status == EXIT_GAME_TEXT_PATCHED && !text_logged) {
        text_logged = 1;
        printf("Exit Game: title text installed (%s)\n", language->language);
    } else if ((status == EXIT_GAME_TEXT_UNKNOWN || status == EXIT_GAME_TEXT_NO_ROOM) && !text_logged) {
        text_logged = 1;
        printf("Exit Game: title text left as is, %s\n", exit_game_text_status_text(status));
    }
}

/* At the call that cuts the main menu's item list to five (rdi = the item list, rsi = 5; r12 = the menu, r13 and r15 the
 * arguments the five row widgets were built with): builds a sixth row widget the same way and keeps six items. */
typedef struct {
    uint64_t items, widgets;
    int context, list, scale_ok;
    float scale_x, scale_y;
} RowState;

static uint64_t read_u64(const uint8_t *at) {
    uint64_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

/* The title menu at the item-list resize: six items added, five row widgets, the list object inside the menu, and the
 * row scale in r15 sane (its value differs by region). */
static RowState row_state(const GuestRegs *regs) {
    RowState state = {0};
    const uint8_t *menu = (const uint8_t *)(uintptr_t)regs->r12;
    state.context = regs->r12 && regs->r13;
    state.list = state.context && regs->rdi == regs->r12 + MENU_ITEM_LIST_OFFSET;
    if (state.list) {
        const uint64_t items_begin = read_u64(menu + MENU_ITEM_LIST_OFFSET + 8), items_end = read_u64(menu + MENU_ITEM_LIST_OFFSET + 16);
        const uint64_t widgets_begin = read_u64(menu + MENU_WIDGETS_OFFSET), widgets_end = read_u64(menu + MENU_WIDGETS_OFFSET + 8);
        if (items_end >= items_begin && widgets_end >= widgets_begin) {
            state.items = (items_end - items_begin) / MENU_ITEM_SIZE;
            state.widgets = (widgets_end - widgets_begin) / sizeof(uint64_t);
        }
    }
    const uint32_t low = (uint32_t)regs->r15, high = (uint32_t)(regs->r15 >> 32);
    memcpy(&state.scale_x, &low, sizeof(float));
    memcpy(&state.scale_y, &high, sizeof(float));
    state.scale_ok = low == high && state.scale_x > 0.1f && state.scale_x <= 4.0f;
    return state;
}

static const char *row_refusal(const GuestRegs *regs, const RowState *state, char *buffer, size_t size) {
    if (regs->rsi != EXIT_GAME_OLD_ROW_COUNT) return "item list is not being sized to 5";
    if (!state->context) return "title menu context mismatch";
    if (!state->list) return "invalid list pointer";
    if (state->items != EXIT_GAME_MENU_ITEMS) {
        snprintf(buffer, size, "item count %llu expected %d", (unsigned long long)state->items, EXIT_GAME_MENU_ITEMS);
        return buffer;
    }
    if (state->widgets != EXIT_GAME_OLD_ROW_COUNT) {
        snprintf(buffer, size, "widget count %llu expected %d", (unsigned long long)state->widgets, EXIT_GAME_OLD_ROW_COUNT);
        return buffer;
    }
    if (!state->scale_ok) return "scale mismatch";
    if (!new_widget || !construct_widget || !add_widget) return "row functions unavailable";
    return NULL;
}

void GUEST_HOOK_ABI exit_game_row_hook(GuestRegs *regs) {
    const RowState state = row_state(regs);
    if (state.context) title_menu = regs->r12;
    char detail[64];
    const char *refusal = row_refusal(regs, &state, detail, sizeof(detail));
    if (!debug_row_logged++) /* DEBUG_SAULO */
        printf("DEBUG_SAULO exit game row: items %llu widgets %llu context %d list %d scale %.2f,%.2f\n",
               (unsigned long long)state.items, (unsigned long long)state.widgets, state.context, state.list,
               (double)state.scale_x, (double)state.scale_y);
    if (refusal) {
        if (!row_logged++) printf("Exit Game: sixth row not added, %s\n", refusal);
        return;
    }
    void *widget = new_widget(WIDGET_SIZE);
    if (!widget) {
        if (!row_logged++) printf("Exit Game: sixth row not added, no widget memory\n");
        return;
    }
    construct_widget(widget, (void *)(uintptr_t)regs->r13);
    const uint32_t width = WIDGET_ROW_WIDTH, height = WIDGET_ROW_HEIGHT;
    const uint64_t scale = regs->r15;
    memcpy((uint8_t *)widget + WIDGET_ROW_WIDTH_OFFSET, &width, sizeof(width));
    memcpy((uint8_t *)widget + WIDGET_ROW_HEIGHT_OFFSET, &height, sizeof(height));
    memcpy((uint8_t *)widget + WIDGET_SCALE_OFFSET, &scale, sizeof(scale));
    if ((add_widget((void *)(uintptr_t)regs->r12, widget) & 0xff) != 1) {
        if (!row_logged++) printf("Exit Game: sixth row not added, the menu refused the widget\n");
        return;
    }
    regs->rsi = EXIT_GAME_MENU_ITEMS;
    if (!row_logged++) printf("Exit Game: sixth row enabled\n");
}

static void menu_counts(const uint8_t *menu, uint64_t *items, uint64_t *widgets) {
    const uint64_t items_begin = read_u64(menu + MENU_ITEM_LIST_OFFSET + 8), items_end = read_u64(menu + MENU_ITEM_LIST_OFFSET + 16);
    const uint64_t widgets_begin = read_u64(menu + MENU_WIDGETS_OFFSET), widgets_end = read_u64(menu + MENU_WIDGETS_OFFSET + 8);
    *items = items_end >= items_begin ? (items_end - items_begin) / MENU_ITEM_SIZE : 0;
    *widgets = widgets_end >= widgets_begin ? (widgets_end - widgets_begin) / sizeof(uint64_t) : 0;
}

static uint64_t default_clock_ms(void) {
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

void exit_game_set_clock(ExitGameClock clock) {
    now_ms = clock ? clock : default_clock_ms;
}

/* Only the title menu's tick refreshes the selection, so a menu that is gone stops being selected by itself. */
void exit_game_set_selected(int selected) {
    __atomic_store_n(&selected_at, selected ? now_ms() + 1 : 0, __ATOMIC_RELEASE);
}

int exit_game_selected(void) {
    const uint64_t at = __atomic_load_n(&selected_at, __ATOMIC_ACQUIRE);
    if (!at) return 0;
    const uint64_t now = now_ms() + 1;
    return now >= at && now - at <= SELECTION_FRESH_MS;
}

int exit_game_invoke_handler(void) {
    if (!handler || __atomic_exchange_n(&requested, 1, __ATOMIC_ACQ_REL)) return 0;
    handler();
    return 1;
}

/* The host's accept press (controller South or Enter) while the sixth row is selected: it requests the exit and stays
 * hidden from the guest until released. Returns 1 while the press must be hidden. */
int exit_game_filter_accept(int accept_down) {
    static int previous_down, hidden;
    const int pressed = accept_down && !previous_down;
    previous_down = accept_down;
    if (pressed && handler && exit_game_selected()) {
        hidden = 1;
        printf("Exit Game: activation detected\n");
        exit_game_invoke_handler();
    }
    if (!accept_down) hidden = 0;
    return hidden;
}

/* After the title's read of the menu result (r13 = the title scene, whose menu is at +0x1350): publishes whether the
 * sixth row is the selected one. */
void GUEST_HOOK_ABI exit_game_tick_hook(GuestRegs *regs) {
    const uint8_t *scene = (const uint8_t *)(uintptr_t)regs->r13;
    int selected = 0;
    if (scene && regs->r13 + TITLE_MENU_OFFSET == title_menu) {
        uint64_t items, widgets;
        int32_t cursor;
        menu_counts(scene + TITLE_MENU_OFFSET, &items, &widgets);
        memcpy(&cursor, scene + TITLE_CURSOR_OFFSET, sizeof(cursor));
        selected = cursor == EXIT_GAME_ROW_INDEX && items == EXIT_GAME_MENU_ITEMS && widgets == EXIT_GAME_MENU_ITEMS;
    }
    if (selected && !exit_game_selected()) printf("DEBUG_SAULO Exit Game selected\n");
    exit_game_set_selected(selected);
}

size_t exit_game_hook_stub_bytes(void) {
    return EXIT_GAME_HOOK_COUNT * guest_hook_stub_size();
}

static int call_target(const uint8_t *image, uint64_t size, uint64_t at, uint64_t *target) {
    if (at + 5 > size || image[at] != 0xe8) return 0;
    int32_t displacement;
    memcpy(&displacement, image + at + 1, sizeof(displacement));
    const int64_t destination = (int64_t)at + 5 + displacement;
    if (destination < 0 || (uint64_t)destination >= size) return 0;
    *target = (uint64_t)destination;
    return 1;
}

static int help_limit_matches(const uint8_t *image, uint64_t size, uint32_t at) {
    return at >= 2 && at < size && image[at - 2] == 0x83 && image[at - 1] == 0xf8 && image[at] == HELP_LIMIT_OLD;
}

static int32_t *part_table_of(uint8_t *image, uint64_t size, uint32_t load) {
    static const uint8_t lea[3] = {0x48, 0x8d, 0x15};
    if ((uint64_t)load + 7 > size || memcmp(image + load, lea, sizeof(lea))) return NULL;
    int32_t displacement;
    memcpy(&displacement, image + load + 3, sizeof(displacement));
    const int64_t table = (int64_t)load + 7 + displacement;
    if (table < 0 || (uint64_t)table + (PART_TABLE_LAST + 1) * sizeof(int32_t) > size || table % sizeof(int32_t)) return NULL;
    int32_t *entries = (int32_t *)(void *)(image + table);
    for (int i = PART_TABLE_FIRST; i <= PART_TABLE_LAST; ++i)
        if (entries[i] != kPartTable[i - PART_TABLE_FIRST]) return NULL;
    return entries;
}

int exit_game_hook_install_located(uint8_t *image, uint64_t size, uint64_t entry, const DcpExitGameSites *sites,
                                   uint8_t *stubs, size_t stubs_size) {
    if (!sites || !image) {
        printf("Exit Game: not installed, no sites\n");
        return 0;
    }
    const uint64_t widget_call = sites->row_widget;
    uint64_t new_target, ctor_target, add_target, item_target;
    if (widget_call < EXIT_GAME_NEW_CALL_BACK ||
        !call_target(image, size, widget_call - EXIT_GAME_NEW_CALL_BACK, &new_target) ||
        !call_target(image, size, widget_call - EXIT_GAME_CTOR_CALL_BACK, &ctor_target) ||
        !call_target(image, size, widget_call - EXIT_GAME_ADD_CALL_BACK, &add_target) ||
        !call_target(image, size, widget_call, &item_target)) {
        printf("Exit Game: not installed, the row widget calls differ\n");
        return 0;
    }
    if (!help_limit_matches(image, size, sites->help_limits[0]) || !help_limit_matches(image, size, sites->help_limits[1]) ||
        sites->help_limits[0] == sites->help_limits[1]) {
        printf("Exit Game: not installed, the help text limits differ\n");
        return 0;
    }
    int32_t *part_table = part_table_of(image, size, sites->part_table_loads[0]);
    if (!part_table || part_table != part_table_of(image, size, sites->part_table_loads[1])) {
        printf("Exit Game: not installed, the layout part table differs\n");
        return 0;
    }
    GuestHook hooks[EXIT_GAME_HOOK_COUNT] = {
        {"exit game sheet", sites->sheet_fill, SHEET_FILL_LENGTH, {0}, exit_game_sheet_hook},
        {"exit game row", sites->row_widget, 5, {0}, exit_game_row_hook},
        {"exit game tick", sites->menu_tick, TICK_LENGTH, {0}, exit_game_tick_hook},
    };
    memcpy(hooks[0].original, kSheetFillBytes, SHEET_FILL_LENGTH);
    if (widget_call + 5 > size) return 0;
    memcpy(hooks[1].original, image + widget_call, 5);
    memcpy(hooks[2].original, kTickBytes, TICK_LENGTH);
    const GuestImageId id = {size, entry};
    size_t bad = 0;
    const GuestHookStatus status = guest_hooks_install(image, size, entry, &id, hooks, EXIT_GAME_HOOK_COUNT, stubs, stubs_size, &bad);
    if (status != GUEST_HOOK_OK) {
        printf("Exit Game: not installed: %s (hook %zu: %s)\n", guest_hook_status_text(status), bad, hooks[bad].name);
        return 0;
    }
    for (int i = PART_TABLE_FIRST; i <= PART_TABLE_LAST; ++i) part_table[i] -= PART_TABLE_VARIANT_DELTA;
    image[sites->help_limits[0]] = HELP_LIMIT_NEW;
    image[sites->help_limits[1]] = HELP_LIMIT_NEW;
    exit_game_set_row_functions((ExitGameNewFn)(uintptr_t)(image + new_target), (ExitGameCtorFn)(uintptr_t)(image + ctor_target),
                                (ExitGameAddFn)(uintptr_t)(image + add_target));
    printf("Exit Game: sites installed (sheet 0x%x, row 0x%x, tick 0x%x)\n", sites->sheet_fill, sites->row_widget,
           sites->menu_tick);
    printf("Exit Game: layout table patched %d entries (table 0x%llx)\n", PART_TABLE_LAST + 1 - PART_TABLE_FIRST,
           (unsigned long long)((const uint8_t *)part_table - image));
    return 1;
}
