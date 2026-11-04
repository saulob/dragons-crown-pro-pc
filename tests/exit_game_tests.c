/* Focused tests of the title's four-row menu with Exit Game: the SystemMsg text of every language, the hooks and the
 * install checks, on synthetic sheets, layouts and images. The locators and the real boot images are covered by
 * dcp-build-parity-tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "exit_game_hook.h"
#include "exit_game_text.h"
#include "title_layout_fixture.h"

static int failures = 0;
#define CHECK(cond)                                        \
    do {                                                   \
        if (!(cond)) {                                     \
            printf("FAIL line %d: %s\n", __LINE__, #cond); \
            ++failures;                                    \
        }                                                  \
    } while (0)

enum { SHEET_BYTES = 0x20 + EXIT_GAME_SYSTEM_MSG_COUNT * 8 + 0x8000 };

static void put_u32(uint8_t *at, uint32_t value) {
    for (int i = 0; i < 4; ++i) at[i] = (uint8_t)(value >> (8 * i));
}

/* A SystemMsg-shaped sheet: record i is "r<i>", except the four title records. Returns the sheet size. */
static size_t build_sheet(uint8_t *sheet, const ExitGameLanguage *language, int patched_row) {
    memset(sheet, 0, SHEET_BYTES);
    memcpy(sheet, "FMSB", 4);
    put_u32(sheet + 0x14, EXIT_GAME_SYSTEM_MSG_COUNT);
    sheet[0x18] = 3;
    uint8_t *p = sheet + 0x20 + EXIT_GAME_SYSTEM_MSG_COUNT * 8;
    for (int i = 0; i < EXIT_GAME_SYSTEM_MSG_COUNT; ++i) {
        char text[32];
        const char *record = text;
        snprintf(text, sizeof(text), "r%d", i);
        if (i == EXIT_GAME_INSTALL_RECORD) record = language->install_original;
        else if (i == EXIT_GAME_ROW_RECORD) record = patched_row ? language->row : "PlayStation$Store";
        else if (i == EXIT_GAME_HELP_RECORD) record = patched_row ? language->help : language->help_original;
        else if (i == EXIT_GAME_STORE_HELP_RECORD) record = "Accessing ##(PSSTORE).";
        memcpy(p, record, strlen(record) + 1);
        p += strlen(record) + 1;
    }
    const size_t end = (size_t)(p - sheet);
    put_u32(sheet + 4, (uint32_t)(end - 0x20));
    memcpy(sheet + end, "FEOC", 4);
    return end + 4;
}

static const char *record_at(const uint8_t *sheet, int index) {
    const uint8_t *p = sheet + 0x20 + EXIT_GAME_SYSTEM_MSG_COUNT * 8;
    for (int i = 0; i < index; ++i) p += strlen((const char *)p) + 1;
    return (const char *)p;
}

static void test_language_table(void) {
    CHECK(EXIT_GAME_LANGUAGE_COUNT == 8);
    for (int i = 0; i < EXIT_GAME_LANGUAGE_COUNT; ++i) {
        const ExitGameLanguage *l = &exit_game_languages[i];
        CHECK(l->language && l->install_original && l->help_original && l->row && l->help);
        CHECK(strlen(l->row) > 0 && strlen(l->row) <= strlen("PlayStation$Store"));
        CHECK(strlen(l->help) > 0 && strlen(l->help) <= strlen(l->help_original));
        for (int j = 0; j < i; ++j) CHECK(strcmp(l->language, exit_game_languages[j].language));
    }
}

static void test_text_every_language(void) {
    uint8_t *sheet = malloc(SHEET_BYTES), *original = malloc(SHEET_BYTES);
    CHECK(sheet && original);
    if (!sheet || !original) return;
    for (int i = 0; i < EXIT_GAME_LANGUAGE_COUNT; ++i) {
        const ExitGameLanguage *l = &exit_game_languages[i], *found = NULL;
        const size_t size = build_sheet(sheet, l, 0);
        memcpy(original, sheet, size);
        CHECK(exit_game_text_patch(sheet, size, &found) == EXIT_GAME_TEXT_PATCHED && found == l);
        CHECK(!strcmp(record_at(sheet, EXIT_GAME_ROW_RECORD), l->row));
        CHECK(!strcmp(record_at(sheet, EXIT_GAME_HELP_RECORD), l->help));
        /* the sheet keeps its size and every record keeps its index: the other records are byte for byte as they were */
        for (int r = 0; r < EXIT_GAME_SYSTEM_MSG_COUNT; ++r) {
            if (r >= EXIT_GAME_INSTALL_RECORD && r <= EXIT_GAME_ROW_RECORD) continue;
            if (r >= EXIT_GAME_HELP_RECORD && r <= EXIT_GAME_STORE_HELP_RECORD) continue;
            CHECK(!strcmp(record_at(sheet, r), record_at(original, r)));
        }
        CHECK(!memcmp(sheet + size - 4, "FEOC", 4));
        CHECK(!memcmp(sheet, original, 0x20 + EXIT_GAME_SYSTEM_MSG_COUNT * 8));
        CHECK(exit_game_text_patch(sheet, size, NULL) == EXIT_GAME_TEXT_ALREADY);
        const ExitGameLanguage *pre_patched = NULL;
        build_sheet(sheet, l, 1);
        CHECK(exit_game_text_patch(sheet, size, &pre_patched) == EXIT_GAME_TEXT_ALREADY && pre_patched == l);
    }
    free(sheet);
    free(original);
}

