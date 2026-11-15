/* Focused tests of the endgame unlocks: the availability rules over character records, the hooks run with guest
 * registers, the locators and the all-or-nothing install on the local boot images under build/image when they are
 * there (read only). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "endgame_unlocks.h"

static int failures = 0;
#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            printf("FAIL line %d: %s\n", __LINE__, #cond);       \
            ++failures;                                          \
        }                                                        \
    } while (0)

#define LAB ENDGAME_UNLOCK_LABYRINTH
#define TOWER ENDGAME_UNLOCK_ULTIMATE_TOWER
#define BOTH (LAB | TOWER)
#define STORY_SKIP_STEP 0x2d
#define RECORD_BYTES 0x23b0

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

static void test_environment(void) {
    set_env("DCP_UNLOCK_LABYRINTH", NULL);
    set_env("DCP_UNLOCK_ULTIMATE_TOWER", NULL);
    CHECK(endgame_unlocks_wanted(0) == 0);
    CHECK(endgame_unlocks_wanted(LAB) == LAB && endgame_unlocks_wanted(TOWER) == TOWER && endgame_unlocks_wanted(BOTH) == BOTH);
    set_env("DCP_UNLOCK_LABYRINTH", "1");
    CHECK(endgame_unlocks_wanted(0) == LAB && endgame_unlocks_wanted(TOWER) == BOTH);
    set_env("DCP_UNLOCK_LABYRINTH", "off");
    CHECK(endgame_unlocks_wanted(BOTH) == TOWER);
    set_env("DCP_UNLOCK_LABYRINTH", "maybe");
    CHECK(endgame_unlocks_wanted(0) == 0 && endgame_unlocks_wanted(LAB) == LAB);
    set_env("DCP_UNLOCK_LABYRINTH", NULL);
    set_env("DCP_UNLOCK_ULTIMATE_TOWER", "on");
    CHECK(endgame_unlocks_wanted(0) == TOWER && endgame_unlocks_wanted(LAB) == BOTH);
    set_env("DCP_UNLOCK_ULTIMATE_TOWER", "0");
    CHECK(endgame_unlocks_wanted(BOTH) == LAB);
    set_env("DCP_UNLOCK_ULTIMATE_TOWER", NULL);
}

/* ---- records ---- */

static void record_at(unsigned char *record, int step, int difficulty) {
    memset(record, 0, RECORD_BYTES);
    record[ENDGAME_STORY_STEP] = (unsigned char)step;
    record[ENDGAME_DIFFICULTY] = (unsigned char)difficulty;
}

/* The game's own prayer records (id, required tier, cost, ..., kind 0x67 at +0x10, difficulty at +0x14). */
static void prayer_at(unsigned char *prayer, int id, int required, int value) {
    memset(prayer, 0, 32);
    const int32_t fields[] = {id, required};
    memcpy(prayer, fields, sizeof(fields));
    const int16_t kind = 0x67;
    memcpy(prayer + 0x10, &kind, sizeof(kind));
    memcpy(prayer + 0x14, &value, sizeof(value));
}

static void test_native_answers(void) {
    static unsigned char record[RECORD_BYTES];
    for (int step = 0; step < 0x40; ++step) {
        record_at(record, step, 0);
        CHECK(endgame_stables_open(record) == (step > 0x28));
        CHECK(endgame_native_labyrinth(record) == (step > 0x2d));
        const int tier = step >= 0x2e && step <= 0x30 ? 1 : step >= 0x31 && step <= 0x32 ? 2 : step > 0x32 ? 3 : 0;
        CHECK(endgame_native_prayer_tier(record) == tier);
        const int cleared = step == 0x30 || step == 0x31 ? 2 : step > 0x31 ? 4 : 0;
        CHECK(endgame_native_cleared_tier(record) == cleared);
    }
    record_at(record, STORY_SKIP_STEP, 0);
    CHECK(endgame_stables_open(record) && !endgame_native_labyrinth(record) && endgame_native_prayer_tier(record) == 0);
}

