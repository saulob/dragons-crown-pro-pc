#include "endgame_unlocks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ANY (-1)
#define PRAYER_KIND_DIFFICULTY 0x67

/* The stage selection's step to the next stage: the Labyrinth wrapper decides whether the Labyrinth follows stage 8. */
static const short kDirector[] = {
    0x41, 0x8b, 0x9c, 0x24, 0xf4, 0x2c, 0x00, 0x00, 0xe8, ANY, ANY, ANY, ANY, 0x84, 0xc0, 0x74,
    0x0b, 0x41, 0xbe, 0x11, 0x00, 0x00, 0x00, 0x83, 0xfb, 0x08, 0x74, 0x6c, 0x48, 0x8b, 0x7c, 0x24,
    ANY, 0xe8, ANY, ANY, ANY, ANY, 0x83, 0xfb, 0x11, 0x75, 0x07, 0x45, 0x31, 0xf6, 0x84, 0xc0,
};
/* push rbp; mov rbp,rsp; call lead record; test rax,rax; je; mov rdi,rax; call story check; jmp; xor eax,eax; pop; ret */
static const short kWrapper[] = {
    0x55, 0x48, 0x89, 0xe5, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x85, 0xc0, 0x74, 0x0a, 0x48, 0x89,
    0xc7, 0xe8, ANY, ANY, ANY, ANY, 0xeb, 0x02, 0x31, 0xc0, 0x5d, 0xc3,
};
/* cmp byte [rdi+0x6f],imm; seta al; ret */
static const uint8_t kLabyrinthBody[] = {0x80, 0x7f, 0x6f, ENDGAME_LABYRINTH_AFTER, 0x0f, 0x97, 0xc0, 0xc3};
static const uint8_t kStablesBody[] = {0x80, 0x7f, 0x6f, ENDGAME_STABLES_AFTER, 0x0f, 0x97, 0xc0, 0xc3};
/* The prayer list loop: prayer record, tier check, and for prayer 19 the save's Ultimate flag through [rbp-0x48]. */
static const short kList[] = {
    0x44, 0x89, 0xff, 0x44, 0x89, 0x7d, 0xcc, 0xe8, ANY, ANY, ANY, ANY, 0x4c, 0x89, 0xe7, 0x48,
    0x89, 0xc3, 0xe8, ANY, ANY, ANY, ANY, 0x3b, 0x43, 0x04, 0x0f, 0x8c, 0xbd, 0x00, 0x00, 0x00,
    0x8b, 0x4d, 0xcc, 0x83, 0xf9, 0x13, 0x75, 0x38, 0x48, 0x8b, 0x45, 0xb8, 0x80, 0xb8, 0xa9, 0x01,
    0x00, 0x00, 0x00, 0x0f, 0x84, 0xa4, 0x00, 0x00, 0x00, 0x49,
};
/* A prayer row: tier check (hidden name below it), then for prayers 14..19 the cleared-difficulty check (dimmed). */
static const short kRow[] = {
    0x80, 0xa3, 0x60, 0x03, 0x00, 0x00, 0xfd, 0x41, 0xbc, 0xa8, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x83,
    0x48, 0x03, 0x00, 0x00, 0x48, 0x8b, 0x40, 0x28, 0x4c, 0x03, 0xa0, 0x80, 0x00, 0x00, 0x00, 0x4c,
    0x89, 0xe7, 0xe8, ANY, ANY, ANY, ANY, 0x41, 0x3b, 0x46, 0x04, 0x7d, 0x0b, 0x48, 0x8d, 0x7d,
    0xa8, 0xe8, ANY, ANY, ANY, ANY, 0xeb, 0x23, 0x8b, 0x83, 0x64, 0x03, 0x00, 0x00, 0x83, 0xc0,
    0xf2, 0x83, 0xf8, 0x05, 0x77, 0x15, 0x4c, 0x89, 0xe7, 0xe8, ANY, ANY, ANY, ANY, 0x41, 0x3b,
    0x46, 0x04, 0x7d, 0x07, 0x80, 0x8b, 0x60, 0x03, 0x00, 0x00, 0x02,
};
/* Confirming a prayer: below the required tier the Temple says the prayer is not yet available. */
static const short kConfirm[] = {
    0x89, 0xbd, 0x68, 0xff, 0xff, 0xff, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x89, 0xc3, 0xbf, 0xa8,
    0x00, 0x00, 0x00, 0x8b, 0x43, 0x08, 0x89, 0x85, 0x70, 0xff, 0xff, 0xff, 0x45, 0x8b, 0xa5, 0x38,
    0xf2, 0x0a, 0x00, 0x49, 0x03, 0xbe, 0x80, 0x00, 0x00, 0x00, 0xe8, ANY, ANY, ANY, ANY, 0x3b,
    0x43, 0x04, 0x0f, 0x8d, 0xfd, 0x0a, 0x00, 0x00, 0x48, 0x8b, 0xbd, 0x78, 0xff, 0xff,
};
/* Prayer tier from the story step: 0x2e..0x30 -> 1, 0x31..0x32 -> 2, above -> 3, else 0. */
static const uint8_t kTierBody[] = {
    0x0f, 0xb6, 0x4f, 0x6f, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x8d, 0x51, 0xd2, 0x83, 0xfa, 0x03, 0x72,
    0x18, 0x8d, 0x51, 0xcf, 0xb8, 0x02, 0x00, 0x00, 0x00, 0x83, 0xfa, 0x02, 0x72, 0x0b, 0x31, 0xc0,
    0x80, 0xf9, 0x32, 0x0f, 0x97, 0xc0, 0x8d, 0x04, 0x40, 0xc3,
};
/* Cleared difficulty from the story step: 0x30..0x31 -> 2, above -> 4, else 0. */
static const uint8_t kClearedBody[] = {
    0x8a, 0x47, 0x6f, 0x31, 0xd2, 0x89, 0xc1, 0x80, 0xe1, 0xfe, 0x3c, 0x31, 0xb8, 0x02, 0x00, 0x00,
    0x00, 0x0f, 0x97, 0xc2, 0xc1, 0xe2, 0x02, 0x80, 0xf9, 0x30, 0x0f, 0x45, 0xc2, 0xc3,
};
/* The difficulty setter: record byte 0x21a9 holds the difficulty, 0x63 is Ultimate. */
static const uint8_t kSetter[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x48, 0x63, 0xc2, 0x49, 0x89, 0xfe,
    0x31, 0xdb, 0x41, 0x89, 0xf7, 0x48, 0x69, 0xc8, 0xb0, 0x23, 0x00, 0x00, 0x83, 0xf8, 0x03, 0x49,
    0x8d, 0x84, 0x0e, 0xa8, 0x00, 0x00, 0x00, 0x48, 0x0f, 0x46, 0xd8, 0x77, 0x72, 0x83, 0xbb, 0xe0,
    0x21, 0x00, 0x00, 0xff, 0x74, 0x69, 0x44, 0x88, 0xbb, 0xa9, 0x21, 0x00, 0x00, 0x4c, 0x89, 0xf7,
    0x49, 0x8b, 0x06, 0xff, 0x90, 0xd0, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x80, 0x20, 0x01, 0x00, 0x00,
    0x4c, 0x89, 0xf7, 0x44, 0x89, 0xb8, 0x84, 0x00, 0x00, 0x00, 0x49, 0x8b, 0x06, 0xff, 0x90, 0xd0,
    0x00, 0x00, 0x00, 0x48, 0x8b, 0xb8, 0x20, 0x01, 0x00, 0x00, 0x31, 0xf6, 0x48, 0x83, 0xc7, 0x70,
    0x41, 0x83, 0xff, 0x63, 0x40, 0x0f, 0x94, 0xc6,
};

