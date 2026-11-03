/* Focused tests of Japanese Menu Buttons: the locator and the install on synthetic images, and on the real boot images
 * under build/image when they are there (read only). What the menus do with the layout needs a game run. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "menu_buttons.h"

static int failures = 0;
#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            printf("FAIL line %d: %s\n", __LINE__, #cond);       \
            ++failures;                                          \
        }                                                        \
    } while (0)

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
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

static void test_environment(void) {
    set_env("DCP_JAPANESE_MENU_BUTTONS", NULL);
    CHECK(menu_buttons_wanted(0) == 0 && menu_buttons_wanted(1) == 1);
    set_env("DCP_JAPANESE_MENU_BUTTONS", "1");
    CHECK(menu_buttons_wanted(0) == 1);
    set_env("DCP_JAPANESE_MENU_BUTTONS", "off");
    CHECK(menu_buttons_wanted(1) == 0);
    set_env("DCP_JAPANESE_MENU_BUTTONS", "maybe");
    CHECK(menu_buttons_wanted(1) == 1 && menu_buttons_wanted(0) == 0);
    set_env("DCP_JAPANESE_MENU_BUTTONS", NULL);
}

enum { kSize = 0x600, kGetter = 0x00, kReader = 0x40, kCircle = 0x120, kWriteA = 0x200, kWriteB = 0x300 };
enum { kReaderLength = 54, kCrossBranch = kReader + 134, kWriteALength = 52, kWriteBLength = 38 };
enum { kPrompt0 = 0x400, kPrompt1 = 0x420, kPrompt2 = 0x440, kLoad = 0x460, kPromptCompare = 13, kLoadSite = 5 + 15 };

static const unsigned char kReaderBytes[kReaderLength] = {
    0x8b, 0x93, 0xa0, 0x00, 0x00, 0x00, 0x83, 0xb8, 0x34, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x8b, 0xa0, 0x00, 0x00, 0x00,
    0x74, 0x17, 0x83, 0xfa, 0x18, 0x74, 0x6b, 0x83, 0xbb, 0xa4, 0x00, 0x00, 0x00, 0x18, 0x75, 0x27, 0x48, 0x8d, 0x8b, 0xa4,
    0x00, 0x00, 0x00, 0xeb, 0x59, 0x83, 0xfa, 0x18, 0x0f, 0x84, 0xaa, 0x00, 0x00, 0x00,
};
static const unsigned char kCrossBytes[12] = {0xc7, 0x01, 0x06, 0x00, 0x00, 0x00, 0xfe, 0x83, 0xb5, 0x00, 0x00, 0x00};
static const unsigned char kCircleBytes[12] = {0xc7, 0x01, 0x05, 0x00, 0x00, 0x00, 0xfe, 0x83, 0xb5, 0x00, 0x00, 0x00};
static const unsigned char kWriteABytes[kWriteALength] = {
    0x41, 0xc7, 0x84, 0x24, 0x30, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x41, 0xc7, 0x84, 0x24, 0x34, 0x01,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xbe, 0x01, 0x00, 0x00, 0x00, 0x4c, 0x89, 0xe7, 0x41, 0x8a, 0x44, 0x24,
    0x5c, 0x49, 0x8b, 0x0c, 0x24, 0xfe, 0xc0, 0x0f, 0xb6, 0xd0, 0xff, 0x91, 0xa0, 0x00, 0x00, 0x00,
};
static const unsigned char kWriteBBytes[kWriteBLength] = {
    0xc7, 0x83, 0x30, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc7, 0x83, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x49, 0x8b, 0x06, 0x48, 0x3b, 0x45, 0xe8, 0x75, 0x09, 0x48, 0x83, 0xc4, 0x10, 0x5b, 0x41, 0x5e,
    0x5d, 0xc3,
};
static const unsigned char kPromptBytes[] = {0x31, 0xc9, 0x83, 0xb8, 0x34, 0x01, 0x00, 0x00, 0x00, 0x90, 0x90, 0x0f, 0x95, 0xc1};
static const unsigned char kLoadBytes[] = {0x41, 0xbc, 0xff, 0xff, 0xff, 0xff, 0x45, 0x85, 0xff, 0x0f, 0x88, 0x00, 0x00, 0x00,
                                           0x00, 0x8b, 0x80, 0x34, 0x01, 0x00, 0x00};
static const unsigned char kLoadWesternBytes[] = {0xb8, 0x01, 0x00, 0x00, 0x00, 0x90};
enum { kImmediateA = kWriteA + 20, kImmediateB = kWriteB + 16 };

static void put_call(unsigned char *image, int at) {
    image[at] = 0xe8;
    const int32_t rel = (int32_t)(kGetter - (at + 5));
    memcpy(image + at + 1, &rel, sizeof(rel));
}

/* A stand-in image: the key setup with its getter and branches, the other readers, and the two stores when `writers`. */
static unsigned char *synthetic(int writers) {
    unsigned char *image = malloc(kSize);
    memset(image, 0x90, kSize);
    static const unsigned char getter[] = {0x48, 0x8b, 0x05, 0x10, 0x00, 0x00, 0x00, 0xc3};
    memcpy(image + kGetter, getter, sizeof(getter));
    put_call(image, kReader - 5);
    memcpy(image + kReader, kReaderBytes, sizeof(kReaderBytes));
    memcpy(image + kCrossBranch, kCrossBytes, sizeof(kCrossBytes));
    memcpy(image + kCircle, kCircleBytes, sizeof(kCircleBytes));
    const int prompts[] = {kPrompt0, kPrompt1, kPrompt2};
    for (int i = 0; i < 3; ++i) {
        put_call(image, prompts[i]);
        memcpy(image + prompts[i] + 5, kPromptBytes, sizeof(kPromptBytes));
    }
    put_call(image, kLoad);
    memcpy(image + kLoad + 5, kLoadBytes, sizeof(kLoadBytes));
    if (writers) {
        memcpy(image + kWriteA, kWriteABytes, sizeof(kWriteABytes));
        memcpy(image + kWriteB, kWriteBBytes, sizeof(kWriteBBytes));
    }
    return image;
}