static void test_text_leaves_other_sheets_alone(void) {
    uint8_t *sheet = malloc(SHEET_BYTES), *copy = malloc(SHEET_BYTES);
    CHECK(sheet && copy);
    if (!sheet || !copy) return;
    size_t size = build_sheet(sheet, &exit_game_languages[0], 0);
    memcpy(copy, sheet, size);
    sheet[0] = 'X';
    CHECK(exit_game_text_patch(sheet, size, NULL) == EXIT_GAME_TEXT_NOT_SYSTEM_MSG);
    sheet[0] = 'F';
    put_u32(sheet + 0x14, 1610);
    CHECK(exit_game_text_patch(sheet, size, NULL) == EXIT_GAME_TEXT_NOT_SYSTEM_MSG);
    put_u32(sheet + 0x14, EXIT_GAME_SYSTEM_MSG_COUNT);
    sheet[0x18] = 1;
    CHECK(exit_game_text_patch(sheet, size, NULL) == EXIT_GAME_TEXT_NOT_SYSTEM_MSG);
    sheet[0x18] = 3;
    memcpy(sheet + size - 4, "FEOX", 4);
    CHECK(exit_game_text_patch(sheet, size, NULL) == EXIT_GAME_TEXT_NOT_SYSTEM_MSG);
    memcpy(sheet + size - 4, "FEOC", 4);
    CHECK(exit_game_text_patch(sheet, size - 1, NULL) == EXIT_GAME_TEXT_NOT_SYSTEM_MSG);
    CHECK(!memcmp(sheet, copy, size));

    /* a language the table does not know, and a row the sheet does not hold, are left as they are */
    ExitGameLanguage other = exit_game_languages[0];
    other.install_original = "Foreign";
    size = build_sheet(sheet, &other, 0);
    memcpy(copy, sheet, size);
    CHECK(exit_game_text_patch(sheet, size, NULL) == EXIT_GAME_TEXT_UNKNOWN);
    CHECK(!memcmp(sheet, copy, size));
    free(sheet);
    free(copy);
}


static int closes;
static int close_result = 1;
static int count_close(void) {
    ++closes;
    return close_result;
}

static uint8_t scene_memory[0x3000];
#define menu_memory (scene_memory + 0x1350)
static uint64_t widget_slots[8];
static uint8_t item_slots[8 * 0x30];

static void set_menu(unsigned items, unsigned widgets) {
    const uint64_t items_begin = (uint64_t)(uintptr_t)item_slots, items_end = items_begin + items * 0x30;
    const uint64_t widgets_begin = (uint64_t)(uintptr_t)widget_slots, widgets_end = widgets_begin + widgets * 8;
    memcpy(menu_memory + 0xc78, &items_begin, 8);
    memcpy(menu_memory + 0xc80, &items_end, 8);
    memcpy(menu_memory + 0x68, &widgets_begin, 8);
    memcpy(menu_memory + 0x70, &widgets_end, 8);
}

/* The title's state at the cut call: six items added, four row widgets built, the list being cut to four. */
static GuestRegs row_regs(unsigned items, unsigned widgets) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    set_menu(items, widgets);
    regs.rsi = EXIT_GAME_MENU_ITEMS;
    regs.r12 = (uint64_t)(uintptr_t)menu_memory;
    regs.rdi = regs.r12 + 0xc70;
    regs.r13 = 0x10049d43e0ull;
    return regs;
}

static uint64_t fake_time;
static uint64_t fake_clock(void) {
    return fake_time;
}

static GuestRegs tick_regs(int32_t cursor, unsigned items, unsigned widgets) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    set_menu(items, widgets);
    memcpy(scene_memory + 0x1ca8, &cursor, sizeof(cursor));
    regs.r13 = (uint64_t)(uintptr_t)scene_memory;
    return regs;
}

