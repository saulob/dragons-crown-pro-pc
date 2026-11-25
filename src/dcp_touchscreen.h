/* Touchscreen input for player 1: the window thread forwards SDL finger events here and the pad sampler takes one sample
 * per game read, so a screen touch reaches the game as a DualShock 4 touchpad contact. A short tap also clicks the pad. */
#ifndef DCP_TOUCHSCREEN_H
#define DCP_TOUCHSCREEN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DcpTouchscreen DcpTouchscreen;

/* How a physical touchscreen moves the game's pointer (dcp.ini: touchscreen_mode). PS4 Touchpad is the default and what a
 * missing, empty or unknown value means. */
typedef enum {
    DCP_TOUCHSCREEN_MODE_PS4_TOUCHPAD,         /* dragging moves the pointer, like the DualShock 4 touchpad */
    DCP_TOUCHSCREEN_MODE_PS_VITA_DIRECT_TOUCH, /* the pointer is placed under the finger */
    DCP_TOUCHSCREEN_MODE_COUNT
} DcpTouchscreenMode;

extern const char *const dcp_touchscreen_mode_key;
/* The mode a saved value names; PS4 Touchpad for anything else. */
int dcp_touchscreen_mode_from_value(const char *value);
const char *dcp_touchscreen_mode_name(int mode);
const char *dcp_touchscreen_mode_label(int mode);

/* The presented game picture, as fractions of the window. */
typedef struct {
    float x, y, width, height;
} DcpTouchArea;

typedef struct {
    uint32_t buttons;  /* PS4 button bits to add to the pad's */
    int touch;         /* a contact is on the surface this read */
    uint16_t x, y;
    uint8_t id;
    int new_contact;   /* the first read that carries this contact */
} DcpTouchSample;

typedef enum {
    DCP_TOUCH_EVENT_DETECTED,         /* touchscreen_detected: once per session */
    DCP_TOUCH_EVENT_TOUCHPAD_USED,    /* touchscreen_touchpad_used: each contact handed to the pad */
    DCP_TOUCH_EVENT_MOUSE_SUPPRESSED, /* touchscreen_synthetic_mouse_suppressed: each touch-made mouse press */
    DCP_TOUCH_EVENT_COUNT
} DcpTouchEvent;

typedef struct DcpTouchTelemetry DcpTouchTelemetry;

DcpTouchTelemetry *dcp_touch_telemetry_new(void);
void dcp_touch_telemetry_free(DcpTouchTelemetry *telemetry);
/* Counts the event. Returns whether a log line is due (the first, then every tenth power) and writes it to `line`. */
int dcp_touch_telemetry_note(DcpTouchTelemetry *telemetry, DcpTouchEvent event, char *line, size_t size);
/* The process-wide counters, logged as they are noted. */
void dcp_touch_telemetry_record(DcpTouchEvent event);

DcpTouchscreen *dcp_touchscreen_new(void);
void dcp_touchscreen_free(DcpTouchscreen *state);
DcpTouchscreen *dcp_touchscreen_global(void);

/* A picture of `width` x `height` drawn at (`x`, `y`) on a surface of `surface_width` x `surface_height`; the whole
 * window when any size is unknown. */
DcpTouchArea dcp_touch_area_from_pixels(int x, int y, int width, int height, int surface_width, int surface_height);
/* Maps a window fraction to touch surface units, clamped to the picture. Returns whether the point is on the picture. */
int dcp_touch_point(DcpTouchArea area, float window_x, float window_y, uint16_t *x, uint16_t *y);

/* Window thread. Positions are window fractions; times are SDL event timestamps in nanoseconds. Only the first finger is
 * followed, and only when it lands on the picture. */
void dcp_touchscreen_down(DcpTouchscreen *state, uint64_t finger, DcpTouchArea area, float x, float y, uint64_t time_ns);
void dcp_touchscreen_motion(DcpTouchscreen *state, uint64_t finger, DcpTouchArea area, float x, float y);
void dcp_touchscreen_up(DcpTouchscreen *state, uint64_t finger, DcpTouchArea area, float x, float y, uint64_t time_ns);
void dcp_touchscreen_cancel(DcpTouchscreen *state, uint64_t finger);
/* While blocked (overlay, text entry, focus away) touches are dropped and the contact is cleared. */
void dcp_touchscreen_block(DcpTouchscreen *state, int blocked);
void dcp_touchscreen_release_all(DcpTouchscreen *state);

/* Pad sampler: one call per game read of player 1. */
void dcp_touchscreen_sample(DcpTouchscreen *state, DcpTouchSample *out);

#ifdef __cplusplus
}
#endif

#endif
