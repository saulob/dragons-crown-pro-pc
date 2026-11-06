// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>
#include <unordered_map>

#include "video_core/renderer_opengl/gl_capabilities.h"
#include "video_core/renderer_opengl/gl_format.h"

namespace OpenGL {
namespace {

using TextureTable = std::unordered_map<vk::Format, TextureFormat>;

const TextureTable& DesktopTextures() {
    using F = vk::Format;
    static constexpr auto color = [](GLenum internal, GLenum layout, GLenum type) {
        return TextureFormat{internal, layout, type, false};
    };
    static constexpr auto block = [](GLenum internal) {
        return TextureFormat{internal, GL_RGBA, GL_UNSIGNED_BYTE, true};
    };
    static const auto table = [] {
        TextureTable t;
        t[F::eR8Unorm] = color(GL_R8, GL_RED, GL_UNSIGNED_BYTE);
        t[F::eR8Snorm] = color(GL_R8_SNORM, GL_RED, GL_BYTE);
        t[F::eR8Uint] = color(GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE);
        t[F::eR8Sint] = color(GL_R8I, GL_RED_INTEGER, GL_BYTE);
        t[F::eR8G8Unorm] = color(GL_RG8, GL_RG, GL_UNSIGNED_BYTE);
        t[F::eR8G8Snorm] = color(GL_RG8_SNORM, GL_RG, GL_BYTE);
        t[F::eR8G8Uint] = color(GL_RG8UI, GL_RG_INTEGER, GL_UNSIGNED_BYTE);
        t[F::eR8G8Sint] = color(GL_RG8I, GL_RG_INTEGER, GL_BYTE);
        t[F::eR8G8B8A8Unorm] = color(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
        t[F::eR8G8B8A8Srgb] = color(GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE);
        t[F::eR8G8B8A8Snorm] = color(GL_RGBA8_SNORM, GL_RGBA, GL_BYTE);
        t[F::eR8G8B8A8Uint] = color(GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE);
        t[F::eR8G8B8A8Sint] = color(GL_RGBA8I, GL_RGBA_INTEGER, GL_BYTE);
        t[F::eB8G8R8A8Unorm] = color(GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE);
        t[F::eB8G8R8A8Srgb] = color(GL_SRGB8_ALPHA8, GL_BGRA, GL_UNSIGNED_BYTE);
        t[F::eR16Unorm] = color(GL_R16, GL_RED, GL_UNSIGNED_SHORT);
        t[F::eR16Snorm] = color(GL_R16_SNORM, GL_RED, GL_SHORT);
        t[F::eR16Uint] = color(GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT);
        t[F::eR16Sint] = color(GL_R16I, GL_RED_INTEGER, GL_SHORT);
        t[F::eR16Sfloat] = color(GL_R16F, GL_RED, GL_HALF_FLOAT);
        t[F::eR16G16Unorm] = color(GL_RG16, GL_RG, GL_UNSIGNED_SHORT);
        t[F::eR16G16Snorm] = color(GL_RG16_SNORM, GL_RG, GL_SHORT);
        t[F::eR16G16Uint] = color(GL_RG16UI, GL_RG_INTEGER, GL_UNSIGNED_SHORT);
        t[F::eR16G16Sint] = color(GL_RG16I, GL_RG_INTEGER, GL_SHORT);
        t[F::eR16G16Sfloat] = color(GL_RG16F, GL_RG, GL_HALF_FLOAT);
        t[F::eR16G16B16A16Unorm] = color(GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT);
        t[F::eR16G16B16A16Snorm] = color(GL_RGBA16_SNORM, GL_RGBA, GL_SHORT);
        t[F::eR16G16B16A16Uint] = color(GL_RGBA16UI, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT);
        t[F::eR16G16B16A16Sint] = color(GL_RGBA16I, GL_RGBA_INTEGER, GL_SHORT);
        t[F::eR16G16B16A16Sfloat] = color(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT);
        t[F::eR32Uint] = color(GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT);
        t[F::eR32Sint] = color(GL_R32I, GL_RED_INTEGER, GL_INT);
        t[F::eR32Sfloat] = color(GL_R32F, GL_RED, GL_FLOAT);
        t[F::eR32G32Uint] = color(GL_RG32UI, GL_RG_INTEGER, GL_UNSIGNED_INT);
        t[F::eR32G32Sint] = color(GL_RG32I, GL_RG_INTEGER, GL_INT);
        t[F::eR32G32Sfloat] = color(GL_RG32F, GL_RG, GL_FLOAT);
        t[F::eR32G32B32A32Uint] = color(GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT);
        t[F::eR32G32B32A32Sint] = color(GL_RGBA32I, GL_RGBA_INTEGER, GL_INT);
        t[F::eR32G32B32A32Sfloat] = color(GL_RGBA32F, GL_RGBA, GL_FLOAT);
        t[F::eA2B10G10R10UnormPack32] =
            color(GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV);
        t[F::eA2B10G10R10UintPack32] =
            color(GL_RGB10_A2UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT_2_10_10_10_REV);
        t[F::eA2R10G10B10UnormPack32] =
            color(GL_RGB10_A2, GL_BGRA, GL_UNSIGNED_INT_2_10_10_10_REV);
        t[F::eB10G11R11UfloatPack32] =
            color(GL_R11F_G11F_B10F, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV);
        t[F::eE5B9G9R9UfloatPack32] = color(GL_RGB9_E5, GL_RGB, GL_UNSIGNED_INT_5_9_9_9_REV);
        t[F::eR5G6B5UnormPack16] = color(GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5);
        t[F::eB5G6R5UnormPack16] = color(GL_RGB565, GL_BGR, GL_UNSIGNED_SHORT_5_6_5);
        t[F::eR4G4B4A4UnormPack16] = color(GL_RGBA4, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4);
        t[F::eB4G4R4A4UnormPack16] = color(GL_RGBA4, GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4);
        t[F::eR5G5B5A1UnormPack16] = color(GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1);
        t[F::eA1R5G5B5UnormPack16] = color(GL_RGB5_A1, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV);
        t[F::eBc1RgbaUnormBlock] = block(GL_COMPRESSED_RGBA_S3TC_DXT1_EXT);
        t[F::eBc1RgbaSrgbBlock] = block(GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT);
        t[F::eBc2UnormBlock] = block(GL_COMPRESSED_RGBA_S3TC_DXT3_EXT);
        t[F::eBc2SrgbBlock] = block(GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT);
        t[F::eBc3UnormBlock] = block(GL_COMPRESSED_RGBA_S3TC_DXT5_EXT);
        t[F::eBc3SrgbBlock] = block(GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT);
        t[F::eBc4UnormBlock] = block(GL_COMPRESSED_RED_RGTC1);
        t[F::eBc4SnormBlock] = block(GL_COMPRESSED_SIGNED_RED_RGTC1);
        t[F::eBc5UnormBlock] = block(GL_COMPRESSED_RG_RGTC2);
        t[F::eBc5SnormBlock] = block(GL_COMPRESSED_SIGNED_RG_RGTC2);
        t[F::eBc6HUfloatBlock] = block(GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT);
        t[F::eBc6HSfloatBlock] = block(GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT);
        t[F::eBc7UnormBlock] = block(GL_COMPRESSED_RGBA_BPTC_UNORM);
        t[F::eBc7SrgbBlock] = block(GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM);
        t[F::eD16Unorm] = color(GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT);
        t[F::eD32Sfloat] = color(GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT);
        t[F::eD16UnormS8Uint] =
            color(GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8);
        t[F::eD32SfloatS8Uint] =
            color(GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV);
        return t;
    }();
    return table;
}

const TextureTable& GlesTextures() {
    using F = vk::Format;
    using C = TexelConversion;
    static const auto table = [] {
        TextureTable t = DesktopTextures();
        const auto convert = [&t](F format, GLenum layout, GLenum type, C conversion) {
            t[format].format = layout;
            t[format].type = type;
            t[format].conversion = conversion;
        };
        convert(F::eB8G8R8A8Unorm, GL_RGBA, GL_UNSIGNED_BYTE, C::SwapRedBlue8888);
        convert(F::eB8G8R8A8Srgb, GL_RGBA, GL_UNSIGNED_BYTE, C::SwapRedBlue8888);
        convert(F::eA2R10G10B10UnormPack32, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, C::SwapRedBlue2101010);
        convert(F::eB5G6R5UnormPack16, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, C::B5G6R5);
        convert(F::eB4G4R4A4UnormPack16, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, C::B4G4R4A4);
        convert(F::eA1R5G5B5UnormPack16, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, C::A1R5G5B5);
        for (const F format : {F::eBc1RgbaSrgbBlock, F::eBc2SrgbBlock, F::eBc3SrgbBlock}) {
            t[format].need = FormatNeed::S3tcSrgb;
        }
        for (const F format : {F::eBc4UnormBlock, F::eBc4SnormBlock, F::eBc5UnormBlock, F::eBc5SnormBlock}) {
            t[format].need = FormatNeed::Rgtc;
        }
        for (const F format :
             {F::eBc6HUfloatBlock, F::eBc6HSfloatBlock, F::eBc7UnormBlock, F::eBc7SrgbBlock}) {
            t[format].need = FormatNeed::Bptc;
        }
        for (const F format :
             {F::eR16Unorm, F::eR16Snorm, F::eR16G16Unorm, F::eR16G16Snorm, F::eR16G16B16A16Unorm,
              F::eR16G16B16A16Snorm}) {
            t[format].need = FormatNeed::Norm16;
        }
        return t;
    }();
    return table;
}

u32 ComponentBytes(GLenum type) {
    switch (type) {
    case GL_BYTE:
    case GL_UNSIGNED_BYTE:
        return 1;
    case GL_SHORT:
    case GL_UNSIGNED_SHORT:
    case GL_HALF_FLOAT:
        return 2;
    default:
        return 4;
    }
}

float HalfToFloat(u16 half) {
    const u32 sign = u32(half & 0x8000u) << 16;
    const u32 exponent = (half >> 10) & 0x1Fu;
    const u32 mantissa = half & 0x3FFu;
    if (exponent == 0) {
        return std::copysign(std::ldexp(float(mantissa), -24), sign != 0 ? -1.0f : 1.0f);
    }
    return std::bit_cast<float>(sign | (exponent == 31 ? 0x7F800000u : (exponent + 112) << 23) |
                                (mantissa << 13));
}

float SignedNormalized(s32 value, u32 bits) {
    return std::max(float(value) / float((1u << (bits - 1)) - 1), -1.0f);
}

u32 FloatBits(float value) {
    return std::bit_cast<u32>(value);
}

template <typename Component>
Component ReadComponent(const u8* data) {
    Component value;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

template <typename Component>
u32 DecodeComponent(const VertexFormat& format, const u8* data) {
    const Component value = ReadComponent<Component>(data);
    if (format.integer) {
        return u32(s64(value));
    }
    if (!format.normalized) {
        return FloatBits(float(value));
    }
    constexpr u32 bits = sizeof(Component) * 8;
    if constexpr (std::is_signed_v<Component>) {
        return FloatBits(SignedNormalized(s32(value), bits));
    } else {
        return FloatBits(float(value) / float(std::numeric_limits<Component>::max()));
    }
}

VertexElement DefaultVertexElement(const VertexFormat& format) {
    return {0, 0, 0, format.integer ? 1u : FloatBits(1.0f)};
}

VertexElement DecodePacked1010102(const VertexFormat& format, const u8* data) {
    const u32 packed = ReadComponent<u32>(data);
    const bool is_signed = format.type == GL_INT_2_10_10_10_REV;
    VertexElement out{};
    for (u32 component = 0; component < 4; ++component) {
        const u32 shift = component * 10;
        const u32 bits = component == 3 ? 2 : 10;
        const u32 raw = (packed >> shift) & ((1u << bits) - 1);
        const s32 extended = s32(raw << (32 - bits)) >> (32 - bits);
        out[component] = is_signed ? FloatBits(SignedNormalized(extended, bits))
                                   : FloatBits(float(raw) / float((1u << bits) - 1));
    }
    return out;
}

template <typename Texel, typename Rearrange>
void RearrangeTexels(u8* texels, size_t bytes, Rearrange rearrange) {
    for (size_t offset = 0; offset + sizeof(Texel) <= bytes; offset += sizeof(Texel)) {
        Texel texel;
        std::memcpy(&texel, texels + offset, sizeof(Texel));
        texel = Texel(rearrange(texel));
        std::memcpy(texels + offset, &texel, sizeof(Texel));
    }
}

} // namespace

const TextureFormat* FindTextureFormat(vk::Format format, bool gles) {
    const auto& table = gles ? GlesTextures() : DesktopTextures();
    const auto it = table.find(format);
    return it == table.end() ? nullptr : &it->second;
}

bool StoresBlueFirst(const TextureFormat& format) {
    return format.format == GL_BGRA || format.conversion == TexelConversion::SwapRedBlue8888 ||
           format.conversion == TexelConversion::SwapRedBlue2101010;
}

bool FormatAvailable(const TextureFormat& format, const Capabilities& capabilities) {
    switch (format.need) {
    case FormatNeed::S3tcSrgb:
        return capabilities.s3tc_srgb;
    case FormatNeed::Rgtc:
        return capabilities.rgtc;
    case FormatNeed::Bptc:
        return capabilities.bptc;
    case FormatNeed::Norm16:
        return capabilities.norm16;
    default:
        return true;
    }
}

void ConvertTexels(TexelConversion conversion, u8* texels, size_t bytes) {
    switch (conversion) {
    case TexelConversion::SwapRedBlue8888:
        RearrangeTexels<u32>(texels, bytes, [](u32 p) {
            return (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
        });
        break;
    case TexelConversion::SwapRedBlue2101010:
        RearrangeTexels<u32>(texels, bytes, [](u32 p) {
            return (p & 0xC00FFC00u) | ((p & 0x3FFu) << 20) | ((p >> 20) & 0x3FFu);
        });
        break;
    case TexelConversion::B5G6R5:
        RearrangeTexels<u16>(texels, bytes, [](u16 p) {
            return (p & 0x07E0u) | ((p & 0x1Fu) << 11) | (p >> 11);
        });
        break;
    case TexelConversion::B4G4R4A4:
        RearrangeTexels<u16>(texels, bytes, [](u16 p) {
            return (p & 0x0F0Fu) | ((p & 0xF0u) << 8) | ((p >> 8) & 0xF0u);
        });
        break;
    case TexelConversion::A1R5G5B5:
        RearrangeTexels<u16>(texels, bytes, [](u16 p) { return ((p & 0x7FFFu) << 1) | (p >> 15); });
        break;
    default:
        break;
    }
}

u32 VertexFormatBytes(const VertexFormat& format) {
    const bool packed = format.type == GL_INT_2_10_10_10_REV || format.type == GL_UNSIGNED_INT_2_10_10_10_REV;
    return packed ? 4 : u32(format.size) * ComponentBytes(format.type);
}

VertexElement ReadVertexElement(const VertexFormat& format, const u8* data, u64 size, u32 stride,
                                u64 element) {
    const u64 bytes = VertexFormatBytes(format);
    if (!data || element > (size / std::max<u32>(stride, 1)) || element * stride + bytes > size) {
        return DefaultVertexElement(format);
    }
    const u8* source = data + element * stride;
    if (format.type == GL_INT_2_10_10_10_REV || format.type == GL_UNSIGNED_INT_2_10_10_10_REV) {
        return DecodePacked1010102(format, source);
    }
    VertexElement out = DefaultVertexElement(format);
    const u32 step = ComponentBytes(format.type);
    for (s32 component = 0; component < format.size; ++component) {
        const u8* at = source + u32(component) * step;
        switch (format.type) {
        case GL_BYTE:
            out[component] = DecodeComponent<s8>(format, at);
            break;
        case GL_UNSIGNED_BYTE:
            out[component] = DecodeComponent<u8>(format, at);
            break;
        case GL_SHORT:
            out[component] = DecodeComponent<s16>(format, at);
            break;
        case GL_UNSIGNED_SHORT:
            out[component] = DecodeComponent<u16>(format, at);
            break;
        case GL_HALF_FLOAT:
            out[component] = FloatBits(HalfToFloat(ReadComponent<u16>(at)));
            break;
        default:
            out[component] = ReadComponent<u32>(at);
            break;
        }
    }
    return out;
}

const VertexFormat* FindVertexFormat(vk::Format format) {
    using F = vk::Format;
    static const auto table = [] {
        std::unordered_map<vk::Format, VertexFormat> t;
        const auto add = [&t](F unorm, F snorm, F uscaled, F sscaled, F uint_format, F sint,
                              GLint size, GLenum utype, GLenum stype) {
            t[unorm] = {size, utype, true, false};
            t[snorm] = {size, stype, true, false};
            t[uscaled] = {size, utype, false, false};
            t[sscaled] = {size, stype, false, false};
            t[uint_format] = {size, utype, false, true};
            t[sint] = {size, stype, false, true};
        };
        add(F::eR8Unorm, F::eR8Snorm, F::eR8Uscaled, F::eR8Sscaled, F::eR8Uint, F::eR8Sint, 1,
            GL_UNSIGNED_BYTE, GL_BYTE);
        add(F::eR8G8Unorm, F::eR8G8Snorm, F::eR8G8Uscaled, F::eR8G8Sscaled, F::eR8G8Uint,
            F::eR8G8Sint, 2, GL_UNSIGNED_BYTE, GL_BYTE);
        add(F::eR8G8B8A8Unorm, F::eR8G8B8A8Snorm, F::eR8G8B8A8Uscaled, F::eR8G8B8A8Sscaled,
            F::eR8G8B8A8Uint, F::eR8G8B8A8Sint, 4, GL_UNSIGNED_BYTE, GL_BYTE);
        add(F::eR16Unorm, F::eR16Snorm, F::eR16Uscaled, F::eR16Sscaled, F::eR16Uint, F::eR16Sint, 1,
            GL_UNSIGNED_SHORT, GL_SHORT);
        add(F::eR16G16Unorm, F::eR16G16Snorm, F::eR16G16Uscaled, F::eR16G16Sscaled, F::eR16G16Uint,
            F::eR16G16Sint, 2, GL_UNSIGNED_SHORT, GL_SHORT);
        add(F::eR16G16B16A16Unorm, F::eR16G16B16A16Snorm, F::eR16G16B16A16Uscaled,
            F::eR16G16B16A16Sscaled, F::eR16G16B16A16Uint, F::eR16G16B16A16Sint, 4,
            GL_UNSIGNED_SHORT, GL_SHORT);
        t[F::eR16Sfloat] = {1, GL_HALF_FLOAT, false, false};
        t[F::eR16G16Sfloat] = {2, GL_HALF_FLOAT, false, false};
        t[F::eR16G16B16A16Sfloat] = {4, GL_HALF_FLOAT, false, false};
        t[F::eR32Sfloat] = {1, GL_FLOAT, false, false};
        t[F::eR32G32Sfloat] = {2, GL_FLOAT, false, false};
        t[F::eR32G32B32Sfloat] = {3, GL_FLOAT, false, false};
        t[F::eR32G32B32A32Sfloat] = {4, GL_FLOAT, false, false};
        t[F::eR32Uint] = {1, GL_UNSIGNED_INT, false, true};
        t[F::eR32G32Uint] = {2, GL_UNSIGNED_INT, false, true};
        t[F::eR32G32B32Uint] = {3, GL_UNSIGNED_INT, false, true};
        t[F::eR32G32B32A32Uint] = {4, GL_UNSIGNED_INT, false, true};
        t[F::eR32Sint] = {1, GL_INT, false, true};
        t[F::eR32G32Sint] = {2, GL_INT, false, true};
        t[F::eR32G32B32Sint] = {3, GL_INT, false, true};
        t[F::eR32G32B32A32Sint] = {4, GL_INT, false, true};
        t[F::eA2B10G10R10UnormPack32] = {4, GL_UNSIGNED_INT_2_10_10_10_REV, true, false};
        t[F::eA2B10G10R10SnormPack32] = {4, GL_INT_2_10_10_10_REV, true, false};
        return t;
    }();
    const auto it = table.find(format);
    return it == table.end() ? nullptr : &it->second;
}

GLenum CompareFunc(vk::CompareOp op) {
    switch (op) {
    case vk::CompareOp::eNever:
        return GL_NEVER;
    case vk::CompareOp::eLess:
        return GL_LESS;
    case vk::CompareOp::eEqual:
        return GL_EQUAL;
    case vk::CompareOp::eLessOrEqual:
        return GL_LEQUAL;
    case vk::CompareOp::eGreater:
        return GL_GREATER;
    case vk::CompareOp::eNotEqual:
        return GL_NOTEQUAL;
    case vk::CompareOp::eGreaterOrEqual:
        return GL_GEQUAL;
    default:
        return GL_ALWAYS;
    }
}

GLenum StencilOp(vk::StencilOp op) {
    switch (op) {
    case vk::StencilOp::eZero:
        return GL_ZERO;
    case vk::StencilOp::eReplace:
        return GL_REPLACE;
    case vk::StencilOp::eIncrementAndClamp:
        return GL_INCR;
    case vk::StencilOp::eDecrementAndClamp:
        return GL_DECR;
    case vk::StencilOp::eInvert:
        return GL_INVERT;
    case vk::StencilOp::eIncrementAndWrap:
        return GL_INCR_WRAP;
    case vk::StencilOp::eDecrementAndWrap:
        return GL_DECR_WRAP;
    default:
        return GL_KEEP;
    }
}

GLenum WrapMode(vk::SamplerAddressMode mode) {
    switch (mode) {
    case vk::SamplerAddressMode::eRepeat:
        return GL_REPEAT;
    case vk::SamplerAddressMode::eMirroredRepeat:
    case vk::SamplerAddressMode::eMirrorClampToEdge:
        return GL_MIRRORED_REPEAT;
    case vk::SamplerAddressMode::eClampToBorder:
        return GL_CLAMP_TO_BORDER;
    default:
        return GL_CLAMP_TO_EDGE;
    }
}

GLenum LogicOp(vk::LogicOp op) {
    switch (op) {
    case vk::LogicOp::eClear:
        return GL_CLEAR;
    case vk::LogicOp::eAnd:
        return GL_AND;
    case vk::LogicOp::eAndReverse:
        return GL_AND_REVERSE;
    case vk::LogicOp::eAndInverted:
        return GL_AND_INVERTED;
    case vk::LogicOp::eNoOp:
        return GL_NOOP;
    case vk::LogicOp::eXor:
        return GL_XOR;
    case vk::LogicOp::eOr:
        return GL_OR;
    case vk::LogicOp::eNor:
        return GL_NOR;
    case vk::LogicOp::eEquivalent:
        return GL_EQUIV;
    case vk::LogicOp::eInvert:
        return GL_INVERT;
    case vk::LogicOp::eOrReverse:
        return GL_OR_REVERSE;
    case vk::LogicOp::eCopyInverted:
        return GL_COPY_INVERTED;
    case vk::LogicOp::eOrInverted:
        return GL_OR_INVERTED;
    case vk::LogicOp::eNand:
        return GL_NAND;
    case vk::LogicOp::eSet:
        return GL_SET;
    default:
        return GL_COPY;
    }
}

GLenum BlendFactor(vk::BlendFactor factor) {
    switch (factor) {
    case vk::BlendFactor::eZero:
        return GL_ZERO;
    case vk::BlendFactor::eOne:
        return GL_ONE;
    case vk::BlendFactor::eSrcColor:
        return GL_SRC_COLOR;
    case vk::BlendFactor::eOneMinusSrcColor:
        return GL_ONE_MINUS_SRC_COLOR;
    case vk::BlendFactor::eDstColor:
        return GL_DST_COLOR;
    case vk::BlendFactor::eOneMinusDstColor:
        return GL_ONE_MINUS_DST_COLOR;
    case vk::BlendFactor::eSrcAlpha:
        return GL_SRC_ALPHA;
    case vk::BlendFactor::eOneMinusSrcAlpha:
        return GL_ONE_MINUS_SRC_ALPHA;
    case vk::BlendFactor::eDstAlpha:
        return GL_DST_ALPHA;
    case vk::BlendFactor::eOneMinusDstAlpha:
        return GL_ONE_MINUS_DST_ALPHA;
    case vk::BlendFactor::eConstantColor:
        return GL_CONSTANT_COLOR;
    case vk::BlendFactor::eOneMinusConstantColor:
        return GL_ONE_MINUS_CONSTANT_COLOR;
    case vk::BlendFactor::eConstantAlpha:
        return GL_CONSTANT_ALPHA;
    case vk::BlendFactor::eOneMinusConstantAlpha:
        return GL_ONE_MINUS_CONSTANT_ALPHA;
    case vk::BlendFactor::eSrcAlphaSaturate:
        return GL_SRC_ALPHA_SATURATE;
    case vk::BlendFactor::eSrc1Color:
        return GL_SRC1_COLOR;
    case vk::BlendFactor::eOneMinusSrc1Color:
        return GL_ONE_MINUS_SRC1_COLOR;
    case vk::BlendFactor::eSrc1Alpha:
        return GL_SRC1_ALPHA;
    case vk::BlendFactor::eOneMinusSrc1Alpha:
        return GL_ONE_MINUS_SRC1_ALPHA;
    default:
        return GL_ONE;
    }
}

GLenum BlendEquation(vk::BlendOp op) {
    switch (op) {
    case vk::BlendOp::eSubtract:
        return GL_FUNC_SUBTRACT;
    case vk::BlendOp::eReverseSubtract:
        return GL_FUNC_REVERSE_SUBTRACT;
    case vk::BlendOp::eMin:
        return GL_MIN;
    case vk::BlendOp::eMax:
        return GL_MAX;
    default:
        return GL_FUNC_ADD;
    }
}

GLint Swizzle(vk::ComponentSwizzle swizzle, GLint identity) {
    switch (swizzle) {
    case vk::ComponentSwizzle::eZero:
        return GL_ZERO;
    case vk::ComponentSwizzle::eOne:
        return GL_ONE;
    case vk::ComponentSwizzle::eR:
        return GL_RED;
    case vk::ComponentSwizzle::eG:
        return GL_GREEN;
    case vk::ComponentSwizzle::eB:
        return GL_BLUE;
    case vk::ComponentSwizzle::eA:
        return GL_ALPHA;
    default:
        return identity;
    }
}

} // namespace OpenGL
