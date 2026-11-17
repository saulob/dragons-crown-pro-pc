#include "boss_battle.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "boss_battle_record.h"
#include "boss_battle_text.h"

enum {
    NONE = -1,
    FRAMES_PER_SECOND = 60,
    TRAVEL_TIMEOUT_FRAMES = 600,
    RETURN_RETRY_FRAMES = 600,
    RETURN_RETRIES = 3,
    SCENE_ACT_PLAY = 2,
    HEARTBEAT_FRAMES = 300,
    HEARTBEATS = 24,
    SCENE_LINES = 96,
    LAST_CLEAR_KIND = 2 /* end kinds 0..2 are the clears a stage-ending boss's death asks for */
};

#define ARENA(id, name, file, round, first, last, stage_name, route) \
    {id, name, "round/" file ".brb", round, first, last, stage_name, route, -1, 0, NULL}

/* The order of play: route A then route B, each by the round table's normal-difficulty reward for that route, then the
 * two special bosses. Arenas are the rounds' BOSSTEST rooms; a multi-room fight spans first..last. The Demon King's room
 * is the labyrinth's RD9Boss room, entered as on Floor 9 (index 8), the first floor whose exit leads there. */
static const BossBattleEncounter kEncounters[BOSS_BATTLE_ENCOUNTERS] = {
    ARENA("harpy", "Harpy", "MysticRuins", 3, 8, 8, "1_st06", 'A'),
    ARENA("wyvern", "Wyvern", "AncientLabyrinth", 6, 9, 9, "1_st06", 'A'),
    ARENA("doom_beetle", "Doom Beetle", "MoldyDungeon", 4, 10, 10, "1_st07", 'A'),
    ARENA("golem", "Golem", "ForgottenRealms", 5, 9, 9, "1_st07", 'A'),
    ARENA("gazer", "Gazer", "LostWoods", 8, 11, 11, "1_st09", 'A'),
    ARENA("pirates", "Pirates", "GhostshipCove", 0, 10, 10, "1_st07", 'A'),
    ARENA("minotaur", "Minotaur", "UnderCitadel", 1, 11, 11, "1_st08", 'A'),
    ARENA("vampire", "Vampire", "Catacomb", 2, 9, 9, "1_st07", 'A'),
    ARENA("warlock", "Warlock", "MageTower", 7, 12, 12, "1_st06", 'A'),
    ARENA("medusa", "Medusa", "MysticRuins", 3, 15, 15, "2_st06", 'B'),
    ARENA("wraith", "Wraith", "Catacomb", 2, 11, 14, "2_st01", 'B'),
    ARENA("killer_rabbit", "Killer Rabbit", "LostWoods", 8, 16, 16, "2_st04", 'B'),
    ARENA("kraken", "Kraken", "GhostshipCove", 0, 17, 17, "2_st05", 'B'),
    ARENA("cyclops", "Cyclops", "MoldyDungeon", 4, 17, 17, "2_st06", 'B'),
    ARENA("chimera", "Chimera", "MageTower", 7, 18, 21, "2_st04", 'B'),
    ARENA("gargoyle_gate", "Gargoyle Gate", "UnderCitadel", 1, 17, 17, "2_st05", 'B'),
    ARENA("red_dragon", "Red Dragon", "AncientLabyrinth", 6, 11, 13, "2_st01", 'B'),
    ARENA("arch_demon", "Arch Demon", "ForgottenRealms", 5, 16, 16, "2_st06", 'B'),
    {"ancient_dragon", "Ancient Dragon", "round/LostZone.brb", 10, 0, 0, "st00", 0, -1, 1, NULL},
    {"demon_king", "Demon King", "round/RandomDungeon.brb", 17, 68, 68, "bg27f", 0, 8, 0, NULL},
};

const BossBattleInnEntry boss_battle_inn_entry = {"round/Town00.brb", 2, 0, "Inn"};

const BossBattleEncounter *boss_battle_encounter(int index) {
    return index >= 0 && index < BOSS_BATTLE_ENCOUNTERS ? &kEncounters[index] : NULL;
}

int boss_battle_number(int index) {
    return boss_battle_encounter(index) ? index + 1 : 0;
}

int boss_battle_label(int index, char *out, size_t size) {
    const BossBattleEncounter *encounter = boss_battle_encounter(index);
    if (!encounter || !size) return 0;
    snprintf(out, size, "%02d %s", boss_battle_number(index), boss_battle_text_boss(index));
    return 1;
}

int boss_battle_startable_count(void) {
    int count = 0;
    for (int i = 0; i < BOSS_BATTLE_ENCOUNTERS; ++i) count += !kEncounters[i].unavailable;
    return count;
}

/* Game thread only. */
static int pause_owned, follower;

static atomic_int enabled, hook_installed, state = BOSS_BATTLE_OFF, session = 0, quit_pending = 0;
static atomic_int checkpoint_open = 0; /* a Fight All checkpoint runs: a quit is taken there */
static atomic_int pending = NONE, travelling = NONE, menu_reopen = 0, start_transition = BOSS_BATTLE_START_NONE;
static atomic_uint session_number, current_session, reopen_session;
static struct {
    BossBattleGuardFn protect_saves, snapshot, restore;
    BossBattleUndoFn release_saves;
} guard;
static atomic_flag text_lock = ATOMIC_FLAG_INIT;
static char notice_text[192], character_name[64], character_class[48];
static int notice_waiting, character_level, character_class_index = -1;

/* Game thread only. */
static struct {
    BossBattleOutcomeKind outcome;
    unsigned milestones, travel_frames, arena_frames, fight_frames, return_frames, frames;
    unsigned heartbeats, scene_lines, return_requests;
    int accepted, never_left, cursor, waiting_logged;
    int stage_end_seen, clear_screen_skipped, choice_answered, player_down;
    unsigned player_logged;
    BossBattleScene last_scene;
    int scene_known;
} s;

enum { CHECKPOINT_TIMEOUT_FRAMES = 1800 };

typedef enum { FIGHT_ALL_OFF, FIGHT_ALL_SELECTED, FIGHT_ALL_BOSS, FIGHT_ALL_CHECKPOINT } FightAllPhase;

/* Game thread only. */
static struct {
    FightAllPhase phase;
    BossBattleFightAllResult result;
    BossBattleFightAllTransit transit;
    int run_started, boss, cleared, restored;
    uint32_t combat_ms;
    unsigned run, checkpoint_frames, last_session;
    int follow; /* a guest's run: the host starts each boss */
    /* the inter-boss wait, for the transition log lines */
    unsigned wait_frames, wait_logged;
    uint32_t wait_ms;
} fight_all;
static int cursor_fight_all;
static void fight_all_boss_started(int index);

static void lock_text(void) {
    while (atomic_flag_test_and_set(&text_lock)) {
    }
}

static void unlock_text(void) {
    atomic_flag_clear(&text_lock);
}

BossBattleState boss_battle_state(void) {
    return (BossBattleState)atomic_load(&state);
}

const char *boss_battle_state_text(BossBattleState value) {
    switch (value) {
    case BOSS_BATTLE_OFF: return "off";
    case BOSS_BATTLE_TRAVELLING: return "travelling";
    case BOSS_BATTLE_INITIALIZING: return "initializing arena";
    case BOSS_BATTLE_FIGHTING: return "fighting";
    case BOSS_BATTLE_RESOLVING: return "resolving";
    case BOSS_BATTLE_RETURNING: return "returning";
    }
    return "unknown";
}

BossBattleOutcomeKind boss_battle_outcome(void) {
    return s.outcome;
}

unsigned boss_battle_milestones(void) {
    return s.milestones;
}

unsigned boss_battle_session_id(void) {
    return atomic_load(&current_session);
}

#define LOG(...)                                                    \
    do {                                                            \
        printf("Boss Battle [%u]: ", atomic_load(&current_session)); \
        printf(__VA_ARGS__);                                        \
        putchar('\n');                                              \
    } while (0)

static void set_state(BossBattleState next) {
    atomic_store(&state, next);
}

BossBattleMode boss_battle_mode(void) {
    return BOSS_BATTLE_MODE_OFFLINE;
}

const char *boss_battle_mode_pending(BossBattleMode mode) {
    return mode == BOSS_BATTLE_MODE_OFFLINE ? NULL : "Online Boss Battle is not available yet.";
}

void boss_battle_set_enabled(int value) {
    if (atomic_exchange(&enabled, value != 0) != (value != 0)) printf("Boss Battle: %s\n", value ? "enabled" : "disabled");
}

int boss_battle_enabled(void) {
    return atomic_load(&enabled);
}

void boss_battle_set_save_guard(BossBattleGuardFn protect, BossBattleUndoFn release) {
    guard.protect_saves = protect;
    guard.release_saves = release;
}

void boss_battle_set_memory_guard(BossBattleGuardFn snapshot, BossBattleGuardFn restore) {
    guard.snapshot = snapshot;
    guard.restore = restore;
}

void boss_battle_set_available(int available) {
    atomic_store(&hook_installed, available != 0);
}

int boss_battle_available(void) {
    return atomic_load(&hook_installed) && guard.protect_saves && guard.release_saves && guard.snapshot && guard.restore;
}

static void post_notice(const char *text) {
    lock_text();
    snprintf(notice_text, sizeof(notice_text), "%s", text);
    notice_waiting = 1;
    unlock_text();
}

int boss_battle_session_active(void) {
    return atomic_load(&session);
}

int boss_battle_inn_row_wanted(void) {
    return boss_battle_enabled() && boss_battle_available() && !boss_battle_session_active();
}

int boss_battle_take_notice(char *out, size_t size) {
    lock_text();
    const int waiting = notice_waiting;
    if (waiting && size) snprintf(out, size, "%s", notice_text);
    notice_waiting = 0;
    unlock_text();
    return waiting;
}

