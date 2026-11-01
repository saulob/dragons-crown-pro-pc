#include "call_site.h"
#include "fault_origin.h"
#include "module_history.h"
#include "startup_phase.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(condition) do { if (!(condition)) { printf("FAIL line %d: %s\n", __LINE__, #condition); ++failures; } } while (0)

static ModuleHistory history;

static void loaded_modules(void) {
    memset(&history, 0, sizeof(history));
    module_history_loaded(&history, 0x7ff900000000, 0x100000, "ntdll.dll", NULL);
    module_history_loaded(&history, 0x7ff980000000, 0x40000, "overlay.dll", "C:\\Tools\\overlay.dll");
    char text[160];
    CHECK(module_history_describe(&history, 0x7ff900001234, 100, text, sizeof(text)));
    CHECK(strcmp(text, "ntdll.dll+0x1234") == 0);
    ModuleMatch match;
    CHECK(module_history_find(&history, 0x7ff980000010, &match));
    CHECK(!match.unloaded && match.record->external && match.offset == 0x10);
    CHECK(strcmp(match.record->path, "C:\\Tools\\overlay.dll") == 0);
    CHECK(!module_history_find(&history, 0x7ff900100000, &match));
}

static void unloaded_modules(void) {
    memset(&history, 0, sizeof(history));
    module_history_loaded(&history, 0x7ff9a7200000, 0x200000, "gone.dll", NULL);
    module_history_unloaded(&history, 0x7ff9a7200000, 0, NULL, NULL, 1000, "launcher SDL shutdown");
    char text[200];
    CHECK(module_history_describe(&history, 0x7ff9a72ef215, 1018, text, sizeof(text)));
    CHECK(strcmp(text, "UNLOADED gone.dll+0xef215, unloaded 18 ms ago during launcher SDL shutdown") == 0);
    ModuleMatch match;
    CHECK(module_history_find(&history, 0x7ff9a7200000, &match) && match.unloaded);

    module_history_loaded(&history, 0x7ff9a7200000, 0x1000, "again.dll", NULL);
    CHECK(module_history_find(&history, 0x7ff9a7200010, &match) && !match.unloaded);
    CHECK(strcmp(match.record->name, "again.dll") == 0);
    CHECK(module_history_find(&history, 0x7ff9a72ef215, &match) && match.unloaded);

    module_history_unloaded(&history, 0x7ff900000000, 0x1000, "never-seen.dll", "C:\\x\\never-seen.dll", 5, NULL);
    CHECK(module_history_describe(&history, 0x7ff900000008, 4, text, sizeof(text)));
    CHECK(strstr(text, "unloaded 0 ms ago during an unknown phase") != NULL);
    module_history_unloaded(&history, 0x7ff900500000, 0, NULL, NULL, 5, NULL);
    CHECK(history.unload_total == 2);
}

static void recent_unloads_wrap(void) {
    memset(&history, 0, sizeof(history));
    for (unsigned i = 0; i < MODULE_UNLOADS_MAX + 5; ++i)
        module_history_unloaded(&history, 0x10000000 + (uintptr_t)i * 0x10000, 0x1000, "m.dll", NULL, i, "phase");
    const ModuleRecord *recent[MODULE_UNLOADS_MAX + 5];
    CHECK(module_history_recent_unloads(&history, recent, 3) == 3);
    CHECK(recent[0]->unloaded_ms == MODULE_UNLOADS_MAX + 4 && recent[2]->unloaded_ms == MODULE_UNLOADS_MAX + 2);
    CHECK(module_history_recent_unloads(&history, recent, MODULE_UNLOADS_MAX + 5) == MODULE_UNLOADS_MAX);
    ModuleMatch match;
    CHECK(!module_history_find(&history, 0x10000000, &match));
    CHECK(module_history_find(&history, 0x10000000 + (uintptr_t)(MODULE_UNLOADS_MAX + 4) * 0x10000, &match) && match.unloaded);
}

static void table_full(void) {
    memset(&history, 0, sizeof(history));
    for (unsigned i = 0; i < MODULE_LOADED_MAX + 3; ++i)
        module_history_loaded(&history, 0x20000000 + (uintptr_t)i * 0x10000, 0x1000, "m.dll", NULL);
    CHECK(history.dropped == 3);
    ModuleMatch match;
    CHECK(module_history_find(&history, 0x20000000 + (uintptr_t)(MODULE_LOADED_MAX - 1) * 0x10000, &match));
    CHECK(!module_history_find(&history, 0x20000000 + (uintptr_t)MODULE_LOADED_MAX * 0x10000, &match));
    module_history_unloaded(&history, 0x20000000, 0, NULL, NULL, 1, "phase");
    module_history_loaded(&history, 0x30000000, 0x1000, "late.dll", NULL);
    CHECK(history.dropped == 3 && module_history_find(&history, 0x30000000, &match));
}

