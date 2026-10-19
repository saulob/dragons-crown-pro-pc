#include "runtime_log.h"
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <string.h>

static HANDLE echo_source, echo_target;
static volatile LONG64 echoed;

static void make_directories(const char *dir) {
    char path[4096];
    snprintf(path, sizeof(path), "%s", dir);
    for (char *p = path; *p; ++p) {
        if ((*p == '\\' || *p == '/') && p > path && p[-1] != ':') {
            char separator = *p;
            *p = 0;
            CreateDirectoryA(path, NULL);
            *p = separator;
        }
    }
    CreateDirectoryA(path, NULL);
}

static unsigned last_number(const char *dir) {
    char pattern[4200];
    snprintf(pattern, sizeof(pattern), "%s\\dragons_crown_pro_*.log", dir);
    WIN32_FIND_DATAA found;
    HANDLE find = FindFirstFileA(pattern, &found);
    unsigned last = 0;
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        unsigned number = 0;
        int used = 0;
        if (sscanf(found.cFileName, "dragons_crown_pro_%u.log%n", &number, &used) == 1 && used > 0 && !found.cFileName[used] && number > last)
            last = number;
    } while (FindNextFileA(find, &found));
    FindClose(find);
    return last;
}

int runtime_log_create(const char *dir, const char *file, char *path, size_t size, void **handle) {
    if (file) {
        if (snprintf(path, size, "%s", file) >= (int)size) return 0;
        char *separator = strrchr(path, '\\');
        if (strrchr(path, '/') > separator) separator = strrchr(path, '/');
        if (separator) {
            char folder = *separator;
            *separator = 0;
            make_directories(path);
            *separator = folder;
        }
        *handle = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        return *handle != INVALID_HANDLE_VALUE;
    }
    make_directories(dir);
    for (unsigned number = last_number(dir) + 1, tries = 0; tries < 1000; ++number, ++tries) {
        if (snprintf(path, size, "%s\\dragons_crown_pro_%03u.log", dir, number) >= (int)size) return 0;
        *handle = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (*handle != INVALID_HANDLE_VALUE) return 1;
        if (GetLastError() != ERROR_FILE_EXISTS) return 0;
    }
    return 0;
}

static DWORD WINAPI echo_log(LPVOID unused) {
    (void)unused;
    char buffer[4096];
    for (;;) {
        DWORD read = 0, written = 0;
        if (ReadFile(echo_source, buffer, sizeof(buffer), &read, NULL) && read) {
            WriteFile(echo_target, buffer, read, &written, NULL);
            InterlockedAdd64(&echoed, read);
        } else {
            Sleep(5);
        }
    }
}

int runtime_log_start(const char *dir, const char *file, char *path, size_t size) {
    void *log;
    if (!runtime_log_create(dir, file, path, size, &log)) return 0;
    runtime_log_set_session_path(path);
    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE), original = NULL;
    /* _dup2 below closes the handle that fd 1 owns now. */
    if (console && console != INVALID_HANDLE_VALUE)
        DuplicateHandle(GetCurrentProcess(), console, GetCurrentProcess(), &original, 0, FALSE, DUPLICATE_SAME_ACCESS);
    int fd = _open_osfhandle((intptr_t)log, _O_WRONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(log);
        return 0;
    }
    fflush(NULL);
    _dup2(fd, 1);
    _dup2(fd, 2);
    _close(fd);
    HANDLE reader = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (original && reader != INVALID_HANDLE_VALUE) {
        echo_source = reader;
        echo_target = original;
        HANDLE thread = CreateThread(NULL, 0, echo_log, NULL, 0, NULL);
        if (thread) CloseHandle(thread);
        else echo_source = NULL;
    }
    return 1;
}

void runtime_log_flush(void) {
    LARGE_INTEGER size;
    fflush(NULL);
    if (!echo_source || !GetFileSizeEx(echo_source, &size)) return;
    for (int wait = 0; wait < 200 && InterlockedCompareExchange64(&echoed, 0, 0) < size.QuadPart; ++wait) Sleep(5);
}
