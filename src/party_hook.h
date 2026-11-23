/* Online party snapshots: an observe-only hook at the entry of the player unit's per-tick update, installed only while
 * the game is online. It reads each human player's slot and class (dcp_party.h) and changes nothing in the game. */
#ifndef DCP_PARTY_HOOK_H
#define DCP_PARTY_HOOK_H

#include <stddef.h>
#include <stdint.h>

#include "feature_sites.h"

#ifdef __cplusplus
extern "C" {
#endif

size_t party_hook_stub_bytes(void);

/* Records a player unit's slot, class and level (dcp_party.h) without the hook: for the cheats' tick, which patches the
 * same entry offline. */
void party_hook_observe_unit(uint64_t unit);

/* Consumers of every observed player unit, called on the game thread after it is recorded, in registration order: the
 * Comms Wheel shows its balloons from it and Character Highlight selects its seats. A fixed small list: adding a
 * registered listener or a null one changes nothing and returns 0; adding to a full list returns 0. */
#define PARTY_HOOK_LISTENERS 4
int party_hook_add_unit_listener(void (*listener)(uint64_t unit));
void party_hook_remove_unit_listener(void (*listener)(uint64_t unit));
void party_hook_clear_unit_listeners(void);
void party_hook_notify_listeners(uint64_t unit);

/* 1 when the located site and the game's displacements are the handler's and the hook is in; 0 otherwise, the image
 * untouched. */
int party_hook_install_located(uint8_t *image, uint64_t size, uint64_t entry, const DcpPartySites *sites, uint8_t *stubs,
                               size_t stubs_size);

#ifdef __cplusplus
}
#endif

#endif
