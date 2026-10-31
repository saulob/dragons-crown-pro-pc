#include "module_diagnostics.h"
#include "call_site.h"
#include "module_history.h"
#include "startup_phase.h"

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

static void history_start(void);

void module_diagnostics_baseline(void) {
    history_start();
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

/* The crash report reads this without the loader lock, so it is filled from a loader notification. */
typedef struct { USHORT length, maximum; PWSTR buffer; } NotificationString;
typedef struct {
    ULONG flags;
    const NotificationString *full_name, *base_name;
    PVOID dll_base;
    ULONG image_size;
} ModuleNotification;
typedef VOID (CALLBACK *NotificationCallback)(ULONG reason, const ModuleNotification *data, PVOID context);
typedef LONG (NTAPI *RegisterNotificationFn)(ULONG flags, NotificationCallback callback, PVOID context, PVOID *cookie);
typedef LONG (NTAPI *QueryThreadFn)(HANDLE thread, ULONG information_class, PVOID buffer, ULONG length, PULONG returned);

enum { NOTIFICATION_LOADED = 1, NOTIFICATION_UNLOADED = 2, THREAD_WIN32_START_ADDRESS = 9, RECENT_UNLOADS_SHOWN = 8,
       EXTERNAL_MODULES_SHOWN = 24, MIN_USER_ADDRESS = 0x10000, BACKTRACE_FRAMES = 12 };

static ModuleHistory history;
static DWORD main_thread_id;
static uintptr_t self_base;
static QueryThreadFn query_thread;
static wchar_t windows_folder[MAX_PATH];
static UINT windows_folder_length;

static void narrow(char *out, size_t size, const wchar_t *text, size_t length) {
    size_t n = 0;
    for (; n + 1 < size && n < length; ++n) out[n] = text[n] > 0 && text[n] < 128 ? (char)text[n] : '?';
    out[n] = 0;
}

static int in_windows_folder(const wchar_t *path, size_t length) {
    return windows_folder_length && length > windows_folder_length && !_wcsnicmp(path, windows_folder, windows_folder_length) &&
           path[windows_folder_length] == L'\\';
}

static void note_module(const NotificationString *base_name, const NotificationString *full_name, uintptr_t base, size_t size,
                        int unloaded) {
    char name[MODULE_NAME_SIZE], path[MODULE_PATH_SIZE];
    narrow(name, sizeof(name), base_name->buffer, base_name->length / sizeof(wchar_t));
    const int external = !in_windows_folder(full_name->buffer, full_name->length / sizeof(wchar_t));
    if (external) narrow(path, sizeof(path), full_name->buffer, full_name->length / sizeof(wchar_t));
    if (unloaded)
        module_history_unloaded(&history, base, size, name, external ? path : NULL, GetTickCount64(), dcp_startup_phase_current());
    else
        module_history_loaded(&history, base, size, name, external ? path : NULL);
}

static VOID CALLBACK on_module_event(ULONG reason, const ModuleNotification *data, PVOID context) {
    (void)context;
    if ((reason != NOTIFICATION_LOADED && reason != NOTIFICATION_UNLOADED) || !data->base_name || !data->full_name) return;
    note_module(data->base_name, data->full_name, (uintptr_t)data->dll_base, data->image_size, reason == NOTIFICATION_UNLOADED);
}

static void history_start(void) {
    main_thread_id = GetCurrentThreadId();
    self_base = (uintptr_t)GetModuleHandleW(NULL);
    windows_folder_length = GetSystemWindowsDirectoryW(windows_folder, MAX_PATH);
    if (windows_folder_length >= MAX_PATH) windows_folder_length = 0;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    RegisterNotificationFn register_notification =
        ntdll ? (RegisterNotificationFn)(void *)GetProcAddress(ntdll, "LdrRegisterDllNotification") : NULL;
    query_thread = ntdll ? (QueryThreadFn)(void *)GetProcAddress(ntdll, "NtQueryInformationThread") : NULL;
    HANDLE snapshot = snapshot_modules();
    if (snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W entry = {.dwSize = sizeof(entry)};
        for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry)) {
            char name[MODULE_NAME_SIZE], path[MODULE_PATH_SIZE];
            const int external = !in_windows_folder(entry.szExePath, wcslen(entry.szExePath));
            narrow(name, sizeof(name), entry.szModule, wcslen(entry.szModule));
            if (external) narrow(path, sizeof(path), entry.szExePath, wcslen(entry.szExePath));
            module_history_loaded(&history, (uintptr_t)entry.modBaseAddr, entry.modBaseSize, name, external ? path : NULL);
        }
        CloseHandle(snapshot);
    }
    PVOID cookie = NULL;
    if (register_notification) register_notification(0, on_module_event, NULL, &cookie);
}

