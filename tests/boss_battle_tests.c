/* Focused tests of Boss Battle: the encounter table, the setting, the native boss list on the Inn's own list, the
 * session's origin and lifecycle (Arena loaded, Player spawned, Party ready, Encounter ready, Playable), the Attempt,
 * Win (native boss clear and stage clear), Death and Quit rules, the stage-end act bypass, the return to the Inn with
 * the campaign put back, the help-line statistics, the capped telemetry, the special bosses' hooks, and the site
 * checks and install on the local boot images under build/image when they are there. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boss_battle.h"
#include "boss_battle_record.h"
#include "exit_game_text.h"
#include "hydeland_town.h"

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define dup _dup
#define dup2 _dup2
#define fileno _fileno
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static int failures = 0;
#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            printf("FAIL line %d: %s\n", __LINE__, #cond);       \
            ++failures;                                          \
        }                                                        \
    } while (0)

static const char *const kNames[BOSS_BATTLE_ENCOUNTERS] = {
    "Harpy",  "Wyvern", "Doom Beetle",   "Golem",  "Gazer",   "Pirates", "Minotaur",      "Vampire",    "Warlock", "Medusa",
    "Wraith", "Killer Rabbit", "Kraken", "Cyclops", "Chimera", "Gargoyle Gate", "Red Dragon", "Arch Demon",
    "Ancient Dragon", "Demon King",
};

static const char *const kRounds[] = {
    "round/GhostshipCove.brb", "round/UnderCitadel.brb", "round/Catacomb.brb", "round/MysticRuins.brb",
    "round/MoldyDungeon.brb", "round/ForgottenRealms.brb", "round/AncientLabyrinth.brb", "round/MageTower.brb",
    "round/LostWoods.brb", "round/Town00.brb", "round/LostZone.brb", NULL, NULL, NULL, NULL, NULL, NULL,
    "round/RandomDungeon.brb",
};

static const struct {
    const char *id;
    int round, first, last;
    const char *stage_name;
} kArenas[18] = {
    {"harpy", 3, 8, 8, "1_st06"},           {"wyvern", 6, 9, 9, "1_st06"},         {"doom_beetle", 4, 10, 10, "1_st07"},
    {"golem", 5, 9, 9, "1_st07"},           {"gazer", 8, 11, 11, "1_st09"},        {"pirates", 0, 10, 10, "1_st07"},
    {"minotaur", 1, 11, 11, "1_st08"},      {"vampire", 2, 9, 9, "1_st07"},        {"warlock", 7, 12, 12, "1_st06"},
    {"medusa", 3, 15, 15, "2_st06"},        {"wraith", 2, 11, 14, "2_st01"},       {"killer_rabbit", 8, 16, 16, "2_st04"},
    {"kraken", 0, 17, 17, "2_st05"},        {"cyclops", 4, 17, 17, "2_st06"},      {"chimera", 7, 18, 21, "2_st04"},
    {"gargoyle_gate", 1, 17, 17, "2_st05"}, {"red_dragon", 6, 11, 13, "2_st01"},   {"arch_demon", 5, 16, 16, "2_st06"},
};

enum { HARPY = 0, MEDUSA = 9, WRAITH = 10, ANCIENT_DRAGON = 18, DEMON_KING = 19 };

static void put_pointer(uint8_t *where, const void *target) {
    const uint64_t value = (uint64_t)(uintptr_t)target;
    memcpy(where, &value, sizeof(value));
}

static void put_u32(uint8_t *where, uint32_t value) {
    memcpy(where, &value, sizeof(value));
}

static void test_encounters(void) {
    int route_a = 0, route_b = 0;
    for (int i = 0; i < BOSS_BATTLE_ENCOUNTERS; ++i) {
        const BossBattleEncounter *e = boss_battle_encounter(i);
        char label[64], expected[64];
        CHECK(e && !strcmp(e->name, kNames[i]) && boss_battle_number(i) == i + 1);
        snprintf(expected, sizeof(expected), "%02d %s", i + 1, kNames[i]);
        CHECK(boss_battle_label(i, label, sizeof(label)) && !strcmp(label, expected));
        for (int j = 0; j < i; ++j) CHECK(strcmp(e->id, boss_battle_encounter(j)->id));
        CHECK(e->round_index >= 0 && e->round_index < (int)(sizeof(kRounds) / sizeof(*kRounds)));
        CHECK(kRounds[e->round_index] && !strcmp(kRounds[e->round_index], e->round));
        CHECK(e->stage >= 0 && e->last_stage >= e->stage && e->stage_name && *e->stage_name);
        CHECK(!e->unavailable && !e->pending == (i != DEMON_KING));
        route_a += e->route == 'A';
        route_b += e->route == 'B';
        if (i < 18) {
            CHECK(!strcmp(e->id, kArenas[i].id) && e->round_index == kArenas[i].round && e->stage == kArenas[i].first &&
                  e->last_stage == kArenas[i].last && !strcmp(e->stage_name, kArenas[i].stage_name));
            CHECK(e->route == (i < 9 ? 'A' : 'B') && e->labyrinth_floor == -1 && !e->story_gated);
        }
    }
    CHECK(route_a == 9 && route_b == 9 && boss_battle_startable_count() == BOSS_BATTLE_ENCOUNTERS);
    /* The proven Harpy destination. */
    const BossBattleEncounter *harpy = boss_battle_encounter(HARPY);
    CHECK(!strcmp(harpy->round, "round/MysticRuins.brb") && harpy->round_index == 3 && harpy->stage == 8 &&
          harpy->last_stage == 8 && !strcmp(harpy->stage_name, "1_st06"));
    const BossBattleEncounter *dragon = boss_battle_encounter(ANCIENT_DRAGON);
    CHECK(!strcmp(dragon->round, "round/LostZone.brb") && dragon->round_index == 10 && dragon->stage == 0 &&
          !strcmp(dragon->stage_name, "st00") && dragon->story_gated && dragon->labyrinth_floor == -1 && !dragon->route);
    const BossBattleEncounter *king = boss_battle_encounter(DEMON_KING);
    CHECK(!strcmp(king->round, "round/RandomDungeon.brb") && king->round_index == 17 && king->stage == 68 &&
          !strcmp(king->stage_name, "bg27f") && king->labyrinth_floor == 8 && !king->story_gated);
    char label[64];
    CHECK(!boss_battle_encounter(-1) && !boss_battle_encounter(BOSS_BATTLE_ENCOUNTERS) && boss_battle_number(-1) == 0 &&
          !boss_battle_label(BOSS_BATTLE_ENCOUNTERS, label, sizeof(label)));
}

static void test_arena_rooms(void) {
    const BossBattleEncounter *harpy = boss_battle_encounter(HARPY), *wraith = boss_battle_encounter(WRAITH);
    const BossBattleScene harpy_room = {2, 0, 3, 8, -1, 0}, waiting = {2, 0, 3, 0, 8, 0}, start_room = {2, 0, 3, 0, -1, 0};
    CHECK(boss_battle_in_arena(harpy, &harpy_room) && !boss_battle_in_arena(harpy, &waiting) &&
          !boss_battle_in_arena(harpy, &start_room));
    for (int stage = 10; stage <= 15; ++stage) {
        const BossBattleScene room = {2, 0, 2, stage, -1, 0};
        CHECK(boss_battle_in_arena(wraith, &room) == (stage >= 11 && stage <= 14));
    }
    CHECK(!strcmp(boss_battle_inn_entry.round, "round/Town00.brb") && boss_battle_inn_entry.stage == 2 &&
          boss_battle_inn_entry.entry == 0 && !strcmp(boss_battle_inn_entry.stage_name, "Inn"));
}

/* ---- the Hydeland town hook other features share ---- */

static uint8_t fake_town[0x1000], fake_context[0x80], fake_manager[0x200], fake_mode[0x600], fake_scene[0x200],
    fake_queue[0x100];
static uint32_t fake_mask_table[4];

static void fake_hydeland(void) {
    memset(fake_town, 0, sizeof(fake_town));
    memset(fake_context, 0, sizeof(fake_context));
    memset(fake_manager, 0, sizeof(fake_manager));
    memset(fake_mode, 0, sizeof(fake_mode));
    memset(fake_scene, 0, sizeof(fake_scene));
    memset(fake_queue, 0, sizeof(fake_queue));
    fake_mask_table[1] = 1;
    put_pointer(fake_town + HYDELAND_TOWN_CONTEXT, fake_context);
    put_pointer(fake_context + HYDELAND_CONTEXT_MANAGER, fake_manager);
    put_pointer(fake_manager + HYDELAND_MANAGER_MODE, fake_mode);
    put_pointer(fake_manager + HYDELAND_MANAGER_SCENE, fake_scene);
    put_pointer(fake_manager + HYDELAND_MANAGER_QUEUE, fake_queue);
    fake_mode[HYDELAND_OBJECT_STATE] = HYDELAND_MODE_TOWN;
    put_u32(fake_mode + HYDELAND_MODE_INPUT, 1);
    fake_scene[HYDELAND_OBJECT_STATE] = HYDELAND_SCENE_PLAY;
    put_u32(fake_scene + HYDELAND_AREA_TYPE, HYDELAND_AREA_TOWN);
    put_u32(fake_scene + HYDELAND_AREA_LOCATION, HYDELAND_AREA_STREET);
    hydeland_town_set_mask_table((uint64_t)(uintptr_t)fake_mask_table);
}

static int early_story_calls, second_calls;
static void GUEST_HOOK_ABI fake_early_story_town(GuestRegs *regs) {
    (void)regs;
    ++early_story_calls;
}
static void GUEST_HOOK_ABI fake_second_town(GuestRegs *regs) {
    (void)regs;
    ++second_calls;
}

static void test_shared_hook(void) {
    static const uint8_t call[] = {0x48, 0x89, 0xdf, 0xe8, 0x10, 0x20, 0x30, 0x00, 0x90};
    GuestHook hook;
    CHECK(hydeland_town_hook_at(call, sizeof(call), 3, &hook) == 1 && hook.handler == hydeland_town_hook);
    fake_hydeland();
    CHECK(hydeland_town_accepts_input(hydeland_town_manager((uint64_t)(uintptr_t)fake_town)));
    early_story_calls = second_calls = 0;
    hydeland_town_observe(HYDELAND_TOWN_EARLY_STORY, fake_early_story_town);
    hydeland_town_observe(HYDELAND_TOWN_BOSS_BATTLE, fake_second_town);
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rdi = (uint64_t)(uintptr_t)fake_town;
    for (int i = 0; i < 3; ++i) hydeland_town_hook(&regs);
    CHECK(early_story_calls == 3 && second_calls == 3 && !regs.skip);
    hydeland_town_observe(HYDELAND_TOWN_EARLY_STORY, NULL);
    hydeland_town_observe(HYDELAND_TOWN_BOSS_BATTLE, NULL);
}

/* ---- the session on the game thread, with a recording campaign guard ---- */

static char guard_log[256];
static int protect_result = 1, snapshot_result = 1, restore_result = 1;

static void note(const char *what) {
    strncat(guard_log, what, sizeof(guard_log) - strlen(guard_log) - 1);
}
static int fake_protect(void) {
    note("P");
    return protect_result;
}
static void fake_release(void) {
    note("R");
}
static int fake_snapshot(void) {
    note("S");
    return snapshot_result;
}
static int fake_restore(void) {
    note("B");
    return restore_result;
}

static void start_guarded(void) {
    boss_battle_reset();
    guard_log[0] = 0;
    protect_result = snapshot_result = restore_result = 1;
    boss_battle_set_save_guard(fake_protect, fake_release);
    boss_battle_set_memory_guard(fake_snapshot, fake_restore);
    boss_battle_set_available(1);
    boss_battle_set_enabled(1);
    boss_battle_set_character("Zak", "Fighter", 1);
}

static const BossBattleScene kInnScene = {2, 0, 9, 2, -1, 0};
static const BossBattleScene kLoading = {4, 3, 3, 2, 8, 0};
static const BossBattleScene kArenaSetup = {4, 9, 3, 8, -1, 0};
static const BossBattleScene kArenaPlay = {2, 0, 3, 8, -1, 0};

static BossBattleProbe probe(BossBattleScene scene, int player, int party, int bosses) {
    BossBattleProbe p;
    memset(&p, 0, sizeof(p));
    p.scene = scene;
    p.player_record = player;
    p.player_unit = player;
    p.player_unit_address = player ? 0x1000 : 0;
    p.party_slots = party;
    p.party_units = party;
    p.bosses = bosses;
    p.boss_address = bosses ? 0x2000 : 0;
    p.boss_hp = bosses ? 100 : 0;
    return p;
}

static BossBattleFrameAction frames(BossBattleProbe p, int count) {
    BossBattleFrameAction last = BOSS_BATTLE_FRAME_NONE;
    for (int i = 0; i < count; ++i) last = boss_battle_frame(&p);
    return last;
}

