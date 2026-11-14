/* Boss Battle: with enable_boss_battle on, the Dragon's Haven Inn menu gains a Boss Battle row after Report adventurers.
 * The row opens the Boss Battle screen on the Inn's native Options page; a boss chosen there sets the campaign aside
 * (saves protected, campaign memory kept), sends the Inn's party to the game server as leaving the Inn does, then
 * requests the boss's arena. Every way back ends in the Inn, where the campaign is put back and the screen opens again. */
#ifndef DCP_BOSS_BATTLE_H
#define DCP_BOSS_BATTLE_H

#include <stddef.h>
#include <stdint.h>

#include "boss_battle_record.h"
#include "feature_sites.h"
#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { BOSS_BATTLE_ENCOUNTERS = 20 };

typedef struct {
    const char *id;
    const char *name;
    const char *round;       /* the round file the scene request names */
    int round_index;         /* the round's place in the game's round table, as the scene reports it */
    int stage;               /* the arena's place in the round's stage list */
    int last_stage;          /* the fight's last room; stage itself unless the fight moves between rooms */
    const char *stage_name;
    char route;              /* 'A', 'B', or 0 outside the two-route dungeons */
    int labyrinth_floor;     /* the Labyrinth of Chaos floor index the scene is set to before the request, or -1 */
    int story_gated;         /* the room places its boss only for characters past Lucain's Tower (story step 45) */
    const char *unavailable; /* why it cannot be started yet; NULL when it can */
    const char *pending;     /* startable, but not yet confirmed in a full run: what is unconfirmed; NULL when proven */
} BossBattleEncounter;

/* Menu order, top to bottom; NULL out of range. */
const BossBattleEncounter *boss_battle_encounter(int index);
/* The two-digit number the menu shows: index + 1, 0 out of range. */
int boss_battle_number(int index);
/* "01 Harpy"; 0 out of range. */
int boss_battle_label(int index, char *out, size_t size);
int boss_battle_startable_count(void);

/* The Inn's own stage in Hydeland's round, entered by its round-start entry. */
typedef struct {
    const char *round;
    int stage;
    int entry;
    const char *stage_name;
} BossBattleInnEntry;
extern const BossBattleInnEntry boss_battle_inn_entry;
enum { BOSS_BATTLE_TOWN_ROUND = 9 };

/* The scene manager on a frame: its act, the act's step, the round, the stage, the stage a request asked for until the
 * stage setup has used it (-1), and the entry the stage was entered by. */
typedef struct {
    int act, step, round, stage, next_stage, entry;
} BossBattleScene;

/* The scene is set up in one of the encounter's rooms. */
int boss_battle_in_arena(const BossBattleEncounter *encounter, const BossBattleScene *scene);

/* While a session runs: the round's boss room is story gated, so its placement groups are chosen as for a character
 * past the story's step 45. */
int boss_battle_story_gated_round(int round);

typedef enum {
    BOSS_BATTLE_OFF,
    BOSS_BATTLE_TRAVELLING,   /* the arena was requested and is not set up yet */
    BOSS_BATTLE_INITIALIZING, /* the arena is set up; the fight is not playable yet */
    BOSS_BATTLE_FIGHTING,     /* playable: the attempt is recorded */
    BOSS_BATTLE_RESOLVING,    /* decided (Win, Death, Quit or cancelled): leaving at the next play frame */
    BOSS_BATTLE_RETURNING     /* the Inn was requested */
} BossBattleState;

BossBattleState boss_battle_state(void);
const char *boss_battle_state_text(BossBattleState state);

typedef enum {
    BOSS_BATTLE_OUTCOME_NONE,
    BOSS_BATTLE_OUTCOME_WIN,
    BOSS_BATTLE_OUTCOME_DEATH,
    BOSS_BATTLE_OUTCOME_QUIT,
    BOSS_BATTLE_OUTCOME_CANCELLED /* ended before the fight was playable: nothing recorded */
} BossBattleOutcomeKind;
BossBattleOutcomeKind boss_battle_outcome(void);

/* The milestones of a fight's start; Playable starts the attempt. */
enum {
    BOSS_BATTLE_ARENA_LOADED = 1 << 0,
    BOSS_BATTLE_PLAYER_SPAWNED = 1 << 1,
    BOSS_BATTLE_PARTY_READY = 1 << 2,
    BOSS_BATTLE_ENCOUNTER_READY = 1 << 3,
    BOSS_BATTLE_PLAYABLE = 1 << 4
};
unsigned boss_battle_milestones(void);
/* The session's number in this run's log ("Boss Battle [n]"); 0 outside a session. */
unsigned boss_battle_session_id(void);

