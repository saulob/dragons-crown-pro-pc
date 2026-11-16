/* Boss Battle Online without the game: the visit's prompt and Network Status, room isolation, host authority, the
 * host's state and a guest's follow, the shared Early Story hooks, the prompt controller, a follower's own session,
 * and the new text in every language. The lobby side is in dcp-online-hle-tests and the Worker's tests. */
#include <stdio.h>
#include <string.h>

#include "boss_battle.h"
#include "boss_battle_online.h"
#include "boss_battle_text.h"
#include "early_story_online.h"
#include "network_access.h"
#include "network_hint.h"
#include "online_prompt.h"

static int failures;
#define CHECK(x)                                                    \
    do {                                                            \
        if (!(x)) {                                                 \
            ++failures;                                             \
            printf("FAIL line %d: %s\n", __LINE__, #x);             \
        }                                                           \
    } while (0)

enum { HARPY = 0, DEMON_LORD = 19 };

static void fresh(void) {
    boss_battle_online_room_left("test");
    boss_battle_online_reset();
    boss_battle_online_set_available(1);
    boss_battle_online_set_hosting(1);
    boss_battle_online_set_capable(1);
}

static void test_visit_and_status(void) {
    boss_battle_online_set_available(0);
    CHECK(boss_battle_online_visit_begin(0) == 0 && boss_battle_online_state() == BOSS_BATTLE_ONLINE_OFF);
    CHECK(!boss_battle_online_access() && !boss_battle_online_hosts_room() && boss_battle_online_may_start());
    CHECK(boss_battle_online_status_message(0) == 1252 && boss_battle_online_status_message(-1) == 1252);

    fresh();
    CHECK(boss_battle_online_visit_begin(0) == 1 && boss_battle_online_state() == BOSS_BATTLE_ONLINE_PROMPT);
    CHECK(boss_battle_online_access() && !boss_battle_online_selected());
    CHECK(boss_battle_online_visit_begin(0) == 1); /* the same visit: still the one prompt */
    boss_battle_online_prompt_finished(0);
    CHECK(boss_battle_online_state() == BOSS_BATTLE_ONLINE_OFFLINE_SELECTED && !boss_battle_online_access());
    CHECK(boss_battle_online_visit_begin(0) == 0 && boss_battle_online_may_start() && !boss_battle_online_hosts_room());
    boss_battle_online_visit_end();
    CHECK(boss_battle_online_visit_begin(0) == 1); /* asked again on the next visit */
    boss_battle_online_prompt_finished(1);
    CHECK(boss_battle_online_state() == BOSS_BATTLE_ONLINE_WAITING && boss_battle_online_selected());
    CHECK(boss_battle_online_status_message(1) == 1253 && boss_battle_online_room_attr() == BOSS_BATTLE_ROOM_OPEN);
    boss_battle_online_native_changed(0);
    CHECK(boss_battle_online_state() == BOSS_BATTLE_ONLINE_OFFLINE_SELECTED);

    fresh();
    CHECK(boss_battle_online_visit_begin(1) == 0 && boss_battle_online_selected()); /* already online: no prompt */
    boss_battle_online_prompt_unavailable("test");
    CHECK(boss_battle_online_selected());
    fresh();
    boss_battle_online_visit_begin(0);
    boss_battle_online_prompt_unavailable("the prompt cannot run here");
    CHECK(boss_battle_online_state() == BOSS_BATTLE_ONLINE_OFFLINE_SELECTED);
}

static void test_state_codec(void) {
    const BossBattleHostState sent = {BOSS_BATTLE_PHASE_STARTING, 1, DEMON_LORD, 0x01020304u};
    uint8_t bytes[BOSS_BATTLE_STATE_SIZE];
    boss_battle_state_encode(&sent, bytes);
    BossBattleHostState got;
    CHECK(boss_battle_state_decode(bytes, sizeof(bytes), &got) && got.phase == sent.phase && got.fight_all == 1 &&
          got.boss == DEMON_LORD && got.sequence == sent.sequence);
    const BossBattleHostState none = {BOSS_BATTLE_PHASE_WAITING, 0, -1, 7};
    boss_battle_state_encode(&none, bytes);
    CHECK(boss_battle_state_decode(bytes, sizeof(bytes), &got) && got.boss == -1);
    CHECK(!boss_battle_state_decode(bytes, sizeof(bytes) - 1, &got));
    bytes[0] = 2;
    CHECK(!boss_battle_state_decode(bytes, sizeof(bytes), &got));
    bytes[0] = BOSS_BATTLE_STATE_FORMAT;
    bytes[1] = BOSS_BATTLE_PHASES;
    CHECK(!boss_battle_state_decode(bytes, sizeof(bytes), &got));
    CHECK(BOSS_BATTLE_ROOM_ATTR == 0x7f03 && BOSS_BATTLE_STATE_ATTR == 0x7f04);
}

static void test_room_isolation(void) {
    const char *why = NULL;
    fresh();
    CHECK(boss_battle_online_join_allowed(0, &why) && boss_battle_online_join_allowed(BOSS_BATTLE_ROOM_OPEN, &why));
    boss_battle_online_set_available(0);
    CHECK(!boss_battle_online_join_allowed(BOSS_BATTLE_ROOM_OPEN, &why) && strstr(why, "not available"));
    fresh();
    CHECK(!boss_battle_online_search_wanted());
    boss_battle_online_visit_begin(1);
    CHECK(boss_battle_online_search_wanted() && boss_battle_online_join_allowed(BOSS_BATTLE_ROOM_OPEN, &why));
    CHECK(!boss_battle_online_join_allowed(0, &why) && strstr(why, "not a Boss Battle"));
    CHECK(!boss_battle_online_join_allowed(BOSS_BATTLE_ROOM_FIGHTING, &why) && strstr(why, "fight"));

    /* a normal room that already has players is never turned into a Boss Battle room */
    boss_battle_online_room_joined(1, 0, 2, 0);
    CHECK(!boss_battle_online_hosts_room() && !boss_battle_online_may_start() &&
          boss_battle_online_room_attr() == BOSS_BATTLE_ROOM_NONE);
    boss_battle_online_members_changed(1);
    CHECK(boss_battle_online_hosts_room() && boss_battle_online_room_attr() == BOSS_BATTLE_ROOM_OPEN);
    boss_battle_online_room_tagged(BOSS_BATTLE_ROOM_OPEN);
    CHECK(boss_battle_online_boss_room() && boss_battle_online_state() == BOSS_BATTLE_ONLINE_HOST);

    /* offline visits never start a fight in someone else's room */
    fresh();
    boss_battle_online_room_joined(0, 0, 2, 1);
    CHECK(!boss_battle_online_may_start());
    boss_battle_online_room_left("test");
    boss_battle_online_room_joined(1, 0, 3, 0);
    CHECK(!boss_battle_online_may_start());
}

static void test_host_authority(void) {
    uint8_t state[BOSS_BATTLE_STATE_SIZE];
    BossBattleHostState host;
    fresh();
    boss_battle_online_visit_begin(1);
    /* alone online: a start needs nobody's answer */
    CHECK(boss_battle_online_host_start(HARPY, 0) && boss_battle_online_start_ready() == 1);
    boss_battle_online_start_abandoned();
    boss_battle_online_room_joined(1, BOSS_BATTLE_ROOM_OPEN, 1, 0);
    boss_battle_online_members_changed(2);
    CHECK(boss_battle_online_is_host() && boss_battle_online_slot() == 0 && boss_battle_online_members() == 2);
    const uint32_t before = boss_battle_online_publish_version();
    CHECK(boss_battle_online_host_start(DEMON_LORD, 0) && boss_battle_online_publish_version() != before);
    CHECK(boss_battle_online_state() == BOSS_BATTLE_ONLINE_STARTING && boss_battle_online_start_ready() == 0);
    CHECK(boss_battle_online_room_attr() == BOSS_BATTLE_ROOM_FIGHTING && boss_battle_online_room_wanted());
    CHECK(boss_battle_online_publish(state) && boss_battle_state_decode(state, sizeof(state), &host) &&
          host.phase == BOSS_BATTLE_PHASE_STARTING && host.boss == DEMON_LORD && host.sequence == 2);
    boss_battle_online_published(boss_battle_online_publish_version() - 2, 1); /* an older publish's answer */
    CHECK(boss_battle_online_start_ready() == 0);
    boss_battle_online_published(boss_battle_online_publish_version(), 1);
    CHECK(boss_battle_online_start_ready() == 1);
    boss_battle_online_note_local(BOSS_BATTLE_LOCAL_TRAVELLING, DEMON_LORD, 0);
    CHECK(boss_battle_online_state() == BOSS_BATTLE_ONLINE_STARTING && boss_battle_online_host_state().phase == BOSS_BATTLE_PHASE_FIGHTING);
    boss_battle_online_note_local(BOSS_BATTLE_LOCAL_FIGHTING, DEMON_LORD, 0);
    CHECK(boss_battle_online_state() == BOSS_BATTLE_ONLINE_FIGHTING);
    /* the hidden Fight All checkpoint keeps the room and refuses joins */
    boss_battle_online_note_local(BOSS_BATTLE_LOCAL_CHECKPOINT, HARPY, 1);
    CHECK(boss_battle_online_state() == BOSS_BATTLE_ONLINE_CHECKPOINT && boss_battle_online_room_wanted() &&
          boss_battle_online_room_attr() == BOSS_BATTLE_ROOM_FIGHTING && boss_battle_online_host_state().fight_all);
    boss_battle_online_note_local(BOSS_BATTLE_LOCAL_RETURNING, HARPY, 1);
    CHECK(boss_battle_online_state() == BOSS_BATTLE_ONLINE_RETURNING);
    boss_battle_online_note_local(BOSS_BATTLE_LOCAL_IDLE, -1, 0);
    CHECK(boss_battle_online_state() == BOSS_BATTLE_ONLINE_HOST && boss_battle_online_room_attr() == BOSS_BATTLE_ROOM_OPEN);
    /* a start the room never heard: the host waits again */
    CHECK(boss_battle_online_host_start(HARPY, 0));
    boss_battle_online_published(boss_battle_online_publish_version(), 0);
    CHECK(boss_battle_online_start_ready() == -1);
    boss_battle_online_start_abandoned();
    CHECK(boss_battle_online_host_state().phase == BOSS_BATTLE_PHASE_WAITING && boss_battle_online_start_ready() == 1);
    /* the only guest leaves while a start waits for it: nobody is left to hear it */
    CHECK(boss_battle_online_host_start(HARPY, 0) && boss_battle_online_start_ready() == 0);
    boss_battle_online_members_changed(1);
    CHECK(boss_battle_online_start_ready() == 1);
    boss_battle_online_start_abandoned();
    /* the visit ends: the members hear it, the tag goes */
    boss_battle_online_visit_end();
    CHECK(boss_battle_online_publish(state) && boss_battle_state_decode(state, sizeof(state), &host) &&
          host.phase == BOSS_BATTLE_PHASE_ENDED);
    CHECK(boss_battle_online_room_attr() == BOSS_BATTLE_ROOM_NONE && !boss_battle_online_room_wanted());
}

static void host_says(BossBattlePhase phase, int boss, int fight_all, uint32_t sequence) {
    const BossBattleHostState state = {phase, fight_all, boss, sequence};
    uint8_t bytes[BOSS_BATTLE_STATE_SIZE];
    boss_battle_state_encode(&state, bytes);
    boss_battle_online_internal_update(bytes, sizeof(bytes));
}

static void test_guest_follow(void) {
    int boss = -1, fight_all = -1;
    fresh();
    boss_battle_online_visit_begin(1);
    boss_battle_online_room_joined(0, BOSS_BATTLE_ROOM_OPEN, 2, 1);
    CHECK(boss_battle_online_is_guest() && boss_battle_online_slot() == 1 && !boss_battle_online_may_start() &&
          !boss_battle_online_hosts_room() && boss_battle_online_room_wanted());
    CHECK(!boss_battle_online_host_start(HARPY, 0) && boss_battle_online_state() == BOSS_BATTLE_ONLINE_GUEST);
    uint8_t state[BOSS_BATTLE_STATE_SIZE];
    CHECK(!boss_battle_online_publish(state));
    host_says(BOSS_BATTLE_PHASE_WAITING, -1, 0, 0);
    CHECK(!boss_battle_online_take_follow(&boss, &fight_all));
    host_says(BOSS_BATTLE_PHASE_STARTING, HARPY, 0, 1);
    CHECK(boss_battle_online_take_follow(&boss, &fight_all) && boss == HARPY && !fight_all);
    CHECK(!boss_battle_online_take_follow(&boss, &fight_all)); /* once per start */
    host_says(BOSS_BATTLE_PHASE_FIGHTING, HARPY, 0, 1);
    host_says(BOSS_BATTLE_PHASE_WAITING, -1, 0, 1);
    host_says(BOSS_BATTLE_PHASE_STARTING, DEMON_LORD, 0, 2);
    boss_battle_online_note_local(BOSS_BATTLE_LOCAL_FIGHTING, HARPY, 0);
    CHECK(!boss_battle_online_take_follow(&boss, &fight_all)); /* still in its own fight */
    boss_battle_online_note_local(BOSS_BATTLE_LOCAL_IDLE, -1, 0);
    CHECK(boss_battle_online_take_follow(&boss, &fight_all) && boss == DEMON_LORD);
    host_says(BOSS_BATTLE_PHASE_STARTING, 5, 0, 1); /* stale */
    CHECK(boss_battle_online_host_state().sequence == 2 && !boss_battle_online_take_follow(&boss, &fight_all));
    /* Fight All: boss 01, then boss 02 once this guest's own checkpoint is reached */
    host_says(BOSS_BATTLE_PHASE_STARTING, 0, 1, 3);
    CHECK(boss_battle_online_take_follow(&boss, &fight_all) && boss == 0 && fight_all);
    boss_battle_online_note_local(BOSS_BATTLE_LOCAL_CHECKPOINT, 0, 1);
    host_says(BOSS_BATTLE_PHASE_STARTING, 1, 1, 4);
    CHECK(boss_battle_online_take_follow(&boss, &fight_all) && boss == 1 && fight_all);
    CHECK(boss_battle_online_room_wanted()); /* the room survives the checkpoint */
    boss_battle_online_note_local(BOSS_BATTLE_LOCAL_IDLE, -1, 0);
    /* a start already under way when this guest sees it is not followed */
    host_says(BOSS_BATTLE_PHASE_FIGHTING, 3, 0, 5);
    CHECK(!boss_battle_online_take_follow(&boss, &fight_all));
    host_says(BOSS_BATTLE_PHASE_STARTING, 3, 0, 5);
    CHECK(!boss_battle_online_take_follow(&boss, &fight_all));
    /* the host leaves Boss Battle */
    host_says(BOSS_BATTLE_PHASE_ENDED, -1, 0, 5);
    CHECK(boss_battle_online_state() == BOSS_BATTLE_ONLINE_ENDED && !boss_battle_online_room_wanted());

    /* ownership changes: the session ends for everyone, the new owner clears the tag */
    fresh();
    boss_battle_online_visit_begin(1);
    boss_battle_online_room_joined(0, BOSS_BATTLE_ROOM_OPEN, 3, 2);
    boss_battle_online_owner_changed(1);
    CHECK(boss_battle_online_state() == BOSS_BATTLE_ONLINE_ENDED && boss_battle_online_is_host() &&
          !boss_battle_online_boss_room() && boss_battle_online_room_attr() == BOSS_BATTLE_ROOM_NONE);
    /* the host disconnects or the room closes: the guest's session ends */
    fresh();
    boss_battle_online_visit_begin(1);
    boss_battle_online_room_joined(0, BOSS_BATTLE_ROOM_OPEN, 2, 1);
    host_says(BOSS_BATTLE_PHASE_STARTING, HARPY, 0, 1);
    boss_battle_online_room_left("the room was closed");
    CHECK(boss_battle_online_state() == BOSS_BATTLE_ONLINE_ENDED && !boss_battle_online_take_follow(&boss, &fight_all));
    /* a guest's disconnect leaves the host's session as it was */
    fresh();
    boss_battle_online_visit_begin(1);
    boss_battle_online_room_joined(1, BOSS_BATTLE_ROOM_OPEN, 2, 0);
    boss_battle_online_members_changed(1);
    CHECK(boss_battle_online_state() == BOSS_BATTLE_ONLINE_HOST && boss_battle_online_room_attr() == BOSS_BATTLE_ROOM_OPEN);
}

static uint8_t fake_record[0x80];

static void test_shared_hooks(void) {
    static const NetworkAccessRules rules = {boss_battle_online_access, boss_battle_online_room_wanted,
                                             boss_battle_pause_available};
    fresh();
    network_access_set_installed(1);
    network_access_set(NETWORK_ACCESS_BOSS_BATTLE, NULL);
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    memset(fake_record, 0, sizeof(fake_record));
    regs.rdi = (uint64_t)(uintptr_t)fake_record;
    early_story_online_check_hook(&regs);
    CHECK(regs.skip && regs.rax == 0);
    network_access_set(NETWORK_ACCESS_BOSS_BATTLE, &rules);
    boss_battle_online_visit_begin(0); /* the prompt: the online check passes for it */
    memset(&regs, 0, sizeof(regs));
    regs.rdi = (uint64_t)(uintptr_t)fake_record;
    early_story_online_check_hook(&regs);
    CHECK(regs.skip && regs.rax == 1);
    boss_battle_online_prompt_finished(0);
    memset(&regs, 0, sizeof(regs));
    regs.rdi = (uint64_t)(uintptr_t)fake_record;
    early_story_online_check_hook(&regs);
    CHECK(regs.rax == 0);

    /* the Inn keeps the host's room as the Castle Gate does */
    fresh();
    memset(&regs, 0, sizeof(regs));
    regs.rax = 3;
    early_story_room_wish_hook(&regs);
    CHECK(regs.rax == 3);
    boss_battle_online_visit_begin(1);
    early_story_room_wish_hook(&regs);
    CHECK(regs.rax == EARLY_STORY_ROOM_WISH_GATE);

    /* without a fight the director's step check is the game's */
    memset(&regs, 0, sizeof(regs));
    early_story_menu_step_hook(&regs);
    CHECK(!regs.skip);
    network_access_set(NETWORK_ACCESS_BOSS_BATTLE, NULL);
    network_access_set_installed(0);
}

static int prompt_calls, prompt_frames;
static uint64_t prompt_owner_sysobj;

static uint8_t GUEST_HOOK_ABI fake_prompt(uint64_t address, int slot) {
    uint8_t *owner = (uint8_t *)(uintptr_t)address;
    ++prompt_calls;
    memcpy(&prompt_owner_sysobj, owner + ONLINE_PROMPT_OWNER_SYSOBJ, sizeof(prompt_owner_sysobj));
    owner[ONLINE_PROMPT_OWNER_STATES + slot] = 1;
    return ++prompt_frames >= 3;
}

static int net_online;
static uint8_t GUEST_HOOK_ABI fake_is_online(uint64_t net) {
    (void)net;
    return (uint8_t)net_online;
}

static void test_prompt_and_native_state(void) {
    static OnlinePrompt prompt;
    online_prompt_configure(NULL, 0);
    CHECK(!online_prompt_configured() && online_prompt_start(&prompt, 0x1234) == ONLINE_PROMPT_FINISHED);
    online_prompt_configure(fake_prompt, 0x5150);
    prompt_calls = prompt_frames = 0;
    CHECK(online_prompt_start(&prompt, 0x4000) == ONLINE_PROMPT_SHOWN && prompt.open && prompt_owner_sysobj == 0x4000);
    uint64_t vtable;
    memcpy(&vtable, prompt.owner, sizeof(vtable));
    CHECK(vtable == 0x5150);
    CHECK(online_prompt_step(&prompt) == ONLINE_PROMPT_RUNNING);
    CHECK(online_prompt_step(&prompt) == ONLINE_PROMPT_FINISHED && !prompt.open && prompt.settled && prompt_calls == 3);
    online_prompt_configure(NULL, 0);

    static uint8_t manager[0x100], game[0x16920], net[0x10], vtable_bytes[0x300];
    CHECK(online_native_online(0) == -1 && online_native_online((uint64_t)(uintptr_t)manager) == -1);
    const uint64_t game_at = (uint64_t)(uintptr_t)game, net_at = (uint64_t)(uintptr_t)net;
    const uint64_t vtable_at = (uint64_t)(uintptr_t)vtable_bytes, method = (uint64_t)(uintptr_t)fake_is_online;
    memcpy(manager + ONLINE_MANAGER_GAME, &game_at, 8);
    memcpy(game + ONLINE_GAME_NET, &net_at, 8);
    memcpy(net, &vtable_at, 8);
    memcpy(vtable_bytes + ONLINE_NET_IS_ONLINE, &method, 8);
    net_online = 0;
    CHECK(online_native_online((uint64_t)(uintptr_t)manager) == 0);
    net_online = 1;
    CHECK(online_native_online((uint64_t)(uintptr_t)manager) == 1);
}

static int protects, releases, snapshots, restores;
static int protect(void) { return ++protects; }
static void release(void) { ++releases; }
static int snapshot(void) { return ++snapshots; }
static int restore(void) { return ++restores; }

static BossBattleProbe arena_probe(void) {
    BossBattleProbe probe;
    memset(&probe, 0, sizeof(probe));
    probe.scene = (BossBattleScene){2, 0, 10, 0, -1, 0};
    return probe;
}

static void test_follower_session(void) {
    boss_battle_reset();
    boss_battle_set_save_guard(protect, release);
    boss_battle_set_memory_guard(snapshot, restore);
    boss_battle_set_available(1);
    boss_battle_set_character("Guest", 0, 10);
    protects = releases = snapshots = restores = 0;
    int index = -1;
    CHECK(!boss_battle_follow(BOSS_BATTLE_ENCOUNTERS, 0));
    CHECK(boss_battle_follow(DEMON_LORD, 0) && boss_battle_begin_queued(&index) && index == DEMON_LORD);
    boss_battle_follow_started(index);
    /* the guest's own campaign is set aside on its own PC */
    CHECK(boss_battle_follower() && protects == 1 && snapshots == 1 && boss_battle_state() == BOSS_BATTLE_TRAVELLING);
    CHECK(!boss_battle_follow(HARPY, 0));
    /* a follower never requests a scene, even resolved */
    CHECK(boss_battle_party_wiped() && boss_battle_state() == BOSS_BATTLE_RESOLVING);
    const BossBattleProbe probe = arena_probe();
    CHECK(boss_battle_frame(&probe) == BOSS_BATTLE_FRAME_NONE);
    /* back in the Inn the host's travel brought it to: its own restore */
    boss_battle_inn_arrived();
    CHECK(!boss_battle_session_active() && !boss_battle_follower() && restores == 1 && releases == 1);

    /* the host is gone mid-fight: the follower's own game takes it back */
    CHECK(boss_battle_follow(HARPY, 0) && boss_battle_begin_queued(&index));
    boss_battle_follow_started(index);
    boss_battle_follow_released("the host left");
    CHECK(!boss_battle_follower() && boss_battle_session_active());
    CHECK(boss_battle_party_wiped() && boss_battle_frame(&probe) == BOSS_BATTLE_FRAME_REQUEST_INN);
    boss_battle_inn_arrived();
    CHECK(restores == 2);

    /* Fight All: a guest's run starts with the host's first boss; its next boss only after its own checkpoint */
    CHECK(!boss_battle_fight_all_follow_ready(1) && !boss_battle_follow(1, 1));
    CHECK(boss_battle_follow(0, 1) && boss_battle_begin_queued(&index) && index == 0);
    boss_battle_follow_started(index);
    CHECK(boss_battle_fight_all_active() && boss_battle_follower());
    boss_battle_follow_released("the host left");
    boss_battle_inn_arrived();
    CHECK(!boss_battle_fight_all_active() && !boss_battle_session_active());
    boss_battle_reset();
}

static void test_text(void) {
    const char *english = NULL;
    for (int i = 0; i < BOSS_BATTLE_TEXT_LANGUAGES; ++i) {
        const char *key = boss_battle_text_language_key(i);
        CHECK(key && boss_battle_text_set_language(key, 0) == 1);
        const char *waiting = boss_battle_text(BOSS_BATTLE_TEXT_WAITING_FOR_HOST);
        CHECK(waiting && *waiting && !strchr(waiting, '%'));
        if (i == 0) english = waiting;
        else CHECK(strcmp(waiting, english) != 0);
    }
    CHECK(boss_battle_text_set_language("russian", 1) == 1 && *boss_battle_text(BOSS_BATTLE_TEXT_WAITING_FOR_HOST));
    boss_battle_text_set_language("english", 0);
    CHECK(!strcmp(boss_battle_text(BOSS_BATTLE_TEXT_WAITING_FOR_HOST), "Waiting for the host"));
}

static void host_state_update(BossBattlePhase phase, int boss, uint32_t sequence) {
    const BossBattleHostState state = {phase, 0, boss, sequence};
    uint8_t bytes[BOSS_BATTLE_STATE_SIZE];
    boss_battle_state_encode(&state, bytes);
    CHECK(boss_battle_online_internal_update(bytes, sizeof(bytes)));
}

/* Capability is the hooks; the setting only decides who hosts, as Early Story's active and hosting do. */
static void test_capability_and_visitor(void) {
    const char *why = NULL;
    fresh();
    boss_battle_online_set_hosting(0);
    CHECK(boss_battle_online_available() && !boss_battle_online_hosting());
    CHECK(boss_battle_online_visit_begin(0) == 0 && boss_battle_online_state() == BOSS_BATTLE_ONLINE_OFF);
    CHECK(boss_battle_online_visit_begin(1) == 0 && !boss_battle_online_selected() && !boss_battle_online_hosts_room() &&
          boss_battle_online_room_attr() == BOSS_BATTLE_ROOM_NONE && !boss_battle_online_room_wanted());
    CHECK(boss_battle_online_join_allowed(BOSS_BATTLE_ROOM_OPEN, &why));

    early_story_set_hosting(0);
    boss_battle_online_room_joined(0, BOSS_BATTLE_ROOM_OPEN, 2, 1);
    CHECK(boss_battle_online_visitor() && boss_battle_online_selected() && boss_battle_online_is_guest() &&
          boss_battle_online_state() == BOSS_BATTLE_ONLINE_GUEST && boss_battle_online_access() &&
          boss_battle_online_room_wanted());
    CHECK(!boss_battle_online_may_start() && !boss_battle_online_hosts_room() && !boss_battle_online_hosting() &&
          boss_battle_online_room_attr() == BOSS_BATTLE_ROOM_NONE);
    CHECK(!early_story_hosting() && !early_story_visiting());
    int boss = -1, fight_all = -1;
    host_state_update(BOSS_BATTLE_PHASE_STARTING, 3, 1);
    CHECK(boss_battle_online_take_follow(&boss, &fight_all) == 1 && boss == 3 && !fight_all);

    boss_battle_online_room_left("test");
    CHECK(!boss_battle_online_visitor() && !boss_battle_online_selected() && boss_battle_online_state() == BOSS_BATTLE_ONLINE_OFF &&
          !boss_battle_online_room_wanted() && !boss_battle_online_access() && !boss_battle_online_hosting());

    /* a normal room never starts a visit */
    boss_battle_online_room_joined(0, 0, 2, 1);
    CHECK(!boss_battle_online_visitor() && !boss_battle_online_selected());
    boss_battle_online_room_left("test");

    /* the host ending the session, and the owner changing, end the visit as well */
    boss_battle_online_room_joined(0, BOSS_BATTLE_ROOM_OPEN, 2, 1);
    host_state_update(BOSS_BATTLE_PHASE_ENDED, -1, 2);
    CHECK(!boss_battle_online_visitor() && boss_battle_online_state() == BOSS_BATTLE_ONLINE_OFF);
    boss_battle_online_room_left("test");
    boss_battle_online_room_joined(0, BOSS_BATTLE_ROOM_OPEN, 2, 1);
    boss_battle_online_owner_changed(1);
    CHECK(!boss_battle_online_visitor() && !boss_battle_online_selected() && !boss_battle_online_hosts_room());
    boss_battle_online_room_left("test");

    /* the game going offline ends it too, and a visitor never carries a prompt or a visit of its own */
    boss_battle_online_room_joined(0, BOSS_BATTLE_ROOM_OPEN, 2, 1);
    boss_battle_online_native_changed(0);
    CHECK(!boss_battle_online_visitor() && !boss_battle_online_selected());
    boss_battle_online_native_changed(1);
    CHECK(!boss_battle_online_selected());
    boss_battle_online_room_left("test");

    /* the setting on: the player hosts */
    boss_battle_online_set_hosting(1);
    CHECK(boss_battle_online_hosting() && boss_battle_online_visit_begin(1) == 0 && boss_battle_online_selected() &&
          boss_battle_online_hosts_room() && !boss_battle_online_visitor());
    boss_battle_online_room_joined(1, BOSS_BATTLE_ROOM_OPEN, 1, 0);
    CHECK(boss_battle_online_state() == BOSS_BATTLE_ONLINE_HOST && !boss_battle_online_visitor());
    boss_battle_online_room_left("test");
    boss_battle_online_visit_end();

    /* a guest with the setting on and its own visit is the host's visitor while seated, then back on its own visit */
    fresh();
    boss_battle_online_visit_begin(1);
    boss_battle_online_room_joined(0, BOSS_BATTLE_ROOM_OPEN, 2, 1);
    CHECK(boss_battle_online_visitor() && boss_battle_online_state() == BOSS_BATTLE_ONLINE_GUEST);
    boss_battle_online_room_joined(0, BOSS_BATTLE_ROOM_OPEN, 2, 1); /* the Network Menu's close reports the room again */
    CHECK(boss_battle_online_visitor() && boss_battle_online_selected());
    boss_battle_online_room_left("test");
    CHECK(!boss_battle_online_visitor() && boss_battle_online_state() == BOSS_BATTLE_ONLINE_ENDED);
    boss_battle_online_visit_end();

    /* the hooks failed: no capability, no visit, no hosting */
    fresh();
    boss_battle_online_set_available(0);
    CHECK(!boss_battle_online_available() && !boss_battle_online_hosting() && !boss_battle_online_join_allowed(BOSS_BATTLE_ROOM_OPEN, &why));
    boss_battle_online_room_joined(0, BOSS_BATTLE_ROOM_OPEN, 2, 1);
    CHECK(!boss_battle_online_visitor() && !boss_battle_online_selected());
    boss_battle_online_room_left("test");
    early_story_set_hosting(1);
}

static uint64_t fake_now = 1000;
static uint64_t fake_clock(void) {
    return fake_now;
}

static int pad_frame(int down) {
    fake_now += 16;
    return network_shortcut_pad_filter(down);
}

/* One director frame: its poll of player 1's Menu press, as the shared hook answers it. */
static int director_poll(void) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    fake_now += 16;
    early_story_menu_press_hook(&regs);
    return regs.skip ? (int)regs.rax : 0;
}

