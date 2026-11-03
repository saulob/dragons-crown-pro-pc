/* Focused tests of Skip Intro: the one-shot press and the poll hook, with the real USA boot image under
 * build/image when it is there (read only). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skip_intro.h"

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
    set_env("DCP_SKIP_INTRO", NULL);
    CHECK(skip_intro_wanted(0) == 0 && skip_intro_wanted(1) == 1);
    set_env("DCP_SKIP_INTRO", "1");
    CHECK(skip_intro_wanted(0) == 1);
    set_env("DCP_SKIP_INTRO", "off");
    CHECK(skip_intro_wanted(1) == 0);
    set_env("DCP_SKIP_INTRO", "maybe");
    CHECK(skip_intro_wanted(1) == 1 && skip_intro_wanted(0) == 0);
    set_env("DCP_SKIP_INTRO", NULL);
}

static void test_claim(void) {
    CHECK(skip_intro_claim() == 0);
    skip_intro_arm();
    CHECK(skip_intro_claim() == 1);
    CHECK(skip_intro_claim() == 0 && skip_intro_claim() == 0);
    skip_intro_arm();
    skip_intro_arm();
    CHECK(skip_intro_claim() == 1 && skip_intro_claim() == 0);
}

enum { kWindowLength = 77, kHookInWindow = 19, kHookLength = 7 };
/* Displacements the code does not check: the stack-check load and the jump taken when the poll is false. */
static int is_unchecked(int i) {
    return (i >= 40 && i < 44) || (i >= 60 && i < 64);
}

static unsigned char *memmem_bytes(unsigned char *image, uint64_t size, const char *text) {
    const size_t length = strlen(text) + 1;
    for (uint64_t at = 0; at + length <= size; ++at)
        if (!memcmp(image + at, text, length)) return image + at;
    return NULL;
}

static const struct {
    const char *folder, *game;
    uint64_t window, entry, notice;
} kRealBuilds[] = {
    {"v1.03", "CUSA10454-1.00", 0x502076, 0x54c690, 0},
    {"CUSA07001-1.00", "CUSA07001-1.00", 0x4ffdc6, 0x54a800, 0x174b0},
    {"CUSA07001-1.02", "CUSA07001-1.02", 0x500356, 0x54a660, 0x174d0},
    {"CUSA07001-1.03", "CUSA07001-1.03", 0x500416, 0x54a720, 0x174d0},
    {"CUSA10209-1.00", "CUSA10209-1.00", 0x4ffdd6, 0x54a790, 0},
};
enum { kRealCount = sizeof(kRealBuilds) / sizeof(*kRealBuilds) };

/* Offsets in the notice step (from its first-frame test): the 27 displacements the code does not read back, the
 * construction the bypass replaces, and the object it keeps. */
static const unsigned kNoticeFields[] = {0xf,   0x17,  0x24,  0x31,  0x3c,  0x50,  0x5c,  0xac,  0xc0,
                                         0xcc,  0xdd,  0xec,  0xf6,  0x101, 0x10c, 0x114, 0x11c, 0x127,
                                         0x138, 0x153, 0x169, 0x177, 0x188, 0x198, 0x1aa, 0x1c6, 0x1d2};
enum {
    kNoticeLength = 470,
    kNoticeBegins = 0x1b,
    kMarker = 0x40,
    kMarkerLength = 15,
    kObject = 0xf0,
    kStackGuard = 0x109,
    kStepJump = 0x1d1,
    kBypassLength = SKIP_INTRO_BYPASS_BYTES
};

static int in_notice_field(unsigned offset) {
    for (size_t i = 0; i < sizeof(kNoticeFields) / sizeof(*kNoticeFields); ++i)
        if (offset >= kNoticeFields[i] && offset < kNoticeFields[i] + 4) return 1;
    return 0;
}