/* Only Offline exists for now; the request refuses any mode that is still pending. */
typedef enum { BOSS_BATTLE_MODE_OFFLINE, BOSS_BATTLE_MODE_ONLINE, BOSS_BATTLE_MODES } BossBattleMode;
BossBattleMode boss_battle_mode(void);
const char *boss_battle_mode_pending(BossBattleMode mode);

/* enable_boss_battle: read each time the Inn builds its menu. */
void boss_battle_set_enabled(int enabled);
int boss_battle_enabled(void);

/* The campaign boundary. protect moves save operations away from the campaign saves (set by the loader), snapshot keeps
 * the campaign state in memory (set by the install); restore puts that state back and release gives the campaign saves
 * back. A session begins with protect then snapshot and ends with restore then release. */
typedef int (*BossBattleGuardFn)(void);
typedef void (*BossBattleUndoFn)(void);
void boss_battle_set_save_guard(BossBattleGuardFn protect, BossBattleUndoFn release);
void boss_battle_set_memory_guard(BossBattleGuardFn snapshot, BossBattleGuardFn restore);

/* The campaign in memory (boss_battle_campaign.c): the save object the game rewrites whole and the live play data of
 * its user, which the Inn commits into the save. A snapshot copies both and changes nothing the game uses; while it is
 * held every save write is given the kept save, and the restore copies both back into the same objects. A restore that
 * cannot find them keeps every later save write on the kept save until the game exits. */
enum { BOSS_BATTLE_SAVE_BYTES = 0x20eb18, BOSS_BATTLE_PLAY_DATA_BYTES = 0xaf8d4 };
void boss_battle_campaign_configure(uint64_t play_data_table);
/* The game context whose save the next snapshot keeps. */
void boss_battle_campaign_use(uint64_t context);
int boss_battle_campaign_snapshot(void);
int boss_battle_campaign_restore(void);
/* The save the writer copies: the kept one while it must be, `save` otherwise. */
uint64_t boss_battle_campaign_save_source(uint64_t save);
int boss_battle_campaign_held(void);
/* The live play data the snapshot reads, for the log; 0 when there is none. */
uint64_t boss_battle_campaign_play_data(void);
void boss_battle_campaign_reset(void);

/* 1 from a successful begin until the campaign is put back: campaign saves and trophies are off limits. */
int boss_battle_session_active(void);

/* The Inn menu offers the row: enabled, installed, and no session running. */
int boss_battle_inn_row_wanted(void);

typedef enum {
    BOSS_BATTLE_QUEUED,
    BOSS_BATTLE_REFUSED_BUSY,
    BOSS_BATTLE_REFUSED_ENCOUNTER,
    BOSS_BATTLE_REFUSED_MODE
} BossBattleRequestResult;

/* The boss chosen in the Inn's boss list; begin_session runs on the same frame. */
BossBattleRequestResult boss_battle_request(int index);
/* Protect, snapshot and open the session for the queued boss; 1 when the Inn may send it now (`index` gets it). */
int boss_battle_begin_queued(int *index);

/* The campaign character, read from the game when the boss list opens. `key` names it in the Boss Battle records. */
void boss_battle_set_character(const char *name, const char *class_name, int level);
int boss_battle_character_text(char *out, size_t size);
int boss_battle_character_key(char *out, size_t size);

/* A short message for the player after the game thread acted on a request; 1 when one was waiting. */
int boss_battle_take_notice(char *out, size_t size);

/* What the game shows of the fight on a scene frame of a session. */
typedef struct {
    BossBattleScene scene;
    int player_record;    /* party slot 0 holds the local character */
    int player_unit;      /* and its unit exists */
    int player_dead;      /* that unit is in the dead state */
    int player_down;      /* that unit is down, waiting for a respawn or the continue offer */
    int player_lives;     /* respawns it has left */
    int party_slots;      /* occupied party slots */
    int party_units;      /* of them, the ones whose unit exists */
    int bosses;           /* bosses on the boss HP gauge */
    uint64_t player_unit_address, boss_address;
    int boss_hp;
    int end_kind;         /* the stage end's kind, meaningful in the stage-end act */
    int clear_choice;     /* the stage clear screen's answer: -1 none, 1 return to town */
} BossBattleProbe;

