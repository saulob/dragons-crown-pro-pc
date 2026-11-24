/* libScePad on up to four SDL3 gamepads (pad handle N is player N), with a keyboard fallback for
 * player 1. SDL events are pumped by the window thread (gpu/shim/window.cpp); here state is only sampled.
 *
 * Keyboard layout: dcp_keys.c holds the defaults; the launcher's bindings in dcp.ini and then DCP_KEY_MAP
 * (see apply_button_map) change them. */
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_pad_rumble.h"
#include "comms_session.h"
#include "dcpgpu.h"
#include "dcp_exit_shortcut.h"
#include "dcp_keys.h"
#include "dcp_mouse.h"
#include "dcp_pad_buttons.h"
#include "dcp_pad_owner.h"
#include "dcp_pointer_motion.h"
#include "dcp_trackpad.h"
#include "dcp_touchscreen.h"
#include "dcp_sdl_compat.h"
#include "early_story_online.h"
#include "exit_game_hook.h"
#include "network_access.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <sys/stat.h>

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define ERR_NO_HANDLE ((int32_t)0x80920008)
#define ERR_USER_NOT_LOGIN ((int32_t)0x809b0081)
#define MAX_PADS 4

typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized, sdl_ready;
static size_t reads;
typedef struct {
    bool opened;
    int32_t user;
    SDL_Gamepad *gamepad;
    uint8_t connected_count;
    int hold_after_capture;
} PadSlot;
static PadSlot slots[MAX_PADS]; /* slots[N-1]: player N, pad handle N */

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }
#define STICK_DEADZONE 0.10f
static uint8_t axis(int16_t v) {
    float f=(float)v/32767.f;
    if (f>1) f=1;
    if (f<-1) f=-1;
    f=fabsf(f)<=STICK_DEADZONE ? 0.f : (f>0 ? f-STICK_DEADZONE : f+STICK_DEADZONE)/(1.f-STICK_DEADZONE);
    int x=(int)lroundf(f*127.5f+127.5f);
    return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x);
}
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}
static int pointer_owns_touch; /* the last sample: Pointer Control drives the touch surface; under `lock` */
static void touch_click(PadData *d, int right) {
    d->buttons|=BTN_TOUCHPAD;
    if (pointer_owns_touch) return;
    d->touch_count=1;
    d->touches[0]=(PadTouch){.x=right ? 1440 : 480,.y=471,.id=0};
}

static DcpPadOwners owners;
static int owners_ready;
static int free_slot(void) { return dcp_pad_owners_free_slot(&owners); }

int runtime_pad_player_of_instance(uint32_t instance) {
    pthread_mutex_lock(&lock);
    const int slot = owners_ready ? dcp_pad_owners_slot_of(&owners,instance) : -1;
    pthread_mutex_unlock(&lock);
    return slot;
}
/* Never logged: the path can carry a device serial on some platforms. */
static uint64_t device_identity(SDL_JoystickID id) {
    uint64_t hash=1469598103934665603ull;
    const SDL_GUID guid=SDL_GetGamepadGUIDForID(id);
    for (size_t i=0;i<sizeof(guid.data);++i) hash=(hash^guid.data[i])*1099511628211ull;
    const char *path=SDL_GetGamepadPathForID(id);
    for (; path && *path; ++path) hash=(hash^(uint8_t)*path)*1099511628211ull;
    hash=(hash^(uint32_t)(SDL_GetGamepadPlayerIndexForID(id)+1))*1099511628211ull;
    return hash ? hash : 1;
}
static bool steam_virtual(uint16_t vendor, uint16_t product) { return vendor==0x28de && product==0x11ff; }
static char keys_file[4200];
/* The player's own mappings. SDL_GAMECONTROLLERCONFIG, which SDL reads itself, keeps priority over them. */
static void load_user_gamepad_mappings(void) {
    char path[4300];
    if (!dcp_gamepad_db_path(keys_file,path,sizeof(path)) || !SDL_GetPathInfo(path,NULL)) return;
    const int added=SDL_AddGamepadMappingsFromFile(path);
    if (added<0) printf("Runtime: gamepad mappings not read from %s: %s\n",path,SDL_GetError());
    else printf("Runtime: %d gamepad mappings read from %s\n",added,path);
}
/* SDL lists a joystick as a gamepad only when it has a mapping: says once per connection which ones have none. */
static void log_unmapped_joysticks(void) {
    static SDL_JoystickID logged[16];
    static size_t next;
    int count=0;
    SDL_JoystickID *ids=SDL_GetJoysticks(&count);
    for (int j=0;ids && j<count;++j) {
        bool seen=SDL_IsGamepad(ids[j]);
        for (size_t i=0;i<sizeof(logged)/sizeof(*logged);++i) if (logged[i]==ids[j]) seen=true;
        if (seen) continue;
        logged[next++%(sizeof(logged)/sizeof(*logged))]=ids[j];
        char guid[33];
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(ids[j]),guid,sizeof(guid));
        const char *name=SDL_GetJoystickNameForID(ids[j]);
        printf("Runtime: joystick without a gamepad mapping: %s (guid %s, %04x:%04x); add a line for this GUID to gamecontrollerdb.txt in the user folder\n",
               name?name:"unknown",guid,SDL_GetJoystickVendorForID(ids[j]),SDL_GetJoystickProductForID(ids[j]));
    }
    SDL_free(ids);
}
static void release_gamepad(int slot) {
    SDL_CloseGamepad(slots[slot].gamepad);
    slots[slot].gamepad=NULL;
    dcp_pad_owners_release(&owners,slot);
}
static void open_assigned(const DcpPadAssignment *a) {
    SDL_Gamepad *g=SDL_OpenGamepad(a->device.instance);
    if (!g) {
        dcp_pad_owners_release(&owners,a->slot);
        return;
    }
    const int slot=a->slot;
    slots[slot].gamepad=g;
    ++slots[slot].connected_count;
    const unsigned vendor=SDL_GetGamepadVendor(g), product=SDL_GetGamepadProduct(g);
    printf("Runtime: gamepad %d connected: %s (%04x:%04x%s)\n",slot+1,SDL_GetGamepadName(g),vendor,product,
           steam_virtual(vendor,product) ? ", Steam Input virtual gamepad" : "");
    dcp_pad_telemetry_record(a);
    runtime_user_login(slot+1);
}
/* Called under lock. Ownership rules: dcp_pad_owner.h. A disconnected gamepad frees its slot, but the player stays
 * logged in, as on the PS4. */
