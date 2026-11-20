#include "crash_dump.h"

#include "runtime_log.h"

#include <dbghelp.h>
#include <stdio.h>
#include <string.h>

#define DUMP_TIMEOUT_MS 60000
#define DUMP_ATTEMPTS 20

typedef BOOL(WINAPI *WriteDump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

static WriteDump write_minidump;
static HANDLE dump_thread, request_event, done_event;
static EXCEPTION_POINTERS *volatile request;
static volatile DWORD request_thread;
static volatile LONG busy, written;
static char dump_path[4300], dump_comment[1100], dump_line[4400];

int dcp_crash_dump_path(char *out, size_t size, const char *log_path, int attempt) {
    if (!out || !size || !log_path) return 0;
    const char *dot = strrchr(log_path, '.');
    const char *slash = strrchr(log_path, '\\');
    const char *forward = strrchr(log_path, '/');
    if (forward && (!slash || forward > slash)) slash = forward;
    if (!dot || (slash && dot < slash)) return 0;
    const size_t stem = (size_t)(dot - log_path);
    const int length = attempt >= 2 ? snprintf(out, size, "%.*s_%d.dmp", (int)stem, log_path, attempt)
                                    : snprintf(out, size, "%.*s.dmp", (int)stem, log_path);
    return length > 0 && (size_t)length < size;
}

static const char *file_name(const char *path) {
    const char *name = path;
    for (const char *p = path; *p; ++p)
        if (*p == '\\' || *p == '/') name = p + 1;
    return name;
}

static void say(const char *text) { runtime_log_write_direct(text); }

static HANDLE create_dump_file(const char *log_path) {
    for (int attempt = 1; attempt < DUMP_ATTEMPTS; ++attempt) {
        if (!dcp_crash_dump_path(dump_path, sizeof(dump_path), log_path, attempt)) return INVALID_HANDLE_VALUE;
        HANDLE file = CreateFileA(dump_path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) return file;
        if (GetLastError() != ERROR_FILE_EXISTS) return INVALID_HANDLE_VALUE;
    }
    return INVALID_HANDLE_VALUE;
}

static int write_dump(void) {
    const char *log_path = runtime_log_session_path();
    if (!write_minidump || !log_path[0]) {
        say("  minidump: not written (no dbghelp or no session log)\n");
        return 0;
    }
    HANDLE file = create_dump_file(log_path);
    if (file == INVALID_HANDLE_VALUE) {
        say("  minidump: not written (the file could not be created)\n");
        return 0;
    }
    snprintf(dump_comment, sizeof(dump_comment), "Dragon's Crown Pro PC crash dump: process %lu, exception 0x%08lx on thread %lu, log %s",
             GetCurrentProcessId(), request ? request->ExceptionRecord->ExceptionCode : 0ul, (unsigned long)request_thread,
             file_name(log_path));
    MINIDUMP_USER_STREAM stream = {CommentStreamA, (ULONG)strlen(dump_comment) + 1, dump_comment};
    MINIDUMP_USER_STREAM_INFORMATION streams = {1, &stream};
    MINIDUMP_EXCEPTION_INFORMATION exception = {request_thread, request, FALSE};
    const MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                               MiniDumpWithProcessThreadData | MiniDumpWithFullMemoryInfo);
    const BOOL ok = write_minidump(GetCurrentProcess(), GetCurrentProcessId(), file, type, request ? &exception : NULL, &streams, NULL);
    const DWORD error = ok ? 0 : GetLastError();
    CloseHandle(file);
    if (ok) {
        snprintf(dump_line, sizeof(dump_line), "  minidump: %s (next to this log)\n", file_name(dump_path));
    } else {
        snprintf(dump_line, sizeof(dump_line), "  minidump: %s failed, error 0x%08lx\n", file_name(dump_path), error);
        DeleteFileA(dump_path);
    }
    say(dump_line);
    return ok ? 1 : 0;
}

static DWORD WINAPI dump_main(LPVOID unused) {
    (void)unused;
    for (;;) {
        WaitForSingleObject(request_event, INFINITE);
        InterlockedExchange(&written, write_dump());
        SetEvent(done_event);
    }
}

int dcp_crash_dump_install(void) {
    if (dump_thread) return 1;
    const HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    write_minidump = dbghelp ? (WriteDump)(void *)GetProcAddress(dbghelp, "MiniDumpWriteDump") : NULL;
    request_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    done_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (write_minidump && request_event && done_event)
        dump_thread = CreateThread(NULL, 256 * 1024, dump_main, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    return dump_thread != NULL;
}

int dcp_crash_dump_write(EXCEPTION_POINTERS *info, DWORD thread) {
    if (!dump_thread || !info) {
        say("  minidump: not written (no crash thread)\n");
        return 0;
    }
    if (InterlockedCompareExchange(&busy, 1, 0)) return 0;
    request = info;
    request_thread = thread;
    InterlockedExchange(&written, 0);
    ResetEvent(done_event);
    SetEvent(request_event);
    if (WaitForSingleObject(done_event, DUMP_TIMEOUT_MS) != WAIT_OBJECT_0) {
        say("  minidump: not finished in 60 s; the process ends without waiting\n");
        return 0;
    }
    InterlockedExchange(&busy, 0);
    return written != 0;
}
