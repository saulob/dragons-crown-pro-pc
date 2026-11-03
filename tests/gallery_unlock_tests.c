/* Focused tests of Unlock Gallery: the temporary view, the replaced getter call run for real, the locators and the
 * all-or-nothing install on the local boot images under build/image when they are there (read only). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gallery_unlock.h"

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

static void test_environment(void) {
    set_env("DCP_UNLOCK_GALLERY", NULL);
    CHECK(gallery_unlock_wanted(0) == 0 && gallery_unlock_wanted(1) == 1);
    set_env("DCP_UNLOCK_GALLERY", "1");
    CHECK(gallery_unlock_wanted(0) == 1);
    set_env("DCP_UNLOCK_GALLERY", "off");
    CHECK(gallery_unlock_wanted(1) == 0);
    set_env("DCP_UNLOCK_GALLERY", "maybe");
    CHECK(gallery_unlock_wanted(1) == 1 && gallery_unlock_wanted(0) == 0);
    set_env("DCP_UNLOCK_GALLERY", NULL);
}

static void test_entries(void) {
    int seen[GALLERY_UNLOCK_FLAG_BYTES] = {0};
    for (int entry = 0; entry < GALLERY_UNLOCK_ENTRIES; ++entry) {
        const int key = gallery_unlock_entry_key(entry);
        CHECK(key >= 1 && key < GALLERY_UNLOCK_FLAG_BYTES && key != 49 && !seen[key]);
        if (key > 0 && key < GALLERY_UNLOCK_FLAG_BYTES) seen[key] = 1;
    }
    CHECK(!seen[0] && !seen[49]);
    CHECK(gallery_unlock_entry_key(-1) == -1 && gallery_unlock_entry_key(GALLERY_UNLOCK_ENTRIES) == -1);
    CHECK(gallery_unlock_entry_key(0) == 1 && gallery_unlock_entry_key(47) == 48 && gallery_unlock_entry_key(48) == 50);
    CHECK(gallery_unlock_entry_key(56) == 58);
}

static int is_entry_key(int key) {
    for (int entry = 0; entry < GALLERY_UNLOCK_ENTRIES; ++entry)
        if (gallery_unlock_entry_key(entry) == key) return 1;
    return 0;
}

static uint32_t g_random = 0x2545f491u;
static uint32_t next_random(void) {
    g_random ^= g_random << 13, g_random ^= g_random >> 17, g_random ^= g_random << 5;
    return g_random;
}

/* The Gallery's own rules for a list item: locked at 0, shown at 1 (new) and 2 (seen). */
static int list_shows(unsigned char state) {
    return state == 1 || state == 2;
}

/* What the viewer does with the state its list item holds: only a new picture is marked seen, in the real save. */
static void viewer_opens(unsigned char *real, const unsigned char *list, int key) {
    if (list[key] == 1) real[key] = 2;
}

static void test_view(void) {
    unsigned char real[GALLERY_UNLOCK_FLAG_BYTES], copy[GALLERY_UNLOCK_FLAG_BYTES], view[GALLERY_UNLOCK_FLAG_BYTES];
    for (int round = 0; round < 2000; ++round) {
        for (int i = 0; i < GALLERY_UNLOCK_FLAG_BYTES; ++i) real[i] = round < 4 ? (unsigned char)round * 0x7f : (unsigned char)(next_random() % 3);
        if (round % 7 == 0) real[0] = real[49] = 9;
        memcpy(copy, real, sizeof(real));
        gallery_unlock_fill_view(view, real);
        CHECK(!memcmp(real, copy, sizeof(real)));
        int shown = 0;
        for (int key = 0; key < GALLERY_UNLOCK_FLAG_BYTES; ++key) {
            if (is_entry_key(key)) {
                CHECK(view[key] == (real[key] == 1 ? 1 : 2));
                shown += list_shows(view[key]);
            } else {
                CHECK(view[key] == real[key]);
            }
        }
        CHECK(shown == GALLERY_UNLOCK_ENTRIES);
    }
}

