#include "exit_game_hook.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "exit_game_text.h"
#include "title_layout.h"

enum {
    SHEET_DATA_OFFSET = 0xf8,
    SHEET_SIZE_OFFSET = 4,
    SHEET_HEADER = 0x20,
    TITLE_MENU_OFFSET = 0x1350,
    TITLE_CURSOR_OFFSET = 0x1ca8,
    SELECTION_FRESH_MS = 300,
    MENU_ITEM_LIST_OFFSET = 0xc70,
    MENU_WIDGETS_OFFSET = 0x68,
    MENU_ITEM_SIZE = 0x30,
    HELP_LIMIT_OLD = 5,
    HELP_LIMIT_NEW = 6,
    HELP_LEA_BACK = 8,
    PLAYER_RESOURCE = 0xc0,
    PLAYER_SECOND_RESOURCE = 0xc8,
    RESOURCE_LAYOUT = 0x158
};
#define SHEET_FILL_LENGTH 10
#define TICK_LENGTH 6
#define HELP_LEA_LENGTH 6
#define PART_SETTER_LENGTH 8

static const uint8_t kSheetFillBytes[SHEET_FILL_LENGTH] = {0x48, 0x8b, 0x87, 0xf8, 0x00, 0x00, 0x00, 0x8a, 0x48, 0x18};
static const uint8_t kTickBytes[TICK_LENGTH] = {0x83, 0xc0, 0x02, 0x41, 0xb4, 0x01};
static const uint8_t kHelpLeaBytes[HELP_LEA_LENGTH] = {0x8d, 0x88, 0x85, 0x01, 0x00, 0x00};
static const uint8_t kPartSetterBytes[PART_SETTER_LENGTH] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56};

/* The cut call's neighbourhood: the fifth row widget's construction (64 bytes), the six item additions (15 bytes each,
 * `mov esi, SystemMsg id` first) and the `mov esi, 5` that sizes the list. */
enum { WIDGET_BLOCK_SIZE = 64, ITEM_STRIDE = 15, ITEM_COUNT = EXIT_GAME_LIST_ITEMS, CUT_IMMEDIATE_BACK = 4 };
static const uint8_t kWidgetBlockHead[6] = {0xbf, 0xc0, 0x05, 0x00, 0x00, 0xe8};
static const uint8_t kItemTail[6] = {0x31, 0xd2, 0x4c, 0x89, 0xe7, 0xe8};
static const uint8_t kItemIds[ITEM_COUNT] = {0x7e, 0x7f, 0x80, 0x81, 0x82, 0x84}; /* Start, Upload, Download, Options, Gallery, Store */
static const uint8_t kRowItemIds[ITEM_COUNT] = {0x7e, 0x81, 0x82, 0x84, 0x7f, 0x80}; /* the four rows, then the two cut */

/* The menu result's jump table follows the tick: `add eax, 2; mov r12b, 1; cmp eax, 6; ja default; lea rcx, [table]`.
 * Entry r + 2 sets the title's state machine to state r + 2. */
enum { TICK_JA = 9, TICK_LEA = 15, TICK_JUMP = 22, TABLE_ENTRIES = 7, CASE_LENGTH = 13 };
static const uint8_t kTickTail[3] = {0x83, 0xf8, 0x06};
static const uint8_t kTickJump[9] = {0x48, 0x63, 0x04, 0x81, 0x48, 0x01, 0xc8, 0xff, 0xe0};
static const uint8_t kTickLea[3] = {0x48, 0x8d, 0x0d};

/* The menu position of each row, as the game numbers its actions (also the order of its help text). */
static const uint8_t kRowAction[EXIT_GAME_MENU_ITEMS] = {0, 3, 4, 5};

/* Layout part per menu position and animation phase (entries 3..37 of the table; the first three are the title's own).
 * The PS4 variants lay out five rows; the base variants, two part indexes lower, lay out six and have no store panel.
 * The title also selects its entrance, confirm and return parts by immediate index: the same shift applies. */
#define PART_TABLE_FIRST 3
#define PART_TABLE_LAST 37
#define PART_TABLE_VARIANT_DELTA 2
static const int32_t kPartTable[PART_TABLE_LAST + 1 - PART_TABLE_FIRST] = {
    15, 43, 71, 99, 136, 168, 18, 46, 74, 102, 139, 171, 24, 30, 36, 27, 33, 39, 52, 58, 64, 55, 61, 67, 80, 86, 92, 83, 89, 95,
    21, 49, 77, 105, 142};

static const uint8_t kTransitionParts[DCP_TRANSITION_PART_COUNT] = {11, 18, 21, 102, 105, 139, 142};

