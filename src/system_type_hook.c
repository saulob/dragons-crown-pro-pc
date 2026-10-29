#include "system_type_hook.h"

#include <stdio.h>
#include <string.h>

const SystemTypeRecord system_type_records[SYSTEM_TYPE_RECORD_COUNT] = {
    {"CUSA10454 1.00 USA", {0x11f8060, 0x54c690}, {0x4d31e3, 0x4d31fc},
     {{0xe8, 0x18, 0x83, 0xb9, 0xff}, {0xe8, 0xff, 0x82, 0xb9, 0xff}}, 0x58f0e0},
    {"CUSA07001 1.02 Japan", {0x11e8060, 0x54a660}, {0x4d16b3, 0x4d16cc},
     {{0xe8, 0x88, 0x98, 0xb9, 0xff}, {0xe8, 0x6f, 0x98, 0xb9, 0xff}}, 0x58d0b0},
    {"CUSA07001 1.03 Japan", {0x11e8060, 0x54a720}, {0x4d1773, 0x4d178c},
     {{0xe8, 0xc8, 0x97, 0xb9, 0xff}, {0xe8, 0xaf, 0x97, 0xb9, 0xff}}, 0x58d170},
};

const uint8_t system_type_set_text_prologue[SYSTEM_TYPE_SET_TEXT_CHECK] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec};

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
    const SystemTypeRecord *record = NULL;
    for (size_t i = 0; i < SYSTEM_TYPE_RECORD_COUNT; ++i) {
        if (system_type_records[i].image.size == size && system_type_records[i].image.entry == entry)
            record = &system_type_records[i];
    }
    if (!record) {
        printf("System type: hook not installed: not an online build\n");
        return 0;
    }
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