static size_t differences(const unsigned char *a, const unsigned char *b, uint64_t size) {
    size_t count = 0;
    for (uint64_t i = 0; i < size; ++i) count += a[i] != b[i];
    return count;
}

/* What the readers of the flag see: 1 for Cross confirming. A forced compare or load wins over the stored value. */
static int effective_flag(const unsigned char *image, const MenuButtonSites *sites) {
    const int forced_compare = image[sites->compares[0]] == 0xff;
    const int forced_load = !memcmp(image + sites->load, kLoadWesternBytes, sizeof(kLoadWesternBytes));
    CHECK(forced_compare == forced_load);
    for (int i = 1; i < MENU_BUTTONS_COMPARES; ++i) CHECK((image[sites->compares[i]] == 0xff) == forced_compare);
    if (forced_compare) return 1;
    return sites->write_count ? image[sites->writes[0]] : 0;
}

static void test_locate_synthetic(void) {
    MenuButtonSites sites;
    unsigned char *western = synthetic(1);
    CHECK(menu_buttons_locate(western, kSize, "CUSA10454-1.01", &sites) == 0);
    CHECK(sites.reader == kReader && sites.write_count == 2 && sites.western_sites);
    CHECK(sites.writes[0] == kImmediateA && sites.writes[1] == kImmediateB);
    CHECK(sites.compares[0] == kReader + 12 && sites.compares[1] == kPrompt0 + kPromptCompare &&
          sites.compares[2] == kPrompt1 + kPromptCompare && sites.compares[3] == kPrompt2 + kPromptCompare);
    CHECK(sites.load == kLoad + kLoadSite);

    unsigned char *native = synthetic(0);
    CHECK(menu_buttons_locate(native, kSize, "CUSA10209-1.02", &sites) == 0);
    CHECK(sites.reader == kReader && sites.write_count == 0 && sites.western_sites);

    CHECK(menu_buttons_locate(western, kSize, NULL, &sites) == 0 && !sites.reader && !sites.write_count);
    CHECK(menu_buttons_locate(western, kSize, "CUSA10454-1.00", &sites) == 1 && !sites.reader && !sites.write_count);
    CHECK(menu_buttons_locate(native, kSize, "CUSA07001-1.03", &sites) == 1 && !sites.reader);
    free(western);
    free(native);
}