/* The scene's stage-end act and its steps the session acts on. */
enum {
    BOSS_BATTLE_ACT_STAGE_END = 5,
    BOSS_BATTLE_STEP_CLEAR_CHOICE = 7,
    BOSS_BATTLE_STEP_AFTER_CHOICE = 8,
    BOSS_BATTLE_CHOICE_RETURN = 1
};

typedef enum {
    BOSS_BATTLE_FRAME_NONE,
    BOSS_BATTLE_FRAME_REQUEST_INN,  /* send the Inn request now, then call boss_battle_inn_requested */
    BOSS_BATTLE_FRAME_ANSWER_CHOICE /* answer the skipped clear screen with return to town and go on to the next step */
} BossBattleFrameAction;

/* Game thread, once per scene frame of a session. */
BossBattleFrameAction boss_battle_frame(const BossBattleProbe *probe);
void boss_battle_inn_requested(void);

/* The one way to quit a fight, for F4 and the pause menu alike. Accepted only while a session is travelling,
 * initializing, fighting or resolving; before Playable it cancels without recording anything, after it records Quit. */
typedef enum { BOSS_BATTLE_QUIT_KEY, BOSS_BATTLE_QUIT_MENU } BossBattleQuitSource;
int boss_battle_request_quit(BossBattleQuitSource source);
/* Bare F4 from the window thread: never for a repeat, a modifier (Alt+F4 closes the game), or while a menu or a text
 * entry owns the input. */
int boss_battle_quit_key(int repeat, int modifiers, int input_owned);
int boss_battle_quit_available(void);

/* Game thread signals from the hooks. A boss whose placement ends the stage died and the game requests the round end
 * with that boss's end kind: the Win once the fight is playable. */
void boss_battle_boss_cleared(int end_kind);
/* The whole party is down and the game would offer its game over: 1 when Boss Battle takes the defeat instead. */
int boss_battle_party_wiped(void);

/* The stage-end act in a session: 1 to skip the stage clear screen (its continue choice) or the results screen (loot,
 * experience, General results, story), and 1 when its return to Hydeland goes to the Inn instead. */
int boss_battle_skip_clear_screen(void);
int boss_battle_skip_results_screen(void);
int boss_battle_town_return(void);
/* The results screen of a Win runs its experience award and level-up panels; 1 when it then ends there, before the
 * General results, the story and the item menu. */
int boss_battle_results_level_up_done(void);

/* The Boss Battle character: at a session start the campaign record's progression and the saved one give the
 * effective one (1 when the record must take it); after a Win's level-up the record's is saved. The campaign put back
 * afterwards never keeps it. */
int boss_battle_progress_begin(const BossBattleProgress *campaign, BossBattleProgress *effective);
void boss_battle_progress_won(const BossBattleProgress *after);
/* The later of the two: the saved Boss Battle progression, or the campaign's once it caught up. */
BossBattleProgress boss_battle_progress_effective(const BossBattleProgress *saved, const BossBattleProgress *campaign);

/* Game thread, as the title scene is created: a session still running is ended there. */
void boss_battle_title_reached(void);
/* Game thread, as an Inn is built, after it moved the party back into the play data: a session on its way back is ended
 * there, before the Inn reads the play data. */
void boss_battle_inn_arrived(void);

/* The Inn menu with Boss Battle: the six native rows with Boss Battle inserted after Report adventurers. The native code
 * keeps its own row numbers; these convert between them and the rows shown. */
enum { BOSS_BATTLE_INN_ROW = 3, BOSS_BATTLE_INN_ROWS = 7, BOSS_BATTLE_INN_NO_ROW = -100, BOSS_BATTLE_INN_CALLS = 11 };
int boss_battle_inn_native_row(int row);
int boss_battle_inn_display_row(int row);

/* The Boss Battle screen is the native Options page the Inn opens for its Options row, given other rows: row 0 the
 * title bar (the highlighted boss), rows 1-6 six of the bosses in the selectable value column with the highlighted
 * boss's statistics in the static label column, row 7 the bottom action (Return to tavern). The six boss rows scroll
 * through the twenty bosses. */
enum {
    BOSS_BATTLE_SCREEN_ROWS = 8,
    BOSS_BATTLE_SCREEN_TITLE_ROW = 0,
    BOSS_BATTLE_SCREEN_BOSS_ROWS = 6,
    BOSS_BATTLE_SCREEN_RETURN_ROW = 7,
    BOSS_BATTLE_INN_OPTIONS_ROW = 3, /* the native Inn row that opens the Options page */
    BOSS_BATTLE_RETURN_TO_TAVERN_RECORD = 1120
};
typedef enum { BOSS_BATTLE_SCREEN_NONE, BOSS_BATTLE_SCREEN_START, BOSS_BATTLE_SCREEN_CLOSE } BossBattleScreenChoice;