static void refresh_gamepads(void) {
    if (!sdl_ready) {
        if (!SDL_WasInit(SDL_INIT_GAMEPAD)) dcp_prepare_sdl_gamepad();
        sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
        if (sdl_ready>0) load_user_gamepad_mappings();
    }
    if (sdl_ready<0) return;
    if (!owners_ready) { dcp_pad_owners_init(&owners); owners_ready=1; }
    for (int i=0;i<MAX_PADS;++i) {
        if (!slots[i].gamepad || SDL_GamepadConnected(slots[i].gamepad)) continue;
        printf("Runtime: gamepad %d disconnected\n",i+1);
        release_gamepad(i);
    }
    static uint64_t last_scan;
    const uint64_t now=now_us();
    if (free_slot()<0 || (last_scan && now-last_scan<250000)) return;
    last_scan=now;
    log_unmapped_joysticks();
    int count=0;
    SDL_JoystickID *ids=SDL_GetGamepads(&count);
    DcpPadDevice devices[16];
    size_t found=0;
    for (int j=0;ids && j<count && found<sizeof(devices)/sizeof(*devices);++j)
        devices[found++]=(DcpPadDevice){ids[j],device_identity(ids[j]),SDL_GetGamepadPlayerIndexForID(ids[j])};
    SDL_free(ids);
    DcpPadAssignment assigned[MAX_PADS];
    const size_t n=dcp_pad_owners_assign(&owners,devices,found,assigned,MAX_PADS);
    for (size_t i=0;i<n;++i) open_assigned(&assigned[i]);
}
/* Button sources: a host button or key and the PS4 button it presses. The defaults below can be
 * changed per PS4 button with DCP_PAD_MAP (gamepad) and DCP_KEY_MAP (keyboard), e.g.
 * DCP_KEY_MAP="cross=Q,square=Left Shift" or DCP_PAD_MAP="cross=b,circle=a": comma-separated
 * <ps4 button>=<SDL key or gamepad button name>. A mapped PS4 button loses its default sources.
 * Sticks and the touchpad keys are set by the launcher only. */
