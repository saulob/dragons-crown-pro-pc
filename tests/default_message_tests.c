/* Focused tests of the translated character message preview: the class and category records, the preview that writes nothing,
 * the pick that commits what was previewed, the callers it must leave alone, and the all-or-nothing install on synthetic
 * and local boot images (read only). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "default_message_hook.h"

static int failures = 0;
#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            printf("FAIL line %d: %s\n", __LINE__, #cond);       \
            ++failures;                                          \
        }                                                        \
    } while (0)

enum {
    kImageSize = 0x20000,
    kStubArea = 0x4000,
    kPicker = 0x1000,
    kDefaults = 0x2000,
    kRows = 0x4000,
    kCharacterSize = 0x21d0,
    kStackSize = 0x800,
    kRowFrame = 0x600
};

static const int kBlockStart[DEFAULT_MESSAGE_CLASSES] = {1709, 1641, 1845, 1777, 1913, 1981};
static const char *const kClassNames[DEFAULT_MESSAGE_CLASSES] = {"Fighter", "Amazon", "Elf", "Dwarf", "Sorceress", "Wizard"};

static void test_records(void) {
    char seen[2049] = {0};
    int distinct = 0;
    for (int c = 0; c < DEFAULT_MESSAGE_CLASSES; ++c) {
        for (int k = 0; k < DEFAULT_MESSAGE_CATEGORIES; ++k) {
            for (int v = 0; v < DEFAULT_MESSAGE_VARIANTS; ++v) {
                const int record = default_message_record(c, (DefaultMessageCategory)k, v);
                CHECK(record > kBlockStart[c] && record < kBlockStart[c] + 67);
                if (record >= 0 && record < 2049 && !seen[record]) seen[record] = 1, ++distinct;
            }
        }
        CHECK(default_message_record(c, DEFAULT_MESSAGE_DEATH, 0) == kBlockStart[c] + 1);
        CHECK(default_message_record(c, DEFAULT_MESSAGE_DEATH, 14) == kBlockStart[c] + 15);
        CHECK(default_message_record(c, DEFAULT_MESSAGE_RESURRECTION, 0) == kBlockStart[c] + 18);
        CHECK(default_message_record(c, DEFAULT_MESSAGE_SELECTION, 0) == kBlockStart[c] + 35);
        CHECK(default_message_record(c, DEFAULT_MESSAGE_GREETING, 14) == kBlockStart[c] + 66);
    }
    CHECK(distinct == 360);
    CHECK(default_message_record(6, DEFAULT_MESSAGE_DEATH, 0) == -1 && default_message_record(-1, DEFAULT_MESSAGE_DEATH, 0) == -1);
    CHECK(default_message_record(0, DEFAULT_MESSAGE_CATEGORIES, 0) == -1 && default_message_record(0, DEFAULT_MESSAGE_DEATH, 15) == -1);
}

static void put(unsigned char *image, uint64_t at, const void *bytes, size_t length) {
    memcpy(image + at, bytes, length);
}

static void put_call(unsigned char *image, uint64_t at, uint64_t target) {
    const int32_t displacement = (int32_t)(target - (at + 5));
    image[at] = 0xe8;
    memcpy(image + at + 1, &displacement, 4);
}

static void put_row(unsigned char *image, uint64_t block, uint64_t getter, int call_to_jump, uint64_t label) {
    static const unsigned char load[] = {0x48, 0x8b, 0xbd, 0x98, 0xfc, 0xff, 0xff};
    static const unsigned char pattern[] = {0x48, 0x8d, 0xb5, 0x38, 0xfd, 0xff, 0xff, 0x8b, 0x00,
                                            0x41, 0x89, 0x87, 0xc0, 0x06, 0x00, 0x00, 0xe8};
    put(image, block, load, sizeof(load));
    put_call(image, block + 0x2c, getter);
    const uint64_t jump = block + 0x2c + (uint64_t)call_to_jump;
    const int32_t displacement = (int32_t)(label - (jump + 6));
    image[jump] = 0x0f;
    image[jump + 1] = 0x84;
    memcpy(image + jump + 2, &displacement, 4);
    put(image, label + 0x13, pattern, sizeof(pattern));
}

static void synthetic_image(unsigned char *image) {
    static const int32_t tables[5][8] = {{1761, 1693, 1897, 1829, 1965, 2033, 0, 0},
                                         {1744, 1676, 1880, 1812, 1948, 2016, 0, 0},
                                         {1727, 1659, 1863, 1795, 1931, 1999, 0, 0},
                                         {1709, 1641, 1845, 1777, 1913, 1981, 0, 0},
                                         {1725, 1657, 1861, 1793, 1929, 1997, 0, 0}};
    static const unsigned char start[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53};
    static const unsigned char pick[] = {0x31, 0xd2, 0xf7, 0xf3, 0x4d, 0x85, 0xff, 0x41, 0x89, 0xd5};
    static const unsigned char arguments[] = {0x49, 0x89, 0xfd, 0x41, 0x89, 0xf6};
    static const unsigned char after[] = {0x45, 0x84, 0xf6};
    memset(image, 0xcc, kImageSize);
    put(image, 0x100, tables, sizeof(tables));
    put(image, kPicker, start, sizeof(start));
    put_call(image, kPicker + 0xab, 0x800);
    put(image, kPicker + 0xb0, pick, sizeof(pick));
    put(image, kDefaults + 0x1f, arguments, sizeof(arguments));
    put_call(image, kDefaults + 0xc0, kPicker);
    put(image, kDefaults + 0xc5, after, sizeof(after));
    put_call(image, 0x3000, kPicker); /* a display fallback: not followed by the flag test */
    put_row(image, kRows, 0xa00, 23, kRows + 0x100);
    put_row(image, kRows + 0x200, 0x900, 20, kRows + 0x300);
    put_row(image, kRows + 0x400, 0x880, 20, kRows + 0x500);
    put_row(image, kRows + 0x600, 0x810, 20, kRows + 0x700);
}

