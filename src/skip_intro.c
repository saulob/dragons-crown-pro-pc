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

typedef struct {
    const char *game;
    uint64_t window;      /* start of the code above */
    uint64_t player_call; /* what the first call must reach: the player index of the scene */
} BuildSite;

static const BuildSite kBuilds[] = {
    {"CUSA10454-1.00", 0x502076, 0x4ae190},
    {"CUSA07001-1.00", 0x4ffdc6, 0x4ac4a0},
    {"CUSA07001-1.02", 0x500356, 0x4ac920},
    {"CUSA07001-1.03", 0x500416, 0x4ac9e0},
    {"CUSA10209-1.00", 0x4ffdd6, 0x4ac610},
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
    skip_intro_arm();
    puts("Skip intro: on, native skip armed");
    return 1;
}
