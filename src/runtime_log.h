#ifndef RUNTIME_LOG_H
#define RUNTIME_LOG_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Creates the next free dragons_crown_pro_NNN.log in dir, or exactly file when it is given. 0 on failure. */
int runtime_log_create(const char *dir, const char *file, char *path, size_t size, void **handle);

/* Sends stdout and stderr to that log and echoes them to the standard output the process started with, if any. */
int runtime_log_start(const char *dir, const char *file, char *path, size_t size);

/* Windows: the same for the existing log file, appended to; the game process continues the launcher's log. 0 when the
 * file does not exist. */
int runtime_log_continue(const char *file, char *path, size_t size);

/* Appends to the run log with Win32 file APIs, bypassing stdio; 0 before the log exists.
 * Safe from the exception handler. */
int runtime_log_write_direct(const char *text);

/* Waits until what was written so far has reached the console. */
void runtime_log_flush(void);

/* Points stdout and stderr away from the log runtime_log_start opened. On Windows the C runtime opens them without
 * delete sharing, so the log cannot be removed while they hold it. */
void runtime_log_release(void);

/* Whether a finished run's log should be discarded: only when Detailed logging is off, the exit was
 * clean, and no crash was reported. */
int runtime_log_should_remove(int detailed, int clean, int crashed);

/* Detailed logging for this run (the launcher's detailed_logging setting). Default on. */
void runtime_log_set_detailed(int enabled);

/* Marks the exit as normal, so Detailed logging off may discard this run's log. */
void runtime_log_mark_clean_exit(void);

/* Marks a crash, so the log is kept even if the process exits through a normal path. */
void runtime_log_mark_crash(void);

/* Records the file this run writes; called by runtime_log_start. */
void runtime_log_set_session_path(const char *path);

/* Idempotent: discards this run's log when the decision above says so. Registered by
 * runtime_log_install_cleanup for normal exits and called explicitly before hard exits. */
void runtime_log_finalize(void);
void runtime_log_install_cleanup(void);

#ifdef __cplusplus
}
#endif
#endif
