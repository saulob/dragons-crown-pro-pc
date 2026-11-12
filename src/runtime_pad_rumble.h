/* The Options menu's hook into the pad's rumble state (runtime_pad.c). */
#ifndef DCP_RUNTIME_PAD_RUMBLE_H
#define DCP_RUNTIME_PAD_RUMBLE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Call after the menu changed the rumble setting: logs it and stops any rumble already running when it is off. */
void runtime_pad_rumble_changed(void);

#ifdef __cplusplus
}
#endif
#endif
