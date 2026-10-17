/* The cheat state shared by the in-game menu (video core) and the game hooks. */
#ifndef DCP_RUNTIME_CHEATS_H
#define DCP_RUNTIME_CHEATS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called once, after the launcher: `saved_mask` is dcp_saved_cheats() and `experience_multiplier` is
 * dcp_saved_experience_multiplier(), the start of this game session. Starting clears every override the menu made. */
void runtime_cheats_start(unsigned saved_mask, int experience_multiplier);

/* The Experience Multiplier this session (1 = Off, 2, 5 or 10): the saved value, or the menu's change of it. */
int runtime_cheat_experience_multiplier(void);

/* The F10 menu's Experience Multiplier: moves to the next value for this session and returns it. */
int runtime_cheat_experience_cycle(void);

/* 1 when the cheat (a dcp::cheats::Cheat value) is on: the saved value, or the menu's override of it this session. */
int runtime_cheat_enabled(int cheat);

/* 1 once after a cheat that fills a resource (Infinite Health, Mana, Durability) turned On, from the launcher's value at
 * the start or from the menu; the game thread asks when it holds Player 1's unit and fills it when this says 1. */
int runtime_cheat_take_refill(int cheat);

/* One bit per cheat (dcp_cheats.h Bit) whose refill request is waiting; cheap enough for a per-tick hook. */
unsigned runtime_cheat_refills_pending(void);

/* The in-game menu's one-shot actions (dcp::cheats::Action). A request waits for the game thread, which takes it once
 * and runs it on Player 1's unit; leaving the menu drops what has not run. */
void runtime_cheat_action(int action);
int runtime_cheat_action_pending(int action);
int runtime_cheat_take_action(int action);
void runtime_cheat_clear_actions(void);

/* The in-game menu's toggle. Only this session changes; nothing is written to the settings file. */
void runtime_cheat_toggle(int cheat);

/* The native hooks the cheats need: the room for their stubs, and the installation on the loaded image (before its
 * relocations are made executable). 0 when the image is not the supported v1.03: the game then runs without cheats. */
size_t runtime_cheat_hooks_stub_bytes(void);
int runtime_cheat_hooks_install(uint8_t *image, uint64_t size, uint64_t entry, uint8_t *stubs, size_t stubs_size);

/* 1 when the cheat hooks are installed on the loaded image; the in-game menu offers cheats only then. */
int runtime_cheats_available(void);
void runtime_cheats_set_available(int available);

#ifdef __cplusplus
}
#endif

#endif
