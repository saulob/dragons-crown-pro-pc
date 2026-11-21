/* Voice Language: every spoken voice of the game reaches its cue mapper with a mode that picks the Japanese cue or its
 * English twin, and the narrator manager loads one narrator bank chosen from a table. The hooks set that mode at each
 * of the mapper's callers and point the narrator at the same narrator's bank in the chosen language. An extra
 * reference on the chosen language's NPC and enemy banks, taken through the game's own resource acquire, keeps them
 * from being evicted. The banks are the user's own; no game file and no save is changed. */
#ifndef DCP_VOICE_HOOKS_H
#define DCP_VOICE_HOOKS_H

#include <stddef.h>
#include <stdint.h>

#include "dcp_voice.h"
#include "feature_sites.h"
#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_HOOK_COUNT (DCP_VOICE_CUE_CALLS + 4)
#define VOICE_RES_ACQUIRE_SITE_LENGTH 6
#define VOICE_NARRATOR_BANK_SITE_LENGTH 9
#define VOICE_NARRATOR_BASE_SITE_LENGTH 8
#define VOICE_NARRATOR_HELPER_SITE_LENGTH 7
extern const uint8_t voice_res_acquire_site[VOICE_RES_ACQUIRE_SITE_LENGTH];
extern const uint8_t voice_narrator_bank_site[VOICE_NARRATOR_BANK_SITE_LENGTH];
extern const uint8_t voice_narrator_base_site[VOICE_NARRATOR_BASE_SITE_LENGTH];
extern const uint8_t voice_narrator_helper_site[VOICE_NARRATOR_HELPER_SITE_LENGTH];

/* `language` is a DcpVoiceLanguage; `available` the DCP_VOICE_CATEGORY_BIT categories whose banks in that language the
 * installed archive holds. Game Default routes nothing. */
void voice_hooks_configure(int language, unsigned available);
int voice_hooks_language(void);
/* The categories the hooks route: configured, available and, once installed, with their sites in place. */
unsigned voice_hooks_routed(void);

void GUEST_HOOK_ABI voice_cue_call_hook(GuestRegs *regs);
void GUEST_HOOK_ABI voice_res_acquire_hook(GuestRegs *regs);
void GUEST_HOOK_ABI voice_narrator_bank_hook(GuestRegs *regs);
void GUEST_HOOK_ABI voice_narrator_base_hook(GuestRegs *regs);

/* The narrator bank table's numbers in table order; 0 unless every narrator has its other language in the table. */
int voice_narrator_prepare(const uint32_t numbers[DCP_NARRATOR_BANKS]);
/* The table index the narrator at `index` is loaded from in the configured language; -1 when nothing is prepared. */
int voice_narrator_target(int index);

/* Process-lifetime pin of the chosen language's NPC and enemy banks: one extra reference each. */
#define VOICE_PIN_NPC_BANKS 9 /* the NPC families first; the enemy families follow */
#define VOICE_PIN_BANKS 49
typedef void *(GUEST_HOOK_ABI *ResourceAcquire)(void *manager, const char *name);
const char *voice_pin_bank_name(int index, int english);
int voice_pin_target_index(const char *name, int *english);
int voice_pin_bank(int index, void *manager, ResourceAcquire acquire);
uint64_t voice_pinned_mask(void);
void voice_pin_reset(void);

/* How many routed voices of `category` the telemetry has counted; the counters restart with voice_telemetry_reset. */
uint32_t voice_telemetry_count(int category);
void voice_telemetry_reset(void);
/* The startup "Voice telemetry: route_ready" line for the build key ("CUSA10454-1.00") and the requested language. */
void voice_hooks_log_ready(const char *build_key, int requested);

size_t voice_hooks_stub_bytes(void);

/* Installs every hook at the located sites; 1 when all are in place, 0 with the image untouched otherwise. */
int voice_hooks_install(uint8_t *image, uint64_t size, uint64_t entry, const DcpVoiceSites *sites, uint8_t *stubs,
                        size_t stubs_size);

#ifdef __cplusplus
}
#endif

#endif