static void start_title(void) {
    exit_game_reset();
    exit_game_set_clock(fake_clock);
    fake_time = 1000;
    GuestRegs regs = row_regs(EXIT_GAME_LIST_ITEMS, EXIT_GAME_MENU_WIDGETS);
    exit_game_row_hook(&regs);
}

static void test_counts(void) {
    CHECK(EXIT_GAME_MENU_ITEMS == 4 && EXIT_GAME_MENU_WIDGETS == 4 && EXIT_GAME_ROW_INDEX == 3 && EXIT_GAME_LIST_ITEMS == 6);
    CHECK(EXIT_GAME_ROW_INDEX == EXIT_GAME_MENU_ITEMS - 1);
}

/* Only the title's own menu, as the cut call sees it, is ever selected; every other state publishes nothing. */
static void test_row_hook(void) {
    start_title();
    GuestRegs regs = tick_regs(EXIT_GAME_ROW_INDEX, 4, 4);
    exit_game_tick_hook(&regs);
    CHECK(exit_game_selected());

    const struct {
        unsigned items, widgets;
        int cut;
    } states[] = {{5, 4, 4}, {7, 4, 4}, {6, 3, 4}, {6, 5, 4}, {6, 6, 4}, {4, 4, 4}, {6, 4, 5}, {6, 4, 6}, {6, 4, 3}};
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); ++i) {
        exit_game_reset();
        exit_game_set_clock(fake_clock);
        regs = row_regs(states[i].items, states[i].widgets);
        regs.rsi = (uint64_t)states[i].cut;
        exit_game_row_hook(&regs);
        regs = tick_regs(EXIT_GAME_ROW_INDEX, 4, 4);
        exit_game_tick_hook(&regs);
        CHECK(!exit_game_selected());
    }
    for (int fault = 0; fault < 3; ++fault) {
        exit_game_reset();
        exit_game_set_clock(fake_clock);
        regs = row_regs(6, 4);
        if (fault == 0) regs.r13 = 0;
        if (fault == 1) regs.r12 = 0;
        if (fault == 2) regs.rdi += 8;
        exit_game_row_hook(&regs);
        regs = tick_regs(EXIT_GAME_ROW_INDEX, 4, 4);
        exit_game_tick_hook(&regs);
        CHECK(!exit_game_selected());
    }
    exit_game_reset();
}

static void test_selection(void) {
    start_title();
    GuestRegs regs = tick_regs(3, 4, 4);
    CHECK(!exit_game_selected());
    exit_game_tick_hook(&regs);
    CHECK(exit_game_selected());
    /* the cursor leaves the fourth row, whatever row it goes to */
    for (int cursor = 0; cursor < 3; ++cursor) {
        regs = tick_regs(cursor, 4, 4);
        exit_game_tick_hook(&regs);
        CHECK(!exit_game_selected());
    }
    /* the six-row positions of the old menu are not Exit Game */
    for (int cursor = 4; cursor < 6; ++cursor) {
        regs = tick_regs(cursor, 4, 4);
        exit_game_tick_hook(&regs);
        CHECK(!exit_game_selected());
    }
    regs = tick_regs(3, 4, 4);
    exit_game_tick_hook(&regs);
    CHECK(exit_game_selected());
    /* the title menu is gone: nothing refreshes the selection */
    fake_time += 50;
    CHECK(exit_game_selected());
    fake_time += 1000;
    CHECK(!exit_game_selected());
    /* a menu that is not the four-row title menu is never selected */
    regs = tick_regs(3, 5, 5);
    exit_game_tick_hook(&regs);
    CHECK(!exit_game_selected());
    regs = tick_regs(3, 4, 5);
    exit_game_tick_hook(&regs);
    CHECK(!exit_game_selected());
    regs = tick_regs(3, 6, 4);
    exit_game_tick_hook(&regs);
    CHECK(!exit_game_selected());
    regs = tick_regs(3, 4, 4);
    regs.r13 += 0x10;
    exit_game_tick_hook(&regs);
    CHECK(!exit_game_selected());
    regs = tick_regs(3, 4, 4);
    regs.r13 = 0;
    exit_game_tick_hook(&regs);
    CHECK(!exit_game_selected());
    exit_game_reset();
}

