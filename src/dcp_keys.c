#include "dcp_keys.h"

#include <SDL3/SDL.h>
#include <string.h>

const DcpKeyInfo dcp_key_actions[DCP_KEY_COUNT] = {
    {"key_left_stick_up", "Left stick up", SDL_SCANCODE_W},
    {"key_left_stick_down", "Left stick down", SDL_SCANCODE_S},
    {"key_left_stick_left", "Left stick left", SDL_SCANCODE_A},
    {"key_left_stick_right", "Left stick right", SDL_SCANCODE_D},
    {"key_right_stick_up", "Right stick up", SDL_SCANCODE_UP},
    {"key_right_stick_down", "Right stick down", SDL_SCANCODE_DOWN},
    {"key_right_stick_left", "Right stick left", SDL_SCANCODE_LEFT},
    {"key_right_stick_right", "Right stick right", SDL_SCANCODE_RIGHT},
    {"key_cross", "Cross (A)", SDL_SCANCODE_SPACE},
    {"key_circle", "Circle (B)", SDL_SCANCODE_LSHIFT},
    {"key_square", "Square (X)", SDL_SCANCODE_E},
    {"key_triangle", "Triangle (Y)", SDL_SCANCODE_Q},
    {"key_l1", "L1", SDL_SCANCODE_1},
    {"key_r1", "R1", SDL_SCANCODE_3},
    {"key_l2", "L2", SDL_SCANCODE_R},
    {"key_r2", "R2", SDL_SCANCODE_F},
    {"key_l3", "L3", SDL_SCANCODE_Z},
    {"key_r3", "R3", SDL_SCANCODE_C},
    {"key_dpad_up", "D-pad up", SDL_SCANCODE_I},
    {"key_dpad_down", "D-pad down", SDL_SCANCODE_K},
    {"key_dpad_left", "D-pad left", SDL_SCANCODE_J},
    {"key_dpad_right", "D-pad right", SDL_SCANCODE_L},
    {"key_options", "Options (Start)", SDL_SCANCODE_RETURN},
    {"key_touchpad_left", "Touchpad left (Back)", SDL_SCANCODE_TAB},
    {"key_touchpad_right", "Touchpad right", SDL_SCANCODE_BACKSPACE},
    {"hotkey_cheats_menu", "Cheats menu", SDL_SCANCODE_F10},
    {"hotkey_window_mode", "Window mode", SDL_SCANCODE_F11},
    {"hotkey_comms_wheel", "Comms Wheel", SDL_SCANCODE_T},
};

static const struct { const char *name; int scancode; } aliases[] = {
    {"space", SDL_SCANCODE_SPACE}, {"enter", SDL_SCANCODE_RETURN}, {"tab", SDL_SCANCODE_TAB},
    {"backspace", SDL_SCANCODE_BACKSPACE}, {"delete", SDL_SCANCODE_DELETE}, {"insert", SDL_SCANCODE_INSERT},
    {"home", SDL_SCANCODE_HOME}, {"end", SDL_SCANCODE_END},
    {"pgup", SDL_SCANCODE_PAGEUP}, {"pgdown", SDL_SCANCODE_PAGEDOWN},
    {"up", SDL_SCANCODE_UP}, {"down", SDL_SCANCODE_DOWN}, {"left", SDL_SCANCODE_LEFT}, {"right", SDL_SCANCODE_RIGHT},
    {"lshift", SDL_SCANCODE_LSHIFT}, {"rshift", SDL_SCANCODE_RSHIFT},
    {"lctrl", SDL_SCANCODE_LCTRL}, {"rctrl", SDL_SCANCODE_RCTRL},
    {"lalt", SDL_SCANCODE_LALT}, {"ralt", SDL_SCANCODE_RALT},
    {"kp0", SDL_SCANCODE_KP_0}, {"kp1", SDL_SCANCODE_KP_1}, {"kp2", SDL_SCANCODE_KP_2}, {"kp3", SDL_SCANCODE_KP_3},
    {"kp4", SDL_SCANCODE_KP_4}, {"kp5", SDL_SCANCODE_KP_5}, {"kp6", SDL_SCANCODE_KP_6}, {"kp7", SDL_SCANCODE_KP_7},
    {"kp8", SDL_SCANCODE_KP_8}, {"kp9", SDL_SCANCODE_KP_9}, {"kpperiod", SDL_SCANCODE_KP_PERIOD},
    {"kpslash", SDL_SCANCODE_KP_DIVIDE}, {"kpasterisk", SDL_SCANCODE_KP_MULTIPLY},
    {"kpminus", SDL_SCANCODE_KP_MINUS}, {"kpplus", SDL_SCANCODE_KP_PLUS}, {"kpenter", SDL_SCANCODE_KP_ENTER},
};

