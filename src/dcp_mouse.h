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
    int touchpad_side; /* a Touchpad left (1) or right (2) press: the click lands on that side unless the pointer owns the surface */
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
/* Live change of the Pointer Control Mode (DcpPointerMode); anything else is ignored. */
void dcp_mouse_set_pointer_mode(DcpMouseState *state, int mode);
int dcp_mouse_enabled(DcpMouseState *state);

/* Window thread. Movement is in game pixels (see dcp_mouse_game_scale). */
void dcp_mouse_button(DcpMouseState *state, int button, int down);
void dcp_mouse_motion(DcpMouseState *state, float game_dx, float game_dy);
void dcp_mouse_wheel(DcpMouseState *state, float steps);
/* While blocked (overlay, text entry, focus away) presses are dropped and held state is cleared. */
void dcp_mouse_block(DcpMouseState *state, int blocked);
/* Focus loss, a window mode switch, shutdown. */
void dcp_mouse_release_all(DcpMouseState *state);
/* A controller's own touch surface was touched. Hold: Pointer Control ends and stays off until its button is released.
 * Always On: the pointer yields until dcp_mouse_controller_surface reports the surface free. Controller buttons, sticks and
 * triggers never end Pointer Control. */
void dcp_mouse_controller_touch(DcpMouseState *state);
/* Pad sampler, before each sample: whether player 1's controller has a finger on its touch surface, and whether the
 * touchscreen has a contact. Only Always On reads them: while either holds the surface the pointer yields, and it lands
 * again, recentred, once both are free. */
void dcp_mouse_controller_surface(DcpMouseState *state, int touching);
void dcp_mouse_screen_surface(DcpMouseState *state, int touching);
int dcp_mouse_pointer_active(DcpMouseState *state);
/* Whether the window should hold the mouse in relative mode (the OS cursor hidden and unbounded), before the window's own
 * blocking: Hold while the pointer is active, Always On as long as Mouse Controls is on, even while a controller touch has
 * the pointer yield. */
int dcp_mouse_capture_wanted(DcpMouseState *state);
/* Any accepted mouse button is held. */
int dcp_mouse_any_held(DcpMouseState *state);

/* Pad sampler: one call per game read of player 1. */
void dcp_mouse_sample(DcpMouseState *state, DcpMouseSample *out);

/* Game pixels (the 1080-high game picture) per window pixel, for a picture fitted into the window. */
float dcp_mouse_game_scale(int window_width, int window_height);
/* Mouse movement in game pixels to touch surface units, at `percent` sensitivity. */
void dcp_mouse_to_touch_units(int percent, float game_dx, float game_dy, float *touch_dx, float *touch_dy);

/* Telemetry: counts only, never a position, a delta, a device or a text. Each line is logged for the first occurrence and
 * every tenth power after it. */
typedef enum {
    DCP_MOUSE_EVENT_EXTENDED_BINDING_USED,
    DCP_MOUSE_EVENT_ALWAYS_ON_USED,
    DCP_MOUSE_EVENT_POINTER_SUSPENDED,
    DCP_MOUSE_EVENT_POINTER_RESUMED,
    DCP_MOUSE_EVENT_BINDING_INVALID,
    DCP_MOUSE_EVENT_COUNT
} DcpMouseEvent;

typedef enum {
    DCP_MOUSE_REASON_NONE, DCP_MOUSE_REASON_BLOCKED, DCP_MOUSE_REASON_CONTROLLER_TOUCH, DCP_MOUSE_REASON_DISABLED,
    DCP_MOUSE_REASON_COUNT
} DcpMouseReason;

typedef void (*DcpMouseLogSink)(const char *line, void *user);
/* Where a state sends its telemetry lines: the log by default. Lines are delivered outside the state's lock. */
void dcp_mouse_set_log_sink(DcpMouseState *state, DcpMouseLogSink sink, void *user);
/* The saved mouse settings were not all usable: `count` of them were rejected (reported as at most 99). */
void dcp_mouse_note_invalid(DcpMouseState *state, int count);

#ifdef __cplusplus
}
#endif

#endif
