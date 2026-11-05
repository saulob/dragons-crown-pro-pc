// SPDX-License-Identifier: GPL-2.0-or-later

#include <SDL3/SDL_video.h>

#include "video_core/renderer_opengl/gl_api.h"

namespace gl {

#define DCP_GL_DEFINE_CORE(prototype, name) decltype(&::prototype) name;
#define DCP_GL_DEFINE(type, name) type name;
DCP_GL_CORE_FUNCTIONS(DCP_GL_DEFINE_CORE)
DCP_GL_EXTENSION_FUNCTIONS(DCP_GL_DEFINE)
DCP_GL_OPTIONAL_FUNCTIONS(DCP_GL_DEFINE)
#undef DCP_GL_DEFINE_CORE
#undef DCP_GL_DEFINE

bool Load(std::string& missing) {
#define DCP_GL_LOAD_CORE(prototype, name)                                                          \
    name = reinterpret_cast<decltype(name)>(SDL_GL_GetProcAddress(#prototype));                    \
    if (!name) {                                                                                   \
        missing = #prototype;                                                                      \
        return false;                                                                              \
    }
#define DCP_GL_LOAD(type, name)                                                                    \
    name = reinterpret_cast<type>(SDL_GL_GetProcAddress("gl" #name));                              \
    if (!name) {                                                                                   \
        missing = "gl" #name;                                                                      \
        return false;                                                                              \
    }
#define DCP_GL_LOAD_OPTIONAL(type, name)                                                           \
    name = reinterpret_cast<type>(SDL_GL_GetProcAddress("gl" #name));
    DCP_GL_CORE_FUNCTIONS(DCP_GL_LOAD_CORE)
    DCP_GL_EXTENSION_FUNCTIONS(DCP_GL_LOAD)
    DCP_GL_OPTIONAL_FUNCTIONS(DCP_GL_LOAD_OPTIONAL)
#undef DCP_GL_LOAD_CORE
#undef DCP_GL_LOAD
#undef DCP_GL_LOAD_OPTIONAL
    return true;
}

} // namespace gl
