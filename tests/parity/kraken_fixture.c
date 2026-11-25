#include "../../src/kraken_fix.c"

#include "fixture.h"

int fixture_lists_kraken(const char *game) {
    for (size_t i = 0; i < sizeof(kBuilds) / sizeof(*kBuilds); ++i)
        if (!strcmp(game, kBuilds[i].game)) return kBuilds[i].window != SEARCHED;
    return -1;
}

void fixture_kraken(Fx *fx, const char *layout) {
    fx_context(fx, "Kraken fix");
    for (size_t i = 0; i < sizeof(kBuilds) / sizeof(*kBuilds); ++i)
        if (!strcmp(layout, kBuilds[i].game)) FX_BYTES(fx, kBuilds[i].window, kWindow);
}
