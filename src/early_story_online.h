/* Native hooks for Early Story Online (docs/ONLINE.md). Installed for every online player so a guest can join with the
 * setting off; all sites install together or none, leaving the game as shipped (sites: feature_sites.h). */
#ifndef DCP_EARLY_STORY_ONLINE_H
#define DCP_EARLY_STORY_ONLINE_H

#include <stddef.h>
#include <stdint.h>

#include "early_story_session.h"
#include "feature_sites.h"
#include "guest_hook.h"
#include "hydeland_town.h"
#include "network_access.h"
#include "network_hint.h"
#include "online_prompt.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EARLY_STORY_HOOKS 15
#define EARLY_STORY_RECORD_SIZE 0x23b0
#define EARLY_STORY_RECORD_STEP 0x6f
#define EARLY_STORY_LOCAL_RECORDS 4
#define EARLY_STORY_IDENTITY 0x104 /* the character fields the game compares to match a record (US 0x82e30) */
#define EARLY_STORY_IDENTITY_SIZE 0x12
#define EARLY_STORY_SELECTOR_EXIT 0x74 /* the selector's `lea rax, [rax+rcx+0xa8]`, from its start */
#define EARLY_STORY_ORIGINAL_CHECK 0x2c

/* The guest layouts the Hydeland trigger reads, the same on every supported build. */
#define EARLY_STORY_TOWN_CONTEXT HYDELAND_TOWN_CONTEXT
#define EARLY_STORY_CONTEXT_MANAGER HYDELAND_CONTEXT_MANAGER
#define EARLY_STORY_MANAGER_PROMPT_SHOWN ONLINE_PROMPT_RUN_FLAG
#define EARLY_STORY_OWNER_SIZE ONLINE_PROMPT_OWNER_SIZE
#define EARLY_STORY_OWNER_SYSOBJ ONLINE_PROMPT_OWNER_SYSOBJ
#define EARLY_STORY_OWNER_FLAGS ONLINE_PROMPT_OWNER_FLAGS
#define EARLY_STORY_OWNER_STATES ONLINE_PROMPT_OWNER_STATES
#define EARLY_STORY_PROMPT_SLOT ONLINE_PROMPT_SLOT
#define EARLY_STORY_SLOT1_CHANGED ONLINE_PROMPT_SLOT1_CHANGED

#define EARLY_STORY_MANAGER_MODE HYDELAND_MANAGER_MODE
#define EARLY_STORY_MANAGER_SCENE HYDELAND_MANAGER_SCENE
#define EARLY_STORY_MANAGER_QUEUE HYDELAND_MANAGER_QUEUE
#define EARLY_STORY_OBJECT_STATE HYDELAND_OBJECT_STATE
#define EARLY_STORY_MODE_TOWN HYDELAND_MODE_TOWN
#define EARLY_STORY_SCENE_PLAY HYDELAND_SCENE_PLAY
#define EARLY_STORY_MODE_INPUT HYDELAND_MODE_INPUT
#define EARLY_STORY_MASK_INPUT HYDELAND_MASK_INPUT
#define EARLY_STORY_QUEUE_BEGIN HYDELAND_QUEUE_BEGIN
#define EARLY_STORY_QUEUE_END HYDELAND_QUEUE_END
#define EARLY_STORY_PROMPT_MASK_LEA HYDELAND_PROMPT_MASK_LEA
#define EARLY_STORY_MANAGER_UI NETWORK_HINT_MANAGER_UI
#define EARLY_STORY_UI_WINDOWS NETWORK_HINT_UI_WINDOWS
#define EARLY_STORY_UI_SLOT_SIZE NETWORK_HINT_UI_SLOT_SIZE
#define EARLY_STORY_HINT_WINDOW_TYPE NETWORK_HINT_WINDOW_TYPE
#define EARLY_STORY_HINT_TEXT_OBJECT NETWORK_HINT_TEXT_OBJECT
#define EARLY_STORY_HINT_SHOW_SLOT NETWORK_HINT_SHOW_SLOT
#define EARLY_STORY_HINT_MESSAGE NETWORK_HINT_MESSAGE
#define EARLY_STORY_PROMPT_CHECKS_SPAN HYDELAND_PROMPT_CHECKS_SPAN

/* The scene director opens the town menu and the Network Menu as one window kind-switched by location. */
#define EARLY_STORY_AREA_TYPE HYDELAND_AREA_TYPE
#define EARLY_STORY_AREA_LOCATION HYDELAND_AREA_LOCATION
#define EARLY_STORY_AREA_TOWN HYDELAND_AREA_TOWN
#define EARLY_STORY_AREA_HYDELAND HYDELAND_AREA_STREET
#define EARLY_STORY_MENU_KIND 0x1510
#define EARLY_STORY_MENU_KIND_NETWORK 2
#define EARLY_STORY_PAD_TRIANGLE 0x1000 /* libScePad's button bit */
#define EARLY_STORY_SHORTCUT_LEASE_MS NETWORK_SHORTCUT_LEASE_MS
#define EARLY_STORY_SHORTCUT_PRESS_MS NETWORK_SHORTCUT_PRESS_MS
#define EARLY_STORY_MENU_CREATED_BACK 0x96 /* from the kind store hook back to the director's creation flag test */
#define EARLY_STORY_MENU_KIND_HOOK_LENGTH 7
#define EARLY_STORY_ROOM_WISH_HOOK_LENGTH 6
#define EARLY_STORY_ROOM_WISH_GATE 1 /* the Castle Gate: a town location that keeps a room and that friends can join */
#define EARLY_STORY_ROOM_INFO_HOOK_LENGTH 5

