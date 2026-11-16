/* Boss Battle Online: the session rules of a Boss Battle visit with Online Play (docs/ONLINE.md). Runtime state only;
 * the Boss Battle hooks drive the visit, Matching2 (runtime_matching2.cpp) reports the room and publishes the host's
 * state. Independent of Early Story Online. */
#ifndef DCP_BOSS_BATTLE_ONLINE_H
#define DCP_BOSS_BATTLE_ONLINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Private room int attribute: 1 a Boss Battle room taking players, 2 one in a fight. */
#define BOSS_BATTLE_ROOM_ATTR 0x7f03
/* Private room internal binary attribute: the host's Boss Battle state. */
#define BOSS_BATTLE_STATE_ATTR 0x7f04
#define BOSS_BATTLE_STATE_SIZE 8
#define BOSS_BATTLE_STATE_FORMAT 1

enum { BOSS_BATTLE_ROOM_NONE = 0, BOSS_BATTLE_ROOM_OPEN = 1, BOSS_BATTLE_ROOM_FIGHTING = 2 };

/* The game's SystemMsg records "Network Status: Offline" and "Network Status: Online". */
enum { BOSS_BATTLE_STATUS_OFFLINE_MESSAGE = 1252, BOSS_BATTLE_STATUS_ONLINE_MESSAGE = 1253 };

typedef enum {
    BOSS_BATTLE_ONLINE_OFF,
    BOSS_BATTLE_ONLINE_PROMPT,
    BOSS_BATTLE_ONLINE_OFFLINE_SELECTED,
    BOSS_BATTLE_ONLINE_WAITING, /* online, not in a Boss Battle room yet */
    BOSS_BATTLE_ONLINE_HOST,
    BOSS_BATTLE_ONLINE_GUEST,
    BOSS_BATTLE_ONLINE_STARTING,
    BOSS_BATTLE_ONLINE_FIGHTING,
    BOSS_BATTLE_ONLINE_CHECKPOINT,
    BOSS_BATTLE_ONLINE_RETURNING,
    BOSS_BATTLE_ONLINE_ENDED
} BossBattleOnlineState;

const char *boss_battle_online_state_text(BossBattleOnlineState state);
BossBattleOnlineState boss_battle_online_state(void);

/* The host's state as the room carries it. */
typedef enum {
    BOSS_BATTLE_PHASE_NONE,
    BOSS_BATTLE_PHASE_WAITING,
    BOSS_BATTLE_PHASE_STARTING,
    BOSS_BATTLE_PHASE_FIGHTING,
    BOSS_BATTLE_PHASE_CHECKPOINT,
    BOSS_BATTLE_PHASE_RETURNING,
    BOSS_BATTLE_PHASE_ENDED,
    BOSS_BATTLE_PHASES
} BossBattlePhase;

typedef struct {
    BossBattlePhase phase;
    int fight_all;
    int boss; /* the encounter index, -1 for none */
    uint32_t sequence;
} BossBattleHostState;

void boss_battle_state_encode(const BossBattleHostState *state, uint8_t out[BOSS_BATTLE_STATE_SIZE]);
/* 0 for data that is not a Boss Battle state of this format. */
int boss_battle_state_decode(const uint8_t *data, size_t size, BossBattleHostState *state);

/* Online Play is on and the hooks Boss Battle Online needs are in. */
void boss_battle_online_set_available(int available);
int boss_battle_online_available(void);
/* The lobby keeps Boss Battle rooms to clients that declared the capability. */
void boss_battle_online_set_capable(int capable);
int boss_battle_online_capable(void);

/* The Boss Battle page is opened from the Inn; `native_online` is the game's own online state (-1 unknown). 1 when the
 * game's online prompt has to be asked first. */
int boss_battle_online_visit_begin(int native_online);
/* The prompt finished: the game's online state after it. */
void boss_battle_online_prompt_finished(int native_online);
/* The prompt could not be shown here: the visit stays offline. */
void boss_battle_online_prompt_unavailable(const char *why);
/* The player left Boss Battle for the Inn: the session ends and the room loses its Boss Battle tag. */
void boss_battle_online_visit_end(void);
/* The title, or a scene outside Boss Battle: everything ends. */
void boss_battle_online_reset(void);
/* The game went offline (or online) during the visit. */
void boss_battle_online_native_changed(int native_online);

/* The Boss Battle setting lets this player start a Boss Battle visit and host a room; joining needs only the capability. */
void boss_battle_online_set_hosting(int allowed);
int boss_battle_online_hosting(void);
/* Joined as a guest to a Boss Battle room without a visit of its own: a session-long visit that ends with the room. */
int boss_battle_online_visitor(void);

/* The visit asked for online play: the online check passes for the lead character while it lasts. */
int boss_battle_online_access(void);
/* The visit uses online play (prompt answered Yes, or already online). */
int boss_battle_online_selected(void);
/* SystemMsg of the page's Network Status line. */
int boss_battle_online_status_message(int native_online);

