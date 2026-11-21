#include "skip_intro.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ANY (-1)
#define SKIP_INTRO_STUB_BYTES 96
#define SKIP_INTRO_TITLE_STUB_AT 56

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


/* The title flow's step switch: the step byte, then a seven-entry jump table. */
static const short kDispatch[] = {
    0x41, 0x0f, 0xb6, 0x47, 0x5c, 0x48, 0x83, 0xf8, 0x06, 0x0f, 0x87, ANY, ANY, ANY, ANY, 0x48,
    0x8d, 0x0d, ANY, ANY, ANY, ANY, 0x48, 0x63, 0x04, 0x81, 0x48, 0x01, 0xc8, 0xff, 0xe0,
};
/* Step 2: the "back to the title" flag goes to the title step, its absence to the save data step. */
static const short kDecision[] = {
    0x48, 0x8d, 0x05, ANY, ANY, ANY, ANY, 0x49, 0x8b, 0x0f, 0x8b, 0x40, 0x04, 0x41, 0x85, 0x87, 0xec,
    0x00, 0x00, 0x00, 0x48, 0x8b, 0x81, 0xa0, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x0d, ANY, ANY, ANY, ANY,
    0x0f, 0x84, ANY, ANY, ANY, ANY, 0x48, 0x8b, 0x09, 0x48, 0x3b, 0x4d, 0xd0, 0x0f, 0x85, ANY, ANY, ANY,
    ANY, 0xbe, 0x01, 0x00, 0x00, 0x00, 0xba, 0x05, 0x00, 0x00, 0x00, 0xe9, ANY, ANY, ANY, ANY,
};
static const short kDecisionSave[] = {
    0x48, 0x8b, 0x09, 0x48, 0x3b, 0x4d, 0xd0, 0x75, ANY, 0xbe, 0x01, 0x00, 0x00, 0x00, 0xba, 0x03, 0x00, 0x00, 0x00,
};
/* Step 3: the save data scene, created on the first frame and waited for. */
static const short kSaveStep[] = {
    0x4c, 0x89, 0xf7, 0xe8, ANY, ANY, ANY, ANY, 0xbe, 0x01, 0x00, 0x00, 0x00, 0x4c, 0x89, 0xff, 0x48, 0x89, 0xc3,
    0xe8, ANY, ANY, ANY, ANY, 0x84, 0xc0, 0x74, 0x0b, 0x4c, 0x89, 0xf7, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x89,
    0xc3, 0x48, 0x85, 0xdb, 0x0f, 0x84, ANY, ANY, ANY, ANY, 0x48, 0x89, 0xdf, 0xe8, ANY, ANY, ANY, ANY, 0xeb, 0x4d,
};
/* Step 4: the opening scene. When the game says the opening is not needed (`call [rax+0x260]`, true), the step goes
 * to the shared end of step without creating the scene. Step 5, the title, follows it directly. */