int dcp_key_bindable(int scancode) {
    if (scancode <= SDL_SCANCODE_UNKNOWN || scancode >= SDL_SCANCODE_COUNT || scancode == SDL_SCANCODE_ESCAPE) return 0;
    const char *name = SDL_GetScancodeName((SDL_Scancode)scancode);
    return name && *name;
}

int dcp_key_from_name(const char *name) {
    if (!name || !*name) return 0;
    int scancode=SDL_SCANCODE_UNKNOWN;
    for (size_t i=0;i<sizeof(aliases)/sizeof(*aliases);++i) if (!SDL_strcasecmp(name,aliases[i].name)) scancode=aliases[i].scancode;
    if (!scancode) scancode=(int)SDL_GetScancodeFromName(name);
    return dcp_key_bindable(scancode) ? scancode : 0;
}

const char *dcp_key_name(int scancode, char *buffer, size_t size) {
    if (size) buffer[0]=0;
    if (!dcp_key_bindable(scancode) || !size) return buffer;
    for (size_t i=0;i<sizeof(aliases)/sizeof(*aliases);++i) {
        if (aliases[i].scancode==scancode) { SDL_strlcpy(buffer,aliases[i].name,size); return buffer; }
    }
    SDL_strlcpy(buffer,SDL_GetScancodeName((SDL_Scancode)scancode),size);
    for (char *c=buffer;*c;++c) if (*c>='A' && *c<='Z') *c=(char)(*c-'A'+'a');
    return buffer;
}

void dcp_keys_defaults(DcpKeyMap *map) {
    for (int i=0;i<DCP_KEY_COUNT;++i) map->scancode[i]=dcp_key_actions[i].default_scancode;
}

int dcp_keys_apply(DcpKeyMap *map, const char *ini_key, const char *value) {
    for (int i=0;i<DCP_KEY_COUNT;++i) {
        if (strcmp(ini_key,dcp_key_actions[i].ini_key)) continue;
        const int scancode=dcp_key_from_name(value);
        if (!scancode) return 0;
        map->scancode[i]=scancode;
        return 1;
    }
    return 0;
}

static char *trim(char *text) {
    while (*text==' ' || *text=='\t') ++text;
    char *end=text+strlen(text);
    while (end>text && (end[-1]==' ' || end[-1]=='\t' || end[-1]=='\r')) *--end=0;
    return text;
}

void dcp_ini_visit(const char *path, DcpIniVisitor visit, void *user) {
    size_t size=0;
    char *text=path && *path ? (char *)SDL_LoadFile(path,&size) : NULL;
    if (!text) return;
    char *line=text;
    while (line && *line) {
        char *next=strchr(line,'\n');
        if (next) *next++=0;
        char *eq=strchr(line,'=');
        if (eq && *trim(line)!='#') {
            *eq=0;
            char *value=trim(eq+1);
            char *comment=strstr(value," #");
            if (comment) *comment=0;
            visit(trim(line),trim(value),user);
        }
        line=next;
    }
    SDL_free(text);
}

static void apply_key_line(const char *key, const char *value, void *map) { dcp_keys_apply((DcpKeyMap *)map,key,value); }

void dcp_keys_load(const char *path, DcpKeyMap *map) {
    dcp_keys_defaults(map);
    dcp_ini_visit(path,apply_key_line,map);
}

_Static_assert(DCP_KEY_TOUCHPAD_RIGHT-DCP_KEY_CROSS+1==DCP_PAD_TARGET_COUNT,"pad targets follow the keyboard actions");
_Static_assert(DCP_WHEEL_R2==DCP_PAD_TARGET_R2+1,"wheel targets follow the pad targets");

#define KEY_PREFIX_LENGTH 4 /* "key_" */

