// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <d3d11.h>
#include <windows.h>
#include <SDL3/SDL.h>

#include "video_core/renderer_opengl/gl_angle.h"

extern "C" void runtime_log_flush(void);

namespace OpenGL {
namespace {

// From ANGLE's eglext_angle.h, which is not a build dependency.
constexpr SDL_EGLAttrib PlatformAngle = 0x3202;
constexpr SDL_EGLAttrib PlatformAngleType = 0x3203;
constexpr SDL_EGLAttrib PlatformAngleTypeD3D11 = 0x3208;
constexpr SDL_EGLAttrib DeviceExt = 0x322C;
constexpr SDL_EGLAttrib D3D11DeviceAngle = 0x33A1;
constexpr SDL_EGLAttrib AttributeListEnd = 0x3038;

using QueryDisplayAttrib = unsigned(SDLCALL*)(void* display, int attribute, SDL_EGLAttrib* value);
using QueryDeviceAttrib = unsigned(SDLCALL*)(void* device, int attribute, SDL_EGLAttrib* value);

SDL_EGLAttrib* SDLCALL Direct3D11DisplayAttributes(void*) {
    constexpr std::array<SDL_EGLAttrib, 3> attributes{PlatformAngleType, PlatformAngleTypeD3D11, AttributeListEnd};
    auto* copy = static_cast<SDL_EGLAttrib*>(SDL_malloc(sizeof(attributes)));
    if (copy) {
        SDL_memcpy(copy, attributes.data(), sizeof(attributes));
    }
    return copy;
}

std::string FeatureLevelName(D3D_FEATURE_LEVEL level) {
    char name[16];
    std::snprintf(name, sizeof(name), "%d_%d", (level >> 12) & 0xF, (level >> 8) & 0xF);
    return name;
}

// By full path: SDL then gets this module by name, and a copy elsewhere on the search path cannot take its place.
void LoadBesideExecutable(const wchar_t* name) {
    wchar_t executable[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    const auto path = std::filesystem::path(executable).parent_path() / name;
    if (!LoadLibraryW(path.c_str())) {
        const std::string reason = path.filename().string() + " could not be loaded from the game's folder (Windows error " +
                                   std::to_string(GetLastError()) + ")";
        ExitAngleUnavailable(reason.c_str());
    }
}

std::string LoadedPath(const char* module_name) {
    const HMODULE module = GetModuleHandleA(module_name);
    char path[MAX_PATH] = "";
    if (!module || GetModuleFileNameA(module, path, sizeof(path)) == 0) {
        return "not loaded";
    }
    return path;
}

} // namespace

void RequestAngleDirect3D11() {
    LoadBesideExecutable(L"libGLESv2.dll");
    LoadBesideExecutable(L"libEGL.dll");
    // Without this SDL takes the driver's own OpenGL ES where one exists, which is not ANGLE.
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
    // eglGetDisplay would let ANGLE choose its backend.
    SDL_SetHint(SDL_HINT_VIDEO_EGL_ALLOW_GETDISPLAY_FALLBACK, "0");
    SDL_EGL_SetAttributeCallbacks(Direct3D11DisplayAttributes, nullptr, nullptr, nullptr);
    SDL_GL_SetAttribute(SDL_GL_EGL_PLATFORM, int(PlatformAngle));
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
}

void ExitAngleUnavailable(const char* reason) {
    const std::string cause = reason && *reason ? reason : "unknown SDL error";
    std::printf("Direct3D 11: the window could not be created: %s\n", cause.c_str());
    std::fflush(stdout);
    runtime_log_flush();
    const std::string message =
        "The Direct3D 11 renderer could not start.\n\n" + cause +
        "\n\nIt needs libEGL.dll and libGLESv2.dll next to the game, and a graphics card with Direct3D feature "
        "level 10_1 or newer. Choose another renderer in the launcher to play.";
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Dragon's Crown Pro", message.c_str(), nullptr);
    std::_Exit(1);
}

std::string AngleDeviceDescription() {
    std::string level = "unknown";
    const auto query_display =
        reinterpret_cast<QueryDisplayAttrib>(SDL_GL_GetProcAddress("eglQueryDisplayAttribEXT"));
    const auto query_device =
        reinterpret_cast<QueryDeviceAttrib>(SDL_GL_GetProcAddress("eglQueryDeviceAttribEXT"));
    SDL_EGLAttrib device = 0, d3d11 = 0;
    if (query_display && query_device && query_display(SDL_EGL_GetCurrentDisplay(), int(DeviceExt), &device) &&
        query_device(reinterpret_cast<void*>(device), int(D3D11DeviceAngle), &d3d11) && d3d11) {
        level = FeatureLevelName(reinterpret_cast<ID3D11Device*>(d3d11)->GetFeatureLevel());
    }
    return "feature level " + level + ", libGLESv2 " + LoadedPath("libGLESv2.dll") + ", libEGL " +
           LoadedPath("libEGL.dll");
}

} // namespace OpenGL

#endif