static int32_t rel32_at(const unsigned char *at) {
    int32_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

static unsigned char *with_stub_room(const unsigned char *loaded, uint64_t size, size_t room) {
    unsigned char *image = calloc(1, size + room);
    memcpy(image, loaded, size);
    return image;
}

/* The bypass reaches what the notice step's own object creation and its own completion reach, and nothing else. */
static void check_bypass(const unsigned char *image, const unsigned char *before, uint64_t size, uint64_t window) {
    const unsigned char *cave = image + window + kNoticeBegins, *step = before + window;
    const int64_t at = (int64_t)(window + kNoticeBegins), w = (int64_t)window;
    CHECK(!memcmp(cave, step + kObject, 5) && !memcmp(cave + 10, step + kObject + 10, 6));
    CHECK(cave[5] == 0xe8 && at + 10 + rel32_at(cave + 6) == w + kObject + 10 + rel32_at(step + kObject + 6));
    CHECK(cave[16] == 0xe8 && at + 21 + rel32_at(cave + 17) == w + kObject + 0x15 + rel32_at(step + kObject + 0x11));
    CHECK(!memcmp(cave + 21, step + kStackGuard, 3) &&
          at + 28 + rel32_at(cave + 24) == w + kStackGuard + 7 + rel32_at(step + kStackGuard + 3));
    CHECK(cave[28] == 0xe9);
    const int64_t advance = at + kBypassLength + rel32_at(cave + 29);

    /* The fade-finished branch that ends the notice natively lands on the same code. */
    static const unsigned char kDone[] = {0x49, 0x8b, 0x85, 0xd8, 0x00, 0x00, 0x00, 0xc5, 0xf8, 0x57, 0xc0,
                                          0xc5, 0xf8, 0x2e, 0x80, 0x18, 0x01, 0x00, 0x00, 0x0f, 0x83};
    int found = 0;
    int64_t native = -1;
    for (uint64_t k = 0; k + sizeof(kDone) + 4 <= size; ++k) {
        if (memcmp(before + k, kDone, sizeof(kDone))) continue;
        ++found;
        native = (int64_t)k + sizeof(kDone) + 4 + rel32_at(before + k + sizeof(kDone));
    }
    CHECK(found == 1 && advance == native);
    CHECK(!memcmp(before + advance, "\x49\x8b\x45\x00\x41\x8a\x4d\x5c", 8));
}

/* The patch of one image: only the hooked bytes, the notice's construction and the stub change, and the stub returns
 * after the hooked bytes. */
static void check_install(unsigned char *image, const unsigned char *before, uint64_t size, const char *game,
                          uint64_t entry, uint64_t hook, uint64_t notice) {
    const size_t stub_bytes = skip_intro_stub_bytes();
    CHECK(skip_intro_install(image, size, entry, game, 1, image + size, stub_bytes) == 1);
    const uint64_t cave = notice ? notice + kNoticeBegins : 0;
    size_t outside = 0;
    for (uint64_t k = 0; k < size; ++k) {
        const int hooked = k >= hook && k < hook + kHookLength, bypassed = cave && k >= cave && k < cave + kBypassLength;
        outside += image[k] != before[k] && !hooked && !bypassed;
    }
    CHECK(outside == 0 && image[hook] == 0xe9);
    if (notice) {
        check_bypass(image, before, size, notice);
        CHECK(memcmp(image + cave, before + cave, kBypassLength) != 0);
    }
    int32_t rel;
    memcpy(&rel, image + hook + 1, sizeof(rel));
    const long long stub = (long long)hook + 5 + rel;
    CHECK(stub == (long long)size);
    int returns_after = 0;
    const uint64_t resume = (uint64_t)(uintptr_t)(image + hook + kHookLength);
    for (long long p = stub; p + 8 <= (long long)(size + stub_bytes); ++p) returns_after += !memcmp(image + p, &resume, 8);
    CHECK(returns_after == 2);
    CHECK(skip_intro_claim() == 1 && skip_intro_claim() == 0);
}

enum { kAdvanceLength = 0x23, kTailLength = 10 };

/* Appends a second notice step: a copy of the step, of the advance code its jump reaches, and of the tail after it. The
 * query call is re-aimed at the original target, the step's jump at the copied tail. */
static unsigned char *with_second_notice(const unsigned char *loaded, uint64_t size, uint64_t window, size_t room,
                                         uint64_t *new_size) {
    const size_t copy = kNoticeLength + kAdvanceLength + kTailLength;
    unsigned char *twice = calloc(1, size + room + copy);
    memcpy(twice, loaded, size);
    unsigned char *step = twice + size;
    memcpy(step, loaded + window, kNoticeLength);
    const int64_t query = (int64_t)window + 0xe + 5 + rel32_at(loaded + window + 0xf);
    const int64_t tail = (int64_t)window + kStepJump + 5 + rel32_at(loaded + window + kStepJump + 1);
    memcpy(step + kNoticeLength, loaded + tail - kAdvanceLength, kAdvanceLength + kTailLength);
    const int32_t to_query = (int32_t)(query - ((int64_t)size + 0xe + 5));
    const int32_t to_tail = (int32_t)(kNoticeLength + kAdvanceLength - (kStepJump + 5));
    memcpy(step + 0xf, &to_query, sizeof(to_query));
    memcpy(step + kStepJump + 1, &to_tail, sizeof(to_tail));
    *new_size = size + copy;
    return twice;
}

/* The regional notice of one real image: where each build lists it, what the search finds, and what it refuses. */
static void test_notice_of_build(unsigned char *image, uint64_t size, int index) {
    const uint64_t window = kRealBuilds[index].notice;
    uint64_t site = 1;
    const SkipIntroNotice expected = window ? SKIP_INTRO_NOTICE_FOUND : SKIP_INTRO_NOTICE_ABSENT;
    CHECK(skip_intro_locate_notice(image, size, kRealBuilds[index].game, &site) == expected);
    CHECK(site == (window ? window + kNoticeBegins : 0));
    CHECK(skip_intro_locate_notice(image, size, "CUSA00000-9.99", &site) == expected);
    CHECK(site == (window ? window + kNoticeBegins : 0));

    /* A listed build's table is exact: another listed name is refused on this image unless it lists the same place. */
    for (int j = 0; j < kRealCount; ++j) {
        if (j == index) continue;
        const SkipIntroNotice other = skip_intro_locate_notice(image, size, kRealBuilds[j].game, &site);
        CHECK(other == (kRealBuilds[j].notice == window ? expected : SKIP_INTRO_NOTICE_UNMATCHED));
    }
    if (!window) return;

    /* Every byte of the step is verified except the displacements the code does not read back. The search, which has
     * no table, takes a damaged construction marker for another piece of code and refuses a damaged rest. */
    for (unsigned b = 0; b < kNoticeLength; ++b) {
        image[window + b] ^= 1;
        const SkipIntroNotice listed = skip_intro_locate_notice(image, size, kRealBuilds[index].game, &site);
        if (!in_notice_field(b)) CHECK(listed == SKIP_INTRO_NOTICE_UNMATCHED && site == 0);
        const SkipIntroNotice searched = skip_intro_locate_notice(image, size, "CUSA00000-9.99", &site);
        const int marker = b >= kMarker && b < kMarker + kMarkerLength;
        if (!in_notice_field(b)) CHECK(searched == (marker ? SKIP_INTRO_NOTICE_ABSENT : SKIP_INTRO_NOTICE_UNMATCHED) && site == 0);
        image[window + b] ^= 1;
    }
    CHECK(skip_intro_locate_notice(image, size, "CUSA00000-9.99", &site) == SKIP_INTRO_NOTICE_FOUND);
}

static void test_second_notice(const unsigned char *loaded, uint64_t size, int index) {
    const size_t room = skip_intro_stub_bytes();
    uint64_t new_size = 0, site = 1;
    unsigned char *twice = with_second_notice(loaded, size, kRealBuilds[index].notice, room, &new_size);
    CHECK(skip_intro_locate_notice(twice, new_size, "CUSA00000-9.99", &site) == SKIP_INTRO_NOTICE_UNMATCHED && site == 0);
    CHECK(skip_intro_install(twice, new_size, kRealBuilds[index].entry, "CUSA00000-9.99", 1, twice + new_size, room) == 0);
    CHECK(skip_intro_claim() == 0);
    free(twice);
}

static void test_real_images(void) {
    const size_t stub_bytes = skip_intro_stub_bytes();
    unsigned char *loaded[kRealCount] = {0};
    uint64_t sizes[kRealCount] = {0};
    for (int i = 0; i < kRealCount; ++i) loaded[i] = boot_image(kRealBuilds[i].folder, &sizes[i]);
    for (int i = 0; i < kRealCount; ++i) {
        if (!loaded[i]) {
            printf("no boot image for %s: skipped\n", kRealBuilds[i].folder);
            continue;
        }
        const uint64_t size = sizes[i], hook = kRealBuilds[i].window + kHookInWindow;
        unsigned char *image = with_stub_room(loaded[i], size, stub_bytes), *before = with_stub_room(loaded[i], size, stub_bytes);
        uint64_t site = 0;
        CHECK(skip_intro_locate(image, size, kRealBuilds[i].game, &site) == 1 && site == hook);
        CHECK(memcmp(image + hook, "\x4c\x89\xe7\x89\xc6\xff\xd3", kHookLength) == 0);
        site = 0;
        CHECK(skip_intro_locate(image, size, "CUSA00000-9.99", &site) == 0 && site == hook);
        for (int j = 0; j < kRealCount; ++j) {
            if (j != i) CHECK(skip_intro_locate(image, size, kRealBuilds[j].game, &site) == 1 && site == 0);
        }

        /* Every byte of the window is verified except two displacements the code does not read back. */
        for (int b = 0; b < kWindowLength; ++b) {
            image[kRealBuilds[i].window + b] ^= 1;
            const int rejected = skip_intro_locate(image, size, kRealBuilds[i].game, &site) == 1 && site == 0;
            CHECK(rejected == !is_unchecked(b));
            image[kRealBuilds[i].window + b] ^= 1;
        }
        /* The opening context is verified too: the layout path and the state dispatch. */
        unsigned char *layout = memmem_bytes(image, size, "STAGE/OPENING01.MBS");
        CHECK(layout != NULL);
        if (layout) {
            *layout ^= 1;
            CHECK(skip_intro_locate(image, size, kRealBuilds[i].game, &site) == 1 && site == 0);
            *layout ^= 1;
        }
        CHECK(!memcmp(image, before, size));

        test_notice_of_build(image, size, i);
        CHECK(!memcmp(image, before, size));
        if (kRealBuilds[i].notice) test_second_notice(loaded[i], size, i);

        CHECK(skip_intro_install(image, size, kRealBuilds[i].entry, NULL, 1, image + size, stub_bytes) == 0);
        CHECK(skip_intro_install(image, size, kRealBuilds[i].entry, kRealBuilds[i].game, 0, image + size, stub_bytes) == 0);
        CHECK(skip_intro_install(image, size, kRealBuilds[i].entry, kRealBuilds[i].game, 1, image + size, stub_bytes - 1) == 0);
        CHECK(!memcmp(image, before, size + stub_bytes) && skip_intro_claim() == 0);
        check_install(image, before, size, kRealBuilds[i].game, kRealBuilds[i].entry, hook, kRealBuilds[i].notice);
        printf("%s: opening poll hook at 0x%llx, %s\n", kRealBuilds[i].folder, (unsigned long long)hook,
               kRealBuilds[i].notice ? "regional notice bypassed" : "no regional notice");

        /* A name nothing lists is searched for both sites and gets the same patch. */
        unsigned char *searched = with_stub_room(loaded[i], size, stub_bytes), *searched_before = with_stub_room(loaded[i], size, stub_bytes);
        check_install(searched, searched_before, size, "CUSA00000-9.99", kRealBuilds[i].entry, hook, kRealBuilds[i].notice);
        CHECK(!memcmp(searched, image, size));
        free(searched), free(searched_before);
        free(image), free(before);
    }

    /* The two builds without a local image: a copy of the same build's code, searched for. Synthetic only. */
    static const struct {
        const char *game;
        int base;
    } kSynthetic[] = {{"CUSA10454-1.01", 0}, {"CUSA10209-1.02", 4}};
    for (size_t k = 0; k < sizeof(kSynthetic) / sizeof(*kSynthetic); ++k) {
        const int base = kSynthetic[k].base;
        if (!loaded[base]) continue;
        const uint64_t size = sizes[base], hook = kRealBuilds[base].window + kHookInWindow;
        unsigned char *image = with_stub_room(loaded[base], size, stub_bytes), *before = with_stub_room(loaded[base], size, stub_bytes);
        uint64_t site = 0;
        CHECK(skip_intro_locate(image, size, kSynthetic[k].game, &site) == 0 && site == hook);
        CHECK(skip_intro_locate_notice(image, size, kSynthetic[k].game, &site) == SKIP_INTRO_NOTICE_ABSENT && site == 0);
        check_install(image, before, size, kSynthetic[k].game, kRealBuilds[base].entry, hook, 0);
        free(image), free(before);

        /* Zero matches and two matches fail closed. */
        unsigned char *none = with_stub_room(loaded[base], size, stub_bytes);
        none[kRealBuilds[base].window + 4] ^= 1;
        CHECK(skip_intro_locate(none, size, kSynthetic[k].game, &site) == 0 && site == 0);
        CHECK(skip_intro_install(none, size, kRealBuilds[base].entry, kSynthetic[k].game, 1, none + size, stub_bytes) == 0);
        free(none);
        const size_t region = 0x700, from = kRealBuilds[base].window - 0x300;
        unsigned char *twice = with_stub_room(loaded[base], size, region);
        memcpy(twice + size, loaded[base] + from, region);
        const long long lea = (long long)size + 0x300 - 0x107, text = memmem_bytes(twice, size, "STAGE/OPENING01.MBS") - twice;
        const int32_t to_text = (int32_t)(text - (lea + 7));
        memcpy(twice + lea + 3, &to_text, sizeof(to_text));
        CHECK(skip_intro_locate(twice, size + region, kSynthetic[k].game, &site) == 0 && site == 0);
        free(twice);
    }
    for (int i = 0; i < kRealCount; ++i) free(loaded[i]);
}

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

static int g_claim_value, g_original_calls, g_delivered_calls;
static uint64_t g_original_rdi, g_original_rsi;
static int g_original_result;

static int GUEST_HOOK_ABI fake_claim(void) {
    return g_claim_value;
}
static void GUEST_HOOK_ABI fake_delivered(void) {
    ++g_delivered_calls;
}
static int GUEST_HOOK_ABI fake_input(uint64_t self, uint64_t player) {
    ++g_original_calls;
    g_original_rdi = self;
    g_original_rsi = player;
    return g_original_result;
}

typedef struct {
    uint64_t rsp_before, rax, rsp_after, rbx, r12, rbp, r14, r15;
} Landing;
typedef uint64_t (GUEST_HOOK_ABI *Wrapper)(uint64_t r12, uint64_t eax, uint64_t rbx, Landing *out);

enum { kStubAt = 512, kSentinel = 0x5151515151515151ull };

static void emit(unsigned char **at, const void *bytes, size_t count) {
    memcpy(*at, bytes, count);
    *at += count;
}
static void emit64(unsigned char **at, const unsigned char *prefix, size_t count, uint64_t value) {
    emit(at, prefix, count);
    emit(at, &value, 8);
}

/* Enters the stub by a jump with rsp 16-aligned, r12/rbx/eax set, and records the registers where it resumes. */
static Wrapper build_wrapper(unsigned char *memory, uint64_t claim, uint64_t delivered) {
    unsigned char *at = memory;
    static const unsigned char kEnter[] = {0x53, 0x41, 0x54, 0x55, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x08,
                                           0x49, 0x89, 0xcd, 0x49, 0x89, 0xfc, 0x89, 0xf0, 0x48, 0x89, 0xd3};
    emit(&at, kEnter, sizeof(kEnter));
    emit64(&at, (const unsigned char[]){0x48, 0xbd}, 2, kSentinel);
    emit64(&at, (const unsigned char[]){0x49, 0xbe}, 2, kSentinel + 1);
    emit64(&at, (const unsigned char[]){0x49, 0xbf}, 2, kSentinel + 2);
    emit(&at, (const unsigned char[]){0x49, 0x89, 0x65, 0x00}, 4);
    unsigned char stub[96];
    memset(stub, 0xcc, sizeof(stub));
    const size_t wrapper_tail = 64;
    unsigned char *landing = memory + kStubAt - wrapper_tail;
    skip_intro_build_stub(stub, claim, delivered, (uint64_t)(uintptr_t)landing);
    memcpy(memory + kStubAt, stub, sizeof(stub));
    emit64(&at, (const unsigned char[]){0x49, 0xbb}, 2, (uint64_t)(uintptr_t)(memory + kStubAt));
    emit(&at, (const unsigned char[]){0x41, 0xff, 0xe3}, 3);
    at = landing;
    static const unsigned char kLand[] = {0x49, 0x89, 0x45, 0x08, 0x49, 0x89, 0x65, 0x10, 0x49, 0x89, 0x5d, 0x18,
                                          0x4d, 0x89, 0x65, 0x20, 0x49, 0x89, 0x6d, 0x28, 0x4d, 0x89, 0x75, 0x30,
                                          0x4d, 0x89, 0x7d, 0x38, 0x48, 0x83, 0xc4, 0x08, 0x41, 0x5f, 0x41, 0x5e,
                                          0x41, 0x5d, 0x5d, 0x41, 0x5c, 0x5b, 0xc3};
    emit(&at, kLand, sizeof(kLand));
    return (Wrapper)(uintptr_t)memory;
}

static uint64_t run_stub(Wrapper wrapper, uint64_t r12, uint32_t eax, Landing *landing) {
    memset(landing, 0, sizeof(*landing));
    g_original_calls = g_delivered_calls = 0;
    wrapper(r12, eax, (uint64_t)(uintptr_t)fake_input, landing);
    return landing->rax;
}

static void check_landing(const Landing *landing, uint64_t r12) {
    CHECK(landing->rsp_after == landing->rsp_before);
    CHECK(landing->rbx == (uint64_t)(uintptr_t)fake_input && landing->r12 == r12);
    CHECK(landing->rbp == kSentinel && landing->r14 == kSentinel + 1 && landing->r15 == kSentinel + 2);
}

static void test_stub_execution(void) {
    unsigned char *memory = executable_memory(4096);
    if (!memory) {
        printf("no executable memory: stub execution skipped\n");
        return;
    }
    Landing landing;
    const uint64_t r12 = 0xA1B2C3D4E5F60708ull;
    Wrapper wrapper = build_wrapper(memory, (uint64_t)(uintptr_t)fake_claim, (uint64_t)(uintptr_t)fake_delivered);

    g_claim_value = 0, g_original_result = 0;
    CHECK((uint32_t)run_stub(wrapper, r12, 7, &landing) == 0 && g_original_calls == 1 && g_delivered_calls == 0);
    CHECK(g_original_rdi == r12 && g_original_rsi == 7);
    check_landing(&landing, r12);
    g_original_result = 1;
    CHECK((uint32_t)run_stub(wrapper, r12, 9, &landing) == 1 && g_original_calls == 1 && g_original_rsi == 9);
    check_landing(&landing, r12);

    g_claim_value = 1, g_original_result = 0;
    CHECK((uint32_t)run_stub(wrapper, r12, 7, &landing) == 1 && g_original_calls == 0 && g_delivered_calls == 1);
    check_landing(&landing, r12);

    /* The real one-shot: the first poll is a press that never reaches the original, the next ones are the original. */
    wrapper = build_wrapper(memory, (uint64_t)(uintptr_t)skip_intro_claim, (uint64_t)(uintptr_t)fake_delivered);
    skip_intro_arm();
    g_original_result = 0;
    CHECK((uint32_t)run_stub(wrapper, r12, 3, &landing) == 1 && g_original_calls == 0 && g_delivered_calls == 1);
    check_landing(&landing, r12);
    CHECK((uint32_t)run_stub(wrapper, r12, 3, &landing) == 0 && g_original_calls == 1 && g_delivered_calls == 0);
    g_original_result = 1;
    CHECK((uint32_t)run_stub(wrapper, r12, 3, &landing) == 1 && g_original_calls == 1);
    check_landing(&landing, r12);
}

int main(void) {
    set_env("DCP_SKIP_INTRO", NULL);
    test_environment();
    test_claim();
    test_stub_execution();
    test_real_images();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("skip intro tests passed\n");
    return 0;
}
