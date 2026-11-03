/* Focused tests of the title part trace: the hook on synthetic layouts and the install checks on a synthetic image and on
 * the local boot images under build/image when they are there (read only). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "title_trace.h"

static int failures = 0;
#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            printf("FAIL line %d: %s\n", __LINE__, #cond);       \
            ++failures;                                          \
        }                                                        \
    } while (0)

enum { PART = 48, STUB_ROOM = 0x400, SYNTHETIC_SIZE = 0x2000, SETTER_AT = 0x400 };

static const char *const kNames[] = {"START_0", "START_1", "START_2", "UPLOAD_0", "TITLE_GROUND_PS4", "x", "x", "x", "x", "x", "x", "TITLE_1_PS4"};
enum { NAME_COUNT = sizeof(kNames) / sizeof(*kNames) };

static void put_pointer(uint8_t *at, const void *pointer) {
    const uint64_t value = (uint64_t)(uintptr_t)pointer;
    memcpy(at, &value, sizeof(value));
}

typedef struct {
    uint8_t header[0x120];
    uint8_t parts[NAME_COUNT * PART];
    uint8_t player[0x200];
    uint8_t resource[0x200];
} World;

static void make_world(World *world, const char *anchor) {
    memset(world, 0, sizeof(*world));
    memcpy(world->header, "FMBS", 4);
    world->header[0x64] = NAME_COUNT;
    put_pointer(world->header + 0xf8, world->parts);
    for (int i = 0; i < NAME_COUNT; ++i) snprintf((char *)world->parts + i * PART + 0x10, 24, "%s", kNames[i]);
    snprintf((char *)world->parts + 11 * PART + 0x10, 24, "%s", anchor);
    put_pointer(world->player + 0xc0, world->resource);
    put_pointer(world->resource + 0x158, world->header);
}

static void set_part(World *world, int part) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rdi = (uint64_t)(uintptr_t)world->player;
    regs.rsi = (uint64_t)(uint32_t)part;
    title_trace_hook(&regs);
}

static void test_sequence(void) {
    World world;
    make_world(&world, "TITLE_1_PS4");
    title_trace_reset();
    set_part(&world, 4);
    CHECK(title_trace_lines() == 1 && !strcmp(title_trace_last(0), "TITLE_GROUND_PS4") && !title_trace_last(1)[0]);
    set_part(&world, 11);
    set_part(&world, 0);
    set_part(&world, 1);
    set_part(&world, 2);
    set_part(&world, 0);
    CHECK(title_trace_lines() == 6 && !strcmp(title_trace_last(1), "START_0") && !strcmp(title_trace_last(0), "TITLE_GROUND_PS4"));
    set_part(&world, -1);
    set_part(&world, NAME_COUNT);
    CHECK(title_trace_lines() == 6);

    make_world(&world, "OTHER");
    title_trace_reset();
    set_part(&world, 0);
    CHECK(title_trace_lines() == 0);
    GuestRegs bare;
    memset(&bare, 0, sizeof(bare));
    title_trace_hook(&bare);
    CHECK(title_trace_lines() == 0);

    make_world(&world, "TITLE_1_PS4");
    for (int i = 0; i < 200; ++i) set_part(&world, i % 4);
    CHECK(title_trace_lines() == 80);
    title_trace_reset();
}

static const uint8_t kSetterBytes[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x28, 0x49, 0x89, 0xfe,
    0x49, 0x8b, 0x86, 0xc0, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0x28, 0x04, 0x00, 0x00,
    0x4c, 0x8b, 0xa0, 0x58, 0x01, 0x00, 0x00, 0x49, 0x8b, 0x86, 0xc8, 0x00, 0x00, 0x00, 0x4c, 0x89, 0xe2, 0x48, 0x85, 0xc0,
    0x74, 0x07, 0x48, 0x8b, 0x90, 0x58, 0x01, 0x00, 0x00, 0x4d, 0x85, 0xe4, 0x0f, 0x84, 0xf8, 0x03, 0x00, 0x00,
    0x41, 0x0f, 0xb7, 0x44, 0x24, 0x64, 0x39, 0xf0, 0x0f, 0x8e, 0x78, 0x03, 0x00, 0x00, 0x85, 0xf6, 0x0f, 0x88, 0x82, 0x03,
    0x00, 0x00,
};

static uint8_t *synthetic_image(int twice) {
    uint8_t *image = malloc(SYNTHETIC_SIZE + STUB_ROOM);
    memset(image, 0x90, SYNTHETIC_SIZE + STUB_ROOM);
    memcpy(image + SETTER_AT, kSetterBytes, sizeof(kSetterBytes));
    if (twice) memcpy(image + 0x1000, kSetterBytes, sizeof(kSetterBytes));
    return image;
}

static void test_install(void) {
    uint32_t site = 0;
    uint8_t before[SYNTHETIC_SIZE];
    uint8_t *image = synthetic_image(0);
    CHECK(title_trace_locate(image, SYNTHETIC_SIZE, &site) && site == SETTER_AT);
    memcpy(before, image, SYNTHETIC_SIZE);
    CHECK(title_trace_install(image, SYNTHETIC_SIZE, 0x1000, image + SYNTHETIC_SIZE, STUB_ROOM) == 1);
    CHECK(image[SETTER_AT] == 0xe9);
    for (size_t i = 0; i < SYNTHETIC_SIZE; ++i)
        if (image[i] != before[i]) CHECK(i >= SETTER_AT && i < SETTER_AT + 8);
    free(image);

    image = synthetic_image(1);
    memcpy(before, image, SYNTHETIC_SIZE);
    CHECK(title_trace_locate(image, SYNTHETIC_SIZE, &site) == 0);
    CHECK(title_trace_install(image, SYNTHETIC_SIZE, 0x1000, image + SYNTHETIC_SIZE, STUB_ROOM) == 0);
    CHECK(!memcmp(image, before, SYNTHETIC_SIZE));
    free(image);

    image = synthetic_image(0);
    memcpy(before, image, SYNTHETIC_SIZE);
    CHECK(title_trace_install(image, SYNTHETIC_SIZE, 0x1000, image + SYNTHETIC_SIZE, 0) == 0);
    CHECK(!memcmp(image, before, SYNTHETIC_SIZE));
    free(image);

    for (size_t i = 0; i < sizeof(kSetterBytes); ++i) {
        const int wildcard = (i >= 32 && i < 36) || (i >= 70 && i < 74) || (i >= 84 && i < 88) || (i >= 92 && i < 96);
        image = synthetic_image(0);
        image[SETTER_AT + i] ^= 0x40;
        CHECK(title_trace_locate(image, SYNTHETIC_SIZE, &site) == wildcard);
        free(image);
    }
}

static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) return 0;
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = v << 8 | b[i];
    return v;
}

/* The memory image inside build/image/<folder>/boot.bin (layout written by launcher/boot_image.cpp). */
static uint8_t *boot_image(const char *folder, uint64_t *size) {
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
    uint8_t *image = malloc(*size + STUB_ROOM);
    if (image && fread(image, 1, *size, f) != *size) free(image), image = NULL;
    if (image) memset(image + *size, 0, STUB_ROOM);
    fclose(f);
    return image;
}

