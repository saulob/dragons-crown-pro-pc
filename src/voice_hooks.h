/* Japanese NPC voices on CUSA10454 1.00: the cue mapper's US-only path returns the Japanese cue ID for the six NPC
 * voice families. The banks are the user's own; no game file is changed. */
#ifndef DCP_VOICE_HOOKS_H
#define DCP_VOICE_HOOKS_H

#include <stddef.h>
#include <stdint.h>

#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_NPC_SITE_OFFSET 0x68bc9
#define VOICE_NPC_SITE_LENGTH 5
extern const uint8_t voice_npc_site[VOICE_NPC_SITE_LENGTH];

void voice_hooks_set_enabled(int enabled);
int voice_hooks_enabled(void);
void GUEST_HOOK_ABI voice_npc_hook(GuestRegs *regs);

size_t voice_hooks_stub_bytes(void);

/* 1 when the image is CUSA10454 1.00 and the site bytes match; 0 otherwise, the image untouched. */
int voice_hooks_install(uint8_t *image, uint64_t size, uint64_t entry, uint8_t *stubs, size_t stubs_size);

#ifdef __cplusplus
}
#endif

#endif