static const char *numbered_text(unsigned record) {
    static char text[32];
    snprintf(text, sizeof(text), "Sa\xC3\xBA" "de %u", record);
    return text;
}

static const char *missing_text(unsigned record) {
    return record == 2047 ? NULL : numbered_text(record);
}

static const char *long_text(unsigned record) {
    (void)record;
    return "0123456789012345678901234567890123456789";
}

typedef struct {
    uint64_t allocator;
    const uint16_t *data;
    uint64_t unused;
    uint64_t size;
    uint64_t capacity;
} ReadText;

static unsigned char character[kCharacterSize];
static unsigned char stack[kStackSize];

static void *const *row_frame_slot(void) {
    return (void *const *)(stack + kRowFrame - 0x368);
}

static GuestRegs row_regs(const void *edited) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rbp = (uint64_t)(uintptr_t)(stack + kRowFrame);
    regs.rsi = 0x1234;
    memcpy((void *)row_frame_slot(), &edited, sizeof(edited));
    return regs;
}

/* The picker's frame as the pick sees it, with the same result a real call to the picker would reduce. */
static GuestRegs pick_regs(uint64_t *frame, uint64_t back, uint64_t flag, const void *added, uint32_t guest_rand) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    memset(frame, 0, 8 * sizeof(*frame));
    frame[5] = back;
    frame[2] = flag;
    frame[1] = (uint64_t)(uintptr_t)added;
    regs.rbp = (uint64_t)(uintptr_t)(frame + 4);
    regs.rax = guest_rand;
    regs.rbx = DEFAULT_MESSAGE_VARIANTS;
    return regs;
}

