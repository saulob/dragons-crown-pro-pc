/* The four-row title layout patch's data (scripts/title_layout_patch.py): per patched part, the original layer records,
 * the new layer order and part counts, and the keyframes that move to another transform. */
#ifndef DCP_TITLE_LAYOUT_DATA_H
#define DCP_TITLE_LAYOUT_DATA_H

#include <stdint.h>

typedef struct {
    const char *name;
    uint16_t index, layer_offset;
    uint8_t old_count, new_count, old_master, new_master;
    uint16_t order_at, old_layers_at;
} TitleLayoutPart;

typedef struct {
    uint16_t index, old_transform, new_transform;
} TitleLayoutKeyframe;

#include "title_layout_data.inc"

#endif
