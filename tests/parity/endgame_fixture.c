#include "../../src/endgame_unlocks.c"

#include "fixture.h"

void fixture_endgame(Fx *fx, const char *layout) {
    fx_context(fx, "Endgame unlocks");
    for (size_t i = 0; i < COUNT(kBuilds); ++i) {
        if (strcmp(layout, kBuilds[i].game)) continue;
        const Windows *w = &kBuilds[i].windows;
        FX_SHAPE(fx, w->director, kDirector);
        FX_SHAPE(fx, w->list, kList);
        FX_SHAPE(fx, w->row, kRow);
        FX_SHAPE(fx, w->confirm, kConfirm);
        FX_BYTES(fx, w->setter, kSetter);
        const uint64_t stables = fx_alloc(fx, 0x80), wrapper = stables + kStablesWrapperBefore;
        fx_call_to(fx, w->director + kDirectorWrapperCall, wrapper);
        fx_call_to(fx, w->director + kDirectorSecondCall, wrapper);
        FX_SHAPE(fx, wrapper, kWrapper);
        FX_SHAPE(fx, stables, kWrapper);
        fx_call_to(fx, stables + kWrapperRecordCall, fx_call(fx, wrapper + kWrapperRecordCall, 16));
        FX_BYTES(fx, fx_call(fx, wrapper + kWrapperCheckCall, 16), kLabyrinthBody);
        FX_BYTES(fx, fx_call(fx, stables + kWrapperCheckCall, 16), kStablesBody);
        const uint64_t tier = fx_call(fx, w->list + kListTierCall, 0x40);
        FX_BYTES(fx, tier, kTierBody);
        fx_call_to(fx, w->row + kRowTierCall, tier);
        fx_call_to(fx, w->confirm + kConfirmTierCall, tier);
        fx_call_to(fx, w->confirm + kConfirmPrayerCall, fx_call(fx, w->list + kListPrayerCall, 16));
        FX_BYTES(fx, fx_call(fx, w->row + kRowClearedCall, 0x20), kClearedBody);
    }
}