static int start_session(int index) {
    int queued = -1;
    if (boss_battle_request(index) != BOSS_BATTLE_QUEUED || !boss_battle_begin_queued(&queued) || queued != index) return 0;
    boss_battle_request_sent(index, &kInnScene, 1);
    return 1;
}

static BossBattleStats harpy_stats(void) {
    return boss_battle_record_stats("Zak|Fighter", "harpy");
}

static void reach_arena(void) {
    frames(probe(kInnScene, 0, 0, 0), 3);
    frames(probe(kLoading, 0, 0, 0), 3);
    frames(probe(kArenaSetup, 0, 0, 0), 1);
}

static void reach_playable(void) {
    reach_arena();
    frames(probe(kArenaPlay, 1, 1, 1), 1);
}

static void test_setting(void) {
    start_guarded();
    CHECK(boss_battle_inn_row_wanted());
    boss_battle_set_enabled(0);
    CHECK(!boss_battle_enabled() && !boss_battle_inn_row_wanted());
    boss_battle_set_enabled(1);
    boss_battle_set_available(0);
    CHECK(!boss_battle_inn_row_wanted());
    boss_battle_set_available(1);
    boss_battle_set_memory_guard(NULL, NULL);
    CHECK(!boss_battle_available() && !boss_battle_inn_row_wanted());
    boss_battle_reset();
}

/* Only the Boss Battle list starts a session; a normal campaign frame, boss clear, party wipe or result act changes
 * nothing. */
static void test_session_origin(void) {
    start_guarded();
    BossBattleProbe p = probe(kArenaPlay, 1, 1, 1);
    CHECK(boss_battle_frame(&p) == BOSS_BATTLE_FRAME_NONE && !boss_battle_session_active());
    boss_battle_boss_cleared(1);
    CHECK(!boss_battle_party_wiped() && !boss_battle_request_quit(BOSS_BATTLE_QUIT_MENU));
    CHECK(!boss_battle_skip_clear_screen() && !boss_battle_skip_results_screen() && !boss_battle_town_return());
    boss_battle_inn_arrived();
    boss_battle_title_reached();
    CHECK(!guard_log[0] && boss_battle_state() == BOSS_BATTLE_OFF && boss_battle_inn_frame() == BOSS_BATTLE_INN_IDLE);
    CHECK(harpy_stats().attempts == 0 && !boss_battle_story_gated_round(10));

    CHECK(start_session(HARPY) && !strcmp(guard_log, "PS") && boss_battle_session_active() &&
          boss_battle_session_id() > 0 && boss_battle_state() == BOSS_BATTLE_TRAVELLING);
    CHECK(boss_battle_story_gated_round(10) && !boss_battle_story_gated_round(3));
    CHECK(boss_battle_request(MEDUSA) == BOSS_BATTLE_REFUSED_BUSY && !boss_battle_inn_row_wanted());
    CHECK(boss_battle_inn_frame() == BOSS_BATTLE_INN_HOLD);
    boss_battle_reset();
}

/* Arena loaded, player spawned, party ready and encounter ready never count; Playable counts exactly once. */
static void test_milestones(void) {
    start_guarded();
    CHECK(start_session(HARPY));
    frames(probe(kInnScene, 0, 0, 0), 5);
    frames(probe(kLoading, 0, 0, 0), 5);
    CHECK(boss_battle_state() == BOSS_BATTLE_TRAVELLING && !boss_battle_milestones());
    frames(probe(kArenaSetup, 0, 0, 0), 2);
    CHECK(boss_battle_state() == BOSS_BATTLE_INITIALIZING && boss_battle_milestones() == BOSS_BATTLE_ARENA_LOADED);
    CHECK(harpy_stats().attempts == 0);
    frames(probe(kArenaSetup, 1, 0, 0), 2);
    CHECK((boss_battle_milestones() & BOSS_BATTLE_PLAYER_SPAWNED) && harpy_stats().attempts == 0);
    frames(probe(kArenaSetup, 1, 1, 0), 2);
    CHECK((boss_battle_milestones() & BOSS_BATTLE_PARTY_READY) && harpy_stats().attempts == 0);
    frames(probe(kArenaSetup, 1, 1, 1), 2); /* the boss is there, the scene is not at play yet */
    CHECK((boss_battle_milestones() & BOSS_BATTLE_ENCOUNTER_READY) && harpy_stats().attempts == 0 &&
          boss_battle_state() == BOSS_BATTLE_INITIALIZING);
    BossBattleProbe dead = probe(kArenaPlay, 1, 1, 1);
    dead.player_dead = 1;
    frames(dead, 2);
    CHECK(harpy_stats().attempts == 0);
    frames(probe(kArenaPlay, 1, 1, 1), 30);
    CHECK(boss_battle_state() == BOSS_BATTLE_FIGHTING && (boss_battle_milestones() & BOSS_BATTLE_PLAYABLE));
    CHECK(harpy_stats().attempts == 1 && harpy_stats().wins + harpy_stats().deaths + harpy_stats().quits == 0);
    boss_battle_reset();

    /* no player unit or no boss: never playable */
    start_guarded();
    CHECK(start_session(HARPY));
    reach_arena();
    frames(probe(kArenaPlay, 0, 0, 1), 60);
    frames(probe(kArenaPlay, 1, 1, 0), 60);
    CHECK(boss_battle_state() == BOSS_BATTLE_INITIALIZING && harpy_stats().attempts == 1);
    boss_battle_reset();
}

static void finish_at_inn(void) {
    CHECK(boss_battle_state() == BOSS_BATTLE_RESOLVING);
    CHECK(frames(probe(kArenaPlay, 1, 1, 0), 1) == BOSS_BATTLE_FRAME_REQUEST_INN);
    boss_battle_inn_requested();
    CHECK(boss_battle_state() == BOSS_BATTLE_RETURNING && frames(probe(kArenaPlay, 1, 1, 0), 2) == BOSS_BATTLE_FRAME_NONE);
    CHECK(boss_battle_session_active() && !strstr(guard_log, "B"));
    boss_battle_inn_arrived();
    CHECK(!strcmp(guard_log, "PSBR") && !boss_battle_session_active() && boss_battle_state() == BOSS_BATTLE_OFF);
    CHECK(boss_battle_take_menu_reopen() && !boss_battle_take_menu_reopen());
}

static BossBattleProbe stage_end(int step, int round, int stage, int kind, int choice) {
    const BossBattleScene scene = {BOSS_BATTLE_ACT_STAGE_END, step, round, stage, -1, 0};
    BossBattleProbe p = probe(scene, 0, 0, 0);
    p.end_kind = kind;
    p.clear_choice = choice;
    return p;
}

/* The stage-end act of a won fight: no continue choice, no results screen, and its return to Hydeland goes to the Inn,
 * where the campaign is put back and the list reopens. */
static void finish_result_act(int round, int stage, int kind) {
    CHECK(frames(stage_end(0, round, stage, kind, 0), 5) == BOSS_BATTLE_FRAME_NONE);
    CHECK(boss_battle_skip_clear_screen());
    CHECK(frames(stage_end(6, round, stage, kind, 0), 1) == BOSS_BATTLE_FRAME_NONE);
    CHECK(frames(stage_end(BOSS_BATTLE_STEP_CLEAR_CHOICE, round, stage, kind, -1), 1) == BOSS_BATTLE_FRAME_ANSWER_CHOICE);
    CHECK(frames(stage_end(BOSS_BATTLE_STEP_CLEAR_CHOICE, round, stage, kind, -1), 3) == BOSS_BATTLE_FRAME_NONE);
    CHECK(frames(stage_end(17, round, stage, kind, 1), 3) == BOSS_BATTLE_FRAME_NONE);
    CHECK(frames(stage_end(21, BOSS_BATTLE_TOWN_ROUND, 8, kind, 1), 3) == BOSS_BATTLE_FRAME_NONE);
    CHECK(boss_battle_state() == BOSS_BATTLE_RESOLVING && !boss_battle_skip_results_screen()); /* experience, level-up */
    CHECK(boss_battle_results_level_up_done());
    CHECK(boss_battle_town_return() && boss_battle_state() == BOSS_BATTLE_RETURNING);
    const BossBattleScene result_stage = {2, 0, BOSS_BATTLE_TOWN_ROUND, 8, -1, 0};
    CHECK(frames(probe(result_stage, 0, 0, 0), 30) == BOSS_BATTLE_FRAME_NONE); /* the native request is the Inn's */
    CHECK(boss_battle_session_active() && !strstr(guard_log, "B"));
    boss_battle_inn_arrived();
    CHECK(!strcmp(guard_log, "PSBR") && !boss_battle_session_active() && boss_battle_take_menu_reopen());
}

static int start_playable(int index, BossBattleScene arena) {
    if (!start_session(index)) return 0;
    BossBattleScene loading = arena;
    loading.act = 4;
    loading.next_stage = arena.stage;
    frames(probe(kInnScene, 0, 0, 0), 3);
    frames(probe(loading, 0, 0, 0), 3);
    loading.next_stage = -1;
    frames(probe(loading, 0, 0, 0), 1);
    frames(probe(arena, 1, 1, 1), 1);
    return boss_battle_state() == BOSS_BATTLE_FIGHTING;
}

static BossBattleStats stats_of(int index) {
    return boss_battle_record_stats("Zak|Fighter", boss_battle_encounter(index)->id);
}

/* The boss clear the stage-ending boss's death asks for is the Win; the gauge emptying is not one. */
static void test_win(void) {
    start_guarded();
    const BossBattleStats before = harpy_stats();
    CHECK(start_session(HARPY));
    reach_playable();
    CHECK(harpy_stats().attempts == before.attempts + 1);
    frames(probe(kArenaPlay, 1, 1, 1), 120);
    CHECK(frames(probe(kArenaPlay, 1, 1, 0), 30) == BOSS_BATTLE_FRAME_NONE);
    CHECK(boss_battle_state() == BOSS_BATTLE_FIGHTING && harpy_stats().wins == before.wins);
    boss_battle_boss_cleared(1);
    CHECK(boss_battle_outcome() == BOSS_BATTLE_OUTCOME_WIN && harpy_stats().wins == before.wins + 1);
    CHECK(harpy_stats().best_ms > 2000 && harpy_stats().best_ms <= 2600);
    boss_battle_boss_cleared(1);
    CHECK(boss_battle_party_wiped()); /* already decided: no Death */
    CHECK(boss_battle_request_quit(BOSS_BATTLE_QUIT_KEY) == 1);
    finish_result_act(3, 8, 1);
    const BossBattleStats after = harpy_stats();
    CHECK(after.wins == before.wins + 1 && after.deaths == before.deaths && after.quits == before.quits &&
          after.attempts == before.attempts + 1);
}

/* Harpy, Medusa and the Ancient Dragon: the boss clear, or the stage clear the stage-end act commits when no boss clear
 * came first, is one Win, before the arena is left. */
static void test_win_signals(void) {
    static const struct {
        int index;
        BossBattleScene arena;
        int kind;
    } kFights[] = {
        {HARPY, {2, 0, 3, 8, -1, 0}, 1},
        {MEDUSA, {2, 0, 3, 15, -1, 0}, 1},
        {ANCIENT_DRAGON, {2, 0, 10, 0, -1, 0}, 2},
    };
    for (size_t i = 0; i < sizeof(kFights) / sizeof(*kFights); ++i) {
        const int boss = kFights[i].index;
        const BossBattleScene arena = kFights[i].arena;
        for (int committed_only = 0; committed_only < 2; ++committed_only) {
            start_guarded();
            const BossBattleStats before = stats_of(boss);
            CHECK(start_playable(boss, arena));
            if (!committed_only) boss_battle_boss_cleared(kFights[i].kind);
            frames(stage_end(0, arena.round, arena.stage, kFights[i].kind, 0), 1);
            CHECK(boss_battle_outcome() == BOSS_BATTLE_OUTCOME_WIN && boss_battle_state() == BOSS_BATTLE_RESOLVING);
            /* the round change of the result stage is not a departure without an outcome */
            frames(stage_end(18, BOSS_BATTLE_TOWN_ROUND, 8, kFights[i].kind, 1), 2);
            CHECK(boss_battle_outcome() == BOSS_BATTLE_OUTCOME_WIN);
            const BossBattleStats after = stats_of(boss);
            CHECK(after.attempts == before.attempts + 1 && after.wins == before.wins + 1 && after.deaths == before.deaths &&
                  after.quits == before.quits && after.best_ms > 0);
            CHECK(after.wins + after.deaths + after.quits <= after.attempts);
            CHECK(!boss_battle_skip_results_screen() && boss_battle_results_level_up_done() && boss_battle_town_return());
            boss_battle_inn_arrived();
            CHECK(!boss_battle_session_active() && boss_battle_take_menu_reopen() && boss_battle_menu_cursor() == boss);
        }
    }
    boss_battle_reset();
}

