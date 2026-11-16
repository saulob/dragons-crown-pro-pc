/* The game's own "Go online?" prompt run from a stand-in owner, and the game's online state: shared by Early Story
 * Online (Hydeland) and Boss Battle Online (the Inn). */
#ifndef DCP_ONLINE_PROMPT_H
#define DCP_ONLINE_PROMPT_H

#include <stdint.h>

#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ONLINE_PROMPT_OWNER_SIZE 0xb00   /* the prompt owner objects are 0xa78 bytes */
#define ONLINE_PROMPT_OWNER_SYSOBJ 0x1e0 /* owner -> the system object it belongs to */
#define ONLINE_PROMPT_OWNER_FLAGS 0x50   /* owner: slot change flags, 0x800 << slot */
#define ONLINE_PROMPT_OWNER_STATES 0x5b  /* owner: one state byte per slot */
#define ONLINE_PROMPT_SLOT 2
#define ONLINE_PROMPT_SLOT1_CHANGED 0x1000
#define ONLINE_PROMPT_RUN_FLAG 0x14d     /* game manager: set once the prompt has been shown this run */

/* The game manager's online interface: [[manager + 0x78] + 0x16910], whose vtable answers is-online at +0x248. */
#define ONLINE_MANAGER_GAME 0x78
#define ONLINE_GAME_NET 0x16910
#define ONLINE_NET_IS_ONLINE 0x248

/* The game's prompt function: the owner object and the slot whose state it runs, 1 once it is finished. */
typedef uint8_t(GUEST_HOOK_ABI *OnlinePromptFn)(uint64_t owner, int slot);
typedef uint8_t(GUEST_HOOK_ABI *OnlineNetQueryFn)(uint64_t net);

typedef struct {
    _Alignas(16) uint8_t owner[ONLINE_PROMPT_OWNER_SIZE];
    uint64_t system_object;
    int open, settled;
} OnlinePrompt;

typedef enum { ONLINE_PROMPT_RUNNING, ONLINE_PROMPT_SHOWN, ONLINE_PROMPT_FINISHED } OnlinePromptStep;

void online_prompt_configure(OnlinePromptFn prompt, uint64_t owner_vtable);
int online_prompt_configured(void);
/* A fresh owner for `system_object`, and its first frame. */
OnlinePromptStep online_prompt_start(OnlinePrompt *prompt, uint64_t system_object);
/* One more frame of a started prompt. */
OnlinePromptStep online_prompt_step(OnlinePrompt *prompt);
void online_prompt_drop(OnlinePrompt *prompt);

/* The game's own online state for a game manager: 1 online, 0 offline, -1 unknown. */
int online_native_online(uint64_t manager);

#ifdef __cplusplus
}
#endif

#endif
