/* Session telemetry for sustained pointer motion (mouse or right stick) and whether the game keeps reading it while a
 * button is held. Lines carry only an event name, a source class, a held class, a coarse bucket and a count. */
#ifndef DCP_POINTER_MOTION_H
#define DCP_POINTER_MOTION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DCP_MOTION_SOURCE_MOUSE,         /* host cursor only: Pointer Control is not active */
    DCP_MOTION_SOURCE_MOUSE_POINTER, /* Pointer Control is active: the motion reaches the game */
    DCP_MOTION_SOURCE_GAMEPAD_AXIS,  /* right stick, including a trackpad Steam Input maps to it */
    DCP_MOTION_SOURCE_COUNT
} DcpMotionSource;

enum { DCP_MOTION_HELD_MOUSE = 1, DCP_MOTION_HELD_GAMEPAD = 2 };

typedef enum {
    DCP_MOTION_EVENT_ACTIVE,
    DCP_MOTION_EVENT_WHILE_HELD,
    DCP_MOTION_EVENT_STALL_WHILE_HELD,
    DCP_MOTION_EVENT_RECOVERED,
    DCP_MOTION_EVENT_COUNT
} DcpMotionEvent;

#define DCP_MOTION_LINE_SIZE 128
#define DCP_MOTION_MAX_LINES 3
/* No game read of player 1 for this long while host motion keeps arriving is a stall: six missed 60 Hz reads, well past
 * the two-read recentre of Pointer Control's finger. */
#define DCP_MOTION_STALL_NS 100000000ull

typedef struct DcpMotionTelemetry DcpMotionTelemetry;

DcpMotionTelemetry *dcp_motion_telemetry_new(void);
void dcp_motion_telemetry_free(DcpMotionTelemetry *telemetry);

/* Window thread: one host motion event. `held` is a DCP_MOTION_HELD_* mask. Returns the lines to log. */
size_t dcp_motion_host(DcpMotionTelemetry *telemetry, DcpMotionSource source, unsigned held, uint64_t now_ns,
                       char lines[][DCP_MOTION_LINE_SIZE]);
/* Pad sampler: one game read of player 1. Returns the lines to log. */
size_t dcp_motion_read(DcpMotionTelemetry *telemetry, uint64_t now_ns, char lines[][DCP_MOTION_LINE_SIZE]);

/* Process-wide wrappers that print the lines. */
void dcp_motion_record_host(DcpMotionSource source, unsigned held, uint64_t now_ns);
void dcp_motion_record_read(uint64_t now_ns);

#ifdef __cplusplus
}
#endif

#endif
