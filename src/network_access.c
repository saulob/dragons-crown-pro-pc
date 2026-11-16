#include "network_access.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

static _Atomic(int (*)(void)) online_checks[NETWORK_ACCESS_FEATURES];
static _Atomic(int (*)(void)) room_wishes[NETWORK_ACCESS_FEATURES];
static _Atomic(int (*)(void)) menu_steps[NETWORK_ACCESS_FEATURES];
static atomic_int installed_flag;

static _Atomic(NetworkRoomWishFn) room_wish;

void network_access_set_room_wish(NetworkRoomWishFn wish) {
    atomic_store(&room_wish, wish);
}

static _Atomic(NetworkRoomInfoObserver) room_info_observer;

void network_access_set_room_info_observer(NetworkRoomInfoObserver observer) {
    atomic_store(&room_info_observer, observer);
}

void network_access_room_info_built(uint64_t info) {
    const NetworkRoomInfoObserver observer = atomic_load(&room_info_observer);
    if (observer && info) observer(info);
}

int network_access_wish_room(uint64_t scene) {
    const NetworkRoomWishFn wish = atomic_load(&room_wish);
    if (!wish || !scene) return 0;
    wish(scene);
    return 1;
}

static atomic_int releasing;

int network_access_release_room(uint64_t scene) {
    atomic_store(&releasing, 1);
    const int asked = network_access_wish_room(scene);
    atomic_store(&releasing, 0);
    return asked;
}

int network_access_releasing(void) {
    return atomic_load(&releasing);
}

enum { SCENE_ONLINE_SLOT = 0x170, ONLINE_MATCHING = 0x16910, MATCHING_LEAVE_SLOT = 0x160 };

static _Atomic(NetworkRoomLeaveFn) room_leave_after;

void network_access_set_room_leave(NetworkRoomLeaveFn after_leave) {
    atomic_store(&room_leave_after, after_leave);
}

static uint64_t read_u64(uint64_t address) {
    uint64_t value;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof(value));
    return value;
}

static uint64_t virtual_slot(uint64_t object, int slot) {
    return read_u64(read_u64(object) + (uint64_t)slot);
}

int network_access_leave_room(uint64_t scene) {
    typedef uint64_t(GUEST_HOOK_ABI * ObjectFn)(uint64_t object);
    typedef void(GUEST_HOOK_ABI * LeaveFn)(uint64_t matching);
    const NetworkRoomLeaveFn after = atomic_load(&room_leave_after);
    if (!after || !scene) return 0;
    const uint64_t online = ((ObjectFn)(uintptr_t)virtual_slot(scene, SCENE_ONLINE_SLOT))(scene);
    const uint64_t matching = online ? read_u64(online + ONLINE_MATCHING) : 0;
    if (!matching) return 0;
    ((LeaveFn)(uintptr_t)virtual_slot(matching, MATCHING_LEAVE_SLOT))(matching);
    after(online);
    return 1;
}

void network_access_set(NetworkAccessFeature feature, const NetworkAccessRules *rules) {
    if ((unsigned)feature >= NETWORK_ACCESS_FEATURES) return;
    atomic_store(&online_checks[feature], rules ? rules->online_check : NULL);
    atomic_store(&room_wishes[feature], rules ? rules->room_wanted : NULL);
    atomic_store(&menu_steps[feature], rules ? rules->menu_step : NULL);
}

static int any(_Atomic(int (*)(void)) *predicates) {
    for (int i = 0; i < NETWORK_ACCESS_FEATURES; ++i) {
        int (*predicate)(void) = atomic_load(&predicates[i]);
        if (predicate && predicate()) return 1;
    }
    return 0;
}

int network_access_online_check(void) {
    return any(online_checks);
}

int network_access_room_wanted(void) {
    return !atomic_load(&releasing) && any(room_wishes);
}

int network_access_menu_step(void) {
    return any(menu_steps);
}

void network_access_set_installed(int installed) {
    atomic_store(&installed_flag, installed != 0);
}

int network_access_installed(void) {
    return atomic_load(&installed_flag);
}

static _Atomic uint64_t owner_lease, poll_lease, press_at;
static atomic_int owner, previous_down, hidden, quarantine;
static atomic_uint requests, menus_created;
static _Atomic(uint64_t (*)(void)) shortcut_clock;

void network_access_set_clock(uint64_t (*clock)(void)) {
    atomic_store(&shortcut_clock, clock);
}