static const struct { const char *name; uint32_t ps; } ps_names[]={
    {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
    {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
    {"options",BTN_OPTIONS}, {"touchpad",BTN_TOUCHPAD},
    {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
};
typedef struct { int source; uint32_t ps; } ButtonSource;
typedef struct { ButtonSource entries[48]; size_t count; } ButtonMap;
static ButtonMap pad_map={{
    {SDL_GAMEPAD_BUTTON_SOUTH,BTN_CROSS}, {SDL_GAMEPAD_BUTTON_EAST,BTN_CIRCLE},
    {SDL_GAMEPAD_BUTTON_WEST,BTN_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH,BTN_TRIANGLE},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,BTN_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,BTN_R1},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK,BTN_L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK,BTN_R3},
    {SDL_GAMEPAD_BUTTON_START,BTN_OPTIONS}, {SDL_GAMEPAD_BUTTON_BACK,BTN_TOUCHPAD},
    {SDL_GAMEPAD_BUTTON_TOUCHPAD,BTN_TOUCHPAD},
    {SDL_GAMEPAD_BUTTON_DPAD_UP,BTN_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN,BTN_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT,BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,BTN_RIGHT},
},15};
static ButtonMap key_map;
static DcpKeyMap keys;
static void log_rumble(const char *source) {
    printf("Controller rumble: %s%s\n",dcpgpu_controller_rumble() ? "on" : "off",source);
}
void runtime_pad_set_rumble(int enabled) {
    dcpgpu_set_controller_rumble(enabled);
    log_rumble("");
}
void runtime_pad_set_settings(const char *settings_file) {
    snprintf(keys_file,sizeof(keys_file),"%s",settings_file ? settings_file : "");
    DcpMouseMap mouse;
    DcpMouseLoadInfo loaded;
    dcp_mouse_load_info(keys_file,&mouse,&loaded);
    dcp_mouse_configure(dcp_mouse_global(),&mouse);
    dcp_mouse_note_invalid(dcp_mouse_global(),loaded.rejected);
    if (mouse.controls) printf("Mouse controls: on, sensitivity %d%%, pointer control %s, %d extra buttons\n",mouse.sensitivity,
                               dcp_pointer_mode_name(mouse.pointer_mode),loaded.extended_bound);
    else puts("Mouse controls: off");
}

static const struct { DcpKeyAction action; uint32_t ps; } key_buttons[]={
    {DCP_KEY_CROSS,BTN_CROSS}, {DCP_KEY_CIRCLE,BTN_CIRCLE}, {DCP_KEY_SQUARE,BTN_SQUARE}, {DCP_KEY_TRIANGLE,BTN_TRIANGLE},
    {DCP_KEY_L1,BTN_L1}, {DCP_KEY_R1,BTN_R1}, {DCP_KEY_L2,BTN_L2}, {DCP_KEY_R2,BTN_R2},
    {DCP_KEY_L3,BTN_L3}, {DCP_KEY_R3,BTN_R3}, {DCP_KEY_OPTIONS,BTN_OPTIONS},
    {DCP_KEY_DPAD_UP,BTN_UP}, {DCP_KEY_DPAD_DOWN,BTN_DOWN}, {DCP_KEY_DPAD_LEFT,BTN_LEFT}, {DCP_KEY_DPAD_RIGHT,BTN_RIGHT},
};
static void load_key_map(void) {
    dcp_keys_load(keys_file,&keys);
    key_map.count=0;
    for (size_t i=0;i<sizeof(key_buttons)/sizeof(*key_buttons);++i)
        key_map.entries[key_map.count++]=(ButtonSource){keys.scancode[key_buttons[i].action],key_buttons[i].ps};
}

static char *trim(char *text) {
    while (*text==' ' || *text=='\t') ++text;
    char *end=text+strlen(text);
    while (end>text && (end[-1]==' ' || end[-1]=='\t' || end[-1]=='\r' || end[-1]=='\n')) *--end=0;
    return text;
}

/* Applies a DCP_PAD_MAP/DCP_KEY_MAP value; returns how many entries were used. Bad entries are
 * logged and skipped, so their PS4 button keeps its default. */
static int apply_button_map(ButtonMap *map, const char *spec, bool keyboard) {
    if (!spec || !*spec) return 0;
    char copy[1024];
    snprintf(copy,sizeof(copy),"%s",spec);
    int used=0;
    char *entry=copy;
    while (entry) {
        char *next=strchr(entry,',');
        if (next) *next++=0;
        char *eq=strchr(entry,'=');
        if (!eq) {
            if (*trim(entry)) printf("Runtime: pad map: ignored '%s' (expected button=name)\n",trim(entry));
            entry=next;
            continue;
        }
        *eq=0;
        const char *button=trim(entry), *name=trim(eq+1);
        entry=next;
        uint32_t ps=0;
        for (size_t i=0;i<sizeof(ps_names)/sizeof(*ps_names);++i) if (!SDL_strcasecmp(button,ps_names[i].name)) ps=ps_names[i].ps;
        const int source=!*name ? -1 : keyboard ? (int)SDL_GetScancodeFromName(name) : (int)SDL_GetGamepadButtonFromString(name);
        if (!ps || source<=(keyboard ? (int)SDL_SCANCODE_UNKNOWN : (int)SDL_GAMEPAD_BUTTON_INVALID)) {
            printf("Runtime: pad map: ignored '%s=%s'\n",button,name);
            continue;
        }
        size_t kept=0;
        for (size_t i=0;i<map->count;++i) if (map->entries[i].ps!=ps) map->entries[kept++]=map->entries[i];
        map->count=kept;
        if (map->count<sizeof(map->entries)/sizeof(*map->entries)) map->entries[map->count++]=(ButtonSource){source,ps};
        ++used;
    }
    return used;
}

static void load_button_maps(void) {
    static bool loaded;
    if (loaded) return;
    loaded=true;
    load_key_map();
    const int pad=apply_button_map(&pad_map,getenv("DCP_PAD_MAP"),false);
    const int keys=apply_button_map(&key_map,getenv("DCP_KEY_MAP"),true);
    if (pad || keys) printf("Runtime: pad map: %d gamepad and %d keyboard buttons remapped\n",pad,keys);
}

static void merge_mouse(PadData *d, const DcpMouseSample *mouse) {
    d->buttons|=mouse->buttons;
    if (mouse->pointer) { memset(d->touches,0,sizeof(d->touches)); d->touch_count=0; }
    if (mouse->touch) {
        d->touches[0]=(PadTouch){.x=mouse->x,.y=mouse->y,.id=mouse->id};
        d->touch_count=1;
    }
    if (mouse->touchpad_side) touch_click(d,mouse->touchpad_side==2);
}
static bool touchpad_touched(SDL_Gamepad *g, int touchpad) {
    const int fingers=SDL_GetNumGamepadTouchpadFingers(g,touchpad);
    for (int finger=0;finger<fingers;++finger) {
        bool down=false;
        if (SDL_GetGamepadTouchpadFinger(g,touchpad,finger,&down,NULL,NULL,NULL) && down) return true;
    }
    return false;
}
static int valve_touchpads(SDL_Gamepad *g) {
    return g ? dcp_trackpad_native_touchpads(SDL_GetGamepadVendor(g),SDL_GetNumGamepadTouchpads(g)) : 0;
}
/* The touchpad that feeds the game's touch surface: the first one, or on a Steam Deck the trackpad in use. -1 without any. */
static int surface_touchpad(SDL_Gamepad *g) {
    static int deck_pad=1;
    if (!g || SDL_GetNumGamepadTouchpads(g)<1) return -1;
    if (valve_touchpads(g)<2) return 0;
    deck_pad=dcp_trackpad_pick(touchpad_touched(g,0),touchpad_touched(g,1),deck_pad);
    return deck_pad;
}
static bool pad_surface_touched(SDL_Gamepad *g) {
    const int touchpad=surface_touchpad(g);
    return touchpad>=0 && touchpad_touched(g,touchpad);
}
static int native_trackpads(void) {
    int total=0;
    for (int slot=0;slot<MAX_PADS;++slot) total+=valve_touchpads(slots[slot].gamepad);
    return total;
}
/* A finger on the screen owns the touch surface while it is down. */
static void merge_touchscreen(PadData *d, const DcpTouchSample *screen) {
    d->buttons|=screen->buttons;
    if (screen->touch) {
        memset(d->touches,0,sizeof(d->touches));
        d->touches[0]=(PadTouch){.x=screen->x,.y=screen->y,.id=screen->id};
        d->touch_count=1;
    }
}
static void key_axis(uint8_t *axis, bool negative, bool positive) {
    if (negative || positive) *axis=(uint8_t)(128-(negative ? 128 : 0)+(positive ? 127 : 0));
}
/* Player 1 is always connected: the keyboard stands in for a missing gamepad. */
static bool slot_connected(int slot) { return slot==0 || slots[slot].gamepad; }

int runtime_pad_slot_human(int slot) {
    if (slot==0) return 1;
    return slot>0 && slot<MAX_PADS && slots[slot].gamepad;
}
static void sample_host(int slot, PadData *d) {
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->timestamp=now_us();
    refresh_gamepads();
    SDL_Gamepad *g=slots[slot].gamepad;
    d->connected=slot_connected(slot);
    d->connected_count=slots[slot].connected_count ? slots[slot].connected_count : d->connected;
    pointer_owns_touch=0;
    if (dcpgpu_overlay_captures_input()) return; /* settings menu open: neutral input */
    const bool *k=slot==0 && SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    DcpMouseSample mouse={0};
    DcpTouchSample screen={0};
    if (slot==0) {
        const bool right_stick=g && (abs(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX))>=7849 ||
                                     abs(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTY))>=7849);
        dcp_mouse_controller_stick(dcp_mouse_global(),right_stick);
        dcp_mouse_controller_surface(dcp_mouse_global(),pad_surface_touched(g));
        dcp_mouse_trackpad_pads(dcp_mouse_global(),native_trackpads(),valve_touchpads(g)>1 && pad_surface_touched(g));
        dcp_mouse_sample(dcp_mouse_global(),&mouse);
        dcp_touchscreen_sample(dcp_touchscreen_global(),&screen);
        if (screen.new_contact) dcp_touch_telemetry_record(DCP_TOUCH_EVENT_TOUCHPAD_USED);
        pointer_owns_touch=mouse.pointer || screen.touch;
        dcp_mouse_screen_surface(dcp_mouse_global(),screen.touch);
    }
    load_button_maps();
    if (g) {
        /* L3 and R3 together belong to the Options menu: the game never sees the pair. */
        const bool chord=SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_LEFT_STICK) && SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_RIGHT_STICK);
        /* L1 + R1 + Options on one controller is the Exit Game chord: while it is down the game sees none of the three,
         * and what is still held after it ends stays hidden until released (hold_after_capture). */
        const bool exit_chord=dcp_exit_chord_down(dcpgpu_controller_exit_shortcut(),SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER),
                                                  SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER),SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_START));
        if (exit_chord) slots[slot].hold_after_capture=1;
        for (size_t i=0;i<pad_map.count;++i) {
            const int source=pad_map.entries[i].source;
            if (chord && (source==SDL_GAMEPAD_BUTTON_LEFT_STICK || source==SDL_GAMEPAD_BUTTON_RIGHT_STICK)) continue;
            if (exit_chord && (source==SDL_GAMEPAD_BUTTON_LEFT_SHOULDER || source==SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER ||
                               source==SDL_GAMEPAD_BUTTON_START)) continue;
            if (SDL_GetGamepadButton(g,(SDL_GamepadButton)source)) d->buttons|=pad_map.entries[i].ps;
        }
        const uint32_t mapped=d->buttons;
        d->left_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTX)); d->left_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY));
        d->right_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX)); d->right_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTY));
        d->l2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)); d->r2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        if (d->l2>30) d->buttons|=BTN_L2;
        if (d->r2>30) d->buttons|=BTN_R2;
        if (mapped & BTN_L2) d->l2=255;
        if (mapped & BTN_R2) d->r2=255;
        const int touchpad=surface_touchpad(g);
        if (touchpad>=0) {
            const int fingers=SDL_GetNumGamepadTouchpadFingers(g,touchpad);
            for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                bool down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(g,touchpad,finger,&down,&x,&y,NULL) && down) {
                    d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
        // Back/Select on pads without a touch surface is a left-side click.
        if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    }
    /* The mouse is player 1's too: buttons add to the pad's, and Pointer Control takes the touch surface. */
    if (slot==0) {
        merge_mouse(d,&mouse);
        merge_touchscreen(d,&screen);
    }
    /* The keyboard works with or without a gamepad: its buttons add to the pad's, and a stick
     * follows the keys only while one of them is held. */
    if (!k) return;
    for (size_t i=0;i<key_map.count;++i) if (k[key_map.entries[i].source]) d->buttons|=key_map.entries[i].ps;
    if (k[keys.scancode[DCP_KEY_TOUCHPAD_LEFT]]) touch_click(d,0);
    if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0); /* a remapped touchpad key */
    if (k[keys.scancode[DCP_KEY_TOUCHPAD_RIGHT]]) touch_click(d,1);
    if (d->buttons & BTN_L2) d->l2=255;
    if (d->buttons & BTN_R2) d->r2=255;
    key_axis(&d->left_x,k[keys.scancode[DCP_KEY_LEFT_STICK_LEFT]],k[keys.scancode[DCP_KEY_LEFT_STICK_RIGHT]]);
    key_axis(&d->left_y,k[keys.scancode[DCP_KEY_LEFT_STICK_UP]],k[keys.scancode[DCP_KEY_LEFT_STICK_DOWN]]);
    key_axis(&d->right_x,k[keys.scancode[DCP_KEY_RIGHT_STICK_LEFT]],k[keys.scancode[DCP_KEY_RIGHT_STICK_RIGHT]]);
    key_axis(&d->right_y,k[keys.scancode[DCP_KEY_RIGHT_STICK_UP]],k[keys.scancode[DCP_KEY_RIGHT_STICK_DOWN]]);
}