#define COUNT(a) (sizeof(a) / sizeof(*(a)))

enum {
    kDirectorWrapperCall = 8,
    kDirectorSecondCall = 0x21,
    kWrapperRecordCall = 4,
    kWrapperCheckCall = 0x11,
    kStablesWrapperBefore = 0x40,
    kListPrayerCall = 7,
    kListTierCall = 0x12,
    kRowTierCall = 0x22,
    kRowClearedCall = 0x49,
    kConfirmPrayerCall = 6,
    kConfirmTierCall = 0x2a,
    kCallLength = 5,
    kHookCount = 5,
    kListedSlot = 0x48 /* [rbp-0x48]: the save the list's Ultimate flag check reads, used by nothing else */
};

typedef struct {
    uint64_t director, list, row, confirm, setter;
} Windows;

typedef struct {
    const char *game;
    Windows windows;
} BuildWindows;

static const BuildWindows kBuilds[] = {
    {"CUSA10454-1.00", {0x4308a7, 0x3bd150, 0x3bc9c5, 0x3c1eb5, 0x26b40}},
    {"CUSA10454-1.01", {0x4308a7, 0x3bd150, 0x3bc9c5, 0x3c1eb5, 0x26b40}},
    {"CUSA07001-1.00", {0x42face, 0x3bc4b0, 0x3bbd25, 0x3c11b5, 0x26490}},
    {"CUSA07001-1.02", {0x42feb7, 0x3bc880, 0x3bc0f5, 0x3c15c5, 0x26510}},
    {"CUSA07001-1.03", {0x42ff77, 0x3bc8d0, 0x3bc145, 0x3c1615, 0x26510}},
    {"CUSA10209-1.00", {0x42fc3e, 0x3bc620, 0x3bbe95, 0x3c1325, 0x26550}},
};

