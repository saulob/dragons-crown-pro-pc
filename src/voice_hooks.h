/* Japanese NPC voices on CUSA10454 1.00: the cue mapper's US-only path returns the Japanese cue ID for the NPC voice
 * families, and an extra reference on each of their banks, taken through the game's own resource acquire, keeps a
 * bank already loaded for either language from being evicted once the cue mapper starts asking for the other one.
 * The banks are the user's own; no game file is changed. */
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

#define VOICE_RES_ACQUIRE_SITE_OFFSET 0x57cbb0
#define VOICE_RES_ACQUIRE_SITE_LENGTH 6
extern const uint8_t voice_res_acquire_site[VOICE_RES_ACQUIRE_SITE_LENGTH];
#define VOICE_HOOK_COUNT 2

/* Process-lifetime pin of the Japanese NPC voice banks: one extra reference each, taken through the game's own
 * resource acquire. */
#define VOICE_PIN_BANKS 9
typedef void *(GUEST_HOOK_ABI *ResourceAcquire)(void *manager, const char *name);
const char *voice_pin_bank_name(int index);
int voice_pin_target_index(const char *name, int *english);
int voice_pin_bank(int index, void *manager, ResourceAcquire acquire);
unsigned voice_pinned_mask(void);
void voice_pin_reset(void);

void voice_hooks_set_enabled(int enabled);
int voice_hooks_enabled(void);
void GUEST_HOOK_ABI voice_npc_hook(GuestRegs *regs);
void GUEST_HOOK_ABI voice_res_acquire_hook(GuestRegs *regs);

size_t voice_hooks_stub_bytes(void);

/* 1 when the image is CUSA10454 1.00 and the site bytes match; 0 otherwise, the image untouched. */
int voice_hooks_install(uint8_t *image, uint64_t size, uint64_t entry, uint8_t *stubs, size_t stubs_size);

#ifdef __cplusplus
}
#endif

#endif
