#include "skip_intro.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ANY (-1)
#define HOOK_LENGTH 7
#define SKIP_INTRO_STUB_BYTES 96

/* The opening scene's frame: the poll `mov rdi,r12; mov esi,eax; call rbx`, its result test, and the call that ends the
 * scene when it is true. */
static const short kPoll[] = {
    0x49, 0x8b, 0x04, 0x24, 0x4c, 0x89, 0xe7, 0x48, 0x8b, 0x98, 0x38, 0x01, 0x00, 0x00, 0xe8, ANY, ANY, ANY, ANY,
    0x4c, 0x89, 0xe7, 0x89, 0xc6, 0xff, 0xd3, 0x84, 0xc0, 0x0f, 0x84, ANY, ANY, ANY, ANY, 0x49, 0x8b, 0x06,
    0x48, 0x8b, 0x0d, ANY, ANY, ANY, ANY, 0x48, 0x8b, 0x80, 0xa8, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x09, 0x48, 0x3b,
    0x4d, 0xd0, 0x0f, 0x85, ANY, ANY, ANY, ANY, 0xbe, 0x02, 0x00, 0x00, 0x00, 0x4c, 0x89, 0xf7, 0xe9, ANY, ANY,
    ANY, ANY,
};
enum {
    kPollCount = sizeof(kPoll) / sizeof(*kPoll),
    kCallInWindow = 14,
    kHookInWindow = 19,
    kBranchInWindow = 30,
    kEndJumpInWindow = kPollCount - 4
};

/* Where the window's branches must land: the scene's stack-check exit and its tail call that ends the scene. */
static const uint8_t kExit[] = {0x48, 0x8b, 0x05};
static const uint8_t kEnd[] = {0x48, 0x83, 0xc4, 0x68, 0x5b, 0x41, 0x5c, 0x41, 0x5d, 0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xff, 0xe0};

/* Step 5 of the boot object in the Japanese builds, from the first-frame test to the jump at its end: the first frame
 * builds the legal notice (message 459) and the object every build creates here, later frames wait for a press or the
 * timer and fade the notice out. The other builds go from the object straight to the next step. */