static void test_labyrinth_rule(void) {
    static unsigned char record[RECORD_BYTES];
    for (int step = 0; step < 0x40; ++step) {
        for (int ultimate = 0; ultimate < 2; ++ultimate) {
            record_at(record, step, ultimate ? ENDGAME_ULTIMATE : 0);
            const int native = step > 0x2d, stables = step > 0x28;
            CHECK(endgame_labyrinth_open(record, 0) == native);
            CHECK(endgame_labyrinth_open(record, LAB) == (native || stables));
            CHECK(endgame_labyrinth_open(record, TOWER) == (native || (stables && ultimate)));
            CHECK(endgame_labyrinth_open(record, BOTH) == (native || stables));
        }
    }
    record_at(record, 0x28, ENDGAME_ULTIMATE);
    CHECK(!endgame_labyrinth_open(record, BOTH));
    record_at(record, STORY_SKIP_STEP, 0);
    CHECK(endgame_labyrinth_open(record, LAB) && !endgame_labyrinth_open(record, TOWER));
}

static void test_prayer_rule(void) {
    static unsigned char record[RECORD_BYTES];
    unsigned char ultimate[32], normal[32], hard[32], infernal[32], boon[32];
    prayer_at(ultimate, ENDGAME_PRAYER_ULTIMATE, 2, ENDGAME_ULTIMATE);
    prayer_at(normal, ENDGAME_PRAYER_NORMAL, 1, 0);
    prayer_at(hard, 16, 1, 2);
    prayer_at(infernal, 18, 2, 4);
    prayer_at(boon, 9, 1, 0x19);
    memset(boon + 0x10, 0x62, 2);

    record_at(record, STORY_SKIP_STEP, 0);
    CHECK(endgame_prayer_exposed(record, ultimate, TOWER) && endgame_prayer_exposed(record, ultimate, BOTH));
    CHECK(!endgame_prayer_exposed(record, ultimate, 0) && !endgame_prayer_exposed(record, ultimate, LAB));
    CHECK(endgame_prayer_tier(record, ultimate, TOWER) == 2 && endgame_cleared_tier(record, ultimate, TOWER) == 2);
    CHECK(endgame_prayer_tier(record, ultimate, LAB) == 0 && endgame_cleared_tier(record, ultimate, LAB) == 0);
    CHECK(endgame_prayer_tier(record, ultimate, 0) == 0);
    const unsigned char *others[] = {normal, hard, infernal, boon};
    for (size_t i = 0; i < sizeof(others) / sizeof(*others); ++i) {
        CHECK(!endgame_prayer_exposed(record, others[i], BOTH));
        CHECK(endgame_prayer_tier(record, others[i], BOTH) == 0 && endgame_cleared_tier(record, others[i], BOTH) == 0);
    }
    CHECK(!endgame_prayer_exposed(record, NULL, BOTH));

    /* The way back to Normal is offered while the character is on Ultimate. */
    record_at(record, STORY_SKIP_STEP, ENDGAME_ULTIMATE);
    CHECK(endgame_prayer_exposed(record, normal, TOWER) && endgame_prayer_tier(record, normal, TOWER) == 1 &&
          endgame_cleared_tier(record, normal, TOWER) == 1);
    CHECK(!endgame_prayer_exposed(record, hard, TOWER) && !endgame_prayer_exposed(record, infernal, TOWER));
    CHECK(!endgame_prayer_exposed(record, normal, LAB));

    /* Before the Stables nothing changes. */
    for (int step = 0; step <= 0x28; ++step) {
        record_at(record, step, ENDGAME_ULTIMATE);
        CHECK(!endgame_prayer_exposed(record, ultimate, BOTH) && !endgame_prayer_exposed(record, normal, BOTH));
        CHECK(endgame_prayer_tier(record, ultimate, BOTH) == endgame_native_prayer_tier(record));
    }

    /* A character past the requirements keeps the game's higher answers. */
    record_at(record, 0x33, 0);
    CHECK(endgame_prayer_tier(record, ultimate, TOWER) == 3 && endgame_cleared_tier(record, ultimate, TOWER) == 4);
    CHECK(endgame_prayer_tier(record, infernal, TOWER) == 3);
}

