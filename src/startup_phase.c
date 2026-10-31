#include "startup_phase.h"

static const char *current_phase = "process start";

void dcp_startup_phase(const char *phase) {
    __atomic_store_n(&current_phase, phase, __ATOMIC_RELEASE);
}

const char *dcp_startup_phase_current(void) {
    return __atomic_load_n(&current_phase, __ATOMIC_ACQUIRE);
}