static void test_shared_shortcut(void) {
    const NetworkShortcutOwner story = NETWORK_SHORTCUT_EARLY_STORY, boss = NETWORK_SHORTCUT_BOSS_BATTLE;
    network_access_set_clock(fake_clock);
    network_shortcut_reset();

    /* nothing owns it: the press reaches the game and the director hears none */
    CHECK(!network_shortcut_armed() && network_shortcut_owner() == NETWORK_SHORTCUT_NONE);
    CHECK(pad_frame(1) == 0 && director_poll() == 0);
    CHECK(pad_frame(0) == 0);
    fake_now += 200;

    /* Boss Battle owns it while its page asks each frame; it is armed once its request point ran */
    CHECK(network_shortcut_arm(boss, 1) == 1 && network_shortcut_owner() == boss && !network_shortcut_armed());
    CHECK(pad_frame(1) == 0); /* the director has not polled: the game keeps the button */
    pad_frame(0);
    CHECK(director_poll() == 0 && network_shortcut_armed());
    CHECK(pad_frame(1) == 1 && pad_frame(1) == 1); /* hidden from the game, held or not */
    const unsigned before = network_shortcut_requests();
    CHECK(director_poll() == 1 && network_shortcut_requests() == before + 1); /* the same hook that serves Early Story */
    CHECK(director_poll() == 0);
    CHECK(network_shortcut_arm(boss, 0) == 0 && network_shortcut_owner() == NETWORK_SHORTCUT_NONE);
    CHECK(pad_frame(1) == 1); /* still held when the page let go: hidden until released */
    CHECK(pad_frame(0) == 0 && pad_frame(1) == 0);
    pad_frame(0);

    /* a lease that is not renewed lapses, with its owner */
    network_shortcut_arm(boss, 1);
    fake_now += 200;
    CHECK(network_shortcut_owner() == NETWORK_SHORTCUT_NONE && !network_shortcut_armed());

    /* a press too old for the director's frame is not taken */
    network_shortcut_arm(boss, 1);
    director_poll();
    pad_frame(1);
    fake_now += 400;
    network_shortcut_arm(boss, 1);
    pad_frame(0);
    CHECK(director_poll() == 0);

    /* one owner at a time: a live Early Story keeps it from Boss Battle, and takes it from Boss Battle */
    network_shortcut_reset();
    CHECK(network_shortcut_arm(story, 1) == 1 && network_shortcut_arm(boss, 1) == 0 && network_shortcut_owner() == story);
    CHECK(network_shortcut_arm(boss, 0) == 0 && network_shortcut_owner() == story); /* Boss Battle letting go leaves it */
    CHECK(network_shortcut_arm(story, 0) == 0 && network_shortcut_owner() == NETWORK_SHORTCUT_NONE);
    CHECK(network_shortcut_arm(boss, 1) == 1 && network_shortcut_arm(story, 1) == 1 && network_shortcut_owner() == story);
    CHECK(network_shortcut_arm(boss, 1) == 0);
    fake_now += 200; /* Early Story stops asking: Boss Battle gets it back */
    CHECK(network_shortcut_arm(boss, 1) == 1 && network_shortcut_owner() == boss);

    network_access_set_clock(NULL);
    network_shortcut_reset();
}

