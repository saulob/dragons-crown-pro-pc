/* Mouse input for player 1: the window thread forwards SDL mouse events here and the pad sampler takes one sample per game
 * read, so the game (and online play) sees ordinary buttons and an ordinary touch finger.
 *
 * Pointer Control moves one synthetic finger. The game moves its pointer by the difference between a finger's positions on
 * two consecutive reads, and only when a finger was there on both; a finger that lifts for one read and lands elsewhere
 * moves nothing, which is how the finger is recentred. */
#ifndef DCP_MOUSE_H
#define DCP_MOUSE_H

#include <stdint.h>
#include "dcp_keys.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DcpMouseState DcpMouseState;

typedef struct {
    uint32_t buttons;  /* PS4 button bits to add to the pad's */
    int touch;         /* a synthetic finger is on the surface this read */
    uint16_t x, y;
    uint8_t id;
    int pointer;       /* Pointer Control owns the touch surface for this read */
} DcpMouseSample;

DcpMouseState *dcp_mouse_state_new(void);
void dcp_mouse_state_free(DcpMouseState *state);
/* The process-wide state the window thread and the pad sampler share. */
DcpMouseState *dcp_mouse_global(void);

void dcp_mouse_configure(DcpMouseState *state, const DcpMouseMap *map);
/* Mouse Controls: turning it off drops every held button, the pointer and the finger; the mouse is then ignored. */
void dcp_mouse_set_enabled(DcpMouseState *state, int enabled);
/* Live change of the sensitivity percent; normalized to a step. */
void dcp_mouse_set_sensitivity(DcpMouseState *state, int percent);
int dcp_mouse_enabled(DcpMouseState *state);

/* Window thread. Movement is in game pixels (see dcp_mouse_game_scale). */
void dcp_mouse_button(DcpMouseState *state, int button, int down);
void dcp_mouse_motion(DcpMouseState *state, float game_dx, float game_dy);
void dcp_mouse_wheel(DcpMouseState *state, float steps);
/* While blocked (overlay, text entry, focus away) presses are dropped and held state is cleared. */
void dcp_mouse_block(DcpMouseState *state, int blocked);
/* Focus loss, a window mode switch, shutdown. */
void dcp_mouse_release_all(DcpMouseState *state);
/* A controller's own touch surface was touched: it takes the surface, so Pointer Control ends and stays off until its
 * button is released. Controller buttons, sticks and triggers never end Pointer Control. */
void dcp_mouse_controller_touch(DcpMouseState *state);
int dcp_mouse_pointer_active(DcpMouseState *state);
/* Any accepted mouse button is held. */
int dcp_mouse_any_held(DcpMouseState *state);

/* Pad sampler: one call per game read of player 1. */
void dcp_mouse_sample(DcpMouseState *state, DcpMouseSample *out);

/* Game pixels (the 1080-high game picture) per window pixel, for a picture fitted into the window. */
float dcp_mouse_game_scale(int window_width, int window_height);
/* Mouse movement in game pixels to touch surface units, at `percent` sensitivity. */
void dcp_mouse_to_touch_units(int percent, float game_dx, float game_dy, float *touch_dx, float *touch_dy);

#ifdef __cplusplus
}
#endif

#endif