static const char *const kClassKeys[BOSS_BATTLE_TEXT_CLASSES] = {"Fighter", "Amazon", "Elf", "Dwarf", "Sorceress", "Wizard"};

void boss_battle_set_character(const char *name, int class_index, int level) {
    const int known = class_index >= 0 && class_index < BOSS_BATTLE_TEXT_CLASSES;
    lock_text();
    snprintf(character_name, sizeof(character_name), "%s", name ? name : "");
    snprintf(character_class, sizeof(character_class), "%s", known ? kClassKeys[class_index] : "Adventurer");
    character_class_index = known ? class_index : -1;
    character_level = level;
    unlock_text();
}

int boss_battle_character_text(char *out, size_t size) {
    lock_text();
    const int known = character_name[0] != 0;
    const char *shown = boss_battle_text_class(character_class_index);
    if (known && size)
        snprintf(out, size, boss_battle_text(BOSS_BATTLE_TEXT_CHARACTER), character_name, shown ? shown : character_class,
                 character_level);
    unlock_text();
    return known;
}

int boss_battle_character_key(char *out, size_t size) {
    lock_text();
    const int known = character_name[0] != 0;
    if (size) snprintf(out, size, "%s|%s", known ? character_name : "", known ? character_class : "");
    unlock_text();
    return known;
}

int boss_battle_in_arena(const BossBattleEncounter *encounter, const BossBattleScene *scene) {
    return scene->round == encounter->round_index && scene->stage >= encounter->stage &&
           scene->stage <= encounter->last_stage && scene->next_stage == NONE;
}

int boss_battle_story_gated_round(int round) {
    if (!boss_battle_session_active()) return 0;
    for (int i = 0; i < BOSS_BATTLE_ENCOUNTERS; ++i)
        if (kEncounters[i].story_gated && kEncounters[i].round_index == round) return 1;
    return 0;
}

static const BossBattleEncounter *current_encounter(void) {
    return boss_battle_encounter(atomic_load(&travelling));
}

BossBattleRequestResult boss_battle_request(int index) {
    const BossBattleEncounter *encounter = boss_battle_encounter(index);
    if (!encounter || encounter->unavailable) return BOSS_BATTLE_REFUSED_ENCOUNTER;
    if (boss_battle_mode_pending(boss_battle_mode())) return BOSS_BATTLE_REFUSED_MODE;
    int expected = NONE;
    if (boss_battle_session_active() || !atomic_compare_exchange_strong(&pending, &expected, index))
        return BOSS_BATTLE_REFUSED_BUSY;
    s.cursor = index;
    cursor_fight_all = fight_all.phase != FIGHT_ALL_OFF;
    printf("Boss Battle: selected %02d %s\n", boss_battle_number(index), encounter->name);
    return BOSS_BATTLE_QUEUED;
}

static void clear_session_frames(void) {
    const int cursor = s.cursor;
    memset(&s, 0, sizeof(s));
    s.cursor = cursor;
}

static int begin_session(void) {
    if (!boss_battle_available()) {
        post_notice("Boss Battle is not available for this game build.");
        return 0;
    }
    if (!guard.protect_saves()) {
        puts("Boss Battle: not started: the campaign save could not be protected");
        post_notice("Boss Battle did not start: the campaign save could not be protected. Try again.");
        return 0;
    }
    atomic_store(&current_session, atomic_fetch_add(&session_number, 1) + 1);
    LOG("snapshot begins");
    if (!guard.snapshot()) {
        guard.release_saves();
        LOG("not started: the campaign state could not be kept");
        atomic_store(&current_session, 0);
        post_notice("Boss Battle did not start: the campaign state could not be kept.");
        return 0;
    }
    clear_session_frames();
    atomic_store(&quit_pending, 0);
    atomic_store(&session, 1);
    LOG("snapshot captured, campaign saves protected");
    return 1;
}

int boss_battle_begin_queued(int *index) {
    const int queued = atomic_exchange(&pending, NONE);
    if (queued == NONE || boss_battle_session_active() || !begin_session()) return 0;
    *index = queued;
    fight_all_boss_started(queued);
    return 1;
}

BossBattleStartTransition boss_battle_start_transition(void) {
    return (BossBattleStartTransition)atomic_load(&start_transition);
}

void boss_battle_start_transition_set(BossBattleStartTransition next) {
    atomic_store(&start_transition, next);
}

int boss_battle_start_transition_holds(void) {
    const BossBattleStartTransition now = boss_battle_start_transition();
    return now == BOSS_BATTLE_START_FADING || now == BOSS_BATTLE_START_HELD;
}

int boss_battle_start_reveal_begins(void) {
    if (boss_battle_start_transition() != BOSS_BATTLE_START_HELD || boss_battle_state() != BOSS_BATTLE_INITIALIZING) return 0;
    boss_battle_start_transition_set(BOSS_BATTLE_START_REVEALING);
    return 1;
}

int boss_battle_start_reveal_finished(int fader_busy) {
    if (boss_battle_start_transition() != BOSS_BATTLE_START_REVEALING || fader_busy) return 0;
    boss_battle_start_transition_set(BOSS_BATTLE_START_DONE);
    if (fight_all.transit != BOSS_BATTLE_TRANSIT_NONE) fight_all.transit = BOSS_BATTLE_TRANSIT_DONE;
    return 1;
}

enum { MAX_RECORD_LEVEL = 98 }; /* the record keeps the level from 0: level 99 */

typedef enum { VERDICT_UNKNOWN, VERDICT_UNCHANGED, VERDICT_KEPT, VERDICT_REFUSED } ProgressVerdict;
typedef enum { MERGE_NONE, MERGE_PENDING, MERGE_DONE, MERGE_FAILED } ProgressMerge;

/* Game thread only. */
static struct {
    BossBattleCharacterProgress before, after;
    unsigned session;
    int before_known, after_valid, restore_ok, save_ok;
    ProgressVerdict verdict;
    ProgressMerge merge;
    BossBattleSaveState save;
} progress;

#define FIGHT_ALL_LOG(...)                                         \
    do {                                                           \
        printf("Boss Battle Fight All [run %u]: ", fight_all.run); \
        printf(__VA_ARGS__);                                       \
        putchar('\n');                                             \
    } while (0)

static void fight_all_note(BossBattleFightAllEvent event, unsigned value) {
    char key[160];
    if (!boss_battle_character_key(key, sizeof(key))) return;
    boss_battle_record_fight_all_note(key, event, value);
}

static void fight_all_boss_name(int index, char *out, size_t size) {
    snprintf(out, size, "%02d/%d %s", boss_battle_number(index), BOSS_BATTLE_ENCOUNTERS, kEncounters[index].name);
}

static atomic_int reopen_reason = BOSS_BATTLE_REOPEN_CAMPAIGN_RESTORE;

static const char *reopen_reason_text(BossBattleReopenReason reason) {
    switch (reason) {
    case BOSS_BATTLE_REOPEN_NETWORK_MENU_RETURN: return "Network Menu return";
    case BOSS_BATTLE_REOPEN_VISITOR_INN: return "visitor Inn rebuild";
    default: return "campaign restore";
    }
}

static void arm_menu_reopen(unsigned session_number, BossBattleReopenReason reason) {
    atomic_store(&reopen_session, session_number);
    atomic_store(&reopen_reason, reason);
    atomic_store(&menu_reopen, 1);
    LOG("automatic Boss Battle reopen armed (%s)", reopen_reason_text(reason));
}

void boss_battle_arm_menu_reopen(BossBattleReopenReason reason) {
    arm_menu_reopen(atomic_load(&current_session), reason);
}

const char *boss_battle_reopen_reason(void) {
    return reopen_reason_text((BossBattleReopenReason)atomic_load(&reopen_reason));
}

static void fight_all_stop(BossBattleFightAllResult result, const char *why, int reopen) {
    if (fight_all.phase == FIGHT_ALL_OFF) return;
    const int mid_run = fight_all.phase == FIGHT_ALL_CHECKPOINT && fight_all.restored;
    if (result == BOSS_BATTLE_FIGHT_ALL_ABORTED)
        FIGHT_ALL_LOG("ABORTED (technical, no statistics changed): %s; progress %02d/%d", why, fight_all.cleared,
                      BOSS_BATTLE_ENCOUNTERS);
    fight_all.result = result;
    fight_all.phase = FIGHT_ALL_OFF;
    fight_all.transit = BOSS_BATTLE_TRANSIT_NONE;
    fight_all.restored = 0;
    atomic_store(&checkpoint_open, 0);
    boss_battle_start_transition_set(BOSS_BATTLE_START_NONE);
    if (result != BOSS_BATTLE_FIGHT_ALL_ABORTED || !mid_run) return;
    post_notice("Fight All stopped: a safety check failed. The character progression saved so far is kept.");
    if (reopen) arm_menu_reopen(fight_all.last_session, BOSS_BATTLE_REOPEN_CAMPAIGN_RESTORE);
}

void boss_battle_fight_all_abort(const char *why) {
    fight_all_stop(BOSS_BATTLE_FIGHT_ALL_ABORTED, why, 1);
}

static void fight_all_begin_run(void) {
    char key[160];
    memset(&fight_all, 0, sizeof(fight_all));
    fight_all.phase = FIGHT_ALL_SELECTED;
    fight_all.run = boss_battle_character_key(key, sizeof(key)) ? boss_battle_record_fight_all_stats(key).runs + 1 : 1;
    cursor_fight_all = 1;
}

BossBattleFightAllState boss_battle_fight_all_state(void) {
    return (BossBattleFightAllState){fight_all.phase != FIGHT_ALL_OFF,
                                     fight_all.run_started,
                                     fight_all.phase == FIGHT_ALL_CHECKPOINT,
                                     fight_all.boss,
                                     fight_all.cleared,
                                     fight_all.combat_ms,
                                     fight_all.run,
                                     fight_all.result};
}

