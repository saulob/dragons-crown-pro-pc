#include "early_story_session.h"

#include <stdatomic.h>

static atomic_int active_flag;
static atomic_int hosting_flag;
static atomic_int access_flag;
static atomic_int role = EARLY_STORY_ROLE_NONE;
static atomic_int room_host_step = EARLY_STORY_NO_STEP;
static atomic_int native_host_step = EARLY_STORY_NO_STEP;
static atomic_int live_host_step = EARLY_STORY_NO_STEP;
static atomic_int own_step_seen = EARLY_STORY_NO_STEP;
static _Atomic(EarlyStoryStepObserver) own_step_observer;

void early_story_set_active(int active) {
    atomic_store(&active_flag, active != 0);
    atomic_store(&access_flag, 0);
    if (!active) early_story_session_reset();
}

int early_story_active(void) {
    return atomic_load(&active_flag);
}

void early_story_set_hosting(int enabled) {
    atomic_store(&hosting_flag, enabled != 0);
    atomic_store(&access_flag, 0);
}

int early_story_hosting(void) {
    return atomic_load(&hosting_flag);
}

void early_story_grant_access(void) {
    if (atomic_load(&active_flag) && atomic_load(&hosting_flag)) atomic_store(&access_flag, 1);
}

int early_story_access_ready(void) {
    return atomic_load(&access_flag);
}

int early_story_online_unlocked(int step) {
    if (step > EARLY_STORY_ORIGINAL_STEP - 1) return 1;
    return atomic_load(&active_flag) && (atomic_load(&access_flag) || early_story_visiting());
}

void early_story_session_joined(int is_owner, int room_attr) {
    atomic_store(&native_host_step, EARLY_STORY_NO_STEP);
    atomic_store(&live_host_step, EARLY_STORY_NO_STEP);
    atomic_store(&room_host_step, room_attr > 0 ? room_attr - 1 : EARLY_STORY_NO_STEP);
    atomic_store(&role, is_owner ? EARLY_STORY_ROLE_HOST : EARLY_STORY_ROLE_GUEST);
}

void early_story_session_owner_changed(int is_owner) {
    if (atomic_load(&role) == EARLY_STORY_ROLE_NONE) return;
    if (atomic_load(&room_host_step) != EARLY_STORY_NO_STEP) {
        early_story_session_reset();
        return;
    }
    atomic_store(&role, is_owner ? EARLY_STORY_ROLE_HOST : EARLY_STORY_ROLE_GUEST);
}

int early_story_story_room_guest(void) {
    return atomic_load(&active_flag) && atomic_load(&role) == EARLY_STORY_ROLE_GUEST &&
           atomic_load(&room_host_step) != EARLY_STORY_NO_STEP;
}

void early_story_session_reset(void) {
    atomic_store(&role, EARLY_STORY_ROLE_NONE);
    atomic_store(&room_host_step, EARLY_STORY_NO_STEP);
    atomic_store(&native_host_step, EARLY_STORY_NO_STEP);
    atomic_store(&live_host_step, EARLY_STORY_NO_STEP);
}

void early_story_note_own_step(int step) {
    if (atomic_exchange(&own_step_seen, step) == step) return;
    const EarlyStoryStepObserver observer = atomic_load(&own_step_observer);
    if (observer) observer(step);
}

int early_story_own_step(void) {
    return atomic_load(&own_step_seen);
}

void early_story_session_observe_own_step(EarlyStoryStepObserver observer) {
    atomic_store(&own_step_observer, observer);
}

void early_story_note_host_step(int step) {
    atomic_store(&native_host_step, step);
}

int early_story_session_internal_update(int step) {
    if (step < 0 || atomic_load(&role) != EARLY_STORY_ROLE_GUEST || atomic_load(&room_host_step) == EARLY_STORY_NO_STEP)
        return 0;
    atomic_store(&live_host_step, step);
    return 1;
}

int early_story_session_host_step(void) {
    const int live = atomic_load(&live_host_step);
    if (live != EARLY_STORY_NO_STEP) return live;
    const int native = atomic_load(&native_host_step), room = atomic_load(&room_host_step);
    return native > room ? native : room;
}

void early_story_state_encode(int step, uint8_t out[EARLY_STORY_STATE_SIZE]) {
    out[0] = EARLY_STORY_STATE_FORMAT;
    out[1] = (uint8_t)step;
}

int early_story_state_decode(const uint8_t *data, size_t size) {
    if (!data || size < EARLY_STORY_STATE_SIZE || data[0] != EARLY_STORY_STATE_FORMAT) return EARLY_STORY_NO_STEP;
    return data[1];
}

int early_story_context_differs(int own_step, int host_step) {
    if (own_step < 0 || host_step < 0 || own_step == host_step) return 0;
    return own_step < EARLY_STORY_ORIGINAL_STEP || host_step < EARLY_STORY_ORIGINAL_STEP;
}

int early_story_visiting_for(int own_step) {
    if (!atomic_load(&active_flag) || atomic_load(&role) != EARLY_STORY_ROLE_GUEST) return 0;
    if (atomic_load(&room_host_step) != EARLY_STORY_NO_STEP) return 1;
    return early_story_context_differs(own_step, early_story_session_host_step());
}

int early_story_visiting(void) {
    return early_story_visiting_for(atomic_load(&own_step_seen));
}

int early_story_hosts_story_room(int own_step) {
    return atomic_load(&active_flag) && atomic_load(&access_flag) && own_step >= 0 && own_step < EARLY_STORY_ORIGINAL_STEP;
}

int early_story_room_attr_value(int own_step) {
    return early_story_hosts_story_room(own_step) ? own_step + 1 : 0;
}

uint32_t early_story_room_flags(uint32_t flags, int own_step) {
    return early_story_hosts_story_room(own_step) ? flags | EARLY_STORY_ROOM_HIDDEN : flags;
}

int early_story_difficulty_of(int packed) {
    return packed < 0 ? EARLY_STORY_NO_STEP : (packed >> 2) & 7;
}

int early_story_join_allowed(int own_step, int host_step, int stage, int packed, const char **reason) {
    const char *unused;
    if (!reason) reason = &unused;
    *reason = "";
    const int story_room = host_step > 0;
    if (!atomic_load(&active_flag)) {
        if (story_room) *reason = "the room is an Early Story Online session";
        return !story_room;
    }
    if (story_room || own_step < 0 || own_step >= EARLY_STORY_ORIGINAL_STEP) return 1;
    if (stage < 0 || stage > EARLY_STORY_LAST_EARLY_STAGE) {
        *reason = "the host is not in a story stage 0-8";
        return 0;
    }
    if (early_story_difficulty_of(packed) != 0) {
        *reason = "the host is not on Normal difficulty";
        return 0;
    }
    return 1;
}
