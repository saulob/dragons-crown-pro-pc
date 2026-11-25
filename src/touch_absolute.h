/* PS Vita Direct Touch: a physical touchscreen puts the game's hand pointer under the finger and the pointer follows it.
 *
 * The game's own Touch Pad Cursor Control option has an Absolute position mode, where the pointer is set from where the
 * finger is on the touch surface. The pointer update reads that option at two calls; the hooks answer Absolute position
 * there, unless the option is Disable, for the one controller port whose pad carries the touchscreen contact, and the pad
 * sampler maps the screen point to the surface point that mode puts under the finger. The saved option is never written,
 * and every other port and controller touchpad keeps its own mode. */
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

/* Absolute position as the game computes it around the screen centre:
 * pointer / screen size = gain * (touch / touch resolution - 0.5) + offset. */
typedef struct {
    float gain_x, gain_y, offset_y;
} TouchAbsoluteMap;

typedef struct {
    uint64_t sites[TOUCH_ABSOLUTE_SITES]; /* the two option reads */
    uint64_t engine;                      /* the pointer behind the game's controller ports */
    TouchAbsoluteMap map;                 /* read from the game's own constants */
} TouchAbsoluteSites;

/* Finds both option reads, the Absolute position arithmetic and the controller port path from a read to its pad handle,
 * each exactly once in the image, and reads the game's constants. Returns 1 when everything matched; `sites` is cleared
 * otherwise. */
int touch_absolute_locate(const unsigned char *image, uint64_t size, TouchAbsoluteSites *sites);

size_t touch_absolute_stub_bytes(void);

/* Hooks the two reads of a supported build (`game` NULL: none). Returns 1 when installed. Otherwise nothing is written,
 * one line says why and the touchscreen stays a PS4 Touchpad. */
int touch_absolute_install(unsigned char *image, uint64_t size, uint64_t entry, const char *game, unsigned char *stubs,
                           size_t stubs_size);

/* The mode a port's pointer update sees: Absolute position while that port owns the touchscreen contact, unless its touch
 * pad cursor is disabled; the saved mode otherwise. */
int touch_absolute_mode(int saved, int owns_contact);

/* The touch surface point (0..DCP_TOUCH_MAX_*) that Absolute position puts at the screen point (`x`, `y`), given in touch
 * surface units of the game picture. Clamped to the surface. */
void touch_absolute_map_point(const TouchAbsoluteMap *map, uint16_t x, uint16_t y, uint16_t *out_x, uint16_t *out_y);

/* Ready once installed. The pad sampler names the pad handle whose sample carries the touchscreen contact (0: none) and
 * remaps that contact's point. A handle no game port has open gets neither the mode nor the remap. */
int touch_absolute_ready(void);
void touch_absolute_set_contact(int32_t pad_handle);
int touch_absolute_port_owns_contact(uint32_t port);
void touch_absolute_remap(int32_t pad_handle, uint16_t *x, uint16_t *y);

/* From guest code at an option read: rdi is the port's option record and rsi the port. */
void GUEST_HOOK_ABI touch_absolute_read_mode(GuestRegs *regs);

#ifdef __cplusplus
}
#endif

#endif