int boss_battle_fight_all_active(void) {
    return fight_all.phase != FIGHT_ALL_OFF;
}

int boss_battle_fight_all_checkpoint(void) {
    return fight_all.phase == FIGHT_ALL_CHECKPOINT;
}

/* A boss session began for the run: its first boss, or the one a passed checkpoint chose. */
static void fight_all_boss_started(int index) {
    if (fight_all.phase != FIGHT_ALL_SELECTED && fight_all.phase != FIGHT_ALL_CHECKPOINT) return;
    char name[64];
    fight_all.phase = FIGHT_ALL_BOSS;
    fight_all.boss = index;
    fight_all.checkpoint_frames = 0;
    atomic_store(&checkpoint_open, 0);
    if (index == 0) FIGHT_ALL_LOG("started");
    fight_all_boss_name(index, name, sizeof(name));
    FIGHT_ALL_LOG("boss %s started", name);
}

/* The first boss is playable: the run exists. */
static void fight_all_playable(void) {
    if (fight_all.phase != FIGHT_ALL_BOSS || fight_all.boss != 0 || fight_all.run_started) return;
    fight_all.run_started = 1;
    fight_all_note(BOSS_BATTLE_FIGHT_ALL_RUN, 0);
}

static void fight_all_won(uint32_t ms) {
    if (fight_all.phase != FIGHT_ALL_BOSS || fight_all.result != BOSS_BATTLE_FIGHT_ALL_NONE) return;
    char name[64];
    ++fight_all.cleared;
    fight_all.combat_ms += ms;
    fight_all_boss_name(fight_all.boss, name, sizeof(name));
    FIGHT_ALL_LOG("boss %s cleared in %u ms", name, ms);
}

/* Death or Quit of the boss in play: the run's own terminal outcome, once. */
static void fight_all_lost(BossBattleFightAllResult result) {
    if (fight_all.phase != FIGHT_ALL_BOSS || fight_all.result != BOSS_BATTLE_FIGHT_ALL_NONE) return;
    char name[64];
    fight_all_boss_name(fight_all.boss, name, sizeof(name));
    if (!fight_all.run_started) {
        fight_all.result = BOSS_BATTLE_FIGHT_ALL_CANCELLED;
        FIGHT_ALL_LOG("cancelled before the first boss was playable: no run recorded");
        return;
    }
    const int died = result == BOSS_BATTLE_FIGHT_ALL_DIED;
    fight_all_note(died ? BOSS_BATTLE_FIGHT_ALL_DEATH : BOSS_BATTLE_FIGHT_ALL_QUIT, (unsigned)fight_all.cleared);
    fight_all.result = result;
    FIGHT_ALL_LOG("%s on boss %s, progress %02d/%d", died ? "Death" : "Quit", name, fight_all.cleared,
                  BOSS_BATTLE_ENCOUNTERS);
}

/* A boss session ended and the campaign was put back (or not): 1 when the screen should reopen. A won boss that came back
 * to the Inn becomes a checkpoint; anything else ends the run. */
static int fight_all_session_ended(int restored, int came_back) {
    if (fight_all.phase == FIGHT_ALL_OFF) return restored && came_back;
    fight_all.last_session = atomic_load(&current_session);
    if (fight_all.phase == FIGHT_ALL_BOSS && fight_all.result == BOSS_BATTLE_FIGHT_ALL_NONE &&
        s.outcome == BOSS_BATTLE_OUTCOME_WIN && restored && came_back) {
        fight_all.phase = FIGHT_ALL_CHECKPOINT;
        fight_all.transit = BOSS_BATTLE_TRANSIT_CHECKPOINT;
        fight_all.restored = 1;
        fight_all.checkpoint_frames = 0;
        fight_all.wait_logged = 0;
        atomic_store(&quit_pending, 0);
        atomic_store(&checkpoint_open, 1);
        boss_battle_start_transition_set(BOSS_BATTLE_START_HELD);
        return 0;
    }
    if (fight_all.result == BOSS_BATTLE_FIGHT_ALL_NONE)
        fight_all_stop(BOSS_BATTLE_FIGHT_ALL_ABORTED,
                       restored ? "the fight ended without an outcome" : "the campaign could not be put back", 0);
    else
        fight_all_stop(fight_all.result, "", 0);
    return restored && came_back;
}

static void end_session(int reopen_menu) {
    if (!boss_battle_session_active()) return;
    follower = 0;
    if (!(fight_all.phase == FIGHT_ALL_BOSS && s.outcome == BOSS_BATTLE_OUTCOME_WIN && reopen_menu))
        boss_battle_start_transition_set(BOSS_BATTLE_START_NONE);
    LOG("campaign restore begins");
    pause_owned = 0;
    const int restored = guard.restore();
    progress.restore_ok = restored;
    guard.release_saves();
    atomic_store(&session, 0);
    atomic_store(&quit_pending, 0);
    atomic_store(&travelling, NONE);
    set_state(BOSS_BATTLE_OFF);
    const int reopen = fight_all_session_ended(restored, reopen_menu);
    if (restored) {
        LOG("campaign world restored, campaign saves released");
        if (reopen) arm_menu_reopen(atomic_load(&current_session), BOSS_BATTLE_REOPEN_CAMPAIGN_RESTORE);
    } else {
        LOG("campaign NOT restored; saving keeps the campaign from before the fight until the game exits");
        atomic_store(&menu_reopen, 0);
        post_notice("Boss Battle could not put the campaign back. Saving keeps your progress from before the fight; "
                    "restart the game before playing on.");
    }
    atomic_store(&current_session, 0);
}

static int record(BossBattleOutcome outcome, uint32_t ms) {
    char key[160];
    const BossBattleEncounter *encounter = current_encounter();
    if (!encounter || !boss_battle_character_key(key, sizeof(key))) return 0;
    return boss_battle_record_note(key, encounter->id, outcome, ms);
}

static void log_stats(void) {
    char key[160];
    const BossBattleEncounter *encounter = current_encounter();
    if (!encounter || !boss_battle_character_key(key, sizeof(key))) return;
    const BossBattleStats stats = boss_battle_record_stats(key, encounter->id);
    LOG("%s: attempts %u, wins %u, deaths %u, quits %u, best %u ms", encounter->name, stats.attempts, stats.wins,
        stats.deaths, stats.quits, stats.best_ms);
}

static void resolve(BossBattleOutcomeKind outcome, const char *why) {
    s.outcome = outcome;
    set_state(BOSS_BATTLE_RESOLVING);
    if (fight_all.transit != BOSS_BATTLE_TRANSIT_DONE) fight_all.transit = BOSS_BATTLE_TRANSIT_NONE;
    LOG("resolving: %s", why);
}

static void cancel(const char *why) {
    resolve(BOSS_BATTLE_OUTCOME_CANCELLED, why);
    LOG("cancelled before Playable: no Attempt and no outcome recorded");
}

static void finish(BossBattleOutcome outcome, BossBattleOutcomeKind kind, const char *what, uint32_t ms) {
    const int written = record(outcome, ms);
    LOG("%s recorded%s", what, written ? "" : " (not written: no character or no open attempt)");
    log_stats();
    if (kind == BOSS_BATTLE_OUTCOME_WIN) fight_all_won(ms);
    else if (kind == BOSS_BATTLE_OUTCOME_DEATH) fight_all_lost(BOSS_BATTLE_FIGHT_ALL_DIED);
    else if (kind == BOSS_BATTLE_OUTCOME_QUIT) fight_all_lost(BOSS_BATTLE_FIGHT_ALL_QUIT_RUN);
    resolve(kind, what);
}

void boss_battle_request_sent(int index, const BossBattleScene *from, int party_sent) {
    const BossBattleEncounter *encounter = boss_battle_encounter(index);
    atomic_store(&travelling, index);
    set_state(BOSS_BATTLE_TRAVELLING);
    LOG("%s sent to the game server", party_sent ? "Inn party" : "NO Inn party");
    LOG("travel request %s round %d stage %d (%s) entry -1 (the stage's default), from round %d stage %d entry %d",
        encounter->round, encounter->round_index, encounter->stage, encounter->stage_name, from->round, from->stage,
        from->entry);
}

void boss_battle_request_failed(int index, const char *why) {
    printf("Boss Battle: %s not started: %s\n", boss_battle_encounter(index)->name, why);
    post_notice("Boss Battle: the game did not start the battle.");
    end_session(1);
}

static int playable_now(const BossBattleProbe *probe) {
    const unsigned needed = BOSS_BATTLE_ARENA_LOADED | BOSS_BATTLE_PLAYER_SPAWNED | BOSS_BATTLE_ENCOUNTER_READY;
    return (s.milestones & needed) == needed && probe->scene.act == SCENE_ACT_PLAY && probe->player_unit &&
           !probe->player_dead && probe->bosses > 0 && boss_battle_in_arena(current_encounter(), &probe->scene);
}

static const char *missing_milestone(const BossBattleProbe *probe) {
    if (!probe->player_record) return "player record";
    if (!probe->player_unit) return "player unit";
    if (!(s.milestones & BOSS_BATTLE_PARTY_READY)) return "party ready";
    if (!(s.milestones & BOSS_BATTLE_ENCOUNTER_READY)) return "encounter";
    return "playable";
}

static int waiting_mark(unsigned frames) {
    return frames == 1 || frames == 2 * FRAMES_PER_SECOND || frames == 5 * FRAMES_PER_SECOND ||
           frames == 10 * FRAMES_PER_SECOND || frames == 20 * FRAMES_PER_SECOND || frames == 30 * FRAMES_PER_SECOND;
}

