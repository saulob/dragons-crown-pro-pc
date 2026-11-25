#include "../../src/default_message_hook.c"

#include "fixture.h"

static void plant_loads(Fx *fx, uint64_t at, size_t field) {
    unsigned char pair[kLoadPairLength] = {0xc5, 0xfc, 0x10, 0x87, 0, 0, 0, 0, 0xc5, 0xfc, 0x10, 0x8f, 0, 0, 0, 0};
    const int32_t first = (int32_t)field, second = (int32_t)(field + kLoadPairGap);
    memcpy(pair + 4, &first, sizeof(first));
    memcpy(pair + 12, &second, sizeof(second));
    FX_BYTES(fx, at, pair);
}

void fixture_default_messages(Fx *fx) {
    fx_context(fx, "Default messages");
    for (int i = 0; i < DEFAULT_MESSAGE_CATEGORIES; ++i) fx_put(fx, fx_alloc(fx, 0x20), kFirstRecord[i], sizeof(kFirstRecord[i]));
    const uint64_t death = fx_alloc(fx, 0x20);
    FX_BYTES(fx, death, kDeathEnd);
    uint64_t getters[kRowCount];
    for (int i = kRowCount - 1; i >= 0; --i) {
        const size_t field = kCharacterMessages + (size_t)kStoredIndex[i] * kCharacterMessagePair;
        getters[i] = fx_alloc(fx, 0x100);
        plant_loads(fx, getters[i] + 0x38, field);
        plant_loads(fx, getters[i] + 0x80, field + kCharacterAltMessages);
    }
    for (int i = 0; i < kRowCount; ++i) {
        const uint64_t block = fx_alloc(fx, 0x600) + 0x400, call = block + kGetterLoadBeforeCall, jump = call + 20, label = block + 0x100;
        const unsigned char je[2] = {0x0f, 0x84};
        FX_BYTES(fx, block, kGetterLoad);
        fx_call_to(fx, call, getters[i]);
        FX_BYTES(fx, jump, je);
        fx_rel_to(fx, jump + 2, jump + 6, label);
        FX_BYTES(fx, label + kRowLabelBeforePattern, kRowPattern);
    }
    const uint64_t picker = fx_alloc(fx, 0x100), defaults = fx_alloc(fx, 0x100);
    FX_BYTES(fx, picker, kPickerStart);
    fx_call(fx, picker + kPickerToPick - 5, 16);
    fx_put(fx, picker + kPickerToPick, kPickPattern + 5, sizeof(kPickPattern) - 5);
    FX_BYTES(fx, defaults + kDefaultsArgumentsAt, kDefaultsArguments);
    fx_call_to(fx, defaults + kDefaultsToPickerCall, picker);
    FX_BYTES(fx, defaults + kDefaultsToPickerCall + 5, kAfterPickerCall);
}
