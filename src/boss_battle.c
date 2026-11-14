#include "boss_battle.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "boss_battle_record.h"

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
    {id, name, "round/" file ".brb", round, first, last, stage_name, route, -1, 0, NULL, NULL}

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
    {"ancient_dragon", "Ancient Dragon", "round/LostZone.brb", 10, 0, 0, "st00", 0, -1, 1, NULL, NULL},
    {"demon_king", "Demon King", "round/RandomDungeon.brb", 17, 68, 68, "bg27f", 0, 8, 0, NULL,
     "Pending: not yet confirmed in game."},
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
    snprintf(out, size, "%02d %s", boss_battle_number(index), encounter->name);
    return 1;
}

int boss_battle_startable_count(void) {
    int count = 0;
    for (int i = 0; i < BOSS_BATTLE_ENCOUNTERS; ++i) count += !kEncounters[i].unavailable;
    return count;
}

static atomic_int enabled, hook_installed, state = BOSS_BATTLE_OFF, session = 0, quit_pending = 0;
static atomic_int pending = NONE, travelling = NONE, menu_reopen = 0;
static atomic_uint session_number, current_session;
static struct {
    BossBattleGuardFn protect_saves, snapshot, restore;
    BossBattleUndoFn release_saves;
} guard;
static atomic_flag text_lock = ATOMIC_FLAG_INIT;
static char notice_text[192], character_name[64], character_class[48];
static int notice_waiting, character_level;

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

void boss_battle_set_character(const char *name, const char *class_name, int level) {
    lock_text();
    snprintf(character_name, sizeof(character_name), "%s", name ? name : "");
    snprintf(character_class, sizeof(character_class), "%s", class_name ? class_name : "");
    character_level = level;
    unlock_text();
}

int boss_battle_character_text(char *out, size_t size) {
    lock_text();
    const int known = character_name[0] != 0;
    if (known && size) snprintf(out, size, "%s - %s, level %d", character_name, character_class, character_level);
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
    return 1;
}

static void end_session(int reopen_menu) {
    if (!boss_battle_session_active()) return;
    LOG("campaign restore begins");
    const int restored = guard.restore();
    guard.release_saves();
    atomic_store(&session, 0);
    atomic_store(&quit_pending, 0);
    atomic_store(&travelling, NONE);
    set_state(BOSS_BATTLE_OFF);
    if (restored) {
        LOG("campaign restored, campaign saves released");
        if (reopen_menu) atomic_store(&menu_reopen, 1);
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
           frames == 10 * FRAMES_PER_SECOND;
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
        return;
    }
    ++s.arena_frames;
    if (waiting_mark(s.arena_frames))
        LOG("WAITING %s (%u s after the arena loaded)", missing_milestone(probe), s.arena_frames / FRAMES_PER_SECOND);
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
        s.arena_frames = 0;
    } else {
        post_notice("Boss Battle: the game did not enter the boss arena.");
        cancel("a stage was set up, but not the requested arena");
    }
}

static void take_quit(void) {
    if (!atomic_load(&quit_pending)) return;
    switch (boss_battle_state()) {
    case BOSS_BATTLE_INITIALIZING:
        atomic_store(&quit_pending, 0);
        cancel("Quit before Playable");
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
        "encounter ready %s, play %s, playable %s",
        s.frames / FRAMES_PER_SECOND, boss_battle_state_text(boss_battle_state()), encounter ? encounter->name : "-",
        scene->act, scene->step, scene->round, scene->stage, probe->player_unit ? "yes" : "no",
        s.milestones & BOSS_BATTLE_PARTY_READY ? "yes" : "no", probe->bosses > 0 ? "yes" : "no",
        scene->act == SCENE_ACT_PLAY && probe->player_unit && !probe->player_dead ? "yes" : "no",
        s.milestones & BOSS_BATTLE_PLAYABLE ? "yes" : "no");
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
    take_quit();
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
    if (!boss_battle_session_active() || !quit_state(boss_battle_state())) return 0;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&quit_pending, &expected, 1)) return 0;
    LOG("Quit requested (%s) while %s", source == BOSS_BATTLE_QUIT_KEY ? "F4" : "menu",
        boss_battle_state_text(boss_battle_state()));
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