static void reach(unsigned milestone, const char *line) {
    if (s.milestones & milestone) return;
    s.milestones |= milestone;
    LOG("%s", line);
}

static void follow_start(const BossBattleProbe *probe) {
    char line[160];
    if (probe->player_record && !(s.player_logged & 1)) {
        s.player_logged |= 1;
        LOG("player record available (party slot 0)");
    }
    if (probe->player_unit && !(s.milestones & BOSS_BATTLE_PLAYER_SPAWNED) && probe->player_record) {
        snprintf(line, sizeof(line), "player unit created ptr=%#llx%s", (unsigned long long)probe->player_unit_address,
                 probe->player_dead ? " (dead state)" : "");
        reach(BOSS_BATTLE_PLAYER_SPAWNED, line);
    }
    if (probe->party_slots > 0 && probe->party_units == probe->party_slots) {
        snprintf(line, sizeof(line), "party ready: %d slot(s) with units", probe->party_slots);
        reach(BOSS_BATTLE_PARTY_READY, line);
    }
    if (probe->bosses > 0) {
        snprintf(line, sizeof(line), "encounter ready: boss on the HP gauge ptr=%#llx hp=%d",
                 (unsigned long long)probe->boss_address, probe->boss_hp);
        reach(BOSS_BATTLE_ENCOUNTER_READY, line);
    }
    if (playable_now(probe)) {
        reach(BOSS_BATTLE_PLAYABLE, "playable");
        set_state(BOSS_BATTLE_FIGHTING);
        s.fight_frames = 0;
        const int written = record(BOSS_BATTLE_ATTEMPT, 0);
        LOG("Attempt recorded%s", written ? "" : " (not written: no character)");
        log_stats();
        fight_all_playable();
        return;
    }
    ++s.arena_frames;
    if (waiting_mark(s.arena_frames))
        LOG("WAITING %s (%u s after the arena loaded): boss gauge window %s, bosses on it %d, scene act %d",
            missing_milestone(probe), s.arena_frames / FRAMES_PER_SECOND, probe->gauge_window ? "present" : "absent",
            probe->bosses, probe->scene.act);
}

static void follow_travel(const BossBattleProbe *probe) {
    const BossBattleScene *scene = &probe->scene;
    const BossBattleEncounter *encounter = current_encounter();
    ++s.travel_frames;
    if (!s.accepted) {
        if (scene->round == encounter->round_index) {
            s.accepted = 1;
            LOG("round load accepted (round %d, next stage %d)", scene->round, scene->next_stage);
        } else if (s.travel_frames > TRAVEL_TIMEOUT_FRAMES) {
            s.never_left = 1;
            post_notice("Boss Battle: the game did not start the battle.");
            cancel("the game did not take the transition within 10 seconds");
        } else if (waiting_mark(s.travel_frames)) {
            LOG("WAITING arena round (%u s)", s.travel_frames / FRAMES_PER_SECOND);
        }
        return;
    }
    if (scene->next_stage != NONE) return;
    if (boss_battle_in_arena(encounter, scene)) {
        char line[96];
        snprintf(line, sizeof(line), "stage loaded: round %d stage %d entry %d", scene->round, scene->stage, scene->entry);
        reach(BOSS_BATTLE_ARENA_LOADED, line);
        set_state(BOSS_BATTLE_INITIALIZING);
        LOG("Boss Battle transition: arena loaded page_active=%d",
            boss_battle_screen_shown() || boss_battle_screen_requested());
        s.arena_frames = 0;
    } else {
        post_notice("Boss Battle: the game did not enter the boss arena.");
        cancel("a stage was set up, but not the requested arena");
    }
}

static atomic_int quit_source;

static void take_quit(void) {
    if (!atomic_load(&quit_pending)) return;
    switch (boss_battle_state()) {
    case BOSS_BATTLE_TRAVELLING:
        if (atomic_load(&quit_source) != BOSS_BATTLE_QUIT_PEER_LOST) break;
        atomic_store(&quit_pending, 0);
        cancel("a player left before the fight");
        fight_all_lost(BOSS_BATTLE_FIGHT_ALL_QUIT_RUN);
        break;
    case BOSS_BATTLE_INITIALIZING:
        atomic_store(&quit_pending, 0);
        cancel("Quit before Playable");
        fight_all_lost(BOSS_BATTLE_FIGHT_ALL_QUIT_RUN);
        break;
    case BOSS_BATTLE_FIGHTING:
        atomic_store(&quit_pending, 0);
        finish(BOSS_BATTLE_QUIT, BOSS_BATTLE_OUTCOME_QUIT, "Quit", 0);
        break;
    case BOSS_BATTLE_RESOLVING:
    case BOSS_BATTLE_RETURNING: atomic_store(&quit_pending, 0); break;
    default: break;
    }
}

static int in_inn_stage(const BossBattleScene *scene) {
    return scene->round == BOSS_BATTLE_TOWN_ROUND && scene->stage == boss_battle_inn_entry.stage;
}

static int can_leave(const BossBattleScene *scene) {
    return scene->act == SCENE_ACT_PLAY && scene->next_stage == NONE && !in_inn_stage(scene);
}

static void trace(const BossBattleProbe *probe) {
    const BossBattleScene *scene = &probe->scene;
    if ((!s.scene_known || memcmp(scene, &s.last_scene, sizeof(*scene))) && s.scene_lines < SCENE_LINES) {
        ++s.scene_lines;
        LOG("scene act %d step %d, round %d stage %d, next stage %d, entry %d", scene->act, scene->step, scene->round,
            scene->stage, scene->next_stage, scene->entry);
    }
    s.last_scene = *scene;
    s.scene_known = 1;
    if (++s.frames % HEARTBEAT_FRAMES || s.heartbeats >= HEARTBEATS) return;
    ++s.heartbeats;
    const BossBattleEncounter *encounter = current_encounter();
    LOG("heartbeat %u s: state %s, boss %s, act %d step %d round %d stage %d, player unit %s, party ready %s, "
        "encounter ready %s, play %s, playable %s%s",
        s.frames / FRAMES_PER_SECOND, boss_battle_state_text(boss_battle_state()), encounter ? encounter->name : "-",
        scene->act, scene->step, scene->round, scene->stage, probe->player_unit ? "yes" : "no",
        s.milestones & BOSS_BATTLE_PARTY_READY ? "yes" : "no", probe->bosses > 0 ? "yes" : "no",
        scene->act == SCENE_ACT_PLAY && probe->player_unit && !probe->player_dead ? "yes" : "no",
        s.milestones & BOSS_BATTLE_PLAYABLE ? "yes" : "no",
        follower ? " (follower: player unit and playable read party slot 0, the host's seat)" : "");
}

static void defeat(const char *signal) {
    if (boss_battle_state() != BOSS_BATTLE_FIGHTING) return;
    LOG("terminal defeat signal: %s", signal);
    LOG("outcome Death");
    finish(BOSS_BATTLE_DEATH, BOSS_BATTLE_OUTCOME_DEATH, "Death", 0);
}

/* A down player with respawns left gets up again; down with none left, the game would only offer its paid continue,
 * which the campaign gold put back afterwards would make free. */
static void follow_defeat(const BossBattleProbe *probe) {
    if (probe->player_down && !s.player_down) {
        LOG("player down, lives remaining %d", probe->player_lives);
        if (probe->player_lives <= 0) defeat("down with no lives left");
    }
    s.player_down = probe->player_down;
    if (probe->player_dead) defeat("the player is dead");
}

static void win(const char *signal) {
    const uint32_t ms = (uint32_t)((uint64_t)s.fight_frames * 1000 / FRAMES_PER_SECOND);
    LOG("outcome Win (%s, %u ms after Playable)", signal, ms);
    finish(BOSS_BATTLE_WIN, BOSS_BATTLE_OUTCOME_WIN, "Win", ms ? ms : 1);
}

/* The kinds the stage-end act counts as a clear for its per-character records; 3 is the game over. */
static int clear_kind(int kind) {
    return kind >= 0 && kind <= 11 && (0xf07 >> kind & 1);
}

enum { END_KIND_GAME_OVER = 3 };

/* The stage-end act begins once per stage end, however it was asked for: a boss clear the hook missed, the game over,
 * or the player leaving by the pause menu or an exit. */
static void follow_stage_end(const BossBattleProbe *probe) {
    const BossBattleState now = boss_battle_state();
    if (probe->scene.act != BOSS_BATTLE_ACT_STAGE_END || s.stage_end_seen ||
        (now != BOSS_BATTLE_INITIALIZING && now != BOSS_BATTLE_FIGHTING && now != BOSS_BATTLE_RESOLVING))
        return;
    s.stage_end_seen = 1;
    const int kind = probe->end_kind;
    LOG("result pipeline entered: stage end committed, end kind %d", kind);
    if (now == BOSS_BATTLE_INITIALIZING) {
        cancel("the stage ended before Playable");
    } else if (now == BOSS_BATTLE_FIGHTING) {
        if (clear_kind(kind)) {
            LOG("native stage clear signal (end kind %d)", kind);
            win("stage clear committed");
        } else if (kind == END_KIND_GAME_OVER) {
            defeat("the stage ended with the game over");
        } else {
            LOG("the stage was left without a clear (end kind %d)", kind);
            finish(BOSS_BATTLE_QUIT, BOSS_BATTLE_OUTCOME_QUIT, "Quit", 0);
        }
    }
}

/* The clear screen skipped in step 6 leaves step 7 waiting for its answer; return to town is the native one that
 * leads on to the result stage. */
