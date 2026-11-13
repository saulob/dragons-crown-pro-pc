#include "early_story_online.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const uint8_t kSelectorExit[8] = {0x48, 0x8d, 0x84, 0x08, 0xa8, 0x00, 0x00, 0x00};
static const uint8_t kStandardWriter[8] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56};
static const uint8_t kFlagWriter[8] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x89, 0xf3};
static const uint8_t kOnlineAccessor[8] = {0x80, 0x7f, 0x6f, 0x2c, 0x0f, 0x97, 0xc0, 0xc3};

static _Atomic(EarlyStorySelectorFn) selector_function;
static _Atomic(EarlyStoryPromptFn) prompt_function;
static _Atomic uint64_t prompt_owner_vtable, control_mask_table;
static _Alignas(16) uint8_t prompt_owner[EARLY_STORY_OWNER_SIZE];
static uint64_t current_town, owner_town;
static int prompt_open, prompt_settled;
static _Atomic uint64_t shortcut_town_until, shortcut_poll_until, shortcut_press_at;
static atomic_int shortcut_previous_down, shortcut_hidden;
static int shortcut_requested;
static EarlyStoryHintFunctions hint_functions;
static int hint_portuguese, hint_shown, hint_failed, hint_blocked;
static uint64_t hint_window, hint_ui;
static _Atomic(EarlyStoryRoomWishFn) room_wish_function;
static uint64_t room_wish_town;
static _Atomic(uint64_t (*)(void)) shortcut_clock;
static _Atomic uint32_t blocked_writes;
static atomic_uint writers_logged;
static atomic_int visit_baseline = EARLY_STORY_NO_STEP;
static _Atomic(uint8_t *) visit_record;
static uint8_t visit_identity[EARLY_STORY_IDENTITY_SIZE];
static _Thread_local int real_record_wanted;
static uint8_t shadow[EARLY_STORY_LOCAL_RECORDS][EARLY_STORY_RECORD_SIZE];

void early_story_online_set_selector(EarlyStorySelectorFn selector) {
    atomic_store(&selector_function, selector);
}

void early_story_online_set_prompt(EarlyStoryPromptFn prompt, uint64_t owner_vtable, uint64_t control_mask_address) {
    atomic_store(&prompt_owner_vtable, owner_vtable);
    atomic_store(&control_mask_table, control_mask_address);
    atomic_store(&prompt_function, prompt);
    early_story_town_prompt_reset();
}

