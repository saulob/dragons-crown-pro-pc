#include "title_trace.h"

#include <stdio.h>
#include <string.h>

#define ANY (-1)

enum {
    HOOK_LENGTH = 8,
    LAYOUT_PART_COUNT = 0x64,
    LAYOUT_PARTS = 0xf8,
    PLAYER_RESOURCE = 0xc0,
    RESOURCE_LAYOUT = 0x158,
    PART_SIZE = 48,
    PART_NAME = 0x10,
    PART_NAME_SIZE = 24,
    ANCHOR_PART = 11,
    LOG_LIMIT = 80
};

static const short kPartSetter[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x28, 0x49, 0x89, 0xfe,
    0x49, 0x8b, 0x86, 0xc0, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc0, 0x0f, 0x84, ANY, ANY, ANY, ANY,
    0x4c, 0x8b, 0xa0, 0x58, 0x01, 0x00, 0x00, 0x49, 0x8b, 0x86, 0xc8, 0x00, 0x00, 0x00, 0x4c, 0x89, 0xe2, 0x48, 0x85, 0xc0,
    0x74, 0x07, 0x48, 0x8b, 0x90, 0x58, 0x01, 0x00, 0x00, 0x4d, 0x85, 0xe4, 0x0f, 0x84, ANY, ANY, ANY, ANY,
    0x41, 0x0f, 0xb7, 0x44, 0x24, 0x64, 0x39, 0xf0, 0x0f, 0x8e, ANY, ANY, ANY, ANY, 0x85, 0xf6, 0x0f, 0x88, ANY, ANY,
    ANY, ANY,
};

static char last[2][PART_NAME_SIZE + 1];
static int lines;

size_t title_trace_stub_bytes(void) {
    return guest_hook_stub_size();
}

void title_trace_reset(void) {
    memset(last, 0, sizeof(last));
    lines = 0;
}

int title_trace_lines(void) {
    return lines;
}

const char *title_trace_last(int player) {
    return last[player & 1];
}

static uint16_t read_u16(const uint8_t *at) {
    uint16_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

static const uint8_t *read_pointer(const uint8_t *at) {
    uint64_t value;
    memcpy(&value, at, sizeof(value));
    return (const uint8_t *)(uintptr_t)value;
}

static int part_name(const uint8_t *layout, int32_t part, char text[PART_NAME_SIZE + 1]) {
    if (!layout || memcmp(layout, "FMBS", 4) || part < 0 || part >= read_u16(layout + LAYOUT_PART_COUNT)) return 0;
    const uint8_t *parts = read_pointer(layout + LAYOUT_PARTS);
    if (!parts) return 0;
    memcpy(text, parts + (size_t)part * PART_SIZE + PART_NAME, PART_NAME_SIZE);
    text[PART_NAME_SIZE] = '\0';
    return 1;
}

/* Entry of the part setter: rdi = the player, esi = the part. */
void GUEST_HOOK_ABI title_trace_hook(GuestRegs *regs) {
    if (!regs->rdi) return;
    const uint8_t *resource = read_pointer((const uint8_t *)(uintptr_t)regs->rdi + PLAYER_RESOURCE);
    const uint8_t *layout = resource ? read_pointer(resource + RESOURCE_LAYOUT) : NULL;
    char anchor[PART_NAME_SIZE + 1], name[PART_NAME_SIZE + 1];
    if (!part_name(layout, ANCHOR_PART, anchor) || strcmp(anchor, "TITLE_1_PS4") || !part_name(layout, (int32_t)regs->rsi, name))
        return;
    const int player = !strncmp(name, "TITLE_GROUND", 12) ? 0 : 1;
    if (lines < LOG_LIMIT) {
        ++lines;
        printf("DEBUG_SAULO title transition: %s -> %s (%s)\n", last[player][0] ? last[player] : "none", name,
               player ? "art" : "ground");
    }
    memcpy(last[player], name, sizeof(name));
}

static int window_matches(const uint8_t *image, uint64_t at) {
    for (size_t i = 0; i < sizeof(kPartSetter) / sizeof(*kPartSetter); ++i)
        if (kPartSetter[i] != ANY && image[at + i] != (uint8_t)kPartSetter[i]) return 0;
    return 1;
}

int title_trace_locate(const uint8_t *image, uint64_t size, uint32_t *site) {
    const size_t length = sizeof(kPartSetter) / sizeof(*kPartSetter);
    uint64_t found = 0;
    int matches = 0;
    for (uint64_t at = 0; at + length <= size; ++at) {
        if (image[at] != (uint8_t)kPartSetter[0] || !window_matches(image, at)) continue;
        if (matches++) return 0;
        found = at;
    }
    if (!matches || found > UINT32_MAX) return 0;
    *site = (uint32_t)found;
    return 1;
}

int title_trace_install(uint8_t *image, uint64_t size, uint64_t entry, uint8_t *stubs, size_t stubs_size) {
    uint32_t site;
    if (!title_trace_locate(image, size, &site)) {
        printf("DEBUG_SAULO title transition: not traced, the part setter is not found exactly once\n");
        return 0;
    }
    GuestHook hook = {"title trace", site, HOOK_LENGTH, {0}, title_trace_hook};
    memcpy(hook.original, image + site, HOOK_LENGTH);
    const GuestImageId id = {size, entry};
    size_t bad = 0;
    const GuestHookStatus status = guest_hooks_install(image, size, entry, &id, &hook, 1, stubs, stubs_size, &bad);
    if (status != GUEST_HOOK_OK) {
        printf("DEBUG_SAULO title transition: not traced: %s\n", guest_hook_status_text(status));
        return 0;
    }
    title_trace_reset();
    printf("DEBUG_SAULO title transition: tracing the title part sequence (part setter 0x%x)\n", site);
    return 1;
}
