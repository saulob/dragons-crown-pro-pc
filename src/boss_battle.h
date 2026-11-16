/* Boss Battle: with enable_boss_battle on, the Dragon's Haven Inn menu gains a Boss Battle row after Report adventurers.
 * The row opens the Boss Battle screen on the Inn's native Options page; a boss chosen there sets the campaign aside
 * (saves protected, campaign memory kept), sends the Inn's party to the game server as leaving the Inn does, then
 * requests the boss's arena from the Inn's next menu frame. Every way back ends in the Inn, where the campaign is put
 * back with a Win's character progression, saved by the game, and the screen opens again from the Inn's menu frame. On both ways the scene's own fader stays black while the
 * Inn menu runs, so only Return to tavern and Back show it. */
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

/* The campaign character, read from the game when the boss list opens: `class_index` the game's (0 Fighter .. 5 Wizard;
 * another is an Adventurer). `key` names it in the Boss Battle records, in English whatever the language. */
void boss_battle_set_character(const char *name, int class_index, int level);
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
    int gauge_window;     /* the boss HP gauge window exists */
    int bosses;           /* bosses on the boss HP gauge */
    uint64_t player_unit_address, boss_address;
    int boss_hp;
    int end_kind;         /* the stage end's kind, meaningful in the stage-end act */
    int clear_choice;     /* the stage clear screen's answer: -1 none, 1 return to town */
    int fader_busy;       /* the scene fader still runs a fade */
    uint32_t host_ms;     /* DEBUG_SAULO: the host's clock, for the transition telemetry */
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
typedef enum { BOSS_BATTLE_QUIT_KEY, BOSS_BATTLE_QUIT_MENU, BOSS_BATTLE_QUIT_PEER_LOST } BossBattleQuitSource;
int boss_battle_request_quit(BossBattleQuitSource source);
/* Bare F4 from the window thread: never for a repeat, a modifier (Alt+F4 closes the game), or while a menu or a text
 * entry owns the input. */
int boss_battle_quit_key(int repeat, int modifiers, int input_owned);
int boss_battle_quit_available(void);

/* The pause menu of a fight: Start's native menu holds only Quit Boss Battle, under the boss's name. It is Boss Battle's
 * while the arena is initializing or fought, and stays so until it closes. */
int boss_battle_pause_available(void);
int boss_battle_pause_owned(void);
/* The menu opens or is rebuilt: 1 with the boss's name (no number) when it is Boss Battle's. */
int boss_battle_pause_opened(char *title, size_t size);
/* Quit Boss Battle confirmed: the one quit path of F4; 1 when it was accepted. */
int boss_battle_pause_quit(void);
void boss_battle_pause_closed(void);

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

/* The campaign character's progression, as its record keeps it (level from 0): what the native experience award and
 * level-up change, with the name and class that tell the character. Boss Battle fights with the campaign character as
 * it is; a Win's progression is the only thing of the fight the restored campaign keeps. */
typedef struct {
    int32_t xp_pool, xp_progress, skill_points, level, class_index;
    char name[8];
} BossBattleCharacterProgress;
/* After the snapshot, the record the fight starts with. */
void boss_battle_progress_before(const BossBattleCharacterProgress *before);
/* Back at the Inn, before the campaign restore: the record as the fight left it. Kept only for a Win, for the same
 * character, and only when it changed as the native award and level-ups change a record. */
void boss_battle_progress_after(const BossBattleCharacterProgress *after);
/* After a successful campaign restore, `restored` the record put back: 1 once, with that record and the Win's
 * progression in `merged`; the campaign save is then pending. A Fight All checkpoint needs this merge and its save to
 * have gone through before the next boss starts. */
int boss_battle_progress_merge(const BossBattleCharacterProgress *restored, BossBattleCharacterProgress *merged);
/* The merged character reaches the campaign save through the game's own save: pending until the save job is started,
 * running until it ends. */