/* The stage end's kind decides an undecided fight: a game over is the Death, the pause menu or an exit is the Quit;
 * before Playable nothing is recorded. */
static void test_stage_end_kinds(void) {
    static const struct {
        int kind;
        BossBattleOutcomeKind outcome;
    } kKinds[] = {{0, BOSS_BATTLE_OUTCOME_WIN},   {8, BOSS_BATTLE_OUTCOME_WIN},   {3, BOSS_BATTLE_OUTCOME_DEATH},
                  {4, BOSS_BATTLE_OUTCOME_QUIT},  {0x11, BOSS_BATTLE_OUTCOME_QUIT}, {0x13, BOSS_BATTLE_OUTCOME_QUIT},
                  {12, BOSS_BATTLE_OUTCOME_QUIT}};
    for (size_t i = 0; i < sizeof(kKinds) / sizeof(*kKinds); ++i) {
        start_guarded();
        const BossBattleStats before = harpy_stats();
        CHECK(start_playable(HARPY, kArenaPlay));
        frames(stage_end(0, 3, 8, kKinds[i].kind, 0), 3);
        CHECK(boss_battle_outcome() == kKinds[i].outcome && boss_battle_state() == BOSS_BATTLE_RESOLVING);
        const BossBattleStats after = harpy_stats();
        CHECK(after.wins - before.wins == (kKinds[i].outcome == BOSS_BATTLE_OUTCOME_WIN) &&
              after.deaths - before.deaths == (kKinds[i].outcome == BOSS_BATTLE_OUTCOME_DEATH) &&
              after.quits - before.quits == (kKinds[i].outcome == BOSS_BATTLE_OUTCOME_QUIT) &&
              after.attempts == before.attempts + 1);
        boss_battle_reset();
    }
    start_guarded();
    const BossBattleStats before = harpy_stats();
    CHECK(start_session(HARPY));
    reach_arena();
    boss_battle_boss_cleared(1);
    frames(stage_end(0, 3, 8, 1, 0), 3);
    CHECK(boss_battle_outcome() == BOSS_BATTLE_OUTCOME_CANCELLED && harpy_stats().attempts == before.attempts &&
          harpy_stats().wins == before.wins);
    boss_battle_reset();
}

/* The clear screen's answer is given once, only for a screen Boss Battle skipped, only while step 7 waits for it. */
static void test_result_bypass(void) {
    start_guarded();
    CHECK(start_playable(HARPY, kArenaPlay));
    boss_battle_boss_cleared(1);
    CHECK(frames(stage_end(BOSS_BATTLE_STEP_CLEAR_CHOICE, 3, 8, 1, -1), 2) == BOSS_BATTLE_FRAME_NONE); /* shown natively */
    CHECK(boss_battle_skip_clear_screen());
    CHECK(frames(stage_end(BOSS_BATTLE_STEP_CLEAR_CHOICE, 3, 8, 1, 0), 1) == BOSS_BATTLE_FRAME_NONE); /* answered */
    CHECK(frames(stage_end(6, 3, 8, 1, -1), 1) == BOSS_BATTLE_FRAME_NONE);
    CHECK(frames(stage_end(BOSS_BATTLE_STEP_CLEAR_CHOICE, 3, 8, 1, -1), 1) == BOSS_BATTLE_FRAME_ANSWER_CHOICE);
    CHECK(frames(stage_end(BOSS_BATTLE_STEP_CLEAR_CHOICE, 3, 8, 1, -1), 1) == BOSS_BATTLE_FRAME_NONE);
    boss_battle_reset();
    CHECK(!boss_battle_skip_clear_screen() && !boss_battle_skip_results_screen() && !boss_battle_town_return() &&
          !boss_battle_results_level_up_done());
}

/* A down player with lives left gets up: no Death. Down with none left, or dead, is the one Death of the attempt; it
 * leaves for the Inn and the next screen shows it. */
static BossBattleProbe player_state(int down, int dead, int lives) {
    BossBattleProbe p = probe(kArenaPlay, 1, 1, 1);
    p.player_down = down;
    p.player_dead = dead;
    p.player_lives = lives;
    return p;
}

static void test_defeat(void) {
    start_guarded();
    BossBattleStats before = harpy_stats();
    CHECK(start_playable(HARPY, kArenaPlay));
    frames(player_state(1, 0, 2), 90); /* down, two lives left */
    frames(player_state(0, 0, 1), 30); /* up again with one */
    frames(player_state(1, 0, 1), 90);
    frames(player_state(0, 0, 0), 30);
    CHECK(boss_battle_state() == BOSS_BATTLE_FIGHTING && harpy_stats().deaths == before.deaths);
    CHECK(frames(player_state(1, 0, 0), 1) == BOSS_BATTLE_FRAME_REQUEST_INN); /* down with none left */
    frames(player_state(1, 0, 0), 30);
    frames(player_state(0, 1, 0), 30);
    CHECK(boss_battle_outcome() == BOSS_BATTLE_OUTCOME_DEATH && harpy_stats().deaths == before.deaths + 1);
    CHECK(boss_battle_party_wiped() && harpy_stats().deaths == before.deaths + 1); /* the game over: still one */
    boss_battle_inn_requested();
    boss_battle_inn_arrived();
    CHECK(boss_battle_take_menu_reopen() && harpy_stats().attempts == before.attempts + 1);

    /* dead after declining the continue */
    start_guarded();
    before = harpy_stats();
    CHECK(start_playable(HARPY, kArenaPlay));
    frames(player_state(0, 1, 3), 2);
    CHECK(boss_battle_outcome() == BOSS_BATTLE_OUTCOME_DEATH && harpy_stats().deaths == before.deaths + 1);

    /* down with no lives before Playable: nothing recorded */
    start_guarded();
    before = harpy_stats();
    CHECK(start_session(HARPY));
    reach_arena();
    BossBattleProbe early = player_state(1, 0, 0);
    early.bosses = 0;
    frames(early, 5);
    CHECK(boss_battle_state() == BOSS_BATTLE_INITIALIZING && harpy_stats().attempts == before.attempts &&
          harpy_stats().deaths == before.deaths);
    boss_battle_reset();
}

/* The Boss Battle character keeps its own level: it starts from the campaign, grows with Wins only, never falls behind
 * the campaign, and a broken file leaves the campaign's. */
static void test_progression(const char *user_dir) {
    const BossBattleProgress campaign = {0, 0, 120}, saved = {1, 0, 40}, behind = {0, 0, 80}, ahead_xp = {0, 0, 200};
    BossBattleProgress effective = boss_battle_progress_effective(NULL, &campaign);
    CHECK(effective.level == 0 && effective.xp_progress == 120);
    effective = boss_battle_progress_effective(&saved, &campaign);
    CHECK(effective.level == 1 && effective.xp_progress == 40);
    effective = boss_battle_progress_effective(&behind, &campaign);
    CHECK(effective.level == 0 && effective.xp_progress == 120);
    effective = boss_battle_progress_effective(&ahead_xp, &campaign);
    CHECK(effective.xp_progress == 200);
    const BossBattleProgress campaign_later = {2, 0, 10};
    effective = boss_battle_progress_effective(&saved, &campaign_later);
    CHECK(effective.level == 2 && effective.xp_progress == 10); /* the campaign passed it */

    char text[512];
    static const char kGood[] = "DCP Boss Battle Progress 1\nZak|Fighter\t1\t0\t40\n";
    static const char kLevelTooHigh[] = "DCP Boss Battle Progress 1\nZak|Fighter\t999\t0\t40\n";
    static const char kNegative[] = "DCP Boss Battle Progress 1\nZak|Fighter\t1\t-3\t40\n";
    CHECK(boss_battle_progress_parse(kGood, sizeof(kGood) - 1));
    CHECK(boss_battle_progress_format(text, sizeof(text)) && !strcmp(text, kGood));
    CHECK(!boss_battle_progress_parse(kLevelTooHigh, sizeof(kLevelTooHigh) - 1));
    CHECK(!boss_battle_progress_parse(kNegative, sizeof(kNegative) - 1));
    CHECK(!boss_battle_progress_parse("garbage", 7));

    /* a Win's level-up is saved; Death and Quit save nothing */
    char path[1200];
    snprintf(path, sizeof(path), "%s/boss_battle_progress.dat", user_dir);
    remove(path);
    boss_battle_progress_configure(user_dir);
    start_guarded();
    CHECK(start_playable(HARPY, kArenaPlay));
    CHECK(!boss_battle_progress_begin(&campaign, &effective) && effective.level == 0); /* first time: the campaign's */
    boss_battle_boss_cleared(1);
    const BossBattleProgress won = {1, 0, 30};
    boss_battle_progress_won(&won);
    boss_battle_progress_won(&saved); /* captured once */
    BossBattleProgress stored;
    CHECK(boss_battle_progress_get("Zak|Fighter", &stored) && stored.level == 1 && stored.xp_progress == 30);
    boss_battle_inn_arrived();

    CHECK(start_playable(HARPY, kArenaPlay));
    CHECK(boss_battle_progress_begin(&campaign, &effective) && effective.level == 1 && effective.xp_progress == 30);
    CHECK(boss_battle_party_wiped());
    const BossBattleProgress lost = {3, 0, 0};
    boss_battle_progress_won(&lost);
    CHECK(boss_battle_progress_get("Zak|Fighter", &stored) && stored.level == 1);
    boss_battle_inn_arrived();

    CHECK(start_playable(HARPY, kArenaPlay));
    boss_battle_progress_begin(&campaign, &effective);
    CHECK(boss_battle_quit_key(0, 0, 0) && frames(probe(kArenaPlay, 1, 1, 1), 1) == BOSS_BATTLE_FRAME_REQUEST_INN);
    boss_battle_progress_won(&lost);
    CHECK(boss_battle_progress_get("Zak|Fighter", &stored) && stored.level == 1);
    boss_battle_reset();

    /* kept across a restart; an unreadable file is left alone and the campaign is used */
    boss_battle_progress_configure(user_dir);
    CHECK(boss_battle_progress_get("Zak|Fighter", &stored) && stored.level == 1);
    FILE *broken = fopen(path, "wb");
    if (broken) {
        fputs("DCP Boss Battle Progress 1\nZak|Fighter\tnot a level\n", broken);
        fclose(broken);
    }
    boss_battle_progress_configure(user_dir);
    CHECK(!boss_battle_progress_get("Zak|Fighter", &stored) && !boss_battle_progress_put("Zak|Fighter", &won));
    start_guarded();
    CHECK(start_playable(HARPY, kArenaPlay));
    CHECK(!boss_battle_progress_begin(&campaign, &effective) && effective.level == 0 && effective.xp_progress == 120);
    boss_battle_reset();
    remove(path);
    boss_battle_progress_configure(user_dir);
}

static void test_death(void) {
    start_guarded();
    const BossBattleStats before = harpy_stats();
    CHECK(start_session(HARPY));
    reach_playable();
    CHECK(boss_battle_party_wiped() == 1 && boss_battle_outcome() == BOSS_BATTLE_OUTCOME_DEATH);
    CHECK(boss_battle_party_wiped() == 1);
    boss_battle_boss_cleared(1);
    finish_at_inn();
    const BossBattleStats after = harpy_stats();
    CHECK(after.deaths == before.deaths + 1 && after.wins == before.wins && after.attempts == before.attempts + 1);
}

/* F4 after Playable records one Quit; before it, nothing at all; while travelling it waits for the arena. */
static void test_quit(void) {
    start_guarded();
    BossBattleStats before = harpy_stats();
    CHECK(start_session(HARPY));
    reach_playable();
    CHECK(boss_battle_quit_available());
    CHECK(boss_battle_quit_key(0, 0, 0) == 1 && !boss_battle_quit_key(0, 0, 0)); /* one request */
    CHECK(frames(probe(kArenaPlay, 1, 1, 1), 1) == BOSS_BATTLE_FRAME_REQUEST_INN);
    CHECK(boss_battle_outcome() == BOSS_BATTLE_OUTCOME_QUIT && !boss_battle_quit_available());
    boss_battle_inn_requested();
    boss_battle_inn_arrived();
    BossBattleStats after = harpy_stats();
    CHECK(after.quits == before.quits + 1 && after.attempts == before.attempts + 1 && after.wins == before.wins &&
          after.deaths == before.deaths);
    CHECK(boss_battle_take_menu_reopen());

    /* before Playable: cancelled, no Attempt and no Quit */
    start_guarded();
    before = harpy_stats();
    CHECK(start_session(HARPY));
    reach_arena();
    frames(probe(kArenaPlay, 1, 1, 0), 10);
    CHECK(boss_battle_request_quit(BOSS_BATTLE_QUIT_MENU) == 1);
    CHECK(frames(probe(kArenaPlay, 1, 1, 0), 1) == BOSS_BATTLE_FRAME_REQUEST_INN);
    CHECK(boss_battle_outcome() == BOSS_BATTLE_OUTCOME_CANCELLED);
    after = harpy_stats();
    CHECK(after.attempts == before.attempts && after.quits == before.quits);

    /* while still leaving the Inn: kept until the arena is set up, then cancelled there */
    start_guarded();
    before = harpy_stats();
    CHECK(start_session(HARPY));
    CHECK(boss_battle_quit_key(0, 0, 0) == 1);
    CHECK(frames(probe(kInnScene, 0, 0, 0), 5) == BOSS_BATTLE_FRAME_NONE && boss_battle_state() == BOSS_BATTLE_TRAVELLING);
    frames(probe(kLoading, 0, 0, 0), 2);
    CHECK(frames(probe(kArenaSetup, 1, 1, 0), 1) == BOSS_BATTLE_FRAME_NONE); /* not at play: no request yet */
    CHECK(boss_battle_outcome() == BOSS_BATTLE_OUTCOME_CANCELLED);
    CHECK(frames(probe(kArenaPlay, 1, 1, 1), 1) == BOSS_BATTLE_FRAME_REQUEST_INN);
    CHECK(harpy_stats().attempts == before.attempts && harpy_stats().quits == before.quits);
    boss_battle_reset();
}

