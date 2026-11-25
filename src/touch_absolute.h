/* Touchscreen pointer: a finger on the screen puts the game's hand pointer right under it and the pointer follows the
 * finger, as on a real touchscreen.
 *
 * The game's own Touch Pad Cursor Control option already has an Absolute position mode, in which the pointer is set from
 * where the finger is on the touch surface instead of being moved by how far it slid. The pointer update reads that option
 * at two calls; while the touchscreen has a contact, the hooks answer those two reads with Absolute position (unless the
 * player chose Disable), and the pad sampler maps the screen point to the touch surface point that this mode puts at that
 * screen point. A DualShock 4 touchpad, and the saved option, stay as they were. */
#ifndef DCP_TOUCH_ABSOLUTE_H
#define DCP_TOUCH_ABSOLUTE_H

#include <stddef.h>
#include <stdint.h>

#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TOUCH_ABSOLUTE_SITES = 2,
    TOUCH_ABSOLUTE_SITE_BYTES = 5, /* the option read: call rel32 */
    TOUCH_ABSOLUTE_MODE_RELATIVE = 0,
    TOUCH_ABSOLUTE_MODE_ABSOLUTE = 1,
    TOUCH_ABSOLUTE_MODE_DISABLED = 4
};

/* Absolute position mode, as the game computes it in screen units around the screen centre:
 * pointer / screen size = gain * (touch / touch resolution - 0.5) + offset. */
typedef struct {
    float gain_x, gain_y, offset_y;
} TouchAbsoluteMap;

typedef struct {
    uint64_t sites[TOUCH_ABSOLUTE_SITES]; /* the two option reads */
    TouchAbsoluteMap map;                 /* read from the game's own constants */
} TouchAbsoluteSites;

/* Finds both option reads and the Absolute position arithmetic, each exactly once in the image, and reads the game's
 * constants. Returns 1 when everything matched; `sites` is cleared otherwise. */
int touch_absolute_locate(const unsigned char *image, uint64_t size, TouchAbsoluteSites *sites);

size_t touch_absolute_stub_bytes(void);

/* Hooks the two reads for a supported build (`game` NULL: none). Returns 1 when installed; writes nothing otherwise. */
int touch_absolute_install(unsigned char *image, uint64_t size, uint64_t entry, const char *game, unsigned char *stubs,
                           size_t stubs_size);

/* The mode the pointer update sees: Absolute position while the touchscreen has a contact, unless the player disabled
 * the touch pad cursor; the saved mode otherwise. */
int touch_absolute_mode(int saved, int contact);

/* The touch surface point (0..DCP_TOUCH_MAX_*) that Absolute position puts at screen point (`x`, `y`), given as touch
 * surface units of the game picture. Clamped to the surface. */
void touch_absolute_map_point(const TouchAbsoluteMap *map, uint16_t x, uint16_t y, uint16_t *out_x, uint16_t *out_y);

/* Runtime. Ready once installed; the pad sampler reports each read of player 1 whether the touchscreen owns the touch
 * surface, and remaps the contact's point while ready. */
int touch_absolute_ready(void);
void touch_absolute_set_contact(int contact);
void touch_absolute_remap(uint16_t *x, uint16_t *y);

/* From guest code at an option read: rdi is the controller's option record. */
void GUEST_HOOK_ABI touch_absolute_read_mode(GuestRegs *regs);

#ifdef __cplusplus
}
#endif

#endif
