/* The NPC voice cue IDs of CUSA10454 1.00 whose English counterparts are +100000: the town, event and Mikey voices,
 * the thieves Tomtit and Ronnie, and the citizens. */
#pragma once

#include <stdint.h>

static inline int dcp_cue_in_range(uint32_t cue, uint32_t first, uint32_t last) {
    return cue - first <= last - first;
}

static inline int dcp_is_japanese_npc_cue(uint32_t cue) {
    return dcp_cue_in_range(cue, 10040, 10049) || dcp_cue_in_range(cue, 10240, 10249) ||
           dcp_cue_in_range(cue, 10440, 10449) || dcp_cue_in_range(cue, 10540, 10549) ||
           dcp_cue_in_range(cue, 10640, 10649) || dcp_cue_in_range(cue, 10840, 10849) ||
           dcp_cue_in_range(cue, 7100, 7125) || dcp_cue_in_range(cue, 7200, 7225) ||
           dcp_cue_in_range(cue, 8800, 8825);
}
