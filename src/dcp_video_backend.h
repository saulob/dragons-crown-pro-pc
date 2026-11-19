/* Window backend for Steam-launched Wayland desktop sessions: Steam cannot associate a native Wayland window with a
 * non-Steam shortcut, so its Steam Input layout stays on Desktop. Asking SDL for X11 (XWayland) first fixes that. */
#ifndef DCP_VIDEO_BACKEND_H
#define DCP_VIDEO_BACKEND_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef DCP_VIDEO_BACKEND_POLICY_ONLY
#include <SDL3/SDL.h>
#endif

typedef struct {
    int linux_host;
    const char *steam_game_id, *steam_app_id;
    const char *session_type;
    const char *wayland_display, *display;
    const char *gamescope_display;
    const char *video_driver;
    int x11_driver_listed;
} DcpVideoEnvironment;

typedef enum {
    DCP_VIDEO_UNCHANGED,
    DCP_VIDEO_USE_X11,
    DCP_VIDEO_X11_UNAVAILABLE,
    DCP_VIDEO_KEEP_GAMESCOPE,
    DCP_VIDEO_KEEP_USER_DRIVER
} DcpVideoChoice;

#define DCP_VIDEO_X11_FIRST "x11,wayland"
#define DCP_VIDEO_LINE_SIZE 160
#define DCP_VIDEO_MAX_LINES 2

static inline int dcp_video_set(const char *value) { return value && *value; }

static inline int dcp_video_steam_launch(const DcpVideoEnvironment *e) {
    return dcp_video_set(e->steam_game_id) || dcp_video_set(e->steam_app_id);
}

static inline int dcp_video_wayland_session(const DcpVideoEnvironment *e) {
    if (dcp_video_set(e->session_type)) return !strcmp(e->session_type, "wayland");
    return dcp_video_set(e->wayland_display);
}

static inline DcpVideoChoice dcp_video_choose(const DcpVideoEnvironment *e) {
    if (!e->linux_host || !dcp_video_steam_launch(e)) return DCP_VIDEO_UNCHANGED;
    if (dcp_video_set(e->gamescope_display)) return DCP_VIDEO_KEEP_GAMESCOPE;
    if (!dcp_video_wayland_session(e)) return DCP_VIDEO_UNCHANGED;
    if (dcp_video_set(e->video_driver)) return DCP_VIDEO_KEEP_USER_DRIVER;
    if (!dcp_video_set(e->display) || !e->x11_driver_listed) return DCP_VIDEO_X11_UNAVAILABLE;
    return DCP_VIDEO_USE_X11;
}

static inline const char *dcp_video_choice_name(DcpVideoChoice choice) {
    switch (choice) {
    case DCP_VIDEO_USE_X11: return "use_x11";
    case DCP_VIDEO_X11_UNAVAILABLE: return "x11_unavailable";
    case DCP_VIDEO_KEEP_GAMESCOPE: return "keep_gamescope";
    case DCP_VIDEO_KEEP_USER_DRIVER: return "keep_user_driver";
    default: return "unchanged";
    }
}

/* Telemetry lines for one session: only event names, the session type, the requested backend and yes/no values. */
static inline size_t dcp_video_telemetry(DcpVideoChoice choice, char lines[][DCP_VIDEO_LINE_SIZE]) {
    const char *gamescope = choice == DCP_VIDEO_KEEP_GAMESCOPE ? "yes" : "no";
    const char *event = NULL, *backend = "default", *reason = "";
    size_t n = 0;
    if (choice == DCP_VIDEO_UNCHANGED) return 0;
    if (choice != DCP_VIDEO_KEEP_GAMESCOPE)
        snprintf(lines[n++], DCP_VIDEO_LINE_SIZE,
                 "Steam Input telemetry: event=steam_input_wayland_detected session=wayland backend=default "
                 "gamescope=no count=1");
    switch (choice) {
    case DCP_VIDEO_USE_X11: event = "steam_input_xwayland_selected"; backend = "x11"; break;
    case DCP_VIDEO_X11_UNAVAILABLE: event = "steam_input_xwayland_unavailable"; break;
    case DCP_VIDEO_KEEP_GAMESCOPE: event = "steam_input_native_wayland_retained"; reason = " reason=gamescope"; break;
    default: event = "steam_input_native_wayland_retained"; reason = " reason=user_driver"; break;
    }
    snprintf(lines[n++], DCP_VIDEO_LINE_SIZE, "Steam Input telemetry: event=%s session=wayland backend=%s gamescope=%s%s count=1",
             event, backend, gamescope, reason);
    return n;
}

#ifndef DCP_VIDEO_BACKEND_POLICY_ONLY
static inline DcpVideoEnvironment dcp_video_environment(void) {
    DcpVideoEnvironment e;
    memset(&e, 0, sizeof(e));
#ifdef __linux__
    e.linux_host = 1;
#endif
    e.steam_game_id = getenv("SteamGameId");
    e.steam_app_id = getenv("SteamAppId");
    e.session_type = getenv("XDG_SESSION_TYPE");
    e.wayland_display = getenv("WAYLAND_DISPLAY");
    e.display = getenv("DISPLAY");
    e.gamescope_display = getenv("GAMESCOPE_WAYLAND_DISPLAY");
    e.video_driver = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    for (int i = 0, n = SDL_GetNumVideoDrivers(); i < n; ++i) {
        const char *name = SDL_GetVideoDriver(i);
        if (name && !strcmp(name, "x11")) e.x11_driver_listed = 1;
    }
    return e;
}

/* Before the first SDL_INIT_VIDEO, and again after any SDL_Quit (which clears hints). `report` logs once per session. */
static inline DcpVideoChoice dcp_video_backend_prepare(int report) {
    const DcpVideoEnvironment e = dcp_video_environment();
    const DcpVideoChoice choice = dcp_video_choose(&e);
    if (choice == DCP_VIDEO_USE_X11) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, DCP_VIDEO_X11_FIRST);
    if (report) {
        char lines[DCP_VIDEO_MAX_LINES][DCP_VIDEO_LINE_SIZE];
        const size_t n = dcp_video_telemetry(choice, lines);
        for (size_t i = 0; i < n; ++i) puts(lines[i]);
        /* DEBUG_SAULO: remove after the reporter's next log */
        printf("DEBUG_SAULO: video backend steam=%s session=%s gamescope=%s x11_listed=%s display=%s user_driver=%s choice=%s requested=%s\n",
               dcp_video_steam_launch(&e) ? "yes" : "no", dcp_video_set(e.session_type) ? e.session_type : "unset",
               dcp_video_set(e.gamescope_display) ? "yes" : "no", e.x11_driver_listed ? "yes" : "no",
               dcp_video_set(e.display) ? "yes" : "no", dcp_video_set(e.video_driver) ? "yes" : "no",
               dcp_video_choice_name(choice), choice == DCP_VIDEO_USE_X11 ? DCP_VIDEO_X11_FIRST : "default");
    }
    return choice;
}
#endif

#endif