typedef enum { BOSS_BATTLE_SAVE_IDLE, BOSS_BATTLE_SAVE_PENDING, BOSS_BATTLE_SAVE_RUNNING } BossBattleSaveState;
BossBattleSaveState boss_battle_progress_save_state(void);
void boss_battle_progress_save_started(void);
void boss_battle_progress_save_finished(int ok);

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

/* The Inn menu with Boss Battle is drawn as its own seven rows, inside the vertical envelope of the vanilla six: the
 * first row centred on vanilla Select player, the last on vanilla Leave tavern. */
typedef struct {
    float x, width;
    float row_height;   /* a row's rectangle and highlight, below the pitch so neighbours never overlap */
    float pitch;
    float art_scale;    /* the native cursor art's vertical scale: row_height over the vanilla row height */
    float center[BOSS_BATTLE_INN_ROWS];
} BossBattleInnLayout;
/* 0 when the vanilla rows give no room for seven. */
int boss_battle_inn_layout(float x, float width, float vanilla_height, float first_center, float last_center,
                           BossBattleInnLayout *layout);
float boss_battle_inn_row_top(const BossBattleInnLayout *layout, int row);

/* The Boss Battle screen is the native Options page the Inn opens for its Options row, given other rows: row 0 (the
 * page's title bar) Fight All, rows 1-6 six of the bosses in the selectable value column with the highlighted entry's
 * statistics in the static label column, row 7 the bottom action (Return to tavern). The six boss rows scroll through
 * the twenty bosses. */
enum {
    BOSS_BATTLE_SCREEN_ROWS = 8,
    BOSS_BATTLE_SCREEN_FIGHT_ALL_ROW = 0,
    BOSS_BATTLE_SCREEN_BOSS_ROWS = 6,
    BOSS_BATTLE_SCREEN_RETURN_ROW = 7,
    BOSS_BATTLE_INN_OPTIONS_ROW = 3, /* the native Inn row that opens the Options page */
    BOSS_BATTLE_RETURN_TO_TAVERN_RECORD = 1120
};
/* The screen's entries, top to bottom. */
typedef enum {
    BOSS_BATTLE_ENTRY_NONE = -1,
    BOSS_BATTLE_ENTRY_FIGHT_ALL,
    BOSS_BATTLE_ENTRY_BOSS_01,
    BOSS_BATTLE_ENTRY_BOSS_20 = BOSS_BATTLE_ENTRY_BOSS_01 + BOSS_BATTLE_ENCOUNTERS - 1,
    BOSS_BATTLE_ENTRY_RETURN_TO_TAVERN,
    BOSS_BATTLE_ENTRIES
} BossBattleEntry;
/* The boss of an entry, -1 for Fight All and Return to tavern. */
int boss_battle_entry_boss(BossBattleEntry entry);
typedef enum {
    BOSS_BATTLE_SCREEN_NONE,
    BOSS_BATTLE_SCREEN_START,
    BOSS_BATTLE_SCREEN_FIGHT_ALL,
    BOSS_BATTLE_SCREEN_CLOSE
} BossBattleScreenChoice;
/* Fight All: the twenty bosses in menu order, one session each; after a Win the hidden Inn is the checkpoint where the
 * campaign is put back and the character saved before the next boss starts. The screen reopens on Fight All when it ends. */
typedef enum {
    BOSS_BATTLE_FIGHT_ALL_NONE,
    BOSS_BATTLE_FIGHT_ALL_CLEARED,
    BOSS_BATTLE_FIGHT_ALL_DIED,
    BOSS_BATTLE_FIGHT_ALL_QUIT_RUN,
    BOSS_BATTLE_FIGHT_ALL_CANCELLED, /* quit before the first boss was playable: not a run */
    BOSS_BATTLE_FIGHT_ALL_ABORTED    /* a technical failure: no statistics */
} BossBattleFightAllResult;

