#include "runtime_log.h"
#include <stdio.h>
#include <stdlib.h>

static char session_log_path[4200];
static int detailed_logging = 1;
static int clean_exit;
static int crash_reported;
static int finalized;

int runtime_log_should_remove(int detailed, int clean, int crashed) {
    return !detailed && clean && !crashed;
}

void runtime_log_set_detailed(int enabled) { detailed_logging = enabled != 0; }

void runtime_log_mark_clean_exit(void) { clean_exit = 1; }

void runtime_log_mark_crash(void) { crash_reported = 1; }

void runtime_log_set_session_path(const char *path) {
    snprintf(session_log_path, sizeof(session_log_path), "%s", path ? path : "");
    clean_exit = 0;
    crash_reported = 0;
    finalized = 0;
}

void runtime_log_finalize(void) {
    if (finalized) return;
    finalized = 1;
    if (!runtime_log_should_remove(detailed_logging, clean_exit, crash_reported)) return;
    if (!session_log_path[0]) return;
    runtime_log_flush();
    remove(session_log_path);
}

void runtime_log_install_cleanup(void) {
    static int installed;
    if (installed) return;
    installed = 1;
    atexit(runtime_log_finalize);
}
