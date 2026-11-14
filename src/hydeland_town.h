/* The town object's every-frame call (DcpEarlyStorySites::town_portal_call): one guest hook shared by every feature that
 * waits for controllable play in Hydeland, and the game's own readiness checks read the same way for all of them. */
#ifndef DCP_HYDELAND_TOWN_H
#define DCP_HYDELAND_TOWN_H

#include <stddef.h>
#include <stdint.h>

#include "feature_sites.h"
#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The guest layouts the town readiness reads, the same on every supported build. */
#define HYDELAND_TOWN_CONTEXT 0x998   /* town object -> context object */
#define HYDELAND_CONTEXT_MANAGER 0x20 /* context object -> game manager */

/* The prompt function's own control checks (game manager fields). */
#define HYDELAND_MANAGER_MODE 0xb8
#define HYDELAND_MANAGER_SCENE 0x88
#define HYDELAND_MANAGER_QUEUE 0x108
#define HYDELAND_OBJECT_STATE 0x5b
#define HYDELAND_MODE_TOWN 3
#define HYDELAND_SCENE_PLAY 2
#define HYDELAND_MODE_INPUT 0x514
#define HYDELAND_MASK_INPUT 4
#define HYDELAND_QUEUE_BEGIN 0x78
#define HYDELAND_QUEUE_END 0x80
#define HYDELAND_PROMPT_MASK_LEA 0x65
#define HYDELAND_PROMPT_CHECKS_SPAN 0xc0

#define HYDELAND_AREA_TYPE 0x108 /* scene object: stage type, 9 in a town */
#define HYDELAND_AREA_LOCATION 0xf0
#define HYDELAND_AREA_TOWN 9
#define HYDELAND_AREA_STREET 0

/* The game manager of a town object; 0 when it has none. */
uint64_t hydeland_town_manager(uint64_t town);
/* The four checks the game's prompt function makes before it accepts input. */
int hydeland_town_accepts_input(uint64_t manager);
/* The manager's scene is Hydeland's street. */
int hydeland_town_is_hydeland(uint64_t manager);

/* The input mask table the checks read (from the prompt function's lea); 0 turns every check off. */
void hydeland_town_set_mask_table(uint64_t table);
int hydeland_town_prompt_checks_match(const uint8_t *image, uint64_t size, uint32_t prompt_function);
uint64_t hydeland_town_mask_table(const uint8_t *image, uint32_t prompt_function);

typedef void(GUEST_HOOK_ABI *HydelandTownObserver)(GuestRegs *regs);
typedef enum { HYDELAND_TOWN_EARLY_STORY, HYDELAND_TOWN_BOSS_BATTLE, HYDELAND_TOWN_OBSERVERS } HydelandTownSlot;
/* Each feature's every-frame town handling, called in slot order; NULL removes it. */
void hydeland_town_observe(HydelandTownSlot slot, HydelandTownObserver observer);

void GUEST_HOOK_ABI hydeland_town_hook(GuestRegs *regs);
/* The shared hook at the call; 0 when the site is not a call. */
int hydeland_town_hook_at(const uint8_t *image, uint64_t size, uint32_t call, GuestHook *hook);

/* The hook is in the image (whoever installed it); a second install would patch the call twice. */
int hydeland_town_installed(void);
void hydeland_town_set_installed(int installed);

#ifdef __cplusplus
}
#endif

#endif