const char *dcp_pad_target_name(int target) {
    if (target<0 || target>=DCP_PAD_TARGET_COUNT) return "";
    return dcp_key_actions[DCP_KEY_CROSS+target].ini_key+KEY_PREFIX_LENGTH;
}
const char *dcp_pad_target_label(int target) {
    return target>=0 && target<DCP_PAD_TARGET_COUNT ? dcp_key_actions[DCP_KEY_CROSS+target].label : "";
}
int dcp_pad_target_from_name(const char *name) {
    if (!name || !*name) return -1;
    for (int i=0;i<DCP_PAD_TARGET_COUNT;++i) if (!SDL_strcasecmp(name,dcp_pad_target_name(i))) return i;
    return -1;
}

const DcpMouseActionInfo dcp_mouse_actions[DCP_MOUSE_ACTION_COUNT] = {
    {"mouse_square", "Primary action", DCP_MOUSE_LEFT, DCP_PAD_TARGET_SQUARE},
    {"mouse_circle", "Secondary action", DCP_MOUSE_RIGHT, DCP_PAD_TARGET_CIRCLE},
    {"mouse_pointer_control", "Pointer Control", DCP_MOUSE_MIDDLE, -1},
    {"mouse_pointer_interact", "Pointer Interaction", DCP_MOUSE_LEFT, -1},
    {"mouse_cross", "Cross (A)", DCP_MOUSE_NONE, DCP_PAD_TARGET_CROSS},
    {"mouse_triangle", "Triangle (Y)", DCP_MOUSE_NONE, DCP_PAD_TARGET_TRIANGLE},
    {"mouse_l1", "L1", DCP_MOUSE_NONE, DCP_PAD_TARGET_L1},
    {"mouse_r1", "R1", DCP_MOUSE_NONE, DCP_PAD_TARGET_R1},
    {"mouse_l2", "L2", DCP_MOUSE_NONE, DCP_PAD_TARGET_L2},
    {"mouse_r2", "R2", DCP_MOUSE_NONE, DCP_PAD_TARGET_R2},
    {"mouse_l3", "L3", DCP_MOUSE_NONE, DCP_PAD_TARGET_L3},
    {"mouse_r3", "R3", DCP_MOUSE_NONE, DCP_PAD_TARGET_R3},
    {"mouse_dpad_up", "D-pad up", DCP_MOUSE_NONE, DCP_PAD_TARGET_DPAD_UP},
    {"mouse_dpad_down", "D-pad down", DCP_MOUSE_NONE, DCP_PAD_TARGET_DPAD_DOWN},
    {"mouse_dpad_left", "D-pad left", DCP_MOUSE_NONE, DCP_PAD_TARGET_DPAD_LEFT},
    {"mouse_dpad_right", "D-pad right", DCP_MOUSE_NONE, DCP_PAD_TARGET_DPAD_RIGHT},
    {"mouse_options", "Options (Start)", DCP_MOUSE_NONE, DCP_PAD_TARGET_OPTIONS},
    {"mouse_touchpad_left", "Touchpad left (Back)", DCP_MOUSE_NONE, DCP_PAD_TARGET_TOUCHPAD_LEFT},
    {"mouse_touchpad_right", "Touchpad right", DCP_MOUSE_NONE, DCP_PAD_TARGET_TOUCHPAD_RIGHT},
};
const char *const dcp_wheel_up_key="mouse_wheel_up", *const dcp_wheel_down_key="mouse_wheel_down";
const char *const dcp_pointer_sensitivity_key="mouse_pointer_sensitivity", *const dcp_mouse_controls_key="mouse_controls";
const char *const dcp_pointer_mode_key="mouse_pointer_mode", *const dcp_trackpad_key="mouse_steam_deck_trackpad";

static const struct { const char *name; const char *label; int button; } mouse_buttons[]={
    {"left","Left Mouse",DCP_MOUSE_LEFT}, {"right","Right Mouse",DCP_MOUSE_RIGHT}, {"middle","Middle Mouse",DCP_MOUSE_MIDDLE},
    {"x1","Mouse 4",DCP_MOUSE_X1}, {"x2","Mouse 5",DCP_MOUSE_X2},
};
static const struct { const char *name; const char *label; } pointer_modes[DCP_POINTER_MODE_COUNT]={
    {"hold","Hold"}, {"always_on","Always On"},
};
static const struct { const char *name; const char *label; } trackpad_settings[DCP_TRACKPAD_SETTING_COUNT]={
    {"auto","Auto"}, {"off","Off"},
};

