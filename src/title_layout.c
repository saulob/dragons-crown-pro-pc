#include "title_layout.h"

#include <string.h>

enum {
    LAYOUT_TRANSFORM_COUNT = 0x5e,
    LAYOUT_KEYFRAME_COUNT = 0x60,
    LAYOUT_PART_COUNT = 0x64,
    LAYOUT_LAYER_COUNT = 0x66,
    LAYOUT_KEYFRAMES = 0xf0,
    LAYOUT_PARTS = 0xf8,
    LAYOUT_LAYERS = 0x100,
    PART_SIZE = 48,
    PART_NAME = 0x10,
    PART_NAME_SIZE = 24,
    PART_LAYER_OFFSET = 0x28,
    PART_LAYER_COUNT = 0x2a,
    PART_MASTER = 0x2b,
    KEYFRAME_SIZE = 32,
    KEYFRAME_TRANSFORM = 4,
    LAYER_SIZE = 24,
    TITLE_TRANSFORMS = 234,
    TITLE_KEYFRAMES = 4814,
    TITLE_PARTS = 178,
    TITLE_LAYERS = 1467,
    ANCHOR_PART = 11,
    MAX_PART_LAYERS = 20
};

#include "title_layout_data.h"

static uint16_t read_u16(const uint8_t *at) {
    uint16_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

static const uint8_t *read_pointer(const uint8_t *at) {
    uint64_t value;
    memcpy(&value, at, sizeof(value));
    return (const uint8_t *)(uintptr_t)value;
}

typedef struct {
    uint8_t *parts, *layers, *keyframes;
} Tables;

static int is_title(const uint8_t *layout, Tables *tables) {
    if (read_u16(layout + LAYOUT_PART_COUNT) != TITLE_PARTS) return 0;
    tables->parts = (uint8_t *)(uintptr_t)read_pointer(layout + LAYOUT_PARTS);
    if (!tables->parts) return 0;
    return !strncmp((const char *)tables->parts + ANCHOR_PART * PART_SIZE + PART_NAME, "TITLE_1_PS4", PART_NAME_SIZE);
}

static int shape_matches(const uint8_t *layout, Tables *tables) {
    tables->layers = (uint8_t *)(uintptr_t)read_pointer(layout + LAYOUT_LAYERS);
    tables->keyframes = (uint8_t *)(uintptr_t)read_pointer(layout + LAYOUT_KEYFRAMES);
    return tables->layers && tables->keyframes && read_u16(layout + LAYOUT_LAYER_COUNT) == TITLE_LAYERS &&
           read_u16(layout + LAYOUT_KEYFRAME_COUNT) == TITLE_KEYFRAMES &&
           read_u16(layout + LAYOUT_TRANSFORM_COUNT) == TITLE_TRANSFORMS;
}

typedef enum { STATE_ORIGINAL, STATE_PATCHED, STATE_OTHER } PartState;

static void expected_layers(const TitleLayoutPart *part, uint8_t out[MAX_PART_LAYERS * LAYER_SIZE]) {
    for (int i = 0; i < part->new_count; ++i)
        memcpy(out + i * LAYER_SIZE, kTitleLayoutOldLayers + part->old_layers_at + kTitleLayoutOrder[part->order_at + i] * LAYER_SIZE,
               LAYER_SIZE);
}

static PartState part_state(const Tables *tables, const TitleLayoutPart *part) {
    const uint8_t *record = tables->parts + part->index * PART_SIZE;
    if (strncmp((const char *)record + PART_NAME, part->name, PART_NAME_SIZE) || read_u16(record + PART_LAYER_OFFSET) != part->layer_offset)
        return STATE_OTHER;
    const uint8_t *layers = tables->layers + part->layer_offset * LAYER_SIZE;
    if (record[PART_LAYER_COUNT] == part->old_count && record[PART_MASTER] == part->old_master &&
        !memcmp(layers, kTitleLayoutOldLayers + part->old_layers_at, (size_t)part->old_count * LAYER_SIZE))
        return STATE_ORIGINAL;
    uint8_t expected[MAX_PART_LAYERS * LAYER_SIZE];
    expected_layers(part, expected);
    if (record[PART_LAYER_COUNT] == part->new_count && record[PART_MASTER] == part->new_master &&
        !memcmp(layers, expected, (size_t)part->new_count * LAYER_SIZE))
        return STATE_PATCHED;
    return STATE_OTHER;
}

static int keyframes_are(const Tables *tables, int patched) {
    for (int i = 0; i < TITLE_LAYOUT_KEYFRAME_COUNT; ++i) {
        const TitleLayoutKeyframe *kf = &kTitleLayoutKeyframes[i];
        const uint16_t want = patched ? kf->new_transform : kf->old_transform;
        if (read_u16(tables->keyframes + kf->index * KEYFRAME_SIZE + KEYFRAME_TRANSFORM) != want) return 0;
    }
    return 1;
}

TitleLayoutStatus title_layout_patch(uint8_t *layout) {
    Tables tables;
    if (!layout || !is_title(layout, &tables)) return TITLE_LAYOUT_NOT_TITLE;
    if (!shape_matches(layout, &tables)) return TITLE_LAYOUT_UNSUPPORTED;
    int original = 0, patched = 0;
    for (int i = 0; i < TITLE_LAYOUT_PART_COUNT; ++i) {
        const PartState state = part_state(&tables, &kTitleLayoutParts[i]);
        if (state == STATE_ORIGINAL) ++original;
        else if (state == STATE_PATCHED) ++patched;
        else return TITLE_LAYOUT_UNSUPPORTED;
    }
    if (patched == TITLE_LAYOUT_PART_COUNT) return keyframes_are(&tables, 1) ? TITLE_LAYOUT_CURRENT : TITLE_LAYOUT_UNSUPPORTED;
    if (original != TITLE_LAYOUT_PART_COUNT || !keyframes_are(&tables, 0)) return TITLE_LAYOUT_UNSUPPORTED;
    for (int i = 0; i < TITLE_LAYOUT_KEYFRAME_COUNT; ++i) {
        const TitleLayoutKeyframe *kf = &kTitleLayoutKeyframes[i];
        memcpy(tables.keyframes + kf->index * KEYFRAME_SIZE + KEYFRAME_TRANSFORM, &kf->new_transform, sizeof(kf->new_transform));
    }
    for (int i = 0; i < TITLE_LAYOUT_PART_COUNT; ++i) {
        const TitleLayoutPart *part = &kTitleLayoutParts[i];
        uint8_t expected[MAX_PART_LAYERS * LAYER_SIZE];
        expected_layers(part, expected);
        memcpy(tables.layers + part->layer_offset * LAYER_SIZE, expected, (size_t)part->new_count * LAYER_SIZE);
        uint8_t *record = tables.parts + part->index * PART_SIZE;
        record[PART_LAYER_COUNT] = part->new_count;
        record[PART_MASTER] = part->new_master;
    }
    return TITLE_LAYOUT_PATCHED;
}
