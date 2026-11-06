// SPDX-License-Identifier: GPL-2.0-or-later

#include <string_view>
#include <SDL3/SDL_video.h>

#include "video_core/renderer_opengl/gl_api.h"

namespace gl {
namespace {

bool AbsentFromGles(std::string_view name) {
    for (const std::string_view absent :
         {"glDepthRange", "glLogicOp", "glPolygonMode", "glTexImage1D", "glTexSubImage1D",
          "glPrimitiveRestartIndex", "glProvokingVertex", "glTexBuffer",
          "glDrawElementsInstancedBaseVertex", "glEnablei", "glDisablei", "glColorMaski"}) {
        if (name == absent) {
            return true;
        }
    }
    return false;
}

template <typename Function>
void ResolveSuffixed(Function& function, const std::string& name) {
    function = nullptr;
    for (const char* suffix : {"EXT", "OES"}) {
        function = reinterpret_cast<Function>(SDL_GL_GetProcAddress((name + suffix).c_str()));
        if (function) {
            return;
        }
    }
}

} // namespace

#define DCP_GL_DEFINITION_CORE(prototype, name) decltype(&::prototype) name;
#define DCP_GL_DEFINITION(type, name) type name;
DCP_GL_CORE_FUNCTIONS(DCP_GL_DEFINITION_CORE)
DCP_GL_EXTENSION_FUNCTIONS(DCP_GL_DEFINITION)
DCP_GL_OPTIONAL_FUNCTIONS(DCP_GL_DEFINITION)
#undef DCP_GL_DEFINITION_CORE
#undef DCP_GL_DEFINITION

bool Load(std::string& missing, bool gles) {
#define DCP_GL_RESOLVE_CORE(prototype, name)                                                       \
    name = reinterpret_cast<decltype(name)>(SDL_GL_GetProcAddress(#prototype));                    \
    if (!name && !(gles && AbsentFromGles(#prototype))) {                                          \
        missing = #prototype;                                                                      \
        return false;                                                                              \
    }
#define DCP_GL_RESOLVE_REQUIRED(type, name)                                                        \
    name = reinterpret_cast<type>(SDL_GL_GetProcAddress("gl" #name));                              \
    if (!name && !(gles && AbsentFromGles("gl" #name))) {                                          \
        missing = "gl" #name;                                                                      \
        return false;                                                                              \
    }
#define DCP_GL_RESOLVE_OPTIONAL(type, name)                                                        \
    name = reinterpret_cast<type>(SDL_GL_GetProcAddress("gl" #name));
    DCP_GL_CORE_FUNCTIONS(DCP_GL_RESOLVE_CORE)
    DCP_GL_EXTENSION_FUNCTIONS(DCP_GL_RESOLVE_REQUIRED)
    DCP_GL_OPTIONAL_FUNCTIONS(DCP_GL_RESOLVE_OPTIONAL)
#undef DCP_GL_RESOLVE_CORE
#undef DCP_GL_RESOLVE_REQUIRED
#undef DCP_GL_RESOLVE_OPTIONAL
    if (gles) {
        // ANGLE exports the unsuffixed names too, but validates them against ES 3.2.
        ResolveSuffixed(Enablei, "glEnablei");
        ResolveSuffixed(Disablei, "glDisablei");
        ResolveSuffixed(ColorMaski, "glColorMaski");
        ResolveSuffixed(BlendEquationSeparatei, "glBlendEquationSeparatei");
        ResolveSuffixed(BlendFuncSeparatei, "glBlendFuncSeparatei");
        ProvokingVertex = reinterpret_cast<PFNGLPROVOKINGVERTEXPROC>(
            SDL_GL_GetProcAddress("glProvokingVertexANGLE"));
        PolygonMode = reinterpret_cast<decltype(PolygonMode)>(
            SDL_GL_GetProcAddress("glPolygonModeANGLE"));
    }
    return true;
}

} // namespace gl