static uint64_t wall_clock_ms(void) {
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static uint64_t now_ms(void) {
    uint64_t (*clock)(void) = atomic_load(&shortcut_clock);
    return clock ? clock() : wall_clock_ms();
}

void early_story_shortcut_set_clock(uint64_t (*clock)(void)) {
    atomic_store(&shortcut_clock, clock);
}

static void hide_hint(void);

static void shortcut_reset(void) {
    atomic_store(&shortcut_town_until, 0);
    atomic_store(&shortcut_poll_until, 0);
    atomic_store(&shortcut_press_at, 0);
    atomic_store(&shortcut_previous_down, 0);
    atomic_store(&shortcut_hidden, 0);
    shortcut_requested = 0;
    hint_blocked = 0;
    room_wish_town = 0;
    hide_hint();
}

static int shortcut_armed(uint64_t now) {
    return now < atomic_load(&shortcut_town_until) && now < atomic_load(&shortcut_poll_until);
}

int early_story_shortcut_pad_filter(int triangle_down) {
    const int was_down = atomic_exchange(&shortcut_previous_down, triangle_down != 0);
    if (!triangle_down) {
        atomic_store(&shortcut_hidden, 0);
        return 0;
    }
    const uint64_t now = now_ms();
    if (shortcut_armed(now)) {
        if (!was_down) atomic_store(&shortcut_press_at, now ? now : 1);
        atomic_store(&shortcut_hidden, 1);
    }
    return atomic_load(&shortcut_hidden);
}

static int take_shortcut_press(uint64_t now) {
    const uint64_t pressed_at = atomic_exchange(&shortcut_press_at, 0);
    return pressed_at && now - pressed_at <= EARLY_STORY_SHORTCUT_PRESS_MS;
}

int early_story_town_prompt_open(void) {
    return prompt_open;
}

void early_story_town_prompt_reset(void) {
    current_town = owner_town = 0;
    prompt_open = prompt_settled = 0;
    shortcut_reset();
}

uint32_t early_story_blocked_writes(void) {
    return atomic_load(&blocked_writes);
}

/* The game can rewrite the lead record during a session without any of the progression writers (seen at scene setup
 * after a join): the visitor's own step is held at its value from the visit's start. A visit ends only when the session
 * does, and the step goes back only into the same character (the identity the game itself compares). */
static int same_character(const uint8_t *record) {
    return !memcmp(record + EARLY_STORY_IDENTITY, visit_identity, sizeof(visit_identity));
}

static void start_visit(uint8_t *real) {
    int none = EARLY_STORY_NO_STEP;
    if (!atomic_compare_exchange_strong(&visit_baseline, &none, real[EARLY_STORY_RECORD_STEP])) return;
    memcpy(visit_identity, real + EARLY_STORY_IDENTITY, sizeof(visit_identity));
    atomic_store(&visit_record, real);
    atomic_store(&writers_logged, 0);
    printf("Early Story Online: following the host's story, step %d (own step %d)\n", early_story_session_host_step(),
           real[EARLY_STORY_RECORD_STEP]);
}

static void keep_own_step(uint8_t *real, int baseline) {
    if (!same_character(real)) return;
    atomic_store(&visit_record, real);
    if (real[EARLY_STORY_RECORD_STEP] == baseline) return;
    if (!(atomic_fetch_or(&writers_logged, 1u << DCP_EARLY_STORY_WRITERS) & (1u << DCP_EARLY_STORY_WRITERS)))
        printf("Early Story Online: own story step kept at %d (the game set %d while visiting)\n", baseline,
               real[EARLY_STORY_RECORD_STEP]);
    real[EARLY_STORY_RECORD_STEP] = (uint8_t)baseline;
}

static void end_visit(void) {
    const int baseline = atomic_exchange(&visit_baseline, EARLY_STORY_NO_STEP);
    if (baseline == EARLY_STORY_NO_STEP) return;
    uint8_t *record = atomic_exchange(&visit_record, NULL);
    if (record && same_character(record)) record[EARLY_STORY_RECORD_STEP] = (uint8_t)baseline;
    printf("Early Story Online: own story restored\n");
}

/* Whether the session has this character visiting; starts, keeps and ends the visit from the session alone. */
static int follow_session(uint8_t *real) {
    const int baseline = atomic_load(&visit_baseline);
    if (!early_story_visiting_for(baseline != EARLY_STORY_NO_STEP ? baseline : real[EARLY_STORY_RECORD_STEP])) {
        if (baseline != EARLY_STORY_NO_STEP) end_visit();
        return 0;
    }
    if (baseline == EARLY_STORY_NO_STEP) start_visit(real);
    else keep_own_step(real, baseline);
    return 1;
}

/* At the selector's `lea rax, [rax+rcx+0xa8]`: rax is the save's record base and rcx the lead slot's offset in it. */
void GUEST_HOOK_ABI early_story_selector_hook(GuestRegs *regs) {
    const uint64_t offset = regs->rcx;
    if (offset % EARLY_STORY_RECORD_SIZE || offset / EARLY_STORY_RECORD_SIZE >= EARLY_STORY_LOCAL_RECORDS) return;
    uint8_t *real = (uint8_t *)(uintptr_t)(regs->rax + offset + 0xa8);
    const int visiting = follow_session(real);
    early_story_note_own_step(real[EARLY_STORY_RECORD_STEP]);
    if (real_record_wanted || !visiting) return;
    uint8_t *copy = shadow[offset / EARLY_STORY_RECORD_SIZE];
    memcpy(copy, real, EARLY_STORY_RECORD_SIZE);
    copy[EARLY_STORY_RECORD_STEP] = (uint8_t)early_story_session_host_step();
    regs->rax = (uint64_t)(uintptr_t)copy;
    regs->skip = 1;
}

/* Replaces a call of the online wrapper that must keep the original threshold. */
void GUEST_HOOK_ABI early_story_original_online_hook(GuestRegs *regs) {
    const EarlyStorySelectorFn select = atomic_load(&selector_function);
    if (!select) return;
    ++real_record_wanted;
    const uint8_t *record = select(regs->rdi);
    --real_record_wanted;
    regs->rax = record && record[EARLY_STORY_RECORD_STEP] > EARLY_STORY_ORIGINAL_CHECK;
    regs->skip = 1;
}

/* At the call that copies the host's replicated record's story flag into the stage settings (rdi is that record). */
void GUEST_HOOK_ABI early_story_host_record_hook(GuestRegs *regs) {
    if (!regs->rdi) return;
    early_story_note_host_step(((const uint8_t *)(uintptr_t)regs->rdi)[EARLY_STORY_RECORD_STEP]);
}

/* The online wrapper's only accessor: rdi is the lead record, al the answer. */
void GUEST_HOOK_ABI early_story_online_check_hook(GuestRegs *regs) {
    const uint8_t *record = (const uint8_t *)(uintptr_t)regs->rdi;
    regs->rax = record && early_story_online_unlocked(record[EARLY_STORY_RECORD_STEP]);
    regs->skip = 1;
}

static uint64_t guest_u64(uint64_t at) {
    uint64_t value;
    memcpy(&value, (const void *)(uintptr_t)at, sizeof(value));
    return value;
}

static uint32_t guest_u32(uint64_t at) {
    uint32_t value;
    memcpy(&value, (const void *)(uintptr_t)at, sizeof(value));
    return value;
}

static uint8_t guest_u8(uint64_t at) {
    return *(const uint8_t *)(uintptr_t)at;
}

static uint64_t manager_of(uint64_t town) {
    const uint64_t context = guest_u64(town + EARLY_STORY_TOWN_CONTEXT);
    return context ? guest_u64(context + EARLY_STORY_CONTEXT_MANAGER) : 0;
}

/* The four checks the game's prompt function makes before it does anything: the same ones, read the same way. */
static int town_accepts_input(uint64_t manager) {
    const uint64_t mask_table = atomic_load(&control_mask_table);
    if (!manager || !mask_table) return 0;
    const uint64_t mode = guest_u64(manager + EARLY_STORY_MANAGER_MODE);
    const uint64_t scene = guest_u64(manager + EARLY_STORY_MANAGER_SCENE);
    const uint64_t queue = guest_u64(manager + EARLY_STORY_MANAGER_QUEUE);
    if (!mode || !scene || !queue) return 0;
    return guest_u8(mode + EARLY_STORY_OBJECT_STATE) == EARLY_STORY_MODE_TOWN &&
           (guest_u32(mode + EARLY_STORY_MODE_INPUT) & guest_u32(mask_table + EARLY_STORY_MASK_INPUT)) != 0 &&
           guest_u8(scene + EARLY_STORY_OBJECT_STATE) == EARLY_STORY_SCENE_PLAY &&
           guest_u8(queue + EARLY_STORY_OBJECT_STATE) == 0 &&
           guest_u64(queue + EARLY_STORY_QUEUE_END) == guest_u64(queue + EARLY_STORY_QUEUE_BEGIN);
}

static void run_prompt(void) {
    const EarlyStoryPromptFn prompt = atomic_load(&prompt_function);
    const int done = prompt((uint64_t)(uintptr_t)prompt_owner, EARLY_STORY_PROMPT_SLOT) != 0;
    const int was_open = prompt_open;
    prompt_open = !done && prompt_owner[EARLY_STORY_OWNER_STATES + EARLY_STORY_PROMPT_SLOT] != 0;
    prompt_settled |= done;
    if (prompt_open && !was_open) printf("Early Story Online: Hydeland online prompt shown\n");
    if (done) printf("Early Story Online: Hydeland online prompt finished\n");
}

static void open_town_prompt(void) {
    memset(prompt_owner, 0, sizeof(prompt_owner));
    const uint64_t vtable = atomic_load(&prompt_owner_vtable);
    memcpy(prompt_owner, &vtable, sizeof(vtable));
    memcpy(prompt_owner + EARLY_STORY_OWNER_SYSOBJ, &current_town, sizeof(current_town));
    const uint32_t changed = EARLY_STORY_SLOT1_CHANGED;
    memcpy(prompt_owner + EARLY_STORY_OWNER_FLAGS, &changed, sizeof(changed));
    owner_town = current_town;
    run_prompt();
}

static void arm_shortcut(int ready) {
    atomic_store(&shortcut_town_until, ready ? now_ms() + EARLY_STORY_SHORTCUT_LEASE_MS : 0);
}

void early_story_hint_configure(const EarlyStoryHintFunctions *functions) {
    hide_hint();
    hint_failed = 0;
    if (functions) hint_functions = *functions;
    else memset(&hint_functions, 0, sizeof(hint_functions));
}

void early_story_hint_set_portuguese(int portuguese) {
    hint_portuguese = portuguese != 0;
}

int early_story_hint_visible(void) {
    return hint_shown;
}

static uint64_t hint_window_of(uint64_t ui) {
    return guest_u64(ui + EARLY_STORY_UI_WINDOWS + (uint64_t)EARLY_STORY_HINT_WINDOW_TYPE * EARLY_STORY_UI_SLOT_SIZE);
}

static uint64_t hint_show_method(uint64_t window) {
    return guest_u64(guest_u64(window) + EARLY_STORY_HINT_SHOW_SLOT);
}

static int hint_show_is_the_games(uint64_t window) {
    static const uint8_t body[] = {0x89, 0xb7, 0x34, 0x06, 0x00, 0x00, 0x88, 0x97, 0x39, 0x06, 0x00, 0x00,
                                   0xbe, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x47, 0x50};
    const uint64_t method = hint_show_method(window);
    return method && !memcmp((const void *)(uintptr_t)method, body, sizeof(body));
}

static void hide_hint(void) {
    if (!hint_shown) return;
    hint_shown = 0;
    if (hint_window_of(hint_ui) == hint_window) hint_functions.hide(hint_window);
}

static void show_hint(uint64_t manager) {
    const EarlyStoryHintFunctions *game = &hint_functions;
    if (hint_failed || hint_blocked || !game->create || !game->text || !game->hide || !game->refresh) return;
    const uint64_t ui = guest_u64(manager + EARLY_STORY_MANAGER_UI);
    if (!ui || (hint_shown && hint_ui == ui && hint_window_of(ui) == hint_window)) return;
    const uint64_t window = game->create(ui, 0);
    if (!window || (game->verify_show && !hint_show_is_the_games(window))) {
        hint_failed = 1;
        return;
    }
    game->text(window, EARLY_STORY_HINT_MESSAGE, hint_portuguese ? "@#(2) Abrir Menu de Rede" : "@#(2) Open Network Menu");
    game->refresh(window + EARLY_STORY_HINT_TEXT_OBJECT);
    ((EarlyStoryHintShowFn)(uintptr_t)hint_show_method(window))(window, -1, 0);
    hint_window = window;
    hint_ui = ui;
    hint_shown = 1;
}

void early_story_online_set_room_wish(EarlyStoryRoomWishFn room_wish) {
    atomic_store(&room_wish_function, room_wish);
    room_wish_town = 0;
}

int early_story_host_room_wanted(void) {
    return early_story_hosting() && !early_story_visiting() && early_story_hosts_story_room(early_story_own_step());
}

int early_story_town_room_wanted(void) {
    return early_story_host_room_wanted() || early_story_story_room_guest();
}

/* The game asks for the room wish when a scene starts, before the access latch exists on the first arrival: asked again
 * once per town visit after it. */
static void wish_host_room(uint64_t manager) {
    const EarlyStoryRoomWishFn room_wish = atomic_load(&room_wish_function);
    if (!room_wish || room_wish_town == current_town || !early_story_host_room_wanted()) return;
    room_wish_town = current_town;
    room_wish(guest_u64(manager + EARLY_STORY_MANAGER_SCENE));
}

/* At the room wish's `or eax, 8; cmp eax, 9` on a town's location (eax): Castle Gate and Stables keep a room. */
void GUEST_HOOK_ABI early_story_room_wish_hook(GuestRegs *regs) {
    if (early_story_town_room_wanted()) regs->rax = EARLY_STORY_ROOM_WISH_GATE;
}

/* At the room info's store of the scene's location (ax; r13 is the scene): an early host anywhere in town reads as being
 * at the Castle Gate, the town location a friend's game accepts. */
void GUEST_HOOK_ABI early_story_room_info_hook(GuestRegs *regs) {
    const uint64_t scene = regs->r13;
    const uint32_t location = (uint16_t)regs->rax;
    if (!scene) return;
    const uint32_t type = guest_u32(scene + EARLY_STORY_AREA_TYPE);
    if (type == EARLY_STORY_AREA_TOWN && (location | 8) != 9 && early_story_host_room_wanted())
        regs->rax = (regs->rax & ~(uint64_t)0xffff) | EARLY_STORY_ROOM_WISH_GATE;
}

static int hydeland_shortcut_ready(uint64_t manager) {
    const uint64_t scene = guest_u64(manager + EARLY_STORY_MANAGER_SCENE);
    return guest_u32(scene + EARLY_STORY_AREA_TYPE) == EARLY_STORY_AREA_TOWN &&
           guest_u32(scene + EARLY_STORY_AREA_LOCATION) == EARLY_STORY_AREA_HYDELAND;
}

/* Every frame of the town update. An open prompt is kept running whatever the town's checks say. Otherwise, once the
 * town accepts input for a host with the setting on, online access is enabled for the run and the game's own prompt is
 * offered once; its own once-per-run flag keeps the Castle Gate and Stables from asking again. Hydeland's Network Menu
 * shortcut is armed from the frame after the prompt is out of the way. */
void GUEST_HOOK_ABI early_story_town_portal_hook(GuestRegs *regs) {
    current_town = regs->rdi;
    arm_shortcut(0);
    if (prompt_open) {
        if (owner_town != current_town) prompt_open = 0;
        else run_prompt();
        hide_hint();
        return;
    }
    const uint64_t manager = manager_of(current_town);
    if (!atomic_load(&prompt_function) || !early_story_active() || !early_story_hosting() || early_story_visiting() ||
        !town_accepts_input(manager)) {
        hide_hint();
        return;
    }
    if (!early_story_access_ready()) {
        early_story_grant_access();
        printf("Early Story Online: access enabled in Hydeland\n");
    }
    if (!prompt_settled && !guest_u8(manager + EARLY_STORY_MANAGER_PROMPT_SHOWN)) {
        open_town_prompt();
        hide_hint();
        return;
    }
    const int ready = hydeland_shortcut_ready(manager);
    arm_shortcut(ready);
    if (ready) wish_host_room(manager);
    if (ready && shortcut_armed(now_ms())) show_hint(manager);
    else hide_hint();
}

/* The director's test of the Menu button for one player (r13 is the player slot, rax the answer): player 1's Triangle
 * press, kept by the pad layer, counts as that press. The director then runs its own checks for open windows. */
void GUEST_HOOK_ABI early_story_menu_press_hook(GuestRegs *regs) {
    shortcut_requested = 0;
    if (regs->r13 != 0) return;
    hint_blocked = 0;
    const uint64_t now = now_ms();
    atomic_store(&shortcut_poll_until, now + EARLY_STORY_SHORTCUT_LEASE_MS);
    if (!shortcut_armed(now)) {
        hide_hint();
        return;
    }
    if (!take_shortcut_press(now)) return;
    shortcut_requested = 1;
    regs->rax = 1;
    regs->skip = 1;
}

/* The director's story-step check for a town menu: the shortcut does not wait for the character menu to unlock. */
void GUEST_HOOK_ABI early_story_menu_step_hook(GuestRegs *regs) {
    if (!shortcut_requested) return;
    regs->rax = 1;
    regs->skip = 1;
}

/* After the director chose the kind of the menu window it just created (rbx; r15 bit 0 is set only when it did). */
void GUEST_HOOK_ABI early_story_menu_kind_hook(GuestRegs *regs) {
    const int requested = shortcut_requested;
    shortcut_requested = 0;
    if (!requested || !(regs->r15 & 1) || !regs->rbx) return;
    const int32_t kind = EARLY_STORY_MENU_KIND_NETWORK;
    memcpy((void *)(uintptr_t)(regs->rbx + EARLY_STORY_MENU_KIND), &kind, sizeof(kind));
    hint_blocked = 1;
    hide_hint();
}

static void refuse_for_visitor(GuestRegs *regs, unsigned writer, const char *what) {
    if (!early_story_visiting()) return;
    atomic_fetch_add(&blocked_writes, 1);
    if (!(atomic_fetch_or(&writers_logged, 1u << writer) & (1u << writer)))
        printf("Early Story Online: %s not written while visiting (further ones not logged)\n", what);
    regs->rax = 0;
    regs->skip = 1;
}

void GUEST_HOOK_ABI early_story_advance_story_hook(GuestRegs *regs) { refuse_for_visitor(regs, 0, "story step"); }
void GUEST_HOOK_ABI early_story_tier_hook(GuestRegs *regs) { refuse_for_visitor(regs, 1, "difficulty tier"); }
void GUEST_HOOK_ABI early_story_talisman_hook(GuestRegs *regs) { refuse_for_visitor(regs, 2, "talisman"); }
void GUEST_HOOK_ABI early_story_flag_hook(GuestRegs *regs) { refuse_for_visitor(regs, 3, "story flag"); }

size_t early_story_online_stub_bytes(void) {
    return EARLY_STORY_HOOKS * guest_hook_stub_size();
}

static int call_at(const uint8_t *image, uint64_t size, uint32_t at, GuestHook *hook, const char *name,
                   GuestHookHandler handler) {
    if ((uint64_t)at + 5 > size || image[at] != 0xe8) return 0;
    *hook = (GuestHook){name, at, 5, {0}, handler};
    memcpy(hook->original, image + at, 5);
    return 1;
}

static int function_at(const uint8_t *image, uint64_t size, uint32_t at, GuestHook *hook, const char *name,
                       const uint8_t prologue[8], GuestHookHandler handler) {
    if ((uint64_t)at + 8 > size || memcmp(image + at, prologue, 8)) return 0;
    *hook = (GuestHook){name, at, 8, {0}, handler};
    memcpy(hook->original, prologue, 8);
    return 1;
}

static int contains(const uint8_t *image, uint32_t from, uint32_t length, const uint8_t *needle, size_t needle_size) {
    for (uint32_t i = 0; i + needle_size <= length; ++i)
        if (!memcmp(image + from + i, needle, needle_size)) return 1;
    return 0;
}

/* The prompt function opens with the four checks town_accepts_input repeats, and loads the input mask from a table. */
static int prompt_checks_match(const uint8_t *image, uint64_t size, uint32_t function) {
    static const uint8_t mode[] = {0x41, 0x80, 0x7c, 0x24, 0x5b, 0x03};
    static const uint8_t mask[] = {0x8b, 0x40, 0x04, 0x41, 0x85, 0x84, 0x24, 0x14, 0x05, 0x00, 0x00};
    static const uint8_t scene[] = {0x41, 0x80, 0x7e, 0x5b, 0x02};
    static const uint8_t queue[] = {0x4c, 0x8b, 0xbb, 0x08, 0x01, 0x00, 0x00};
    if ((uint64_t)function + EARLY_STORY_PROMPT_CHECKS_SPAN > size) return 0;
    const uint8_t *lea = image + function + EARLY_STORY_PROMPT_MASK_LEA;
    return lea[0] == 0x48 && lea[1] == 0x8d && lea[2] == 0x05 &&
           contains(image, function, EARLY_STORY_PROMPT_CHECKS_SPAN, mode, sizeof(mode)) &&
           contains(image, function, EARLY_STORY_PROMPT_CHECKS_SPAN, mask, sizeof(mask)) &&
           contains(image, function, EARLY_STORY_PROMPT_CHECKS_SPAN, scene, sizeof(scene)) &&
           contains(image, function, EARLY_STORY_PROMPT_CHECKS_SPAN, queue, sizeof(queue));
}

/* The press test is `mov esi, 0x23` (the Menu action) then a call with the pad object in rdi. */
static int menu_press_at(const uint8_t *image, uint64_t size, uint32_t at, GuestHook *hook) {
    static const uint8_t menu_action[8] = {0xbe, 0x23, 0x00, 0x00, 0x00, 0x48, 0x89, 0xc7};
    if (at < sizeof(menu_action) || memcmp(image + at - sizeof(menu_action), menu_action, sizeof(menu_action))) return 0;
    return call_at(image, size, at, hook, "menu press", early_story_menu_press_hook);
}

/* The step check is a call whose answer is tested before the stage type is read. */
static int menu_step_at(const uint8_t *image, uint64_t size, uint32_t at, GuestHook *hook) {
    static const uint8_t tested[4] = {0x84, 0xc0, 0x0f, 0x84};
    if ((uint64_t)at + 9 > size || memcmp(image + at + 5, tested, sizeof(tested))) return 0;
    return call_at(image, size, at, hook, "menu step", early_story_menu_step_hook);
}

/* The kind store ends the creation path; the branch over it is the test of the creation flag (r15b) 0x96 bytes back. */
static int menu_kind_at(const uint8_t *image, uint64_t size, uint32_t at, GuestHook *hook) {
    static const uint8_t kind_store[10] = {0xc7, 0x83, 0x10, 0x15, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
    static const uint8_t created_test[6] = {0x41, 0xf6, 0xc7, 0x01, 0x0f, 0x84};
    static const uint8_t window_load[3] = {0x48, 0x8b, 0x5d};
    static const uint8_t window_test[3] = {0x48, 0x85, 0xdb};
    if (at < EARLY_STORY_MENU_CREATED_BACK || (uint64_t)at + EARLY_STORY_MENU_KIND_HOOK_LENGTH > size) return 0;
    const uint8_t *test = image + at - EARLY_STORY_MENU_CREATED_BACK;
    int32_t over;
    memcpy(&over, test + sizeof(created_test), sizeof(over));
    if (memcmp(image + at - sizeof(kind_store), kind_store, sizeof(kind_store)) ||
        memcmp(test, created_test, sizeof(created_test)) ||
        (int64_t)at - EARLY_STORY_MENU_CREATED_BACK + (int64_t)sizeof(created_test) + 4 + over != (int64_t)at ||
        memcmp(image + at, window_load, sizeof(window_load)) || memcmp(image + at + 4, window_test, sizeof(window_test)))
        return 0;
    *hook = (GuestHook){"menu kind", at, EARLY_STORY_MENU_KIND_HOOK_LENGTH, {0}, early_story_menu_kind_hook};
    memcpy(hook->original, image + at, EARLY_STORY_MENU_KIND_HOOK_LENGTH);
    return 1;
}

/* The room wish's town branch: `or eax, 8; cmp eax, 9` on the location, in the function that asks the online wrapper. */
static int room_wish_at(const uint8_t *image, uint64_t size, const DcpEarlyStorySites *sites, GuestHook *hook) {
    static const uint8_t location_test[EARLY_STORY_ROOM_WISH_HOOK_LENGTH] = {0x83, 0xc8, 0x08, 0x83, 0xf8, 0x09};
    static const uint8_t location_load[6] = {0x8b, 0x83, 0xf0, 0x00, 0x00, 0x00};
    const uint32_t at = sites->room_wish_location;
    if (!sites->room_wish_function || at <= sites->room_wish_function || at < sizeof(location_load) ||
        (uint64_t)at + sizeof(location_test) > size || memcmp(image + at, location_test, sizeof(location_test)) ||
        memcmp(image + at - sizeof(location_load), location_load, sizeof(location_load)))
        return 0;
    *hook = (GuestHook){"room wish", at, EARLY_STORY_ROOM_WISH_HOOK_LENGTH, {0}, early_story_room_wish_hook};
    memcpy(hook->original, location_test, sizeof(location_test));
    return 1;
}

/* `mov word [r15+0x5c], ax` right after `movzx eax, word [r13+0xf0]`: the scene's location into the room info. */
static int room_info_at(const uint8_t *image, uint64_t size, uint32_t at, GuestHook *hook) {
    static const uint8_t store[EARLY_STORY_ROOM_INFO_HOOK_LENGTH] = {0x66, 0x41, 0x89, 0x47, 0x5c};
    static const uint8_t load[8] = {0x41, 0x0f, 0xb7, 0x85, 0xf0, 0x00, 0x00, 0x00};
    if (at < sizeof(load) || (uint64_t)at + sizeof(store) > size || memcmp(image + at, store, sizeof(store)) ||
        memcmp(image + at - sizeof(load), load, sizeof(load)))
        return 0;
    *hook = (GuestHook){"room info", at, EARLY_STORY_ROOM_INFO_HOOK_LENGTH, {0}, early_story_room_info_hook};
    memcpy(hook->original, store, sizeof(store));
    return 1;
}

int early_story_online_hooks(const uint8_t *image, uint64_t size, const DcpEarlyStorySites *sites,
                             GuestHook hooks[EARLY_STORY_HOOKS], const char **why) {
    const char *unused;
    if (!why) why = &unused;
    *why = "";
    if (!image || !sites) return *why = "no sites", 0;
    const uint32_t exit = sites->selector + EARLY_STORY_SELECTOR_EXIT;
    if ((uint64_t)exit + 8 > size || memcmp(image + exit, kSelectorExit, 8)) return *why = "lead selector", 0;
    hooks[0] = (GuestHook){"lead selector", exit, 8, {0}, early_story_selector_hook};
    memcpy(hooks[0].original, kSelectorExit, 8);
    if (!call_at(image, size, sites->hint_call, &hooks[1], "story hint", early_story_original_online_hook))
        return *why = "story hint call", 0;
    if (!call_at(image, size, sites->notice_call, &hooks[2], "network notice", early_story_original_online_hook))
        return *why = "network menu notice call", 0;
    if (!call_at(image, size, sites->host_record_call, &hooks[3], "host record", early_story_host_record_hook))
        return *why = "host record call", 0;
    static const char *const names[DCP_EARLY_STORY_WRITERS] = {"advance story", "difficulty tier", "talisman award",
                                                               "story flag"};
    static const GuestHookHandler handlers[DCP_EARLY_STORY_WRITERS] = {
        early_story_advance_story_hook, early_story_tier_hook, early_story_talisman_hook, early_story_flag_hook};
    for (size_t i = 0; i < DCP_EARLY_STORY_WRITERS; ++i) {
        if (!function_at(image, size, sites->writers[i], &hooks[4 + i], names[i],
                         i == DCP_EARLY_STORY_WRITERS - 1 ? kFlagWriter : kStandardWriter, handlers[i]))
            return *why = names[i], 0;
    }
    if (!function_at(image, size, sites->online_accessor, &hooks[8], "online check", kOnlineAccessor,
                     early_story_online_check_hook))
        return *why = "online check", 0;
    if (!call_at(image, size, sites->town_portal_call, &hooks[9], "hydeland update", early_story_town_portal_hook))
        return *why = "hydeland update call", 0;
    if (!prompt_checks_match(image, size, sites->prompt_function)) return *why = "prompt control checks", 0;
    if (!menu_press_at(image, size, sites->menu_press_call, &hooks[10])) return *why = "menu press call", 0;
    if (!menu_step_at(image, size, sites->menu_step_call, &hooks[11])) return *why = "menu step call", 0;
    if (!menu_kind_at(image, size, sites->menu_kind_store, &hooks[12])) return *why = "menu kind store", 0;
    if (!room_wish_at(image, size, sites, &hooks[13])) return *why = "room wish location", 0;
    if (!room_info_at(image, size, sites->room_info_location, &hooks[14])) return *why = "room info location", 0;
    return EARLY_STORY_HOOKS;
}

int early_story_online_install(uint8_t *image, uint64_t size, uint64_t entry, const DcpEarlyStorySites *sites,
                               uint8_t *stubs, size_t stubs_size) {
    GuestHook hooks[EARLY_STORY_HOOKS];
    const char *why;
    const int count = early_story_online_hooks(image, size, sites, hooks, &why);
    if (!count) {
        printf("Early Story Online: not active, the %s site does not match\n", why);
        return 0;
    }
    size_t bad = 0;
    const GuestHookStatus status =
        guest_hooks_install(image, size, entry, NULL, hooks, (size_t)count, stubs, stubs_size, &bad);
    if (status != GUEST_HOOK_OK) {
        printf("Early Story Online: not active, %s (%s)\n", guest_hook_status_text(status), hooks[bad].name);
        return 0;
    }
    early_story_online_set_selector((EarlyStorySelectorFn)(uintptr_t)(image + sites->selector));
    const uint8_t *lea = image + sites->prompt_function + EARLY_STORY_PROMPT_MASK_LEA;
    int32_t displacement;
    memcpy(&displacement, lea + 3, sizeof(displacement));
    early_story_online_set_prompt((EarlyStoryPromptFn)(uintptr_t)(image + sites->prompt_function),
                                  (uint64_t)(uintptr_t)(image + sites->prompt_owner_vtable),
                                  (uint64_t)(uintptr_t)(lea + 7 + displacement));
    early_story_online_set_room_wish((EarlyStoryRoomWishFn)(uintptr_t)(image + sites->room_wish_function));
    const int hint = sites->hint_create && sites->hint_text && sites->hint_hide && sites->hint_refresh;
    if (hint) {
        const EarlyStoryHintFunctions functions = {(EarlyStoryHintCreateFn)(uintptr_t)(image + sites->hint_create),
                                                   (EarlyStoryHintTextFn)(uintptr_t)(image + sites->hint_text),
                                                   (EarlyStoryHintWindowFn)(uintptr_t)(image + sites->hint_hide),
                                                   (EarlyStoryHintWindowFn)(uintptr_t)(image + sites->hint_refresh), 1};
        early_story_hint_configure(&functions);
    } else {
        early_story_hint_configure(NULL);
    }
    early_story_set_active(1);
    printf("Early Story Online: ready, Hydeland activation armed (prompt 0x%x), Network Menu on Triangle%s\n",
           sites->prompt_function, hint ? ", hint on the top help dialog" : ", no hint on this build");
    return 1;
}