static void no_match(void) {
    memset(&history, 0, sizeof(history));
    module_history_loaded(&history, 0x1000000, 0x1000, "low.dll", NULL);
    module_history_loaded(&history, 0x3000000, 0x1000, "high.dll", NULL);
    char text[64] = "stale";
    CHECK(!module_history_describe(&history, 0x2000000, 0, text, sizeof(text)) && text[0] == 0);
    CHECK(!module_history_describe(&history, 0, 0, text, sizeof(text)));
    const ModuleRecord *below, *above;
    module_history_neighbors(&history, 0x2000000, &below, &above);
    CHECK(below && strcmp(below->name, "low.dll") == 0 && above && strcmp(above->name, "high.dll") == 0);
    module_history_neighbors(&history, 0x4000000, &below, &above);
    CHECK(below && strcmp(below->name, "high.dll") == 0 && !above);
    module_history_neighbors(&history, 0x1000800, &below, &above);
    CHECK(!below && above);
    char small[8];
    module_history_loaded(&history, 0x5000000, 0x1000, "a-very-long-module-name-that-does-not-fit-in-the-record.dll", NULL);
    ModuleMatch match;
    CHECK(module_history_find(&history, 0x5000000, &match) && strlen(match.record->name) == MODULE_NAME_SIZE - 1);
    CHECK(module_history_describe(&history, 0x5000001, 0, small, sizeof(small)) && strlen(small) == sizeof(small) - 1);
}

static int decode(const char *hex, CallSite *site, char *text, size_t size) {
    uint8_t bytes[16];
    size_t count = 0;
    for (const char *p = hex; *p && count < sizeof(bytes); p += p[2] ? 3 : 2) {
        unsigned value;
        sscanf(p, "%2x", &value);
        bytes[count++] = (uint8_t)value;
    }
    if (!call_site_decode(bytes, count, site)) return 0;
    call_site_format(site, text, size);
    return 1;
}

static void call_sites(void) {
    CallSite site;
    char text[64];
    CHECK(decode("48 8b 40 58 e8 62 34 0e 00", &site, text, sizeof(text)) && site.kind == CALL_SITE_DIRECT);
    CHECK(site.length == 5 && site.displacement == 0xe3462 && strcmp(text, "call rel 0xe3462") == 0);
    CHECK(decode("48 8b 01 ff d0", &site, text, sizeof(text)) && site.kind == CALL_SITE_REGISTER && site.reg == 0);
    CHECK(strcmp(text, "call rax") == 0);
    CHECK(decode("90 41 ff d3", &site, text, sizeof(text)) && site.reg == 11 && strcmp(text, "call r11") == 0);
    CHECK(decode("48 8b 07 ff 50 58", &site, text, sizeof(text)) && site.kind == CALL_SITE_MEMORY);
    CHECK(site.reg == 0 && site.displacement == 0x58 && strcmp(text, "call [rax+88]") == 0);
    CHECK(decode("90 90 ff 90 00 01 00 00", &site, text, sizeof(text)) && site.reg == 0 && site.displacement == 0x100);
    CHECK(decode("90 41 ff 54 24 08", &site, text, sizeof(text)) && site.reg == -1);
    CHECK(decode("90 90 ff 10", &site, text, sizeof(text)) && site.reg == 0 && strcmp(text, "call [rax]") == 0);
    CHECK(decode("90 ff 15 10 00 00 00", &site, text, sizeof(text)) && site.kind == CALL_SITE_RIP_MEMORY);
    CHECK(site.displacement == 16 && strcmp(text, "call [rip+16]") == 0);
    CHECK(decode("90 90 90 ff e0", &site, text, sizeof(text)) && site.is_jump && strcmp(text, "jmp rax") == 0);
    CHECK(decode("90 90 41 ff 55 f8", &site, text, sizeof(text)) && site.reg == 13 && site.displacement == -8);
    CHECK(!decode("90 90 90 90 90 90 90 90", &site, text, sizeof(text)));
    CHECK(!decode("ff", &site, text, sizeof(text)));
    CHECK(!decode("ff d0 90", &site, text, sizeof(text)));
}

static void fault_ownership(void) {
    CHECK(fault_is_ours(FAULT_ADDRESS_GUEST, FAULT_ADDRESS_UNLISTED));
    CHECK(fault_is_ours(FAULT_ADDRESS_SELF, FAULT_ADDRESS_OTHER_MODULE));
    CHECK(!fault_is_ours(FAULT_ADDRESS_OTHER_MODULE, FAULT_ADDRESS_SELF));
    CHECK(fault_is_ours(FAULT_ADDRESS_UNLISTED, FAULT_ADDRESS_GUEST));
    CHECK(fault_is_ours(FAULT_ADDRESS_UNLISTED, FAULT_ADDRESS_SELF));
    CHECK(fault_is_ours(FAULT_ADDRESS_UNLISTED, FAULT_ADDRESS_UNLISTED));
    CHECK(!fault_is_ours(FAULT_ADDRESS_UNLISTED, FAULT_ADDRESS_OTHER_MODULE));
}

static void phases(void) {
    CHECK(strcmp(dcp_startup_phase_current(), "process start") == 0);
    dcp_startup_phase("runtime window closed");
    CHECK(strcmp(dcp_startup_phase_current(), "runtime window closed") == 0);
    dcp_startup_phase("process exit");
    CHECK(strcmp(dcp_startup_phase_current(), "process exit") == 0);
}

int main(void) {
    phases();
    call_sites();
    fault_ownership();
    loaded_modules();
    unloaded_modules();
    recent_unloads_wrap();
    table_full();
    no_match();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    puts("module history tests passed");
    return 0;
}