void dcp_mouse_defaults(DcpMouseMap *map) {
    for (int i=0;i<DCP_MOUSE_ACTION_COUNT;++i) map->button[i]=dcp_mouse_actions[i].default_button;
    map->wheel_up=map->wheel_down=DCP_WHEEL_NONE;
    map->sensitivity=DCP_POINTER_SENSITIVITY_DEFAULT;
    map->controls=1;
    map->pointer_mode=DCP_POINTER_MODE_HOLD;
    map->trackpad=DCP_TRACKPAD_AUTO;
}

int dcp_mouse_button_from_name(const char *name) {
    if (!name || !*name) return -1;
    if (!SDL_strcasecmp(name,"none")) return DCP_MOUSE_NONE;
    for (size_t i=0;i<sizeof(mouse_buttons)/sizeof(*mouse_buttons);++i) {
        if (!SDL_strcasecmp(name,mouse_buttons[i].name)) return mouse_buttons[i].button;
    }
    if (!SDL_strncasecmp(name,"mouse",5) && name[5]>='1' && name[5]<='5' && !name[6]) {
        static const int by_number[]={DCP_MOUSE_LEFT,DCP_MOUSE_RIGHT,DCP_MOUSE_MIDDLE,DCP_MOUSE_X1,DCP_MOUSE_X2};
        return by_number[name[5]-'1'];
    }
    return -1;
}

const char *dcp_mouse_button_name(int button) {
    for (size_t i=0;i<sizeof(mouse_buttons)/sizeof(*mouse_buttons);++i) if (mouse_buttons[i].button==button) return mouse_buttons[i].name;
    return "none";
}

const char *dcp_mouse_button_label(int button) {
    for (size_t i=0;i<sizeof(mouse_buttons)/sizeof(*mouse_buttons);++i) if (mouse_buttons[i].button==button) return mouse_buttons[i].label;
    return "(none)";
}

int dcp_pointer_mode_from_name(const char *name) {
    if (!name || !*name) return -1;
    for (int i=0;i<DCP_POINTER_MODE_COUNT;++i) if (!SDL_strcasecmp(name,pointer_modes[i].name)) return i;
    return -1;
}
const char *dcp_pointer_mode_name(int mode) { return mode>=0 && mode<DCP_POINTER_MODE_COUNT ? pointer_modes[mode].name : pointer_modes[DCP_POINTER_MODE_HOLD].name; }
const char *dcp_pointer_mode_label(int mode) { return mode>=0 && mode<DCP_POINTER_MODE_COUNT ? pointer_modes[mode].label : pointer_modes[DCP_POINTER_MODE_HOLD].label; }

int dcp_trackpad_setting_from_name(const char *name) {
    if (!name || !*name) return -1;
    for (int i=0;i<DCP_TRACKPAD_SETTING_COUNT;++i) if (!SDL_strcasecmp(name,trackpad_settings[i].name)) return i;
    return -1;
}
const char *dcp_trackpad_setting_name(int setting) { return setting>=0 && setting<DCP_TRACKPAD_SETTING_COUNT ? trackpad_settings[setting].name : trackpad_settings[DCP_TRACKPAD_AUTO].name; }
const char *dcp_trackpad_setting_label(int setting) { return setting>=0 && setting<DCP_TRACKPAD_SETTING_COUNT ? trackpad_settings[setting].label : trackpad_settings[DCP_TRACKPAD_AUTO].label; }

int dcp_wheel_target_from_name(const char *name) {
    if (!name || !*name) return -1;
    if (!SDL_strcasecmp(name,"none")) return DCP_WHEEL_NONE;
    const int target=dcp_pad_target_from_name(name);
    return target<0 ? -1 : target+1;
}
const char *dcp_wheel_target_name(int target) { return target>0 && target<DCP_WHEEL_TARGET_COUNT ? dcp_pad_target_name(target-1) : "none"; }
const char *dcp_wheel_target_label(int target) { return target>0 && target<DCP_WHEEL_TARGET_COUNT ? dcp_pad_target_label(target-1) : "(none)"; }

int dcp_pointer_sensitivity_valid(int percent) {
    return percent>=DCP_POINTER_SENSITIVITY_MIN && percent<=DCP_POINTER_SENSITIVITY_MAX &&
           (percent-DCP_POINTER_SENSITIVITY_MIN)%DCP_POINTER_SENSITIVITY_STEP==0;
}