/* Bare F4 only: never repeated, with a modifier (Alt+F4 closes the game), over a menu, or outside a session. */
static void test_quit_key(void) {
    start_guarded();
    CHECK(!boss_battle_quit_available() && !boss_battle_quit_key(0, 0, 0)); /* the Inn menu or the boss list */
    CHECK(start_session(HARPY));
    reach_playable();
    CHECK(!boss_battle_quit_key(1, 0, 0) && !boss_battle_quit_key(0, 1, 0) && !boss_battle_quit_key(0, 0, 1));
    CHECK(boss_battle_quit_available() && boss_battle_quit_key(0, 0, 0));
    boss_battle_reset();
    CHECK(!boss_battle_quit_key(0, 0, 0));
}

/* A wipe before Playable cancels; leaving the arena with no outcome leaves the attempt open. */
static void test_unresolved(void) {
    start_guarded();
    BossBattleStats before = harpy_stats();
    CHECK(start_session(HARPY));
    reach_arena();
    CHECK(boss_battle_party_wiped() == 1 && boss_battle_outcome() == BOSS_BATTLE_OUTCOME_CANCELLED);
    CHECK(harpy_stats().attempts == before.attempts && harpy_stats().deaths == before.deaths);

    start_guarded();
    before = harpy_stats();
    CHECK(start_session(HARPY));
    reach_playable();
    const BossBattleScene elsewhere = {2, 0, 3, 7, -1, 0};
    CHECK(frames(probe(elsewhere, 1, 1, 0), 1) == BOSS_BATTLE_FRAME_REQUEST_INN);
    CHECK(boss_battle_outcome() == BOSS_BATTLE_OUTCOME_NONE);
    const BossBattleStats after = harpy_stats();
    CHECK(after.attempts == before.attempts + 1 && after.wins == before.wins && after.deaths == before.deaths &&
          after.quits == before.quits);
    boss_battle_reset();
}

/* The way back: a failed restore never reopens the list and keeps the saves protected; a transition the game never
 * takes ends the session in the same Inn; the title ends it with no list. */
static void test_return(void) {
    char text[192];
    start_guarded();
    CHECK(start_session(HARPY));
    reach_playable();
    boss_battle_boss_cleared(1);
    CHECK(frames(probe(kInnScene, 1, 1, 0), 1) == BOSS_BATTLE_FRAME_NONE); /* never from the Inn's own stage */
    restore_result = 0;
    boss_battle_inn_arrived();
    CHECK(!strcmp(guard_log, "PSBR") && !boss_battle_take_menu_reopen());
    CHECK(boss_battle_take_notice(text, sizeof(text)) && strstr(text, "restart"));

    start_guarded();
    CHECK(start_session(HARPY));
    CHECK(frames(probe(kInnScene, 0, 0, 0), 601) == BOSS_BATTLE_FRAME_NONE);
    CHECK(boss_battle_state() == BOSS_BATTLE_RESOLVING && boss_battle_session_active());
    CHECK(boss_battle_inn_frame() == BOSS_BATTLE_INN_HOLD && !strcmp(guard_log, "PSBR") && !boss_battle_session_active());
    CHECK(boss_battle_take_menu_reopen() && boss_battle_take_notice(text, sizeof(text)));

    start_guarded();
    CHECK(start_session(HARPY));
    const BossBattleScene wrong_room = {2, 0, 3, 0, -1, 0};
    frames(probe(kLoading, 0, 0, 0), 1);
    CHECK(frames(probe(wrong_room, 1, 1, 0), 1) == BOSS_BATTLE_FRAME_REQUEST_INN);
    boss_battle_inn_requested();
    const BossBattleScene street = {2, 0, 9, 0, -1, 0};
    CHECK(frames(probe(street, 1, 1, 0), 599) == BOSS_BATTLE_FRAME_NONE);
    CHECK(frames(probe(street, 1, 1, 0), 1) == BOSS_BATTLE_FRAME_REQUEST_INN); /* asked again if it never arrives */
    boss_battle_title_reached();
    CHECK(!strcmp(guard_log, "PSBR") && !boss_battle_session_active() && !boss_battle_take_menu_reopen());
    boss_battle_reset();
}

static void test_guard_failures(void) {
    char text[192];
    int queued = -1;
    start_guarded();
    protect_result = 0;
    CHECK(boss_battle_request(HARPY) == BOSS_BATTLE_QUEUED && !boss_battle_begin_queued(&queued));
    CHECK(!strcmp(guard_log, "P") && !boss_battle_session_active() && boss_battle_take_notice(text, sizeof(text)));
    start_guarded();
    snapshot_result = 0;
    CHECK(boss_battle_request(HARPY) == BOSS_BATTLE_QUEUED && !boss_battle_begin_queued(&queued));
    CHECK(!strcmp(guard_log, "PSR") && !boss_battle_session_active());
    start_guarded();
    CHECK(boss_battle_request(HARPY) == BOSS_BATTLE_QUEUED && boss_battle_begin_queued(&queued));
    boss_battle_request_failed(HARPY, "no server");
    CHECK(!strcmp(guard_log, "PSBR") && !boss_battle_session_active() && boss_battle_take_notice(text, sizeof(text)));
    boss_battle_reset();
}

/* Telemetry: WAITING lines at 0, 2, 5 and 10 seconds only, capped heartbeats and scene lines. */
static int count_in(const char *path, const char *needle) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), f)) count += strstr(line, needle) != NULL;
    fclose(f);
    return count;
}

static void test_telemetry(const char *user_dir) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/telemetry.txt", user_dir);
    fflush(stdout);
    const int saved = dup(fileno(stdout));
    if (!freopen(path, "w", stdout)) return;
    start_guarded();
    start_session(HARPY);
    reach_arena();
    frames(probe(kArenaSetup, 0, 0, 0), 40 * 60);
    for (int i = 0; i < 200; ++i) {
        BossBattleScene scene = kArenaSetup;
        scene.step = i % 2;
        frames(probe(scene, 0, 0, 0), 1);
    }
    frames(probe(kArenaSetup, 0, 0, 0), 300 * 60);
    boss_battle_reset();
    fflush(stdout);
    dup2(saved, fileno(stdout));
    CHECK(count_in(path, "WAITING player record") == 4);
    CHECK(count_in(path, "heartbeat") == 24);
    CHECK(count_in(path, "]: scene act") == 96);
    remove(path);
}

/* ---- the Inn menu with the inserted row, and the boss list on the same list ---- */

static void test_inn_rows(void) {
    static const int kNative[BOSS_BATTLE_INN_ROWS] = {0, 1, 2, BOSS_BATTLE_INN_NO_ROW, 3, 4, 5};
    CHECK(BOSS_BATTLE_INN_ROW == 3 && BOSS_BATTLE_INN_ROWS == 7);
    for (int row = 0; row < BOSS_BATTLE_INN_ROWS; ++row) CHECK(boss_battle_inn_native_row(row) == kNative[row]);
    CHECK(boss_battle_inn_native_row(-2) == -2 && boss_battle_inn_native_row(-1) == -1);
    for (int native = 0; native < BOSS_BATTLE_INN_ROWS - 1; ++native) {
        const int shown = boss_battle_inn_display_row(native);
        CHECK(shown != BOSS_BATTLE_INN_ROW && boss_battle_inn_native_row(shown) == native);
    }
}

enum { INN_LIST = 0x3ba0, INN_SIZE = 0x5000, ITEM_SIZE = 0x30, MAX_ITEMS = 32 };
static uint8_t fake_inn[INN_SIZE], fake_inn_context[0x100], fake_items[MAX_ITEMS * ITEM_SIZE];
static uint64_t fake_widgets[8];
static uint8_t fake_new_widget[0x5c0];
static int widgets_added, items_added, disabled_calls, disabled_row = -1, disabled_flag = -1;
static int cursor_calls, cursor_row = -1, party_sends;

static uint8_t *inn_list(void) {
    return fake_inn + INN_LIST;
}

static uint64_t count_of(int begin, int end, int stride) {
    uint64_t b, e;
    memcpy(&b, inn_list() + begin, 8);
    memcpy(&e, inn_list() + end, 8);
    return (e - b) / (uint64_t)stride;
}

static int32_t item_tag(int i) {
    int32_t tag;
    memcpy(&tag, fake_items + i * ITEM_SIZE, sizeof(tag));
    return tag;
}

static void push_item(int32_t tag) {
    const uint64_t count = count_of(0xc78, 0xc80, ITEM_SIZE);
    if (count >= MAX_ITEMS) return;
    memcpy(fake_items + count * ITEM_SIZE, &tag, sizeof(tag));
    put_pointer(inn_list() + 0xc80, fake_items + (count + 1) * ITEM_SIZE);
}

static uint64_t GUEST_HOOK_ABI fake_allocate(uint64_t size) {
    return size == sizeof(fake_new_widget) ? (uint64_t)(uintptr_t)fake_new_widget : 0;
}
static void GUEST_HOOK_ABI fake_widget(uint64_t widget, uint64_t manager) {
    (void)widget, (void)manager;
}
static int GUEST_HOOK_ABI fake_add_widget(uint64_t list, uint64_t widget) {
    (void)widget;
    ++widgets_added;
    put_pointer(inn_list() + 0x70, fake_widgets + count_of(0x68, 0x70, 8) + 1);
    return list == (uint64_t)(uintptr_t)inn_list();
}
static void GUEST_HOOK_ABI fake_add_item(uint64_t list, int message, uint64_t sheet) {
    (void)list, (void)sheet;
    ++items_added;
    push_item(message);
}
static void GUEST_HOOK_ABI fake_set_disabled(uint64_t list, int row, int disabled) {
    (void)list;
    ++disabled_calls;
    disabled_row = row;
    disabled_flag = disabled;
}
static void GUEST_HOOK_ABI fake_set_cursor(uint64_t list, int row, int unused, int notify) {
    (void)unused, (void)notify;
    ++cursor_calls;
    cursor_row = row;
    memcpy((uint8_t *)(uintptr_t)list + 0x958, &row, sizeof(row));
}
static void GUEST_HOOK_ABI fake_inn_party(uint64_t inn) {
    (void)inn;
    ++party_sends;
}

/* The Inn's native rows carry their SystemMsg record. */
static void set_inn(unsigned items, unsigned widgets, int32_t result, int32_t cursor) {
    static const int32_t kRows[7] = {919, 920, 921, 922, 923, 924, 925};
    static const int32_t kSix[6] = {919, 920, 921, 923, 924, 925};
    memset(fake_inn, 0, sizeof(fake_inn));
    memset(fake_items, 0, sizeof(fake_items));
    put_pointer(fake_inn + HYDELAND_TOWN_CONTEXT, fake_inn_context);
    put_pointer(inn_list() + 0xc78, fake_items);
    put_pointer(inn_list() + 0xc80, fake_items);
    for (unsigned i = 0; i < items; ++i) push_item(items == 7 ? kRows[i] : items == 6 ? kSix[i] : (int32_t)i);
    for (unsigned i = 0; i < 8; ++i) fake_widgets[i] = (uint64_t)(uintptr_t)fake_new_widget;
    put_pointer(inn_list() + 0x68, fake_widgets);
    put_pointer(inn_list() + 0x70, fake_widgets + widgets);
    memcpy(inn_list() + 0x960, &result, sizeof(result));
    memcpy(inn_list() + 0x958, &cursor, sizeof(cursor));
}

static GuestRegs inn_regs(void) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.r13 = regs.r14 = (uint64_t)(uintptr_t)fake_inn;
    regs.rdi = (uint64_t)(uintptr_t)inn_list();
    regs.rsi = 0x2f7;
    return regs;
}

