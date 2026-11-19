/* Comms Wheel's side of the game: a message is shown through the game's own greeting balloon of the sender's player, so
 * its position, font, duration and Player 1-4 colour are the game's. The text is the only thing replaced. Installed
 * together or not at all (see comms_bridge_install). */
#ifndef DCP_COMMS_BRIDGE_H
#define DCP_COMMS_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The sites of one build, image offsets. */
typedef struct {
    uint32_t show;       /* the greeting balloon function: rdi = the player's owner object */
    uint32_t text_call;  /* the call inside it that copies the message text into the balloon string */
} CommsBridgeSites;

typedef void(GUEST_HOOK_ABI *CommsBridgeShowFn)(uint64_t owner);

/* 1 when the greeting function and its text call are found exactly once with the verified bytes. */
int comms_bridge_locate(const uint8_t *image, uint64_t size, CommsBridgeSites *sites);

size_t comms_bridge_stub_bytes(void);

/* Takes the balloon function directly (tests); `clock` returns milliseconds, NULL for the monotonic clock. */
void comms_bridge_attach(CommsBridgeShowFn show, uint64_t (*clock)(void));

/* The hook at the text call: while the bridge's own call runs, rsi becomes the message text. */
void GUEST_HOOK_ABI comms_bridge_text_hook(GuestRegs *regs);

/* Installs the text hook and takes the balloon function; 1 when the bridge can show messages. */
int comms_bridge_install(uint8_t *image, uint64_t size, uint64_t entry, const CommsBridgeSites *sites, uint8_t *stubs,
                         size_t stubs_size);

/* 1 once the bridge is installed. */
int comms_bridge_active(void);

/* Any thread: the balloon of party slot 0-3 shows message `id` the next time that player's unit runs. Dropped when the
 * game does not show it within a few seconds. */
void comms_bridge_show(int slot, int id);

/* 1 when the unit of party slot 0-3 ran within the last half second: the game is playing, not in a menu, a load or the
 * title. */
int comms_bridge_unit_running(int slot);

/* Game thread, from the player unit's per-tick update (party_hook.cpp): shows what is queued for that unit's slot. */
void comms_bridge_unit_tick(uint64_t unit);

#ifdef __cplusplus
}
#endif

#endif
