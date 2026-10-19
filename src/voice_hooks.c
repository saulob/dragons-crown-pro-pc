#include "voice_hooks.h"

#include <stdio.h>
#include <string.h>

#include "dcp_voice.h"

const uint8_t voice_npc_site[VOICE_NPC_SITE_LENGTH] = {0xb8, 0x1b, 0x97, 0x01, 0x00};

static const GuestImageId kImage = {0x11f8060, 0x54c690};
static int enabled;

void voice_hooks_set_enabled(int value) {
    enabled = value != 0;
}

int voice_hooks_enabled(void) {
    return enabled;
}

/* On the US-only path of the cue mapper, before `mov eax, 0x1971b`: returning the cue unchanged and skipping the rest
 * of the mapper leaves the NPC cue on its Japanese ID. */
void GUEST_HOOK_ABI voice_npc_hook(GuestRegs *regs) {
    if (!enabled || !dcp_is_japanese_npc_cue((uint32_t)regs->rdi)) return;
    regs->rax = regs->rdi;
    regs->skip = 1;
}

size_t voice_hooks_stub_bytes(void) {
    return guest_hook_stub_size();
}

int voice_hooks_install(uint8_t *image, uint64_t size, uint64_t entry, uint8_t *stubs, size_t stubs_size) {
    if (size != kImage.size || entry != kImage.entry) {
        printf("Japanese NPC Voices: hook not installed: not the CUSA10454 1.00 image\n");
        return 0;
    }
    GuestHook hook = {"japanese npc voices", VOICE_NPC_SITE_OFFSET, VOICE_NPC_SITE_LENGTH, {0}, voice_npc_hook};
    memcpy(hook.original, voice_npc_site, sizeof(voice_npc_site));
    size_t bad = 0;
    const GuestHookStatus status = guest_hooks_install(image, size, entry, &kImage, &hook, 1, stubs, stubs_size, &bad);
    if (status != GUEST_HOOK_OK) {
        printf("Japanese NPC Voices: hook not installed: %s\n", guest_hook_status_text(status));
        return 0;
    }
    printf("Japanese NPC Voices: hook installed at 0x68bc9\n");
    return 1;
}