static int answer_clear_choice(const BossBattleProbe *probe) {
    if (!s.clear_screen_skipped || s.choice_answered || probe->scene.act != BOSS_BATTLE_ACT_STAGE_END ||
        probe->scene.step != BOSS_BATTLE_STEP_CLEAR_CHOICE || probe->clear_choice != -1)
        return 0;
    s.choice_answered = 1;
    LOG("stage clear choice answered: return to town");
    return 1;
}

enum { SCENE_ACT_TRAVEL = 4 };

static int transit_wait_mark(unsigned frames) {
    return frames == 30 || frames == 120 || frames == 300;
}

/* The next boss's travel under the held black: requested until the scene's travel act begins, then loading until the
 * arena is set up. */
static void follow_transit(const BossBattleProbe *probe) {
    const BossBattleScene *scene = &probe->scene;
    if (fight_all.transit == BOSS_BATTLE_TRANSIT_REQUESTED) {
        if (scene->act == SCENE_ACT_TRAVEL || boss_battle_state() == BOSS_BATTLE_INITIALIZING) {
            FIGHT_ALL_LOG("scene transition started after %u frames, %u ms", fight_all.wait_frames,
                          probe->host_ms - fight_all.wait_ms);
            fight_all.transit = BOSS_BATTLE_TRANSIT_STAGE_LOADING;
        } else if (!fight_all.wait_frames++) {
            fight_all.wait_ms = probe->host_ms;
            FIGHT_ALL_LOG("next travel requested, waiting for scene transition act=%d step=%d round %d next "
                          "stage %d", scene->act, scene->step, scene->round, scene->next_stage);
        } else if (transit_wait_mark(fight_all.wait_frames)) {
            FIGHT_ALL_LOG("WAITING transition %u frames (%u ms) act=%d step=%d round %d next stage %d fader %s",
                          fight_all.wait_frames, probe->host_ms - fight_all.wait_ms, scene->act, scene->step,
                          scene->round, scene->next_stage, probe->fader_busy ? "fading" : "settled");
        }
    }
    if (fight_all.transit == BOSS_BATTLE_TRANSIT_STAGE_LOADING && boss_battle_state() == BOSS_BATTLE_INITIALIZING)
        fight_all.transit = BOSS_BATTLE_TRANSIT_ARENA_READY;
}

BossBattleFrameAction boss_battle_frame(const BossBattleProbe *probe) {
    if (!boss_battle_session_active()) return BOSS_BATTLE_FRAME_NONE;
    trace(probe);
    follow_stage_end(probe);
    if (answer_clear_choice(probe)) return BOSS_BATTLE_FRAME_ANSWER_CHOICE;
    switch (boss_battle_state()) {
    case BOSS_BATTLE_TRAVELLING: follow_travel(probe); break;
    case BOSS_BATTLE_INITIALIZING:
    case BOSS_BATTLE_FIGHTING:
        if (probe->scene.next_stage == NONE && !boss_battle_in_arena(current_encounter(), &probe->scene)) {
            LOG("left the arena without an outcome (round %d stage %d); the attempt stays unresolved", probe->scene.round,
                probe->scene.stage);
            resolve(boss_battle_state() == BOSS_BATTLE_FIGHTING ? BOSS_BATTLE_OUTCOME_NONE : BOSS_BATTLE_OUTCOME_CANCELLED,
                    "the arena was left");
            break;
        }
        if (boss_battle_state() == BOSS_BATTLE_INITIALIZING) {
            follow_start(probe);
        } else {
            ++s.fight_frames;
            follow_defeat(probe);
        }
        break;
    default: break;
    }
    follow_transit(probe);
    take_quit();
    if (follower) return BOSS_BATTLE_FRAME_NONE;
    switch (boss_battle_state()) {
    case BOSS_BATTLE_RESOLVING:
        if (!s.never_left && can_leave(&probe->scene)) return BOSS_BATTLE_FRAME_REQUEST_INN;
        break;
    case BOSS_BATTLE_RETURNING:
        if (++s.return_frames >= RETURN_RETRY_FRAMES && s.return_requests < RETURN_RETRIES && can_leave(&probe->scene)) {
            LOG("still not in the Inn after %u s; requesting it again", s.return_frames / FRAMES_PER_SECOND);
            return BOSS_BATTLE_FRAME_REQUEST_INN;
        }
        break;
    default: break;
    }
    return BOSS_BATTLE_FRAME_NONE;
}

void boss_battle_inn_requested(void) {
    ++s.return_requests;
    s.return_frames = 0;
    set_state(BOSS_BATTLE_RETURNING);
    LOG("returning: requested %s stage %d (%s) entry %d", boss_battle_inn_entry.round, boss_battle_inn_entry.stage,
        boss_battle_inn_entry.stage_name, boss_battle_inn_entry.entry);
}

static int quit_state(BossBattleState now) {
    return now == BOSS_BATTLE_TRAVELLING || now == BOSS_BATTLE_INITIALIZING || now == BOSS_BATTLE_FIGHTING ||
           now == BOSS_BATTLE_RESOLVING;
}

int boss_battle_request_quit(BossBattleQuitSource source) {
    const int at_checkpoint = atomic_load(&checkpoint_open);
    if (!at_checkpoint && (!boss_battle_session_active() || !quit_state(boss_battle_state()))) return 0;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&quit_pending, &expected, 1)) return 0;
    atomic_store(&quit_source, source);
    LOG("Quit requested (%s) while %s",
        source == BOSS_BATTLE_QUIT_KEY ? "F4" : source == BOSS_BATTLE_QUIT_MENU ? "menu" : "a player left",
        at_checkpoint ? "at the Fight All checkpoint" : boss_battle_state_text(boss_battle_state()));
    return 1;
}

int boss_battle_quit_key(int repeat, int modifiers, int input_owned) {
    if (repeat || modifiers || input_owned) return 0;
    return boss_battle_request_quit(BOSS_BATTLE_QUIT_KEY);
}

int boss_battle_quit_available(void) {
    const BossBattleState now = boss_battle_state();
    return boss_battle_session_active() && now != BOSS_BATTLE_RESOLVING && quit_state(now) && !atomic_load(&quit_pending);
}

int boss_battle_pause_available(void) {
    const BossBattleState now = boss_battle_state();
    return boss_battle_session_active() && (now == BOSS_BATTLE_INITIALIZING || now == BOSS_BATTLE_FIGHTING);
}

int boss_battle_pause_owned(void) {
    return pause_owned && boss_battle_session_active();
}

int boss_battle_pause_opened(char *title, size_t size) {
    const BossBattleEncounter *encounter = current_encounter();
    if (!encounter || !(boss_battle_pause_owned() || boss_battle_pause_available())) return 0;
    snprintf(title, size, "%s", boss_battle_text_boss((int)(encounter - kEncounters)));
    if (!pause_owned) LOG("pause menu opened for %s", encounter->name);
    pause_owned = 1;
    return 1;
}

int boss_battle_pause_quit(void) {
    if (!boss_battle_pause_owned()) return 0;
    LOG("Quit Boss Battle selected");
    return boss_battle_request_quit(BOSS_BATTLE_QUIT_MENU);
}

void boss_battle_pause_closed(void) {
    if (!boss_battle_pause_owned()) return;
    pause_owned = 0;
    LOG("pause menu closed");
}

void boss_battle_boss_cleared(int end_kind) {
    if (!boss_battle_session_active()) return;
    LOG("boss clear committed: the round end is requested (end kind %d)", end_kind);
    switch (boss_battle_state()) {
    case BOSS_BATTLE_FIGHTING: win("boss clear"); break;
    case BOSS_BATTLE_INITIALIZING: cancel("the boss was cleared before Playable"); break;
    default: break;
    }
}

int boss_battle_skip_clear_screen(void) {
    if (!boss_battle_session_active() || boss_battle_state() == BOSS_BATTLE_OFF) return 0;
    s.clear_screen_skipped = 1;
    LOG("Boss Battle bypass activated: the stage clear screen and its continue choice are skipped");
    return 1;
}

int boss_battle_skip_results_screen(void) {
    if (!boss_battle_session_active() || boss_battle_state() == BOSS_BATTLE_OFF) return 0;
    if (s.outcome == BOSS_BATTLE_OUTCOME_WIN) {
        LOG("XP award begin: the results screen runs its experience and level-up");
        return 0;
    }
    LOG("results screen skipped: no experience, loot, General results or story");
    return 1;
}

int boss_battle_results_level_up_done(void) {
    if (!boss_battle_session_active() || boss_battle_state() == BOSS_BATTLE_OFF) return 0;
    LOG("XP award and level-up complete; General results, crown, story and items skipped");
    const BossBattleEncounter *encounter = current_encounter();
    if (encounter && encounter->story_gated) LOG("finale/crown path suppressed");
    return 1;
}

static void progress_log(const char *what, const BossBattleCharacterProgress *p) {
    printf("Boss Battle [%u]: character progression %s level %d xp_pool %d xp_progress %d skill_points %d\n", progress.session,
           what, p->level + 1, p->xp_pool, p->xp_progress, p->skill_points);
}

static int same_character(const BossBattleCharacterProgress *a, const BossBattleCharacterProgress *b) {
    return a->class_index == b->class_index && !memcmp(a->name, b->name, sizeof(a->name));
}

static int same_progression(const BossBattleCharacterProgress *a, const BossBattleCharacterProgress *b) {
    return a->level == b->level && a->xp_pool == b->xp_pool && a->xp_progress == b->xp_progress &&
           a->skill_points == b->skill_points;
}

/* What the native award and level-ups can do to a record: experience only grows, levels only rise, and each new level
 * below 99 gives one skill point. */