/* Game thread only. */
static struct {
    BossBattleProgress begin;
    int known;
} progress;

static int progress_ahead(const BossBattleProgress *a, const BossBattleProgress *b) {
    if (a->level != b->level) return a->level > b->level;
    return (int64_t)a->xp_progress + a->xp_pool > (int64_t)b->xp_progress + b->xp_pool;
}

BossBattleProgress boss_battle_progress_effective(const BossBattleProgress *saved, const BossBattleProgress *campaign) {
    return saved && progress_ahead(saved, campaign) ? *saved : *campaign;
}

static int64_t progress_xp(const BossBattleProgress *p) {
    return (int64_t)p->xp_progress + p->xp_pool;
}

int boss_battle_progress_begin(const BossBattleProgress *campaign, BossBattleProgress *effective) {
    char key[160];
    BossBattleProgress saved;
    boss_battle_character_key(key, sizeof(key));
    const int have = boss_battle_progress_get(key, &saved);
    *effective = boss_battle_progress_effective(have ? &saved : NULL, campaign);
    LOG("campaign baseline level %d xp %lld", campaign->level + 1, (long long)progress_xp(campaign));
    LOG("Boss Battle progression loaded level %d xp %lld%s", effective->level + 1, (long long)progress_xp(effective),
        !have ? " (first Boss Battle: the campaign's)" : progress_ahead(&saved, campaign) ? "" : " (the campaign caught up)");
    progress.begin = *effective;
    progress.known = 1;
    return memcmp(effective, campaign, sizeof(*effective)) != 0;
}

void boss_battle_progress_won(const BossBattleProgress *after) {
    char key[160];
    if (!boss_battle_session_active() || !progress.known || s.outcome != BOSS_BATTLE_OUTCOME_WIN) return;
    progress.known = 0;
    const BossBattleProgress *before = &progress.begin;
    if (after->level > before->level) {
        LOG("level-up begin %d -> %d", before->level + 1, after->level + 1);
        LOG("level-up complete level %d", after->level + 1);
    } else {
        LOG("XP +%lld, level unchanged %d", (long long)(progress_xp(after) - progress_xp(before)), after->level + 1);
    }
    boss_battle_character_key(key, sizeof(key));
    const int written = boss_battle_progress_put(key, after);
    LOG("Boss Battle progression %s level %d xp %lld", written ? "saved" : "NOT saved", after->level + 1,
        (long long)progress_xp(after));
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

int boss_battle_menu_cursor(void) {
    return s.cursor;
}

int boss_battle_take_menu_reopen(void) {
    return atomic_exchange(&menu_reopen, 0);
}

void boss_battle_format_time(uint32_t ms, char *out, size_t size) {
    snprintf(out, size, "%02u:%02u.%03u", ms / 60000, ms / 1000 % 60, ms % 1000);
}

enum { SCREEN_IDLE, SCREEN_REQUESTED, SCREEN_SHOWN, SCREEN_WAIT_FRAMES = 3 };
enum { LAST_OFFSET = BOSS_BATTLE_ENCOUNTERS - BOSS_BATTLE_SCREEN_BOSS_ROWS };

/* Game thread only. */
static struct {
    int state, waits, offset, choice, highlighted;
} screen = {SCREEN_IDLE, 0, 0, NONE, 0};

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
    screen.highlighted = boss;
    screen.offset = boss < BOSS_BATTLE_SCREEN_BOSS_ROWS ? 0 : boss - BOSS_BATTLE_SCREEN_BOSS_ROWS + 1;
    char label[64];
    boss_battle_label(boss, label, sizeof(label));
    printf("Boss Battle: Boss Battle screen opened on the native Options page, %s highlighted\n", label);
    return 1 + boss - screen.offset;
}

int boss_battle_screen_shown(void) {
    return screen.state == SCREEN_SHOWN;
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
}

int boss_battle_screen_offset(void) {
    return screen.offset;
}

int boss_battle_screen_boss(int row) {
    if (row < 1 || row > BOSS_BATTLE_SCREEN_BOSS_ROWS) return NONE;
    return screen.offset + row - 1;
}

/* At the last boss row the rows scroll; from the bottom action the cursor wraps to the title bar, which shows the list
 * from its top again, and the other way round. */