static void use_fake_inn_functions(void) {
    const BossBattleInnFunctions functions = {fake_allocate,     fake_widget,     fake_add_widget,
                                              fake_add_item,     fake_set_disabled, fake_set_cursor,
                                              fake_inn_party};
    boss_battle_set_inn_functions(&functions);
    widgets_added = items_added = disabled_calls = cursor_calls = party_sends = 0;
}

static int main_rows_are_inn(void) {
    static const int32_t kShown[7] = {919, 920, 921, 922, 923, 924, 925};
    if (count_of(0xc78, 0xc80, ITEM_SIZE) != 7) return 0;
    for (int i = 0; i < 7; ++i)
        if (item_tag(i) != kShown[i]) return 0;
    return 1;
}

static GuestRegs result_frame(void) {
    GuestRegs regs = inn_regs();
    boss_battle_inn_result_hook(&regs);
    return regs;
}

static void test_inn_build(void) {
    start_guarded();
    use_fake_inn_functions();
    boss_battle_set_enabled(0);
    set_inn(6, 6, -1, 0);
    GuestRegs regs = inn_regs();
    boss_battle_inn_built_hook(&regs);
    CHECK(count_of(0xc78, 0xc80, ITEM_SIZE) == 6 && !widgets_added && !items_added);
    boss_battle_set_enabled(1);
    set_inn(5, 5, -1, 0);
    regs = inn_regs();
    boss_battle_inn_built_hook(&regs);
    CHECK(count_of(0xc78, 0xc80, ITEM_SIZE) == 5 && !items_added);
    set_inn(6, 6, -1, 0);
    regs = inn_regs();
    boss_battle_inn_built_hook(&regs);
    CHECK(widgets_added == 1 && items_added == 1 && main_rows_are_inn() && count_of(0x68, 0x70, 8) == 7);
    boss_battle_reset();
}

/* Native rows reach the native switch as their own row; Boss Battle asks for the native Options row. */
static void test_inn_results(void) {
    start_guarded();
    use_fake_inn_functions();
    static const int32_t kShown[] = {-2, -1, 0, 1, 2, 4, 5, 6};
    static const int32_t kNative[] = {-2, -1, 0, 1, 2, 3, 4, 5};
    for (size_t i = 0; i < sizeof(kShown) / sizeof(*kShown); ++i) {
        set_inn(7, 7, kShown[i], 0);
        GuestRegs regs = result_frame();
        CHECK(regs.skip && (int32_t)regs.rax == kNative[i] && !boss_battle_screen_requested());
        set_inn(6, 6, kNative[i], 0);
        regs = result_frame();
        CHECK(!regs.skip && !boss_battle_screen_requested());
    }
    const uint32_t latch = 0x800;
    const int32_t pending = 1;
    set_inn(7, 7, BOSS_BATTLE_INN_ROW, BOSS_BATTLE_INN_ROW);
    memcpy(inn_list() + 0x964, &latch, sizeof(latch));
    memcpy(inn_list() + 0xb8, &pending, sizeof(pending));
    GuestRegs regs = result_frame();
    CHECK(regs.skip && (int32_t)regs.rax == -1 && !boss_battle_screen_requested()); /* still animating: no choice */
    boss_battle_reset();
}

/* ---- the Boss Battle screen on the native Options page ---- */

enum { ROW_BYTES = 0xa40, BODY_BYTES = 0x1000, WINDOW_BYTES = 0x1000, SCREEN_TEXTS = 64 };
static uint8_t fake_body[BODY_BYTES], fake_window[WINDOW_BYTES], fake_rows[8][ROW_BYTES];
static uint64_t fake_row_list[8];
static struct {
    uint64_t text;
    char value[160];
} screen_texts[SCREEN_TEXTS];
static int text_sets, frame_calls, frame_value = -1, message_value = -1;

static void GUEST_HOOK_ABI fake_set_text(uint64_t text, const char *value) {
    ++text_sets;
    for (int i = 0; i < SCREEN_TEXTS; ++i)
        if (!screen_texts[i].text || screen_texts[i].text == text) {
            screen_texts[i].text = text;
            snprintf(screen_texts[i].value, sizeof(screen_texts[i].value), "%s", value);
            return;
        }
}
static void GUEST_HOOK_ABI fake_set_message(uint64_t text, int message) {
    (void)text;
    message_value = message;
}
static void GUEST_HOOK_ABI fake_row_frame(uint64_t layout, int frame, float start) {
    (void)layout, (void)start;
    ++frame_calls;
    frame_value = frame;
}
static int step_sets, step_level, step_value;
static void GUEST_HOOK_ABI fake_set_step(uint64_t scene, int level, int value) {
    (void)scene;
    ++step_sets;
    step_level = level;
    step_value = value;
}

static void use_fake_screen_functions(void) {
    const BossBattleScreenFunctions functions = {fake_set_step, fake_set_text, fake_set_message, fake_row_frame};
    boss_battle_set_screen_functions(&functions);
    memset(screen_texts, 0, sizeof(screen_texts));
    text_sets = frame_calls = 0;
    frame_value = message_value = -1;
}

static const char *text_at(const uint8_t *base, int offset) {
    for (int i = 0; i < SCREEN_TEXTS; ++i)
        if (screen_texts[i].text == (uint64_t)(uintptr_t)(base + offset)) return screen_texts[i].value;
    return "";
}

static int32_t i32_at(const uint8_t *base, int offset) {
    int32_t value;
    memcpy(&value, base + offset, sizeof(value));
    return value;
}

/* The Inn's first Options page as the game builds it: the title bar, six setting rows, End edit. */
static void fake_options_page(unsigned rows) {
    static const int32_t kTypes[8] = {0, 1, 3, 3, 1, 3, 1, 5};
    memset(fake_body, 0, sizeof(fake_body));
    memset(fake_window, 0, sizeof(fake_window));
    memset(fake_rows, 0, sizeof(fake_rows));
    for (int i = 0; i < 8; ++i) {
        fake_row_list[i] = (uint64_t)(uintptr_t)fake_rows[i];
        put_u32(fake_rows[i] + 0x364, (uint32_t)kTypes[i]);
        put_u32(fake_rows[i] + 0x370, (uint32_t)i);
        for (int state = 0; state < 5; ++state) put_u32(fake_rows[i] + 0x330 + 4 * state, 0x80808000u + (uint32_t)state);
        put_u32(fake_rows[i] + 0x5c0 + 0x110, i == 1 ? 0x7fffffffu : 0x40404040u + (uint32_t)i); /* greyed options */
    }
    put_pointer(fake_body + 0x68, fake_row_list);
    put_pointer(fake_body + 0x70, fake_row_list + rows);
    put_u32(fake_body + 0x968, rows);
    put_u32(fake_body + 0xc50, 1);
}

/* The label column keeps one colour whatever the row's state (cursor, disabled, ...): the plain one. */
static int label_colour_is_plain(int row) {
    for (int state = 0; state < 5; ++state)
        if (i32_at(fake_rows[row], 0x330 + 4 * state) != (int32_t)0x80808001u) return 0;
    return 1;
}

static int label_colours_differ(int row) {
    return i32_at(fake_rows[row], 0x330) != i32_at(fake_rows[row], 0x338);
}

static GuestRegs page_regs(void) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.r15 = regs.rbx = regs.rdi = (uint64_t)(uintptr_t)fake_body;
    regs.r12 = (uint64_t)(uintptr_t)fake_window;
    return regs;
}

static void build_page(void) {
    GuestRegs regs = page_regs();
    regs.rbx = (uint64_t)(uintptr_t)fake_window;
    boss_battle_options_built_hook(&regs);
}

static void set_page_cursor(int32_t row) {
    memcpy(fake_body + 0x958, &row, sizeof(row));
}

static GuestRegs page_result(int32_t result) {
    GuestRegs regs = page_regs();
    regs.rax = (uint32_t)result;
    boss_battle_options_result_hook(&regs);
    return regs;
}

static GuestRegs page_move(int down) {
    GuestRegs regs = page_regs();
    if (down) boss_battle_list_down_hook(&regs);
    else boss_battle_list_up_hook(&regs);
    return regs;
}

/* Rows 1-6 scroll through the twenty bosses at their ends; the title bar and the bottom action stay. */
static void test_screen_rows(void) {
    char text[160];
    boss_battle_reset();
    start_guarded();
    CHECK(start_session(MEDUSA));
    boss_battle_inn_arrived();
    boss_battle_screen_request();
    CHECK(boss_battle_screen_requested() && !boss_battle_screen_shown());
    CHECK(boss_battle_screen_built() == 6 && boss_battle_screen_offset() == 4 && boss_battle_screen_boss(6) == MEDUSA);
    CHECK(boss_battle_screen_boss(0) == -1 && boss_battle_screen_boss(7) == -1 && boss_battle_screen_boss(1) == 4);
    for (int expected = 5; expected <= 14; ++expected)
        CHECK(boss_battle_screen_move(6, 1) == 1 && boss_battle_screen_offset() == expected);
    CHECK(boss_battle_screen_move(6, 1) == 0 && boss_battle_screen_boss(6) == DEMON_KING); /* on to Return to tavern */
    CHECK(boss_battle_screen_move(7, 1) == 0 && boss_battle_screen_offset() == 0);       /* wraps to the title bar */
    CHECK(boss_battle_screen_move(0, 0) == 0 && boss_battle_screen_offset() == 14);      /* and back */
    CHECK(boss_battle_screen_move(1, 0) == 1 && boss_battle_screen_offset() == 13);
    CHECK(boss_battle_screen_move(3, 1) == 0 && boss_battle_screen_move(3, 0) == 0 && boss_battle_screen_offset() == 13);

    boss_battle_screen_label(1, text, sizeof(text));
    CHECK(!strcmp(text, "14 Cyclops"));
    boss_battle_screen_label(0, text, sizeof(text));
    CHECK(!strcmp(text, ""));
    boss_battle_screen_title(6, text, sizeof(text)); /* the number and the name only: no Ready */
    CHECK(!strcmp(text, "19 Ancient Dragon"));
    boss_battle_screen_help(6, text, sizeof(text));
    CHECK(!strcmp(text, "")); /* no help line for a boss */
    boss_battle_screen_title(7, text, sizeof(text)); /* the bottom action keeps the boss last highlighted */
    CHECK(!strcmp(text, "19 Ancient Dragon"));
    boss_battle_screen_move(6, 1);
    boss_battle_screen_title(6, text, sizeof(text)); /* no Pending in the title either */
    CHECK(!strcmp(text, "20 Demon King"));
    boss_battle_screen_help(6, text, sizeof(text));
    CHECK(!strcmp(text, "Pending: not yet confirmed in game."));
    boss_battle_screen_help(5, text, sizeof(text));
    CHECK(!strcmp(text, ""));
    boss_battle_screen_help(7, text, sizeof(text));
    CHECK(!strcmp(text, "Return to Dragon's Haven Inn."));
    boss_battle_screen_help(0, text, sizeof(text));
    CHECK(!strcmp(text, "Zak - Fighter, level 1"));

    int index = -1;
    CHECK(boss_battle_screen_choose(-1, &index) == BOSS_BATTLE_SCREEN_NONE);
    CHECK(boss_battle_screen_choose(0, &index) == BOSS_BATTLE_SCREEN_NONE);
    CHECK(boss_battle_screen_choose(7, &index) == BOSS_BATTLE_SCREEN_CLOSE);
    CHECK(boss_battle_screen_choose(2, &index) == BOSS_BATTLE_SCREEN_START && index == 15);
    CHECK(boss_battle_screen_closed(&index) && index == 15 && !boss_battle_screen_shown());
    CHECK(!boss_battle_screen_closed(&index));

    /* a request the Inn never turns into a page is dropped */
    boss_battle_screen_request();
    CHECK(boss_battle_screen_waiting() && boss_battle_screen_waiting() && boss_battle_screen_waiting());
    CHECK(!boss_battle_screen_waiting() && !boss_battle_screen_requested());
    boss_battle_reset();
}

/* Statistics of the highlighted boss: Win Rate from Wins and Attempts, "-" with no attempt; Best Time MM:SS.mmm after a
 * win only; a result is shown when the screen opens again. */
