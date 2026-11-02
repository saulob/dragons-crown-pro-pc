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

/* 1 when the located site and the game's displacements are the handler's and the hook is in; 0 otherwise, the image
 * untouched. */
int party_hook_install_located(uint8_t *image, uint64_t size, uint64_t entry, const DcpPartySites *sites, uint8_t *stubs,
                               size_t stubs_size);

#ifdef __cplusplus
}
#endif

#endif
