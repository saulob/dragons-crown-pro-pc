/* Focused tests of Early Story Online's session rules and native handlers: who follows whose story, the shadow record,
 * the blocked progression writes, the join policy and the reset paths. The hooks on the real images are in
 * feature_sites_tests.cpp. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#define dup _dup
#define dup2 _dup2
#define close _close
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "early_story_online.h"

static int failures = 0;
#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            printf("FAIL line %d: %s\n", __LINE__, #cond);       \
            ++failures;                                          \
        }                                                        \
    } while (0)

enum { kSaveBytes = 0xa8 + 4 * EARLY_STORY_RECORD_SIZE };
static uint8_t save[kSaveBytes];

static uint8_t *record(int slot) {
    return save + 0xa8 + (size_t)slot * EARLY_STORY_RECORD_SIZE;
}

static void fill_save(int own_step) {
    for (size_t i = 0; i < sizeof(save); ++i) save[i] = (uint8_t)(i * 7 + 3);
    record(0)[EARLY_STORY_RECORD_STEP] = (uint8_t)own_step;
}

static GuestRegs selector_regs(int slot) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rax = (uint64_t)(uintptr_t)save;
    regs.rcx = (uint64_t)slot * EARLY_STORY_RECORD_SIZE;
    return regs;
}

static const uint8_t *selected(GuestRegs *regs, int slot) {
    *regs = selector_regs(slot);
    early_story_selector_hook(regs);
    return regs->skip ? (const uint8_t *)(uintptr_t)regs->rax : record(slot);
}

static void join_room(int is_owner, int host_step) {
    early_story_session_joined(is_owner, host_step < 0 ? 0 : host_step + 1);
}

static void start(int own_step, int is_owner, int room_step) {
    early_story_set_active(1);
    early_story_set_hosting(1);
    early_story_session_reset();
    GuestRegs leftover = selector_regs(0);
    early_story_selector_hook(&leftover);
    early_story_note_own_step(EARLY_STORY_NO_STEP);
    fill_save(own_step);
    join_room(is_owner, room_step);
}

static const uint8_t *GUEST_HOOK_ABI fake_select(uint64_t facade) {
    (void)facade;
    GuestRegs regs;
    return selected(&regs, 0);
}

static void test_context_rules(void) {
    CHECK(!early_story_context_differs(5, 5) && !early_story_context_differs(45, 45));
    CHECK(early_story_context_differs(45, 5) && early_story_context_differs(5, 45) && early_story_context_differs(44, 6));
    CHECK(!early_story_context_differs(45, 46) && !early_story_context_differs(50, 45));
    CHECK(!early_story_context_differs(-1, 5) && !early_story_context_differs(5, -1));
    CHECK(early_story_difficulty_of(0) == 0 && early_story_difficulty_of(2 << 2) == 2 && early_story_difficulty_of(4 << 2 | 3) == 4);
    CHECK(early_story_difficulty_of(-1) == EARLY_STORY_NO_STEP);
}

static void test_threshold_and_rooms(void) {
    CHECK(EARLY_STORY_ORIGINAL_STEP == 0x2d);
    early_story_set_active(0);
    early_story_set_hosting(1);
    early_story_grant_access();
    CHECK(!early_story_access_ready() && !early_story_hosts_story_room(10) && early_story_room_flags(0x10, 10) == 0x10);
    early_story_set_active(1);
    early_story_set_hosting(1);
    for (int step = -1; step <= 60; ++step) CHECK(!early_story_hosts_story_room(step));

    /* with the access latch every early step hosts a story room, the opening's step 0 included */
    early_story_grant_access();
    CHECK(early_story_access_ready());
    CHECK(early_story_hosts_story_room(0) && early_story_hosts_story_room(3) && early_story_hosts_story_room(4) &&
          early_story_hosts_story_room(44));
    CHECK(!early_story_hosts_story_room(45) && !early_story_hosts_story_room(60) && !early_story_hosts_story_room(-1));
    CHECK(early_story_room_flags(0x4, 0) == (0x4 | EARLY_STORY_ROOM_HIDDEN) && early_story_room_flags(0x4, 30) == (0x4 | EARLY_STORY_ROOM_HIDDEN));
    CHECK(early_story_room_flags(0x4, 45) == 0x4 && early_story_room_flags(0x4, -1) == 0x4);
    CHECK(early_story_room_attr_value(0) == 1 && early_story_room_attr_value(30) == 31 && early_story_room_attr_value(45) == 0 &&
          early_story_room_attr_value(-1) == 0);
    CHECK(EARLY_STORY_ROOM_HIDDEN == 0x10000000u && EARLY_STORY_ROOM_ATTR == 0x7f01);

    /* a guest reads the attribute back as the host's step, step 0 included */
    early_story_session_joined(0, early_story_room_attr_value(0));
    CHECK(early_story_session_host_step() == 0);
    early_story_session_joined(0, early_story_room_attr_value(30));
    CHECK(early_story_session_host_step() == 30);
    early_story_session_joined(0, 0);
    CHECK(early_story_session_host_step() == EARLY_STORY_NO_STEP);
}

/* The setting decides only who hosts: a capable client with it off opens no early room and publishes no attribute. */
static void test_host_setting(void) {
    early_story_set_active(1);
    early_story_set_hosting(0);
    early_story_grant_access();
    CHECK(early_story_active() && !early_story_hosting() && !early_story_access_ready());
    for (int step = 0; step <= 60; ++step) {
        CHECK(!early_story_hosts_story_room(step) && early_story_room_attr_value(step) == 0);
        CHECK(early_story_room_flags(0x4, step) == 0x4);
    }
    early_story_set_hosting(1);
    CHECK(early_story_hosting() && !early_story_access_ready() && !early_story_hosts_story_room(4));
    early_story_grant_access();
    CHECK(early_story_hosts_story_room(0) && early_story_hosts_story_room(44));
    CHECK(early_story_room_flags(0x4, 4) == (0x4 | EARLY_STORY_ROOM_HIDDEN) && early_story_room_attr_value(4) == 5);
    early_story_set_hosting(0);
    CHECK(!early_story_access_ready() && !early_story_hosts_story_room(4));
    early_story_set_hosting(1);
    early_story_grant_access();
    early_story_set_active(0);
    CHECK(!early_story_access_ready() && !early_story_hosts_story_room(4) && !early_story_online_unlocked(4));
    early_story_set_active(1);
}

static int online_answer(int step) {
    uint8_t record[EARLY_STORY_RECORD_SIZE];
    memset(record, 0, sizeof(record));
    record[EARLY_STORY_RECORD_STEP] = (uint8_t)step;
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rax = 77;
    regs.rdi = (uint64_t)(uintptr_t)record;
    early_story_online_check_hook(&regs);
    CHECK(regs.skip == 1 && (regs.rax == 0 || regs.rax == 1));
    return (int)regs.rax;
}

static void test_online_check(void) {
    GuestRegs regs;
    early_story_set_active(1);
    early_story_session_reset();
    early_story_set_hosting(0);
    for (int step = 0; step <= 44; ++step) CHECK(!online_answer(step));
    CHECK(online_answer(45) && online_answer(46) && online_answer(60) && online_answer(255));

    /* the setting alone changes nothing: the latch does */
    early_story_set_hosting(1);
    for (int step = 0; step <= 44; ++step) CHECK(!online_answer(step));
    early_story_grant_access();
    for (int step = 0; step <= 60; ++step) CHECK(online_answer(step));

    early_story_set_active(0);
    CHECK(!online_answer(0) && !online_answer(44) && online_answer(45));
    early_story_set_active(1);

    memset(&regs, 0, sizeof(regs));
    regs.rax = 5;
    early_story_online_check_hook(&regs);
    CHECK(regs.skip == 1 && regs.rax == 0);

    /* a visitor is online at the host's step even with the setting off, and only while visiting */
    early_story_set_hosting(0);
    start(45, 0, 4);
    early_story_set_hosting(0);
    selected(&regs, 0);
    CHECK(early_story_visiting() && online_answer(4) && online_answer(5) && online_answer(0));
    early_story_session_reset();
    CHECK(!early_story_visiting() && !online_answer(4) && online_answer(45));

    /* an early character off the setting is not unlocked by someone else's room until it is really visiting */
    start(10, 0, EARLY_STORY_NO_STEP);
    early_story_set_hosting(0);
    CHECK(!online_answer(10) && !online_answer(44));
    early_story_set_hosting(1);
}

/* A guest with the setting off still follows the host's story and keeps its own progress. */
static void test_guest_with_setting_off(void) {
    GuestRegs regs;
    start(45, 0, 4);
    early_story_set_hosting(0);
    CHECK(early_story_active());
    uint8_t before[kSaveBytes];
    memcpy(before, save, sizeof(save));
    const uint8_t *shadow = selected(&regs, 0);
    CHECK(early_story_visiting() && regs.skip == 1 && shadow[EARLY_STORY_RECORD_STEP] == 4);
    CHECK(!memcmp(save, before, sizeof(save)) && record(0)[EARLY_STORY_RECORD_STEP] == 45);
    early_story_note_host_step(4);
    shadow = selected(&regs, 0);
    CHECK(shadow[EARLY_STORY_RECORD_STEP] == 4);
    CHECK(early_story_join_allowed(45, 4, EARLY_STORY_NO_STEP, EARLY_STORY_NO_STEP, NULL));
    CHECK(early_story_join_allowed(45, 44, EARLY_STORY_NO_STEP, EARLY_STORY_NO_STEP, NULL));

    static const GuestHookHandler writers[] = {early_story_advance_story_hook, early_story_tier_hook,
                                               early_story_talisman_hook, early_story_flag_hook};
    for (size_t i = 0; i < sizeof(writers) / sizeof(*writers); ++i) {
        const uint32_t blocked = early_story_blocked_writes();
        memset(&regs, 0, sizeof(regs));
        regs.rax = 9;
        writers[i](&regs);
        CHECK(regs.skip == 1 && regs.rax == 0 && early_story_blocked_writes() == blocked + 1);
    }

    early_story_session_reset();
    CHECK(selected(&regs, 0) == record(0) && !regs.skip && !early_story_visiting());
    early_story_set_hosting(1);
}

static void test_visiting_detection(void) {
    GuestRegs regs;
    start(45, 0, 5);
    CHECK(early_story_visiting());
    selected(&regs, 0);
    CHECK(early_story_own_step() == 45 && early_story_visiting() && regs.skip == 1);

    start(5, 1, 5);
    selected(&regs, 0);
    CHECK(!early_story_visiting() && !regs.skip);

    /* a guest at the host's step is a visitor too: the whole membership follows the host */
    start(5, 0, 5);
    selected(&regs, 0);
    CHECK(early_story_visiting() && regs.skip == 1);

    start(46, 0, EARLY_STORY_NO_STEP);
    early_story_note_host_step(45);
    selected(&regs, 0);
    CHECK(!early_story_visiting() && !regs.skip);

    start(5, 0, EARLY_STORY_NO_STEP);
    selected(&regs, 0);
    CHECK(!early_story_visiting());
    early_story_note_host_step(46);
    selected(&regs, 0);
    CHECK(early_story_visiting());

    early_story_set_active(0);
    selected(&regs, 0);
    CHECK(!early_story_visiting() && !regs.skip);
}

