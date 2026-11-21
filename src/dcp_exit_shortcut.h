/* The port's Exit Game shortcuts: the keyboard hotkey press rule, the controller chord (L1 + R1 + Options held for two
 * seconds on one controller) and the log lines of both. Pure decisions, no SDL: the window thread feeds in the buttons each
 * controller holds and a monotonic clock. Both routes end in dcpgpu_request_close(). */
#ifndef DCP_EXIT_SHORTCUT_H
#define DCP_EXIT_SHORTCUT_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DCP_EXIT_CHORD_HOLD_MS 2000u
#define DCP_EXIT_CHORD_SLOTS 8
#define DCP_EXIT_LINE_SIZE 128
#define DCP_EXIT_START_LINES_MAX 8

/* A bound key closes on the press edge: a held key's repeats and an unbound key never do. */
static inline int dcp_exit_key_pressed(int event_scancode, int repeat, int bound_scancode) {
    return bound_scancode > 0 && event_scancode == bound_scancode && !repeat;
}

/* The chord is down when the shortcut is enabled and the one controller holds all three buttons. */
static inline int dcp_exit_chord_down(int enabled, int l1, int r1, int options) {
    return enabled && l1 && r1 && options;
}

typedef struct {
    int used;
    int spent;         /* completed or cancelled: the chord must be released before it counts again */
    uint32_t pad;      /* the controller's instance id */
    uint64_t since_ms;
} DcpExitChordSlot;

/* A slot exists only while its controller holds the chord, so the table never fills with idle controllers. */
typedef struct { DcpExitChordSlot slots[DCP_EXIT_CHORD_SLOTS]; } DcpExitChord;

typedef enum { DCP_EXIT_CHORD_NONE, DCP_EXIT_CHORD_STARTED, DCP_EXIT_CHORD_COMPLETED } DcpExitChordEvent;

static inline DcpExitChordSlot *dcp_exit_chord_find(DcpExitChord *chord, uint32_t pad) {
    for (int i = 0; i < DCP_EXIT_CHORD_SLOTS; ++i) {
        if (chord->slots[i].used && chord->slots[i].pad == pad) return &chord->slots[i];
    }
    return NULL;
}

/* A controller that is gone. */
static inline void dcp_exit_chord_forget(DcpExitChord *chord, uint32_t pad) {
    DcpExitChordSlot *slot = dcp_exit_chord_find(chord, pad);
    if (slot) memset(slot, 0, sizeof(*slot));
}

static inline void dcp_exit_chord_clear(DcpExitChord *chord) { memset(chord, 0, sizeof(*chord)); }

/* Focus lost: every hold in progress ends, and a chord still held waits for its release. */
static inline void dcp_exit_chord_cancel(DcpExitChord *chord) {
    for (int i = 0; i < DCP_EXIT_CHORD_SLOTS; ++i) {
        if (chord->slots[i].used) chord->slots[i].spent = 1;
    }
}

/* One sample of one controller. STARTED when its three buttons are first all down, COMPLETED once, when they have stayed
 * down DCP_EXIT_CHORD_HOLD_MS (`held_ms` is the time held); releasing any of them ends the hold. Another controller's
 * buttons never count: each is judged alone. */
static inline DcpExitChordEvent dcp_exit_chord_update(DcpExitChord *chord, uint32_t pad, int enabled, int l1, int r1,
                                                      int options, uint64_t now_ms, uint32_t *held_ms) {
    DcpExitChordSlot *slot = dcp_exit_chord_find(chord, pad);
    if (!dcp_exit_chord_down(enabled, l1, r1, options)) {
        if (slot) memset(slot, 0, sizeof(*slot));
        return DCP_EXIT_CHORD_NONE;
    }
    if (!slot) {
        for (int i = 0; i < DCP_EXIT_CHORD_SLOTS && !slot; ++i) {
            if (!chord->slots[i].used) slot = &chord->slots[i];
        }
        if (!slot) return DCP_EXIT_CHORD_NONE;
        slot->used = 1;
        slot->spent = 0;
        slot->pad = pad;
        slot->since_ms = now_ms;
        return DCP_EXIT_CHORD_STARTED;
    }
    if (slot->spent) return DCP_EXIT_CHORD_NONE;
    const uint64_t elapsed = now_ms >= slot->since_ms ? now_ms - slot->since_ms : 0;
    if (elapsed < DCP_EXIT_CHORD_HOLD_MS) return DCP_EXIT_CHORD_NONE;
    slot->spent = 1;
    if (held_ms) *held_ms = (uint32_t)(elapsed > UINT32_MAX ? UINT32_MAX : elapsed);
    return DCP_EXIT_CHORD_COMPLETED;
}

typedef enum { DCP_EXIT_EVENT_KEYBOARD_USED, DCP_EXIT_EVENT_CONTROLLER_HOLD_STARTED, DCP_EXIT_EVENT_CONTROLLER_USED,
               DCP_EXIT_EVENT_COUNT } DcpExitEvent;

typedef struct { uint64_t count[DCP_EXIT_EVENT_COUNT]; } DcpExitTelemetry;

/* The log line of an event, counting it; 0 and no line once a start has been logged DCP_EXIT_START_LINES_MAX times. No
 * controller name, GUID or serial reaches it. */
static inline int dcp_exit_telemetry_note(DcpExitTelemetry *telemetry, DcpExitEvent event, uint32_t held_ms, char *line,
                                          size_t size) {
    if ((unsigned)event >= (unsigned)DCP_EXIT_EVENT_COUNT) return 0;
    const unsigned long long count = (unsigned long long)++telemetry->count[event];
    switch (event) {
    case DCP_EXIT_EVENT_KEYBOARD_USED:
        snprintf(line, size, "Exit shortcut telemetry: event=keyboard_used count=%llu", count);
        return 1;
    case DCP_EXIT_EVENT_CONTROLLER_HOLD_STARTED:
        if (count > DCP_EXIT_START_LINES_MAX) return 0;
        snprintf(line, size, "Exit shortcut telemetry: event=controller_hold_started count=%llu", count);
        return 1;
    default:
        snprintf(line, size, "Exit shortcut telemetry: event=controller_used hold_ms=%u count=%llu", (unsigned)((held_ms + 5u) / 10u * 10u),
                 count);
        return 1;
    }
}

/* The start-up line: the key and whether the controller chord is on. */
static inline void dcp_exit_shortcut_ready_line(char *line, size_t size, const char *key_name, int controller) {
    snprintf(line, size, "Exit shortcut: keyboard key=%s controller=%s", key_name && *key_name ? key_name : "none",
             controller ? "on" : "off");
}

#endif