static const char *protection_name(DWORD protect) {
    switch (protect & 0xff) {
    case PAGE_NOACCESS: return "---";
    case PAGE_READONLY: return "R--";
    case PAGE_READWRITE: case PAGE_WRITECOPY: return "RW-";
    case PAGE_EXECUTE: return "--X";
    case PAGE_EXECUTE_READ: return "R-X";
    case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return "RWX";
    }
    return "?";
}

static const char *memory_type_name(DWORD type) {
    return type == MEM_IMAGE ? "image" : type == MEM_MAPPED ? "mapped" : "private";
}

int module_diagnostics_describe(char *out, size_t size, uintptr_t address) {
    out[0] = 0;
    if (address < MIN_USER_ADDRESS) return 0;
    if (module_history_describe(&history, address, GetTickCount64(), out, size)) return 1;
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery((void *)address, &info, sizeof(info))) return 0;
    if (info.State == MEM_FREE) snprintf(out, size, "unmapped");
    else if (info.State == MEM_RESERVE) snprintf(out, size, "reserved %s", memory_type_name(info.Type));
    else snprintf(out, size, "%s %s", memory_type_name(info.Type), protection_name(info.Protect));
    return 1;
}

static void report_memory(const char *label, uintptr_t address) {
    MEMORY_BASIC_INFORMATION info;
    if (address < MIN_USER_ADDRESS || !VirtualQuery((void *)address, &info, sizeof(info))) {
        fprintf(stderr, "Crash: %s memory not queryable\n", label);
        return;
    }
    if (info.State == MEM_FREE)
        fprintf(stderr, "Crash: %s memory free, region base %p, region size 0x%llx\n", label, info.BaseAddress,
                (unsigned long long)info.RegionSize);
    else
        fprintf(stderr, "Crash: %s memory %s, %s, %s, allocation base %p, region base %p, region size 0x%llx\n", label,
                info.State == MEM_COMMIT ? "committed" : "reserved", memory_type_name(info.Type),
                info.State == MEM_COMMIT ? protection_name(info.Protect) : "no access", info.AllocationBase, info.BaseAddress,
                (unsigned long long)info.RegionSize);
    ModuleMatch match;
    if (module_history_find(&history, address, &match)) return;
    const ModuleRecord *below, *above;
    module_history_neighbors(&history, address, &below, &above);
    if (below)
        fprintf(stderr, "Crash: %s nearest module below: %s ends 0x%llx before it\n", label, below->name,
                (unsigned long long)(address - below->base - below->size));
    if (above)
        fprintf(stderr, "Crash: %s nearest module above: %s starts 0x%llx after it\n", label, above->name,
                (unsigned long long)(above->base - address));
    if (!below && !above) fprintf(stderr, "Crash: %s no module is known near it\n", label);
}

static void report_address(const char *label, uintptr_t address, uintptr_t guest_base, size_t guest_size) {
    char text[320];
    if (guest_base && address - guest_base < guest_size) {
        fprintf(stderr, "Crash: %s %p guest offset 0x%llx\n", label, (void *)address, (unsigned long long)(address - guest_base));
        return;
    }
    if (module_history_describe(&history, address, GetTickCount64(), text, sizeof(text)))
        fprintf(stderr, "Crash: %s %p %s\n", label, (void *)address, text);
    else
        fprintf(stderr, "Crash: %s %p is in no listed module\n", label, (void *)address);
    report_memory(label, address);
}

void module_diagnostics_report_crash(uintptr_t rip, uintptr_t target, uintptr_t guest_base, size_t guest_size) {
    const DWORD thread = GetCurrentThreadId();
    fprintf(stderr, "Crash: startup phase: %s\n", dcp_startup_phase_current());
    fprintf(stderr, "Crash: thread %lu, %s\n", thread, thread == main_thread_id ? "main thread" : "not main thread");
    uintptr_t start = 0;
    if (query_thread && query_thread(GetCurrentThread(), THREAD_WIN32_START_ADDRESS, &start, sizeof(start), NULL) == 0 && start)
        report_address("thread start", start, guest_base, guest_size);
    report_address("RIP", rip, guest_base, guest_size);
    if (target != rip) report_address("fault target", target, guest_base, guest_size);

    const ModuleRecord *recent[RECENT_UNLOADS_SHOWN];
    const unsigned unloads = module_history_recent_unloads(&history, recent, RECENT_UNLOADS_SHOWN);
    const ULONGLONG now = GetTickCount64();
    for (unsigned i = 0; i < unloads; ++i)
        fprintf(stderr, "Crash: recent unload %s at %p, size 0x%llx, %llu ms ago during %s\n", recent[i]->name,
                (void *)recent[i]->base, (unsigned long long)recent[i]->size,
                (unsigned long long)(now >= recent[i]->unloaded_ms ? now - recent[i]->unloaded_ms : 0),
                recent[i]->unloaded_phase ? recent[i]->unloaded_phase : "an unknown phase");
    if (!unloads) fprintf(stderr, "Crash: no module was unloaded since startup\n");
    if (history.dropped) fprintf(stderr, "Crash: %lu modules were not recorded\n", history.dropped);

    unsigned shown = 0, hidden = 0;
    for (size_t i = 0; i < MODULE_LOADED_MAX; ++i) {
        const ModuleRecord *record = &history.loaded[i];
        if (!record->base || !record->external || record->base == self_base) continue;
        if (shown == EXTERNAL_MODULES_SHOWN) { ++hidden; continue; }
        ++shown;
        fprintf(stderr, "Crash: external module %s at %p, size 0x%llx\n", record->path, (void *)record->base,
                (unsigned long long)record->size);
    }
    if (hidden) fprintf(stderr, "Crash: %u more external modules not listed\n", hidden);
}