static const ReadText *previewed(int character_class, DefaultMessageCategory category) {
    character[0x27] = (unsigned char)character_class;
    GuestRegs regs = row_regs(character);
    default_message_preview(&regs, category);
    return regs.rsi == 0x1234 ? NULL : (const ReadText *)(uintptr_t)regs.rsi;
}

static void units_of(const ReadText *text, char *out, size_t capacity) {
    size_t i = 0;
    for (; i + 1 < capacity && i < text->size; ++i) out[i] = (char)text->data[i];
    out[i] = 0;
}

static int variant_previewed(int character_class) {
    const ReadText *death = previewed(character_class, DEFAULT_MESSAGE_DEATH);
    char text[32];
    if (!death) return -1;
    units_of(death, text, sizeof(text));
    const int record = atoi(strchr(text, ' ') + 1);
    return record - default_message_record(character_class, DEFAULT_MESSAGE_DEATH, 0);
}

static void test_preview_and_commit(unsigned char *image, const DefaultMessageSites *sites) {
    const uint64_t back = (uint64_t)(uintptr_t)(image + sites->picker_return);
    uint64_t frame[8];
    for (int c = 0; c < DEFAULT_MESSAGE_CLASSES; ++c) {
        memset(character, 0, sizeof(character));
        const int variant = variant_previewed(c);
        CHECK(variant >= 0 && variant < DEFAULT_MESSAGE_VARIANTS);
        static unsigned char before[kCharacterSize];
        memcpy(before, character, sizeof(before));
        const ReadText *rows[DEFAULT_MESSAGE_CATEGORIES];
        for (int k = 0; k < DEFAULT_MESSAGE_CATEGORIES; ++k) {
            rows[k] = previewed(c, (DefaultMessageCategory)k);
            CHECK(rows[k] != NULL);
            if (!rows[k]) continue;
            CHECK(rows[k]->size > 0 && rows[k]->size <= DEFAULT_MESSAGE_MAX_UNITS && rows[k]->capacity >= 8);
            CHECK(rows[k]->data[rows[k]->size] == 0);
            CHECK(rows[k]->data[2] == 0xfa); /* the accent decoded to one unit */
            char text[32], expected[32];
            units_of(rows[k], text, sizeof(text));
            const int record = default_message_record(c, (DefaultMessageCategory)k, variant);
            snprintf(expected, sizeof(expected), "Sa%cde %d", 0xfa, record);
            CHECK(!strcmp(text, expected));
            for (int other = 0; other < k; ++other) CHECK(rows[other] != rows[k]);
        }
        CHECK(!memcmp(before, character, sizeof(before)));
        CHECK(variant_previewed(c) == variant);

        for (int rand_value = 0; rand_value < 3; ++rand_value) {
            if (rand_value) variant_previewed(c);
            GuestRegs regs = pick_regs(frame, back, 1, character, 0x7fff0000u + (uint32_t)rand_value);
            default_message_pin(&regs);
            const int committed = (int)((uint32_t)regs.rax % (uint32_t)regs.rbx);
            CHECK(committed == (rand_value ? (int)regs.rax : variant));
            CHECK(regs.rax < DEFAULT_MESSAGE_VARIANTS);
        }
        printf("  %s: four rows previewed and committed\n", kClassNames[c]);
    }

    memset(character, 0, sizeof(character));
    const int variant = variant_previewed(1);
    GuestRegs regs = pick_regs(frame, back + 1, 1, character, 77);
    default_message_pin(&regs);
    CHECK(regs.rax == 77); /* the picker's other callers keep their random text */
    regs = pick_regs(frame, back, 0, character, 77);
    default_message_pin(&regs);
    CHECK(regs.rax == 77); /* generated characters */
    for (int i = 0; i < DEFAULT_MESSAGE_CATEGORIES; ++i) character[0x1f70 + i * 0x78] = 'x';
    regs = pick_regs(frame, back, 1, character, 77);
    default_message_pin(&regs);
    CHECK(regs.rax == 77); /* nothing left to fill */
    character[0x21a8] = 0x80;
    regs = pick_regs(frame, back, 1, character, 77);
    default_message_pin(&regs);
    CHECK(regs.rax == (uint64_t)variant); /* the alternate block is the one in use, and it is empty */
    regs = pick_regs(frame, back, 1, character, 77);
    default_message_pin(&regs);
    CHECK(regs.rax == 77); /* one commit, one pin */

    memset(character, 0, sizeof(character));
    const int held = variant_previewed(5);
    character[0x1f70] = 'x'; /* a typed message keeps its field: the empty ones take the previewed variant */
    regs = pick_regs(frame, back, 1, character, 77);
    default_message_pin(&regs);
    CHECK(regs.rax == (uint64_t)held);

    character[0x27] = 0xff;
    regs = row_regs(character);
    default_message_preview(&regs, DEFAULT_MESSAGE_DEATH);
    CHECK(regs.rsi == 0x1234);
    character[0x27] = 6;
    regs = row_regs(character);
    default_message_preview(&regs, DEFAULT_MESSAGE_DEATH);
    CHECK(regs.rsi == 0x1234);
    regs = row_regs(NULL);
    default_message_preview(&regs, DEFAULT_MESSAGE_DEATH);
    CHECK(regs.rsi == 0x1234);
    regs = pick_regs(frame, back, 1, character, 77);
    default_message_pin(&regs);
    CHECK(regs.rax == 77);
}