static void test_menu_chain(void) {
    /* the director's step check and kind store, answered for either owner */
    network_access_set_clock(fake_clock);
    network_shortcut_reset();
    network_shortcut_arm(NETWORK_SHORTCUT_BOSS_BATTLE, 1);
    director_poll();
    pad_frame(1);
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    early_story_menu_press_hook(&regs);
    CHECK(regs.skip && regs.rax == 1);
    memset(&regs, 0, sizeof(regs));
    early_story_menu_step_hook(&regs);
    CHECK(regs.skip && regs.rax == 1);
    uint8_t window[0x1600] = {0};
    memset(&regs, 0, sizeof(regs));
    regs.rbx = (uint64_t)(uintptr_t)window;
    regs.r15 = 1;
    const unsigned menus = network_access_menus_created();
    early_story_menu_kind_hook(&regs);
    int32_t kind;
    memcpy(&kind, window + EARLY_STORY_MENU_KIND, sizeof(kind));
    CHECK(kind == EARLY_STORY_MENU_KIND_NETWORK && network_access_menus_created() == menus + 1);
    pad_frame(0);
    network_access_set_clock(NULL);
    network_shortcut_reset();
}

static uint8_t hint_ui[0x1000], hint_object[0x800], hint_manager[0x400];
static uint64_t hint_vtable[0x40];
static int hint_creates, hint_shows, hint_hides;
static char hint_text[96];