static void test_accept(void) {
    start_title();
    exit_game_set_handler(count_close);
    closes = 0;
    GuestRegs regs = tick_regs(2, 4, 4);
    exit_game_tick_hook(&regs);
    /* South or Enter on the other rows is the game's */
    CHECK(exit_game_filter_accept(1) == 0 && exit_game_filter_accept(0) == 0 && closes == 0);
    /* only an accept pressed while the Exit Game row is selected counts, and nothing else reaches the handler */
    regs = tick_regs(3, 4, 4);
    exit_game_tick_hook(&regs);
    CHECK(exit_game_filter_accept(0) == 0 && closes == 0 && !exit_game_requested());
    CHECK(exit_game_filter_accept(1) == 1 && closes == 1 && exit_game_requested());
    /* held: still hidden, no second request; released: the game sees input again */
    CHECK(exit_game_filter_accept(1) == 1 && closes == 1);
    exit_game_tick_hook(&regs);
    CHECK(exit_game_filter_accept(1) == 1 && closes == 1);
    CHECK(exit_game_filter_accept(0) == 0);
    CHECK(exit_game_filter_accept(1) == 1 && closes == 1);
    CHECK(exit_game_filter_accept(0) == 0);

    /* an accept already held when the cursor arrives is not a press */
    start_title();
    exit_game_set_handler(count_close);
    closes = 0;
    regs = tick_regs(2, 4, 4);
    exit_game_tick_hook(&regs);
    exit_game_filter_accept(1);
    regs = tick_regs(3, 4, 4);
    exit_game_tick_hook(&regs);
    CHECK(exit_game_filter_accept(1) == 0 && closes == 0);
    exit_game_filter_accept(0);

    /* no host handler: the press stays the game's */
    start_title();
    regs = tick_regs(3, 4, 4);
    exit_game_tick_hook(&regs);
    CHECK(exit_game_filter_accept(1) == 0 && !exit_game_requested());
    exit_game_filter_accept(0);
    CHECK(exit_game_invoke_handler() == 0);
    exit_game_reset();
}

static void test_accept_button(void) {
    CHECK(exit_game_accept_east() == 0);
    exit_game_set_accept_east(1);
    CHECK(exit_game_accept_east() == 1);
    exit_game_set_accept_east(0);
    CHECK(exit_game_accept_east() == 0);

    /* the Japanese button layout moves the accept button, not the rows: Exit Game is still the fourth row */
    for (int east = 0; east < 2; ++east) {
        exit_game_set_accept_east(east);
        start_title();
        exit_game_set_handler(count_close);
        closes = 0;
        for (int cursor = 0; cursor < 3; ++cursor) {
            GuestRegs regs = tick_regs(cursor, 4, 4);
            exit_game_tick_hook(&regs);
            CHECK(!exit_game_selected() && exit_game_filter_accept(1) == 0 && closes == 0);
            exit_game_filter_accept(0);
        }
        GuestRegs regs = tick_regs(3, 4, 4);
        exit_game_tick_hook(&regs);
        CHECK(exit_game_selected() && exit_game_filter_accept(1) == 1 && closes == 1);
        exit_game_filter_accept(0);
        exit_game_reset();
    }
    exit_game_set_accept_east(0);
    FILE *f = fopen(DCP_SOURCE_DIR "/src/runtime_pad.c", "rb");
    char text[1 << 16] = {0};
    CHECK(f && fread(text, 1, sizeof(text) - 1, f) > 0);
    if (f) fclose(f);
    /* The merged pad state, so a keyboard binding or a remapped pad button confirms here as on every other row. */
    CHECK(strstr(text, "d->buttons & (exit_game_accept_east() ? BTN_CIRCLE : BTN_CROSS)"));
    CHECK(strstr(text, "k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER]"));
}

/* The help text id is 389 plus the row's action: Start 0, Options 3, Gallery 4, Exit Game 5. */
static void test_help_hook(void) {
    static const uint64_t kExpected[EXIT_GAME_MENU_ITEMS] = {0, 3, 4, 5};
    for (uint64_t row = 0; row < EXIT_GAME_MENU_ITEMS; ++row) {
        GuestRegs regs;
        memset(&regs, 0, sizeof(regs));
        regs.rax = row;
        regs.rcx = 0x1234;
        exit_game_help_hook(&regs);
        CHECK(regs.rax == kExpected[row] && regs.rcx == 0x1234);
    }
    for (uint64_t row = EXIT_GAME_MENU_ITEMS; row < 8; ++row) {
        GuestRegs regs;
        memset(&regs, 0, sizeof(regs));
        regs.rax = row;
        exit_game_help_hook(&regs);
        CHECK((uint32_t)regs.rax >= EXIT_GAME_ROW_INDEX + 3); /* past the help limit of six: no help text */
    }
}

