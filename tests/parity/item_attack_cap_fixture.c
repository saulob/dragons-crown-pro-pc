#include "../../src/item_attack_cap.c"

#include "fixture.h"

int fixture_lists_item_attack_cap(const char *game) {
    for (size_t i = 0; i < sizeof(kBuilds) / sizeof(*kBuilds); ++i)
        if (!strcmp(game, kBuilds[i].game)) return kBuilds[i].window != SEARCHED;
    return -1;
}

void fixture_item_attack_cap(Fx *fx, const char *layout) {
    for (size_t i = 0; i < sizeof(kBuilds) / sizeof(*kBuilds); ++i) {
        if (strcmp(layout, kBuilds[i].game)) continue;
        fx_context(fx, "item attack cap");
        FX_SHAPE(fx, kBuilds[i].window, kWindow);
        const uint64_t table = fx_rel(fx, kBuilds[i].window + kTableLeaEnd - 4, kBuilds[i].window + kTableLeaEnd, kTableRowBytes * kTableRows);
        const unsigned char first = 5, second = 7, last = 255;
        fx_put(fx, table, &first, 1);
        fx_put(fx, table + kTableRowBytes, &second, 1);
        fx_put(fx, table + kTableRowBytes * (kTableRows - 1), &last, 1);
    }
}
