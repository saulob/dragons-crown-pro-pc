#include "../../src/touch_absolute.c"

#include "fixture.h"

void fixture_touch(Fx *fx) {
    fx_context(fx, "Touchscreen mode");
    const uint64_t update = fx_alloc(fx, 0x500), step = fx_alloc(fx, 0x80), fill = fx_alloc(fx, 0x20);
    FX_SHAPE(fx, update, kUpdateRead);
    FX_SHAPE(fx, step, kStepRead);
    const uint64_t option = fx_call(fx, update + kUpdateOptionCall, 0x40), mode = fx_call(fx, update + kUpdateOptionCall + kModeCallAfterOption, 16);
    FX_SHAPE(fx, option, kOptionRecord);
    FX_SHAPE(fx, mode, kModeBody);
    fx_call_to(fx, step + kStepOptionCall, option);
    fx_call_to(fx, step + kStepOptionCall + kModeCallAfterOption, mode);
    FX_SHAPE(fx, fill, kTouchFill);
    const uint64_t engine = fx_call(fx, fill + kEngineCall, 16);
    FX_SHAPE(fx, engine, kEngine);
    fx_rel(fx, engine + 3, engine + 7, 16);
    const uint64_t record = fx_alloc(fx, 0x20), pad = fx_alloc(fx, 0x20);
    FX_SHAPE(fx, record, kTouchRecord);
    FX_SHAPE(fx, pad, kPadRead);
    for (int port = 0; port < kPorts; ++port) {
        const int field = kTouchRecordPort0 + port * kTouchRecordBytes;
        const short write[] = {0xc7, 0x83, (short)(field & 0xff), (short)(field >> 8), 0, 0, (short)port, 0, 0, 0};
        const uint64_t at = fx_alloc(fx, 0x10);
        FX_SHAPE(fx, at, write);
    }
    const uint64_t absolute = update + 0x100;
    const float gain = 1.5f, a = -0.25f, b = 0.5f;
    FX_SHAPE(fx, absolute, kAbsolute);
    fx_call_to(fx, absolute + kAbsoluteEngineCall, engine);
    fx_put(fx, fx_rel(fx, absolute + kGainDisp, absolute + kGainDisp + 4, 16), &gain, sizeof(gain));
    fx_put(fx, fx_rel(fx, absolute + kOffsetADisp, absolute + kOffsetADisp + 4, 16), &a, sizeof(a));
    fx_put(fx, fx_rel(fx, absolute + kOffsetBDisp, absolute + kOffsetBDisp + 4, 16), &b, sizeof(b));
}
