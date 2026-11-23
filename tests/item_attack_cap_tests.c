/* Focused tests of the Item Attack Cap patch: synthetic images for every case, and the real boot images under build/image
 * when they are there (read only). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "item_attack_cap.h"

static int failures = 0;
#define CHECK(cond)                                        \
    do {                                                   \
        if (!(cond)) {                                     \
            printf("FAIL line %d: %s\n", __LINE__, #cond); \
            ++failures;                                    \
        }                                                  \
    } while (0)

static const unsigned char kWindow[ITEM_ATTACK_CAP_WINDOW_BYTES] = {
    0x41, 0x0f, 0xb7, 0x54, 0x24, 0x50, 0xb9, 0x62, 0x00, 0x00, 0x00, 0x89, 0xd0, 0xc1, 0xe8, 0x08, 0x83, 0xfb, 0x62, 0x0f,
    0x42, 0xcb, 0x31, 0xf6, 0x84, 0xdb, 0x89, 0xc9, 0x48, 0x0f, 0x45, 0xf1, 0x48, 0x6b, 0xce, 0x58, 0x48, 0x8d, 0x35, 0x00,
    0x00, 0x00, 0x00, 0x8a, 0x0c, 0x31, 0x38, 0xca, 0x76, 0x05, 0x41, 0x88, 0x4c, 0x24, 0x50, 0x38, 0xc8, 0x0f, 0x86, 0x00,
    0x00, 0x00, 0x00, 0x41, 0x88, 0x4c, 0x24, 0x51, 0xe9, 0x00, 0x00, 0x00, 0x00,
};
static const unsigned char kNop5[5] = {0x0f, 0x1f, 0x44, 0x00, 0x00};

enum { kWindowAt = 0x239a8, kTableAt = 0x69a900, kSize = 0x700000, kRow = 0x58, kLeaEnd = 43 };

static void put_table(unsigned char *image, uint64_t at, uint64_t table) {
    image[table] = 5;
    image[table + kRow] = 7;
    image[table + 98 * kRow] = 255;
    const int32_t disp = (int32_t)((int64_t)table - (int64_t)(at + kLeaEnd));
    memcpy(image + at + kLeaEnd - 4, &disp, sizeof(disp));
}

static unsigned char *build_image(uint64_t at) {
    unsigned char *image = calloc(1, kSize);
    memcpy(image + at, kWindow, sizeof(kWindow));
    put_table(image, at, kTableAt);
    return image;
}

static size_t differing(const unsigned char *a, const unsigned char *b, uint64_t size) {
    size_t count = 0;
    for (uint64_t i = 0; i < size; ++i) count += a[i] != b[i];
    return count;
}

static void test_synthetic(void) {
    unsigned char *image = build_image(kWindowAt);
    unsigned char *original = build_image(kWindowAt);
    uint64_t window = 1;
    CHECK(item_attack_cap_locate(image, kSize, NULL, &window) == 0 && window == 0);
    CHECK(item_attack_cap_locate(image, kSize, "CUSA00000-1.00", &window) == 0 && window == 0);
    CHECK(item_attack_cap_locate(image, kSize, "CUSA10454-1.00", &window) == 1 && window == kWindowAt);
    CHECK(item_attack_cap_install(image, kSize, NULL) == 0 && !memcmp(image, original, kSize));
    CHECK(item_attack_cap_install(image, kSize, "CUSA00000-1.00") == 0 && !memcmp(image, original, kSize));

    /* The two stores of the clamp are the attack pair's byte writes; only they change, the compares and jumps stay. */
    CHECK(original[kWindowAt + ITEM_ATTACK_CAP_STORE_MIN + 4] == 0x50 && original[kWindowAt + ITEM_ATTACK_CAP_STORE_MAX + 4] == 0x51);
    CHECK(item_attack_cap_install(image, kSize, "CUSA10454-1.00") == 1);
    CHECK(!memcmp(image + kWindowAt + ITEM_ATTACK_CAP_STORE_MIN, kNop5, 5) && !memcmp(image + kWindowAt + ITEM_ATTACK_CAP_STORE_MAX, kNop5, 5));
    CHECK(differing(image, original, kSize) == 10);
    CHECK(!memcmp(image + kWindowAt, original + kWindowAt, ITEM_ATTACK_CAP_STORE_MIN));
    CHECK(!memcmp(image + kWindowAt + 55, original + kWindowAt + 55, 8));
    CHECK(!memcmp(image + kTableAt, original + kTableAt, 99 * kRow));
    /* A patched image no longer matches: nothing is written twice. */
    CHECK(item_attack_cap_locate(image, kSize, "CUSA10454-1.00", &window) == 1 && window == 0);
    CHECK(item_attack_cap_install(image, kSize, "CUSA10454-1.00") == 0);

    /* One byte off in the window, or a table that is not the level caps: refused, nothing written. */
    unsigned char *changed = build_image(kWindowAt);
    changed[kWindowAt + 20] ^= 1;
    CHECK(item_attack_cap_install(changed, kSize, "CUSA10454-1.00") == 0 && changed[kWindowAt + 50] == 0x41);
    unsigned char *wrong_table = build_image(kWindowAt);
    wrong_table[kTableAt] = 6;
    CHECK(item_attack_cap_install(wrong_table, kSize, "CUSA10454-1.00") == 0 && wrong_table[kWindowAt + 50] == 0x41);
    unsigned char *short_image = build_image(kWindowAt);
    CHECK(item_attack_cap_install(short_image, kWindowAt + 20, "CUSA10454-1.00") == 0);
    free(image), free(original), free(changed), free(wrong_table), free(short_image);
}

