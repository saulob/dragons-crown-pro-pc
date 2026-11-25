#include "../../src/hp_override_hooks.c"

#include "fixture.h"

void fixture_character_override(Fx *fx) {
    fx_context(fx, "Character override");
    const uint64_t recalc = fx_alloc(fx, 0x200), spawn = fx_alloc(fx, 0x40), status = fx_alloc(fx, 0x700) + 0x400;
    const uint64_t bundle = fx_alloc(fx, 0x40);
    FX_BYTES(fx, recalc, kRecalcEntry);
    FX_BYTES(fx, recalc + 0x100, kRecalcTail);
    FX_BYTES(fx, spawn, kSpawnLoad);
    fx_call_to(fx, spawn + kSpawnCallOffset, recalc);
    FX_BYTES(fx, status - 0x100, kStatusRecordLoad);
    FX_BYTES(fx, status, kStatusMaxHead);
    FX_BYTES(fx, status + 11, kStatusMaxTail);
    const uint64_t max = fx_call(fx, status + kStatusMaxCall, 0x50);
    FX_BYTES(fx, max, kFunctionPrologue);
    FX_BYTES(fx, max + 0x10, kStatusMaxCompute);
    const uint8_t *const stores[] = {kStatusStrengthStore, kStatusIntelligenceStore, kStatusConstitutionStore,
                                     kStatusMagicResistanceStore, kStatusDexterityStore};
    for (size_t i = 0; i < sizeof(stores) / sizeof(*stores); ++i) fx_put(fx, status + 0x40 + 0x20 * i, stores[i], kStatusStoreLength);
    FX_BYTES(fx, status + 0x100, kStatusStatsTail);
    FX_BYTES(fx, bundle, kBundleStatsTail);
    const uint64_t effect = fx_alloc(fx, 0x700) + 0x400;
    FX_BYTES(fx, effect - 0x100, kEffectStatsPrologue);
    FX_BYTES(fx, effect, kEffectStatsStore);
    FX_BYTES(fx, effect + 0x20, kEffectStatsTail);
}
