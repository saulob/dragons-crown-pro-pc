/* Voice Language: the setting, its legacy migration, and the voice families of the shared DRACRO.CPK. Every family has
 * a Japanese bank and an English bank whose number and cue IDs are the Japanese ones + 100000. */
#pragma once

#include <stdint.h>
#include <string.h>

typedef enum {
    DCP_VOICE_DEFAULT = 0,
    DCP_VOICE_ENGLISH = 1,
    DCP_VOICE_JAPANESE = 2,
    DCP_VOICE_LANGUAGE_COUNT = 3,
} DcpVoiceLanguage;

#define DCP_VOICE_LANGUAGE_KEY "voice_language"
#define DCP_VOICE_LEGACY_KEY "dcp_japanese_npc_voices"
#define DCP_VOICE_ENGLISH_OFFSET 100000u

/* The cue mapper's mode argument: 1 maps a cue to its English twin, any other value keeps the Japanese cue. */
#define DCP_VOICE_MODE_JAPANESE 0
#define DCP_VOICE_MODE_ENGLISH 1

static inline const char *dcp_voice_language_key(int language) {
    static const char *const keys[DCP_VOICE_LANGUAGE_COUNT] = {"default", "english", "japanese"};
    return language >= 0 && language < DCP_VOICE_LANGUAGE_COUNT ? keys[language] : keys[0];
}

static inline const char *dcp_voice_language_name(int language) {
    static const char *const names[DCP_VOICE_LANGUAGE_COUNT] = {"Game Default", "English", "Japanese"};
    return language >= 0 && language < DCP_VOICE_LANGUAGE_COUNT ? names[language] : names[0];
}

/* -1 when `value` names no language. */
static inline int dcp_voice_language_parse(const char *value) {
    if (!value) return -1;
    for (int i = 0; i < DCP_VOICE_LANGUAGE_COUNT; ++i) {
        if (!strcmp(value, dcp_voice_language_key(i))) return i;
    }
    return -1;
}

/* A valid voice_language wins; without one, dcp_japanese_npc_voices = 1 means Japanese and anything else Game Default.
 * `saved` is -1 when voice_language is absent or invalid, `legacy_on` 1 only for a true legacy value. */
static inline int dcp_voice_language_resolve(int saved, int legacy_on) {
    if (saved >= 0 && saved < DCP_VOICE_LANGUAGE_COUNT) return saved;
    return legacy_on == 1 ? DCP_VOICE_JAPANESE : DCP_VOICE_DEFAULT;
}

static inline int dcp_voice_mode_of(int language) {
    return language == DCP_VOICE_ENGLISH ? DCP_VOICE_MODE_ENGLISH : DCP_VOICE_MODE_JAPANESE;
}

typedef enum {
    DCP_VOICE_NONE = 0,
    DCP_VOICE_PLAYER,   /* class voices and camp voices: own, other players', resurrected and AI party members */
    DCP_VOICE_NPC,      /* town and event NPCs, the thieves and the citizens */
    DCP_VOICE_ENEMY,    /* enemies and the voiced NPC fighters that join a fight */
    DCP_VOICE_NARRATOR,
    DCP_VOICE_CATEGORY_COUNT,
} DcpVoiceCategory;

#define DCP_VOICE_CATEGORY_BIT(category) (1u << (category))
#define DCP_VOICE_ALL_CATEGORIES                                                                                  \
    (DCP_VOICE_CATEGORY_BIT(DCP_VOICE_PLAYER) | DCP_VOICE_CATEGORY_BIT(DCP_VOICE_NPC) |                          \
     DCP_VOICE_CATEGORY_BIT(DCP_VOICE_ENEMY) | DCP_VOICE_CATEGORY_BIT(DCP_VOICE_NARRATOR))

static inline const char *dcp_voice_category_name(int category) {
    static const char *const names[DCP_VOICE_CATEGORY_COUNT] = {"none", "player", "npc", "enemy", "narrator"};
    return category >= 0 && category < DCP_VOICE_CATEGORY_COUNT ? names[category] : names[0];
}

static inline int dcp_cue_in_range(uint32_t cue, uint32_t first, uint32_t last) {
    return cue - first <= last - first;
}