/* DCP_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad touchpad_left touchpad_right up down left right) are held while listed;
 * touchpad defaults to a left-side click; lx= ly= rx= ry= (0..255) override
 * the sticks. An empty file releases everything. */
static struct { uint32_t buttons; int stick[4]; int touch_side; } injected={0,{-1,-1,-1,-1},-1};
static int replay_armed;      /* 1 while a DCP_PAD_REPLAY recording plays, 2 once it ended */
static uint64_t replay_start; /* 0: (re)start at the next sample */
static void read_inject(void) {
    static const char *path; static int checked; static uint64_t last_check; static struct timespec mtime;
    if (!checked) { path=getenv("DCP_PAD_FILE"); checked=1; }
    if (!path || !*path) return;
    uint64_t now=now_us();
    if (now-last_check<20000) return;
    last_check=now;
    struct stat st;
    if (stat(path,&st)!=0) return;
#ifdef _WIN32
    /* Whole-second mtimes: the size tells two edits within a second apart. */
    if (st.st_mtime==mtime.tv_sec && st.st_size==mtime.tv_nsec) return;
    mtime=(struct timespec){st.st_mtime,(long)st.st_size};
#else
    if (st.st_mtim.tv_sec==mtime.tv_sec && st.st_mtim.tv_nsec==mtime.tv_nsec) return;
    mtime=st.st_mtim;
#endif
    FILE *f=fopen(path,"r");
    if (!f) return;
    static const char *sticks[]={"lx=","ly=","rx=","ry="};
    injected.buttons=0;
    injected.touch_side=-1;
    for (int i=0;i<4;++i) injected.stick[i]=-1;
    char token[64];
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"replay") && replay_armed!=1) { replay_armed=1; replay_start=0; } /* DCP_PAD_REPLAY */
        if (!strcmp(token,"touchpad_left") || !strcmp(token,"touchpad_right")) {
            injected.buttons|=BTN_TOUCHPAD;
            injected.touch_side=!strcmp(token,"touchpad_right");
        }
        for (size_t i=0;i<sizeof(ps_names)/sizeof(*ps_names);++i) if (!strcmp(token,ps_names[i].name)) injected.buttons|=ps_names[i].ps;
        for (int i=0;i<4;++i) if (!strncmp(token,sticks[i],3)) { int v=atoi(token+3); injected.stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
    }
    fclose(f);
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d\n",injected.buttons,
           injected.stick[0],injected.stick[1],injected.stick[2],injected.stick[3]);
}
/* DCP_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; DCP_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in DCP_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2, written when the state changes. */
typedef struct { uint32_t ms, buttons; uint8_t axes[4], l2, r2; } PadSample;
static FILE *record_file;
static uint64_t record_start;
static PadSample record_last;
static void record_sample(const PadData *d) {
    static const char *path; static int checked, f9_was_down;
    if (!checked) { path=getenv("DCP_PAD_RECORD"); checked=1; }
    if (!path || !*path || !sdl_ready) return;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k && k[SDL_SCANCODE_F9];
    if (f9 && !f9_was_down) {
        if (record_file) {
            fclose(record_file); record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",path);
        } else if ((record_file=fopen(path,"w"))) {
            record_start=now_us();
            memset(&record_last,0xff,sizeof(record_last));
            printf("Runtime: pad recording started (%s, F9 stops)\n",path);
        }
    }
    f9_was_down=f9;
    if (!record_file) return;
    PadSample s={(uint32_t)((now_us()-record_start)/1000),d->buttons,
                 {d->left_x,d->left_y,d->right_x,d->right_y},d->l2,d->r2};
    if (s.buttons==record_last.buttons && !memcmp(s.axes,record_last.axes,4) &&
        s.l2==record_last.l2 && s.r2==record_last.r2) return;
    record_last=s;
    fprintf(record_file,"%u %u %u %u %u %u %u %u\n",s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],
            s.axes[3],s.l2,s.r2);
    fflush(record_file);
}
static PadSample *replay; static size_t replay_count, replay_next;
static void replay_sample(PadData *d) {
    if (!replay_armed) return;
    if (!replay_start) {
        static int loaded;
        if (!loaded) {
            loaded=1;
            const char *path=getenv("DCP_PAD_REPLAY");
            FILE *f=path ? fopen(path,"r") : NULL;
            PadSample s; unsigned v[8]; size_t cap=0;
            while (f && fscanf(f,"%u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])==8) {
                s=(PadSample){v[0],v[1],{(uint8_t)v[2],(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5]},(uint8_t)v[6],(uint8_t)v[7]};
                if (replay_count==cap && !(replay=realloc(replay,(cap=cap ? cap*2 : 1024)*sizeof(*replay)))) break;
                replay[replay_count++]=s;
            }
            if (f) fclose(f);
            printf("Runtime: pad replay of %zu samples from %s\n",replay_count,path ? path : "(unset)");
        }
        replay_start=now_us();
        replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-replay_start)/1000);
    while (replay_next<replay_count && replay[replay_next].ms<=ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next==replay_count && ms>replay[replay_count-1].ms+500) {
        if (replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        return;
    }
    const PadSample *s=&replay[replay_next-1];
    d->buttons=s->buttons;
    d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
    d->l2=s->l2; d->r2=s->r2;
}
/* Buttons of player 1's gamepad and keyboard as mapped, even while the overlay holds the game neutral. */
static uint32_t host_buttons(void) {
    uint32_t buttons=0;
    load_button_maps();
    refresh_gamepads();
    SDL_Gamepad *g=slots[0].gamepad;
    if (g) for (size_t i=0;i<pad_map.count;++i)
        if (SDL_GetGamepadButton(g,(SDL_GamepadButton)pad_map.entries[i].source)) buttons|=pad_map.entries[i].ps;
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    if (k) for (size_t i=0;i<key_map.count;++i) if (k[key_map.entries[i].source]) buttons|=key_map.entries[i].ps;
    return buttons;
}
/* Buttons for the port's save list (sceSaveDataDialog), polled by the overlay every frame while
 * it is open: the game stops reading the pad during the dialog. Gamepad and keyboard as mapped,
 * scripted input, and the left stick as up/down. */
static uint32_t list_buttons(void) {
    pthread_mutex_lock(&lock);
    read_inject();
    uint32_t buttons=host_buttons()|injected.buttons;
    SDL_Gamepad *g=slots[0].gamepad;
    if (g) {
        const int y=SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY);
        if (y<-16000) buttons|=BTN_UP;
        else if (y>16000) buttons|=BTN_DOWN;
    }
    if (injected.stick[1]>=0 && injected.stick[1]<64) buttons|=BTN_UP;
    else if (injected.stick[1]>192) buttons|=BTN_DOWN;
    pthread_mutex_unlock(&lock);
    return buttons;
}
static void sample_player1_scripts(PadData *d) {
    record_sample(d);
    read_inject();
    replay_sample(d);
    d->buttons|=injected.buttons;
    if (injected.touch_side>=0) touch_click(d,injected.touch_side);
    else if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    if (injected.buttons & BTN_L2) d->l2=255;
    if (injected.buttons & BTN_R2) d->r2=255;
    uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
    for (int i=0;i<4;++i) if (injected.stick[i]>=0) *axes[i]=(uint8_t)injected.stick[i];
}

