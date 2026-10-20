#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <string.h>
#include "runtime_log.h"

static int read_all(const char *path, char *out, size_t size) {
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    DWORD read = 0;
    const int ok = ReadFile(file, out, (DWORD)(size - 1), &read, NULL) && read < size;
    CloseHandle(file);
    if (!ok) return 0;
    out[read] = 0;
    return 1;
}

int main(void) {
    char temp[MAX_PATH], dir[MAX_PATH], log[4200], text[8192];
    DWORD length = GetTempPathA(sizeof(temp), temp);
    if (!length || length >= sizeof(temp)) return 1;
    snprintf(dir, sizeof(dir), "%sdcp_log_tests_%lu", temp, GetCurrentProcessId());
    CreateDirectoryA(dir, NULL);

    fflush(NULL);
    _close(0);
    _close(1);
    _close(2);
    if (!runtime_log_start(dir, NULL, log, sizeof(log))) return 2;
    if (_fileno(stdin) < 0 || _fileno(stdout) < 0 || _fileno(stderr) < 0) return 3;

    printf("stdout line\n");
    fprintf(stderr, "stderr line\n");
    _write(1, "raw stdout\n", 11);
    _write(2, "raw stderr\n", 11);
    runtime_log_write_direct("direct line\n");
    fflush(NULL);
    runtime_log_flush();

    if (!read_all(log, text, sizeof(text))) return 4;
    if (!strstr(text, "stdout line") || !strstr(text, "stderr line") ||
        !strstr(text, "raw stdout") || !strstr(text, "raw stderr") ||
        !strstr(text, "direct line")) return 5;

    fclose(stdout);
    fclose(stderr);
    DeleteFileA(log);
    RemoveDirectoryA(dir);
    return 0;
}
