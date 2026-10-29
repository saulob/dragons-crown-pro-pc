/* Focused tests of the online player cards' System type hook: the handler, and the sites on synthetic and on the
 * local boot images. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_hook.h"
#include "system_type_hook.h"

static int checks;
static int failures;

#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

static char asked[32];
static uint64_t set_widget;
static char set_text[64];

static const char *TextOf(const char *online_id) {
    snprintf(asked, sizeof(asked), "%s", online_id);
    return strcmp(online_id, "Nobody") ? "Linux \xC2\xB7 v0.8.1" : NULL;
}

static uint64_t GUEST_HOOK_ABI SetText(uint64_t widget, const char *text) {
    set_widget = widget;
    snprintf(set_text, sizeof(set_text), "%s", text);
    return 0;
}

static GuestRegs Regs(uint32_t record, const char *online_id) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rdi = 0x5b0;
    regs.rsi = record;
    regs.r12 = (uint64_t)(uintptr_t)online_id;
    return regs;
}

static void TestHandler(void) {
    system_type_set_functions(TextOf, SetText);
    GuestRegs regs = Regs(SYSTEM_TYPE_PS4_RECORD, "Alice");
    system_type_hook(&regs);
    CHECK(regs.skip == 1 && set_widget == 0x5b0 && !strcmp(asked, "Alice") && !strcmp(set_text, "Linux \xC2\xB7 v0.8.1"));

    set_widget = 0;
    regs = Regs(0x124, "Alice");
    system_type_hook(&regs);
    CHECK(regs.skip == 0 && set_widget == 0);
    regs = Regs(0x123, "Alice");
    system_type_hook(&regs);
    CHECK(regs.skip == 0 && set_widget == 0);
    regs = Regs(SYSTEM_TYPE_PS4_RECORD, NULL);
    system_type_hook(&regs);
    CHECK(regs.skip == 0 && set_widget == 0);
    regs = Regs(SYSTEM_TYPE_PS4_RECORD, "Nobody");
    system_type_hook(&regs);
    CHECK(regs.skip == 0 && set_widget == 0);
    system_type_set_functions(NULL, NULL);
    regs = Regs(SYSTEM_TYPE_PS4_RECORD, "Alice");
    system_type_hook(&regs);
    CHECK(regs.skip == 0 && set_widget == 0);
}

static size_t Differing(const uint8_t *a, const uint8_t *b, uint64_t size) {
    size_t n = 0;
    for (uint64_t i = 0; i < size; ++i) n += a[i] != b[i];
    return n;
}

static uint64_t StubOffset(uint64_t size) { return (size + 0xfff) & ~(uint64_t)0xfff; }

static void TestSynthetic(void) {
    const SystemTypeRecord *record = &system_type_records[0];
    const uint64_t size = record->image.size;
    const size_t stub_bytes = system_type_hook_stub_bytes();
    uint8_t *image = calloc(1, StubOffset(size) + stub_bytes);
    uint8_t *before = malloc(size);
    if (!image || !before) {
        CHECK(0);
        free(image), free(before);
        return;
    }
    for (int i = 0; i < SYSTEM_TYPE_SITES; ++i) memcpy(image + record->sites[i], record->calls[i], 5);
    memcpy(before, image, size);
    CHECK(!system_type_hook_install(image, size, record->image.entry, image + StubOffset(size), stub_bytes, TextOf));
    CHECK(Differing(image, before, size) == 0);
    CHECK(!system_type_hook_install(image, size, record->image.entry + 1, image + StubOffset(size), stub_bytes, TextOf));

    memcpy(image + record->set_text, system_type_set_text_prologue, SYSTEM_TYPE_SET_TEXT_CHECK);
    image[record->sites[1] + 1] ^= 1;
    memcpy(before, image, size);
    CHECK(!system_type_hook_install(image, size, record->image.entry, image + StubOffset(size), stub_bytes, TextOf));
    CHECK(Differing(image, before, size) == 0);

    image[record->sites[1] + 1] ^= 1;
    memcpy(before, image, size);
    CHECK(system_type_hook_install(image, size, record->image.entry, image + StubOffset(size), stub_bytes, TextOf));
    for (int i = 0; i < SYSTEM_TYPE_SITES; ++i) CHECK(image[record->sites[i]] == 0xe9);
    CHECK(Differing(image, before, size) <= 5 * SYSTEM_TYPE_SITES);
    free(image), free(before);
}

static uint64_t Read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) return 0;
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = v << 8 | b[i];
    return v;
}

/* The memory image inside build/image/<folder>/boot.bin (layout written by launcher/boot_image.cpp), followed by
 * room for the stubs. */
static uint8_t *BootImage(const char *folder, uint64_t *size, uint64_t *entry) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/build/image/%s/boot.bin", DCP_SOURCE_DIR, folder);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "DCPBOOT1", 8)) return fclose(f), NULL;
    *size = Read64(f);
    *entry = Read64(f);
    const uint64_t segments = Read64(f), relocs = Read64(f), names = Read64(f);
    Read64(f), Read64(f);
    for (int i = 0; i < 4; ++i) Read64(f);
    const uint64_t modules = Read64(f);
    fseek(f, (long)(modules * 56), SEEK_CUR);
    const uint64_t bindings = Read64(f);
    fseek(f, (long)(bindings * 24 + segments * 24 + names * 128 + relocs * 32), SEEK_CUR);
    uint8_t *image = calloc(1, StubOffset(*size) + system_type_hook_stub_bytes());
    if (image && fread(image, 1, *size, f) != *size) free(image), image = NULL;
    fclose(f);
    return image;
}

static void TestRealImages(void) {
    static const struct {
        const char *folder;
        int expected;
    } kBuilds[] = {
        {"v1.03", 1}, {"CUSA07001-1.02", 1}, {"CUSA07001-1.03", 1}, {"CUSA07001-1.00", 0}, {"CUSA10209-1.00", 0},
    };
    for (size_t i = 0; i < sizeof(kBuilds) / sizeof(*kBuilds); ++i) {
        uint64_t size = 0, entry = 0;
        uint8_t *image = BootImage(kBuilds[i].folder, &size, &entry);
        if (!image) {
            printf("no boot image for %s: skipped\n", kBuilds[i].folder);
            continue;
        }
        uint8_t *before = malloc(size);
        memcpy(before, image, size);
        const int installed = system_type_hook_install(image, size, entry, image + StubOffset(size),
                                                       system_type_hook_stub_bytes(), TextOf);
        CHECK(installed == kBuilds[i].expected);
        const size_t differing = Differing(image, before, size);
        CHECK(kBuilds[i].expected ? differing > 0 && differing <= 5 * SYSTEM_TYPE_SITES : differing == 0);
        printf("%s: %zu bytes changed\n", kBuilds[i].folder, differing);
        free(image), free(before);
    }
}

int main(void) {
    TestHandler();
    TestSynthetic();
    TestRealImages();
    system_type_set_functions(NULL, NULL);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