static const short kNotice[] = {
    0xbe, 0x01, 0x00, 0x00, 0x00, 0x4c, 0x89, 0xef, 0x41, 0xbf, 0x01, 0x00, 0x00, 0x00, 0xe8, ANY,
    ANY, ANY, ANY, 0x84, 0xc0, 0x0f, 0x84, ANY, ANY, ANY, ANY, 0xbe, 0x26, 0x00, 0x00, 0x00,
    0x4c, 0x89, 0xe7, 0xe8, ANY, ANY, ANY, ANY, 0xbf, 0x40, 0x02, 0x00, 0x00, 0x41, 0x89, 0xc4,
    0xe8, ANY, ANY, ANY, ANY, 0x48, 0x89, 0xc3, 0x48, 0x89, 0xdf, 0xe8, ANY, ANY, ANY, ANY,
    0xbe, 0xcb, 0x01, 0x00, 0x00, 0x48, 0x89, 0xdf, 0x49, 0x89, 0x9d, 0xd8, 0x00, 0x00, 0x00, 0xe8,
    ANY, ANY, ANY, ANY, 0x49, 0x8b, 0xbd, 0xd8, 0x00, 0x00, 0x00, 0xe8, ANY, ANY, ANY, ANY,
    0x49, 0x8b, 0x85, 0xd8, 0x00, 0x00, 0x00, 0x48, 0xb9, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x31, 0xff, 0x44, 0x89, 0xa0, 0xe4, 0x00, 0x00, 0x00, 0xc7, 0x80, 0x30, 0x01, 0x00, 0x00,
    0xcd, 0xcc, 0x4c, 0x3f, 0xc7, 0x80, 0x34, 0x01, 0x00, 0x00, 0xcd, 0xcc, 0x4c, 0x3f, 0xc7, 0x80,
    0x38, 0x01, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f, 0xc7, 0x80, 0x3c, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x3f, 0x48, 0x89, 0x88, 0xf4, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x0d, ANY, ANY, ANY, ANY,
    0x8b, 0x09, 0x89, 0x88, 0x10, 0x01, 0x00, 0x00, 0x4d, 0x8b, 0xa5, 0xd8, 0x00, 0x00, 0x00, 0xe8,
    ANY, ANY, ANY, ANY, 0x83, 0xf8, 0x02, 0x7c, 0x0a, 0x31, 0xff, 0xe8, ANY, ANY, ANY, ANY,
    0x41, 0x89, 0xc7, 0xb8, 0x1e, 0x00, 0x00, 0x00, 0x31, 0xd2, 0x48, 0x8d, 0x35, ANY, ANY, ANY,
    ANY, 0x4c, 0x89, 0xe7, 0x41, 0xf7, 0xff, 0xc5, 0xfa, 0x2a, 0xc0, 0xe8, ANY, ANY, ANY, ANY,
    0xbf, 0x10, 0x05, 0x00, 0x00, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x89, 0xc7, 0x4c, 0x89, 0xee,
    0xe8, ANY, ANY, ANY, ANY, 0x41, 0x8a, 0x45, 0x5d, 0x4c, 0x8b, 0x3d, ANY, ANY, ANY, ANY,
    0x3c, 0x01, 0x0f, 0x84, ANY, ANY, ANY, ANY, 0x84, 0xc0, 0x0f, 0x85, ANY, ANY, ANY, ANY,
    0x41, 0x8b, 0x5d, 0x64, 0x31, 0xff, 0xe8, ANY, ANY, ANY, ANY, 0xb9, 0x01, 0x00, 0x00, 0x00,
    0x83, 0xf8, 0x02, 0x7c, 0x09, 0x31, 0xff, 0xe8, ANY, ANY, ANY, ANY, 0x89, 0xc1, 0xb8, 0x3c,
    0x00, 0x00, 0x00, 0x31, 0xd2, 0xf7, 0xf9, 0xc5, 0xf8, 0x57, 0xc0, 0xc5, 0xfa, 0x2a, 0xc0, 0xc5,
    0xfa, 0x59, 0x05, ANY, ANY, ANY, ANY, 0xc5, 0xfa, 0x2c, 0xc0, 0x39, 0xd8, 0x0f, 0x92, 0xc0,
    0x41, 0x08, 0xc6, 0x41, 0x80, 0xfe, 0x01, 0x0f, 0x85, ANY, ANY, ANY, ANY, 0x4d, 0x8b, 0xb5,
    0xd8, 0x00, 0x00, 0x00, 0x31, 0xff, 0xe8, ANY, ANY, ANY, ANY, 0xb9, 0x01, 0x00, 0x00, 0x00,
    0x83, 0xf8, 0x02, 0x7c, 0x09, 0x31, 0xff, 0xe8, ANY, ANY, ANY, ANY, 0x89, 0xc1, 0xb8, 0x1e,
    0x00, 0x00, 0x00, 0x31, 0xd2, 0x48, 0x8d, 0x35, ANY, ANY, ANY, ANY, 0x4c, 0x89, 0xf7, 0xf7,
    0xf9, 0xc5, 0xf8, 0x57, 0xc0, 0xc5, 0xfa, 0x2a, 0xc0, 0xe8, ANY, ANY, ANY, ANY, 0x49, 0x8b,
    0x45, 0x00, 0x41, 0x8a, 0x4d, 0x5d, 0x48, 0x8b, 0x80, 0xa0, 0x00, 0x00, 0x00, 0x49, 0x8b, 0x17,
    0x48, 0x3b, 0x55, 0xd0, 0x0f, 0x85, ANY, ANY, ANY, ANY, 0xfe, 0xc1, 0xbe, 0x02, 0x00, 0x00,
    0x00, 0xe9, ANY, ANY, ANY, ANY,
};
/* The notice's construction, found loosely so a build that has it but not the exact step is refused. */
static const uint8_t kNoticeMarker[] = {0xbe, 0xcb, 0x01, 0x00, 0x00, 0x48, 0x89, 0xdf, 0x49, 0x89, 0x9d, 0xd8, 0x00, 0x00, 0x00};
/* mov ecx,[rdi+0x50]; mov eax,ecx; and eax,0x1000: the step machine's first-frame test */
static const uint8_t kFirstFrameTest[] = {0x8b, 0x4f, 0x50, 0x89, 0xc8, 0x25, 0x00, 0x10, 0x00, 0x00};
/* How the step ends once the notice is done: advance the step byte, then SetStep through the shared tail. */
static const short kAdvance[] = {
    0x49, 0x8b, 0x45, 0x00, 0x41, 0x8a, 0x4d, 0x5c, 0x48, 0x8b, 0x80, 0xa0, 0x00, 0x00, 0x00, 0x49, 0x8b, 0x17,
    0x48, 0x3b, 0x55, 0xd0, 0x0f, 0x85, ANY, ANY, ANY, ANY, 0xfe, 0xc1, 0xbe, 0x01, 0x00, 0x00, 0x00,
};
static const uint8_t kSetStepTail[] = {0x0f, 0xb6, 0xd1, 0x4c, 0x89, 0xef, 0x48, 0x83, 0xc4, 0x68};
enum {
    kNoticeCount = sizeof(kNotice) / sizeof(*kNotice),
    kAdvanceCount = sizeof(kAdvance) / sizeof(*kAdvance),
    kFirstFrameCall = 0x0e,
    kFirstFrameBranch = 0x15,
    kNoticeBegins = 0x1b,
    kMarkerInWindow = 0x40,
    kObject = 0xf0,
    kObjectEnds = 0x105,
    kStackGuard = 0x109,
    kStepJump = 0x1d1,
    kBypassBytes = SKIP_INTRO_BYPASS_BYTES
};

