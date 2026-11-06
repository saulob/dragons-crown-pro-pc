// SPDX-License-Identifier: GPL-2.0-or-later

#include <SDL3/SDL_video.h>

#include "video_core/renderer_opengl/gl_api.h"

namespace gl {

#define DCP_GL_DEFINITION_CORE(prototype, name) decltype(&::prototype) name;
#define DCP_GL_DEFINITION(type, name) type name;
DCP_GL_CORE_FUNCTIONS(DCP_GL_DEFINITION_CORE)
DCP_GL_EXTENSION_FUNCTIONS(DCP_GL_DEFINITION)
DCP_GL_OPTIONAL_FUNCTIONS(DCP_GL_DEFINITION)
#undef DCP_GL_DEFINITION_CORE
#undef DCP_GL_DEFINITION

bool Load(std::string& missing) {
#define DCP_GL_RESOLVE_CORE(prototype, name)                                                       \
    name = reinterpret_cast<decltype(name)>(SDL_GL_GetProcAddress(#prototype));                    \
    if (!name) {                                                                                   \
        missing = #prototype;                                                                      \
        return false;                                                                              \
    }
#define DCP_GL_RESOLVE_REQUIRED(type, name)                                                        \
    name = reinterpret_cast<type>(SDL_GL_GetProcAddress("gl" #name));                              \
    if (!name) {                                                                                   \
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
    return true;
}

} // namespace gl
