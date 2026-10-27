#include "module_diagnostics.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <windows.h>
#include <tlhelp32.h>

static uintptr_t *baseline;
static size_t baseline_count;
static int baseline_taken;
static DWORD baseline_error;

static HANDLE snapshot_modules(void) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
        if (snapshot != INVALID_HANDLE_VALUE) return snapshot;
        /* ERROR_BAD_LENGTH: a module was loading or unloading during the snapshot. */
        if (GetLastError() != ERROR_BAD_LENGTH) break;
    }
    return INVALID_HANDLE_VALUE;
}

static int in_baseline(uintptr_t base) {
    for (size_t i = 0; i < baseline_count; ++i)
        if (baseline[i] == base) return 1;
    return 0;
}

static int under_folder(const wchar_t *path, const wchar_t *folder, size_t length) {
    if (!length || _wcsnicmp(path, folder, length)) return 0;
    return folder[length - 1] == L'\\' || path[length] == L'\\';
}

void module_diagnostics_baseline(void) {
    HANDLE snapshot = snapshot_modules();
    if (snapshot == INVALID_HANDLE_VALUE) {
        baseline_error = GetLastError();
        return;
    }
    MODULEENTRY32W entry = {.dwSize = sizeof(entry)};
    size_t capacity = 0;
    for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry)) {
        if (baseline_count == capacity) {
            size_t grown = capacity ? capacity * 2 : 128;
            uintptr_t *larger = realloc(baseline, grown * sizeof(*baseline));
            if (!larger) {
                CloseHandle(snapshot);
                baseline_count = 0;
                baseline_error = ERROR_NOT_ENOUGH_MEMORY;
                return;
            }
            baseline = larger;
            capacity = grown;
        }
        baseline[baseline_count++] = (uintptr_t)entry.modBaseAddr;
    }
    CloseHandle(snapshot);
    baseline_taken = 1;
}

/* at_startup: the modules of the baseline itself, except this program; otherwise the ones loaded since. */
static void report(int at_startup, const char *module_label, const char *summary_label) {
    if (!baseline_taken) {
        printf("Modules: no startup list to compare with (error %lu)\n", baseline_error);
        return;
    }
    wchar_t windows[MAX_PATH];
    UINT windows_length = GetSystemWindowsDirectoryW(windows, MAX_PATH);
    if (windows_length >= MAX_PATH) windows_length = 0;
    HANDLE snapshot = snapshot_modules();
    if (snapshot == INVALID_HANDLE_VALUE) {
        printf("Modules: list unavailable (error %lu)\n", GetLastError());
        return;
    }
    MODULEENTRY32W entry = {.dwSize = sizeof(entry)};
    const HMODULE self = GetModuleHandleW(NULL);
    unsigned external = 0, system = 0;
    for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry)) {
        if (in_baseline((uintptr_t)entry.modBaseAddr) != at_startup || entry.hModule == self) continue;
        if (under_folder(entry.szExePath, windows, windows_length)) {
            ++system;
            continue;
        }
        char path[MAX_PATH * 3];
        if (!WideCharToMultiByte(CP_UTF8, 0, entry.szExePath, -1, path, sizeof(path), NULL, NULL))
            snprintf(path, sizeof(path), "?");
        printf("Modules: %s: %s at %p\n", module_label, path, (void *)entry.modBaseAddr);
        ++external;
    }
    CloseHandle(snapshot);
    printf("Modules: %s: %u outside the Windows folder, %u inside it\n", summary_label, external, system);
}

void module_diagnostics_report_startup(void) {
    report(1, "present at startup", "present at startup");
}

void module_diagnostics_report(void) {
    report(0, "external module loaded", "loaded since startup");
}
