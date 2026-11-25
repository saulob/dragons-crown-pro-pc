#include "../../src/gallery_unlock.c"

#include "fixture.h"

void fixture_gallery(Fx *fx, const char *layout) {
    fx_context(fx, "Unlock Gallery");
    for (size_t i = 0; i < sizeof(kBuilds) / sizeof(*kBuilds); ++i) {
        if (strcmp(layout, kBuilds[i].game)) continue;
        fx_shape(fx, kBuilds[i].window, kWindow, kWindowCount);
        FX_SHAPE(fx, fx_call(fx, kBuilds[i].window + kKeyCallInWindow, 0x30), kKeyBody);
        FX_SHAPE(fx, fx_call(fx, kBuilds[i].window + kHookInWindow, 0x10), kGetterBody);
    }
}
