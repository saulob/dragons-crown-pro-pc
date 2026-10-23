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

/* DEBUG_SAULO: log-only sites, installed whether or not the option is on. */
#define VOICE_PLAY_SITE_OFFSET 0x5837f0
#define VOICE_PLAY_SITE_LENGTH 6
extern const uint8_t voice_play_site[VOICE_PLAY_SITE_LENGTH];
#define VOICE_RELEASE_SITE_OFFSET 0x5836d8
#define VOICE_RELEASE_SITE_LENGTH 7
extern const uint8_t voice_release_site[VOICE_RELEASE_SITE_LENGTH];
/* DEBUG_SAULO: voice bank list add/remove and destruction, log-only. */
#define VOICE_BANK_PUSH_SITE_OFFSET 0x582420
#define VOICE_BANK_PUSH_SITE_LENGTH 6
extern const uint8_t voice_bank_push_site[VOICE_BANK_PUSH_SITE_LENGTH];
#define VOICE_BANK_ERASE_SITE_OFFSET 0x582610
#define VOICE_BANK_ERASE_SITE_LENGTH 7
extern const uint8_t voice_bank_erase_site[VOICE_BANK_ERASE_SITE_LENGTH];
#define VOICE_BANK_DTOR_SITE_OFFSET 0x582570
#define VOICE_BANK_DTOR_SITE_LENGTH 6
extern const uint8_t voice_bank_dtor_site[VOICE_BANK_DTOR_SITE_LENGTH];
#define VOICE_BANK_DELETE_SITE_OFFSET 0x582760
#define VOICE_BANK_DELETE_SITE_LENGTH 6
extern const uint8_t voice_bank_delete_site[VOICE_BANK_DELETE_SITE_LENGTH];
/* DEBUG_SAULO: resource acquire and release, log-only. */
#define VOICE_RES_ACQUIRE_SITE_OFFSET 0x57cbb0
#define VOICE_RES_ACQUIRE_SITE_LENGTH 6
extern const uint8_t voice_res_acquire_site[VOICE_RES_ACQUIRE_SITE_LENGTH];
#define VOICE_RES_RELEASE_SITE_OFFSET 0x57d159
#define VOICE_RES_RELEASE_SITE_LENGTH 8
extern const uint8_t voice_res_release_site[VOICE_RES_RELEASE_SITE_LENGTH];
#define VOICE_RES_RELEASE_PTR_SITE_OFFSET 0x57d228
#define VOICE_RES_RELEASE_PTR_SITE_LENGTH 8
extern const uint8_t voice_res_release_ptr_site[VOICE_RES_RELEASE_PTR_SITE_LENGTH];
#define VOICE_HOOK_COUNT 10

/* DEBUG_SAULO */
typedef int(GUEST_HOOK_ABI *CueExists)(const void *acb, uint32_t cue);
enum { kSheetFound, kSheetNone, kSheetNoImage, kSheetNoManager, kSheetBadList };
typedef struct {
    int state;
    uint64_t count;
    uint64_t index;
    const void *handle;
    const char *name;
    uint64_t name_size;
} CueSheet;
CueSheet voice_resolve_cue_sheet(const uint8_t *manager, uint32_t cue, CueExists exists);

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
void GUEST_HOOK_ABI voice_play_hook(GuestRegs *regs);
void GUEST_HOOK_ABI voice_release_hook(GuestRegs *regs);
/* DEBUG_SAULO */
void GUEST_HOOK_ABI voice_bank_push_hook(GuestRegs *regs);
void GUEST_HOOK_ABI voice_bank_erase_hook(GuestRegs *regs);
void GUEST_HOOK_ABI voice_bank_dtor_hook(GuestRegs *regs);
void GUEST_HOOK_ABI voice_bank_delete_hook(GuestRegs *regs);
void GUEST_HOOK_ABI voice_res_acquire_hook(GuestRegs *regs);
void GUEST_HOOK_ABI voice_res_release_hook(GuestRegs *regs);
void GUEST_HOOK_ABI voice_res_release_ptr_hook(GuestRegs *regs);
int voice_bank_index(const uint8_t *manager, const uint8_t *bank);

size_t voice_hooks_stub_bytes(void);

/* 1 when the image is CUSA10454 1.00 and the site bytes match; 0 otherwise, the image untouched. */
int voice_hooks_install(uint8_t *image, uint64_t size, uint64_t entry, uint8_t *stubs, size_t stubs_size);

#ifdef __cplusplus
}
#endif

#endif
