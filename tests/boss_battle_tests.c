/* Focused tests of Boss Battle: the encounter table, the setting, the native boss list on the Inn's own list, the
 * session's origin and lifecycle (Arena loaded, Player spawned, Party ready, Encounter ready, Playable), the Attempt,
 * Win (native boss clear and stage clear), Death and Quit rules, the stage-end act bypass, the return to the Inn with
 * the campaign put back, the help-line statistics, the capped telemetry, the special bosses' hooks, and the site
 * checks and install on the local boot images under build/image when they are there. */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boss_battle.h"
#include "boss_battle_online.h"
#include "early_story_online.h"
#include "network_access.h"
#include "network_hint.h"
#include "boss_battle_text.h"
#include "boss_battle_record.h"
#include "exit_game_text.h"
#include "hydeland_town.h"
#include "inn_layout.h"

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
/* As the English game names them: the Demon King's room holds the DemonLord object, ObjectParam 81 "Demon Lord". */
static const char *const kShownNames[BOSS_BATTLE_ENCOUNTERS] = {
    "Harpy",  "Wyvern", "Doom Beetle",   "Golem",  "Gazer",   "Pirates", "Minotaur",      "Vampire",    "Warlock", "Medusa",
    "Wraith", "Killer Rabbit", "Kraken", "Cyclops", "Chimera", "Gargoyle Gate", "Red Dragon", "Arch Demon",
    "Ancient Dragon", "Demon Lord",
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

enum { HARPY = 0, GOLEM = 3, MEDUSA = 9, WRAITH = 10, ANCIENT_DRAGON = 18, DEMON_KING = 19 };

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
        snprintf(expected, sizeof(expected), "%02d %s", i + 1, kShownNames[i]);
        CHECK(boss_battle_label(i, label, sizeof(label)) && !strcmp(label, expected));
        for (int j = 0; j < i; ++j) CHECK(strcmp(e->id, boss_battle_encounter(j)->id));
        CHECK(e->round_index >= 0 && e->round_index < (int)(sizeof(kRounds) / sizeof(*kRounds)));
        CHECK(kRounds[e->round_index] && !strcmp(kRounds[e->round_index], e->round));
        CHECK(e->stage >= 0 && e->last_stage >= e->stage && e->stage_name && *e->stage_name);
        CHECK(!e->unavailable);
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
    boss_battle_set_character("Zak", 0, 1);
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

    /* a player of an online start left while still travelling: cancelled at once, and the Inn asked for as soon as the
     * scene can leave, with nothing recorded */
    start_guarded();
    before = harpy_stats();
    CHECK(start_session(HARPY));
    CHECK(boss_battle_request_quit(BOSS_BATTLE_QUIT_PEER_LOST) == 1);
    frames(probe(kInnScene, 0, 0, 0), 1);
    CHECK(boss_battle_state() != BOSS_BATTLE_TRAVELLING && boss_battle_outcome() == BOSS_BATTLE_OUTCOME_CANCELLED);
    frames(probe(kLoading, 0, 0, 0), 2);
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
    CHECK(count_in(path, "WAITING player record") == 6);
    CHECK(count_in(path, "heartbeat") == 24);
    CHECK(count_in(path, "]: scene act") == 96);
    remove(path);
}

/* ---- the Inn menu with the inserted row, and the boss list on the same list ---- */

static void test_inn_rows(void) {
    static const int kNative[BOSS_BATTLE_INN_ROWS] = {0, 1, 2, BOSS_BATTLE_INN_NO_ROW, 3, 4, 5};
    CHECK(BOSS_BATTLE_INN_ROW == 3 && BOSS_BATTLE_INN_ROWS == 7);
    CHECK(boss_battle_inn_native_row(4) == 3 && boss_battle_inn_native_row(5) == 4 && boss_battle_inn_native_row(6) == 5);
    for (int row = 0; row < BOSS_BATTLE_INN_ROWS; ++row) CHECK(boss_battle_inn_native_row(row) == kNative[row]);
    CHECK(boss_battle_inn_native_row(-2) == -2 && boss_battle_inn_native_row(-1) == -1);
    for (int native = 0; native < BOSS_BATTLE_INN_ROWS - 1; ++native) {
        const int shown = boss_battle_inn_display_row(native);
        CHECK(shown != BOSS_BATTLE_INN_ROW && boss_battle_inn_native_row(shown) == native);
    }
}

enum { INN_LIST = 0x3ba0, INN_SIZE = 0x5000, ITEM_SIZE = 0x30, MAX_ITEMS = 32, WIDGET_BYTES = 0x5c0, POOL = 24 };
static uint8_t fake_inn[INN_SIZE], fake_inn_context[0x100], fake_items[MAX_ITEMS * ITEM_SIZE];
static uint64_t fake_widgets[8];
static uint8_t fake_new_widget[WIDGET_BYTES], fake_inn_rows[6][WIDGET_BYTES];
static uint8_t widget_pool[POOL][WIDGET_BYTES], fake_list_manager[0x200];
static uint64_t fake_list_vtable[0x200 / 8], fake_widget_vtable[0x100 / 8];
static int pool_used, widgets_added, items_added, disabled_calls, disabled_row = -1, disabled_flag = -1;
static int cursor_calls, cursor_row = -1, party_sends;

/* What the custom Inn menu asked of the game's list and row primitives. */
typedef struct {
    uint64_t widget;
    float y, cell_height;
    int32_t height;
    uint8_t state;
} TextDraw;
typedef struct {
    uint64_t widget;
    int y, height;
    float scale;
} ArtDraw;
static struct {
    int titles, backgrounds, scroll_bars, helps, hovers, deletes, applies, texts, arts;
    TextDraw text[16];
    ArtDraw art[16];
    uint64_t applied[8];
    int applied_item[8];
} menu_calls;

static int pooled(uint64_t widget) {
    return widget >= (uint64_t)(uintptr_t)widget_pool && widget < (uint64_t)(uintptr_t)widget_pool + sizeof(widget_pool);
}

static void GUEST_HOOK_ABI fake_widget_delete(uint64_t widget) {
    (void)widget;
    ++menu_calls.deletes;
}
static void GUEST_HOOK_ABI fake_widget_noop(uint64_t widget) {
    (void)widget;
}
static void GUEST_HOOK_ABI fake_widget_frame(uint64_t widget, int on) {
    (void)widget, (void)on;
}
static void GUEST_HOOK_ABI fake_widget_draw(uint64_t widget) {
    if (menu_calls.texts >= 16) return;
    TextDraw *draw = &menu_calls.text[menu_calls.texts++];
    const uint8_t *w = (const uint8_t *)(uintptr_t)widget;
    draw->widget = widget;
    memcpy(&draw->y, w + 0x34, sizeof(draw->y));
    memcpy(&draw->cell_height, w + 0x35c, sizeof(draw->cell_height));
    memcpy(&draw->height, w + 0x354, sizeof(draw->height));
    draw->state = w[0x360];
}
static void GUEST_HOOK_ABI fake_list_title(uint64_t list) {
    (void)list;
    ++menu_calls.titles;
}
static void GUEST_HOOK_ABI fake_list_background(uint64_t list) {
    (void)list;
    ++menu_calls.backgrounds;
}
static void GUEST_HOOK_ABI fake_list_scroll_bar(uint64_t list) {
    (void)list;
    ++menu_calls.scroll_bars;
}
static uint64_t count_of(int begin, int end, int stride);
static int GUEST_HOOK_ABI fake_list_count(uint64_t list) {
    (void)list;
    return (int)count_of(0xc78, 0xc80, ITEM_SIZE);
}
static void GUEST_HOOK_ABI fake_list_cursor_art(uint64_t list, uint64_t widget, int x, int y, int width, int height) {
    (void)x, (void)width;
    if (menu_calls.arts >= 16) return;
    ArtDraw *art = &menu_calls.art[menu_calls.arts++];
    art->widget = widget;
    art->y = y;
    art->height = height;
    memcpy(&art->scale, (const uint8_t *)(uintptr_t)list + 0xe0 + 0x90, sizeof(art->scale));
}
static void GUEST_HOOK_ABI fake_list_help(uint64_t list, uint64_t widget, int x, int y, int width, int height) {
    (void)list, (void)widget, (void)x, (void)y, (void)width, (void)height;
    ++menu_calls.helps;
}
static void GUEST_HOOK_ABI fake_list_hover(uint64_t list, uint64_t widget, int x, int y, int width, int height) {
    (void)list, (void)widget, (void)x, (void)y, (void)width, (void)height;
    ++menu_calls.hovers;
}
static void GUEST_HOOK_ABI fake_list_apply(uint64_t list, uint64_t widget, int item) {
    (void)list;
    if (menu_calls.applies < 8) {
        menu_calls.applied[menu_calls.applies] = widget;
        menu_calls.applied_item[menu_calls.applies] = item;
    }
    ++menu_calls.applies;
}

static void set_slot(uint64_t *vtable, int slot, void *function) {
    vtable[slot / 8] = (uint64_t)(uintptr_t)function;
}

static void set_fake_vtables(void) {
    set_slot(fake_widget_vtable, 0x08, (void *)fake_widget_delete);
    set_slot(fake_widget_vtable, 0x88, (void *)fake_widget_noop);
    set_slot(fake_widget_vtable, 0x90, (void *)fake_widget_noop);
    set_slot(fake_widget_vtable, 0x98, (void *)fake_widget_noop);
    set_slot(fake_widget_vtable, 0xa8, (void *)fake_widget_frame);
    set_slot(fake_widget_vtable, 0xb0, (void *)fake_widget_draw);
    set_slot(fake_widget_vtable, 0xb8, (void *)fake_widget_noop);
    set_slot(fake_list_vtable, 0xb8, (void *)fake_list_apply);
    set_slot(fake_list_vtable, 0xd8, (void *)fake_list_count);
    set_slot(fake_list_vtable, 0x190, (void *)fake_list_scroll_bar);
    set_slot(fake_list_vtable, 0x198, (void *)fake_list_background);
    set_slot(fake_list_vtable, 0x1a0, (void *)fake_list_title);
    set_slot(fake_list_vtable, 0x1a8, (void *)fake_list_cursor_art);
    set_slot(fake_list_vtable, 0x1b0, (void *)fake_list_help);
    set_slot(fake_list_vtable, 0x1b8, (void *)fake_list_hover);
}

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
    if (size != WIDGET_BYTES || pool_used >= POOL) return 0;
    memset(widget_pool[pool_used], 0, WIDGET_BYTES);
    return (uint64_t)(uintptr_t)widget_pool[pool_used++];
}
static void GUEST_HOOK_ABI fake_widget(uint64_t widget, uint64_t manager) {
    (void)manager;
    put_pointer((uint8_t *)(uintptr_t)widget, fake_widget_vtable);
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
static char party_guard[32];
static void GUEST_HOOK_ABI fake_inn_party(uint64_t inn) {
    (void)inn;
    ++party_sends;
    snprintf(party_guard, sizeof(party_guard), "%s", guard_log);
}

static void put_f32_at(uint8_t *where, float value) {
    memcpy(where, &value, sizeof(value));
}

static void put_i32_at(uint8_t *where, int32_t value) {
    memcpy(where, &value, sizeof(value));
}

/* The vanilla Inn list (x -520, first row at y -155, 200 x 50 cells, its title offset to the side) and its row widgets
 * (200 x 50, no margins, the colour and state the game gives them). */
static void set_native_geometry(void) {
    uint8_t *list = inn_list();
    put_pointer(list, fake_list_vtable);
    put_pointer(list + 0x28, fake_list_manager);
    put_f32_at(list + 0x30, -520.0f);
    put_f32_at(list + 0x34, -155.0f);
    put_pointer(list + 0x58, fake_new_widget);
    put_i32_at(list + 0x9c, 1);
    put_f32_at(list + 0xa0, 200.0f);
    put_f32_at(list + 0xa4, 50.0f);
    put_f32_at(list + 0xd4, -585.0f);
    put_f32_at(list + 0xd8, -345.0f);
    put_f32_at(list + 0xe0 + 0x90, 1.0f);
    put_f32_at(list + 0x3b0 + 0x90, 1.0f);
    put_i32_at(list + 0x95c, -1);
    put_i32_at(list + 0x964, 0x10000000);
    list[0x47] = 0xff;
    for (int i = 0; i < 6; ++i) {
        put_i32_at(fake_inn_rows[i] + 0x350, 200);
        put_i32_at(fake_inn_rows[i] + 0x354, 50);
        put_i32_at(fake_inn_rows[i] + 0x44, 0x80ffffff);
        fake_inn_rows[i][0x360] = i == 1 ? 0x2 : 0;
    }
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
    memset(fake_inn_rows, 0, sizeof(fake_inn_rows));
    for (unsigned i = 0; i < 8; ++i)
        fake_widgets[i] = (uint64_t)(uintptr_t)(i < 6 ? fake_inn_rows[i] : fake_new_widget);
    put_pointer(inn_list() + 0x68, fake_widgets);
    put_pointer(inn_list() + 0x70, fake_widgets + widgets);
    set_native_geometry();
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
    set_fake_vtables();
    GuestRegs gone;
    memset(&gone, 0, sizeof(gone));
    gone.rdi = (uint64_t)(uintptr_t)fake_inn;
    boss_battle_inn_destroyed_hook(&gone);
    memset(&menu_calls, 0, sizeof(menu_calls));
    pool_used = widgets_added = items_added = disabled_calls = cursor_calls = party_sends = 0;
    party_guard[0] = 0;
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
    CHECK(count_of(0xc78, 0xc80, ITEM_SIZE) == 6 && !widgets_added && !items_added && !pool_used);
    boss_battle_set_enabled(1);
    set_inn(5, 5, -1, 0);
    regs = inn_regs();
    boss_battle_inn_built_hook(&regs);
    CHECK(count_of(0xc78, 0xc80, ITEM_SIZE) == 5 && !items_added && !pool_used);
    set_inn(6, 6, -1, 0);
    regs = inn_regs();
    boss_battle_inn_built_hook(&regs);
    CHECK(widgets_added == 1 && items_added == 1 && main_rows_are_inn() && count_of(0x68, 0x70, 8) == 7);
    boss_battle_reset();
}

static void test_inn_layout(void) {
    BossBattleInnLayout layout;
    CHECK(boss_battle_inn_layout(-520.0f, 200.0f, 50.0f, -130.0f, 120.0f, &layout) == 1);
    CHECK(layout.center[0] == -130.0f && layout.center[BOSS_BATTLE_INN_ROWS - 1] == 120.0f);
    CHECK(layout.row_height == 40.0f && layout.art_scale == 0.8f && layout.x == -520.0f && layout.width == 200.0f);
    CHECK(layout.pitch > 41.66f && layout.pitch < 41.67f);
    for (int row = 0; row + 1 < BOSS_BATTLE_INN_ROWS; ++row) {
        const float step = layout.center[row + 1] - layout.center[row];
        CHECK(step > 41.66f && step < 41.67f);
        CHECK(boss_battle_inn_row_top(&layout, row) + layout.row_height < boss_battle_inn_row_top(&layout, row + 1));
    }
    CHECK(boss_battle_inn_layout(-520.0f, 200.0f, 50.0f, -130.0f, -125.0f, &layout) == 0);
    CHECK(boss_battle_inn_layout(-520.0f, 200.0f, 30.0f, -130.0f, 120.0f, &layout) == 0);
}

static void reset_frame_calls(void) {
    menu_calls.titles = menu_calls.backgrounds = menu_calls.scroll_bars = menu_calls.helps = menu_calls.hovers = 0;
    menu_calls.texts = menu_calls.arts = 0;
}

static GuestRegs draw_frame(uint8_t *list) {
    GuestRegs regs = inn_regs();
    regs.rdi = (uint64_t)(uintptr_t)list;
    reset_frame_calls();
    boss_battle_inn_draw_hook(&regs);
    return regs;
}

static void build_custom_menu(void) {
    start_guarded();
    use_fake_inn_functions();
    boss_battle_set_enabled(1);
    set_inn(6, 6, -1, 0);
    GuestRegs regs = inn_regs();
    boss_battle_inn_built_hook(&regs);
}

static int32_t list_i32(int offset) {
    int32_t value;
    memcpy(&value, inn_list() + offset, sizeof(value));
    return value;
}

static float list_f32(int offset) {
    float value;
    memcpy(&value, inn_list() + offset, sizeof(value));
    return value;
}

static int selected_rows(int *selected) {
    int count = 0;
    for (int i = 0; i < menu_calls.texts; ++i)
        if (menu_calls.text[i].state & 0x8) ++count, *selected = i;
    return count;
}

/* Boss Battle off: the Inn menu is the game's own, drawn by the game. */
static void test_inn_menu_off(void) {
    start_guarded();
    use_fake_inn_functions();
    boss_battle_set_enabled(0);
    set_inn(6, 6, -1, 0);
    GuestRegs regs = inn_regs();
    boss_battle_inn_built_hook(&regs);
    static uint8_t before[INN_SIZE];
    memcpy(before, fake_inn, sizeof(before));
    regs = draw_frame(inn_list());
    CHECK(!regs.skip && !pool_used && !menu_calls.applies && !menu_calls.texts && !menu_calls.titles && !menu_calls.arts);
    CHECK(!memcmp(before, fake_inn, sizeof(before)));
    boss_battle_reset();
}

/* Boss Battle on: seven rows of the game's row class, named from the list's own items, Boss Battle fourth. */
static void test_inn_menu_built(void) {
    build_custom_menu();
    CHECK(pool_used == BOSS_BATTLE_INN_ROWS + 1 && widgets_added == 1 && items_added == 1 && main_rows_are_inn());
    CHECK(menu_calls.applies == BOSS_BATTLE_INN_ROWS);
    for (int row = 0; row < BOSS_BATTLE_INN_ROWS; ++row) {
        const uint8_t *widget = widget_pool[row];
        int32_t width, height;
        uint64_t owner;
        memcpy(&width, widget + 0x350, sizeof(width));
        memcpy(&height, widget + 0x354, sizeof(height));
        memcpy(&owner, widget + 0x348, sizeof(owner));
        CHECK(menu_calls.applied[row] == (uint64_t)(uintptr_t)widget && menu_calls.applied_item[row] == row);
        CHECK(width == 200 && height == 40 && owner == (uint64_t)(uintptr_t)inn_list());
    }
    CHECK(item_tag(menu_calls.applied_item[BOSS_BATTLE_INN_ROW]) == BOSS_BATTLE_INN_ROW_RECORD);
    CHECK(boss_battle_inn_native_row(4) == 3 && boss_battle_inn_native_row(5) == 4 && boss_battle_inn_native_row(6) == 5);
    boss_battle_reset();
}

/* Each frame: the game's title, then the seven custom rows inside the vanilla envelope, one highlighted row with the
 * native cursor art at the row's height, and nothing of the Inn changed. */
static void test_inn_menu_draw(void) {
    build_custom_menu();
    static uint8_t before[INN_SIZE];
    static const int kCursors[] = {0, 1, 2, 3, 4, 5, 6, 0, 6, 5};
    for (size_t c = 0; c < sizeof(kCursors) / sizeof(*kCursors); ++c) {
        const int cursor = kCursors[c];
        put_i32_at(inn_list() + 0x958, cursor);
        memcpy(before, fake_inn, sizeof(before));
        const GuestRegs regs = draw_frame(inn_list());
        CHECK(regs.skip && menu_calls.texts == BOSS_BATTLE_INN_ROWS && menu_calls.titles == 1);
        CHECK(!menu_calls.backgrounds && menu_calls.scroll_bars == 1 && menu_calls.helps == 1 && !menu_calls.hovers);
        CHECK(!memcmp(before, fake_inn, sizeof(before)));
        int selected = -1;
        CHECK(selected_rows(&selected) == 1 && selected == cursor);
        CHECK(menu_calls.arts == 1 && menu_calls.art[0].widget == menu_calls.text[cursor].widget);
        CHECK(menu_calls.art[0].scale == 0.8f && menu_calls.art[0].height == 40 && list_f32(0xe0 + 0x90) == 1.0f);
        CHECK(menu_calls.art[0].y == (int)menu_calls.text[cursor].y);
        for (int row = 0; row < BOSS_BATTLE_INN_ROWS; ++row) {
            const TextDraw *text = &menu_calls.text[row];
            CHECK(text->widget == (uint64_t)(uintptr_t)widget_pool[row] && pooled(text->widget));
            CHECK(text->height == 40 && text->cell_height == 40.0f);
        }
    }
    const TextDraw *text = menu_calls.text;
    CHECK(text[0].y + (float)(text[0].height / 2) == -130.0f);
    CHECK(text[6].y + (float)(text[6].height / 2) == 120.0f);
    for (int row = 0; row + 1 < BOSS_BATTLE_INN_ROWS; ++row) {
        const float step = text[row + 1].y - text[row].y;
        CHECK(step > 41.66f && step < 41.67f && text[row].y + text[row].cell_height < text[row + 1].y);
    }
    CHECK(text[1].state & 0x2);
    uint32_t colour;
    memcpy(&colour, widget_pool[0] + 0x44, sizeof(colour));
    CHECK(colour == 0x80ffffff);
    boss_battle_reset();
}

/* The native list's flags keep their meaning: an open submenu hides the cursor art, a decided row blinks, the result
 * latch counts down, a hidden list draws nothing; other lists are never drawn by the custom menu. */
static void test_inn_menu_states(void) {
    build_custom_menu();
    put_i32_at(inn_list() + 0x958, 2);
    put_i32_at(inn_list() + 0x964, 0x10000000 | 0x8);
    GuestRegs regs = draw_frame(inn_list());
    int selected = -1;
    CHECK(regs.skip && !menu_calls.arts && menu_calls.texts == 7 && selected_rows(&selected) == 1 && selected == 2);

    put_i32_at(inn_list() + 0x964, 0x10000000 | 0x10000);
    fake_list_manager[0x140] = 0x2;
    regs = draw_frame(inn_list());
    CHECK(regs.skip && menu_calls.texts == 6);
    fake_list_manager[0x140] = 0;
    regs = draw_frame(inn_list());
    CHECK(menu_calls.texts == 7);

    put_i32_at(inn_list() + 0x964, 0x10000000 | 0x800);
    put_i32_at(inn_list() + 0x960, 2);
    put_i32_at(inn_list() + 0xb8, 3);
    draw_frame(inn_list());
    CHECK(list_i32(0xb8) == 2);
    put_i32_at(inn_list() + 0x960, -1);
    draw_frame(inn_list());
    CHECK(list_i32(0xb8) == 2);

    put_i32_at(inn_list() + 0x964, 0x10000000 | 0x400);
    regs = draw_frame(inn_list());
    CHECK(regs.skip && !menu_calls.texts && !menu_calls.titles);

    put_i32_at(inn_list() + 0x964, 0x10000000);
    regs = draw_frame(fake_inn + 0x4e10);
    CHECK(!regs.skip && !menu_calls.texts && !menu_calls.titles);
    boss_battle_reset();
}

/* The rows belong to their Inn: deleted with it, made once per build, never doubled. */
static void test_inn_menu_lifetime(void) {
    build_custom_menu();
    GuestRegs gone;
    memset(&gone, 0, sizeof(gone));
    gone.rdi = (uint64_t)(uintptr_t)fake_inn + 0x10;
    boss_battle_inn_destroyed_hook(&gone);
    CHECK(!menu_calls.deletes && draw_frame(inn_list()).skip);

    GuestRegs again = inn_regs();
    boss_battle_inn_built_hook(&again);
    CHECK(pool_used == BOSS_BATTLE_INN_ROWS + 1 && draw_frame(inn_list()).skip && menu_calls.texts == 7);

    gone.rdi = (uint64_t)(uintptr_t)fake_inn;
    boss_battle_inn_destroyed_hook(&gone);
    CHECK(menu_calls.deletes == BOSS_BATTLE_INN_ROWS && !draw_frame(inn_list()).skip && !menu_calls.texts);

    set_inn(6, 6, -1, 0);
    GuestRegs regs = inn_regs();
    boss_battle_inn_built_hook(&regs);
    CHECK(pool_used == 2 * (BOSS_BATTLE_INN_ROWS + 1) && menu_calls.applies == 2 * BOSS_BATTLE_INN_ROWS);
    regs = draw_frame(inn_list());
    CHECK(regs.skip && menu_calls.texts == BOSS_BATTLE_INN_ROWS && menu_calls.deletes == BOSS_BATTLE_INN_ROWS);
    for (int row = 0; row < BOSS_BATTLE_INN_ROWS; ++row)
        CHECK(menu_calls.text[row].widget == (uint64_t)(uintptr_t)widget_pool[BOSS_BATTLE_INN_ROWS + 1 + row]);

    boss_battle_inn_destroyed_hook(&gone);
    CHECK(menu_calls.deletes == 2 * BOSS_BATTLE_INN_ROWS);
    boss_battle_reset();
}

static float absf(float value) {
    return value < 0.0f ? -value : value;
}

/* ---- the Inn background's seven rows on the shared menu layout ---- */

enum {
    MBS_PARTS = 0xf8,
    MBS_LAYERS = 0x100,
    MBS_KEYFRAMES = 0xf0,
    MBS_TRANSFORMS = 0xe8,
    MBS_PART_COUNT = 471,
    INN_E0 = 57,
    INN_1 = 3,
    ROW_ELEMENT = 123,
    LIT_ELEMENT = 122,
    FLASH_ELEMENT = 135
};

typedef struct {
    uint8_t *data, *pristine;
    long size;
} MenuLayout;

/* The layout as the game holds it once loaded: the file in place, its table offsets turned into addresses and the
 * header marked as relocated; `relocate` 0 keeps the file as it is on disk. */
static int read_layout_at(const char *folder, const char *name, MenuLayout *layout, int relocate) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/build/research/archaeology/res/%s/stage/%s", DCP_SOURCE_DIR, folder, name);
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    fseek(file, 0, SEEK_END);
    layout->size = ftell(file);
    fseek(file, 0, SEEK_SET);
    layout->data = malloc((size_t)layout->size);
    layout->pristine = malloc((size_t)layout->size);
    const size_t read = fread(layout->data, 1, (size_t)layout->size, file);
    fclose(file);
    if (read != (size_t)layout->size) return 0;
    for (int i = 0; relocate && i < 11; ++i) {
        uint64_t offset;
        memcpy(&offset, layout->data + 0xb0 + 8 * i, sizeof(offset));
        put_pointer(layout->data + 0xb0 + 8 * i, offset ? layout->data + offset : NULL);
    }
    if (relocate) layout->data[0x17] |= 0x04;
    memcpy(layout->pristine, layout->data, (size_t)layout->size);
    return 1;
}

static int read_layout(const char *name, MenuLayout *layout, int relocate) {
    return read_layout_at("us100", name, layout, relocate);
}

static int load_menu_layout(const char *name, MenuLayout *layout) {
    return read_layout(name, layout, 1);
}

/* A copy of the pristine layout whose tables are its own. */
static uint8_t *clone_layout(const MenuLayout *layout) {
    uint8_t *copy = malloc((size_t)layout->size);
    memcpy(copy, layout->pristine, (size_t)layout->size);
    for (int i = 0; i < 11; ++i) {
        uint64_t at;
        memcpy(&at, copy + 0xb0 + 8 * i, sizeof(at));
        if (at) put_pointer(copy + 0xb0 + 8 * i, copy + (at - (uint64_t)(uintptr_t)layout->data));
    }
    return copy;
}

static const uint8_t *mbs_table(const uint8_t *layout, int at) {
    uint64_t value;
    memcpy(&value, layout + at, sizeof(value));
    return (const uint8_t *)(uintptr_t)value;
}

static uint16_t mbs_u16(const uint8_t *at) {
    uint16_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

static float mbs_f32(const uint8_t *at) {
    float value;
    memcpy(&value, at, sizeof(value));
    return value;
}

typedef struct {
    uint16_t first;
    uint8_t count, master;
} PartView;

static PartView part_view(const uint8_t *layout, int part) {
    const uint8_t *record = mbs_table(layout, MBS_PARTS) + part * 48;
    return (PartView){mbs_u16(record + 0x28), record[0x2a], record[0x2b]};
}

typedef struct {
    uint32_t element;
    float x, y, scale_y;
    const uint8_t *keyframe, *transform;
} KeyView;

static int layer_keys(const uint8_t *layout, int layer, KeyView *keys, int max) {
    const uint8_t *record = mbs_table(layout, MBS_LAYERS) + layer * 24;
    const int start = mbs_u16(record), count = mbs_u16(record + 2);
    for (int k = 0; k < count && k < max; ++k) {
        const uint8_t *keyframe = mbs_table(layout, MBS_KEYFRAMES) + (start + k) * 32;
        const uint8_t *transform = mbs_table(layout, MBS_TRANSFORMS) + mbs_u16(keyframe + 4) * 36;
        uint32_t element;
        memcpy(&element, keyframe, sizeof(element));
        keys[k] = (KeyView){element, mbs_f32(transform), mbs_f32(transform + 4), mbs_f32(transform + 28), keyframe, transform};
    }
    return count < max ? count : max;
}

/* The y of a part's first key showing `element` on the menu column, or a value off every row. */
static float element_y(const uint8_t *layout, int part, uint32_t element) {
    const PartView view = part_view(layout, part);
    for (int at = 0; at < view.count; ++at) {
        KeyView keys[16];
        const int n = layer_keys(layout, view.first + at, keys, 16);
        for (int k = 0; k < n; ++k)
            if (keys[k].element == element && keys[k].x == -420.0f) return keys[k].y;
    }
    return 9999.0f;
}

static void custom_centers(float centers[BOSS_BATTLE_INN_ROWS]) {
    BossBattleInnLayout layout;
    CHECK(boss_battle_inn_layout(-520.0f, 200.0f, 50.0f, -130.0f, 120.0f, &layout));
    memcpy(centers, layout.center, sizeof(layout.center));
}

static int expected_template(int row, int decided) {
    if (row < 3) return row;
    if (row > 3) return row - 1;
    return decided == 3 ? 2 : 3;
}

static int row_of(const uint8_t *layout, int layer, const float rows[6]) {
    KeyView keys[16];
    const int n = layer_keys(layout, layer, keys, 16);
    for (int k = 0; k < n; ++k)
        for (int r = 0; r < 6; ++r)
            if (keys[k].x == -420.0f && keys[k].y == rows[r]) return r;
    return -1;
}

/* Each custom row is its native row's layer, keys moved to the custom centre and backings 0.8 as tall; every other
 * layer of the part is the native one, in the native order. */
static void check_seven_row_part(const MenuLayout *menu, int part, int first_row, const float centers[7], const float rows[6]) {
    const PartView before = part_view(menu->pristine, part), after = part_view(menu->data, part);
    CHECK(after.count == before.count + 1);
    const int run = first_row - before.first;
    int decided = -1, decided_at = -1;
    for (int at = run + 6; at < before.count; ++at) {
        const int row = row_of(menu->pristine, before.first + at, rows);
        if (row >= 0) decided = row, decided_at = at;
    }
    int native_of_row[6];
    for (int at = 0; at < 6; ++at) native_of_row[row_of(menu->pristine, first_row + at, rows)] = first_row + at;
    for (int row = 0; row < 7; ++row) {
        const int from = expected_template(row, decided);
        KeyView old_keys[16], new_keys[16];
        const int n = layer_keys(menu->pristine, native_of_row[from], old_keys, 16);
        CHECK(layer_keys(menu->data, after.first + run + row, new_keys, 16) == n);
        int on_centre = 0;
        for (int k = 0; k < n; ++k) {
            CHECK(new_keys[k].element == old_keys[k].element && new_keys[k].x == old_keys[k].x);
            CHECK(absf(new_keys[k].y - (old_keys[k].y + centers[row] - rows[from])) < 0.001f);
            const float scale = old_keys[k].x == -420.0f ? 0.8f : 1.0f;
            CHECK(absf(new_keys[k].scale_y - old_keys[k].scale_y * scale) < 0.0001f);
            CHECK(!memcmp(new_keys[k].keyframe, old_keys[k].keyframe, 4) && !memcmp(new_keys[k].keyframe + 6, old_keys[k].keyframe + 6, 26));
            on_centre |= old_keys[k].x == -420.0f && new_keys[k].y == centers[row];
        }
        CHECK(on_centre);
    }
    for (int at = 0; at < before.count; ++at) {
        if (at >= run && at < run + 6) continue;
        const int to = at < run ? at : at + 1;
        const uint8_t *old_record = mbs_table(menu->pristine, MBS_LAYERS) + (before.first + at) * 24;
        const uint8_t *new_record = mbs_table(menu->data, MBS_LAYERS) + (after.first + to) * 24;
        if (at == decided_at) {
            KeyView keys[16];
            const int n = layer_keys(menu->data, after.first + to, keys, 16);
            int lit = 0;
            for (int k = 0; k < n; ++k) lit |= keys[k].x == -420.0f && keys[k].y == centers[decided < 3 ? decided : decided + 1];
            CHECK(lit);
        } else {
            CHECK(!memcmp(old_record, new_record, 24));
        }
    }
    for (int at = 0; at < after.count; ++at) {
        KeyView keys[16];
        const int n = layer_keys(menu->data, after.first + at, keys, 16);
        for (int k = 0; k < n; ++k)
            for (int r = 1; r < 5; ++r) CHECK(!(keys[k].x == -420.0f && keys[k].y == rows[r]));
    }
}

static void test_inn_background_layout(void) {
    static MenuLayout menu;
    if (!menu.data && !load_menu_layout("Menu.mbs", &menu)) {
        puts("build/research/.../Menu.mbs not found: Inn background layout checks skipped");
        return;
    }
    static const char *const kParts[INN_LAYOUT_PARTS] = {"INN_0", "INN_1", "INN_2", "INN_A0", "INN_C0", "INN_C2",
                                                           "INN_D0", "INN_D2", "INN_E0", "INN_E1", "INN_F0", "INN_G0",
                                                           "INN_G0_1", "INN_G2", "INN_G2_1", "INN_H0"};
    static const int kRows[6] = {-130, -80, -30, 20, 70, 120};
    InnLayoutScan scan;
    CHECK(inn_layout_scan(menu.data, &scan) == 1 && scan.count == INN_LAYOUT_PARTS);
    for (int r = 0; r < 6; ++r) CHECK(scan.rows[r] == (float)kRows[r]);
    for (int i = 0; i < INN_LAYOUT_PARTS; ++i)
        CHECK(!strcmp((const char *)mbs_table(menu.data, MBS_PARTS) + scan.part[i] * 48 + 0x10, kParts[i]));
    CHECK(scan.part[1] == INN_1 && scan.first_row_layer[1] == 17);
    for (int r = 0; r < 6; ++r) {
        KeyView key;
        CHECK(layer_keys(menu.data, 17 + r, &key, 1) == 1 && key.element == ROW_ELEMENT && key.x == -420.0f &&
              key.y == (float)kRows[r] && key.scale_y == 0.9f);
    }

    float centers[7];
    custom_centers(centers);
    CHECK(inn_layout_apply(menu.data, centers, 0.8f) == INN_LAYOUT_APPLIED && inn_layout_applied(menu.data));
    for (long at = 0; at < menu.size; ++at) {
        const int header = (at >= 0x5e && at < 0x62) || (at >= 0x66 && at < 0x68) || (at >= 0xe8 && at < 0x108);
        if (!header && menu.data[at] != menu.pristine[at]) {
            CHECK(!"the layout's own data is never written");
            break;
        }
    }
    for (int i = 0; i < INN_LAYOUT_PARTS; ++i) check_seven_row_part(&menu, scan.part[i], scan.first_row_layer[i], centers, scan.rows);

    int untouched = 0, party = 0;
    for (int part = 0; part < MBS_PART_COUNT; ++part) {
        int seven = 0;
        for (int i = 0; i < INN_LAYOUT_PARTS; ++i) seven |= scan.part[i] == part;
        if (seven) continue;
        const uint8_t *record = mbs_table(menu.data, MBS_PARTS) + part * 48;
        CHECK(!memcmp(record, mbs_table(menu.pristine, MBS_PARTS) + part * 48, 48));
        const PartView view = part_view(menu.data, part);
        for (int at = 0; at < view.count; ++at) {
            KeyView old_keys[32], new_keys[32];
            const int n = layer_keys(menu.pristine, view.first + at, old_keys, 32);
            CHECK(layer_keys(menu.data, view.first + at, new_keys, 32) == n);
            for (int k = 0; k < n; ++k) CHECK(!memcmp(old_keys[k].transform, new_keys[k].transform, 36));
        }
        party += !strncmp((const char *)record + 0x10, "PARTY_", 6) || !strncmp((const char *)record + 0x10, "INN_CURSOR", 10);
        ++untouched;
    }
    CHECK(untouched == MBS_PART_COUNT - INN_LAYOUT_PARTS && party > 40);

    CHECK(inn_layout_position(menu.data, INN_1, 14, 17) == 15 && inn_layout_position(menu.data, INN_1, 14, 16) == 14);
    CHECK(inn_layout_position(menu.data, INN_1, 0, 17) == 0 && inn_layout_position(menu.data, 108, 14, 17) == 14);
    CHECK(!memcmp(mbs_table(menu.data, MBS_LAYERS) + (part_view(menu.data, INN_1).first + 15) * 24,
                  mbs_table(menu.pristine, MBS_LAYERS) + (part_view(menu.pristine, INN_1).first + 14) * 24, 24));

    CHECK(element_y(menu.data, INN_E0, LIT_ELEMENT) == centers[4] && element_y(menu.data, INN_E0, FLASH_ELEMENT) == centers[4]);
    inn_layout_options_row(menu.data, 3);
    CHECK(element_y(menu.data, INN_E0, LIT_ELEMENT) == centers[3] && element_y(menu.data, INN_E0, FLASH_ELEMENT) == centers[3]);
    inn_layout_options_row(menu.data, 4);
    CHECK(element_y(menu.data, INN_E0, LIT_ELEMENT) == centers[4]);

    const uint16_t layers = mbs_u16(menu.data + 0x66), keyframes = mbs_u16(menu.data + 0x60);
    CHECK(inn_layout_restore(menu.data) == 1 && !inn_layout_applied(menu.data));
    CHECK(!memcmp(menu.data, menu.pristine, (size_t)menu.size));
    CHECK(inn_layout_restore(menu.data) == 0);
    CHECK(inn_layout_apply(menu.data, centers, 0.8f) == INN_LAYOUT_APPLIED && inn_layout_apply(menu.data, centers, 0.8f) == INN_LAYOUT_APPLIED);
    CHECK(mbs_u16(menu.data + 0x66) == layers && mbs_u16(menu.data + 0x60) == keyframes);
    CHECK(inn_layout_restore(menu.data) == 1 && !memcmp(menu.data, menu.pristine, (size_t)menu.size));

    uint8_t *other = clone_layout(&menu);
    const uint16_t fewer = MBS_PART_COUNT - 1;
    memcpy(other + 0x64, &fewer, sizeof(fewer));
    CHECK(inn_layout_apply(other, centers, 0.8f) == INN_LAYOUT_OTHER && !inn_layout_applied(other));
    free(other);
    other = clone_layout(&menu);
    const uint8_t *row_key = mbs_table(other, MBS_KEYFRAMES) + mbs_u16(mbs_table(other, MBS_LAYERS) + 18 * 24) * 32;
    const float moved = 21.0f;
    memcpy((uint8_t *)(uintptr_t)mbs_table(other, MBS_TRANSFORMS) + mbs_u16(row_key + 4) * 36 + 4, &moved, sizeof(moved));
    uint8_t *copy = malloc((size_t)menu.size);
    memcpy(copy, other, (size_t)menu.size);
    CHECK(inn_layout_apply(other, centers, 0.8f) == INN_LAYOUT_UNSUPPORTED && !memcmp(other, copy, (size_t)menu.size));
    CHECK(!inn_layout_scan(other, &scan) && !strcmp(scan.failure, "INN_1: row layers fingerprint mismatch"));
    CHECK(!memcmp(menu.data, menu.pristine, (size_t)menu.size));
    free(copy);
    free(other);

    other = clone_layout(&menu);
    const uint16_t more_layers = 2047;
    memcpy(other + 0x66, &more_layers, sizeof(more_layers));
    CHECK(inn_layout_apply(other, centers, 0.8f) == INN_LAYOUT_OTHER);
    CHECK(!inn_layout_scan(other, &scan) && !strcmp(scan.failure, "not the known menu layout"));
    free(other);
    other = clone_layout(&menu);
    put_pointer(other + MBS_PARTS, NULL);
    CHECK(inn_layout_apply(other, centers, 0.8f) == INN_LAYOUT_OTHER);
    CHECK(!inn_layout_scan(other, &scan) && !strcmp(scan.failure, "runtime tables unavailable"));
    free(other);
    other = clone_layout(&menu);
    uint8_t *layers_of_other = (uint8_t *)(uintptr_t)mbs_table(other, MBS_LAYERS);
    const PartView c0 = part_view(other, 42);
    memcpy(layers_of_other + (c0.first + 3) * 24, layers_of_other + (c0.first + 2) * 24, 4);
    memcpy(copy = malloc((size_t)menu.size), other, (size_t)menu.size);
    CHECK(inn_layout_apply(other, centers, 0.8f) == INN_LAYOUT_UNSUPPORTED && !memcmp(other, copy, (size_t)menu.size));
    CHECK(!inn_layout_scan(other, &scan) && !strcmp(scan.failure, "INN_C0: expected 6 row layers"));
    free(copy);
    free(other);

    MenuLayout raw;
    CHECK(read_layout("Menu.mbs", &raw, 0));
    CHECK(inn_layout_apply(raw.data, centers, 0.8f) == INN_LAYOUT_OTHER && !memcmp(raw.data, raw.pristine, (size_t)raw.size));
    CHECK(!inn_layout_scan(raw.data, &scan) && !strcmp(scan.failure, "runtime tables unavailable"));
    free(raw.data);
    free(raw.pristine);
}

/* Every Menu layout a supported build loads takes the seven rows the same way: the shared archive's (the only one of the
 * Japanese and Asian builds), CUSA10454 1.00's regional base and language copies, and CUSA10454 1.01's. */
static void test_regional_inn_layouts(void) {
    static const struct {
        const char *folder, *name;
    } kLayouts[] = {{"base", "Menu.mbs"},     {"us100", "Menu.mbs"},    {"us100", "Menu_US.mbs"}, {"us100", "Menu_UK.mbs"},
                    {"us100", "Menu_FR.mbs"}, {"us100", "Menu_DE.mbs"}, {"us100", "Menu_IT.mbs"}, {"us100", "Menu_ES.mbs"},
                    {"ru101", "Menu.mbs"},    {"ru101", "Menu_US.mbs"}};
    float centers[7];
    custom_centers(centers);
    int checked = 0;
    for (size_t i = 0; i < sizeof(kLayouts) / sizeof(*kLayouts); ++i) {
        MenuLayout menu;
        if (!read_layout_at(kLayouts[i].folder, kLayouts[i].name, &menu, 1)) continue;
        inn_layout_reset();
        InnLayoutScan scan;
        CHECK(inn_layout_scan(menu.data, &scan) == 1 && scan.count == INN_LAYOUT_PARTS);
        if (scan.count != INN_LAYOUT_PARTS) printf("  %s/%s: %s\n", kLayouts[i].folder, kLayouts[i].name, scan.failure);
        CHECK(inn_layout_apply(menu.data, centers, 0.8f) == INN_LAYOUT_APPLIED);
        for (int p = 0; p < scan.count; ++p) check_seven_row_part(&menu, scan.part[p], scan.first_row_layer[p], centers, scan.rows);
        CHECK(inn_layout_restore(menu.data) == 1 && !memcmp(menu.data, menu.pristine, (size_t)menu.size));
        inn_layout_reset();
        free(menu.data);
        free(menu.pristine);
        ++checked;
    }
    printf("Inn background: %d regional menu layouts take seven rows\n", checked);
}

/* The hooks give the Inn's own background seven rows while the Boss Battle menu lives, and only then. */
static MenuLayout hook_base, hook_language;
static uint8_t fake_resources[2][0x200];

static int attach_background(void) {
    if (!hook_base.data && (!load_menu_layout("Menu.mbs", &hook_base) || !load_menu_layout("Menu_US.mbs", &hook_language)))
        return 0;
    put_pointer(fake_resources[0] + 0x158, hook_base.data);
    put_pointer(fake_resources[1] + 0x158, hook_language.data);
    put_pointer(fake_inn + 0x38d0 + 0xc0, fake_resources[0]);
    put_pointer(fake_inn + 0x38d0 + 0xc8, fake_resources[1]);
    return 1;
}

static int backgrounds_pristine(void) {
    return !memcmp(hook_base.data, hook_base.pristine, (size_t)hook_base.size) &&
           !memcmp(hook_language.data, hook_language.pristine, (size_t)hook_language.size);
}

static GuestRegs layer_call(int part, int layers, int position) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    uint8_t *player = fake_inn + 0x38d0;
    const int16_t current = (int16_t)part;
    const uint16_t count = (uint16_t)layers;
    memcpy(player + 0x150, &current, sizeof(current));
    memcpy(player + 0x180, &count, sizeof(count));
    regs.rdi = (uint64_t)(uintptr_t)player;
    regs.rsi = (uint32_t)position;
    boss_battle_inn_layer_hook(&regs);
    return regs;
}

static void test_inn_background_hooks(void) {
    start_guarded();
    use_fake_inn_functions();
    boss_battle_set_enabled(0);
    set_inn(6, 6, -1, 0);
    if (!attach_background()) {
        puts("build/research/.../Menu.mbs not found: Inn background hook checks skipped");
        return;
    }
    GuestRegs regs = inn_regs();
    boss_battle_inn_built_hook(&regs);
    draw_frame(inn_list());
    CHECK(!inn_layout_applied(hook_base.data) && !inn_layout_applied(hook_language.data) && backgrounds_pristine());
    CHECK((int32_t)layer_call(INN_1, 16, 14).rsi == 14);

    boss_battle_set_enabled(1);
    set_inn(6, 6, -1, 0);
    regs = inn_regs();
    boss_battle_inn_built_hook(&regs);
    CHECK(!inn_layout_applied(hook_base.data) && main_rows_are_inn() && item_tag(BOSS_BATTLE_INN_ROW) == BOSS_BATTLE_INN_ROW_RECORD);
    CHECK(draw_frame(inn_list()).skip && menu_calls.texts == BOSS_BATTLE_INN_ROWS);
    attach_background();
    draw_frame(inn_list());
    CHECK(inn_layout_applied(hook_base.data) && inn_layout_applied(hook_language.data));
    float centers[7];
    custom_centers(centers);
    for (int r = 0; r < 7; ++r) {
        const PartView view = part_view(hook_base.data, INN_1);
        KeyView key;
        CHECK(layer_keys(hook_base.data, view.first + 1 + r, &key, 1) == 1 && key.y == centers[r] &&
              absf(key.scale_y - 0.72f) < 0.0001f);
        CHECK(menu_calls.text[r].y + (float)(menu_calls.text[r].height / 2) == key.y);
    }
    CHECK((int32_t)layer_call(INN_1, 17, 14).rsi == 15 && (int32_t)layer_call(INN_1, 16, 14).rsi == 14);
    CHECK((int32_t)layer_call(INN_1, 17, 0).rsi == 0);
    regs = layer_call(INN_1, 17, 14);
    regs.rdi += 8;
    regs.rsi = 14;
    boss_battle_inn_layer_hook(&regs);
    CHECK((int32_t)regs.rsi == 14);

    set_inn(7, 7, BOSS_BATTLE_INN_ROW, BOSS_BATTLE_INN_ROW);
    attach_background();
    regs = result_frame();
    CHECK(element_y(hook_base.data, INN_E0, LIT_ELEMENT) == centers[3] && element_y(hook_base.data, INN_E0, FLASH_ELEMENT) == centers[3]);
    start_guarded();
    set_inn(7, 7, 4, 4);
    attach_background();
    regs = result_frame();
    CHECK((int32_t)regs.rax == 3 && element_y(hook_base.data, INN_E0, LIT_ELEMENT) == centers[4]);

    GuestRegs gone;
    memset(&gone, 0, sizeof(gone));
    gone.rdi = (uint64_t)(uintptr_t)fake_inn;
    boss_battle_inn_destroyed_hook(&gone);
    CHECK(!inn_layout_applied(hook_base.data) && !inn_layout_applied(hook_language.data) && backgrounds_pristine());
    CHECK((int32_t)layer_call(INN_1, 17, 14).rsi == 14);

    set_inn(6, 6, -1, 0);
    attach_background();
    regs = inn_regs();
    boss_battle_inn_built_hook(&regs);
    const uint16_t layers = mbs_u16(hook_base.data + 0x66);
    CHECK(inn_layout_applied(hook_base.data) && inn_layout_applied(hook_language.data));
    boss_battle_inn_destroyed_hook(&gone);
    set_inn(6, 6, -1, 0);
    attach_background();
    regs = inn_regs();
    boss_battle_inn_built_hook(&regs);
    CHECK(inn_layout_applied(hook_base.data) && mbs_u16(hook_base.data + 0x66) == layers);

    boss_battle_set_enabled(0);
    set_inn(6, 6, -1, 0);
    attach_background();
    regs = inn_regs();
    boss_battle_inn_built_hook(&regs);
    CHECK(!inn_layout_applied(hook_base.data) && backgrounds_pristine());
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

static void network_render_text(uint64_t text, const char *value);

static void GUEST_HOOK_ABI fake_set_text(uint64_t text, const char *value) {
    ++text_sets;
    network_render_text(text, value);
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
static int se_calls, se_value = -1;
static uint64_t se_list;
static void GUEST_HOOK_ABI fake_play_se(uint64_t list, int se) {
    ++se_calls;
    se_list = list;
    se_value = se;
}
static int step_sets, step_level, step_value;
static void GUEST_HOOK_ABI fake_set_step(uint64_t scene, int level, int value) {
    (void)scene;
    ++step_sets;
    step_level = level;
    step_value = value;
}

static int text_changes;
static void GUEST_HOOK_ABI fake_text_changed(uint64_t text) {
    ++text_changes;
    const uint32_t pending = 0x477fff00u;
    memcpy((void *)(uintptr_t)(text + 0x10c), &pending, sizeof(pending));
}

static void use_fake_screen_functions(void) {
    const BossBattleScreenFunctions functions = {fake_set_step,  fake_set_text, fake_set_message,
                                                 fake_row_frame, fake_play_se,  fake_text_changed};
    boss_battle_set_screen_functions(&functions);
    memset(screen_texts, 0, sizeof(screen_texts));
    text_sets = frame_calls = se_calls = 0;
    se_value = -1;
    se_list = 0;
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

static GuestRegs page_update(void) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rdi = (uint64_t)(uintptr_t)fake_body;
    boss_battle_list_update_hook(&regs);
    return regs;
}

static int page_hidden(void) {
    return (i32_at(fake_body, 0x964) & 0x400) != 0;
}

static GuestRegs page_move(int down) {
    GuestRegs regs = page_regs();
    if (down) boss_battle_list_down_hook(&regs);
    else boss_battle_list_up_hook(&regs);
    return regs;
}

/* Rows 1-6 scroll through the twenty bosses at their ends; Fight All and the bottom action stay. */
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
    CHECK(boss_battle_screen_move(7, 1) == 0 && boss_battle_screen_offset() == 0);       /* wraps to Fight All */
    CHECK(boss_battle_screen_move(0, 0) == 0 && boss_battle_screen_offset() == 14);      /* and back */
    CHECK(boss_battle_screen_move(1, 0) == 1 && boss_battle_screen_offset() == 13);
    CHECK(boss_battle_screen_move(3, 1) == 0 && boss_battle_screen_move(3, 0) == 0 && boss_battle_screen_offset() == 13);

    boss_battle_screen_label(1, text, sizeof(text));
    CHECK(!strcmp(text, "14 Cyclops"));
    boss_battle_screen_label(0, text, sizeof(text));
    CHECK(!strcmp(text, "Fight All"));
    boss_battle_screen_label(7, text, sizeof(text));
    CHECK(!strcmp(text, ""));
    boss_battle_screen_label(6, text, sizeof(text)); /* the number and the name only: no Ready */
    CHECK(!strcmp(text, "19 Ancient Dragon"));
    boss_battle_screen_help(6, text, sizeof(text)); /* a boss shows the character */
    CHECK(!strcmp(text, "Zak - Fighter, level 1"));
    boss_battle_screen_move(6, 1);
    boss_battle_screen_label(6, text, sizeof(text));
    CHECK(!strcmp(text, "20 Demon Lord"));
    boss_battle_screen_help(6, text, sizeof(text)); /* the Demon King as every other boss */
    CHECK(!strcmp(text, "Zak - Fighter, level 1"));
    boss_battle_screen_help(7, text, sizeof(text));
    CHECK(!strcmp(text, "Return to Dragon's Haven Inn."));
    boss_battle_screen_help(0, text, sizeof(text));
    CHECK(!strcmp(text, "Fight every boss in sequence."));

    int index = -1;
    CHECK(boss_battle_screen_choose(-1, &index) == BOSS_BATTLE_SCREEN_NONE);
    CHECK(boss_battle_screen_choose(0, &index) == BOSS_BATTLE_SCREEN_FIGHT_ALL && index == -1);
    CHECK(boss_battle_screen_choose(8, &index) == BOSS_BATTLE_SCREEN_NONE && index == -1);
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

/* The language changes the words only: the character's key, the boss ids, the record file and the numbers stay. */
static void test_language_independence(void) {
    boss_battle_reset();
    boss_battle_record_clear();
    start_guarded();
    boss_battle_set_character("Zak", 2, 7);
    char key[160], english_key[160], text[160];
    CHECK(boss_battle_character_key(english_key, sizeof(english_key)) && !strcmp(english_key, "Zak|Elf"));
    CHECK(boss_battle_record_note(english_key, "harpy", BOSS_BATTLE_ATTEMPT, 0) &&
          boss_battle_record_note(english_key, "harpy", BOSS_BATTLE_WIN, 61000) &&
          boss_battle_record_note(english_key, "harpy", BOSS_BATTLE_ATTEMPT, 0) &&
          boss_battle_record_note(english_key, "harpy", BOSS_BATTLE_DEATH, 0));
    char before[4096];
    const size_t length = boss_battle_record_format(before, sizeof(before));
    boss_battle_screen_request();
    static const char *const kLanguages[] = {"english", "japanese", "brazilian_portuguese", "russian", "korean"};
    char wins[sizeof(kLanguages) / sizeof(*kLanguages)][64];
    for (size_t i = 0; i < sizeof(kLanguages) / sizeof(*kLanguages); ++i) {
        CHECK(boss_battle_text_set_language(kLanguages[i], 0));
        CHECK(boss_battle_character_key(key, sizeof(key)) && !strcmp(key, english_key));
        const BossBattleStats stats = boss_battle_record_stats(key, boss_battle_encounter(0)->id);
        CHECK(stats.attempts == 2 && stats.wins == 1 && stats.deaths == 1);
        boss_battle_screen_statistic(2, 1, wins[i], sizeof(wins[i]));
        CHECK(strlen(wins[i]) > 2 && !strcmp(wins[i] + strlen(wins[i]) - 2, " 1"));
        boss_battle_screen_label(1, text, sizeof(text));
        CHECK(!strncmp(text, "01 ", 3) && !strcmp(text + 3, boss_battle_text_boss(0)));
        boss_battle_screen_help(1, text, sizeof(text));
        CHECK(strstr(text, "Zak") && strstr(text, boss_battle_text_class(2)) && strstr(text, "7"));
        char after[4096];
        CHECK(boss_battle_record_format(after, sizeof(after)) == length && !memcmp(after, before, length));
        CHECK(!strcmp(boss_battle_encounter(19)->id, "demon_king"));
    }
    CHECK(strcmp(wins[0], wins[1]) && strcmp(wins[0], wins[2]) && !strcmp(wins[0], "Wins 1"));
    boss_battle_text_set_language("english", 0);
    boss_battle_screen_reset();
    boss_battle_record_clear();
    boss_battle_reset();
}

/* Walking down the screen meets Fight All, the twenty bosses and Return to tavern, each once and in that order; no boss
 * is marked pending. Fight All is its own entry, not the first boss: confirming it starts nothing and records nothing. */
static void test_screen_entries(void) {
    char text[160], key[160];
    boss_battle_reset();
    boss_battle_record_clear();
    start_guarded();
    boss_battle_screen_request();
    CHECK(boss_battle_screen_built() == 1 && boss_battle_screen_offset() == 0);
    int row = 0, count = 0;
    BossBattleEntry entry = boss_battle_screen_entry(row);
    for (;;) {
        CHECK(entry == (BossBattleEntry)count);
        boss_battle_screen_label(row, text, sizeof(text));
        if (entry == BOSS_BATTLE_ENTRY_FIGHT_ALL) CHECK(!strcmp(text, "Fight All"));
        else if (entry != BOSS_BATTLE_ENTRY_RETURN_TO_TAVERN) CHECK(!strncmp(text + 3, kShownNames[count - 1], 20));
        boss_battle_screen_help(row, text, sizeof(text));
        CHECK(!strstr(text, "Pending") && !strstr(text, "confirmed"));
        for (int i = 1; i <= BOSS_BATTLE_SCREEN_BOSS_ROWS; ++i) {
            boss_battle_screen_statistic(i, row, text, sizeof(text));
            CHECK(!strstr(text, "Pending"));
        }
        ++count;
        if (entry == BOSS_BATTLE_ENTRY_RETURN_TO_TAVERN || count > BOSS_BATTLE_ENTRIES) break;
        if (!boss_battle_screen_move(row, 1)) ++row;
        entry = boss_battle_screen_entry(row);
    }
    CHECK(count == BOSS_BATTLE_ENTRIES && row == BOSS_BATTLE_SCREEN_RETURN_ROW);
    CHECK(BOSS_BATTLE_ENTRY_FIGHT_ALL == 0 && BOSS_BATTLE_ENTRY_BOSS_20 == BOSS_BATTLE_ENCOUNTERS &&
          BOSS_BATTLE_ENTRY_RETURN_TO_TAVERN == BOSS_BATTLE_ENCOUNTERS + 1);
    CHECK(boss_battle_entry_boss(BOSS_BATTLE_ENTRY_FIGHT_ALL) == -1 && boss_battle_entry_boss(BOSS_BATTLE_ENTRY_BOSS_01) == HARPY &&
          boss_battle_entry_boss(BOSS_BATTLE_ENTRY_BOSS_20) == DEMON_KING &&
          boss_battle_entry_boss(BOSS_BATTLE_ENTRY_RETURN_TO_TAVERN) == -1);

    /* Fight All highlighted: its own statistics, not the first boss's */
    CHECK(start_playable(HARPY, kArenaPlay) && boss_battle_party_wiped());
    boss_battle_inn_arrived();
    boss_battle_take_menu_reopen();
    boss_battle_screen_request();
    CHECK(boss_battle_screen_built() == 1 && boss_battle_screen_boss(1) == HARPY);
    boss_battle_screen_statistic(1, 1, text, sizeof(text));
    CHECK(!strcmp(text, "Attempts 1"));
    static const char *const kFightAll[6] = {"Runs 0", "Clears 0", "Deaths 0", "Quits 0", "Best Progress -", "Best Time -"};
    for (int i = 1; i <= 6; ++i) {
        boss_battle_screen_statistic(i, 0, text, sizeof(text));
        CHECK(!strcmp(text, kFightAll[i - 1]));
        boss_battle_screen_statistic(i, 7, text, sizeof(text)); /* the bottom action keeps Fight All's */
        CHECK(!strcmp(text, kFightAll[i - 1]));
    }

    /* confirming Fight All queues a run that starts the first boss from the Inn's next menu frame; no session, no boss
     * statistics and no run until that boss is playable */
    BossBattleStats before[BOSS_BATTLE_ENCOUNTERS];
    boss_battle_character_key(key, sizeof(key));
    for (int i = 0; i < BOSS_BATTLE_ENCOUNTERS; ++i) before[i] = boss_battle_record_stats(key, boss_battle_encounter(i)->id);
    int index = -1;
    CHECK(boss_battle_screen_choose(0, &index) == BOSS_BATTLE_SCREEN_FIGHT_ALL && index == -1);
    CHECK(boss_battle_fight_all_request() == 1 && boss_battle_fight_all_active() && !boss_battle_fight_all_checkpoint());
    CHECK(boss_battle_screen_shown() && boss_battle_screen_take_close_cut() && !boss_battle_session_active());
    CHECK(boss_battle_start_transition() == BOSS_BATTLE_START_NONE);
    CHECK(boss_battle_fight_all_request() == 0); /* one run at a time */
    for (int i = 0; i < BOSS_BATTLE_ENCOUNTERS; ++i) {
        const BossBattleStats after = boss_battle_record_stats(key, boss_battle_encounter(i)->id);
        CHECK(!memcmp(&after, &before[i], sizeof(after)));
    }
    CHECK(boss_battle_record_fight_all_stats(key).runs == 0);
    CHECK(boss_battle_screen_closed(&index) && index == HARPY);
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

/* The Inn's Boss Battle row opens the native Options page as the screen; a boss chosen there starts as the Options mode
 * leaves the page (see test_direct_start), or else from the Inn's next menu frame; Return to tavern and Back leave the
 * Inn menu as it was; the game's own Options stay its own. */
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
    CHECK(page_result(3).r15 == (uint64_t)(uintptr_t)fake_body && !page_move(1).skip && !se_calls);

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
    CHECK(label_colours_differ(0) && label_colours_differ(7)); /* Fight All and Return to tavern stay the game's */
    CHECK(frame_calls == 6 && frame_value == 0xe0 && message_value == BOSS_BATTLE_RETURN_TO_TAVERN_RECORD);
    /* the bosses are the selectable value column (right), the statistics the static label column (left) */
    CHECK(!strcmp(text_at(fake_rows[1], 0x5c0), "01 Harpy") && !strcmp(text_at(fake_rows[6], 0x5c0), "06 Pirates"));
    CHECK(!strcmp(text_at(fake_rows[1], 0x380), "Attempts 0") && !strcmp(text_at(fake_rows[6], 0x380), "Best Time -"));
    char character[160];
    CHECK(boss_battle_character_text(character, sizeof(character)));
    CHECK(!strcmp(text_at(fake_rows[0], 0x5c0), "Fight All") && !strcmp(text_at(fake_window, 0xfa0), character));
    /* every boss name in the one plain value colour, played or not */
    for (int i = 1; i <= 6; ++i) CHECK(i32_at(fake_rows[i], 0x5c0 + 0x110) == 0x7fffffff);

    /* no row logic of the game; the cursor follows into the texts; the boss rows scroll at their ends */
    regs = page_regs();
    boss_battle_options_update_hook(&regs);
    CHECK(regs.skip && regs.rax == 0);
    set_page_cursor(3);
    regs = page_result(-1);
    CHECK(regs.r15 == (uint64_t)(uintptr_t)fake_body && !strcmp(text_at(fake_rows[0], 0x5c0), "Fight All") &&
          !strcmp(text_at(fake_rows[3], 0x380), "Deaths 0"));
    set_page_cursor(6);
    regs = page_move(1);
    CHECK(regs.skip && regs.rax == 1 && boss_battle_screen_offset() == 1);
    CHECK(!strcmp(text_at(fake_rows[1], 0x5c0), "02 Wyvern") && !strcmp(text_at(fake_rows[6], 0x5c0), "07 Minotaur"));
    set_page_cursor(3);
    CHECK(!page_move(1).skip && !page_move(0).skip);
    set_page_cursor(7);
    page_result(-1);
    CHECK(!strcmp(text_at(fake_window, 0xfa0), "Return to Dragon's Haven Inn."));
    /* the list sounds the cursor (scrolled or moved) and Back itself */
    page_result(-2);
    CHECK(!se_calls);

    /* Fight All: a distinct entry with its help and its own statistics, no placeholder */
    set_page_cursor(0);
    page_result(-1);
    CHECK(!strcmp(text_at(fake_window, 0xfa0), "Fight every boss in sequence.") &&
          !strcmp(text_at(fake_rows[1], 0x380), "Runs 0") && !strcmp(text_at(fake_rows[5], 0x380), "Best Progress -") &&
          !strcmp(text_at(fake_rows[6], 0x380), "Best Time -"));
    page_result(-1);
    CHECK(se_calls == 0 && !boss_battle_fight_all_active());

    /* Return to tavern closes the page with the game's menu sound once; the Inn's next menu frame starts nothing */
    regs = page_result(7);
    CHECK(regs.r15 == 1 && se_calls == 1 && se_value == BOSS_BATTLE_SE_DECIDE);
    set_inn(7, 7, -1, BOSS_BATTLE_INN_ROW);
    regs = result_frame();
    CHECK(regs.skip && (uint32_t)regs.rax == 0xffffffffu && !boss_battle_screen_shown() && !boss_battle_session_active());
    CHECK(!guard_log[0]);

    /* fallback: a page still open at the Inn's next menu frame starts its boss there (no game server here: put back at once) */
    set_inn(7, 7, BOSS_BATTLE_INN_ROW, BOSS_BATTLE_INN_ROW);
    result_frame();
    fake_options_page(8);
    build_page();
    set_page_cursor(2);
    se_calls = 0;
    regs = page_result(2);
    CHECK(regs.r15 == 1 && se_calls == 1 && se_value == BOSS_BATTLE_SE_DECIDE); /* a boss: the menu sound once */
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
          !strcmp(text_at(fake_rows[0], 0x5c0), "Fight All"));
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

/* ---- the arena and the Boss Battle screen without a visible Inn menu, the Inn's lifecycle untouched ---- */

enum { INN_ACT = 0x103, INN_STEP = 0x104, INN_SHOWN = 0x3989, FADE_BLACK = (int)0xff000000u, FADE_LAYER = 0x2a };

static uint8_t game_manager[0x200], game_scene[0x9400], game_context[0x9000], fader_owner[0x100], scene_class;
static struct {
    int calls, party_sends_before, stage;
    char round[64], guard_before[32];
} arena_request;
static struct {
    int calls, colour[8], frames[8], layer[8], requests_before[8];
    int last_colour, last_frames, last_layer;
} fades;

static void GUEST_HOOK_ABI fake_scene_request(uint64_t server, const char *round, int stage, int entry,
                                              const char *stage_name, const uint8_t *route_flag, int force, int code,
                                              int slot, int option, const void *places) {
    (void)server, (void)entry, (void)stage_name, (void)route_flag, (void)force, (void)code, (void)slot, (void)option,
        (void)places;
    ++arena_request.calls;
    arena_request.party_sends_before = party_sends;
    arena_request.stage = stage;
    snprintf(arena_request.round, sizeof(arena_request.round), "%s", round);
    snprintf(arena_request.guard_before, sizeof(arena_request.guard_before), "%s", guard_log);
}

static uint64_t GUEST_HOOK_ABI fake_get_fader(uint64_t owner) {
    return owner == (uint64_t)(uintptr_t)fader_owner ? owner + 0x78 : 0;
}

static void GUEST_HOOK_ABI fake_fade(uint64_t fader, uint32_t colour, int frames, int layer) {
    CHECK(fader == (uint64_t)(uintptr_t)fader_owner + 0x78);
    fades.last_colour = (int)colour;
    fades.last_frames = frames;
    fades.last_layer = layer;
    if (fades.calls < 8) {
        fades.colour[fades.calls] = (int)colour;
        fades.frames[fades.calls] = frames;
        fades.layer[fades.calls] = layer;
        fades.requests_before[fades.calls] = arena_request.calls;
    }
    ++fades.calls;
}

static void use_fake_game(void) {
    memset(game_manager, 0, sizeof(game_manager));
    memset(game_scene, 0, sizeof(game_scene));
    memset(game_context, 0, sizeof(game_context));
    memset(&arena_request, 0, sizeof(arena_request));
    memset(&fades, 0, sizeof(fades));
    put_pointer(fake_inn_context + HYDELAND_CONTEXT_MANAGER, game_manager);
    put_pointer(game_manager + HYDELAND_MANAGER_SCENE, game_scene);
    put_pointer(game_manager + 0xd8, fader_owner);
    put_pointer(game_scene, &scene_class);
    put_pointer(game_scene + 0x90, game_context);
    put_pointer(game_context + 0x88, game_scene);
    put_pointer(game_context + 0x80, game_context);
    boss_battle_set_scene((uint64_t)(uintptr_t)&scene_class, fake_scene_request);
    const BossBattleFadeFunctions functions = {fake_get_fader, fake_fade};
    boss_battle_set_fade_functions(&functions);
}

static void release_fake_game(void) {
    memset(fake_inn_context + HYDELAND_CONTEXT_MANAGER, 0, 8);
    boss_battle_set_scene(0, NULL);
    boss_battle_set_fade_functions(NULL);
    boss_battle_set_screen_functions(NULL);
    boss_battle_reset();
}

/* The Inn's Boss Battle row opened the page as the screen, with the cursor on `row`. */
static void open_boss_screen(int row) {
    set_inn(7, 7, BOSS_BATTLE_INN_ROW, BOSS_BATTLE_INN_ROW);
    result_frame();
    fake_options_page(8);
    build_page();
    set_page_cursor(row);
}

/* The Options mode's first-frame check after the page closed, with the Inn's own bytes recorded around it. */
static GuestRegs options_leave_frame(void) {
    uint8_t before[0x200];
    memcpy(before, fake_inn + INN_SHOWN - 0x100, sizeof(before));
    GuestRegs regs = inn_regs();
    boss_battle_options_leave_hook(&regs);
    CHECK(!regs.skip && !memcmp(before, fake_inn + INN_SHOWN - 0x100, sizeof(before))); /* the mode runs as native */
    return regs;
}

static GuestRegs arrival_fade(void) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    boss_battle_arrival_fade_hook(&regs);
    CHECK(!regs.skip);
    return regs;
}

/* A frame of the page window's input act, with the Options mode in `step`. */
static void page_tick(int step) {
    fake_inn[INN_ACT] = 0x8e;
    fake_inn[INN_STEP] = (uint8_t)step;
    GuestRegs regs = page_regs();
    boss_battle_options_update_hook(&regs);
}

static void inn_arrives(void) {
    GuestRegs regs = inn_regs();
    boss_battle_inn_flushed_hook(&regs);
}

/* Confirming a boss fades the scene fader to black while the page closes natively; once the Options mode found the page
 * closed it cuts to black as the Inn's Leave does; the party and the arena request then go out from the Inn's next menu
 * frame, as before. */
static void test_direct_start(void) {
    start_guarded();
    use_fake_inn_functions();
    use_fake_screen_functions();
    use_fake_game();
    open_boss_screen(3);
    CHECK(!fades.calls);
    GuestRegs page = page_result(3);
    CHECK(page.r15 == 1 && !boss_battle_session_active() && !arena_request.calls && !party_sends);
    CHECK(fades.calls == 1 && fades.colour[0] == FADE_BLACK && fades.frames[0] == 30 && fades.layer[0] == FADE_LAYER);

    fake_inn[INN_ACT] = 0x8e;
    fake_inn[INN_STEP] = 3;
    options_leave_frame();
    CHECK(fades.calls == 2 && fades.colour[1] == FADE_BLACK && fades.frames[1] == 0 && fades.layer[1] == FADE_LAYER);
    CHECK(!boss_battle_session_active() && !arena_request.calls && !party_sends && boss_battle_screen_shown());
    options_leave_frame();
    CHECK(fades.calls == 2); /* once */

    set_inn(7, 7, -1, BOSS_BATTLE_INN_ROW);
    fake_inn[INN_ACT] = 0xa4;
    fake_inn[INN_STEP] = 1;
    GuestRegs menu = result_frame();
    CHECK(menu.skip && (uint32_t)menu.rax == 0xffffffffu);
    CHECK(boss_battle_session_active() && boss_battle_state() == BOSS_BATTLE_TRAVELLING && boss_battle_menu_cursor() == 2);
    CHECK(party_sends == 1 && arena_request.calls == 1 && arena_request.party_sends_before == 1);
    CHECK(!strcmp(party_guard, "PS") && !strcmp(arena_request.guard_before, "PS")); /* isolation and snapshot first */
    CHECK(!strcmp(arena_request.round, "round/MoldyDungeon.brb") && arena_request.stage == 10);
    CHECK(!boss_battle_screen_shown() && !boss_battle_screen_requested() && fades.calls == 2);

    /* travel: the arena's own fades take over; the page is gone before the arena loads */
    const BossBattleScene from = {2, 0, 4, 2, 10, 0}, loading = {4, 3, 4, 2, 10, 0}, arena = {2, 0, 4, 10, -1, 0};
    frames(probe(from, 0, 0, 0), 1);
    frames(probe(loading, 0, 0, 0), 3);
    frames(probe(arena, 1, 1, 0), 1);
    CHECK(boss_battle_state() == BOSS_BATTLE_INITIALIZING && !boss_battle_screen_shown() && !boss_battle_screen_requested());
    frames(probe(arena, 1, 1, 1), 1);
    CHECK(boss_battle_state() == BOSS_BATTLE_FIGHTING && boss_battle_milestones() & BOSS_BATTLE_PLAYABLE);
    CHECK(fades.calls == 2 && arena_request.calls == 1);
    release_fake_game();
}

static GuestRegs native_fade(uint64_t fader, uint32_t colour, int frames, int layer) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rdi = fader;
    regs.rsi = colour;
    regs.rdx = (uint32_t)frames;
    regs.rcx = (uint32_t)layer;
    boss_battle_fade_hook(&regs);
    CHECK(!regs.skip);
    return regs;
}

static uint64_t scene_fader_address(void) {
    return (uint64_t)(uintptr_t)fader_owner + 0x78;
}

static GuestRegs help_line(int show) {
    GuestRegs regs = inn_regs();
    regs.rdx = (uint32_t)show;
    boss_battle_inn_help_line_hook(&regs);
    CHECK(!regs.skip);
    return regs;
}

static int native_fade_passes(void) {
    const GuestRegs regs = native_fade(scene_fader_address(), 0, 30, 0x27);
    return regs.rsi == 0 && regs.rdx == 30 && regs.rcx == 0x27;
}

/* The travel act's step 19 (STEP_FADEIN) as the game makes it: its arrival call, then the fade itself, both untouched. */
static void native_arena_arrival(void) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rdi = scene_fader_address();
    regs.rdx = 30;
    regs.rcx = 0x27;
    boss_battle_arrival_fade_hook(&regs);
    CHECK(!regs.skip && regs.rsi == 0 && regs.rdx == 30 && regs.rcx == 0x27);
    CHECK(boss_battle_start_transition() == BOSS_BATTLE_START_REVEALING && native_fade_passes());
}

/* Back and Return to tavern never hold black; a confirmed boss holds the scene fader black on the top layer against every
 * native fade of the travel, closes the Inn help line instead of showing it, and leaves other faders alone. */
static void test_start_black_hold(void) {
    for (int back = 0; back < 2; ++back) {
        start_guarded();
        use_fake_inn_functions();
        use_fake_screen_functions();
        use_fake_game();
        open_boss_screen(back ? 3 : 7);
        page_result(back ? -2 : 7);
        options_leave_frame();
        CHECK(boss_battle_start_transition() == BOSS_BATTLE_START_NONE && !fades.calls);
        CHECK(native_fade_passes() && help_line(1).rdx == 1);
        release_fake_game();
    }

    start_guarded();
    use_fake_inn_functions();
    use_fake_screen_functions();
    use_fake_game();
    open_boss_screen(3);
    page_result(3);
    CHECK(boss_battle_start_transition() == BOSS_BATTLE_START_FADING && fades.calls == 1);
    CHECK(help_line(1).rdx == 0 && help_line(0).rdx == 0);
    GuestRegs held = native_fade(scene_fader_address(), 0, 30, 0x27);
    CHECK(held.rsi == (uint32_t)FADE_BLACK && held.rdx == 0 && held.rcx == FADE_LAYER);
    const GuestRegs other = native_fade(0x1234, 0, 30, 0x27);
    CHECK(other.rsi == 0 && other.rdx == 30 && other.rcx == 0x27);

    options_leave_frame();
    CHECK(boss_battle_start_transition() == BOSS_BATTLE_START_HELD && fades.calls == 2);
    set_inn(7, 7, -1, BOSS_BATTLE_INN_ROW);
    result_frame();
    CHECK(boss_battle_session_active() && party_sends == 1 && arena_request.calls == 1);
    CHECK(boss_battle_start_transition() == BOSS_BATTLE_START_HELD && help_line(1).rdx == 0);
    /* the travel's own fade-out and fade-in stay black */
    held = native_fade(scene_fader_address(), (uint32_t)FADE_BLACK, 30, 0x27);
    CHECK(held.rsi == (uint32_t)FADE_BLACK && held.rdx == 0 && held.rcx == FADE_LAYER);
    held = native_fade(scene_fader_address(), 0, 30, 0x27);
    CHECK(held.rsi == (uint32_t)FADE_BLACK && held.rdx == 0 && held.rcx == FADE_LAYER);
    CHECK(fades.calls == 2);
    release_fake_game();
}

/* The black ends once, through the arena's native arrival fade-in: not before the arena's stage is set up, with the
 * fade's own colour, frames and layer, and complete only once that fade has finished; no fade of our own follows. */
static void test_start_reveal(void) {
    start_guarded();
    use_fake_inn_functions();
    use_fake_screen_functions();
    use_fake_game();
    open_boss_screen(3);
    page_result(3);
    options_leave_frame();
    set_inn(7, 7, -1, BOSS_BATTLE_INN_ROW);
    result_frame();
    const BossBattleScene from = {2, 0, 4, 2, 10, 0}, loading = {4, 3, 4, 2, 10, 0}, loaded = {4, 6, 4, 10, -1, 0},
                          arena = {2, 0, 4, 10, -1, 0};
    frames(probe(from, 0, 0, 0), 1);
    frames(probe(loading, 0, 0, 0), 3);
    CHECK(!boss_battle_start_reveal_begins() && boss_battle_start_transition() == BOSS_BATTLE_START_HELD);
    frames(probe(loaded, 1, 1, 0), 1);
    CHECK(boss_battle_state() == BOSS_BATTLE_INITIALIZING && !boss_battle_screen_shown() && !boss_battle_screen_requested());
    CHECK(!boss_battle_start_reveal_finished(0) && boss_battle_start_transition() == BOSS_BATTLE_START_HELD);
    const int fades_before = fades.calls;
    CHECK(boss_battle_start_reveal_begins() && boss_battle_start_transition() == BOSS_BATTLE_START_REVEALING);
    CHECK(!boss_battle_start_reveal_begins());
    CHECK(native_fade_passes()); /* the game's own 30-frame fade-in, untouched */
    CHECK(!boss_battle_start_reveal_finished(1) && boss_battle_start_transition() == BOSS_BATTLE_START_REVEALING);
    CHECK(boss_battle_start_reveal_finished(0) && boss_battle_start_transition() == BOSS_BATTLE_START_DONE);
    CHECK(!boss_battle_start_reveal_finished(0) && fades.calls == fades_before);
    CHECK(native_fade_passes() && help_line(1).rdx == 1);
    frames(probe(arena, 1, 1, 1), 1);
    CHECK(boss_battle_state() == BOSS_BATTLE_FIGHTING);

    /* the return ends the transition for good */
    CHECK(boss_battle_quit_key(0, 0, 0));
    frames(probe(arena, 1, 1, 1), 1);
    boss_battle_inn_requested();
    inn_arrives();
    CHECK(boss_battle_start_transition() == BOSS_BATTLE_START_NONE && native_fade_passes());
    release_fake_game();
}

/* A request the game would not take ends the session at once and fades back to the Inn menu. */
static void test_direct_start_refused(void) {
    start_guarded();
    use_fake_inn_functions();
    use_fake_screen_functions();
    use_fake_game();
    memset(game_context + 0x80, 0, 8); /* no game server */
    open_boss_screen(3);
    page_result(3);
    options_leave_frame();
    set_inn(7, 7, -1, BOSS_BATTLE_INN_ROW);
    result_frame();
    CHECK(!boss_battle_session_active() && !arena_request.calls && !strcmp(guard_log, "PSBR"));
    CHECK(fades.calls == 3 && fades.colour[2] == 0 && fades.frames[2] == 30);
    release_fake_game();
}

/* Return to tavern and Back close the page for the Inn menu: no fade, nothing starts, the menu is shown as native. */
static void test_intentional_returns(void) {
    for (int back = 0; back < 2; ++back) {
        start_guarded();
        use_fake_inn_functions();
        use_fake_screen_functions();
        use_fake_game();
        open_boss_screen(back ? 3 : 7);
        const GuestRegs page = page_result(back ? -2 : 7);
        CHECK((page.r15 == 1) == !back);
        options_leave_frame();
        set_inn(7, 7, -1, BOSS_BATTLE_INN_ROW);
        const GuestRegs menu = result_frame();
        CHECK(menu.skip && (uint32_t)menu.rax == 0xffffffffu && !boss_battle_screen_shown());
        CHECK(!boss_battle_session_active() && !arena_request.calls && !party_sends && !guard_log[0] && !fades.calls);
        CHECK(!boss_battle_menu_reopen_pending() && !boss_battle_screen_requested());
        const GuestRegs arrival = arrival_fade();
        CHECK(arrival.rsi == 0 && !fades.calls);
        release_fake_game();
    }
}

typedef enum { ENDING_WIN, ENDING_DEATH, ENDING_QUIT, ENDING_CANCEL } Ending;

static void fight_and_return(int boss, BossBattleScene arena, Ending ending) {
    if (ending == ENDING_CANCEL) {
        CHECK(start_session(boss));
        reach_arena();
        frames(probe(kArenaPlay, 1, 1, 0), 10);
        CHECK(boss_battle_request_quit(BOSS_BATTLE_QUIT_MENU) == 1);
        CHECK(frames(probe(kArenaPlay, 1, 1, 0), 1) == BOSS_BATTLE_FRAME_REQUEST_INN);
        CHECK(boss_battle_outcome() == BOSS_BATTLE_OUTCOME_CANCELLED);
        boss_battle_inn_requested();
        return;
    }
    CHECK(start_playable(boss, arena));
    switch (ending) {
    case ENDING_WIN:
        boss_battle_boss_cleared(1);
        CHECK(!boss_battle_skip_results_screen() && boss_battle_results_level_up_done()); /* experience and level-up run */
        CHECK(boss_battle_town_return());
        break;
    case ENDING_DEATH:
        CHECK(boss_battle_party_wiped() == 1 && boss_battle_outcome() == BOSS_BATTLE_OUTCOME_DEATH);
        CHECK(boss_battle_skip_results_screen());
        boss_battle_inn_requested();
        break;
    default:
        CHECK(boss_battle_quit_key(0, 0, 0));
        CHECK(frames(probe(arena, 1, 1, 1), 1) == BOSS_BATTLE_FRAME_REQUEST_INN);
        CHECK(boss_battle_outcome() == BOSS_BATTLE_OUTCOME_QUIT && boss_battle_skip_results_screen());
        boss_battle_inn_requested();
        break;
    }
}

/* Win, Death, Quit and a cancel before Playable all come back to the Inn: the campaign is put back as the Inn is built,
 * the arrival keeps the scene black, the Inn's menu frame opens the page on the same boss with its new statistics, and
 * the scene fades in once the page is built. */
static void test_direct_reopen(void) {
    static const struct {
        int boss;
        BossBattleScene arena;
        Ending ending;
    } kReturns[] = {
        {MEDUSA, {2, 0, 3, 15, -1, 0}, ENDING_WIN},
        {2, {2, 0, 4, 10, -1, 0}, ENDING_DEATH},
        {HARPY, {2, 0, 3, 8, -1, 0}, ENDING_QUIT},
        {HARPY, {2, 0, 3, 8, -1, 0}, ENDING_CANCEL},
    };
    for (size_t i = 0; i < sizeof(kReturns) / sizeof(*kReturns); ++i) {
        const int boss = kReturns[i].boss;
        start_guarded();
        use_fake_inn_functions();
        use_fake_screen_functions();
        use_fake_game();
        const BossBattleStats before = stats_of(boss);
        fight_and_return(boss, kReturns[i].arena, kReturns[i].ending);
        const BossBattleStats after = stats_of(boss);
        CHECK(boss_battle_session_active() && !boss_battle_menu_reopen_pending() && !strcmp(guard_log, "PS"));

        inn_arrives();
        CHECK(!strcmp(guard_log, "PSBR") && !boss_battle_session_active() && boss_battle_menu_reopen_pending());
        GuestRegs arrival = arrival_fade();
        CHECK(arrival.rsi == (uint32_t)FADE_BLACK && arrival.rcx == FADE_LAYER && !fades.calls);

        set_inn(7, 7, -1, 0);
        GuestRegs menu = result_frame();
        CHECK(menu.skip && menu.rax == BOSS_BATTLE_INN_OPTIONS_ROW && boss_battle_screen_requested());
        CHECK(!boss_battle_menu_reopen_pending() && boss_battle_menu_cursor() == boss && !fades.calls);
        set_inn(7, 7, -1, 0);
        result_frame(); /* the Options mode has not built the page yet: still black */
        CHECK(!fades.calls);

        boss_battle_set_character("Zak", 0, 1);
        fake_options_page(8);
        build_page();
        CHECK(!fades.calls); /* constructed is not shown */
        page_tick(0);
        CHECK(!fades.calls); /* the Options mode still opening */
        page_tick(1);
        CHECK(fades.calls == 1 && fades.colour[0] == 0 && fades.frames[0] == 30 && fades.layer[0] == FADE_LAYER);
        char label[64], expected[64];
        boss_battle_label(boss, label, sizeof(label));
        CHECK(boss_battle_screen_shown() && !strcmp(text_at(fake_rows[0], 0x5c0), "Fight All"));
        CHECK(!strcmp(text_at(fake_rows[cursor_row], 0x5c0), label));
        snprintf(expected, sizeof(expected), "Attempts %u", after.attempts);
        CHECK(!strcmp(text_at(fake_rows[1], 0x380), expected));
        snprintf(expected, sizeof(expected), "Wins %u", after.wins);
        CHECK(!strcmp(text_at(fake_rows[2], 0x380), expected));
        snprintf(expected, sizeof(expected), "Deaths %u", after.deaths);
        CHECK(!strcmp(text_at(fake_rows[3], 0x380), expected));
        snprintf(expected, sizeof(expected), "Quits %u", after.quits);
        CHECK(!strcmp(text_at(fake_rows[4], 0x380), expected));
        CHECK(after.attempts == before.attempts + (kReturns[i].ending != ENDING_CANCEL) &&
              after.wins == before.wins + (kReturns[i].ending == ENDING_WIN) &&
              after.deaths == before.deaths + (kReturns[i].ending == ENDING_DEATH) &&
              after.quits == before.quits + (kReturns[i].ending == ENDING_QUIT));
        page_tick(1);
        CHECK(fades.calls == 1); /* once */
        release_fake_game();
    }
}

/* A failed restore never reopens nor holds black; a held black always ends when no Boss Battle page will show. */
static void test_reopen_conditions(void) {
    start_guarded();
    use_fake_inn_functions();
    use_fake_screen_functions();
    use_fake_game();
    restore_result = 0;
    CHECK(start_session(HARPY));
    inn_arrives();
    CHECK(!boss_battle_menu_reopen_pending() && arrival_fade().rsi == 0);
    char text[192];
    CHECK(boss_battle_take_notice(text, sizeof(text)));

    /* Boss Battle switched off before the Inn menu: no Boss Battle row, the scene fades in at the menu frame */
    start_guarded();
    use_fake_game();
    CHECK(start_session(HARPY));
    inn_arrives();
    CHECK(arrival_fade().rsi == (uint32_t)FADE_BLACK);
    set_inn(6, 6, -1, 0);
    result_frame();
    CHECK(fades.calls == 1 && fades.colour[0] == 0);
    set_inn(6, 6, -1, 0);
    result_frame();
    CHECK(fades.calls == 1);

    /* the page is not the expected one: it stays the game's Options, and the scene fades in */
    start_guarded();
    use_fake_game();
    CHECK(start_session(HARPY));
    inn_arrives();
    arrival_fade();
    set_inn(7, 7, -1, 0);
    result_frame();
    fake_options_page(7);
    build_page();
    CHECK(!boss_battle_screen_shown() && fades.calls == 1 && fades.colour[0] == 0);
    release_fake_game();
}

/* Outside Boss Battle the Inn, the arrival and the game's own Options page run exactly as the game has them. */
static void test_normal_inn_untouched(void) {
    start_guarded();
    use_fake_inn_functions();
    use_fake_screen_functions();
    use_fake_game();
    set_inn(7, 7, -1, 0);
    options_leave_frame();
    CHECK(arrival_fade().rsi == 0);
    fake_options_page(8);
    build_page();
    CHECK(!boss_battle_screen_shown() && i32_at(fake_body, 0xc50) == 1);
    options_leave_frame();
    const GuestRegs menu = result_frame();
    CHECK((int32_t)menu.rax == -1 && !fades.calls && !guard_log[0] && !arena_request.calls && !party_sends);
    release_fake_game();
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

/* ---- character progression: shared with the campaign, the world kept apart ---- */

enum { RECORD_AT = 0x12bc0, WORLD_FLAG = 0x70, SAVE_SLOTS = 0x50 };
static uint8_t fake_saver[0x100], *fake_disk;
static int commits, save_requests;

static uint8_t *campaign_record(void) {
    return fake_play_data + RECORD_AT;
}

static const uint8_t *GUEST_HOOK_ABI fake_lead_selector(uint64_t facade) {
    (void)facade;
    return campaign_record();
}

static void GUEST_HOOK_ABI fake_record_level(uint64_t record, uint32_t level) {
    ((uint8_t *)(uintptr_t)record)[0x26] = (uint8_t)level;
}
static int GUEST_HOOK_ABI fake_level_pattern(uint64_t record) {
    return ((const uint8_t *)(uintptr_t)record)[0x27];
}
/* the level's stats, as a marker the tests can tell apart */
static void GUEST_HOOK_ABI fake_write_stats(uint64_t record, int pattern, int unused, int level, int unused2) {
    (void)pattern, (void)unused, (void)unused2;
    put_u32((uint8_t *)(uintptr_t)record + 0x0c, 1000u + (uint32_t)level);
    put_u32((uint8_t *)(uintptr_t)record + 0x10, 1000u + (uint32_t)level);
}

/* As the game's commit and save act: the user's play data into the save's current slot, then the slot to disk. */
static void GUEST_HOOK_ABI fake_commit(uint64_t save) {
    uint8_t *object = (uint8_t *)(uintptr_t)save;
    ++commits;
    if (object[0x20eb14] > 2) return;
    memcpy(object + SAVE_SLOTS + (size_t)object[0x20eb14] * BOSS_BATTLE_PLAY_DATA_BYTES,
           (const void *)(uintptr_t)fake_play_table[object[0x20eb15]], BOSS_BATTLE_PLAY_DATA_BYTES);
}
static uint8_t GUEST_HOOK_ABI fake_save_request(uint64_t saver) {
    uint8_t *object = (uint8_t *)(uintptr_t)saver;
    if (object[0x5b]) return 0;
    ++save_requests;
    object[0x5b] = 1;
    put_u32(object + 0x70, 0);
    memcpy(fake_disk, fake_save + SAVE_SLOTS, BOSS_BATTLE_PLAY_DATA_BYTES);
    return 1;
}

static int32_t record_i32(int offset) {
    return i32_at(campaign_record(), offset);
}
static int record_level(void) {
    return campaign_record()[0x26];
}
static int record_skill_points(void) {
    int16_t value;
    memcpy(&value, campaign_record() + 0x24, sizeof(value));
    return value;
}

/* A campaign with Zak the Fighter at `level` (from 0) in its play data, the Inn's game manager holding the save and the
 * save manager, and the real campaign snapshot. */
static void progression_campaign(int level, int32_t xp_progress, int16_t skill_points) {
    fake_campaign();
    if (!fake_disk) fake_disk = calloc(1, BOSS_BATTLE_PLAY_DATA_BYTES);
    fake_save[0x20eb14] = 0;
    uint8_t *record = campaign_record();
    put_u32(record + 0x04, 0);
    put_u32(record + 0x08, (uint32_t)xp_progress);
    put_u32(record + 0x0c, 1000u + (uint32_t)level);
    put_u32(record + 0x10, 1000u + (uint32_t)level);
    memcpy(record + 0x24, &skill_points, sizeof(skill_points));
    record[0x26] = (uint8_t)level;
    record[0x27] = 0;
    memcpy(record + 0x138, "Zak\0\0\0\0\0", 8);
    start_guarded();
    boss_battle_set_memory_guard(boss_battle_campaign_snapshot, boss_battle_campaign_restore);
    use_fake_inn_functions();
    use_fake_screen_functions();
    use_fake_game();
    put_pointer(game_manager + 0xe0, fake_saver);
    put_pointer(game_manager + 0xe8, fake_save);
    memset(fake_saver, 0, sizeof(fake_saver));
    boss_battle_set_lead_selector(fake_lead_selector);
    const BossBattleLevelFunctions levels = {fake_record_level, fake_level_pattern, fake_write_stats};
    boss_battle_set_level_functions(&levels);
    const BossBattleSaveFunctions saves = {fake_commit, fake_save_request};
    boss_battle_set_save_functions(&saves);
    commits = save_requests = 0;
}

static void release_progression(void) {
    boss_battle_set_lead_selector(NULL);
    boss_battle_set_level_functions(NULL);
    boss_battle_set_save_functions(NULL);
    release_fake_game();
    boss_battle_campaign_reset();
}

/* The page opened from the Inn's Boss Battle row. */
static void open_page(void) {
    open_boss_screen(1);
}

/* The page the Inn opens again after a fight: its menu frame takes the reopen, then the Options page is built. */
static void reopened_page(void) {
    set_inn(7, 7, -1, 0);
    result_frame();
    fake_options_page(8);
    build_page();
    set_page_cursor(1);
}

/* A frame of the shown page. */
static void page_frame(void) {
    GuestRegs regs = page_regs();
    boss_battle_options_update_hook(&regs);
}

/* Harpy confirmed on the shown page, and the Inn's next menu frame starts her fight. */
static void start_harpy_from_page(void) {
    CHECK(boss_battle_screen_shown() && boss_battle_screen_boss(1) == HARPY);
    page_result(1);
    fake_inn[INN_ACT] = 0x8e;
    fake_inn[INN_STEP] = 3;
    options_leave_frame();
    set_inn(7, 7, -1, BOSS_BATTLE_INN_ROW);
    fake_inn[INN_ACT] = 0xa4;
    fake_inn[INN_STEP] = 1;
    result_frame();
    CHECK(boss_battle_session_active() && boss_battle_campaign_held());
}

static void reach_harpy_arena(int playable) {
    BossBattleScene loading = kArenaPlay;
    loading.act = 4;
    loading.next_stage = kArenaPlay.stage;
    frames(probe(kInnScene, 0, 0, 0), 3);
    frames(probe(loading, 0, 0, 0), 3);
    loading.next_stage = -1;
    frames(probe(loading, 0, 0, 0), 1);
    frames(probe(kArenaPlay, 1, 1, playable), 1);
}

/* The fight's way back: the Inn requested, then the Inn constructor's hook (party records back in the play data). */
static void arrive_at_inn(void) {
    CHECK(frames(probe(kArenaPlay, 1, 1, 0), 1) == BOSS_BATTLE_FRAME_REQUEST_INN);
    boss_battle_inn_requested();
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    boss_battle_inn_flushed_hook(&regs);
    CHECK(!boss_battle_session_active() && !boss_battle_campaign_held());
}

/* What the stage and the native result leave behind in a fight: world progress, transient state, and a level-up. */
static void fight_changes_world(void) {
    memset(fake_play_data + WORLD_FLAG, 0x5a, 16);            /* a boss-clear / story flag in the play data */
    memset(fake_save + SAVE_SLOTS + WORLD_FLAG, 0x5b, 16);    /* and in the save's slot */
    put_u32(campaign_record() + 0x10, 7);                     /* HP after the fight */
    campaign_record()[0x30] = 0x99;                           /* other record state of the fight */
}

static void native_level_up(int levels, int32_t progress_after, int32_t pool_after) {
    uint8_t *record = campaign_record();
    put_u32(record + 0x04, (uint32_t)pool_after);
    put_u32(record + 0x08, (uint32_t)progress_after);
    for (int i = 0; i < levels; ++i) {
        const int16_t skill_points = (int16_t)(record_skill_points() + 1);
        memcpy(record + 0x24, &skill_points, sizeof(skill_points));
        fake_record_level((uint64_t)(uintptr_t)record, (uint32_t)record[0x26] + 1);
        fake_write_stats((uint64_t)(uintptr_t)record, 0, 0, record[0x26], 0);
    }
}

static void finish_save(int error) {
    fake_saver[0x5b] = 0;
    put_u32(fake_saver + 0x70, (uint32_t)error);
    page_frame();
}

static void test_character_progression(void) {
    char text[160];

    /* campaign level 1 enters at level 1; a Win's native level-up to 2 survives the campaign restore */
    progression_campaign(0, 40, 0);
    open_page();
    CHECK(boss_battle_character_text(text, sizeof(text)) && !strcmp(text, "Zak - Fighter, level 1"));
    start_harpy_from_page();
    CHECK(record_level() == 0 && record_i32(0x08) == 40); /* the fight's character is the campaign's */
    reach_harpy_arena(1);
    CHECK(boss_battle_state() == BOSS_BATTLE_FIGHTING);
    fight_changes_world();
    boss_battle_boss_cleared(1);
    native_level_up(1, 15, 0);
    arrive_at_inn();
    CHECK(record_level() == 1 && record_i32(0x08) == 15 && record_i32(0x04) == 0 && record_skill_points() == 1);
    CHECK(record_i32(0x0c) == 1001 && record_i32(0x10) == 1001); /* level 2's stats, not the fight's HP */
    CHECK(campaign_record()[0x30] == 0x22);                       /* nothing else of the record */
    CHECK(fake_play_data[WORLD_FLAG] == 0x22 && fake_save[SAVE_SLOTS + WORLD_FLAG] == 0x11); /* world put back */
    CHECK(boss_battle_progress_save_state() == BOSS_BATTLE_SAVE_PENDING && !commits);
    CHECK(stats_of(HARPY).wins >= 1);

    /* the reopened page reads the character again; the game's own save takes the merged character to disk */
    reopened_page();
    CHECK(boss_battle_character_text(text, sizeof(text)) && !strcmp(text, "Zak - Fighter, level 2"));
    CHECK(commits == 1 && save_requests == 1 && boss_battle_progress_save_state() == BOSS_BATTLE_SAVE_RUNNING);
    CHECK(fake_save[SAVE_SLOTS + RECORD_AT + 0x26] == 1 && fake_save[SAVE_SLOTS + WORLD_FLAG] == 0x22);
    page_frame();
    CHECK(save_requests == 1); /* the save act still runs */
    finish_save(0);
    CHECK(boss_battle_progress_save_state() == BOSS_BATTLE_SAVE_IDLE && save_requests == 1);

    /* a restart loads the saved slot: still level 2, world as the campaign had it */
    memset(fake_play_data, 0, BOSS_BATTLE_PLAY_DATA_BYTES);
    memcpy(fake_play_data, fake_disk, BOSS_BATTLE_PLAY_DATA_BYTES);
    CHECK(record_level() == 1 && record_skill_points() == 1 && record_i32(0x08) == 15 && fake_play_data[WORLD_FLAG] == 0x22);

    /* the second fight starts at level 2; a Death gives nothing and takes nothing */
    const BossBattleStats before_death = stats_of(HARPY);
    start_harpy_from_page();
    CHECK(record_level() == 1 && record_i32(0x08) == 15);
    reach_harpy_arena(1);
    fight_changes_world();
    put_u32(campaign_record() + 0x08, 99); /* a change without a Win */
    CHECK(boss_battle_party_wiped());
    arrive_at_inn();
    CHECK(record_level() == 1 && record_i32(0x08) == 15 && record_skill_points() == 1 && fake_play_data[WORLD_FLAG] == 0x22);
    CHECK(boss_battle_progress_save_state() == BOSS_BATTLE_SAVE_IDLE && stats_of(HARPY).deaths == before_death.deaths + 1);

    /* Quit after Playable and a cancel before it: the same */
    reopened_page();
    start_harpy_from_page();
    reach_harpy_arena(1);
    put_u32(campaign_record() + 0x08, 77);
    CHECK(boss_battle_quit_key(0, 0, 0));
    arrive_at_inn();
    CHECK(record_level() == 1 && record_i32(0x08) == 15 && boss_battle_progress_save_state() == BOSS_BATTLE_SAVE_IDLE);
    reopened_page();
    start_harpy_from_page();
    reach_harpy_arena(0);
    CHECK(boss_battle_state() == BOSS_BATTLE_INITIALIZING && boss_battle_quit_key(0, 0, 0));
    arrive_at_inn();
    CHECK(record_level() == 1 && record_i32(0x08) == 15 && boss_battle_progress_save_state() == BOSS_BATTLE_SAVE_IDLE);

    /* a Win without a level-up keeps its experience; the fight's HP stays out */
    reopened_page();
    start_harpy_from_page();
    reach_harpy_arena(1);
    fight_changes_world();
    boss_battle_boss_cleared(1);
    native_level_up(0, 60, 0);
    arrive_at_inn();
    CHECK(record_level() == 1 && record_i32(0x08) == 60 && record_i32(0x10) == 1001 && fake_play_data[WORLD_FLAG] == 0x22);
    CHECK(boss_battle_progress_save_state() == BOSS_BATTLE_SAVE_PENDING);
    reopened_page();
    finish_save(1); /* a failed save is reported; the character stays merged in memory */
    CHECK(boss_battle_progress_save_state() == BOSS_BATTLE_SAVE_IDLE && record_i32(0x08) == 60);
    release_progression();

    /* campaign level 10 enters at level 10: nothing of Boss Battle lowers it */
    progression_campaign(9, 500, 9);
    open_page();
    CHECK(boss_battle_character_text(text, sizeof(text)) && !strcmp(text, "Zak - Fighter, level 10"));
    start_harpy_from_page();
    CHECK(record_level() == 9 && record_i32(0x08) == 500 && record_skill_points() == 9);
    reach_harpy_arena(1);
    boss_battle_boss_cleared(1);
    native_level_up(2, 3, 0);
    arrive_at_inn();
    CHECK(record_level() == 11 && record_skill_points() == 11 && record_i32(0x0c) == 1011);
    release_progression();

    /* a record that does not look like a native level-up of the same character is not merged */
    progression_campaign(0, 40, 0);
    open_page();
    start_harpy_from_page();
    reach_harpy_arena(1);
    boss_battle_boss_cleared(1);
    native_level_up(1, 15, 0);
    const int16_t five = 5;
    memcpy(campaign_record() + 0x24, &five, sizeof(five)); /* five skill points from one level */
    arrive_at_inn();
    CHECK(record_level() == 0 && record_i32(0x08) == 40 && boss_battle_progress_save_state() == BOSS_BATTLE_SAVE_IDLE);
    reopened_page();
    start_harpy_from_page();
    reach_harpy_arena(1);
    boss_battle_boss_cleared(1);
    native_level_up(1, 15, 0);
    memcpy(campaign_record() + 0x138, "Tia\0\0\0\0\0", 8); /* another character */
    arrive_at_inn();
    CHECK(record_level() == 0 && !memcmp(campaign_record() + 0x138, "Zak", 3));
    CHECK(boss_battle_progress_save_state() == BOSS_BATTLE_SAVE_IDLE);

    /* a restore that fails merges nothing and keeps the saves on the campaign kept before the fight */
    reopened_page();
    start_harpy_from_page();
    reach_harpy_arena(1);
    boss_battle_boss_cleared(1);
    native_level_up(1, 15, 0);
    fake_play_table[1] = (uint64_t)(uintptr_t)other_play_data;
    CHECK(frames(probe(kArenaPlay, 1, 1, 0), 1) == BOSS_BATTLE_FRAME_REQUEST_INN);
    boss_battle_inn_requested();
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    boss_battle_inn_flushed_hook(&regs);
    CHECK(!boss_battle_session_active() && boss_battle_progress_save_state() == BOSS_BATTLE_SAVE_IDLE);
    CHECK(boss_battle_campaign_save_source((uint64_t)(uintptr_t)fake_save) != (uint64_t)(uintptr_t)fake_save);
    release_progression();
}

/* ---- the pause menu of a fight ---- */

enum {
    PAUSE_WINDOW_BYTES = 0x1c00,
    PAUSE_ROW_BYTES = 0x5c0,
    PAUSE_LIST = 0x1520,
    PAUSE_TITLE = 0xbf0,
    PAUSE_MODE = 0x1510,
    PAUSE_STATE = 0xab,
    PAUSE_RESULT = 0xadf4,
    PAUSE_MAX_ROWS = 4,
    MESSAGE_CHECK_REQUESTS = 0x4aa,
    MESSAGE_QUIT_ADVENTURE = 0x4a8,
    MESSAGE_EXTRA = 0x4ea
};
static uint8_t pause_window[PAUSE_WINDOW_BYTES], pause_rows[PAUSE_MAX_ROWS][PAUSE_ROW_BYTES], pause_dialog[0x800];
static uint64_t pause_row_list[PAUSE_MAX_ROWS];
static int pause_built[PAUSE_MAX_ROWS], pause_built_count, pause_setups, pause_mode_seen = -1;

static const char *const kNativeTitle = "Ancient Temple Ruins";

/* The game's setup: it builds its rows through the row builder, whatever the hook there leaves of them, and titles the
 * menu with its location. */
static void GUEST_HOOK_ABI fake_pause_setup(uint64_t window) {
    static const int kMessages[3] = {MESSAGE_CHECK_REQUESTS, MESSAGE_QUIT_ADVENTURE, MESSAGE_EXTRA};
    static const int kActions[3] = {2, 1, 9};
    uint8_t *base = (uint8_t *)(uintptr_t)window;
    ++pause_setups;
    pause_mode_seen = i32_at(base, PAUSE_MODE);
    pause_built_count = 0;
    put_u32(base + PAUSE_LIST + 0x68, 0);
    for (int i = 0; i < 3; ++i) {
        GuestRegs regs;
        memset(&regs, 0, sizeof(regs));
        regs.rdi = window + PAUSE_LIST;
        regs.rsi = (uint32_t)kMessages[i];
        boss_battle_pause_row_hook(&regs);
        if (regs.skip) continue;
        uint8_t *row = pause_rows[pause_built_count];
        memset(row, 0, PAUSE_ROW_BYTES);
        put_u32(row + 0x5b4, (uint32_t)kActions[i]);
        pause_row_list[pause_built_count] = (uint64_t)(uintptr_t)row;
        pause_built[pause_built_count++] = kMessages[i];
    }
    put_pointer(base + PAUSE_LIST + 0x68, pause_row_list);
    put_pointer(base + PAUSE_LIST + 0x70, pause_row_list + pause_built_count);
    fake_set_text(window + PAUSE_TITLE, kNativeTitle);
}

/* As the game's accessor: -1 while the menu is open, the answer once it closed. */
static int32_t GUEST_HOOK_ABI fake_pause_result(uint64_t window, uint32_t pending) {
    (void)pending;
    const uint8_t *base = (const uint8_t *)(uintptr_t)window;
    return base[PAUSE_STATE] == 0x16 ? i32_at(base, PAUSE_RESULT) : -1;
}

static void use_fake_pause_functions(void) {
    const BossBattlePauseFunctions functions = {fake_pause_setup, fake_pause_result, PAUSE_RESULT};
    boss_battle_set_pause_functions(&functions);
    memset(pause_window, 0, sizeof(pause_window));
    memset(pause_dialog, 0, sizeof(pause_dialog));
    pause_setups = pause_built_count = 0;
    pause_mode_seen = -1;
}

static GuestRegs pause_call(void (GUEST_HOOK_ABI *hook)(GuestRegs *), uint64_t rdi, uint64_t rsi) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rdi = rdi;
    regs.rsi = rsi;
    regs.rax = 0x1234;
    regs.rcx = 0x5678;
    hook(&regs);
    return regs;
}

static uint64_t window_address(void) {
    return (uint64_t)(uintptr_t)pause_window;
}

/* Start on a fight: the gates pass, the scene reads the plain kind, the setup builds the menu. */
static void open_pause_menu(void) {
    GuestRegs gate = pause_call(boss_battle_pause_start_gate_hook, 0, 0);
    CHECK(gate.skip && gate.rax == 1);
    gate = pause_call(boss_battle_pause_level_gate_hook, 0, 0);
    CHECK(gate.skip && gate.rax == 5);
    GuestRegs mode = pause_call(boss_battle_pause_mode_hook, 0, 0);
    CHECK(mode.rcx != 0x5678 && i32_at((const uint8_t *)(uintptr_t)mode.rcx, 0x108) == 0 &&
          i32_at((const uint8_t *)(uintptr_t)mode.rcx, 0xf0) == 0);
    put_u32(pause_window + PAUSE_MODE, 3); /* what the scene's kind would have chosen */
    const GuestRegs setup = pause_call(boss_battle_pause_setup_hook, window_address(), 0);
    CHECK(setup.skip && pause_setups >= 1);
}

static void check_pause_rows(const char *title) {
    CHECK(pause_mode_seen == 0); /* the plain adventure menu */
    CHECK(!strcmp(text_at(pause_window, PAUSE_TITLE), title) && strcmp(text_at(pause_window, PAUSE_TITLE), kNativeTitle));
    CHECK(!isdigit((unsigned char)title[0]) && !strstr(title, " - ")); /* no number */
    CHECK(pause_built_count == 1 && pause_built[0] == MESSAGE_QUIT_ADVENTURE); /* no Check requests, no other row */
    CHECK(i32_at(pause_rows[0], 0x5b4) == 1 && !strcmp(text_at(pause_rows[0], 0x370), "Quit Boss Battle"));
    CHECK(!strstr(text_at(pause_rows[0], 0x370), "adventure") && !strstr(text_at(pause_rows[0], 0x370), "requests"));
    const uint8_t *list = pause_window + PAUSE_LIST;
    uint64_t begin, end;
    memcpy(&begin, list + 0x68, 8);
    memcpy(&end, list + 0x70, 8);
    CHECK((end - begin) / 8 == 1); /* nothing else to select */
}

static void close_pause(int32_t answer) {
    pause_window[PAUSE_STATE] = 0x16;
    put_u32(pause_window + PAUSE_RESULT, (uint32_t)answer);
}

static BossBattleStats stats_for(const char *class_name) {
    char key[64];
    snprintf(key, sizeof(key), "Zak|%s", class_name);
    return boss_battle_record_stats(key, "harpy");
}

static BossBattleScene arena_of(int index) {
    return (BossBattleScene){2, 0, kArenas[index].round, kArenas[index].first, -1, 0};
}

static void test_pause_menu(void) {
    static const struct {
        const char *class_name;
        int class_index, level;
    } kCharacters[] = {{"Fighter", 0, 1},   {"Amazon", 1, 25},    {"Elf", 2, 99},
                       {"Dwarf", 3, 1},     {"Sorceress", 4, 25}, {"Wizard", 5, 99}};
    GuestRegs regs;

    /* the campaign's own pause menu is not touched */
    start_guarded();
    use_fake_screen_functions();
    use_fake_pause_functions();
    CHECK(!boss_battle_pause_available() && !boss_battle_pause_owned());
    regs = pause_call(boss_battle_pause_start_gate_hook, 0, 0);
    CHECK(!regs.skip && regs.rax == 0x1234);
    regs = pause_call(boss_battle_pause_level_gate_hook, 0, 0);
    CHECK(!regs.skip && regs.rax == 0x1234);
    regs = pause_call(boss_battle_pause_mode_hook, 0, 0);
    CHECK(regs.rcx == 0x5678);
    regs = pause_call(boss_battle_pause_row_hook, 0, MESSAGE_CHECK_REQUESTS);
    CHECK(!regs.skip);
    regs = pause_call(boss_battle_pause_confirm_hook, (uint64_t)(uintptr_t)pause_dialog, 0x4ac);
    CHECK(!regs.skip && !text_sets);
    put_u32(pause_window + PAUSE_MODE, 3);
    fake_pause_setup(window_address()); /* what the game's own setup does, its rows all built */
    regs = pause_call(boss_battle_pause_setup_hook, window_address(), 0);
    CHECK(!regs.skip && pause_built_count == 3 && pause_mode_seen == 3 && !strcmp(text_at(pause_window, PAUSE_TITLE), kNativeTitle));
    close_pause(1);
    regs = pause_call(boss_battle_pause_result_hook, window_address(), 0);
    CHECK(!regs.skip && regs.rax == 0x1234 && i32_at(pause_window, PAUSE_RESULT) == 1 && !boss_battle_quit_available());
    CHECK(se_calls == 0);

    /* every class and level: the same menu, its one row quits as F4 does, Back does not */
    for (size_t c = 0; c < sizeof(kCharacters) / sizeof(*kCharacters); ++c) {
        start_guarded();
        use_fake_screen_functions();
        use_fake_pause_functions();
        boss_battle_set_character("Zak", kCharacters[c].class_index, kCharacters[c].level);
        CHECK(start_session(HARPY));
        reach_arena();
        frames(probe(kArenaPlay, 1, 1, 0), 10);
        CHECK(boss_battle_state() == BOSS_BATTLE_INITIALIZING && boss_battle_pause_available());
        char title[64];
        CHECK(boss_battle_pause_opened(title, sizeof(title)) && !strcmp(title, "Harpy"));
        boss_battle_pause_closed();

        /* before Playable: Quit Boss Battle cancels, no Attempt and no outcome */
        const BossBattleStats early = stats_for(kCharacters[c].class_name);
        open_pause_menu();
        check_pause_rows("Harpy");
        CHECK(boss_battle_pause_owned() && boss_battle_quit_available());
        regs = pause_call(boss_battle_pause_confirm_hook, (uint64_t)(uintptr_t)pause_dialog, 0x4ac);
        CHECK(regs.skip && message_value == 0x4ac && !strcmp(text_at(pause_dialog, 0), "Quit Boss Battle?"));
        regs = pause_call(boss_battle_pause_result_hook, window_address(), 0);
        CHECK(regs.skip && (int32_t)regs.rax == -1 && boss_battle_pause_owned() && boss_battle_quit_available());
        close_pause(1);
        regs = pause_call(boss_battle_pause_result_hook, window_address(), 0);
        CHECK(regs.skip && (int32_t)regs.rax == -1 && i32_at(pause_window, PAUSE_RESULT) == -1 && !boss_battle_pause_owned());
        CHECK(!boss_battle_quit_available()); /* the one quit is pending */
        regs = pause_call(boss_battle_pause_result_hook, window_address(), 0);
        CHECK(!regs.skip); /* the menu is no longer Boss Battle's */
        CHECK(frames(probe(kArenaPlay, 1, 1, 0), 1) == BOSS_BATTLE_FRAME_REQUEST_INN);
        boss_battle_inn_requested();
        boss_battle_inn_arrived();
        CHECK(boss_battle_outcome() == BOSS_BATTLE_OUTCOME_CANCELLED);
        CHECK(stats_for(kCharacters[c].class_name).attempts == early.attempts &&
              stats_for(kCharacters[c].class_name).quits == early.quits);

        /* at Playable: Back resumes without quitting, Quit Boss Battle records exactly one Quit */
        start_guarded();
        use_fake_screen_functions();
        use_fake_pause_functions();
        boss_battle_set_character("Zak", kCharacters[c].class_index, kCharacters[c].level);
        const BossBattleStats before = stats_for(kCharacters[c].class_name);
        CHECK(start_playable(HARPY, kArenaPlay));
        open_pause_menu();
        check_pause_rows("Harpy");
        close_pause(-1);
        regs = pause_call(boss_battle_pause_result_hook, window_address(), 0);
        CHECK(regs.skip && (int32_t)regs.rax == -1 && !boss_battle_pause_owned() && boss_battle_quit_available());
        CHECK(boss_battle_state() == BOSS_BATTLE_FIGHTING && stats_for(kCharacters[c].class_name).quits == before.quits);
        memset(pause_window, 0, sizeof(pause_window));
        open_pause_menu();
        close_pause(1);
        regs = pause_call(boss_battle_pause_result_hook, window_address(), 0);
        pause_call(boss_battle_pause_result_hook, window_address(), 0);
        CHECK((int32_t)regs.rax == -1 && !boss_battle_quit_available());
        CHECK(frames(probe(kArenaPlay, 1, 1, 1), 1) == BOSS_BATTLE_FRAME_REQUEST_INN);
        boss_battle_inn_requested();
        boss_battle_inn_arrived();
        CHECK(boss_battle_outcome() == BOSS_BATTLE_OUTCOME_QUIT);
        const BossBattleStats after = stats_for(kCharacters[c].class_name);
        CHECK(after.quits == before.quits + 1 && after.attempts == before.attempts + 1 && after.wins == before.wins);
    }

    /* the same quit as F4: the same outcome and statistics */
    start_guarded();
    use_fake_screen_functions();
    use_fake_pause_functions();
    BossBattleStats mark = harpy_stats();
    CHECK(start_playable(HARPY, kArenaPlay) && boss_battle_quit_key(0, 0, 0));
    CHECK(frames(probe(kArenaPlay, 1, 1, 1), 1) == BOSS_BATTLE_FRAME_REQUEST_INN);
    boss_battle_inn_requested();
    boss_battle_inn_arrived();
    const BossBattleStats by_key = harpy_stats();
    const BossBattleOutcomeKind key_outcome = boss_battle_outcome();
    CHECK(by_key.quits == mark.quits + 1);
    mark = harpy_stats();
    start_guarded();
    use_fake_screen_functions();
    use_fake_pause_functions();
    CHECK(start_playable(HARPY, kArenaPlay));
    open_pause_menu();
    close_pause(1);
    pause_call(boss_battle_pause_result_hook, window_address(), 0);
    CHECK(frames(probe(kArenaPlay, 1, 1, 1), 1) == BOSS_BATTLE_FRAME_REQUEST_INN);
    boss_battle_inn_requested();
    boss_battle_inn_arrived();
    CHECK(boss_battle_outcome() == key_outcome && harpy_stats().quits == mark.quits + 1 &&
          harpy_stats().attempts == mark.attempts + 1);

    /* the title is the boss, never the stage's name, whatever the stage holds */
    for (int boss = 0; boss < 18; ++boss) {
        start_guarded();
        use_fake_screen_functions();
        use_fake_pause_functions();
        CHECK(start_playable(boss, arena_of(boss)));
        open_pause_menu();
        check_pause_rows(boss_battle_encounter(boss)->name);
        boss_battle_reset();
    }
    start_guarded();
    use_fake_screen_functions();
    use_fake_pause_functions();
    CHECK(start_playable(GOLEM, arena_of(GOLEM)));
    open_pause_menu();
    CHECK(!strcmp(text_at(pause_window, PAUSE_TITLE), "Golem"));
    boss_battle_reset();

    /* not Boss Battle's outside a fight: travelling, resolving and returning leave the game's menu */
    start_guarded();
    use_fake_screen_functions();
    use_fake_pause_functions();
    CHECK(start_session(HARPY) && !boss_battle_pause_available());
    regs = pause_call(boss_battle_pause_start_gate_hook, 0, 0);
    CHECK(!regs.skip);
    boss_battle_reset();
    CHECK(!boss_battle_pause_available() && !boss_battle_pause_owned());
    regs = pause_call(boss_battle_pause_setup_hook, window_address(), 0);
    CHECK(!regs.skip && !strcmp(text_at(pause_window, PAUSE_TITLE), ""));

    /* a menu open when the fight is won stays Boss Battle's until it closes; the session's end drops it */
    start_guarded();
    use_fake_screen_functions();
    use_fake_pause_functions();
    CHECK(start_playable(HARPY, kArenaPlay));
    open_pause_menu();
    boss_battle_boss_cleared(1);
    CHECK(boss_battle_state() == BOSS_BATTLE_RESOLVING && !boss_battle_pause_available() && boss_battle_pause_owned());
    regs = pause_call(boss_battle_pause_row_hook, 0, MESSAGE_CHECK_REQUESTS);
    CHECK(regs.skip);
    finish_at_inn();
    CHECK(!boss_battle_pause_owned());
    CHECK(se_calls == 0); /* the menu's sounds are the game's */
    boss_battle_reset();
    boss_battle_set_screen_functions(NULL);
    boss_battle_set_pause_functions(NULL);
}

/* ---- Fight All ---- */

static BossBattleScene boss_arena(int boss) {
    const BossBattleEncounter *encounter = boss_battle_encounter(boss);
    const BossBattleScene scene = {2, 0, encounter->round_index, encounter->stage, -1, 0};
    return scene;
}

static BossBattleFightAllStats run_stats(void) {
    return boss_battle_record_fight_all_stats("Zak|Fighter");
}

static BossBattleCharacterProgress fa_character; /* the campaign's record, as the state-level runs keep it */

static BossBattleCharacterProgress character_at(int level, int pool, int progress, int skill_points) {
    BossBattleCharacterProgress character = {pool, progress, skill_points, level, 0, "Zak"};
    return character;
}

/* The page confirmed Fight All: the run is queued and the screen closes as for a chosen boss. */
static void fa_select_keeping_records(void) {
    start_guarded();
    boss_battle_screen_request();
    boss_battle_screen_built();
    fa_character = character_at(0, 0, 40, 0);
    int index = -1;
    CHECK(boss_battle_fight_all_request() == 1 && boss_battle_screen_closed(&index) && index == HARPY);
}

static void fa_select(void) {
    boss_battle_record_clear();
    fa_select_keeping_records();
}

/* The travel to boss `boss` under the held black, its native arrival fade-in run to its end, then its first played
 * frame; nothing but that fade reveals it. */
static void arrive_through_native_fade(int boss) {
    BossBattleScene arena = boss_arena(boss), loading = arena;
    loading.act = 4;
    loading.next_stage = arena.stage;
    frames(probe(kInnScene, 0, 0, 0), 3);
    frames(probe(loading, 0, 0, 0), 3);
    loading.next_stage = -1;
    frames(probe(loading, 0, 0, 0), 1);
    CHECK(boss_battle_start_transition() == BOSS_BATTLE_START_HELD);
    const int fades_before = fades.calls;
    native_arena_arrival();
    CHECK(!boss_battle_start_reveal_finished(1));
    CHECK(boss_battle_start_reveal_finished(0) && boss_battle_start_transition() == BOSS_BATTLE_START_DONE);
    frames(probe(arena, 1, 1, 1), 1);
    CHECK(fades.calls == fades_before && boss_battle_state() == BOSS_BATTLE_FIGHTING);
}

static void reach_boss_arena(int boss, int playable) {
    BossBattleScene arena = boss_arena(boss), loading = arena;
    loading.act = 4;
    loading.next_stage = arena.stage;
    frames(probe(kInnScene, 0, 0, 0), 3);
    frames(probe(loading, 0, 0, 0), 3);
    loading.next_stage = -1;
    frames(probe(loading, 0, 0, 0), 1);
    frames(probe(arena, 1, 1, playable), 1);
}

typedef enum { WIN_LEVEL_UP, WIN_XP_ONLY, WIN_NO_CHANGE, WIN_NOT_NATIVE } WinKind;

/* Boss `boss` of the run: started as the Inn starts it, played to Playable, won after `fight` frames of combat, then back
 * at the Inn as the Inn constructor's hook sees it: result progression captured, campaign restored, progression merged. */
static void fa_win(int boss, int fight, WinKind kind) {
    CHECK(start_session(boss));
    boss_battle_progress_before(&fa_character);
    reach_boss_arena(boss, 1);
    CHECK(boss_battle_state() == BOSS_BATTLE_FIGHTING);
    frames(probe(boss_arena(boss), 1, 1, 1), fight);
    boss_battle_boss_cleared(1);
    CHECK(boss_battle_outcome() == BOSS_BATTLE_OUTCOME_WIN);
    BossBattleCharacterProgress after = fa_character;
    switch (kind) {
    case WIN_LEVEL_UP: ++after.level, ++after.skill_points, after.xp_progress += 7; break;
    case WIN_XP_ONLY: after.xp_progress += 7; break;
    case WIN_NOT_NATIVE: after.level += 1, after.skill_points += 5; break;
    case WIN_NO_CHANGE: break;
    }
    CHECK(frames(probe(boss_arena(boss), 1, 1, 0), 1) == BOSS_BATTLE_FRAME_REQUEST_INN);
    boss_battle_inn_requested();
    boss_battle_progress_after(&after);
    boss_battle_inn_arrived();
    BossBattleCharacterProgress merged;
    if (boss_battle_progress_merge(&fa_character, &merged)) {
        CHECK(merged.level == after.level && merged.xp_progress == after.xp_progress && merged.skill_points == after.skill_points);
        fa_character = merged;
    }
}

/* Plays the game's save for a pending character save. */
static void fa_run_save(int ok) {
    if (boss_battle_progress_save_state() == BOSS_BATTLE_SAVE_PENDING) boss_battle_progress_save_started();
    if (boss_battle_progress_save_state() == BOSS_BATTLE_SAVE_RUNNING) boss_battle_progress_save_finished(ok);
}

/* The scene manager of the fake game, as the hooks read it. */
static void set_game_scene(BossBattleScene scene) {
    game_scene[0x5b] = (uint8_t)scene.act;
    game_scene[0x5c] = (uint8_t)scene.step;
    put_u32(game_scene + 0x108, (uint32_t)scene.round);
    put_u32(game_scene + 0xf0, (uint32_t)scene.stage);
    put_u32(game_scene + 0xf8, (uint32_t)scene.next_stage);
    put_u32(game_scene + 0xf4, (uint32_t)scene.entry);
}

/* An Inn menu frame of a checkpoint with the Inn scene ready to travel. */
static BossBattleFightAllStep fa_poll(int *next) {
    return boss_battle_fight_all_poll(&kInnScene, 0, next);
}

/* The Inn's menu frames of a checkpoint until the poll decides. */
static BossBattleFightAllStep fa_checkpoint(int *next) {
    BossBattleFightAllStep step = fa_poll(next);
    for (int guard = 0; step == BOSS_BATTLE_FIGHT_ALL_STEP_WAIT && guard < 4; ++guard) {
        fa_run_save(1);
        step = fa_poll(next);
    }
    return step;
}

/* The player confirms Fight All; Harpy is requested, nothing is recorded as a run until she is playable. */
static void test_fight_all_start(void) {
    fa_select();
    BossBattleFightAllState state = boss_battle_fight_all_state();
    CHECK(state.active && !state.run_started && !state.checkpoint && state.boss == 0 && state.cleared == 0 && state.run == 1);
    CHECK(!boss_battle_session_active() && run_stats().runs == 0 && state.result == BOSS_BATTLE_FIGHT_ALL_NONE);
    CHECK(start_session(HARPY));
    CHECK(boss_battle_state() == BOSS_BATTLE_TRAVELLING && boss_battle_encounter(0) == boss_battle_encounter(state.boss));
    reach_boss_arena(HARPY, 0);
    CHECK(boss_battle_state() == BOSS_BATTLE_INITIALIZING && run_stats().runs == 0 && stats_of(HARPY).attempts == 0);
    frames(probe(boss_arena(HARPY), 1, 1, 1), 1);
    state = boss_battle_fight_all_state();
    CHECK(boss_battle_state() == BOSS_BATTLE_FIGHTING && state.run_started && state.boss == 0);
    CHECK(run_stats().runs == 1 && stats_of(HARPY).attempts == 1);
    frames(probe(boss_arena(HARPY), 1, 1, 1), 30);
    CHECK(run_stats().runs == 1 && stats_of(HARPY).attempts == 1 && boss_battle_fight_all_state().boss == 0);
    boss_battle_reset();

    /* a page that was never confirmed starts nothing; one run at a time; the title ends a queued run */
    start_guarded();
    CHECK(boss_battle_fight_all_request() == 0 && !boss_battle_fight_all_active());
    fa_select();
    boss_battle_title_reached();
    CHECK(!boss_battle_fight_all_active() && boss_battle_fight_all_state().result == BOSS_BATTLE_FIGHT_ALL_ABORTED);
    boss_battle_reset();
}

/* The canonical order, one boss after the other, each from a passed checkpoint; the progression carries from boss to boss
 * and the twentieth Win completes the run. */
static void test_fight_all_sequence(const char *user_dir) {
    fa_select();
    int next = -1;
    int level = 0, visited[BOSS_BATTLE_ENCOUNTERS] = {0};
    for (int boss = 0; boss < BOSS_BATTLE_ENCOUNTERS; ++boss) {
        CHECK(boss_battle_fight_all_state().boss == boss && !visited[boss]);
        visited[boss] = 1;
        const WinKind kind = boss % 3 == 0 ? WIN_LEVEL_UP : boss % 3 == 1 ? WIN_XP_ONLY : WIN_NO_CHANGE;
        const BossBattleCharacterProgress before = fa_character;
        fa_win(boss, 60, kind);
        level += kind == WIN_LEVEL_UP;
        CHECK(fa_character.level == level && fa_character.xp_progress == before.xp_progress + (kind != WIN_NO_CHANGE ? 7 : 0));
        CHECK(fa_character.skill_points == level && boss_battle_fight_all_checkpoint() &&
              boss_battle_fight_all_state().cleared == boss + 1);
        CHECK(!boss_battle_menu_reopen_pending() && boss_battle_start_transition() == BOSS_BATTLE_START_HELD);
        const BossBattleFightAllStep step = fa_checkpoint(&next);
        if (boss < BOSS_BATTLE_ENCOUNTERS - 1) {
            CHECK(step == BOSS_BATTLE_FIGHT_ALL_STEP_START && next == boss + 1 && !boss_battle_session_active());
            CHECK(boss_battle_fight_all_state().boss == boss + 1 && boss_battle_fight_all_state().checkpoint);
        } else {
            CHECK(step == BOSS_BATTLE_FIGHT_ALL_STEP_ENDED);
        }
        const BossBattleStats stats = stats_of(boss);
        CHECK(stats.attempts == 1 && stats.wins == 1 && stats.deaths == 0 && stats.quits == 0 && stats.best_ms == 1000);
    }
    const BossBattleFightAllState state = boss_battle_fight_all_state();
    CHECK(!state.active && !state.checkpoint && state.result == BOSS_BATTLE_FIGHT_ALL_CLEARED && state.cleared == 20 &&
          state.combat_ms == 20000);
    CHECK(fa_poll(&next) == BOSS_BATTLE_FIGHT_ALL_STEP_IDLE && !boss_battle_session_active());
    CHECK(boss_battle_menu_reopen_pending() && boss_battle_start_transition() == BOSS_BATTLE_START_NONE);
    BossBattleFightAllStats stats = run_stats();
    CHECK(stats.runs == 1 && stats.clears == 1 && stats.deaths == 0 && stats.quits == 0 && stats.best_progress == 20 &&
          stats.best_ms == 20000);
    char text[64];
    boss_battle_screen_request();
    CHECK(boss_battle_screen_built() == BOSS_BATTLE_SCREEN_FIGHT_ALL_ROW); /* the page reopens on Fight All */
    boss_battle_screen_statistic(1, 0, text, sizeof(text));
    CHECK(!strcmp(text, "Runs 1"));
    boss_battle_screen_statistic(2, 0, text, sizeof(text));
    CHECK(!strcmp(text, "Clears 1"));
    boss_battle_screen_statistic(5, 0, text, sizeof(text));
    CHECK(!strcmp(text, "Best Progress 20/20"));
    boss_battle_screen_statistic(6, 0, text, sizeof(text));
    CHECK(!strcmp(text, "Best Time 00:20.000"));

    /* the file keeps all of it */
    boss_battle_record_configure(user_dir);
    stats = run_stats();
    CHECK(stats.runs == 1 && stats.clears == 1 && stats.best_progress == 20 && stats.best_ms == 20000);
    CHECK(stats_of(HARPY).wins == 1 && stats_of(DEMON_KING).best_ms == 1000);
    boss_battle_reset();
}

/* The twentieth Win does not start another run, and a chosen boss afterwards is a manual one again. */
static void test_fight_all_after_clear(void) {
    fa_select();
    int next = -1;
    for (int boss = 0; boss < BOSS_BATTLE_ENCOUNTERS; ++boss) {
        fa_win(boss, 30, WIN_NO_CHANGE);
        CHECK(fa_checkpoint(&next) == (boss == 19 ? BOSS_BATTLE_FIGHT_ALL_STEP_ENDED : BOSS_BATTLE_FIGHT_ALL_STEP_START));
    }
    CHECK(!boss_battle_fight_all_active() && !boss_battle_session_active() && run_stats().clears == 1);
    CHECK(run_stats().best_ms == 10000);
    boss_battle_take_menu_reopen();
    boss_battle_screen_request();
    CHECK(boss_battle_screen_built() == 0);
    int index = -1;
    CHECK(boss_battle_screen_choose(1, &index) == BOSS_BATTLE_SCREEN_START && index == HARPY);
    CHECK(boss_battle_screen_closed(&index) && index == HARPY && !boss_battle_fight_all_active());
    CHECK(start_playable(HARPY, boss_arena(HARPY)) && !boss_battle_fight_all_active() && run_stats().runs == 1);
    CHECK(boss_battle_quit_key(0, 0, 0));
    frames(probe(boss_arena(HARPY), 1, 1, 1), 1);
    boss_battle_inn_requested();
    boss_battle_inn_arrived();
    CHECK(run_stats().quits == 0 && run_stats().runs == 1 && stats_of(HARPY).quits == 1);
    boss_battle_take_menu_reopen();
    boss_battle_screen_request();
    CHECK(boss_battle_screen_built() == 1); /* a manual boss reopens on that boss */

    /* a second run is numbered after the first */
    boss_battle_screen_closed(&index);
    boss_battle_screen_request();
    boss_battle_screen_built();
    CHECK(boss_battle_fight_all_request() == 1 && boss_battle_fight_all_state().run == 2);
    boss_battle_reset();
}

/* Every checkpoint needs the campaign back, the progression merged and its save done, and a failure ends the run
 * without launching anything and without touching a statistic. */
static void test_fight_all_checkpoints(void) {
    int next = -1;
    /* the next boss waits for the character save */
    fa_select();
    fa_win(HARPY, 60, WIN_LEVEL_UP);
    CHECK(!strcmp(guard_log, "PSBR") && boss_battle_progress_save_state() == BOSS_BATTLE_SAVE_PENDING);
    CHECK(fa_poll(&next) == BOSS_BATTLE_FIGHT_ALL_STEP_WAIT && !boss_battle_session_active());
    boss_battle_progress_save_started();
    CHECK(fa_poll(&next) == BOSS_BATTLE_FIGHT_ALL_STEP_WAIT && boss_battle_fight_all_checkpoint());
    boss_battle_progress_save_finished(1);
    CHECK(fa_poll(&next) == BOSS_BATTLE_FIGHT_ALL_STEP_START && next == 1);
    CHECK(start_session(next) && !strcmp(guard_log, "PSBRPS") && boss_battle_fight_all_state().boss == 1);
    CHECK(!boss_battle_fight_all_checkpoint() && boss_battle_state() == BOSS_BATTLE_TRAVELLING);
    boss_battle_reset();

    /* a failed save ends the run; the next boss never starts */
    next = -1;
    fa_select();
    fa_win(HARPY, 60, WIN_LEVEL_UP);
    boss_battle_progress_save_started();
    boss_battle_progress_save_finished(0);
    CHECK(fa_poll(&next) == BOSS_BATTLE_FIGHT_ALL_STEP_ENDED && next == -1);
    BossBattleFightAllState state = boss_battle_fight_all_state();
    CHECK(!state.active && state.result == BOSS_BATTLE_FIGHT_ALL_ABORTED && !boss_battle_session_active());
    CHECK(!strcmp(guard_log, "PSBR") && boss_battle_menu_reopen_pending() && boss_battle_start_transition() == BOSS_BATTLE_START_NONE);
    BossBattleFightAllStats stats = run_stats();
    CHECK(stats.runs == 1 && stats.clears == 0 && stats.deaths == 0 && stats.quits == 0 && stats.best_progress == 0);
    char notice[192];
    CHECK(boss_battle_take_notice(notice, sizeof(notice)) && strstr(notice, "Fight All"));
    boss_battle_screen_request();
    CHECK(boss_battle_screen_built() == 0);
    boss_battle_reset();

    /* a save that never finishes times out */
    fa_select();
    fa_win(HARPY, 60, WIN_LEVEL_UP);
    boss_battle_progress_save_started();
    int frames_waited = 0;
    BossBattleFightAllStep step;
    while ((step = fa_poll(&next)) == BOSS_BATTLE_FIGHT_ALL_STEP_WAIT && frames_waited < 5000) ++frames_waited;
    CHECK(step == BOSS_BATTLE_FIGHT_ALL_STEP_ENDED && frames_waited == 1800 && !boss_battle_fight_all_active());
    boss_battle_reset();

    /* a campaign that did not come back: no checkpoint, no reopen */
    fa_select();
    restore_result = 0;
    fa_win(HARPY, 60, WIN_LEVEL_UP);
    CHECK(!boss_battle_fight_all_checkpoint() && !boss_battle_fight_all_active() && !boss_battle_menu_reopen_pending());
    CHECK(fa_poll(&next) == BOSS_BATTLE_FIGHT_ALL_STEP_IDLE);
    CHECK(boss_battle_fight_all_state().result == BOSS_BATTLE_FIGHT_ALL_ABORTED && run_stats().deaths == 0 && run_stats().quits == 0);
    boss_battle_reset();

    /* a result that is not a native award is refused, and the next boss never starts */
    fa_select();
    fa_win(HARPY, 60, WIN_NOT_NATIVE);
    CHECK(fa_poll(&next) == BOSS_BATTLE_FIGHT_ALL_STEP_ENDED && !boss_battle_fight_all_active());
    CHECK(fa_character.level == 0 && !strcmp(guard_log, "PSBR") && boss_battle_menu_reopen_pending());
    boss_battle_reset();

    /* a restored record that is not the campaign's is not merged: the run stops */
    fa_select();
    CHECK(start_session(HARPY));
    boss_battle_progress_before(&fa_character);
    reach_boss_arena(HARPY, 1);
    boss_battle_boss_cleared(1);
    BossBattleCharacterProgress after = fa_character, merged, other = fa_character;
    after.level = 1, after.skill_points = 1;
    other.xp_progress = 99;
    CHECK(frames(probe(boss_arena(HARPY), 1, 1, 0), 1) == BOSS_BATTLE_FRAME_REQUEST_INN);
    boss_battle_inn_requested();
    boss_battle_progress_after(&after);
    boss_battle_inn_arrived();
    CHECK(!boss_battle_progress_merge(&other, &merged));
    CHECK(fa_poll(&next) == BOSS_BATTLE_FIGHT_ALL_STEP_ENDED && !boss_battle_session_active());
    boss_battle_reset();

    /* no character record captured: the progression cannot be verified, so the run stops */
    fa_select();
    CHECK(start_playable(HARPY, boss_arena(HARPY)));
    boss_battle_boss_cleared(1);
    boss_battle_inn_requested();
    boss_battle_inn_arrived();
    CHECK(fa_poll(&next) == BOSS_BATTLE_FIGHT_ALL_STEP_ENDED && !boss_battle_session_active());
    boss_battle_reset();
}

/* Death ends the run: the boss's own Death, the run's Death once, progress from the bosses defeated, no Clear and no
 * time, and the progression of the earlier wins stays. */
static void test_fight_all_death(void) {
    fa_select();
    int next = -1;
    for (int boss = 0; boss < 7; ++boss) {
        fa_win(boss, 60, WIN_LEVEL_UP);
        CHECK(fa_checkpoint(&next) == BOSS_BATTLE_FIGHT_ALL_STEP_START && next == boss + 1);
    }
    CHECK(fa_character.level == 7 && boss_battle_fight_all_state().cleared == 7);
    CHECK(start_session(7));
    boss_battle_progress_before(&fa_character);
    reach_boss_arena(7, 1);
    CHECK(boss_battle_state() == BOSS_BATTLE_FIGHTING);
    BossBattleCharacterProgress after = fa_character;
    after.level = 8, after.skill_points = 8, after.xp_progress += 50;
    CHECK(boss_battle_party_wiped() == 1 && boss_battle_outcome() == BOSS_BATTLE_OUTCOME_DEATH);
    CHECK(boss_battle_fight_all_state().result == BOSS_BATTLE_FIGHT_ALL_DIED);
    CHECK(boss_battle_party_wiped() == 1); /* a second signal counts nothing */
    CHECK(frames(probe(boss_arena(7), 1, 1, 0), 1) == BOSS_BATTLE_FRAME_REQUEST_INN);
    boss_battle_inn_requested();
    boss_battle_progress_after(&after);
    boss_battle_inn_arrived();
    BossBattleCharacterProgress merged;
    CHECK(!boss_battle_progress_merge(&fa_character, &merged) && fa_character.level == 7); /* no XP from the failed boss */
    CHECK(!boss_battle_fight_all_active() && boss_battle_fight_all_state().result == BOSS_BATTLE_FIGHT_ALL_DIED);
    CHECK(fa_poll(&next) == BOSS_BATTLE_FIGHT_ALL_STEP_IDLE);
    const BossBattleFightAllStats stats = run_stats();
    CHECK(stats.runs == 1 && stats.deaths == 1 && stats.quits == 0 && stats.clears == 0 && stats.best_progress == 7 &&
          stats.best_ms == 0);
    const BossBattleStats vampire = stats_of(7);
    CHECK(vampire.attempts == 1 && vampire.deaths == 1 && vampire.wins == 0 && stats_of(0).wins == 1);
    CHECK(boss_battle_menu_reopen_pending());
    boss_battle_take_menu_reopen();
    boss_battle_screen_request();
    CHECK(boss_battle_screen_built() == BOSS_BATTLE_SCREEN_FIGHT_ALL_ROW); /* Fight All, not the last boss */
    char text[64];
    boss_battle_screen_statistic(5, 0, text, sizeof(text));
    CHECK(!strcmp(text, "Best Progress 07/20"));
    boss_battle_screen_statistic(6, 0, text, sizeof(text));
    CHECK(!strcmp(text, "Best Time -")); /* a partial run never sets it */
    boss_battle_reset();

    /* the best progress of a later, shorter run stays */
    fa_select_keeping_records();
    CHECK(start_playable(HARPY, boss_arena(HARPY)) && boss_battle_party_wiped());
    boss_battle_inn_requested();
    boss_battle_inn_arrived();
    CHECK(run_stats().runs == 2 && run_stats().deaths == 2 && run_stats().best_progress == 7);
    boss_battle_reset();
}

/* Quit, from any source: before the first boss is playable it is no run; after a boss was won, while the next loads, the
 * run quits but that boss has no attempt; while the next is playable it quits too. F4 and the pause menu are one path. */
static void test_fight_all_quit(void) {
    int next = -1;
    char title[64];
    BossBattleFightAllStats stats;

    /* before Harpy is playable */
    fa_select();
    CHECK(start_session(HARPY));
    reach_boss_arena(HARPY, 0);
    CHECK(boss_battle_pause_opened(title, sizeof(title)) && !strcmp(title, "Harpy") && boss_battle_pause_quit());
    CHECK(frames(probe(boss_arena(HARPY), 1, 1, 0), 1) == BOSS_BATTLE_FRAME_REQUEST_INN);
    boss_battle_inn_requested();
    boss_battle_inn_arrived();
    stats = run_stats();
    CHECK(stats.runs == 0 && stats.quits == 0 && stats_of(HARPY).attempts == 0 && stats_of(HARPY).quits == 0);
    CHECK(boss_battle_fight_all_state().result == BOSS_BATTLE_FIGHT_ALL_CANCELLED && !boss_battle_fight_all_active());
    CHECK(boss_battle_menu_reopen_pending());
    boss_battle_screen_request();
    CHECK(boss_battle_screen_built() == 0);
    boss_battle_reset();

    /* after Harpy was cleared, while Wyvern loads (F4) */
    fa_select();
    fa_win(HARPY, 60, WIN_LEVEL_UP);
    CHECK(fa_checkpoint(&next) == BOSS_BATTLE_FIGHT_ALL_STEP_START && next == 1);
    CHECK(start_session(next));
    boss_battle_progress_before(&fa_character);
    reach_boss_arena(1, 0);
    CHECK(boss_battle_state() == BOSS_BATTLE_INITIALIZING && boss_battle_quit_key(0, 0, 0) && !boss_battle_quit_key(0, 0, 0));
    CHECK(frames(probe(boss_arena(1), 1, 1, 0), 1) == BOSS_BATTLE_FRAME_REQUEST_INN);
    boss_battle_inn_requested();
    boss_battle_inn_arrived();
    stats = run_stats();
    CHECK(stats.runs == 1 && stats.quits == 1 && stats.deaths == 0 && stats.clears == 0 && stats.best_progress == 1);
    CHECK(stats_of(1).attempts == 0 && stats_of(1).quits == 0 && stats_of(HARPY).wins == 1);
    CHECK(boss_battle_fight_all_state().result == BOSS_BATTLE_FIGHT_ALL_QUIT_RUN && !boss_battle_fight_all_active());
    BossBattleCharacterProgress merged;
    CHECK(!boss_battle_progress_merge(&fa_character, &merged) && fa_character.level == 1); /* Harpy's level stays */
    boss_battle_reset();

    /* while Wyvern is playable (the pause menu) */
    fa_select();
    fa_win(HARPY, 60, WIN_XP_ONLY);
    CHECK(fa_checkpoint(&next) == BOSS_BATTLE_FIGHT_ALL_STEP_START);
    CHECK(start_session(next));
    boss_battle_progress_before(&fa_character);
    reach_boss_arena(1, 1);
    CHECK(boss_battle_state() == BOSS_BATTLE_FIGHTING);
    CHECK(boss_battle_pause_opened(title, sizeof(title)) && !strcmp(title, "Wyvern"));
    CHECK(boss_battle_pause_quit() && !boss_battle_pause_quit());
    CHECK(frames(probe(boss_arena(1), 1, 1, 1), 1) == BOSS_BATTLE_FRAME_REQUEST_INN);
    boss_battle_inn_requested();
    boss_battle_inn_arrived();
    stats = run_stats();
    CHECK(stats.runs == 1 && stats.quits == 1 && stats.best_progress == 1 && stats.deaths == 0);
    CHECK(stats_of(1).attempts == 1 && stats_of(1).quits == 1 && stats_of(1).wins == 0);
    CHECK(boss_battle_menu_reopen_pending());
    boss_battle_reset();

    /* F4 at the hidden checkpoint after Harpy: the run quits there and Wyvern never starts */
    fa_select();
    CHECK(!boss_battle_quit_key(0, 0, 0)); /* nothing to quit before the first boss */
    fa_win(HARPY, 60, WIN_LEVEL_UP);
    CHECK(boss_battle_quit_key(0, 0, 0) && !boss_battle_quit_key(0, 0, 0));
    CHECK(fa_poll(&next) == BOSS_BATTLE_FIGHT_ALL_STEP_ENDED && !boss_battle_session_active());
    stats = run_stats();
    CHECK(stats.runs == 1 && stats.quits == 1 && stats.deaths == 0 && stats.clears == 0 && stats.best_progress == 1);
    CHECK(stats_of(1).attempts == 0 && boss_battle_fight_all_state().result == BOSS_BATTLE_FIGHT_ALL_QUIT_RUN);
    CHECK(!strcmp(guard_log, "PSBR") && boss_battle_menu_reopen_pending() && !boss_battle_fight_all_active());
    CHECK(!boss_battle_quit_key(0, 0, 0));
    boss_battle_reset();

    /* a quit that races the twentieth Win's checkpoint does not turn the clear into a quit */
    fa_select();
    for (int boss = 0; boss < BOSS_BATTLE_ENCOUNTERS; ++boss) {
        fa_win(boss, 30, WIN_NO_CHANGE);
        if (boss < BOSS_BATTLE_ENCOUNTERS - 1) CHECK(fa_checkpoint(&next) == BOSS_BATTLE_FIGHT_ALL_STEP_START);
    }
    CHECK(boss_battle_quit_key(0, 0, 0));
    CHECK(fa_poll(&next) == BOSS_BATTLE_FIGHT_ALL_STEP_ENDED);
    stats = run_stats();
    CHECK(stats.runs == 1 && stats.clears == 1 && stats.quits == 0 && boss_battle_fight_all_state().result == BOSS_BATTLE_FIGHT_ALL_CLEARED);
    boss_battle_reset();
}

/* A boss whose session ends without an outcome stops the run without a statistic. */
static void test_fight_all_unresolved(void) {
    fa_select();
    CHECK(start_playable(HARPY, boss_arena(HARPY)));
    frames(probe(kInnScene, 0, 0, 0), 1); /* the arena was left */
    CHECK(boss_battle_state() == BOSS_BATTLE_RESOLVING);
    CHECK(frames(probe(kArenaPlay, 1, 1, 0), 1) == BOSS_BATTLE_FRAME_REQUEST_INN);
    boss_battle_inn_requested();
    boss_battle_inn_arrived();
    const BossBattleFightAllStats stats = run_stats();
    CHECK(stats.runs == 1 && stats.deaths == 0 && stats.quits == 0 && stats.clears == 0);
    CHECK(boss_battle_fight_all_state().result == BOSS_BATTLE_FIGHT_ALL_ABORTED && !boss_battle_fight_all_active());
    boss_battle_reset();
}

/* The whole run through the hooks, as the game drives them: the page confirms Fight All, Harpy starts under black, her
 * Win returns to the hidden Inn where the campaign is put back and the character saved, Wyvern starts from the Inn's menu
 * frame with the character Harpy raised, and so on until a Death returns to the page on Fight All. */
static void test_fight_all_hooks(void) {
    progression_campaign(0, 40, 0);
    boss_battle_record_clear();
    open_page();
    CHECK(boss_battle_screen_shown());
    set_page_cursor(0);
    page_result(-1);
    CHECK(!strcmp(text_at(fake_rows[1], 0x380), "Runs 0"));
    se_calls = 0;
    GuestRegs regs = page_result(0);
    CHECK(regs.r15 == 1 && se_calls == 1 && se_value == BOSS_BATTLE_SE_DECIDE && boss_battle_fight_all_active());
    CHECK(boss_battle_start_transition() == BOSS_BATTLE_START_FADING && fades.calls == 1 && fades.colour[0] == FADE_BLACK);
    CHECK(!boss_battle_session_active() && !arena_request.calls && !party_sends);
    fake_inn[INN_ACT] = 0x8e;
    fake_inn[INN_STEP] = 3;
    options_leave_frame();
    CHECK(boss_battle_start_transition() == BOSS_BATTLE_START_HELD && fades.calls == 2);
    set_inn(7, 7, -1, BOSS_BATTLE_INN_ROW);
    fake_inn[INN_ACT] = 0xa4;
    fake_inn[INN_STEP] = 1;
    regs = result_frame();
    CHECK(regs.skip && boss_battle_session_active() && boss_battle_state() == BOSS_BATTLE_TRAVELLING);
    CHECK(arena_request.calls == 1 && party_sends == 1 && !strcmp(arena_request.round, "round/MysticRuins.brb") &&
          arena_request.stage == 8);
    CHECK(record_level() == 0 && boss_battle_fight_all_state().boss == 0 && run_stats().runs == 0);

    arrive_through_native_fade(HARPY);
    BossBattleProbe seen = probe(boss_arena(HARPY), 1, 1, 1);
    CHECK(run_stats().runs == 1 && stats_of(HARPY).attempts == 1);
    frames(seen, 60);
    fight_changes_world();
    boss_battle_boss_cleared(1);
    native_level_up(1, 15, 0);
    const int requests = arena_request.calls;
    arrive_at_inn();
    CHECK(boss_battle_fight_all_checkpoint() && !boss_battle_menu_reopen_pending() && boss_battle_fight_all_state().cleared == 1);
    CHECK(record_level() == 1 && record_i32(0x08) == 15 && fake_play_data[WORLD_FLAG] == 0x22 && fake_save[SAVE_SLOTS + WORLD_FLAG] == 0x11);
    CHECK(boss_battle_start_transition() == BOSS_BATTLE_START_HELD && !boss_battle_screen_requested());
    CHECK(stats_of(HARPY).wins == 1 && stats_of(HARPY).best_ms == 1000 && run_stats().runs == 1);

    /* the Inn stays black and silent while the checkpoint runs */
    GuestRegs arrival;
    memset(&arrival, 0, sizeof(arrival));
    arrival.rdi = scene_fader_address();
    boss_battle_arrival_fade_hook(&arrival);
    CHECK(arrival.rsi == (uint32_t)FADE_BLACK && arrival.rcx == FADE_LAYER);
    GuestRegs held = native_fade(scene_fader_address(), 0, 30, 0x27);
    CHECK(held.rsi == (uint32_t)FADE_BLACK && held.rdx == 0 && held.rcx == FADE_LAYER && help_line(1).rdx == 0);

    /* the character save runs from the Inn's menu frame; Wyvern waits for it */
    set_inn(7, 7, -1, 0);
    regs = result_frame();
    CHECK(regs.skip && (uint32_t)regs.rax == 0xffffffffu && commits == 1 && save_requests == 1);
    CHECK(fake_disk[RECORD_AT + 0x26] == 1 && fake_save[SAVE_SLOTS + WORLD_FLAG] == 0x22);
    set_inn(7, 7, -1, 0);
    regs = result_frame();
    CHECK(regs.skip && save_requests == 1 && arena_request.calls == requests && !boss_battle_session_active());
    fake_saver[0x5b] = 0;
    put_u32(fake_saver + 0x70, 0);
    const int fades_before = fades.calls;
    /* saved, but the Inn scene still runs its arrival: Wyvern waits for it, with no input */
    set_game_scene((BossBattleScene){4, 20, BOSS_BATTLE_TOWN_ROUND, 2, -1, 0});
    for (int frame = 0; frame < 5; ++frame) {
        set_inn(7, 7, -1, 0);
        regs = result_frame();
        CHECK(regs.skip && (uint32_t)regs.rax == 0xffffffffu && !boss_battle_session_active());
    }
    CHECK(boss_battle_progress_save_state() == BOSS_BATTLE_SAVE_IDLE && arena_request.calls == requests &&
          boss_battle_fight_all_transit() == BOSS_BATTLE_TRANSIT_SCENE_WAIT);
    set_game_scene(kInnScene);
    set_inn(7, 7, -1, 0);
    regs = result_frame();
    CHECK(regs.skip && (uint32_t)regs.rax == 0xffffffffu && boss_battle_session_active() && !boss_battle_fight_all_checkpoint());
    CHECK(arena_request.calls == requests + 1 && party_sends == 2 && !strcmp(arena_request.round, "round/AncientLabyrinth.brb") &&
          arena_request.stage == 9);
    CHECK(!strcmp(arena_request.guard_before, "PRP") && !strcmp(party_guard, "PRP")); /* isolated again before the party */
    CHECK(boss_battle_fight_all_state().boss == 1 && fades.calls == fades_before && !boss_battle_screen_requested());
    CHECK(boss_battle_start_transition() == BOSS_BATTLE_START_HELD && boss_battle_state() == BOSS_BATTLE_TRAVELLING);
    CHECK(record_level() == 1 && record_i32(0x08) == 15 && record_skill_points() == 1); /* Wyvern's character is Harpy's result */
    held = native_fade(scene_fader_address(), 0, 30, 0x27);
    CHECK(held.rsi == (uint32_t)FADE_BLACK && held.rdx == 0 && held.rcx == FADE_LAYER);

    arrive_through_native_fade(1);
    seen = probe(boss_arena(1), 1, 1, 1);
    frames(seen, 90);
    boss_battle_boss_cleared(1);
    native_level_up(1, 5, 0);
    arrive_at_inn();
    CHECK(record_level() == 2 && record_skill_points() == 2 && boss_battle_fight_all_checkpoint());
    set_inn(7, 7, -1, 0);
    result_frame();
    fake_saver[0x5b] = 0;
    put_u32(fake_saver + 0x70, 0);
    set_inn(7, 7, -1, 0);
    regs = result_frame();
    CHECK(boss_battle_session_active() && boss_battle_fight_all_state().boss == 2 && record_level() == 2);
    CHECK(!strcmp(arena_request.round, "round/MoldyDungeon.brb") && stats_of(1).wins == 1 && stats_of(1).best_ms == 1500);

    /* Death on Doom Beetle: back through the same return, page on Fight All, earlier progress kept and saved */
    reach_boss_arena(2, 1);
    CHECK(boss_battle_party_wiped());
    arrive_at_inn();
    CHECK(!boss_battle_fight_all_active() && boss_battle_menu_reopen_pending() && record_level() == 2);
    CHECK(fake_disk[RECORD_AT + 0x26] == 2);
    memset(&arrival, 0, sizeof(arrival));
    arrival.rdi = scene_fader_address();
    boss_battle_arrival_fade_hook(&arrival);
    CHECK(arrival.rsi == (uint32_t)FADE_BLACK);
    set_inn(7, 7, -1, 0);
    result_frame();
    fake_options_page(8);
    build_page();
    CHECK(boss_battle_screen_shown() && cursor_row == 0 && !strcmp(text_at(fake_rows[0], 0x5c0), "Fight All"));
    CHECK(!strcmp(text_at(fake_rows[1], 0x380), "Runs 1") && !strcmp(text_at(fake_rows[2], 0x380), "Clears 0") &&
          !strcmp(text_at(fake_rows[3], 0x380), "Deaths 1") && !strcmp(text_at(fake_rows[4], 0x380), "Quits 0") &&
          !strcmp(text_at(fake_rows[5], 0x380), "Best Progress 02/20") && !strcmp(text_at(fake_rows[6], 0x380), "Best Time -"));
    CHECK(stats_of(2).attempts == 1 && stats_of(2).deaths == 1);
    release_progression();

    /* a character save that fails stops the run at the checkpoint and reopens the page */
    progression_campaign(0, 40, 0);
    boss_battle_record_clear();
    open_page();
    set_page_cursor(0);
    page_result(0);
    fake_inn[INN_ACT] = 0x8e;
    fake_inn[INN_STEP] = 3;
    options_leave_frame();
    set_inn(7, 7, -1, BOSS_BATTLE_INN_ROW);
    result_frame();
    reach_boss_arena(HARPY, 1);
    boss_battle_boss_cleared(1);
    native_level_up(1, 15, 0);
    arrive_at_inn();
    set_inn(7, 7, -1, 0);
    result_frame();
    fake_saver[0x5b] = 0;
    put_u32(fake_saver + 0x70, 1);
    set_inn(7, 7, -1, 0);
    regs = result_frame();
    CHECK(!boss_battle_fight_all_active() && !boss_battle_session_active() && arena_request.calls == 1);
    CHECK(regs.skip && regs.rax == BOSS_BATTLE_INN_OPTIONS_ROW && boss_battle_screen_requested());
    CHECK(run_stats().runs == 1 && run_stats().deaths == 0 && run_stats().quits == 0 && run_stats().clears == 0);
    release_progression();

    /* a game server that is not there: Fight All stops before the first boss and the Inn menu fades back in */
    progression_campaign(0, 40, 0);
    boss_battle_record_clear();
    memset(game_context + 0x80, 0, 8);
    open_page();
    set_page_cursor(0);
    page_result(0);
    fake_inn[INN_ACT] = 0x8e;
    fake_inn[INN_STEP] = 3;
    options_leave_frame();
    set_inn(7, 7, -1, BOSS_BATTLE_INN_ROW);
    result_frame();
    CHECK(!boss_battle_fight_all_active() && !boss_battle_session_active() && run_stats().runs == 0);
    release_progression();
}

/* The Inn scene can start a travel only as its own menu does: playing in the Inn, nothing pending, the fader settled. */
static void test_fight_all_scene_ready(void) {
    const BossBattleScene arrival = {4, 19, BOSS_BATTLE_TOWN_ROUND, 2, -1, 0}, pending = {2, 0, BOSS_BATTLE_TOWN_ROUND, 2, 9, 0},
                          town = {2, 0, BOSS_BATTLE_TOWN_ROUND, 8, -1, 0}, leaving = {4, 0, 6, 2, 9, 0};
    CHECK(boss_battle_fight_all_scene_ready(&kInnScene, 0));
    CHECK(!boss_battle_fight_all_scene_ready(&kInnScene, 1)); /* the fader still fades */
    CHECK(!boss_battle_fight_all_scene_ready(&arrival, 0) && !boss_battle_fight_all_scene_ready(&pending, 0));
    CHECK(!boss_battle_fight_all_scene_ready(&town, 0) && !boss_battle_fight_all_scene_ready(&leaving, 0));
    CHECK(!boss_battle_fight_all_scene_ready(NULL, 0));
}

/* Every boss pair waits for the same native condition after its save: a saved checkpoint alone never starts the next
 * boss, the next boss starts by itself once the scene is ready, and nothing waits for input. */
static void test_fight_all_scene_gate(void) {
    const BossBattleScene arrival = {4, 20, BOSS_BATTLE_TOWN_ROUND, 2, -1, 0};
    int next = -1;
    fa_select();
    for (int boss = 0; boss < BOSS_BATTLE_ENCOUNTERS - 1; ++boss) {
        fa_win(boss, 30, boss % 2 ? WIN_XP_ONLY : WIN_LEVEL_UP);
        CHECK(boss_battle_fight_all_transit() == BOSS_BATTLE_TRANSIT_CHECKPOINT);
        CHECK(boss_battle_fight_all_poll(&arrival, 0, &next) == BOSS_BATTLE_FIGHT_ALL_STEP_WAIT); /* the save first */
        fa_run_save(1);
        const size_t guards = strlen(guard_log);
        for (int frame = 0; frame < 40; ++frame) {
            CHECK(boss_battle_fight_all_poll(&arrival, 0, &next) == BOSS_BATTLE_FIGHT_ALL_STEP_WAIT);
            CHECK(boss_battle_fight_all_poll(&kInnScene, 1, &next) == BOSS_BATTLE_FIGHT_ALL_STEP_WAIT);
        }
        CHECK(boss_battle_fight_all_transit() == BOSS_BATTLE_TRANSIT_SCENE_WAIT);
        CHECK(strlen(guard_log) == guards && !boss_battle_session_active() && boss_battle_fight_all_state().boss == boss);
        CHECK(boss_battle_fight_all_poll(&kInnScene, 0, &next) == BOSS_BATTLE_FIGHT_ALL_STEP_START && next == boss + 1);
        CHECK(boss_battle_fight_all_transit() == BOSS_BATTLE_TRANSIT_REQUESTED);
    }
    CHECK(run_stats().best_progress == 19);
    boss_battle_reset();

    /* a scene that never becomes ready ends the run safely instead of leaving it black */
    fa_select();
    fa_win(HARPY, 30, WIN_NO_CHANGE);
    BossBattleFightAllStep step;
    int frames_waited = 0;
    while ((step = boss_battle_fight_all_poll(&arrival, 0, &next)) == BOSS_BATTLE_FIGHT_ALL_STEP_WAIT && frames_waited < 5000)
        ++frames_waited;
    CHECK(step == BOSS_BATTLE_FIGHT_ALL_STEP_ENDED && frames_waited == 1800 && boss_battle_menu_reopen_pending());
    CHECK(boss_battle_fight_all_state().result == BOSS_BATTLE_FIGHT_ALL_ABORTED);
    CHECK(run_stats().quits == 0 && run_stats().deaths == 0);
    boss_battle_reset();

    /* F4 while the scene is not ready yet quits the run there */
    fa_select();
    fa_win(HARPY, 30, WIN_NO_CHANGE);
    CHECK(boss_battle_fight_all_poll(&arrival, 0, &next) == BOSS_BATTLE_FIGHT_ALL_STEP_WAIT);
    CHECK(boss_battle_quit_key(0, 0, 0));
    CHECK(boss_battle_fight_all_poll(&arrival, 0, &next) == BOSS_BATTLE_FIGHT_ALL_STEP_ENDED);
    CHECK(run_stats().quits == 1 && !boss_battle_session_active());
    boss_battle_reset();
}

/* The inter-boss transit from the checkpoint to the next arena's reveal, and nothing else. */
static void test_fight_all_transit(void) {
    int next = -1;
    fa_select();
    CHECK(start_session(HARPY));
    reach_boss_arena(HARPY, 1);
    CHECK(boss_battle_fight_all_transit() == BOSS_BATTLE_TRANSIT_NONE);
    frames(probe(boss_arena(HARPY), 1, 1, 1), 30);
    boss_battle_boss_cleared(1);
    CHECK(boss_battle_fight_all_transit() == BOSS_BATTLE_TRANSIT_NONE); /* the award and level-up show as they are */
    CHECK(frames(probe(boss_arena(HARPY), 1, 1, 0), 1) == BOSS_BATTLE_FRAME_REQUEST_INN);
    boss_battle_inn_requested();
    BossBattleCharacterProgress same = fa_character;
    boss_battle_progress_before(&same);
    boss_battle_progress_after(&same);
    boss_battle_inn_arrived();
    CHECK(boss_battle_fight_all_transit() == BOSS_BATTLE_TRANSIT_CHECKPOINT);
    CHECK(fa_poll(&next) == BOSS_BATTLE_FIGHT_ALL_STEP_START);
    CHECK(start_session(next) && boss_battle_fight_all_transit() == BOSS_BATTLE_TRANSIT_REQUESTED);
    const BossBattleScene inn_left = {2, 0, 6, 2, 9, 0}, travel = {4, 3, 6, 2, 9, 0}, loaded = {4, 6, 6, 9, -1, 0};
    BossBattleProbe p = probe(inn_left, 0, 0, 0);
    p.host_ms = 1000;
    frames(p, 3);
    CHECK(boss_battle_fight_all_transit() == BOSS_BATTLE_TRANSIT_REQUESTED);
    p = probe(travel, 0, 0, 0);
    p.host_ms = 4500;
    frames(p, 2);
    CHECK(boss_battle_fight_all_transit() == BOSS_BATTLE_TRANSIT_STAGE_LOADING);
    frames(probe(loaded, 1, 1, 0), 1);
    CHECK(boss_battle_state() == BOSS_BATTLE_INITIALIZING && boss_battle_fight_all_transit() == BOSS_BATTLE_TRANSIT_ARENA_READY);
    CHECK(boss_battle_start_reveal_begins() && boss_battle_fight_all_transit() == BOSS_BATTLE_TRANSIT_ARENA_READY);
    CHECK(!boss_battle_start_reveal_finished(1) && boss_battle_fight_all_transit() == BOSS_BATTLE_TRANSIT_ARENA_READY);
    CHECK(boss_battle_start_reveal_finished(0) && boss_battle_fight_all_transit() == BOSS_BATTLE_TRANSIT_DONE);
    frames(probe(boss_arena(1), 1, 1, 1), 30);
    CHECK(boss_battle_state() == BOSS_BATTLE_FIGHTING && boss_battle_fight_all_transit() == BOSS_BATTLE_TRANSIT_DONE);
    boss_battle_reset();

    /* a quit while the next boss loads ends it at once */
    fa_select();
    fa_win(HARPY, 30, WIN_NO_CHANGE);
    CHECK(fa_poll(&next) == BOSS_BATTLE_FIGHT_ALL_STEP_START && start_session(next));
    reach_boss_arena(1, 0);
    CHECK(boss_battle_fight_all_transit() == BOSS_BATTLE_TRANSIT_ARENA_READY && boss_battle_quit_key(0, 0, 0));
    frames(probe(boss_arena(1), 1, 1, 0), 1);
    CHECK(boss_battle_state() == BOSS_BATTLE_RESOLVING);
    boss_battle_reset();

    /* a chosen boss has no transit */
    start_guarded();
    CHECK(start_playable(MEDUSA, boss_arena(MEDUSA)) && boss_battle_fight_all_transit() == BOSS_BATTLE_TRANSIT_NONE);
    CHECK(boss_battle_party_wiped());
    boss_battle_inn_requested();
    boss_battle_inn_arrived();
    CHECK(boss_battle_fight_all_transit() == BOSS_BATTLE_TRANSIT_NONE);
    boss_battle_reset();
}

/* A party of one in the fake context, as the scene hook's probe reads it: slot 0 is the local player with a unit. */
static uint8_t fake_units[0x200], fake_unit[0x1000];
static void fake_party(void) {
    for (int slot = 0; slot < 4; ++slot) put_u32(game_context + 0xa8 + slot * 0x23b0 + 0x21e0, slot ? 0xffffffffu : 0);
    memset(fake_units, 0, sizeof(fake_units));
    memset(fake_unit, 0, sizeof(fake_unit));
    put_pointer(fake_units + 0x120, fake_unit);
    put_pointer(game_context + 0x90, fake_units);
    put_pointer(game_context + 0xd8, fader_owner); /* the context's screen object, as the game manager's */
}

static void scene_frame(BossBattleScene scene) {
    set_game_scene(scene);
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.r15 = (uint64_t)(uintptr_t)game_scene;
    boss_battle_scene_hook(&regs);
}

/* Harpy, Wyvern and Doom Beetle through the hooks: the travel's fades stay black, the arena's step-19 fade-in runs as the
 * game asks, the scene hook sees the transition complete only once the scene fader has no frames left, no fade of ours
 * follows, and the Inn the fight returns to stays black for the page. */
static void test_native_arena_fade_in(void) {
    static uint8_t scene_fader[0x100];
    for (int boss = HARPY; boss <= 2; ++boss) {
        start_guarded();
        use_fake_inn_functions();
        use_fake_screen_functions();
        use_fake_game();
        fake_party();
        memset(scene_fader, 0, sizeof(scene_fader));
        put_pointer(fader_owner + 0x78, scene_fader);
        open_boss_screen(boss + 1);
        page_result(boss + 1);
        options_leave_frame();
        set_inn(7, 7, -1, BOSS_BATTLE_INN_ROW);
        result_frame();
        CHECK(boss_battle_session_active() && boss_battle_start_transition() == BOSS_BATTLE_START_HELD);
        const BossBattleScene arena = boss_arena(boss);
        BossBattleScene travel = arena;
        travel.act = 4;
        travel.step = 3;
        travel.next_stage = arena.stage;
        for (int frame = 0; frame < 3; ++frame) scene_frame(travel);
        travel.step = 18;
        travel.next_stage = -1;
        scene_frame(travel);
        CHECK(boss_battle_state() == BOSS_BATTLE_INITIALIZING);
        const GuestRegs travel_fade = native_fade(scene_fader_address(), (uint32_t)FADE_BLACK, 30, 0x27);
        CHECK(travel_fade.rsi == (uint32_t)FADE_BLACK && travel_fade.rdx == 0 && travel_fade.rcx == FADE_LAYER);
        const int fades_before = fades.calls;
        native_arena_arrival();
        travel.step = 19;
        for (int left = 30; left > 0; left -= 10) {
            put_u32(scene_fader + 0x7c, (uint32_t)left);
            scene_frame(travel);
            CHECK(boss_battle_start_transition() == BOSS_BATTLE_START_REVEALING);
        }
        put_u32(scene_fader + 0x7c, 0);
        scene_frame(travel);
        CHECK(boss_battle_start_transition() == BOSS_BATTLE_START_DONE);
        for (int frame = 0; frame < 30; ++frame) scene_frame(arena);
        CHECK(boss_battle_start_transition() == BOSS_BATTLE_START_DONE && fades.calls == fades_before);
        CHECK(native_fade_passes());

        CHECK(boss_battle_quit_key(0, 0, 0));
        frames(probe(arena, 1, 1, 1), 1);
        boss_battle_inn_requested();
        inn_arrives();
        const GuestRegs inn = arrival_fade();
        CHECK(inn.rsi == (uint32_t)FADE_BLACK && inn.rcx == FADE_LAYER && boss_battle_menu_reopen_pending());
        memset(fader_owner + 0x78, 0, 8);
        release_fake_game();
    }
}

/* A chosen boss behaves as before around a Fight All: its own highlight, no Fight All state, no Fight All statistic. */
static void test_fight_all_manual_unchanged(void) {
    start_guarded();
    boss_battle_record_clear();
    const BossBattleFightAllStats before = run_stats();
    CHECK(start_playable(MEDUSA, boss_arena(MEDUSA)) && !boss_battle_fight_all_active());
    CHECK(boss_battle_party_wiped());
    boss_battle_inn_requested();
    boss_battle_inn_arrived();
    CHECK(!boss_battle_fight_all_active() && boss_battle_fight_all_state().result == BOSS_BATTLE_FIGHT_ALL_NONE);
    CHECK(boss_battle_menu_reopen_pending() && fa_poll(&(int){0}) == BOSS_BATTLE_FIGHT_ALL_STEP_IDLE);
    boss_battle_take_menu_reopen();
    boss_battle_screen_request();
    CHECK(boss_battle_screen_built() == 1 + MEDUSA - boss_battle_screen_offset());
    const BossBattleFightAllStats after = run_stats();
    CHECK(!memcmp(&before, &after, sizeof(after)) && stats_of(MEDUSA).deaths == 1);
    boss_battle_reset();
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
    if (!f) { /* a build the launcher has not prepared here: its research copy */
        snprintf(path, sizeof(path), "%s/build/research/image/%s/boot.bin", DCP_SOURCE_DIR, folder);
        f = fopen(path, "rb");
    }
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
    /* where the request keeps the scene in rbx (CUSA07001 1.00, CUSA10209 1.00) */
    GuestRegs in_rbx;
    memset(&in_rbx, 0, sizeof(in_rbx));
    in_rbx.rbx = (uint64_t)(uintptr_t)scene;
    in_rbx.rdx = in_rbx.rcx = 0xffffffffu;
    in_rbx.r8 = 0x1234;
    boss_battle_town_return_hook(&in_rbx);
    CHECK(in_rbx.rdx == 0xffffffffu && in_rbx.r8 == 0x1234);
    boss_battle_town_return_rbx_hook(&in_rbx);
    CHECK(in_rbx.rdx == 2 && in_rbx.rcx == 0 && !strcmp((const char *)(uintptr_t)in_rbx.r8, "Inn"));
    boss_battle_set_screen_functions(NULL);
    boss_battle_reset();
}

static const uint64_t kOtherFeatureSites[] = {0x58e330, 0x53c22a, 0x53d064, 0x578000, 0x53d348, 0x53d38a, 0x86f00, 0x3cdbd9};
/* Calls other features find by their bytes and return address (Ultrawide's Inn frame draw); they stay unpatched. */
static const uint64_t kOtherFeatureCalls[] = {0x3a5055};
static const uint64_t kCalls[BOSS_BATTLE_INN_CALLS] = {0x3a4023, 0x3a4053, 0x3a4ca6, 0x3a5831, 0x3a5843, 0x3a5855,
                                                       0x3a623f, 0x3a624e, 0x3a625d, 0x3a62c9, 0x3a62e5};

static void test_us_image(void) {
    const size_t room = boss_battle_stub_bytes();
    DcpEarlyStorySites town;
    memset(&town, 0, sizeof(town));
    town.prompt_function = 0x369920;
    town.selector = 0x4763d0;
    town.town_portal_call = 0x3cdbd9;
    town.menu_kind_store = 0x432cf2;
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
    CHECK(boss_battle_build_listed("CUSA10454-1.00") && !boss_battle_build_listed(NULL));
    CHECK(boss_battle_locate(image, size, "CUSA10454-1.00", &sites, &why) == 1);
    if (why && sites.hook == 0) printf("locate: %s\n", why);
    /* STEP_FADEIN: fade(get_fader(screen), colour 0, 30 frames, layer 0x27), the call the arrival hook sits on */
    static const unsigned char kStepFadeIn[] = {0x48, 0x89, 0xc7, 0xe8};
    static const unsigned char kStepFadeInArgs[] = {0x31, 0xf6, 0xba, 0x1e, 0x00, 0x00, 0x00, 0xb9, 0x27, 0x00, 0x00, 0x00};
    CHECK(!memcmp(before + 0x42c76d, kStepFadeInArgs, sizeof(kStepFadeInArgs)) &&
          !memcmp(before + 0x42c779, kStepFadeIn, sizeof(kStepFadeIn)) && sites.arrival_fade_in == 0x42c77c &&
          sites.fade == 0x4437d0);
    CHECK(sites.hook == 0x428948 && sites.request == 0x30c70 && sites.scene_vtable == 0xb8cf30);
    CHECK(network_menu_create_function(before, size, 0x432cf2) == 0x4b0640 && network_menu_create_function(before, size, 0x432cf0) == 0);
    CHECK(sites.inn_built == 0x3a4501 && sites.inn_result == 0x3a62a0 && sites.inn_help == 0x3a652a &&
          sites.inn_help_table == 0x74df30);
    CHECK(sites.allocate == 0x56f340 && sites.widget == 0x480e20 && sites.add_widget == 0x481f50 &&
          sites.add_item == 0x4853c0 && sites.set_disabled == 0x485560 && sites.set_cursor == 0x482080);
    CHECK(sites.inn_party == 0x3ac840 && sites.inn_draw == 0x3a506d && sites.inn_destroyed == 0x3a3460);
    CHECK(sites.inn_layer == 0x3a504d && sites.inn_layer_tail == 0x3a3fc3);
    for (int i = 0; i < BOSS_BATTLE_INN_CALLS; ++i) CHECK(sites.inn_calls[i] == kCalls[i] && image[kCalls[i]] == 0xe8);
    CHECK(sites.arrange_story == 0x45030b && sites.story_test == 0x477c20 && sites.story_clear == 0x4c4649);
    CHECK(sites.dragon_talismans == 0x10da5c && sites.title == 0x4af5ec);
    CHECK(sites.inn_flushed == 0x3a316f && sites.save_copy == 0x8cc6e && sites.play_data_table == 0xc1dc60);
    CHECK(sites.party_wiped == 0x429564 && sites.boss_cleared == 0xce806 && sites.clear_screen == 0x42e929 &&
          sites.results_screen == 0x42f678 && sites.town_return == 0x4300ef && sites.set_step == 0x58da30 &&
          sites.set_text == 0x58f0e0 && sites.set_message == 0x6b500);
    CHECK(sites.options_title == 0x507a91 && sites.options_built == 0x507aef && sites.options_update == 0x5074b7 &&
          sites.options_result == 0x5075a5 && sites.list_up == 0x482300 && sites.list_down == 0x4824a0 && sites.list_update == 0x482a90 &&
          sites.options_body_vtable == 0xbb82b0 && sites.row_frame == 0x578f40);
    CHECK(sites.options_leave == 0x3ac08b && sites.arrival_fade_in == 0x42c77c && sites.inn_help_line == 0x3a6540 &&
          sites.cue_player == 0x5837f0 && sites.list_play_se == 0x484da0 && sites.get_fader == 0x443af0 &&
          sites.fade == 0x4437d0);
    CHECK(sites.results_level_done == 0x4c3d7a && sites.set_level == 0x81fb0 && sites.level_pattern == 0x81fe0 &&
          sites.write_stats == 0x82000);
    CHECK(sites.save_commit == 0x81910 && sites.save_request == 0x8e5a0);
    CHECK(sites.pause_start_gate == 0x432b21 && sites.pause_level_gate == 0x432c4f && sites.pause_mode == 0x432c8c &&
          sites.pause_row == 0x50b160 && sites.pause_setup == 0x50b740 && sites.pause_setup_calls[0] == 0x50e299 &&
          sites.pause_setup_calls[1] == 0x50e41b && sites.pause_setup_calls[2] == 0x50e8ab &&
          sites.pause_confirm_text == 0x510f5e && sites.pause_result == 0x4327be && sites.pause_result_fn == 0x514fe0 &&
          sites.pause_floor_reward == 0x513d02);
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
        0x513cf2, 0x513d02, 0x4300ef, 0x430104, 0x754282, 0xb8cf30 + 0xa0, 0x58da30, 0x3a6540, 0x3664d0, 0x3664ec, 0x366542, 0x366557,
        0x5910a0, 0x6b619,  0x591612, 0x58f0e0, 0x74db9c + 5 * 4, 0x3a64d2, 0x3a64d6, 0xb177b0 + 0x728, 0x3abf50,
        0x3abf6f, 0x507a5f, 0x507a83, 0x507a91, 0x507ae7, 0x507aef, 0x506730, 0x506745, 0xbb84a8 + 0xe0, 0x50710e,
        0x507250, 0x5074a2, 0x5074b7, 0x5074c7, 0x5075a2, 0x5075a5, 0xbb82b0 + 0xb8, 0x5032a9, 0xbb82b0 + 0x148,
        0xbb82b0 + 0x150, 0x482300, 0x4824a0, 0x578f40, 0xbb2720 + 0xe0, 0x4c39f0, 0x757b88 + 2 * 4, 0x4c3a1b,
        0x4c3cf0, 0x4c3d76, 0x4c3d7a, 0x4c3d81, 0x81fb0,  0x81fe0,  0x82000,   0x3abea4, 0x3ac083, 0x3ac08b, 0x58dbe0,
        0x3ac429, 0x3ac430, 0x3ac444, 0x443af0, 0x4437d0, 0x4288ab, 0x42bb08, 0x753ec4 + 19 * 4, 0x42c6be, 0x7544ff,
        0x42c765, 0x42c768, 0x42c77c, 0x4437f6, 0x7d672, 0x5837f0, 0x5839de, 0x5c9e69, 0xbb82b0 + 0xb0, 0x484da0,
        0x484db0, 0x68630, 0x6863c, 0xbef620 + 43 * 16, 0x3ac486, 0x3ac491, 0x3a5a7a, 0x3a5aac, 0x3a5acb, 0x81910,
        0x8191f, 0x8e5a0, 0x8e5bc, 0x432b21, 0x432b2a, 0x432b37, 0x432c4f, 0x432c54, 0x432c8c, 0x432c92, 0x432cc4,
        0x477d20, 0x477d24, 0x4763b0, 0x4763b4, 0x50b160, 0x50b171, 0x50b93d, 0x50b94f, 0x50b740, 0x50b751, 0x50e296,
        0x50e299, 0x50e41b, 0x50e8ab, 0x510f1f, 0x510f5e, 0x4327b6, 0x4327be, 0x514fe0, 0x514ff7,
        0xb177b0 + 0xc08, 0x3a5066, 0x3a506d, 0x3a5073, 0xbad618 + 0x188, 0x484df0, 0x484e3d, 0x483cb0, 0x483cc7,
        0xb177b0, 0x3a3460, 0x3a346d};
    for (size_t p = 0; p < sizeof(probes) / sizeof(*probes); ++p) {
        image[probes[p]] ^= 0x10;
        const int found = boss_battle_locate(image, size, "CUSA10454-1.00", &sites, &why);
        CHECK(found == 0);
        if (found) printf("  probe %#llx was not refused\n", (unsigned long long)probes[p]);
        image[probes[p]] ^= 0x10;
    }
    /* A changed Inn background site keeps the background native, never Boss Battle away. */
    const uint64_t background_probes[] = {0x36700a, 0x367012, 0x36701e, 0x367026, 0x3a558d, 0x3a5595, 0x3a55a9,
                                          0x3a6182, 0x3a6191, 0x3a5044, 0x3a504d, 0x3a3fb6, 0x3a3fc3, 0x577be0,
                                          0x577be7, 0x578f64, 0x578fb6, 0x578008, 0x578017, 0x578027, 0x57802f};
    for (size_t p = 0; p < sizeof(background_probes) / sizeof(*background_probes); ++p) {
        image[background_probes[p]] ^= 0x10;
        const int found = boss_battle_locate(image, size, "CUSA10454-1.00", &sites, &why);
        CHECK(found == 1 && sites.inn_background_failure && !sites.inn_layer && !sites.inn_layer_tail);
        if (!found || !sites.inn_background_failure)
            printf("  background probe %#llx: found %d\n", (unsigned long long)background_probes[p], found);
        image[background_probes[p]] ^= 0x10;
    }
    /* Exit Game, installed first, hooks the entry of the layout part's layers. */
    static const uint8_t kExitGameEntry[8] = {0xe9, 0x10, 0x20, 0x30, 0x00, 0x90, 0x90, 0x90};
    memcpy(image + 0x578000, kExitGameEntry, sizeof(kExitGameEntry));
    CHECK(boss_battle_locate(image, size, "CUSA10454-1.00", &sites, &why) == 1 && !sites.inn_background_failure &&
          sites.inn_layer == 0x3a504d && sites.inn_layer_tail == 0x3a3fc3);
    memcpy(image + 0x578000, before + 0x578000, sizeof(kExitGameEntry));
    CHECK(boss_battle_locate(image, size, "CUSA10454-1.00", &sites, &why) == 1 && !sites.inn_background_failure);
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
    /* Online Play: alone, Boss Battle keeps its own Start check; with the Early Story hooks in, theirs (one patch) */
    unsigned char *online_copy = malloc(size + room);
    memcpy(online_copy, before, size + room);
    CHECK(boss_battle_install(online_copy, size, 0, "CUSA10454-1.00", 1, &town, online_copy + size, room) == 1 &&
          boss_battle_available() && !boss_battle_online_available() && online_copy[0x432b21] == 0xe9);
    memcpy(online_copy, before, size + room);
    network_access_set_installed(1);
    CHECK(boss_battle_install(online_copy, size, 0, "CUSA10454-1.00", 1, &town, online_copy + size, room) == 1 &&
          online_copy[0x432b21] == 0xe8 && online_copy[0x432c4f] == 0xe9 && network_menu_request_available() &&
          online_copy[0x43286b] == before[0x43286b]);
    network_menu_set_create(NULL);
    network_access_set_installed(0);
    network_access_set(NETWORK_ACCESS_BOSS_BATTLE, NULL);
    free(online_copy);
    boss_battle_set_available(0);
    boss_battle_set_memory_guard(NULL, NULL);
    CHECK(boss_battle_install(image, size, 0, "CUSA10454-1.00", 0, NULL, image + size, room) == 0);
    CHECK(boss_battle_install(image, size, 0, "CUSA10454-1.00", 0, &town, image + size, room - 1) == 0);
    CHECK(!memcmp(image, before, size + room));

    unsigned char *native_background = malloc(size + room);
    memcpy(native_background, before, size + room);
    native_background[0x3a5044] ^= 0x10;
    CHECK(boss_battle_install(native_background, size, 0, "CUSA10454-1.00", 0, &town, native_background + size, room) == 1 &&
          boss_battle_available());
    CHECK(native_background[0x3a506d] == 0xe9 && !memcmp(native_background + 0x3a504d, before + 0x3a504d, 5) &&
          !memcmp(native_background + 0x3a3fc3, before + 0x3a3fc3, 5));
    use_fake_inn_functions();
    set_inn(6, 6, -1, 0);
    if (attach_background()) {
        GuestRegs built = inn_regs();
        boss_battle_inn_built_hook(&built);
        CHECK(main_rows_are_inn() && draw_frame(inn_list()).skip && menu_calls.texts == BOSS_BATTLE_INN_ROWS);
        CHECK(!inn_layout_applied(hook_base.data) && backgrounds_pristine());
        GuestRegs gone;
        memset(&gone, 0, sizeof(gone));
        gone.rdi = (uint64_t)(uintptr_t)fake_inn;
        boss_battle_inn_destroyed_hook(&gone);
    }
    free(native_background);
    boss_battle_set_available(0);

    PatchedSite all[57 + BOSS_BATTLE_INN_CALLS] = {
        {0x428948, 5}, {0x3a4501, 8}, {0x3a62a0, 5}, {0x3a652a, 5}, {0x45030b, 5}, {0x4c4649, 5}, {0x10da5c, 5},
        {0x4af5ec, 5}, {0x3a316f, 11}, {0x8cc6e, 5}, {0x429564, 5}, {0xce806, 5},  {0x42e929, 5}, {0x42f678, 5},
        {0x4300ef, 5}, {0x507a91, 5}, {0x507aef, 6}, {0x5074b7, 5}, {0x5075a5, 5}, {0x482300, 6}, {0x4824a0, 6}, {0x482a90, 6},
        {0x4c3d7a, 10}, {0x3ac08b, 5}, {0x42c77c, 5}, {0x4437d0, 6}, {0x3a6540, 5}, {0x5837f0, 6},
        {0x432b21, 5}, {0x432c4f, 5}, {0x432c8c, 6}, {0x50b160, 6}, {0x50e299, 5}, {0x50e41b, 5}, {0x50e8ab, 5},
        {0x510f5e, 5}, {0x4327be, 5}, {0x3a506d, 5}, {0x3a3460, 6}, {0x3a504d, 5}, {0x3a3fc3, 5}, {0x513d02, 5},
        {0x481f50, 6}, {0x50e914, 5}, {0x50e5d0, 5}, {0x483014, 6},
        {0x438daa, 5}, {0x438dc7, 5}, {0x438dea, 5}, {0x438df3, 5}, {0x2924b, 5}, {0x290e5, 5}, {0x429388, 5}, {0x4295d8, 5}, {0x4d2675, 5}, {0x436fb0, 6}, {0x2f08e, 10}};
    for (int i = 0; i < BOSS_BATTLE_INN_CALLS; ++i) all[57 + i] = (PatchedSite){kCalls[i], 5};
    const size_t sites_count = sizeof(all) / sizeof(*all);
    for (size_t i = 0; i < sites_count; ++i) {
        for (size_t j = 0; j < i; ++j)
            CHECK(all[i].at + all[i].length <= all[j].at || all[j].at + all[j].length <= all[i].at);
        for (size_t k = 0; k < sizeof(kOtherFeatureSites) / sizeof(*kOtherFeatureSites); ++k)
            CHECK(kOtherFeatureSites[k] + 16 <= all[i].at || all[i].at + all[i].length + 16 <= kOtherFeatureSites[k]);
        for (size_t k = 0; k < sizeof(kOtherFeatureCalls) / sizeof(*kOtherFeatureCalls); ++k)
            CHECK(kOtherFeatureCalls[k] + 5 <= all[i].at || all[i].at + all[i].length <= kOtherFeatureCalls[k]);
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

/* Each listed build's sites from its own boot image. The lead character selector and the town portal call are the
 * Early Story locator's (tests/feature_sites_tests.cpp pins them per build). */
static const struct {
    const char *folder, *game;
    uint64_t selector, town_portal_call;
} kRealBuilds[] = {
    {"v1.03", "CUSA10454-1.00", 0x4763d0, 0x3cdbd9},          {"CUSA10454-1.01", "CUSA10454-1.01", 0x4763d0, 0x3cdbd9},
    {"CUSA07001-1.00", "CUSA07001-1.00", 0x4755d0, 0x3cce49}, {"CUSA07001-1.02", "CUSA07001-1.02", 0x4759a0, 0x3cd259},
    {"CUSA07001-1.03", "CUSA07001-1.03", 0x475a60, 0x3cd2a9}, {"CUSA10209-1.00", "CUSA10209-1.00", 0x475740, 0x3ccfb9},
};

enum { SITE_LIST = 57 + BOSS_BATTLE_INN_CALLS };

static size_t hooked_sites(const BossBattleSites *s, PatchedSite out[SITE_LIST]) {
    const PatchedSite fixed[57] = {
        {s->hook, 5}, {s->inn_built, 8}, {s->inn_result, 5}, {s->inn_help, 5}, {s->arrange_story, 5}, {s->story_clear, 5},
        {s->dragon_talismans, 5}, {s->title, 5}, {s->inn_flushed, 11}, {s->save_copy, 5}, {s->party_wiped, 5},
        {s->boss_cleared, 5}, {s->clear_screen, 5}, {s->results_screen, 5}, {s->town_return, 5}, {s->options_title, 5},
        {s->options_built, 6}, {s->options_update, 5}, {s->options_result, 5}, {s->list_up, 6}, {s->list_down, 6}, {s->list_update, 6},
        {s->results_level_done, 10}, {s->options_leave, 5}, {s->arrival_fade_in, 5}, {s->fade, 6}, {s->inn_help_line, 5},
        {s->cue_player, 6}, {s->pause_start_gate, 5}, {s->pause_level_gate, 5}, {s->pause_mode, 6},
        {s->pause_row, s->pause_rows_inline ? 5 : 6}, {s->pause_setup_calls[0], 5}, {s->pause_setup_calls[1], 5},
        {s->pause_setup_calls[2], 5}, {s->pause_confirm_text, 5}, {s->pause_result, 5}, {s->inn_draw, 5},
        {s->inn_destroyed, 6}, {s->inn_layer, 5}, {s->inn_layer_tail, 5}, {s->pause_floor_reward, 5},
        {s->network_rows, 6}, {s->network_choice[0], 5}, {s->network_choice[1], 5},
        {s->network_back_query, 6}, {s->room_info_calls[0], 5}, {s->room_info_calls[1], 5},
        {s->room_info_calls[2], 5}, {s->room_info_calls[3], 5}, {s->join_state_call, 5},
        {s->join_unit_call, 5}, {s->party_slot_calls[0], 5}, {s->party_slot_calls[1], 5}, {s->location_text_call, 5},
        {s->add_player, 6}, {s->slot_request, 10}};
    memcpy(out, fixed, sizeof(fixed));
    for (int i = 0; i < BOSS_BATTLE_INN_CALLS; ++i) out[57 + i] = (PatchedSite){s->inn_calls[i], 5};
    return SITE_LIST;
}

static uint64_t rel32_at(const unsigned char *image, uint64_t at) {
    int32_t rel;
    memcpy(&rel, image + at + 1, sizeof(rel));
    return (uint64_t)((int64_t)at + 5 + rel);
}

/* Exit Game, installed first, hooks the entry of the layout part's layers: the background still verifies. */
static void check_after_exit_game(unsigned char *image, uint64_t size, const char *game, const BossBattleSites *sites) {
    const uint64_t part_set = rel32_at(image, sites->row_frame + 0x24), part_layers = rel32_at(image, part_set + 0x46);
    unsigned char saved[8];
    memcpy(saved, image + part_layers, sizeof(saved));
    static const uint8_t kExitGameEntry[8] = {0xe9, 0x10, 0x20, 0x30, 0x00, 0x90, 0x90, 0x90};
    memcpy(image + part_layers, kExitGameEntry, sizeof(kExitGameEntry));
    BossBattleSites again;
    const char *why = NULL;
    CHECK(boss_battle_locate(image, size, game, &again, &why) == 1 && !again.inn_background_failure &&
          again.inn_layer == sites->inn_layer);
    memcpy(image + part_layers, saved, sizeof(saved));
}

static int32_t frame_i32(const uint8_t *at) {
    int32_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

/* The inline setup's jump to the plain mode's rows goes to its Quit row with the first row's saves, while the fight owns
 * the menu; any other jump, or the campaign's own menu, is left alone. */
static void check_inline_pause_rows(const unsigned char *image, const BossBattleSites *sites) {
    static uint8_t frame[0x100];
    const uint64_t plain = (uint64_t)(uintptr_t)(image + sites->pause_plain_rows);
    const uint64_t quit = (uint64_t)(uintptr_t)(image + sites->pause_quit_row);
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    memset(frame, 0, sizeof(frame));
    regs.rcx = (uint64_t)(uintptr_t)(image + 0x1000);
    regs.rax = plain - regs.rcx;
    regs.rbp = (uint64_t)(uintptr_t)(frame + 0x80);
    regs.rbx = 0x11223344aabbccddull;
    regs.r15 = 0x55667788u;
    regs.r12 = 0x77;
    GuestRegs campaign = regs;
    boss_battle_pause_rows_hook(&campaign);
    CHECK(!memcmp(&campaign, &regs, sizeof(regs)));
    start_guarded();
    use_fake_screen_functions();
    use_fake_pause_functions();
    boss_battle_set_character("Zak", 0, 1);
    CHECK(start_session(HARPY));
    reach_arena();
    frames(probe(kArenaPlay, 1, 1, 0), 10);
    open_pause_menu();
    CHECK(boss_battle_pause_owned());
    GuestRegs other = regs;
    other.rax += 0x10;
    boss_battle_pause_rows_hook(&other);
    CHECK(other.rax == regs.rax + 0x10 && other.r12 == regs.r12);
    boss_battle_pause_rows_hook(&regs);
    uint64_t saved_r12;
    memcpy(&saved_r12, frame + 0x80 - 0x80, sizeof(saved_r12));
    CHECK(regs.rax + regs.rcx == quit && regs.r12 == (uint64_t)(uintptr_t)(image + sites->pause_row_vtable));
    CHECK(frame_i32(frame + 0x80 - 0x70) == (int32_t)0xaabbccdd && frame_i32(frame + 0x80 - 0x5c) == 0x55667788 &&
          saved_r12 == 0x77);
    boss_battle_pause_closed();
    boss_battle_set_screen_functions(NULL);
    boss_battle_reset();
}

/* The labyrinth's floor reward panel of the pause window stays empty during a fight, and is the game's otherwise. */
static void check_floor_reward_hook(void) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    boss_battle_reset();
    boss_battle_pause_floor_reward_hook(&regs);
    CHECK(!regs.skip);
    start_guarded();
    CHECK(start_session(ANCIENT_DRAGON) && boss_battle_session_active());
    boss_battle_pause_floor_reward_hook(&regs);
    CHECK(regs.skip);
    boss_battle_reset();
}

/* Where the image holds `pattern` (-1 any byte); up to `max` hits. */
static size_t find_pattern(const unsigned char *image, uint64_t size, const short *pattern, size_t length, uint64_t *hits,
                           size_t max) {
    size_t found = 0;
    for (uint64_t at = 0; at + length <= size && found < max; ++at) {
        size_t k = 0;
        while (k < length && (pattern[k] < 0 || image[at + k] == (unsigned char)pattern[k])) ++k;
        if (k == length) hits[found++] = at;
    }
    return found;
}

/* The Network Menu's row and close-path checks refuse an image whose code is not the code they were written for. */
static void check_network_row_sites(unsigned char *image, uint64_t size, const char *game) {
    static const short kClosePath[] = {0x48, 0x85, 0xc0, 0x0f, 0x84, -1, -1, -1, -1, 0x8b, 0x80, 0xb4,
                                       0x05, 0x00, 0x00, 0x83, 0xf8, 0x13, 0x0f, 0x87, -1, -1, -1, -1};
    static const short kReturnRow[] = {0xbe, 0xa9, 0x04, 0x00, 0x00};
    static const short kEndRow[] = {0xbe, 0xba, 0x04, 0x00, 0x00};
    uint64_t hits[8];
    const char *why = NULL;
    BossBattleSites probe;
    CHECK(find_pattern(image, size, kClosePath, sizeof(kClosePath) / sizeof(*kClosePath), hits, 8) == 1);
    const uint64_t close = hits[0];
    image[close + 17] ^= 0x01; /* the largest action that still has a row of its own */
    CHECK(boss_battle_locate(image, size, game, &probe, &why) == 0 && why && !strcmp(why, "Network Menu close path"));
    image[close + 17] ^= 0x01;
    image[close + 20] ^= 0x04; /* no longer the path of no row */
    CHECK(boss_battle_locate(image, size, game, &probe, &why) == 0 && why && !strcmp(why, "Network Menu close path"));
    image[close + 20] ^= 0x04;
    static const short kConfirmPath[] = {0xe8, -1, -1, -1, -1, 0xb9, 0xff, 0xff, 0xff, 0xff, 0x48, 0x85, 0xc0, 0x0f, 0x84,
                                         -1, -1, -1, -1, 0x8b, 0x88, 0xb4, 0x05, 0x00, 0x00, 0x8d, 0x41, 0xff, 0x83, 0xf8,
                                         0x12, 0x0f, 0x87, -1, -1, -1, -1};
    CHECK(find_pattern(image, size, kConfirmPath, sizeof(kConfirmPath) / sizeof(*kConfirmPath), hits, 8) == 1);
    const uint64_t confirm = hits[0];
    image[confirm + 30] ^= 0x01; /* no longer the largest action the table has */
    CHECK(boss_battle_locate(image, size, game, &probe, &why) == 0 && why && !strcmp(why, "Network Menu choice"));
    image[confirm + 30] ^= 0x01;
    image[confirm + 33] ^= 0x04; /* the out-of-range jump no longer meets the no-row jump */
    CHECK(boss_battle_locate(image, size, game, &probe, &why) == 0 && why && !strcmp(why, "Network Menu choice"));
    image[confirm + 33] ^= 0x04;
    image[confirm + 1] ^= 0x10; /* a different row lookup than the first read's */
    CHECK(boss_battle_locate(image, size, game, &probe, &why) == 0 && why && !strcmp(why, "Network Menu choice"));
    image[confirm + 1] ^= 0x10;
    /* the game's list clear releases each row through its deleting destructor, the slot the inline Return row is released by */
    static const short kListClear[] = {0x49, 0x89, 0xfe, 0x48, 0xbe, 0xf8, 0xff, 0xff, 0xff, 0x07, 0x00,
                                       0x00, 0x00, 0x49, 0x8b, 0x4e, 0x70, 0x49, 0x8b, 0x46, 0x68};
    CHECK(find_pattern(image, size, kListClear, sizeof(kListClear) / sizeof(*kListClear), hits, 8) == 1);
    static const unsigned char kDeleteCall[] = {0x48, 0x8b, 0x07, 0xff, 0x50, 0x08};
    int deletes = 0;
    for (uint64_t at = hits[0]; at + sizeof(kDeleteCall) < hits[0] + 96; ++at) deletes |= !memcmp(image + at, kDeleteCall, sizeof(kDeleteCall));
    CHECK(deletes);
    static const short kBackQuery[] = {0x41, 0xc7, 0x86, 0x60, 0x09, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x4c, 0x89,
                                       0xf7, 0x4d, 0x8d, 0xbe, 0x60, 0x09, 0x00, 0x00, 0x49, 0x8b, 0x06, 0xff, 0x90,
                                       0x20, 0x01, 0x00, 0x00, 0x84, 0xc0, 0x74, -1, 0x41, 0x8b, 0x86, 0x64, 0x09,
                                       0x00, 0x00, 0x85, 0xc0, 0x78, -1, 0x41, 0xc7, 0x07, 0xfe, 0xff, 0xff, 0xff};
    CHECK(find_pattern(image, size, kBackQuery, sizeof(kBackQuery) / sizeof(*kBackQuery), hits, 8) == 1);
    const uint64_t back_query = hits[0];
    image[back_query + 48] ^= 0x01; /* B no longer answers -2 */
    CHECK(boss_battle_locate(image, size, game, &probe, &why) == 0 && why && !strcmp(why, "Network Menu Back query"));
    image[back_query + 48] ^= 0x01;
    image[back_query + 26] ^= 0x08; /* another query than Back */
    CHECK(boss_battle_locate(image, size, game, &probe, &why) == 0 && why && !strcmp(why, "Network Menu Back query"));
    image[back_query + 26] ^= 0x08;
    static const short kInfoSlot[] = {0x49, 0x8d, 0x9d, 0x20, 0xde, 0xff, 0xff, 0x48, 0x89, 0xdf, 0xe8, -1, -1, -1, -1, 0x49, 0x89,
                                      0xc4, 0x4d, 0x85, 0xe4, 0x0f, 0x84, -1, -1, -1, -1, 0xe8, -1, -1, -1, -1, 0x4c, 0x8b,
                                      0x78, 0x68, 0x4c, 0x89, 0xe7, 0xe8, -1, -1, -1, -1, 0x48, 0x8b, 0x4d, 0x80, 0x89, 0xc0,
                                      0xf6, 0xc1, 0x01, 0x0f, 0x85, -1, -1, -1, -1, 0x48, 0x69, 0xc0, 0xd0, 0x01, 0x00, 0x00,
                                      0x48, 0x89, 0xdf, 0x45, 0x8b, 0x7c, 0x07, 0x28, 0xe8, -1, -1, -1, -1, 0x84, 0xc0, 0x74,
                                      0x6d, 0xe8, -1, -1, -1, -1, 0x44, 0x39, 0xf8, 0x4c, 0x8b, 0x7d, 0xa0, 0x75, 0x63, 0x49,
                                      0x8b, 0x85, 0x60, 0xdf, 0xff, 0xff, 0x49};
    CHECK(find_pattern(image, size, kInfoSlot, sizeof(kInfoSlot) / sizeof(*kInfoSlot), hits, 8) == 1);
    const uint64_t info_block = hits[0];
    image[info_block + 0x61 + 3] ^= 0x01; /* the host copy is no longer from the slot's host fields */
    CHECK(boss_battle_locate(image, size, game, &probe, &why) == 0 && why && !strcmp(why, "room info builder"));
    image[info_block + 0x61 + 3] ^= 0x01;
    image[info_block + 0x4f] ^= 0x01; /* the controllable test is not the slot's */
    CHECK(boss_battle_locate(image, size, game, &probe, &why) == 0 && why && !strcmp(why, "room info builder"));
    image[info_block + 0x4f] ^= 0x01;
    const short *rows[2] = {kReturnRow, kEndRow};
    for (int r = 0; r < 2; ++r) {
        const size_t n = find_pattern(image, size, rows[r], 5, hits, 8);
        CHECK(n >= 1);
        for (size_t i = 0; i < n; ++i) image[hits[i] + 1] ^= 0x10;
        CHECK(boss_battle_locate(image, size, game, &probe, &why) == 0 && why && !strcmp(why, "Network Menu rows"));
        for (size_t i = 0; i < n; ++i) image[hits[i] + 1] ^= 0x10;
    }
    CHECK(boss_battle_locate(image, size, game, &probe, &why) == 1);
}

static void test_real_images(void) {
    CHECK(!boss_battle_build_listed("CUSA10209-1.02") && !boss_battle_build_listed("CUSA10487-1.03") && !boss_battle_build_listed(NULL));
    for (size_t b = 0; b < sizeof(kRealBuilds) / sizeof(*kRealBuilds); ++b) {
        const char *game = kRealBuilds[b].game;
        const size_t room = boss_battle_stub_bytes();
        uint64_t size = 0;
        unsigned char *image = boot_image(kRealBuilds[b].folder, &size, room);
        if (!image) {
            printf("%s: no boot image, real image checks skipped\n", kRealBuilds[b].folder);
            continue;
        }
        CHECK(boss_battle_build_listed(game));
        unsigned char *before = malloc(size + room);
        memcpy(before, image, size + room);
        const char *why = NULL;
        BossBattleSites sites;
        memset(&sites, 0, sizeof(sites));
        const int found = boss_battle_locate(image, size, game, &sites, &why);
        CHECK(found == 1 && !sites.inn_background_failure);
        if (!found) {
            printf("%s: %s\n", game, why);
            free(image), free(before);
            continue;
        }
        PatchedSite all[SITE_LIST];
        const size_t count = hooked_sites(&sites, all);
        for (size_t i = 0; i < count; ++i) {
            CHECK(all[i].at && all[i].at + all[i].length <= size);
            for (size_t j = 0; j < i; ++j) CHECK(all[i].at + all[i].length <= all[j].at || all[j].at + all[j].length <= all[i].at);
        }
        int refused = 0;
        for (size_t i = 0; i < count; ++i) {
            image[all[i].at] ^= 0x10;
            BossBattleSites probe;
            const int still = boss_battle_locate(image, size, game, &probe, &why);
            const int background = all[i].at == sites.inn_layer || all[i].at == sites.inn_layer_tail;
            const int ok = background ? still == 1 && probe.inn_background_failure : still == 0;
            CHECK(ok);
            if (!ok) printf("  %s: changed site %#llx was not refused\n", game, (unsigned long long)all[i].at);
            refused += ok;
            image[all[i].at] ^= 0x10;
        }
        check_network_row_sites(image, size, game);
        check_after_exit_game(image, size, game, &sites);
        CHECK(!memcmp(image, before, size + room));
        DcpEarlyStorySites town;
        memset(&town, 0, sizeof(town));
        town.selector = (uint32_t)kRealBuilds[b].selector;
        town.town_portal_call = (uint32_t)kRealBuilds[b].town_portal_call;
        start_guarded();
        boss_battle_set_memory_guard(NULL, NULL);
        CHECK(boss_battle_install(image, size, 0, game, 0, &town, image + size, room) == 1 && boss_battle_available());
        CHECK(bytes_changed_outside(image, before, size, all, count) == 0);
        for (size_t i = 0; i < count; ++i) CHECK(image[all[i].at] == 0xe9);
        CHECK(image[kRealBuilds[b].town_portal_call] == 0xe8 && !hydeland_town_installed());
        CHECK(boss_battle_startable_count() == BOSS_BATTLE_ENCOUNTERS);
        if (sites.pause_rows_inline) check_inline_pause_rows(image, &sites);
        check_floor_reward_hook();
        printf("%s: %zu sites, %d of %zu changes refused, %d of %d encounters, pause rows %s, Town00 scene in %s\n", game,
               count, refused, count, boss_battle_startable_count(), BOSS_BATTLE_ENCOUNTERS,
               sites.pause_rows_inline ? "inline" : "by helper", sites.town_return_scene_rbx ? "rbx" : "r14");
        boss_battle_set_available(0);
        boss_battle_set_inn_functions(NULL);
        boss_battle_set_pause_functions(NULL);
        boss_battle_campaign_configure(0);
        boss_battle_reset();
        free(image), free(before);
    }
}

static uint8_t shortcut_manager[0x400], shortcut_ui[0x1000], shortcut_hint_object[0x800], shortcut_menu_window[0x2400];
static uint64_t shortcut_hint_vtable[0x40], shortcut_now = 1000;
static int shortcut_hint_creates, shortcut_hint_shows, shortcut_hint_hides;
static char shortcut_hint_text[96];

static uint64_t shortcut_clock(void) {
    return shortcut_now;
}

static uint64_t GUEST_HOOK_ABI shortcut_hint_create(uint64_t ui, int init) {
    (void)init;
    CHECK(ui == (uint64_t)(uintptr_t)shortcut_ui);
    ++shortcut_hint_creates;
    put_pointer(shortcut_ui + NETWORK_HINT_UI_WINDOWS + (size_t)NETWORK_HINT_WINDOW_TYPE * NETWORK_HINT_UI_SLOT_SIZE,
                shortcut_hint_object);
    put_pointer(shortcut_hint_object, shortcut_hint_vtable);
    return (uint64_t)(uintptr_t)shortcut_hint_object;
}

static void GUEST_HOOK_ABI shortcut_hint_set_text(uint64_t window, int message, const char *text) {
    (void)window;
    (void)message;
    snprintf(shortcut_hint_text, sizeof(shortcut_hint_text), "%s", text);
}

static void GUEST_HOOK_ABI shortcut_hint_refresh(uint64_t object) {
    (void)object;
}

static void GUEST_HOOK_ABI shortcut_hint_hide(uint64_t window) {
    (void)window;
    ++shortcut_hint_hides;
}

static void GUEST_HOOK_ABI shortcut_hint_show(uint64_t window, int duration, int flag) {
    (void)window;
    (void)duration;
    (void)flag;
    ++shortcut_hint_shows;
}

static int shortcut_hint_up(void) {
    return network_hint_shown_by(NETWORK_HINT_BOSS_BATTLE);
}

static int shortcut_menu_creates, shortcut_menu_inits, shortcut_menu_refuse;
static uint32_t shortcut_menu_pad = 0xff;
static uint64_t shortcut_menu_vtable[0x40];

static void GUEST_HOOK_ABI shortcut_menu_init(uint64_t window) {
    CHECK(window == (uint64_t)(uintptr_t)shortcut_menu_window);
    ++shortcut_menu_inits;
}

/* The game's menu window creation: the window goes into the Inn's menu window slot. */
static uint64_t GUEST_HOOK_ABI shortcut_menu_create(uint64_t ui, uint32_t pad) {
    CHECK(ui == (uint64_t)(uintptr_t)shortcut_ui);
    ++shortcut_menu_creates;
    shortcut_menu_pad = pad;
    if (shortcut_menu_refuse) return 0;
    memset(shortcut_menu_window, 0, sizeof(shortcut_menu_window));
    shortcut_menu_vtable[0x100 / 8] = (uint64_t)(uintptr_t)shortcut_menu_init;
    put_pointer(shortcut_menu_window, shortcut_menu_vtable);
    put_pointer(shortcut_ui + 0x238, shortcut_menu_window);
    return (uint64_t)(uintptr_t)shortcut_menu_window;
}

static int network_menu_open_in(const uint8_t *ui) {
    uint64_t window;
    memcpy(&window, ui + 0x238, sizeof(window));
    return window && i32_at((const uint8_t *)(uintptr_t)window, EARLY_STORY_MENU_KIND) == EARLY_STORY_MENU_KIND_NETWORK;
}

static void menu_scene_frame(void) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    boss_battle_scene_hook(&regs);
}

static uint64_t fake_steps_vtable[0x40];
static unsigned inn_step_sets, last_step_slot, last_step_value;
static int expect_native_hidden;

/* The Inn's step object: a set of the step slot moves the Options mode on. */
static void GUEST_HOOK_ABI fake_step_set(uint64_t steps, uint32_t slot, uint32_t value) {
    CHECK(steps == (uint64_t)(uintptr_t)(fake_inn + 0xa8));
    ++inn_step_sets;
    last_step_slot = slot;
    last_step_value = value;
    fake_inn[INN_ACT + slot] = (uint8_t)value;
}

static void page_act(void) {
    GuestRegs regs = page_regs();
    boss_battle_options_update_hook(&regs);
}

enum { NETWORK_ROW_ACTION = 0x5b4, NETWORK_ROW_TEXT = 0x370, NETWORK_ROW_LIST = 0x1520, NETWORK_ROW_COUNT = 6 };
enum { ACTION_JOIN_RANDOM = 0xa, ACTION_JOIN_FRIEND = 0xb, ACTION_END_ONLINE = 0xf, ACTION_RETURN_TO_TOWN = 1 };
enum { MESSAGE_RETURN_TO_TOWN = 0x4a9, BACK_INDEX = -2 };

static uint8_t network_rows_storage[NETWORK_ROW_COUNT][0x5c0];
static uint64_t network_row_vtable[4];
static uint64_t network_list_widgets[NETWORK_ROW_COUNT];
static int network_rows_alive, network_rows_deleted, network_setups, network_inline_build;
static int end_online_handler_calls, return_to_town_handler_calls, close_path_calls, other_choice_calls;

/* The row's deleting destructor, which the game's own list clear calls on every row it holds. */
static char network_render_heap[NETWORK_ROW_COUNT][64];

/* The text object's own string, which the row's draw reads: inline up to 15 bytes, else behind a pointer. */
static void network_render_text(uint64_t text, const char *value) {
    if (text < (uint64_t)(uintptr_t)network_rows_storage ||
        text >= (uint64_t)(uintptr_t)network_rows_storage + sizeof(network_rows_storage))
        return;
    const size_t slot = (text - (uint64_t)(uintptr_t)network_rows_storage) / sizeof(network_rows_storage[0]);
    uint8_t *object = (uint8_t *)(uintptr_t)text;
    const uint64_t length = strlen(value), capacity = length > 15 ? length : 15;
    memset(object + 0x10, 0, 0x20);
    if (length > 15) {
        snprintf(network_render_heap[slot], sizeof(network_render_heap[slot]), "%s", value);
        put_pointer(object + 0x10, network_render_heap[slot]);
    } else {
        memcpy(object + 0x10, value, length + 1);
    }
    memcpy(object + 0x20, &length, sizeof(length));
    memcpy(object + 0x28, &capacity, sizeof(capacity));
}

static const char *network_rendered(const uint8_t *row) {
    uint64_t length, capacity, data;
    memcpy(&length, row + NETWORK_ROW_TEXT + 0x20, sizeof(length));
    memcpy(&capacity, row + NETWORK_ROW_TEXT + 0x28, sizeof(capacity));
    memcpy(&data, row + NETWORK_ROW_TEXT + 0x10, sizeof(data));
    static char shown[64];
    const char *source = capacity > 15 ? (const char *)(uintptr_t)data : (const char *)(row + NETWORK_ROW_TEXT + 0x10);
    snprintf(shown, sizeof(shown), "%.*s", (int)length, source);
    return shown;
}

static void GUEST_HOOK_ABI fake_row_delete(uint64_t row) {
    CHECK(row >= (uint64_t)(uintptr_t)network_rows_storage &&
          row < (uint64_t)(uintptr_t)network_rows_storage + sizeof(network_rows_storage));
    ++network_rows_deleted;
    --network_rows_alive;
}

static uint8_t *network_list_at(void) {
    return shortcut_menu_window + NETWORK_ROW_LIST;
}

static int network_list_count(void) {
    uint64_t begin, end;
    memcpy(&begin, network_list_at() + 0x68, sizeof(begin));
    memcpy(&end, network_list_at() + 0x70, sizeof(end));
    return (int)((end - begin) / sizeof(uint64_t));
}

static void set_network_list_count(int count) {
    put_pointer(network_list_at() + 0x68, network_list_widgets);
    put_pointer(network_list_at() + 0x70, network_list_widgets + count);
}

static uint64_t GUEST_HOOK_ABI fake_network_row_at(uint64_t list, int32_t index) {
    CHECK(list == (uint64_t)(uintptr_t)network_list_at());
    return index >= 0 && index < network_list_count() ? network_list_widgets[index] : 0;
}

static uint8_t *network_row(int action) {
    uint8_t *row = network_rows_storage[0];
    for (int i = 0; i < NETWORK_ROW_COUNT; ++i) { /* a free storage slot */
        uint8_t *candidate = network_rows_storage[i];
        int used = 0;
        for (int k = 0; k < network_list_count(); ++k) used |= network_list_widgets[k] == (uint64_t)(uintptr_t)candidate;
        if (!used) {
            row = candidate;
            break;
        }
    }
    memset(row, 0, sizeof(network_rows_storage[0]));
    network_row_vtable[1] = (uint64_t)(uintptr_t)fake_row_delete;
    put_pointer(row, network_row_vtable);
    put_u32(row + NETWORK_ROW_ACTION, (uint32_t)action);
    if (action == ACTION_END_ONLINE) network_render_text((uint64_t)(uintptr_t)row + NETWORK_ROW_TEXT, "End online");
    ++network_rows_alive;
    return row;
}

static void append_network_row(uint8_t *row) {
    network_list_widgets[network_list_count()] = (uint64_t)(uintptr_t)row;
    set_network_list_count(network_list_count() + 1);
}

/* The game's list clear: every row it holds is released through its deleting destructor. */
static void clear_network_list(void) {
    for (int i = 0; i < network_list_count(); ++i) fake_row_delete(network_list_widgets[i]);
    set_network_list_count(0);
}

/* A window of its own: the previous one went with its rows. */
static void drop_network_list(void) {
    network_rows_alive -= network_list_count();
    set_network_list_count(0);
}

/* The game's Network Menu setup: each row is built whole and added to the window's list, the Return to town row through
 * the row helper on the builds that have one, and inline on the others. */
static void build_network_rows(void) {
    static const int kNative[] = {ACTION_JOIN_RANDOM, ACTION_JOIN_FRIEND, ACTION_END_ONLINE, ACTION_RETURN_TO_TOWN};
    memset(screen_texts, 0, sizeof(screen_texts));
    put_u32(network_list_at() + 0x958, 0);
    for (size_t i = 0; i < sizeof(kNative) / sizeof(*kNative); ++i) {
        GuestRegs regs;
        memset(&regs, 0, sizeof(regs));
        regs.rdi = (uint64_t)(uintptr_t)network_list_at();
        if (kNative[i] == ACTION_RETURN_TO_TOWN && !network_inline_build) {
            regs.rsi = MESSAGE_RETURN_TO_TOWN;
            boss_battle_pause_row_hook(&regs);
            if (regs.skip) continue;
        }
        uint8_t *row = network_row(kNative[i]);
        regs.rsi = (uint64_t)(uintptr_t)row;
        regs.skip = 0;
        boss_battle_network_row_hook(&regs);
        if (!regs.skip) append_network_row(row);
    }
    for (int i = 0; i < network_list_count(); ++i) { /* the game builds the labels after the rows are added */
        const uint8_t *row = (const uint8_t *)(uintptr_t)network_list_widgets[i];
        if (i32_at(row, NETWORK_ROW_ACTION) == ACTION_END_ONLINE) network_render_text((uint64_t)(uintptr_t)row + NETWORK_ROW_TEXT, "End online");
    }
}

static void GUEST_HOOK_ABI fake_network_setup(uint64_t window) {
    CHECK(window == (uint64_t)(uintptr_t)shortcut_menu_window);
    ++network_setups;
    clear_network_list();
    build_network_rows();
}

/* A call of the menu's setup: the hook decides in the Boss Battle context, the game's setup runs otherwise. */
static void run_network_setup(void) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rdi = (uint64_t)(uintptr_t)shortcut_menu_window;
    boss_battle_pause_setup_hook(&regs);
    if (!regs.skip) fake_network_setup(regs.rdi);
}

static void count_choice(int index) {
    uint64_t reads[2];
    for (int k = 0; k < 2; ++k) { /* the menu reads the chosen row twice, each through the game's row lookup */
        GuestRegs regs;
        memset(&regs, 0, sizeof(regs));
        regs.rdi = (uint64_t)(uintptr_t)network_list_at();
        regs.rsi = (uint64_t)(int64_t)index;
        boss_battle_network_choice_hook(&regs);
        reads[k] = regs.skip ? regs.rax : fake_network_row_at(regs.rdi, index);
    }
    const int action = reads[0] ? i32_at((const uint8_t *)(uintptr_t)reads[0], NETWORK_ROW_ACTION) : 0;
    if (!reads[0] && !reads[1]) ++close_path_calls;
    else if (action == ACTION_END_ONLINE) ++end_online_handler_calls;
    else if (action == ACTION_RETURN_TO_TOWN) ++return_to_town_handler_calls;
    else ++other_choice_calls;
}

static void reset_choice_counts(void) {
    end_online_handler_calls = return_to_town_handler_calls = close_path_calls = other_choice_calls = 0;
}

static int row_index_of(int action) {
    for (int i = 0; i < network_list_count(); ++i)
        if (i32_at((const uint8_t *)(uintptr_t)network_list_widgets[i], NETWORK_ROW_ACTION) == action) return i;
    return -1;
}

typedef uint8_t(GUEST_HOOK_ABI *TestListQueryFn)(uint64_t list);
enum { LIST_DECIDE_SLOT = 0x118, LIST_BACK_SLOT = 0x120, LIST_LOCKED = 0x1, SE_LIST_DECIDE = 43, SE_LIST_CANCEL = 44 };

static uint64_t network_list_vtable[0x130 / 8];
static int pad_decide, pad_back;

static uint8_t GUEST_HOOK_ABI fake_decide_query(uint64_t list) {
    CHECK(list == (uint64_t)(uintptr_t)network_list_at());
    return (uint8_t)pad_decide;
}

static uint8_t GUEST_HOOK_ABI fake_back_query(uint64_t list) {
    CHECK(list == (uint64_t)(uintptr_t)network_list_at());
    return (uint8_t)pad_back;
}

/* What the list update does from its Back query on: what it answers, which button it consumes, what it sounds, and
 * whether a choice locks the list. */
typedef struct {
    int result, cancel_consumed, decide_consumed, sound, locked;
} ListAnswer;

static ListAnswer list_update_answer(int decide, int back) {
    uint8_t *list = network_list_at();
    network_list_vtable[LIST_DECIDE_SLOT / 8] = (uint64_t)(uintptr_t)fake_decide_query;
    network_list_vtable[LIST_BACK_SLOT / 8] = (uint64_t)(uintptr_t)fake_back_query;
    put_pointer(list, network_list_vtable);
    pad_decide = decide;
    pad_back = back;
    ListAnswer answer = {-1, 0, 0, 0, 0};
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rdi = (uint64_t)(uintptr_t)list;
    regs.rax = (uint64_t)(uintptr_t)network_list_vtable;
    boss_battle_network_back_query_hook(&regs);
    CHECK(!regs.skip);
    uint64_t back_query;
    memcpy(&back_query, (const void *)(uintptr_t)(regs.rax + LIST_BACK_SLOT), sizeof(back_query));
    if (((TestListQueryFn)(uintptr_t)back_query)(regs.rdi)) {
        answer.result = BACK_INDEX;
        answer.cancel_consumed = 1;
        answer.sound = SE_LIST_CANCEL;
    } else if (fake_decide_query(regs.rdi)) {
        answer.result = i32_at(list, 0x958);
        answer.decide_consumed = 1;
        answer.sound = SE_LIST_DECIDE;
        answer.locked = 1;
        put_u32(list + 0x964, (uint32_t)i32_at(list, 0x964) | LIST_LOCKED);
    }
    return answer;
}

static int same_answer(ListAnswer a, ListAnswer b) {
    return a.result == b.result && a.cancel_consumed == b.cancel_consumed && a.decide_consumed == b.decide_consumed &&
           a.sound == b.sound && a.locked == b.locked;
}

/* Confirm on Close Network Menu is answered by the list exactly as B is, from the Back query on: no Confirm sound, no
 * Confirm consumed, no locked list, no row read for a choice. Join rows stay the game's own choices. */
static void check_closing(int close_row) {
    uint8_t *list = network_list_at();
    put_u32(list + 0x958, (uint32_t)row_index_of(ACTION_END_ONLINE));
    put_u32(list + 0x964, 0);
    const ListAnswer physical_b = list_update_answer(0, 1);
    CHECK(physical_b.result == BACK_INDEX && physical_b.sound == SE_LIST_CANCEL && !physical_b.locked);
    const ListAnswer answer = close_row ? list_update_answer(1, 0) : list_update_answer(0, 1);
    CHECK(same_answer(answer, physical_b));
    CHECK(!(i32_at(list, 0x964) & LIST_LOCKED));
    reset_choice_counts();
    count_choice(answer.result);
    CHECK(close_path_calls == 1 && end_online_handler_calls == 0 && return_to_town_handler_calls == 0 && other_choice_calls == 0);
    CHECK(i32_at((const uint8_t *)(uintptr_t)network_list_widgets[row_index_of(ACTION_END_ONLINE)], NETWORK_ROW_ACTION) ==
          ACTION_END_ONLINE);
    reset_choice_counts();
    put_u32(list + 0x958, (uint32_t)row_index_of(ACTION_JOIN_RANDOM));
    const ListAnswer join = list_update_answer(1, 0);
    CHECK(join.result == row_index_of(ACTION_JOIN_RANDOM) && join.sound == SE_LIST_DECIDE);
    count_choice(join.result);
    CHECK(other_choice_calls == 1 && close_path_calls == 0);
    put_u32(list + 0x964, 0);
    put_u32(list + 0x958, (uint32_t)row_index_of(ACTION_END_ONLINE));
}

/* The list shows exactly Join a random room, Join a Friend's room and Close Network Menu, each row an object the list
 * owns, End online's own row still carrying its native action. */
static void check_visible_rows(void) {
    CHECK(network_list_count() == 3 && network_rows_alive == 3);
    for (int i = 0; i < network_list_count(); ++i) CHECK(network_list_widgets[i] != 0);
    CHECK(i32_at(network_list_at(), 0x958) >= 0 && i32_at(network_list_at(), 0x958) < network_list_count());
    CHECK(row_index_of(ACTION_JOIN_RANDOM) == 0 && row_index_of(ACTION_JOIN_FRIEND) == 1 && row_index_of(ACTION_END_ONLINE) == 2);
    CHECK(row_index_of(ACTION_RETURN_TO_TOWN) < 0);
    CHECK(!strcmp(network_rendered((const uint8_t *)(uintptr_t)network_list_widgets[2]), "Close Network Menu"));
    CHECK(!(i32_at(network_list_at(), 0x964) & 0x1)); /* no list left locked by a choice */
    CHECK(!*network_rendered((const uint8_t *)(uintptr_t)network_list_widgets[0]) &&
          !*network_rendered((const uint8_t *)(uintptr_t)network_list_widgets[1]));
}

/* The Network Menu opened over the Boss Battle page in a window of its own, its rows built by the game's setup. */
static void check_boss_battle_network_rows(int inline_build, int close_row) {
    network_inline_build = inline_build;
    network_setups = network_rows_deleted = 0;
    drop_network_list();
    run_network_setup();
    CHECK(network_rows_deleted == (inline_build ? 1 : 0)); /* the inline Return row is released, not kept */
    check_visible_rows();
    CHECK(network_setups == 1);
    check_closing(close_row);
}

enum { KEPT_CLEAN, KEPT_WITH_RETURN, KEPT_PARTIAL };

/* The list the window holds on this open was built before the open was known, or is not whole: it is read as it is when
 * it is right, and rebuilt by the game's own setup when it holds Return to town. */
static void check_reused_network_rows(int kind, int close_row) {
    drop_network_list();
    network_setups = network_rows_deleted = 0;
    static const int kClean[] = {ACTION_JOIN_RANDOM, ACTION_JOIN_FRIEND, ACTION_END_ONLINE};
    static const int kWithReturn[] = {ACTION_JOIN_RANDOM, ACTION_JOIN_FRIEND, ACTION_END_ONLINE, ACTION_RETURN_TO_TOWN};
    static const int kPartial[] = {ACTION_JOIN_RANDOM, ACTION_RETURN_TO_TOWN};
    const int *actions = kind == KEPT_CLEAN ? kClean : kind == KEPT_WITH_RETURN ? kWithReturn : kPartial;
    const int count = kind == KEPT_CLEAN ? 3 : kind == KEPT_WITH_RETURN ? 4 : 2;
    for (int i = 0; i < count; ++i) append_network_row(network_row(actions[i]));
    put_u32(network_list_at() + 0x958, count - 1);
    memset(screen_texts, 0, sizeof(screen_texts));
    const int sets_before = text_sets;
    menu_scene_frame();
    menu_scene_frame();
    CHECK(network_setups == (kind == KEPT_CLEAN ? 0 : 1));
    CHECK(text_sets > sets_before);
    if (kind == KEPT_CLEAN) CHECK(network_rows_deleted == 0);
    check_visible_rows();
    check_closing(close_row);
    reset_choice_counts();
    count_choice(BACK_INDEX);
    CHECK(close_path_calls == 1);
}

/* Outside the Boss Battle context the rows and their handlers are the game's. */
static void check_native_network_rows(void) {
    network_inline_build = 0;
    network_rows_deleted = 0;
    drop_network_list();
    const int sets_before = text_sets;
    run_network_setup();
    CHECK(network_list_count() == 4 && network_rows_deleted == 0 && network_rows_alive == 4 && text_sets == sets_before);
    CHECK(!strcmp(network_rendered((const uint8_t *)(uintptr_t)network_list_widgets[2]), "End online"));
    reset_choice_counts();
    count_choice(row_index_of(ACTION_END_ONLINE));
    count_choice(row_index_of(ACTION_RETURN_TO_TOWN));
    CHECK(end_online_handler_calls == 1 && return_to_town_handler_calls == 1 && close_path_calls == 0);
    count_choice(BACK_INDEX);
    CHECK(close_path_calls == 1);
    put_u32(network_list_at() + 0x958, (uint32_t)row_index_of(ACTION_END_ONLINE));
    put_u32(network_list_at() + 0x964, 0);
    const ListAnswer confirm = list_update_answer(1, 0);
    CHECK(confirm.result == row_index_of(ACTION_END_ONLINE) && confirm.sound == SE_LIST_DECIDE && confirm.locked &&
          confirm.decide_consumed);
    reset_choice_counts();
    count_choice(confirm.result);
    CHECK(end_online_handler_calls == 1 && close_path_calls == 0);
    put_u32(network_list_at() + 0x964, 0);
    drop_network_list();
}

/* The game closes the Network Menu and terminates the Options page it was opened over, while its Inn waits in the
 * Options mode's step 1: no page callback and no Inn menu frame run, only the scene frame. The scene fader covers the
 * Inn, which is moved on as for a closed page, leaves the Options mode, and runs its menu frame, which reopens the page
 * as on a first open; the cover goes once the page runs. */
static void close_menu_and_reopen(int row) {
    put_pointer(fake_inn + 0xa8, fake_steps_vtable);
    fake_steps_vtable[0xa0 / 8] = (uint64_t)(uintptr_t)fake_step_set;
    fake_inn[INN_ACT] = 0x8e;
    fake_inn[INN_STEP] = 1;
    inn_step_sets = 0;
    const int fades_before = fades.calls;
    put_pointer(shortcut_ui + 0x238, NULL);
    CHECK(page_update().skip && page_result(3).rax == 0xffffffffu);
    menu_scene_frame();
    CHECK(network_menu_quarantine_active() && network_menu_pad_hold(1) == 1);
    CHECK(boss_battle_screen_shown() && !boss_battle_menu_reopen_pending() && inn_step_sets == 0);
    CHECK(fades.calls == fades_before + 1 && fades.last_colour == FADE_BLACK && fades.last_frames == 5 &&
          fades.last_layer == FADE_LAYER);
    GuestRegs covered = native_fade(scene_fader_address(), 0, 30, 0x27); /* the cover holds against the game's own fades */
    CHECK(covered.rsi == (uint64_t)(uint32_t)FADE_BLACK && covered.rdx == 0 && covered.rcx == FADE_LAYER);
    CHECK(page_update().skip && page_result(3).rax == 0xffffffffu);
    GuestRegs menu = result_frame(); /* the Inn's menu frame running meanwhile must not end the visit */
    CHECK(boss_battle_screen_shown() && boss_battle_online_selected() && !(menu.skip && menu.rax == BOSS_BATTLE_INN_OPTIONS_ROW));
    put_u32(fake_body + 0x964, (uint32_t)(i32_at(fake_body, 0x964) | 0x1000)); /* an unrelated native bit */
    const int32_t stale = i32_at(fake_body, 0x964);

    /* the old page terminated while the cover still fades: the Inn is not moved on under a visible menu */
    put_u32(fader_owner + 0xf4, 10);
    put_u32(fake_window + 0xa4, 2);
    menu_scene_frame();
    CHECK(inn_step_sets == 0 && !boss_battle_menu_reopen_pending() && fades.calls == fades_before + 1);
    put_u32(fader_owner + 0xf4, 0);

    /* covered: the Inn takes its own step for a closed page; no window is created here */
    menu_scene_frame();
    CHECK(inn_step_sets == 1 && last_step_slot == 1 && last_step_value == 2 && fake_inn[INN_STEP] == 2);
    CHECK(boss_battle_menu_reopen_pending() && !boss_battle_screen_shown() && !boss_battle_screen_requested());
    CHECK(!page_update().skip && boss_battle_network_menu_returning());
    menu_scene_frame();
    CHECK(inn_step_sets == 1 && fades.calls == fades_before + 1);

    /* the Inn leaves the Options mode and runs its menu frame: the reopen is the first open's own Options row */
    fake_inn[INN_ACT] = 0x5b;
    fake_inn[INN_STEP] = 0;
    menu_scene_frame();
    menu = result_frame();
    CHECK(menu.skip && menu.rax == BOSS_BATTLE_INN_OPTIONS_ROW && boss_battle_screen_requested() &&
          !boss_battle_menu_reopen_pending());
    menu = result_frame();
    CHECK(!(menu.skip && menu.rax == BOSS_BATTLE_INN_OPTIONS_ROW)); /* opened once */
    CHECK(fades.calls == fades_before + 1); /* the Inn's lifecycle ran under the cover */

    /* the Options mode builds the page, in the old page's storage */
    fake_options_page(8);
    put_u32(fake_body + 0x964, (uint32_t)stale);
    build_page();
    CHECK(boss_battle_screen_shown() && page_hidden() == expect_native_hidden && i32_at(fake_body, 0x958) == row);
    CHECK(i32_at(fake_body, 0x964) == (expect_native_hidden ? stale : (stale & ~0x400)) && (i32_at(fake_body, 0x964) & 0x1000));

    /* built is not running: only the page's own act, list update and result make it the returned page */
    CHECK(boss_battle_network_menu_returning() && fades.calls == fades_before + 1);
    page_result(-1);
    CHECK(boss_battle_network_menu_returning() && fades.calls == fades_before + 1);
    fake_inn[INN_ACT] = 0x8e; /* the page shown, its window running its act: not the proof yet */
    fake_inn[INN_STEP] = 1;
    page_act();
    CHECK(boss_battle_network_menu_returning() && fades.calls == fades_before + 1);
    CHECK(!page_update().skip);
    CHECK(network_menu_pad_hold(0) == 0 && !network_menu_quarantine_active());
    shortcut_now += 16;
    CHECK(fades.calls == fades_before + 1);
    const GuestRegs regs = page_result(-1);
    CHECK(!boss_battle_network_menu_returning());
    CHECK(fades.calls == fades_before + 2 && fades.last_colour == 0 && fades.last_frames == 10 && fades.last_layer == FADE_LAYER);
    CHECK(native_fade_passes());
    CHECK(shortcut_hint_up() && network_shortcut_owner() == NETWORK_SHORTCUT_BOSS_BATTLE && !regs.skip &&
          boss_battle_online_selected());
}

static uint8_t info_slot_data[0x400], info_live_character[0x400], room_info_model[0x80];
static uint64_t info_live_slot;
static int info_native_controllable = 1;
enum { INFO_ID = 0x140, INFO_BLOCK = 0x148, INFO_ID_SIZE = 8, INFO_BLOCK_SIZE = 48 };

static uint64_t GUEST_HOOK_ABI fake_info_character(uint64_t slot) {
    return slot == info_live_slot ? (uint64_t)(uintptr_t)info_live_character : 0;
}

/* The game's room info builder for one party slot: the four predicates are the hooked calls, and the host fields are
 * copied by the game's own code (8 bytes, then 48) from the slot as it holds them at that moment, never by the hooks. */
enum { INFO_PRESENT = 0x5e, INFO_SLOT_CLASSES = 0x60, INFO_HOST_CLASS = 0x64, LEAD_CLASS = 3 };

static void run_room_info_builder(uint64_t slot, uint64_t r14) {
    memset(room_info_model, 0, sizeof(room_info_model));
    memset(room_info_model + INFO_SLOT_CLASSES, 0xff, 4);
    room_info_model[INFO_HOST_CLASS] = 0xff;
    GuestRegs a;
    memset(&a, 0, sizeof(a));
    a.rdi = slot;
    a.r14 = r14;
    boss_battle_info_character_hook(&a);
    const uint64_t character = a.skip ? a.rax : fake_info_character(slot);
    if (character) {
        const int live = character == (uint64_t)(uintptr_t)info_live_character;
        GuestRegs b = a;
        b.skip = 0;
        b.rdi = character;
        boss_battle_info_index_hook(&b);
        GuestRegs c = a;
        c.skip = 0;
        c.rdi = slot;
        boss_battle_info_controllable_hook(&c);
        GuestRegs d = a;
        d.skip = 0;
        d.r15 = 7;
        boss_battle_info_match_hook(&d);
        if ((c.skip ? c.rax : (uint64_t)(live && info_native_controllable)) &&
            (d.skip ? d.rax : (live ? 7u : 8u)) == 7u) {
            memcpy(room_info_model, (const void *)(uintptr_t)(slot + INFO_ID), INFO_ID_SIZE);
            memcpy(room_info_model + INFO_ID_SIZE, (const void *)(uintptr_t)(slot + INFO_BLOCK), INFO_BLOCK_SIZE);
        }
    }
    network_access_room_info_built((uint64_t)(uintptr_t)room_info_model);
}

static uint8_t info_lead_record[0x23b0];

static const unsigned char *GUEST_HOOK_ABI info_lead_selector(uint64_t facade) {
    (void)facade;
    return info_lead_record;
}

static void set_identity(uint8_t *record, uint8_t seed) {
    for (int i = 0; i < INFO_ID_SIZE; ++i) record[INFO_ID + i] = (uint8_t)(seed + i);
    for (int i = 0; i < INFO_BLOCK_SIZE; ++i) record[INFO_BLOCK + i] = (uint8_t)(seed * 3 + i + 1);
}

/* The friend row's portrait: the host's class, the first slot's class and its present bit, from the lead character. */
static int room_info_has_lead_class(void) {
    return room_info_model[INFO_HOST_CLASS] == LEAD_CLASS && room_info_model[INFO_SLOT_CLASSES] == LEAD_CLASS &&
           (room_info_model[INFO_PRESENT] & 0x20);
}

static int room_info_is(uint8_t seed) {
    uint8_t expected[INFO_ID_SIZE + INFO_BLOCK_SIZE];
    for (int i = 0; i < INFO_ID_SIZE; ++i) expected[i] = (uint8_t)(seed + i);
    for (int i = 0; i < INFO_BLOCK_SIZE; ++i) expected[INFO_ID_SIZE + i] = (uint8_t)(seed * 3 + i + 1);
    return !memcmp(room_info_model, expected, sizeof(expected));
}

/* A fresh process straight into the Inn: no live character ever existed, the party slot is empty, no room exists. The
 * game's builder is shown slot 0 as a live character, and the save's lead character record as its identity; the slot's
 * own bytes are back afterwards, a slot that has its own identity keeps it, and nothing is made up. */
static void check_room_info_builder(int hosting_here) {
    boss_battle_set_room_info_character(fake_info_character);
    boss_battle_set_lead_selector(info_lead_selector);
    memset(info_slot_data, 0, sizeof(info_slot_data));
    memset(info_lead_record, 0, sizeof(info_lead_record));
    const uint64_t slot = (uint64_t)(uintptr_t)info_slot_data;
    uint8_t empty[sizeof(info_slot_data)];
    memset(empty, 0, sizeof(empty));
    info_live_slot = 0;
    /* an empty lead record: nothing is made up */
    run_room_info_builder(slot, 0);
    CHECK(room_info_model[0] == 0 && !memcmp(info_slot_data, empty, sizeof(empty)));
    /* the save's lead character: its own bytes, 8 then 48, through the game's own copy, and its class for the portrait */
    set_identity(info_lead_record, 5);
    info_lead_record[0x27] = LEAD_CLASS;
    run_room_info_builder(slot, 0);
    if (hosting_here) CHECK(room_info_is(5) && room_info_has_lead_class());
    else CHECK(room_info_model[0] == 0 && room_info_model[INFO_HOST_CLASS] == 0xff && !room_info_model[INFO_PRESENT]);
    CHECK(!memcmp(info_slot_data, empty, sizeof(empty))); /* the Inn's slot is as it was */
    run_room_info_builder(slot, 0); /* every build, not once */
    if (hosting_here) CHECK(room_info_is(5));
    run_room_info_builder(slot, 1); /* another slot is never given it */
    CHECK(room_info_model[0] == 0 && !memcmp(info_slot_data, empty, sizeof(empty)));
    /* a slot that holds its own identity keeps it */
    set_identity(info_slot_data, 9);
    uint8_t native[sizeof(info_slot_data)];
    memcpy(native, info_slot_data, sizeof(native));
    run_room_info_builder(slot, 0);
    if (hosting_here) CHECK(room_info_is(9));
    CHECK(!memcmp(native, info_slot_data, sizeof(native)));
    /* the Inn's character found but not controllable, slot empty: the same lead identity */
    memset(info_slot_data, 0, sizeof(info_slot_data));
    info_live_slot = slot;
    info_native_controllable = 0;
    run_room_info_builder(slot, 0);
    if (hosting_here) CHECK(room_info_is(5));
    else CHECK(room_info_model[0] == 0);
    CHECK(!memcmp(info_slot_data, empty, sizeof(empty)));
    info_native_controllable = 1;
    info_live_slot = 0;
    boss_battle_set_lead_selector(NULL);
    boss_battle_set_room_info_character(NULL);
}

static int round_trips;
static int room_wishes;
static uint64_t room_wish_scene;

static int room_wish_releasing, room_wish_wanted;

static void GUEST_HOOK_ABI fake_room_wish(uint64_t scene) {
    ++room_wishes;
    room_wish_scene = scene;
    room_wish_releasing = network_access_releasing();
    room_wish_wanted = network_access_room_wanted();
}

enum {
    JOIN_RECORD_STATE = 4, JOIN_SCENE_STAGE = 0xf0, UNIT_SIDE_AT = 0x2980, UNIT_STATE_AT = 0x7b8, UNIT_COUNTED_AT = 0x1098,
    JOIN_UNITS = 3, STAGE_INN = 2
};

static uint8_t join_scene[0x200], join_unit_data[JOIN_UNITS][0x2a00];
static uint64_t join_unit_vtable[0x1100 / 8];
static int join_place_closed, join_unit_present[JOIN_UNITS], join_unit_counted, join_action_busy;

static uint64_t GUEST_HOOK_ABI fake_join_unit(uint64_t units, uint32_t index) {
    (void)units;
    return index < JOIN_UNITS && join_unit_present[index] ? (uint64_t)(uintptr_t)join_unit_data[index] : 0;
}
static uint64_t GUEST_HOOK_ABI fake_unit_counted(uint64_t unit) {
    (void)unit;
    return 0xab00 | (uint64_t)(join_unit_counted != 0);
}

/* The game's join reply for a scene at act 2: closed (1) when the place keeps no room, when a unit of the player's side
 * that the game counts is not in state 3, or when the player's action is the busy one. Only the unit read is hooked; the
 * place and the action are the game's own. */
static int32_t native_join_state(void) {
    uint8_t record[0x20];
    memset(record, 0, sizeof(record));
    int32_t state = join_place_closed;
    for (uint32_t i = 0; i < JOIN_UNITS; ++i) {
        GuestRegs unit;
        memset(&unit, 0, sizeof(unit));
        unit.rdi = 0x2000;
        unit.rsi = i;
        boss_battle_join_unit_hook(&unit);
        const uint64_t unit_at = unit.skip ? unit.rax : fake_join_unit(unit.rdi, i);
        if (unit_at && i32_at((const uint8_t *)(uintptr_t)unit_at, UNIT_SIDE_AT) == 1 && (fake_unit_counted(unit_at) & 0xff) &&
            i32_at((const uint8_t *)(uintptr_t)unit_at, UNIT_STATE_AT) != 3)
            state = 1;
    }
    if (join_action_busy) state = 1;
    memcpy(record + JOIN_RECORD_STATE, &state, sizeof(state));
    GuestRegs stored;
    memset(&stored, 0, sizeof(stored));
    stored.r13 = (uint64_t)(uintptr_t)record;
    stored.r14 = (uint64_t)(uintptr_t)join_scene;
    boss_battle_join_state_hook(&stored);
    CHECK(!stored.skip);
    int32_t serialized;
    memcpy(&serialized, record + JOIN_RECORD_STATE, sizeof(serialized));
    CHECK(serialized == state);
    return serialized;
}

static void set_join_scene(int place_closed, int unit_state, int counted, int action_busy) {
    memset(join_scene, 0, sizeof(join_scene));
    put_u32(join_scene + JOIN_SCENE_STAGE, STAGE_INN);
    join_scene[0x5b] = 2;
    join_place_closed = place_closed;
    memset(join_unit_data, 0, sizeof(join_unit_data));
    memset(join_unit_present, 0, sizeof(join_unit_present));
    join_unit_vtable[UNIT_COUNTED_AT / 8] = (uint64_t)(uintptr_t)fake_unit_counted;
    if (unit_state >= 0) {
        join_unit_present[0] = 1;
        put_pointer(join_unit_data[0], join_unit_vtable);
        put_u32(join_unit_data[0] + UNIT_SIDE_AT, 1);
        put_u32(join_unit_data[0] + UNIT_STATE_AT, (uint32_t)unit_state);
    }
    join_unit_counted = counted;
    join_action_busy = action_busy;
}

/* The waiting host's own character in the Inn (counted, state 0) is the only read an open Boss Battle host room changes:
 * the game then computes 0. A fight, a closed room or a non-host keeps the game's 1; the place and the action keep closing
 * the room as the game decides, for every host. */
static void check_join_state(int open_boss_host) {
    const BossBattleJoinProbes probes = {fake_join_unit};
    boss_battle_set_join_probes(&probes);
    set_join_scene(0, -1, 0, 0);
    CHECK(native_join_state() == 0);
    set_join_scene(0, 3, 1, 0);
    CHECK(native_join_state() == 0); /* a unit in state 3 never closes it */
    set_join_scene(0, 0, 0, 0);
    CHECK(native_join_state() == 0); /* nor one the game does not count */
    set_join_scene(0, 0, 1, 0);
    CHECK(native_join_state() == (open_boss_host ? 0 : 1));
    set_join_scene(1, 0, 1, 0);
    CHECK(native_join_state() == 1); /* the place stays the game's */
    set_join_scene(0, 0, 1, 1);
    CHECK(native_join_state() == 1); /* the action stays the game's */
    boss_battle_set_join_probes(NULL);
    set_join_scene(0, 0, 1, 0);
    GuestRegs none;
    memset(&none, 0, sizeof(none));
    boss_battle_join_unit_hook(&none);
    CHECK(!none.skip);
}

enum { SEAT_BASE = 0xa8, SEAT_SIZE = 0x23b0, SEAT_OWNER = 0x21e0, SERVER_CONNECTED_SLOTS = 0x8fb0 };
static uint8_t seat_party[SERVER_CONNECTED_SLOTS + 0x10];
static int seat_lookups;

static uint64_t GUEST_HOOK_ABI fake_seat_lookup(uint64_t party, uint32_t player) {
    ++seat_lookups;
    for (int i = 0; i < 4; ++i) {
        const uint8_t *slot = (const uint8_t *)(uintptr_t)(party + SEAT_BASE + (uint64_t)i * SEAT_SIZE);
        if (i32_at(slot, SEAT_OWNER) == 0 && i32_at(slot, SEAT_OWNER - 4) == (int32_t)player) return (uint64_t)i;
    }
    return 4;
}

static uint64_t seat_answer(uint32_t player, int *skipped) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rdi = (uint64_t)(uintptr_t)seat_party;
    regs.rsi = player;
    boss_battle_party_slot_hook(&regs);
    *skipped = (int)regs.skip;
    return regs.skip ? regs.rax : fake_seat_lookup(regs.rdi, player);
}

/* The scene's online party pass, on a Boss Battle visitor whose local player has no seat yet: an empty slot, which the
 * pass skips, never the slot at 0. A seated player gets its own slot; outside Boss Battle Online the game's lookup runs. */
static void check_party_seat(int boss_online) {
    boss_battle_set_party_slot_lookup(fake_seat_lookup);
    memset(seat_party, 0, sizeof(seat_party));
    for (int i = 0; i < 4; ++i) put_u32(seat_party + SEAT_BASE + (size_t)i * SEAT_SIZE + SEAT_OWNER, 0xffffffffu);
    put_u32(seat_party + SEAT_BASE + SEAT_OWNER, 0x10); /* the host's character, remote */
    int skipped;
    const uint64_t unseated = seat_answer(0, &skipped);
    if (boss_online) CHECK(skipped && unseated == 1 && i32_at(seat_party + SEAT_BASE + SEAT_SIZE, SEAT_OWNER) == -1);
    else CHECK(!skipped && unseated == 4);
    put_u32(seat_party + SEAT_BASE + SEAT_SIZE + SEAT_OWNER, 0);
    put_u32(seat_party + SEAT_BASE + SEAT_SIZE + SEAT_OWNER - 4, 0);
    CHECK(seat_answer(0, &skipped) == 1);
    boss_battle_set_party_slot_lookup(NULL);
}

static uint8_t card_object[0x400], card_line[0x40];

static int friend_in_boss_room(const char *online_id) { return !strcmp(online_id, "BossHost"); }

static void set_card_online_id(const char *name) {
    uint8_t *text = card_object + 0x370;
    const uint64_t length = strlen(name), capacity = 15;
    memset(text + 0x10, 0, 0x20);
    memcpy(text + 0x10, name, length);
    memcpy(text + 0x20, &length, sizeof(length));
    memcpy(text + 0x28, &capacity, sizeof(capacity));
}

/* The friend card's location line, from the Online ID the card already holds: a friend the lobby says is in a Boss Battle
 * room reads Boss Battle; any other friend keeps the game's table, and so does any friend when nothing says. */
static void check_card_location(const char *name, int boss_room) {
    set_card_online_id(name);
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rdi = (uint64_t)(uintptr_t)card_line;
    regs.r15 = (uint64_t)(uintptr_t)card_object;
    const int before = text_sets;
    boss_battle_location_text_hook(&regs);
    if (boss_room)
        CHECK(regs.skip && text_sets == before + 1 && !strcmp(text_at(card_line, 0), "Boss Battle"));
    else
        CHECK(!regs.skip && text_sets == before);
}

/* B and the menu's Close row take the same close path: the cycle after either is the same. */
static void network_menu_round_trip(int row) {
    const int close_row = round_trips++ & 1;
    set_page_cursor(row);
    CHECK(network_shortcut_pad_filter(1) == 1);
    shortcut_now += 16;
    page_result(-1);
    CHECK(page_hidden() && network_menu_open_in(shortcut_ui) && !shortcut_hint_up());
    network_shortcut_pad_filter(0);
    const int trip = round_trips - 1, route = trip % 4; /* built now (by helper, then inline), kept clean, kept with Return, partial */
    if (route == 0) check_boss_battle_network_rows((trip / 4) & 1, close_row);
    else check_reused_network_rows(route - 1, close_row);
    close_menu_and_reopen(row);
    check_native_network_rows();
}

/* The page arms the shared Network Menu shortcut and its hint, takes no part in the menu itself, and never closes. */
static void test_screen_shortcut_hooks(void) {
    start_guarded();
    use_fake_inn_functions();
    use_fake_screen_functions();
    static const int32_t kHelp[6] = {953, 954, 955, 957, 958, 759};
    boss_battle_set_inn_help_table((uint64_t)(uintptr_t)kHelp);
    memset(shortcut_manager, 0, sizeof(shortcut_manager));
    memset(shortcut_ui, 0, sizeof(shortcut_ui));
    memset(shortcut_hint_object, 0, sizeof(shortcut_hint_object));
    put_pointer(fake_inn_context + 0x20, shortcut_manager);
    put_pointer(shortcut_manager + NETWORK_HINT_MANAGER_UI, shortcut_ui);
    memset(fader_owner, 0, sizeof(fader_owner));
    memset(&fades, 0, sizeof(fades));
    put_pointer(shortcut_manager + 0xd8, fader_owner);
    put_pointer(shortcut_manager + HYDELAND_MANAGER_SCENE, game_scene);
    network_access_set_room_wish(fake_room_wish);
    room_wishes = 0;
    const BossBattleFadeFunctions fade_functions = {fake_get_fader, fake_fade};
    boss_battle_set_fade_functions(&fade_functions);
    round_trips = 0;
    boss_battle_set_network_row_at(fake_network_row_at);
    const BossBattlePauseFunctions network_setup = {fake_network_setup, 0, 0};
    boss_battle_set_pause_functions(&network_setup);
    shortcut_hint_vtable[NETWORK_HINT_SHOW_SLOT / 8] = (uint64_t)(uintptr_t)shortcut_hint_show;
    shortcut_hint_creates = shortcut_hint_shows = shortcut_hint_hides = 0;
    const NetworkHintFunctions hint = {shortcut_hint_create, (NetworkHintTextFn)(uintptr_t)shortcut_hint_set_text,
                                       shortcut_hint_hide, shortcut_hint_refresh, 0};
    network_hint_configure(&hint);
    network_hint_set_portuguese(0);
    network_access_set_installed(1);
    network_access_set_clock(shortcut_clock);
    network_shortcut_reset();

    fake_options_page(8);
    set_inn(7, 7, BOSS_BATTLE_INN_ROW, BOSS_BATTLE_INN_ROW);
    GuestRegs regs = result_frame();
    CHECK(regs.skip && boss_battle_screen_requested());
    boss_battle_online_set_available(1);
    boss_battle_online_set_hosting(1);
    boss_battle_online_visit_begin(1);
    build_page();
    CHECK(boss_battle_screen_shown());

    /* the host's room is asked for from the Inn, as Early Story's is from town: before any Network Menu, before a boss */
    CHECK(room_wishes == 0);
    shortcut_now += 16;
    regs = page_result(-1);
    CHECK(room_wishes == 1 && room_wish_scene == (uint64_t)(uintptr_t)game_scene);
    /* the state that failed in the game: prompt accepted, page up, no room yet and no room-hosting predicate needed */
    CHECK(boss_battle_online_selected() && !boss_battle_online_in_room() && !boss_battle_online_visitor());
    check_room_info_builder(1);
    CHECK(!boss_battle_online_in_room() && !boss_battle_online_is_host());
    /* without the game's menu request the page never owns the shortcut */
    CHECK(network_shortcut_owner() == NETWORK_SHORTCUT_NONE && !network_shortcut_armed() && !shortcut_hint_up());

    /* the Inn has no character and no director poll: the page itself arms the shortcut and the shared hint */
    network_menu_set_create(shortcut_menu_create);
    shortcut_now += 16;
    regs = page_result(-1);
    CHECK(network_shortcut_owner() == NETWORK_SHORTCUT_BOSS_BATTLE && network_shortcut_armed() && shortcut_hint_up() &&
          shortcut_hint_creates == 1 && shortcut_hint_shows == 1 && !strcmp(shortcut_hint_text, "@#(2) Open Network Menu"));
    CHECK(!(regs.r15 & 1) && boss_battle_screen_shown());
    regs = page_result(-1);
    CHECK(!(regs.r15 & 1) && boss_battle_screen_shown() && shortcut_hint_shows == 1 && shortcut_menu_creates == 0);

    /* a request the game cannot serve: logged, the page is untouched, the press is spent */
    set_page_cursor(5);
    const int32_t cursor = i32_at(fake_body, 0x958);
    shortcut_menu_refuse = 1;
    CHECK(network_shortcut_pad_filter(1) == 1);
    shortcut_now += 16;
    regs = page_result(-1);
    CHECK(shortcut_menu_creates == 1 && !(regs.r15 & 1) && boss_battle_screen_shown() &&
          i32_at(fake_body, 0x958) == cursor && !network_menu_open_in(shortcut_ui));
    network_shortcut_pad_filter(0);
    shortcut_menu_refuse = 0;

    /* Y / Triangle: hidden from the game, then the game's menu request for pad 0 and player 0, made the Network Menu;
     * the page is suspended: hidden, no input, and nothing else of it changes */
    const unsigned created = network_access_menus_created();
    memset(shortcut_menu_window, 0, sizeof(shortcut_menu_window));
    put_u32(shortcut_menu_window + EARLY_STORY_MENU_KIND, EARLY_STORY_MENU_KIND_NETWORK);
    check_native_network_rows(); /* a Network Menu the Boss Battle page did not open keeps the game's rows */
    memset(shortcut_menu_window, 0, sizeof(shortcut_menu_window));
    CHECK(!page_hidden() && !page_update().skip);
    CHECK(network_shortcut_pad_filter(1) == 1);
    shortcut_now += 16;
    regs = page_result(3);
    CHECK(shortcut_menu_creates == 2 && shortcut_menu_inits == 1 && shortcut_menu_pad == 0 &&
          i32_at(shortcut_menu_window, 0xf8) == 0 && network_menu_open_in(shortcut_ui) &&
          network_access_menus_created() == created + 1);
    CHECK(regs.rax == 0xffffffffu && !(regs.r15 & 1) && boss_battle_screen_shown() && i32_at(fake_body, 0x958) == cursor &&
          !shortcut_hint_up());
    CHECK(page_hidden() && boss_battle_online_selected() && boss_battle_online_hosting());
    check_boss_battle_network_rows(0, 0);
    put_u32(shortcut_menu_window + EARLY_STORY_MENU_KIND, 5); /* the Inn's own menu kind is not the Network Menu */
    check_native_network_rows();
    put_u32(shortcut_menu_window + EARLY_STORY_MENU_KIND, EARLY_STORY_MENU_KIND_NETWORK);
    CHECK(network_shortcut_pad_filter(1) == 1); /* still held: still hidden */
    network_shortcut_pad_filter(0);

    /* suspended: the list reads no pad (no cursor, no sound, no Back), nothing is confirmed, the page is not closed */
    for (int frame = 0; frame < 3; ++frame) {
        shortcut_now += 16;
        const GuestRegs update = page_update();
        CHECK(update.skip && update.rax == 0xffffffffu && page_hidden());
        regs = page_result(3);
        CHECK(regs.rax == 0xffffffffu && !(regs.r15 & 1) && boss_battle_screen_shown());
        CHECK(shortcut_menu_creates == 2 && i32_at(fake_body, 0x958) == cursor && boss_battle_online_selected());
        CHECK(!network_menu_quarantine_active()); /* the quarantine starts at the close, not before */
        regs = page_move(1);
        CHECK(regs.skip && regs.rax == 0 && i32_at(fake_body, 0x958) == cursor);
    }

    /* closed, the old page terminated, a new one built on the same boss, the same visit */
    close_menu_and_reopen(cursor);
    CHECK(boss_battle_menu_cursor() == cursor - 1 && !boss_battle_cursor_fight_all() && boss_battle_online_hosting());

    /* again and again, on a boss row and on Fight All: nothing leaks from one cycle to the next */
    for (int cycle = 0; cycle < 3; ++cycle) network_menu_round_trip(cycle == 1 ? 1 : 5);
    CHECK(boss_battle_menu_cursor() == 4);
    network_menu_round_trip(BOSS_BATTLE_SCREEN_FIGHT_ALL_ROW);
    CHECK(boss_battle_cursor_fight_all());
    network_menu_round_trip(3);
    CHECK(!boss_battle_cursor_fight_all() && boss_battle_menu_cursor() == 2);

    /* a window of the Inn's own is up: the shortcut and the hint go, the page keeps its input */
    memset(shortcut_menu_window, 0, sizeof(shortcut_menu_window));
    put_u32(shortcut_menu_window + EARLY_STORY_MENU_KIND, 5);
    put_pointer(shortcut_ui + 0x238, shortcut_menu_window);
    shortcut_now += 16;
    regs = page_result(-1);
    CHECK(!(regs.r15 & 1) && network_shortcut_owner() == NETWORK_SHORTCUT_NONE && !shortcut_hint_up());
    CHECK(!page_hidden() && !page_update().skip);
    regs = page_move(1);
    CHECK(!regs.skip || regs.rax == 1); /* the page's own move, not the blocked one */

    /* a Network Menu opened by anything else suspends the page the same way */
    put_u32(shortcut_menu_window + EARLY_STORY_MENU_KIND, EARLY_STORY_MENU_KIND_NETWORK);
    shortcut_now += 16;
    regs = page_result(-1);
    CHECK(regs.rax == 0xffffffffu && !(regs.r15 & 1) && network_shortcut_owner() == NETWORK_SHORTCUT_NONE &&
          !shortcut_hint_up() && page_hidden() && page_update().skip);
    close_menu_and_reopen(i32_at(fake_body, 0x958));

    /* a boss start under way hides both */
    boss_battle_start_transition_set(BOSS_BATTLE_START_FADING);
    shortcut_now += 16;
    page_result(-1);
    CHECK(network_shortcut_owner() == NETWORK_SHORTCUT_NONE && !shortcut_hint_up());
    boss_battle_start_transition_set(BOSS_BATTLE_START_NONE);

    /* Early Story, with its conditions, keeps the shortcut from the page */
    network_shortcut_arm(NETWORK_SHORTCUT_EARLY_STORY, 1);
    shortcut_now += 16;
    page_result(-1);
    CHECK(network_shortcut_owner() == NETWORK_SHORTCUT_EARLY_STORY && !shortcut_hint_up());
    network_shortcut_arm(NETWORK_SHORTCUT_EARLY_STORY, 0);

    /* the room is asked for once per visit, whatever the Network Menu, the page and the room do */
    CHECK(room_wishes == 1);
    boss_battle_online_room_joined(1, BOSS_BATTLE_ROOM_OPEN, 1, 0);
    CHECK(boss_battle_online_is_host() && boss_battle_online_room_attr() == BOSS_BATTLE_ROOM_OPEN);
    shortcut_now += 16;
    page_result(-1);
    CHECK(room_wishes == 1);
    shortcut_now += 16;
    page_result(-1);
    CHECK(room_wishes == 1);
    boss_battle_online_room_left("test");
    CHECK(!boss_battle_online_in_room());
    shortcut_now += 16;
    page_result(-1);
    page_result(-1);
    CHECK(room_wishes == 1); /* a room left to join a friend's is not made again */
    boss_battle_online_room_joined(1, BOSS_BATTLE_ROOM_OPEN, 1, 0);

    /* a page that never comes up live: after the retries the Inn is left to its own menu and the cover goes */
    set_page_cursor(4);
    shortcut_now += 16;
    page_result(-1);
    CHECK(network_shortcut_pad_filter(1) == 1);
    shortcut_now += 16;
    page_result(-1);
    network_shortcut_pad_filter(0);
    put_pointer(shortcut_ui + 0x238, NULL);
    fake_inn[INN_ACT] = 0x8e;
    fake_inn[INN_STEP] = 1;
    inn_step_sets = 0;
    const int cover_before = fades.calls;
    menu_scene_frame();
    CHECK(fades.calls == cover_before + 1 && fades.last_colour == FADE_BLACK);
    put_u32(fake_window + 0xa4, 2);
    menu_scene_frame();
    CHECK(inn_step_sets == 1 && boss_battle_menu_reopen_pending());
    for (int attempt = 1; attempt <= 2; ++attempt) {
        fake_inn[INN_ACT] = 0x5b;
        fake_inn[INN_STEP] = 0;
        CHECK(result_frame().rax == BOSS_BATTLE_INN_OPTIONS_ROW);
        fake_options_page(8);
        put_u32(fake_window + 0xa4, 2);
        build_page();
        fake_inn[INN_ACT] = 0x8e;
        fake_inn[INN_STEP] = 1;
        menu_scene_frame();
        CHECK(inn_step_sets == (unsigned)attempt + 1 && fades.calls == cover_before + attempt);
    }
    CHECK(!boss_battle_menu_reopen_pending() && !boss_battle_screen_shown() && !boss_battle_screen_requested());
    fake_inn[INN_ACT] = 0x5b;
    fake_inn[INN_STEP] = 0;
    const int32_t no_press = -1;
    memcpy(inn_list() + 0x960, &no_press, sizeof(no_press)); /* the player presses nothing on the Inn menu */
    menu_scene_frame();
    const GuestRegs menu_after_failure = result_frame();
    CHECK(!(menu_after_failure.skip && menu_after_failure.rax == BOSS_BATTLE_INN_OPTIONS_ROW) && !boss_battle_screen_requested() &&
          !boss_battle_menu_reopen_pending());
    CHECK(fades.calls == cover_before + 2 && fades.last_colour == 0 && fades.last_frames == 10 && native_fade_passes());
    CHECK(!boss_battle_network_menu_returning() && network_menu_pad_hold(0) == 0); /* released once, not stuck */
    set_inn(7, 7, BOSS_BATTLE_INN_ROW, BOSS_BATTLE_INN_ROW);
    CHECK(result_frame().skip && boss_battle_screen_requested());
    fake_options_page(8);
    build_page();
    CHECK(boss_battle_screen_shown());
    set_page_cursor(3);
    shortcut_now += 16;
    page_result(-1);
    CHECK(shortcut_hint_up());

    /* a guest with Boss Battle off, in the host's room: the same shortcut while the room and the menu allow it */
    boss_battle_online_visit_end();
    boss_battle_online_set_hosting(0);
    boss_battle_online_room_joined(0, BOSS_BATTLE_ROOM_OPEN, 2, 1);
    CHECK(boss_battle_online_visitor() && !boss_battle_online_hosting());
    shortcut_now += 16;
    page_result(-1);
    CHECK(network_shortcut_owner() == NETWORK_SHORTCUT_BOSS_BATTLE && shortcut_hint_up());
    network_menu_round_trip(3);
    CHECK(boss_battle_online_visitor() && !boss_battle_online_hosting() && boss_battle_screen_shown());

    /* the join's scene reload rebuilds the Inn under the Network Menu: the visitor stays, the dead page is dropped, and
     * the new Inn's menu build asks for the guest's page once through the shared reopen */
    set_page_cursor(3);
    CHECK(network_shortcut_pad_filter(1) == 1);
    shortcut_now += 16;
    page_result(-1);
    network_shortcut_pad_filter(0);
    CHECK(page_hidden() && boss_battle_network_menu_returning() == 0 && boss_battle_online_visitor());
    GuestRegs rebuilt;
    memset(&rebuilt, 0, sizeof(rebuilt));
    put_pointer(shortcut_ui + 0x238, NULL); /* the reload ends the Network Menu window too */
    boss_battle_inn_destroyed_hook(&rebuilt);
    CHECK(boss_battle_online_visitor() && boss_battle_online_selected() && !boss_battle_screen_shown() &&
          !boss_battle_network_menu_returning());
    set_inn(6, 6, -1, -1);
    GuestRegs built = inn_regs();
    boss_battle_inn_built_hook(&built);
    put_pointer(fake_inn + 0xa8, fake_steps_vtable);
    CHECK(boss_battle_menu_reopen_pending() && boss_battle_network_menu_returning());
    const GuestRegs reopened = result_frame();
    CHECK(reopened.skip && reopened.rax == BOSS_BATTLE_INN_OPTIONS_ROW && boss_battle_screen_requested() &&
          !boss_battle_menu_reopen_pending());
    fake_options_page(8);
    build_page();
    CHECK(boss_battle_screen_shown() && boss_battle_online_visitor());
    page_act();
    page_update();
    page_result(-1);
    shortcut_now += 16;
    page_result(-1);
    CHECK(shortcut_hint_up());

    /* a page set up on a terminated window is not the return: the Inn is moved on again and reopens it */
    set_page_cursor(4);
    CHECK(network_shortcut_pad_filter(1) == 1);
    shortcut_now += 16;
    page_result(-1);
    network_shortcut_pad_filter(0);
    put_pointer(shortcut_ui + 0x238, NULL);
    fake_inn[INN_ACT] = 0x8e;
    fake_inn[INN_STEP] = 1;
    inn_step_sets = 0;
    menu_scene_frame();
    put_u32(fake_window + 0xa4, 2);
    menu_scene_frame();
    fake_inn[INN_ACT] = 0x5b;
    CHECK(inn_step_sets == 1 && result_frame().rax == BOSS_BATTLE_INN_OPTIONS_ROW);
    fake_options_page(8);
    put_u32(fake_window + 0xa4, 2);
    build_page();
    CHECK(boss_battle_network_menu_returning());
    fake_inn[INN_ACT] = 0x8e;
    fake_inn[INN_STEP] = 1;
    menu_scene_frame();
    CHECK(inn_step_sets == 2 && boss_battle_menu_reopen_pending());
    fake_inn[INN_ACT] = 0x5b;
    menu_scene_frame();
    CHECK(result_frame().rax == BOSS_BATTLE_INN_OPTIONS_ROW);
    fake_options_page(8);
    build_page();
    page_act();
    page_update();
    page_result(-1);
    CHECK(!boss_battle_network_menu_returning() && boss_battle_screen_shown() && !page_hidden() &&
          i32_at(fake_body, 0x958) == 4 && boss_battle_online_selected());
    CHECK(network_menu_pad_hold(0) == 0);
    shortcut_now += 16;
    page_result(-1);
    CHECK(shortcut_hint_up());

    /* a hidden bit the game itself set is not Boss Battle's to clear */
    put_u32(fake_body + 0x964, (uint32_t)(i32_at(fake_body, 0x964) | 0x400));
    expect_native_hidden = 1;
    network_menu_round_trip(3);
    expect_native_hidden = 0;
    put_u32(fake_body + 0x964, 0);

    /* an open Boss Battle room is joinable from the Castle Gate; a fight, or no Boss room, leaves the game's answer */
    boss_battle_online_room_left("test");
    boss_battle_online_visit_end();
    boss_battle_online_set_available(1);
    boss_battle_online_set_hosting(1);
    boss_battle_online_visit_begin(1);
    boss_battle_online_room_joined(1, BOSS_BATTLE_ROOM_OPEN, 1, 0);
    CHECK(boss_battle_online_hosts_room() && boss_battle_online_is_host() && boss_battle_online_boss_room());
    check_join_state(1);
    check_party_seat(1);
    boss_battle_online_note_local(BOSS_BATTLE_LOCAL_FIGHTING, 3, 0);
    check_join_state(0);
    boss_battle_online_note_local(BOSS_BATTLE_LOCAL_IDLE, -1, 0);
    check_join_state(1);
    {
        const BossBattleScreenFunctions card_functions = {fake_set_step,  fake_set_text, fake_set_message,
                                                          fake_row_frame, fake_play_se,  fake_text_changed};
        boss_battle_set_screen_functions(&card_functions);
        check_card_location("BossHost", 0); /* nothing says it is a Boss Battle room */
        boss_battle_online_set_friend_room_lookup(friend_in_boss_room);
        check_card_location("BossHost", 1);
        check_card_location("Someone", 0);
        boss_battle_online_set_friend_room_lookup(NULL);
        check_card_location("BossHost", 0);
    }

    /* the visit ends with the Inn: the game's own room wish is asked once with nothing wanting a room, so the game leaves
     * the Boss Battle room; a later Early Story room is the game's new one */
    const int wishes_before = room_wishes;
    GuestRegs gone;
    memset(&gone, 0, sizeof(gone));
    boss_battle_inn_destroyed_hook(&gone);
    CHECK(!boss_battle_online_selected() && room_wishes == wishes_before + 1);
    CHECK(room_wish_releasing && !room_wish_wanted && !network_access_releasing());
    CHECK(room_wish_scene == (uint64_t)(uintptr_t)game_scene);
    check_join_state(0);
    boss_battle_online_room_left("the game left the room");
    check_party_seat(0);
    boss_battle_inn_destroyed_hook(&gone);
    CHECK(room_wishes == wishes_before + 1);

    /* the page is gone: the shortcut and the hint with it, and the saved setting is as it was */
    regs = page_regs();
    boss_battle_title_hook(&regs);
    CHECK(network_shortcut_owner() == NETWORK_SHORTCUT_NONE && !shortcut_hint_up());
    boss_battle_online_room_left("test");
    CHECK(!boss_battle_online_visitor() && !boss_battle_online_selected());
    check_room_info_builder(0); /* no Boss Battle host here: Early Story's and the campaign's builder is the game's */

    check_native_network_rows(); /* the page is gone: no Network Menu is filtered */
    boss_battle_set_fade_functions(NULL);
    boss_battle_set_network_row_at(NULL);
    boss_battle_set_pause_functions(NULL);
    network_access_set_room_wish(NULL);
    network_hint_configure(NULL);
    network_menu_set_create(NULL);
    put_pointer(fake_inn_context + 0x20, NULL);
    network_access_set_clock(NULL);
    network_access_set_installed(0);
    network_shortcut_reset();
    boss_battle_online_set_hosting(1);
    boss_battle_online_set_available(0);
    boss_battle_online_reset();
    boss_battle_screen_reset();
}

static void host_announces(BossBattlePhase phase, int boss, int fight_all, uint32_t sequence) {
    const BossBattleHostState state = {phase, fight_all, boss, sequence};
    uint8_t bytes[BOSS_BATTLE_STATE_SIZE];
    boss_battle_state_encode(&state, bytes);
    CHECK(boss_battle_online_internal_update(bytes, sizeof(bytes)));
}

/* A guest in the host's Boss Battle room waits in the Inn; only the host's start moves it. It then takes the host's boss,
 * whatever its own page showed, once per start, and sends its own party through the Inn's party step as the host does,
 * before the host's travel takes it to the arena. */
static uint8_t follow_manager[0x200];

static void set_guest_party(int guest_seated) {
    for (int i = 0; i < 4; ++i) put_u32(seat_party + SEAT_BASE + (size_t)i * SEAT_SIZE + SEAT_OWNER, 0xffffffffu);
    put_u32(seat_party + SEAT_BASE + SEAT_OWNER, 1); /* the host, remote */
    if (guest_seated) put_u32(seat_party + SEAT_BASE + SEAT_SIZE + SEAT_OWNER, 0);
}

/* A guest in the host's Boss Battle room waits in the Inn; only the host's start moves it. A start that arrives before the
 * room's game server has seated this guest's own party is kept, not consumed; once seated, the host's boss is followed
 * once, whatever the guest's page showed, and the party it already has is not sent again. Before that, nothing moves. */
static void test_guest_follows_host_start(void) {
    start_guarded();
    use_fake_inn_functions();
    boss_battle_set_lead_selector(NULL);
    memset(seat_party, 0, sizeof(seat_party));
    memset(follow_manager, 0, sizeof(follow_manager));
    put_pointer(follow_manager + 0x80, seat_party);
    put_pointer(fake_inn_context + HYDELAND_CONTEXT_MANAGER, follow_manager);
    set_guest_party(0);
    boss_battle_online_reset();
    boss_battle_online_set_available(1);
    boss_battle_online_set_hosting(0);
    boss_battle_online_visit_begin(1);
    boss_battle_online_room_joined(0, BOSS_BATTLE_ROOM_OPEN, 2, 1);
    CHECK(boss_battle_online_is_guest() && boss_battle_online_slot() == 1);
    set_inn(7, 7, -1, 0);
    result_frame();
    CHECK(!boss_battle_session_active() && party_sends == 0); /* the join alone moves nothing */
    host_announces(BOSS_BATTLE_PHASE_WAITING, -1, 0, 1);
    result_frame();
    CHECK(!boss_battle_session_active() && party_sends == 0);
    /* the host's start reaches a guest not seated yet: kept */
    host_announces(BOSS_BATTLE_PHASE_STARTING, MEDUSA, 0, 2);
    result_frame();
    result_frame();
    CHECK(!boss_battle_session_active() && party_sends == 0);
    /* seated by the room's game server: the kept start runs once, without a second party */
    set_guest_party(1);
    const GuestRegs started = result_frame();
    CHECK(started.skip && boss_battle_session_active() && boss_battle_follower() && party_sends == 0);
    CHECK(boss_battle_online_host_state().boss == MEDUSA);
    result_frame();
    CHECK(party_sends == 0 && boss_battle_follower());
    /* travelling: no empty slot stands in for the guest's seat */
    set_guest_party(0);
    boss_battle_set_party_slot_lookup(fake_seat_lookup);
    int skipped;
    CHECK(seat_answer(0, &skipped) == 4);
    boss_battle_set_party_slot_lookup(NULL);
    boss_battle_follow_released("test");
    boss_battle_reset();
    boss_battle_online_reset();
    memset(fake_inn_context + HYDELAND_CONTEXT_MANAGER, 0, 8);
}

/* ---- the visitor lifecycle: a guest on its own online page joins a host's Boss Battle room through the Network Menu,
 * the join's scene reload rebuilds the Inn, and the guest's page comes back on the new Inn, waiting for the host ---- */

#define PHASE_CHECK(phase, name, cond)                                                            \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            printf("FAIL visitor lifecycle phase %s: %s (line %d)\n", phase, name, __LINE__);    \
            ++failures;                                                                           \
        }                                                                                         \
    } while (0)

typedef struct {
    int party_sends; /* the guest's waiting party, through the native Inn party step */
    int slot;        /* the party slot the native "add player" gave the guest's own player */
} VisitorTranscript;

/* The game's own teardown, as the fixture's game has it: the full party reset empties every seat, and the Matching2 leave
 * is answered by the room-left event Matching2 reports. */
static int party_resets, room_leaves, seats_detached;
/* The characters alive in the scene: the add-player handler makes one for each seat it fills, and only the game's own
 * removal ends one; a seat reset leaves them standing. */
enum { MAX_CHARACTERS = 8 };
typedef struct {
    int alive, slot, owner;
} LiveCharacter;
static LiveCharacter characters[MAX_CHARACTERS];

static void character_created(int slot, int owner) {
    for (int i = 0; i < MAX_CHARACTERS; ++i)
        if (!characters[i].alive) {
            characters[i].alive = 1;
            characters[i].slot = slot;
            characters[i].owner = owner;
            return;
        }
}

static int live_characters(int owner) {
    int count = 0;
    for (int i = 0; i < MAX_CHARACTERS; ++i)
        if (characters[i].alive && (owner < -1 || characters[i].owner == owner)) ++count;
    return count;
}

static uint64_t GUEST_HOOK_ABI fake_seat_character(uint64_t slot) {
    const int index = (int)((slot - (uint64_t)(uintptr_t)(seat_party + SEAT_BASE)) / SEAT_SIZE);
    for (int i = 0; i < MAX_CHARACTERS; ++i)
        if (characters[i].alive && characters[i].slot == index) return (uint64_t)(uintptr_t)&characters[i];
    return 0;
}

static void GUEST_HOOK_ABI fake_remove_character(uint64_t character) {
    CHECK(((uint8_t *)(uintptr_t)character - (uint8_t *)characters) % sizeof(characters[0]) == 0);
    ((LiveCharacter *)(uintptr_t)character)->alive = 0;
}

static void GUEST_HOOK_ABI fake_detach_seat(uint64_t slot, int keep) {
    CHECK(keep == 0 && slot >= (uint64_t)(uintptr_t)(seat_party + SEAT_BASE));
    ++seats_detached;
}
static uint8_t leave_online[0x16920], leave_matching[0x10];
static uint64_t leave_scene_vtable[0x40], leave_matching_vtable[0x40];

static void GUEST_HOOK_ABI fake_party_reset(uint64_t party) {
    CHECK(party == (uint64_t)(uintptr_t)seat_party);
    ++party_resets;
    seat_party[SERVER_CONNECTED_SLOTS] = 0;
    for (int i = 0; i < 4; ++i) {
        put_u32(seat_party + SEAT_BASE + (size_t)i * SEAT_SIZE + SEAT_OWNER, 0xffffffffu);
        put_u32(seat_party + SEAT_BASE + (size_t)i * SEAT_SIZE + SEAT_OWNER - 4, (uint32_t)i);
    }
}

static uint64_t GUEST_HOOK_ABI fake_scene_online(uint64_t scene) {
    CHECK(scene == (uint64_t)(uintptr_t)game_scene);
    return (uint64_t)(uintptr_t)leave_online;
}

static void GUEST_HOOK_ABI fake_matching_leave(uint64_t matching) {
    CHECK(matching == (uint64_t)(uintptr_t)leave_matching);
    ++room_leaves;
}

static void GUEST_HOOK_ABI fake_after_leave(uint64_t online) {
    CHECK(online == (uint64_t)(uintptr_t)leave_online);
    boss_battle_online_room_left("the game left the room");
}

static void use_native_teardown(int on) {
    party_resets = room_leaves = 0;
    leave_scene_vtable[0x170 / 8] = (uint64_t)(uintptr_t)fake_scene_online;
    leave_matching_vtable[0x160 / 8] = (uint64_t)(uintptr_t)fake_matching_leave;
    put_pointer(leave_matching, leave_matching_vtable);
    put_pointer(leave_online + 0x16910, leave_matching);
    put_pointer(game_scene, on ? leave_scene_vtable : NULL);
    memset(characters, 0, sizeof(characters));
    seats_detached = 0;
    const BossBattleSeatTeardown teardown = {fake_party_reset, fake_seat_character, fake_remove_character, fake_detach_seat};
    boss_battle_set_seat_teardown(on ? &teardown : NULL);
    network_access_set_room_leave(on ? fake_after_leave : NULL);
}

static void lifecycle_begin(void) {
    start_guarded();
    use_fake_inn_functions();
    use_fake_screen_functions();
    boss_battle_set_lead_selector(NULL);
    static const int32_t kHelp[6] = {953, 954, 955, 957, 958, 759};
    boss_battle_set_inn_help_table((uint64_t)(uintptr_t)kHelp);
    memset(shortcut_manager, 0, sizeof(shortcut_manager));
    memset(shortcut_ui, 0, sizeof(shortcut_ui));
    memset(shortcut_hint_object, 0, sizeof(shortcut_hint_object));
    memset(shortcut_menu_window, 0, sizeof(shortcut_menu_window));
    put_pointer(fake_inn_context + 0x20, shortcut_manager);
    put_pointer(shortcut_manager + NETWORK_HINT_MANAGER_UI, shortcut_ui);
    memset(fader_owner, 0, sizeof(fader_owner));
    memset(&fades, 0, sizeof(fades));
    put_pointer(shortcut_manager + 0xd8, fader_owner);
    put_pointer(shortcut_manager + HYDELAND_MANAGER_SCENE, game_scene);
    memset(seat_party, 0, sizeof(seat_party));
    for (int i = 0; i < 4; ++i) put_u32(seat_party + SEAT_BASE + (size_t)i * SEAT_SIZE + SEAT_OWNER, 0xffffffffu);
    seat_party[SERVER_CONNECTED_SLOTS] = 0x3; /* the host's slot 0 and the guest's slot 1 joined the game session */
    put_pointer(shortcut_manager + 0x80, seat_party);
    network_access_set_room_wish(fake_room_wish);
    room_wishes = 0;
    const BossBattleFadeFunctions fade_functions = {fake_get_fader, fake_fade};
    boss_battle_set_fade_functions(&fade_functions);
    boss_battle_set_network_row_at(fake_network_row_at);
    const BossBattlePauseFunctions network_setup = {fake_network_setup, 0, 0};
    boss_battle_set_pause_functions(&network_setup);
    shortcut_hint_vtable[NETWORK_HINT_SHOW_SLOT / 8] = (uint64_t)(uintptr_t)shortcut_hint_show;
    const NetworkHintFunctions hint = {shortcut_hint_create, (NetworkHintTextFn)(uintptr_t)shortcut_hint_set_text,
                                       shortcut_hint_hide, shortcut_hint_refresh, 0};
    network_hint_configure(&hint);
    network_hint_set_portuguese(0);
    network_access_set_installed(1);
    network_access_set_clock(shortcut_clock);
    network_shortcut_reset();
    network_menu_set_create(shortcut_menu_create);
    shortcut_menu_refuse = 0;
    boss_battle_online_reset();
    boss_battle_online_set_available(0);
    use_native_teardown(1);
}

static void lifecycle_end(void) {
    use_native_teardown(0);
    GuestRegs title = page_regs();
    boss_battle_title_hook(&title);
    boss_battle_follow_released("lifecycle end");
    boss_battle_online_room_left("lifecycle end");
    boss_battle_set_party_slot_lookup(NULL);
    boss_battle_set_fade_functions(NULL);
    boss_battle_set_network_row_at(NULL);
    boss_battle_set_pause_functions(NULL);
    network_access_set_room_wish(NULL);
    network_hint_configure(NULL);
    network_menu_set_create(NULL);
    put_pointer(fake_inn_context + 0x20, NULL);
    network_access_set_clock(NULL);
    network_access_set_installed(0);
    network_shortcut_reset();
    boss_battle_online_set_hosting(1);
    boss_battle_online_set_available(0);
    boss_battle_online_reset();
    boss_battle_screen_reset();
    boss_battle_reset();
}

/* A new Inn object as the game constructs it: six native rows, its menu builder (the Boss Battle row added), and the
 * Inn's step object; nothing pressed. */
static void construct_inn(void) {
    set_inn(6, 6, -1, -1);
    GuestRegs built = inn_regs();
    boss_battle_inn_built_hook(&built);
    put_pointer(fake_inn + 0xa8, fake_steps_vtable);
    fake_steps_vtable[0xa0 / 8] = (uint64_t)(uintptr_t)fake_step_set;
    fake_inn[INN_ACT] = 0x5b;
    fake_inn[INN_STEP] = 0;
}

static void inn_press(int32_t row) {
    memcpy(inn_list() + 0x960, &row, sizeof(row));
}

static int local_seat_of(uint32_t player) {
    for (int i = 0; i < 4; ++i) {
        const uint8_t *slot = seat_party + SEAT_BASE + (size_t)i * SEAT_SIZE;
        if (i32_at(slot, SEAT_OWNER) == 0 && i32_at(slot, SEAT_OWNER - 4) == (int32_t)player) return i;
    }
    return -1;
}

static int remote_seats(void) {
    int seats = 0;
    for (int i = 0; i < 4; ++i)
        if (i32_at(seat_party + SEAT_BASE + (size_t)i * SEAT_SIZE, SEAT_OWNER) == 1) ++seats;
    return seats;
}

/* The game's add-player: a seat filled, with its own new character. */
static void seat(int slot, int owner, int pad) {
    put_u32(seat_party + SEAT_BASE + (size_t)slot * SEAT_SIZE + SEAT_OWNER, (uint32_t)owner);
    put_u32(seat_party + SEAT_BASE + (size_t)slot * SEAT_SIZE + SEAT_OWNER - 4, (uint32_t)pad);
    if (owner >= 0) character_created(slot, owner);
}

/* The page as the player sees it: shown, not hidden, out of any Network Menu return, its window running its act. */
static int page_active(void) {
    return boss_battle_screen_shown() && !page_hidden() && !boss_battle_network_menu_returning();
}

static int page_waits_for_host(void) {
    return !strncmp(text_at(fake_window, 0xfa0), boss_battle_text(BOSS_BATTLE_TEXT_WAITING_FOR_HOST),
                    strlen(boss_battle_text(BOSS_BATTLE_TEXT_WAITING_FOR_HOST)));
}

/* One frame of a live page: its window's act, its list's update and its result. */
static GuestRegs live_page_frame(int32_t result) {
    fake_inn[INN_ACT] = 0x8e;
    fake_inn[INN_STEP] = 1;
    page_act();
    page_update();
    shortcut_now += 16;
    return page_result(result);
}

/* How the rebuilt Inn comes up after its menu build. The real log: intro acts 0x80 and 0, then Options mode 0x8e step 1,
 * left there, so its menu frame never runs. The other case reaches the menu act. */
typedef enum { NEW_INN_STALE_OPTIONS, NEW_INN_MENU } NewInnEntry;

typedef struct {
    int requested, built, party_before_page;
} InnRun;

/* One game frame of the new Inn: the scene frame, then the Inn's own act. Only the menu act (0x5b) reads its list result,
 * which is where the Inn result hook runs; Options mode step 0 makes the page window, step 2 leaves Options mode. */
static void inn_game_frame(InnRun *run) {
    menu_scene_frame();
    const int act = fake_inn[INN_ACT], step = fake_inn[INN_STEP];
    if (act == 0x8e && step == 2) {
        fake_inn[INN_ACT] = 0x5b;
        fake_inn[INN_STEP] = 0;
    } else if (act == 0x8e && step == 0 && run->requested) {
        fake_options_page(8);
        build_page();
        run->built = 1;
        fake_inn[INN_STEP] = 1;
    } else if (act == 0x8e && run->built) {
        page_act();
        page_update();
        shortcut_now += 16;
        page_result(-1);
    } else if (act == 0x5b) {
        const GuestRegs menu = result_frame();
        if (menu.skip && menu.rax == BOSS_BATTLE_INN_OPTIONS_ROW) {
            ++run->requested;
            if (party_sends) run->party_before_page = 1;
            fake_inn[INN_ACT] = 0x8e;
            fake_inn[INN_STEP] = 0;
        }
    }
}

static void run_new_inn(NewInnEntry entry, InnRun *run) {
    static const uint8_t kStale[][2] = {{0x80, 0}, {0, 0}, {0, 2}, {0, 7}, {0, 8}, {0x8e, 1}};
    static const uint8_t kMenu[][2] = {{0x80, 0}, {0, 0}, {0x5b, 0}};
    const uint8_t(*acts)[2] = entry == NEW_INN_STALE_OPTIONS ? kStale : kMenu;
    const size_t count = entry == NEW_INN_STALE_OPTIONS ? sizeof(kStale) / sizeof(*kStale) : sizeof(kMenu) / sizeof(*kMenu);
    for (size_t i = 0; i < count; ++i) {
        fake_inn[INN_ACT] = acts[i][0];
        fake_inn[INN_STEP] = acts[i][1];
        inn_game_frame(run);
        if (fake_inn[INN_ACT] != acts[i][0] || fake_inn[INN_STEP] != acts[i][1]) break;
    }
    for (int frame = 0; frame < 16 && !page_active(); ++frame) inn_game_frame(run);
}

/* Phases A to L, on the guest, in the order of the runtime log. */
static VisitorTranscript guest_lifecycle(int fight_all, NewInnEntry entry) {
    VisitorTranscript t = {0, -1};
    const int remembered = fight_all ? BOSS_BATTLE_SCREEN_FIGHT_ALL_ROW : 3;

    /* A: the guest's own Boss Battle visit, online, its page active */
    construct_inn();
    inn_press(BOSS_BATTLE_INN_ROW);
    PHASE_CHECK("A", "Inn row opens the page", result_frame().skip && boss_battle_screen_requested());
    boss_battle_online_set_available(1);
    boss_battle_online_set_hosting(1);
    boss_battle_online_visit_begin(1);
    fake_options_page(8);
    build_page();
    live_page_frame(-1);
    boss_battle_online_room_joined(1, BOSS_BATTLE_ROOM_OPEN, 1, 0);
    live_page_frame(-1);
    PHASE_CHECK("A", "own visit online", boss_battle_online_selected() && !boss_battle_online_visitor());
    PHASE_CHECK("A", "page active", page_active());
    PHASE_CHECK("A", "no party sent alone in the room", party_sends == 0);

    /* B: the Network Menu over the page, which is suspended */
    set_page_cursor(remembered);
    live_page_frame(-1);
    PHASE_CHECK("B", "shortcut armed", network_shortcut_armed());
    network_shortcut_pad_filter(1);
    live_page_frame(remembered);
    network_shortcut_pad_filter(0);
    PHASE_CHECK("B", "Network Menu open", network_menu_open_in(shortcut_ui));
    PHASE_CHECK("B", "page suspended", page_hidden() && boss_battle_screen_shown());

    /* C: the guest joins the host's room: visitor, guest role, two members, seat 1, the host a remote member */
    boss_battle_online_room_left("the game left it to join a friend's room");
    boss_battle_online_room_joined(0, BOSS_BATTLE_ROOM_OPEN, 2, 1);
    PHASE_CHECK("C", "visitor 0 -> 1", boss_battle_online_visitor() == 1);
    PHASE_CHECK("C", "room role guest", boss_battle_online_is_guest() && !boss_battle_online_is_host());
    PHASE_CHECK("C", "room members 2", boss_battle_online_members() == 2);
    PHASE_CHECK("C", "member seat 1", boss_battle_online_slot() == 1);
    PHASE_CHECK("C", "no Boss session", !boss_battle_session_active());

    /* D: the Network Menu closes */
    put_pointer(shortcut_ui + 0x238, NULL);
    menu_scene_frame();
    PHASE_CHECK("D", "Network Menu closed", !network_menu_open_in(shortcut_ui));
    PHASE_CHECK("D", "Network Menu return pending", boss_battle_network_menu_returning());

    /* E: the join's scene reload destroys the old Inn, and its page with it, while the return is pending */
    GuestRegs gone;
    memset(&gone, 0, sizeof(gone));
    gone.rdi = (uint64_t)(uintptr_t)fake_inn;
    boss_battle_inn_destroyed_hook(&gone);
    menu_scene_frame();
    menu_scene_frame();
    PHASE_CHECK("E", "visitor kept", boss_battle_online_visitor() && boss_battle_online_selected());
    PHASE_CHECK("E", "old page dropped", !boss_battle_screen_shown() && !boss_battle_network_menu_returning());
    PHASE_CHECK("E", "still in the host's room", boss_battle_online_is_guest() && boss_battle_online_members() == 2);

    /* F, G: the new Inn object, and the host's party seat replicated into slot 0 before the Inn is ready */
    set_inn(6, 6, -1, -1);
    seat(0, 1, 0);
    boss_battle_set_party_slot_lookup(fake_seat_lookup);
    int skipped;
    const uint64_t answered = seat_answer(0, &skipped);
    PHASE_CHECK("G", "remote Host in slot 0", remote_seats() == 1);
    PHASE_CHECK("G", "guest has no local seat", local_seat_of(0) < 0 && skipped && answered == 1 &&
                                                       i32_at(seat_party + SEAT_BASE + SEAT_SIZE, SEAT_OWNER) == -1);

    /* H: the new Inn's menu is built, then the Inn runs its own acts as the runtime log shows them */
    const unsigned wishes = (unsigned)room_wishes;
    GuestRegs built = inn_regs();
    boss_battle_inn_built_hook(&built);
    put_pointer(fake_inn + 0xa8, fake_steps_vtable);
    fake_steps_vtable[0xa0 / 8] = (uint64_t)(uintptr_t)fake_step_set;
    PHASE_CHECK("H", "new Inn ready", main_rows_are_inn());
    PHASE_CHECK("H", "visitor still 1", boss_battle_online_visitor() == 1);
    PHASE_CHECK("H", "Network Menu closed", !network_menu_open_in(shortcut_ui));
    InnRun run = {0, 0, 0};
    run_new_inn(entry, &run);

    /* I: the page request and the page, from what the game runs, with no menu frame the game does not run */
    PHASE_CHECK("I", "visitor Inn rebuild completed but Boss Battle page request was not issued", run.requested >= 1);
    PHASE_CHECK("I", "Boss Battle page requested more than once", run.requested <= 1);
    PHASE_CHECK("I", "no reopen left armed after the request", !boss_battle_menu_reopen_pending());
    PHASE_CHECK("I", "page requested but never constructed", run.built && boss_battle_screen_shown());
    PHASE_CHECK("I", "remembered entry", fight_all ? boss_battle_cursor_fight_all()
                                                   : i32_at(fake_body, 0x958) == remembered && !boss_battle_cursor_fight_all());
    PHASE_CHECK("I", "guest party sent before its page was active", run.party_before_page == 0);

    /* J, K: the page becomes active, waiting for the host; the guest's waiting party goes once */
    live_page_frame(-1);
    PHASE_CHECK("J", "page active", page_active());
    PHASE_CHECK("J", "Waiting for host", page_waits_for_host());
    PHASE_CHECK("J", "visitor role kept", boss_battle_online_visitor() && boss_battle_online_is_guest());
    PHASE_CHECK("K", "guest visitor page active but waiting party was not sent", party_sends >= 1);
    live_page_frame(-1);
    live_page_frame(-1);
    PHASE_CHECK("K", "guest waiting party sent more than once", party_sends == 1);
    PHASE_CHECK("J", "guest cannot start a boss", (live_page_frame(1).r15 & 1) == 0 && !boss_battle_session_active() &&
                                                      se_value == BOSS_BATTLE_SE_CANCEL);

    /* L: the native "add player" seats the guest's own player in slot 1, pad 0 */
    seat(1, 0, 0);
    live_page_frame(-1);
    PHASE_CHECK("L", "local slot valid", local_seat_of(0) == 1 && seat_answer(0, &skipped) == 1);
    PHASE_CHECK("L", "no second party", party_sends == 1);
    PHASE_CHECK("L", "no room created or left", room_wishes == (int)wishes && boss_battle_online_is_guest() &&
                                                     boss_battle_online_members() == 2 && boss_battle_online_visitor());
    boss_battle_set_party_slot_lookup(NULL);
    t.party_sends = party_sends;
    t.slot = local_seat_of(0);
    return t;
}

/* M, N: the host, waiting on its page with the guest a member: 1 of 2 seated refuses the start, the guest's seat
 * replicated from its party send makes 2 of 2, and Harpy then starts. The state the host publishes is returned. */
static int host_lifecycle(const VisitorTranscript *guest, uint8_t published[BOSS_BATTLE_STATE_SIZE], int fight_all) {
    construct_inn();
    inn_press(BOSS_BATTLE_INN_ROW);
    result_frame();
    boss_battle_online_set_available(1);
    boss_battle_online_set_hosting(1);
    boss_battle_online_visit_begin(1);
    fake_options_page(8);
    build_page();
    boss_battle_online_room_joined(1, BOSS_BATTLE_ROOM_OPEN, 1, 0);
    live_page_frame(-1);
    boss_battle_online_members_changed(2);
    live_page_frame(-1);
    PHASE_CHECK("M", "host party sent once members > 1", party_sends == 1);
    seat(0, 0, 0);
    PHASE_CHECK("M", "room has 2 members", boss_battle_online_is_host() && boss_battle_online_members() == 2);
    const int32_t start_row = fight_all ? BOSS_BATTLE_SCREEN_FIGHT_ALL_ROW : 1;
    const uint32_t sequence_before = boss_battle_online_host_state().sequence;
    se_value = -1;
    GuestRegs refused = live_page_frame(start_row);
    PHASE_CHECK("M", "ready 1 of 2 before the guest's seat", 1 + remote_seats() == 1);
    PHASE_CHECK("M", "start refused at 1 of 2", !(refused.r15 & 1) && se_value == BOSS_BATTLE_SE_CANCEL &&
                                                     boss_battle_online_host_state().phase == BOSS_BATTLE_PHASE_WAITING);
    if (guest->party_sends == 1 && guest->slot >= 0) seat(guest->slot, 1, 0);
    PHASE_CHECK("M", "guest native seat was not replicated to Host readiness",
                remote_seats() == 1 && 1 + remote_seats() == boss_battle_online_members());
    se_value = -1;
    const GuestRegs started = live_page_frame(start_row);
    const BossBattleHostState host = boss_battle_online_host_state();
    PHASE_CHECK("N", "start allowed", (started.r15 & 1) && se_value == BOSS_BATTLE_SE_DECIDE);
    PHASE_CHECK("N", "host start is Harpy", host.phase == BOSS_BATTLE_PHASE_STARTING && host.boss == HARPY &&
                                                host.fight_all == fight_all && host.sequence == sequence_before + 1);
    return boss_battle_online_publish(published);
}

/* O: the guest consumes the host's first start on its first sight of it. */
static void guest_follows(const uint8_t *published, int boss, int fight_all) {
    PHASE_CHECK("O", "host state received", boss_battle_online_internal_update(published, BOSS_BATTLE_STATE_SIZE));
    const GuestRegs page = live_page_frame(-1);
    PHASE_CHECK("O", "page closes for the host's start", (page.r15 & 1) != 0);
    fake_inn[INN_ACT] = 0x5b;
    fake_inn[INN_STEP] = 0;
    result_frame();
    PHASE_CHECK("O", "host start followed", boss_battle_session_active() && boss_battle_follower());
    PHASE_CHECK("O", "the host's boss", boss_battle_online_host_state().boss == boss &&
                                            boss_battle_online_host_state().fight_all == fight_all);
    PHASE_CHECK("O", "no second party for the start", party_sends == 1);
}

static void test_visitor_lifecycle(void) {
    for (int entry = NEW_INN_STALE_OPTIONS; entry <= NEW_INN_MENU; ++entry)
        for (int fight_all = 0; fight_all <= 1; ++fight_all) {
            lifecycle_begin();
            const VisitorTranscript guest = guest_lifecycle(fight_all, (NewInnEntry)entry);
            lifecycle_end();

            uint8_t published[BOSS_BATTLE_STATE_SIZE] = {0};
            lifecycle_begin();
            PHASE_CHECK("N", "host state published", host_lifecycle(&guest, published, fight_all));
            lifecycle_end();
            boss_battle_online_note_local(BOSS_BATTLE_LOCAL_TRAVELLING, 3, 0);
            PHASE_CHECK("N", "no start latch survives the visit", !boss_battle_online_take_start_peer_lost());
            boss_battle_online_note_local(BOSS_BATTLE_LOCAL_IDLE, -1, 0);

            lifecycle_begin();
            guest_lifecycle(fight_all, (NewInnEntry)entry);
            guest_follows(published, HARPY, fight_all);
            if (!fight_all) {
                /* a later start of the same room is followed too */
                boss_battle_follow_released("the first fight ended");
                boss_battle_reset();
                const uint32_t first = (uint32_t)published[4] | (uint32_t)published[5] << 8 |
                                       (uint32_t)published[6] << 16 | (uint32_t)published[7] << 24;
                host_announces(BOSS_BATTLE_PHASE_WAITING, -1, 0, first + 1);
                host_announces(BOSS_BATTLE_PHASE_STARTING, MEDUSA, 0, first + 2);
                result_frame();
                PHASE_CHECK("O", "second start followed", boss_battle_session_active() && boss_battle_follower() &&
                                                              boss_battle_online_host_state().boss == MEDUSA);
            }
            lifecycle_end();
        }
}

/* ---- the handoff: the host ends its Boss Battle visit with Return to tavern, the room stays for normal online play,
 * and the guest leaves the Inn with the host for Hydeland ---- */

static int seats_with_owner(int owner) {
    int seats = 0;
    for (int i = 0; i < 4; ++i)
        if (i32_at(seat_party + SEAT_BASE + (size_t)i * SEAT_SIZE, SEAT_OWNER) == owner) ++seats;
    return seats;
}

static int local_pad_seats(int pad) {
    int seats = 0;
    for (int i = 0; i < 4; ++i) {
        const uint8_t *slot = seat_party + SEAT_BASE + (size_t)i * SEAT_SIZE;
        if (i32_at(slot, SEAT_OWNER) == 0 && i32_at(slot, SEAT_OWNER - 4) == pad) ++seats;
    }
    return seats;
}

static void end_host_session(void) {
    const BossBattleHostState last = boss_battle_online_host_state();
    host_announces(BOSS_BATTLE_PHASE_ENDED, -1, 0, last.sequence + 1);
}

/* After the Boss Battle visit: no page, no pending reopen or start, its room left, its seats reset by the game. */
static void check_after_visit(const char *phase, int sends) {
    PHASE_CHECK(phase, "Boss Battle page still active after the visit ended", !boss_battle_screen_shown());
    PHASE_CHECK(phase, "Boss Battle page still requested after the visit ended", !boss_battle_screen_requested());
    PHASE_CHECK(phase, "Boss Battle reopen still pending after the visit ended", !boss_battle_menu_reopen_pending());
    PHASE_CHECK(phase, "Boss Battle page return still running", !boss_battle_network_menu_returning());
    PHASE_CHECK(phase, "Boss session still active", !boss_battle_session_active() && !boss_battle_follower());
    int boss = -1, fight_all = 0;
    PHASE_CHECK(phase, "a Boss start is still pending", !boss_battle_online_take_follow(&boss, &fight_all));
    PHASE_CHECK(phase, "visitor role still set", !boss_battle_online_visitor());
    PHASE_CHECK(phase, "Boss Battle visit still online", !boss_battle_online_selected());
    PHASE_CHECK(phase, "room was left at Return to tavern: the players are no longer together",
                boss_battle_online_in_room() && boss_battle_online_members() == 2 && room_leaves == 0);
    PHASE_CHECK(phase, "room still tagged as a Boss Battle room", !boss_battle_online_boss_room());
    PHASE_CHECK(phase, "Boss seats survive the teardown",
                party_resets == 1 && seats_with_owner(0) == 0 && seats_with_owner(1) == 0);
    PHASE_CHECK(phase, "stale Boss characters survive the seat reset (they would stand in Hydeland as NPCs)",
                live_characters(-2) == 0 && seats_detached == 2);
    PHASE_CHECK(phase, "more than one party sent after the visit ended", party_sends <= sends + 1);
    PHASE_CHECK(phase, "a later room would carry the Boss Battle session",
                boss_battle_online_room_attr() == BOSS_BATTLE_ROOM_NONE && !boss_battle_online_search_wanted());
}

/* Hydeland's own placement after the visit: the game seats this machine's player and the other one in the first free
 * seats, as the runtime showed (member 0x8 at the first free slot). */
static int stage_add(int owner) {
    for (int i = 0; i < 4; ++i)
        if (i32_at(seat_party + SEAT_BASE + (size_t)i * SEAT_SIZE, SEAT_OWNER) == -1) {
            seat(i, owner, owner == 0 ? 0 : 255);
            return i;
        }
    return -1;
}

/* The scene's online party pass for local player 0, through the production lookup hook: the slot it reads must be a seat,
 * either empty (skipped) or player 0's own; no seat at all is the null slot 0x271a0 faults on. */
static void check_local_player_update(const char *phase) {
    boss_battle_set_party_slot_lookup(fake_seat_lookup);
    int skipped;
    const uint64_t slot = seat_answer(0, &skipped);
    boss_battle_set_party_slot_lookup(NULL);
    const uint8_t *seat = slot < 4 ? seat_party + SEAT_BASE + slot * SEAT_SIZE : NULL;
    PHASE_CHECK(phase, "Guest Early Story handoff reached local-player update before a native local seat existed",
                seat && (i32_at(seat, SEAT_OWNER) == -1 || (i32_at(seat, SEAT_OWNER) == 0 && i32_at(seat, SEAT_OWNER - 4) == 0)));
}

/* Hydeland seats the players one add at a time: the host's own player first on the host, the other player first on the
 * guest, and the scene's party pass runs between the adds. */
static void check_hydeland(const char *phase, int host_machine) {
    check_local_player_update(phase);
    stage_add(host_machine ? 0 : 1);
    check_local_player_update(phase);
    stage_add(host_machine ? 1 : 0);
    check_local_player_update(phase);
    PHASE_CHECK(phase, "Hydeland add duplicated a player", seats_with_owner(0) == 1 && seats_with_owner(1) == 1);
    PHASE_CHECK(phase, "player 0 drives more than one character", local_pad_seats(0) == 1);
    PHASE_CHECK(phase, "Hydeland seated a third player", i32_at(seat_party + SEAT_BASE + 2 * SEAT_SIZE, SEAT_OWNER) == -1);
    PHASE_CHECK(phase, "not exactly two live player characters in Hydeland",
                live_characters(-2) == 2 && live_characters(0) == 1 && live_characters(1) == 1);
}

/* The page the guest had, frames later in Hydeland: not the screen any more, so its input is the game's. */
static void check_no_boss_input(const char *phase) {
    const int sends = party_sends;
    const GuestRegs result = page_result(3);
    PHASE_CHECK(phase, "Boss Battle page input after the session ended", !result.skip && result.r15 == (uint64_t)(uintptr_t)fake_body);
    PHASE_CHECK(phase, "Boss Battle list input blocked after the session ended", !page_update().skip);
    PHASE_CHECK(phase, "a party was sent from a stale page", party_sends == sends);
}

static void guest_leaves_with_host(void) {
    GuestRegs gone;
    memset(&gone, 0, sizeof(gone));
    gone.rdi = (uint64_t)(uintptr_t)fake_inn;
    boss_battle_inn_destroyed_hook(&gone);
    menu_scene_frame();
}

/* The host's page, live in a Boss Battle room the guest joined, both seated, as it is after a fight's return. */
static void host_page_with_guest(void) {
    construct_inn();
    boss_battle_online_set_available(1);
    boss_battle_online_set_hosting(1);
    boss_battle_online_visit_begin(1);
    boss_battle_online_room_joined(1, BOSS_BATTLE_ROOM_OPEN, 2, 0);
    seat(0, 0, 0);
    seat(1, 1, 255);
    boss_battle_arm_menu_reopen(BOSS_BATTLE_REOPEN_CAMPAIGN_RESTORE);
    inn_press(-1);
    PHASE_CHECK("R", "Boss page did not reopen after the fight", result_frame().rax == BOSS_BATTLE_INN_OPTIONS_ROW);
    fake_options_page(8);
    build_page();
    live_page_frame(-1);
    PHASE_CHECK("R", "host page not active with the guest seated", page_active() && seats_with_owner(1) == 1);
}

/* Matching2's own tag update on the owner's room, which follows the visit's room attribute. */
static void publish_room_tag(void) {
    boss_battle_online_room_tagged(boss_battle_online_room_attr());
}

static void host_return_to_tavern(void) {
    const GuestRegs back = live_page_frame(BOSS_BATTLE_SCREEN_RETURN_ROW);
    PHASE_CHECK("T", "Return to tavern did not close the host's page", (back.r15 & 1) != 0);
    fake_inn[INN_ACT] = 0x5b;
    fake_inn[INN_STEP] = 0;
    inn_press(-1);
    result_frame();
    publish_room_tag();
}

/* The player chooses Boss Battle on the Inn menu again: a press after frames with none. */
static void host_enters_boss_battle(void) {
    inn_press(-1);
    result_frame();
    inn_press(BOSS_BATTLE_INN_ROW);
    PHASE_CHECK("E", "Boss Battle row did not open the page", result_frame().rax == BOSS_BATTLE_INN_OPTIONS_ROW);
    boss_battle_online_native_changed(1);
    fake_options_page(8);
    build_page();
    live_page_frame(-1);
}

static void test_leave_and_reenter(void) {
    lifecycle_begin();
    host_page_with_guest();
    const int sends = party_sends;
    host_return_to_tavern();
    check_after_visit("T", sends);
    PHASE_CHECK("T", "Boss page reopened without a new press",
                result_frame().rax != BOSS_BATTLE_INN_OPTIONS_ROW && !boss_battle_screen_requested());

    /* a fresh visit in the same room: host role again, the room tagged again, a new party and start sequence */
    const int wishes = room_wishes;
    const uint32_t sequence = boss_battle_online_host_state().sequence;
    host_enters_boss_battle();
    publish_room_tag();
    live_page_frame(-1);
    PHASE_CHECK("E", "fresh visit not online", boss_battle_online_selected() && !boss_battle_online_visitor());
    PHASE_CHECK("E", "Host reentry asked for a new room instead of taking back its own", room_wishes == wishes);
    PHASE_CHECK("E", "Host reentry is not recognized as Host in its own room",
                boss_battle_online_is_host() && boss_battle_online_room_attr() == BOSS_BATTLE_ROOM_OPEN &&
                    boss_battle_online_boss_room() && boss_battle_online_members() == 2 && room_leaves == 0);
    live_page_frame(-1);
    PHASE_CHECK("E", "fresh host party not sent with the guest still there", party_sends == sends + 1);
    seat(0, 0, 0);
    se_value = -1;
    live_page_frame(1);
    PHASE_CHECK("E", "host started at 1 of 2", boss_battle_online_host_state().phase == BOSS_BATTLE_PHASE_WAITING);
    seat(1, 1, 255);
    se_value = -1;
    const GuestRegs started = live_page_frame(1);
    const BossBattleHostState host = boss_battle_online_host_state();
    PHASE_CHECK("E", "only the host starts a fight, on the host", boss_battle_online_may_start());
    PHASE_CHECK("E", "Harpy refused after reentry", (started.r15 & 1) && se_value == BOSS_BATTLE_SE_DECIDE &&
                                                        host.phase == BOSS_BATTLE_PHASE_STARTING && host.boss == HARPY);
    PHASE_CHECK("E", "start sequence not fresh", host.sequence == sequence + 1);
    lifecycle_end();

    /* a host alone in its Boss Battle room leaves it with the visit: nobody is kept waiting in it */
    lifecycle_begin();
    host_page_with_guest();
    boss_battle_online_members_changed(1);
    host_return_to_tavern();
    PHASE_CHECK("S", "a room nobody else is in was kept", !boss_battle_online_in_room() && room_leaves == 1);
    lifecycle_end();

    /* the guest stays in the host's room after the visit, and becomes its visitor again on the host's next visit */
    lifecycle_begin();
    guest_lifecycle(0, NEW_INN_MENU);
    end_host_session();
    live_page_frame(-1);
    put_u32(fake_window + 0xa4, 2);
    menu_scene_frame();
    fake_inn[INN_ACT] = 0x5b;
    fake_inn[INN_STEP] = 0;
    inn_press(-1);
    result_frame();
    const int guest_sends = party_sends;
    check_after_visit("G", guest_sends);
    host_announces(BOSS_BATTLE_PHASE_WAITING, -1, 0, boss_battle_online_host_state().sequence + 1);
    PHASE_CHECK("G", "guest did not become the host's visitor again", boss_battle_online_visitor() && boss_battle_online_boss_room());
    const GuestRegs menu = result_frame();
    PHASE_CHECK("G", "guest Boss page not requested on the host's next visit",
                menu.rax == BOSS_BATTLE_INN_OPTIONS_ROW && boss_battle_screen_requested());
    fake_options_page(8);
    build_page();
    seat(0, 1, 255);
    live_page_frame(-1);
    PHASE_CHECK("G", "guest page not waiting for host", page_active() && page_waits_for_host());
    PHASE_CHECK("G", "guest waiting party not sent again", party_sends == guest_sends + 1);
    lifecycle_end();

    /* the host's Hydeland after Return to tavern: one local host, one remote guest */
    lifecycle_begin();
    host_page_with_guest();
    host_return_to_tavern();
    guest_leaves_with_host();
    check_after_visit("Y", party_sends);
    check_hydeland("Y", 1);
    lifecycle_end();
}

/* The visit's teardown sites on every supported build with a local image: the full party reset and the room leave. */
static void test_handoff_sites(void) {
    static const struct {
        const char *folder;
        uint64_t room_wish;
        BossBattleHandoffSites sites;
    } kSites[] = {
        {"v1.03", 0x43bc60, {0x27d20, 0x26f90, 0x2f5620, 0x26fd0, 0x9d9e0}},
        {"CUSA10454-1.01", 0x43bc60, {0x27d20, 0x26f90, 0x2f5620, 0x26fd0, 0x9d9e0}},
        {"CUSA07001-1.00", 0x43ae90, {0x27670, 0x268e0, 0x2f4b30, 0x26920, 0x9d060}},
        {"CUSA07001-1.02", 0x43b270, {0x276f0, 0x26960, 0x2f4f00, 0x269a0, 0x9d300}},
        {"CUSA07001-1.03", 0x43b330, {0x276f0, 0x26960, 0x2f4f50, 0x269a0, 0x9d300}},
        {"CUSA10209-1.00", 0x43b000, {0x27730, 0x269a0, 0x2f4cc0, 0x269e0, 0x9d1f0}},
    };
    for (size_t b = 0; b < sizeof(kSites) / sizeof(*kSites); ++b) {
        uint64_t size = 0;
        BossBattleHandoffSites found;
        unsigned char *image = boot_image(kSites[b].folder, &size, 0);
        if (!image) {
            printf("%s: no boot image, handoff sites skipped\n", kSites[b].folder);
            continue;
        }
        PHASE_CHECK(kSites[b].folder, "handoff sites not found",
                    boss_battle_locate_handoff(image, size, kSites[b].room_wish, &found));
        PHASE_CHECK(kSites[b].folder, "party reset, seat teardown or room leave at the wrong place",
                    !memcmp(&found, &kSites[b].sites, sizeof(found)));
        image[kSites[b].sites.party_reset + 9] ^= 0xff;
        PHASE_CHECK(kSites[b].folder, "a changed party reset was accepted",
                    !boss_battle_locate_handoff(image, size, kSites[b].room_wish, &found));
        image[kSites[b].sites.party_reset + 9] ^= 0xff;
        image[kSites[b].sites.party_reset + 0x1f] ^= 0xff;
        PHASE_CHECK(kSites[b].folder, "a party reset clearing another connected-slot field was accepted",
                    !boss_battle_locate_handoff(image, size, kSites[b].room_wish, &found));
        image[kSites[b].sites.party_reset + 0x1f] ^= 0xff;
        PHASE_CHECK(kSites[b].folder, "a room leave outside the room wish was accepted",
                    !boss_battle_locate_handoff(image, size, kSites[b].sites.party_reset, &found));
        printf("%s: handoff sites party reset %#llx, seat character %#llx, removal %#llx, detach %#llx, room leave follow-up "
               "%#llx\n",
               kSites[b].folder, (unsigned long long)found.party_reset, (unsigned long long)found.seat_character,
               (unsigned long long)found.remove_character, (unsigned long long)found.detach_seat,
               (unsigned long long)found.after_leave);
        free(image);
    }
}

static void test_visitor_handoff(void) {
    test_handoff_sites();
    for (int inn_first = 0; inn_first <= 1; ++inn_first) {
        lifecycle_begin();
        guest_lifecycle(0, NEW_INN_MENU);
        const int sends = party_sends;
        PHASE_CHECK("1", "guest not seated 2 of 2 before the handoff", seats_with_owner(0) == 1 && seats_with_owner(1) == 1);

        /* the host's Return to tavern ends its visit; the guest hears the end */
        end_host_session();
        PHASE_CHECK("2", "visitor role kept after the host ended the session", !boss_battle_online_visitor());

        uint64_t page_result_closed = 0;
        if (inn_first) {
            /* the guest follows the host out before its page ran a frame */
            guest_leaves_with_host();
            const GuestRegs late = page_result(-1);
            page_result_closed = late.r15;
            PHASE_CHECK("3", "Boss Battle page outside the Inn not closed", (late.r15 & 1) != 0);
        } else {
            const GuestRegs page = live_page_frame(-1);
            page_result_closed = page.r15;
            PHASE_CHECK("3", "guest Boss Battle page not closed when the host ended the session", (page.r15 & 1) != 0);
            PHASE_CHECK("3", "a party was sent while the page closed", party_sends == sends);
        }
        const int closed = (page_result_closed & 1) != 0;
        if (closed) put_u32(fake_window + 0xa4, 2); /* the native close terminates the page's window */
        menu_scene_frame();
        if (!inn_first && closed) {
            fake_inn[INN_ACT] = 0x5b;
            fake_inn[INN_STEP] = 0;
            const GuestRegs menu = result_frame();
            PHASE_CHECK("4", "the Inn reopened the Boss Battle page", !(menu.skip && menu.rax == BOSS_BATTLE_INN_OPTIONS_ROW));
            check_after_visit("4", sends);
        }
        if (!inn_first) guest_leaves_with_host();
        check_after_visit("5", sends);
        check_no_boss_input("5");
        for (int frame = 0; frame < 4; ++frame) menu_scene_frame();
        check_after_visit("6", sends);
        check_hydeland("7", 0);
        lifecycle_end();
    }
    test_leave_and_reenter();
}

#ifdef BOSS_BATTLE_EARLY_STORY_HANDOFF
/* ---- the Boss Battle room handed to Early Story: the real Early Story session and room-wish code on the shared room ---- */

enum { STORY_STEP = 10, HOST_STORY_STEP = 20, TOWN_LOCATION = 3 };

/* Early Story Online as it is on, in town, before the online check: its own setting, access and the lead's step. */
static void early_story_on(void) {
    early_story_set_active(1);
    early_story_set_hosting(1);
    early_story_grant_access();
    early_story_note_own_step(STORY_STEP);
    early_story_session_reset();
    boss_battle_online_set_room_handoff(early_story_session_room_handoff);
}

static void early_story_off(void) {
    boss_battle_online_set_room_handoff(NULL);
    early_story_set_active(0);
    early_story_set_hosting(0);
    early_story_note_own_step(EARLY_STORY_NO_STEP);
    early_story_note_host_step(EARLY_STORY_NO_STEP);
}

/* Matching2's join rule for the room the visit is in (runtime_matching2.cpp, room joined). */
static void matching2_joined_boss_room(int owner) {
    early_story_session_room_handoff(owner, 1);
}

/* The game's room wish at a town location: 1 when Early Story's hook keeps the room (the Castle Gate answer). */
static int town_keeps_room(void) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rax = TOWN_LOCATION;
    early_story_room_wish_hook(&regs);
    return regs.rax == EARLY_STORY_ROOM_WISH_GATE;
}

enum { INN_LEAVE_DISPLAY_ROW = 6, INN_NATIVE_LEAVE = 5 };

static int host_leaves_inn(void) {
    fake_inn[INN_ACT] = 0x5b;
    fake_inn[INN_STEP] = 0;
    inn_press(INN_LEAVE_DISPLAY_ROW);
    const GuestRegs menu = result_frame();
    inn_press(-1);
    return menu.skip && (int32_t)menu.rax == INN_NATIVE_LEAVE;
}

enum { HOST_MEMBER_SLOT = 0, GUEST_MEMBER_SLOT = 1, SLOT_CHARACTER_ID = 0x1f6c };
enum { HOST_CHARACTER_ID = 0x4801, GUEST_CHARACTER_ID = 0x7302, PLAYER_PAD = 0 };

/* The host's game server seating a CREATEPLAYER_REQUEST, which every machine then seats as its add-player says: the
 * production hook at its read of the requested slot, then the game's own choice, the requested seat when it is free,
 * else the first free one. Every request asks for its sender's pad, 0 on both machines. */
static int host_seats_request(int sender_seat, int local) {
    uint8_t request[0x40] = {0};
    request[0x1b] = (uint8_t)sender_seat;
    put_i32_at(request + 0x28, PLAYER_PAD);
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.r13 = (uint64_t)(uintptr_t)request;
    boss_battle_slot_request_hook(&regs);
    int slot = i32_at(request, 0x28);
    if (slot < 0 || slot >= 4 || i32_at(seat_party + SEAT_BASE + (size_t)slot * SEAT_SIZE, SEAT_OWNER) != -1)
        for (slot = 0; slot < 4 && i32_at(seat_party + SEAT_BASE + (size_t)slot * SEAT_SIZE, SEAT_OWNER) != -1; ++slot) {
        }
    if (slot >= 4) return -1;
    seat(slot, local ? 0 : 1, local ? 0 : 255);
    put_u32(seat_party + SEAT_BASE + (size_t)slot * SEAT_SIZE + SLOT_CHARACTER_ID,
            sender_seat == HOST_MEMBER_SLOT ? HOST_CHARACTER_ID : GUEST_CHARACTER_ID);
    return slot;
}

/* This machine's campaign: its own character in its save's records, the rest of its progression as the fixture fills it. */
enum { PLAY_DATA_RECORDS = 0xa480, RECORD_SIZE = 0x21d0, RECORDS = 0x4a };
static uint8_t *progression_before;

static void use_machine_save(int host_machine) {
    fake_campaign();
    put_u32(fake_play_data + PLAY_DATA_RECORDS + SLOT_CHARACTER_ID, host_machine ? HOST_CHARACTER_ID : GUEST_CHARACTER_ID);
    if (!progression_before) progression_before = malloc(BOSS_BATTLE_PLAY_DATA_BYTES);
    memcpy(progression_before, fake_play_data, BOSS_BATTLE_PLAY_DATA_BYTES);
}

/* The game's lookup of a party slot's character in this machine's save (0x893c0): its record index, -1 when not there. */
static int save_record_of_slot(int slot) {
    const uint32_t id = (uint32_t)i32_at(seat_party + SEAT_BASE + (size_t)slot * SEAT_SIZE, SLOT_CHARACTER_ID);
    for (int i = 0; i < RECORDS; ++i)
        if ((uint32_t)i32_at(fake_play_data + PLAY_DATA_RECORDS + (size_t)i * RECORD_SIZE, SLOT_CHARACTER_ID) == id) return i;
    return -1;
}

/* Town doors request their scene with no operating slot, so the game's town menus (Morgan's item and repair windows
 * among them) operate party slot 0 and look its character up in this machine's save. */
static void check_town_operator(const char *phase, int host_machine) {
    if (host_machine) {
        PHASE_CHECK(phase, "Host town interaction resolves party slot 0 to a character missing from the Host's save "
                           "(the item window's table is null: 0x8b87b)",
                    save_record_of_slot(HOST_MEMBER_SLOT) == 0);
        PHASE_CHECK(phase, "Morgan's repair would target the Guest's character on the Host",
                    i32_at(seat_party + SEAT_BASE + HOST_MEMBER_SLOT * SEAT_SIZE, SLOT_CHARACTER_ID) == HOST_CHARACTER_ID);
    } else {
        PHASE_CHECK(phase, "the Guest's own character is not in its member slot or not in its save",
                    i32_at(seat_party + SEAT_BASE + GUEST_MEMBER_SLOT * SEAT_SIZE, SEAT_OWNER) == 0 &&
                        save_record_of_slot(GUEST_MEMBER_SLOT) == 0);
    }
    PHASE_CHECK(phase, "party slot 0 is not the Host's character, slot 1 not the Guest's",
                i32_at(seat_party + SEAT_BASE + HOST_MEMBER_SLOT * SEAT_SIZE, SLOT_CHARACTER_ID) == HOST_CHARACTER_ID &&
                    i32_at(seat_party + SEAT_BASE + GUEST_MEMBER_SLOT * SEAT_SIZE, SLOT_CHARACTER_ID) == GUEST_CHARACTER_ID);
    PHASE_CHECK(phase, "the handoff changed this machine's campaign progression (its repair tutorial among it)",
                !memcmp(progression_before, fake_play_data, BOSS_BATTLE_PLAY_DATA_BYTES));
}

/* The Early Story step the game's lead selector gives this machine's lead record, over a save of its own. */
static uint8_t story_save[0xa8 + EARLY_STORY_RECORD_SIZE];

static void story_save_begin(int own_step) {
    for (size_t i = 0; i < sizeof(story_save); ++i) story_save[i] = (uint8_t)(i * 7 + 3);
    story_save[0xa8 + EARLY_STORY_RECORD_STEP] = (uint8_t)own_step;
}

static int story_step_seen_by_game(void) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rax = (uint64_t)(uintptr_t)story_save;
    early_story_selector_hook(&regs);
    return ((const uint8_t *)(uintptr_t)(regs.skip ? regs.rax : (uint64_t)(uintptr_t)(story_save + 0xa8)))[EARLY_STORY_RECORD_STEP];
}

static int story_step_saved(void) {
    return story_save[0xa8 + EARLY_STORY_RECORD_STEP];
}

/* The handed-back room, on the guest machine: the host's story session reaches a guest whose story is still in the opening
 * with the owner's live step, and the game follows it while the guest's own save keeps its own step. */
static void check_guest_follows_story(const char *phase, int own_step, int host_step, int host_repairs_to) {
    story_save_begin(own_step);
    early_story_note_own_step(own_step);
    early_story_session_room_handoff(0, 0);
    uint8_t before[sizeof(story_save)];
    memcpy(before, story_save, sizeof(before));
    story_step_seen_by_game();
    const int opening = own_step < EARLY_STORY_ORIGINAL_STEP;
    uint8_t state[EARLY_STORY_STATE_SIZE];
    early_story_state_encode(host_step, state);
    const int joined = early_story_session_internal_update(early_story_state_decode(state, sizeof(state)));
    PHASE_CHECK(phase, opening ? "the host's story session did not reach a guest still in the opening"
                               : "a guest past the opening was put into the host's story session",
                joined == opening);
    PHASE_CHECK(phase, "the guest game does not see the host's step", story_step_seen_by_game() == (opening ? host_step : own_step));
    early_story_state_encode(host_repairs_to, state);
    early_story_session_internal_update(early_story_state_decode(state, sizeof(state)));
    PHASE_CHECK(phase, opening ? "the guest stays on the objective the host has repaired past"
                               : "a guest past the opening was moved to the host's step",
                story_step_seen_by_game() == (opening ? host_repairs_to : own_step));
    PHASE_CHECK(phase, "the guest's own save was written", !memcmp(before, story_save, sizeof(before)));
    early_story_session_reset();
    PHASE_CHECK(phase, "the guest does not get its own story back", story_step_seen_by_game() == own_step);
    PHASE_CHECK(phase, "the guest's own step was not restored", story_step_saved() == own_step);
}

/* The host machine: its handed-back room becomes the story session a host of its step would have created. */
static void check_host_resumes_story(const char *phase, int own_step, int resumes) {
    early_story_note_own_step(own_step);
    PHASE_CHECK(phase, resumes ? "the host's handed-back room did not resume as a story session"
                               : "a host past the opening resumed the opening's story session",
                early_story_session_resume_story_room(own_step) == resumes);
    PHASE_CHECK(phase, "the resumed room is not published with the host's step",
                !resumes || early_story_room_attr_value(own_step) == own_step + 1);
    PHASE_CHECK(phase, "the room resumed twice", !early_story_session_resume_story_room(own_step));
}

/* The host's Inn after the handoff: it does not seat itself again, and its way out waits until the guest's party, sent by
 * the guest and seated by the host's game server, is seated with it. */
static void host_waits_for_guest_seat(const char *phase, int seat_first) {
    const int sends = party_sends;
    fake_inn[INN_ACT] = 0x5b;
    fake_inn[INN_STEP] = 0;
    inn_press(-1);
    result_frame();
    PHASE_CHECK(phase, "the host seated itself in the Inn (the town would seat it twice)", party_sends == sends);
    if (!seat_first)
        PHASE_CHECK(phase, "Early Story handoff attempted travel with 1 of 2 native party participants", !host_leaves_inn());
    host_seats_request(GUEST_MEMBER_SLOT, 0);
    result_frame();
    PHASE_CHECK(phase, "host travel held with every member seated", host_leaves_inn());
    PHASE_CHECK(phase, "the host's party is not the guest alone before travel", seats_with_owner(1) == 1 && seats_with_owner(0) == 0);
}

/* The host's game server answers a SETUPSCENE_START_REQUEST (0x5dd) with the start command (0x7dd) only for a slot in its
 * connected slots, and erases any other request: that slot's scene setup then never starts. */
static int host_answers_start_request(int slot) {
    return (seat_party[SERVER_CONNECTED_SLOTS] >> slot) & 1;
}

static void check_travel_start(const char *phase) {
    PHASE_CHECK(phase, "the host's game server would drop the guest's travel start request (guest slot left the session)",
                host_answers_start_request(0) && host_answers_start_request(1));
}

/* The game's travel out of the Inn seats the host's own character on every machine and carries the guest's seat. */
static void check_town_arrival(const char *phase, int host_machine) {
    check_local_player_update(phase);
    host_seats_request(HOST_MEMBER_SLOT, host_machine);
    check_local_player_update(phase);
    static const char *const kTown[] = {"Hydeland", "Adventurers Guild", "Town", "Morgan's Magic Item Shop", "Town"};
    for (size_t i = 0; i < sizeof(kTown) / sizeof(*kTown); ++i) {
        menu_scene_frame();
        check_local_player_update(kTown[i]);
        check_town_operator(kTown[i], host_machine);
        check_travel_start(kTown[i]);
    }
    PHASE_CHECK(phase, "Hydeland add duplicated a player", seats_with_owner(0) == 1 && seats_with_owner(1) == 1);
    PHASE_CHECK(phase, "player 0 drives more than one character", local_pad_seats(0) == 1);
    PHASE_CHECK(phase, "not exactly two live player characters in Hydeland",
                live_characters(-2) == 2 && live_characters(0) == 1 && live_characters(1) == 1);
}

static void test_early_story_handoff(void) {
    /* the host: Return to tavern hands its room, with the guest in it, to Early Story as the owner's normal room */
    lifecycle_begin();
    early_story_on();
    host_page_with_guest();
    matching2_joined_boss_room(1);
    PHASE_CHECK("ES-H", "Early Story kept a story role in a Boss Battle room", !early_story_visiting() && !early_story_story_room_guest());
    use_machine_save(1);
    host_return_to_tavern();
    PHASE_CHECK("ES-H", "room left at Return to tavern", boss_battle_online_in_room() && boss_battle_online_members() == 2 &&
                                                            room_leaves == 0 && boss_battle_online_is_host());
    PHASE_CHECK("ES-H", "Boss Battle still wants the room", !boss_battle_online_room_wanted() && !boss_battle_online_boss_room());
    PHASE_CHECK("ES-H", "Early Story does not keep the host's existing room in town", town_keeps_room());
    PHASE_CHECK("ES-H", "Early Story treats the host as a guest", !early_story_story_room_guest() && !early_story_visiting());
    host_waits_for_guest_seat("ES-H", 0);
    check_travel_start("ES-H");
    const int wishes = room_wishes;
    guest_leaves_with_host();
    PHASE_CHECK("ES-H", "Boss Battle asked the game for a room on the way out", room_wishes == wishes && room_leaves == 0);
    check_town_arrival("ES-H", 1);
    check_host_resumes_story("ES-H", STORY_STEP, 1);
    lifecycle_end();

    /* a host already past the opening: its story is not the one the room follows */
    lifecycle_begin();
    early_story_on();
    host_page_with_guest();
    matching2_joined_boss_room(1);
    use_machine_save(1);
    host_return_to_tavern();
    check_host_resumes_story("ES-H4", 0x31, 0);
    early_story_off();
    lifecycle_end();

    /* the guest's seat reaches the host before the host tries to leave */
    lifecycle_begin();
    early_story_on();
    host_page_with_guest();
    matching2_joined_boss_room(1);
    use_machine_save(1);
    host_return_to_tavern();
    host_waits_for_guest_seat("ES-H2", 1);
    check_travel_start("ES-H2");
    guest_leaves_with_host();
    check_town_arrival("ES-H2", 1);
    early_story_off();

    /* the other arrival order at the host's game server: the host's own request first, then the guest's */
    lifecycle_end();
    lifecycle_begin();
    early_story_on();
    host_page_with_guest();
    matching2_joined_boss_room(1);
    use_machine_save(1);
    host_return_to_tavern();
    host_seats_request(HOST_MEMBER_SLOT, 1);
    host_seats_request(GUEST_MEMBER_SLOT, 0);
    check_town_operator("ES-H3", 1);
    check_local_player_update("ES-H3");
    check_travel_start("ES-H3");
    early_story_off();

    /* the same room taken back by a new Boss Battle visit: Early Story lets go of it again */
    lifecycle_end();
    lifecycle_begin();
    early_story_on();
    host_page_with_guest();
    matching2_joined_boss_room(1);
    host_return_to_tavern();
    PHASE_CHECK("ES-R", "the room did not resume as a story session before the next visit",
                early_story_session_resume_story_room(STORY_STEP));
    host_enters_boss_battle();
    publish_room_tag();
    PHASE_CHECK("ES-R", "the reclaimed room is not a Boss Battle room again",
                boss_battle_online_boss_room() && boss_battle_online_room_wanted() && !early_story_visiting());
    PHASE_CHECK("ES-R", "the Boss room resumed as a story session", !early_story_session_resume_story_room(STORY_STEP));
    early_story_off();
    lifecycle_end();

    /* the real order: the host's Return to tavern, then what its Matching2 pump sends (the Boss state goes out before the
     * room tag), then the guest's frames in the Inn, and only then the host's travel */
    lifecycle_end();
    lifecycle_begin();
    early_story_on();
    host_page_with_guest();
    matching2_joined_boss_room(1);
    host_return_to_tavern();
    uint8_t host_end[BOSS_BATTLE_STATE_SIZE] = {0};
    const int host_sent = boss_battle_online_publish(host_end);
    BossBattleHostState host_end_state = {BOSS_BATTLE_PHASE_NONE, 0, -1, 0};
    PHASE_CHECK("ES-A", "the host did not send its Boss Battle end before the room went neutral",
                host_sent && boss_battle_state_decode(host_end, sizeof(host_end), &host_end_state) &&
                    host_end_state.phase == BOSS_BATTLE_PHASE_ENDED);
    early_story_off();
    lifecycle_end();

    lifecycle_begin();
    early_story_on();
    guest_lifecycle(0, NEW_INN_MENU);
    matching2_joined_boss_room(0);
    use_machine_save(0);
    early_story_note_host_step(HOST_STORY_STEP);
    PHASE_CHECK("ES-G", "Early Story followed the host's story inside a Boss Battle room", !early_story_visiting());
    if (host_sent) boss_battle_online_internal_update(host_end, sizeof(host_end)); /* Matching2's room.internal */
    menu_scene_frame();
    const GuestRegs closing = live_page_frame(-1);
    if (closing.r15 & 1) put_u32(fake_window + 0xa4, 2);
    menu_scene_frame();
    early_story_note_host_step(HOST_STORY_STEP); /* the game reads the host's record again in the shared room */
    PHASE_CHECK("ES-G", "Guest received Boss tag 0 but remained in Boss visitor state before Host travel",
                !boss_battle_online_visitor() && !boss_battle_online_selected());
    PHASE_CHECK("ES-G", "Guest Boss page still up before Host travel", (closing.r15 & 1) && !boss_battle_screen_shown());
    PHASE_CHECK("ES-G", "Guest Boss characters alive before Host travel", live_characters(-2) == 0);
    PHASE_CHECK("ES-G", "Guest Boss seats not reset before Host travel",
                party_resets == 1 && seats_with_owner(0) == 0 && seats_with_owner(1) == 0);
    PHASE_CHECK("ES-G", "Early Story is not the host's guest before Host travel",
                early_story_visiting() && !early_story_host_room_wanted());
    PHASE_CHECK("ES-G", "guest left the host's room before Host travel",
                boss_battle_online_in_room() && boss_battle_online_members() == 2 && !boss_battle_online_is_host() &&
                    room_leaves == 0);
    PHASE_CHECK("ES-G", "Boss Battle still wants a room on the guest", !boss_battle_online_room_wanted());

    /* the guest's Inn menu runs again: it sends its own party once and the host's game server seats it */
    const int guest_sends = party_sends;
    fake_inn[INN_ACT] = 0x5b;
    fake_inn[INN_STEP] = 0;
    inn_press(-1);
    result_frame();
    result_frame();
    PHASE_CHECK("ES-G", "Guest did not send its party to the host's game server before travel", party_sends == guest_sends + 1);
    host_seats_request(GUEST_MEMBER_SLOT, 1);
    result_frame();
    check_local_player_update("ES-G");
    PHASE_CHECK("ES-G", "Guest is not seated with the host before travel",
                local_seat_of(0) >= 0 && live_characters(0) == 1 && live_characters(1) == 0 && party_sends == guest_sends + 1);

    /* the host's travel: the guest's Inn goes with the scene; nothing of Boss Battle runs during it */
    const int guest_wishes = room_wishes;
    guest_leaves_with_host();
    menu_scene_frame();
    PHASE_CHECK("ES-G", "Boss Battle tore the guest down during the Host's travel", party_resets == 1 && seats_detached == 2);
    PHASE_CHECK("ES-G", "the guest asked the game for a room during the travel", room_wishes == guest_wishes && room_leaves == 0);
    PHASE_CHECK("ES-G", "the guest is not ready to synchronize with the host",
                boss_battle_online_in_room() && boss_battle_online_members() == 2 && early_story_visiting() &&
                    !boss_battle_online_visitor() && !boss_battle_session_active() && live_characters(0) == 1 &&
                    live_characters(1) == 0);
    check_town_arrival("ES-G", 0);
    check_guest_follows_story("ES-G", 3, 3, 4);
    check_guest_follows_story("ES-G2", 0x31, 3, 4);

    /* the host's next Boss Battle visit in that room: the guest is its visitor again, Early Story lets go */
    host_announces(BOSS_BATTLE_PHASE_WAITING, -1, 0, boss_battle_online_host_state().sequence + 1);
    PHASE_CHECK("ES-G", "the guest is not the host's visitor again", boss_battle_online_visitor());
    PHASE_CHECK("ES-G", "Early Story still follows the host's story in a Boss Battle room", !early_story_visiting());
    early_story_off();
    lifecycle_end();

    /* the fallback: no end reached the guest and its scene is already leaving the Inn for town */
    lifecycle_begin();
    early_story_on();
    guest_lifecycle(0, NEW_INN_MENU);
    matching2_joined_boss_room(0);
    boss_battle_set_scene((uint64_t)(uintptr_t)leave_scene_vtable, NULL);
    const BossBattleScene leaving = {kInnScene.act, kInnScene.step, kInnScene.round, kInnScene.stage, 0, 0};
    set_game_scene(leaving);
    GuestRegs frame;
    memset(&frame, 0, sizeof(frame));
    frame.r15 = (uint64_t)(uintptr_t)game_scene;
    boss_battle_scene_hook(&frame);
    PHASE_CHECK("ES-F", "the scene left the Inn while the guest was still a Boss visitor",
                !boss_battle_online_visitor() && party_resets == 1 && live_characters(-2) == 0 && early_story_visiting() == 0);
    const GuestRegs late = live_page_frame(-1);
    PHASE_CHECK("ES-F", "the guest page did not close after the fallback", (late.r15 & 1) != 0);
    boss_battle_set_scene(0, NULL);
    early_story_off();
    lifecycle_end();
}
#endif

int main(void) {
    char user_dir[1100];
#ifdef BOSS_BATTLE_EARLY_STORY_HANDOFF
    snprintf(user_dir, sizeof(user_dir), "%s/build/boss-battle-early-story-handoff-tests", DCP_SOURCE_DIR);
#elif defined(BOSS_BATTLE_HANDOFF)
    snprintf(user_dir, sizeof(user_dir), "%s/build/boss-battle-handoff-tests", DCP_SOURCE_DIR);
#elif defined(BOSS_BATTLE_VISITOR_LIFECYCLE)
    snprintf(user_dir, sizeof(user_dir), "%s/build/boss-battle-visitor-lifecycle-tests", DCP_SOURCE_DIR);
#else
    snprintf(user_dir, sizeof(user_dir), "%s/build/boss-battle-tests", DCP_SOURCE_DIR);
#endif
    char record_file[1200];
    snprintf(record_file, sizeof(record_file), "%s/boss_battle.dat", user_dir);
#ifdef _WIN32
    _mkdir(user_dir);
#else
    mkdir(user_dir, 0755);
#endif
    remove(record_file);
    boss_battle_record_configure(user_dir);
#ifdef BOSS_BATTLE_EARLY_STORY_HANDOFF
    test_early_story_handoff();
    remove(record_file);
    if (failures) {
        printf("%d Boss Battle Early Story handoff check(s) failed\n", failures);
        return 1;
    }
    puts("Boss Battle Early Story handoff tests passed");
    return 0;
#endif
#ifdef BOSS_BATTLE_HANDOFF
    test_visitor_handoff();
    remove(record_file);
    if (failures) {
        printf("%d Boss Battle handoff check(s) failed\n", failures);
        return 1;
    }
    puts("Boss Battle handoff tests passed");
    return 0;
#endif
#ifdef BOSS_BATTLE_VISITOR_LIFECYCLE
    test_visitor_lifecycle();
    remove(record_file);
    if (failures) {
        printf("%d Boss Battle visitor lifecycle check(s) failed\n", failures);
        return 1;
    }
    puts("Boss Battle visitor lifecycle tests passed");
    return 0;
#endif
    test_encounters();
    test_regional_inn_layouts();
    test_language_independence();
    test_arena_rooms();
    test_shared_hook();
    test_setting();
    test_session_origin();
    test_milestones();
    test_win();
    test_win_signals();
    test_stage_end_kinds();
    test_defeat();
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
    test_inn_layout();
    test_inn_menu_off();
    test_inn_menu_built();
    test_inn_menu_draw();
    test_inn_menu_states();
    test_inn_menu_lifetime();
    test_inn_background_layout();
    test_inn_background_hooks();
    test_inn_results();
    test_screen_rows();
    test_screen_entries();
    test_screen_statistics();
    test_screen_hooks();
    test_screen_shortcut_hooks();
    test_visitor_lifecycle();
    test_visitor_handoff();
    test_direct_start();
    test_start_black_hold();
    test_start_reveal();
    test_direct_start_refused();
    test_intentional_returns();
    test_direct_reopen();
    test_reopen_conditions();
    test_normal_inn_untouched();
    test_inn_help();
    test_inn_row_calls();
    test_outcome_hooks();
    test_campaign_restore();
    test_campaign_session();
    test_character_progression();
    test_pause_menu();
    test_fight_all_start();
    test_fight_all_sequence(user_dir);
    test_fight_all_after_clear();
    test_fight_all_checkpoints();
    test_fight_all_death();
    test_fight_all_quit();
    test_fight_all_unresolved();
    test_fight_all_hooks();
    test_fight_all_manual_unchanged();
    test_fight_all_scene_ready();
    test_fight_all_scene_gate();
    test_fight_all_transit();
    test_native_arena_fade_in();
    test_us_image();
    test_real_images();
    test_guest_follows_host_start();
    remove(record_file);
    if (failures) {
        printf("%d Boss Battle check(s) failed\n", failures);
        return 1;
    }
    puts("Boss Battle tests passed");
    return 0;
}
