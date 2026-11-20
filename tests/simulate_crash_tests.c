#include "simulate_crash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define SET_ENV(value) _putenv("DCP_SIMULATE_CRASH=" value)
#else
#define SET_ENV(value) setenv("DCP_SIMULATE_CRASH", value, 1)
#endif

static int failures;
#define CHECK(condition) do { if (!(condition)) { printf("FAILED: %s (line %d)\n", #condition, __LINE__); ++failures; } } while (0)

static char *read_source(const char *relative) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", DCP_SOURCE_DIR, relative);
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    rewind(file);
    char *text = malloc((size_t)size + 1);
    if (text && fread(text, 1, (size_t)size, file) == (size_t)size) text[size] = 0;
    else if (text) { free(text); text = NULL; }
    fclose(file);
    return text;
}

static const char *after(const char *text, const char *marker, const char *from) {
    const char *found = strstr(from ? from : text, marker);
    return found ? found + strlen(marker) : NULL;
}

int main(void) {
    CHECK(!dcp_simulate_crash_requested(NULL));
    CHECK(!dcp_simulate_crash_requested(""));
    static const char *const others[] = {"0", "2", "true", "yes", "01", "11", " 1", "1 ", "on"};
    for (size_t i = 0; i < sizeof(others) / sizeof(*others); ++i) CHECK(!dcp_simulate_crash_requested(others[i]));
    CHECK(dcp_simulate_crash_requested("1"));

    SET_ENV("");
    dcp_simulate_crash_from_environment();
    SET_ENV("0");
    dcp_simulate_crash_from_environment();
    SET_ENV("true");
    dcp_simulate_crash_from_environment();
    SET_ENV("");

    char *loader = read_source("src/loader.c");
    CHECK(loader != NULL);
    if (loader) {
        const char *install = after(loader, "dcp_crash_dump_install();", NULL);
        const char *vectored = install ? after(loader, "AddVectoredExceptionHandler(1, vectored_fault);", install) : NULL;
        const char *unhandled = vectored ? after(loader, "SetUnhandledExceptionFilter(unhandled_fault);", vectored) : NULL;
        const char *logged = after(loader, "log_started=runtime_log_start(", NULL);
        const char *trigger = unhandled ? after(loader, "dcp_simulate_crash_from_environment();", unhandled) : NULL;
        CHECK(logged && install && vectored && unhandled && trigger);
        CHECK(logged && install && logged < install);
        CHECK(trigger && !after(loader, "dcp_simulate_crash_from_environment();", trigger));
        const char *launcher = after(loader, "DcpLauncherResult launched=dcp_launcher_run(&launcher);", NULL);
        CHECK(launcher && trigger && launcher < trigger);
        free(loader);
    }

    char *source = read_source("src/simulate_crash.c");
    CHECK(source != NULL);
    if (source) {
        CHECK(!strstr(source, "abort(") && !strstr(source, "exit(") && !strstr(source, "TerminateProcess") && !strstr(source, "RaiseException"));
        CHECK(strstr(source, "Diagnostics: DCP_SIMULATE_CRASH=1, deliberately triggering an access violation") != NULL);
        free(source);
    }
    if (!failures) puts("simulate crash tests passed");
    return failures ? 1 : 0;
}
