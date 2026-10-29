/* The launcher, called by the loader before the guest and the game window exist. */
#ifndef DCP_LAUNCHER_H
#define DCP_LAUNCHER_H

#include <stddef.h>

#include "dcp_resolution.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when the processor and graphics system can run the game. Otherwise 0, with what is wrong and what to do about it in
 * the buffers. */
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
    int game_process;          /* Windows: with Allow overlay programs on, Play starts the game as a process of its own */
    const char *log_file;      /* this run's log, continued by the game process; NULL when there is none */
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
    char error[512]; /* why the paths cannot be used, when it is known (DCP_GAME_BUILD) */
} DcpPaths;

/* 0 when the executable's folder is unknown, a path does not fit or DCP_GAME_BUILD cannot be used. */
int dcp_resolve_paths(const DcpPathRequest *request, DcpPaths *paths);

/* The folder holding this run's log: the log_file's parent when one is given, else logs_dir. 0 when
 * neither yields a path. */
int dcp_logs_directory(const char *logs_dir, const char *log_file, char *out, size_t size);

/* Index into dcp_resolutions (dcp_resolution.h) of the render_resolution in the settings file, among the resolutions the
 * build image_dir was built from: 0 (native) when the file or the value is missing or the build has no verified render
 * resolution sites. */
int dcp_render_resolution(const char *settings_file, const char *image_dir);

/* Index into dcp_aspect_ratios of the aspect_ratio in the settings file, among the shapes the build image_dir was built
 * from: 0 (16:9 Original) when the file or the value is missing or the build has no verified render resolution sites. */
int dcp_aspect_ratio(const char *settings_file, const char *image_dir);

/* The system language the game is told for the text_language in the settings file, among the languages the prepared
 * game folder app0 holds of the build image_dir was built from: the first of them (English when the build is unknown)
 * when the file or the value is missing or not one of them. */
int dcp_text_language(const char *settings_file, const char *image_dir, const char *app0);

/* The cheats the settings file turns on, one bit per cheat (dcp_cheats.h); 0 when the file or the keys are missing. */
unsigned dcp_saved_cheats(const char *settings_file);

/* The Experience Multiplier the settings file sets (1 = Off, 2, 5 or 10); Off when the file or the value is missing or not
 * one of those. */
int dcp_saved_experience_multiplier(const char *settings_file);

/* The Movement Speed the settings file sets (1 = Off, 2 or 4); Off when the file or the value is missing, invalid or the
 * old 2x setting is off. The old boolean setting is read only when the newer setting is absent. */
int dcp_saved_movement_speed(const char *settings_file);

/* 1 when the settings file routes the NPC voice cues to the Japanese banks (dcp_japanese_npc_voices; 0 by default). */
int dcp_japanese_npc_voices(const char *settings_file);

/* 1 when the settings file asks for True Ultrawide (true_ultrawide; 0 by default, and for a missing or unknown value). */
int dcp_true_ultrawide(const char *settings_file);

/* The Skip Offers the settings file turns on (skip_offers.h bits; none by default). */
unsigned dcp_skip_offers(const char *settings_file);

/* 1 when the settings file keeps the logs of normal sessions (detailed_logging; 1 by default). */
int dcp_detailed_logging(const char *settings_file);

/* What the loader decided for this run: the parts of the "Settings:" log lines the settings file alone does not say. */
typedef struct DcpRunSettings {
    const char *game_build;        /* label; NULL for an unknown build */
    int online_play;               /* effective Online Play (DCP_ONLINE applied): cheats are locked */
    int render_width, render_height;
    const char *render_resolution; /* dcp_resolutions key */
    const char *aspect_ratio;      /* dcp_aspect_ratios name */
    int true_ultrawide_effective;
    unsigned skip_offers;          /* the skip_offers.h bits applied */
} DcpRunSettings;

/* Prints the startup state support needs as "Settings:" lines: port version, platform, online protocol, game build,
 * Online Play and the display, gameplay and skip settings in effect. Never the Online name, Friend Code or account.
 * `run` may be NULL: the settings file's values then. */
void dcp_log_settings_summary(const char *settings_file, const char *image_dir, const char *app0,
                              const DcpRunSettings *run);

/* Starts the game process dcp_launcher_run created suspended. Call it once nothing writes to the log any more; 1 when
 * it runs. A process that could not be started is ended. */
int dcp_resume_game_process(void);

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
    int japanese_npc_voices;     /* the NPC voice routing hook is validated for this build */
    int true_ultrawide;          /* dcp_pass_ortho_sites are verified for this build */
    const DcpResolutionSite *resolution_sites;
    size_t resolution_site_count;
    int resolution_by_signature; /* the render size sites are found by signature in the loaded image */
    const char *title_id;
    const char *version;         /* the build's profile version: with title_id, the build key of the loader's patches */
} DcpGameBuild;

/* The supported game build a boot image folder was built from (its image.json); 0 when it is not one. */
int dcp_game_build(const char *image_dir, DcpGameBuild *build);

typedef enum {
    DCP_LAUNCHER_PLAY = 0,
    DCP_LAUNCHER_QUIT = 1,
    DCP_LAUNCHER_BLOCKED = 2,
    DCP_LAUNCHER_GAME_STARTED = 3 /* game_process: the game process exists, suspended; this process ends */
} DcpLauncherResult;

/* Shows the launcher when its rules say so (game files not ready, "Show this launcher" on, or Shift held) and returns
 * once the player pressed Play or quit. Returns DCP_LAUNCHER_PLAY at once when it is not shown.
 * While check_graphics fails, Play is refused: the launcher says why and stays open. A launcher that is not shown
 * explains in a window of its own and returns DCP_LAUNCHER_BLOCKED.
 * With game_process and Allow overlay programs on, Play creates the game process instead and returns
 * DCP_LAUNCHER_GAME_STARTED; when it cannot be created, the launcher stays open with the reason (or explains in a window
 * of its own). */
DcpLauncherResult dcp_launcher_run(const DcpLauncherConfig *config);

#ifdef __cplusplus
}
#endif

#endif
