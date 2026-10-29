#include "runtime_log.h"
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <string.h>

static HANDLE echo_source, echo_target;
static volatile LONG64 echoed;
static char direct_log_path[4200];

int runtime_log_write_direct(const char *text) {
    if (!text || !direct_log_path[0]) return 0;
    HANDLE file = CreateFileA(direct_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    const DWORD size = (DWORD)strlen(text);
    DWORD written = 0;
    const int ok = WriteFile(file, text, size, &written, NULL) && written == size && FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
}

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

static int redirect_to_log(const char *path, int append) {
    runtime_log_set_session_path(path);
    snprintf(direct_log_path, sizeof(direct_log_path), "%s", path);
    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE), original = NULL;
    if (console && console != INVALID_HANDLE_VALUE && GetFileType(console) != FILE_TYPE_UNKNOWN)
        DuplicateHandle(GetCurrentProcess(), console, GetCurrentProcess(), &original, 0, FALSE, DUPLICATE_SAME_ACCESS);
    fflush(NULL);
    FILE *out = freopen(path, append ? "a" : "w", stdout);
    FILE *err = out ? freopen(path, "a", stderr) : NULL;
    if (!out || !err) return 0;
    const int out_fd = _fileno(stdout), err_fd = _fileno(stderr);
    if (out_fd >= 0) _setmode(out_fd, _O_BINARY);
    if (err_fd >= 0) _setmode(err_fd, _O_BINARY);
    if (out_fd >= 0 && err_fd >= 0 && out_fd != err_fd) _dup2(out_fd, err_fd);
    if (out_fd >= 0) {
        _dup2(out_fd, 1);
        _dup2(out_fd, 2);
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (_fileno(stdin) < 0 && freopen("NUL", "r", stdin)) setvbuf(stdin, NULL, _IONBF, 0);
    HANDLE reader = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER existing = {0};
    if (append && reader != INVALID_HANDLE_VALUE && GetFileSizeEx(reader, &existing)) {
        SetFilePointerEx(reader, existing, NULL, FILE_BEGIN);
        InterlockedExchange64(&echoed, existing.QuadPart);
    }
    if (original && reader != INVALID_HANDLE_VALUE) {
        echo_source = reader;
        echo_target = original;
        HANDLE thread = CreateThread(NULL, 0, echo_log, NULL, 0, NULL);
        if (thread) CloseHandle(thread);
        else echo_source = NULL;
    }
    return 1;
}

int runtime_log_start(const char *dir, const char *file, char *path, size_t size) {
    void *created;
    if (!runtime_log_create(dir, file, path, size, &created)) return 0;
    CloseHandle((HANDLE)created);
    return redirect_to_log(path, 0);
}

int runtime_log_continue(const char *file, char *path, size_t size) {
    if (!file || GetFileAttributesA(file) == INVALID_FILE_ATTRIBUTES) return 0;
    if (snprintf(path, size, "%s", file) >= (int)size) return 0;
    return redirect_to_log(path, 1);
}

void runtime_log_release(void) {
    if (!direct_log_path[0]) return;
    fflush(NULL);
    freopen("NUL", "w", stdout);
    freopen("NUL", "w", stderr);
    for (int fd = 1; fd <= 2; ++fd) {
        const int nul = _open("NUL", _O_WRONLY | _O_BINARY);
        if (nul < 0) break;
        if (nul != fd) {
            _dup2(nul, fd);
            _close(nul);
        }
    }
}

void runtime_log_flush(void) {
    LARGE_INTEGER size;
    fflush(NULL);
    if (!echo_source || !GetFileSizeEx(echo_source, &size)) return;
    for (int wait = 0; wait < 200 && InterlockedCompareExchange64(&echoed, 0, 0) < size.QuadPart; ++wait) Sleep(5);
}
