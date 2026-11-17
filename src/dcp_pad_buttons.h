/* PS4 pad data shared by the runtime pad and the mouse input: button bits and the touch surface. */
#ifndef DCP_PAD_BUTTONS_H
#define DCP_PAD_BUTTONS_H

enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
};

/* The surface scePadGetControllerInformation reports (1920x943); a touch is 0..MAX on each axis. */
#define DCP_TOUCH_RES_X 1920
#define DCP_TOUCH_RES_Y 943
#define DCP_TOUCH_MAX_X (DCP_TOUCH_RES_X-1)
#define DCP_TOUCH_MAX_Y (DCP_TOUCH_RES_Y-1)

#endif
