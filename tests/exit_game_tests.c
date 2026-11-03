/* Focused tests of the title menu's Exit Game row: the SystemMsg text of every language, the three hooks and the install
 * checks, on synthetic sheets and images. The locators and the real boot images are covered by dcp-build-parity-tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "exit_game_hook.h"
#include "exit_game_text.h"

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

static int built, constructed, added, add_result;
static uint8_t widget_memory[0x600];
static uint8_t scene_memory[0x3000];
#define menu_memory (scene_memory + 0x1350)
static uint64_t widget_slots[8];
static uint8_t item_slots[8 * 0x30];
static void *menu_pointer = menu_memory;
static void *GUEST_HOOK_ABI fake_new(uint64_t size) {
    ++built;
    return size == 0x5c0 ? widget_memory : NULL;
}
static void GUEST_HOOK_ABI fake_ctor(void *widget, void *argument) {
    CHECK(widget == widget_memory && argument == (void *)(uintptr_t)0x10049d43e0ull);
    ++constructed;
}
static uint64_t GUEST_HOOK_ABI fake_add(void *menu, void *widget) {
    CHECK(menu == menu_pointer && widget == widget_memory);
    ++added;
    return add_result;
}

static void set_menu(unsigned items, unsigned widgets) {
    const uint64_t items_begin = (uint64_t)(uintptr_t)item_slots, items_end = items_begin + items * 0x30;
    const uint64_t widgets_begin = (uint64_t)(uintptr_t)widget_slots, widgets_end = widgets_begin + widgets * 8;
    memcpy(menu_memory + 0xc78, &items_begin, 8);
    memcpy(menu_memory + 0xc80, &items_end, 8);
    memcpy(menu_memory + 0x68, &widgets_begin, 8);
    memcpy(menu_memory + 0x70, &widgets_end, 8);
}

/* The title's state at the resize call; the scale and every pointer are region and run dependent. */
static GuestRegs row_regs(unsigned items, unsigned widgets, uint64_t scale) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    set_menu(items, widgets);
    regs.rsi = EXIT_GAME_OLD_ROW_COUNT;
    regs.r12 = (uint64_t)(uintptr_t)menu_memory;
    regs.rdi = regs.r12 + 0xc70;
    regs.r13 = 0x10049d43e0ull;
    regs.r15 = scale;
    return regs;
}

#define SCALE_UNITED 0x3f4000003f400000ull
#define SCALE_JAPAN 0x3f4ccccd3f4ccccdull

static void test_row_hook(void) {
    exit_game_reset();
    built = constructed = added = 0;
    add_result = 1;
    GuestRegs regs = row_regs(6, 5, SCALE_UNITED);
    exit_game_row_hook(&regs);
    CHECK(regs.rsi == EXIT_GAME_OLD_ROW_COUNT && built == 0);

    exit_game_set_row_functions(fake_new, fake_ctor, fake_add);
    for (int region = 0; region < 2; ++region) {
        const uint64_t scale = region ? SCALE_JAPAN : SCALE_UNITED;
        exit_game_reset();
        exit_game_set_row_functions(fake_new, fake_ctor, fake_add);
        built = constructed = added = 0;
        memset(widget_memory, 0, sizeof(widget_memory));
        regs = row_regs(6, 5, scale);
        exit_game_row_hook(&regs);
        CHECK(regs.rsi == EXIT_GAME_MENU_ITEMS && built == 1 && constructed == 1 && added == 1);
        uint32_t width, height;
        uint64_t written;
        memcpy(&width, widget_memory + 0x350, 4);
        memcpy(&height, widget_memory + 0x354, 4);
        memcpy(&written, widget_memory + 0x320, 8);
        CHECK(width == 0x500 && height == 0x32 && written == scale);

        /* the menu now holds six widgets: a second call adds no seventh row */
        regs = row_regs(6, 6, scale);
        exit_game_row_hook(&regs);
        CHECK(regs.rsi == EXIT_GAME_OLD_ROW_COUNT && built == 1 && added == 1);
    }

    exit_game_reset();
    exit_game_set_row_functions(fake_new, fake_ctor, fake_add);
    built = constructed = added = 0;
    add_result = 0;
    regs = row_regs(6, 5, SCALE_UNITED);
    exit_game_row_hook(&regs);
    CHECK(regs.rsi == EXIT_GAME_OLD_ROW_COUNT && added == 1);

    add_result = 1;
    built = constructed = added = 0;
    const struct {
        unsigned items, widgets;
        uint64_t scale;
    } states[] = {{5, 5, SCALE_UNITED}, {7, 5, SCALE_UNITED}, {6, 4, SCALE_UNITED}, {6, 6, SCALE_UNITED},
                  {6, 5, 0x3f4000003f4ccccdull}, {6, 5, 0}, {6, 5, 0x4120000041200000ull}};
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); ++i) {
        regs = row_regs(states[i].items, states[i].widgets, states[i].scale);
        exit_game_row_hook(&regs);
        CHECK(regs.rsi == EXIT_GAME_OLD_ROW_COUNT);
    }
    regs = row_regs(6, 5, SCALE_UNITED);
    regs.rsi = 4;
    exit_game_row_hook(&regs);
    regs = row_regs(6, 5, SCALE_UNITED);
    regs.r13 = 0;
    exit_game_row_hook(&regs);
    regs = row_regs(6, 5, SCALE_UNITED);
    regs.r12 = 0;
    exit_game_row_hook(&regs);
    regs = row_regs(6, 5, SCALE_UNITED);
    regs.rdi += 8;
    exit_game_row_hook(&regs);
    CHECK(built == 0 && constructed == 0 && added == 0);
    exit_game_reset();
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
    exit_game_set_row_functions(fake_new, fake_ctor, fake_add);
    fake_time = 1000;
    GuestRegs regs = row_regs(6, 5, SCALE_UNITED);
    exit_game_row_hook(&regs);
}