static void test_shadow_record(void) {
    GuestRegs regs;
    start(45, 0, 5);
    uint8_t before[kSaveBytes];
    memcpy(before, save, sizeof(save));
    const uint8_t *shadow = selected(&regs, 0);
    CHECK(regs.skip == 1 && shadow != record(0) && shadow[EARLY_STORY_RECORD_STEP] == 5);
    CHECK(!memcmp(save, before, sizeof(save)) && record(0)[EARLY_STORY_RECORD_STEP] == 45);
    uint8_t expected[EARLY_STORY_RECORD_SIZE];
    memcpy(expected, record(0), sizeof(expected));
    expected[EARLY_STORY_RECORD_STEP] = 5;
    CHECK(!memcmp(shadow, expected, sizeof(expected)));

    /* a write through the shadow never reaches the save */
    uint8_t *writable = (uint8_t *)(uintptr_t)regs.rax;
    writable[EARLY_STORY_RECORD_STEP] = 99;
    writable[0x100] ^= 0xff;
    CHECK(!memcmp(save, before, sizeof(save)));

    /* other slots and a malformed offset are the game's own record */
    record(2)[EARLY_STORY_RECORD_STEP] = 12;
    selected(&regs, 2);
    CHECK(regs.skip == 1 && ((const uint8_t *)(uintptr_t)regs.rax)[EARLY_STORY_RECORD_STEP] == 5);
    regs = selector_regs(0);
    regs.rcx = 3;
    early_story_selector_hook(&regs);
    CHECK(!regs.skip);
    regs = selector_regs(4);
    early_story_selector_hook(&regs);
    CHECK(!regs.skip);

    /* the host's story changes between stages: the next read follows it */
    early_story_note_host_step(6);
    shadow = selected(&regs, 0);
    CHECK(shadow[EARLY_STORY_RECORD_STEP] == 6);
}

static void test_original_threshold_handler(void) {
    GuestRegs regs;
    early_story_online_set_selector(NULL);
    memset(&regs, 0, sizeof(regs));
    early_story_original_online_hook(&regs);
    CHECK(!regs.skip);

    early_story_online_set_selector(fake_select);
    start(10, 0, EARLY_STORY_NO_STEP);
    memset(&regs, 0, sizeof(regs));
    regs.rax = 77;
    early_story_original_online_hook(&regs);
    CHECK(regs.skip == 1 && regs.rax == 0);
    record(0)[EARLY_STORY_RECORD_STEP] = EARLY_STORY_ORIGINAL_STEP;
    memset(&regs, 0, sizeof(regs));
    early_story_original_online_hook(&regs);
    CHECK(regs.skip == 1 && regs.rax == 1);
    record(0)[EARLY_STORY_RECORD_STEP] = EARLY_STORY_ORIGINAL_CHECK;
    memset(&regs, 0, sizeof(regs));
    early_story_original_online_hook(&regs);
    CHECK(regs.rax == 0);
    /* with online access enabled, the step-45 notice and the hint still see a locked story */
    early_story_set_hosting(1);
    early_story_grant_access();
    for (int step = 0; step < EARLY_STORY_ORIGINAL_STEP; ++step) {
        record(0)[EARLY_STORY_RECORD_STEP] = (uint8_t)step;
        memset(&regs, 0, sizeof(regs));
        early_story_original_online_hook(&regs);
        CHECK(regs.skip == 1 && regs.rax == 0);
    }

    /* a visiting guest at step 45 still gets the real answer, not the host's step 5 */
    start(45, 0, 5);
    memset(&regs, 0, sizeof(regs));
    early_story_original_online_hook(&regs);
    CHECK(regs.skip == 1 && regs.rax == 1 && early_story_visiting());
    GuestRegs again;
    CHECK(selected(&again, 0)[EARLY_STORY_RECORD_STEP] == 5);

    /* and a visiting early guest at step 10 is not unlocked by the host's step 45 */
    start(10, 0, EARLY_STORY_NO_STEP);
    early_story_note_host_step(45);
    memset(&regs, 0, sizeof(regs));
    early_story_original_online_hook(&regs);
    CHECK(regs.rax == 0);
    early_story_online_set_selector(NULL);
}

/* The Hydeland activation, against a fake of the game's objects and of the original prompt function's state machine:
 * state 0 checks the online check, the run flag (manager + 0x14d) and the network, sets the flag and moves on; state 1
 * is the Yes/No dialog; state 2 the sign-in; state 3 the portrait hint. Ineligible, it is finished at once. */
enum { kFakeVtable = 0x5150, kTownBytes = 0x1000 };
static uint8_t fake_town[kTownBytes], fake_second_town[kTownBytes], fake_context[0x80], fake_manager[0x200];
static uint8_t fake_mode[0x600], fake_scene[0x200], fake_queue[0x100], fake_gate_owner[EARLY_STORY_OWNER_SIZE];
static uint32_t fake_mask_table[4];
static uint8_t *town_in_use = fake_town;
static int fake_online, fake_dialogs, fake_signins, fake_calls, fake_answer_yes, fake_dialog_frames;

static uint8_t GUEST_HOOK_ABI fake_prompt(uint64_t address, int slot);

static void put_pointer(uint8_t *where, const void *target) {
    const uint64_t value = (uint64_t)(uintptr_t)target;
    memcpy(where, &value, sizeof(value));
}

static void fake_control(int accepts) {
    const uint32_t input = accepts ? 1 : 0;
    fake_mode[EARLY_STORY_OBJECT_STATE] = EARLY_STORY_MODE_TOWN;
    memcpy(fake_mode + EARLY_STORY_MODE_INPUT, &input, sizeof(input));
    fake_scene[EARLY_STORY_OBJECT_STATE] = EARLY_STORY_SCENE_PLAY;
    fake_queue[EARLY_STORY_OBJECT_STATE] = 0;
    memset(fake_queue + EARLY_STORY_QUEUE_BEGIN, 0, 16);
}

static void fake_world(int step, int answer_yes) {
    memset(fake_town, 0, sizeof(fake_town));
    memset(fake_context, 0, sizeof(fake_context));
    memset(fake_manager, 0, sizeof(fake_manager));
    memset(fake_mode, 0, sizeof(fake_mode));
    memset(fake_scene, 0, sizeof(fake_scene));
    memset(fake_queue, 0, sizeof(fake_queue));
    memset(fake_gate_owner, 0, sizeof(fake_gate_owner));
    fake_mask_table[1] = 1;
    put_pointer(fake_town + EARLY_STORY_TOWN_CONTEXT, fake_context);
    put_pointer(fake_context + EARLY_STORY_CONTEXT_MANAGER, fake_manager);
    put_pointer(fake_manager + EARLY_STORY_MANAGER_MODE, fake_mode);
    put_pointer(fake_manager + EARLY_STORY_MANAGER_SCENE, fake_scene);
    put_pointer(fake_manager + EARLY_STORY_MANAGER_QUEUE, fake_queue);
    fake_control(1);
    fake_online = fake_dialogs = fake_signins = fake_calls = fake_dialog_frames = 0;
    fake_answer_yes = answer_yes;
    town_in_use = fake_town;
    start(step, 0, EARLY_STORY_NO_STEP);
    early_story_online_set_selector(fake_select);
    early_story_online_set_prompt(fake_prompt, kFakeVtable, (uint64_t)(uintptr_t)fake_mask_table);
}

static uint8_t GUEST_HOOK_ABI fake_prompt(uint64_t address, int slot) {
    uint8_t *owner = (uint8_t *)(uintptr_t)address;
    uint8_t *state = owner + EARLY_STORY_OWNER_STATES + slot;
    ++fake_calls;
    CHECK(slot == EARLY_STORY_PROMPT_SLOT);
    switch (*state) {
    case 0:
        if (owner != fake_gate_owner) {
            uint64_t vtable, system_object, town = (uint64_t)(uintptr_t)town_in_use;
            uint32_t flags;
            memcpy(&vtable, owner, sizeof(vtable));
            memcpy(&system_object, owner + EARLY_STORY_OWNER_SYSOBJ, sizeof(system_object));
            memcpy(&flags, owner + EARLY_STORY_OWNER_FLAGS, sizeof(flags));
            CHECK(vtable == kFakeVtable && system_object == town && flags == EARLY_STORY_SLOT1_CHANGED);
        }
        if (!early_story_online_unlocked(record(0)[EARLY_STORY_RECORD_STEP])) return 1;
        if (fake_online || fake_manager[EARLY_STORY_MANAGER_PROMPT_SHOWN]) return 1;
        fake_manager[EARLY_STORY_MANAGER_PROMPT_SHOWN] = 1;
        *state = 1;
        return 0;
    case 1:
        if (!fake_dialogs) ++fake_dialogs;
        if (++fake_dialog_frames < 3) return 0;
        if (!fake_answer_yes) return 1;
        *state = 2;
        return 0;
    case 2:
        ++fake_signins;
        fake_online = 1;
        *state = 3;
        return 0;
    default:
        return 1;
    }
}

/* The original Castle Gate: its logic object runs the same function from its first frame until it is finished. */
static int fake_gate_visit(void) {
    memset(fake_gate_owner, 0, sizeof(fake_gate_owner));
    const int dialogs = fake_dialogs;
    for (int frame = 0; frame < 20; ++frame)
        if (fake_prompt((uint64_t)(uintptr_t)fake_gate_owner, EARLY_STORY_PROMPT_SLOT)) return fake_dialogs - dialogs;
    return -1;
}

/* One frame of the town update: the call that runs every frame. */
static void town_frame(void) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rdi = (uint64_t)(uintptr_t)town_in_use;
    early_story_town_portal_hook(&regs);
    CHECK(!regs.skip);
}

static void town_frames(int count) {
    for (int i = 0; i < count; ++i) town_frame();
}

/* What the Network Status line and the Network Menu read: the online wrapper's check on the lead character. */
static int network_ui_available(void) {
    return online_answer(record(0)[EARLY_STORY_RECORD_STEP]);
}

