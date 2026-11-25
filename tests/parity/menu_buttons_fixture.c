#include "../../src/menu_buttons.c"

#include "fixture.h"

static uint64_t plant_reader(Fx *fx, uint64_t reader) {
    const unsigned char zero = 0, ret = 0xc3;
    fx_context(fx, "Japanese Menu Buttons");
    fx_shape(fx, reader, kReader, kReaderCount);
    fx_put(fx, reader + kCompareInReader, &zero, 1);
    FX_BYTES(fx, reader + kCrossBranchInReader, kCrossBranch);
    FX_BYTES(fx, fx_rel(fx, reader + kJumpInReader, reader + kReaderCount, 0x10), kCircleBranch);
    const uint64_t getter = fx_call(fx, reader - kGetterBeforeReader, 0x10);
    FX_BYTES(fx, getter, kGetter);
    fx_put(fx, getter + 7, &ret, 1);
    return getter;
}

void fixture_menu_buttons_again(Fx *fx) {
    plant_reader(fx, fx_alloc(fx, 0x140) + 0x40);
}

void fixture_menu_buttons(Fx *fx, const char *layout) {
    for (size_t i = 0; i < sizeof(kBuilds) / sizeof(*kBuilds); ++i) {
        if (strcmp(layout, kBuilds[i].game)) continue;
        const unsigned char zero = 0;
        const uint64_t getter = plant_reader(fx, kBuilds[i].reader);
        for (int p = 0; p < kPromptSites; ++p) {
            const uint64_t at = fx_alloc(fx, 0x40);
            fx_call_to(fx, at, getter);
            FX_BYTES(fx, at + 5, kPromptCompare);
            fx_put(fx, at + kCompareInPrompt, &zero, 1);
            FX_BYTES(fx, at + kCompareInPrompt + 4, kSetNotEqual);
        }
        const uint64_t load = fx_alloc(fx, 0x40);
        fx_call_to(fx, load, getter);
        FX_BYTES(fx, load + 5, kLoadHead);
        FX_BYTES(fx, load + kLoadAfterCall, kLoadOriginal);
        for (int s = 0; s < kBuilds[i].write_count; ++s)
            fx_put(fx, kBuilds[i].writes[s] - kShapes[s].immediate, kShapes[s].bytes, kShapes[s].length);
    }
}
