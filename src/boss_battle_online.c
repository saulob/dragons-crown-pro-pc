#include "boss_battle_online.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef enum { VISIT_OFF, VISIT_PROMPT, VISIT_OFFLINE, VISIT_ONLINE, VISIT_ENDED } Visit;
typedef enum { START_NONE, START_WAITING, START_READY, START_FAILED } StartGate;

/* Written by the game thread (the hooks) and by Matching2 under its own lock, which is also the game thread's pump. */
static atomic_int available_flag, capable_flag, hosting_flag, visitor_flag;
static atomic_int visit = VISIT_OFF;
static atomic_int local = BOSS_BATTLE_LOCAL_IDLE;
static atomic_int in_room, is_owner, room_boss, members, slot, start_players;
/* The room this client owns was a Boss Battle room: a later visit takes it back with the players still in it. */
static atomic_int owned_boss_room;
static _Atomic(BossBattleRoomHandoffFn) room_handoff;

void boss_battle_online_set_room_handoff(BossBattleRoomHandoffFn handoff) {
    atomic_store(&room_handoff, handoff);
}

static void hand_room(int boss_room) {
    const BossBattleRoomHandoffFn handoff = atomic_load(&room_handoff);
    if (handoff && atomic_load(&in_room)) handoff(atomic_load(&is_owner), boss_room);
}
static struct {
    BossBattleHostState own, received;
    int start_pending, received_known;
    uint32_t version, start_version, taken_sequence;
    StartGate gate;
} s = {{BOSS_BATTLE_PHASE_NONE, 0, -1, 0}, {BOSS_BATTLE_PHASE_NONE, 0, -1, 0}, 0, 0, 0, 0, 0, START_NONE};

static const char *const kStateText[] = {"off",   "prompt",   "offline", "online waiting", "online host", "online guest",
                                         "starting", "fighting", "checkpoint", "returning", "ended"};

const char *boss_battle_online_state_text(BossBattleOnlineState state) {
    return (unsigned)state < sizeof(kStateText) / sizeof(*kStateText) ? kStateText[state] : "unknown";
}

static int guest_seat(void) {
    return atomic_load(&in_room) && !atomic_load(&is_owner);
}

static int boss_seat(void) {
    return atomic_load(&in_room) && atomic_load(&room_boss);
}

BossBattleOnlineState boss_battle_online_state(void) {
    switch (atomic_load(&visit)) {
    case VISIT_PROMPT: return BOSS_BATTLE_ONLINE_PROMPT;
    case VISIT_OFFLINE: return BOSS_BATTLE_ONLINE_OFFLINE_SELECTED;
    case VISIT_ENDED: return BOSS_BATTLE_ONLINE_ENDED;
    case VISIT_ONLINE: break;
    default: return BOSS_BATTLE_ONLINE_OFF;
    }
    switch (atomic_load(&local)) {
    case BOSS_BATTLE_LOCAL_TRAVELLING: return BOSS_BATTLE_ONLINE_STARTING;
    case BOSS_BATTLE_LOCAL_FIGHTING: return BOSS_BATTLE_ONLINE_FIGHTING;
    case BOSS_BATTLE_LOCAL_CHECKPOINT: return BOSS_BATTLE_ONLINE_CHECKPOINT;
    case BOSS_BATTLE_LOCAL_RETURNING: return BOSS_BATTLE_ONLINE_RETURNING;
    default: break;
    }
    if (s.start_pending) return BOSS_BATTLE_ONLINE_STARTING;
    if (!boss_seat()) return BOSS_BATTLE_ONLINE_WAITING;
    return atomic_load(&is_owner) ? BOSS_BATTLE_ONLINE_HOST : BOSS_BATTLE_ONLINE_GUEST;
}

void boss_battle_state_encode(const BossBattleHostState *state, uint8_t out[BOSS_BATTLE_STATE_SIZE]) {
    out[0] = BOSS_BATTLE_STATE_FORMAT;
    out[1] = (uint8_t)state->phase;
    out[2] = (uint8_t)(state->fight_all != 0);
    out[3] = state->boss < 0 ? 0xff : (uint8_t)state->boss;
    for (int i = 0; i < 4; ++i) out[4 + i] = (uint8_t)(state->sequence >> (8 * i));
}