typedef struct {
    int active;            /* confirmed and not ended */
    int run_started;       /* the first boss reached Playable */
    int checkpoint;        /* between bosses, at the hidden Inn */
    int boss;              /* the current boss; the next one at a checkpoint that passed */
    int cleared;           /* bosses defeated */
    uint32_t combat_ms;    /* Playable to Win, summed */
    unsigned run;          /* the run's number in the statistics */
    BossBattleFightAllResult result; /* kept until the next run is confirmed */
} BossBattleFightAllState;
BossBattleFightAllState boss_battle_fight_all_state(void);
int boss_battle_fight_all_active(void);
int boss_battle_fight_all_checkpoint(void);

/* Fight All confirmed on the screen: 1 when a run was queued; the screen then closes as for a chosen boss. */
int boss_battle_fight_all_request(void);

/* The way from one won boss to the next, under the held black. */
typedef enum {
    BOSS_BATTLE_TRANSIT_NONE,
    BOSS_BATTLE_TRANSIT_CHECKPOINT,    /* the campaign is put back, the character merged and saved */
    BOSS_BATTLE_TRANSIT_SCENE_WAIT,    /* saved; the Inn scene cannot start a travel yet */
    BOSS_BATTLE_TRANSIT_REQUESTED,     /* the next boss's travel was requested; the scene has not left yet */
    BOSS_BATTLE_TRANSIT_STAGE_LOADING, /* the scene's travel act runs */
    BOSS_BATTLE_TRANSIT_ARENA_READY,   /* the arena is set up, not revealed */
    BOSS_BATTLE_TRANSIT_DONE           /* revealed */
} BossBattleFightAllTransit;
BossBattleFightAllTransit boss_battle_fight_all_transit(void);

/* The Inn can start a travel as its own menu does: the scene plays (act 2) in the Inn's stage, no stage is pending and
 * the scene fader has finished its fade. */
int boss_battle_fight_all_scene_ready(const BossBattleScene *scene, int fader_busy);

typedef enum {
    BOSS_BATTLE_FIGHT_ALL_STEP_IDLE,  /* no checkpoint: the Inn runs as the game has it */
    BOSS_BATTLE_FIGHT_ALL_STEP_WAIT,  /* the campaign is being put back or saved, or the scene is not ready: hold */
    BOSS_BATTLE_FIGHT_ALL_STEP_START, /* every requirement is met: start `index` now */
    BOSS_BATTLE_FIGHT_ALL_STEP_ENDED  /* the run ended here (cleared or aborted) */
} BossBattleFightAllStep;
/* Game thread, once per Inn menu frame of a checkpoint, with the Inn's scene and whether its fader still fades. */
BossBattleFightAllStep boss_battle_fight_all_poll(const BossBattleScene *scene, int fader_busy, int *index);
/* A technical failure: ends the run without touching any statistic, and reopens the screen after a checkpoint. */
void boss_battle_fight_all_abort(const char *why);
/* The game's menu sounds (its SE table indexes): the list's cursor, confirmation and back. */
enum { BOSS_BATTLE_SE_SELECT = 42, BOSS_BATTLE_SE_DECIDE = 43, BOSS_BATTLE_SE_CANCEL = 44 };

/* The Inn asks for its Options page as the Boss Battle screen; the page reports itself built, then closed. A request
 * the Inn never turns into a page is dropped after a few menu frames (`waiting` 0 then). */