static const short kOpeningStep[] = {
    0x4c, 0x89, 0xf7, 0xe8, ANY, ANY, ANY, ANY, 0x49, 0x8b, 0xbc, 0x24, 0x10, 0x69, 0x01, 0x00, 0x48, 0x89, 0xc3,
    0x48, 0x8b, 0x07, 0xff, 0x90, 0x60, 0x02, 0x00, 0x00, 0x84, 0xc0, 0x75, 0x6b, 0xbe, 0x01, 0x00, 0x00, 0x00,
    0x4c, 0x89, 0xff, 0xe8, ANY, ANY, ANY, ANY, 0x84, 0xc0, 0x74, 0x0b, 0x4c, 0x89, 0xf7, 0xe8, ANY, ANY, ANY,
    ANY, 0x48, 0x89, 0xc3, 0x48, 0x85, 0xdb, 0x0f, 0x84, ANY, ANY, ANY, ANY, 0x48, 0x89, 0xdf, 0xe8, ANY, ANY,
    ANY, ANY, 0x84, 0xc0, 0x75, 0x3f, 0xe9, ANY, ANY, ANY, ANY,
};
static const short kTitleStep[] = {
    0x4c, 0x89, 0xf7, 0xe8, ANY, ANY, ANY, ANY, 0x49, 0x8b, 0xbd, 0x90, 0x00, 0x00, 0x00, 0x31, 0xf6, 0x48,
    0x89, 0xc3, 0xe8, ANY, ANY, ANY, ANY, 0x49, 0x8b, 0xbc, 0x24, 0x10, 0x69, 0x01, 0x00, 0x49, 0x89, 0xc5,
    0x48, 0x8b, 0x07, 0xff, 0x90, 0x60, 0x02, 0x00, 0x00, 0x84, 0xc0, 0x0f, 0x84, ANY, ANY, ANY, ANY,
};
/* The end of step both reach: finish the scene when there is one, then SetStep(1, step + 1). */
static const short kStepDone[] = {
    0x48, 0x85, 0xdb, 0x74, 0x0c, 0x48, 0x83, 0xc3, 0x50, 0x48, 0x89, 0xdf, 0xe8, ANY, ANY, ANY, ANY, 0x49,
    0x8b, 0x07, 0x48, 0x8b, 0x15, ANY, ANY, ANY, ANY, 0x41, 0x8a, 0x4f, 0x5c, 0x48, 0x8b, 0x80, 0xa0, 0x00,
    0x00, 0x00, 0x48, 0x8b, 0x12, 0x48, 0x3b, 0x55, 0xd0, 0x0f, 0x85, ANY, ANY, ANY, ANY, 0xfe, 0xc1, 0xbe,
    0x01, 0x00, 0x00, 0x00, 0x0f, 0xb6, 0xd1, 0xe9, ANY, ANY, ANY, ANY,
};
/* The title step's first frame: clear the flag, create the title scene. */
static const short kTitleCreate[] = {
    0xbe, 0x01, 0x00, 0x00, 0x00, 0x4c, 0x89, 0xff, 0xe8, ANY, ANY, ANY, ANY, 0x84, 0xc0, 0x74, 0x25, 0x48,
    0x8d, 0x05, ANY, ANY, ANY, ANY, 0x4c, 0x89, 0xf7, 0x8b, 0x40, 0x04, 0xf7, 0xd0, 0x41, 0x21, 0x87, 0xec,
    0x00, 0x00, 0x00, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x89, 0xc3, 0x4c, 0x89, 0xab, 0xa8, 0x06, 0x00, 0x00,
};
/* The scene factory: id range check, then its case table. */
static const short kFactoryHead[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x41, 0x89,
    0xf6, 0x49, 0x89, 0xff, 0x41, 0x8d, 0x5e, 0xff, 0x83, 0xfb, 0x42,
};
static const short kFactoryDispatch[] = {
    0x31, 0xc0, 0x83, 0xfb, 0x42, 0x0f, 0x87, ANY, ANY, ANY, ANY, 0x48, 0x8d, 0x05,
    ANY, ANY, ANY, ANY, 0x48, 0x63, 0x0c, 0x98, 0x48, 0x01, 0xc1, 0xff, 0xe1,
};
static const short kFactoryCase[] = {
    0xbf, ANY, ANY, ANY, ANY, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x89, 0xc3, 0x4c, 0x89, 0xfe, 0x48, 0x89, 0xdf, 0xe8, ANY, ANY, ANY, ANY,
};
enum {
    kDispatchCount = sizeof(kDispatch) / sizeof(*kDispatch),
    kDecisionCount = sizeof(kDecision) / sizeof(*kDecision),
    kDecisionSaveCount = sizeof(kDecisionSave) / sizeof(*kDecisionSave),
    kSaveStepCount = sizeof(kSaveStep) / sizeof(*kSaveStep),
    kOpeningStepCount = sizeof(kOpeningStep) / sizeof(*kOpeningStep),
    kTitleStepCount = sizeof(kTitleStep) / sizeof(*kTitleStep),
    kStepDoneCount = sizeof(kStepDone) / sizeof(*kStepDone),
    kTitleCreateCount = sizeof(kTitleCreate) / sizeof(*kTitleCreate),
    kFactoryHeadCount = sizeof(kFactoryHead) / sizeof(*kFactoryHead),
    kFactoryDispatchCount = sizeof(kFactoryDispatch) / sizeof(*kFactoryDispatch),
    kFactoryCaseCount = sizeof(kFactoryCase) / sizeof(*kFactoryCase),
    kDispatchReach = 0x400,
    kDispatchDefault = 0x09,
    kDispatchTable = 0x0f,
    kStepEntries = 7,
    kSaveStepIndex = 3,
    kOpeningStepIndex = 4,
    kTitleStepIndex = 5,
    kDecisionIndex = 2,
    kDecisionBranch = 0x22,
    kGetterCall = 0x03,
    kSaveFirstFrame = 0x13,
    kSaveCreate = 0x1f,
    kSaveExit = 0x2a,
    kRedirectInStep = 0x16,
    kOpeningFirstFrame = 0x28,
    kOpeningCreate = 0x34,
    kOpeningExit = 0x3f,
    kOpeningExitJump = 0x51,
    kTitleCreateBranch = 0x2f,
    kTitleFirstFrame = 0x08,
    kTitleCall = 0x27,
    kFactoryCaseTable = 0x60,
    kCtorCall = 0x13,
    kCtorReach = 0x20,
    kSceneSave = 1,
    kSceneOpening = 2,
    kSceneTitle = 7
};

