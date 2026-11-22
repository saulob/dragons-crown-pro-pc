/* Kraken tentacle fix: the update reads its mast's broken flag through a handle that is null while no mast exists. The hook
 * answers a null handle with a mast whose flag is clear; a mast that exists is read as before. */
#ifndef DCP_KRAKEN_FIX_H
#define DCP_KRAKEN_FIX_H

#include <stddef.h>
#include <stdint.h>

#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    KRAKEN_FIX_FLAG_OFFSET = 0x38c1,
    KRAKEN_FIX_SITE_BYTES = 7
};

/* The hooked compare for a supported build ("CUSA10454-1.00"; NULL or any other name: none), 0 when its code does not
 * match. The tentacle update must appear in the image exactly once, and where the build's table says when it has one.
 * Returns 1 when the build is supported. */
int kraken_fix_locate(const unsigned char *image, uint64_t size, const char *game, uint64_t *site);

size_t kraken_fix_stub_bytes(void);

/* Hooks the compare; returns 1 when installed. Writes nothing and says why when the code or the stub space does not fit. */
int kraken_fix_install(unsigned char *image, uint64_t size, uint64_t entry, const char *game, unsigned char *stubs,
                       size_t stubs_size);

/* Told the tentacle each time the fix answers a null handle. */
void kraken_fix_set_observer(void (*observer)(uint64_t tentacle));

/* From guest code at the compare: rax is the mast, r15 the tentacle. */
void GUEST_HOOK_ABI kraken_fix_null_mast(GuestRegs *regs);

#ifdef __cplusplus
}
#endif

#endif
