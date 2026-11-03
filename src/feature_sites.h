/* Structural locators for the executable-dependent features of every supported build but CUSA10454 1.00, whose tables
 * are the reference: Cheats, Japanese NPC Voices and True Ultrawide. A feature resolves only when every one of its sites is found the expected
 * number of times with the verified bytes, the sites keep their relations and none overlaps another; otherwise nothing
 * is returned and `failure` names the first site that did not resolve. Reads the image only. The loader calls these for
 * a supported build whose profile asks for them, never for an unknown executable. */
#ifndef DCP_FEATURE_SITES_H
#define DCP_FEATURE_SITES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t begin, end;
} DcpExecRange;

typedef struct {
    const uint8_t *bytes;
    uint64_t size;
    const DcpExecRange *executable;
    size_t executable_count;
} DcpImageView;

typedef struct {
    size_t found, expected;
    char failure[192];
} DcpLocateReport;

#define DCP_CHEAT_HOOKS 25
#define DCP_CHEAT_FUNCTIONS 15

/* The game's struct displacements the cheat handlers use, as the game's own instructions encode them. */
typedef enum {
    DCP_LAYOUT_DAMAGE_UNIT_FRAME,
    DCP_LAYOUT_CONTEXT_VIRTUAL,
    DCP_LAYOUT_UNIT_OWNER,
    DCP_LAYOUT_UNIT_PLAYER_SLOT,
    DCP_LAYOUT_UNIT_CLASS,
    DCP_LAYOUT_UNIT_LEVEL,
    DCP_LAYOUT_UNIT_HP,
    DCP_LAYOUT_UNIT_STAT,
    DCP_LAYOUT_UNIT_XP,
    DCP_LAYOUT_UNIT_XP_PROGRESS,
    DCP_LAYOUT_UNIT_EQUIPMENT,
    DCP_LAYOUT_OWNER_CATEGORY,
    DCP_LAYOUT_OWNER_UNIT,
    DCP_LAYOUT_OWNER_CLASS,
    DCP_LAYOUT_ARROW_COUNT,
    DCP_LAYOUT_EQUIPMENT_OWNER,
    DCP_LAYOUT_EQUIPMENT_BAG,
    DCP_LAYOUT_ITEM_FLAGS,
    DCP_LAYOUT_ITEM_DURABILITY,
    DCP_LAYOUT_ITEM_MAX_DURABILITY,
    DCP_LAYOUT_ITEM_USES,
    DCP_LAYOUT_GOLD,
    DCP_LAYOUT_GAME_SAVE_BASE,
    DCP_LAYOUT_SAVE_RECORD_OFFSET,
    DCP_LAYOUT_SAVE_RECORD_SIZE,
    DCP_LAYOUT_GAME_GOLD_CONTEXT,
    DCP_LAYOUT_SKILL_POINTS,
    DCP_LAYOUT_SKILL_POINTS_MIRROR,
    DCP_LAYOUT_RECORD_LEVEL,
    DCP_LAYOUT_RECORD_XP_POOL,
    DCP_LAYOUT_RECORD_XP_PROGRESS,
    DCP_LAYOUT_LEVEL_XP,
    DCP_LAYOUT_ABSOLUTE_MAX_LEVEL,
    DCP_LAYOUT_COUNT
} DcpLayoutField;

/* Order of DcpCheatSites.functions. */
typedef enum {
    DCP_CHEAT_FN_IS_DEAD,
    DCP_CHEAT_FN_REFILL,
    DCP_CHEAT_FN_GET_GOLD,
    DCP_CHEAT_FN_ADD_GOLD,
    DCP_CHEAT_FN_RECORD_MODE,
    DCP_CHEAT_FN_RECORD_STAGE,
    DCP_CHEAT_FN_MAX_LEVEL,
    DCP_CHEAT_FN_LEVEL_ENTRY,
    DCP_CHEAT_FN_LEVEL_PATTERN,
    DCP_CHEAT_FN_SET_LEVEL,
    DCP_CHEAT_FN_WRITE_STATS,
    DCP_CHEAT_FN_EQUIPMENT_COUNT,
    DCP_CHEAT_FN_EQUIPMENT_ITEM,
    DCP_CHEAT_FN_IS_GEAR,
    DCP_CHEAT_FN_SET_DURABILITY
} DcpCheatFunction;