int boss_battle_state_decode(const uint8_t *data, size_t size, BossBattleHostState *state) {
    if (!data || size < BOSS_BATTLE_STATE_SIZE || data[0] != BOSS_BATTLE_STATE_FORMAT || data[1] == BOSS_BATTLE_PHASE_NONE ||
        data[1] >= BOSS_BATTLE_PHASES || data[2] > 1)
        return 0;
    state->phase = (BossBattlePhase)data[1];
    state->fight_all = data[2];
    state->boss = data[3] == 0xff ? -1 : data[3];
    state->sequence = 0;
    for (int i = 0; i < 4; ++i) state->sequence |= (uint32_t)data[4 + i] << (8 * i);
    return 1;
}

void boss_battle_online_set_available(int available) {
    atomic_store(&available_flag, available != 0);
    if (!available) boss_battle_online_reset();
}

int boss_battle_online_available(void) {
    return atomic_load(&available_flag);
}

void boss_battle_online_set_capable(int capable) {
    atomic_store(&capable_flag, capable != 0);
}

int boss_battle_online_capable(void) {
    return atomic_load(&capable_flag);
}

void boss_battle_online_set_hosting(int allowed) {
    atomic_store(&hosting_flag, allowed != 0);
}

int boss_battle_online_hosting(void) {
    return boss_battle_online_available() && atomic_load(&hosting_flag);
}

int boss_battle_online_visitor(void) {
    return atomic_load(&visitor_flag);
}

static void set_own(BossBattlePhase phase, int boss, int fight_all) {
    if (s.own.phase == phase && s.own.boss == boss && s.own.fight_all == fight_all) return;
    s.own.phase = phase;
    s.own.boss = boss;
    s.own.fight_all = fight_all;
    ++s.version;
}

static void go_online(void) {
    atomic_store(&visit, VISIT_ONLINE);
    set_own(BOSS_BATTLE_PHASE_WAITING, -1, 0);
    printf("Boss Battle Online: online, %s\n", boss_battle_online_state_text(boss_battle_online_state()));
}

int boss_battle_online_visit_begin(int native_online) {
    const Visit now = (Visit)atomic_load(&visit);
    if (now == VISIT_ONLINE || now == VISIT_PROMPT) return now == VISIT_PROMPT;
    if (now == VISIT_OFFLINE) return 0;
    if (!boss_battle_online_hosting()) {
        atomic_store(&visit, VISIT_OFF);
        return 0;
    }
    if (native_online == 1) {
        go_online();
        return 0;
    }
    atomic_store(&visit, VISIT_PROMPT);
    puts("Boss Battle Online: the game's online prompt is asked");
    return 1;
}

void boss_battle_online_prompt_finished(int native_online) {
    if (atomic_load(&visit) != VISIT_PROMPT) return;
    if (native_online == 1) {
        go_online();
        return;
    }
    atomic_store(&visit, VISIT_OFFLINE);
    puts("Boss Battle Online: offline for this visit");
}

void boss_battle_online_prompt_unavailable(const char *why) {
    if (atomic_load(&visit) != VISIT_PROMPT) return;
    atomic_store(&visit, VISIT_OFFLINE);
    printf("Boss Battle Online: offline for this visit, %s\n", why);
}

static void end_session(void) {
    s.start_pending = 0;
    s.gate = START_NONE;
    s.received_known = 0;
    s.received = (BossBattleHostState){BOSS_BATTLE_PHASE_NONE, 0, -1, 0};
    set_own(BOSS_BATTLE_PHASE_ENDED, -1, 0);
}

/* A guest that was already online on its own page when it joined keeps its own visit's end; one that came only to
 * follow the host has nothing of its own to go back to. */
static atomic_int visitor_own_visit;

/* Set when a guest joins a host's Boss Battle room; the hooks take it once to cover the join visually. */
static atomic_int guest_join_flag;

int boss_battle_online_take_guest_join(void) {
    return atomic_exchange(&guest_join_flag, 0) && atomic_load(&visitor_flag);
}

