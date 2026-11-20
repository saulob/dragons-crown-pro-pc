#include "crash_dump.h"
#include "runtime_log.h"

#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* Nothing here crashes: the dump is written for a synthetic exception record, on the test's own thread. */

#define MOST_DUMP_BYTES (8u * 1024u * 1024u)

static int failures;
static FILE *console;
#define CHECK(condition) do { if (!(condition)) { fprintf(console, "FAILED: %s (line %d)\n", #condition, __LINE__); ++failures; } } while (0)

static char *read_file(const char *path, size_t *size) {
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER length;
    char *data = NULL;
    if (GetFileSizeEx(file, &length) && length.QuadPart <= 64 * 1024 * 1024) {
        data = malloc((size_t)length.QuadPart + 1);
        DWORD read = 0;
        if (data && (!ReadFile(file, data, (DWORD)length.QuadPart, &read, NULL) || (LONGLONG)read != length.QuadPart)) {
            free(data);
            data = NULL;
        }
        if (data) {
            data[length.QuadPart] = 0;
            *size = (size_t)length.QuadPart;
        }
    }
    CloseHandle(file);
    return data;
}

static int exists(const char *path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }

static void make_empty(const char *path) {
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
}

static void dump_paths(void) {
    char path[100];
    CHECK(dcp_crash_dump_path(path, sizeof(path), "C:\\logs\\dragons_crown_pro_017.log", 1) && !strcmp(path, "C:\\logs\\dragons_crown_pro_017.dmp"));
    CHECK(dcp_crash_dump_path(path, sizeof(path), "C:\\logs\\dragons_crown_pro_017.log", 2) && !strcmp(path, "C:\\logs\\dragons_crown_pro_017_2.dmp"));
    CHECK(dcp_crash_dump_path(path, sizeof(path), "C:\\logs\\dragons_crown_pro_017.log", 19) && !strcmp(path, "C:\\logs\\dragons_crown_pro_017_19.dmp"));
    CHECK(dcp_crash_dump_path(path, sizeof(path), "C:/logs/dragons_crown_pro_433.log", 1) && !strcmp(path, "C:/logs/dragons_crown_pro_433.dmp"));
    CHECK(!dcp_crash_dump_path(path, 8, "C:\\logs\\dragons_crown_pro_017.log", 1));
    CHECK(!dcp_crash_dump_path(path, sizeof(path), "C:\\logs.d\\noextension", 1));
    CHECK(!dcp_crash_dump_path(path, sizeof(path), "", 1));
    CHECK(!dcp_crash_dump_path(path, sizeof(path), NULL, 1));
    CHECK(!dcp_crash_dump_path(path, 0, "C:\\logs\\dragons_crown_pro_017.log", 1));
}

static void check_dump_file(const char *path, size_t *size_out) {
    size_t size = 0;
    char *bytes = read_file(path, &size);
    CHECK(bytes != NULL);
    if (!bytes) return;
    CHECK(size >= 32 && size <= MOST_DUMP_BYTES);
    CHECK(!memcmp(bytes, "MDMP", 4));
    if (size_out) *size_out = size;
    free(bytes);
}