static void test_save_safety(void) {
    unsigned char real[GALLERY_UNLOCK_FLAG_BYTES] = {0}, view[GALLERY_UNLOCK_FLAG_BYTES], before[GALLERY_UNLOCK_FLAG_BYTES];
    real[gallery_unlock_entry_key(3)] = 1; /* really found, not yet looked at */
    real[gallery_unlock_entry_key(10)] = 2;
    memcpy(before, real, sizeof(real));
    gallery_unlock_fill_view(view, real);

    /* Opening every picture through the view changes only what the save really holds as new. */
    for (int entry = 0; entry < GALLERY_UNLOCK_ENTRIES; ++entry) viewer_opens(real, view, gallery_unlock_entry_key(entry));
    for (int key = 0; key < GALLERY_UNLOCK_FLAG_BYTES; ++key) {
        const int found_new = key == gallery_unlock_entry_key(3);
        CHECK(real[key] == (found_new ? 2 : before[key]));
    }
    CHECK(real[gallery_unlock_entry_key(20)] == 0 && real[gallery_unlock_entry_key(56)] == 0);

    /* Off: the list reads the save, so exactly its own pictures show. */
    int shown = 0;
    for (int entry = 0; entry < GALLERY_UNLOCK_ENTRIES; ++entry) shown += list_shows(real[gallery_unlock_entry_key(entry)]);
    CHECK(shown == 2);
}

static void test_handler(void) {
    enum { kSaveBytes = GALLERY_UNLOCK_SAVE_OFFSET + 0x100 };
    unsigned char *save = calloc(1, kSaveBytes), *before = calloc(1, kSaveBytes);
    unsigned char *flags = save + GALLERY_UNLOCK_SAVE_OFFSET;
    for (size_t i = 0; i < kSaveBytes; ++i) save[i] = (unsigned char)(next_random() & 0xff);
    for (int i = 0; i < GALLERY_UNLOCK_FLAG_BYTES; ++i) flags[i] = (unsigned char)(next_random() % 3);
    memcpy(before, save, kSaveBytes);

    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rdi = (uint64_t)(uintptr_t)save;
    regs.rax = 0x1234;
    gallery_unlock_view(&regs);
    CHECK(regs.skip == 1 && regs.rax != 0x1234 && regs.rax != (uint64_t)(uintptr_t)flags);
    const unsigned char *view = (const unsigned char *)(uintptr_t)regs.rax;
    for (int entry = 0; entry < GALLERY_UNLOCK_ENTRIES; ++entry) {
        const int key = gallery_unlock_entry_key(entry);
        CHECK(view[key] == (flags[key] == 1 ? 1 : 2));
    }
    CHECK(view[0] == flags[0] && view[49] == flags[49]);
    CHECK(!memcmp(save, before, kSaveBytes));

    memset(&regs, 0, sizeof(regs));
    regs.rax = 0x1234;
    gallery_unlock_view(&regs);
    CHECK(regs.skip == 0 && regs.rax == 0x1234);
    free(save), free(before);
}

/* ---- the replaced call, run for real ---- */

#ifdef _WIN32
#include <windows.h>
static unsigned char *executable_memory(size_t size) {
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}
#else
#include <sys/mman.h>
static unsigned char *executable_memory(size_t size) {
    void *memory = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return memory == MAP_FAILED ? NULL : memory;
}
#endif

typedef uint64_t (GUEST_HOOK_ABI *ReadState)(const unsigned char *save, uint64_t key);

