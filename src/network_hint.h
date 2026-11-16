/* The Network Menu hint on the game's top help dialog, shared by Early Story Online (Hydeland) and Boss Battle Online
 * (the Boss Battle page). One owner shows it at a time; a feature hides only what it showed. */
#ifndef DCP_NETWORK_HINT_H
#define DCP_NETWORK_HINT_H

#include <stdint.h>

#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETWORK_HINT_MANAGER_UI 0xa8 /* game manager: the window manager */
#define NETWORK_HINT_UI_WINDOWS 0x78 /* window manager: one 0x20 byte slot per window type, from type 1 */
#define NETWORK_HINT_UI_SLOT_SIZE 0x20
#define NETWORK_HINT_WINDOW_TYPE 0x28 /* the top help dialog */
#define NETWORK_HINT_TEXT_OBJECT 0x3e0
#define NETWORK_HINT_SHOW_SLOT 0x188
#define NETWORK_HINT_MESSAGE 0x432 /* SystemMsg "%s" */

/* The logical Triangle glyph: the game draws it as Y or Triangle by the controller. */
#define NETWORK_HINT_GLYPH "@#(2) "
#define NETWORK_HINT_TEXT_EN "Open Network Menu"
#define NETWORK_HINT_TEXT_PT "Abrir Menu de Rede"

/* The game's top help dialog functions: the window of a type (created on first use), its formatted text, its hide, and
 * the text object's reveal reset. All four, or no hint is shown. */
typedef uint64_t(GUEST_HOOK_ABI *NetworkHintCreateFn)(uint64_t ui, int init);
typedef void(GUEST_HOOK_ABI *NetworkHintTextFn)(uint64_t window, int message, ...);
typedef void(GUEST_HOOK_ABI *NetworkHintWindowFn)(uint64_t object);
typedef void(GUEST_HOOK_ABI *NetworkHintShowFn)(uint64_t window, int duration, int flag);

typedef struct {
    NetworkHintCreateFn create;
    NetworkHintTextFn text;
    NetworkHintWindowFn hide, refresh;
    int verify_show; /* the window's show method must be the game's (off only where tests fake it) */
} NetworkHintFunctions;

typedef enum { NETWORK_HINT_EARLY_STORY = 1, NETWORK_HINT_BOSS_BATTLE, NETWORK_HINT_OWNERS } NetworkHintOwner;

void network_hint_configure(const NetworkHintFunctions *functions);
void network_hint_set_portuguese(int portuguese);
int network_hint_shown_by(NetworkHintOwner owner);
/* Shows the hint on `manager`'s window manager unless another owner has it or it failed before. */
void network_hint_show(NetworkHintOwner owner, uint64_t manager);
void network_hint_hide(NetworkHintOwner owner);
void network_hint_hide_any(void);

#ifdef __cplusplus
}
#endif

#endif