static void test_synthetic_install(void) {
    unsigned char *memory = malloc(kImageSize + kStubArea);
    CHECK(memory != NULL);
    if (!memory) return;
    unsigned char *copy = malloc(kImageSize);
    CHECK(default_message_stub_bytes() <= kStubArea);
    synthetic_image(memory);
    memcpy(copy, memory, kImageSize);

    DefaultMessageSites sites;
    CHECK(default_message_locate(memory, kImageSize, &sites));
    CHECK(sites.rows[0] == kRows + 0x100 + 0x13 + 7 && sites.rows[3] == kRows + 0x700 + 0x13 + 7);
    CHECK(sites.pick == kPicker + 0xb0 && sites.picker_return == kDefaults + 0xc5);

    CHECK(!default_message_install(memory, kImageSize, 0, missing_text, memory + kImageSize, kStubArea));
    CHECK(!memcmp(memory, copy, kImageSize));
    CHECK(!default_message_install(memory, kImageSize, 0, numbered_text, memory + kImageSize, 8));
    CHECK(!memcmp(memory, copy, kImageSize));

    memory[0x100] ^= 1; /* the record tables moved */
    CHECK(!default_message_locate(memory, kImageSize, &sites));
    CHECK(!default_message_install(memory, kImageSize, 0, numbered_text, memory + kImageSize, kStubArea));
    memory[0x100] ^= 1;
    memory[kDefaults + 0xc6] ^= 1; /* no call from the character defaults */
    CHECK(!default_message_locate(memory, kImageSize, &sites));
    memory[kDefaults + 0xc6] ^= 1;
    memory[kRows + 0x700 + 0x13] ^= 1; /* a row is missing */
    CHECK(!default_message_locate(memory, kImageSize, &sites));
    memory[kRows + 0x700 + 0x13] ^= 1;
    const int32_t swapped = 0xa00 - (kRows + 0x62c + 5);
    memcpy(memory + kRows + 0x62d, &swapped, 4); /* the rows are not in the known order */
    CHECK(!default_message_locate(memory, kImageSize, &sites));
    memcpy(memory, copy, kImageSize);

    CHECK(default_message_locate(memory, kImageSize, &sites));
    const DefaultMessageSites located = sites;
    CHECK(default_message_install(memory, kImageSize, 0, numbered_text, memory + kImageSize, kStubArea));
    CHECK(memory[located.rows[0]] == 0xe9 && memory[located.pick] == 0xe9);
    test_preview_and_commit(memory, &located);

    memcpy(memory, copy, kImageSize);
    CHECK(default_message_install(memory, kImageSize, 0, long_text, memory + kImageSize, kStubArea));
    memset(character, 0, sizeof(character));
    const ReadText *row = previewed(2, DEFAULT_MESSAGE_GREETING);
    CHECK(row && row->size == DEFAULT_MESSAGE_MAX_UNITS && row->data[DEFAULT_MESSAGE_MAX_UNITS] == 0);
    free(copy);
    free(memory);
}