static void test_sheet_hook(void) {
    uint8_t *sheet = malloc(SHEET_BYTES);
    CHECK(sheet != NULL);
    if (!sheet) return;
    build_sheet(sheet, &exit_game_languages[2], 0);
    uint8_t object[0x100] = {0};
    uint8_t *data = sheet;
    memcpy(object + 0xf8, &data, sizeof(data));
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rdi = (uint64_t)(uintptr_t)object;
    exit_game_reset();
    exit_game_sheet_hook(&regs);
    CHECK(!strcmp(record_at(sheet, EXIT_GAME_ROW_RECORD), exit_game_languages[2].row));
    regs.rdi = 0;
    exit_game_sheet_hook(&regs);
    memset(object, 0, sizeof(object));
    regs.rdi = (uint64_t)(uintptr_t)object;
    exit_game_sheet_hook(&regs);
    free(sheet);
}

/* The part setter's player holds two resources, each pointing at a layout: both are patched when they are the title's. */
static void test_layout_hook(void) {
    FixtureLayout first, second;
    CHECK(fixture_make(&first) && fixture_make(&second));
    uint8_t player[0x200] = {0}, resource_a[0x200] = {0}, resource_b[0x200] = {0};
    fixture_put_pointer(player + 0xc0, resource_a);
    fixture_put_pointer(player + 0xc8, resource_b);
    fixture_put_pointer(resource_a + 0x158, first.header);
    fixture_put_pointer(resource_b + 0x158, second.header);
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    exit_game_reset();
    exit_game_layout_hook(&regs); /* no player */
    regs.rdi = (uint64_t)(uintptr_t)player;
    exit_game_layout_hook(&regs);
    CHECK(first.parts[kTitleLayoutParts[1].index * 48 + 0x2a] == kTitleLayoutParts[1].new_count);
    CHECK(second.parts[kTitleLayoutParts[1].index * 48 + 0x2a] == kTitleLayoutParts[1].new_count);
    exit_game_layout_hook(&regs); /* idempotent */
    CHECK(first.parts[kTitleLayoutParts[1].index * 48 + 0x2a] == kTitleLayoutParts[1].new_count);

    /* a player without resources, with a resource without a layout, and with a layout of another scene */
    memset(resource_b + 0x158, 0, 8);
    memset(player + 0xc0, 0, 8);
    exit_game_layout_hook(&regs);
    memset(player, 0, sizeof(player));
    exit_game_layout_hook(&regs);
    fixture_free(&first);
    fixture_free(&second);

    /* a title layout that differs from the known one stays untouched */
    FixtureLayout odd;
    CHECK(fixture_make(&odd));
    odd.layers[kTitleLayoutParts[2].layer_offset * 24 + 5] ^= 1;
    uint8_t before[178 * 48];
    memcpy(before, odd.parts, sizeof(before));
    fixture_put_pointer(player + 0xc0, resource_a);
    fixture_put_pointer(resource_a + 0x158, odd.header);
    exit_game_reset();
    exit_game_layout_hook(&regs);
    CHECK(!memcmp(before, odd.parts, sizeof(before)));
    fixture_free(&odd);
    exit_game_reset();
}

enum {
    IMAGE_BYTES = 0x2000,
    STUB_ROOM = 0x1000,
    FILL_AT = 0x100,
    CUT_AT = 0x900,
    BLOCK_AT = CUT_AT - EXIT_GAME_WIDGET_BLOCK_BACK,
    LIMIT_A = 0x602,
    LIMIT_B = 0x702,
    LOAD_A = 0xa00,
    LOAD_B = 0xa40,
    TICK_AT = 0xc00,
    JUMP_TABLE_AT = 0xd00,
    CASE_AT = 0xd80,
    DEFAULT_AT = 0xe40,
    SETTER_AT = 0xf00,
    TABLE_AT = 0x1800,
    TRANSITION_AT = 0x1b00
};

/* TITLE_1, START_1, START_2, OPTION_1, OPTION_2, GALLERY_1, GALLERY_2 */
static const uint8_t kPs4TransitionParts[DCP_TRANSITION_PART_COUNT] = {11, 18, 21, 102, 105, 139, 142};
static const uint8_t kSixRowTransitionParts[DCP_TRANSITION_PART_COUNT] = {9, 16, 19, 100, 103, 137, 140};
static const uint8_t kItemIds[6] = {0x7e, 0x7f, 0x80, 0x81, 0x82, 0x84};
static const uint8_t kRowItemIds[6] = {0x7e, 0x81, 0x82, 0x84, 0x7f, 0x80};

