// SPDX-License-Identifier: GPL-2.0-or-later
// OpenGL equivalents of the formats and fixed-function values the shared PS4 metadata describes.
#pragma once

#include <array>
#include "common/types.h"
#include "video_core/renderer_opengl/gl_api.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace OpenGL {

struct Capabilities;

/// OpenGL ES uploads only red-first layouts: other guest layouts are rearranged on the CPU.
enum class TexelConversion : u8 { None, SwapRedBlue8888, SwapRedBlue2101010, B5G6R5, B4G4R4A4, A1R5G5B5 };

/// What OpenGL ES needs beyond core for a format.
enum class FormatNeed : u8 { None, S3tcSrgb, Rgtc, Bptc, Norm16 };

struct TextureFormat {
    GLenum internal_format;
    GLenum format;
    GLenum type;
    bool compressed;
    TexelConversion conversion = TexelConversion::None;
    FormatNeed need = FormatNeed::None;
};

struct VertexFormat {
    GLint size;
    GLenum type;
    bool normalized;
    bool integer;
};

/// ImageInfo and the shader specializations speak vk::Format: it is the shared format name.
/// Null for a format the backend does not map yet.
const TextureFormat* FindTextureFormat(vk::Format format, bool gles = false);
const VertexFormat* FindVertexFormat(vk::Format format);

/// Whether guest memory holds this image with blue first: the presenter's channel order depends on it.
bool StoresBlueFirst(const TextureFormat& format);

using VertexElement = std::array<u32, 4>;
u32 VertexFormatBytes(const VertexFormat& format);
/// What the vertex stage would see for this element, as the bits of its vec4, uvec4 or ivec4. Past
/// `size` it is what an attribute without data reads, as hardware's bounds check gives.
VertexElement ReadVertexElement(const VertexFormat& format, const u8* data, u64 size, u32 stride,
                                u64 element);
bool FormatAvailable(const TextureFormat& format, const Capabilities& capabilities);
void ConvertTexels(TexelConversion conversion, u8* texels, size_t bytes);

GLenum CompareFunc(vk::CompareOp op);
GLenum StencilOp(vk::StencilOp op);
GLenum WrapMode(vk::SamplerAddressMode mode);
GLenum LogicOp(vk::LogicOp op);
GLenum BlendFactor(vk::BlendFactor factor);
GLenum BlendEquation(vk::BlendOp op);
GLint Swizzle(vk::ComponentSwizzle swizzle, GLint identity);

} // namespace OpenGL
