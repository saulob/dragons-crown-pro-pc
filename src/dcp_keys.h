/* Keyboard bindings: the actions, their defaults and the saved key names, shared by the launcher, the runtime pad
 * and the video core. Keys are SDL scancodes, saved as the shadPS4 names (space, lshift, f10, pgup...) or SDL's own
 * lowercase names for the rest. */
#ifndef DCP_KEYS_H
#define DCP_KEYS_H

#include <stddef.h>

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
    DCP_KEY_HOTKEY_CHEATS, DCP_KEY_HOTKEY_WINDOW_MODE,
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

#ifdef __cplusplus
}
#endif

#endif
