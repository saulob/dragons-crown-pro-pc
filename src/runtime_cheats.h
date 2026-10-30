/* The cheat state shared by the in-game menu (video core) and the game hooks. */
#ifndef DCP_RUNTIME_CHEATS_H
#define DCP_RUNTIME_CHEATS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called once, after the launcher: `saved_mask` is dcp_saved_cheats(), `experience_multiplier` is
 * dcp_saved_experience_multiplier() and `movement_speed` is dcp_saved_movement_speed(), the start of this game session.
 * Starting clears every override the menu made. */
#ifdef __cplusplus
void runtime_cheats_start(unsigned saved_mask, int experience_multiplier, int movement_speed = 1);
#else
void runtime_cheats_start(unsigned saved_mask, int experience_multiplier, int movement_speed);
#endif

/* Online Play on (runtime_online_play_enabled) locks every cheat Off for the whole process: saved values, the menu,
 * refills and actions. Set before runtime_cheats_start; turning it on also clears what is already set. */
void runtime_cheats_set_online(int online);
int runtime_cheats_online_locked(void);

/* The Experience Multiplier this session (1 = Off, 2, 5 or 10): the saved value, or the menu's change of it. */
int runtime_cheat_experience_multiplier(void);

/* The F10 menu's Experience Multiplier: moves to the next value for this session and returns it. */
int runtime_cheat_experience_cycle(void);

/* Movement Speed this session (1 = Off, 2 or 4): the saved value, or the menu's change of it. */
int runtime_cheat_movement_speed(void);

/* The F10 menu's Movement Speed: moves to the next value for this session and returns it. */
int runtime_cheat_movement_cycle(void);

/* The port's live controller state: 1 for Player 1 or a slot with a gamepad. The loader sets this. */
void runtime_cheats_set_slot_human(int (*is_human)(int slot));
int runtime_cheat_slot_human(int slot);

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

/* What the log says about the cheats, once the saved settings are in effect; build_has_hooks is the build's native
 * cheat support. Online Play wins on every build. */
#define RUNTIME_CHEATS_ONLINE_LOCKED 0 /* "Cheats: disabled for Online Play" */
#define RUNTIME_CHEATS_UNAVAILABLE 1   /* "Cheats: enabled: none": the build has no cheat hooks */
#define RUNTIME_CHEATS_LISTED 2        /* the enabled cheats, Experience Multiplier and Movement Speed */
int runtime_cheats_log_state(int build_has_hooks);

/* 1 when the native cheat hooks go in: the build has them and Online Play is off. */
int runtime_cheats_hooks_wanted(int build_has_hooks);

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