static uint64_t default_clock_ms(void);
static ExitGameHandler handler;
static ExitGameClock now_ms = default_clock_ms;
static uint64_t selected_at;
static int requested;
static int text_logged;
static int row_logged;
static int layout_logged;
static int menu_logged; /* DEBUG_SAULO */
static uint64_t title_menu;

void exit_game_set_handler(ExitGameHandler value) {
    handler = value;
}

void exit_game_reset(void) {
    row_logged = 0;
    layout_logged = 0;
    menu_logged = 0;
    title_menu = 0;
    handler = NULL;
    now_ms = default_clock_ms;
    selected_at = 0;
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

static uint64_t read_u64(const uint8_t *at) {
    uint64_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

static void menu_counts(const uint8_t *menu, uint64_t *items, uint64_t *widgets) {
    const uint64_t items_begin = read_u64(menu + MENU_ITEM_LIST_OFFSET + 8), items_end = read_u64(menu + MENU_ITEM_LIST_OFFSET + 16);
    const uint64_t widgets_begin = read_u64(menu + MENU_WIDGETS_OFFSET), widgets_end = read_u64(menu + MENU_WIDGETS_OFFSET + 8);
    *items = items_end >= items_begin ? (items_end - items_begin) / MENU_ITEM_SIZE : 0;
    *widgets = widgets_end >= widgets_begin ? (widgets_end - widgets_begin) / sizeof(uint64_t) : 0;
}

/* At the call that cuts the main menu's item list (rdi = the item list, rsi = the new length; r12 = the menu): the six
 * items are added and the four row widgets are built, so this is the title's own menu. */
void GUEST_HOOK_ABI exit_game_row_hook(GuestRegs *regs) {
    const uint8_t *menu = (const uint8_t *)(uintptr_t)regs->r12;
    uint64_t items = 0, widgets = 0;
    const int list = menu && regs->r13 && regs->rdi == regs->r12 + MENU_ITEM_LIST_OFFSET;
    if (list) menu_counts(menu, &items, &widgets);
    if (list && regs->rsi == EXIT_GAME_MENU_ITEMS && items == EXIT_GAME_LIST_ITEMS && widgets == EXIT_GAME_MENU_WIDGETS) {
        title_menu = regs->r12;
        return;
    }
    if (!row_logged++)
        printf("Exit Game: title menu not recognised (items %llu widgets %llu cut %llu)\n", (unsigned long long)items,
               (unsigned long long)widgets, (unsigned long long)regs->rsi);
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

/* The host's accept press (controller South or Enter) while the Exit Game row is selected: it requests the exit and
 * stays hidden from the guest until released. Returns 1 while the press must be hidden. */
static int accept_east;

void exit_game_set_accept_east(int east) {
    accept_east = east != 0;
}

int exit_game_accept_east(void) {
    return accept_east;
}

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
 * Exit Game row is the selected one. */
void GUEST_HOOK_ABI exit_game_tick_hook(GuestRegs *regs) {
    const uint8_t *scene = (const uint8_t *)(uintptr_t)regs->r13;
    int selected = 0;
    if (scene && regs->r13 + TITLE_MENU_OFFSET == title_menu) {
        uint64_t items, widgets;
        int32_t cursor;
        menu_counts(scene + TITLE_MENU_OFFSET, &items, &widgets);
        memcpy(&cursor, scene + TITLE_CURSOR_OFFSET, sizeof(cursor));
        selected = cursor == EXIT_GAME_ROW_INDEX && items == EXIT_GAME_MENU_ITEMS && widgets == EXIT_GAME_MENU_WIDGETS;
        if (!menu_logged++) /* DEBUG_SAULO */
            printf("DEBUG_SAULO title menu: items %llu widgets %llu\n", (unsigned long long)items, (unsigned long long)widgets);
    }
    if (selected && !exit_game_selected()) printf("DEBUG_SAULO Exit Game selected\n");
    exit_game_set_selected(selected);
}

/* Before the help text id is computed from the menu position (rax = the position): the id follows the row's action. */
void GUEST_HOOK_ABI exit_game_help_hook(GuestRegs *regs) {
    regs->rax = regs->rax < EXIT_GAME_MENU_ITEMS ? kRowAction[regs->rax] : 0xffffffffu;
}

static const uint8_t *read_pointer(const uint8_t *at) {
    uint64_t value;
    memcpy(&value, at, sizeof(value));
    return (const uint8_t *)(uintptr_t)value;
}

/* Entry of the layout part setter (rdi = the layout player): the title's layouts are patched before their first part is
 * set. */
void GUEST_HOOK_ABI exit_game_layout_hook(GuestRegs *regs) {
    if (!regs->rdi) return;
    const uint8_t *player = (const uint8_t *)(uintptr_t)regs->rdi;
    static const int kResources[2] = {PLAYER_RESOURCE, PLAYER_SECOND_RESOURCE};
    for (int i = 0; i < 2; ++i) {
        const uint8_t *resource = read_pointer(player + kResources[i]);
        if (!resource) continue;
        const TitleLayoutStatus status = title_layout_patch((uint8_t *)(uintptr_t)read_pointer(resource + RESOURCE_LAYOUT));
        if (status == TITLE_LAYOUT_PATCHED && !layout_logged) {
            layout_logged = 1;
            printf("Exit Game: title layout patched to four rows\n");
        } else if (status == TITLE_LAYOUT_UNSUPPORTED && !layout_logged) {
            layout_logged = 1;
            printf("Exit Game: title layout differs from the known one, its art is left as is\n");
        }
    }
}

size_t exit_game_hook_stub_bytes(void) {
    return EXIT_GAME_HOOK_COUNT * guest_hook_stub_size();
}

static int help_limit_matches(const uint8_t *image, uint64_t size, uint32_t at) {
    return at >= HELP_LEA_BACK && at < size && image[at - 2] == 0x83 && image[at - 1] == 0xf8 && image[at] == HELP_LIMIT_OLD &&
           !memcmp(image + at - HELP_LEA_BACK, kHelpLeaBytes, HELP_LEA_LENGTH);
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

static int transition_parts_match(const uint8_t *image, uint64_t size, const DcpExitGameSites *sites) {
    for (int i = 0; i < DCP_TRANSITION_PART_COUNT; ++i) {
        const uint32_t at = sites->transition_parts[i];
        if (at + 4ull > size || image[at] != kTransitionParts[i] || image[at + 1] || image[at + 2] || image[at + 3]) return 0;
        for (int j = 0; j < i; ++j)
            if (sites->transition_parts[j] == at) return 0;
    }
    return 1;
}

/* The row construction, the six item additions and the cut around the call at `cut`. */
static int row_region_matches(const uint8_t *image, uint64_t size, uint64_t cut) {
    if (cut < EXIT_GAME_WIDGET_BLOCK_BACK || cut + 5 > size || image[cut] != 0xe8) return 0;
    const uint8_t *block = image + cut - EXIT_GAME_WIDGET_BLOCK_BACK;
    if (memcmp(block, kWidgetBlockHead, sizeof(kWidgetBlockHead)) || block[WIDGET_BLOCK_SIZE - 5] != 0xe8) return 0;
    for (int i = 0; i < ITEM_COUNT; ++i) {
        const uint8_t *item = block + WIDGET_BLOCK_SIZE + i * ITEM_STRIDE;
        if (item[0] != 0xbe || item[1] != kItemIds[i] || item[2] != 0x01 || item[3] || item[4] ||
            memcmp(item + 5, kItemTail, sizeof(kItemTail)))
            return 0;
    }
    const uint8_t *immediate = image + cut - CUT_IMMEDIATE_BACK;
    return immediate[-1] == 0xbe && immediate[0] == HELP_LIMIT_OLD && !immediate[1] && !immediate[2] && !immediate[3];
}

/* The menu result's jump table, its entries 3 and 4 (Options, Gallery) being the cases to keep. */
static int32_t *jump_table_of(uint8_t *image, uint64_t size, uint32_t tick) {
    if ((uint64_t)tick + TICK_JUMP + sizeof(kTickJump) > size || memcmp(image + tick, kTickBytes, TICK_LENGTH) ||
        memcmp(image + tick + TICK_LENGTH, kTickTail, sizeof(kTickTail)) || image[tick + TICK_JA] != 0x0f ||
        image[tick + TICK_JA + 1] != 0x87 || memcmp(image + tick + TICK_LEA, kTickLea, sizeof(kTickLea)) ||
        memcmp(image + tick + TICK_JUMP, kTickJump, sizeof(kTickJump)))
        return NULL;
    int32_t to_default, to_table;
    memcpy(&to_default, image + tick + TICK_JA + 2, sizeof(to_default));
    memcpy(&to_table, image + tick + TICK_LEA + 3, sizeof(to_table));
    const int64_t table = (int64_t)tick + TICK_JUMP + to_table, fallback = (int64_t)tick + TICK_JA + 6 + to_default;
    if (table < 0 || table % sizeof(int32_t) || (uint64_t)table + TABLE_ENTRIES * sizeof(int32_t) > size) return NULL;
    int32_t *entries = (int32_t *)(void *)(image + table);
    if (table + entries[1] != fallback) return NULL;
    for (int k = 2; k < TABLE_ENTRIES; ++k) {
        const int64_t target = table + entries[k];
        static const uint8_t head[8] = {0x49, 0x8b, 0x06, 0xbe, 0x01, 0x00, 0x00, 0x00};
        if (target < 0 || (uint64_t)target + CASE_LENGTH > size || memcmp(image + target, head, sizeof(head)) ||
            image[target + 8] != 0xba || image[target + 9] != k || image[target + 10] || image[target + 11] || image[target + 12])
            return NULL;
    }
    return entries;
}

int exit_game_hook_install_located(uint8_t *image, uint64_t size, uint64_t entry, const DcpExitGameSites *sites,
                                   uint8_t *stubs, size_t stubs_size) {
    if (!sites || !image) {
        printf("Exit Game: not installed, no sites\n");
        return 0;
    }
    const uint64_t cut = sites->row_widget;
    if (!row_region_matches(image, size, cut)) {
        printf("Exit Game: not installed, the title's row construction differs\n");
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
    if (!transition_parts_match(image, size, sites)) {
        printf("Exit Game: not installed, the title's transition parts differ\n");
        return 0;
    }
    int32_t *jump_table = jump_table_of(image, size, sites->menu_tick);
    if (!jump_table) {
        printf("Exit Game: not installed, the menu result dispatch differs\n");
        return 0;
    }
    GuestHook hooks[EXIT_GAME_HOOK_COUNT] = {
        {"exit game sheet", sites->sheet_fill, SHEET_FILL_LENGTH, {0}, exit_game_sheet_hook},
        {"exit game row", sites->row_widget, 5, {0}, exit_game_row_hook},
        {"exit game tick", sites->menu_tick, TICK_LENGTH, {0}, exit_game_tick_hook},
        {"exit game help 1", sites->help_limits[0] - HELP_LEA_BACK, HELP_LEA_LENGTH, {0}, exit_game_help_hook},
        {"exit game help 2", sites->help_limits[1] - HELP_LEA_BACK, HELP_LEA_LENGTH, {0}, exit_game_help_hook},
        {"title layout", sites->part_setter, PART_SETTER_LENGTH, {0}, exit_game_layout_hook},
    };
    memcpy(hooks[0].original, kSheetFillBytes, SHEET_FILL_LENGTH);
    memcpy(hooks[1].original, image + cut, 5);
    memcpy(hooks[2].original, kTickBytes, TICK_LENGTH);
    memcpy(hooks[3].original, kHelpLeaBytes, HELP_LEA_LENGTH);
    memcpy(hooks[4].original, kHelpLeaBytes, HELP_LEA_LENGTH);
    memcpy(hooks[5].original, kPartSetterBytes, PART_SETTER_LENGTH);
    const GuestImageId id = {size, entry};
    size_t bad = 0;
    const GuestHookStatus status = guest_hooks_install(image, size, entry, &id, hooks, EXIT_GAME_HOOK_COUNT, stubs, stubs_size, &bad);
    if (status != GUEST_HOOK_OK) {
        printf("Exit Game: not installed: %s (hook %zu: %s)\n", guest_hook_status_text(status), bad, hooks[bad].name);
        return 0;
    }
    uint8_t *block = image + cut - EXIT_GAME_WIDGET_BLOCK_BACK;
    block[0] = 0xe9; /* the fifth row widget is not built */
    const int32_t over = WIDGET_BLOCK_SIZE - 5;
    memcpy(block + 1, &over, sizeof(over));
    for (int i = 0; i < ITEM_COUNT; ++i) block[WIDGET_BLOCK_SIZE + i * ITEM_STRIDE + 1] = kRowItemIds[i];
    image[cut - CUT_IMMEDIATE_BACK] = EXIT_GAME_MENU_ITEMS;
    jump_table[3] = jump_table[5];
    jump_table[4] = jump_table[6];
    jump_table[5] = jump_table[6] = jump_table[1];
    for (int i = PART_TABLE_FIRST; i <= PART_TABLE_LAST; ++i) part_table[i] -= PART_TABLE_VARIANT_DELTA;
    for (int row = 1; row < EXIT_GAME_MENU_ITEMS; ++row) part_table[PART_TABLE_FIRST + row] = part_table[PART_TABLE_FIRST + kRowAction[row]];
    for (int i = 0; i < DCP_TRANSITION_PART_COUNT; ++i) image[sites->transition_parts[i]] -= PART_TABLE_VARIANT_DELTA;
    image[sites->help_limits[0]] = HELP_LIMIT_NEW;
    image[sites->help_limits[1]] = HELP_LIMIT_NEW;
    printf("Exit Game: sites installed (sheet 0x%x, row 0x%x, tick 0x%x, layout 0x%x)\n", sites->sheet_fill, sites->row_widget,
           sites->menu_tick, sites->part_setter);
    printf("DEBUG_SAULO title menu: 4 rows installed\n");
    return 1;
}