typedef struct {
    const char *game;
    uint64_t window;      /* start of the code above */
    uint64_t player_call; /* what the first call must reach: the player index of the scene */
    uint64_t notice;      /* start of kNotice; 0 when the build never shows the notice */
} BuildSite;

static const BuildSite kBuilds[] = {
    {"CUSA10454-1.00", 0x502076, 0x4ae190, 0},
    {"CUSA07001-1.00", 0x4ffdc6, 0x4ac4a0, 0x174b0},
    {"CUSA07001-1.02", 0x500356, 0x4ac920, 0x174d0},
    {"CUSA07001-1.03", 0x500416, 0x4ac9e0, 0x174d0},
    {"CUSA10209-1.00", 0x4ffdd6, 0x4ac610, 0},
};

static const char kOpeningLayout[] = "STAGE/OPENING01.MBS";
static const uint8_t kStateDispatch[2][5] = {{0x80, 0xfb, 0x02, 0x0f, 0x84}, {0x80, 0xfb, 0x01, 0x0f, 0x84}};
static const uint8_t kLoadAddress[] = {0x48, 0x8d, 0x35};
enum { kContextReach = 0x300 };

static int enabled_by_name(const char *value, int fallback) {
    if (!value || !*value) return fallback;
    if (!strcmp(value, "1") || !strcmp(value, "on") || !strcmp(value, "true")) return 1;
    if (!strcmp(value, "0") || !strcmp(value, "off") || !strcmp(value, "false")) return 0;
    return fallback;
}

int skip_intro_wanted(int saved) {
    return enabled_by_name(getenv("DCP_SKIP_INTRO"), saved != 0);
}