static void test_install_synthetic(void) {
    MenuButtonSites sites;
    unsigned char *image = synthetic(1), *before = synthetic(1);
    CHECK(menu_buttons_locate(before, kSize, "CUSA10454-1.01", &sites) == 0);
    CHECK(effective_flag(before, &sites) == 1);
    CHECK(menu_buttons_install(image, kSize, NULL, 1) == 0 && !memcmp(image, before, kSize));
    CHECK(menu_buttons_install(image, kSize, NULL, 0) == 0 && !memcmp(image, before, kSize));
    CHECK(menu_buttons_install(image, kSize, "CUSA10454-1.01", 0) == 1);
    CHECK(!memcmp(image, before, kSize));
    CHECK(menu_buttons_install(image, kSize, "CUSA10454-1.01", 1) == 1);
    CHECK(differences(image, before, kSize) == 2 && image[kImmediateA] == 0 && image[kImmediateB] == 0);
    CHECK(effective_flag(image, &sites) == 0);
    CHECK(menu_buttons_install(image, kSize, "CUSA10454-1.01", 1) == 1 && differences(image, before, kSize) == 2);
    free(image);
    free(before);

    unsigned char *native = synthetic(0), *native_before = synthetic(0);
    CHECK(menu_buttons_locate(native_before, kSize, "CUSA10209-1.02", &sites) == 0);
    CHECK(menu_buttons_install(native, kSize, "CUSA10209-1.02", 1) == 1);
    CHECK(!memcmp(native, native_before, kSize));
    CHECK(effective_flag(native, &sites) == 0);
    CHECK(menu_buttons_install(native, kSize, "CUSA10209-1.02", 0) == 1);
    CHECK(effective_flag(native, &sites) == 1);
    CHECK(differences(native, native_before, kSize) == 4 + 5);
    CHECK(menu_buttons_install(native, kSize, "CUSA10209-1.02", 0) == 1 && effective_flag(native, &sites) == 1);
    CHECK(menu_buttons_install(native, kSize, "CUSA10209-1.02", 1) == 1);
    free(native);
    free(native_before);
}

/* One byte of the code or its sites changed: nothing is written, whichever site the mismatch is next to. */
static void check_untouched(unsigned char *image, uint64_t size, int japanese) {
    unsigned char *copy = malloc(size);
    memcpy(copy, image, size);
    CHECK(menu_buttons_install(image, size, "CUSA10454-1.01", japanese) == 0);
    CHECK(!memcmp(image, copy, size));
    free(copy);
}

