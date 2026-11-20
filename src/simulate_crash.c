#include "simulate_crash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int dcp_simulate_crash_requested(const char *value) { return value && !strcmp(value, "1"); }

#ifdef _WIN32
static __attribute__((noinline)) void write_to_null_page(void) { *(volatile int *)0x10 = 1; }
#endif

void dcp_simulate_crash_from_environment(void) {
    if (!dcp_simulate_crash_requested(getenv("DCP_SIMULATE_CRASH"))) return;
#ifdef _WIN32
    puts("Diagnostics: DCP_SIMULATE_CRASH=1, deliberately triggering an access violation");
    fflush(NULL);
    write_to_null_page();
#else
    puts("Diagnostics: DCP_SIMULATE_CRASH=1 is only supported on Windows; ignored");
#endif
}