#define DCP_PLAYER_VOICE_FAMILY_COUNT 6
static const uint32_t kDcpPlayerVoiceFamilies[DCP_PLAYER_VOICE_FAMILY_COUNT] = {1000, 1100, 1200, 1300, 1400, 1500};
#define DCP_CAMP_VOICE_FAMILY_COUNT 6
static const uint32_t kDcpCampVoiceFamilies[DCP_CAMP_VOICE_FAMILY_COUNT] = {2900, 2910, 2920, 2930, 2940, 2950};
#define DCP_GIMMICK_VOICE_FAMILY_COUNT 6
static const uint32_t kDcpGimmickVoiceFamilies[DCP_GIMMICK_VOICE_FAMILY_COUNT] = {10000, 10200, 10400, 10500, 10600, 10800};
#define DCP_NPC_ENEMY_BANK_FAMILY_COUNT 3
static const uint32_t kDcpNpcEnemyBankFamilies[DCP_NPC_ENEMY_BANK_FAMILY_COUNT] = {7100, 7200, 8800};
#define DCP_ENEMY_VOICE_FAMILY_COUNT 40
static const uint32_t kDcpEnemyVoiceFamilies[DCP_ENEMY_VOICE_FAMILY_COUNT] = {
    3000, 3100, 3300, 3400, 3700, 3800, 4200, 4300, 4500, 4700, 5600, 5700,
    5800, 6200, 7300, 7400, 7900, 8200, 8300, 8400, 8500, 8600, 8700, 90000,
    90100, 90200, 90300, 90400, 90500, 90600, 90700, 90800, 90900, 91000, 91100, 91200,
    91300, 91400, 91500, 91600,
};
#define DCP_NARRATOR_COUNT 7 /* the narrator, then the Fighter, Amazon, Elf, Dwarf, Sorceress and Wizard narrators */
#define DCP_NARRATOR_FIRST_BASE 20000u
#define DCP_NARRATOR_BASE_STEP 10000u

/* The family (Japanese bank number) of a cue the game's cue mapper gives an English twin, 0 for any other cue. These
 * are the mapper's own ranges; cues are the Japanese IDs the game passes to it. */
static inline uint32_t dcp_voice_cue_family(uint32_t cue) {
    for (int i = 0; i < DCP_PLAYER_VOICE_FAMILY_COUNT; ++i) {
        if (dcp_cue_in_range(cue, kDcpPlayerVoiceFamilies[i], kDcpPlayerVoiceFamilies[i] + 18)) return kDcpPlayerVoiceFamilies[i];
    }
    for (int i = 0; i < DCP_CAMP_VOICE_FAMILY_COUNT; ++i) {
        if (dcp_cue_in_range(cue, kDcpCampVoiceFamilies[i], kDcpCampVoiceFamilies[i] + 3)) return kDcpCampVoiceFamilies[i];
    }
    if (dcp_cue_in_range(cue, 10010, 10015)) return 10000;
    for (int i = 0; i < DCP_GIMMICK_VOICE_FAMILY_COUNT; ++i) {
        const uint32_t family = kDcpGimmickVoiceFamilies[i];
        if (dcp_cue_in_range(cue, family + 40, family + 49)) return family;
    }
    for (int i = 0; i < DCP_NPC_ENEMY_BANK_FAMILY_COUNT; ++i) {
        const uint32_t family = kDcpNpcEnemyBankFamilies[i];
        if (dcp_cue_in_range(cue, family, family + (family == 8800 ? 49 : 25))) return family;
    }
    for (int i = 0; i < DCP_ENEMY_VOICE_FAMILY_COUNT; ++i) {
        if (dcp_cue_in_range(cue, kDcpEnemyVoiceFamilies[i], kDcpEnemyVoiceFamilies[i] + 25)) return kDcpEnemyVoiceFamilies[i];
    }
    return 0;
}

static inline int dcp_voice_cue_category(uint32_t cue) {
    const uint32_t family = dcp_voice_cue_family(cue);
    if (!family) return DCP_VOICE_NONE;
    if (family < 3000) return DCP_VOICE_PLAYER;
    if (family >= 10000 && family < 11000) return DCP_VOICE_NPC;
    for (int i = 0; i < DCP_NPC_ENEMY_BANK_FAMILY_COUNT; ++i) {
        if (kDcpNpcEnemyBankFamilies[i] == family) return DCP_VOICE_NPC;
    }
    return DCP_VOICE_ENEMY;
}

/* 1 for a narrator bank number or cue base: 20000..80000 (Japanese) or 120000..180000 (English), step 10000. */
static inline int dcp_narrator_base_valid(uint32_t base) {
    const uint32_t japanese = base >= DCP_VOICE_ENGLISH_OFFSET ? base - DCP_VOICE_ENGLISH_OFFSET : base;
    return japanese % DCP_NARRATOR_BASE_STEP == 0 &&
           dcp_cue_in_range(japanese, DCP_NARRATOR_FIRST_BASE,
                            DCP_NARRATOR_FIRST_BASE + (DCP_NARRATOR_COUNT - 1) * DCP_NARRATOR_BASE_STEP);
}

/* The same narrator in `language`; the base unchanged for Game Default or a value that is not a narrator base. */
static inline uint32_t dcp_narrator_base_for(uint32_t base, int language) {
    if (!dcp_narrator_base_valid(base) || language == DCP_VOICE_DEFAULT) return base;
    const uint32_t japanese = base >= DCP_VOICE_ENGLISH_OFFSET ? base - DCP_VOICE_ENGLISH_OFFSET : base;
    return language == DCP_VOICE_ENGLISH ? japanese + DCP_VOICE_ENGLISH_OFFSET : japanese;
}
