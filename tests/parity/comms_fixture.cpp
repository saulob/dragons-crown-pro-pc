#include "../../src/comms_bridge.cpp"

#include "fixture.h"

extern "C" void fixture_comms(Fx* fx) {
    fx_context(fx, "Quick Chat");
    const uint64_t show = fx_alloc(fx, 0x620), caller = fx_alloc(fx, 0x10);
    for (size_t i = 0; i < sizeof(kShowPrologue) / sizeof(kShowPrologue[0]); ++i) {
        if (kShowPrologue[i] < 0) continue;
        const auto byte = static_cast<uint8_t>(kShowPrologue[i]);
        fx_put(fx, show + i, &byte, 1);
    }
    fx_put(fx, show + 0x200, kTextSite, sizeof(kTextSite));
    fx_call(fx, show + 0x200 + sizeof(kTextSite) - 1, 16);
    fx_call_to(fx, caller, show);
}