static unsigned active_unlocks;

static int enabled_by_name(const char *value, int fallback) {
    if (!value || !*value) return fallback;
    if (!strcmp(value, "1") || !strcmp(value, "on") || !strcmp(value, "true")) return 1;
    if (!strcmp(value, "0") || !strcmp(value, "off") || !strcmp(value, "false")) return 0;
    return fallback;
}

unsigned endgame_unlocks_wanted(unsigned saved) {
    unsigned wanted = 0;
    if (enabled_by_name(getenv("DCP_UNLOCK_LABYRINTH"), (saved & ENDGAME_UNLOCK_LABYRINTH) != 0))
        wanted |= ENDGAME_UNLOCK_LABYRINTH;
    if (enabled_by_name(getenv("DCP_UNLOCK_ULTIMATE_TOWER"), (saved & ENDGAME_UNLOCK_ULTIMATE_TOWER) != 0))
        wanted |= ENDGAME_UNLOCK_ULTIMATE_TOWER;
    return wanted;
}

/* ---- the game's answers and the unlocked ones ---- */

static int32_t read32(const unsigned char *at) {
    int32_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

static int16_t read16(const unsigned char *at) {
    int16_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

int endgame_stables_open(const unsigned char *record) {
    return record[ENDGAME_STORY_STEP] > ENDGAME_STABLES_AFTER;
}

int endgame_native_labyrinth(const unsigned char *record) {
    return record[ENDGAME_STORY_STEP] > ENDGAME_LABYRINTH_AFTER;
}

int endgame_native_prayer_tier(const unsigned char *record) {
    const unsigned step = record[ENDGAME_STORY_STEP];
    if (step - 0x2eu < 3) return 1;
    if (step - 0x31u < 2) return 2;
    return step > 0x32 ? 3 : 0;
}

int endgame_native_cleared_tier(const unsigned char *record) {
    const unsigned step = record[ENDGAME_STORY_STEP];
    if ((step & 0xfe) == 0x30) return 2;
    return step > 0x31 ? 4 : 0;
}

static int on_ultimate(const unsigned char *record) {
    return record[ENDGAME_DIFFICULTY] == ENDGAME_ULTIMATE;
}

int endgame_labyrinth_open(const unsigned char *record, unsigned unlocks) {
    if (endgame_native_labyrinth(record)) return 1;
    if (!endgame_stables_open(record)) return 0;
    return (unlocks & ENDGAME_UNLOCK_LABYRINTH) || ((unlocks & ENDGAME_UNLOCK_ULTIMATE_TOWER) && on_ultimate(record));
}

static int is_difficulty_prayer(const unsigned char *prayer, int id, int value) {
    return read32(prayer) == id && read16(prayer + 0x10) == PRAYER_KIND_DIFFICULTY && read32(prayer + 0x14) == value;
}

int endgame_prayer_exposed(const unsigned char *record, const unsigned char *prayer, unsigned unlocks) {
    if (!(unlocks & ENDGAME_UNLOCK_ULTIMATE_TOWER) || !prayer || !endgame_stables_open(record)) return 0;
    if (is_difficulty_prayer(prayer, ENDGAME_PRAYER_ULTIMATE, ENDGAME_ULTIMATE)) return 1;
    return on_ultimate(record) && is_difficulty_prayer(prayer, ENDGAME_PRAYER_NORMAL, 0);
}

static int at_least_required(int tier, const unsigned char *prayer) {
    const int required = read32(prayer + 4);
    return tier < required ? required : tier;
}

int endgame_prayer_tier(const unsigned char *record, const unsigned char *prayer, unsigned unlocks) {
    const int tier = endgame_native_prayer_tier(record);
    return endgame_prayer_exposed(record, prayer, unlocks) ? at_least_required(tier, prayer) : tier;
}

int endgame_cleared_tier(const unsigned char *record, const unsigned char *prayer, unsigned unlocks) {
    const int tier = endgame_native_cleared_tier(record);
    return endgame_prayer_exposed(record, prayer, unlocks) ? at_least_required(tier, prayer) : tier;
}

/* ---- hooks ---- */

void endgame_unlocks_set(unsigned unlocks) {
    active_unlocks = unlocks;
}

static const unsigned char *record_of(const GuestRegs *regs) {
    return (const unsigned char *)(uintptr_t)regs->rdi;
}

static void answer(GuestRegs *regs, int value) {
    regs->rax = (uint64_t)(uint32_t)value;
    regs->skip = 1;
}

void GUEST_HOOK_ABI endgame_labyrinth_check(GuestRegs *regs) {
    answer(regs, endgame_labyrinth_open(record_of(regs), active_unlocks));
}

void GUEST_HOOK_ABI endgame_list_tier(GuestRegs *regs) {
    static const unsigned char listed = 1;
    const unsigned char *record = record_of(regs), *prayer = (const unsigned char *)(uintptr_t)regs->rbx;
    if (endgame_prayer_exposed(record, prayer, active_unlocks) &&
        is_difficulty_prayer(prayer, ENDGAME_PRAYER_ULTIMATE, ENDGAME_ULTIMATE)) {
        const uintptr_t view = (uintptr_t)&listed - ENDGAME_ULTIMATE_LISTED;
        memcpy((void *)(uintptr_t)(regs->rbp - kListedSlot), &view, sizeof(view));
    }
    answer(regs, endgame_prayer_tier(record, prayer, active_unlocks));
}

void GUEST_HOOK_ABI endgame_row_tier(GuestRegs *regs) {
    answer(regs, endgame_prayer_tier(record_of(regs), (const unsigned char *)(uintptr_t)regs->r14, active_unlocks));
}

void GUEST_HOOK_ABI endgame_row_cleared(GuestRegs *regs) {
    answer(regs, endgame_cleared_tier(record_of(regs), (const unsigned char *)(uintptr_t)regs->r14, active_unlocks));
}

void GUEST_HOOK_ABI endgame_confirm_tier(GuestRegs *regs) {
    answer(regs, endgame_prayer_tier(record_of(regs), (const unsigned char *)(uintptr_t)regs->rbx, active_unlocks));
}

/* ---- locating ---- */

static int shape_at(const unsigned char *image, uint64_t size, uint64_t at, const short *shape, size_t count) {
    if (at > size || size - at < count) return 0;
    for (size_t i = 0; i < count; ++i)
        if (shape[i] != ANY && image[at + i] != (unsigned char)shape[i]) return 0;
    return 1;
}

static int bytes_at(const unsigned char *image, uint64_t size, int64_t at, const uint8_t *bytes, size_t count) {
    return at >= 0 && (uint64_t)at <= size && size - (uint64_t)at >= count && !memcmp(image + at, bytes, count);
}

static int64_t call_target(const unsigned char *image, uint64_t at) {
    int32_t rel;
    memcpy(&rel, image + at + 1, sizeof(rel));
    return (int64_t)at + kCallLength + rel;
}

static int wrapper_at(const unsigned char *image, uint64_t size, int64_t at) {
    return at >= 0 && shape_at(image, size, (uint64_t)at, kWrapper, COUNT(kWrapper));
}

/* The Labyrinth wrapper both director calls reach, and the Stables wrapper the build places just before it. */
static uint64_t labyrinth_site(const unsigned char *image, uint64_t size, uint64_t director) {
    const int64_t wrapper = call_target(image, director + kDirectorWrapperCall);
    if (call_target(image, director + kDirectorSecondCall) != wrapper || !wrapper_at(image, size, wrapper)) return 0;
    const int64_t stables = wrapper - kStablesWrapperBefore;
    if (!wrapper_at(image, size, stables) ||
        call_target(image, (uint64_t)stables + kWrapperRecordCall) != call_target(image, (uint64_t)wrapper + kWrapperRecordCall))
        return 0;
    if (!bytes_at(image, size, call_target(image, (uint64_t)wrapper + kWrapperCheckCall), kLabyrinthBody, sizeof(kLabyrinthBody)) ||
        !bytes_at(image, size, call_target(image, (uint64_t)stables + kWrapperCheckCall), kStablesBody, sizeof(kStablesBody)))
        return 0;
    return (uint64_t)wrapper + kWrapperCheckCall;
}

static int temple_sites(const unsigned char *image, uint64_t size, const Windows *w, EndgameSites *sites) {
    const int64_t tier = call_target(image, w->list + kListTierCall);
    if (!bytes_at(image, size, tier, kTierBody, sizeof(kTierBody)) ||
        call_target(image, w->row + kRowTierCall) != tier || call_target(image, w->confirm + kConfirmTierCall) != tier ||
        call_target(image, w->list + kListPrayerCall) != call_target(image, w->confirm + kConfirmPrayerCall) ||
        !bytes_at(image, size, call_target(image, w->row + kRowClearedCall), kClearedBody, sizeof(kClearedBody)))
        return 0;
    sites->list_tier = w->list + kListTierCall;
    sites->row_tier = w->row + kRowTierCall;
    sites->row_cleared = w->row + kRowClearedCall;
    sites->confirm_tier = w->confirm + kConfirmTierCall;
    return 1;
}

static int windows_match(const unsigned char *image, uint64_t size, const Windows *w, EndgameSites *sites) {
    memset(sites, 0, sizeof(*sites));
    if (!shape_at(image, size, w->director, kDirector, COUNT(kDirector)) || !shape_at(image, size, w->list, kList, COUNT(kList)) ||
        !shape_at(image, size, w->row, kRow, COUNT(kRow)) || !shape_at(image, size, w->confirm, kConfirm, COUNT(kConfirm)) ||
        !bytes_at(image, size, (int64_t)w->setter, kSetter, sizeof(kSetter)))
        return 0;
    sites->labyrinth = labyrinth_site(image, size, w->director);
    if (!sites->labyrinth || !temple_sites(image, size, w, sites)) {
        memset(sites, 0, sizeof(*sites));
        return 0;
    }
    return 1;
}

static uint64_t find_once(const unsigned char *image, uint64_t size, const short *shape, size_t count) {
    uint64_t found = 0;
    int matches = 0;
    for (uint64_t at = 0; at + count <= size; ++at) {
        if (image[at] != (unsigned char)shape[0] || !shape_at(image, size, at, shape, count)) continue;
        if (matches++) return 0;
        found = at;
    }
    return found;
}

static uint64_t find_setter(const unsigned char *image, uint64_t size) {
    short shape[sizeof(kSetter)];
    for (size_t i = 0; i < sizeof(kSetter); ++i) shape[i] = kSetter[i];
    return find_once(image, size, shape, COUNT(shape));
}

int endgame_unlocks_locate(const unsigned char *image, uint64_t size, const char *game, EndgameSites *sites) {
    memset(sites, 0, sizeof(*sites));
    for (size_t i = 0; game && i < COUNT(kBuilds); ++i) {
        if (strcmp(game, kBuilds[i].game)) continue;
        windows_match(image, size, &kBuilds[i].windows, sites);
        return 1;
    }
    if (!game) return 0;
    const Windows found = {find_once(image, size, kDirector, COUNT(kDirector)), find_once(image, size, kList, COUNT(kList)),
                           find_once(image, size, kRow, COUNT(kRow)), find_once(image, size, kConfirm, COUNT(kConfirm)),
                           find_setter(image, size)};
    if (found.director && found.list && found.row && found.confirm && found.setter) windows_match(image, size, &found, sites);
    return 0;
}

/* ---- install ---- */

size_t endgame_unlocks_stub_bytes(void) {
    return kHookCount * guest_hook_stub_size();
}

static const char *on_off(unsigned unlocks, unsigned bit) {
    return unlocks & bit ? "on" : "off";
}

static unsigned unavailable(const char *game, const char *reason) {
    printf("Endgame unlocks: requested but unavailable for %s: %s\n", game ? game : "an unknown game build", reason);
    return 0;
}

unsigned endgame_unlocks_install(unsigned char *image, uint64_t size, uint64_t entry, const char *game, unsigned wanted,
                                 unsigned char *stubs, size_t stubs_size) {
    wanted &= ENDGAME_UNLOCK_LABYRINTH | ENDGAME_UNLOCK_ULTIMATE_TOWER;
    endgame_unlocks_set(0);
    if (!wanted) {
        puts("Endgame unlocks: Labyrinth off, Ultimate + Tower off");
        return 0;
    }
    if (!game) return unavailable(game, "not a supported build");
    EndgameSites sites;
    const int listed = endgame_unlocks_locate(image, size, game, &sites);
    if (!sites.labyrinth) return unavailable(game, listed ? "the verified sites do not match" : "sites not found exactly once");

    GuestHook hooks[kHookCount] = {
        {"endgame labyrinth", (uint32_t)sites.labyrinth, kCallLength, {0}, endgame_labyrinth_check},
        {"endgame prayer list", (uint32_t)sites.list_tier, kCallLength, {0}, endgame_list_tier},
        {"endgame prayer row", (uint32_t)sites.row_tier, kCallLength, {0}, endgame_row_tier},
        {"endgame prayer row cleared", (uint32_t)sites.row_cleared, kCallLength, {0}, endgame_row_cleared},
        {"endgame prayer confirm", (uint32_t)sites.confirm_tier, kCallLength, {0}, endgame_confirm_tier},
    };
    const size_t count = wanted & ENDGAME_UNLOCK_ULTIMATE_TOWER ? kHookCount : 1;
    for (size_t i = 0; i < count; ++i) memcpy(hooks[i].original, image + hooks[i].offset, kCallLength);
    size_t bad = 0;
    const GuestHookStatus status = guest_hooks_install(image, size, entry, NULL, hooks, count, stubs, stubs_size, &bad);
    if (status != GUEST_HOOK_OK) return unavailable(game, guest_hook_status_text(status));
    endgame_unlocks_set(wanted);
    printf("Endgame unlocks: Labyrinth %s, Ultimate + Tower %s, after the Stables open (%s)\n",
           on_off(wanted, ENDGAME_UNLOCK_LABYRINTH), on_off(wanted, ENDGAME_UNLOCK_ULTIMATE_TOWER), game);
    return wanted;
}
