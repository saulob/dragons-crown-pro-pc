/* The launcher, called by the loader before the guest and the game window exist. */
#ifndef DCP_LAUNCHER_H
#define DCP_LAUNCHER_H

#include <stddef.h>

#include "dcp_resolution.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when the graphics system can run the game. Otherwise 0, with what is wrong and what to do about it in the buffers. */
typedef int (*DcpGraphicsCheck)(char *cause, size_t cause_size, char *advice, size_t advice_size);

typedef struct {
    const char *app0;          /* prepared game folder (/app0) */
    const char *image_dir;     /* folder with boot.bin, content.bin and image.json */
    const char *user_dir;      /* user data; dcp.ini lives here unless settings_file is set */
    const char *settings_file; /* NULL: <user_dir>/dcp.ini */
    const char *logs_dir;      /* NULL when unknown */
    int skip_once;             /* --dcp-skip-launcher: do not show it unless the game files are not ready */
    int force_show;            /* --launcher: show it even when "Show this launcher" is off, like holding Shift */
    DcpGraphicsCheck check_graphics; /* NULL: Play does not check; see dcp_check_vulkan */
} DcpLauncherConfig;

/* NULL asks for the default, which is relative to the executable (launcher/paths.h). */
typedef struct {
    const char *boot;
    const char *content_profile;
    const char *app0;
    const char *user_dir;
    const char *logs_dir;
} DcpPathRequest;

typedef struct {
    char boot[4096];
    char content_profile[4096];
    char app0[4096];
    char image_dir[4096];
    char user_dir[4096];
    char logs_dir[4096];
} DcpPaths;

/* 0 when the executable's folder is unknown or a path does not fit. */
int dcp_resolve_paths(const DcpPathRequest *request, DcpPaths *paths);

/* Index into dcp_resolutions (dcp_resolution.h) of the render_resolution in the settings file; 0 (native) when the
 * file or the value is missing. */
int dcp_render_resolution(const char *settings_file);

/* Index into dcp_aspect_ratios of the aspect_ratio in the settings file; 0 (16:9 Original) when the file or the value
 * is missing. */
int dcp_aspect_ratio(const char *settings_file);

/* The system language the game is told for the text_language in the settings file; 1 (English) when the file or the
 * value is missing. */
int dcp_text_language(const char *settings_file);

/* The cheats the settings file turns on, one bit per cheat (dcp_cheats.h); 0 when the file or the keys are missing. */
unsigned dcp_saved_cheats(const char *settings_file);

/* The Experience Multiplier the settings file sets (1 = Off, 2, 5 or 10); Off when the file or the value is missing or not
 * one of those. */
int dcp_saved_experience_multiplier(const char *settings_file);

/* Fills `names` with the distinct names of the Vulkan devices the renderer can use and returns how many (none without a
 * usable Vulkan on Linux); enumerates only, no device is created. */
int dcp_gpu_names(char (*names)[256], int max);

/* A DcpGraphicsCheck: 1 when Vulkan can run the renderer. Otherwise logs why and returns 0. It asks again on every call
 * until it succeeds, so a driver installed meanwhile is found. */
int dcp_check_vulkan(char *cause, size_t cause_size, char *advice, size_t advice_size);

/* Builds boot.bin, content.bin and image.json in `out` from the prepared game folder `app0` (UTF-8 paths). 0 on
 * success; otherwise 1 and the reason in `error`. */
int dcp_build_boot_image(const char *app0, const char *out, char *error, size_t error_size);

typedef struct {
    const char *label;
    int cheats;                  /* the native cheat hooks are validated for this build */
    const DcpResolutionSite *resolution_sites;
    size_t resolution_site_count;
} DcpGameBuild;

/* The supported game build a boot image folder was built from (its image.json); 0 when it is not one. */
int dcp_game_build(const char *image_dir, DcpGameBuild *build);

typedef enum { DCP_LAUNCHER_PLAY = 0, DCP_LAUNCHER_QUIT = 1, DCP_LAUNCHER_BLOCKED = 2 } DcpLauncherResult;

/* Shows the launcher when its rules say so (game files not ready, "Show this launcher" on, or Shift held) and returns
 * once the player pressed Play or quit. Returns DCP_LAUNCHER_PLAY at once when it is not shown.
 * While check_graphics fails, Play is refused: the launcher says why and stays open. A launcher that is not shown
 * explains in a window of its own and returns DCP_LAUNCHER_BLOCKED. */
DcpLauncherResult dcp_launcher_run(const DcpLauncherConfig *config);

#ifdef __cplusplus
}
#endif

#endif