typedef struct {
    const char *game;
    uint64_t opening_step; /* step 4 of the title flow */
    uint64_t notice;       /* start of kNotice; 0 when the build never shows the notice */
} BuildSite;

static const BuildSite kBuilds[] = {
    {"CUSA10454-1.00", 0x428b3f, 0},
    {"CUSA10454-1.01", 0x428b3f, 0},
    {"CUSA07001-1.00", 0x427d9f, 0x174b0},
    {"CUSA07001-1.02", 0x4281bf, 0x174d0},
    {"CUSA07001-1.03", 0x42820f, 0x174d0},
    {"CUSA10209-1.00", 0x427f0f, 0},
};

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

static int in_image(uint64_t size, int64_t at, uint64_t count) {
    return at >= 0 && (uint64_t)at <= size && size - (uint64_t)at >= count;
}

/* Where the call or jump (e8/e9 rel32) at `at` goes. */
static int64_t call_target(const unsigned char *image, int64_t at) {
    return rel32_target(image, at + 5, at + 1);
}

/* Where the conditional jump (0f 8x rel32) at `at` goes. */
static int64_t branch_target(const unsigned char *image, int64_t at) {
    return rel32_target(image, at + 6, at + 2);
}

/* `mov rax,[rdi+slot]; ret`: the scene manager's pointer to the scene. */
static int scene_getter(const unsigned char *image, uint64_t size, int64_t at, unsigned scene) {
    uint8_t getter[8] = {0x48, 0x8b, 0x87, 0, 0, 0, 0, 0xc3};
    const uint32_t slot = 0x78 + 0x20 * scene;
    memcpy(getter + 3, &slot, sizeof(slot));
    return bytes_at(image, size, at, getter, sizeof(getter));
}

/* `mov esi,scene; jmp factory`: the factory, or -1. */
static int64_t scene_factory(const unsigned char *image, uint64_t size, int64_t at, unsigned scene) {
    const uint8_t thunk[6] = {0xbe, (uint8_t)scene, 0x00, 0x00, 0x00, 0xe9};
    if (!bytes_at(image, size, at, thunk, sizeof(thunk)) || !in_image(size, at + 6, 4)) return -1;
    return call_target(image, at + 5);
}

/* The base constructor the factory's case for `scene` reaches with that scene id, or -1. */
static int64_t scene_base_constructor(const unsigned char *image, uint64_t size, int64_t factory, unsigned scene) {
    const int64_t dispatch = factory + kFactoryCaseTable;
    const int64_t table = rel32_target(image, dispatch + 0x12, dispatch + 0x0e);
    if (!in_image(size, table, 4u * scene)) return -1;
    const int64_t entry = table + load_rel32(image + table + 4 * (scene - 1));
    if (!pattern_at(kFactoryCase, kFactoryCaseCount, image, size, entry)) return -1;
    const int64_t ctor = call_target(image, entry + kCtorCall);
    const uint8_t pass_id[9] = {0xba, (uint8_t)scene, 0x00, 0x00, 0x00, 0x48, 0x89, 0xfb, 0xe8};
    if (!in_image(size, ctor, kCtorReach + sizeof(pass_id) + 4) || image[ctor] != 0x55) return -1;
    for (int64_t at = ctor; at < ctor + kCtorReach; ++at)
        if (!memcmp(image + at, pass_id, sizeof(pass_id))) return call_target(image, at + 8);
    return -1;
}