static void test_selection(void) {
    start_title();
    GuestRegs regs = tick_regs(5, 6, 6);
    CHECK(!exit_game_selected());
    exit_game_tick_hook(&regs);
    CHECK(exit_game_selected());
    /* the cursor leaves the sixth row */
    regs = tick_regs(4, 6, 6);
    exit_game_tick_hook(&regs);
    CHECK(!exit_game_selected());
    regs = tick_regs(5, 6, 6);
    exit_game_tick_hook(&regs);
    CHECK(exit_game_selected());
    /* the title menu is gone: nothing refreshes the selection */
    fake_time += 50;
    CHECK(exit_game_selected());
    fake_time += 1000;
    CHECK(!exit_game_selected());
    /* a menu that is not the six-row title menu is never selected */
    regs = tick_regs(5, 5, 5);
    exit_game_tick_hook(&regs);
    CHECK(!exit_game_selected());
    regs = tick_regs(5, 6, 6);
    regs.r13 += 0x10;
    exit_game_tick_hook(&regs);
    CHECK(!exit_game_selected());
    regs = tick_regs(5, 6, 6);
    regs.r13 = 0;
    exit_game_tick_hook(&regs);
    CHECK(!exit_game_selected());
    exit_game_reset();
}

static void test_accept(void) {
    start_title();
    exit_game_set_handler(count_close);
    closes = 0;
    GuestRegs regs = tick_regs(4, 6, 6);
    exit_game_tick_hook(&regs);
    /* South or Enter on the other rows is the game's */
    CHECK(exit_game_filter_accept(1) == 0 && exit_game_filter_accept(0) == 0 && closes == 0);
    /* only an accept pressed while the sixth row is selected counts, and nothing else reaches the handler */
    regs = tick_regs(5, 6, 6);
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
    regs = tick_regs(4, 6, 6);
    exit_game_tick_hook(&regs);
    exit_game_filter_accept(1);
    regs = tick_regs(5, 6, 6);
    exit_game_tick_hook(&regs);
    CHECK(exit_game_filter_accept(1) == 0 && closes == 0);
    exit_game_filter_accept(0);

    /* no host handler: the press stays the game's */
    start_title();
    regs = tick_regs(5, 6, 6);
    exit_game_tick_hook(&regs);
    CHECK(exit_game_filter_accept(1) == 0 && !exit_game_requested());
    exit_game_filter_accept(0);
    CHECK(exit_game_invoke_handler() == 0);
    exit_game_reset();
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

enum { IMAGE_BYTES = 0x2000, STUB_ROOM = 0x1000, FILL_AT = 0x100, WIDGET_CALL_AT = 0x900, LIMIT_A = 0x602, LIMIT_B = 0x702, LOAD_A = 0xa00, LOAD_B = 0xa40, TICK_AT = 0xc00, TABLE_AT = 0x1800 };

static void put_call(uint8_t *image, uint32_t at, uint32_t target) {
    image[at] = 0xe8;
    put_u32(image + at + 1, target - (at + 5));
}

static uint8_t *new_image(DcpExitGameSites *sites) {
    uint8_t *image = calloc(1, IMAGE_BYTES + STUB_ROOM);
    if (!image) return NULL;
    static const uint8_t fill[10] = {0x48, 0x8b, 0x87, 0xf8, 0x00, 0x00, 0x00, 0x8a, 0x48, 0x18};
    memcpy(image + FILL_AT, fill, sizeof(fill));
    put_call(image, WIDGET_CALL_AT - EXIT_GAME_NEW_CALL_BACK, 0x1000);
    put_call(image, WIDGET_CALL_AT - EXIT_GAME_CTOR_CALL_BACK, 0x1100);
    put_call(image, WIDGET_CALL_AT - EXIT_GAME_ADD_CALL_BACK, 0x1200);
    put_call(image, WIDGET_CALL_AT, 0x1300);
    for (int limit = 0; limit < 2; ++limit) {
        const uint32_t at = limit ? LIMIT_B : LIMIT_A;
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
    static const uint8_t tick[6] = {0x83, 0xc0, 0x02, 0x41, 0xb4, 0x01};
    memcpy(image + TICK_AT, tick, sizeof(tick));
    sites->menu_tick = TICK_AT;
    sites->part_table_loads[0] = LOAD_A;
    sites->part_table_loads[1] = LOAD_B;
    sites->sheet_fill = FILL_AT;
    sites->row_widget = WIDGET_CALL_AT;
    sites->help_limits[0] = LIMIT_A;
    sites->help_limits[1] = LIMIT_B;
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
    CHECK(install(image, &sites) == 1);
    CHECK(image[FILL_AT] == 0xe9 && image[TICK_AT] == 0xe9 && image[WIDGET_CALL_AT] == 0xe9);
    CHECK(image[LIMIT_A] == EXIT_GAME_MENU_ITEMS && image[LIMIT_B] == EXIT_GAME_MENU_ITEMS);
    {
        /* every row part moves to its six-row variant, two part indexes lower; the title's own entries stay */
        int32_t table[38];
        memcpy(table, image + TABLE_AT, sizeof(table));
        CHECK(table[0] == 5 && table[1] == 8 && table[2] == 11);
        CHECK(table[3] == 13 && table[4] == 41 && table[7] == 134 && table[8] == 166 && table[14] == 169 && table[37] == 140);
    }
    CHECK(exit_game_hook_stub_bytes() >= EXIT_GAME_HOOK_COUNT);
    free(image);

    /* each site that does not match leaves the whole image as it was */
    for (int fault = 0; fault < 13; ++fault) {
        DcpExitGameSites faulty;
        image = new_image(&faulty);
        uint8_t *before = malloc(IMAGE_BYTES + STUB_ROOM);
        CHECK(image && before);
        if (!image || !before) return;
        switch (fault) {
        case 0: image[FILL_AT + 3] ^= 1; break;
        case 1: image[TICK_AT + 1] ^= 1; break;
        case 2: image[WIDGET_CALL_AT - EXIT_GAME_NEW_CALL_BACK] = 0x90; break;
        case 3: image[WIDGET_CALL_AT - EXIT_GAME_CTOR_CALL_BACK] = 0x90; break;
        case 4: image[WIDGET_CALL_AT - EXIT_GAME_ADD_CALL_BACK] = 0x90; break;
        case 5: image[WIDGET_CALL_AT] = 0x90; break;
        case 6: image[LIMIT_A] = 4; break;
        case 7: image[LIMIT_B - 1] = 0x3d; break;
        case 8: faulty.help_limits[1] = faulty.help_limits[0]; break;
        case 9: image[TABLE_AT + 4 * 8] = 169; break;
        case 12: image[TICK_AT] = 0x90; break;
        case 10: image[LOAD_B + 1] = 0x8b; break;
        case 11: put_u32(image + LOAD_B + 3, TABLE_AT + 4 - (LOAD_B + 7)); break;
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
    test_row_hook();
    test_selection();
    test_accept();
    test_sheet_hook();
    test_install();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("exit game tests passed\n");
    return 0;
}