void boss_battle_screen_request(void);
int boss_battle_screen_requested(void);
int boss_battle_screen_waiting(void);
/* The page is the screen: the cursor row it opens on, the last boss chosen in view. */
int boss_battle_screen_built(void);
int boss_battle_screen_shown(void);
/* The page closed natively with a boss chosen: 1 once, for the cut to black before the Inn menu shows again. */
int boss_battle_screen_take_close_cut(void);
/* The screen closed; 1 with the boss chosen in it. */
int boss_battle_screen_closed(int *index);
void boss_battle_screen_reset(void);
int boss_battle_screen_offset(void);
BossBattleEntry boss_battle_screen_entry(int row);
/* The boss of a row, -1 for Fight All and the bottom action. */
int boss_battle_screen_boss(int row);
/* The native cursor about to move from `row`: 1 when the boss rows scroll instead and the cursor stays. */
int boss_battle_screen_move(int row, int down);
/* A list result of the page (row, -1 nothing): a boss row starts it, Fight All asks for its run (the caller then calls
 * boss_battle_fight_all_request), the bottom action closes. */
BossBattleScreenChoice boss_battle_screen_choose(int result, int *index);
/* The texts of the page for the cursor row: a row's entry (the selectable value column), the highlighted entry's
 * statistic on that row (the static label column), the help line. */
void boss_battle_screen_label(int row, char *out, size_t size);
void boss_battle_screen_statistic(int row, int cursor_row, char *out, size_t size);
void boss_battle_screen_help(int cursor_row, char *out, size_t size);
/* The boss the screen opens on: the last one chosen. */
int boss_battle_menu_cursor(void);
/* The page's highlighted row is kept as the boss (or Fight All) the next page opens on. */
void boss_battle_screen_remember(int row);
int boss_battle_cursor_fight_all(void);
/* The page was replaced by the game: the next one opens on the remembered entry through the Inn's reopen. */
void boss_battle_screen_return(void);
/* The page asked for once, from the Inn's menu frame: after a fight's return put the campaign back (never after a failed
 * restore), after the Network Menu's return, or on the new Inn a visitor's join rebuilt. */
typedef enum {
    BOSS_BATTLE_REOPEN_CAMPAIGN_RESTORE,
    BOSS_BATTLE_REOPEN_NETWORK_MENU_RETURN,
    BOSS_BATTLE_REOPEN_VISITOR_INN
} BossBattleReopenReason;
void boss_battle_arm_menu_reopen(BossBattleReopenReason reason);
int boss_battle_menu_reopen_pending(void);
int boss_battle_take_menu_reopen(void);
const char *boss_battle_reopen_reason(void);
/* The session number the reopen belongs to, for the log. */
unsigned boss_battle_reopen_session(void);
/* A boss confirmed on the screen: black from the confirmation until the arena is played, then one fade-in. */
typedef enum {
    BOSS_BATTLE_START_NONE,
    BOSS_BATTLE_START_FADING, /* fading to black while the page closes */
    BOSS_BATTLE_START_HELD,      /* fully black: the Inn and the travel run under it */
    BOSS_BATTLE_START_REVEALING, /* the arena's native arrival fade-in runs */
    BOSS_BATTLE_START_DONE       /* the arena was revealed */
} BossBattleStartTransition;
BossBattleStartTransition boss_battle_start_transition(void);
void boss_battle_start_transition_set(BossBattleStartTransition next);
int boss_battle_start_transition_holds(void);
/* At the scene's arrival fade-in: 1 when it is the held arena's, which then reveals it. */
int boss_battle_start_reveal_begins(void);
/* Once, on the first frame that fade-in has finished: 1 when the start transition is complete. */
int boss_battle_start_reveal_finished(int fader_busy);
/* MM:SS.mmm */
void boss_battle_format_time(uint32_t ms, char *out, size_t size);
/* MM:SS.mmm, HH:MM:SS.mmm from one hour: a Fight All run's time. */
void boss_battle_format_run_time(uint32_t ms, char *out, size_t size);

typedef enum {
    BOSS_BATTLE_INN_IDLE, /* the menu runs as the game wants */
    BOSS_BATTLE_INN_HOLD  /* this frame the menu must not act */
} BossBattleInnAction;

/* Game thread, once per frame of the Inn menu: a session that never left this Inn is ended here; one on its way out
 * holds the menu. */
