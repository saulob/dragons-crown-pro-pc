// Stands in for <GLES3/gl3.h> in the one file that includes it: imgui's backend calls OpenGL ES by name, and these
// names go through pointers SDL resolves from ANGLE, so the executable links no OpenGL ES library.
#pragma once

#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_video.h>

// Desktop features the ES3 build must not see: the backend tests these macros, not its target.
#undef GL_CLIP_ORIGIN
#undef GL_POLYGON_MODE

#define DCP_IMGUI_GLES3_FUNCTIONS(X)                                                               \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                                                       \
    X(PFNGLBINDBUFFERPROC, glBindBuffer)                                                           \
    X(PFNGLBINDSAMPLERPROC, glBindSampler)                                                         \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                                                 \
    X(PFNGLBLENDEQUATIONPROC, glBlendEquation)                                                     \
    X(PFNGLBLENDEQUATIONSEPARATEPROC, glBlendEquationSeparate)                                     \
    X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate)                                             \
    X(PFNGLBUFFERDATAPROC, glBufferData)                                                           \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData)                                                     \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                                                     \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                                                     \
    X(PFNGLCREATESHADERPROC, glCreateShader)                                                       \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)                                                     \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram)                                                     \
    X(PFNGLDELETESAMPLERSPROC, glDeleteSamplers)                                                   \
    X(PFNGLDELETESHADERPROC, glDeleteShader)                                                       \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays)                                           \
    X(PFNGLDETACHSHADERPROC, glDetachShader)                                                       \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray)                               \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)                                 \
    X(PFNGLGENBUFFERSPROC, glGenBuffers)                                                           \
    X(PFNGLGENSAMPLERSPROC, glGenSamplers)                                                         \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                                                 \
    X(PFNGLGETATTRIBLOCATIONPROC, glGetAttribLocation)                                             \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                                             \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                                       \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                                               \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                                         \
    X(PFNGLGETSTRINGIPROC, glGetStringi)                                                           \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                                           \
    X(PFNGLGETVERTEXATTRIBIVPROC, glGetVertexAttribiv)                                             \
    X(PFNGLGETVERTEXATTRIBPOINTERVPROC, glGetVertexAttribPointerv)                                 \
    X(PFNGLISPROGRAMPROC, glIsProgram)                                                             \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                                         \
    X(PFNGLSAMPLERPARAMETERIPROC, glSamplerParameteri)                                             \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                                                       \
    X(PFNGLUNIFORM1IPROC, glUniform1i)                                                             \
    X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv)                                               \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                                                           \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)

#define DCP_IMGUI_GLES3_CORE_FUNCTIONS(X)                                                          \
    X(glActiveTexture)                                                                             \
    X(glBindTexture)                                                                               \
    X(glDeleteTextures)                                                                            \
    X(glDisable)                                                                                   \
    X(glDrawElements)                                                                              \
    X(glEnable)                                                                                    \
    X(glGenTextures)                                                                               \
    X(glGetError)                                                                                  \
    X(glGetIntegerv)                                                                               \
    X(glGetString)                                                                                 \
    X(glIsEnabled)                                                                                 \
    X(glPixelStorei)                                                                               \
    X(glScissor)                                                                                   \
    X(glTexImage2D)                                                                                \
    X(glTexParameteri)                                                                             \
    X(glTexSubImage2D)                                                                             \
    X(glViewport)

#define DCP_DECLARE(type, name) static type dcp_##name;
DCP_IMGUI_GLES3_FUNCTIONS(DCP_DECLARE)
#undef DCP_DECLARE
#define DCP_DECLARE(name) static decltype(&::name) dcp_##name;
DCP_IMGUI_GLES3_CORE_FUNCTIONS(DCP_DECLARE)
#undef DCP_DECLARE