static void put_pointer(uint8_t *at, const void *pointer) {
    const uint64_t value = (uint64_t)(uintptr_t)pointer;
    memcpy(at, &value, sizeof(value));
}

static uint64_t GUEST_HOOK_ABI hint_create(uint64_t ui, int init) {
    CHECK(ui == (uint64_t)(uintptr_t)hint_ui && init == 0);
    ++hint_creates;
    put_pointer(hint_ui + NETWORK_HINT_UI_WINDOWS + (size_t)NETWORK_HINT_WINDOW_TYPE * NETWORK_HINT_UI_SLOT_SIZE, hint_object);
    put_pointer(hint_object, hint_vtable);
    return (uint64_t)(uintptr_t)hint_object;
}

static void GUEST_HOOK_ABI hint_set_text(uint64_t window, int message, const char *text) {
    (void)window;
    CHECK(message == NETWORK_HINT_MESSAGE);
    snprintf(hint_text, sizeof(hint_text), "%s", text);
}

static void GUEST_HOOK_ABI hint_refresh(uint64_t object) {
    CHECK(object == (uint64_t)(uintptr_t)hint_object + NETWORK_HINT_TEXT_OBJECT);
}

static void GUEST_HOOK_ABI hint_hide(uint64_t window) {
    CHECK(window == (uint64_t)(uintptr_t)hint_object);
    ++hint_hides;
}