/* This client wants its own room to be a Boss Battle room: an online visit that is not a guest's. */
int boss_battle_online_hosts_room(void);
/* The int attribute this client's room carries: BOSS_BATTLE_ROOM_*. */
int boss_battle_online_room_attr(void);
/* The Inn keeps the room as the Castle Gate does: a Boss Battle host, or a guest seated in a Boss Battle room. */
int boss_battle_online_room_wanted(void);

/* A search or join made from the Boss Battle page only finds Boss Battle rooms; any other finds none. */
int boss_battle_online_search_wanted(void);
/* `room_attr` the room's BOSS_BATTLE_ROOM_ATTR (0 a normal room). 1 to join; 0 with a short reason. */
int boss_battle_online_join_allowed(int room_attr, const char **reason);

/* The room as Matching2 sees it. `slot` is this client's party slot. */
void boss_battle_online_room_joined(int is_owner, int room_attr, int members, int slot);
void boss_battle_online_members_changed(int members);
/* The owner's room now carries `room_attr` as its Boss Battle tag. */
void boss_battle_online_room_tagged(int room_attr);
/* The feature that keeps a shared room when Boss Battle lets it go (boss_room 0, the room's players stay together) or
 * takes it back (boss_room 1): Early Story's session in the game (early_story_session_room_handoff). */
typedef void (*BossBattleRoomHandoffFn)(int is_owner, int boss_room);
void boss_battle_online_set_room_handoff(BossBattleRoomHandoffFn handoff);
/* A visit ended with its players still in its room: the room stays theirs, no longer a Boss Battle room. */
void boss_battle_online_room_neutral(void);
/* This guest stops being the host's visitor without the host's end state (its scene leaves the Inn for town first). */
void boss_battle_online_end_visitor(const char *why);
/* A Boss Battle session ends with its owner, as an Early Story one does. */
void boss_battle_online_owner_changed(int is_owner);
void boss_battle_online_room_left(const char *why);
/* A seated guest got the owner's state; stale sequences are ignored. 1 when it applies. */
int boss_battle_online_internal_update(const uint8_t *data, size_t size);

/* Logs where the player stands after the Network Menu: a Boss Battle room, or none yet. */
void boss_battle_online_report_room(void);
int boss_battle_online_in_room(void);
int boss_battle_online_is_host(void);
int boss_battle_online_is_guest(void);
int boss_battle_online_members(void);
int boss_battle_online_slot(void);
int boss_battle_online_boss_room(void);

/* Whether the friend with this Online ID is in a Boss Battle room taking players, as the lobby's room search last said.
 * The runtime's Matching2 sets the lookup; none answers no. */
typedef int (*BossBattleFriendRoomFn)(const char *online_id);
void boss_battle_online_set_friend_room_lookup(BossBattleFriendRoomFn lookup);
int boss_battle_online_friend_in_boss_room(const char *online_id);
/* The host's state: this host's own, or the one a guest last received. */
BossBattleHostState boss_battle_online_host_state(void);

/* Only a host (or a player alone) chooses a boss; a guest waits for the host. */
int boss_battle_online_may_start(void);
/* A host confirmed a boss or Fight All: the room hears it before the arena is requested. */
int boss_battle_online_host_start(int boss, int fight_all);
/* Once, when a player of the host's start has left the room while the host still travels to the boss. */
int boss_battle_online_take_start_peer_lost(void);
/* The state the host's room must carry, and its version (it changes with every new state). */
uint32_t boss_battle_online_publish_version(void);
int boss_battle_online_publish(uint8_t out[BOSS_BATTLE_STATE_SIZE]);
/* The lobby answered the publish of `version`. */
void boss_battle_online_published(uint32_t version, int ok);
/* The host may request the arena: the start reached the room, or nobody else is in it. -1 when the start failed. */
int boss_battle_online_start_ready(void);
/* The start did not happen: the room hears that the host waits again. */
void boss_battle_online_start_abandoned(void);

/* The local fight as Boss Battle runs it. */
typedef enum {
    BOSS_BATTLE_LOCAL_IDLE,
    BOSS_BATTLE_LOCAL_TRAVELLING,
    BOSS_BATTLE_LOCAL_FIGHTING,
    BOSS_BATTLE_LOCAL_CHECKPOINT,
    BOSS_BATTLE_LOCAL_RETURNING
} BossBattleLocal;
void boss_battle_online_note_local(BossBattleLocal local, int boss, int fight_all);

/* A guest's next fight to follow: 1 once per host start, with its boss and whether it is Fight All. */
int boss_battle_online_take_follow(int *boss, int *fight_all);
/* A guest followed a start it could not take. */
void boss_battle_online_follow_failed(const char *why);

#ifdef __cplusplus
}
#endif

#endif
