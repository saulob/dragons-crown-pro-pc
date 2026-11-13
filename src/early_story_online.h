/* Native hooks for Early Story Online (docs/ONLINE.md). Installed for every online player so a guest can join with the
 * setting off; all sites install together or none, leaving the game as shipped (sites: feature_sites.h). */
#ifndef DCP_EARLY_STORY_ONLINE_H
#define DCP_EARLY_STORY_ONLINE_H

#include <stddef.h>
#include <stdint.h>

#include "early_story_session.h"
#include "feature_sites.h"
#include "guest_hook.h"

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
#define EARLY_STORY_TOWN_CONTEXT 0x998        /* town object -> context object */
#define EARLY_STORY_CONTEXT_MANAGER 0x20      /* context object -> game manager */
#define EARLY_STORY_MANAGER_PROMPT_SHOWN 0x14d /* game manager: set once the prompt has been shown this run */
#define EARLY_STORY_OWNER_SIZE 0xb00          /* the prompt owner objects are 0xa78 bytes */
#define EARLY_STORY_OWNER_SYSOBJ 0x1e0        /* owner -> the system object it belongs to */
#define EARLY_STORY_OWNER_FLAGS 0x50          /* owner: slot change flags, 0x800 << slot */
#define EARLY_STORY_OWNER_STATES 0x5b         /* owner: one state byte per slot */
#define EARLY_STORY_PROMPT_SLOT 2
#define EARLY_STORY_SLOT1_CHANGED 0x1000

/* The prompt function's own control checks (game manager fields), repeated by the town trigger. */
#define EARLY_STORY_MANAGER_MODE 0xb8
#define EARLY_STORY_MANAGER_SCENE 0x88
#define EARLY_STORY_MANAGER_QUEUE 0x108
#define EARLY_STORY_OBJECT_STATE 0x5b
#define EARLY_STORY_MODE_TOWN 3
#define EARLY_STORY_SCENE_PLAY 2
#define EARLY_STORY_MODE_INPUT 0x514
#define EARLY_STORY_MASK_INPUT 4
#define EARLY_STORY_QUEUE_BEGIN 0x78
#define EARLY_STORY_QUEUE_END 0x80
#define EARLY_STORY_PROMPT_MASK_LEA 0x65
#define EARLY_STORY_MANAGER_UI 0xa8       /* game manager: the window manager */
#define EARLY_STORY_UI_WINDOWS 0x78       /* window manager: one 0x20 byte slot per window type, from type 1 */
#define EARLY_STORY_UI_SLOT_SIZE 0x20
#define EARLY_STORY_HINT_WINDOW_TYPE 0x28 /* the top help dialog, which Hydeland leaves unused */
#define EARLY_STORY_HINT_TEXT_OBJECT 0x3e0
#define EARLY_STORY_HINT_SHOW_SLOT 0x188
#define EARLY_STORY_HINT_MESSAGE 0x432 /* SystemMsg "%s" */
#define EARLY_STORY_PROMPT_CHECKS_SPAN 0xc0

/* The scene director opens the town menu and the Network Menu as one window kind-switched by location. */
#define EARLY_STORY_AREA_TYPE 0x108 /* scene object: stage type, 9 in a town */
#define EARLY_STORY_AREA_LOCATION 0xf0
#define EARLY_STORY_AREA_TOWN 9
#define EARLY_STORY_AREA_HYDELAND 0
#define EARLY_STORY_MENU_KIND 0x1510
#define EARLY_STORY_MENU_KIND_NETWORK 2
#define EARLY_STORY_PAD_TRIANGLE 0x1000 /* libScePad's button bit */
#define EARLY_STORY_SHORTCUT_LEASE_MS 150
#define EARLY_STORY_SHORTCUT_PRESS_MS 250
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
typedef uint8_t (GUEST_HOOK_ABI *EarlyStoryPromptFn)(uint64_t owner, int slot);

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
typedef uint64_t(GUEST_HOOK_ABI *EarlyStoryHintCreateFn)(uint64_t ui, int init);
typedef void(GUEST_HOOK_ABI *EarlyStoryHintTextFn)(uint64_t window, int message, ...);
typedef void(GUEST_HOOK_ABI *EarlyStoryHintWindowFn)(uint64_t object);
typedef void(GUEST_HOOK_ABI *EarlyStoryHintShowFn)(uint64_t window, int duration, int flag);

typedef struct {
    EarlyStoryHintCreateFn create;
    EarlyStoryHintTextFn text;
    EarlyStoryHintWindowFn hide, refresh;
    int verify_show; /* the window's show method must be the game's (off only where tests fake it) */
} EarlyStoryHintFunctions;

void early_story_hint_configure(const EarlyStoryHintFunctions *functions);
void early_story_hint_set_portuguese(int portuguese);
int early_story_hint_visible(void);

size_t early_story_online_stub_bytes(void);

/* The hooks for `sites` on `image`, in the order selector, hint call, notice call, host record, the four writers, the
 * online check, the town's every-frame call, the director's Menu press test, step check and kind store, the room wish's
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