static void GUEST_HOOK_ABI hint_show(uint64_t window, int duration, int flag) {
    CHECK(window == (uint64_t)(uintptr_t)hint_object && duration == -1 && flag == 0);
    ++hint_shows;
}

/* Early Story keeps the top help dialog hint; the page uses the header instead. */
static uint64_t hint_world(void) {
    memset(hint_ui, 0, sizeof(hint_ui));
    memset(hint_object, 0, sizeof(hint_object));
    memset(hint_manager, 0, sizeof(hint_manager));
    hint_vtable[NETWORK_HINT_SHOW_SLOT / 8] = (uint64_t)(uintptr_t)hint_show;
    put_pointer(hint_manager + NETWORK_HINT_MANAGER_UI, hint_ui);
    const NetworkHintFunctions functions = {hint_create, (NetworkHintTextFn)(uintptr_t)hint_set_text, hint_hide, hint_refresh, 0};
    network_hint_configure(&functions);
    network_hint_set_portuguese(0);
    hint_creates = hint_shows = hint_hides = 0;
    return (uint64_t)(uintptr_t)hint_manager;
}

/* Both features show the one hint through the one implementation: the same dialog, the same glyph, one owner at a time. */
static void test_hint_owners(void) {
    const uint64_t manager = hint_world();
    network_hint_show(NETWORK_HINT_BOSS_BATTLE, manager);
    CHECK(network_hint_shown_by(NETWORK_HINT_BOSS_BATTLE) && !network_hint_shown_by(NETWORK_HINT_EARLY_STORY) &&
          hint_creates == 1 && hint_shows == 1 && !strcmp(hint_text, "@#(2) Open Network Menu"));
    network_hint_show(NETWORK_HINT_BOSS_BATTLE, manager);
    CHECK(hint_creates == 1 && hint_shows == 1);
    network_hint_show(NETWORK_HINT_EARLY_STORY, manager);
    network_hint_hide(NETWORK_HINT_EARLY_STORY);
    CHECK(network_hint_shown_by(NETWORK_HINT_BOSS_BATTLE) && hint_hides == 0);
    /* hidden while the menu is up (the director's hook hides whoever has it), and back when the page asks again */
    network_hint_hide_any();
    CHECK(!network_hint_shown_by(NETWORK_HINT_BOSS_BATTLE) && hint_hides == 1);
    network_hint_show(NETWORK_HINT_BOSS_BATTLE, manager);
    CHECK(network_hint_shown_by(NETWORK_HINT_BOSS_BATTLE) && hint_shows == 2);
    network_hint_hide(NETWORK_HINT_BOSS_BATTLE);
    network_hint_set_portuguese(1);
    network_hint_show(NETWORK_HINT_BOSS_BATTLE, manager);
    CHECK(!strcmp(hint_text, "@#(2) Abrir Menu de Rede"));
    network_hint_hide_any();
    network_hint_configure(NULL);
}