/* A supported build without a known site: the window is searched for and used only when it appears once. */
static void test_signature(void) {
    unsigned char *image = build_image(0x25000);
    uint64_t window = 0;
    CHECK(item_attack_cap_locate(image, kSize, "CUSA10454-1.00", &window) == 1 && window == 0); /* its own site does not match */
    CHECK(item_attack_cap_locate(image, kSize, "CUSA10209-1.02", &window) == 1 && window == 0x25000);
    CHECK(item_attack_cap_locate(image, kSize, "CUSA10487-1.03", &window) == 1 && window == 0x25000);
    CHECK(item_attack_cap_install(image, kSize, "CUSA10487-1.03") == 1);
    CHECK(!memcmp(image + 0x25000 + ITEM_ATTACK_CAP_STORE_MIN, kNop5, 5));
    free(image);

    unsigned char *twice = build_image(0x25000);
    memcpy(twice + 0x30000, kWindow, sizeof(kWindow));
    put_table(twice, 0x30000, kTableAt);
    CHECK(item_attack_cap_locate(twice, kSize, "CUSA10209-1.02", &window) == 1 && window == 0);
    CHECK(item_attack_cap_install(twice, kSize, "CUSA10209-1.02") == 0 && twice[0x25000 + 50] == 0x41);
    free(twice);

    unsigned char *none = calloc(1, kSize);
    CHECK(item_attack_cap_locate(none, kSize, "CUSA10209-1.02", &window) == 1 && window == 0);
    free(none);
}

static uint64_t read64(FILE *f) {
    unsigned char b[8];
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
    if (!f) {
        snprintf(path, sizeof(path), "%s/build/research/image/%s/boot.bin", DCP_SOURCE_DIR, folder);
        f = fopen(path, "rb");
    }
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
    unsigned char *image = malloc(*size);
    if (image && fread(image, 1, *size, f) != *size) free(image), image = NULL;
    fclose(f);
    return image;
}

static void test_real_images(void) {
    static const struct {
        const char *folder, *game;
        uint64_t window;
    } kBuilds[] = {
        {"v1.03", "CUSA10454-1.00", 0x239a8},
        {"CUSA10454-1.01", "CUSA10454-1.01", 0x239a8},
        {"CUSA07001-1.00", "CUSA07001-1.00", 0x236e8},
        {"CUSA07001-1.02", "CUSA07001-1.02", 0x23708},
        {"CUSA07001-1.03", "CUSA07001-1.03", 0x23708},
        {"CUSA10209-1.00", "CUSA10209-1.00", 0x235c8},
    };
    for (size_t i = 0; i < sizeof(kBuilds) / sizeof(*kBuilds); ++i) {
        uint64_t size = 0;
        unsigned char *image = boot_image(kBuilds[i].folder, &size);
        if (!image) {
            printf("no boot image for %s: skipped\n", kBuilds[i].folder);
            continue;
        }
        /* The build's own site matches and the search finds the same one. */
        uint64_t window = 0;
        CHECK(item_attack_cap_locate(image, size, kBuilds[i].game, &window) == 1 && window != 0);
        CHECK(kBuilds[i].window == 0 || window == kBuilds[i].window);
        const uint64_t site = window;
        window = 0;
        CHECK(item_attack_cap_locate(image, size, "CUSA10487-1.03", &window) == 1 && window == site);
        unsigned char *before = malloc(size);
        memcpy(before, image, size);
        CHECK(item_attack_cap_install(image, size, kBuilds[i].game) == 1);
        CHECK(differing(image, before, size) == 10); /* each 5-byte store becomes one nop */
        CHECK(!memcmp(image + site + ITEM_ATTACK_CAP_STORE_MIN, kNop5, 5));
        CHECK(!memcmp(image + site + ITEM_ATTACK_CAP_STORE_MAX, kNop5, 5));
        printf("%s: %zu bytes changed\n", kBuilds[i].folder, differing(image, before, size));
        free(image), free(before);
    }
}

int main(void) {
    test_synthetic();
    test_signature();
    test_real_images();
    printf("%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