/* Player 1's accept for the port-owned Exit Game row: the game's confirm button, as the pad or a bound key gives it, or Enter. */
static int exit_game_accept_down(const PadData *d) {
    if (d->buttons & (exit_game_accept_east() ? BTN_CIRCLE : BTN_CROSS)) return 1;
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    return k && (k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER]);
}

/* The Comms Wheel is modal: while it is open it owns D-pad Up, the right stick, Cross and Circle; a keyboard session also
 * owns the direction keys. */
static void apply_comms_wheel(PadData *d) {
    SDL_Gamepad *g=slots[0].gamepad;
    uint8_t pad_buttons=0;
    if (g) {
        if (SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_BACK)) pad_buttons|=DCP_COMMS_PAD_BACK;
        if (SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) pad_buttons|=DCP_COMMS_PAD_L1;
        if (SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) pad_buttons|=DCP_COMMS_PAD_R1;
        if (SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_DPAD_UP)) pad_buttons|=DCP_COMMS_PAD_DPAD_UP;
    }
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    int nav=0;
    if (k) {
        nav|=(k[keys.scancode[DCP_KEY_LEFT_STICK_UP]] || k[keys.scancode[DCP_KEY_RIGHT_STICK_UP]]) ? 1 : 0;
        nav|=(k[keys.scancode[DCP_KEY_LEFT_STICK_DOWN]] || k[keys.scancode[DCP_KEY_RIGHT_STICK_DOWN]]) ? 2 : 0;
        nav|=(k[keys.scancode[DCP_KEY_LEFT_STICK_LEFT]] || k[keys.scancode[DCP_KEY_RIGHT_STICK_LEFT]]) ? 4 : 0;
        nav|=(k[keys.scancode[DCP_KEY_LEFT_STICK_RIGHT]] || k[keys.scancode[DCP_KEY_RIGHT_STICK_RIGHT]]) ? 8 : 0;
    }
    const DcpCommsPadIn in={pad_buttons,comms_session_available(),
                            d->right_x,d->right_y,(d->buttons & BTN_CIRCLE)!=0,(d->buttons & BTN_CROSS)!=0,nav};
    DcpCommsPadOut out;
    dcpgpu_comms_wheel_step(&in,&out);
    if (out.hide_dpad_up) d->buttons&=~BTN_UP;
    if (out.hide_pad_buttons & DCP_COMMS_PAD_BACK) {
        d->buttons&=~BTN_TOUCHPAD;
        if (!pointer_owns_touch) { memset(d->touches,0,sizeof(d->touches)); d->touch_count=0; }
    }
    if (out.hide_pad_buttons & DCP_COMMS_PAD_L1) d->buttons&=~BTN_L1;
    if (out.hide_pad_buttons & DCP_COMMS_PAD_R1) d->buttons&=~BTN_R1;
    if (out.hide_cross) d->buttons&=~BTN_CROSS;
    if (out.hide_circle) d->buttons&=~BTN_CIRCLE;
    if (out.neutral_right_stick) d->right_x=d->right_y=128;
    if (out.neutral_keyboard_axes) d->left_x=d->left_y=d->right_x=d->right_y=128;
    if (out.send>=0) comms_session_send(out.send);
}