/* The save data, opening and title scenes are the factory's scenes 1, 2 and 7, built on the same base. */
static int scenes_match(const unsigned char *image, uint64_t size, int64_t factory) {
    if (!pattern_at(kFactoryHead, kFactoryHeadCount, image, size, factory) ||
        !pattern_at(kFactoryDispatch, kFactoryDispatchCount, image, size, factory + kFactoryCaseTable))
        return 0;
    const int64_t base = scene_base_constructor(image, size, factory, kSceneSave);
    return base >= 0 && scene_base_constructor(image, size, factory, kSceneOpening) == base &&
           scene_base_constructor(image, size, factory, kSceneTitle) == base;
}

/* The switch above the opening step: its default exits the step, entry 3 is the save data step, entry 4 the opening,
 * entry 5 the title, and entry 2 chooses the title when returning to it and the save data step otherwise. */
static int steps_match(const unsigned char *image, uint64_t size, int64_t opening, int64_t exit, int64_t first_frame,
                       int64_t factory) {
    int64_t dispatch = -1;
    int found = 0;
    for (int64_t at = opening > kDispatchReach ? opening - kDispatchReach : 0; at < opening; ++at)
        if (pattern_at(kDispatch, kDispatchCount, image, size, at)) ++found, dispatch = at;
    if (found != 1 || branch_target(image, dispatch + kDispatchDefault) != exit) return 0;
    const int64_t table = rel32_target(image, dispatch + kDispatchTable + 7, dispatch + kDispatchTable + 3);
    if (!in_image(size, table, 4 * kStepEntries)) return 0;
    int64_t entry[kStepEntries];
    for (int i = 0; i < kStepEntries; ++i) entry[i] = table + load_rel32(image + table + 4 * i);
    if (entry[kOpeningStepIndex] != opening || entry[kTitleStepIndex] != opening + kOpeningStepCount) return 0;

    const int64_t save = entry[kSaveStepIndex], decision = entry[kDecisionIndex];
    return pattern_at(kSaveStep, kSaveStepCount, image, size, save) &&
           scene_getter(image, size, call_target(image, save + kGetterCall), kSceneSave) &&
           call_target(image, save + kSaveFirstFrame) == first_frame &&
           scene_factory(image, size, call_target(image, save + kSaveCreate), kSceneSave) == factory &&
           branch_target(image, save + kSaveExit) == exit && save + kSaveStepCount == opening &&
           pattern_at(kDecision, kDecisionCount, image, size, decision) &&
           pattern_at(kDecisionSave, kDecisionSaveCount, image, size, branch_target(image, decision + kDecisionBranch));
}

static int title_flow_matches(const unsigned char *image, uint64_t size, uint64_t step, SkipIntroSites *sites) {
    const int64_t opening = (int64_t)step, title = opening + kOpeningStepCount, done = title + kTitleStepCount;
    if (!pattern_at(kOpeningStep, kOpeningStepCount, image, size, opening) ||
        !pattern_at(kTitleStep, kTitleStepCount, image, size, title) || !pattern_at(kStepDone, kStepDoneCount, image, size, done))
        return 0;
    const int64_t exit = branch_target(image, opening + kOpeningExit);
    const int64_t create = branch_target(image, title + kTitleCreateBranch);
    const int64_t first_frame = call_target(image, opening + kOpeningFirstFrame);
    if (call_target(image, opening + kOpeningExitJump) != exit ||
        !scene_getter(image, size, call_target(image, opening + kGetterCall), kSceneOpening) ||
        !scene_getter(image, size, call_target(image, title + kGetterCall), kSceneTitle) ||
        !pattern_at(kTitleCreate, kTitleCreateCount, image, size, create) ||
        call_target(image, create + kTitleFirstFrame) != first_frame)
        return 0;
    const int64_t factory = scene_factory(image, size, call_target(image, opening + kOpeningCreate), kSceneOpening);
    if (factory < 0 || scene_factory(image, size, call_target(image, create + kTitleCall), kSceneTitle) != factory ||
        !steps_match(image, size, opening, exit, first_frame, factory) || !scenes_match(image, size, factory))
        return 0;
    sites->redirect = step + kRedirectInStep;
    sites->title = (uint64_t)create + kTitleCall;
    return 1;
}