static void test_screen_statistics(void) {
    char text[160], time[24];
    static const char *const kEmpty[6] = {"Attempts 0", "Wins 0", "Deaths 0", "Quits 0", "Win Rate -", "Best Time -"};
    boss_battle_format_time(84532, time, sizeof(time));
    CHECK(!strcmp(time, "01:24.532"));
    boss_battle_format_time(0, time, sizeof(time));
    CHECK(!strcmp(time, "00:00.000"));
    boss_battle_format_time(3599999, time, sizeof(time));
    CHECK(!strcmp(time, "59:59.999"));
    boss_battle_record_clear();
    start_guarded();
    boss_battle_screen_request();
    boss_battle_screen_built();
    for (int row = 1; row <= 6; ++row) {
        boss_battle_screen_statistic(row, 1, text, sizeof(text));
        CHECK(!strcmp(text, kEmpty[row - 1]));
    }
    boss_battle_screen_statistic(0, 1, text, sizeof(text));
    CHECK(!strcmp(text, ""));

    CHECK(start_playable(HARPY, kArenaPlay) && boss_battle_party_wiped());
    boss_battle_inn_arrived();
    CHECK(start_playable(HARPY, kArenaPlay));
    frames(probe(kArenaPlay, 1, 1, 1), 60);
    boss_battle_boss_cleared(1);
    boss_battle_inn_arrived();
    CHECK(boss_battle_take_menu_reopen() && boss_battle_menu_cursor() == HARPY);
    boss_battle_screen_request();
    const int row = boss_battle_screen_built();
    static const char *const kAfter[6] = {"Attempts 2", "Wins 1", "Deaths 1", "Quits 0", "Win Rate 50.0%",
                                          "Best Time 00:01.000"};
    for (int i = 1; i <= 6; ++i) {
        boss_battle_screen_statistic(i, row, text, sizeof(text));
        CHECK(!strcmp(text, kAfter[i - 1]));
    }
    boss_battle_screen_reset();

    CHECK(start_playable(HARPY, kArenaPlay) && boss_battle_quit_key(0, 0, 0));
    CHECK(frames(probe(kArenaPlay, 1, 1, 1), 1) == BOSS_BATTLE_FRAME_REQUEST_INN);
    boss_battle_inn_requested();
    boss_battle_inn_arrived();
    boss_battle_screen_request();
    const int quit_row = boss_battle_screen_built();
    boss_battle_screen_statistic(1, quit_row, text, sizeof(text));
    CHECK(!strcmp(text, "Attempts 3"));
    boss_battle_screen_statistic(4, quit_row, text, sizeof(text));
    CHECK(!strcmp(text, "Quits 1"));
    boss_battle_screen_statistic(5, quit_row, text, sizeof(text));
    CHECK(!strcmp(text, "Win Rate 33.3%"));
    boss_battle_reset();
}

/* The Inn's Boss Battle row opens the native Options page as the screen; a boss chosen there starts from the Inn's next
 * menu frame; Return to tavern and Back leave the Inn menu as it was; the game's own Options stay its own. */
static void test_screen_hooks(void) {
    start_guarded();
    use_fake_inn_functions();
    use_fake_screen_functions();
    static const int32_t kHelp[6] = {953, 954, 955, 957, 958, 759};
    boss_battle_set_inn_help_table((uint64_t)(uintptr_t)kHelp);

    /* the game's own Options: no title, rows or logic of Boss Battle */
    fake_options_page(8);
    GuestRegs regs = page_regs();
    regs.rsi = 0x39b;
    boss_battle_options_title_hook(&regs);
    CHECK(regs.rsi == 0x39b);
    build_page();
    CHECK(i32_at(fake_body, 0xc50) == 1 && !text_sets && !frame_calls && label_colours_differ(2));
    regs = page_regs();
    boss_battle_options_update_hook(&regs);
    CHECK(!regs.skip);
    CHECK(page_result(3).r15 == (uint64_t)(uintptr_t)fake_body && !page_move(1).skip);

    /* Boss Battle row: the native Options row, as the screen */
    set_inn(7, 7, BOSS_BATTLE_INN_ROW, BOSS_BATTLE_INN_ROW);
    regs = result_frame();
    CHECK(regs.skip && regs.rax == BOSS_BATTLE_INN_OPTIONS_ROW && boss_battle_screen_requested());
    regs = page_regs();
    regs.rsi = 0x39b;
    boss_battle_options_title_hook(&regs);
    CHECK(regs.rsi == BOSS_BATTLE_INN_ROW_RECORD);
    build_page();
    CHECK(boss_battle_screen_shown() && i32_at(fake_body, 0xc50) == 4 && cursor_row == 1 && i32_at(fake_body, 0x958) == 1);
    for (int i = 1; i <= 6; ++i)
        CHECK(i32_at(fake_rows[i], 0x364) == 1 && !strcmp(text_at(fake_rows[i], 0x800), "") && label_colour_is_plain(i));
    CHECK(i32_at(fake_rows[0], 0x364) == 0 && i32_at(fake_rows[7], 0x364) == 5);
    CHECK(label_colours_differ(0) && label_colours_differ(7)); /* the title bar and Return to tavern stay the game's */
    CHECK(frame_calls == 6 && frame_value == 0xe0 && message_value == BOSS_BATTLE_RETURN_TO_TAVERN_RECORD);
    /* the bosses are the selectable value column (right), the statistics the static label column (left) */
    CHECK(!strcmp(text_at(fake_rows[1], 0x5c0), "01 Harpy") && !strcmp(text_at(fake_rows[6], 0x5c0), "06 Pirates"));
    CHECK(!strcmp(text_at(fake_rows[1], 0x380), "Attempts 0") && !strcmp(text_at(fake_rows[6], 0x380), "Best Time -"));
    CHECK(!strcmp(text_at(fake_rows[0], 0x5c0), "01 Harpy"));
    CHECK(!strcmp(text_at(fake_window, 0xfa0), ""));
    /* every boss name in the one plain value colour, played or not */
    for (int i = 1; i <= 6; ++i) CHECK(i32_at(fake_rows[i], 0x5c0 + 0x110) == 0x7fffffff);

    /* no row logic of the game; the cursor follows into the texts; the boss rows scroll at their ends */
    regs = page_regs();
    boss_battle_options_update_hook(&regs);
    CHECK(regs.skip && regs.rax == 0);
    set_page_cursor(3);
    regs = page_result(-1);
    CHECK(regs.r15 == (uint64_t)(uintptr_t)fake_body && !strcmp(text_at(fake_rows[0], 0x5c0), "03 Doom Beetle"));
    set_page_cursor(6);
    regs = page_move(1);
    CHECK(regs.skip && regs.rax == 1 && boss_battle_screen_offset() == 1);
    CHECK(!strcmp(text_at(fake_rows[1], 0x5c0), "02 Wyvern") && !strcmp(text_at(fake_rows[6], 0x5c0), "07 Minotaur"));
    set_page_cursor(3);
    CHECK(!page_move(1).skip && !page_move(0).skip);
    set_page_cursor(7);
    page_result(-1);
    CHECK(!strcmp(text_at(fake_window, 0xfa0), "Return to Dragon's Haven Inn."));

    /* Return to tavern closes the page; the Inn's next menu frame starts nothing */
    regs = page_result(7);
    CHECK(regs.r15 == 1);
    set_inn(7, 7, -1, BOSS_BATTLE_INN_ROW);
    regs = result_frame();
    CHECK(regs.skip && (uint32_t)regs.rax == 0xffffffffu && !boss_battle_screen_shown() && !boss_battle_session_active());
    CHECK(!guard_log[0]);

    /* a boss closes the page and starts from the Inn's next menu frame (no game server here: put back at once) */
    set_inn(7, 7, BOSS_BATTLE_INN_ROW, BOSS_BATTLE_INN_ROW);
    result_frame();
    fake_options_page(8);
    build_page();
    set_page_cursor(2);
    regs = page_result(2);
    CHECK(regs.r15 == 1);
    set_inn(7, 7, -1, BOSS_BATTLE_INN_ROW);
    regs = result_frame();
    CHECK(regs.skip && !strcmp(guard_log, "PSBR") && !boss_battle_session_active() && boss_battle_menu_cursor() == 1);
    char text[192];
    CHECK(boss_battle_take_notice(text, sizeof(text)));

    /* the Inn reached after a fight opens the screen on the last boss chosen, once */
    guard_log[0] = 0;
    CHECK(start_session(MEDUSA));
    boss_battle_inn_arrived();
    set_inn(7, 7, -1, 0);
    regs = result_frame();
    CHECK(regs.skip && regs.rax == BOSS_BATTLE_INN_OPTIONS_ROW && boss_battle_screen_requested());
    fake_options_page(8);
    build_page();
    CHECK(cursor_row == 6 && !strcmp(text_at(fake_rows[6], 0x5c0), "10 Medusa") &&
          !strcmp(text_at(fake_rows[0], 0x5c0), "10 Medusa"));
    regs = page_result(-2); /* Back: the game closes the page */
    set_inn(7, 7, -1, 0);
    result_frame();
    CHECK(!boss_battle_screen_shown() && !boss_battle_take_menu_reopen());

    /* a page that is not the first Options page stays the game's */
    set_inn(7, 7, BOSS_BATTLE_INN_ROW, BOSS_BATTLE_INN_ROW);
    result_frame();
    fake_options_page(7);
    build_page();
    CHECK(!boss_battle_screen_shown() && !boss_battle_screen_requested() && i32_at(fake_body, 0xc50) == 1);

    /* during a session the Inn menu takes no choice */
    CHECK(start_session(HARPY));
    for (int32_t shown = -2; shown <= 6; ++shown) {
        set_inn(7, 7, shown, 0);
        regs = result_frame();
        CHECK(regs.skip && (uint32_t)regs.rax == 0xffffffffu && !boss_battle_screen_requested());
    }
    boss_battle_set_inn_help_table(0);
    boss_battle_set_screen_functions(NULL);
    boss_battle_reset();
}

static void test_inn_help(void) {
    static const int32_t kHelp[6] = {953, 954, 955, 957, 958, 759};
    boss_battle_reset();
    boss_battle_set_inn_help_table((uint64_t)(uintptr_t)kHelp);
    static const int32_t kExpected[BOSS_BATTLE_INN_ROWS] = {953, 954, 955, BOSS_BATTLE_INN_HELP_RECORD, 957, 958, 759};
    for (int row = 0; row < BOSS_BATTLE_INN_ROWS; ++row) {
        set_inn(7, 7, -1, row);
        GuestRegs regs = inn_regs();
        boss_battle_inn_help_hook(&regs);
        CHECK((int32_t)regs.rsi == kExpected[row]);
    }
    set_inn(6, 6, -1, 5);
    GuestRegs regs = inn_regs();
    boss_battle_inn_help_hook(&regs);
    CHECK(regs.rsi == 0x2f7);
    boss_battle_set_inn_help_table(0);
}

static void test_inn_row_calls(void) {
    start_guarded();
    use_fake_inn_functions();
    for (int native = 0; native <= 5; ++native) {
        set_inn(7, 7, -1, 0);
        GuestRegs regs = inn_regs();
        regs.rsi = (uint64_t)native;
        boss_battle_inn_shift_hook(&regs);
        CHECK(!regs.skip && (int)regs.rsi == (native < 3 ? native : native + 1) && !disabled_calls);
        set_inn(6, 6, -1, 0);
        regs = inn_regs();
        regs.rsi = (uint64_t)native;
        boss_battle_inn_shift_hook(&regs);
        boss_battle_inn_mirror_hook(&regs);
        CHECK((int)regs.rsi == native && !disabled_calls);
    }
    set_inn(7, 7, -1, 0);
    GuestRegs regs = inn_regs();
    regs.rsi = 5;
    regs.rdx = 1;
    boss_battle_inn_mirror_hook(&regs);
    CHECK(regs.rsi == 6 && disabled_calls == 1 && disabled_row == BOSS_BATTLE_INN_ROW && disabled_flag == 1);
    /* the boss list's 21 rows are never translated */
    set_inn(21, 7, -1, 0);
    regs = inn_regs();
    regs.rsi = 5;
    boss_battle_inn_mirror_hook(&regs);
    CHECK(regs.rsi == 5 && disabled_calls == 1);
    boss_battle_reset();
}

/* ---- the outcome hooks outside and inside a session ---- */