static const char *progression_refused(const BossBattleCharacterProgress *before, const BossBattleCharacterProgress *after) {
    if (!same_character(before, after)) return "another character";
    if (after->level < before->level || after->level > MAX_RECORD_LEVEL) return "level out of range";
    if (after->xp_pool < 0 || after->xp_progress < 0) return "negative experience";
    if (after->level == before->level &&
        (int64_t)after->xp_pool + after->xp_progress < (int64_t)before->xp_pool + before->xp_progress)
        return "experience went down";
    const int levels = after->level - before->level;
    if (after->skill_points < before->skill_points || after->skill_points - before->skill_points > levels)
        return "skill points not from level-ups";
    return NULL;
}

void boss_battle_progress_before(const BossBattleCharacterProgress *before) {
    const BossBattleSaveState save = progress.save;
    memset(&progress, 0, sizeof(progress));
    progress.save = save;
    progress.session = boss_battle_session_id();
    progress.before = *before;
    progress.before_known = 1;
    progress_log("before fight", before);
}

void boss_battle_progress_after(const BossBattleCharacterProgress *after) {
    if (!boss_battle_session_active() || !progress.before_known || progress.session != boss_battle_session_id()) return;
    progress_log("after native result", after);
    if (s.outcome != BOSS_BATTLE_OUTCOME_WIN) {
        if (!same_progression(&progress.before, after)) LOG("character progression changed without a Win: discarded");
        return;
    }
    const char *refused = progression_refused(&progress.before, after);
    if (refused) {
        LOG("character progression NOT kept: %s", refused);
        progress.verdict = VERDICT_REFUSED;
        return;
    }
    if (same_progression(&progress.before, after)) {
        progress.verdict = VERDICT_UNCHANGED;
        return;
    }
    progress.after = *after;
    progress.after_valid = 1;
    progress.verdict = VERDICT_KEPT;
    progress.merge = MERGE_PENDING;
}

int boss_battle_progress_merge(const BossBattleCharacterProgress *restored, BossBattleCharacterProgress *merged) {
    const int kept = progress.after_valid;
    const int valid = kept && progress.restore_ok;
    progress.after_valid = progress.restore_ok = 0;
    if (kept && !valid) progress.merge = MERGE_FAILED;
    if (!valid) return 0;
    if (!same_character(restored, &progress.before) || !same_progression(restored, &progress.before)) {
        printf("Boss Battle [%u]: character progression NOT merged: the restored record is not the campaign's\n",
               progress.session);
        progress.merge = MERGE_FAILED;
        return 0;
    }
    progress.merge = MERGE_DONE;
    *merged = *restored;
    merged->xp_pool = progress.after.xp_pool;
    merged->xp_progress = progress.after.xp_progress;
    merged->skill_points = progress.after.skill_points;
    merged->level = progress.after.level;
    progress.save = BOSS_BATTLE_SAVE_PENDING;
    progress_log("merged", merged);
    return 1;
}

BossBattleSaveState boss_battle_progress_save_state(void) {
    return progress.save;
}

void boss_battle_progress_save_started(void) {
    if (progress.save != BOSS_BATTLE_SAVE_PENDING) return;
    progress.save = BOSS_BATTLE_SAVE_RUNNING;
    printf("Boss Battle [%u]: campaign save requested for the character progression\n", progress.session);
}

void boss_battle_progress_save_finished(int ok) {
    if (progress.save != BOSS_BATTLE_SAVE_RUNNING) return;
    progress.save = BOSS_BATTLE_SAVE_IDLE;
    progress.save_ok = ok != 0;
    if (ok) printf("Boss Battle [%u]: character progression persisted to campaign save\n", progress.session);
    else printf("Boss Battle [%u]: campaign save FAILED; the character progression is kept in memory and saved by the "
                "game's next save\n", progress.session);
}

/* NULL when the checkpoint holds; *waiting set when it only has to be waited for. */
static const char *checkpoint_failure(int *waiting) {
    *waiting = 0;
    if (!fight_all.restored) return "the campaign was not restored";
    switch (progress.verdict) {
    case VERDICT_REFUSED: return "the character progression was refused";
    case VERDICT_UNKNOWN: return "the character progression was not captured";
    case VERDICT_UNCHANGED: return NULL;
    case VERDICT_KEPT: break;
    }
    if (progress.merge == MERGE_PENDING) {
        *waiting = 1;
        return NULL;
    }
    if (progress.merge != MERGE_DONE) return "the character progression was not merged";
    if (progress.save != BOSS_BATTLE_SAVE_IDLE) {
        *waiting = 1;
        return NULL;
    }
    return progress.save_ok ? NULL : "the campaign save failed";
}

int boss_battle_fight_all_scene_ready(const BossBattleScene *scene, int fader_busy) {
    return scene && scene->act == SCENE_ACT_PLAY && scene->next_stage == NONE && in_inn_stage(scene) && !fader_busy;
}

BossBattleFightAllTransit boss_battle_fight_all_transit(void) {
    return fight_all.transit;
}

/* Saved: the next boss waits only for the Inn scene to be able to travel, as the Inn's own menu travels. */
static BossBattleFightAllStep wait_for_scene(const BossBattleScene *scene, int fader_busy, int *index) {
    if (fight_all.follow) return BOSS_BATTLE_FIGHT_ALL_STEP_WAIT;
    if (boss_battle_fight_all_scene_ready(scene, fader_busy)) {
        if (fight_all.wait_frames) FIGHT_ALL_LOG("Inn scene ready after %u frames", fight_all.wait_frames);
        fight_all.transit = BOSS_BATTLE_TRANSIT_REQUESTED;
        fight_all.wait_frames = 0;
        ++fight_all.boss;
        *index = fight_all.boss;
        return BOSS_BATTLE_FIGHT_ALL_STEP_START;
    }
    if (++fight_all.checkpoint_frames > CHECKPOINT_TIMEOUT_FRAMES) {
        boss_battle_fight_all_abort("the Inn scene could not start the next travel");
        return BOSS_BATTLE_FIGHT_ALL_STEP_ENDED;
    }
    if (++fight_all.wait_frames == 1 || transit_wait_mark(fight_all.wait_frames))
        FIGHT_ALL_LOG("WAITING scene-ready %u frames act=%d step=%d round %d stage %d next stage %d fader %s",
                      fight_all.wait_frames, scene ? scene->act : -1, scene ? scene->step : -1, scene ? scene->round : -1,
                      scene ? scene->stage : -1, scene ? scene->next_stage : -1, fader_busy ? "fading" : "settled");
    return BOSS_BATTLE_FIGHT_ALL_STEP_WAIT;
}

BossBattleFightAllStep boss_battle_fight_all_poll(const BossBattleScene *scene, int fader_busy, int *index) {
    if (fight_all.phase != FIGHT_ALL_CHECKPOINT) return BOSS_BATTLE_FIGHT_ALL_STEP_IDLE;
    int waiting;
    const char *failure = checkpoint_failure(&waiting);
    if (!failure && waiting && ++fight_all.checkpoint_frames > CHECKPOINT_TIMEOUT_FRAMES)
        failure = "the checkpoint did not finish in time";
    if (failure) {
        boss_battle_fight_all_abort(failure);
        return BOSS_BATTLE_FIGHT_ALL_STEP_ENDED;
    }
    char name[64];
    const int next = fight_all.boss + 1;
    if (atomic_exchange(&quit_pending, 0) && next < BOSS_BATTLE_ENCOUNTERS) {
        fight_all_boss_name(fight_all.boss, name, sizeof(name));
        fight_all_note(BOSS_BATTLE_FIGHT_ALL_QUIT, (unsigned)fight_all.cleared);
        FIGHT_ALL_LOG("Quit after boss %s, progress %02d/%d", name, fight_all.cleared, BOSS_BATTLE_ENCOUNTERS);
        fight_all_stop(BOSS_BATTLE_FIGHT_ALL_QUIT_RUN, "", 0);
        arm_menu_reopen(fight_all.last_session, BOSS_BATTLE_REOPEN_CAMPAIGN_RESTORE);
        return BOSS_BATTLE_FIGHT_ALL_STEP_ENDED;
    }
    if (waiting) return BOSS_BATTLE_FIGHT_ALL_STEP_WAIT;
    if (next >= BOSS_BATTLE_ENCOUNTERS) {
        fight_all_note(BOSS_BATTLE_FIGHT_ALL_CLEAR, fight_all.combat_ms);
        char time[24];
        boss_battle_format_run_time(fight_all.combat_ms, time, sizeof(time));
        FIGHT_ALL_LOG("CLEAR %d/%d, combat time %s", fight_all.cleared, BOSS_BATTLE_ENCOUNTERS, time);
        fight_all_stop(BOSS_BATTLE_FIGHT_ALL_CLEARED, "", 0);
        arm_menu_reopen(fight_all.last_session, BOSS_BATTLE_REOPEN_CAMPAIGN_RESTORE);
        return BOSS_BATTLE_FIGHT_ALL_STEP_ENDED;
    }
    if (fight_all.transit == BOSS_BATTLE_TRANSIT_CHECKPOINT) {
        fight_all_note(BOSS_BATTLE_FIGHT_ALL_PROGRESS, (unsigned)fight_all.cleared);
        fight_all_boss_name(next, name, sizeof(name));
        FIGHT_ALL_LOG("checkpoint saved, next boss %s", name);
        fight_all.transit = BOSS_BATTLE_TRANSIT_SCENE_WAIT;
        fight_all.wait_frames = 0;
    }
    return wait_for_scene(scene, fader_busy, index);
}

int boss_battle_town_return(void) {
    if (!boss_battle_session_active() || boss_battle_state() == BOSS_BATTLE_OFF) return 0;
    LOG("Inn return requested: the stage-end act's return to Hydeland goes to the Inn");
    boss_battle_inn_requested();
    return 1;
}