static void test_hydeland_activation(void) {
    /* Off: nothing changes, whatever the step or the town */
    for (int step = 0; step <= 60; ++step) {
        fake_world(step, 1);
        early_story_set_hosting(0);
        town_frames(30);
        CHECK(fake_calls == 0 && !early_story_access_ready() && !early_story_town_prompt_open());
        CHECK(network_ui_available() == (step >= EARLY_STORY_ORIGINAL_STEP));
    }

    /* On: the real post-Inn state is step 0, and no step is required: every step is offered once */
    for (int step = 0; step <= 60; ++step) {
        fake_world(step, 1);
        town_frames(30);
        CHECK(early_story_access_ready() && fake_dialogs == 1 && fake_online && !early_story_town_prompt_open());
        CHECK(fake_manager[EARLY_STORY_MANAGER_PROMPT_SHOWN] == 1 && network_ui_available());
    }

    /* nothing before the town accepts input, then the first controllable frame opens it, without any player action */
    fake_world(0, 1);
    fake_control(0);
    town_frames(30);
    CHECK(fake_calls == 0 && !early_story_access_ready() && !network_ui_available());
    fake_control(1);
    town_frame();
    CHECK(early_story_access_ready() && early_story_town_prompt_open() && fake_calls == 1);

    /* each of the four checks keeps it away on its own */
    for (int check = 0; check < 4; ++check) {
        fake_world(0, 1);
        if (check == 0) fake_mode[EARLY_STORY_OBJECT_STATE] = 0;
        if (check == 1) memset(fake_mode + EARLY_STORY_MODE_INPUT, 0, 4);
        if (check == 2) fake_scene[EARLY_STORY_OBJECT_STATE] = 0;
        if (check == 3) fake_queue[EARLY_STORY_QUEUE_BEGIN] = 1;
        town_frames(10);
        CHECK(fake_calls == 0 && !early_story_access_ready());
    }

    /* an open prompt is polled even when the town stops accepting input: nothing can stall it */
    fake_world(0, 1);
    town_frame();
    CHECK(early_story_town_prompt_open());
    fake_control(0);
    town_frames(12);
    CHECK(!early_story_town_prompt_open() && fake_online && fake_signins == 1);

    /* Yes: online, and the Network Status and Network Menu stay available, in Hydeland and after leaving it */
    fake_world(0, 1);
    town_frames(30);
    CHECK(fake_online && network_ui_available());
    memcpy(fake_second_town, fake_town, sizeof(fake_second_town));
    town_in_use = fake_second_town;
    town_frames(30);
    CHECK(network_ui_available() && online_answer(0) && online_answer(3) && early_story_access_ready());
    CHECK(fake_dialogs == 1 && fake_signins == 1);

    /* No: offline, the menu is still available, and nothing asks again that run */
    fake_world(0, 0);
    town_frames(30);
    CHECK(fake_dialogs == 1 && !fake_online && fake_signins == 0 && network_ui_available() && early_story_access_ready());
    const int calls_after_no = fake_calls;
    town_frames(300);
    CHECK(fake_calls == calls_after_no && fake_dialogs == 1 && network_ui_available());
    CHECK(fake_gate_visit() == 0 && fake_dialogs == 1);

    /* returns from the Inn, the Guild and the shop: no second prompt, access stays */
    fake_world(0, 1);
    town_frames(30);
    for (int facility = 0; facility < 3; ++facility) {
        const int dialogs = fake_dialogs, calls = fake_calls;
        memcpy(fake_second_town, fake_town, sizeof(fake_second_town));
        fake_second_town[8] = (uint8_t)(facility + 1);
        town_in_use = fake_second_town;
        town_frames(40);
        town_in_use = fake_town;
        town_frames(40);
        CHECK(fake_dialogs == dialogs && fake_calls == calls && early_story_access_ready() && network_ui_available());
    }
    fake_online = 0;
    town_frames(40);
    CHECK(fake_dialogs == 1 && network_ui_available());

    /* the first story stage and the later ones see online as available */
    for (int step = 0; step <= 44; ++step) CHECK(online_answer(step));

    /* the Town Gate after Yes shows nothing; as a fallback it still asks when Hydeland did not */
    fake_world(0, 1);
    town_frames(30);
    fake_online = 1;
    CHECK(fake_gate_visit() == 0 && fake_dialogs == 1);
    fake_world(50, 1);
    fake_control(0);
    town_frames(30);
    CHECK(fake_calls == 0);
    CHECK(fake_gate_visit() == 1 && fake_online && fake_dialogs == 1);
    fake_online = 0;
    fake_control(1);
    town_frames(50);
    CHECK(fake_dialogs == 1 && early_story_access_ready());

    /* already online: finished at once, not tried again, access still granted */
    fake_world(20, 1);
    fake_online = 1;
    town_frames(50);
    CHECK(fake_calls == 1 && fake_dialogs == 0 && early_story_access_ready() && !fake_manager[EARLY_STORY_MANAGER_PROMPT_SHOWN]);

    /* a visiting guest, a guest with the setting off, an inactive feature, a missing manager, no prompt function */
    fake_world(45, 1);
    join_room(0, 0);
    {
        GuestRegs regs;
        selected(&regs, 0);
    }
    CHECK(early_story_visiting());
    town_frames(30);
    CHECK(fake_calls == 0 && !early_story_access_ready());
    fake_world(45, 1);
    early_story_set_hosting(0);
    town_frames(30);
    CHECK(fake_calls == 0 && !early_story_access_ready());
    fake_world(0, 1);
    early_story_set_active(0);
    town_frames(30);
    CHECK(fake_calls == 0 && !network_ui_available());
    fake_world(0, 1);
    memset(fake_town + EARLY_STORY_TOWN_CONTEXT, 0, 8);
    town_frames(30);
    CHECK(fake_calls == 0 && !early_story_access_ready());
    fake_world(0, 1);
    early_story_online_set_prompt(NULL, 0, 0);
    town_frames(30);
    CHECK(fake_calls == 0 && !early_story_access_ready());

    /* the town is replaced while the prompt is open: it is dropped, and the run flag keeps it from coming back */
    fake_world(0, 1);
    town_frame();
    CHECK(early_story_town_prompt_open());
    memcpy(fake_second_town, fake_town, sizeof(fake_second_town));
    town_in_use = fake_second_town;
    town_frame();
    CHECK(!early_story_town_prompt_open());
    const int dialogs_before = fake_dialogs;
    town_frames(30);
    CHECK(fake_dialogs == dialogs_before && early_story_access_ready());

    /* nothing of the game's is written: the save and its story step, the town, the manager but its run flag, the checks */
    fake_world(0, 1);
    uint8_t save_before[kSaveBytes], town_before[kTownBytes], manager_before[sizeof(fake_manager)];
    uint8_t mode_before[sizeof(fake_mode)], scene_before[sizeof(fake_scene)], queue_before[sizeof(fake_queue)];
    memcpy(save_before, save, sizeof(save));
    memcpy(town_before, fake_town, sizeof(fake_town));
    memcpy(manager_before, fake_manager, sizeof(fake_manager));
    memcpy(mode_before, fake_mode, sizeof(fake_mode));
    memcpy(scene_before, fake_scene, sizeof(fake_scene));
    memcpy(queue_before, fake_queue, sizeof(fake_queue));
    town_frames(40);
    CHECK(fake_dialogs == 1 && fake_online && early_story_access_ready());
    CHECK(!memcmp(save_before, save, sizeof(save)) && record(0)[EARLY_STORY_RECORD_STEP] == 0);
    CHECK(!memcmp(town_before, fake_town, sizeof(fake_town)) && !memcmp(mode_before, fake_mode, sizeof(fake_mode)));
    CHECK(!memcmp(scene_before, fake_scene, sizeof(fake_scene)) && !memcmp(queue_before, fake_queue, sizeof(fake_queue)));
    fake_manager[EARLY_STORY_MANAGER_PROMPT_SHOWN] = 0;
    CHECK(!memcmp(manager_before, fake_manager, sizeof(fake_manager)));

    /* the latch is runtime only: changing the setting or the feature's state clears it */
    early_story_set_hosting(1);
    CHECK(!early_story_access_ready());

    early_story_online_set_prompt(NULL, 0, 0);
    early_story_online_set_selector(NULL);
    early_story_set_hosting(1);
}

/* The Network Menu shortcut: the pad layer's claim on Triangle, and the three hooks on the scene director's Menu path. */
static uint64_t fake_now = 1000;
static uint64_t fake_clock(void) {
    return fake_now;
}
static uint8_t shortcut_window[0x1600];

static void fake_area(uint32_t type, uint32_t location) {
    memcpy(fake_scene + EARLY_STORY_AREA_TYPE, &type, sizeof(type));
    memcpy(fake_scene + EARLY_STORY_AREA_LOCATION, &location, sizeof(location));
}

static int32_t shortcut_kind(void) {
    int32_t kind;
    memcpy(&kind, shortcut_window + EARLY_STORY_MENU_KIND, sizeof(kind));
    return kind;
}

/* One director frame for a player slot with no menu window open: the press test, and when it said yes the step check
 * and the kind store the way the game's creation path runs them. Returns 1 when a Network Menu was created, 2 when the
 * game's own Menu press created the town menu, 0 when no menu was. */
static int director_frame(int slot, int menu_pressed, int step_check_answer) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.r13 = (uint64_t)slot;
    regs.rax = (uint64_t)menu_pressed;
    early_story_menu_press_hook(&regs);
    const int pressed = regs.skip ? (int)regs.rax : menu_pressed;
    if (regs.skip) CHECK(slot == 0 && regs.rax == 1);
    if (!pressed) return 0;
    memset(&regs, 0, sizeof(regs));
    regs.rax = (uint64_t)step_check_answer;
    early_story_menu_step_hook(&regs);
    const int created = regs.skip ? (int)regs.rax : step_check_answer;
    if (regs.skip) CHECK(regs.rax == 1);
    if (!created) return 0;
    const int32_t town_kind = 1;
    memcpy(shortcut_window + EARLY_STORY_MENU_KIND, &town_kind, sizeof(town_kind));
    memset(&regs, 0, sizeof(regs));
    regs.rbx = (uint64_t)(uintptr_t)shortcut_window;
    regs.r15 = 1;
    early_story_menu_kind_hook(&regs);
    CHECK(!regs.skip);
    return shortcut_kind() == EARLY_STORY_MENU_KIND_NETWORK ? 1 : 2;
}

/* A frame of Hydeland: the town update, the director's poll, then the pad read. */
static void shortcut_frame(void) {
    fake_now += 16;
    town_frame();
}

static int triangle(int down) {
    return early_story_shortcut_pad_filter(down);
}

/* Presses Triangle for a frame while the director polls; returns what the director then did. */
static int press_and_poll(int step_check_answer) {
    shortcut_frame();
    const int hidden = triangle(1);
    const int opened = director_frame(0, 0, step_check_answer);
    triangle(0);
    shortcut_frame();
    (void)hidden;
    return opened;
}

static void shortcut_world(int step, int answer_yes) {
    fake_world(step, answer_yes);
    early_story_shortcut_set_clock(fake_clock);
    fake_area(EARLY_STORY_AREA_TOWN, EARLY_STORY_AREA_HYDELAND);
    memset(shortcut_window, 0, sizeof(shortcut_window));
    fake_now += 1000;
    early_story_town_prompt_reset();
    triangle(0);
}

/* Settles the original prompt: frames until it is out of the way, then the director polls. */
static void settle_prompt(void) {
    for (int i = 0; i < 40; ++i) {
        shortcut_frame();
        director_frame(0, 0, 0);
    }
}