static void test_real_images(void) {
    static const struct {
        const char *folder, *game;
        uint32_t site;
    } builds[] = {
        {"v1.03", "CUSA10454 1.00", 0x578000},       {"CUSA07001-1.00", "CUSA07001 1.00", 0x5760a0},
        {"CUSA07001-1.02", "CUSA07001 1.02", 0x575fd0}, {"CUSA07001-1.03", "CUSA07001 1.03", 0x576090},
        {"CUSA10209-1.00", "CUSA10209 1.00", 0x576100},
    };
    for (size_t i = 0; i < sizeof(builds) / sizeof(*builds); ++i) {
        uint64_t size = 0;
        uint8_t *image = boot_image(builds[i].folder, &size);
        if (!image) {
            printf("skip %s: no local boot image\n", builds[i].game);
            continue;
        }
        uint32_t site = 0;
        CHECK(title_trace_locate(image, size, &site) && site == builds[i].site);
        CHECK(title_trace_install(image, size, 0x1000, image + size, STUB_ROOM) == 1 && image[builds[i].site] == 0xe9);
        printf("real image %s: part setter 0x%x, installed\n", builds[i].game, builds[i].site);
        free(image);
    }
}

int main(void) {
    test_sequence();
    test_install();
    test_real_images();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    puts("title trace tests passed");
    return 0;
}