int dcp_pointer_sensitivity_normalize(int percent) {
    if (percent<DCP_POINTER_SENSITIVITY_MIN) return DCP_POINTER_SENSITIVITY_MIN;
    if (percent>DCP_POINTER_SENSITIVITY_MAX) return DCP_POINTER_SENSITIVITY_MAX;
    const int steps=(percent-DCP_POINTER_SENSITIVITY_MIN+DCP_POINTER_SENSITIVITY_STEP/2)/DCP_POINTER_SENSITIVITY_STEP;
    return DCP_POINTER_SENSITIVITY_MIN+steps*DCP_POINTER_SENSITIVITY_STEP;
}

static int parse_percent(const char *value) {
    if (!value || !*value) return -1;
    int percent=0;
    for (const char *c=value;*c;++c) {
        if (*c<'0' || *c>'9' || percent>1000) return -1;
        percent=percent*10+(*c-'0');
    }
    return percent;
}

static int parse_flag(const char *value) {
    if (!value) return -1;
    if (!SDL_strcasecmp(value,"1") || !SDL_strcasecmp(value,"on") || !SDL_strcasecmp(value,"yes") || !SDL_strcasecmp(value,"true")) return 1;
    if (!SDL_strcasecmp(value,"0") || !SDL_strcasecmp(value,"off") || !SDL_strcasecmp(value,"no") || !SDL_strcasecmp(value,"false")) return 0;
    return -1;
}

int dcp_mouse_setting_key(const char *ini_key) {
    if (!strcmp(ini_key,dcp_mouse_controls_key)) return 1;
    if (!strcmp(ini_key,dcp_wheel_up_key) || !strcmp(ini_key,dcp_wheel_down_key) || !strcmp(ini_key,dcp_pointer_sensitivity_key) ||
        !strcmp(ini_key,dcp_pointer_mode_key) || !strcmp(ini_key,dcp_trackpad_key)) return 1;
    for (int i=0;i<DCP_MOUSE_ACTION_COUNT;++i) if (!strcmp(ini_key,dcp_mouse_actions[i].ini_key)) return 1;
    return 0;
}

int dcp_mouse_setting_value_valid(const char *ini_key, const char *value) {
    if (!strcmp(ini_key,dcp_mouse_controls_key)) return parse_flag(value)>=0;
    if (!strcmp(ini_key,dcp_pointer_sensitivity_key)) return parse_percent(value)>=0;
    if (!strcmp(ini_key,dcp_pointer_mode_key)) return dcp_pointer_mode_from_name(value)>=0;
    if (!strcmp(ini_key,dcp_trackpad_key)) return dcp_trackpad_setting_from_name(value)>=0;
    if (!strcmp(ini_key,dcp_wheel_up_key) || !strcmp(ini_key,dcp_wheel_down_key)) return dcp_wheel_target_from_name(value)>=0;
    for (int i=0;i<DCP_MOUSE_ACTION_COUNT;++i) {
        if (strcmp(ini_key,dcp_mouse_actions[i].ini_key)) continue;
        const int button=dcp_mouse_button_from_name(value);
        return button>=0 && (button!=DCP_MOUSE_NONE || i!=DCP_MOUSE_POINTER_INTERACT);
    }
    return 0;
}

int dcp_mouse_apply(DcpMouseMap *map, const char *ini_key, const char *value) {
    if (!dcp_mouse_setting_key(ini_key) || !dcp_mouse_setting_value_valid(ini_key,value)) return 0;
    if (!strcmp(ini_key,dcp_mouse_controls_key)) map->controls=parse_flag(value);
    else if (!strcmp(ini_key,dcp_pointer_sensitivity_key)) map->sensitivity=dcp_pointer_sensitivity_normalize(parse_percent(value));
    else if (!strcmp(ini_key,dcp_pointer_mode_key)) map->pointer_mode=dcp_pointer_mode_from_name(value);
    else if (!strcmp(ini_key,dcp_trackpad_key)) map->trackpad=dcp_trackpad_setting_from_name(value);
    else if (!strcmp(ini_key,dcp_wheel_up_key)) map->wheel_up=dcp_wheel_target_from_name(value);
    else if (!strcmp(ini_key,dcp_wheel_down_key)) map->wheel_down=dcp_wheel_target_from_name(value);
    else for (int i=0;i<DCP_MOUSE_ACTION_COUNT;++i) if (!strcmp(ini_key,dcp_mouse_actions[i].ini_key)) map->button[i]=dcp_mouse_button_from_name(value);
    return 1;
}

