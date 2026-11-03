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
    uint64_t window, entry;
} kRealBuilds[] = {
    {"v1.03", "CUSA10454-1.00", 0x502076, 0x54c690},
    {"CUSA07001-1.00", "CUSA07001-1.00", 0x4ffdc6, 0x54a800},
    {"CUSA07001-1.02", "CUSA07001-1.02", 0x500356, 0x54a660},
    {"CUSA07001-1.03", "CUSA07001-1.03", 0x500416, 0x54a720},
    {"CUSA10209-1.00", "CUSA10209-1.00", 0x4ffdd6, 0x54a790},
};
enum { kRealCount = sizeof(kRealBuilds) / sizeof(*kRealBuilds) };

static unsigned char *with_stub_room(const unsigned char *loaded, uint64_t size, size_t room) {
    unsigned char *image = calloc(1, size + room);
    memcpy(image, loaded, size);
    return image;
}

/* The patch of one image: only the hooked bytes and the stub change, and the stub returns after them. */
static void check_install(unsigned char *image, const unsigned char *before, uint64_t size, const char *game,
                          uint64_t entry, uint64_t hook) {
    const size_t stub_bytes = skip_intro_stub_bytes();
    CHECK(skip_intro_install(image, size, entry, game, 1, image + size, stub_bytes) == 1);
    size_t outside = 0;
    for (uint64_t k = 0; k < size; ++k) outside += image[k] != before[k] && (k < hook || k >= hook + kHookLength);
    CHECK(outside == 0 && image[hook] == 0xe9);
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

        CHECK(skip_intro_install(image, size, kRealBuilds[i].entry, NULL, 1, image + size, stub_bytes) == 0);
        CHECK(skip_intro_install(image, size, kRealBuilds[i].entry, kRealBuilds[i].game, 0, image + size, stub_bytes) == 0);
        CHECK(skip_intro_install(image, size, kRealBuilds[i].entry, kRealBuilds[i].game, 1, image + size, stub_bytes - 1) == 0);
        CHECK(!memcmp(image, before, size + stub_bytes) && skip_intro_claim() == 0);
        check_install(image, before, size, kRealBuilds[i].game, kRealBuilds[i].entry, hook);
        printf("%s: opening poll hook at 0x%llx\n", kRealBuilds[i].folder, (unsigned long long)hook);
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
        check_install(image, before, size, kSynthetic[k].game, kRealBuilds[base].entry, hook);
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
