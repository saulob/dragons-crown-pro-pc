#include "../../src/skip_intro.c"

#include "fixture.h"

static void plant_getter(Fx *fx, uint64_t call, unsigned scene) {
    const uint64_t getter = fx_call(fx, call, 16);
    uint8_t bytes[8] = {0x48, 0x8b, 0x87, 0, 0, 0, 0, 0xc3};
    const uint32_t slot = 0x78 + 0x20 * scene;
    memcpy(bytes + 3, &slot, sizeof(slot));
    FX_BYTES(fx, getter, bytes);
}

static void plant_factory_call(Fx *fx, uint64_t call, unsigned scene, uint64_t factory) {
    const uint64_t thunk = fx_call(fx, call, 16);
    const uint8_t bytes[5] = {0xbe, (uint8_t)scene, 0, 0, 0};
    FX_BYTES(fx, thunk, bytes);
    fx_jump_to(fx, thunk + 5, factory);
}

static void plant_scene_case(Fx *fx, uint64_t table, unsigned scene, uint64_t base) {
    const uint64_t entry = fx_alloc(fx, 0x20);
    fx_entry(fx, table, (int)scene - 1, entry);
    fx_shape(fx, entry, kFactoryCase, kFactoryCaseCount);
    const uint64_t ctor = fx_call(fx, entry + kCtorCall, 0x40);
    const uint8_t prologue = 0x55, pass_id[8] = {0xba, (uint8_t)scene, 0, 0, 0, 0x48, 0x89, 0xfb};
    fx_put(fx, ctor, &prologue, 1);
    FX_BYTES(fx, ctor + 8, pass_id);
    fx_call_to(fx, ctor + 16, base);
}

static void plant_title_flow(Fx *fx, uint64_t opening) {
    const uint64_t title = opening + kOpeningStepCount, done = title + kTitleStepCount, save = opening - kSaveStepCount;
    const uint64_t dispatch = opening - 0x1c0, exit = fx_alloc(fx, 16), factory = fx_alloc(fx, 0x80), create = fx_alloc(fx, 0x60);
    const uint64_t first_frame = fx_alloc(fx, 16), decision = fx_alloc(fx, 0x60), base = fx_alloc(fx, 16);
    fx_shape(fx, save, kSaveStep, kSaveStepCount);
    fx_shape(fx, opening, kOpeningStep, kOpeningStepCount);
    fx_shape(fx, title, kTitleStep, kTitleStepCount);
    fx_shape(fx, done, kStepDone, kStepDoneCount);
    fx_rel_to(fx, opening + kOpeningExit + 2, opening + kOpeningExit + 6, exit);
    fx_jump_to(fx, opening + kOpeningExitJump, exit);
    plant_getter(fx, opening + kGetterCall, kSceneOpening);
    plant_getter(fx, title + kGetterCall, kSceneTitle);
    fx_call_to(fx, opening + kOpeningFirstFrame, first_frame);
    plant_factory_call(fx, opening + kOpeningCreate, kSceneOpening, factory);
    fx_rel_to(fx, title + kTitleCreateBranch + 2, title + kTitleCreateBranch + 6, create);
    fx_shape(fx, create, kTitleCreate, kTitleCreateCount);
    fx_call_to(fx, create + kTitleFirstFrame, first_frame);
    plant_factory_call(fx, create + kTitleCall, kSceneTitle, factory);

    fx_shape(fx, dispatch, kDispatch, kDispatchCount);
    fx_rel_to(fx, dispatch + kDispatchDefault + 2, dispatch + kDispatchDefault + 6, exit);
    const uint64_t steps = fx_rel(fx, dispatch + kDispatchTable + 3, dispatch + kDispatchTable + 7, 4 * kStepEntries);
    fx_entry(fx, steps, kSaveStepIndex, save);
    fx_entry(fx, steps, kOpeningStepIndex, opening);
    fx_entry(fx, steps, kTitleStepIndex, title);
    fx_entry(fx, steps, kDecisionIndex, decision);
    plant_getter(fx, save + kGetterCall, kSceneSave);
    fx_call_to(fx, save + kSaveFirstFrame, first_frame);
    plant_factory_call(fx, save + kSaveCreate, kSceneSave, factory);
    fx_rel_to(fx, save + kSaveExit + 2, save + kSaveExit + 6, exit);
    fx_shape(fx, decision, kDecision, kDecisionCount);
    const uint64_t to_save = fx_rel(fx, decision + kDecisionBranch + 2, decision + kDecisionBranch + 6, 0x20);
    fx_shape(fx, to_save, kDecisionSave, kDecisionSaveCount);

    fx_shape(fx, factory, kFactoryHead, kFactoryHeadCount);
    fx_shape(fx, factory + kFactoryCaseTable, kFactoryDispatch, kFactoryDispatchCount);
    const uint64_t cases = fx_rel(fx, factory + kFactoryCaseTable + 0x0e, factory + kFactoryCaseTable + 0x12, 4 * 0x43);
    plant_scene_case(fx, cases, kSceneSave, base);
    plant_scene_case(fx, cases, kSceneOpening, base);
    plant_scene_case(fx, cases, kSceneTitle, base);
}

static void plant_notice(Fx *fx, uint64_t at) {
    fx_shape(fx, at, kNotice, kNoticeCount);
    const uint64_t query = fx_call(fx, at + kFirstFrameCall, 16), tail = fx_alloc(fx, 0x60) + 0x40;
    FX_BYTES(fx, query, kFirstFrameTest);
    fx_rel_to(fx, at + kFirstFrameBranch + 2, at + kFirstFrameBranch + 6, at + kObjectEnds);
    fx_jump_to(fx, at + kStepJump, tail);
    fx_shape(fx, tail - kAdvanceCount, kAdvance, kAdvanceCount);
    FX_BYTES(fx, tail, kSetStepTail);
    fx_call(fx, at + kObject + 5, 16);
    fx_call(fx, at + kObjectEnds - 5, 16);
    fx_rel(fx, at + kStackGuard + 3, at + kStackGuard + 7, 16);
}

void fixture_skip_intro_again(Fx *fx) {
    plant_title_flow(fx, fx_alloc(fx, 0x600) + 0x300);
}

void fixture_skip_intro(Fx *fx, const char *layout) {
    fx_context(fx, "Skip Intro");
    for (size_t i = 0; i < sizeof(kBuilds) / sizeof(*kBuilds); ++i) {
        if (strcmp(layout, kBuilds[i].game)) continue;
        plant_title_flow(fx, kBuilds[i].opening_step);
        if (kBuilds[i].notice) plant_notice(fx, kBuilds[i].notice);
    }
}