BossBattleInnAction boss_battle_inn_frame(void);
void boss_battle_request_sent(int index, const BossBattleScene *from, int party_sent);
void boss_battle_request_failed(int index, const char *why);

/* An online guest's session: the host's game server takes the party to the arena and back, so a follower never
 * requests a scene. follow queues the host's boss (Fight All from its first boss, or the next one once this guest's
 * checkpoint saved); follow_started marks the session that begin_queued opened for it. */
int boss_battle_follow(int index, int fight_all);
void boss_battle_follow_started(int index);
int boss_battle_follower(void);
int boss_battle_fight_all_follow_ready(int index);
/* The host is gone: a follower's fight ends as a Quit from its own game, a guest's checkpoint ends its run. */
void boss_battle_follow_released(const char *why);

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
    uint64_t inn_draw;     /* the Inn's draw: its call of the menu list's draw */
    uint64_t inn_destroyed; /* the Inn destructor */
    uint64_t inn_layer, inn_layer_tail; /* the Inn's calls that show or hide a background layer by position */
    const char *inn_background_failure; /* why the Inn background keeps six rows; NULL when it can have seven */
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
    int town_return_scene_rbx; /* the scene is in rbx there, not r14 */
    uint64_t set_step;         /* the scene's act and step setter */
    uint64_t set_text;         /* a text's plain string setter */
    uint64_t set_message;      /* a text's message setter (SystemMsg number) */
    uint64_t options_title;    /* the Inn's Options page: its title widget's message */
    uint64_t options_built;    /* after the page's rows are built */
    uint64_t options_update;   /* the page's per-frame row logic */
    uint64_t options_result;   /* after the page's list read its input (eax the result) */
    uint64_t options_leave;    /* the Inn's Options mode, once its page closed: its first-frame check */
    uint64_t arrival_fade_in;  /* the scene's arrival step: its fade-in of the scene fader */
    uint64_t inn_help_line;    /* the Inn menu frame's call of its help line (edx 1 shows it, 0 closes it) */
    uint64_t cue_player;       /* DEBUG_SAULO: the cue player the menu sounds reach */
    uint64_t list_play_se;     /* a list's menu sound by SE table index */
    uint64_t get_fader, fade;  /* the scene fader of the game manager's fader owner, and its fade */
    uint64_t list_up, list_down; /* the list's cursor moves, every list's */
    uint64_t list_update;        /* the list's per-frame update, every list's */
    uint64_t options_body_vtable;
    uint64_t row_frame;        /* a layout's frame setter: a row's background */
    uint64_t results_level_done;               /* the results screen asking for its General results */
    uint64_t set_level, level_pattern, write_stats; /* a character record's level, stat pattern and stats */
    uint64_t save_commit, save_request;        /* the play data into the save's current slot; the save manager's save */
    uint64_t pause_start_gate, pause_level_gate; /* the stage scene's Start checks: a progressed character, a level */
    uint64_t pause_mode;                       /* the scene's read of its kind as it opens the pause menu */
    uint64_t pause_row;                        /* the pause menu's row builder, or its mode switch where rows are inline */
    int pause_rows_inline;
    uint64_t pause_plain_rows, pause_quit_row; /* inline: the plain mode's first row and its Quit row */
    uint64_t pause_row_vtable;                 /* inline: the row widget vtable the first row loads */
    uint64_t pause_setup, pause_setup_calls[3]; /* the pause menu's setup and its three calls */
    uint64_t pause_confirm_text;               /* the quit confirmation's message */
    uint64_t pause_result, pause_result_fn;    /* the scene's read of the pause menu's result */
    uint64_t pause_floor_reward;               /* the pause window's call that shows the labyrinth floor's reward */
    uint64_t network_rows;                     /* the list's widget add, which the Network Menu's rows pass through */
    uint64_t network_choice[2], network_row_at; /* the menu's two reads of the chosen row, and the row lookup they call */
    uint64_t network_back_query;               /* the list update's call of its Back query */
    uint64_t room_info_calls[4];               /* the room info builder's character, index, controllable and match calls */
    uint64_t room_info_character, room_info_controllable; /* the functions of the first and third */
    uint64_t join_state_call;                  /* the join reply's call right after it stores its join state */
    uint64_t join_unit_call, join_unit_function; /* the join reply's read of one unit of the scene, and its function */
    uint64_t party_slot_calls[2], party_slot_function; /* the online party pass's local slot lookups, and the lookup */
    uint64_t add_player;                       /* the scene's "add player" message handler, which seats a player */
    uint64_t slot_request;                     /* the host's read of the slot a CREATEPLAYER_REQUEST asks for */
    uint64_t location_text_call;               /* the friend card's call that sets the location line's message */
    uint64_t text_changed;                     /* marks a text object for layout again */
    uint32_t pause_result_field;               /* the window's result, as the result function reads it */
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
void GUEST_HOOK_ABI boss_battle_inn_draw_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_inn_destroyed_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_inn_layer_hook(GuestRegs *regs);
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
void GUEST_HOOK_ABI boss_battle_town_return_rbx_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_pause_rows_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_pause_floor_reward_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_network_row_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_network_choice_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_network_back_query_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_info_character_hook(GuestRegs *regs);
void boss_battle_set_room_info_character(uint64_t(GUEST_HOOK_ABI *lookup)(uint64_t slot));
void GUEST_HOOK_ABI boss_battle_info_index_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_info_controllable_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_info_match_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_join_state_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_join_unit_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_party_slot_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_add_player_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_slot_request_hook(GuestRegs *regs);
void boss_battle_set_party_slot_lookup(uint64_t(GUEST_HOOK_ABI *lookup)(uint64_t party, uint32_t player));
/* The end of a Boss Battle visit that had a room, at image offsets: the game's full party reset (the one its scene reset
 * ends with), its own per-seat character teardown (what its add-player handler does to a seat it replaces), and the
 * call after its own room leave, found in the room wish. */
