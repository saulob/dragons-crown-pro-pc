/* A synthetic title layout in the game's loaded form, built from the patch data: the parts, layers and keyframes the patch
 * touches hold their original values, everything else is zero. */
#ifndef DCP_TEST_TITLE_LAYOUT_FIXTURE_H
#define DCP_TEST_TITLE_LAYOUT_FIXTURE_H

#include <stdlib.h>
#include <string.h>

#include "title_layout_data.h"

enum {
    FIXTURE_HEADER = 0x108,
    FIXTURE_PARTS = 178,
    FIXTURE_LAYERS = 1467,
    FIXTURE_KEYFRAMES = 4814,
    FIXTURE_TRANSFORMS = 234,
    FIXTURE_ANCHOR_PART = 11
};

typedef struct {
    uint8_t *header, *parts, *layers, *keyframes;
} FixtureLayout;

static inline void fixture_put_u16(uint8_t *at, uint16_t value) {
    memcpy(at, &value, sizeof(value));
}

static inline void fixture_put_pointer(uint8_t *at, const void *pointer) {
    const uint64_t value = (uint64_t)(uintptr_t)pointer;
    memcpy(at, &value, sizeof(value));
}

static inline int fixture_make(FixtureLayout *layout) {
    layout->header = calloc(1, FIXTURE_HEADER);
    layout->parts = calloc(FIXTURE_PARTS, 48);
    layout->layers = calloc(FIXTURE_LAYERS, 24);
    layout->keyframes = calloc(FIXTURE_KEYFRAMES, 32);
    if (!layout->header || !layout->parts || !layout->layers || !layout->keyframes) return 0;
    fixture_put_u16(layout->header + 0x5e, FIXTURE_TRANSFORMS);
    fixture_put_u16(layout->header + 0x60, FIXTURE_KEYFRAMES);
    fixture_put_u16(layout->header + 0x64, FIXTURE_PARTS);
    fixture_put_u16(layout->header + 0x66, FIXTURE_LAYERS);
    fixture_put_pointer(layout->header + 0xf0, layout->keyframes);
    fixture_put_pointer(layout->header + 0xf8, layout->parts);
    fixture_put_pointer(layout->header + 0x100, layout->layers);
    strcpy((char *)layout->parts + FIXTURE_ANCHOR_PART * 48 + 0x10, "TITLE_1_PS4");
    for (int i = 0; i < TITLE_LAYOUT_PART_COUNT; ++i) {
        const TitleLayoutPart *part = &kTitleLayoutParts[i];
        uint8_t *record = layout->parts + part->index * 48;
        strcpy((char *)record + 0x10, part->name);
        fixture_put_u16(record + 0x28, part->layer_offset);
        record[0x2a] = part->old_count;
        record[0x2b] = part->old_master;
        memcpy(layout->layers + part->layer_offset * 24, kTitleLayoutOldLayers + part->old_layers_at, (size_t)part->old_count * 24);
    }
    for (int i = 0; i < TITLE_LAYOUT_KEYFRAME_COUNT; ++i)
        fixture_put_u16(layout->keyframes + kTitleLayoutKeyframes[i].index * 32 + 4, kTitleLayoutKeyframes[i].old_transform);
    return 1;
}

static inline void fixture_free(FixtureLayout *layout) {
    free(layout->header);
    free(layout->parts);
    free(layout->layers);
    free(layout->keyframes);
}

/* The order table is the one array only the patch itself reads; the fixture reads it to state what a patched part holds. */
static inline const uint8_t *fixture_order(const TitleLayoutPart *part) {
    return kTitleLayoutOrder + part->order_at;
}

#endif
