/* Focused tests of Skip Intro: the startup redirect and its stubs, the regional notice, and the Issue #79 state model,
 * with the real boot images under build/image when they are there (read only). */
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
    if (!f) { /* a build the launcher has not prepared here: its research copy */
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

static int32_t rel32_at(const unsigned char *at) {
    int32_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

static int64_t call_target(const unsigned char *image, int64_t at) {
    return at + 5 + rel32_at(image + at + 1);
}

/* ---- the stubs, executed ---- */

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

typedef int(GUEST_HOOK_ABI *Question)(void *self);
typedef void *(GUEST_HOOK_ABI *Create)(void *manager);

/* The object the opening step asks: its vtable's 0x260 slot is the question. */
typedef struct {
    void **vtable;
} Asked;

static int g_claim_value, g_questions, g_redirects, g_reached, g_creates, g_misaligned, g_answer;
static void *g_asked_self, *g_created_for;

static void note_alignment(void) {
    g_misaligned += ((uintptr_t)__builtin_frame_address(0) & 15) != 0;
}
static int GUEST_HOOK_ABI fake_claim(void) {
    note_alignment();
    return g_claim_value;
}
static void GUEST_HOOK_ABI fake_redirected(void) {
    note_alignment();
    ++g_redirects;
}
static int GUEST_HOOK_ABI fake_question(void *self) {
    note_alignment();
    ++g_questions;
    g_asked_self = self;
    return g_answer;
}
static void GUEST_HOOK_ABI fake_reached(void) {
    note_alignment();
    g_reached += 1 + g_creates;
}
static void *GUEST_HOOK_ABI fake_create(void *manager) {
    note_alignment();
    ++g_creates;
    g_created_for = manager;
    return (char *)manager + 1;
}

static void *g_vtable[0x270 / sizeof(void *)];
static Asked g_asked = {g_vtable};

static void reset_counts(void) {
    g_questions = g_redirects = g_reached = g_creates = g_misaligned = 0;
    g_asked_self = g_created_for = NULL;
}

static void test_stub_execution(void) {
    unsigned char *memory = executable_memory(4096);
    if (!memory) {
        printf("no executable memory: stub execution skipped\n");
        return;
    }
    g_vtable[0x260 / sizeof(void *)] = (void *)(uintptr_t)fake_question;
    memset(memory, 0xcc, 4096);
    const size_t redirect_length = skip_intro_build_redirect_stub(memory, (uint64_t)(uintptr_t)fake_claim,
                                                                  (uint64_t)(uintptr_t)fake_redirected);
    CHECK(redirect_length <= 56);
    const Question redirect = (Question)(uintptr_t)memory;

    /* Not claimed: the game's own question, with its own object and answer. */
    for (g_answer = 0; g_answer <= 1; ++g_answer) {
        reset_counts();
        g_claim_value = 0;
        CHECK(redirect(&g_asked) == g_answer && g_questions == 1 && g_redirects == 0 && g_asked_self == &g_asked);
        CHECK(g_misaligned == 0);
    }
    /* Claimed: "not needed" without asking. */
    reset_counts();
    g_claim_value = 1, g_answer = 0;
    CHECK(redirect(&g_asked) == 1 && g_questions == 0 && g_redirects == 1 && g_misaligned == 0);

    /* The real one-shot: the first question at startup is answered, the later ones are the game's. */
    skip_intro_build_redirect_stub(memory, (uint64_t)(uintptr_t)skip_intro_claim, (uint64_t)(uintptr_t)fake_redirected);
    reset_counts();
    skip_intro_arm();
    g_answer = 0;
    CHECK(redirect(&g_asked) == 1 && g_questions == 0 && g_redirects == 1);
    CHECK(redirect(&g_asked) == 0 && g_questions == 1 && g_redirects == 1);
    CHECK(g_misaligned == 0);

    unsigned char *title_at = memory + 1024;
    const size_t title_length = skip_intro_build_title_stub(title_at, (uint64_t)(uintptr_t)fake_reached,
                                                            (uint64_t)(uintptr_t)fake_create);
    CHECK(title_length <= 96 - 56);
    const Create title = (Create)(uintptr_t)title_at;
    reset_counts();
    char manager[4];
    CHECK(title(manager) == manager + 1 && g_creates == 1 && g_created_for == manager);
    CHECK(g_reached == 1 && g_misaligned == 0); /* reported before the scene is created */
}

/* ---- the title flow in the real images ---- */

enum {
    kOpeningLength = 0x56,
    kTitleLength = 0x35,
    kDoneLength = 0x42,
    kTitleCreateLength = 0x36,
    kSaveLength = 0x3a,
    kDecisionLength = 0x44,
    kDispatchLength = 0x1f,
    kRedirectInStep = 0x16,
    kTitleCallInCreate = 0x27,
    kFactoryHeadLength = 23,
    kFactoryDispatchLength = 27,
    kFactoryCaseLength = 24
};

static const struct {
    const char *folder, *game;
    uint64_t entry, opening, save, dispatch, decision, title_create, factory, notice, old_poll;
} kRealBuilds[] = {
    {"v1.03", "CUSA10454-1.00", 0x54c690, 0x428b3f, 0x428b05, 0x428a28, 0x428ac1, 0x428c7a, 0x4af4b0, 0, 0x502076},
    {"CUSA10454-1.01", "CUSA10454-1.01", 0x54c690, 0x428b3f, 0x428b05, 0x428a28, 0x428ac1, 0x428c7a, 0x4af4b0, 0, 0x502076},
    {"CUSA07001-1.00", "CUSA07001-1.00", 0x54a800, 0x427d9f, 0x427d65, 0x427c88, 0x427d21, 0x427eda, 0x4ad7c0, 0x174b0, 0x4ffdc6},
    {"CUSA07001-1.02", "CUSA07001-1.02", 0x54a660, 0x4281bf, 0x428185, 0x4280a8, 0x428141, 0x4282fa, 0x4adc40, 0x174d0, 0x500356},
    {"CUSA07001-1.03", "CUSA07001-1.03", 0x54a720, 0x42820f, 0x4281d5, 0x4280f8, 0x428191, 0x42834a, 0x4add00, 0x174d0, 0x500416},
    {"CUSA10209-1.00", "CUSA10209-1.00", 0x54a790, 0x427f0f, 0x427ed5, 0x427df8, 0x427e91, 0x42804a, 0x4ad930, 0, 0x4ffdd6},
};
enum { kRealCount = sizeof(kRealBuilds) / sizeof(*kRealBuilds) };

typedef struct {
    const char *name;
    uint64_t at, length;
    const unsigned unchecked[5]; /* 4-byte fields nothing reads back; 0 ends the list */
} Window;

static int unchecked_byte(const Window *w, unsigned b) {
    for (int i = 0; i < 5 && w->unchecked[i]; ++i)
        if (b >= w->unchecked[i] && b < w->unchecked[i] + 4) return 1;
    return 0;
}

static uint64_t factory_case(const unsigned char *image, uint64_t factory, unsigned scene) {
    const int64_t table = (int64_t)factory + 0x60 + 0x12 + rel32_at(image + factory + 0x60 + 0x0e);
    return (uint64_t)(table + rel32_at(image + table + 4 * (scene - 1)));
}

static uint64_t step_table(const unsigned char *image, uint64_t dispatch) {
    return dispatch + 0x16 + rel32_at(image + dispatch + 0x12);
}

static int sites_are(const SkipIntroSites *sites, uint64_t redirect, uint64_t title) {
    return sites->redirect == redirect && sites->title == title;
}

static int listed_rejects(const unsigned char *image, uint64_t size, const char *game) {
    SkipIntroSites sites = {1, 1};
    return skip_intro_locate(image, size, game, &sites) == 1 && sites.redirect == 0 && sites.title == 0;
}

static void test_title_flow_mutations(unsigned char *image, uint64_t size, int index) {
    const char *game = kRealBuilds[index].game;
    const uint64_t opening = kRealBuilds[index].opening, factory = kRealBuilds[index].factory;
    const uint64_t decision_save = kRealBuilds[index].decision + 0x28 + rel32_at(image + kRealBuilds[index].decision + 0x24);
    const Window windows[] = {
        {"opening step", opening, kOpeningLength, {0x49}},
        {"title step", opening + kOpeningLength, kTitleLength, {0x15}},
        {"end of step", opening + kOpeningLength + kTitleLength, kDoneLength, {0x0d, 0x17, 0x2f, 0x3e}},
        {"title creation", kRealBuilds[index].title_create, kTitleCreateLength, {0x14}},
        {"save step", kRealBuilds[index].save, kSaveLength, {0x34}},
        {"decision", kRealBuilds[index].decision, kDecisionLength, {0x03, 0x1e, 0x31, 0x40}},
        {"decision to save", decision_save, 19, {0}},
        {"dispatch", kRealBuilds[index].dispatch, kDispatchLength, {0}},
        {"factory", factory, kFactoryHeadLength, {0}},
        {"factory dispatch", factory + 0x60, kFactoryDispatchLength, {0x07}},
        {"save scene case", factory_case(image, factory, 1), kFactoryCaseLength, {0x01, 0x06}},
        {"opening scene case", factory_case(image, factory, 2), kFactoryCaseLength, {0x01, 0x06}},
        {"title scene case", factory_case(image, factory, 7), kFactoryCaseLength, {0x01, 0x06}},
    };
    int wrong = 0;
    for (size_t w = 0; w < sizeof(windows) / sizeof(*windows); ++w) {
        for (unsigned b = 0; b < windows[w].length; ++b) {
            /* the one-byte branch distance from step 2 to the save data step is not read back */
            if (w == 6 && b == 8) continue;
            image[windows[w].at + b] ^= 1;
            const int rejected = listed_rejects(image, size, game);
            if (rejected == unchecked_byte(&windows[w], b)) {
                if (!wrong++) printf("%s: %s byte 0x%x %s\n", game, windows[w].name, b, rejected ? "rejected" : "accepted");
            }
            image[windows[w].at + b] ^= 1;
        }
    }
    CHECK(wrong == 0);

    /* The step table: the decision, save, opening and title entries are verified; the others are not read. */
    const uint64_t table = step_table(image, kRealBuilds[index].dispatch);
    for (int e = 0; e < 7; ++e) {
        image[table + 4 * e] ^= 1;
        CHECK(listed_rejects(image, size, game) == (e >= 2 && e <= 5));
        image[table + 4 * e] ^= 1;
    }
    /* Scene identity: the id each constructor gives its base, and that base. */
    for (unsigned scene = 1; scene <= 7; scene += scene == 2 ? 5 : 1) {
        const uint64_t ctor = (uint64_t)call_target(image, (int64_t)factory_case(image, factory, scene) + 0x13);
        const unsigned char pass_id[] = {0xba, (unsigned char)scene, 0, 0, 0, 0x48, 0x89, 0xfb, 0xe8};
        uint64_t at = 0;
        for (uint64_t k = ctor; k < ctor + 0x20 && !at; ++k)
            if (!memcmp(image + k, pass_id, sizeof(pass_id))) at = k;
        CHECK(at != 0);
        image[at + 1] ^= 4;
        CHECK(listed_rejects(image, size, game));
        image[at + 1] ^= 4;
        image[at + 9] ^= 1;
        CHECK(listed_rejects(image, size, game));
        image[at + 9] ^= 1;
    }
    /* The scene getters each step uses: the opening step must read the opening scene's slot. */
    const uint64_t getter = (uint64_t)call_target(image, (int64_t)opening + 3);
    image[getter + 3] ^= 0x40;
    CHECK(listed_rejects(image, size, game));
    image[getter + 3] ^= 0x40;
    /* The create thunk's scene id. */
    const uint64_t create = (uint64_t)call_target(image, (int64_t)opening + 0x34);
    image[create + 1] ^= 1;
    CHECK(listed_rejects(image, size, game));
    image[create + 1] ^= 1;
    SkipIntroSites sites;
    CHECK(skip_intro_locate(image, size, game, &sites) == 1 && sites.redirect);
}

/* ---- the regional notice (Japanese builds) ---- */

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

/* The patch of one image: only the question call, the title call, the notice's construction and the stub area change;
 * both calls reach the stubs, and the title stub goes on to the original creation. */
static void check_install(unsigned char *image, const unsigned char *before, uint64_t size, const char *game,
                          uint64_t entry, const SkipIntroSites *sites, uint64_t notice) {
    const size_t stub_bytes = skip_intro_stub_bytes();
    CHECK(skip_intro_install(image, size, entry, game, 1, image + size, stub_bytes) == 1);
    const uint64_t cave = notice ? notice + kNoticeBegins : 0;
    size_t outside = 0;
    for (uint64_t k = 0; k < size; ++k) {
        const int redirected = k >= sites->redirect && k < sites->redirect + SKIP_INTRO_REDIRECT_BYTES;
        const int title = k >= sites->title && k < sites->title + SKIP_INTRO_TITLE_CALL_BYTES;
        const int bypassed = cave && k >= cave && k < cave + kBypassLength;
        outside += image[k] != before[k] && !redirected && !title && !bypassed;
    }
    CHECK(outside == 0);
    CHECK(!memcmp(before + sites->redirect, "\xff\x90\x60\x02\x00\x00", 6) && before[sites->title] == 0xe8);
    CHECK(image[sites->redirect] == 0xe8 && image[sites->redirect + 5] == 0x90 && image[sites->title] == 0xe8);
    CHECK(call_target(image, (int64_t)sites->redirect) == (int64_t)size);
    CHECK(call_target(image, (int64_t)sites->title) == (int64_t)size + 56);
    if (notice) {
        check_bypass(image, before, size, notice);
        CHECK(memcmp(image + cave, before + cave, kBypassLength) != 0);
    }
    const uint64_t create = (uint64_t)(uintptr_t)(image + call_target(before, (int64_t)sites->title));
    int creates = 0;
    for (uint64_t p = size + 56; p + 8 <= size + stub_bytes; ++p) creates += !memcmp(image + p, &create, 8);
    CHECK(creates == 1);
    int vtable_jumps = 0;
    for (uint64_t p = size; p + 6 <= size + 56; ++p) vtable_jumps += !memcmp(image + p, "\xff\xa0\x60\x02\x00\x00", 6);
    CHECK(vtable_jumps == 1);
    CHECK(skip_intro_claim() == 1 && skip_intro_claim() == 0);
}

/* Off writes nothing anywhere, and so do the refusals. */
static void check_untouched(unsigned char *image, const unsigned char *before, uint64_t size, const char *game, uint64_t entry) {
    const size_t stub_bytes = skip_intro_stub_bytes();
    CHECK(skip_intro_install(image, size, entry, game, 0, image + size, stub_bytes) == 0);
    CHECK(skip_intro_install(image, size, entry, NULL, 1, image + size, stub_bytes) == 0);
    CHECK(skip_intro_install(image, size, entry, game, 1, image + size, stub_bytes - 1) == 0);
    CHECK(!memcmp(image, before, size + stub_bytes) && skip_intro_claim() == 0);
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

/* ---- Issue #79: the old poll came after the logos ---- */

static int64_t find_bytes(const unsigned char *image, int64_t from, int64_t to, const void *bytes, size_t count) {
    for (int64_t at = from; at + (int64_t)count <= to; ++at)
        if (!memcmp(image + at, bytes, count)) return at;
    return -1;
}

static int lea_to_text(const unsigned char *image, uint64_t size, int64_t from, int64_t to, const char *text) {
    for (int64_t at = from; at + 7 <= to; ++at) {
        if (image[at + 1] != 0x8d || (image[at] != 0x48 && image[at] != 0x4c) || (image[at + 2] & 0xc7) != 0x05) continue;
        const int64_t target = at + 7 + rel32_at(image + at + 3);
        if (target > 0 && (uint64_t)target + strlen(text) + 1 <= size && !memcmp(image + target, text, strlen(text) + 1)) return 1;
    }
    return 0;
}

/* The opening scene (scene 2) begins at part 0, loads STAGE/OPENING%02d (part 0 is OPENING00, the ATLUS, Basiscape and
 * CRIWARE logos) in its state 0 and only polls for a press in state 1, once that part plays; a press then starts a
 * 60-frame fade with the part still drawn. The old hook answered that poll, so the logos always began first. */
static void test_issue79_evidence(const unsigned char *image, uint64_t size, int index) {
    const uint64_t factory = kRealBuilds[index].factory, poll = kRealBuilds[index].old_poll;
    const int64_t ctor = call_target(image, (int64_t)factory_case(image, factory, 2) + 0x13);
    CHECK(find_bytes(image, ctor, ctor + 0x90, "\xc7\x83\x7c\x09\x00\x00\x00\x00\x00\x00", 10) > 0);

    CHECK(!memcmp(image + poll + 19, "\x4c\x89\xe7\x89\xc6\xff\xd3\x84\xc0\x0f\x84", 11));
    const int64_t state2 = find_bytes(image, (int64_t)poll - 0x300, (int64_t)poll, "\x80\xfb\x02\x0f\x84", 5);
    const int64_t state1 = find_bytes(image, (int64_t)poll - 0x300, (int64_t)poll, "\x80\xfb\x01\x0f\x84", 5);
    CHECK(state2 > 0 && state1 > state2);
    if (state1 < 0 || state2 < 0) return;
    const int64_t state1_code = state1 + 9 + rel32_at(image + state1 + 5), state2_code = state2 + 9 + rel32_at(image + state2 + 5);
    CHECK(state1_code < (int64_t)poll && (int64_t)poll < state2_code);

    const int64_t load = find_bytes(image, state1 + 9, state1_code, "\x41\x8b\xb4\x24\x7c\x09\x00\x00\xe8", 9);
    CHECK(load > 0);
    if (load < 0) return;
    const int64_t probe = call_target(image, load + 8);
    CHECK(lea_to_text(image, size, probe, probe + 0x80, "STAGE/OPENING%02d.MBS"));
    CHECK(lea_to_text(image, size, state1 + 9, state1_code, "STAGE/OPENING01.MBS"));
    CHECK(find_bytes(image, state2_code, state2_code + 0x60, "\xb8\x3c\x00\x00\x00", 5) > 0);
}

typedef struct {
    int returning;   /* step 2's flag: back to the title from the game */
    int save_frames; /* frames the save data scene is up: 1 with a save, more with the first-boot prompt */
    int old_hook;    /* the poll answered "pressed" once */
    Question opening_question;
    /* observed */
    int frame, save_last, redirect_frame, title_frame, opening_created, logo_frames, opening_frames, titles;
} Flow;

enum { kLogoPartFrames = 600, kOpeningPartFrames = 2400, kFadeFrames = 60, kTitleIdleFrames = 1800 };

/* Steps 2 to 5 of the title flow and the opening scene, as the images verify them, until `rounds` titles were shown;
 * every title before the last times out back to step 4 as the idle title does. */
static void run_flow(Flow *flow, int rounds) {
    int step = 2, opening_state = -1, part = 0, part_frame = 0, fade = 0, save_left = flow->save_frames, title_left = 0;
    flow->save_last = flow->redirect_frame = flow->title_frame = -1;
    flow->opening_created = flow->logo_frames = flow->opening_frames = flow->titles = 0;
    for (flow->frame = 0; flow->frame < 20000 && flow->titles < rounds; ++flow->frame) {
        switch (step) {
        case 2: step = flow->returning ? 5 : 3; break;
        case 3:
            flow->save_last = flow->frame;
            if (--save_left <= 0) step = 4;
            break;
        case 4:
            if (opening_state < 0) {
                if (flow->opening_question(&g_asked)) {
                    flow->redirect_frame = flow->frame;
                    step = 5;
                    break;
                }
                opening_state = 0, part = 0, part_frame = 0, ++flow->opening_created;
                break;
            }
            if (opening_state == 0) {
                opening_state = 1;
                break;
            }
            if (opening_state == 1 && flow->old_hook && skip_intro_claim()) opening_state = 2, fade = kFadeFrames;
            if (part == 0) ++flow->logo_frames;
            else ++flow->opening_frames;
            if (opening_state == 2 && --fade <= 0) opening_state = 3;
            if (opening_state == 1 && ++part_frame >= (part == 0 ? kLogoPartFrames : kOpeningPartFrames)) {
                part_frame = 0;
                if (++part == 2) opening_state = 3;
            }
            if (opening_state == 3) opening_state = -1, step = 5;
            break;
        case 5:
            if (!title_left) {
                ++flow->titles;
                if (flow->title_frame < 0) flow->title_frame = flow->frame;
                title_left = kTitleIdleFrames;
            } else if (--title_left == 0) {
                step = 4;
            }
            break;
        }
    }
}

static void test_issue79_model(void) {
    g_vtable[0x260 / sizeof(void *)] = (void *)(uintptr_t)fake_question;
    g_answer = 0; /* the game's own answer on PS4: the opening is always needed */
    unsigned char *memory = executable_memory(4096);
    if (!memory) {
        printf("no executable memory: Issue #79 model skipped\n");
        return;
    }
    skip_intro_build_redirect_stub(memory, (uint64_t)(uintptr_t)skip_intro_claim, (uint64_t)(uintptr_t)fake_redirected);
    const Question stub = (Question)(uintptr_t)memory;

    /* Off, and the old hook: the logos begin before anything is pressed. */
    while (skip_intro_claim()) {}
    Flow vanilla = {.save_frames = 1, .opening_question = fake_question};
    run_flow(&vanilla, 1);
    CHECK(vanilla.opening_created == 1 && vanilla.logo_frames == kLogoPartFrames && vanilla.opening_frames == kOpeningPartFrames);
    skip_intro_arm();
    Flow old = {.save_frames = 1, .old_hook = 1, .opening_question = fake_question};
    run_flow(&old, 1);
    CHECK(old.opening_created == 1 && old.logo_frames == kFadeFrames && old.opening_frames == 0);
    CHECK(old.title_frame > old.save_last);

    /* The redirect: existing save, first-boot prompt; the save data step always runs first and the logos never start. */
    const int save_frames[] = {1, 240};
    for (size_t i = 0; i < sizeof(save_frames) / sizeof(*save_frames); ++i) {
        skip_intro_arm();
        Flow redirect = {.save_frames = save_frames[i], .opening_question = stub};
        run_flow(&redirect, 1);
        CHECK(redirect.opening_created == 0 && redirect.logo_frames == 0 && redirect.opening_frames == 0);
        CHECK(redirect.save_last >= save_frames[i] && redirect.redirect_frame > redirect.save_last);
        CHECK(redirect.title_frame == redirect.redirect_frame + 1 && redirect.titles == 1);
        CHECK(skip_intro_claim() == 0);
    }

    /* The idle title replays the opening as the game does; only the startup is redirected. */
    skip_intro_arm();
    Flow idle = {.save_frames = 1, .opening_question = stub};
    run_flow(&idle, 3);
    CHECK(idle.titles == 3 && idle.opening_created == 2 && idle.logo_frames == 2 * kLogoPartFrames);
    CHECK(idle.redirect_frame >= 0 && idle.redirect_frame < idle.title_frame);

    /* Back to the title from the game: step 2 already skips the save data and opening steps, and the claim stays. */
    skip_intro_arm();
    Flow back = {.returning = 1, .save_frames = 1, .opening_question = stub};
    run_flow(&back, 1);
    CHECK(back.save_last < 0 && back.opening_created == 0 && back.redirect_frame < 0);
    CHECK(skip_intro_claim() == 1);
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
        const uint64_t size = sizes[i];
        const SkipIntroSites expected = {kRealBuilds[i].opening + kRedirectInStep, kRealBuilds[i].title_create + kTitleCallInCreate};
        unsigned char *image = with_stub_room(loaded[i], size, stub_bytes), *before = with_stub_room(loaded[i], size, stub_bytes);
        SkipIntroSites sites;
        CHECK(skip_intro_locate(image, size, kRealBuilds[i].game, &sites) == 1 && sites_are(&sites, expected.redirect, expected.title));
        CHECK(skip_intro_locate(image, size, "CUSA00000-9.99", &sites) == 0 && sites_are(&sites, expected.redirect, expected.title));
        CHECK(skip_intro_locate(image, size, NULL, &sites) == 0 && sites_are(&sites, 0, 0));
        for (int j = 0; j < kRealCount; ++j) {
            if (j == i) continue;
            const int same_code = kRealBuilds[j].opening == kRealBuilds[i].opening && kRealBuilds[j].entry == kRealBuilds[i].entry;
            CHECK(skip_intro_locate(image, size, kRealBuilds[j].game, &sites) == 1 &&
                  sites_are(&sites, same_code ? expected.redirect : 0, same_code ? expected.title : 0));
        }
        test_title_flow_mutations(image, size, i);
        CHECK(!memcmp(image, before, size));
        test_issue79_evidence(image, size, i);

        test_notice_of_build(image, size, i);
        CHECK(!memcmp(image, before, size));
        if (kRealBuilds[i].notice) test_second_notice(loaded[i], size, i);

        check_untouched(image, before, size, kRealBuilds[i].game, kRealBuilds[i].entry);
        check_install(image, before, size, kRealBuilds[i].game, kRealBuilds[i].entry, &expected, kRealBuilds[i].notice);
        printf("%s: startup redirect at 0x%llx, title report at 0x%llx, %s\n", kRealBuilds[i].folder,
               (unsigned long long)expected.redirect, (unsigned long long)expected.title,
               kRealBuilds[i].notice ? "regional notice bypassed" : "no regional notice");

        /* A name nothing lists is searched for every site and gets the same patch. */
        unsigned char *searched = with_stub_room(loaded[i], size, stub_bytes), *searched_before = with_stub_room(loaded[i], size, stub_bytes);
        check_install(searched, searched_before, size, "CUSA00000-9.99", kRealBuilds[i].entry, &expected, kRealBuilds[i].notice);
        CHECK(!memcmp(searched, image, size));
        free(searched), free(searched_before);
        free(image), free(before);
    }

    /* The two builds without a local image: a copy of the same build's code, searched for. Synthetic only. */
    static const struct {
        const char *game;
        int base;
    } kSynthetic[] = {{"CUSA10487-1.03", 0}, {"CUSA10209-1.02", 5}};
    for (size_t k = 0; k < sizeof(kSynthetic) / sizeof(*kSynthetic); ++k) {
        const int base = kSynthetic[k].base;
        if (!loaded[base]) continue;
        const uint64_t size = sizes[base];
        const SkipIntroSites expected = {kRealBuilds[base].opening + kRedirectInStep, kRealBuilds[base].title_create + kTitleCallInCreate};
        unsigned char *image = with_stub_room(loaded[base], size, stub_bytes), *before = with_stub_room(loaded[base], size, stub_bytes);
        SkipIntroSites sites;
        uint64_t notice = 1;
        CHECK(skip_intro_locate(image, size, kSynthetic[k].game, &sites) == 0 && sites_are(&sites, expected.redirect, expected.title));
        CHECK(skip_intro_locate_notice(image, size, kSynthetic[k].game, &notice) == SKIP_INTRO_NOTICE_ABSENT && notice == 0);
        check_untouched(image, before, size, kSynthetic[k].game, kRealBuilds[base].entry);
        check_install(image, before, size, kSynthetic[k].game, kRealBuilds[base].entry, &expected, 0);
        free(image), free(before);

        /* No title flow, and two complete title flows (the whole code twice), fail closed. */
        unsigned char *none = with_stub_room(loaded[base], size, stub_bytes);
        none[kRealBuilds[base].opening + kRedirectInStep + 2] ^= 1;
        CHECK(skip_intro_locate(none, size, kSynthetic[k].game, &sites) == 0 && sites_are(&sites, 0, 0));
        CHECK(skip_intro_install(none, size, kRealBuilds[base].entry, kSynthetic[k].game, 1, none + size, stub_bytes) == 0);
        CHECK(skip_intro_claim() == 0);
        free(none);
        unsigned char *twice = calloc(1, 2 * size + stub_bytes);
        memcpy(twice, loaded[base], size);
        memcpy(twice + size, loaded[base], size);
        CHECK(skip_intro_locate(twice, 2 * size, kSynthetic[k].game, &sites) == 0 && sites_are(&sites, 0, 0));
        CHECK(skip_intro_install(twice, 2 * size, kRealBuilds[base].entry, kSynthetic[k].game, 1, twice + 2 * size, stub_bytes) == 0);
        CHECK(skip_intro_locate(twice, 2 * size, kRealBuilds[base].game, &sites) == 1 && sites_are(&sites, expected.redirect, expected.title));
        CHECK(skip_intro_claim() == 0);
        free(twice);
    }
    for (int i = 0; i < kRealCount; ++i) free(loaded[i]);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    set_env("DCP_SKIP_INTRO", NULL);
    printf("Skip Intro tests (startup logos, opening movie, first boot, title transition, all builds)\n");
    test_environment();
    test_claim();
    test_stub_execution();
    test_issue79_model();
    test_real_images();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("skip intro tests passed\n");
    return 0;
}
