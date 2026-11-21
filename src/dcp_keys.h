/* Keyboard bindings: the actions, their defaults and the saved key names, shared by the launcher, the runtime pad
 * and the video core. Keys are SDL scancodes, saved as the shadPS4 names (space, lshift, f10, pgup...) or SDL's own
 * lowercase names for the rest. */
#ifndef DCP_KEYS_H
#define DCP_KEYS_H

#include <stddef.h>
#include "dcp_trackpad.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DCP_KEY_LEFT_STICK_UP, DCP_KEY_LEFT_STICK_DOWN, DCP_KEY_LEFT_STICK_LEFT, DCP_KEY_LEFT_STICK_RIGHT,
    DCP_KEY_RIGHT_STICK_UP, DCP_KEY_RIGHT_STICK_DOWN, DCP_KEY_RIGHT_STICK_LEFT, DCP_KEY_RIGHT_STICK_RIGHT,
    DCP_KEY_CROSS, DCP_KEY_CIRCLE, DCP_KEY_SQUARE, DCP_KEY_TRIANGLE,
    DCP_KEY_L1, DCP_KEY_R1, DCP_KEY_L2, DCP_KEY_R2, DCP_KEY_L3, DCP_KEY_R3,
    DCP_KEY_DPAD_UP, DCP_KEY_DPAD_DOWN, DCP_KEY_DPAD_LEFT, DCP_KEY_DPAD_RIGHT,
    DCP_KEY_OPTIONS, DCP_KEY_TOUCHPAD_LEFT, DCP_KEY_TOUCHPAD_RIGHT,
    DCP_KEY_HOTKEY_CHEATS, DCP_KEY_HOTKEY_WINDOW_MODE, DCP_KEY_HOTKEY_COMMS_WHEEL,
    DCP_KEY_COUNT
} DcpKeyAction;

#define DCP_KEY_FIRST_HOTKEY DCP_KEY_HOTKEY_CHEATS

typedef struct {
    const char *ini_key; /* the line in dcp.ini */
    const char *label;   /* the launcher's row */
    int default_scancode;
} DcpKeyInfo;

extern const DcpKeyInfo dcp_key_actions[DCP_KEY_COUNT];

typedef struct { int scancode[DCP_KEY_COUNT]; } DcpKeyMap;

void dcp_keys_defaults(DcpKeyMap *map);

/* The scancode a saved name stands for (case-insensitive), or 0 when it names no bindable key. */
int dcp_key_from_name(const char *name);
/* False for no key and for Escape, which cancels a capture. */
int dcp_key_bindable(int scancode);
/* The name saved for a scancode, written to `buffer`; empty when the key is not bindable. */
const char *dcp_key_name(int scancode, char *buffer, size_t size);

/* Applies one saved line to the map; false when the key is not an action or the value names no bindable key. */
int dcp_keys_apply(DcpKeyMap *map, const char *ini_key, const char *value);
/* Defaults, then every valid binding in the dcp.ini at `path`; a missing file or line keeps the default. */
void dcp_keys_load(const char *path, DcpKeyMap *map);

/* Calls `visit` for each "key = value" line of the dcp.ini at `path` (comments and trailing " #..." removed). */
typedef void (*DcpIniVisitor)(const char *key, const char *value, void *user);
void dcp_ini_visit(const char *path, DcpIniVisitor visit, void *user);

/* Mouse bindings. A mouse button is the number SDL gives it (SDL_BUTTON_LEFT..SDL_BUTTON_X2); 0 is no button. */
typedef enum {
    DCP_MOUSE_NONE=0, DCP_MOUSE_LEFT=1, DCP_MOUSE_MIDDLE=2, DCP_MOUSE_RIGHT=3, DCP_MOUSE_X1=4, DCP_MOUSE_X2=5,
    DCP_MOUSE_BUTTON_COUNT
} DcpMouseButton;

/* The PS4 digital controls a mouse button or a wheel step can press: the keyboard's list from Cross to Touchpad right, in
 * the same order and under the same names. */