static void test_network_shortcut(void) {
    /* before the town accepts input, and with the prompt up, Triangle belongs to the game */
    shortcut_world(0, 0);
    fake_control(0);
    for (int i = 0; i < 5; ++i) {
        shortcut_frame();
        CHECK(triangle(1) == 0 && director_frame(0, 0, 0) == 0);
        triangle(0);
    }
    fake_control(1);
    shortcut_frame();
    CHECK(early_story_town_prompt_open());
    for (int i = 0; i < 2; ++i) {
        shortcut_frame();
        director_frame(0, 0, 0);
        CHECK(triangle(1) == 0);
        CHECK(director_frame(0, 0, 0) == 0);
        triangle(0);
    }

    /* No answer: Hydeland step 0, no character menu (the game's step check says no): Triangle opens the Network Menu */
    shortcut_world(0, 0);
    settle_prompt();
    CHECK(!fake_online && network_ui_available() && !early_story_town_prompt_open());
    CHECK(press_and_poll(0) == 1 && shortcut_kind() == EARLY_STORY_MENU_KIND_NETWORK);
    /* the director's menu path polled: closing the menu and pressing again works any number of times */
    CHECK(press_and_poll(0) == 1);
    CHECK(press_and_poll(0) == 1);

    /* Yes answer: the same, online */
    shortcut_world(0, 1);
    settle_prompt();
    CHECK(fake_online && network_ui_available());
    CHECK(press_and_poll(0) == 1);

    /* the game's own Menu press still goes through the game: the step check says no at step 0, the town menu at step 10 */
    shortcut_world(0, 0);
    settle_prompt();
    shortcut_frame();
    CHECK(director_frame(0, 1, 0) == 0);
    shortcut_world(10, 0);
    settle_prompt();
    shortcut_frame();
    CHECK(director_frame(0, 1, 1) == 2 && shortcut_kind() == 1);
    CHECK(director_frame(0, 0, 1) == 0);
    /* and the shortcut still opens the Network Menu once the character menu exists */
    CHECK(press_and_poll(1) == 1);
    CHECK(director_frame(0, 1, 1) == 2 && shortcut_kind() == 1);

    /* the press is the player's Triangle only: another slot's frame does not take it */
    shortcut_world(0, 0);
    settle_prompt();
    shortcut_frame();
    CHECK(triangle(1) == 1);
    CHECK(director_frame(1, 0, 0) == 0 && director_frame(2, 0, 0) == 0);
    CHECK(director_frame(0, 0, 0) == 1);
    CHECK(director_frame(0, 0, 0) == 0);
    triangle(0);

    /* a press goes stale when the director does not poll (a menu window is open, the scene is busy) */
    shortcut_world(0, 0);
    settle_prompt();
    shortcut_frame();
    CHECK(triangle(1) == 1);
    fake_now += EARLY_STORY_SHORTCUT_PRESS_MS + 10;
    shortcut_frame();
    CHECK(director_frame(0, 0, 0) == 0);
    triangle(0);

    /* while a menu window is open the director does not poll: the lease lapses and Triangle reaches the game again */
    shortcut_world(0, 0);
    settle_prompt();
    CHECK(press_and_poll(0) == 1);
    for (int i = 0; i < 12; ++i) shortcut_frame();
    CHECK(triangle(1) == 0);
    triangle(0);

    /* held through the leave: stays hidden until released, a button already held when armed is not a press */
    shortcut_world(0, 0);
    settle_prompt();
    shortcut_frame();
    CHECK(triangle(1) == 1);
    fake_area(EARLY_STORY_AREA_TOWN, 1);
    shortcut_frame();
    shortcut_frame();
    CHECK(triangle(1) == 1);
    CHECK(triangle(0) == 0 && triangle(1) == 0);
    triangle(0);

    /* outside Hydeland the shortcut is off: the Inn, the Guild and the shop (other locations), a stage, the title */
    for (uint32_t location = 1; location <= 5; ++location) {
        shortcut_world(0, 0);
        settle_prompt();
        fake_area(EARLY_STORY_AREA_TOWN, location);
        shortcut_frame();
        CHECK(triangle(1) == 0 && director_frame(0, 0, 0) == 0);
        triangle(0);
    }
    for (uint32_t type = 0; type < 20; ++type) {
        if (type == EARLY_STORY_AREA_TOWN) continue;
        shortcut_world(0, 0);
        settle_prompt();
        fake_area(type, EARLY_STORY_AREA_HYDELAND);
        shortcut_frame();
        CHECK(triangle(1) == 0 && director_frame(0, 0, 0) == 0);
        triangle(0);
    }
    /* and back in Hydeland it works again at once */
    shortcut_world(0, 0);
    settle_prompt();
    fake_area(EARLY_STORY_AREA_TOWN, 1);
    CHECK(press_and_poll(0) == 0);
    fake_area(EARLY_STORY_AREA_TOWN, EARLY_STORY_AREA_HYDELAND);
    CHECK(press_and_poll(0) == 1);

    /* a visiting guest, a guest with the setting off, the feature inactive, access not granted: untouched */
    shortcut_world(45, 1);
    join_room(0, 0);
    {
        GuestRegs lead;
        selected(&lead, 0);
    }
    CHECK(early_story_visiting());
    settle_prompt();
    shortcut_frame();
    CHECK(triangle(1) == 0 && director_frame(0, 0, 1) == 0);
    triangle(0);
    shortcut_world(0, 1);
    early_story_set_hosting(0);
    settle_prompt();
    shortcut_frame();
    CHECK(triangle(1) == 0 && director_frame(0, 0, 0) == 0);
    triangle(0);
    shortcut_world(0, 1);
    early_story_set_active(0);
    settle_prompt();
    shortcut_frame();
    CHECK(triangle(1) == 0 && director_frame(0, 0, 0) == 0);
    triangle(0);
    shortcut_world(0, 1);
    memset(fake_town + EARLY_STORY_TOWN_CONTEXT, 0, 8);
    settle_prompt();
    shortcut_frame();
    CHECK(triangle(1) == 0 && director_frame(0, 0, 0) == 0);
    triangle(0);

    /* a held Triangle opens the menu once, however long it is held, and a new press opens it again */
    shortcut_world(0, 0);
    settle_prompt();
    shortcut_frame();
    int opened = 0;
    for (int frame = 0; frame < 30; ++frame) {
        shortcut_frame();
        CHECK(triangle(1) == 1);
        opened += director_frame(0, 0, 0) == 1;
    }
    CHECK(opened == 1);
    triangle(0);
    CHECK(press_and_poll(0) == 1);

    /* with Online Play off the hooks are not installed: nothing is ever armed and Triangle is never held back */
    early_story_town_prompt_reset();
    for (int frame = 0; frame < 10; ++frame) {
        fake_now += 16;
        CHECK(triangle(1) == 0);
    }
    triangle(0);

    /* the kind store does nothing without a request, and a request is spent when the director created no window */
    shortcut_world(0, 0);
    settle_prompt();
    {
        GuestRegs regs;
        const int32_t town_kind = 1;
        memcpy(shortcut_window + EARLY_STORY_MENU_KIND, &town_kind, sizeof(town_kind));
        memset(&regs, 0, sizeof(regs));
        regs.rbx = (uint64_t)(uintptr_t)shortcut_window;
        regs.r15 = 1;
        early_story_menu_kind_hook(&regs);
        CHECK(shortcut_kind() == 1);
        shortcut_frame();
        CHECK(triangle(1) == 1);
        GuestRegs press;
        memset(&press, 0, sizeof(press));
        early_story_menu_press_hook(&press);
        CHECK(press.skip == 1);
        regs.r15 = 0;
        early_story_menu_kind_hook(&regs);
        CHECK(shortcut_kind() == 1);
        regs.r15 = 1;
        early_story_menu_kind_hook(&regs);
        CHECK(shortcut_kind() == 1);
        triangle(0);
    }

    early_story_shortcut_set_clock(NULL);
    early_story_online_set_prompt(NULL, 0, 0);
    early_story_online_set_selector(NULL);
    early_story_set_hosting(1);
}

static uint8_t fake_ui[0x1000], fake_hint_object[0x800];
static uint64_t fake_hint_vtable[0x40];
static int hint_creates, hint_shows, hint_hides, hint_refreshes, hint_message;
static char hint_text[96];

static uint8_t *hint_slot(void) {
    return fake_ui + EARLY_STORY_UI_WINDOWS + (size_t)EARLY_STORY_HINT_WINDOW_TYPE * EARLY_STORY_UI_SLOT_SIZE;
}

static uint64_t GUEST_HOOK_ABI fake_hint_create(uint64_t ui, int init) {
    CHECK(ui == (uint64_t)(uintptr_t)fake_ui && init == 0);
    ++hint_creates;
    put_pointer(hint_slot(), fake_hint_object);
    put_pointer(fake_hint_object, fake_hint_vtable);
    return (uint64_t)(uintptr_t)fake_hint_object;
}

static void GUEST_HOOK_ABI fake_hint_text(uint64_t window, int message, const char *text) {
    snprintf(hint_text, sizeof(hint_text), "%s", text);
    CHECK(window == (uint64_t)(uintptr_t)fake_hint_object);
    hint_message = message;
}

static void GUEST_HOOK_ABI fake_hint_refresh(uint64_t object) {
    CHECK(object == (uint64_t)(uintptr_t)fake_hint_object + EARLY_STORY_HINT_TEXT_OBJECT);
    ++hint_refreshes;
}

static void GUEST_HOOK_ABI fake_hint_hide(uint64_t window) {
    CHECK(window == (uint64_t)(uintptr_t)fake_hint_object);
    ++hint_hides;
}

static void GUEST_HOOK_ABI fake_hint_show(uint64_t window, int duration, int flag) {
    CHECK(window == (uint64_t)(uintptr_t)fake_hint_object && duration == -1 && flag == 0);
    ++hint_shows;
}

static void hint_world(int step, int answer_yes, int verify_show) {
    shortcut_world(step, answer_yes);
    memset(fake_ui, 0, sizeof(fake_ui));
    memset(fake_hint_object, 0, sizeof(fake_hint_object));
    fake_hint_vtable[EARLY_STORY_HINT_SHOW_SLOT / 8] = (uint64_t)(uintptr_t)fake_hint_show;
    put_pointer(fake_manager + EARLY_STORY_MANAGER_UI, fake_ui);
    const EarlyStoryHintFunctions functions = {fake_hint_create, (EarlyStoryHintTextFn)(uintptr_t)fake_hint_text, fake_hint_hide,
                                               fake_hint_refresh, verify_show};
    early_story_hint_configure(&functions);
    early_story_hint_set_portuguese(0);
    hint_creates = hint_shows = hint_hides = hint_refreshes = hint_message = 0;
    hint_text[0] = 0;
}

/* Frames in which only the director runs, as in a facility screen or a stage: the town update does not. */
static void director_only_frames(int count) {
    for (int i = 0; i < count; ++i) {
        fake_now += 16;
        director_frame(0, 0, 0);
    }
}

