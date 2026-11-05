// SPDX-License-Identifier: GPL-2.0-or-later
// OpenGL equivalents of the formats and fixed-function values the shared PS4 metadata describes.
#pragma once

#include "video_core/renderer_opengl/gl_api.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace OpenGL {

struct TextureFormat {
    GLenum internal_format;
    GLenum format;
    GLenum type;
    bool compressed;
};

struct VertexFormat {
    GLint size;
    GLenum type;
    bool normalized;
    bool integer;
};

/// ImageInfo and the shader specializations speak vk::Format: it is the shared format name.
/// Null for a format the backend does not map yet.
const TextureFormat* FindTextureFormat(vk::Format format);
const VertexFormat* FindVertexFormat(vk::Format format);

GLenum CompareFunc(vk::CompareOp op);
GLenum StencilOp(vk::StencilOp op);
GLenum WrapMode(vk::SamplerAddressMode mode);
GLenum LogicOp(vk::LogicOp op);
GLenum BlendFactor(vk::BlendFactor factor);
GLenum BlendEquation(vk::BlendOp op);
GLint Swizzle(vk::ComponentSwizzle swizzle, GLint identity);

} // namespace OpenGL