typedef enum {
    DCP_PAD_TARGET_CROSS, DCP_PAD_TARGET_CIRCLE, DCP_PAD_TARGET_SQUARE, DCP_PAD_TARGET_TRIANGLE,
    DCP_PAD_TARGET_L1, DCP_PAD_TARGET_R1, DCP_PAD_TARGET_L2, DCP_PAD_TARGET_R2, DCP_PAD_TARGET_L3, DCP_PAD_TARGET_R3,
    DCP_PAD_TARGET_DPAD_UP, DCP_PAD_TARGET_DPAD_DOWN, DCP_PAD_TARGET_DPAD_LEFT, DCP_PAD_TARGET_DPAD_RIGHT,
    DCP_PAD_TARGET_OPTIONS, DCP_PAD_TARGET_TOUCHPAD_LEFT, DCP_PAD_TARGET_TOUCHPAD_RIGHT,
    DCP_PAD_TARGET_COUNT
} DcpPadTarget;

/* "cross", "l1", "dpad_up", "touchpad_left"...; empty for a number that is no target. */
const char *dcp_pad_target_name(int target);
/* "Cross (A)", "L1", "D-pad up"... */
const char *dcp_pad_target_label(int target);
int dcp_pad_target_from_name(const char *name); /* -1 when unknown */

/* A mouse action is either a PS4 control (its `target`) or one of the two Pointer Control actions. The first four keep
 * their saved names from before the others existed. */
typedef enum {
    DCP_MOUSE_SQUARE,           /* primary action */
    DCP_MOUSE_CIRCLE,           /* secondary action */
    DCP_MOUSE_POINTER_CONTROL,  /* Hold: mouse movement drives the pointer while held */
    DCP_MOUSE_POINTER_INTERACT, /* while the pointer is active: the pointer interaction */
    DCP_MOUSE_CROSS, DCP_MOUSE_TRIANGLE, DCP_MOUSE_L1, DCP_MOUSE_R1, DCP_MOUSE_L2, DCP_MOUSE_R2, DCP_MOUSE_L3, DCP_MOUSE_R3,
    DCP_MOUSE_DPAD_UP, DCP_MOUSE_DPAD_DOWN, DCP_MOUSE_DPAD_LEFT, DCP_MOUSE_DPAD_RIGHT,
    DCP_MOUSE_OPTIONS, DCP_MOUSE_TOUCHPAD_LEFT, DCP_MOUSE_TOUCHPAD_RIGHT,
    DCP_MOUSE_ACTION_COUNT
} DcpMouseAction;

#define DCP_MOUSE_FIRST_EXTENDED DCP_MOUSE_CROSS

/* What a wheel step presses: a PS4 control held for a few game reads, or nothing. The values after NONE follow DcpPadTarget. */
typedef enum {
    DCP_WHEEL_NONE, DCP_WHEEL_CROSS, DCP_WHEEL_CIRCLE, DCP_WHEEL_SQUARE, DCP_WHEEL_TRIANGLE,
    DCP_WHEEL_L1, DCP_WHEEL_R1, DCP_WHEEL_L2, DCP_WHEEL_R2,
    DCP_WHEEL_TARGET_COUNT=DCP_PAD_TARGET_COUNT+1
} DcpWheelTarget;

/* Pointer Control Mode: Hold drives the pointer only while its button is held; Always On whenever gameplay owns the mouse. */
typedef enum { DCP_POINTER_MODE_HOLD, DCP_POINTER_MODE_ALWAYS_ON, DCP_POINTER_MODE_COUNT } DcpPointerMode;

typedef struct {
    const char *ini_key;
    const char *label;
    int default_button;
    int target; /* DcpPadTarget, or -1 for the Pointer Control actions */
} DcpMouseActionInfo;

extern const DcpMouseActionInfo dcp_mouse_actions[DCP_MOUSE_ACTION_COUNT];
extern const char *const dcp_wheel_up_key, *const dcp_wheel_down_key, *const dcp_pointer_sensitivity_key,
    *const dcp_mouse_controls_key, *const dcp_pointer_mode_key, *const dcp_trackpad_key;

#define DCP_POINTER_SENSITIVITY_MIN 25
#define DCP_POINTER_SENSITIVITY_MAX 200
#define DCP_POINTER_SENSITIVITY_STEP 5
#define DCP_POINTER_SENSITIVITY_DEFAULT 100

