/* Steam Deck trackpad routing. Steam Input shows the trackpads to the game as a mouse or as a virtual Xbox gamepad (28de:11ff),
 * neither of which carries touchpads, so on Gaming Mode the mouse is the trackpad. This header holds the pure decisions and
 * the privacy-safe telemetry lines. No SDL. */
#ifndef DCP_TRACKPAD_H
#define DCP_TRACKPAD_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DCP_TRACKPAD_VALVE_VENDOR 0x28de
#define DCP_TRACKPAD_LINE_SIZE 160
#define DCP_TRACKPAD_MAX_UPDATES 3

typedef struct {
    int linux_host;
    int steam_deck; /* SteamDeck=1 */
    int gamescope;
} DcpTrackpadEnv;

static inline int dcp_trackpad_steam_deck_value(const char *value) { return value && !strcmp(value, "1"); }

typedef enum { DCP_TRACKPAD_AUTO, DCP_TRACKPAD_OFF, DCP_TRACKPAD_SETTING_COUNT } DcpTrackpadSetting;

typedef enum { DCP_TRACKPAD_MODE_NONE, DCP_TRACKPAD_MODE_NATIVE, DCP_TRACKPAD_MODE_MOUSE_FALLBACK } DcpTrackpadMode;

typedef enum {
    DCP_TRACKPAD_REASON_NONE,
    DCP_TRACKPAD_REASON_NOT_GAMING_MODE,
    DCP_TRACKPAD_REASON_SETTING_OFF,
    DCP_TRACKPAD_REASON_MOUSE_CONTROLS_OFF,
    DCP_TRACKPAD_REASON_COUNT
} DcpTrackpadReason;

typedef struct {
    DcpTrackpadEnv env;
    int setting;           /* DcpTrackpadSetting */
    int mouse_controls;
    int native_touchpads;  /* touchpads of the open Valve gamepads */
} DcpTrackpadInput;

typedef struct {
    int mode;   /* DcpTrackpadMode */
    int reason; /* DcpTrackpadReason when the mode is NONE on a Steam Deck */
} DcpTrackpadRoute;

/* Native: SDL gives the game a Valve gamepad with touchpads. Mouse fallback: Gaming Mode with the trackpad presented as the
 * mouse. Anything else, a desktop or a Steam Deck in desktop mode included, keeps ordinary mouse semantics. */
static inline DcpTrackpadRoute dcp_trackpad_route(const DcpTrackpadInput *in) {
    DcpTrackpadRoute route = {DCP_TRACKPAD_MODE_NONE, DCP_TRACKPAD_REASON_NONE};
    if (!in->env.linux_host || !in->env.steam_deck) return route;
    if (in->native_touchpads > 0) {
        route.mode = DCP_TRACKPAD_MODE_NATIVE;
        return route;
    }
    if (!in->env.gamescope) route.reason = DCP_TRACKPAD_REASON_NOT_GAMING_MODE;
    else if (in->setting == DCP_TRACKPAD_OFF) route.reason = DCP_TRACKPAD_REASON_SETTING_OFF;
    else if (!in->mouse_controls) route.reason = DCP_TRACKPAD_REASON_MOUSE_CONTROLS_OFF;
    else route.mode = DCP_TRACKPAD_MODE_MOUSE_FALLBACK;
    return route;
}

/* A Valve gamepad with two touchpads is the Deck itself: the right pad drives the game's touch surface, the left one when only
 * it is touched, and a pad stays chosen while it is touched. */
static inline int dcp_trackpad_pick(int left_touched, int right_touched, int previous) {
    if (previous == 0 && left_touched) return 0;
    if (previous == 1 && right_touched) return 1;
    if (right_touched) return 1;
    if (left_touched) return 0;
    return previous == 0 ? 0 : 1;
}

/* Touchpads SDL gives the game on a Valve gamepad. The Steam Virtual Gamepad (28de:11ff) has none, and other makers' touchpads
 * are ordinary controller touchpads. */
static inline int dcp_trackpad_native_touchpads(int vendor, int touchpads) {
    return vendor == DCP_TRACKPAD_VALVE_VENDOR && touchpads > 0 ? touchpads : 0;
}