static void test_outcome_hooks(void) {
    GuestRegs regs, screen;
    memset(&regs, 0, sizeof(regs));
    regs.rsi = 1;
    start_guarded();
    boss_battle_boss_cleared_hook(&regs);
    boss_battle_party_wiped_hook(&regs);
    memset(&screen, 0, sizeof(screen));
    screen.rax = 0x1234;
    boss_battle_clear_screen_hook(&screen);
    boss_battle_results_screen_hook(&screen);
    CHECK(!regs.skip && !screen.skip && screen.rax == 0x1234 && !boss_battle_session_active()); /* campaign play */

    const BossBattleStats before = harpy_stats();
    CHECK(start_session(HARPY));
    reach_playable();
    boss_battle_boss_cleared_hook(&regs);
    CHECK(boss_battle_outcome() == BOSS_BATTLE_OUTCOME_WIN && harpy_stats().wins == before.wins + 1 && !regs.skip);
    boss_battle_party_wiped_hook(&regs);
    CHECK(regs.skip && harpy_stats().deaths == before.deaths); /* the game over is never shown in a session */
    boss_battle_clear_screen_hook(&screen);
    CHECK(screen.skip && screen.rax == 0);
    memset(&screen, 0, sizeof(screen));
    screen.rax = 0x1234;
    boss_battle_results_screen_hook(&screen);
    CHECK(!screen.skip && screen.rax == 0x1234); /* a Win's results screen opens for its experience and level-up */
    GuestRegs act = {0};
    act.rsi = 3;
    boss_battle_results_level_done_hook(&act);
    CHECK(act.rsi == 6 && !act.skip); /* then closes instead of its General results */
    boss_battle_reset();
    act.rsi = 3;
    boss_battle_results_level_done_hook(&act);
    CHECK(act.rsi == 3); /* campaign results stay the game's */

    /* a stage end that is not a Win keeps the results screen closed */
    start_guarded();
    CHECK(start_session(HARPY));
    reach_playable();
    CHECK(boss_battle_quit_key(0, 0, 0) && frames(probe(kArenaPlay, 1, 1, 1), 1) == BOSS_BATTLE_FRAME_REQUEST_INN);
    memset(&screen, 0, sizeof(screen));
    screen.rax = 0x1234;
    boss_battle_results_screen_hook(&screen);
    CHECK(screen.skip && screen.rax == 0);
    boss_battle_reset();
}

/* ---- the campaign in memory ---- */

static uint8_t *fake_save, *fake_play_data, *other_play_data;
static uint64_t fake_play_table[4];
static uint8_t fake_game_context[0x100];

static void fake_campaign(void) {
    if (!fake_save) fake_save = calloc(1, BOSS_BATTLE_SAVE_BYTES);
    if (!fake_play_data) fake_play_data = calloc(1, BOSS_BATTLE_PLAY_DATA_BYTES);
    if (!other_play_data) other_play_data = calloc(1, BOSS_BATTLE_PLAY_DATA_BYTES);
    memset(fake_save, 0x11, BOSS_BATTLE_SAVE_BYTES);
    memset(fake_play_data, 0x22, BOSS_BATTLE_PLAY_DATA_BYTES);
    fake_save[0x20eb15] = 1;
    fake_play_table[1] = (uint64_t)(uintptr_t)fake_play_data;
    put_pointer(fake_game_context + 0xe8, fake_save);
    boss_battle_campaign_reset();
    boss_battle_campaign_configure((uint64_t)(uintptr_t)fake_play_table);
    boss_battle_campaign_use((uint64_t)(uintptr_t)fake_game_context);
}

static int filled(const uint8_t *bytes, size_t size, uint8_t value) {
    for (size_t i = 0; i < size; ++i)
        if (bytes[i] != value && !(size == BOSS_BATTLE_SAVE_BYTES && i == 0x20eb15)) return 0;
    return 1;
}

static void test_campaign_restore(void) {
    fake_campaign();
    const uint64_t live = (uint64_t)(uintptr_t)fake_save;
    CHECK(boss_battle_campaign_play_data() == (uint64_t)(uintptr_t)fake_play_data);
    CHECK(boss_battle_campaign_save_source(live) == live && !boss_battle_campaign_held());
    CHECK(boss_battle_campaign_snapshot() && boss_battle_campaign_held());
    /* the snapshot changes nothing the game uses */
    CHECK(boss_battle_campaign_play_data() == (uint64_t)(uintptr_t)fake_play_data && fake_play_table[1] ==
          (uint64_t)(uintptr_t)fake_play_data && filled(fake_play_data, BOSS_BATTLE_PLAY_DATA_BYTES, 0x22));
    const uint64_t kept = boss_battle_campaign_save_source(live);
    CHECK(kept != live && filled((const uint8_t *)(uintptr_t)kept, BOSS_BATTLE_SAVE_BYTES, 0x11));
    memset(fake_play_data + 0x70, 0x5a, 0x1000);
    memset(fake_play_data + 0xa480, 0x5b, 0x21d0);
    memset(fake_save + 0x50, 0x5d, 0xaf8d4);
    CHECK(boss_battle_campaign_restore() && !boss_battle_campaign_held());
    CHECK(filled(fake_save, BOSS_BATTLE_SAVE_BYTES, 0x11) && filled(fake_play_data, BOSS_BATTLE_PLAY_DATA_BYTES, 0x22));
    CHECK(boss_battle_campaign_save_source(live) == live);

    fake_campaign();
    CHECK(boss_battle_campaign_snapshot());
    fake_play_table[1] = (uint64_t)(uintptr_t)other_play_data;
    memset(fake_play_data, 0x33, 16);
    CHECK(!boss_battle_campaign_restore() && fake_play_data[0] == 0x33);
    CHECK(boss_battle_campaign_save_source(live) != live);

    fake_campaign();
    put_pointer(fake_game_context + 0xe8, NULL);
    CHECK(!boss_battle_campaign_snapshot() && !boss_battle_campaign_held());
    boss_battle_campaign_reset();
}

/* With the real guard: the Inn reached after the fight puts the campaign back before the list reopens; a restore that
 * cannot find the kept objects keeps every save on the kept campaign and never reopens the list. */
static void test_campaign_session(void) {
    fake_campaign();
    start_guarded();
    boss_battle_set_memory_guard(boss_battle_campaign_snapshot, boss_battle_campaign_restore);
    const uint64_t live = (uint64_t)(uintptr_t)fake_save;
    CHECK(start_session(HARPY) && boss_battle_campaign_held());
    reach_playable();
    memset(fake_play_data + 0xa480, 0x77, 0x21d0);
    CHECK(boss_battle_party_wiped());
    CHECK(boss_battle_campaign_save_source(live) != live);
    boss_battle_inn_arrived();
    CHECK(filled(fake_play_data, BOSS_BATTLE_PLAY_DATA_BYTES, 0x22) && !strcmp(guard_log, "PR"));
    CHECK(boss_battle_campaign_save_source(live) == live && boss_battle_take_menu_reopen());

    fake_campaign();
    start_guarded();
    boss_battle_set_memory_guard(boss_battle_campaign_snapshot, boss_battle_campaign_restore);
    CHECK(start_session(HARPY));
    reach_playable();
    fake_play_table[1] = (uint64_t)(uintptr_t)other_play_data;
    boss_battle_inn_arrived();
    CHECK(!boss_battle_session_active() && boss_battle_campaign_save_source(live) != live && !boss_battle_take_menu_reopen());
    boss_battle_reset();
    boss_battle_campaign_reset();
}

/* ---- the local boot images ---- */

static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) return 0;
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = v << 8 | b[i];
    return v;
}

static unsigned char *boot_image(const char *folder, uint64_t *size, size_t room) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/build/image/%s/boot.bin", DCP_SOURCE_DIR, folder);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "DCPBOOT1", 8)) return fclose(f), NULL;
    *size = read64(f);
    read64(f);
    const uint64_t segments = read64(f), relocs = read64(f), names = read64(f);
    read64(f), read64(f);
    for (int i = 0; i < 4; ++i) read64(f);
    const uint64_t modules = read64(f);
    fseek(f, (long)(modules * 56), SEEK_CUR);
    const uint64_t bindings = read64(f);
    fseek(f, (long)(bindings * 24 + segments * 24 + names * 128 + relocs * 32), SEEK_CUR);
    unsigned char *image = calloc(1, *size + room);
    if (image && fread(image, 1, *size, f) != *size) free(image), image = NULL;
    fclose(f);
    return image;
}

static const struct {
    const char *folder, *game;
} kOtherBuilds[] = {
    {"CUSA07001-1.00", "CUSA07001-1.00"},
    {"CUSA07001-1.02", "CUSA07001-1.02"},
    {"CUSA07001-1.03", "CUSA07001-1.03"},
    {"CUSA10209-1.00", "CUSA10209-1.00"},
};

typedef struct {
    uint64_t at, length;
} PatchedSite;

static int bytes_changed_outside(const unsigned char *image, const unsigned char *before, uint64_t size,
                                 const PatchedSite *sites, size_t count) {
    int changed = 0;
    for (uint64_t k = 0; k < size; ++k) {
        int patched = 0;
        for (size_t c = 0; c < count; ++c) patched |= k >= sites[c].at && k < sites[c].at + sites[c].length;
        changed += image[k] != before[k] && !patched;
    }
    return changed;
}

static void check_story_hooks(const unsigned char *image) {
    static uint8_t scene[0x200], settings[0x100], context[0x100];
    memset(scene, 0, sizeof(scene));
    put_pointer(scene, image + 0xb8cf30);
    put_pointer(context + 0x88, scene);
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.r15 = (uint64_t)(uintptr_t)scene;
    regs.r12 = (uint64_t)(uintptr_t)settings;
    regs.r13 = 4;
    put_u32(scene + 0x108, 10);
    start_guarded();
    boss_battle_arrange_story_hook(&regs);
    boss_battle_story_clear_hook(&regs);
    CHECK(!regs.skip);
    GuestRegs dragon;
    memset(&dragon, 0, sizeof(dragon));
    dragon.r15 = (uint64_t)(uintptr_t)context;
    boss_battle_dragon_hook(&dragon);
    CHECK(!dragon.skip);
    /* a session's scene frame on a scene that is not the scene manager reads nothing */
    GuestRegs frame;
    memset(&frame, 0, sizeof(frame));
    frame.r15 = (uint64_t)(uintptr_t)settings;
    CHECK(start_session(ANCIENT_DRAGON));
    boss_battle_scene_hook(&frame);
    CHECK(boss_battle_state() == BOSS_BATTLE_TRAVELLING);
    put_u32(scene + 0x108, 3);
    boss_battle_arrange_story_hook(&regs);
    boss_battle_dragon_hook(&dragon);
    CHECK(!regs.skip && !dragon.skip);
    put_u32(scene + 0x108, 10);
    put_pointer(scene, image + 0xb8cf38);
    boss_battle_arrange_story_hook(&regs);
    boss_battle_dragon_hook(&dragon);
    CHECK(!regs.skip && !dragon.skip);
    put_pointer(scene, image + 0xb8cf30);
    regs.rax = 1;
    boss_battle_arrange_story_hook(&regs);
    CHECK(regs.skip && regs.rax == 0);
    boss_battle_dragon_hook(&dragon);
    CHECK(dragon.skip && dragon.rax == 1);
    memset(&regs, 0, sizeof(regs));
    regs.rax = 1;
    boss_battle_story_clear_hook(&regs);
    CHECK(regs.skip && regs.rax == 0);
    boss_battle_reset();
}

/* On the image's scene vtable: the skipped clear screen's answer, and the stage-end act's return sent to the Inn. */
static void check_result_hooks(const unsigned char *image) {
    static uint8_t scene[0x9400];
    memset(scene, 0, sizeof(scene));
    put_pointer(scene, image + 0xb8cf30);
    scene[0x5b] = BOSS_BATTLE_ACT_STAGE_END;
    scene[0x5c] = BOSS_BATTLE_STEP_CLEAR_CHOICE;
    put_u32(scene + 0x108, 10);
    put_u32(scene + 0xf8, 0xffffffffu);
    put_u32(scene + 0xe8, 2);
    put_u32(scene + 0x9314, 0xffffffffu);
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.r14 = regs.r15 = (uint64_t)(uintptr_t)scene;
    regs.rdx = regs.rcx = 0xffffffffu;
    regs.r8 = 0x1234;
    start_guarded();
    use_fake_screen_functions();
    boss_battle_town_return_hook(&regs);
    CHECK(regs.rdx == 0xffffffffu && regs.r8 == 0x1234); /* campaign play: Hydeland as the game wants */
    const BossBattleScene arena = {2, 0, 10, 0, -1, 0};
    CHECK(start_playable(ANCIENT_DRAGON, arena));
    boss_battle_boss_cleared(2);
    CHECK(boss_battle_skip_clear_screen());
    step_sets = 0;
    boss_battle_scene_hook(&regs);
    int32_t choice;
    memcpy(&choice, scene + 0x9314, sizeof(choice));
    CHECK(step_sets == 1 && step_level == 1 && step_value == BOSS_BATTLE_STEP_AFTER_CHOICE && choice == 1);
    boss_battle_scene_hook(&regs);
    CHECK(step_sets == 1);
    boss_battle_town_return_hook(&regs);
    CHECK(regs.rdx == 2 && regs.rcx == 0 && !strcmp((const char *)(uintptr_t)regs.r8, "Inn") &&
          boss_battle_state() == BOSS_BATTLE_RETURNING);
    boss_battle_set_screen_functions(NULL);
    boss_battle_reset();
}

static const uint64_t kOtherFeatureSites[] = {0x58e330, 0x53c22a, 0x53d064, 0x578000, 0x53d348, 0x53d38a, 0x86f00, 0x3cdbd9};
static const uint64_t kCalls[BOSS_BATTLE_INN_CALLS] = {0x3a4023, 0x3a4053, 0x3a4ca6, 0x3a5831, 0x3a5843, 0x3a5855,
                                                       0x3a623f, 0x3a624e, 0x3a625d, 0x3a62c9, 0x3a62e5};

