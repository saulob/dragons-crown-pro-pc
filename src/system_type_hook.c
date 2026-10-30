#include "system_type_hook.h"

#include <stdio.h>
#include <string.h>

const SystemTypeRecord system_type_records[SYSTEM_TYPE_RECORD_COUNT] = {
    {"CUSA10454 1.00 USA", {0x11f8060, 0x54c690}, {0x4d31e3, 0x4d31fc},
     {{0xe8, 0x18, 0x83, 0xb9, 0xff}, {0xe8, 0xff, 0x82, 0xb9, 0xff}}, 0x58f0e0},
    {"CUSA07001 1.00 Japan", {0x11e8060, 0x54a800}, {0x4d11f3, 0x4d120c},
     {{0xe8, 0xf8, 0x9c, 0xb9, 0xff}, {0xe8, 0xdf, 0x9c, 0xb9, 0xff}}, 0x58d180},
    {"CUSA07001 1.02 Japan", {0x11e8060, 0x54a660}, {0x4d16b3, 0x4d16cc},
     {{0xe8, 0x88, 0x98, 0xb9, 0xff}, {0xe8, 0x6f, 0x98, 0xb9, 0xff}}, 0x58d0b0},
    {"CUSA07001 1.03 Japan", {0x11e8060, 0x54a720}, {0x4d1773, 0x4d178c},
     {{0xe8, 0xc8, 0x97, 0xb9, 0xff}, {0xe8, 0xaf, 0x97, 0xb9, 0xff}}, 0x58d170},
    {"CUSA10209 1.00 Asia/Hong Kong", {0x1238060, 0x54a790}, {0x4d1363, 0x4d137c},
     {{0xe8, 0x58, 0x9c, 0xb9, 0xff}, {0xe8, 0x3f, 0x9c, 0xb9, 0xff}}, 0x58d1e0},
};

const uint8_t system_type_set_text_prologue[SYSTEM_TYPE_SET_TEXT_CHECK] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec};

/* The card filler from its Online ID line (set from r12) to the call after the two PS4 record sites; -1 is a call or
 * rip-relative displacement byte. */
static const int16_t kCard[] = {
    0x49, 0x8d, 0xbe, 0x70, 0x03, 0x00, 0x00, 0x4c, 0x0f, 0x47, 0xa5, 0x28, 0xff, 0xff, 0xff, 0x4c,
    0x89, 0xe6, 0xe8, -1,   -1,   -1,   -1,   0x0f, 0xb7, 0x45, 0xae, 0xf6, 0xc4, 0x20, 0x75, 0x4e,
    0x48, 0x8b, 0x1d, -1,   -1,   -1,   -1,   0x44, 0x8b, 0xad, 0x1c, 0xff, 0xff, 0xff, 0x49, 0x8d,
    0xbe, 0xb0, 0x05, 0x00, 0x00, 0xf6, 0xc4, 0x40, 0x75, 0x55, 0x66, 0x85, 0xc0, 0x78, 0x70, 0xe8,
    -1,   -1,   -1,   -1,   0xeb, 0x73, 0x49, 0x8d, 0xbe, 0xb0, 0x05, 0x00, 0x00, 0xbe, 0x23, 0x01,
    0x00, 0x00, 0xe8, -1,   -1,   -1,   -1,   0x48, 0x8b, 0x1d, -1,   -1,   -1,   -1,   0x44, 0x8b,
    0xad, 0x1c, 0xff, 0xff, 0xff, 0xeb, 0x39, 0xbe, 0x24, 0x01, 0x00, 0x00, 0xeb, 0x2d, 0x49, 0x8d,
    0xbe, 0xb0, 0x05, 0x00, 0x00, 0xbe, 0x23, 0x01, 0x00, 0x00, 0xe8, -1,   -1,   -1,   -1,   0x48,
    0x8b, 0x1d, -1,   -1,   -1,   -1,   0x44, 0x8b, 0xad, 0x1c, 0xff, 0xff, 0xff, 0xeb, 0x2a, 0xbe,
    0x24, 0x01, 0x00, 0x00, 0xeb, 0x1e, 0xbe, 0x22, 0x01, 0x00, 0x00, 0xe8, -1,   -1,   -1,   -1,
    0x4c, 0x89, 0xff, 0x4c, 0x89, 0xf6, 0x48, 0x8d, 0x8d, 0x58, 0xff, 0xff, 0xff, 0xeb, 0x17, 0xbe,
    0x22, 0x01, 0x00, 0x00, 0xe8, -1,   -1,   -1,   -1,   0x4c, 0x89, 0xff, 0x4c, 0x89, 0xf6, 0x48,
    0x8d, 0x8d, 0x50, 0xff, 0xff, 0xff, 0xe8, -1,   -1,   -1,   -1};
