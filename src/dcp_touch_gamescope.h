/* Touchscreen routing under Gamescope. Gamescope keeps one touch click mode for the whole compositor: a write to the
 * property on any of its X roots changes it and the newest write wins. This header holds the per-root decisions (when to
 * ask for Passthrough, what each root restores) and the privacy-safe route telemetry. No SDL, no X11. */
#ifndef DCP_TOUCH_GAMESCOPE_H
#define DCP_TOUCH_GAMESCOPE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DCP_GAMESCOPE_TOUCH_ATOM "STEAM_TOUCH_CLICK_MODE"
#define DCP_GAMESCOPE_TOUCH_MARKER_ATOM "GAMESCOPE_INPUT_COUNTER"
#define DCP_GAMESCOPE_TOUCH_LEFT 1u /* Gamescope's default */
#define DCP_GAMESCOPE_TOUCH_PASSTHROUGH 4u
/* Steam writes the mode on its own Gamescope root, so the game keeps rewriting Passthrough while it has focus. */
#define DCP_GAMESCOPE_TOUCH_RECHECK_MS 1000u
#define DCP_GAMESCOPE_TOUCH_MAX_ROOTS 4
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

/* One X root the game asks for Passthrough. The game owns it from its first write until its release and remembers the
 * value that root held. */
typedef struct {
    int owned;
    uint32_t saved;
} DcpTouchModeRoot;

/* Adopts the root's current value as the one to restore, taking back a newer choice from Steam.
 * Passthrough is written on every call: a competing write on another root wins Gamescope's single mode even while this
 * root already reads Passthrough. */
static inline void dcp_touch_mode_claim(DcpTouchModeRoot *root, int have, uint32_t current) {
    if (!root->owned || (have && current != DCP_GAMESCOPE_TOUCH_PASSTHROUGH)) {
        root->saved = have ? current : DCP_GAMESCOPE_TOUCH_LEFT;
        root->owned = 1;
    }
}

/* Gives back the saved value only while Passthrough is still the one this game wrote; returns whether to write.
 * A root that already held Passthrough gives nothing back: rewriting its own claim value would re-impose it on the
 * compositor after a competing write. */
static inline int dcp_touch_mode_release(DcpTouchModeRoot *root, int have, uint32_t current, uint32_t *value) {
    const int owned = root->owned;
    root->owned = 0;
    if (!owned || !have || current != DCP_GAMESCOPE_TOUCH_PASSTHROUGH) return 0;
    if (root->saved == DCP_GAMESCOPE_TOUCH_PASSTHROUGH) return 0;
    *value = root->saved;
    return 1;
}

/* Only these names reach a log line. */
static inline const char *dcp_touch_video_name(const char *driver) {
    if (!driver) return "none";
    if (!strcmp(driver, "wayland") || !strcmp(driver, "x11") || !strcmp(driver, "windows")) return driver;
    return "other";
}

typedef enum { DCP_TOUCH_CLAIM_NO_X11, DCP_TOUCH_CLAIM_NO_DISPLAY } DcpTouchClaimFailure;

typedef struct {
    const char *video;
    int gamescope;
    int direct_devices;
    int roots;      /* X roots the game claimed Passthrough on */
    int compositor; /* one of them is Gamescope's own root rather than the game's display */
    int failure;    /* DcpTouchClaimFailure when no root could be claimed */
} DcpTouchRouteInfo;

typedef enum {
    DCP_TOUCH_ROUTE_READY,
    DCP_TOUCH_ROUTE_UPDATED,
    DCP_TOUCH_ROUTE_CLAIMED,
    DCP_TOUCH_ROUTE_CLAIM_FAILED,
    DCP_TOUCH_ROUTE_REASSERT,
    DCP_TOUCH_ROUTE_EVENT_COUNT
} DcpTouchRouteEvent;

/* READY, UPDATED, CLAIMED and CLAIM_FAILED are due once; UPDATED only when the direct device count differs from READY;
 * REASSERT at the first, then every tenth power. */
typedef struct {
    int logged[DCP_TOUCH_ROUTE_EVENT_COUNT];
    int ready_devices;
    unsigned reasserts;
} DcpTouchRouteLog;

static inline int dcp_touch_route_due(unsigned count) {
    while (count >= 10 && count % 10 == 0) count /= 10;
    return count == 1;
}

static inline int dcp_touch_route_note(DcpTouchRouteLog *log, DcpTouchRouteEvent event, const DcpTouchRouteInfo *route,
                                       char *line, size_t size) {
    if ((int)event < 0 || (int)event >= DCP_TOUCH_ROUTE_EVENT_COUNT) return 0;
    const int devices = route->direct_devices < 0 ? 0 : route->direct_devices > 99 ? 99 : route->direct_devices;
    const int roots = route->roots < 0 ? 0 : route->roots > 9 ? 9 : route->roots;
    const char *gamescope = route->gamescope ? "yes" : "no";
    switch (event) {
    case DCP_TOUCH_ROUTE_READY:
        if (log->logged[(int)event]) return 0;
        log->ready_devices = devices;
        snprintf(line, size, "Touchscreen telemetry: event=touch_route_ready video=%s gamescope=%s direct_devices=%d count=1",
                 dcp_touch_video_name(route->video), gamescope, devices);
        break;
    case DCP_TOUCH_ROUTE_UPDATED:
        if (log->logged[(int)event] || (log->logged[DCP_TOUCH_ROUTE_READY] && devices == log->ready_devices)) return 0;
        snprintf(line, size, "Touchscreen telemetry: event=touch_route_updated video=%s gamescope=%s direct_devices=%d count=1",
                 dcp_touch_video_name(route->video), gamescope, devices);
        break;
    case DCP_TOUCH_ROUTE_CLAIMED:
        if (log->logged[(int)event]) return 0;
        snprintf(line, size, "Touchscreen telemetry: event=gamescope_touch_claim roots=%d compositor=%s count=1", roots,
                 route->compositor ? "yes" : "no");
        break;
    case DCP_TOUCH_ROUTE_CLAIM_FAILED:
        if (log->logged[(int)event]) return 0;
        snprintf(line, size, "Touchscreen telemetry: event=gamescope_touch_claim_failed reason=%s count=1",
                 route->failure == DCP_TOUCH_CLAIM_NO_DISPLAY ? "display" : "x11");
        break;
    case DCP_TOUCH_ROUTE_REASSERT:
        if (!log->logged[DCP_TOUCH_ROUTE_CLAIMED] || !dcp_touch_route_due(++log->reasserts)) return 0;
        snprintf(line, size, "Touchscreen telemetry: event=gamescope_touch_reassert roots=%d count=%u", roots,
                 log->reasserts);
        break;
    default:
        return 0;
    }
    log->logged[(int)event] = 1;
    return 1;
}

#endif
