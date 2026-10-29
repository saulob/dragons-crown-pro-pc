/* Focused tests of the Story and Tutorial Skip Offers: synthetic images for every case, and the real boot images under
 * build/image when they are there (read only). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skip_offers.h"

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

static const unsigned char kStory[47] = {
    0x41, 0x80, 0xbd, 0xda, 0xf8, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x82, 0xb0, 0x00, 0x00, 0x00, 0x74, 0x1f, 0x44, 0x22, 0xa5,
    0x3b, 0xfd, 0xff, 0xff, 0x22, 0x8d, 0x28, 0xfd, 0xff, 0xff, 0x45, 0x20, 0xfe, 0x45, 0x20, 0xe6, 0x44, 0x20, 0xf1, 0x75,
    0x07, 0xba, 0x09, 0x00, 0x00, 0x00, 0xeb};
static const unsigned char kTutorial[12] = {0x84, 0xc0, 0x0f, 0x84, 0x44, 0x05, 0x00, 0x00, 0x4c, 0x8b, 0x6d, 0xb0};
static const unsigned char kMessage[5] = {0xba, 0xd8, 0x05, 0x00, 0x00};

enum { kStoryAt = 0x3aed82, kTutorialAt = 0x3a8ad9, kSize = 0x400000 };

static unsigned char *us_image(void) {
    unsigned char *image = calloc(1, kSize);
    memcpy(image + kStoryAt, kStory, sizeof(kStory));
    memcpy(image + kTutorialAt, kTutorial, sizeof(kTutorial));
    memcpy(image + kTutorialAt + 0x58, kMessage, sizeof(kMessage));
    return image;
}

static void test_synthetic(void) {
    unsigned char *image = us_image();
    unsigned char *original = us_image();
    CHECK(skip_offers_install(image, kSize, "CUSA10454-1.00", 0) == 0);
    CHECK(!memcmp(image, original, kSize));
    CHECK(skip_offers_install(image, kSize, "CUSA10209-1.00", SKIP_OFFER_STORY | SKIP_OFFER_TUTORIAL) == 0);
    CHECK(skip_offers_install(image, kSize, NULL, SKIP_OFFER_STORY) == 0);
    CHECK(!memcmp(image, original, kSize));

    CHECK(skip_offers_install(image, kSize, "CUSA10454-1.00", SKIP_OFFER_STORY) == SKIP_OFFER_STORY);
    CHECK(image[kStoryAt + 39] == 0x90 && image[kStoryAt + 40] == 0x90);
    CHECK(image[kStoryAt + 15] == 0x74 && image[kStoryAt + 16] == 0x1f); /* the new-character test stays */
    CHECK(!memcmp(image + kTutorialAt, kTutorial, sizeof(kTutorial)));
    image[kStoryAt + 39] = 0x75, image[kStoryAt + 40] = 0x07;
    CHECK(!memcmp(image, original, kSize));

    CHECK(skip_offers_install(image, kSize, "CUSA10454-1.00", SKIP_OFFER_TUTORIAL) == SKIP_OFFER_TUTORIAL);
    static const unsigned char nop6[6] = {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00};
    CHECK(!memcmp(image + kTutorialAt + 2, nop6, 6) && image[kTutorialAt] == 0x84);
    CHECK(!memcmp(image + kStoryAt, kStory, sizeof(kStory)));

    /* One byte off in a window: that offer is not applied, the other still is. */
    unsigned char *changed = us_image();
    changed[kStoryAt + 20] ^= 1;
    CHECK(skip_offers_install(changed, kSize, "CUSA10454-1.00", SKIP_OFFER_STORY | SKIP_OFFER_TUTORIAL) == SKIP_OFFER_TUTORIAL);
    CHECK(changed[kStoryAt + 39] == 0x75);
    unsigned char *no_message = us_image();
    no_message[kTutorialAt + 0x58 + 1] = 0;
    CHECK(skip_offers_install(no_message, kSize, "CUSA10454-1.00", SKIP_OFFER_TUTORIAL) == 0);
    CHECK(skip_offers_install(us_image(), kStoryAt + 20, "CUSA10454-1.00", SKIP_OFFER_STORY) == 0);
    free(image), free(original), free(changed), free(no_message);
}

