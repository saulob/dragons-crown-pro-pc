/* Unlock Gallery: the Gallery lists 57 Treasure Art pictures and reads which ones the save holds through a getter that
 * returns the save's 59 byte art array (0 locked, 1 new, 2 seen). With the setting on, only the call that builds the
 * Gallery list is answered with a temporary copy of that array in which every picture is at least seen. The save, the
 * unlock writers, the trophy count and the title screen's own reads keep the real array. */
#ifndef DCP_GALLERY_UNLOCK_H
#define DCP_GALLERY_UNLOCK_H

#include <stddef.h>
#include <stdint.h>

#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GALLERY_UNLOCK_ENTRIES = 57,
    GALLERY_UNLOCK_FLAG_BYTES = 59,
    GALLERY_UNLOCK_SAVE_OFFSET = 0x20eacc
};

/* The saved choice (0/1) with DCP_UNLOCK_GALLERY (0/1, off/on) taking precedence. */
int gallery_unlock_wanted(int saved);

/* The array index of a picture (0..56), or -1 for anything else. */
int gallery_unlock_entry_key(int entry);

/* view = flags with every picture shown: new stays new, anything else (locked, or a value the game never writes) becomes
 * seen; both GALLERY_UNLOCK_FLAG_BYTES long. Other bytes are copied. */
void gallery_unlock_fill_view(unsigned char *view, const unsigned char *flags);

/* The hooked getter call for a build ("CUSA10454-1.00"; NULL: no build), 0 when its code does not match. Returns 1 when
 * the build is listed; any other build is searched and must match exactly once. */
int gallery_unlock_locate(const unsigned char *image, uint64_t size, const char *game, uint64_t *site);

size_t gallery_unlock_stub_bytes(void);

/* Hooks the list's getter call when the setting is on; returns 1 when installed. Writes nothing and says why when the
 * code or the stub space does not fit. */
int gallery_unlock_install(unsigned char *image, uint64_t size, uint64_t entry, const char *game, int wanted,
                           unsigned char *stubs, size_t stubs_size);

/* From guest code at the replaced call: rdi is the save, rax becomes the view. */
void GUEST_HOOK_ABI gallery_unlock_view(GuestRegs *regs);

#ifdef __cplusplus
}
#endif

#endif