static void test_us_image(void) {
    const size_t room = boss_battle_stub_bytes();
    DcpEarlyStorySites town;
    memset(&town, 0, sizeof(town));
    town.prompt_function = 0x369920;
    town.selector = 0x4763d0;
    town.town_portal_call = 0x3cdbd9;
    uint64_t size = 0;
    unsigned char *image = boot_image("v1.03", &size, room);
    if (!image) {
        puts("build/image/v1.03/boot.bin not found: real image checks skipped");
        return;
    }
    unsigned char *before = malloc(size + room);
    memcpy(before, image, size + room);
    const char *why = NULL;
    BossBattleSites sites;
    memset(&sites, 0, sizeof(sites));
    CHECK(boss_battle_build_listed("CUSA10454-1.00") && !boss_battle_build_listed("CUSA07001-1.00") &&
          !boss_battle_build_listed(NULL));
    CHECK(boss_battle_locate(image, size, "CUSA10454-1.00", &sites, &why) == 1);
    if (why && sites.hook == 0) printf("locate: %s\n", why);
    CHECK(sites.hook == 0x428948 && sites.request == 0x30c70 && sites.scene_vtable == 0xb8cf30);
    CHECK(sites.inn_built == 0x3a4501 && sites.inn_result == 0x3a62a0 && sites.inn_help == 0x3a652a &&
          sites.inn_help_table == 0x74df30);
    CHECK(sites.allocate == 0x56f340 && sites.widget == 0x480e20 && sites.add_widget == 0x481f50 &&
          sites.add_item == 0x4853c0 && sites.set_disabled == 0x485560 && sites.set_cursor == 0x482080);
    CHECK(sites.inn_party == 0x3ac840);
    for (int i = 0; i < BOSS_BATTLE_INN_CALLS; ++i) CHECK(sites.inn_calls[i] == kCalls[i] && image[kCalls[i]] == 0xe8);
    CHECK(sites.arrange_story == 0x45030b && sites.story_test == 0x477c20 && sites.story_clear == 0x4c4649);
    CHECK(sites.dragon_talismans == 0x10da5c && sites.title == 0x4af5ec);
    CHECK(sites.inn_flushed == 0x3a316f && sites.save_copy == 0x8cc6e && sites.play_data_table == 0xc1dc60);
    CHECK(sites.party_wiped == 0x429564 && sites.boss_cleared == 0xce806 && sites.clear_screen == 0x42e929 &&
          sites.results_screen == 0x42f678 && sites.town_return == 0x4300ef && sites.set_step == 0x58da30 &&
          sites.set_text == 0x58f0e0 && sites.set_message == 0x6b500);
    CHECK(sites.options_title == 0x507a91 && sites.options_built == 0x507aef && sites.options_update == 0x5074b7 &&
          sites.options_result == 0x5075a5 && sites.list_up == 0x482300 && sites.list_down == 0x4824a0 &&
          sites.options_body_vtable == 0xbb82b0 && sites.row_frame == 0x578f40);
    CHECK(sites.results_level_done == 0x4c3d7a && sites.set_level == 0x81fb0 && sites.level_pattern == 0x81fe0 &&
          sites.write_stats == 0x82000);
    CHECK(boss_battle_locate(image, size, NULL, &sites, &why) == 0 && boss_battle_locate(image, size, "CUSA00000-9.99", &sites, &why) == 0);

    const uint64_t probes[] = {
        0x428690, 0x428815, 0x428923, 0x428948, 0x4288dd, 0x42ddc0, 0x42de1e, 0x42e330, 0x30c70,  0x30ccb,  0x30ce9,
        0x30dda,  0x30e2a,  0x4aa70,  0x34520,  0xb177b0 + 0xc88, 0xb177b0 + 0x590, 0x3a442e, 0x3a4482, 0x3a44f7,
        0x3a4501, 0x3a6296, 0x3a62a0, 0x3a62a8, 0x3a6511, 0x3a652a, 0x483370, 0x483387, 0x4502e9, 0x45030b, 0x450331,
        0x477c20, 0x86ee3,  0x4c4634, 0x4c4649, 0x4c4667, 0x478530, 0x435dab, 0x435dbf, 0x435dc6, 0x3a4016, 0x3a4023,
        0x3a4044, 0x3a4c96, 0x3a5824, 0x3a5836, 0x3a5848, 0x3a6235, 0x3a6244, 0x3a6253, 0x3a62c1, 0x3a62d6, 0x3a62e5,
        0x485560, 0x482080, 0x10da1d, 0x10da5c, 0x476ed0, 0x4af5d9, 0x4af5ec, 0x53bd30, 0x3a2e20, 0x3a3153, 0x3a316f,
        0x8cc50,  0x8cc6e,  0x8c730,  0x7c8ce,  0x815a8,  0x815c1,  0xb177b0 + 0x7d0, 0x3ac534, 0x3ac54c, 0x3ac840,
        0x3ac939, 0x3ac95b, 0x3ac97f, 0x4b0c30, 0x4b0c40, 0x4294c3, 0x4294e3, 0x429564, 0x32350,  0x32372,
        0xce7c6,  0xce7d4,  0xce7fa,  0xce806,  0x2a0e90, 0x2a0eb5, 0x32290,  0x322b1,  0x753f2c + 6 * 4,
        0x753f2c + 21 * 4, 0x42e921, 0x42e929, 0x42e93d, 0x4af4b0, 0x4af4c6, 0x42f644, 0x42f678, 0x42f691, 0x4300c1,
        0x4300ef, 0x430104, 0x754282, 0xb8cf30 + 0xa0, 0x58da30, 0x3a6540, 0x3664d0, 0x3664ec, 0x366542, 0x366557,
        0x5910a0, 0x6b619,  0x591612, 0x58f0e0, 0x74db9c + 5 * 4, 0x3a64d2, 0x3a64d6, 0xb177b0 + 0x728, 0x3abf50,
        0x3abf6f, 0x507a5f, 0x507a83, 0x507a91, 0x507ae7, 0x507aef, 0x506730, 0x506745, 0xbb84a8 + 0xe0, 0x50710e,
        0x507250, 0x5074a2, 0x5074b7, 0x5074c7, 0x5075a2, 0x5075a5, 0xbb82b0 + 0xb8, 0x5032a9, 0xbb82b0 + 0x148,
        0xbb82b0 + 0x150, 0x482300, 0x4824a0, 0x578f40, 0xbb2720 + 0xe0, 0x4c39f0, 0x757b88 + 2 * 4, 0x4c3a1b,
        0x4c3cf0, 0x4c3d76, 0x4c3d7a, 0x4c3d81, 0x81fb0,  0x81fe0,  0x82000};
    for (size_t p = 0; p < sizeof(probes) / sizeof(*probes); ++p) {
        image[probes[p]] ^= 0x10;
        const int found = boss_battle_locate(image, size, "CUSA10454-1.00", &sites, &why);
        CHECK(found == 0);
        if (found) printf("  probe %#llx was not refused\n", (unsigned long long)probes[p]);
        image[probes[p]] ^= 0x10;
    }
    const char *mystic = "round/MysticRuins.brb";
    unsigned char *name = NULL;
    for (uint64_t at = 0; at + 22 < size && !name; ++at)
        if (!memcmp(image + at, mystic, 22)) name = image + at;
    CHECK(name != NULL);
    if (name) {
        name[6] ^= 0x20;
        CHECK(boss_battle_locate(image, size, "CUSA10454-1.00", &sites, &why) == 0 && !strncmp(why, "round table", 11));
        name[6] ^= 0x20;
    }
    CHECK(!memcmp(image, before, size + room));

    start_guarded();
    boss_battle_set_memory_guard(NULL, NULL);
    CHECK(boss_battle_install(image, size, 0, "CUSA10454-1.00", 1, &town, image + size, room) == 0 && !boss_battle_available());
    CHECK(boss_battle_install(image, size, 0, "CUSA10454-1.00", 0, NULL, image + size, room) == 0);
    CHECK(boss_battle_install(image, size, 0, "CUSA10454-1.00", 0, &town, image + size, room - 1) == 0);
    CHECK(!memcmp(image, before, size + room));

    PatchedSite all[22 + BOSS_BATTLE_INN_CALLS] = {
        {0x428948, 5}, {0x3a4501, 8}, {0x3a62a0, 5}, {0x3a652a, 5}, {0x45030b, 5}, {0x4c4649, 5}, {0x10da5c, 5},
        {0x4af5ec, 5}, {0x3a316f, 11}, {0x8cc6e, 5}, {0x429564, 5}, {0xce806, 5},  {0x42e929, 5}, {0x42f678, 5},
        {0x4300ef, 5}, {0x507a91, 5}, {0x507aef, 6}, {0x5074b7, 5}, {0x5075a5, 5}, {0x482300, 6}, {0x4824a0, 6},
        {0x4c3d7a, 10}};
    for (int i = 0; i < BOSS_BATTLE_INN_CALLS; ++i) all[22 + i] = (PatchedSite){kCalls[i], 5};
    const size_t sites_count = sizeof(all) / sizeof(*all);
    for (size_t i = 0; i < sites_count; ++i) {
        for (size_t j = 0; j < i; ++j)
            CHECK(all[i].at + all[i].length <= all[j].at || all[j].at + all[j].length <= all[i].at);
        for (size_t k = 0; k < sizeof(kOtherFeatureSites) / sizeof(*kOtherFeatureSites); ++k)
            CHECK(kOtherFeatureSites[k] + 16 <= all[i].at || all[i].at + all[i].length + 16 <= kOtherFeatureSites[k]);
    }
    CHECK(boss_battle_install(image, size, 0, "CUSA10454-1.00", 0, &town, image + size, room) == 1 && boss_battle_available());
    CHECK(bytes_changed_outside(image, before, size, all, sites_count) == 0);
    for (size_t i = 0; i < sites_count; ++i) CHECK(image[all[i].at] == 0xe9);
    CHECK(image[0x3cdbd9] == 0xe8 && !hydeland_town_installed()); /* the town call is not Boss Battle's */
    int32_t rel;
    memcpy(&rel, image + 0x428949, sizeof(rel));
    CHECK(0x428948 + 5 + (int64_t)rel == (int64_t)size);
    check_story_hooks(image);
    check_result_hooks(image);
    boss_battle_set_available(0);
    boss_battle_set_inn_functions(NULL);
    boss_battle_campaign_configure(0);
    printf("v1.03: %zu sites, Inn row after Report adventurers, %d of %d encounters\n", sites_count,
           boss_battle_startable_count(), BOSS_BATTLE_ENCOUNTERS);
    free(image), free(before);
}

static void test_other_images(void) {
    for (size_t i = 0; i < sizeof(kOtherBuilds) / sizeof(*kOtherBuilds); ++i) {
        uint64_t size = 0;
        unsigned char *image = boot_image(kOtherBuilds[i].folder, &size, 0);
        if (!image) continue;
        const char *why = NULL;
        BossBattleSites sites;
        CHECK(boss_battle_locate(image, size, kOtherBuilds[i].game, &sites, &why) == 0 && !strcmp(why, "not a listed build"));
        printf("%s: not listed, Boss Battle unavailable\n", kOtherBuilds[i].folder);
        free(image);
    }
}

int main(void) {
    char user_dir[1100];
    snprintf(user_dir, sizeof(user_dir), "%s/build/boss-battle-tests", DCP_SOURCE_DIR);
    char record_file[1200];
    snprintf(record_file, sizeof(record_file), "%s/boss_battle.dat", user_dir);
#ifdef _WIN32
    _mkdir(user_dir);
#else
    mkdir(user_dir, 0755);
#endif
    remove(record_file);
    boss_battle_record_configure(user_dir);
    boss_battle_progress_configure(user_dir);
    test_encounters();
    test_arena_rooms();
    test_shared_hook();
    test_setting();
    test_session_origin();
    test_milestones();
    test_win();
    test_win_signals();
    test_stage_end_kinds();
    test_defeat();
    test_progression(user_dir);
    test_result_bypass();
    test_death();
    test_quit();
    test_quit_key();
    test_unresolved();
    test_return();
    test_guard_failures();
    test_telemetry(user_dir);
    test_inn_rows();
    test_inn_build();
    test_inn_results();
    test_screen_rows();
    test_screen_statistics();
    test_screen_hooks();
    test_inn_help();
    test_inn_row_calls();
    test_outcome_hooks();
    test_campaign_restore();
    test_campaign_session();
    test_us_image();
    test_other_images();
    remove(record_file);
    if (failures) {
        printf("%d Boss Battle check(s) failed\n", failures);
        return 1;
    }
    puts("Boss Battle tests passed");
    return 0;
}