void dcp_mouse_normalize(DcpMouseMap *map) {
    const int control=map->button[DCP_MOUSE_POINTER_CONTROL], click=map->button[DCP_MOUSE_POINTER_INTERACT];
    if (control!=DCP_MOUSE_NONE && control==click) {
        map->button[DCP_MOUSE_POINTER_CONTROL]=dcp_mouse_actions[DCP_MOUSE_POINTER_CONTROL].default_button;
        map->button[DCP_MOUSE_POINTER_INTERACT]=dcp_mouse_actions[DCP_MOUSE_POINTER_INTERACT].default_button;
    }
}

int dcp_mouse_binding_allowed(const DcpMouseMap *map, DcpMouseAction action, int button) {
    if (button<0 || button>=DCP_MOUSE_BUTTON_COUNT) return 0;
    if (action==DCP_MOUSE_POINTER_INTERACT) return button!=DCP_MOUSE_NONE && button!=map->button[DCP_MOUSE_POINTER_CONTROL];
    if (action==DCP_MOUSE_POINTER_CONTROL) return button==DCP_MOUSE_NONE || button!=map->button[DCP_MOUSE_POINTER_INTERACT];
    return 1;
}

typedef struct { DcpMouseMap *map; DcpMouseLoadInfo *info; } MouseLoad;

static void apply_mouse_line(const char *key, const char *value, void *user) {
    MouseLoad *load=user;
    if (dcp_mouse_apply(load->map,key,value)) return;
    if (dcp_mouse_setting_key(key) && *value) ++load->info->rejected;
}

void dcp_mouse_load_info(const char *path, DcpMouseMap *map, DcpMouseLoadInfo *info) {
    DcpMouseLoadInfo local={0};
    if (!info) info=&local;
    memset(info,0,sizeof(*info));
    dcp_mouse_defaults(map);
    MouseLoad load={map,info};
    dcp_ini_visit(path,apply_mouse_line,&load);
    const int control=map->button[DCP_MOUSE_POINTER_CONTROL], click=map->button[DCP_MOUSE_POINTER_INTERACT];
    if (control!=DCP_MOUSE_NONE && control==click) ++info->rejected;
    dcp_mouse_normalize(map);
    for (int i=DCP_MOUSE_FIRST_EXTENDED;i<DCP_MOUSE_ACTION_COUNT;++i) if (map->button[i]!=DCP_MOUSE_NONE) ++info->extended_bound;
}

void dcp_mouse_load(const char *path, DcpMouseMap *map) { dcp_mouse_load_info(path,map,NULL); }

unsigned dcp_mouse_button_targets(const DcpMouseMap *map, int button) {
    unsigned targets=0;
    if (button<=DCP_MOUSE_NONE || button>=DCP_MOUSE_BUTTON_COUNT) return 0;
    for (int i=0;i<DCP_MOUSE_ACTION_COUNT;++i) {
        if (dcp_mouse_actions[i].target>=0 && map->button[i]==button) targets|=1u<<dcp_mouse_actions[i].target;
    }
    return targets;
}

int dcp_mouse_button_locked(const DcpMouseMap *map, int button) {
    if (button<=DCP_MOUSE_NONE || button>=DCP_MOUSE_BUTTON_COUNT) return 0;
    return map->pointer_mode==DCP_POINTER_MODE_ALWAYS_ON ? map->button[DCP_MOUSE_POINTER_INTERACT]==button
                                                         : map->button[DCP_MOUSE_POINTER_CONTROL]==button;
}

int dcp_mouse_assign_target(DcpMouseMap *map, int button, int target, int *moved_from) {
    if (moved_from) *moved_from=DCP_MOUSE_NONE;
    if (button<=DCP_MOUSE_NONE || button>=DCP_MOUSE_BUTTON_COUNT || target<-1 || target>=DCP_PAD_TARGET_COUNT) return 0;
    if (dcp_mouse_button_locked(map,button)) return 0;
    int chosen=-1;
    for (int i=0;i<DCP_MOUSE_ACTION_COUNT;++i) {
        if (dcp_mouse_actions[i].target<0) continue;
        if (dcp_mouse_actions[i].target==target) chosen=i;
        else if (map->button[i]==button) map->button[i]=DCP_MOUSE_NONE;
    }
    if (chosen<0) return 1;
    if (map->button[chosen]!=button && moved_from) *moved_from=map->button[chosen];
    map->button[chosen]=button;
    return 1;
}