typedef struct {
    uint64_t party_reset, seat_character, remove_character, detach_seat, after_leave;
} BossBattleHandoffSites;
int boss_battle_locate_handoff(const uint8_t *image, uint64_t size, uint64_t room_wish_function, BossBattleHandoffSites *sites);
typedef struct {
    void(GUEST_HOOK_ABI *party_reset)(uint64_t party);
    uint64_t(GUEST_HOOK_ABI *seat_character)(uint64_t slot);
    void(GUEST_HOOK_ABI *remove_character)(uint64_t character);
    void(GUEST_HOOK_ABI *detach_seat)(uint64_t slot, int keep);
} BossBattleSeatTeardown;
void boss_battle_set_seat_teardown(const BossBattleSeatTeardown *teardown);
void GUEST_HOOK_ABI boss_battle_location_text_hook(GuestRegs *regs);

/* The game's read of one unit of the scene that the join reply's unit check makes (the unit list and an index). */
typedef struct {
    uint64_t(GUEST_HOOK_ABI *unit)(uint64_t units, uint32_t index);
} BossBattleJoinProbes;
void boss_battle_set_join_probes(const BossBattleJoinProbes *probes);
void boss_battle_set_network_row_at(uint64_t(GUEST_HOOK_ABI *row_at)(uint64_t list, int32_t index));
void GUEST_HOOK_ABI boss_battle_options_title_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_options_built_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_options_update_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_options_result_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_options_leave_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_arrival_fade_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_fade_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_inn_help_line_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_cue_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_pause_start_gate_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_pause_level_gate_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_pause_mode_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_pause_row_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_pause_setup_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_pause_confirm_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_pause_result_hook(GuestRegs *regs);

