#pragma once
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#endif
#ifndef DCP_SDL_COMPAT_POLICY_ONLY
#include <SDL3/SDL.h>
#endif

typedef enum { DCP_HIDAPI_LEAVE, DCP_HIDAPI_DISABLE, DCP_HIDAPI_PRESERVE } DcpHidapiAction;

static inline DcpHidapiAction dcp_hidapi_action(int under_wine, const char *configured) {
    if (configured && configured[0]) return DCP_HIDAPI_PRESERVE;
    return under_wine ? DCP_HIDAPI_DISABLE : DCP_HIDAPI_LEAVE;
}

static inline int dcp_running_under_wine(void) {
#ifdef _WIN32
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != NULL;
#else
    return 0;
#endif
}

#ifndef DCP_SDL_COMPAT_POLICY_ONLY
/* Before the first SDL_INIT_GAMEPAD, which reads the hint; later calls see "0" and do nothing. */
static inline void dcp_prepare_sdl_gamepad(void) {
    if (dcp_hidapi_action(dcp_running_under_wine(), SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI)) != DCP_HIDAPI_DISABLE) return;
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    printf("Runtime: Proton/Wine detected; SDL HIDAPI gamepads disabled for compatibility\n");
}
#endif