/* The one title flow in the image; none when there is none or more than one. */
static void find_title_flow(const unsigned char *image, uint64_t size, SkipIntroSites *sites) {
    SkipIntroSites found = {0, 0}, one;
    int matches = 0;
    for (uint64_t at = 0; at + kOpeningStepCount <= size; ++at) {
        if (image[at] != 0x4c || !title_flow_matches(image, size, at, &one)) continue;
        if (matches++) {
            found.redirect = found.title = 0;
            break;
        }
        found = one;
    }
    *sites = found;
}

int skip_intro_locate(const unsigned char *image, uint64_t size, const char *game, SkipIntroSites *sites) {
    const SkipIntroSites none = {0, 0};
    int listed = 0;
    *sites = none;
    for (size_t i = 0; game && i < sizeof(kBuilds) / sizeof(*kBuilds); ++i) {
        if (strcmp(game, kBuilds[i].game)) continue;
        listed = 1;
        if (!title_flow_matches(image, size, kBuilds[i].opening_step, sites)) *sites = none;
    }
    if (!listed && game) find_title_flow(image, size, sites);
    return listed;
}

static atomic_int g_armed, g_redirects, g_title_reported;

void skip_intro_arm(void) {
    atomic_store(&g_armed, 1);
}

int GUEST_HOOK_ABI skip_intro_claim(void) {
    return atomic_exchange(&g_armed, 0);
}

void GUEST_HOOK_ABI skip_intro_redirected(void) {
    const int count = atomic_fetch_add(&g_redirects, 1) + 1;
    printf("Skip Intro telemetry: event=startup_redirect from=opening_logos to=title count=%d\n", count);
}

void GUEST_HOOK_ABI skip_intro_title_reached(void) {
    if (!atomic_load(&g_redirects) || atomic_exchange(&g_title_reported, 1)) return;
    puts("Skip Intro telemetry: event=title_reached count=1");
}

static unsigned char *put(unsigned char *at, const unsigned char *bytes, size_t count) {
    memcpy(at, bytes, count);
    return at + count;
}

/* Called in place of `call [rax+0x260]` with rdi the object asked. */
size_t skip_intro_build_redirect_stub(unsigned char *out, uint64_t claim, uint64_t redirected) {
    static const unsigned char kSaveAndLoad[] = {0x57, 0x48, 0xb8};
    static const unsigned char kAsk[] = {0xff, 0xd0, 0x85, 0xc0, 0x74};
    static const unsigned char kLoad[] = {0x48, 0xb8};
    static const unsigned char kNotNeeded[] = {0xff, 0xd0, 0x5f, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3};
    static const unsigned char kOriginal[] = {0x5f, 0x48, 0x8b, 0x07, 0xff, 0xa0, 0x60, 0x02, 0x00, 0x00};
    unsigned char *at = put(out, kSaveAndLoad, sizeof(kSaveAndLoad));
    at = put(at, (const unsigned char *)&claim, 8);
    at = put(at, kAsk, sizeof(kAsk));
    unsigned char *skip = at++;
    at = put(at, kLoad, sizeof(kLoad));
    at = put(at, (const unsigned char *)&redirected, 8);
    at = put(at, kNotNeeded, sizeof(kNotNeeded));
    *skip = (unsigned char)(at - skip - 1);
    at = put(at, kOriginal, sizeof(kOriginal));
    return (size_t)(at - out);
}