/* getter at 0x100: lea rax,[rdi+0x20eacc]; ret. reader at 0x200: mov rcx,rsi; call getter; movzx eax,[rax+rcx]; ret */
static void test_stub_execution(void) {
    enum { kGetter = 0x100, kReader = 0x200, kCall = 0x203, kImage = 0x300 };
    unsigned char *memory = executable_memory(8192);
    if (!memory) {
        printf("no executable memory: stub execution skipped\n");
        return;
    }
    memset(memory, 0xcc, 8192);
    static const unsigned char getter[] = {0x48, 0x8d, 0x87, 0xcc, 0xea, 0x20, 0x00, 0xc3};
    static const unsigned char reader[] = {0x48, 0x89, 0xf1, 0xe8, 0, 0, 0, 0, 0x0f, 0xb6, 0x04, 0x08, 0xc3};
    memcpy(memory + kGetter, getter, sizeof(getter));
    memcpy(memory + kReader, reader, sizeof(reader));
    const int32_t rel = kGetter - (kCall + 5);
    memcpy(memory + kCall + 1, &rel, sizeof(rel));

    unsigned char *save = calloc(1, GALLERY_UNLOCK_SAVE_OFFSET + 0x100), *before = calloc(1, GALLERY_UNLOCK_SAVE_OFFSET + 0x100);
    unsigned char *flags = save + GALLERY_UNLOCK_SAVE_OFFSET;
    flags[5] = 1;
    flags[6] = 2;
    flags[49] = 1;
    ReadState read = (ReadState)(uintptr_t)(memory + kReader);
    CHECK(read(save, 5) == 1 && read(save, 7) == 0);

    GuestHook hook = {"gallery list", kCall, 5, {0}, gallery_unlock_view};
    memcpy(hook.original, memory + kCall, 5);
    size_t bad = 0;
    CHECK(guest_hooks_install(memory, kImage, 0, NULL, &hook, 1, memory + 0x1000, 0x800, &bad) == GUEST_HOOK_OK);
    memcpy(before, save, GALLERY_UNLOCK_SAVE_OFFSET + 0x100);

    CHECK(read(save, 5) == 1 && read(save, 6) == 2); /* really found stays as it is */
    CHECK(read(save, 1) == 2 && read(save, 48) == 2 && read(save, 50) == 2 && read(save, 58) == 2);
    CHECK(read(save, 0) == 0 && read(save, 49) == 1); /* not pictures: the save's own bytes */
    CHECK(!memcmp(save, before, GALLERY_UNLOCK_SAVE_OFFSET + 0x100));
    free(save), free(before);
}

/* ---- the local boot images ---- */

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

static const struct {
    const char *folder, *game;
    uint64_t window, entry;
} kRealBuilds[] = {
    {"v1.03", "CUSA10454-1.00", 0x4d5492, 0x54c690},
    {"CUSA07001-1.00", "CUSA07001-1.00", 0x4d34a2, 0x54a800},
    {"CUSA07001-1.02", "CUSA07001-1.02", 0x4d3962, 0x54a660},
    {"CUSA07001-1.03", "CUSA07001-1.03", 0x4d3a22, 0x54a720},
    {"CUSA10209-1.00", "CUSA10209-1.00", 0x4d3612, 0x54a790},
};
enum { kRealCount = sizeof(kRealBuilds) / sizeof(*kRealBuilds) };

enum { kWindowLength = 66, kHookInWindow = 24, kHookLength = 5, kKeyBodyLength = 29, kGetterBodyLength = 8 };

/* Window bytes the locator does not read back: the two branch displacements and the art number getter's call. */
static int is_unchecked(int i) {
    return i == 12 || i == 47 || (i >= 54 && i < 58);
}

static unsigned char *with_stub_room(const unsigned char *loaded, uint64_t size, size_t room) {
    unsigned char *image = calloc(1, size + room);
    memcpy(image, loaded, size);
    return image;
}

static int64_t call_target(const unsigned char *image, uint64_t at) {
    int32_t rel;
    memcpy(&rel, image + at + 1, sizeof(rel));
    return (int64_t)at + 5 + rel;
}

/* Every call in the code that reaches `target`. */
static int calls_to(const unsigned char *image, uint64_t size, int64_t target) {
    int count = 0;
    for (uint64_t at = 0; at + 5 <= size && at < 0x640000; ++at) count += image[at] == 0xe8 && call_target(image, at) == target;
    return count;
}