#define CARD_SET_TEXT_CALL 0x12
#define CARD_RECORD_CALL_1 0x52
#define CARD_RECORD_CALL_2 0x7a
#define CARD_SITE_1 0x9b
#define CARD_SITE_2 0xb4

static int card_at(const uint8_t *image, uint64_t size, uint64_t at) {
    if (at + sizeof(kCard) / sizeof(*kCard) > size) return 0;
    for (size_t i = 0; i < sizeof(kCard) / sizeof(*kCard); ++i)
        if (kCard[i] >= 0 && image[at + i] != (uint8_t)kCard[i]) return 0;
    return 1;
}

static uint64_t call_target(const uint8_t *image, uint64_t at) {
    int32_t rel;
    memcpy(&rel, image + at + 1, 4);
    return at + 5 + (int64_t)rel;
}

int system_type_locate(const uint8_t *image, uint64_t size, uint64_t entry, SystemTypeRecord *out) {
    for (size_t i = 0; i < SYSTEM_TYPE_RECORD_COUNT; ++i) {
        if (system_type_records[i].image.size == size && system_type_records[i].image.entry == entry) {
            *out = system_type_records[i];
            return 1;
        }
    }
    uint64_t card = 0;
    int found = 0;
    for (uint64_t i = 0; i < size && found < 2; ++i)
        if (image[i] == kCard[0] && card_at(image, size, i)) card = i, ++found;
    if (found != 1) return 0;
    /* All four record calls set one record through the same function. */
    const uint64_t setter = call_target(image, card + CARD_RECORD_CALL_1);
    if (call_target(image, card + CARD_RECORD_CALL_2) != setter || call_target(image, card + CARD_SITE_1) != setter ||
        call_target(image, card + CARD_SITE_2) != setter)
        return 0;
    const uint64_t set_text = call_target(image, card + CARD_SET_TEXT_CALL);
    if (set_text >= size || card + CARD_SITE_2 >= UINT32_MAX || set_text >= UINT32_MAX) return 0;
    *out = (SystemTypeRecord){"found by signature", {size, entry},
                              {(uint32_t)(card + CARD_SITE_1), (uint32_t)(card + CARD_SITE_2)}, {{0}}, (uint32_t)set_text};
    for (size_t i = 0; i < SYSTEM_TYPE_SITES; ++i) memcpy(out->calls[i], image + out->sites[i], 5);
    return 2;
}

static SystemTypeTextFn text_of;
static SystemTypeSetTextFn set_text;

void system_type_set_functions(SystemTypeTextFn text, SystemTypeSetTextFn set) {
    text_of = text;
    set_text = set;
}

/* At both sites r12 still points at the Online ID the card has just shown (its +0x370 line). */
void GUEST_HOOK_ABI system_type_hook(GuestRegs *regs) {
    if ((uint32_t)regs->rsi != SYSTEM_TYPE_PS4_RECORD || !text_of || !set_text || !regs->r12) return;
    const char *text = text_of((const char *)(uintptr_t)regs->r12);
    if (!text) return;
    set_text(regs->rdi, text);
    regs->skip = 1;
}

size_t system_type_hook_stub_bytes(void) {
    return SYSTEM_TYPE_SITES * guest_hook_stub_size();
}

int system_type_hook_install(uint8_t *image, uint64_t size, uint64_t entry, uint8_t *stubs, size_t stubs_size,
                             SystemTypeTextFn text) {
    SystemTypeRecord located;
    if (!system_type_locate(image, size, entry, &located)) {
        printf("System type: hook not installed: the player card's sites were not found\n");
        return 0;
    }
    const SystemTypeRecord *record = &located;
    if (record->set_text + SYSTEM_TYPE_SET_TEXT_CHECK > size ||
        memcmp(image + record->set_text, system_type_set_text_prologue, SYSTEM_TYPE_SET_TEXT_CHECK)) {
        printf("System type: hook not installed: unexpected text setter (%s)\n", record->build);
        return 0;
    }
    GuestHook hooks[SYSTEM_TYPE_SITES];
    for (size_t i = 0; i < SYSTEM_TYPE_SITES; ++i) {
        hooks[i] = (GuestHook){"system type", record->sites[i], 5, {0}, system_type_hook};
        memcpy(hooks[i].original, record->calls[i], 5);
    }
    size_t bad = 0;
    const GuestHookStatus status =
        guest_hooks_install(image, size, entry, &record->image, hooks, SYSTEM_TYPE_SITES, stubs, stubs_size, &bad);
    if (status != GUEST_HOOK_OK) {
        printf("System type: hook not installed: %s (site 0x%x, %s)\n", guest_hook_status_text(status),
               record->sites[bad], record->build);
        return 0;
    }
    system_type_set_functions(text, (SystemTypeSetTextFn)(uintptr_t)(image + record->set_text));
    printf("System type: hooks installed at 0x%x, 0x%x (%s)\n", record->sites[0], record->sites[1], record->build);
    return 1;
}
