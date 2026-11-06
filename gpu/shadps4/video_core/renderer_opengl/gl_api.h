// SPDX-License-Identifier: GPL-2.0-or-later
// OpenGL entry points, loaded through SDL so the executable does not link an OpenGL library.
#pragma once

#include <string>
#include <SDL3/SDL_opengl.h>

// Prototypes SDL_opengl.h declares for OpenGL 1.0 to 1.3: only their types are used.
#define DCP_GL_CORE_FUNCTIONS(X)                                                                   \
    X(glActiveTexture, ActiveTexture)                                                              \
    X(glBindTexture, BindTexture)                                                                  \
    X(glBlendColor, BlendColor)                                                                    \
    X(glClear, Clear)                                                                              \
    X(glClearColor, ClearColor)                                                                    \
    X(glCompressedTexImage2D, CompressedTexImage2D)                                                \
    X(glCompressedTexImage3D, CompressedTexImage3D)                                                \
    X(glCompressedTexSubImage2D, CompressedTexSubImage2D)                                          \
    X(glCompressedTexSubImage3D, CompressedTexSubImage3D)                                          \
    X(glCullFace, CullFace)                                                                        \
    X(glDeleteTextures, DeleteTextures)                                                            \
    X(glDepthFunc, DepthFunc)                                                                      \
    X(glDepthMask, DepthMask)                                                                      \
    X(glDepthRange, DepthRange)                                                                    \
    X(glDisable, Disable)                                                                          \
    X(glDrawArrays, DrawArrays)                                                                    \
    X(glEnable, Enable)                                                                            \
    X(glFinish, Finish)                                                                            \
    X(glFlush, Flush)                                                                              \
    X(glFrontFace, FrontFace)                                                                      \
    X(glGenTextures, GenTextures)                                                                  \
    X(glGetError, GetError)                                                                        \
    X(glGetFloatv, GetFloatv)                                                                      \
    X(glGetIntegerv, GetIntegerv)                                                                  \
    X(glGetString, GetString)                                                                      \
    X(glLineWidth, LineWidth)                                                                      \
    X(glLogicOp, LogicOp)                                                                          \
    X(glPixelStorei, PixelStorei)                                                                  \
    X(glPolygonMode, PolygonMode)                                                                  \
    X(glPolygonOffset, PolygonOffset)                                                              \
    X(glReadBuffer, ReadBuffer)                                                                    \
    X(glScissor, Scissor)                                                                          \
    X(glTexImage1D, TexImage1D)                                                                    \
    X(glTexImage2D, TexImage2D)                                                                    \
    X(glTexImage3D, TexImage3D)                                                                    \
    X(glTexParameteri, TexParameteri)                                                              \
    X(glTexParameteriv, TexParameteriv)                                                            \
    X(glTexSubImage1D, TexSubImage1D)                                                              \
    X(glTexSubImage2D, TexSubImage2D)                                                              \
    X(glTexSubImage3D, TexSubImage3D)                                                              \
    X(glViewport, Viewport)