/* The key table the executable fills in: {entry, key, art number} records of 12 bytes, written by `mov [rip+d],imm`. */
static int read_key_table(const unsigned char *image, uint64_t size, int64_t key_function, int records[GALLERY_UNLOCK_ENTRIES][3]) {
    int32_t disp;
    memcpy(&disp, image + key_function + 16, sizeof(disp));
    const int64_t base = key_function + 20 + disp;
    unsigned char seen[GALLERY_UNLOCK_ENTRIES][3] = {{0}};
    for (uint64_t at = 0; at + 11 <= size && at < 0x640000; ++at) {
        if (image[at + 1] != 0x05 || (image[at] != 0xc7 && image[at] != 0xc6)) continue;
        int32_t d;
        memcpy(&d, image + at + 2, sizeof(d));
        const int dword = image[at] == 0xc7;
        const int64_t target = (int64_t)at + (dword ? 10 : 7) + d;
        if (target < base || target >= base + GALLERY_UNLOCK_ENTRIES * 12) continue;
        const int entry = (int)((target - base) / 12), field = (int)((target - base) % 12) / 4;
        if ((target - base) % 4 || (dword ? 0 : 1) != (field == 2)) continue;
        uint32_t value = dword ? 0 : image[at + 6];
        if (dword) memcpy(&value, image + at + 6, sizeof(value));
        records[entry][field] = (int)value;
        seen[entry][field] = 1;
    }
    for (int entry = 0; entry < GALLERY_UNLOCK_ENTRIES; ++entry)
        if (!seen[entry][0] || !seen[entry][1] || !seen[entry][2]) return 0;
    return 1;
}

/* The viewer marks a picture seen only for a new item, through the getter, with the value 2. */
static const short kMarkSeen[] = {
    0x83, 0xfe, 0x02, 0x74, -1, 0x83, 0xfe, 0x01, 0x75, -1, 0x83, 0xfa, 0x3a, 0x7f, -1, 0x48, 0x8d, 0x14, 0x88, 0xc7,
    0x02, 0x02, 0x00, 0x00, 0x00, 0x8b, 0x7c, 0x88, 0x04, 0xe8, -1, -1, -1, -1, 0x48, 0x8b, 0x8b, 0xa8, 0x05, 0x00, 0x00,
    0x41, 0x89, 0xc4, 0x48, 0x8b, 0xb9, 0xe8, 0x00, 0x00, 0x00, 0xe8, -1, -1, -1, -1, 0x44, 0x89, 0xe1, 0xc6, 0x04,
    0x08, 0x02,
};
enum { kMarkSeenLength = sizeof(kMarkSeen) / sizeof(*kMarkSeen), kMarkCallInWindow = 51 };

static int mark_seen_sites(const unsigned char *image, uint64_t size, int64_t getter, uint64_t *site) {
    int found = 0;
    for (uint64_t at = 0; at + kMarkSeenLength <= size; ++at) {
        int match = 1;
        for (int i = 0; match && i < kMarkSeenLength; ++i) match = kMarkSeen[i] < 0 || image[at + i] == (unsigned char)kMarkSeen[i];
        if (match && call_target(image, at + kMarkCallInWindow) == getter) *site = at + kMarkCallInWindow, ++found;
    }
    return found;
}

static void check_install(unsigned char *image, const unsigned char *before, uint64_t size, const char *game, uint64_t entry,
                          uint64_t hook) {
    const size_t stub_bytes = gallery_unlock_stub_bytes();
    CHECK(gallery_unlock_install(image, size, entry, game, 1, image + size, stub_bytes) == 1);
    size_t outside = 0;
    for (uint64_t k = 0; k < size; ++k) outside += image[k] != before[k] && (k < hook || k >= hook + kHookLength);
    CHECK(outside == 0 && image[hook] == 0xe9);
    int32_t rel;
    memcpy(&rel, image + hook + 1, sizeof(rel));
    CHECK((long long)hook + 5 + rel == (long long)size);
}

