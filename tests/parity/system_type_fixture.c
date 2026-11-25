#include "../../src/system_type_hook.c"

#include "fixture.h"

void fixture_system_type(Fx *fx, const char *label) {
    fx_context(fx, "System type");
    for (size_t i = 0; i < SYSTEM_TYPE_RECORD_COUNT; ++i) {
        const SystemTypeRecord *record = &system_type_records[i];
        if (strcmp(label, record->build)) continue;
        const uint64_t card = record->sites[0] - CARD_SITE_1;
        fx_shape(fx, card, kCard, sizeof(kCard) / sizeof(*kCard));
        int32_t rel;
        memcpy(&rel, record->calls[0] + 1, sizeof(rel));
        const uint64_t setter = (uint64_t)((int64_t)record->sites[0] + 5 + rel);
        fx_call_to(fx, card + CARD_SITE_1, setter);
        fx_call_to(fx, card + CARD_SITE_2, setter);
        fx_call_to(fx, card + CARD_RECORD_CALL_1, setter);
        fx_call_to(fx, card + CARD_RECORD_CALL_2, setter);
        fx_call_to(fx, card + CARD_SET_TEXT_CALL, record->set_text);
        FX_BYTES(fx, record->set_text, system_type_set_text_prologue);
    }
}
