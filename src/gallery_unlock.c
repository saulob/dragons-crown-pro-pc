#include "gallery_unlock.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ANY (-1)
#define STATE_NEW 1
#define STATE_SEEN 2

/* The list builder's loop body: the entry's array index, the save's art array through the getter, the stored state,
 * and the art number of an entry that is not locked. The getter call is the one the setting replaces. */
static const short kWindow[] = {
    0xe8, ANY, ANY, ANY, ANY, 0x41, 0x89, 0xc4, 0x45, 0x85, 0xe4, 0x78, ANY, 0x49, 0x8b, 0x45, 0x28, 0x48, 0x8b, 0xb8,
    0xe8, 0x00, 0x00, 0x00, 0xe8, ANY, ANY, ANY, ANY, 0x44, 0x89, 0xe1, 0x0f, 0xb6, 0x04, 0x08, 0x89, 0xc1, 0x89, 0x45,
    0xc0, 0xfe, 0xc9, 0x80, 0xf9, 0x02, 0x73, ANY, 0x89, 0xdf, 0x89, 0x5d, 0xc4, 0xe8, ANY, ANY, ANY, ANY, 0x0f, 0xbe,
    0xc0, 0xff, 0xc0, 0x89, 0x45, 0xc8,
};
enum {
    kWindowCount = sizeof(kWindow) / sizeof(*kWindow),
    kKeyCallInWindow = 0,
    kHookInWindow = 24,
    kHookLength = 5
};

/* The array index of an entry: a table of 57 records the executable fills in at start; index 49 belongs to no entry. */
static const short kKeyBody[] = {
    0xb8, 0xff, 0xff, 0xff, 0xff, 0x83, 0xff, 0x38, 0x77, 0x12, 0x48, 0x63, 0xc7, 0x48, 0x8d, 0x0d, ANY, ANY, ANY, ANY,
    0x48, 0x8d, 0x04, 0x40, 0x8b, 0x44, 0x81, 0x04, 0xc3,
};
/* lea rax,[rdi+GALLERY_UNLOCK_SAVE_OFFSET]; ret */
static const short kGetterBody[] = {0x48, 0x8d, 0x87, 0xcc, 0xea, 0x20, 0x00, 0xc3};

typedef struct {
    const char *game;
    uint64_t window;
} BuildSite;

static const BuildSite kBuilds[] = {
    {"CUSA10454-1.00", 0x4d5492},
    {"CUSA07001-1.00", 0x4d34a2},
    {"CUSA07001-1.02", 0x4d3962},
    {"CUSA07001-1.03", 0x4d3a22},
    {"CUSA10209-1.00", 0x4d3612},
};

static int enabled_by_name(const char *value, int fallback) {
    if (!value || !*value) return fallback;
    if (!strcmp(value, "1") || !strcmp(value, "on") || !strcmp(value, "true")) return 1;
    if (!strcmp(value, "0") || !strcmp(value, "off") || !strcmp(value, "false")) return 0;
    return fallback;
}

int gallery_unlock_wanted(int saved) {
    return enabled_by_name(getenv("DCP_UNLOCK_GALLERY"), saved != 0);
}

int gallery_unlock_entry_key(int entry) {
    if (entry < 0 || entry >= GALLERY_UNLOCK_ENTRIES) return -1;
    return entry < 48 ? entry + 1 : entry + 2;
}

void gallery_unlock_fill_view(unsigned char *view, const unsigned char *flags) {
    memcpy(view, flags, GALLERY_UNLOCK_FLAG_BYTES);
    for (int entry = 0; entry < GALLERY_UNLOCK_ENTRIES; ++entry) {
        unsigned char *state = &view[gallery_unlock_entry_key(entry)];
        if (*state != STATE_NEW) *state = STATE_SEEN;
    }
}

static int shape_at(const unsigned char *image, uint64_t size, uint64_t at, const short *shape, size_t count) {
    if (at > size || size - at < count) return 0;
    for (size_t i = 0; i < count; ++i)
        if (shape[i] != ANY && image[at + i] != (unsigned char)shape[i]) return 0;
    return 1;
}