int boss_battle_party_wiped(void) {
    if (!boss_battle_session_active()) return 0;
    switch (boss_battle_state()) {
    case BOSS_BATTLE_FIGHTING: defeat("every player is down; the game over is replaced by the return to the Inn"); break;
    case BOSS_BATTLE_TRAVELLING:
    case BOSS_BATTLE_INITIALIZING: cancel("the party went down before Playable"); break;
    default: break;
    }
    return 1;
}

void boss_battle_inn_arrived(void) {
    if (!boss_battle_session_active() || boss_battle_state() == BOSS_BATTLE_OFF) return;
    LOG("back in Dragon's Haven Inn (%s, outcome %d)", boss_battle_state_text(boss_battle_state()), s.outcome);
    end_session(1);
}

void boss_battle_title_reached(void) {
    if (fight_all.phase == FIGHT_ALL_SELECTED || fight_all.phase == FIGHT_ALL_CHECKPOINT)
        fight_all_stop(BOSS_BATTLE_FIGHT_ALL_ABORTED, "the title was reached", 0);
    if (!boss_battle_session_active()) return;
    LOG("the title was reached during a session");
    end_session(0);
}

BossBattleInnAction boss_battle_inn_frame(void) {
    if (!boss_battle_session_active()) return BOSS_BATTLE_INN_IDLE;
    if (boss_battle_state() == BOSS_BATTLE_RESOLVING && s.never_left) {
        LOG("the Inn was never left; the session ends here");
        end_session(1);
    }
    return BOSS_BATTLE_INN_HOLD;
}

int boss_battle_inn_native_row(int row) {
    if (row == BOSS_BATTLE_INN_ROW) return BOSS_BATTLE_INN_NO_ROW;
    return row > BOSS_BATTLE_INN_ROW && row < BOSS_BATTLE_INN_ROWS ? row - 1 : row;
}

int boss_battle_inn_display_row(int row) {
    return row >= BOSS_BATTLE_INN_ROW && row < BOSS_BATTLE_INN_ROWS - 1 ? row + 1 : row;
}

int boss_battle_inn_layout(float x, float width, float vanilla_height, float first_center, float last_center,
                           BossBattleInnLayout *layout) {
    const float pitch = (last_center - first_center) / (BOSS_BATTLE_INN_ROWS - 1);
    const float row_height = (float)(int)pitch - 1.0f;
    if (!(pitch > 1.0f) || row_height < 1.0f || row_height > vanilla_height) return 0;
    layout->x = x;
    layout->width = width;
    layout->row_height = row_height;
    layout->pitch = pitch;
    layout->art_scale = row_height / vanilla_height;
    for (int row = 0; row < BOSS_BATTLE_INN_ROWS - 1; ++row) layout->center[row] = first_center + pitch * (float)row;
    layout->center[BOSS_BATTLE_INN_ROWS - 1] = last_center;
    return 1;
}

float boss_battle_inn_row_top(const BossBattleInnLayout *layout, int row) {
    return layout->center[row] - layout->row_height / 2.0f;
}

int boss_battle_menu_cursor(void) {
    return s.cursor;
}

int boss_battle_menu_reopen_pending(void) {
    return atomic_load(&menu_reopen);
}

int boss_battle_take_menu_reopen(void) {
    return atomic_exchange(&menu_reopen, 0);
}

unsigned boss_battle_reopen_session(void) {
    return atomic_load(&reopen_session);
}

void boss_battle_format_time(uint32_t ms, char *out, size_t size) {
    snprintf(out, size, "%02u:%02u.%03u", ms / 60000, ms / 1000 % 60, ms % 1000);
}

void boss_battle_format_run_time(uint32_t ms, char *out, size_t size) {
    if (ms < 3600000u) boss_battle_format_time(ms, out, size);
    else snprintf(out, size, "%02u:%02u:%02u.%03u", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000);
}

enum { SCREEN_IDLE, SCREEN_REQUESTED, SCREEN_SHOWN, SCREEN_WAIT_FRAMES = 3 };
enum { LAST_OFFSET = BOSS_BATTLE_ENCOUNTERS - BOSS_BATTLE_SCREEN_BOSS_ROWS };

/* Game thread only. */
static struct {
    int state, waits, offset, choice, highlighted, cut;
} screen = {SCREEN_IDLE, 0, 0, NONE, BOSS_BATTLE_ENTRY_BOSS_01, 0};

void boss_battle_screen_request(void) {
    screen.state = SCREEN_REQUESTED;
    screen.waits = 0;
    screen.choice = NONE;
}

int boss_battle_screen_requested(void) {
    return screen.state == SCREEN_REQUESTED;
}

int boss_battle_screen_waiting(void) {
    if (screen.state != SCREEN_REQUESTED) return 0;
    if (++screen.waits <= SCREEN_WAIT_FRAMES) return 1;
    puts("Boss Battle: the Inn did not open its Options page for the Boss Battle screen");
    screen.state = SCREEN_IDLE;
    return 0;
}

int boss_battle_screen_built(void) {
    const int boss = s.cursor >= 0 && s.cursor < BOSS_BATTLE_ENCOUNTERS ? s.cursor : 0;
    screen.state = SCREEN_SHOWN;
    if (cursor_fight_all) {
        screen.highlighted = BOSS_BATTLE_ENTRY_FIGHT_ALL;
        screen.offset = 0;
        puts("Boss Battle: Boss Battle screen opened on the native Options page, Fight All highlighted");
        return BOSS_BATTLE_SCREEN_FIGHT_ALL_ROW;
    }
    screen.highlighted = BOSS_BATTLE_ENTRY_BOSS_01 + boss;
    screen.offset = boss < BOSS_BATTLE_SCREEN_BOSS_ROWS ? 0 : boss - BOSS_BATTLE_SCREEN_BOSS_ROWS + 1;
    char label[64];
    boss_battle_label(boss, label, sizeof(label));
    printf("Boss Battle: Boss Battle screen opened on the native Options page, %s highlighted\n", label);
    return 1 + boss - screen.offset;
}

int boss_battle_screen_shown(void) {
    return screen.state == SCREEN_SHOWN;
}

int boss_battle_screen_take_close_cut(void) {
    if (screen.state != SCREEN_SHOWN || screen.choice == NONE || screen.cut) return 0;
    screen.cut = 1;
    return 1;
}

void boss_battle_screen_remember(int row) {
    if (screen.state != SCREEN_SHOWN) return;
    const BossBattleEntry entry = boss_battle_screen_entry(row);
    const int boss = boss_battle_entry_boss(entry);
    if (boss != NONE) {
        s.cursor = boss;
        cursor_fight_all = 0;
    } else if (entry == BOSS_BATTLE_ENTRY_FIGHT_ALL) {
        cursor_fight_all = 1;
    }
}

int boss_battle_cursor_fight_all(void) {
    return cursor_fight_all;
}

void boss_battle_screen_return(void) {
    if (screen.state != SCREEN_SHOWN) return;
    screen.state = SCREEN_IDLE;
    screen.choice = NONE;
    arm_menu_reopen(atomic_load(&current_session), BOSS_BATTLE_REOPEN_NETWORK_MENU_RETURN);
}

int boss_battle_screen_closed(int *index) {
    const int chosen = screen.state == SCREEN_SHOWN ? screen.choice : NONE;
    if (screen.state == SCREEN_SHOWN) puts("Boss Battle: Boss Battle screen closed");
    screen.state = SCREEN_IDLE;
    screen.choice = NONE;
    if (chosen == NONE) return 0;
    *index = chosen;
    return 1;
}

void boss_battle_screen_reset(void) {
    screen.state = SCREEN_IDLE;
    screen.choice = NONE;
    if (fight_all.phase == FIGHT_ALL_SELECTED) fight_all.phase = FIGHT_ALL_OFF;
}

int boss_battle_screen_offset(void) {
    return screen.offset;
}

int boss_battle_entry_boss(BossBattleEntry entry) {
    return entry >= BOSS_BATTLE_ENTRY_BOSS_01 && entry <= BOSS_BATTLE_ENTRY_BOSS_20 ? entry - BOSS_BATTLE_ENTRY_BOSS_01 : NONE;
}

BossBattleEntry boss_battle_screen_entry(int row) {
    if (row == BOSS_BATTLE_SCREEN_FIGHT_ALL_ROW) return BOSS_BATTLE_ENTRY_FIGHT_ALL;
    if (row == BOSS_BATTLE_SCREEN_RETURN_ROW) return BOSS_BATTLE_ENTRY_RETURN_TO_TAVERN;
    if (row < 1 || row > BOSS_BATTLE_SCREEN_BOSS_ROWS) return BOSS_BATTLE_ENTRY_NONE;
    return (BossBattleEntry)(BOSS_BATTLE_ENTRY_BOSS_01 + screen.offset + row - 1);
}

int boss_battle_screen_boss(int row) {
    return boss_battle_entry_boss(boss_battle_screen_entry(row));
}

int boss_battle_fight_all_request(void) {
    if (screen.state != SCREEN_SHOWN || !boss_battle_available() || boss_battle_session_active() ||
        fight_all.phase != FIGHT_ALL_OFF || atomic_load(&pending) != NONE || boss_battle_encounter(0)->unavailable ||
        boss_battle_mode_pending(boss_battle_mode()))
        return 0;
    fight_all_begin_run();
    screen.choice = 0;
    screen.cut = 0;
    puts("Boss Battle: Fight All selected");
    return 1;
}

/* At the last boss row the rows scroll; from the bottom action the cursor wraps to Fight All, with the list from its
 * top again, and the other way round. */