int main(void) {
    char temp[MAX_PATH], dir[MAX_PATH], log[4200], dump[4300], second[4300], more[4300];
    int console_fd = _dup(1);
    if (console_fd < 0) console_fd = _dup(2);
    console = console_fd >= 0 ? _fdopen(console_fd, "w") : stderr;
    if (!console) return 90;
    setvbuf(console, NULL, _IONBF, 0);

    dump_paths();

    const DWORD length = GetTempPathA(sizeof(temp), temp);
    if (!length || length >= sizeof(temp)) return 1;
    snprintf(dir, sizeof(dir), "%sdcp_crash_dump_tests_%lu", temp, GetCurrentProcessId());
    CreateDirectoryA(dir, NULL);
    if (!runtime_log_start(dir, NULL, log, sizeof(log))) return 2;
    CHECK(strstr(log, "dragons_crown_pro_001.log") != NULL && !strcmp(runtime_log_session_path(), log));
    snprintf(dump, sizeof(dump), "%s\\dragons_crown_pro_001.dmp", dir);
    snprintf(second, sizeof(second), "%s\\dragons_crown_pro_001_2.dmp", dir);

    CONTEXT context;
    EXCEPTION_RECORD record;
    EXCEPTION_POINTERS info = {&record, &context};
    memset(&record, 0, sizeof(record));
    RtlCaptureContext(&context);
    record.ExceptionCode = 0xC0000005u;
    record.ExceptionAddress = (PVOID)(uintptr_t)context.Rip;

    /* Before the dump thread exists nothing is written, and the textual log says so. */
    CHECK(dcp_crash_dump_write(&info, GetCurrentThreadId()) == 0);
    CHECK(!exists(dump));
    size_t text_size = 0;
    runtime_log_flush();
    char *text = read_file(log, &text_size);
    CHECK(text && strstr(text, "minidump: not written (no crash thread)"));
    free(text);

    CHECK(dcp_crash_dump_install() == 1 && dcp_crash_dump_install() == 1);
    CHECK(dcp_crash_dump_write(NULL, GetCurrentThreadId()) == 0);

    /* The dump takes the exact name of the session log, and the log says where it is. */
    const DWORD started = GetTickCount();
    CHECK(dcp_crash_dump_write(&info, GetCurrentThreadId()) == 1);
    CHECK(GetTickCount() - started < 30000);
    CHECK(exists(dump) && !exists(second));
    size_t first_size = 0;
    check_dump_file(dump, &first_size);
    runtime_log_flush();
    text = read_file(log, &text_size);
    CHECK(text && strstr(text, "minidump: dragons_crown_pro_001.dmp (next to this log)"));
    free(text);

    /* A dump of an earlier crash is never overwritten: the next one takes the next name. */
    size_t kept_size = 0;
    char *kept = read_file(dump, &kept_size);
    CHECK(kept != NULL);
    CHECK(dcp_crash_dump_write(&info, GetCurrentThreadId()) == 1);
    CHECK(exists(second));
    check_dump_file(second, NULL);
    size_t after_size = 0;
    char *after = read_file(dump, &after_size);
    CHECK(kept && after && kept_size == after_size && kept_size == first_size && !memcmp(kept, after, kept_size));
    free(kept);
    free(after);
    runtime_log_flush();
    text = read_file(log, &text_size);
    CHECK(text && strstr(text, "minidump: dragons_crown_pro_001_2.dmp (next to this log)"));
    free(text);

    /* With every name taken the dump is skipped, logged, and nothing existing is touched. */
    for (int attempt = 3; attempt < 20; ++attempt) {
        snprintf(more, sizeof(more), "%s\\dragons_crown_pro_001_%d.dmp", dir, attempt);
        make_empty(more);
    }
    CHECK(dcp_crash_dump_write(&info, GetCurrentThreadId()) == 0);
    snprintf(more, sizeof(more), "%s\\dragons_crown_pro_001_3.dmp", dir);
    size_t untouched = 1;
    char *empty = read_file(more, &untouched);
    CHECK(empty != NULL && untouched == 0);
    free(empty);
    runtime_log_flush();
    text = read_file(log, &text_size);
    CHECK(text && strstr(text, "minidump: not written (the file could not be created)"));
    free(text);

    /* No session log, no dump: the report is still logged. */
    runtime_log_set_session_path("");
    CHECK(dcp_crash_dump_write(&info, GetCurrentThreadId()) == 0);
    runtime_log_flush();
    text = read_file(log, &text_size);
    CHECK(text && strstr(text, "minidump: not written (no dbghelp or no session log)"));
    free(text);

    runtime_log_set_session_path(log);
    runtime_log_flush();
    runtime_log_release();
    for (int attempt = 1; attempt < 20; ++attempt) {
        if (attempt == 1) snprintf(more, sizeof(more), "%s\\dragons_crown_pro_001.dmp", dir);
        else snprintf(more, sizeof(more), "%s\\dragons_crown_pro_001_%d.dmp", dir, attempt);
        DeleteFileA(more);
    }
    DeleteFileA(log);
    RemoveDirectoryA(dir);
    if (!failures) fprintf(console, "crash dump tests passed\n");
    return failures ? 1 : 0;
}