/* Called in place of the title scene's creation with rdi the scene manager. */
size_t skip_intro_build_title_stub(unsigned char *out, uint64_t reached, uint64_t create) {
    static const unsigned char kSaveAndLoad[] = {0x57, 0x48, 0xb8};
    static const unsigned char kCallAndLoad[] = {0xff, 0xd0, 0x5f, 0x48, 0xb8};
    static const unsigned char kJump[] = {0xff, 0xe0};
    unsigned char *at = put(out, kSaveAndLoad, sizeof(kSaveAndLoad));
    at = put(at, (const unsigned char *)&reached, 8);
    at = put(at, kCallAndLoad, sizeof(kCallAndLoad));
    at = put(at, (const unsigned char *)&create, 8);
    at = put(at, kJump, sizeof(kJump));
    return (size_t)(at - out);
}

size_t skip_intro_stub_bytes(void) {
    return SKIP_INTRO_STUB_BYTES;
}

static int unavailable(const char *game, const char *reason) {
    printf("Skip Intro: requested but unavailable for %s: %s\n", game ? game : "an unknown game build", reason);
    return 0;
}

static int reachable(const unsigned char *from_next, const unsigned char *to, int32_t *rel) {
    const int64_t distance = (int64_t)(intptr_t)to - (int64_t)(intptr_t)from_next;
    if (distance > INT32_MAX || distance < INT32_MIN) return 0;
    *rel = (int32_t)distance;
    return 1;
}

static void write_call(unsigned char *at, int32_t rel, size_t length) {
    at[0] = 0xe8;
    memcpy(at + 1, &rel, sizeof(rel));
    memset(at + 5, 0x90, length - 5);
}

int skip_intro_install(unsigned char *image, uint64_t size, uint64_t entry, const char *game, int wanted,
                       unsigned char *stubs, size_t stubs_size) {
    (void)entry;
    if (!wanted) {
        puts("Skip Intro: off");
        return 0;
    }
    if (!game) return unavailable(game, "not a supported build");
    SkipIntroSites sites;
    const int listed = skip_intro_locate(image, size, game, &sites);
    if (!sites.redirect) return unavailable(game, listed ? "the verified title flow does not match" : "the title flow was not found exactly once");
    uint64_t notice = 0;
    if (skip_intro_locate_notice(image, size, game, &notice) == SKIP_INTRO_NOTICE_UNMATCHED)
        return unavailable(game, "the regional startup notice does not match");
    if (stubs_size < SKIP_INTRO_STUB_BYTES) return unavailable(game, "no room for the stub");
    unsigned char *redirect = image + sites.redirect, *title = image + sites.title;
    int32_t to_redirect, to_title;
    if (!reachable(redirect + 5, stubs, &to_redirect) || !reachable(title + 5, stubs + SKIP_INTRO_TITLE_STUB_AT, &to_title))
        return unavailable(game, "stub too far from the code");
    const uint64_t create = (uint64_t)(uintptr_t)(title + 5 + load_rel32(title + 1));
    unsigned char stub[SKIP_INTRO_STUB_BYTES];
    memset(stub, 0xcc, sizeof(stub));
    const size_t redirect_length = skip_intro_build_redirect_stub(stub, (uint64_t)(uintptr_t)skip_intro_claim,
                                                                  (uint64_t)(uintptr_t)skip_intro_redirected);
    const size_t title_length = skip_intro_build_title_stub(stub + SKIP_INTRO_TITLE_STUB_AT,
                                                            (uint64_t)(uintptr_t)skip_intro_title_reached, create);
    if (redirect_length > SKIP_INTRO_TITLE_STUB_AT || SKIP_INTRO_TITLE_STUB_AT + title_length > SKIP_INTRO_STUB_BYTES)
        return unavailable(game, "stub too large");
    memcpy(stubs, stub, sizeof(stub));
    write_call(redirect, to_redirect, SKIP_INTRO_REDIRECT_BYTES);
    write_call(title, to_title, SKIP_INTRO_TITLE_CALL_BYTES);
    if (notice) {
        unsigned char bypass[kBypassBytes];
        build_notice_bypass(bypass, image, notice - kNoticeBegins);
        memcpy(image + notice, bypass, sizeof(bypass));
        printf("Skip Intro: regional notice bypass installed (%s)\n", game);
    }
    skip_intro_arm();
    printf("Skip Intro: installed build=%s route=save_check->title (opening scene not created)\n", game);
    return 1;
}