/* The Inn asks for its Options page as the Boss Battle screen; the page reports itself built, then closed. A request
 * the Inn never turns into a page is dropped after a few menu frames (`waiting` 0 then). */
void boss_battle_screen_request(void);
int boss_battle_screen_requested(void);
int boss_battle_screen_waiting(void);
/* The page is the screen: the cursor row it opens on, the last boss chosen in view. */
int boss_battle_screen_built(void);
int boss_battle_screen_shown(void);
/* The screen closed; 1 with the boss chosen in it. */
int boss_battle_screen_closed(int *index);
void boss_battle_screen_reset(void);
int boss_battle_screen_offset(void);
/* The boss of a row, -1 for the title bar and the bottom action. */
int boss_battle_screen_boss(int row);
/* The native cursor about to move from `row`: 1 when the boss rows scroll instead and the cursor stays. */
int boss_battle_screen_move(int row, int down);
/* A list result of the page (row, -1 nothing): a boss row starts it, the bottom action closes. */
BossBattleScreenChoice boss_battle_screen_choose(int result, int *index);
/* The texts of the page for the cursor row: a row's boss (the selectable value column) and the highlighted boss's
 * statistic on that row (the static label column), the title bar's boss, the help line. */
void boss_battle_screen_label(int row, char *out, size_t size);
void boss_battle_screen_statistic(int row, int cursor_row, char *out, size_t size);
void boss_battle_screen_title(int cursor_row, char *out, size_t size);
void boss_battle_screen_help(int cursor_row, char *out, size_t size);
/* The boss the screen opens on: the last one chosen. */
int boss_battle_menu_cursor(void);
/* The Inn's first menu frame after a return opens the screen once; 0 after a failed restore. */
int boss_battle_take_menu_reopen(void);
/* MM:SS.mmm */
void boss_battle_format_time(uint32_t ms, char *out, size_t size);

typedef enum {
    BOSS_BATTLE_INN_IDLE, /* the menu runs as the game wants */
    BOSS_BATTLE_INN_HOLD  /* this frame the menu must not act */
} BossBattleInnAction;

/* Game thread, once per frame of the Inn menu: a session that never left this Inn is ended here; one on its way out
 * holds the menu. */
BossBattleInnAction boss_battle_inn_frame(void);
void boss_battle_request_sent(int index, const BossBattleScene *from, int party_sent);
void boss_battle_request_failed(int index, const char *why);

/* Clears every session state; the tests use it between cases. */
void boss_battle_reset(void);

/* The build's sites, for the build key ("CUSA10454-1.00"). */
typedef struct {
    uint64_t hook;         /* the scene frame's call into the menu handler */
    uint64_t request;      /* the scene request builder */
    uint64_t scene_vtable; /* image offset of the scene manager's vtable, as objects hold it */
    uint64_t inn_built;    /* after the Inn menu builder cut its item list */
    uint64_t inn_result;   /* the Inn menu frame's read of the list result */
    uint64_t inn_help;     /* the Inn menu frame's help text for the cursor row */
    uint64_t inn_help_table;
    uint64_t allocate, widget, add_widget, add_item; /* the game functions the Inn menu builder calls */
    uint64_t set_disabled, set_cursor;               /* the list's row enable switch and cursor */
    uint64_t inn_party;                              /* the Inn's leave: its party sent to the game server */
    uint64_t inn_calls[BOSS_BATTLE_INN_CALLS];       /* the Inn's list calls that pass a native row */
    uint64_t arrange_story;    /* the placement group check's call of the story-below-45 test */
    uint64_t story_test;       /* that test */
    uint64_t story_clear;      /* the result's check whether a clear advances the story tier */
    uint64_t dragon_talismans; /* the Ancient Dragon's check of the nine talismans */
    uint64_t title;            /* the scene factory's call of the title constructor */
    uint64_t inn_flushed;      /* the Inn constructor, after it moved the party records back into the play data */
    uint64_t save_copy;        /* the save writer's call that copies the save for the write */
    uint64_t play_data_table;  /* the play data of each user */
    uint64_t party_wiped;      /* the all-down check's game over request */
    uint64_t boss_cleared;     /* a stage-ending boss's death: the round end request */
    uint64_t clear_screen;     /* the stage-end act opening the stage clear screen */
    uint64_t results_screen;   /* the stage-end act opening the results screen */
    uint64_t town_return;      /* the stage-end act's request for Hydeland */
    uint64_t set_step;         /* the scene's act and step setter */
    uint64_t set_text;         /* a text's plain string setter */
    uint64_t set_message;      /* a text's message setter (SystemMsg number) */
    uint64_t options_title;    /* the Inn's Options page: its title widget's message */
    uint64_t options_built;    /* after the page's rows are built */
    uint64_t options_update;   /* the page's per-frame row logic */
    uint64_t options_result;   /* after the page's list read its input (eax the result) */
    uint64_t list_up, list_down; /* the list's cursor moves, every list's */
    uint64_t options_body_vtable;
    uint64_t row_frame;        /* a layout's frame setter: a row's background */
    uint64_t results_level_done;               /* the results screen asking for its General results */
    uint64_t set_level, level_pattern, write_stats; /* a character record's level, stat pattern and stats */
} BossBattleSites;