#define DCP_GL_EXTENSION_FUNCTIONS(X)                                                              \
    X(PFNGLATTACHSHADERPROC, AttachShader)                                                         \
    X(PFNGLBINDBUFFERPROC, BindBuffer)                                                             \
    X(PFNGLBINDFRAMEBUFFERPROC, BindFramebuffer)                                                   \
    X(PFNGLBINDSAMPLERPROC, BindSampler)                                                           \
    X(PFNGLBINDVERTEXARRAYPROC, BindVertexArray)                                                   \
    X(PFNGLBLENDEQUATIONSEPARATEPROC, BlendEquationSeparate)                                       \
    X(PFNGLBLENDFUNCSEPARATEPROC, BlendFuncSeparate)                                               \
    X(PFNGLBLITFRAMEBUFFERPROC, BlitFramebuffer)                                                   \
    X(PFNGLBUFFERDATAPROC, BufferData)                                                             \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, CheckFramebufferStatus)                                     \
    X(PFNGLCLIENTWAITSYNCPROC, ClientWaitSync)                                                     \
    X(PFNGLCLEARBUFFERFVPROC, ClearBufferfv)                                                       \
    X(PFNGLCLEARBUFFERIVPROC, ClearBufferiv)                                                       \
    X(PFNGLCLEARBUFFERUIVPROC, ClearBufferuiv)                                                     \
    X(PFNGLCOLORMASKIPROC, ColorMaski)                                                             \
    X(PFNGLCOMPILESHADERPROC, CompileShader)                                                       \
    X(PFNGLCREATEPROGRAMPROC, CreateProgram)                                                       \
    X(PFNGLCREATESHADERPROC, CreateShader)                                                         \
    X(PFNGLDELETEBUFFERSPROC, DeleteBuffers)                                                       \
    X(PFNGLDELETEFRAMEBUFFERSPROC, DeleteFramebuffers)                                             \
    X(PFNGLDELETEPROGRAMPROC, DeleteProgram)                                                       \
    X(PFNGLDELETESAMPLERSPROC, DeleteSamplers)                                                     \
    X(PFNGLDELETESHADERPROC, DeleteShader)                                                         \
    X(PFNGLDELETESYNCPROC, DeleteSync)                                                             \
    X(PFNGLDELETEVERTEXARRAYSPROC, DeleteVertexArrays)                                             \
    X(PFNGLDISABLEIPROC, Disablei)                                                                 \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, DisableVertexAttribArray)                                 \
    X(PFNGLDRAWARRAYSINSTANCEDPROC, DrawArraysInstanced)                                           \
    X(PFNGLDRAWBUFFERSPROC, DrawBuffers)                                                           \
    X(PFNGLDRAWELEMENTSINSTANCEDBASEVERTEXPROC, DrawElementsInstancedBaseVertex)                   \
    X(PFNGLENABLEIPROC, Enablei)                                                                   \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, EnableVertexAttribArray)                                   \
    X(PFNGLFENCESYNCPROC, FenceSync)                                                               \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, FramebufferTexture2D)                                         \
    X(PFNGLGENBUFFERSPROC, GenBuffers)                                                             \
    X(PFNGLGENFRAMEBUFFERSPROC, GenFramebuffers)                                                   \
    X(PFNGLGENSAMPLERSPROC, GenSamplers)                                                           \
    X(PFNGLGENVERTEXARRAYSPROC, GenVertexArrays)                                                   \
    X(PFNGLGETPROGRAMINFOLOGPROC, GetProgramInfoLog)                                               \
    X(PFNGLGETPROGRAMIVPROC, GetProgramiv)                                                         \
    X(PFNGLGETSHADERINFOLOGPROC, GetShaderInfoLog)                                                 \
    X(PFNGLGETSHADERIVPROC, GetShaderiv)                                                           \
    X(PFNGLGETSTRINGIPROC, GetStringi)                                                             \
    X(PFNGLGETUNIFORMLOCATIONPROC, GetUniformLocation)                                             \
    X(PFNGLLINKPROGRAMPROC, LinkProgram)                                                           \
    X(PFNGLMAPBUFFERRANGEPROC, MapBufferRange)                                                     \
    X(PFNGLPRIMITIVERESTARTINDEXPROC, PrimitiveRestartIndex)                                       \
    X(PFNGLPROVOKINGVERTEXPROC, ProvokingVertex)                                                   \
    X(PFNGLSAMPLERPARAMETERFPROC, SamplerParameterf)                                               \
    X(PFNGLSAMPLERPARAMETERFVPROC, SamplerParameterfv)                                             \
    X(PFNGLSAMPLERPARAMETERIPROC, SamplerParameteri)                                               \
    X(PFNGLSHADERSOURCEPROC, ShaderSource)                                                         \
    X(PFNGLSTENCILFUNCSEPARATEPROC, StencilFuncSeparate)                                           \
    X(PFNGLSTENCILMASKSEPARATEPROC, StencilMaskSeparate)                                           \
    X(PFNGLSTENCILOPSEPARATEPROC, StencilOpSeparate)                                               \
    X(PFNGLTEXBUFFERPROC, TexBuffer)                                                               \
    X(PFNGLUNIFORM1FPROC, Uniform1f)                                                               \
    X(PFNGLUNIFORM1IPROC, Uniform1i)                                                               \
    X(PFNGLUNIFORM1UIPROC, Uniform1ui)                                                             \
    X(PFNGLUNIFORM2UIVPROC, Uniform2uiv)                                                           \
    X(PFNGLUNIFORM4FPROC, Uniform4f)                                                               \
    X(PFNGLUNIFORM4UIVPROC, Uniform4uiv)                                                           \
    X(PFNGLUNMAPBUFFERPROC, UnmapBuffer)                                                           \
    X(PFNGLUSEPROGRAMPROC, UseProgram)                                                             \
    X(PFNGLVERTEXATTRIBDIVISORPROC, VertexAttribDivisor)                                           \
    X(PFNGLVERTEXATTRIBIPOINTERPROC, VertexAttribIPointer)                                         \
    X(PFNGLVERTEXATTRIBPOINTERPROC, VertexAttribPointer)

// Absent on a plain 3.3 context: per-target blending (OpenGL 4.0) and persistent buffers
// (ARB_buffer_storage). Check the capability, not the pointer: a driver can return one anyway.
// The last three are OpenGL ES core calls the desktop path never makes.
#define DCP_GL_OPTIONAL_FUNCTIONS(X)                                                               \
    X(PFNGLBLENDEQUATIONSEPARATEIPROC, BlendEquationSeparatei)                                     \
    X(PFNGLBLENDFUNCSEPARATEIPROC, BlendFuncSeparatei)                                             \
    X(PFNGLBUFFERSTORAGEPROC, BufferStorage)                                                       \
    X(PFNGLDEPTHRANGEFPROC, DepthRangef)                                                           \
    X(PFNGLDRAWELEMENTSINSTANCEDPROC, DrawElementsInstanced)                                       \
    X(PFNGLTEXSTORAGE2DPROC, TexStorage2D)

namespace gl {

#define DCP_GL_DECLARATION_CORE(prototype, name) extern decltype(&::prototype) name;
#define DCP_GL_DECLARATION(type, name) extern type name;
DCP_GL_CORE_FUNCTIONS(DCP_GL_DECLARATION_CORE)
DCP_GL_EXTENSION_FUNCTIONS(DCP_GL_DECLARATION)
DCP_GL_OPTIONAL_FUNCTIONS(DCP_GL_DECLARATION)
#undef DCP_GL_DECLARATION_CORE
#undef DCP_GL_DECLARATION

/// With a context current. False and the entry point's name when a required one is missing.
/// `gles`: OpenGL ES has no equivalent for some, and spells others with an extension suffix.
bool Load(std::string& missing, bool gles = false);

} // namespace gl