static void test_early_story_hint(void) {
    const uint64_t manager = hint_world();

    network_hint_show(NETWORK_HINT_EARLY_STORY, manager);
    CHECK(network_hint_shown_by(NETWORK_HINT_EARLY_STORY) && hint_creates == 1 && hint_shows == 1);
    CHECK(!strcmp(hint_text, "@#(2) Open Network Menu"));
    network_hint_show(NETWORK_HINT_EARLY_STORY, manager);
    CHECK(hint_creates == 1 && hint_shows == 1);
    network_hint_hide(NETWORK_HINT_EARLY_STORY);
    CHECK(!network_hint_shown_by(NETWORK_HINT_EARLY_STORY) && hint_hides == 1);
    network_hint_set_portuguese(1);
    network_hint_show(NETWORK_HINT_EARLY_STORY, manager);
    CHECK(!strcmp(hint_text, "@#(2) Abrir Menu de Rede"));
    network_hint_hide(NETWORK_HINT_EARLY_STORY);
    network_hint_set_portuguese(0);
    network_hint_configure(NULL);
    network_hint_show(NETWORK_HINT_EARLY_STORY, manager);
    CHECK(!network_hint_shown_by(NETWORK_HINT_EARLY_STORY));
}

static uint8_t menu_ui[0x400], menu_window[0x1600];
static uint64_t menu_window_vtable[0x40];
static int menu_creates, menu_inits;
static uint32_t menu_pad = 0xff;

