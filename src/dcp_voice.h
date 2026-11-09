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

/* The enemy and AI ally voice families that have banks in both languages (7100, 7200 and 8800 are NPC families above).
 * The cue mapper moves the first 26 cues of each one +100000. */
#define DCP_ENEMY_VOICE_FAMILY_COUNT 40
static const uint32_t kDcpEnemyVoiceFamilies[DCP_ENEMY_VOICE_FAMILY_COUNT] = {
    3000, 3100, 3300, 3400, 3700, 3800, 4200, 4300, 4500, 4700, 5600, 5700,
    5800, 6200, 7300, 7400, 7900, 8200, 8300, 8400, 8500, 8600, 8700, 90000,
    90100, 90200, 90300, 90400, 90500, 90600, 90700, 90800, 90900, 91000, 91100, 91200,
    91300, 91400, 91500, 91600,
};

static inline int dcp_is_japanese_enemy_cue(uint32_t cue) {
    for (int i = 0; i < DCP_ENEMY_VOICE_FAMILY_COUNT; ++i) {
        if (dcp_cue_in_range(cue, kDcpEnemyVoiceFamilies[i], kDcpEnemyVoiceFamilies[i] + 25)) return 1;
    }
    return 0;
}