uint64_t network_access_now_ms(void) {
    uint64_t (*clock)(void) = atomic_load(&shortcut_clock);
    if (clock) return clock();
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

void network_shortcut_reset(void) {
    atomic_store(&owner, NETWORK_SHORTCUT_NONE);
    atomic_store(&owner_lease, 0);
    atomic_store(&poll_lease, 0);
    atomic_store(&press_at, 0);
    atomic_store(&previous_down, 0);
    atomic_store(&hidden, 0);
    atomic_store(&quarantine, 0);
}

int network_shortcut_arm(NetworkShortcutOwner who, int ready) {
    const int current = atomic_load(&owner);
    if (!ready) {
        if (current == (int)who) {
            atomic_store(&owner, NETWORK_SHORTCUT_NONE);
            atomic_store(&owner_lease, 0);
        }
        return 0;
    }
    const int live = current != NETWORK_SHORTCUT_NONE && network_access_now_ms() < atomic_load(&owner_lease);
    if (live && current != (int)who && who != NETWORK_SHORTCUT_EARLY_STORY) return 0;
    atomic_store(&owner, (int)who);
    atomic_store(&owner_lease, network_access_now_ms() + NETWORK_SHORTCUT_LEASE_MS);
    return 1;
}

NetworkShortcutOwner network_shortcut_owner(void) {
    const int current = atomic_load(&owner);
    return current != NETWORK_SHORTCUT_NONE && network_access_now_ms() < atomic_load(&owner_lease)
               ? (NetworkShortcutOwner)current
               : NETWORK_SHORTCUT_NONE;
}

void network_shortcut_poll(void) {
    atomic_store(&poll_lease, network_access_now_ms() + NETWORK_SHORTCUT_LEASE_MS);
}

static int armed_at(uint64_t now) {
    return now < atomic_load(&owner_lease) && now < atomic_load(&poll_lease);
}

int network_shortcut_armed(void) {
    return armed_at(network_access_now_ms());
}

int network_shortcut_pad_filter(int triangle_down) {
    const int was_down = atomic_exchange(&previous_down, triangle_down != 0);
    if (!triangle_down) {
        atomic_store(&hidden, 0);
        return 0;
    }
    const uint64_t now = network_access_now_ms();
    if (armed_at(now)) {
        if (!was_down) atomic_store(&press_at, now ? now : 1);
        atomic_store(&hidden, 1);
    }
    return atomic_load(&hidden);
}

void network_menu_quarantine_begin(void) {
    atomic_store(&quarantine, 1);
}

int network_menu_quarantine_active(void) {
    return atomic_load(&quarantine);
}

int network_menu_pad_hold(int any_button_down) {
    if (!atomic_load(&quarantine)) return 0;
    if (any_button_down) return 1;
    atomic_store(&quarantine, 0);
    return 0;
}

int network_shortcut_take_press(void) {
    const uint64_t pressed_at = atomic_exchange(&press_at, 0);
    const int taken = pressed_at && network_access_now_ms() - pressed_at <= NETWORK_SHORTCUT_PRESS_MS;
    if (taken) atomic_fetch_add(&requests, 1);
    return taken;
}

unsigned network_shortcut_requests(void) {
    return atomic_load(&requests);
}

void network_access_menu_created(void) {
    atomic_fetch_add(&menus_created, 1);
}

unsigned network_access_menus_created(void) {
    return atomic_load(&menus_created);
}

enum { MENU_WINDOW_PLAYER = 0xf8, MENU_WINDOW_INIT = 0x100, MENU_WINDOW_KIND = 0x1510, MENU_KIND_NETWORK = 2 };
enum { CREATE_CALL_BEFORE_KIND_STORE = 0x85 };

static _Atomic(NetworkMenuCreateFn) menu_create;

uint64_t network_menu_create_function(const unsigned char *image, uint64_t size, uint64_t kind_store) {
    static const unsigned char kPadArgument[] = {0x44, 0x89, 0xe6};
    static const unsigned char kAfterCreate[] = {0x48, 0x89, 0xc3, 0x44, 0x89, 0xab, 0xf8, 0x00, 0x00, 0x00, 0x48,
                                                 0x89, 0xdf, 0x48, 0x8b, 0x03, 0xff, 0x90, 0x00, 0x01, 0x00, 0x00};
    static const unsigned char kCreate[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x41,
                                            0x89, 0xf6, 0xbe, 0x0e, 0x00, 0x00, 0x00, 0xe8};
    if (kind_store < CREATE_CALL_BEFORE_KIND_STORE + sizeof(kPadArgument) || kind_store > size) return 0;
    const uint64_t call = kind_store - CREATE_CALL_BEFORE_KIND_STORE;
    if (image[call] != 0xe8 || memcmp(image + call - sizeof(kPadArgument), kPadArgument, sizeof(kPadArgument)) ||
        memcmp(image + call + 5, kAfterCreate, sizeof(kAfterCreate)))
        return 0;
    int32_t displacement;
    memcpy(&displacement, image + call + 1, sizeof(displacement));
    const int64_t target = (int64_t)call + 5 + displacement;
    if (target <= 0 || (uint64_t)target + sizeof(kCreate) > size || memcmp(image + target, kCreate, sizeof(kCreate)))
        return 0;
    return (uint64_t)target;
}

void network_menu_set_create(NetworkMenuCreateFn create) {
    atomic_store(&menu_create, create);
}

int network_menu_request_available(void) {
    return atomic_load(&menu_create) != NULL;
}

void network_menu_window_network(uint64_t window) {
    const int32_t kind = MENU_KIND_NETWORK;
    memcpy((void *)(uintptr_t)(window + MENU_WINDOW_KIND), &kind, sizeof(kind));
    network_access_menu_created();
}

uint64_t network_menu_request(uint64_t ui) {
    const NetworkMenuCreateFn create = atomic_load(&menu_create);
    if (!create || !ui) return 0;
    const uint64_t window = create(ui, 0);
    if (!window) return 0;
    const int32_t player = 0;
    memcpy((void *)(uintptr_t)(window + MENU_WINDOW_PLAYER), &player, sizeof(player));
    uint64_t vtable, init;
    memcpy(&vtable, (const void *)(uintptr_t)window, sizeof(vtable));
    memcpy(&init, (const void *)(uintptr_t)(vtable + MENU_WINDOW_INIT), sizeof(init));
    ((void(GUEST_HOOK_ABI *)(uint64_t))(uintptr_t)init)(window);
    network_menu_window_network(window);
    return window;
}