static void set_visitor(int on, const char *reason) {
    const int was = atomic_exchange(&visitor_flag, on);
    if (was != on) printf("DEBUG_SAULO VISITOR old=%d new=%d reason=%s\n", was, on, reason); /* DEBUG_SAULO */
}

static void drop_visitor(void) {
    if (!atomic_load(&visitor_flag)) return;
    set_visitor(0, "the host's Boss Battle session is over for this guest");
    atomic_store(&visit, atomic_exchange(&visitor_own_visit, 0) ? VISIT_ENDED : VISIT_OFF);
    atomic_store(&room_boss, 0);
    end_session();
    puts("Boss Battle Online: visitor mode ended");
}

static void session_over(void) {
    if (atomic_load(&visitor_flag)) drop_visitor();
    else if (atomic_load(&visit) == VISIT_ONLINE) atomic_store(&visit, VISIT_ENDED);
}

static void start_visitor(void) {
    if (atomic_load(&visitor_flag)) return;
    set_visitor(1, "joined a host's Boss Battle room");
    if (atomic_load(&visit) == VISIT_ONLINE) {
        atomic_store(&visitor_own_visit, 1);
        return;
    }
    atomic_store(&visit, VISIT_ONLINE);
    set_own(BOSS_BATTLE_PHASE_WAITING, -1, 0);
    puts("Boss Battle Online: visitor mode, following the host's Boss Battle session");
}

void boss_battle_online_end_visitor(const char *why) {
    if (!atomic_load(&visitor_flag)) return;
    printf("Boss Battle Online: visitor mode ends: %s\n", why);
    drop_visitor();
}

void boss_battle_online_visit_end(void) {
    set_visitor(0, "the Boss Battle visit ended");
    atomic_store(&visitor_own_visit, 0);
    const Visit was = (Visit)atomic_exchange(&visit, VISIT_OFF);
    atomic_store(&local, BOSS_BATTLE_LOCAL_IDLE);
    atomic_store(&start_players, 0);
    end_session();
    if (was == VISIT_ONLINE || was == VISIT_ENDED) puts("Boss Battle Online: the Boss Battle visit ended");
}

void boss_battle_online_reset(void) {
    atomic_store(&guest_join_flag, 0);
    set_visitor(0, "reset");
    atomic_store(&visitor_own_visit, 0);
    atomic_store(&visit, VISIT_OFF);
    atomic_store(&local, BOSS_BATTLE_LOCAL_IDLE);
    atomic_store(&start_players, 0);
    end_session();
}

void boss_battle_online_native_changed(int native_online) {
    const Visit now = (Visit)atomic_load(&visit);
    if (now == VISIT_ONLINE && native_online == 0) {
        const int follower_only = atomic_load(&visitor_flag) && !atomic_exchange(&visitor_own_visit, 0);
        set_visitor(0, "the game went offline");
        atomic_store(&visit, follower_only ? VISIT_OFF : VISIT_OFFLINE);
        end_session();
        puts("Boss Battle Online: the game went offline");
    } else if (now == VISIT_OFFLINE && native_online == 1 && boss_battle_online_hosting()) {
        go_online();
    }
}

int boss_battle_online_access(void) {
    const Visit now = (Visit)atomic_load(&visit);
    return now == VISIT_PROMPT || now == VISIT_ONLINE;
}

int boss_battle_online_selected(void) {
    return atomic_load(&visit) == VISIT_ONLINE;
}

int boss_battle_online_status_message(int native_online) {
    return native_online == 1 ? BOSS_BATTLE_STATUS_ONLINE_MESSAGE : BOSS_BATTLE_STATUS_OFFLINE_MESSAGE;
}

/* A normal room that already has other players is never turned into a Boss Battle room. */
static int shared_normal_room(void) {
    return atomic_load(&in_room) && !atomic_load(&room_boss) && atomic_load(&members) > 1 &&
           !(atomic_load(&is_owner) && atomic_load(&owned_boss_room));
}

int boss_battle_online_hosts_room(void) {
    return boss_battle_online_selected() && !atomic_load(&visitor_flag) && !guest_seat() && !shared_normal_room();
}