static void sample(int slot, PadData *d) {
    PadSlot *s=&slots[slot];
    sample_host(slot,d);
    /* After the menu or the text dialog closes, buttons still held (the Cross that accepted a
     * name) stay hidden until released: the game would take them as a new press. */
    if (dcpgpu_overlay_captures_input()) { s->hold_after_capture=1; return; }
    if (slot==0 && exit_game_filter_accept(exit_game_accept_down(d))) {
        memset(d->touches,0,sizeof(d->touches));
        d->buttons=0; d->l2=d->r2=0; d->touch_count=0;
        s->hold_after_capture=1;
        return;
    }
    if (slot==0) sample_player1_scripts(d);
    /* Hydeland's Network Menu shortcut keeps player 1's Triangle for itself while it is armed. */
    if (slot==0 && early_story_shortcut_pad_filter((d->buttons & BTN_TRIANGLE)!=0)) d->buttons&=~BTN_TRIANGLE;
    if (slot==0 && network_menu_pad_hold(d->buttons!=0)) d->buttons=0;
    if (slot==0) apply_comms_wheel(d);
    if (s->hold_after_capture) {
        if (d->buttons) d->buttons=0;
        else s->hold_after_capture=0;
    }
}

/* DCP_INPUT_LATENCY_TRACE=1: from the host press the window thread saw to the game's first read of it. */
static void trace_read(const PadData *d) {
    static uint32_t last_buttons;
    static uint64_t window_start, last_press, presses, sum_ns, max_ns, window_reads;
    if (!dcpgpu_latency_trace_enabled()) return;
    const uint64_t now=SDL_GetTicksNS(), press=dcpgpu_latency_press_ns();
    if (!window_start) window_start=now;
    ++window_reads;
    if ((d->buttons & ~last_buttons) && press && press!=last_press && press<=now) {
        last_press=press; ++presses; sum_ns+=now-press;
        if (now-press>max_ns) max_ns=now-press;
    }
    last_buttons=d->buttons;
    if (now-window_start<5000000000ull) return;
    printf("Latency trace: pad: %.0f reads/s; host press -> game read avg %.1f ms, max %.1f ms (%llu presses)\n",
           window_reads/((now-window_start)/1e9),presses ? sum_ns/1e6/presses : 0.0,max_ns/1e6,(unsigned long long)presses);
    window_start=now; presses=sum_ns=max_ns=window_reads=0;
}