/* The selector as the loaded game exposes it: the facade object in, the lead character record out (0 when none). */
typedef const uint8_t *(GUEST_HOOK_ABI *EarlyStorySelectorFn)(uint64_t facade);

void GUEST_HOOK_ABI early_story_selector_hook(GuestRegs *regs);
void GUEST_HOOK_ABI early_story_original_online_hook(GuestRegs *regs);
void GUEST_HOOK_ABI early_story_host_record_hook(GuestRegs *regs);
void GUEST_HOOK_ABI early_story_advance_story_hook(GuestRegs *regs);
void GUEST_HOOK_ABI early_story_tier_hook(GuestRegs *regs);
void GUEST_HOOK_ABI early_story_talisman_hook(GuestRegs *regs);
void GUEST_HOOK_ABI early_story_flag_hook(GuestRegs *regs);
void GUEST_HOOK_ABI early_story_online_check_hook(GuestRegs *regs);

/* The game's prompt function: the owner object and the slot whose state it runs, 1 once it is finished. */
typedef OnlinePromptFn EarlyStoryPromptFn;

void GUEST_HOOK_ABI early_story_town_portal_hook(GuestRegs *regs);
void GUEST_HOOK_ABI early_story_menu_press_hook(GuestRegs *regs);
void GUEST_HOOK_ABI early_story_menu_step_hook(GuestRegs *regs);
void GUEST_HOOK_ABI early_story_menu_kind_hook(GuestRegs *regs);
void GUEST_HOOK_ABI early_story_room_wish_hook(GuestRegs *regs);
void GUEST_HOOK_ABI early_story_room_info_hook(GuestRegs *regs);

/* The director's room wish: from the scene, it tells the network layer whether the player should be in a room. */
typedef void(GUEST_HOOK_ABI *EarlyStoryRoomWishFn)(uint64_t director);
void early_story_online_set_room_wish(EarlyStoryRoomWishFn room_wish);
/* An early-story host keeps a room in every town location, as the game does at the Castle Gate. */
int early_story_host_room_wanted(void);
/* The town keeps the room: for that host, and for any guest seated in a story room until it leaves. */
int early_story_town_room_wanted(void);

/* The pad layer's side of the Network Menu shortcut (player 1, Triangle): true while the button must be hidden from the
 * game. While the shortcut is armed a press is kept for the scene director's Menu path instead of reaching the game,
 * and a button still held when the shortcut disarms stays hidden until released. */
int early_story_shortcut_pad_filter(int triangle_down);
/* The shortcut's clock in milliseconds; NULL restores the real one (tests). */
void early_story_shortcut_set_clock(uint64_t (*clock)(void));

void early_story_online_set_selector(EarlyStorySelectorFn selector);
void early_story_online_set_prompt(EarlyStoryPromptFn prompt, uint64_t owner_vtable, uint64_t control_mask_address);
/* The Hydeland prompt is on screen. */
int early_story_town_prompt_open(void);
void early_story_town_prompt_reset(void);

/* How many progression writes were refused for a visiting guest since startup. */
uint32_t early_story_blocked_writes(void);

/* The game's top help dialog functions: the window of a type (created on first use), its formatted text, its hide, and
 * the text object's reveal reset. All four, or the hint is left out and the shortcut still works. */
typedef NetworkHintCreateFn EarlyStoryHintCreateFn;
typedef NetworkHintTextFn EarlyStoryHintTextFn;
typedef NetworkHintWindowFn EarlyStoryHintWindowFn;
typedef NetworkHintShowFn EarlyStoryHintShowFn;
typedef NetworkHintFunctions EarlyStoryHintFunctions;

void early_story_hint_configure(const EarlyStoryHintFunctions *functions);
void early_story_hint_set_portuguese(int portuguese);
int early_story_hint_visible(void);

size_t early_story_online_stub_bytes(void);

/* The hooks for `sites` on `image`, in the order selector, hint call, notice call, host record, the four writers, the
 * online check, the town's every-frame call (the shared hydeland_town_hook), the director's Menu press test, step check and kind store, the room wish's
 * location test, then the room info's location store.
 * The number of hooks when every site holds the verified bytes; 0 otherwise, with `why` naming the first that did not. */
int early_story_online_hooks(const uint8_t *image, uint64_t size, const DcpEarlyStorySites *sites,
                             GuestHook hooks[EARLY_STORY_HOOKS], const char **why);

/* Installs everything or nothing: 1 when the hooks are in and the session rules are active. */
int early_story_online_install(uint8_t *image, uint64_t size, uint64_t entry, const DcpEarlyStorySites *sites,
                               uint8_t *stubs, size_t stubs_size);

#ifdef __cplusplus
}
#endif

#endif