static int local_busy(void) {
    return s.start_pending || atomic_load(&local) != BOSS_BATTLE_LOCAL_IDLE;
}

int boss_battle_online_room_attr(void) {
    if (!boss_battle_online_hosts_room()) return BOSS_BATTLE_ROOM_NONE;
    return local_busy() ? BOSS_BATTLE_ROOM_FIGHTING : BOSS_BATTLE_ROOM_OPEN;
}

int boss_battle_online_room_wanted(void) {
    return boss_battle_online_hosts_room() || (boss_battle_online_selected() && guest_seat() && boss_seat());
}

int boss_battle_online_search_wanted(void) {
    return boss_battle_online_selected();
}

int boss_battle_online_join_allowed(int room_attr, const char **reason) {
    const char *unused;
    if (!reason) reason = &unused;
    *reason = "";
    if (room_attr <= 0) {
        if (boss_battle_online_search_wanted()) *reason = "the room is not a Boss Battle session";
        return !boss_battle_online_search_wanted();
    }
    if (!boss_battle_online_available()) {
        *reason = "Boss Battle Online is not available";
        return 0;
    }
    if (room_attr != BOSS_BATTLE_ROOM_OPEN) {
        *reason = "the host is in a fight";
        return 0;
    }
    return 1;
}

void boss_battle_online_room_joined(int owner, int room_attr, int count, int party_slot) {
    atomic_store(&is_owner, owner != 0);
    atomic_store(&room_boss, room_attr > 0);
    atomic_store(&members, count);
    atomic_store(&slot, party_slot);
    atomic_store(&in_room, 1);
    atomic_store(&owned_boss_room, owner && room_attr > 0);
    s.received_known = 0;
    s.received = (BossBattleHostState){BOSS_BATTLE_PHASE_NONE, 0, -1, 0};
    s.taken_sequence = 0;
    if (room_attr <= 0) return;
    if (!owner && boss_battle_online_available()) {
        start_visitor();
        atomic_store(&guest_join_flag, 1);
    }
    printf("Boss Battle Online: room ready, %s, %d players\n", owner ? "host" : "guest", count);
    if (owner && count <= 1) puts("Boss Battle Online: waiting for party");
    if (!owner) puts("Boss Battle Online: joined as guest, visitor waiting for host");
}

void boss_battle_online_room_tagged(int room_attr) {
    if (!atomic_load(&in_room) || !atomic_load(&is_owner)) return;
    const int was = atomic_exchange(&room_boss, room_attr > 0);
    if (room_attr > 0) atomic_store(&owned_boss_room, 1);
    if (room_attr > 0 && !was) hand_room(1);
}

void boss_battle_online_room_neutral(void) {
    if (!atomic_load(&in_room)) return;
    atomic_store(&room_boss, 0);
    hand_room(0);
    puts("Boss Battle Online: the room stays with its players, no longer a Boss Battle room");
}

void boss_battle_online_members_changed(int count) {
    const int before = atomic_exchange(&members, count);
    if (!boss_seat() || !atomic_load(&is_owner) || count == before) return;
    if (count > before) printf("Boss Battle Online: guest joined, %d players\n", count);
    if (count <= 1) puts("Boss Battle Online: waiting for party");
}

void boss_battle_online_owner_changed(int owner) {
    const int was_boss = boss_seat();
    atomic_store(&is_owner, owner != 0);
    if (!was_boss) return;
    atomic_store(&room_boss, 0);
    session_over();
    end_session();
    puts("Boss Battle Online: the room changed owner, the Boss Battle session ended");
}

void boss_battle_online_room_left(const char *why) {
    const int was_guest = guest_seat() && boss_seat();
    atomic_store(&guest_join_flag, 0);
    atomic_store(&in_room, 0);
    atomic_store(&is_owner, 0);
    atomic_store(&room_boss, 0);
    atomic_store(&owned_boss_room, 0);
    atomic_store(&members, 0);
    atomic_store(&slot, 0);
    s.received_known = 0;
    if (s.gate == START_WAITING) s.gate = START_READY;
    if (!was_guest) return;
    session_over();
    end_session();
    printf("Boss Battle Online: left the host's Boss Battle room (%s)\n", why ? why : "");
}