static void put_call(uint8_t *image, uint32_t at, uint32_t target) {
    image[at] = 0xe8;
    put_u32(image + at + 1, target - (at + 5));
}

static uint8_t *new_image(DcpExitGameSites *sites) {
    uint8_t *image = calloc(1, IMAGE_BYTES + STUB_ROOM);
    if (!image) return NULL;
    static const uint8_t fill[10] = {0x48, 0x8b, 0x87, 0xf8, 0x00, 0x00, 0x00, 0x8a, 0x48, 0x18};
    memcpy(image + FILL_AT, fill, sizeof(fill));
    {
        static const uint8_t head[6] = {0xbf, 0xc0, 0x05, 0x00, 0x00, 0xe8};
        static const uint8_t item_tail[6] = {0x31, 0xd2, 0x4c, 0x89, 0xe7, 0xe8};
        static const uint8_t reload[4] = {0x48, 0x8b, 0x7d, 0xb0};
        static const uint8_t cut_size[5] = {0xbe, 0x05, 0x00, 0x00, 0x00};
        memcpy(image + BLOCK_AT, head, sizeof(head));
        put_call(image, BLOCK_AT + 59, 0x1200);
        for (int i = 0; i < 6; ++i) {
            uint8_t *item = image + BLOCK_AT + 64 + i * 15;
            item[0] = 0xbe;
            item[1] = kItemIds[i];
            item[2] = 0x01;
            memcpy(item + 5, item_tail, sizeof(item_tail));
            put_call(image, BLOCK_AT + 64 + i * 15 + 10, 0x1300);
        }
        memcpy(image + BLOCK_AT + 154, reload, sizeof(reload));
        memcpy(image + BLOCK_AT + 158, cut_size, sizeof(cut_size));
        put_call(image, CUT_AT, 0x1400);
    }
    for (int limit = 0; limit < 2; ++limit) {
        static const uint8_t lea[6] = {0x8d, 0x88, 0x85, 0x01, 0x00, 0x00};
        const uint32_t at = limit ? LIMIT_B : LIMIT_A;
        memcpy(image + at - 8, lea, sizeof(lea));
        image[at - 2] = 0x83;
        image[at - 1] = 0xf8;
        image[at] = 5;
    }
    static const int32_t table[38] = {5,  8,  11, 15, 43, 71, 99, 136, 168, 18, 46, 74, 102, 139, 171, 24, 30, 36, 27,
                                      33, 39, 52, 58, 64, 55, 61, 67, 80,  86,  92, 83, 89,  95,  21, 49, 77, 105, 142};
    memcpy(image + TABLE_AT, table, sizeof(table));
    for (int load = 0; load < 2; ++load) {
        const uint32_t at = load ? LOAD_B : LOAD_A;
        image[at] = 0x48;
        image[at + 1] = 0x8d;
        image[at + 2] = 0x15;
        put_u32(image + at + 3, TABLE_AT - (at + 7));
    }
    {
        static const uint8_t tick[6] = {0x83, 0xc0, 0x02, 0x41, 0xb4, 0x01};
        static const uint8_t compare[3] = {0x83, 0xf8, 0x06};
        static const uint8_t jump[9] = {0x48, 0x63, 0x04, 0x81, 0x48, 0x01, 0xc8, 0xff, 0xe0};
        memcpy(image + TICK_AT, tick, sizeof(tick));
        memcpy(image + TICK_AT + 6, compare, sizeof(compare));
        image[TICK_AT + 9] = 0x0f;
        image[TICK_AT + 10] = 0x87;
        put_u32(image + TICK_AT + 11, DEFAULT_AT - (TICK_AT + 15));
        image[TICK_AT + 15] = 0x48;
        image[TICK_AT + 16] = 0x8d;
        image[TICK_AT + 17] = 0x0d;
        put_u32(image + TICK_AT + 18, JUMP_TABLE_AT - (TICK_AT + 22));
        memcpy(image + TICK_AT + 22, jump, sizeof(jump));
        put_u32(image + JUMP_TABLE_AT, 0x40);
        put_u32(image + JUMP_TABLE_AT + 4, DEFAULT_AT - JUMP_TABLE_AT);
        for (int k = 2; k < 7; ++k) {
            static const uint8_t head[8] = {0x49, 0x8b, 0x06, 0xbe, 0x01, 0x00, 0x00, 0x00};
            uint8_t *state = image + CASE_AT + k * 0x10;
            put_u32(image + JUMP_TABLE_AT + 4 * k, CASE_AT + k * 0x10 - JUMP_TABLE_AT);
            memcpy(state, head, sizeof(head));
            state[8] = 0xba;
            state[9] = (uint8_t)k;
        }
    }
    {
        static const uint8_t setter[8] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56};
        memcpy(image + SETTER_AT, setter, sizeof(setter));
    }
    for (int i = 0; i < DCP_TRANSITION_PART_COUNT; ++i) {
        const uint32_t at = TRANSITION_AT + (uint32_t)i * 0x20;
        image[at] = 0xbe;
        image[at + 1] = kPs4TransitionParts[i];
        sites->transition_parts[i] = at + 1;
    }
    sites->menu_tick = TICK_AT;
    sites->part_table_loads[0] = LOAD_A;
    sites->part_table_loads[1] = LOAD_B;
    sites->sheet_fill = FILL_AT;
    sites->row_widget = CUT_AT;
    sites->help_limits[0] = LIMIT_A;
    sites->help_limits[1] = LIMIT_B;
    sites->part_setter = SETTER_AT;
    return image;
}