/* The scene fader (the game manager's +0xd8 owns it): found, then faded to an ARGB colour over frames on a layer. */
typedef struct {
    uint64_t(GUEST_HOOK_ABI *get_fader)(uint64_t owner);
    void(GUEST_HOOK_ABI *fade)(uint64_t fader, uint32_t colour, int frames, int layer);
} BossBattleFadeFunctions;
void boss_battle_set_fade_functions(const BossBattleFadeFunctions *functions);
void GUEST_HOOK_ABI boss_battle_list_up_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_list_down_hook(GuestRegs *regs);
void GUEST_HOOK_ABI boss_battle_list_update_hook(GuestRegs *regs);
/* The page the Network Menu was opened over is being replaced and the new one has not run its own frame yet. */
int boss_battle_network_menu_returning(void);
/* The scene fader holds black for a guest that joined a host's room, until its Boss Battle page is active. */
int boss_battle_guest_join_cover(void);
void GUEST_HOOK_ABI boss_battle_results_level_done_hook(GuestRegs *regs);

/* The scene object's class and its scene request function, which the hooks use to tell a scene and to request one; set by
 * the install, the tests set their own. */
typedef void(GUEST_HOOK_ABI *BossBattleSceneRequestFn)(uint64_t server, const char *round, int stage, int entry,
                                                       const char *stage_name, const uint8_t *route_flag, int force,
                                                       int code, int slot, int option, const void *places);
void boss_battle_set_scene(uint64_t vtable, BossBattleSceneRequestFn request);
/* The game's lead character selector (the facade's game manager is all it reads); set by the install. */
typedef const uint8_t *(GUEST_HOOK_ABI *BossBattleLeadSelectorFn)(uint64_t facade);
void boss_battle_set_lead_selector(BossBattleLeadSelectorFn selector);

/* The game functions the scene and screen hooks call; set by the install, the tests set their own. */
typedef struct {
    void(GUEST_HOOK_ABI *set_step)(uint64_t scene, int level, int value);
    void(GUEST_HOOK_ABI *set_text)(uint64_t text, const char *value);
    void(GUEST_HOOK_ABI *set_message)(uint64_t text, int message);
    void(GUEST_HOOK_ABI *row_frame)(uint64_t layout, int frame, float start);
    void(GUEST_HOOK_ABI *play_se)(uint64_t list, int se);
    void(GUEST_HOOK_ABI *text_changed)(uint64_t text);
} BossBattleScreenFunctions;
void boss_battle_set_screen_functions(const BossBattleScreenFunctions *functions);

typedef struct {
    void(GUEST_HOOK_ABI *set_level)(uint64_t record, uint32_t level);
    int(GUEST_HOOK_ABI *level_pattern)(uint64_t record);
    void(GUEST_HOOK_ABI *write_stats)(uint64_t stats, int pattern, int unused, int level, int unused2);
} BossBattleLevelFunctions;
void boss_battle_set_level_functions(const BossBattleLevelFunctions *functions);

/* The game's own campaign save, as the Inn's save step runs it: commit, then the save manager's request (0 while a save
 * runs). */
typedef struct {
    void(GUEST_HOOK_ABI *commit)(uint64_t save);
    uint8_t(GUEST_HOOK_ABI *request)(uint64_t saver);
} BossBattleSaveFunctions;
void boss_battle_set_save_functions(const BossBattleSaveFunctions *functions);

/* The pause menu's own functions: its setup, and the result the scene reads (-1 while it is open; 0 or the row's answer
 * once it closed). */
typedef struct {
    void(GUEST_HOOK_ABI *setup)(uint64_t window);
    int32_t(GUEST_HOOK_ABI *result)(uint64_t window, uint32_t pending);
    uint32_t result_field; /* the window's result the scene reads */
} BossBattlePauseFunctions;
void boss_battle_set_pause_functions(const BossBattlePauseFunctions *functions);

#ifdef __cplusplus
}
#endif

#endif