int boss_battle_online_internal_update(const uint8_t *data, size_t size) {
    BossBattleHostState state;
    if (!guest_seat() || !boss_battle_state_decode(data, size, &state)) return 0;
    if (!boss_seat()) {
        /* the owner publishes a Boss Battle state only while its room is a Boss Battle room again */
        if (state.phase == BOSS_BATTLE_PHASE_ENDED || !boss_battle_online_available()) return 0;
        atomic_store(&room_boss, 1);
        s.received_known = 0;
        hand_room(1);
        start_visitor();
        puts("Boss Battle Online: the host's room is a Boss Battle room again, visitor waiting for host");
    }
    if (s.received_known && state.sequence < s.received.sequence) return 0;
    s.received = state;
    s.received_known = 1;
    if (state.phase == BOSS_BATTLE_PHASE_ENDED && atomic_load(&visit) == VISIT_ONLINE) {
        session_over();
        puts("Boss Battle Online: the host ended the Boss Battle session");
        puts("DEBUG_SAULO BOSSHANDOFF host end received, visitor=0");
    }
    return 1;
}

int boss_battle_online_in_room(void) {
    return atomic_load(&in_room);
}

int boss_battle_online_is_host(void) {
    return atomic_load(&in_room) && atomic_load(&is_owner);
}

int boss_battle_online_is_guest(void) {
    return guest_seat();
}

int boss_battle_online_members(void) {
    return atomic_load(&members);
}

int boss_battle_online_slot(void) {
    return atomic_load(&slot);
}

int boss_battle_online_boss_room(void) {
    return boss_seat();
}

static _Atomic(BossBattleFriendRoomFn) friend_room_lookup;

void boss_battle_online_set_friend_room_lookup(BossBattleFriendRoomFn lookup) {
    atomic_store(&friend_room_lookup, lookup);
}

int boss_battle_online_friend_in_boss_room(const char *online_id) {
    const BossBattleFriendRoomFn lookup = atomic_load(&friend_room_lookup);
    return lookup && online_id && lookup(online_id);
}

BossBattleHostState boss_battle_online_host_state(void) {
    return guest_seat() ? s.received : s.own;
}

int boss_battle_online_may_start(void) {
    return !guest_seat() && !shared_normal_room();
}

int boss_battle_online_host_start(int boss, int fight_all) {
    if (!boss_battle_online_may_start()) return 0;
    if (!boss_battle_online_selected()) return 1;
    ++s.own.sequence;
    s.start_pending = 1;
    set_own(BOSS_BATTLE_PHASE_STARTING, boss, fight_all);
    ++s.version;
    s.start_version = s.version;
    s.gate = boss_seat() && atomic_load(&is_owner) && atomic_load(&members) > 1 ? START_WAITING : START_READY;
    const int players = boss_seat() ? atomic_load(&members) : 1;
    atomic_store(&start_players, players);
    if (boss_seat() && atomic_load(&is_owner)) puts("Boss Battle Online: existing room reused for boss start");
    printf("Boss Battle Online: host starting boss %d%s with %d player%s%s\n", boss, fight_all ? " (Fight All)" : "", players,
           players == 1 ? "" : "s", boss_seat() ? "" : ", no Boss Battle room");
    return 1;
}

int boss_battle_online_take_start_peer_lost(void) {
    const int expected = atomic_load(&start_players);
    if (expected < 2 || atomic_load(&local) != BOSS_BATTLE_LOCAL_TRAVELLING || atomic_load(&members) >= expected) return 0;
    atomic_store(&start_players, 0);
    printf("Boss Battle Online: a player left before the fight (%d of %d), the start is cancelled\n", atomic_load(&members),
           expected);
    return 1;
}

uint32_t boss_battle_online_publish_version(void) {
    return s.version;
}

/* The owner of a room it hosted as a Boss Battle room still tells its members the visit ended after the room went neutral:
 * that state is the members' only signal to end their own visit. */
