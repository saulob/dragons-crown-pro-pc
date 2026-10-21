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

void dcp_keys_load(const char *path, DcpKeyMap *map) {
    dcp_keys_defaults(map);
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
            dcp_keys_apply(map,trim(line),trim(value));
        }
        line=next;
    }
    SDL_free(text);
}