static void test_environment(void) {
    set_env("DCP_STORY_SKIP", NULL), set_env("DCP_TUTORIAL_SKIP", NULL);
    CHECK(skip_offers_wanted(SKIP_OFFER_STORY | SKIP_OFFER_TUTORIAL) == (SKIP_OFFER_STORY | SKIP_OFFER_TUTORIAL));
    CHECK(skip_offers_wanted(0) == 0);
    set_env("DCP_STORY_SKIP", "1");
    set_env("DCP_TUTORIAL_SKIP", "1");
    CHECK(skip_offers_wanted(0) == (SKIP_OFFER_STORY | SKIP_OFFER_TUTORIAL));
    set_env("DCP_STORY_SKIP", "0");
    set_env("DCP_TUTORIAL_SKIP", "0");
    CHECK(skip_offers_wanted(SKIP_OFFER_STORY | SKIP_OFFER_TUTORIAL) == 0);
    set_env("DCP_STORY_SKIP", "1");
    set_env("DCP_TUTORIAL_SKIP", "off");
    CHECK(skip_offers_wanted(SKIP_OFFER_TUTORIAL) == SKIP_OFFER_STORY);
    set_env("DCP_STORY_SKIP", "0");
    set_env("DCP_TUTORIAL_SKIP", "on");
    CHECK(skip_offers_wanted(SKIP_OFFER_STORY) == SKIP_OFFER_TUTORIAL);
    set_env("DCP_STORY_SKIP", "maybe");
    CHECK((skip_offers_wanted(SKIP_OFFER_STORY) & SKIP_OFFER_STORY) != 0);
    set_env("DCP_STORY_SKIP", NULL), set_env("DCP_TUTORIAL_SKIP", NULL);
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

static void test_real_images(void) {
    static const struct {
        const char *folder, *game;
        unsigned expected;
    } kBuilds[] = {
        {"v1.03", "CUSA10454-1.00", SKIP_OFFER_STORY | SKIP_OFFER_TUTORIAL},
        {"CUSA07001-1.02", "CUSA07001-1.02", SKIP_OFFER_STORY | SKIP_OFFER_TUTORIAL},
        {"CUSA07001-1.03", "CUSA07001-1.03", SKIP_OFFER_STORY | SKIP_OFFER_TUTORIAL},
        {"CUSA07001-1.00", "CUSA07001-1.00", 0},
        {"CUSA10209-1.00", "CUSA10209-1.00", 0},
    };
    for (size_t i = 0; i < sizeof(kBuilds) / sizeof(*kBuilds); ++i) {
        uint64_t size = 0;
        unsigned char *image = boot_image(kBuilds[i].folder, &size);
        if (!image) {
            printf("no boot image for %s: skipped\n", kBuilds[i].folder);
            continue;
        }
        unsigned char *before = malloc(size);
        memcpy(before, image, size);
        const unsigned applied = skip_offers_install(image, size, kBuilds[i].game, SKIP_OFFER_STORY | SKIP_OFFER_TUTORIAL);
        CHECK(applied == kBuilds[i].expected);
        size_t differing = 0;
        for (uint64_t k = 0; k < size; ++k) differing += image[k] != before[k];
        CHECK(differing == (kBuilds[i].expected ? 2u + 4u : 0u)); /* jne: 2 bytes; je rel32 -> 6-byte nop: 4 differ */
        printf("%s: %zu bytes changed\n", kBuilds[i].folder, differing);
        free(image), free(before);
    }
}

int main(void) {
    test_synthetic();
    test_environment();
    test_real_images();
    printf("%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