typedef struct {
    uint32_t hooks[DCP_CHEAT_HOOKS];         /* in the order of cheats::Hooks() */
    uint32_t functions[DCP_CHEAT_FUNCTIONS]; /* in the order of DcpCheatFunction */
    uint32_t layout[DCP_LAYOUT_COUNT];       /* the displacement each DcpLayoutField reads in this build */
} DcpCheatSites;

/* What the online party snapshot reads: the entry of the player unit's per-tick update and the four displacements of
 * the unit and its owner it observes. Found without the rest of the cheat set. */
typedef struct {
    uint32_t tick;
    uint32_t unit_owner, unit_slot, unit_class, owner_category;
} DcpPartySites;

typedef struct {
    uint32_t npc_cue;          /* the cue mapper's `mov eax, 0x1971b` */
    uint32_t resource_acquire; /* the resource manager's acquire */
} DcpVoiceSites;

/* The title menu's Exit Game row: the sheet loader's fill, the call that sizes the main menu's item list (a sixth row
 * widget is added just before it), the two help-text limits, the two loads of the table that maps a menu position to
 * its layout part, and the title's per-frame read of the menu result. */
typedef struct {
    uint32_t sheet_fill;
    uint32_t row_widget;
    uint32_t help_limits[2];
    uint32_t part_table_loads[2];
    uint32_t menu_tick;
} DcpExitGameSites;

#define DCP_ORTHO_LOADS 6
#define DCP_ULTRAWIDE_SITE_COUNT 22
typedef struct {
    uint32_t submit;
    uint32_t fill_calls[3];
    uint32_t frame_commit;
    uint32_t queue;
    uint32_t part_queue_call;
    uint32_t model_draw;
    uint32_t widget_draw_call;
    uint32_t map_draw_call;
    uint32_t facility_draw_call;
    uint32_t worldmap_draw;
    uint32_t fade_jumps[2];
    uint32_t camera_view_call;
    uint32_t view_build;
    uint32_t ortho_loads[DCP_ORTHO_LOADS];
} DcpUltrawideSites;

/* What a supported build's profile asked the loader to resolve, resolved once per process. */
typedef struct {
    int cheats, voices, ultrawide;
    DcpCheatSites cheat_sites;
    DcpVoiceSites voice_sites;
    DcpUltrawideSites ultrawide_sites;
    char cheat_failure[192], voice_failure[192], ultrawide_failure[192];
} DcpFeatureSites;

/* 1 when any two of the `count` ranges share a byte. */
int dcp_ranges_overlap(const DcpExecRange *ranges, size_t count);

int dcp_locate_cheat_sites(const DcpImageView *image, DcpCheatSites *out, DcpLocateReport *report);
int dcp_locate_party_sites(const DcpImageView *image, DcpPartySites *out, DcpLocateReport *report);
int dcp_locate_exit_game_sites(const DcpImageView *image, DcpExitGameSites *out, DcpLocateReport *report);
int dcp_locate_voice_sites(const DcpImageView *image, DcpVoiceSites *out, DcpLocateReport *report);
int dcp_locate_ultrawide_sites(const DcpImageView *image, DcpUltrawideSites *out, DcpLocateReport *report);

static inline const char *dcp_layout_field_name(DcpLayoutField field) {
    static const char *const names[DCP_LAYOUT_COUNT] = {
        "damage unit frame", "unit context virtual", "unit owner", "unit player slot", "unit class", "unit level", "unit hp",
        "unit stat", "unit xp", "unit xp progress", "unit equipment", "owner category", "owner unit", "owner class",
        "arrow count", "equipment owner", "equipment bag", "item flags", "item durability", "item max durability",
        "item uses", "gold", "game save base", "save record offset", "save record size", "game gold context",
        "skill points", "skill points mirror", "record level", "record xp pool", "record xp progress", "level xp",
        "absolute max level"};
    return field >= 0 && field < DCP_LAYOUT_COUNT ? names[field] : "";
}

/* The layout probes' masked patterns and how many times each occurs in a build, for the tests. */
size_t dcp_layout_probe_count(void);
const char *dcp_layout_probe_pattern(size_t index, size_t *occurrences);
/* The `i`th displacement a probe reads: which field, and where in the pattern it sits and how wide it is. 0 past the last. */
int dcp_layout_probe_field(size_t index, size_t i, DcpLayoutField *field, size_t *position, size_t *width);

#ifdef __cplusplus
}
#endif

#endif