static int read_memory(uintptr_t address, void *out, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void *)address, out, size, &got) && got == size;
}

static void describe_pc(char *out, size_t size, uintptr_t pc, uintptr_t guest_base, size_t guest_size) {
    if (guest_base && pc - guest_base < guest_size) snprintf(out, size, "guest offset 0x%llx", (unsigned long long)(pc - guest_base));
    else if (!module_diagnostics_describe(out, size, pc)) out[0] = 0;
}

/* The instruction that made the call into an address no function covers, from the return address on the stack. */
static void report_call_site(const CONTEXT *context, uintptr_t return_address, uintptr_t guest_base, size_t guest_size) {
    uint8_t bytes[9];
    CallSite site;
    char text[320], target[320];
    if (!read_memory(return_address - sizeof(bytes), bytes, sizeof(bytes))) return;
    fprintf(stderr, "Crash: call site bytes %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", bytes[0], bytes[1], bytes[2], bytes[3],
            bytes[4], bytes[5], bytes[6], bytes[7], bytes[8]);
    if (!call_site_decode(bytes, sizeof(bytes), &site)) return;
    call_site_format(&site, text, sizeof(text));
    if (site.kind == CALL_SITE_DIRECT) {
        const uintptr_t destination = return_address + (intptr_t)site.displacement;
        describe_pc(target, sizeof(target), destination, guest_base, guest_size);
        fprintf(stderr, "Crash: call site %s -> %p %s\n", text, (void *)destination, target);
    } else if (site.kind == CALL_SITE_REGISTER) {
        fprintf(stderr, "Crash: call site %s = %llx\n", text, (unsigned long long)(&context->Rax)[site.reg]);
    } else if (site.reg >= 0 || site.kind == CALL_SITE_RIP_MEMORY) {
        const uintptr_t base = site.kind == CALL_SITE_RIP_MEMORY ? return_address : (uintptr_t)(&context->Rax)[site.reg];
        const uintptr_t slot = base + (intptr_t)site.displacement;
        uint64_t value = 0;
        if (read_memory(slot, &value, sizeof(value)))
            fprintf(stderr, "Crash: call site %s, slot %p holds %llx\n", text, (void *)slot, (unsigned long long)value);
        else
            fprintf(stderr, "Crash: call site %s, slot %p unreadable\n", text, (void *)slot);
    } else {
        fprintf(stderr, "Crash: call site %s\n", text);
    }
}

/* Native frames from the exception context with the unwind data of the loaded modules; no DbgHelp and no allocation. */
void module_diagnostics_report_backtrace(const CONTEXT *fault, uintptr_t guest_base, size_t guest_size) {
    const NT_TIB *tib = (const NT_TIB *)NtCurrentTeb();
    const uintptr_t stack_low = (uintptr_t)tib->StackLimit, stack_high = (uintptr_t)tib->StackBase;
    CONTEXT context = *fault;
    char text[320];
    for (unsigned frame = 0; frame < BACKTRACE_FRAMES; ++frame) {
        const uintptr_t pc = (uintptr_t)context.Rip;
        describe_pc(text, sizeof(text), pc, guest_base, guest_size);
        fprintf(stderr, "Crash: frame %u %p %s\n", frame, (void *)pc, text);
        if (context.Rsp < stack_low || context.Rsp + sizeof(uint64_t) > stack_high) break;
        DWORD64 image_base = 0;
        const RUNTIME_FUNCTION *function = RtlLookupFunctionEntry(frame ? pc - 1 : pc, &image_base, NULL);
        const uintptr_t previous_sp = (uintptr_t)context.Rsp;
        if (function) {
            PVOID handler_data = NULL;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, pc, (PRUNTIME_FUNCTION)function, &context, &handler_data, &establisher, NULL);
        } else {
            ModuleMatch known;
            uint64_t caller = 0;
            const int in_guest = guest_base && pc - guest_base < guest_size;
            if (frame != 0 || in_guest || (module_history_find(&history, pc, &known) && !known.unloaded)) break;
            if (!read_memory((uintptr_t)context.Rsp, &caller, sizeof(caller))) break;
            report_call_site(&context, (uintptr_t)caller, guest_base, guest_size);
            context.Rip = caller;
            context.Rsp += sizeof(caller);
        }
        if (!context.Rip || (uintptr_t)context.Rsp <= previous_sp) break;
    }
}