static uint64_t read64(FILE *f) {
    unsigned char b[8] = {0};
    if (fread(b, 1, 8, f) != 8) return 0;
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = v << 8 | b[i];
    return v;
}

/* The memory image inside build/image/<folder>/boot.bin (layout written by launcher/boot_image.cpp). */
static unsigned char *boot_image(const char *folder, uint64_t *size) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/build/image/%s/boot.bin", DCP_SOURCE_DIR, folder);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "DCPBOOT1", 8)) return fclose(f), NULL;
    *size = read64(f);
    read64(f);
    const uint64_t segments = read64(f), relocs = read64(f), names = read64(f);
    read64(f), read64(f);
    for (int i = 0; i < 4; ++i) read64(f);
    const uint64_t modules = read64(f);
    fseek(f, (long)(modules * 56), SEEK_CUR);
    const uint64_t bindings = read64(f);
    fseek(f, (long)(bindings * 24 + segments * 24 + names * 128 + relocs * 32), SEEK_CUR);
    unsigned char *image = malloc(*size + kStubArea);
    if (image && fread(image, 1, *size, f) != *size) free(image), image = NULL;
    fclose(f);
    return image;
}

static const struct {
    const char *folder;
    uint64_t rows[DEFAULT_MESSAGE_CATEGORIES], pick, picker_return;
} kRealBuilds[] = {
    {"v1.03", {0x3a1346, 0x3a13b7, 0x3a1462, 0x3a150d}, 0x4a2a0, 0x87ee5},
    {"CUSA07001-1.00", {0x3a07a6, 0x3a0817, 0x3a08aa, 0x3a093d}, 0x49bf0, 0x875c5},
    {"CUSA07001-1.02", {0x3a0be6, 0x3a0c57, 0x3a0cea, 0x3a0d7d}, 0x49c70, 0x87865},
    {"CUSA07001-1.03", {0x3a0c36, 0x3a0ca7, 0x3a0d3a, 0x3a0dcd}, 0x49c70, 0x87865},
    {"CUSA10209-1.00", {0x3a0916, 0x3a0987, 0x3a0a1a, 0x3a0aad}, 0x49cb0, 0x87735},
};

static void test_real_images(void) {
    for (size_t i = 0; i < sizeof(kRealBuilds) / sizeof(*kRealBuilds); ++i) {
        uint64_t size = 0;
        unsigned char *image = boot_image(kRealBuilds[i].folder, &size);
        if (!image) {
            printf("  %s: no local boot image\n", kRealBuilds[i].folder);
            continue;
        }
        DefaultMessageSites sites;
        CHECK(default_message_locate(image, size, &sites));
        CHECK(!memcmp(sites.rows, kRealBuilds[i].rows, sizeof(sites.rows)));
        CHECK(sites.pick == kRealBuilds[i].pick && sites.picker_return == kRealBuilds[i].picker_return);
        CHECK(default_message_install(image, size, 0, numbered_text, image + size, kStubArea));
        printf("  %s: rows, pick and records located\n", kRealBuilds[i].folder);
        free(image);
    }
}

int main(void) {
    test_records();
    test_synthetic_install();
    test_real_images();
    if (failures) {
        printf("%d default message checks failed\n", failures);
        return 1;
    }
    puts("Default message tests passed");
    return 0;
}