static void GUEST_HOOK_ABI fake_menu_init(uint64_t window) {
    CHECK(window == (uint64_t)(uintptr_t)menu_window);
    ++menu_inits;
}

static uint64_t GUEST_HOOK_ABI fake_menu_create(uint64_t ui, uint32_t pad) {
    CHECK(ui == (uint64_t)(uintptr_t)menu_ui);
    ++menu_creates;
    menu_pad = pad;
    const uint64_t vtable = (uint64_t)(uintptr_t)menu_window_vtable;
    memcpy(menu_window, &vtable, sizeof(vtable));
    return (uint64_t)(uintptr_t)menu_window;
}

static int32_t menu_window_kind(void) {
    int32_t kind;
    memcpy(&kind, menu_window + EARLY_STORY_MENU_KIND, sizeof(kind));
    return kind;
}

/* Early Story in Hydeland: the director's Menu poll with player 1 (its player checks passed), its step check, then
 * the kind of the window it created. Returns 1 when that window became the Network Menu. */
static int early_story_director_frame(void) {
    GuestRegs press;
    memset(&press, 0, sizeof(press));
    fake_now += 16;
    early_story_menu_press_hook(&press);
    if (!press.skip || !press.rax) return 0;
    GuestRegs step;
    memset(&step, 0, sizeof(step));
    early_story_menu_step_hook(&step);
    GuestRegs kind;
    memset(&kind, 0, sizeof(kind));
    kind.r15 = 1;
    kind.rbx = (uint64_t)(uintptr_t)menu_window;
    early_story_menu_kind_hook(&kind);
    return step.skip && menu_window_kind() == EARLY_STORY_MENU_KIND_NETWORK;
}

/* Boss Battle's page in the Inn, which has no character: the page is the request point and, on the kept press, makes
 * the director's menu request itself. Returns 1 when the Network Menu was created. */
static int boss_battle_page_frame(int ready) {
    fake_now += 16;
    if (!network_shortcut_arm(NETWORK_SHORTCUT_BOSS_BATTLE, ready && network_menu_request_available())) return 0;
    network_shortcut_poll();
    return network_shortcut_take_press() && network_menu_request((uint64_t)(uintptr_t)menu_ui) != 0;
}

/* Both owners end in the same kind step on a window the game created; only Early Story goes through the director. */
static void test_network_menu_request(void) {
    network_access_set_clock(fake_clock);
    network_shortcut_reset();
    menu_window_vtable[0x100 / 8] = (uint64_t)(uintptr_t)fake_menu_init;

    /* Early Story: the director's front half, then the shared kind step */
    memset(menu_window, 0, sizeof(menu_window));
    unsigned created = network_access_menus_created();
    network_shortcut_arm(NETWORK_SHORTCUT_EARLY_STORY, 1);
    CHECK(early_story_director_frame() == 0 && network_shortcut_armed());
    network_shortcut_arm(NETWORK_SHORTCUT_EARLY_STORY, 1);
    CHECK(network_shortcut_pad_filter(1) == 1 && early_story_director_frame() == 1);
    CHECK(network_access_menus_created() == created + 1 && menu_creates == 0);
    network_shortcut_pad_filter(0);
    network_shortcut_reset();

    /* Boss Battle without the game's request: never owns the shortcut, the press reaches the game */
    network_menu_set_create(NULL);
    CHECK(boss_battle_page_frame(1) == 0 && !network_shortcut_armed() && network_shortcut_pad_filter(1) == 0);
    network_shortcut_pad_filter(0);

    /* Boss Battle in the Inn: no director poll, the page arms it; the press is hidden and becomes the same request */
    network_menu_set_create(fake_menu_create);
    memset(menu_window, 0, sizeof(menu_window));
    created = network_access_menus_created();
    const unsigned requests = network_shortcut_requests();
    CHECK(boss_battle_page_frame(1) == 0 && network_shortcut_armed() && menu_creates == 0);
    CHECK(network_shortcut_pad_filter(1) == 1);
    CHECK(boss_battle_page_frame(1) == 1 && menu_creates == 1 && menu_inits == 1 && menu_pad == 0);
    CHECK(menu_window_kind() == EARLY_STORY_MENU_KIND_NETWORK && network_access_menus_created() == created + 1 &&
          network_shortcut_requests() == requests + 1);
    int32_t player;
    memcpy(&player, menu_window + 0xf8, sizeof(player));
    CHECK(player == 0);
    CHECK(network_shortcut_pad_filter(1) == 1 && boss_battle_page_frame(1) == 0 && menu_creates == 1); /* held */
    network_shortcut_pad_filter(0);

    /* not ready (prompt, menu, window, transition, Online Play off): nothing armed, nothing requested */
    CHECK(boss_battle_page_frame(0) == 0 && !network_shortcut_armed() && network_shortcut_pad_filter(1) == 0);
    network_shortcut_pad_filter(0);
    CHECK(menu_creates == 1);

    /* Early Story keeps the shortcut from a page */
    network_shortcut_arm(NETWORK_SHORTCUT_EARLY_STORY, 1);
    CHECK(boss_battle_page_frame(1) == 0 && network_shortcut_owner() == NETWORK_SHORTCUT_EARLY_STORY);

    network_menu_set_create(NULL);
    network_access_set_clock(NULL);
    network_shortcut_reset();
}

/* After the Network Menu closes, player 1's buttons stay hidden until all are released, once. */
static void test_menu_quarantine(void) {
    network_shortcut_reset();
    CHECK(network_menu_pad_hold(1) == 0 && network_menu_pad_hold(0) == 0);
    network_menu_quarantine_begin();
    CHECK(network_menu_pad_hold(1) == 1 && network_menu_pad_hold(1) == 1);
    CHECK(network_menu_pad_hold(0) == 0 && network_menu_pad_hold(1) == 0);
    network_menu_quarantine_begin();
    network_shortcut_reset();
    CHECK(network_menu_pad_hold(1) == 0);
}

/* The room exists while the host waits on the page, as Early Story's does in town: a friend can join before any boss, the
 * start reuses it, and it returns to joinable afterwards. */