/* The game functions the Inn hooks call. Set by the install; the tests set their own. */
typedef struct {
    uint64_t(GUEST_HOOK_ABI *allocate)(uint64_t size);
    void(GUEST_HOOK_ABI *widget)(uint64_t widget, uint64_t manager);
    int(GUEST_HOOK_ABI *add_widget)(uint64_t list, uint64_t widget);
    void(GUEST_HOOK_ABI *add_item)(uint64_t list, int message, uint64_t sheet);
    void(GUEST_HOOK_ABI *set_disabled)(uint64_t list, int row, int disabled);
    void(GUEST_HOOK_ABI *set_cursor)(uint64_t list, int row, int unused, int notify);
    void(GUEST_HOOK_ABI *inn_party)(uint64_t inn);
} BossBattleInnFunctions;
void boss_battle_set_inn_functions(const BossBattleInnFunctions *functions);
/* The Inn's help message per native row; set by the install. */
void boss_battle_set_inn_help_table(uint64_t table);

/* 1 for a build whose sites are listed. */
int boss_battle_build_listed(const char *game);

/* 1 with every site verified for a listed build; 0 and `why` otherwise. Reads the image only. */
int boss_battle_locate(const unsigned char *image, uint64_t size, const char *game, BossBattleSites *sites,
                       const char **why);

size_t boss_battle_stub_bytes(void);

/* Installs the hooks; `town` gives the lead character selector. Returns 1 when Boss Battle can be offered. */
int boss_battle_install(unsigned char *image, uint64_t size, uint64_t entry, const char *game, int online,
                        const DcpEarlyStorySites *town, unsigned char *stubs, size_t stubs_size);
int boss_battle_available(void);
void boss_battle_set_available(int available);

void GUEST_HOOK_ABI boss_battle_scene_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_inn_built_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_inn_result_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_inn_help_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_inn_shift_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_inn_mirror_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_arrange_story_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_story_clear_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_dragon_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_title_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_inn_flushed_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_save_copy_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_party_wiped_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_boss_cleared_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_clear_screen_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_results_screen_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_town_return_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_options_title_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_options_built_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_options_update_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_options_result_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_list_up_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_list_down_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_results_level_done_hook(GuestRegs *regs);

/* The game functions the scene and screen hooks call; set by the install, the tests set their own. */
typedef struct {
    void(GUEST_HOOK_ABI *set_step)(uint64_t scene, int level, int value);
    void(GUEST_HOOK_ABI *set_text)(uint64_t text, const char *value);
    void(GUEST_HOOK_ABI *set_message)(uint64_t text, int message);
    void(GUEST_HOOK_ABI *row_frame)(uint64_t layout, int frame, float start);
} BossBattleScreenFunctions;
void boss_battle_set_screen_functions(const BossBattleScreenFunctions *functions);

typedef struct {
    void(GUEST_HOOK_ABI *set_level)(uint64_t record, uint32_t level);
    int(GUEST_HOOK_ABI *level_pattern)(uint64_t record);
    void(GUEST_HOOK_ABI *write_stats)(uint64_t stats, int pattern, int unused, int level, int unused2);
} BossBattleLevelFunctions;
void boss_battle_set_level_functions(const BossBattleLevelFunctions *functions);

#ifdef __cplusplus
}
#endif

#endif