static bool LoadGles3ForImGui() {
#define DCP_RESOLVE(type, name)                                                                    \
    dcp_##name = reinterpret_cast<type>(SDL_GL_GetProcAddress(#name));                             \
    if (!dcp_##name) {                                                                             \
        return false;                                                                              \
    }
#define DCP_RESOLVE_CORE(name)                                                                     \
    dcp_##name = reinterpret_cast<decltype(dcp_##name)>(SDL_GL_GetProcAddress(#name));             \
    if (!dcp_##name) {                                                                             \
        return false;                                                                              \
    }
    DCP_IMGUI_GLES3_FUNCTIONS(DCP_RESOLVE)
    DCP_IMGUI_GLES3_CORE_FUNCTIONS(DCP_RESOLVE_CORE)
#undef DCP_RESOLVE
#undef DCP_RESOLVE_CORE
    return true;
}

#define glActiveTexture dcp_glActiveTexture
#define glAttachShader dcp_glAttachShader
#define glBindBuffer dcp_glBindBuffer
#define glBindSampler dcp_glBindSampler
#define glBindTexture dcp_glBindTexture
#define glBindVertexArray dcp_glBindVertexArray
#define glBlendEquation dcp_glBlendEquation
#define glBlendEquationSeparate dcp_glBlendEquationSeparate
#define glBlendFuncSeparate dcp_glBlendFuncSeparate
#define glBufferData dcp_glBufferData
#define glBufferSubData dcp_glBufferSubData
#define glCompileShader dcp_glCompileShader
#define glCreateProgram dcp_glCreateProgram
#define glCreateShader dcp_glCreateShader
#define glDeleteBuffers dcp_glDeleteBuffers
#define glDeleteProgram dcp_glDeleteProgram
#define glDeleteSamplers dcp_glDeleteSamplers
#define glDeleteShader dcp_glDeleteShader
#define glDeleteTextures dcp_glDeleteTextures
#define glDeleteVertexArrays dcp_glDeleteVertexArrays
#define glDetachShader dcp_glDetachShader
#define glDisable dcp_glDisable
#define glDisableVertexAttribArray dcp_glDisableVertexAttribArray
#define glDrawElements dcp_glDrawElements
#define glEnable dcp_glEnable
#define glEnableVertexAttribArray dcp_glEnableVertexAttribArray
#define glGenBuffers dcp_glGenBuffers
#define glGenSamplers dcp_glGenSamplers
#define glGenTextures dcp_glGenTextures
#define glGenVertexArrays dcp_glGenVertexArrays
#define glGetAttribLocation dcp_glGetAttribLocation
#define glGetError dcp_glGetError
#define glGetIntegerv dcp_glGetIntegerv
#define glGetProgramInfoLog dcp_glGetProgramInfoLog
#define glGetProgramiv dcp_glGetProgramiv
#define glGetShaderInfoLog dcp_glGetShaderInfoLog
#define glGetShaderiv dcp_glGetShaderiv
#define glGetString dcp_glGetString
#define glGetStringi dcp_glGetStringi
#define glGetUniformLocation dcp_glGetUniformLocation
#define glGetVertexAttribPointerv dcp_glGetVertexAttribPointerv
#define glGetVertexAttribiv dcp_glGetVertexAttribiv
#define glIsEnabled dcp_glIsEnabled
#define glIsProgram dcp_glIsProgram
#define glLinkProgram dcp_glLinkProgram
#define glPixelStorei dcp_glPixelStorei
#define glSamplerParameteri dcp_glSamplerParameteri
#define glScissor dcp_glScissor
#define glShaderSource dcp_glShaderSource
#define glTexImage2D dcp_glTexImage2D
#define glTexParameteri dcp_glTexParameteri
#define glTexSubImage2D dcp_glTexSubImage2D
#define glUniform1i dcp_glUniform1i
#define glUniformMatrix4fv dcp_glUniformMatrix4fv
#define glUseProgram dcp_glUseProgram
#define glVertexAttribPointer dcp_glVertexAttribPointer
#define glViewport dcp_glViewport