static void test_shortcut_hint(void) {
    /* Hydeland, access active, prompt answered: one hint on the top help dialog, once */
    hint_world(0, 0, 0);
    town_frame();
    CHECK(early_story_town_prompt_open() && !early_story_hint_visible() && hint_creates == 0);
    settle_prompt();
    CHECK(early_story_hint_visible() && hint_creates == 1 && hint_shows == 1 && hint_refreshes == 1);
    CHECK(hint_message == EARLY_STORY_HINT_MESSAGE && !strcmp(hint_text, "@#(2) Open Network Menu"));
    for (int i = 0; i < 100; ++i) {
        shortcut_frame();
        director_frame(0, 0, 0);
    }
    CHECK(hint_creates == 1 && hint_shows == 1 && hint_hides == 0);

    /* the glyph code is the logical Triangle (the game draws Y with Xbox prompts), the text the language's */
    hint_world(0, 0, 0);
    early_story_hint_set_portuguese(1);
    settle_prompt();
    CHECK(!strcmp(hint_text, "@#(2) Abrir Menu de Rede"));
    early_story_hint_set_portuguese(0);

    /* after Yes the same, and the prompt itself never shows it */
    hint_world(0, 1, 0);
    shortcut_frame();
    CHECK(early_story_town_prompt_open() && hint_creates == 0);
    settle_prompt();
    CHECK(early_story_hint_visible() && hint_creates == 1);

    /* the shortcut opens the Network Menu: the hint goes at once, comes back when the menu is closed */
    hint_world(0, 0, 0);
    settle_prompt();
    CHECK(early_story_hint_visible());
    CHECK(press_and_poll(0) == 1);
    CHECK(!early_story_hint_visible() && hint_hides == 1);
    for (int i = 0; i < 12; ++i) shortcut_frame();
    CHECK(!early_story_hint_visible());
    settle_prompt();
    CHECK(early_story_hint_visible() && hint_shows == 2 && hint_hides == 1);

    /* leaving Hydeland for the Inn, the Guild, the shop, the Gate, the Stables or a stage hides it; coming back shows it once */
    for (uint32_t location = 1; location <= 5; ++location) {
        hint_world(0, 0, 0);
        settle_prompt();
        CHECK(early_story_hint_visible());
        fake_area(EARLY_STORY_AREA_TOWN, location);
        shortcut_frame();
        CHECK(!early_story_hint_visible() && hint_hides == 1);
        fake_area(EARLY_STORY_AREA_TOWN, EARLY_STORY_AREA_HYDELAND);
        shortcut_frame();
        director_frame(0, 0, 0);
        shortcut_frame();
        CHECK(early_story_hint_visible() && hint_shows == 2);
    }
    hint_world(0, 0, 0);
    settle_prompt();
    fake_area(7, EARLY_STORY_AREA_HYDELAND);
    shortcut_frame();
    CHECK(!early_story_hint_visible());

    /* the town update stops running there: the director's poll alone takes the hint away */
    hint_world(0, 0, 0);
    settle_prompt();
    CHECK(early_story_hint_visible());
    director_only_frames(12);
    CHECK(!early_story_hint_visible() && hint_hides == 1);

    /* the game destroyed the window itself: nothing is called on it */
    hint_world(0, 0, 0);
    settle_prompt();
    memset(hint_slot(), 0, 8);
    director_only_frames(12);
    CHECK(!early_story_hint_visible() && hint_hides == 0);

    /* a visitor, a guest with the setting off, an inactive feature, no window manager: no hint */
    hint_world(45, 1, 0);
    join_room(0, 0);
    {
        GuestRegs lead;
        selected(&lead, 0);
    }
    settle_prompt();
    CHECK(!early_story_hint_visible() && hint_creates == 0);
    hint_world(0, 1, 0);
    early_story_set_hosting(0);
    settle_prompt();
    CHECK(!early_story_hint_visible() && hint_creates == 0);
    hint_world(0, 1, 0);
    early_story_set_active(0);
    settle_prompt();
    CHECK(!early_story_hint_visible() && hint_creates == 0);
    hint_world(0, 1, 0);
    memset(fake_manager + EARLY_STORY_MANAGER_UI, 0, 8);
    settle_prompt();
    CHECK(!early_story_hint_visible() && hint_creates == 0);
    /* Online Play off: the hooks are not installed and the hint is not configured */
    hint_world(0, 1, 0);
    early_story_hint_configure(NULL);
    settle_prompt();
    CHECK(!early_story_hint_visible() && hint_creates == 0);

    /* the shortcut does not depend on the hint */
    CHECK(press_and_poll(0) == 0 || press_and_poll(0) == 1);
    hint_world(0, 0, 0);
    early_story_hint_configure(NULL);
    settle_prompt();
    CHECK(press_and_poll(0) == 1);

    /* a window whose show method is not the game's is left alone for the run, and the shortcut still works */
    hint_world(0, 0, 1);
    settle_prompt();
    CHECK(!early_story_hint_visible() && hint_creates == 1 && hint_shows == 0);
    settle_prompt();
    CHECK(hint_creates == 1);
    CHECK(press_and_poll(0) == 1);

    early_story_hint_configure(NULL);
    early_story_shortcut_set_clock(NULL);
    early_story_online_set_prompt(NULL, 0, 0);
    early_story_online_set_selector(NULL);
    early_story_set_hosting(1);
}

/* The host room: the director's room wish (fake), and its town location test. */
static int room_wish_calls;
static uint64_t room_wish_director;

static void GUEST_HOOK_ABI fake_room_wish(uint64_t director) {
    ++room_wish_calls;
    room_wish_director = director;
}

static uint64_t wished_location(uint32_t location) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rax = location;
    early_story_room_wish_hook(&regs);
    CHECK(!regs.skip);
    return regs.rax;
}

/* The room info's location as the hook leaves it, for a scene of `type` at `location`. */
static uint32_t advertised_location(uint32_t location, uint32_t type) {
    uint32_t saved_type;
    memcpy(&saved_type, fake_scene + EARLY_STORY_AREA_TYPE, sizeof(saved_type));
    memcpy(fake_scene + EARLY_STORY_AREA_TYPE, &type, sizeof(type));
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rax = location;
    regs.r13 = (uint64_t)(uintptr_t)fake_scene;
    early_story_room_info_hook(&regs);
    memcpy(fake_scene + EARLY_STORY_AREA_TYPE, &saved_type, sizeof(saved_type));
    CHECK(!regs.skip);
    return (uint32_t)(uint16_t)regs.rax;
}

static void room_world(int own_step, int answer_yes) {
    shortcut_world(own_step, answer_yes);
    early_story_note_own_step(own_step);
    early_story_online_set_room_wish(fake_room_wish);
    room_wish_calls = 0;
    room_wish_director = 0;
}

static int town_keeps_rooms(void) {
    for (uint32_t location = 0; location <= 9; ++location)
        if (wished_location(location) != EARLY_STORY_ROOM_WISH_GATE) return 0;
    return 1;
}

static int town_left_to_the_game(void) {
    for (uint32_t location = 0; location <= 9; ++location)
        if (wished_location(location) != location) return 0;
    return 1;
}

static void test_host_room_wish(void) {
    /* an early host: nothing before the access latch, then the room wish once, after the prompt (Yes or No) */
    for (int answer = 0; answer <= 1; ++answer) {
        room_world(0, answer);
        CHECK(town_left_to_the_game() && !early_story_host_room_wanted());
        town_frame();
        CHECK(early_story_town_prompt_open() && room_wish_calls == 0);
        settle_prompt();
        CHECK(room_wish_calls == 1 && room_wish_director == (uint64_t)(uintptr_t)fake_scene);
        CHECK(early_story_host_room_wanted() && town_keeps_rooms());
        for (int i = 0; i < 100; ++i) shortcut_frame();
        CHECK(room_wish_calls == 1);
    }

    /* every town location keeps the room (Hydeland, Inn, Guild, shop, Gate, Stables); a new visit asks once more */
    room_world(0, 0);
    settle_prompt();
    fake_area(EARLY_STORY_AREA_TOWN, 3);
    for (int i = 0; i < 10; ++i) shortcut_frame();
    CHECK(room_wish_calls == 1 && town_keeps_rooms());
    fake_area(EARLY_STORY_AREA_TOWN, EARLY_STORY_AREA_HYDELAND);
    memcpy(fake_second_town, fake_town, sizeof(fake_second_town));
    town_in_use = fake_second_town;
    settle_prompt();
    CHECK(room_wish_calls == 2);
    town_in_use = fake_town;

    /* a character past the original unlock keeps the game's own room lifecycle */
    room_world(EARLY_STORY_ORIGINAL_STEP, 1);
    settle_prompt();
    CHECK(room_wish_calls == 0 && !early_story_host_room_wanted() && town_left_to_the_game());
    room_world(60, 1);
    settle_prompt();
    CHECK(room_wish_calls == 0 && town_left_to_the_game());

    /* the setting off, the feature inactive, no access yet: the game's own wish; a visiting guest is no host either */
    room_world(0, 1);
    early_story_set_hosting(0);
    settle_prompt();
    CHECK(room_wish_calls == 0 && town_left_to_the_game());
    room_world(0, 1);
    join_room(0, 20);
    {
        GuestRegs lead;
        selected(&lead, 0);
    }
    CHECK(early_story_visiting());
    settle_prompt();
    CHECK(room_wish_calls == 0 && !early_story_host_room_wanted() && town_keeps_rooms());
    room_world(0, 1);
    early_story_set_active(0);
    settle_prompt();
    CHECK(room_wish_calls == 0 && town_left_to_the_game());
    room_world(0, 1);
    fake_control(0);
    settle_prompt();
    CHECK(room_wish_calls == 0 && !early_story_access_ready() && town_left_to_the_game());

    /* a guest seated in a story room keeps it in town too, an early guest or one past the unlock with the setting off;
     * a normal room, or the end of the session, leaves the town to the game */
    room_world(0, 1);
    early_story_set_hosting(0);
    join_room(0, 0);
    CHECK(early_story_story_room_guest() && early_story_visiting() && town_keeps_rooms());
    early_story_session_reset();
    CHECK(!early_story_story_room_guest() && town_left_to_the_game());
    room_world(50, 1);
    early_story_set_hosting(0);
    join_room(0, 0);
    CHECK(early_story_visiting() && town_keeps_rooms());
    early_story_session_reset();
    join_room(0, -1);
    CHECK(!early_story_story_room_guest() && town_left_to_the_game());
    join_room(1, 0);
    CHECK(!early_story_story_room_guest());
    early_story_session_reset();

    /* the room info friends read: an early host anywhere in town reads as being at the Castle Gate */
    room_world(0, 1);
    settle_prompt();
    for (uint32_t location = 0; location <= 9; ++location)
        CHECK(advertised_location(location, EARLY_STORY_AREA_TOWN) ==
              ((location | 8) == 9 ? location : EARLY_STORY_ROOM_WISH_GATE));
    CHECK(advertised_location(0, 3) == 0 && advertised_location(0x2a5, EARLY_STORY_AREA_TOWN) == EARLY_STORY_ROOM_WISH_GATE);
    {
        GuestRegs regs;
        memset(&regs, 0, sizeof(regs));
        regs.rax = 0xabcd00000000ull;
        regs.r13 = (uint64_t)(uintptr_t)fake_scene;
        early_story_room_info_hook(&regs);
        CHECK(regs.rax == (0xabcd00000000ull | EARLY_STORY_ROOM_WISH_GATE) && !regs.skip);
        regs.r13 = 0;
        regs.rax = 0;
        early_story_room_info_hook(&regs);
        CHECK(regs.rax == 0);
    }
    room_world(EARLY_STORY_ORIGINAL_STEP, 1);
    settle_prompt();
    CHECK(advertised_location(0, EARLY_STORY_AREA_TOWN) == 0);
    room_world(0, 1);
    early_story_set_hosting(0);
    settle_prompt();
    CHECK(advertised_location(0, EARLY_STORY_AREA_TOWN) == 0);
    room_world(0, 1);
    early_story_set_hosting(0);
    join_room(0, 0);
    CHECK(early_story_story_room_guest() && advertised_location(0, EARLY_STORY_AREA_TOWN) == 0);
    early_story_session_reset();

    /* without the game's function the latch still opens the town for the room, it is only not asked again */
    room_world(0, 1);
    early_story_online_set_room_wish(NULL);
    settle_prompt();
    CHECK(room_wish_calls == 0 && town_keeps_rooms());

    early_story_online_set_room_wish(NULL);
    early_story_shortcut_set_clock(NULL);
    early_story_online_set_prompt(NULL, 0, 0);
    early_story_online_set_selector(NULL);
    early_story_set_hosting(1);
    early_story_note_own_step(EARLY_STORY_NO_STEP);
}

