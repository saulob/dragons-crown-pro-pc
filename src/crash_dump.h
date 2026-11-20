/* Windows: the minidump of a fatal exception, written beside the session log after loader.c's own report. A dedicated
 * thread writes it, because MiniDumpWriteDump suspends every other thread, and the crashing thread waits for it for a
 * bounded time, so a dump that cannot be written never keeps the process from ending. */
#ifndef DCP_CRASH_DUMP_H
#define DCP_CRASH_DUMP_H

#include <stddef.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The dump of a session log: the log's path with .dmp, "_N" before it from attempt 2; 0 when it does not fit. */
int dcp_crash_dump_path(char *out, size_t size, const char *log_path, int attempt);

/* Loads dbghelp and starts the dump thread; 1 when dumps can be written. Call once, before any fault can be reported. */
int dcp_crash_dump_install(void);

/* Writes the dump of `info`, a fault on `thread`, next to the log of runtime_log_session_path(), and says in that log what
 * became of it. Never overwrites a dump and gives up after a minute. Returns 1 when a dump was written. */
int dcp_crash_dump_write(EXCEPTION_POINTERS *info, DWORD thread);

#ifdef __cplusplus
}
#endif
#endif