int boss_battle_online_publish(uint8_t out[BOSS_BATTLE_STATE_SIZE]) {
    const int hosted = boss_seat() || (atomic_load(&in_room) && atomic_load(&owned_boss_room));
    if (!hosted || !atomic_load(&is_owner) || s.own.phase == BOSS_BATTLE_PHASE_NONE) return 0;
    boss_battle_state_encode(&s.own, out);
    return 1;
}

void boss_battle_online_published(uint32_t version, int ok) {
    if (s.gate != START_WAITING || version < s.start_version) return;
    s.gate = ok ? START_READY : START_FAILED;
    if (!ok) puts("Boss Battle Online: the room did not take the host's start");
}

int boss_battle_online_start_ready(void) {
    if (s.gate == START_FAILED) return -1;
    if (s.gate == START_WAITING && !(boss_seat() && atomic_load(&members) > 1)) s.gate = START_READY;
    return s.gate != START_WAITING;
}

void boss_battle_online_start_abandoned(void) {
    if (!s.start_pending) return;
    s.start_pending = 0;
    s.gate = START_NONE;
    set_own(BOSS_BATTLE_PHASE_WAITING, -1, 0);
}

static BossBattlePhase phase_of(BossBattleLocal now) {
    switch (now) {
    case BOSS_BATTLE_LOCAL_TRAVELLING:
    case BOSS_BATTLE_LOCAL_FIGHTING: return BOSS_BATTLE_PHASE_FIGHTING;
    case BOSS_BATTLE_LOCAL_CHECKPOINT: return BOSS_BATTLE_PHASE_CHECKPOINT;
    case BOSS_BATTLE_LOCAL_RETURNING: return BOSS_BATTLE_PHASE_RETURNING;
    default: return BOSS_BATTLE_PHASE_WAITING;
    }
}

void boss_battle_online_note_local(BossBattleLocal now, int boss, int fight_all) {
    atomic_store(&local, now);
    if (now != BOSS_BATTLE_LOCAL_IDLE) {
        s.start_pending = 0;
        s.gate = START_NONE;
    }
    if (!boss_battle_online_selected() || guest_seat()) return;
    if (s.start_pending) return;
    set_own(phase_of(now), now == BOSS_BATTLE_LOCAL_IDLE ? -1 : boss, now == BOSS_BATTLE_LOCAL_IDLE ? 0 : fight_all);
}

int boss_battle_online_take_follow(int *boss, int *fight_all) {
    if (!boss_battle_online_selected() || !guest_seat() || !boss_seat() || !s.received_known) return 0;
    const BossBattleHostState host = s.received;
    if (host.sequence == s.taken_sequence) return 0;
    if (host.phase != BOSS_BATTLE_PHASE_STARTING) {
        if (host.phase == BOSS_BATTLE_PHASE_FIGHTING || host.phase == BOSS_BATTLE_PHASE_CHECKPOINT) {
            s.taken_sequence = host.sequence;
            printf("Boss Battle Online: start %u is already under way, not followed\n", host.sequence);
        }
        return 0;
    }
    const BossBattleLocal now = (BossBattleLocal)atomic_load(&local);
    if (now != BOSS_BATTLE_LOCAL_IDLE && now != BOSS_BATTLE_LOCAL_CHECKPOINT) return 0;
    s.taken_sequence = host.sequence;
    *boss = host.boss;
    *fight_all = host.fight_all;
    printf("Boss Battle Online: following host start %u, boss %d%s\n", host.sequence, host.boss,
           host.fight_all ? " (Fight All)" : "");
    return 1;
}

void boss_battle_online_follow_failed(const char *why) {
    printf("Boss Battle Online: the host's start was not followed: %s\n", why);
}

void boss_battle_online_report_room(void) {
    if (!boss_seat()) {
        puts("Boss Battle Online: no Boss Battle room yet");
        return;
    }
    printf("Boss Battle Online: room ready, %s, %d players\n", atomic_load(&is_owner) ? "host" : "guest", atomic_load(&members));
    if (atomic_load(&is_owner) && atomic_load(&members) <= 1) puts("Boss Battle Online: waiting for party");
}