/* One story session membership: the guest follows the host from the join to the leave, whatever the two steps do and
 * wherever the players walk; its own record is never written and its own story comes back once, at the leave. */
/* The host's step on a guest: seeded by the room attribute at the join, then the owner's live state in the order it
 * arrives; a replicated record never overrides that. */
static void test_host_step_updates(void) {
    GuestRegs regs;
    start(0, 0, 0);
    uint8_t before[EARLY_STORY_RECORD_SIZE];
    memcpy(before, record(0), sizeof(before));
    CHECK(early_story_session_host_step() == 0);
    for (int step = 1; step <= 4; ++step) {
        CHECK(early_story_session_internal_update(step));
        CHECK(early_story_session_host_step() == step);
        CHECK(early_story_visiting() && selected(&regs, 0)[EARLY_STORY_RECORD_STEP] == step);
    }
    CHECK(!memcmp(before, record(0), sizeof(before)) && early_story_own_step() == 0);

    /* the spec's case: join 0, live 1, a stale replicated 0 later: still 1 */
    start(0, 0, 0);
    early_story_session_internal_update(1);
    early_story_note_host_step(0);
    CHECK(early_story_session_host_step() == 1);
    early_story_session_internal_update(2);
    CHECK(early_story_session_host_step() == 2);
    early_story_session_internal_update(3);
    CHECK(early_story_session_host_step() == 3);
    /* nor does a newer-looking one: the live state is the owner's own word */
    early_story_note_host_step(9);
    CHECK(early_story_session_host_step() == 3);
    /* the step writer is unconditional and the lead record can change: a lower live step is followed, not refused */
    CHECK(early_story_session_internal_update(1) && early_story_session_host_step() == 1);
    CHECK(!early_story_session_internal_update(EARLY_STORY_NO_STEP) && early_story_session_host_step() == 1);
    CHECK(selected(&regs, 0)[EARLY_STORY_RECORD_STEP] == 1 && record(0)[EARLY_STORY_RECORD_STEP] == 0);

    /* before any live state, the replicated record can only move the join seed forward */
    start(0, 0, 3);
    early_story_note_host_step(1);
    CHECK(early_story_session_host_step() == 3);
    early_story_note_host_step(5);
    CHECK(early_story_session_host_step() == 5);

    /* leaving forgets it all; a new room starts again from its own seed */
    early_story_session_internal_update(7);
    early_story_session_reset();
    CHECK(early_story_session_host_step() == EARLY_STORY_NO_STEP);
    CHECK(!early_story_session_internal_update(9) && early_story_session_host_step() == EARLY_STORY_NO_STEP);
    start(0, 0, 0);
    early_story_session_internal_update(7);
    join_room(0, 2);
    CHECK(early_story_session_host_step() == 2);

    /* the owner and the guest of a normal room ignore it */
    start(0, 1, 0);
    CHECK(!early_story_session_internal_update(4) && early_story_session_host_step() == 0 && !early_story_visiting());
    start(50, 0, EARLY_STORY_NO_STEP);
    CHECK(!early_story_session_internal_update(4) && early_story_session_host_step() == EARLY_STORY_NO_STEP);
    CHECK(!early_story_visiting());
    early_story_session_reset();
}

static void test_state_attribute(void) {
    uint8_t state[EARLY_STORY_STATE_SIZE];
    for (int step = 0; step <= 0xff; ++step) {
        early_story_state_encode(step, state);
        CHECK(state[0] == EARLY_STORY_STATE_FORMAT && early_story_state_decode(state, sizeof(state)) == step);
    }
    const uint8_t longer[4] = {EARLY_STORY_STATE_FORMAT, 3, 0xaa, 0xbb};
    CHECK(early_story_state_decode(longer, sizeof(longer)) == 3);
    const uint8_t other_format[2] = {2, 3};
    CHECK(early_story_state_decode(other_format, sizeof(other_format)) == EARLY_STORY_NO_STEP);
    CHECK(early_story_state_decode(longer, 1) == EARLY_STORY_NO_STEP);
    CHECK(early_story_state_decode(NULL, 2) == EARLY_STORY_NO_STEP);
    CHECK(EARLY_STORY_STATE_ATTR != 0x57 && EARLY_STORY_STATE_ATTR != 0x58 && EARLY_STORY_STATE_ATTR != EARLY_STORY_ROOM_ATTR);
}

static int observed[16], observed_count;
static void observe(int step) {
    if (observed_count < 16) observed[observed_count++] = step;
}

/* The host's transitions reach the publisher once each, from the selector the game reads the step through. */
static void test_own_step_observer(void) {
    GuestRegs regs;
    start(0, 1, 0);
    observed_count = 0;
    early_story_session_observe_own_step(observe);
    for (int frame = 0; frame < 3; ++frame) selected(&regs, 0);
    for (int step = 1; step <= 4; ++step) {
        record(0)[EARLY_STORY_RECORD_STEP] = (uint8_t)step;
        for (int frame = 0; frame < 3; ++frame) selected(&regs, 0);
    }
    CHECK(observed_count == 5 && observed[0] == 0 && observed[1] == 1 && observed[4] == 4);
    early_story_session_observe_own_step(NULL);
    record(0)[EARLY_STORY_RECORD_STEP] = 5;
    selected(&regs, 0);
    CHECK(observed_count == 5 && early_story_own_step() == 5);
    early_story_session_reset();
}

/* The log lines a block of calls printed. */
static char capture_path[512];
static int capture_saved = -1;

static void capture_begin(void) {
    const char *dir = getenv("TEMP") ? getenv("TEMP") : getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    snprintf(capture_path, sizeof(capture_path), "%s/dcp-early-story-log-%d.txt", dir, (int)getpid());
    fflush(stdout);
    FILE *f = fopen(capture_path, "w");
    if (!f) return;
    capture_saved = dup(fileno(stdout));
    dup2(fileno(f), fileno(stdout));
    fclose(f);
}

static char *capture_end(void) {
    fflush(stdout);
    if (capture_saved >= 0) {
        dup2(capture_saved, fileno(stdout));
        close(capture_saved);
        capture_saved = -1;
    }
    FILE *f = fopen(capture_path, "rb");
    char *text = calloc(1 << 16, 1);
    if (f && text) fread(text, 1, (1 << 16) - 1, f);
    if (f) fclose(f);
    remove(capture_path);
    return text;
}

static int occurrences(const char *text, const char *what) {
    int n = 0;
    for (const char *at = text ? strstr(text, what) : NULL; at; at = strstr(at + 1, what)) ++n;
    return n;
}

static const char kFollowing[] = "following the host's story";
static const char kRestored[] = "own story restored";
static const char kKept[] = "own story step kept at";

/* The game refilling the lead record from session data that carried the shadowed step (the failed run's own 0 -> 1). */
static void game_refills_lead_record(int step) {
    record(0)[EARLY_STORY_RECORD_STEP] = (uint8_t)step;
}

static int original_threshold_frame(void) {
    early_story_online_set_selector(fake_select);
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    early_story_original_online_hook(&regs);
    return (int)regs.rax;
}

/* Bug: own step 0 -> 1 while visiting. The guest's own step is held at its value from the visit's start through the
 * whole session and is what it returns to. */
static void test_own_step_baseline(void) {
    GuestRegs regs;
    start(0, 0, 0);
    capture_begin();
    CHECK(early_story_session_internal_update(1));
    CHECK(selected(&regs, 0)[EARLY_STORY_RECORD_STEP] == 1 && early_story_visiting() && early_story_own_step() == 0);
    game_refills_lead_record(1);
    CHECK(selected(&regs, 0)[EARLY_STORY_RECORD_STEP] == 1 && record(0)[EARLY_STORY_RECORD_STEP] == 0);
    CHECK(early_story_own_step() == 0);
    for (int step = 2; step <= 4; ++step) {
        early_story_session_internal_update(step);
        game_refills_lead_record(step);
        CHECK(selected(&regs, 0)[EARLY_STORY_RECORD_STEP] == step);
        CHECK(record(0)[EARLY_STORY_RECORD_STEP] == 0 && early_story_own_step() == 0 && early_story_visiting());
    }
    /* a read of the real record (the original threshold) sees the baseline too */
    game_refills_lead_record(4);
    CHECK(original_threshold_frame() == 0 && record(0)[EARLY_STORY_RECORD_STEP] == 0);
    early_story_online_set_selector(NULL);
    char *log = capture_end();
    CHECK(occurrences(log, kFollowing) == 1 && occurrences(log, kRestored) == 0 && occurrences(log, kKept) == 1);
    free(log);

    /* the lead record moved to another slot during scene setup: same character, held all the same */
    memcpy(record(1), record(0), EARLY_STORY_RECORD_SIZE);
    record(1)[EARLY_STORY_RECORD_STEP] = 4;
    CHECK(selected(&regs, 1)[EARLY_STORY_RECORD_STEP] == 4 && record(1)[EARLY_STORY_RECORD_STEP] == 0);

    /* each real way out restores the step once, even when the game set it again just before */
    for (int exit = 0; exit < 3; ++exit) {
        start(0, 0, 0);
        early_story_session_internal_update(3);
        selected(&regs, 0);
        game_refills_lead_record(3);
        capture_begin();
        early_story_session_reset(); /* leave, disconnect, room destroyed: ClearRoom */
        for (int frame = 0; frame < 4; ++frame) CHECK(selected(&regs, 0) == record(0) && !regs.skip);
        log = capture_end();
        CHECK(occurrences(log, kRestored) == 1 && occurrences(log, kFollowing) == 0);
        CHECK(record(0)[EARLY_STORY_RECORD_STEP] == 0 && early_story_own_step() == 0 && !early_story_visiting());
        free(log);
    }

    /* another character loaded before the next read is never written */
    start(0, 0, 0);
    early_story_session_internal_update(2);
    selected(&regs, 0);
    early_story_session_reset();
    fill_save(7);
    record(0)[EARLY_STORY_IDENTITY] ^= 0x55;
    selected(&regs, 0);
    CHECK(record(0)[EARLY_STORY_RECORD_STEP] == 7);

    /* normal rooms: past the unlock nothing visits, nothing is held */
    start(50, 0, EARLY_STORY_NO_STEP);
    early_story_note_host_step(46);
    capture_begin();
    game_refills_lead_record(51);
    CHECK(selected(&regs, 0) == record(0) && record(0)[EARLY_STORY_RECORD_STEP] == 51 && !early_story_visiting());
    log = capture_end();
    CHECK(occurrences(log, kFollowing) == 0 && occurrences(log, kRestored) == 0 && occurrences(log, kKept) == 0);
    free(log);
    /* an early guest of an advanced host is held at its own step though its step would no longer differ */
    start(5, 0, EARLY_STORY_NO_STEP);
    early_story_note_host_step(50);
    CHECK(selected(&regs, 0)[EARLY_STORY_RECORD_STEP] == 50 && early_story_visiting());
    game_refills_lead_record(50);
    CHECK(selected(&regs, 0)[EARLY_STORY_RECORD_STEP] == 50 && record(0)[EARLY_STORY_RECORD_STEP] == 5);
    early_story_session_reset();
    selected(&regs, 0);
}