typedef struct {
    int button[DCP_MOUSE_ACTION_COUNT];
    int wheel_up, wheel_down; /* DcpWheelTarget */
    int sensitivity;          /* percent, DCP_POINTER_SENSITIVITY_MIN..MAX in steps of 5 */
    int controls;             /* Mouse Controls: 0 ignores the mouse in gameplay */
    int pointer_mode;         /* DcpPointerMode */
    int trackpad;             /* DcpTrackpadSetting: Auto drives the pointer from the Steam Deck trackpad in Gaming Mode */
} DcpMouseMap;

void dcp_mouse_defaults(DcpMouseMap *map);
/* "left", "right", "middle", "x1", "x2" (also mouse1..mouse5, case-insensitive); "none" gives DCP_MOUSE_NONE.
 * -1 for anything else, an empty name included. */
int dcp_mouse_button_from_name(const char *name);
/* The saved name of a button; "none" for DCP_MOUSE_NONE and for numbers that are no button. */
const char *dcp_mouse_button_name(int button);
/* "Left Mouse", "Right Mouse", "Middle Mouse", "Mouse 4", "Mouse 5"; "(none)" for no button. */
const char *dcp_mouse_button_label(int button);
int dcp_pointer_mode_from_name(const char *name); /* "hold", "always_on"; -1 when unknown */
const char *dcp_pointer_mode_name(int mode);
const char *dcp_pointer_mode_label(int mode); /* "Hold", "Always On" */
int dcp_trackpad_setting_from_name(const char *name); /* "auto", "off"; -1 when unknown */
const char *dcp_trackpad_setting_name(int setting);
const char *dcp_trackpad_setting_label(int setting); /* "Auto", "Off" */
int dcp_wheel_target_from_name(const char *name); /* -1 when unknown */
const char *dcp_wheel_target_name(int target);
const char *dcp_wheel_target_label(int target);
/* False unless `percent` is a step between the minimum and the maximum. */
int dcp_pointer_sensitivity_valid(int percent);
/* The nearest step, clamped to the minimum and the maximum. */
int dcp_pointer_sensitivity_normalize(int percent);

/* True when a saved key belongs to the mouse settings. */
int dcp_mouse_setting_key(const char *ini_key);
/* Whether a saved value is usable for that key. The click cannot be unbound. */
int dcp_mouse_setting_value_valid(const char *ini_key, const char *value);

/* Applies one saved line; false when the key is not a mouse setting or the value is not usable. */
int dcp_mouse_apply(DcpMouseMap *map, const char *ini_key, const char *value);
/* Puts the pointer control and its click back to their defaults when a saved file gave them the same button. */
void dcp_mouse_normalize(DcpMouseMap *map);
/* Defaults, then every valid mouse line of the dcp.ini at `path`, then dcp_mouse_normalize. */
void dcp_mouse_load(const char *path, DcpMouseMap *map);

typedef struct {
    int rejected;       /* mouse lines with a value that was not usable, and a Pointer Control pair put back to its defaults */
    int extended_bound; /* PS4 controls beyond Square and Circle that a mouse button performs */
} DcpMouseLoadInfo;
void dcp_mouse_load_info(const char *path, DcpMouseMap *map, DcpMouseLoadInfo *info);

/* The PS4 controls (bit 1<<DcpPadTarget) that pressing `button` performs. A file can bind several to one button; the
 * launcher binds one. */
unsigned dcp_mouse_button_targets(const DcpMouseMap *map, int button);
/* The Pointer Control actions own the button: Pointer Control while Hold is selected, Pointer Interaction while Always On is,
 * so a PS4 control on it would never press. */
int dcp_mouse_button_locked(const DcpMouseMap *map, int button);
/* The launcher's rule: `button` performs only `target` (-1: nothing). The target leaves the button that had it; its previous
 * button is written to `moved_from` (DCP_MOUSE_NONE when it had none). False, and nothing changed, for a button that is no
 * button, a target out of range or a locked button. */
int dcp_mouse_assign_target(DcpMouseMap *map, int button, int target, int *moved_from);
/* False when giving `button` to `action` would make the pointer control and its click share a button, or leave the click
 * unbound. */
int dcp_mouse_binding_allowed(const DcpMouseMap *map, DcpMouseAction action, int button);

#ifdef __cplusplus
}
#endif

#endif