static int install(uint8_t *image, const DcpExitGameSites *sites) {
    return exit_game_hook_install_located(image, IMAGE_BYTES, 0x54c690, sites, image + IMAGE_BYTES, STUB_ROOM);
}

static void test_install(void) {
    DcpExitGameSites sites;
    uint8_t *image = new_image(&sites);
    CHECK(image != NULL);
    if (!image) return;
    int32_t jump_before[7];
    memcpy(jump_before, image + JUMP_TABLE_AT, sizeof(jump_before));
    CHECK(install(image, &sites) == 1);
    CHECK(image[FILL_AT] == 0xe9 && image[TICK_AT] == 0xe9 && image[CUT_AT] == 0xe9 && image[SETTER_AT] == 0xe9);
    CHECK(image[LIMIT_A - 8] == 0xe9 && image[LIMIT_B - 8] == 0xe9);
    CHECK(image[LIMIT_A] == 6 && image[LIMIT_B] == 6);
    {
        /* the fifth row widget is jumped over, the items are added in the row order, and the list is cut to four */
        int32_t over;
        memcpy(&over, image + BLOCK_AT + 1, sizeof(over));
        CHECK(image[BLOCK_AT] == 0xe9 && over == 59);
        for (int i = 0; i < 6; ++i) CHECK(image[BLOCK_AT + 64 + i * 15 + 1] == kRowItemIds[i]);
        CHECK(image[CUT_AT - 4] == EXIT_GAME_MENU_ITEMS && image[CUT_AT - 5] == 0xbe);
    }
    {
        /* the menu result of the second and third rows runs the Options and Gallery cases; the fourth runs none */
        int32_t jump[7];
        memcpy(jump, image + JUMP_TABLE_AT, sizeof(jump));
        CHECK(jump[0] == jump_before[0] && jump[1] == jump_before[1] && jump[2] == jump_before[2]);
        CHECK(jump[3] == jump_before[5] && jump[4] == jump_before[6]);
        CHECK(jump[5] == jump_before[1] && jump[6] == jump_before[1]);
        CHECK(image[JUMP_TABLE_AT + jump[3] + 9] == 5 && image[JUMP_TABLE_AT + jump[4] + 9] == 6);
    }
    {
        /* each row's idle part is its action's six-row variant, two part indexes lower; the title's own entries stay */
        int32_t table[38];
        memcpy(table, image + TABLE_AT, sizeof(table));
        CHECK(table[0] == 5 && table[1] == 8 && table[2] == 11);
        CHECK(table[3] == 13 && table[4] == 97 && table[5] == 134 && table[6] == 166);
        CHECK(table[7] == 134 && table[8] == 166 && table[14] == 169 && table[37] == 140);
    }
    {
        /* Start Game, Options, Gallery and Exit Game are the four rows: the menu result, the idle part and the help text of
         * each reach that action, and no row reaches Upload Save Data or Download Save Data */
        int32_t jump[7], table[38];
        memcpy(jump, image + JUMP_TABLE_AT, sizeof(jump));
        memcpy(table, image + TABLE_AT, sizeof(table));
        static const int kState[EXIT_GAME_MENU_ITEMS] = {2, 5, 6, 0};
        static const int kIdlePart[EXIT_GAME_MENU_ITEMS] = {13, 97, 134, 166}; /* START_0, OPTION_0, GALLERY_0, PSSTORE_0 */
        static const uint64_t kHelpAction[EXIT_GAME_MENU_ITEMS] = {0, 3, 4, 5};
        for (int row = 0; row < EXIT_GAME_MENU_ITEMS; ++row) {
            const int32_t target = JUMP_TABLE_AT + jump[row + 2];
            if (kState[row]) CHECK(image[target + 9] == kState[row]);
            else CHECK(target == DEFAULT_AT);
            CHECK(kState[row] != 3 && kState[row] != 4);
            CHECK(table[3 + row] == kIdlePart[row]);
            CHECK(table[3 + row] != 41 && table[3 + row] != 69); /* UPLOAD_0, DOWNLOAD_0 */
            GuestRegs regs;
            memset(&regs, 0, sizeof(regs));
            regs.rax = (uint64_t)row;
            exit_game_help_hook(&regs);
            CHECK(regs.rax == kHelpAction[row] && regs.rax != 1 && regs.rax != 2);
        }
    }
    for (int i = 0; i < DCP_TRANSITION_PART_COUNT; ++i) {
        /* every fixed entrance, confirm and return part moves with its row family; nothing around the index changes */
        const uint32_t at = sites.transition_parts[i];
        CHECK(image[at] == kSixRowTransitionParts[i] && image[at - 1] == 0xbe && !image[at + 1] && !image[at + 2] && !image[at + 3]);
        CHECK(kSixRowTransitionParts[i] + 2 == kPs4TransitionParts[i]);
    }
    CHECK(exit_game_hook_stub_bytes() >= EXIT_GAME_HOOK_COUNT);
    free(image);

    /* each site that does not match leaves the whole image as it was */
    for (int fault = 0; fault < 25; ++fault) {
        DcpExitGameSites faulty;
        image = new_image(&faulty);
        uint8_t *before = malloc(IMAGE_BYTES + STUB_ROOM);
        CHECK(image && before);
        if (!image || !before) return;
        switch (fault) {
        case 0: image[FILL_AT + 3] ^= 1; break;
        case 1: image[TICK_AT + 1] ^= 1; break;
        case 2: image[BLOCK_AT] = 0x90; break;
        case 3: image[BLOCK_AT + 59] = 0x90; break;
        case 4: image[BLOCK_AT + 64 + 3 * 15 + 1] = 0x83; break;
        case 5: image[BLOCK_AT + 64 + 5 * 15 + 5] = 0x90; break;
        case 6: image[CUT_AT - 4] = 6; break;
        case 7: image[CUT_AT] = 0x90; break;
        case 8: image[LIMIT_A] = 4; break;
        case 9: image[LIMIT_B - 1] = 0x3d; break;
        case 10: faulty.help_limits[1] = faulty.help_limits[0]; break;
        case 11: image[TABLE_AT + 4 * 8] = 169; break;
        case 12: image[TICK_AT] = 0x90; break;
        case 13: image[faulty.transition_parts[4]] = 47; break;
        case 14: image[faulty.transition_parts[2] + 1] = 1; break;
        case 15: faulty.transition_parts[5] = faulty.transition_parts[4]; break;
        case 16: faulty.transition_parts[6] = IMAGE_BYTES - 2; break;
        case 17: image[LOAD_B + 1] = 0x8b; break;
        case 18: put_u32(image + LOAD_B + 3, TABLE_AT + 4 - (LOAD_B + 7)); break;
        case 19: image[CASE_AT + 5 * 0x10 + 9] = 4; break;
        case 20: put_u32(image + JUMP_TABLE_AT + 4, DEFAULT_AT + 4 - JUMP_TABLE_AT); break;
        case 21: image[TICK_AT + 10] = 0x86; break;
        case 22: image[TICK_AT + 17] = 0x05; break;
        case 23: image[SETTER_AT + 1] ^= 1; break;
        case 24: image[LIMIT_A - 8 + 2] = 0x84; break;
        }
        memcpy(before, image, IMAGE_BYTES + STUB_ROOM);
        CHECK(install(image, &faulty) == 0);
        CHECK(!memcmp(before, image, IMAGE_BYTES));
        free(image);
        free(before);
    }
    CHECK(exit_game_hook_install_located(NULL, IMAGE_BYTES, 0, &sites, NULL, 0) == 0);
    image = new_image(&sites);
    CHECK(image && exit_game_hook_install_located(image, IMAGE_BYTES, 0x54c690, NULL, image + IMAGE_BYTES, STUB_ROOM) == 0);
    free(image);
}

int main(void) {
    test_language_table();
    test_text_every_language();
    test_text_leaves_other_sheets_alone();
    test_counts();
    test_row_hook();
    test_selection();
    test_accept();
    test_accept_button();
    test_help_hook();
    test_sheet_hook();
    test_layout_hook();
    test_install();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("exit game tests passed\n");
    return 0;
}