static int64_t call_target(const unsigned char *image, uint64_t at) {
    int32_t rel;
    memcpy(&rel, image + at + 1, sizeof(rel));
    return (int64_t)at + 5 + rel;
}

static int target_matches(const unsigned char *image, uint64_t size, int64_t target, const short *shape, size_t count) {
    return target >= 0 && shape_at(image, size, (uint64_t)target, shape, count);
}

static int window_matches(const unsigned char *image, uint64_t size, uint64_t at) {
    return shape_at(image, size, at, kWindow, kWindowCount) &&
           target_matches(image, size, call_target(image, at + kKeyCallInWindow), kKeyBody,
                          sizeof(kKeyBody) / sizeof(*kKeyBody)) &&
           target_matches(image, size, call_target(image, at + kHookInWindow), kGetterBody,
                          sizeof(kGetterBody) / sizeof(*kGetterBody));
}

/* The one place in the image the list builder appears; 0 when it appears nowhere or more than once. */
static uint64_t find_window(const unsigned char *image, uint64_t size) {
    uint64_t found = 0;
    int matches = 0;
    for (uint64_t at = 0; at + kWindowCount <= size; ++at) {
        if (image[at] != 0xe8 || !window_matches(image, size, at)) continue;
        if (matches++) return 0;
        found = at;
    }
    return found;
}

int gallery_unlock_locate(const unsigned char *image, uint64_t size, const char *game, uint64_t *site) {
    uint64_t window = 0;
    int listed = 0;
    for (size_t i = 0; game && i < sizeof(kBuilds) / sizeof(*kBuilds); ++i) {
        if (strcmp(game, kBuilds[i].game)) continue;
        listed = 1;
        window = window_matches(image, size, kBuilds[i].window) ? kBuilds[i].window : 0;
    }
    if (!listed && game) window = find_window(image, size);
    *site = window ? window + kHookInWindow : 0;
    return listed;
}

size_t gallery_unlock_stub_bytes(void) {
    return guest_hook_stub_size();
}

void GUEST_HOOK_ABI gallery_unlock_view(GuestRegs *regs) {
    static _Thread_local unsigned char view[GALLERY_UNLOCK_FLAG_BYTES];
    const unsigned char *save = (const unsigned char *)(uintptr_t)regs->rdi;
    if (!save) return;
    const unsigned char *flags = save + GALLERY_UNLOCK_SAVE_OFFSET;
    gallery_unlock_fill_view(view, flags);
    regs->rax = (uint64_t)(uintptr_t)view;
    regs->skip = 1;
}

static int unavailable(const char *game, const char *reason) {
    printf("Unlock Gallery: requested but unavailable for %s: %s\n", game ? game : "an unknown game build", reason);
    return 0;
}

int gallery_unlock_install(unsigned char *image, uint64_t size, uint64_t entry, const char *game, int wanted,
                           unsigned char *stubs, size_t stubs_size) {
    if (!wanted) {
        puts("Unlock Gallery: off");
        return 0;
    }
    if (!game) return unavailable(game, "not a supported build");
    uint64_t site = 0;
    const int listed = gallery_unlock_locate(image, size, game, &site);
    if (!site) return unavailable(game, listed ? "the verified site does not match" : "Gallery list builder not found exactly once");
    GuestHook hook = {"gallery list", (uint32_t)site, kHookLength, {0}, gallery_unlock_view};
    memcpy(hook.original, image + site, kHookLength);
    size_t bad = 0;
    const GuestHookStatus status = guest_hooks_install(image, size, entry, NULL, &hook, 1, stubs, stubs_size, &bad);
    if (status != GUEST_HOOK_OK) return unavailable(game, guest_hook_status_text(status));
    printf("Unlock Gallery: on, %d legitimate entries exposed (%s)\n", GALLERY_UNLOCK_ENTRIES, game);
    return 1;
}