static void check_build(const unsigned char *loaded, uint64_t size, int index) {
    const uint64_t hook = kRealBuilds[index].window + kHookInWindow;
    const size_t stub_bytes = gallery_unlock_stub_bytes();
    unsigned char *image = with_stub_room(loaded, size, stub_bytes), *before = with_stub_room(loaded, size, stub_bytes);
    const int64_t key_function = call_target(image, kRealBuilds[index].window), getter = call_target(image, hook);

    uint64_t site = 0;
    CHECK(gallery_unlock_locate(image, size, kRealBuilds[index].game, &site) == 1 && site == hook);
    site = 0;
    CHECK(gallery_unlock_locate(image, size, "CUSA00000-9.99", &site) == 0 && site == hook);
    for (int j = 0; j < kRealCount; ++j)
        if (j != index) CHECK(gallery_unlock_locate(image, size, kRealBuilds[j].game, &site) == 1 && site == 0);

    /* Every byte of the window, of the key function and of the getter is verified except the few noted above. */
    for (int b = 0; b < kWindowLength; ++b) {
        image[kRealBuilds[index].window + b] ^= 1;
        const int rejected = gallery_unlock_locate(image, size, kRealBuilds[index].game, &site) == 1 && site == 0;
        CHECK(rejected == !is_unchecked(b));
        image[kRealBuilds[index].window + b] ^= 1;
    }
    for (int b = 0; b < kKeyBodyLength; ++b) {
        image[key_function + b] ^= 1;
        const int rejected = gallery_unlock_locate(image, size, kRealBuilds[index].game, &site) == 1 && site == 0;
        CHECK(rejected == !(b >= 16 && b < 20));
        image[key_function + b] ^= 1;
    }
    for (int b = 0; b < kGetterBodyLength; ++b) {
        image[getter + b] ^= 1;
        CHECK(gallery_unlock_locate(image, size, kRealBuilds[index].game, &site) == 1 && site == 0);
        CHECK(gallery_unlock_locate(image, size, "CUSA00000-9.99", &site) == 0 && site == 0);
        image[getter + b] ^= 1;
    }
    CHECK(!memcmp(image, before, size));

    /* Nothing is written unless asked for, known and matching, with room. */
    CHECK(gallery_unlock_install(image, size, kRealBuilds[index].entry, NULL, 1, image + size, stub_bytes) == 0);
    CHECK(gallery_unlock_install(image, size, kRealBuilds[index].entry, kRealBuilds[index].game, 0, image + size, stub_bytes) == 0);
    CHECK(gallery_unlock_install(image, size, kRealBuilds[index].entry, kRealBuilds[index].game, 1, image + size, stub_bytes - 1) == 0);
    image[hook + 3] ^= 1;
    CHECK(gallery_unlock_install(image, size, kRealBuilds[index].entry, kRealBuilds[index].game, 1, image + size, stub_bytes) == 0);
    image[hook + 3] ^= 1;
    image[key_function] ^= 1;
    CHECK(gallery_unlock_install(image, size, kRealBuilds[index].entry, kRealBuilds[index].game, 1, image + size, stub_bytes) == 0);
    image[key_function] ^= 1;
    CHECK(!memcmp(image, before, size + stub_bytes));

    /* The getter has eleven callers; the install replaces the Gallery list's and none of the others. */
    CHECK(calls_to(image, size, getter) == 11);
    uint64_t mark = 0;
    CHECK(mark_seen_sites(image, size, getter, &mark) == 1 && mark != hook);
    check_install(image, before, size, kRealBuilds[index].game, kRealBuilds[index].entry, hook);
    CHECK(calls_to(image, size, getter) == 10);
    CHECK(image[mark] == 0xe8 && !memcmp(image + mark, before + mark, 5));
    CHECK(!memcmp(image + getter, before + getter, kGetterBodyLength));

    int records[GALLERY_UNLOCK_ENTRIES][3] = {{0}};
    CHECK(read_key_table(image, size, key_function, records));
    int art_used[GALLERY_UNLOCK_ENTRIES] = {0};
    for (int entry = 0; entry < GALLERY_UNLOCK_ENTRIES; ++entry) {
        CHECK(records[entry][0] == entry && records[entry][1] == gallery_unlock_entry_key(entry));
        CHECK(records[entry][2] >= 0 && records[entry][2] < GALLERY_UNLOCK_ENTRIES && !art_used[records[entry][2] % GALLERY_UNLOCK_ENTRIES]);
        art_used[records[entry][2] % GALLERY_UNLOCK_ENTRIES] = 1;
    }
    printf("%s: Gallery list hook at 0x%llx, 57 pictures\n", kRealBuilds[index].folder, (unsigned long long)hook);
    free(image), free(before);
}

