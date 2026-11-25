#include "../../src/ultrawide_hooks.c"

#include "fixture.h"

/* What the hooks check beyond the located sites. */
void fixture_ultrawide(Fx *fx, uint64_t queue, uint64_t model_draw, uint64_t worldmap_draw) {
    static const uint8_t buckets[] = {0x49, 0x8b, 0x8f, 0xb0, 0x02, 0x00, 0x00, 0x49, 0x8b, 0x97, 0xb8, 0x02, 0x00, 0x00};
    static const uint8_t bucket_end[] = {0x48, 0x8b, 0x44, 0x19, 0x10}, bucket_begin[] = {0x48, 0x2b, 0x44, 0x19, 0x08};
    static const uint8_t prologue[] = {0x55, 0x48, 0x89, 0xe5};
    fx_context(fx, "True Ultrawide preconditions");
    FX_BYTES(fx, queue + 0x57, buckets);
    FX_BYTES(fx, queue + 0x9b, bucket_end);
    FX_BYTES(fx, queue + 0xa9, bucket_begin);
    fx_call(fx, model_draw + MODEL_DRAW_PART_RETURN - 5, 16);
    FX_BYTES(fx, worldmap_draw + WORLDMAP_DRAW_SIZE, prologue);
}