static ABI int32_t pad_init(void) {
    pthread_mutex_lock(&lock); initialized=1; pthread_mutex_unlock(&lock);
    dcpgpu_choice_set_input(list_buttons);
    return 0;
}
/* The slot of an open pad handle, or -1. */
static int open_slot(int32_t handle) { return handle>=1 && handle<=MAX_PADS && slots[handle-1].opened ? handle-1 : -1; }
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (type!=0 && type!=2) return ERR_INVALID_ARG; /* standard / special port */
    if (index) return ERR_INVALID_ARG;
    const int player=runtime_user_player(user);
    if (!player) return ERR_USER_NOT_LOGIN;
    pthread_mutex_lock(&lock);
    PadSlot *s=&slots[player-1];
    const bool already=s->opened;
    if (!already) { s->opened=true; s->user=user; }
    pthread_mutex_unlock(&lock);
    if (already) return ERR_ALREADY_OPENED;
    printf("Runtime: pad %d opened for user %d (%s)\n",player,user,player==1 ? "SDL gamepad or keyboard" : "SDL gamepad");
    pthread_mutex_lock(&lock);
    pthread_mutex_unlock(&lock);
    return player;
}
static ABI int32_t pad_close(int32_t handle) {
    const int slot=open_slot(handle);
    if (slot<0) return ERR_INVALID_HANDLE;
    slots[slot].opened=false; return 0;
}
static ABI int32_t pad_read_state(int32_t handle, PadData *data) {
    const int slot=open_slot(handle);
    if (slot<0) return ERR_INVALID_HANDLE;
    if (!data) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    sample(slot,data); ++reads;
    if (slot==0) {
        trace_read(data);
        dcp_motion_record_read(SDL_GetTicksNS());
    }
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Buffered read: the port samples once per call, so one entry is returned. */
static ABI int32_t pad_read(int32_t handle, PadData *data, int32_t count) {
    if (open_slot(handle)<0) return ERR_INVALID_HANDLE;
    if (!data || count<1 || count>64) return ERR_INVALID_ARG;
    pad_read_state(handle,data);
    return 1;
}
static ABI int32_t pad_info(int32_t handle, ControllerInfo *info) {
    const int slot=open_slot(handle);
    if (slot<0) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connection_type=0; info->device_class=0;
    pthread_mutex_lock(&lock);
    refresh_gamepads();
    info->connected=slot_connected(slot);
    info->connected_count=slots[slot].connected_count ? slots[slot].connected_count : info->connected;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    const int slot=open_slot(handle);
    if (slot<0) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    refresh_gamepads();
    SDL_Gamepad *g=slots[slot].gamepad;
    if (g && dcpgpu_controller_rumble()) SDL_RumbleGamepad(g,(uint16_t)(param[0]*257),(uint16_t)(param[1]*257),1000);
    pthread_mutex_unlock(&lock);
    return 0;
}
void runtime_pad_rumble_changed(void) {
    log_rumble(" (Options menu)");
    if (dcpgpu_controller_rumble()) return;
    pthread_mutex_lock(&lock);
    for (int i=0;i<MAX_PADS;++i) if (slots[i].gamepad) SDL_RumbleGamepad(slots[i].gamepad,0,0,0);
    pthread_mutex_unlock(&lock);
}
static ABI int32_t pad_ok_handle(int32_t handle) { return open_slot(handle)>=0 ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_ok_handle_flag(int32_t handle, uint8_t flag) { (void)flag; return pad_ok_handle(handle); }

static ABI int32_t pad_light_bar(int32_t handle, const uint8_t *rgb) {
    (void)rgb; return pad_ok_handle(handle);
}
/* scePadGetHandle: the handle scePadOpen returned for this user's standard port. */
static ABI int32_t pad_get_handle(int32_t user, int32_t type, int32_t index) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if ((type!=0 && type!=2) || index) return ERR_INVALID_ARG;
    int32_t handle=ERR_NO_HANDLE;
    pthread_mutex_lock(&lock);
    for (int i=0;i<MAX_PADS;++i) if (slots[i].opened && slots[i].user==user) handle=i+1;
    pthread_mutex_unlock(&lock);
    return handle;
}
static const RuntimeExport exports[]={
    {"scePadGetHandle",pad_get_handle},
    {"scePadSetLightBar",pad_light_bar}, {"scePadResetLightBar",pad_ok_handle},
    {"scePadInit",pad_init}, {"scePadOpen",pad_open}, {"scePadClose",pad_close},
    {"scePadReadState",pad_read_state}, {"scePadRead",pad_read},
    {"scePadGetControllerInformation",pad_info}, {"scePadSetVibration",pad_vibration},
    {"scePadResetOrientation",pad_ok_handle},
    {"scePadSetAngularVelocityDeadbandState",pad_ok_handle_flag}, {"scePadSetTiltCorrectionState",pad_ok_handle_flag},
    {"scePadSetMotionSensorState",pad_ok_handle_flag},
};
uintptr_t runtime_pad_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_pad_report(void) {
    printf("Runtime: pad reads=%zu",reads);
    for (int i=0;i<MAX_PADS;++i) printf(", gamepad %d=%s",i+1,slots[i].gamepad ? SDL_GetGamepadName(slots[i].gamepad) : "none");
    putchar('\n');
}