int boss_battle_screen_move(int row, int down) {
    if (down) {
        if (row == BOSS_BATTLE_SCREEN_BOSS_ROWS && screen.offset < LAST_OFFSET) return ++screen.offset, 1;
        if (row == BOSS_BATTLE_SCREEN_RETURN_ROW) screen.offset = 0;
        return 0;
    }
    if (row == 1 && screen.offset > 0) return --screen.offset, 1;
    if (row == BOSS_BATTLE_SCREEN_FIGHT_ALL_ROW) screen.offset = LAST_OFFSET;
    return 0;
}

BossBattleScreenChoice boss_battle_screen_choose(int result, int *index) {
    if (screen.state != SCREEN_SHOWN) return BOSS_BATTLE_SCREEN_NONE;
    const BossBattleEntry entry = boss_battle_screen_entry(result);
    if (entry == BOSS_BATTLE_ENTRY_RETURN_TO_TAVERN) {
        puts("Boss Battle: Return to tavern");
        return BOSS_BATTLE_SCREEN_CLOSE;
    }
    if (entry == BOSS_BATTLE_ENTRY_FIGHT_ALL) return BOSS_BATTLE_SCREEN_FIGHT_ALL;
    const int boss = boss_battle_entry_boss(entry);
    if (boss == NONE) return BOSS_BATTLE_SCREEN_NONE;
    screen.choice = boss;
    screen.cut = 0;
    *index = boss;
    return BOSS_BATTLE_SCREEN_START;
}

/* The bottom action keeps the entry highlighted before it. */
static BossBattleEntry highlighted_entry(int cursor_row) {
    const BossBattleEntry entry = boss_battle_screen_entry(cursor_row);
    if (entry == BOSS_BATTLE_ENTRY_FIGHT_ALL || boss_battle_entry_boss(entry) != NONE) screen.highlighted = entry;
    return (BossBattleEntry)screen.highlighted;
}

void boss_battle_screen_label(int row, char *out, size_t size) {
    const BossBattleEntry entry = boss_battle_screen_entry(row);
    if (entry == BOSS_BATTLE_ENTRY_FIGHT_ALL) snprintf(out, size, "%s", boss_battle_text(BOSS_BATTLE_TEXT_FIGHT_ALL));
    else if (!boss_battle_label(boss_battle_entry_boss(entry), out, size)) snprintf(out, size, "%s", "");
}

static const BossBattleTextKey kStatisticNames[BOSS_BATTLE_SCREEN_BOSS_ROWS] = {
    BOSS_BATTLE_TEXT_ATTEMPTS, BOSS_BATTLE_TEXT_WINS,     BOSS_BATTLE_TEXT_DEATHS,
    BOSS_BATTLE_TEXT_QUITS,    BOSS_BATTLE_TEXT_WIN_RATE, BOSS_BATTLE_TEXT_BEST_TIME};
static const BossBattleTextKey kFightAllStatisticNames[BOSS_BATTLE_SCREEN_BOSS_ROWS] = {
    BOSS_BATTLE_TEXT_RUNS,  BOSS_BATTLE_TEXT_CLEARS,        BOSS_BATTLE_TEXT_DEATHS,
    BOSS_BATTLE_TEXT_QUITS, BOSS_BATTLE_TEXT_BEST_PROGRESS, BOSS_BATTLE_TEXT_BEST_TIME};

static void fight_all_statistic(int row, char *text, size_t size) {
    char key[160];
    boss_battle_character_key(key, sizeof(key));
    const BossBattleFightAllStats stats = boss_battle_record_fight_all_stats(key);
    switch (row) {
    case 1: snprintf(text, size, "%u", stats.runs); break;
    case 2: snprintf(text, size, "%u", stats.clears); break;
    case 3: snprintf(text, size, "%u", stats.deaths); break;
    case 4: snprintf(text, size, "%u", stats.quits); break;
    case 5:
        if (stats.best_progress) snprintf(text, size, "%02u/%d", stats.best_progress, BOSS_BATTLE_ENCOUNTERS);
        break;
    default:
        if (stats.clears && stats.best_ms) boss_battle_format_run_time(stats.best_ms, text, size);
        break;
    }
}

void boss_battle_screen_statistic(int row, int cursor_row, char *out, size_t size) {
    const BossBattleEntry highlighted = highlighted_entry(cursor_row);
    const BossBattleEncounter *encounter = boss_battle_encounter(boss_battle_entry_boss(highlighted));
    if (row < 1 || row > BOSS_BATTLE_SCREEN_BOSS_ROWS) {
        snprintf(out, size, "%s", "");
        return;
    }
    char text[24] = "-";
    if (highlighted == BOSS_BATTLE_ENTRY_FIGHT_ALL) {
        fight_all_statistic(row, text, sizeof(text));
        snprintf(out, size, "%s %s", boss_battle_text(kFightAllStatisticNames[row - 1]), text);
        return;
    }
    if (encounter) {
        char key[160];
        boss_battle_character_key(key, sizeof(key));
        const BossBattleStats stats = boss_battle_record_stats(key, encounter->id);
        switch (row) {
        case 1: snprintf(text, sizeof(text), "%u", stats.attempts); break;
        case 2: snprintf(text, sizeof(text), "%u", stats.wins); break;
        case 3: snprintf(text, sizeof(text), "%u", stats.deaths); break;
        case 4: snprintf(text, sizeof(text), "%u", stats.quits); break;
        case 5:
            if (stats.attempts) snprintf(text, sizeof(text), "%.1f%%", 100.0 * stats.wins / stats.attempts);
            break;
        default:
            if (stats.wins && stats.best_ms) boss_battle_format_time(stats.best_ms, text, sizeof(text));
            break;
        }
    }
    snprintf(out, size, "%s %s", boss_battle_text(kStatisticNames[row - 1]), text);
}

void boss_battle_screen_help(int cursor_row, char *out, size_t size) {
    switch (boss_battle_screen_entry(cursor_row)) {
    case BOSS_BATTLE_ENTRY_FIGHT_ALL: snprintf(out, size, "%s", boss_battle_text(BOSS_BATTLE_TEXT_FIGHT_ALL_HELP)); break;
    case BOSS_BATTLE_ENTRY_RETURN_TO_TAVERN: snprintf(out, size, "%s", boss_battle_text(BOSS_BATTLE_TEXT_RETURN_HELP)); break;
    case BOSS_BATTLE_ENTRY_NONE: snprintf(out, size, "%s", ""); break;
    default:
        if (!boss_battle_character_text(out, size)) snprintf(out, size, "%s", "");
        break;
    }
}

void boss_battle_reset(void) {
    memset(&s, 0, sizeof(s));
    follower = 0;
    atomic_store(&quit_pending, 0);
    atomic_store(&pending, NONE);
    atomic_store(&travelling, NONE);
    atomic_store(&menu_reopen, 0);
    atomic_store(&reopen_session, 0);
    atomic_store(&start_transition, BOSS_BATTLE_START_NONE);
    boss_battle_screen_reset();
    screen.offset = 0;
    screen.highlighted = BOSS_BATTLE_ENTRY_BOSS_01;
    memset(&progress, 0, sizeof(progress));
    memset(&fight_all, 0, sizeof(fight_all));
    cursor_fight_all = 0;
    atomic_store(&checkpoint_open, 0);
    pause_owned = 0;
    atomic_store(&state, BOSS_BATTLE_OFF);
    atomic_store(&session, 0);
    atomic_store(&current_session, 0);
    atomic_store(&enabled, 0);
    lock_text();
    notice_waiting = 0;
    character_name[0] = character_class[0] = 0;
    character_level = 0;
    character_class_index = -1;
    unlock_text();
}

int boss_battle_follower(void) {
    return follower && boss_battle_session_active();
}

int boss_battle_fight_all_follow_ready(int index) {
    int waiting;
    return fight_all.phase == FIGHT_ALL_CHECKPOINT && fight_all.follow && fight_all.boss + 1 == index &&
           !checkpoint_failure(&waiting) && !waiting && fight_all.transit == BOSS_BATTLE_TRANSIT_SCENE_WAIT;
}

int boss_battle_follow(int index, int fight_all_run) {
    const BossBattleEncounter *encounter = boss_battle_encounter(index);
    if (!encounter || encounter->unavailable || boss_battle_session_active() || !boss_battle_available()) return 0;
    if (fight_all_run && index == 0) {
        if (fight_all.phase == FIGHT_ALL_CHECKPOINT) fight_all_stop(BOSS_BATTLE_FIGHT_ALL_ABORTED, "a new run began", 0);
        fight_all_begin_run();
        fight_all.follow = 1;
    } else if (fight_all_run) {
        if (!boss_battle_fight_all_follow_ready(index)) return 0;
        fight_all.transit = BOSS_BATTLE_TRANSIT_REQUESTED;
    } else if (fight_all.phase != FIGHT_ALL_OFF) {
        fight_all_stop(BOSS_BATTLE_FIGHT_ALL_ABORTED, "the host started a single boss", 0);
    }
    int expected = NONE;
    if (!atomic_compare_exchange_strong(&pending, &expected, index)) return 0;
    s.cursor = index;
    printf("Boss Battle: following the host into %02d %s\n", boss_battle_number(index), encounter->name);
    return 1;
}

void boss_battle_follow_started(int index) {
    follower = 1;
    atomic_store(&travelling, index);
    set_state(BOSS_BATTLE_TRAVELLING);
    LOG("waiting for the host's travel to %s", boss_battle_encounter(index)->name);
}

void boss_battle_follow_released(const char *why) {
    if (fight_all.follow && fight_all.phase == FIGHT_ALL_CHECKPOINT) {
        fight_all_stop(BOSS_BATTLE_FIGHT_ALL_ABORTED, why, 1);
        return;
    }
    fight_all.follow = 0;
    if (!boss_battle_follower()) return;
    follower = 0;
    LOG("the host is gone (%s): the fight is left as a Quit", why);
    boss_battle_request_quit(BOSS_BATTLE_QUIT_MENU);
}