static int32_t load_rel32(const unsigned char *at) {
    int32_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

static int bytes_at(const unsigned char *image, uint64_t size, int64_t at, const uint8_t *bytes, size_t count) {
    return at >= 0 && (uint64_t)at <= size && size - (uint64_t)at >= count && !memcmp(image + at, bytes, count);
}

/* The scene's layout path is loaded and its state dispatch is switched on shortly before the poll. */
static int opening_context(const unsigned char *image, uint64_t size, uint64_t window) {
    if (window < kContextReach) return 0;
    int layout = 0, dispatch[2] = {0, 0};
    for (uint64_t at = window - kContextReach; at < window; ++at) {
        if (!memcmp(image + at, kLoadAddress, sizeof(kLoadAddress))) {
            const int64_t target = (int64_t)at + 7 + load_rel32(image + at + 3);
            layout |= bytes_at(image, size, target, (const uint8_t *)kOpeningLayout, sizeof(kOpeningLayout));
        }
        for (int d = 0; d < 2; ++d) dispatch[d] |= !memcmp(image + at, kStateDispatch[d], sizeof(kStateDispatch[d]));
    }
    return layout && dispatch[0] && dispatch[1];
}

/* `player_call` 0: any call target, for a build searched without a table. */
static int window_matches(const unsigned char *image, uint64_t size, uint64_t at, uint64_t player_call) {
    if (at > size || size - at < kPollCount) return 0;
    for (size_t i = 0; i < kPollCount; ++i)
        if (kPoll[i] != ANY && image[at + i] != (unsigned char)kPoll[i]) return 0;
    const int64_t player = (int64_t)at + kCallInWindow + 5 + load_rel32(image + at + kCallInWindow + 1);
    const int64_t exit = (int64_t)at + kBranchInWindow + 4 + load_rel32(image + at + kBranchInWindow);
    const int64_t end = (int64_t)at + kPollCount + load_rel32(image + at + kEndJumpInWindow);
    return (!player_call || player == (int64_t)player_call) && bytes_at(image, size, exit, kExit, sizeof(kExit)) &&
           bytes_at(image, size, end, kEnd, sizeof(kEnd)) && opening_context(image, size, at);
}

/* The one place in the image the poll appears; 0 when it appears nowhere or more than once. */
static uint64_t find_window(const unsigned char *image, uint64_t size) {
    uint64_t found = 0;
    int matches = 0;
    for (uint64_t at = 0; at + kPollCount <= size; ++at) {
        if (image[at] != kPoll[0] || !window_matches(image, size, at, 0)) continue;
        if (matches++) return 0;
        found = at;
    }
    return found;
}

int skip_intro_locate(const unsigned char *image, uint64_t size, const char *game, uint64_t *site) {
    uint64_t window = 0;
    int listed = 0;
    for (size_t i = 0; game && i < sizeof(kBuilds) / sizeof(*kBuilds); ++i) {
        if (strcmp(game, kBuilds[i].game)) continue;
        listed = 1;
        window = window_matches(image, size, kBuilds[i].window, kBuilds[i].player_call) ? kBuilds[i].window : 0;
    }
    if (!listed && game) window = find_window(image, size);
    *site = window ? window + kHookInWindow : 0;
    return listed;
}

static int pattern_at(const short *pattern, size_t count, const unsigned char *image, uint64_t size, int64_t at) {
    if (at < 0 || (uint64_t)at > size || size - (uint64_t)at < count) return 0;
    for (size_t i = 0; i < count; ++i)
        if (pattern[i] != ANY && image[at + i] != (unsigned char)pattern[i]) return 0;
    return 1;
}

static int64_t rel32_target(const unsigned char *image, int64_t next, int64_t field) {
    return next + load_rel32(image + field);
}

/* The notice step at `at`: its first-frame test, its guard, and the jump at its end that reaches the shared SetStep
 * tail, whose advance sequence is the one the notice's completion takes. */
static int notice_matches(const unsigned char *image, uint64_t size, uint64_t at) {
    if (!pattern_at(kNotice, kNoticeCount, image, size, (int64_t)at)) return 0;
    const int64_t base = (int64_t)at;
    const int64_t query = rel32_target(image, base + kFirstFrameCall + 5, base + kFirstFrameCall + 1);
    const int64_t guard = rel32_target(image, base + kFirstFrameBranch + 6, base + kFirstFrameBranch + 2);
    const int64_t tail = rel32_target(image, base + kStepJump + 5, base + kStepJump + 1);
    return bytes_at(image, size, query, kFirstFrameTest, sizeof(kFirstFrameTest)) && guard == base + kObjectEnds &&
           pattern_at(kAdvance, kAdvanceCount, image, size, tail - (int64_t)kAdvanceCount) &&
           bytes_at(image, size, tail, kSetStepTail, sizeof(kSetStepTail));
}

static int notice_marks(const unsigned char *image, uint64_t at, uint64_t size) {
    return at + sizeof(kNoticeMarker) <= size && !memcmp(image + at, kNoticeMarker, sizeof(kNoticeMarker));
}

/* Every place the build constructs the notice, and the one exact step among them when there is just one. */
static int find_notice(const unsigned char *image, uint64_t size, uint64_t *window) {
    int marks = 0;
    uint64_t found = 0;
    for (uint64_t at = 0; at + sizeof(kNoticeMarker) <= size; ++at) {
        if (image[at] != kNoticeMarker[0] || !notice_marks(image, at, size)) continue;
        ++marks;
        found = at >= kMarkerInWindow && notice_matches(image, size, at - kMarkerInWindow) ? at - kMarkerInWindow : 0;
    }
    *window = found;
    return marks;
}

SkipIntroNotice skip_intro_locate_notice(const unsigned char *image, uint64_t size, const char *game, uint64_t *site) {
    *site = 0;
    uint64_t window = 0;
    for (size_t i = 0; game && i < sizeof(kBuilds) / sizeof(*kBuilds); ++i) {
        if (strcmp(game, kBuilds[i].game) || !kBuilds[i].notice) continue;
        if (!notice_matches(image, size, kBuilds[i].notice)) return SKIP_INTRO_NOTICE_UNMATCHED;
        *site = kBuilds[i].notice + kNoticeBegins;
        return SKIP_INTRO_NOTICE_FOUND;
    }
    const int marks = find_notice(image, size, &window);
    if (!marks) return SKIP_INTRO_NOTICE_ABSENT;
    if (marks > 1 || !window) return SKIP_INTRO_NOTICE_UNMATCHED;
    for (size_t i = 0; game && i < sizeof(kBuilds) / sizeof(*kBuilds); ++i)
        if (!strcmp(game, kBuilds[i].game)) return SKIP_INTRO_NOTICE_UNMATCHED;
    *site = window + kNoticeBegins;
    return SKIP_INTRO_NOTICE_FOUND;
}

/* What the other builds do in this step: the object, then the next step. The notice window's construction is dead
 * code from here, so the same bytes carry it. The stack guard pointer is reloaded because the notice step reuses r15. */
static void build_notice_bypass(unsigned char *out, const unsigned char *image, uint64_t window) {
    const int64_t base = (int64_t)window, cave = base + kNoticeBegins;
    const int64_t object_new = rel32_target(image, base + kObject + 10, base + kObject + 6);
    const int64_t object_init = rel32_target(image, base + kObjectEnds, base + kObjectEnds - 4);
    const int64_t guard = rel32_target(image, base + kStackGuard + 7, base + kStackGuard + 3);
    const int64_t jump = base + kStepJump;
    const int64_t tail = rel32_target(image, jump + 5, jump + 1);
    const int32_t calls[3] = {(int32_t)(object_new - (cave + 10)), (int32_t)(object_init - (cave + 21)),
                              (int32_t)(guard - (cave + 28))};
    const int32_t advance = (int32_t)(tail - (int64_t)kAdvanceCount - (cave + kBypassBytes));
    memcpy(out, image + base + kObject, 5);
    out[5] = 0xe8;
    memcpy(out + 6, &calls[0], 4);
    memcpy(out + 10, image + base + kObject + 10, 6);
    out[16] = 0xe8;
    memcpy(out + 17, &calls[1], 4);
    memcpy(out + 21, image + base + kStackGuard, 3);
    memcpy(out + 24, &calls[2], 4);
    out[28] = 0xe9;
    memcpy(out + 29, &advance, 4);
}

static atomic_int g_armed;

void skip_intro_arm(void) {
    atomic_store(&g_armed, 1);
}

int GUEST_HOOK_ABI skip_intro_claim(void) {
    return atomic_exchange(&g_armed, 0);
}

void GUEST_HOOK_ABI skip_intro_delivered(void) {
    puts("Skip intro: startup sequence skipped");
}

/* Runs where `mov rdi,r12; mov esi,eax; call rbx` was, entered by a jump. A claimed press returns 1 to the code after
 * it without making the call; otherwise the call is made with the original rbx, r12 and eax. */
size_t skip_intro_build_stub(unsigned char *out, uint64_t claim, uint64_t delivered, uint64_t resume) {
    static const unsigned char kSave[] = {0x50, 0x50, 0x48, 0xb8};
    static const unsigned char kTest[] = {0xff, 0xd0, 0x85, 0xc0, 0x74};
    static const unsigned char kPressed[] = {0x48, 0xb8};
    static const unsigned char kReturnPressed[] = {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x10, 0xb8, 0x01, 0x00, 0x00, 0x00};
    static const unsigned char kJump[] = {0xff, 0x25, 0x00, 0x00, 0x00, 0x00};
    static const unsigned char kOriginal[] = {0x48, 0x83, 0xc4, 0x08, 0x58, 0x4c, 0x89, 0xe7, 0x89, 0xc6, 0xff, 0xd3};
    unsigned char *at = out;
    memcpy(at, kSave, sizeof(kSave)), at += sizeof(kSave);
    memcpy(at, &claim, 8), at += 8;
    memcpy(at, kTest, sizeof(kTest)), at += sizeof(kTest);
    unsigned char *skip = at++;
    memcpy(at, kPressed, sizeof(kPressed)), at += sizeof(kPressed);
    memcpy(at, &delivered, 8), at += 8;
    memcpy(at, kReturnPressed, sizeof(kReturnPressed)), at += sizeof(kReturnPressed);
    memcpy(at, kJump, sizeof(kJump)), at += sizeof(kJump);
    memcpy(at, &resume, 8), at += 8;
    *skip = (unsigned char)(at - skip - 1);
    memcpy(at, kOriginal, sizeof(kOriginal)), at += sizeof(kOriginal);
    memcpy(at, kJump, sizeof(kJump)), at += sizeof(kJump);
    memcpy(at, &resume, 8), at += 8;
    return (size_t)(at - out);
}

size_t skip_intro_stub_bytes(void) {
    return SKIP_INTRO_STUB_BYTES;
}

static int unavailable(const char *game, const char *reason) {
    printf("Skip intro: requested but unavailable for %s: %s\n", game ? game : "an unknown game build", reason);
    return 0;
}

int skip_intro_install(unsigned char *image, uint64_t size, uint64_t entry, const char *game, int wanted,
                       unsigned char *stubs, size_t stubs_size) {
    (void)entry;
    if (!wanted) {
        puts("Skip intro: off");
        return 0;
    }
    if (!game) return unavailable(game, "not a supported build");
    uint64_t site = 0;
    const int listed = skip_intro_locate(image, size, game, &site);
    if (!site) return unavailable(game, listed ? "the verified site does not match" : "opening input poll not found exactly once");
    uint64_t notice = 0;
    if (skip_intro_locate_notice(image, size, game, &notice) == SKIP_INTRO_NOTICE_UNMATCHED)
        return unavailable(game, "the regional startup notice does not match");
    if (stubs_size < SKIP_INTRO_STUB_BYTES) return unavailable(game, "no room for the stub");
    const int64_t distance = (int64_t)(intptr_t)stubs - (int64_t)(intptr_t)(image + site + 5);
    if (distance > INT32_MAX || distance < INT32_MIN) return unavailable(game, "stub too far from the code");
    unsigned char stub[SKIP_INTRO_STUB_BYTES] = {0};
    const size_t length = skip_intro_build_stub(stub, (uint64_t)(uintptr_t)skip_intro_claim,
                                                (uint64_t)(uintptr_t)skip_intro_delivered,
                                                (uint64_t)(uintptr_t)(image + site + HOOK_LENGTH));
    if (length > SKIP_INTRO_STUB_BYTES) return unavailable(game, "stub too large");
    memcpy(stubs, stub, SKIP_INTRO_STUB_BYTES);
    const int32_t rel = (int32_t)distance;
    image[site] = 0xe9;
    memcpy(image + site + 1, &rel, sizeof(rel));
    memset(image + site + 5, 0x90, HOOK_LENGTH - 5);
    if (notice) {
        unsigned char bypass[kBypassBytes];
        build_notice_bypass(bypass, image, notice - kNoticeBegins);
        memcpy(image + notice, bypass, sizeof(bypass));
        printf("Skip intro: regional notice bypass installed (%s)\n", game);
    }
    skip_intro_arm();
    puts("Skip intro: on, native skip armed");
    return 1;
}