static void test_mismatch_writes_nothing(void) {
    unsigned char *original = synthetic(1);
    const int spots[] = {kGetter, kReader - 5, kReader, kReader + 9, kReader + 26, kReader + 50, kCrossBranch + 2,
                         kCircle + 2, kWriteA + 4, kWriteA + 24, kWriteA + 51, kWriteB + 6, kWriteB + 16 + 1, kWriteB + 37};
    for (size_t i = 0; i < sizeof(spots) / sizeof(*spots); ++i) {
        unsigned char *image = synthetic(1);
        image[spots[i]] ^= 0x40;
        check_untouched(image, kSize, 1);
        free(image);
    }

    const int western_spots[] = {kPrompt0 + 5, kPrompt1 + kPromptCompare, kPrompt2 + 5, kPrompt2 + 5 + 12, kLoad + 5,
                                 kLoad + kLoadSite, kLoad + kLoadSite + 2, kLoad + 5 + 9};
    for (size_t i = 0; i < sizeof(western_spots) / sizeof(*western_spots); ++i) {
        unsigned char *image = synthetic(0);
        image[western_spots[i]] ^= 0x40;
        MenuButtonSites sites;
        menu_buttons_locate(image, kSize, "CUSA10209-1.02", &sites);
        CHECK(sites.reader && !sites.western_sites);
        check_untouched(image, kSize, 0);
        unsigned char *copy = malloc(kSize);
        memcpy(copy, image, kSize);
        CHECK(menu_buttons_install(image, kSize, "CUSA10209-1.02", 1) == 1 && !memcmp(image, copy, kSize));
        free(copy);
        free(image);
    }

    unsigned char *fourth = synthetic(0);
    put_call(fourth, 0x480);
    memcpy(fourth + 0x485, kPromptBytes, sizeof(kPromptBytes));
    check_untouched(fourth, kSize, 0);
    free(fourth);

    unsigned char *only_one = synthetic(1);
    memset(only_one + kWriteB, 0x90, kWriteBLength);
    check_untouched(only_one, kSize, 1);
    check_untouched(only_one, kSize, 0);

    unsigned char *twice = malloc(kSize * 2);
    memcpy(twice, original, kSize);
    memcpy(twice + kSize, original, kSize);
    unsigned char *twice_before = malloc(kSize * 2);
    memcpy(twice_before, twice, kSize * 2);
    CHECK(menu_buttons_install(twice, kSize * 2, "CUSA10454-1.01", 1) == 0 && !memcmp(twice, twice_before, kSize * 2));

    unsigned char *extra = synthetic(1);
    memcpy(extra + 0x380, "\xc7\x83\x34\x01\x00\x00\x01\x00\x00\x00", 10);
    check_untouched(extra, kSize, 1);
    check_untouched(extra, kSize, 0);

    unsigned char *none = malloc(kSize);
    memset(none, 0x90, kSize);
    CHECK(menu_buttons_install(none, kSize, "CUSA10454-1.01", 1) == 0);
    CHECK(menu_buttons_install(none, 0, "CUSA10454-1.01", 0) == 0);

    free(original);
    free(only_one);
    free(twice);
    free(twice_before);
    free(extra);
    free(none);
}

static const struct {
    const char *folder, *game;
    uint64_t reader;
    int writers;
    uint64_t writes[2];
    uint64_t compares[3], load;
} kRealBuilds[] = {
    {"v1.03", "CUSA10454-1.00", 0x7ee45, 2, {0x176c8, 0x177e0}, {0x4f3983, 0x4f43b6, 0x508b5a}, 0x6b20b},
    {"CUSA07001-1.00", "CUSA07001-1.00", 0x7e775, 0, {0, 0}, {0x4f17d3, 0x4f2206, 0x506741}, 0x6ac03},
    {"CUSA07001-1.02", "CUSA07001-1.02", 0x7e7c5, 0, {0, 0}, {0x4f1d63, 0x4f2796, 0x506d7a}, 0x6ac53},
    {"CUSA07001-1.03", "CUSA07001-1.03", 0x7e7c5, 0, {0, 0}, {0x4f1e23, 0x4f2856, 0x506e3a}, 0x6ac53},
    {"CUSA10209-1.00", "CUSA10209-1.00", 0x7e865, 0, {0, 0}, {0x4f17e3, 0x4f2216, 0x506751}, 0x6acd3},
};
enum { kRealCount = sizeof(kRealBuilds) / sizeof(*kRealBuilds) };

/* The physical button (6 Cross, 5 Circle) the key setup gives to the first key for a flag value, read from its code. */
static int first_key_button(const unsigned char *image, uint64_t reader, int flag) {
    if (flag) return image[reader + 134 + 2];
    int32_t rel;
    memcpy(&rel, image + reader + 50, sizeof(rel));
    return image[reader + 54 + rel + 2];
}

/* The key that follows: its store comes after the first key's within the branch. */
static int second_key_button(const unsigned char *image, uint64_t size, uint64_t reader, int flag) {
    uint64_t at = flag ? reader + 134 : 0;
    if (!flag) {
        int32_t rel;
        memcpy(&rel, image + reader + 50, sizeof(rel));
        at = reader + 54 + rel;
    }
    static const unsigned char tail[] = {0x00, 0x00, 0x00, 0xfe, 0x83, 0xe5, 0x00, 0x00, 0x00};
    for (uint64_t i = at + 12; i + 3 + sizeof(tail) <= size && i < at + 0x200; ++i)
        if (image[i] == 0xc7 && image[i + 1] == 0x00 && !memcmp(image + i + 3, tail, sizeof(tail))) return image[i + 2];
    return 0;
}

