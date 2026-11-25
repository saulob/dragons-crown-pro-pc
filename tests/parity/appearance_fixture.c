#include "../../src/appearance_hooks.c"

#include "fixture.h"

void fixture_appearance(Fx *fx) {
    fx_context(fx, "Appearance");
    const uint64_t unit_of = fx_alloc(fx, 16), atlas = fx_alloc(fx, 0x20);
    FX_SHAPE(fx, unit_of, kUnitOf);
    FX_SHAPE(fx, atlas, kAtlasName);
    for (int i = 0; i < 2; ++i) {
        const uint64_t at = fx_alloc(fx, 0x40);
        FX_SHAPE(fx, at, kUnitGetter);
        fx_call_to(fx, at + kUnitGetterFirstCall, unit_of);
        fx_call_to(fx, at + kUnitGetterSecondCall, unit_of);
        fx_jump_to(fx, at + kUnitGetterJump, atlas);
    }
    const uint64_t select = fx_alloc(fx, 0x30), copy = fx_alloc(fx, 0x50), set = fx_alloc(fx, 0x20), descriptor = fx_alloc(fx, 0x20);
    FX_SHAPE(fx, select, kSelectPreview);
    fx_call_to(fx, select + kSelectCall, atlas);
    FX_SHAPE(fx, copy, kEffectCopy);
    fx_call_to(fx, copy + kEffectCopyCall, unit_of);
    FX_SHAPE(fx, set, kEffectSet);
    FX_SHAPE(fx, descriptor, kEffectAtlas);
    fx_jump_to(fx, descriptor + kEffectAtlasJump, atlas);
    for (int w = 0; w < APPEARANCE_WEAPON_COUNT; ++w) {
        const uint64_t name = fx_alloc(fx, 0x150), preview = fx_alloc(fx, 0x20);
        const uint8_t lea[3] = {0x48, 0x8d, 0x05};
        FX_BYTES(fx, name + 0x20, lea);
        fx_rel_to(fx, name + 0x23, name + 0x27, fx_text(fx, kWeaponDefault[w]));
        for (int i = 0; i < 2; ++i) {
            const uint64_t at = fx_alloc(fx, 0x60);
            FX_SHAPE(fx, at, kWeaponGetter);
            fx_call_to(fx, at + kWeaponGetterFirstCall, unit_of);
            fx_call_to(fx, at + kWeaponGetterSecondCall, unit_of);
            fx_jump_to(fx, at + kWeaponGetterJump, name);
        }
        FX_SHAPE(fx, preview, kWeaponSelect);
        fx_call_to(fx, preview + kWeaponSelectCall, name);
    }
}
