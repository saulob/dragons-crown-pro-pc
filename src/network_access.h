/* What features other than Early Story ask of the online hooks Early Story Online installs (early_story_online.h): a
 * grant for the game's online check, a room kept in town, the scene director's step check, and the one Network Menu
 * shortcut they share. With nothing registered those hooks behave as Early Story alone has them. */
#ifndef DCP_NETWORK_ACCESS_H
#define DCP_NETWORK_ACCESS_H

#include <stdint.h>

#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Player 1's Triangle as the Network Menu shortcut. One feature owns it at a time. While it is armed the pad layer hides
 * a press from the game and keeps it for the scene director's Menu poll, and a button still held when it disarms stays
 * hidden until released. It is armed while its owner asks again each frame and its request point ran recently: the
 * scene director's Menu poll for Early Story, the Boss Battle page for Boss Battle. */
typedef enum { NETWORK_SHORTCUT_NONE, NETWORK_SHORTCUT_EARLY_STORY, NETWORK_SHORTCUT_BOSS_BATTLE } NetworkShortcutOwner;

#define NETWORK_SHORTCUT_LEASE_MS 150
#define NETWORK_SHORTCUT_PRESS_MS 250

/* The shortcut's clock in milliseconds; NULL restores the real one (tests). */
void network_access_set_clock(uint64_t (*clock)(void));
uint64_t network_access_now_ms(void);
void network_shortcut_reset(void);
/* `owner` is ready (1) or not (0) this frame: 1 when it now owns the shortcut. Early Story takes it from Boss Battle, a
 * Boss Battle never takes it from a live Early Story. */
int network_shortcut_arm(NetworkShortcutOwner owner, int ready);
NetworkShortcutOwner network_shortcut_owner(void);
/* The owner's request point ran: the director polled player 1's Menu press, or the Boss Battle page ran. */
void network_shortcut_poll(void);
int network_shortcut_armed(void);
/* The pad layer: 1 while the button must be hidden from the game. */
int network_shortcut_pad_filter(int triangle_down);
/* After the Network Menu closed: the pad layer hides player 1's buttons from the game until all are released, so the
 * button that closed it is not also the page's. */
void network_menu_quarantine_begin(void);
int network_menu_pad_hold(int any_button_down);
int network_menu_quarantine_active(void);
/* A press kept while armed, once. */
int network_shortcut_take_press(void);
/* Presses the director took for a Network Menu, and Network Menus created for them. */
unsigned network_shortcut_requests(void);
void network_access_menu_created(void);
unsigned network_access_menus_created(void);

/* The game's menu window creation (ui, pad) that the scene director calls once its player checks passed, found from
 * the director's kind store; 0 unless it is exactly the expected call. */
typedef uint64_t(GUEST_HOOK_ABI *NetworkMenuCreateFn)(uint64_t ui, uint32_t pad);
uint64_t network_menu_create_function(const unsigned char *image, uint64_t size, uint64_t kind_store);
void network_menu_set_create(NetworkMenuCreateFn create);
int network_menu_request_available(void);
/* Makes `window`, a menu window the game just created, the Network Menu. */
void network_menu_window_network(uint64_t window);
/* The director's menu request for player 1, without its player checks: the game's window for pad 0 and player 0,
 * initialized and made the Network Menu. The window, or 0. */
uint64_t network_menu_request(uint64_t ui);

typedef enum { NETWORK_ACCESS_BOSS_BATTLE, NETWORK_ACCESS_FEATURES } NetworkAccessFeature;

typedef struct {
    int (*online_check)(void); /* the online check passes for the lead character */
    int (*room_wanted)(void);  /* the town keeps a room, as at the Castle Gate */
    int (*menu_step)(void);    /* the director's step check passes */
} NetworkAccessRules;

/* NULL removes the feature's rules. */
void network_access_set(NetworkAccessFeature feature, const NetworkAccessRules *rules);
int network_access_online_check(void);
int network_access_room_wanted(void);
int network_access_menu_step(void);

/* The game's room wish, which Early Story's town asks once per visit: a feature that keeps a room outside the town's own
 * arrival asks it with the scene whenever it wants its room made. 0 when the hooks that found it are not installed. */
typedef void(GUEST_HOOK_ABI *NetworkRoomWishFn)(uint64_t scene);
void network_access_set_room_wish(NetworkRoomWishFn room_wish);
int network_access_wish_room(uint64_t scene);
/* The same wish asked with no feature wanting a room, so the game leaves the one it is in unless its own place keeps it. */
int network_access_release_room(uint64_t scene);
int network_access_releasing(void);
/* The game's own room leave, as its room wish and Network Menu make it wherever the player is: the Matching2 object's
 * leave on the scene's online object, then the call after it (`after_leave`, located per build). 0 when unavailable. */
typedef void(GUEST_HOOK_ABI *NetworkRoomLeaveFn)(uint64_t online);
void network_access_set_room_leave(NetworkRoomLeaveFn after_leave);
int network_access_leave_room(uint64_t scene);

/* The room info builder finished (its host fields are set): a feature that watches it is told the info's address. */
typedef void (*NetworkRoomInfoObserver)(uint64_t info);
void network_access_set_room_info_observer(NetworkRoomInfoObserver observer);
void network_access_room_info_built(uint64_t info);

/* The hooks are installed: the Network Menu shortcut can work. */
void network_access_set_installed(int installed);
int network_access_installed(void);

#ifdef __cplusplus
}
#endif

#endif