static void check_layout(const unsigned char *image, uint64_t size, const MenuButtonSites *sites, int confirm_cross) {
    const int flag = effective_flag(image, sites);
    CHECK(flag == confirm_cross);
    CHECK(first_key_button(image, sites->reader, flag) == (confirm_cross ? 6 : 5));
    CHECK(second_key_button(image, size, sites->reader, flag) == (confirm_cross ? 5 : 6));
}

static void test_real_builds(void) {
    int present = 0;
    for (int i = 0; i < kRealCount; ++i) {
        uint64_t size = 0;
        unsigned char *image = boot_image(kRealBuilds[i].folder, &size);
        if (!image) {
            printf("SKIP %s: no local boot image\n", kRealBuilds[i].game);
            continue;
        }
        ++present;
        printf("real image %s\n", kRealBuilds[i].game);
        MenuButtonSites sites, searched;
        CHECK(menu_buttons_locate(image, size, kRealBuilds[i].game, &sites) == 1);
        CHECK(sites.reader == kRealBuilds[i].reader && sites.write_count == kRealBuilds[i].writers && sites.western_sites);
        for (int w = 0; w < sites.write_count; ++w) CHECK(sites.writes[w] == kRealBuilds[i].writes[w]);
        CHECK(sites.compares[0] == kRealBuilds[i].reader + 12 && sites.load == kRealBuilds[i].load);
        for (int c = 0; c < 3; ++c) CHECK(sites.compares[c + 1] == kRealBuilds[i].compares[c]);
        CHECK(menu_buttons_locate(image, size, "unlisted", &searched) == 0);
        CHECK(searched.reader == sites.reader && searched.write_count == sites.write_count && searched.load == sites.load);
        for (int c = 0; c < MENU_BUTTONS_COMPARES; ++c) CHECK(searched.compares[c] == sites.compares[c]);
        check_layout(image, size, &sites, kRealBuilds[i].writers ? 1 : 0);

        unsigned char *off = malloc(size), *on = malloc(size);
        memcpy(off, image, size);
        memcpy(on, image, size);
        CHECK(menu_buttons_install(off, size, kRealBuilds[i].game, 0) == 1);
        CHECK(menu_buttons_install(on, size, kRealBuilds[i].game, 1) == 1);
        check_layout(off, size, &sites, 1);
        check_layout(on, size, &sites, 0);

        const size_t off_changes = differences(off, image, size), on_changes = differences(on, image, size);
        if (kRealBuilds[i].writers) {
            CHECK(off_changes == 0 && on_changes == 2);
        } else {
            CHECK(on_changes == 0 && off_changes == 9);
            for (uint64_t k = 0; k < size; ++k) {
                const int in_compare = k == sites.compares[0] || k == sites.compares[1] || k == sites.compares[2] ||
                                       k == sites.compares[3];
                const int in_load = k >= sites.load && k < sites.load + 6;
                CHECK(off[k] == image[k] || in_compare || in_load);
            }
        }
        MenuButtonSites after;
        CHECK(menu_buttons_locate(off, size, kRealBuilds[i].game, &after) == 1 && after.reader == sites.reader);
        CHECK(menu_buttons_install(off, size, kRealBuilds[i].game, 0) == 1 && differences(off, image, size) == off_changes);

        unsigned char *other = malloc(size);
        memcpy(other, image, size);
        CHECK(menu_buttons_install(other, size, "CUSA10454-1.99", 0) == 1 && !memcmp(other, off, size));
        memcpy(other, image, size);
        CHECK(menu_buttons_install(other, size, "CUSA00000-1.00", 1) == 1 && !memcmp(other, on, size));
        free(other);
        free(off);
        free(on);
        free(image);
    }
    printf("%d of %d real boot images present\n", present, kRealCount);
}

int main(void) {
    test_environment();
    test_locate_synthetic();
    test_install_synthetic();
    test_mismatch_writes_nothing();
    test_real_builds();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    puts("menu buttons tests passed");
    return 0;
}