static void test_room_before_start(void) {
    fresh();
    boss_battle_online_visit_begin(1);
    CHECK(boss_battle_online_hosts_room() && boss_battle_online_room_wanted() && !boss_battle_online_in_room());
    CHECK(boss_battle_online_room_attr() == BOSS_BATTLE_ROOM_OPEN); /* what the room is created with, before any boss */
    boss_battle_online_room_joined(1, BOSS_BATTLE_ROOM_OPEN, 1, 0);
    CHECK(boss_battle_online_is_host() && boss_battle_online_boss_room() && boss_battle_online_state() == BOSS_BATTLE_ONLINE_HOST);
    CHECK(boss_battle_online_host_state().phase == BOSS_BATTLE_PHASE_WAITING);
    const char *reason = NULL;
    CHECK(boss_battle_online_join_allowed(BOSS_BATTLE_ROOM_OPEN, &reason)); /* a friend can join before a boss */
    boss_battle_online_members_changed(2);
    CHECK(boss_battle_online_room_attr() == BOSS_BATTLE_ROOM_OPEN && boss_battle_online_room_wanted());
    /* the network menu closing changes nothing about the room */
    boss_battle_online_report_room();
    CHECK(boss_battle_online_is_host() && boss_battle_online_members() == 2);
    /* the start reuses the room */
    CHECK(boss_battle_online_host_start(HARPY, 0) && boss_battle_online_is_host() && boss_battle_online_members() == 2);
    CHECK(boss_battle_online_room_attr() == BOSS_BATTLE_ROOM_FIGHTING && !boss_battle_online_join_allowed(BOSS_BATTLE_ROOM_FIGHTING, &reason));
    boss_battle_online_published(boss_battle_online_publish_version(), 1);
    boss_battle_online_note_local(BOSS_BATTLE_LOCAL_TRAVELLING, HARPY, 0);
    boss_battle_online_note_local(BOSS_BATTLE_LOCAL_FIGHTING, HARPY, 0);
    CHECK(boss_battle_online_is_host() && boss_battle_online_room_attr() == BOSS_BATTLE_ROOM_FIGHTING);
    /* back on the page the same room is joinable again */
    boss_battle_online_note_local(BOSS_BATTLE_LOCAL_IDLE, -1, 0);
    CHECK(boss_battle_online_is_host() && boss_battle_online_room_attr() == BOSS_BATTLE_ROOM_OPEN &&
          boss_battle_online_join_allowed(BOSS_BATTLE_ROOM_OPEN, &reason));
    /* Online Play off or an offline visit: no room is wanted, so none is asked for */
    boss_battle_online_visit_end();
    CHECK(!boss_battle_online_room_wanted() && boss_battle_online_room_attr() == BOSS_BATTLE_ROOM_NONE);
    fresh();
    boss_battle_online_set_hosting(0);
    CHECK(!boss_battle_online_visit_begin(1) && !boss_battle_online_hosts_room() && !boss_battle_online_room_wanted());
    boss_battle_online_set_hosting(1);
}

/* Client B's side of a friend join, with its Boss Battle setting On and Off: as in Early Story a client with the setting
 * On has made its own room, the game leaves it to join the friend's, and nothing makes it again under the join. */
static void guest_joins_friend(int setting_on) {
    fresh();
    boss_battle_online_set_hosting(setting_on);
    boss_battle_online_visit_begin(1);
    if (setting_on) {
        CHECK(boss_battle_online_hosts_room());
        boss_battle_online_room_joined(1, BOSS_BATTLE_ROOM_OPEN, 1, 0); /* its own empty room */
        CHECK(boss_battle_online_is_host());
        boss_battle_online_room_left("the room was left"); /* the game's leave before the join */
        CHECK(!boss_battle_online_in_room() && !boss_battle_online_is_guest());
    } else {
        CHECK(!boss_battle_online_hosts_room() && !boss_battle_online_room_wanted());
    }
    const char *reason = NULL;
    CHECK(boss_battle_online_join_allowed(BOSS_BATTLE_ROOM_OPEN, &reason));
    boss_battle_online_room_joined(0, BOSS_BATTLE_ROOM_OPEN, 2, 1); /* the host's room */
    CHECK(boss_battle_online_is_guest() && !boss_battle_online_is_host());
    CHECK(boss_battle_online_visitor()); /* the host's visitor, whatever its own setting */
    CHECK(!boss_battle_online_may_start() && !boss_battle_online_hosts_room() && boss_battle_online_slot() == 1);
    CHECK(boss_battle_online_state() == BOSS_BATTLE_ONLINE_GUEST);
    boss_battle_online_set_hosting(1);
}

/* Client A hosts alone, B joins, A's start reuses the one room. */
static void test_friend_join(void) {
    guest_joins_friend(1);
    guest_joins_friend(0);
    fresh();
    boss_battle_online_visit_begin(1);
    boss_battle_online_room_joined(1, BOSS_BATTLE_ROOM_OPEN, 1, 0);
    boss_battle_online_members_changed(2);
    CHECK(boss_battle_online_is_host() && boss_battle_online_members() == 2 && boss_battle_online_may_start());
    CHECK(boss_battle_online_host_start(HARPY, 0) && boss_battle_online_is_host() && boss_battle_online_members() == 2);
    CHECK(boss_battle_online_room_attr() == BOSS_BATTLE_ROOM_FIGHTING);
    /* the guest leaves while the host travels to the boss: the start is given up once */
    CHECK(!boss_battle_online_take_start_peer_lost());
    boss_battle_online_note_local(BOSS_BATTLE_LOCAL_TRAVELLING, HARPY, 0);
    CHECK(!boss_battle_online_take_start_peer_lost());
    boss_battle_online_members_changed(1);
    CHECK(boss_battle_online_take_start_peer_lost() && !boss_battle_online_take_start_peer_lost());
    boss_battle_online_note_local(BOSS_BATTLE_LOCAL_IDLE, -1, 0);
    /* two players who both host keep one room each */
    fresh();
    boss_battle_online_visit_begin(1);
    boss_battle_online_room_joined(1, BOSS_BATTLE_ROOM_OPEN, 1, 0);
    CHECK(boss_battle_online_is_host() && boss_battle_online_room_wanted());
}

int main(void) {
    test_visit_and_status();
    test_state_codec();
    test_room_isolation();
    test_host_authority();
    test_guest_follow();
    test_shared_hooks();
    test_prompt_and_native_state();
    test_follower_session();
    test_text();
    test_capability_and_visitor();
    test_shared_shortcut();
    test_menu_chain();
    test_network_menu_request();
    test_menu_quarantine();
    test_early_story_hint();
    test_hint_owners();
    test_room_before_start();
    test_friend_join();
    boss_battle_online_reset();
    if (failures) {
        printf("%d Boss Battle Online check(s) failed\n", failures);
        return 1;
    }
    puts("Boss Battle Online tests passed");
    return 0;
}