static void test_real_images(void) {
    unsigned char *loaded[kRealCount] = {0};
    uint64_t sizes[kRealCount] = {0};
    for (int i = 0; i < kRealCount; ++i) loaded[i] = boot_image(kRealBuilds[i].folder, &sizes[i]);
    for (int i = 0; i < kRealCount; ++i) {
        if (loaded[i]) check_build(loaded[i], sizes[i], i);
        else printf("no boot image for %s: skipped\n", kRealBuilds[i].folder);
    }

    /* The two builds without a local image: a copy of the same build's code, searched for. Synthetic only. */
    static const struct {
        const char *game;
        int base;
    } kSynthetic[] = {{"CUSA10454-1.01", 0}, {"CUSA10209-1.02", 4}};
    const size_t stub_bytes = gallery_unlock_stub_bytes();
    for (size_t k = 0; k < sizeof(kSynthetic) / sizeof(*kSynthetic); ++k) {
        const int base = kSynthetic[k].base;
        if (!loaded[base]) continue;
        const uint64_t size = sizes[base], hook = kRealBuilds[base].window + kHookInWindow;
        unsigned char *image = with_stub_room(loaded[base], size, stub_bytes), *before = with_stub_room(loaded[base], size, stub_bytes);
        uint64_t site = 0;
        CHECK(gallery_unlock_locate(image, size, kSynthetic[k].game, &site) == 0 && site == hook);
        check_install(image, before, size, kSynthetic[k].game, kRealBuilds[base].entry, hook);
        free(image), free(before);

        /* Zero matches and two matches fail closed. */
        unsigned char *none = with_stub_room(loaded[base], size, stub_bytes);
        none[kRealBuilds[base].window + 5] ^= 1;
        CHECK(gallery_unlock_locate(none, size, kSynthetic[k].game, &site) == 0 && site == 0);
        CHECK(gallery_unlock_install(none, size, kRealBuilds[base].entry, kSynthetic[k].game, 1, none + size, stub_bytes) == 0);
        free(none);
        const size_t region = 0x100;
        unsigned char *twice = with_stub_room(loaded[base], size, region);
        memcpy(twice + size, loaded[base] + kRealBuilds[base].window, kWindowLength);
        const int64_t copy = (int64_t)size;
        const int32_t key = (int32_t)(call_target(loaded[base], kRealBuilds[base].window) - (copy + 5));
        const int32_t getter = (int32_t)(call_target(loaded[base], hook) - (copy + kHookInWindow + 5));
        memcpy(twice + copy + 1, &key, sizeof(key));
        memcpy(twice + copy + kHookInWindow + 1, &getter, sizeof(getter));
        CHECK(gallery_unlock_locate(twice, size + region, kSynthetic[k].game, &site) == 0 && site == 0);
        CHECK(gallery_unlock_locate(twice, size + region, kRealBuilds[base].game, &site) == 1);
        free(twice);
    }
    for (int i = 0; i < kRealCount; ++i) free(loaded[i]);
}

/* The module reaches the save array and the hook only: no trophy, network or save-writing code. */
static void test_source(void) {
    FILE *f = fopen(DCP_SOURCE_DIR "/src/gallery_unlock.c", "rb");
    CHECK(f != NULL);
    if (!f) return;
    static char text[32768];
    const size_t length = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[length] = 0;
    CHECK(!strstr(text, "trophy") && !strstr(text, "Trophy") && !strstr(text, "online") && !strstr(text, "runtime_"));
    CHECK(!strstr(text, "Full Gallery") && !strstr(text, "full_gallery"));
}

int main(void) {
    set_env("DCP_UNLOCK_GALLERY", NULL);
    test_environment();
    test_entries();
    test_view();
    test_save_safety();
    test_handler();
    test_stub_execution();
    test_real_images();
    test_source();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("gallery unlock tests passed\n");
    return 0;
}
