/* Early Story Online, the session rules: who follows whose story, and which rooms and joins are allowed. Runtime state
 * only; nothing here reads or writes the game's save. The native hooks (early_story_online.h) and the Matching2 layer
 * (runtime_matching2.cpp) both use it. */
#ifndef DCP_EARLY_STORY_SESSION_H
#define DCP_EARLY_STORY_SESSION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EARLY_STORY_NO_STEP (-1)
#define EARLY_STORY_ORIGINAL_STEP 0x2d  /* the step from which the game itself offers online play */
#define EARLY_STORY_ROOM_ATTR 0x7f01    /* private room int attribute: the host's story step + 1; 0 = not a story session */
#define EARLY_STORY_ROOM_HIDDEN 0x10000000u /* the lobby's FLAG_HIDDEN */
/* Private room internal binary attribute: the owner's live story state. The game's own internal attribute is 0x57. */
#define EARLY_STORY_STATE_ATTR 0x7f02
#define EARLY_STORY_STATE_SIZE 2
#define EARLY_STORY_STATE_FORMAT 1

#define EARLY_STORY_STAGE_ATTR 0x4d  /* the game's room attribute: the host's stage (9 = town) */
#define EARLY_STORY_PACKED_ATTR 0x53 /* the game's packed room settings; bits 2-4 are the difficulty */
#define EARLY_STORY_LAST_EARLY_STAGE 8

typedef enum { EARLY_STORY_ROLE_NONE = 0, EARLY_STORY_ROLE_HOST, EARLY_STORY_ROLE_GUEST } EarlyStoryRole;

/* The native hooks are installed: this client can take part in a story session. Set once at startup, cleared by a
 * failed install. It does not depend on the Early Story Online setting. */
void early_story_set_active(int active);
int early_story_active(void);

/* The Early Story Online setting: this player may open online play and host a story session before step 45. Changing
 * it clears the access latch. */
void early_story_set_hosting(int enabled);
int early_story_hosting(void);

/* The runtime access latch: set once the host reaches controllable Hydeland, kept for the rest of the run, never saved.
 * Only a host with the setting on can hold it. */
void early_story_grant_access(void);
int early_story_access_ready(void);

/* Whether the online check passes for a lead record at `step`: the game's own step 45, or from the access latch, or
 * while the character is visiting a host's story. */
int early_story_online_unlocked(int step);

/* The room we are in: its role and the value of its story attribute (0 or less: a normal room). */
void early_story_session_joined(int is_owner, int room_attr);
/* A story session ends with its owner: everyone in the room gets their own story back. A normal room swaps the role. */
void early_story_session_owner_changed(int is_owner);
/* A guest seated in a story session room. */
int early_story_story_room_guest(void);
/* Leave, kick, disconnect, room closed, failed join, host gone: the next story read sees the real record. */
void early_story_session_reset(void);

/* The lead character's persistent step as the selector last saw it; the observer hears each change. */
void early_story_note_own_step(int step);
int early_story_own_step(void);
typedef void (*EarlyStoryStepObserver)(int step);
void early_story_session_observe_own_step(EarlyStoryStepObserver observer);
/* The host's step from the game's replicated host record, read at stage setup. */
void early_story_note_host_step(int step);
/* A seated guest of a story room got the owner's live state (EARLY_STORY_STATE_ATTR). Updates arrive in the order the
 * owner made them, so the latest wins whatever its value. 1 when it applies. */
int early_story_session_internal_update(int step);
/* The step the session follows: the owner's latest live state; before one, the newer of the room's story attribute at
 * the join and the replicated host record. EARLY_STORY_NO_STEP before any. */
int early_story_session_host_step(void);

void early_story_state_encode(int step, uint8_t out[EARLY_STORY_STATE_SIZE]);
/* The step in a live state attribute, or EARLY_STORY_NO_STEP. */
int early_story_state_decode(const uint8_t *data, size_t size);

/* Different stories, at least one of them still in the opening. Two characters past it keep today's online play. */
int early_story_context_differs(int own_step, int host_step);
/* A guest in a story session room, for as long as it is seated there whatever the two steps are, or a guest in another
 * room whose real step `own_step` differs from the host's: the shadow record and the blocked writers apply. */
int early_story_visiting_for(int own_step);
int early_story_visiting(void);

/* Whether a character at `own_step` hosts a story session room, and what the host publishes. */
int early_story_hosts_story_room(int own_step);
int early_story_room_attr_value(int own_step);
uint32_t early_story_room_flags(uint32_t flags, int own_step);

/* A guest joining a room: `room_host_step` is the room's story attribute (0 or less: a normal room), `stage` and
 * `packed` the game's own room attributes (EARLY_STORY_NO_STEP when absent). 1 to join; 0 with a short reason. */
int early_story_join_allowed(int own_step, int room_host_step, int stage, int packed, const char **reason);

int early_story_difficulty_of(int packed);

#ifdef __cplusplus
}
#endif

#endif
