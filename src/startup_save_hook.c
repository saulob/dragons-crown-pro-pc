#include "startup_save_hook.h"

#include <stdio.h>
#include <string.h>

enum {
    BLOCK_OWNER_OFFSET = 0xe8,
    SCENE_CATEGORY_OFFSET = 0x70,
    CATEGORY_OK = 0,
    CATEGORY_NOT_FOUND = 3,
    KIND_STARTUP = 1,
    STEP_ASK = 1,
    STEP_CREATE = 3,
    STEP_TITLE = 4
};

#define SET_STEP_TAIL {0x48, 0x8b, 0x80, 0xa0, 0x00, 0x00, 0x00, 0x4c, 0x89, 0xf7} /* mov rax,[rax+0xa0]; mov rdi,r14 */
#define BLOCK_PROLOGUE {0x48, 0x8d, 0x87, 0x07, 0xeb, 0x20, 0x00, 0xc3}
#define ACCEPT_PROLOGUE {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x49}

const StartupSaveRecord startup_save_records[STARTUP_SAVE_RECORD_COUNT] = {
    {"CUSA07001 1.00 Japan", {0x11e8060, 0x54a800}, 0x4dbb00, SET_STEP_TAIL, 0x812c0, BLOCK_PROLOGUE, 0x8b230, ACCEPT_PROLOGUE},
    {"CUSA07001 1.02 Japan", {0x11e8060, 0x54a660}, 0x4dc060, SET_STEP_TAIL, 0x813a0, BLOCK_PROLOGUE, 0x8b4d0, ACCEPT_PROLOGUE},
    {"CUSA07001 1.03 Japan", {0x11e8060, 0x54a720}, 0x4dc120, SET_STEP_TAIL, 0x813a0, BLOCK_PROLOGUE, 0x8b4d0, ACCEPT_PROLOGUE},
};

static StartupSaveBlockFn block_fn;
static StartupSaveAcceptFn accept_fn;
static int created_by_guest;

void startup_save_set_functions(StartupSaveBlockFn block, StartupSaveAcceptFn accept) {
    block_fn = block;
    accept_fn = accept;
    created_by_guest = 0;
}

static int32_t load_int32(uint64_t at) {
    int32_t value;
    memcpy(&value, (const void *)(uintptr_t)at, sizeof(value));
    return value;
}

/* At the tail that sets the startup screen's next step (rsi = kind, rdx = step, r12 = the controller, r15 = the save
 * scene). A load that found no save goes to the guest's create step; once that finishes, the successful-load
 * continuation runs before the step to the title. */
void GUEST_HOOK_ABI startup_save_step_hook(GuestRegs *regs) {
    if ((uint32_t)regs->rsi != KIND_STARTUP || !regs->r15 || !block_fn || !accept_fn) return;
    const int32_t category = load_int32(regs->r15 + SCENE_CATEGORY_OFFSET);
    if ((uint32_t)regs->rdx == STEP_ASK && category == CATEGORY_NOT_FOUND) {
        printf("Save data: no save found, the game creates one\n");
        regs->rdx = STEP_CREATE;
        created_by_guest = 1;
    } else if ((uint32_t)regs->rdx == STEP_TITLE && created_by_guest) {
        created_by_guest = 0;
        if (category != CATEGORY_OK) return;
        uint64_t owner;
        memcpy(&owner, (const void *)(uintptr_t)(regs->r12 + BLOCK_OWNER_OFFSET), sizeof(owner));
        accept_fn(block_fn(owner), regs->r12);
    }
}

size_t startup_save_hook_stub_bytes(void) {
    return guest_hook_stub_size();
}

int startup_save_hook_install(uint8_t *image, uint64_t size, uint64_t entry, uint8_t *stubs, size_t stubs_size) {
    for (size_t i = 0; i < STARTUP_SAVE_RECORD_COUNT; ++i) {
        const StartupSaveRecord *record = &startup_save_records[i];
        if (size != record->image.size || entry != record->image.entry) continue;
        if ((uint64_t)record->save_offset + STARTUP_SAVE_PROLOGUE_LENGTH > size ||
            (uint64_t)record->accept_offset + STARTUP_SAVE_PROLOGUE_LENGTH > size ||
            memcmp(image + record->save_offset, record->save_prologue, STARTUP_SAVE_PROLOGUE_LENGTH) ||
            memcmp(image + record->accept_offset, record->accept_prologue, STARTUP_SAVE_PROLOGUE_LENGTH)) {
            printf("Save data: startup save hook not installed, unexpected continuation bytes\n");
            return 0;
        }
        GuestHook hook = {record->build, record->step_offset, STARTUP_SAVE_SITE_LENGTH, {0}, startup_save_step_hook};
        memcpy(hook.original, record->step_original, STARTUP_SAVE_SITE_LENGTH);
        size_t bad;
        const GuestHookStatus status = guest_hooks_install(image, size, entry, &record->image, &hook, 1, stubs, stubs_size, &bad);
        if (status != GUEST_HOOK_OK) {
            printf("Save data: startup save hook not installed, %s\n", guest_hook_status_text(status));
            return 0;
        }
        startup_save_set_functions((StartupSaveBlockFn)(uintptr_t)(image + record->save_offset),
                                   (StartupSaveAcceptFn)(uintptr_t)(image + record->accept_offset));
        printf("Save data: startup save hook installed for %s\n", record->build);
        return 1;
    }
    return 0;
}