typedef enum {
    DCP_TRACKPAD_EVENT_NATIVE_USED,
    DCP_TRACKPAD_EVENT_MOUSE_FALLBACK_USED,
    DCP_TRACKPAD_EVENT_POINTER_INTERACTION,
    DCP_TRACKPAD_EVENT_UNAVAILABLE,
    DCP_TRACKPAD_EVENT_SYNTHETIC_TOUCH_ACTIVE,
    DCP_TRACKPAD_EVENT_YIELDED_TO_STICK,
    DCP_TRACKPAD_EVENT_COUNT
} DcpTrackpadEvent;

static inline const char *dcp_trackpad_mode_name(int mode) {
    return mode == DCP_TRACKPAD_MODE_NATIVE ? "native" : mode == DCP_TRACKPAD_MODE_MOUSE_FALLBACK ? "mouse_fallback" : "none";
}

static inline const char *dcp_trackpad_reason_name(int reason) {
    switch (reason) {
    case DCP_TRACKPAD_REASON_NOT_GAMING_MODE: return "not_gaming_mode";
    case DCP_TRACKPAD_REASON_SETTING_OFF: return "setting_off";
    case DCP_TRACKPAD_REASON_MOUSE_CONTROLS_OFF: return "mouse_controls_off";
    default: return "";
    }
}

/* Counts only: no position, finger, mouse or device identity reaches a line. */
typedef struct {
    uint64_t count[DCP_TRACKPAD_EVENT_COUNT][DCP_TRACKPAD_REASON_COUNT];
    int ready_logged;
    int updates;
    int last_mode, last_native, last_mice;
} DcpTrackpadLog;

static inline int dcp_trackpad_count_due(uint64_t count) {
    while (count >= 10 && count % 10 == 0) count /= 10;
    return count == 1;
}

static inline int dcp_trackpad_note(DcpTrackpadLog *log, DcpTrackpadEvent event, DcpTrackpadReason reason, char *line, size_t size) {
    if ((int)event < 0 || (int)event >= DCP_TRACKPAD_EVENT_COUNT || (int)reason < 0 || (int)reason >= DCP_TRACKPAD_REASON_COUNT) return 0;
    const uint64_t count = ++log->count[(int)event][(int)reason];
    if (!dcp_trackpad_count_due(count)) return 0;
    static const char *const names[DCP_TRACKPAD_EVENT_COUNT] = {
        "trackpad_native_used", "trackpad_mouse_fallback_used", "trackpad_pointer_interaction", "trackpad_unavailable",
        "trackpad_synthetic_touch_active", "trackpad_yielded_to_stick",
    };
    if (event == DCP_TRACKPAD_EVENT_UNAVAILABLE) {
        snprintf(line, size, "Trackpad telemetry: event=%s reason=%s count=%llu", names[event], dcp_trackpad_reason_name(reason),
                 (unsigned long long)count);
    } else {
        snprintf(line, size, "Trackpad telemetry: event=%s count=%llu", names[event], (unsigned long long)count);
    }
    return 1;
}

static inline int dcp_trackpad_clamp_count(int value, int limit) { return value < 0 ? 0 : value > limit ? limit : value; }

/* The route line: ready once, then updated when the mode or a device count changes (at most DCP_TRACKPAD_MAX_UPDATES times). */
static inline int dcp_trackpad_route_note(DcpTrackpadLog *log, const DcpTrackpadEnv *env, int native_touchpads, int mice, int mode,
                                          char *line, size_t size) {
    const int native = dcp_trackpad_clamp_count(native_touchpads, 9), pointers = dcp_trackpad_clamp_count(mice, 9);
    const char *event;
    if (!log->ready_logged) {
        event = "trackpad_route_ready";
        log->ready_logged = 1;
    } else if (log->updates < DCP_TRACKPAD_MAX_UPDATES &&
               (mode != log->last_mode || native != log->last_native || pointers != log->last_mice)) {
        event = "trackpad_route_updated";
        ++log->updates;
    } else {
        return 0;
    }
    log->last_mode = mode;
    log->last_native = native;
    log->last_mice = pointers;
    snprintf(line, size, "Trackpad telemetry: event=%s steamdeck=%s gamescope=%s native_touchpads=%d mice=%d mode=%s count=1", event,
             env->steam_deck ? "yes" : "no", env->gamescope ? "yes" : "no", native, pointers, dcp_trackpad_mode_name(mode));
    return 1;
}

#endif