/* Bug: "own story restored" / "following" every frame at the Town Gate. A visit is the room membership: scene changes,
 * reads of the real record and other records never end or restart it. */
static void test_visit_lifetime(void) {
    static const char *const scenes[] = {"Hydeland -> Guild", "Guild -> Hydeland", "Hydeland -> Shop",
                                         "Shop -> Hydeland",  "Hydeland -> Town Gate", "Town Gate -> stage"};
    GuestRegs regs;
    start(0, 0, 0);
    capture_begin();
    early_story_session_internal_update(1);
    selected(&regs, 0);
    for (size_t scene = 0; scene < sizeof(scenes) / sizeof(*scenes); ++scene) {
        early_story_session_internal_update((int)scene + 1);
        for (int frame = 0; frame < 30; ++frame) {
            /* the Castle Gate and Stables ask the original threshold every frame: a real-record read */
            original_threshold_frame();
            CHECK(selected(&regs, 0)[EARLY_STORY_RECORD_STEP] == (int)scene + 1 && regs.skip == 1);
            CHECK(early_story_visiting());
        }
        game_refills_lead_record((int)scene + 1);
        CHECK(selected(&regs, 0)[EARLY_STORY_RECORD_STEP] == (int)scene + 1 && early_story_own_step() == 0);
        if (!early_story_visiting()) printf("FAIL scene %s\n", scenes[scene]);
    }
    early_story_online_set_selector(NULL);
    char *log = capture_end();
    CHECK(occurrences(log, kFollowing) == 1 && occurrences(log, kRestored) == 0 && !strstr(log, "FAIL scene"));
    free(log);

    capture_begin();
    early_story_session_reset();
    for (int frame = 0; frame < 30; ++frame) selected(&regs, 0);
    log = capture_end();
    CHECK(occurrences(log, kRestored) == 1 && occurrences(log, kFollowing) == 0);
    free(log);
}

static void test_story_session_lifetime(void) {
    static const GuestHookHandler writers[] = {early_story_advance_story_hook, early_story_tier_hook,
                                               early_story_talisman_hook, early_story_flag_hook};
    const int exits = 4;
    for (int exit = 0; exit < exits; ++exit) {
        start(1, 0, 0);
        GuestRegs regs;
        CHECK(selected(&regs, 0)[EARLY_STORY_RECORD_STEP] == 0 && early_story_visiting());
        uint8_t before[EARLY_STORY_RECORD_SIZE];
        memcpy(before, record(0), sizeof(before));
        /* the host walks its story 0 -> 1 -> 2 -> 3 (equal to the guest's own 1 on the way) through Hydeland, the Guild
         * and the shop; the guest's game tries its own writes all along */
        for (int host_step = 0; host_step <= 3; ++host_step) {
            early_story_note_host_step(host_step);
            for (int frame = 0; frame < 5; ++frame) {
                const uint8_t *seen = selected(&regs, 0);
                CHECK(early_story_visiting() && regs.skip == 1 && seen[EARLY_STORY_RECORD_STEP] == host_step);
                for (size_t w = 0; w < sizeof(writers) / sizeof(*writers); ++w) {
                    GuestRegs write;
                    memset(&write, 0, sizeof(write));
                    write.rax = 1;
                    writers[w](&write);
                    CHECK(write.skip == 1 && write.rax == 0);
                }
            }
        }
        CHECK(!memcmp(before, record(0), sizeof(before)) && record(0)[EARLY_STORY_RECORD_STEP] == 1);
        /* the real ways out: leave, disconnect, room gone, a normal room */
        if (exit < 3) early_story_session_reset();
        else join_room(0, EARLY_STORY_NO_STEP);
        CHECK(!early_story_visiting());
        CHECK(selected(&regs, 0) == record(0) && !regs.skip && record(0)[EARLY_STORY_RECORD_STEP] == 1);
    }

    /* the host keeps its own record and its writes */
    start(0, 1, 0);
    GuestRegs regs;
    CHECK(selected(&regs, 0) == record(0) && !regs.skip && !early_story_visiting());
    memset(&regs, 0, sizeof(regs));
    early_story_advance_story_hook(&regs);
    CHECK(!regs.skip);

    /* an advanced guest in an early host's room follows it and keeps its campaign */
    start(60, 0, 2);
    CHECK(selected(&regs, 0)[EARLY_STORY_RECORD_STEP] == 2 && early_story_visiting());
    early_story_session_reset();
    CHECK(selected(&regs, 0)[EARLY_STORY_RECORD_STEP] == 60 && !early_story_visiting());

    /* normal rooms: two characters past the unlock play as before, an early guest of an advanced host still visits */
    start(50, 0, EARLY_STORY_NO_STEP);
    early_story_note_host_step(46);
    selected(&regs, 0);
    CHECK(!early_story_visiting() && !regs.skip);
    start(5, 0, EARLY_STORY_NO_STEP);
    early_story_note_host_step(50);
    CHECK(selected(&regs, 0)[EARLY_STORY_RECORD_STEP] == 50 && early_story_visiting());

    /* the town step reader the boundaries use goes through the selector: host and guest read the host's step */
    start(1, 0, 0);
    early_story_note_host_step(2);
    CHECK(selected(&regs, 0)[EARLY_STORY_RECORD_STEP] == 2);
    start(2, 1, 2);
    CHECK(selected(&regs, 0)[EARLY_STORY_RECORD_STEP] == 2);
    early_story_session_reset();
}

static void test_host_record_capture(void) {
    GuestRegs regs;
    start(45, 0, EARLY_STORY_NO_STEP);
    uint8_t host_record[EARLY_STORY_RECORD_SIZE];
    memset(host_record, 0, sizeof(host_record));
    host_record[EARLY_STORY_RECORD_STEP] = 5;
    memset(&regs, 0, sizeof(regs));
    regs.rdi = (uint64_t)(uintptr_t)host_record;
    early_story_host_record_hook(&regs);
    CHECK(!regs.skip && early_story_session_host_step() == 5);
    CHECK(host_record[EARLY_STORY_RECORD_STEP] == 5);
    selected(&regs, 0);
    CHECK(regs.skip == 1);
    regs.rdi = 0;
    early_story_host_record_hook(&regs);
    CHECK(early_story_session_host_step() == 5);
}

static void test_writers(void) {
    static const GuestHookHandler writers[] = {early_story_advance_story_hook, early_story_tier_hook,
                                               early_story_talisman_hook, early_story_flag_hook};
    GuestRegs regs;
    for (size_t i = 0; i < sizeof(writers) / sizeof(*writers); ++i) {
        start(45, 0, 5);
        selected(&regs, 0);
        const uint32_t before = early_story_blocked_writes();
        memset(&regs, 0, sizeof(regs));
        regs.rax = 9;
        writers[i](&regs);
        CHECK(regs.skip == 1 && regs.rax == 0 && early_story_blocked_writes() == before + 1);

        start(5, 0, 5);
        selected(&regs, 0);
        memset(&regs, 0, sizeof(regs));
        regs.rax = 9;
        writers[i](&regs);
        CHECK(regs.skip == 1 && regs.rax == 0);

        start(5, 0, EARLY_STORY_NO_STEP);
        early_story_note_host_step(5);
        selected(&regs, 0);
        memset(&regs, 0, sizeof(regs));
        regs.rax = 9;
        writers[i](&regs);
        CHECK(!regs.skip && regs.rax == 9);

        start(5, 1, 5);
        selected(&regs, 0);
        memset(&regs, 0, sizeof(regs));
        writers[i](&regs);
        CHECK(!regs.skip);

        start(46, 0, EARLY_STORY_NO_STEP);
        early_story_note_host_step(45);
        selected(&regs, 0);
        memset(&regs, 0, sizeof(regs));
        writers[i](&regs);
        CHECK(!regs.skip);

        start(45, 0, 5);
        selected(&regs, 0);
        early_story_session_reset();
        memset(&regs, 0, sizeof(regs));
        writers[i](&regs);
        CHECK(!regs.skip);
    }
}

static void test_reset_paths(void) {
    GuestRegs regs;
    start(45, 0, 5);
    early_story_note_host_step(5);
    selected(&regs, 0);
    CHECK(early_story_visiting());
    early_story_session_reset();
    CHECK(!early_story_visiting() && early_story_session_host_step() == EARLY_STORY_NO_STEP);
    CHECK(selected(&regs, 0) == record(0) && !regs.skip);

    /* a story session ends with its owner, for the new owner and for everyone else */
    for (int new_owner = 0; new_owner <= 1; ++new_owner) {
        start(45, 0, 5);
        early_story_session_internal_update(6);
        selected(&regs, 0);
        CHECK(early_story_visiting() && early_story_story_room_guest());
        early_story_session_owner_changed(new_owner);
        CHECK(!early_story_visiting() && !early_story_story_room_guest());
        CHECK(early_story_session_host_step() == EARLY_STORY_NO_STEP && !early_story_session_internal_update(7));
        CHECK(selected(&regs, 0) == record(0) && !regs.skip);
        early_story_session_owner_changed(0);
        CHECK(!early_story_visiting());
    }

    /* a normal room keeps swapping the role */
    start(45, 0, EARLY_STORY_NO_STEP);
    early_story_note_host_step(5);
    selected(&regs, 0);
    CHECK(early_story_visiting());
    early_story_session_owner_changed(1);
    CHECK(!early_story_visiting());
    early_story_session_owner_changed(0);
    CHECK(early_story_visiting());

    early_story_session_reset();
    early_story_session_owner_changed(0);
    CHECK(!early_story_visiting());

    start(45, 0, 5);
    early_story_set_active(0);
    CHECK(!early_story_visiting() && early_story_session_host_step() == EARLY_STORY_NO_STEP);

    start(45, 0, 5);
    join_room(0, 6);
    CHECK(early_story_session_host_step() == 6);
    join_room(0, EARLY_STORY_NO_STEP);
    CHECK(early_story_session_host_step() == EARLY_STORY_NO_STEP);
}

