/* Touchscreen routing under Gamescope. Gamescope turns a touchscreen into pointer motion and a left click unless its
 * touch click mode is Passthrough; this header holds the pure decisions (when to ask for Passthrough, who owns the mode)
 * and the privacy-safe route telemetry lines. No SDL, no X11. */
#ifndef DCP_TOUCH_GAMESCOPE_H
#define DCP_TOUCH_GAMESCOPE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DCP_GAMESCOPE_TOUCH_ATOM "STEAM_TOUCH_CLICK_MODE"
#define DCP_GAMESCOPE_TOUCH_LEFT 1u /* Gamescope's default */
#define DCP_GAMESCOPE_TOUCH_PASSTHROUGH 4u
#define DCP_GAMESCOPE_TOUCH_RECHECK_MS 2000u
#define DCP_TOUCH_ROUTE_LINE_SIZE 160

typedef struct {
    int linux_host;
    int gamescope; /* the session runs under Gamescope */
    const char *display;
    const char *opt_out; /* DCP_TOUCH_GAMESCOPE: "0" keeps Gamescope's own touch handling */
} DcpTouchGamescopeEnv;

static inline int dcp_touch_env_set(const char *value) { return value && *value; }

static inline int dcp_touch_gamescope_wanted(const DcpTouchGamescopeEnv *e) {
    if (!e->linux_host || !e->gamescope || !dcp_touch_env_set(e->display)) return 0;
    return !(e->opt_out && !strcmp(e->opt_out, "0"));
}

/* The game owns the mode from its first write until its release; a change by Steam meanwhile is taken back and becomes the
 * value the release restores. */
typedef struct {
    int applied;
    uint32_t saved;
} DcpTouchModeState;

typedef enum { DCP_TOUCH_MODE_KEEP, DCP_TOUCH_MODE_WRITE } DcpTouchModeAction;

/* `have` is whether the property exists. Returns WRITE when `*value` must be written. */
static inline DcpTouchModeAction dcp_touch_mode_claim(DcpTouchModeState *s, int have, uint32_t current, uint32_t *value) {
    if (have && current == DCP_GAMESCOPE_TOUCH_PASSTHROUGH) return DCP_TOUCH_MODE_KEEP;
    s->saved = have ? current : DCP_GAMESCOPE_TOUCH_LEFT;
    s->applied = 1;
    *value = DCP_GAMESCOPE_TOUCH_PASSTHROUGH;
    return DCP_TOUCH_MODE_WRITE;
}

/* Gives the saved mode back only while Passthrough is still the one this game wrote. */
static inline DcpTouchModeAction dcp_touch_mode_release(DcpTouchModeState *s, int have, uint32_t current, uint32_t *value) {
    const int ours = s->applied && have && current == DCP_GAMESCOPE_TOUCH_PASSTHROUGH;
    s->applied = 0;
    if (!ours) return DCP_TOUCH_MODE_KEEP;
    *value = s->saved;
    return DCP_TOUCH_MODE_WRITE;
}

/* Only these names reach a log line. */
static inline const char *dcp_touch_video_name(const char *driver) {
    if (!driver) return "none";
    if (!strcmp(driver, "wayland") || !strcmp(driver, "x11") || !strcmp(driver, "windows")) return driver;
    return "other";
}

typedef struct {
    const char *video;
    int gamescope;
    int direct_devices;
} DcpTouchRouteInfo;

typedef enum {
    DCP_TOUCH_ROUTE_READY,
    DCP_TOUCH_ROUTE_UPDATED,
    DCP_TOUCH_ROUTE_FALLBACK_USED,
    DCP_TOUCH_ROUTE_FALLBACK_FAILED,
    DCP_TOUCH_ROUTE_EVENT_COUNT
} DcpTouchRouteEvent;

/* Each event is due once. UPDATED is due only when the direct device count differs from the one READY logged. */
typedef struct {
    int logged[DCP_TOUCH_ROUTE_EVENT_COUNT];
    int ready_devices;
} DcpTouchRouteLog;

static inline int dcp_touch_route_note(DcpTouchRouteLog *log, DcpTouchRouteEvent event, const DcpTouchRouteInfo *route, char *line,
                                       size_t size) {
    if ((int)event < 0 || (int)event >= DCP_TOUCH_ROUTE_EVENT_COUNT || log->logged[(int)event]) return 0;
    const int devices = route->direct_devices < 0 ? 0 : route->direct_devices > 99 ? 99 : route->direct_devices;
    const char *gamescope = route->gamescope ? "yes" : "no";
    switch (event) {
    case DCP_TOUCH_ROUTE_READY:
        log->ready_devices = devices;
        snprintf(line, size, "Touchscreen telemetry: event=touch_route_ready video=%s gamescope=%s direct_devices=%d count=1",
                 dcp_touch_video_name(route->video), gamescope, devices);
        break;
    case DCP_TOUCH_ROUTE_UPDATED:
        if (log->logged[DCP_TOUCH_ROUTE_READY] && devices == log->ready_devices) return 0;
        snprintf(line, size, "Touchscreen telemetry: event=touch_route_updated video=%s gamescope=%s direct_devices=%d count=1",
                 dcp_touch_video_name(route->video), gamescope, devices);
        break;
    case DCP_TOUCH_ROUTE_FALLBACK_USED:
        snprintf(line, size, "Touchscreen telemetry: event=touchscreen_fallback_used source=gamescope count=1");
        break;
    default:
        snprintf(line, size, "Touchscreen telemetry: event=touchscreen_fallback_unavailable source=gamescope count=1");
        break;
    }
    log->logged[(int)event] = 1;
    return 1;
}

#endif