/* ---- the hooks with guest registers ---- */

static void test_handlers(void) {
    static unsigned char record[RECORD_BYTES], before[RECORD_BYTES];
    unsigned char ultimate[32], infernal[32];
    prayer_at(ultimate, ENDGAME_PRAYER_ULTIMATE, 2, ENDGAME_ULTIMATE);
    prayer_at(infernal, 18, 2, 4);
    uint64_t frame[16] = {0};
    const uint64_t save = 0x123456789ull;
    frame[16 - 9] = save; /* [rbp-0x48] */

    record_at(record, STORY_SKIP_STEP, 0);
    memcpy(before, record, RECORD_BYTES);
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rdi = (uintptr_t)record;
    regs.rbp = (uintptr_t)&frame[16];

    endgame_unlocks_set(0);
    regs.rax = 0x55;
    endgame_labyrinth_check(&regs);
    CHECK(regs.skip == 1 && regs.rax == 0);
    endgame_unlocks_set(LAB);
    endgame_labyrinth_check(&regs);
    CHECK(regs.rax == 1);
    endgame_unlocks_set(TOWER);
    endgame_labyrinth_check(&regs);
    CHECK(regs.rax == 0);

    regs.rbx = (uintptr_t)infernal;
    endgame_list_tier(&regs);
    CHECK(regs.rax == 0 && frame[16 - 9] == save);
    regs.rbx = (uintptr_t)ultimate;
    endgame_unlocks_set(LAB);
    endgame_list_tier(&regs);
    CHECK(regs.rax == 0 && frame[16 - 9] == save);
    endgame_unlocks_set(TOWER);
    endgame_list_tier(&regs);
    const unsigned char *listed = (const unsigned char *)(uintptr_t)frame[16 - 9];
    CHECK(regs.rax == 2 && frame[16 - 9] != save && listed[ENDGAME_ULTIMATE_LISTED] == 1);
    for (int i = 0; i < 16; ++i) CHECK(i == 16 - 9 || frame[i] == 0);

    regs.r14 = (uintptr_t)ultimate;
    regs.rax = 0;
    endgame_row_tier(&regs);
    CHECK(regs.rax == 2);
    regs.rax = 0;
    endgame_row_cleared(&regs);
    CHECK(regs.rax == 2);
    regs.rax = 0;
    endgame_confirm_tier(&regs);
    CHECK(regs.rax == 2);
    regs.r14 = (uintptr_t)infernal;
    endgame_row_tier(&regs);
    CHECK(regs.rax == 0);

    record[ENDGAME_STORY_STEP] = 0x28;
    regs.rbx = (uintptr_t)ultimate;
    frame[16 - 9] = save;
    endgame_unlocks_set(BOTH);
    endgame_list_tier(&regs);
    CHECK(regs.rax == 0 && frame[16 - 9] == save);
    endgame_labyrinth_check(&regs);
    CHECK(regs.rax == 0);
    record[ENDGAME_STORY_STEP] = STORY_SKIP_STEP;
    CHECK(!memcmp(record, before, RECORD_BYTES));
    endgame_unlocks_set(0);
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

typedef uint64_t (GUEST_HOOK_ABI *StoryCheck)(const unsigned char *record);

/* check at 0x100: cmp byte [rdi+0x6f],0x2d; seta al; ret. caller at 0x200: xor eax,eax; call check; movzx eax,al; ret */
static void test_stub_execution(void) {
    enum { kCheck = 0x100, kCaller = 0x200, kCall = 0x202, kImage = 0x300 };
    unsigned char *memory = executable_memory(8192);
    if (!memory) {
        printf("no executable memory: stub execution skipped\n");
        return;
    }
    memset(memory, 0xcc, 8192);
    static const unsigned char check[] = {0x80, 0x7f, 0x6f, 0x2d, 0x0f, 0x97, 0xc0, 0xc3};
    static const unsigned char caller[] = {0x31, 0xc0, 0xe8, 0, 0, 0, 0, 0x0f, 0xb6, 0xc0, 0xc3};
    memcpy(memory + kCheck, check, sizeof(check));
    memcpy(memory + kCaller, caller, sizeof(caller));
    const int32_t rel = kCheck - (kCall + 5);
    memcpy(memory + kCall + 1, &rel, sizeof(rel));
    static unsigned char record[RECORD_BYTES];
    StoryCheck run = (StoryCheck)(uintptr_t)(memory + kCaller);
    record_at(record, STORY_SKIP_STEP, 0);
    CHECK(run(record) == 0);

    GuestHook hook = {"endgame labyrinth", kCall, 5, {0}, endgame_labyrinth_check};
    memcpy(hook.original, memory + kCall, 5);
    size_t bad = 0;
    CHECK(guest_hooks_install(memory, kImage, 0, NULL, &hook, 1, memory + 0x1000, 0x800, &bad) == GUEST_HOOK_OK);
    endgame_unlocks_set(0);
    CHECK(run(record) == 0);
    endgame_unlocks_set(LAB);
    CHECK(run(record) == 1);
    record[ENDGAME_STORY_STEP] = 0x28;
    CHECK(run(record) == 0);
    record[ENDGAME_STORY_STEP] = 0x2e;
    endgame_unlocks_set(0);
    CHECK(run(record) == 1);
    endgame_unlocks_set(0);
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
    uint64_t entry;
    EndgameSites sites;
    uint64_t setter, story_skip;
} kRealBuilds[] = {
    {"v1.03", "CUSA10454-1.00", 0x54c690, {0x477e91, 0x3bd162, 0x3bc9e7, 0x3bca0e, 0x3c1edf}, 0x26b40, 0x3ae3c2},
    {"CUSA07001-1.00", "CUSA07001-1.00", 0x54a800, {0x477091, 0x3bc4c2, 0x3bbd47, 0x3bbd6e, 0x3c11df}, 0x26490, 0x3ad7a2},
    {"CUSA07001-1.02", "CUSA07001-1.02", 0x54a660, {0x477461, 0x3bc892, 0x3bc117, 0x3bc13e, 0x3c15ef}, 0x26510, 0x3adb62},
    {"CUSA07001-1.03", "CUSA07001-1.03", 0x54a720, {0x477521, 0x3bc8e2, 0x3bc167, 0x3bc18e, 0x3c163f}, 0x26510, 0x3adbb2},
    {"CUSA10209-1.00", "CUSA10209-1.00", 0x54a790, {0x477201, 0x3bc632, 0x3bbeb7, 0x3bbede, 0x3c134f}, 0x26550, 0x3ad912},
};
enum { kRealCount = sizeof(kRealBuilds) / sizeof(*kRealBuilds) };

static int64_t call_target(const unsigned char *image, uint64_t at) {
    int32_t rel;
    memcpy(&rel, image + at + 1, sizeof(rel));
    return (int64_t)at + 5 + rel;
}

static int calls_to(const unsigned char *image, uint64_t size, int64_t target) {
    int count = 0;
    for (uint64_t at = 0; at + 5 <= size && at < 0x640000; ++at) count += image[at] == 0xe8 && call_target(image, at) == target;
    return count;
}

static int same_sites(const EndgameSites *a, const EndgameSites *b) {
    return a->labyrinth == b->labyrinth && a->list_tier == b->list_tier && a->row_tier == b->row_tier &&
           a->row_cleared == b->row_cleared && a->confirm_tier == b->confirm_tier;
}

static int no_sites(const EndgameSites *s) {
    return !s->labyrinth && !s->list_tier && !s->row_tier && !s->row_cleared && !s->confirm_tier;
}

static uint64_t site_list(const EndgameSites *s, int i) {
    const uint64_t all[] = {s->labyrinth, s->list_tier, s->row_tier, s->row_cleared, s->confirm_tier};
    return all[i];
}

static unsigned char *with_stub_room(const unsigned char *loaded, uint64_t size, size_t room) {
    unsigned char *image = calloc(1, size + room);
    memcpy(image, loaded, size);
    return image;
}

/* The game's prayer table through the lookup the list calls: 20 records of 32 bytes. */
static const unsigned char *prayer_table(const unsigned char *image, uint64_t size, uint64_t list_tier) {
    const int64_t lookup = call_target(image, list_tier - 0x12 + 7);
    static const unsigned char head[] = {0x48, 0x63, 0xcf, 0x31, 0xc0, 0x48, 0x89, 0xca, 0x48, 0xc1, 0xe2, 0x05, 0x48, 0x03, 0x15};
    if (lookup < 0 || (uint64_t)lookup + 0x20 > size || memcmp(image + lookup, head, sizeof(head))) return NULL;
    int32_t disp;
    memcpy(&disp, image + lookup + 15, sizeof(disp));
    uint64_t table;
    memcpy(&table, image + lookup + 19 + disp, sizeof(table));
    return table && table + 20 * 32 <= size ? image + table : NULL;
}

static void check_prayers(const unsigned char *image, uint64_t size, uint64_t list_tier) {
    const unsigned char *table = prayer_table(image, size, list_tier);
    CHECK(table != NULL);
    if (!table) return;
    static unsigned char record[RECORD_BYTES];
    record_at(record, STORY_SKIP_STEP, 0);
    int exposed = 0;
    for (int i = 0; i < 20; ++i) {
        int32_t id;
        memcpy(&id, table + i * 32, sizeof(id));
        CHECK(id == i);
        exposed += endgame_prayer_exposed(record, table + i * 32, TOWER);
    }
    CHECK(exposed == 1 && endgame_prayer_exposed(record, table + ENDGAME_PRAYER_ULTIMATE * 32, TOWER));
    CHECK(endgame_prayer_tier(record, table + ENDGAME_PRAYER_ULTIMATE * 32, TOWER) == 2);
    record_at(record, STORY_SKIP_STEP, ENDGAME_ULTIMATE);
    exposed = 0;
    for (int i = 0; i < 20; ++i) exposed += endgame_prayer_exposed(record, table + i * 32, TOWER);
    CHECK(exposed == 2 && endgame_prayer_exposed(record, table + ENDGAME_PRAYER_NORMAL * 32, TOWER));
}

/* The game's Story Skip: SetProgress(0x2d), a step past the Stables and before the Labyrinth. */
static void check_story_skip(const unsigned char *image, uint64_t size, uint64_t at) {
    static const unsigned char shape[] = {0x83, 0xf8, 0x02, 0x0f, 0x85, 0, 0, 0, 0, 0x4c, 0x89, 0xfb,
                                          0xbe, STORY_SKIP_STEP, 0x00, 0x00, 0x00, 0x48, 0x89, 0xdf, 0xe8};
    CHECK(at + sizeof(shape) + 4 <= size);
    for (size_t i = 0; i < sizeof(shape); ++i) CHECK((i >= 5 && i < 9) || image[at + i] == shape[i]);
    const int64_t setter = call_target(image, at + 20);
    static const unsigned char store[] = {0x40, 0x88, 0x77, ENDGAME_STORY_STEP};
    CHECK(setter > 0 && !memcmp(image + setter, store, sizeof(store)));
    static unsigned char record[RECORD_BYTES];
    record_at(record, image[at + 13], 0);
    CHECK(endgame_stables_open(record) && !endgame_native_labyrinth(record));
}

static void check_install(const unsigned char *loaded, uint64_t size, const char *game, uint64_t entry,
                          const EndgameSites *sites, unsigned wanted, int hooks) {
    const size_t stub_bytes = endgame_unlocks_stub_bytes();
    unsigned char *image = with_stub_room(loaded, size, stub_bytes);
    CHECK(endgame_unlocks_install(image, size, entry, game, wanted, image + size, stub_bytes) == wanted);
    size_t outside = 0;
    for (uint64_t k = 0; k < size; ++k) {
        int hooked = 0;
        for (int i = 0; i < hooks; ++i) hooked |= k >= site_list(sites, i) && k < site_list(sites, i) + 5;
        outside += !hooked && image[k] != loaded[k];
    }
    CHECK(outside == 0);
    for (int i = 0; i < 5; ++i) {
        const uint64_t site = site_list(sites, i);
        if (i < hooks) {
            const int64_t stub = call_target(image, site);
            CHECK(image[site] == 0xe9 && stub >= (int64_t)size && stub < (int64_t)(size + stub_bytes));
        } else {
            CHECK(!memcmp(image + site, loaded + site, 5));
        }
    }
    endgame_unlocks_set(0);
    free(image);
}

static void check_build(const unsigned char *loaded, uint64_t size, int index) {
    const EndgameSites *expected = &kRealBuilds[index].sites;
    const char *game = kRealBuilds[index].game;
    const uint64_t entry = kRealBuilds[index].entry;
    const size_t stub_bytes = endgame_unlocks_stub_bytes();
    unsigned char *image = with_stub_room(loaded, size, stub_bytes);
    EndgameSites sites;

    CHECK(endgame_unlocks_locate(image, size, game, &sites) == 1 && same_sites(&sites, expected));
    CHECK(endgame_unlocks_locate(image, size, "CUSA00000-9.99", &sites) == 0 && same_sites(&sites, expected));
    CHECK(endgame_unlocks_locate(image, size, NULL, &sites) == 0 && no_sites(&sites));
    for (int j = 0; j < kRealCount; ++j)
        if (j != index) CHECK(endgame_unlocks_locate(image, size, kRealBuilds[j].game, &sites) == 1 && no_sites(&sites));

    /* The checks the hooks answer for: the Labyrinth's and the Stables' story checks, the prayer tier and cleared
     * difficulty, and the difficulty setter. A changed byte in any refuses every site. */
    const int64_t labyrinth_check = call_target(image, expected->labyrinth);
    const int64_t stables_check = call_target(image, expected->labyrinth - 0x40);
    const int64_t tier = call_target(image, expected->list_tier), cleared = call_target(image, expected->row_cleared);
    CHECK(image[labyrinth_check + 3] == 0x2d && image[stables_check + 3] == 0x28);
    CHECK(calls_to(image, size, labyrinth_check) == 2 && calls_to(image, size, tier) == 5);
    const int64_t touched[] = {labyrinth_check + 3, stables_check + 3, tier + 10, cleared + 11,
                               (int64_t)kRealBuilds[index].setter + 0x39, (int64_t)kRealBuilds[index].setter + 0x73,
                               (int64_t)expected->labyrinth - 1, (int64_t)expected->list_tier + 0x1d, (int64_t)expected->row_tier - 1,
                               (int64_t)expected->confirm_tier + 6};
    for (size_t t = 0; t < sizeof(touched) / sizeof(*touched); ++t) {
        image[touched[t]] ^= 1;
        CHECK(endgame_unlocks_locate(image, size, game, &sites) == 1 && no_sites(&sites));
        CHECK(endgame_unlocks_install(image, size, entry, game, BOTH, image + size, stub_bytes) == 0);
        image[touched[t]] ^= 1;
    }
    CHECK(!memcmp(image, loaded, size));

    /* Nothing is written unless asked for, known and matching, with room. */
    CHECK(endgame_unlocks_install(image, size, entry, game, 0, image + size, stub_bytes) == 0);
    CHECK(endgame_unlocks_install(image, size, entry, NULL, BOTH, image + size, stub_bytes) == 0);
    CHECK(endgame_unlocks_install(image, size, entry, game, BOTH, image + size, stub_bytes - 1) == 0);
    CHECK(!memcmp(image, loaded, size));

    check_install(loaded, size, game, entry, expected, LAB, 1);
    check_install(loaded, size, game, entry, expected, TOWER, 5);
    check_install(loaded, size, game, entry, expected, BOTH, 5);

    /* The town's "Labyrinth is open" notice keeps the game's own check, so it never fires early. */
    unsigned char *installed = with_stub_room(loaded, size, stub_bytes);
    CHECK(endgame_unlocks_install(installed, size, entry, game, BOTH, installed + size, stub_bytes) == BOTH);
    CHECK(calls_to(installed, size, labyrinth_check) == 1 && calls_to(installed, size, tier) == 2);
    endgame_unlocks_set(0);
    free(installed);

    check_prayers(image, size, expected->list_tier);
    check_story_skip(image, size, kRealBuilds[index].story_skip);
    printf("%s: Labyrinth check 0x%llx, Temple 0x%llx 0x%llx 0x%llx 0x%llx\n", kRealBuilds[index].folder,
           (unsigned long long)expected->labyrinth, (unsigned long long)expected->list_tier,
           (unsigned long long)expected->row_tier, (unsigned long long)expected->row_cleared,
           (unsigned long long)expected->confirm_tier);
    free(image);
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
    for (size_t k = 0; k < sizeof(kSynthetic) / sizeof(*kSynthetic); ++k) {
        const int base = kSynthetic[k].base;
        if (!loaded[base]) continue;
        const uint64_t size = sizes[base];
        EndgameSites sites;
        CHECK(endgame_unlocks_locate(loaded[base], size, kSynthetic[k].game, &sites) == 0 &&
              same_sites(&sites, &kRealBuilds[base].sites));
        check_install(loaded[base], size, kSynthetic[k].game, kRealBuilds[base].entry, &kRealBuilds[base].sites, BOTH, 5);

        unsigned char *none = with_stub_room(loaded[base], size, 0);
        none[kRealBuilds[base].sites.list_tier - 0x12 + 30] ^= 1;
        CHECK(endgame_unlocks_locate(none, size, kSynthetic[k].game, &sites) == 0 && no_sites(&sites));
        free(none);
        const size_t region = 0x100;
        unsigned char *twice = with_stub_room(loaded[base], size, region);
        memcpy(twice + size, loaded[base] + kRealBuilds[base].sites.row_tier - 0x22, 0x5b);
        CHECK(endgame_unlocks_locate(twice, size + region, kSynthetic[k].game, &sites) == 0 && no_sites(&sites));
        CHECK(endgame_unlocks_locate(twice, size + region, kRealBuilds[base].game, &sites) == 1 &&
              same_sites(&sites, &kRealBuilds[base].sites));
        free(twice);
    }
    for (int i = 0; i < kRealCount; ++i) free(loaded[i]);
}

/* The module reads records and answers checks: it never writes the save, story or difficulty, and stays out of Boss
 * Battle, online and the Story Skip. */
static void test_source(void) {
    FILE *f = fopen(DCP_SOURCE_DIR "/src/endgame_unlocks.c", "rb");
    CHECK(f != NULL);
    if (!f) return;
    static char text[65536];
    const size_t length = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[length] = 0;
    CHECK(!strstr(text, "boss") && !strstr(text, "online") && !strstr(text, "runtime_") && !strstr(text, "skip_offers"));
    CHECK(!strstr(text, "record[ENDGAME_STORY_STEP] = ") && !strstr(text, "record[ENDGAME_DIFFICULTY] = "));
    CHECK(!strstr(text, "DEBUG_SAULO"));
}

int main(void) {
    test_environment();
    test_native_answers();
    test_labyrinth_rule();
    test_prayer_rule();
    test_handlers();
    test_stub_execution();
    test_real_images();
    test_source();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("endgame unlocks tests passed\n");
    return 0;
}