int boss_battle_screen_move(int row, int down) {
    if (down) {
        if (row == BOSS_BATTLE_SCREEN_BOSS_ROWS && screen.offset < LAST_OFFSET) return ++screen.offset, 1;
        if (row == BOSS_BATTLE_SCREEN_RETURN_ROW) screen.offset = 0;
        return 0;
    }
    if (row == 1 && screen.offset > 0) return --screen.offset, 1;
    if (row == BOSS_BATTLE_SCREEN_TITLE_ROW) screen.offset = LAST_OFFSET;
    return 0;
}

BossBattleScreenChoice boss_battle_screen_choose(int result, int *index) {
    if (screen.state != SCREEN_SHOWN) return BOSS_BATTLE_SCREEN_NONE;
    if (result == BOSS_BATTLE_SCREEN_RETURN_ROW) {
        puts("Boss Battle: Return to tavern");
        return BOSS_BATTLE_SCREEN_CLOSE;
    }
    const int boss = boss_battle_screen_boss(result);
    if (boss == NONE) return BOSS_BATTLE_SCREEN_NONE;
    screen.choice = boss;
    *index = boss;
    return BOSS_BATTLE_SCREEN_START;
}

static int highlighted_boss(int cursor_row) {
    const int boss = boss_battle_screen_boss(cursor_row);
    if (boss != NONE) screen.highlighted = boss;
    return screen.highlighted;
}

void boss_battle_screen_label(int row, char *out, size_t size) {
    const int boss = boss_battle_screen_boss(row);
    if (boss == NONE || !boss_battle_label(boss, out, size)) snprintf(out, size, "%s", "");
}

void boss_battle_screen_statistic(int row, int cursor_row, char *out, size_t size) {
    const BossBattleEncounter *encounter = boss_battle_encounter(highlighted_boss(cursor_row));
    char key[160], text[24] = "-";
    boss_battle_character_key(key, sizeof(key));
    const BossBattleStats stats = boss_battle_record_stats(key, encounter->id);
    switch (row) {
    case 1: snprintf(out, size, "Attempts %u", stats.attempts); break;
    case 2: snprintf(out, size, "Wins %u", stats.wins); break;
    case 3: snprintf(out, size, "Deaths %u", stats.deaths); break;
    case 4: snprintf(out, size, "Quits %u", stats.quits); break;
    case 5:
        if (stats.attempts) snprintf(text, sizeof(text), "%.1f%%", 100.0 * stats.wins / stats.attempts);
        snprintf(out, size, "Win Rate %s", text);
        break;
    case 6:
        if (stats.wins && stats.best_ms) boss_battle_format_time(stats.best_ms, text, sizeof(text));
        snprintf(out, size, "Best Time %s", text);
        break;
    default: snprintf(out, size, "%s", ""); break;
    }
}

void boss_battle_screen_title(int cursor_row, char *out, size_t size) {
    boss_battle_label(highlighted_boss(cursor_row), out, size);
}

void boss_battle_screen_help(int cursor_row, char *out, size_t size) {
    const int boss = boss_battle_screen_boss(cursor_row);
    if (cursor_row == BOSS_BATTLE_SCREEN_RETURN_ROW) snprintf(out, size, "Return to Dragon's Haven Inn.");
    else if (boss != NONE && boss_battle_encounter(boss)->pending) snprintf(out, size, "Pending: not yet confirmed in game.");
    else if (boss != NONE) snprintf(out, size, "%s", "");
    else if (!boss_battle_character_text(out, size)) snprintf(out, size, "Choose a boss.");
}

void boss_battle_reset(void) {
    memset(&s, 0, sizeof(s));
    atomic_store(&quit_pending, 0);
    atomic_store(&pending, NONE);
    atomic_store(&travelling, NONE);
    atomic_store(&menu_reopen, 0);
    boss_battle_screen_reset();
    screen.offset = screen.highlighted = 0;
    progress.known = 0;
    atomic_store(&state, BOSS_BATTLE_OFF);
    atomic_store(&session, 0);
    atomic_store(&current_session, 0);
    atomic_store(&enabled, 0);
    lock_text();
    notice_waiting = 0;
    character_name[0] = character_class[0] = 0;
    character_level = 0;
    unlock_text();
}