static int allowed(int own, int host, int stage, int packed) {
    const char *reason = NULL;
    const int result = early_story_join_allowed(own, host, stage, packed, &reason);
    CHECK(result ? reason[0] == 0 : reason[0] != 0);
    return result;
}

static void test_join_policy(void) {
    early_story_set_active(1);
    /* an early room: friends and codes with any progress */
    CHECK(allowed(45, 5, EARLY_STORY_NO_STEP, EARLY_STORY_NO_STEP));
    CHECK(allowed(5, 5, EARLY_STORY_NO_STEP, EARLY_STORY_NO_STEP));
    CHECK(allowed(7, 30, 3, 0) && allowed(60, 44, 9, 16));
    /* advanced characters join advanced hosts as today */
    CHECK(allowed(45, 0, 3, 0) && allowed(52, 0, 17, 4 << 2) && allowed(46, 0, 15, 2 << 2));
    /* an early character visits only a Normal stage 0-8 */
    CHECK(allowed(5, 0, 0, 0) && allowed(44, 0, 8, 0) && allowed(5, 0, 4, 3));
    CHECK(!allowed(5, 0, 9, 0));
    CHECK(!allowed(5, 0, 2, 2 << 2));
    CHECK(!allowed(5, 0, 2, 4 << 2));
    CHECK(!allowed(5, 0, 17, 0));
    CHECK(!allowed(5, 0, 15, 0));
    CHECK(!allowed(5, 0, EARLY_STORY_NO_STEP, 0));
    CHECK(!allowed(5, 0, 2, EARLY_STORY_NO_STEP));
    CHECK(allowed(EARLY_STORY_NO_STEP, 0, 17, 16));
    /* without the feature a story room is refused and a normal one is not */
    early_story_set_active(0);
    CHECK(!allowed(45, 5, EARLY_STORY_NO_STEP, EARLY_STORY_NO_STEP));
    CHECK(allowed(45, 0, 3, 0) && allowed(5, 0, 17, 16));
    CHECK(early_story_join_allowed(5, 0, 0, 0, NULL) == 1);
}

static char *read_text(const char *relative) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", DCP_SOURCE_DIR, relative);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = calloc((size_t)size + 1, 1);
    if (text && fread(text, 1, (size_t)size, f) != (size_t)size) free(text), text = NULL;
    fclose(f);
    return text;
}

/* The reset belongs in the one function every way out of a room goes through. */
static void test_source_wiring(void) {
    char *matching = read_text("src/runtime_matching2.cpp");
    char *loader = read_text("src/loader.c");
    char *options = read_text("gpu/shim/dcp_options_menu.cpp");
    CHECK(matching && loader);
    if (matching) {
        const char *clear = strstr(matching, "void ClearRoom() {");
        CHECK(clear && strstr(clear, "early_story_session_reset();") && strstr(clear, "early_story_session_reset();") < strstr(clear, "g.room = 0;"));
        const char *gone = strstr(matching, "void RoomGone(");
        CHECK(gone && strstr(gone, "ClearRoom();") && strstr(gone, "ClearRoom();") < strstr(gone, "Json HelloBody()"));
        const char *leave = strstr(matching, "m2_leave_room(");
        CHECK(leave && strstr(leave, "ClearRoom();") && strstr(leave, "ClearRoom();") < strstr(leave, "m2_search_room("));
        const char *stop = strstr(matching, "m2_context_stop(");
        CHECK(stop && strstr(stop, "ClearRoom();") && strstr(stop, "ClearRoom();") < strstr(stop, "start_wanted = false"));
        CHECK(strstr(matching, "early_story_session_owner_changed(") && strstr(matching, "RefuseStoryJoin(") &&
              strstr(matching, "early_story_room_flags("));
        CHECK(strstr(matching, "early_story_session_joined("));
        /* external data has no room event; the live state rides SetRoomDataInternal and the game never sees it */
        CHECK(!strstr(matching, "room.external") && !strstr(matching, "room_attr_changed"));
        const char *internal = strstr(matching, "m.event == \"room.internal\"");
        CHECK(internal && strstr(internal, "WithoutStoryState(b[\"bin_int\"]") &&
              strstr(internal, "WithoutStoryState(b[\"bin_int\"]") < strstr(internal, "PushRoomEvent(kRoomInternal") &&
              strstr(internal, "InternalAttrs(*block, game_attrs, n)") < strstr(internal, "PushRoomEvent(kRoomInternal"));
        const char *join = strstr(matching, "uint8_t* JoinResponse(");
        CHECK(join && strstr(join, "WithoutStoryState(room[\"bin_int\"]") &&
              strstr(join, "WithoutStoryState(room[\"bin_int\"]") < strstr(join, "uint8_t* SearchResponse("));
        const char *publish = strstr(matching, "void PublishStoryState() {");
        CHECK(publish && strstr(publish, "!g.story_room_hosted || !g.room || g.me != g.owner") &&
              strstr(publish, "\"room.set_internal\"") < strstr(publish, "void OnOwnStoryStep("));
        CHECK(strstr(matching, "early_story_session_observe_own_step(OnOwnStoryStep)"));
        const char *game_internal = strstr(matching, "DCP_SYSV int32_t m2_set_room_internal(");
        CHECK(game_internal && strstr(game_internal, "PublishStoryState();") &&
              strstr(game_internal, "PublishStoryState();") < strstr(game_internal, "DCP_SYSV int32_t m2_set_room_external("));
        CHECK(strstr(matching, "story_room && !g.story_capable") && strstr(matching, "hello.set(\"story_session\", true)"));
        CHECK(strstr(matching, "story_session_capability(hello_reply)") && strstr(matching, "story_room_hosted"));
        /* the room is refused before the game sees a seat in it */
        CHECK(strstr(matching, "data = JoinResponse(*block, m.body, story_step);") &&
              strstr(matching, "RefuseStoryJoin(m.body[\"room\"])") < strstr(matching, "data = JoinResponse(*block, m.body, story_step);"));
    }
    if (matching) {
        const char *hello = strstr(matching, "Json HelloBody() {");
        const char *hello_end = hello ? strstr(hello, "\n}\n") : NULL;
        CHECK(hello && hello_end && strstr(hello, "early_story_active()") && strstr(hello, "early_story_active()") < hello_end);
        CHECK(hello && hello_end && (!strstr(hello, "early_story_hosting") || strstr(hello, "early_story_hosting") > hello_end));
        CHECK(!strstr(matching, "early_story_hosting("));
        CHECK(strstr(matching, "does not keep story rooms to capable players") != NULL);
        CHECK(!strstr(matching, "Turn it on in the launcher"));
    }
    if (loader) {
        CHECK(strstr(loader, "dcp_early_story_online(settings_file)") && strstr(loader, "dcp_resolve_early_story("));
        CHECK(strstr(loader, "runtime_online_active()") != NULL);
        CHECK(!strstr(loader, "dcp_locate_early_story_sites"));
        /* the hooks follow Online Play, never the setting; the setting only reaches the host switch */
        CHECK(strstr(loader, "early_story_set_hosting(early_story_requested)") != NULL);
        CHECK(strstr(loader, "known_build && runtime_online_active() ? early_story_online_stub_bytes()") != NULL);
        CHECK(!strstr(loader, "early_story_requested ? early_story_online_stub_bytes()"));
        CHECK(!strstr(loader, "early_story_requested && runtime_online_active()"));
    }
    if (options) CHECK(!strstr(options, "early_story"));
    /* Triangle is the one logical button: Xbox Y, PlayStation Triangle and the Q key all reach it before the filter */
    char *pad = read_text("src/runtime_pad.c");
    char *keys = read_text("src/dcp_keys.c");
    CHECK(pad && keys);
    if (pad && keys) {
        CHECK(strstr(pad, "BTN_TRIANGLE=0x1000") && strstr(pad, "{SDL_GAMEPAD_BUTTON_NORTH,BTN_TRIANGLE}"));
        CHECK(strstr(pad, "{DCP_KEY_TRIANGLE,BTN_TRIANGLE}") && strstr(keys, "{\"key_triangle\", \"Triangle (Y)\", SDL_SCANCODE_Q}"));
        CHECK(EARLY_STORY_PAD_TRIANGLE == 0x1000);
        const char *filter = strstr(pad, "early_story_shortcut_pad_filter(");
        CHECK(filter && filter > strstr(pad, "static void sample(int slot") && filter > strstr(pad, "sample_host(slot,d);") &&
              filter > strstr(pad, "sample_player1_scripts(d);"));
    }
    free(pad), free(keys);
    /* the investigation's diagnostics and telemetry are gone for good */
    static const char *const feature_files[] = {"src/early_story_online.c",  "src/early_story_online.h",
                                                "src/early_story_session.c", "src/early_story_session.h",
                                                "src/runtime_pad.c",         "src/runtime_matching2.cpp",
                                                "src/feature_sites.h",       "launcher/feature_sites.cpp",
                                                "launcher/build_scan_sites.cpp", "tests/online_hle_tests.cpp"};
    for (size_t i = 0; i < sizeof(feature_files) / sizeof(*feature_files); ++i) {
        char *text = read_text(feature_files[i]);
        CHECK(text && !strstr(text, "DEBUG_SAULO") && !strstr(text, "debug_saulo") && !strstr(text, "runtime_pad_key_down") &&
              !strstr(text, "friend_check") && !strstr(text, "host_step_source"));
        free(text);
    }
    /* the visit is the session's, never a per-read toggle */
    char *hooks = read_text("src/early_story_online.c");
    if (hooks) {
        const char *selector = strstr(hooks, "void GUEST_HOOK_ABI early_story_selector_hook(");
        CHECK(selector && strstr(selector, "follow_session(real)") &&
              strstr(selector, "follow_session(real)") < strstr(selector, "if (real_record_wanted || !visiting) return;"));
        CHECK(!strstr(hooks, "following;") && occurrences(hooks, kFollowing) == 1 && occurrences(hooks, kRestored) == 1);
    }
    free(hooks);
    if (loader) {
        CHECK(!strstr(loader, "debug_saulo") && !strstr(loader, "runtime_pad_key_down"));
        CHECK(strstr(loader, "early_story_hint_set_portuguese(dcp_text_language_is_portuguese_brazil("));
    }
    free(matching), free(loader), free(options);
}

int main(void) {
    test_context_rules();
    test_threshold_and_rooms();
    test_host_setting();
    test_online_check();
    test_guest_with_setting_off();
    test_visiting_detection();
    test_shadow_record();
    test_original_threshold_handler();
    test_hydeland_activation();
    test_network_shortcut();
    test_shortcut_hint();
    test_host_room_wish();
    test_host_record_capture();
    test_story_session_lifetime();
    test_host_step_updates();
    test_state_attribute();
    test_own_step_observer();
    test_own_step_baseline();
    test_visit_lifetime();
    test_writers();
    test_reset_paths();
    test_join_policy();
    test_source_wiring();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("early story online tests passed\n");
    return 0;
}
