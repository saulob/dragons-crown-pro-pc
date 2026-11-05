// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include <xxhash.h>

#include "common/alignment.h"
#include "core/memory.h"
#include "dcp_settings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_opengl/gl_capabilities.h"
#include "video_core/renderer_opengl/gl_rasterizer.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_blend_rewrite.h"
#include "video_core/renderer_vulkan/vk_depth_stencil_state.h"

namespace OpenGL {
namespace {

constexpr u32 RingBytes = 64 * 1024 * 1024;
constexpr u32 GdsBytes = 64 * 1024;
constexpr u32 DwordAlignment = 4;
constexpr u32 VertexAlignment = 16;
constexpr u32 MaxVertexAttributes = 16;
constexpr u32 ConstantAttributeDivisor = 0x7FFFFFFF;
/// Dragon's buffer clear: one value into dim_x * 64 dwords. There is no compute stage to run it on.
constexpr u64 BufferFillShader = 0x8d304694;

GLenum PrimitiveMode(AmdGpu::PrimitiveType type) {
    switch (type) {
    case AmdGpu::PrimitiveType::PointList:
        return GL_POINTS;
    case AmdGpu::PrimitiveType::LineList:
        return GL_LINES;
    case AmdGpu::PrimitiveType::LineStrip:
        return GL_LINE_STRIP;
    case AmdGpu::PrimitiveType::TriangleList:
    case AmdGpu::PrimitiveType::RectList:
        return GL_TRIANGLES;
    case AmdGpu::PrimitiveType::TriangleFan:
    case AmdGpu::PrimitiveType::Polygon:
        return GL_TRIANGLE_FAN;
    case AmdGpu::PrimitiveType::TriangleStrip:
    case AmdGpu::PrimitiveType::QuadStrip:
        return GL_TRIANGLE_STRIP;
    case AmdGpu::PrimitiveType::AdjLineList:
    case AmdGpu::PrimitiveType::QuadList:
        return GL_LINES_ADJACENCY;
    case AmdGpu::PrimitiveType::AdjLineStrip:
        return GL_LINE_STRIP_ADJACENCY;
    case AmdGpu::PrimitiveType::AdjTriangleList:
        return GL_TRIANGLES_ADJACENCY;
    case AmdGpu::PrimitiveType::AdjTriangleStrip:
        return GL_TRIANGLE_STRIP_ADJACENCY;
    default:
        return 0;
    }
}

void ClearColorAttachment(const AmdGpu::ColorBuffer& buffer, u32 index) {
    gl::ColorMaski(index, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    const auto clear = Vulkan::LiverpoolToVK::ColorBufferClearValue(buffer);
    const auto number_class = AmdGpu::GetNumberClass(buffer.GetNumberFmt());
    if (number_class == AmdGpu::NumberClass::Uint) {
        gl::ClearBufferuiv(GL_COLOR, GLint(index), clear.color.uint32.data());
    } else if (number_class == AmdGpu::NumberClass::Sint) {
        gl::ClearBufferiv(GL_COLOR, GLint(index), clear.color.int32.data());
    } else {
        gl::ClearBufferfv(GL_COLOR, GLint(index), clear.color.float32.data());
    }
}

std::pair<u32, u32> DrawOffsets(const AmdGpu::Regs& regs, const Shader::Info& vs,
                                const std::optional<Shader::Gcn::FetchShaderData>& fetch) {
    u32 vertex_offset = regs.index_offset;
    u32 instance_offset = 0;
    if (fetch) {
        if (vertex_offset == 0 && fetch->vertex_offset_sgpr != -1) {
            vertex_offset = vs.UserData()[fetch->vertex_offset_sgpr];
        }
        if (fetch->instance_offset_sgpr != -1) {
            instance_offset = vs.UserData()[fetch->instance_offset_sgpr];
        }
    }
    return {vertex_offset, instance_offset};
}

} // namespace

StreamRing::StreamRing() {
    GLint max_texels = 0;
    gl::GetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &max_texels);
    capacity = u32(std::min<u64>(RingBytes, u64(max_texels) * 4));
    gl::GenBuffers(1, &buffer);
    gl::BindBuffer(GL_ARRAY_BUFFER, buffer);
    gl::BufferData(GL_ARRAY_BUFFER, capacity, nullptr, GL_STREAM_DRAW);
    gl::GenTextures(1, &texture);
    gl::BindTexture(GL_TEXTURE_BUFFER, texture);
    gl::TexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, buffer);
}

StreamRing::~StreamRing() {
    gl::DeleteTextures(1, &texture);
    gl::DeleteBuffers(1, &buffer);
}

void StreamRing::Wrap() {
    gl::BindBuffer(GL_ARRAY_BUFFER, buffer);
    gl::BufferData(GL_ARRAY_BUFFER, capacity, nullptr, GL_STREAM_DRAW);
    head = 0;
    pending_start = 0;
    guest_copies.clear();
}

void StreamRing::BeginDraw() {
    if (head > capacity - capacity / 3) {
        Wrap();
    }
}

u32 StreamRing::Push(const void* data, u32 size, u32 alignment) {
    const u32 offset = Common::AlignUp(head, alignment);
    if (u64(offset) + size > capacity) {
        return NoOffset;
    }
    pending.resize(offset + size - pending_start);
    std::memcpy(pending.data() + (offset - pending_start), data, size);
    head = offset + size;
    return offset;
}

u32 StreamRing::PushGuest(VAddr address, u32 size, u32 alignment) {
    // By content: the guest and the command stream can rewrite a buffer between two draws.
    const auto* data = reinterpret_cast<const u8*>(address);
    const u64 key = XXH3_64bits_withSeed(&size, sizeof(size), address ^ HashGuestRange(data, size));
    if (const auto it = guest_copies.find(key); it != guest_copies.end()) {
        return it->second;
    }
    const u32 offset = Push(data, size, alignment);
    if (offset != NoOffset) {
        guest_copies.emplace(key, offset);
    }
    return offset;
}

void StreamRing::Commit() {
    if (pending.empty()) {
        return;
    }
    gl::BindBuffer(GL_ARRAY_BUFFER, buffer);
    void* mapped = gl::MapBufferRange(GL_ARRAY_BUFFER, pending_start, GLsizeiptr(pending.size()),
                                      GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT |
                                          GL_MAP_INVALIDATE_RANGE_BIT);
    if (mapped) {
        std::memcpy(mapped, pending.data(), pending.size());
        gl::UnmapBuffer(GL_ARRAY_BUFFER);
    }
    pending_start = head;
    pending.clear();
}

void StreamRing::ForgetGuestCopies() {
    guest_copies.clear();
}

Rasterizer::Rasterizer(AmdGpu::Liverpool* liverpool_, const Capabilities& capabilities)
    : liverpool{liverpool_}, memory{Core::Memory::Instance()},
      separate_blend{capabilities.draw_buffers_blend}, anisotropy{capabilities.anisotropy},
      max_viewport_width{[] {
          GLint dims[2]{};
          gl::GetIntegerv(GL_MAX_VIEWPORT_DIMS, dims);
          return u32(std::min(dims[0], 16384));
      }()},
      max_viewport_height{[] {
          GLint dims[2]{};
          gl::GetIntegerv(GL_MAX_VIEWPORT_DIMS, dims);
          return u32(std::min(dims[1], 16384));
      }()},
      shaders{max_viewport_width, max_viewport_height}, gds(GdsBytes) {
    if (anisotropy) {
        gl::GetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &max_anisotropy);
    }
    gl::GenVertexArrays(1, &vertex_array);
    gl::GenFramebuffers(2, resolve_framebuffers);
    gl::GenTextures(1, &black_texture);
    gl::BindTexture(GL_TEXTURE_2D, black_texture);
    constexpr u32 opaque_black = 0xFF000000;
    gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &opaque_black);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    texture_cache.SetDestroyCallback([this] {
        for (const auto& [key, framebuffer] : framebuffers) {
            gl::DeleteFramebuffers(1, &framebuffer);
        }
        framebuffers.clear();
    });
    gl::Enable(GL_FRAMEBUFFER_SRGB);
    gl::Enable(GL_PROGRAM_POINT_SIZE);
    liverpool->BindRasterizer(this);
    memory->SetRasterizer(this);
}

Rasterizer::~Rasterizer() {
    for (const auto& [key, framebuffer] : framebuffers) {
        gl::DeleteFramebuffers(1, &framebuffer);
    }
    for (const auto& [key, sampler] : samplers) {
        gl::DeleteSamplers(1, &sampler);
    }
    gl::DeleteFramebuffers(2, resolve_framebuffers);
    gl::DeleteTextures(1, &black_texture);
    gl::DeleteVertexArrays(1, &vertex_array);
}

void Rasterizer::Report(const std::string& what) {
    if (reported.insert(what).second) {
        std::printf("OpenGL: %s\n", what.c_str());
    }
}

bool Rasterizer::FilterDraw() {
    const auto& regs = liverpool->regs;
    using Mode = AmdGpu::ColorControl::OperationMode;
    const auto mode = regs.color_control.mode;
    if (mode == Mode::EliminateFastClear) {
        EliminateFastClear();
        return false;
    }
    if (mode == Mode::Resolve) {
        Resolve();
        return false;
    }
    if (mode == Mode::FmaskDecompress || regs.primitive_type == AmdGpu::PrimitiveType::None) {
        return false;
    }
    const bool depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const bool stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    if (mode == Mode::Disable && (depth_copy || stencil_copy)) {
        Report("depth-stencil copy passes are not implemented");
        return false;
    }
    return true;
}

void Rasterizer::EliminateFastClear() {
    const auto& buffer = liverpool->regs.color_buffers[0];
    if (!buffer || !buffer.info.fast_clear) {
        return;
    }
    Image* image = texture_cache.FindColorTarget(buffer, liverpool->last_cb_extent[0]);
    if (!image || !texture_cache.IsMetaCleared(buffer.CmaskAddress())) {
        return;
    }
    texture_cache.TouchMeta(buffer.CmaskAddress());
    AttachedTargets targets{};
    targets.color[0] = image;
    const GLuint framebuffer = FramebufferFor(targets);
    gl::BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl::Disable(GL_SCISSOR_TEST);
    gl::Disable(GL_RASTERIZER_DISCARD);
    ClearColorAttachment(buffer, 0);
}

void Rasterizer::Resolve() {
    const auto& regs = liverpool->regs;
    Image* source = texture_cache.FindColorTarget(regs.color_buffers[0], liverpool->last_cb_extent[0]);
    Image* target = texture_cache.FindColorTarget(regs.color_buffers[1], liverpool->last_cb_extent[1]);
    if (!source || !target) {
        return;
    }
    gl::BindFramebuffer(GL_READ_FRAMEBUFFER, resolve_framebuffers[0]);
    gl::FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                             source->handle, 0);
    gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_framebuffers[1]);
    gl::FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                             target->handle, 0);
    gl::Disable(GL_SCISSOR_TEST);
    gl::Disable(GL_RASTERIZER_DISCARD);
    gl::ColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    const GLint width = GLint(std::min(source->info.size.width, target->info.size.width));
    const GLint height = GLint(std::min(source->info.size.height, target->info.size.height));
    gl::BlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

GLuint Rasterizer::FramebufferFor(const AttachedTargets& targets) {
    u64 key = targets.depth ? targets.depth->handle : 0;
    for (const Image* image : targets.color) {
        key = key * 0x100000001B3ull ^ (image ? image->handle : 0);
    }
    if (const auto it = framebuffers.find(key); it != framebuffers.end()) {
        return it->second;
    }
    GLuint framebuffer = 0;
    gl::GenFramebuffers(1, &framebuffer);
    gl::BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    std::array<GLenum, AmdGpu::NUM_COLOR_BUFFERS> draw_buffers{};
    u32 count = 0;
    for (u32 i = 0; i < targets.color.size(); ++i) {
        if (const Image* image = targets.color[i]) {
            gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D,
                                     image->handle, 0);
            draw_buffers[i] = GL_COLOR_ATTACHMENT0 + i;
            count = i + 1;
        } else {
            draw_buffers[i] = GL_NONE;
        }
    }
    gl::DrawBuffers(GLsizei(count), draw_buffers.data());
    if (count == 0) {
        gl::ReadBuffer(GL_NONE);
    }
    if (const Image* depth = targets.depth) {
        const GLenum attachment = depth->info.props.has_stencil ? GL_DEPTH_STENCIL_ATTACHMENT
                                                                : GL_DEPTH_ATTACHMENT;
        gl::FramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, depth->handle, 0);
    }
    if (gl::CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        Report("a framebuffer combination is incomplete; its draws may be lost");
    }
    framebuffers.emplace(key, framebuffer);
    return framebuffer;
}

bool Rasterizer::BindTargets(const GraphicsProgram& program, AttachedTargets& targets) {
    const auto& regs = liverpool->regs;
    const bool skip_color =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    const u32 mrt_mask = program.fragment ? program.fragment->info->mrt_mask : 0;
    for (u32 cb = 0; cb < u32(std::bit_width(mrt_mask)) && !skip_color; ++cb) {
        const auto& buffer = regs.color_buffers[cb];
        if (!buffer || !regs.color_target_mask.GetMask(cb) || !(mrt_mask & (1u << cb))) {
            continue;
        }
        if (buffer.NumSamples() > 1) {
            Report("multisampled targets render single sampled");
        }
        if (buffer.info.fast_clear) {
            texture_cache.RegisterMeta(buffer.CmaskAddress());
        }
        if (buffer.BaseSlice() != 0) {
            Report("a target slice other than the first renders to the first");
        }
        targets.color[cb] = texture_cache.FindColorTarget(buffer, liverpool->last_cb_extent[cb]);
    }
    if (Vulkan::GetEffectiveDepthStencilState(regs).needs_attachment) {
        const VAddr htile = regs.depth_htile_data_base.GetAddress();
        if (regs.depth_buffer.z_info.tile_surface_enable) {
            texture_cache.RegisterMeta(htile);
        }
        targets.depth = texture_cache.FindDepthTarget(regs.depth_buffer,
                                                      regs.depth_view.NumSlices(), htile,
                                                      liverpool->last_db_extent);
    }
    const bool any_color = std::ranges::any_of(targets.color, [](auto* image) { return image; });
    if (!any_color && !targets.depth) {
        return false;
    }
    gl::BindFramebuffer(GL_FRAMEBUFFER, FramebufferFor(targets));
    return true;
}

// Vulkan clears through the render pass load operation; here it runs before the draw that asked.
void Rasterizer::ClearTargets(const AttachedTargets& targets) {
    const auto& regs = liverpool->regs;
    gl::Disable(GL_SCISSOR_TEST);
    gl::Disable(GL_RASTERIZER_DISCARD);
    for (u32 cb = 0; cb < targets.color.size(); ++cb) {
        if (!targets.color[cb]) {
            continue;
        }
        const auto& buffer = regs.color_buffers[cb];
        if (!texture_cache.IsMetaCleared(buffer.CmaskAddress())) {
            continue;
        }
        texture_cache.TouchMeta(buffer.CmaskAddress());
        ClearColorAttachment(buffer, cb);
    }
    if (!targets.depth) {
        return;
    }
    const VAddr htile = regs.depth_htile_data_base.GetAddress();
    const bool depth_clear =
        regs.depth_buffer.DepthValid() &&
        ((regs.depth_render_control.depth_clear_enable && regs.depth_control.depth_enable &&
          regs.depth_control.depth_write_enable) ||
         texture_cache.IsMetaCleared(htile));
    const bool stencil_clear =
        regs.depth_buffer.StencilValid() && regs.depth_render_control.stencil_clear_enable;
    texture_cache.TouchMeta(htile);
    if (depth_clear) {
        gl::DepthMask(GL_TRUE);
        const float value = regs.depth_clear;
        gl::ClearBufferfv(GL_DEPTH, 0, &value);
    }
    if (stencil_clear) {
        gl::StencilMaskSeparate(GL_FRONT_AND_BACK, 0xFF);
        const GLint value = GLint(regs.stencil_clear);
        gl::ClearBufferiv(GL_STENCIL, 0, &value);
    }
}

GLuint Rasterizer::SamplerFor(AmdGpu::Sampler sharp, bool shadow, bool is_depth) {
    using namespace Vulkan::LiverpoolToVK;
    const int forced = DcpSettings::Get().anisotropy;
    bool force_aniso = false;
    if (forced > 1 && !is_depth && sharp.mip_filter != AmdGpu::MipFilter::None &&
        (sharp.xy_min_filter == AmdGpu::Filter::Bilinear ||
         sharp.xy_min_filter == AmdGpu::Filter::AnisoLinear)) {
        force_aniso = !AmdGpu::IsAnisoFilter(sharp.xy_min_filter) || sharp.MaxAniso() < float(forced);
    }
    const SamplerKey key{sharp.raw0, sharp.raw1 ^ (force_aniso ? u64(forced) << 44 : 0), shadow};
    if (const auto it = samplers.find(key); it != samplers.end()) {
        return it->second;
    }
    GLuint sampler = 0;
    gl::GenSamplers(1, &sampler);
    const bool linear_min = Filter(sharp.xy_min_filter) == vk::Filter::eLinear;
    GLint min_filter = linear_min ? GL_LINEAR : GL_NEAREST;
    if (sharp.mip_filter != AmdGpu::MipFilter::None) {
        const bool linear_mip = MipFilter(sharp.mip_filter) == vk::SamplerMipmapMode::eLinear;
        min_filter = linear_min ? (linear_mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST)
                                : (linear_mip ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
    }
    gl::SamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, min_filter);
    gl::SamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER,
                          Filter(sharp.xy_mag_filter) == vk::Filter::eLinear ? GL_LINEAR
                                                                             : GL_NEAREST);
    gl::SamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GLint(WrapMode(ClampMode(sharp.clamp_x))));
    gl::SamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GLint(WrapMode(ClampMode(sharp.clamp_y))));
    gl::SamplerParameteri(sampler, GL_TEXTURE_WRAP_R, GLint(WrapMode(ClampMode(sharp.clamp_z))));
    gl::SamplerParameterf(sampler, GL_TEXTURE_LOD_BIAS, sharp.LodBias());
    gl::SamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, sharp.MinLod());
    gl::SamplerParameterf(sampler, GL_TEXTURE_MAX_LOD, sharp.MaxLod());
    if (anisotropy) {
        const bool aniso_filter = force_aniso || AmdGpu::IsAnisoFilter(sharp.xy_mag_filter) ||
                                  AmdGpu::IsAnisoFilter(sharp.xy_min_filter);
        const float wanted = force_aniso ? float(forced) : aniso_filter ? sharp.MaxAniso() : 1.0f;
        gl::SamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                              std::clamp(wanted, 1.0f, max_anisotropy));
    }
    if (shadow) {
        gl::SamplerParameteri(sampler, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        gl::SamplerParameteri(sampler, GL_TEXTURE_COMPARE_FUNC,
                              GLint(CompareFunc(DepthCompare(sharp.depth_compare_func))));
    }
    const auto border = BorderColor(sharp.border_color_type);
    if (border == vk::BorderColor::eFloatOpaqueWhite) {
        const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        gl::SamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR, white);
    } else if (border == vk::BorderColor::eFloatOpaqueBlack) {
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        gl::SamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR, black);
    } else if (border == vk::BorderColor::eFloatCustomEXT) {
        const auto base = liverpool->regs.ta_bc_base;
        const auto* colors = base.Address<std::array<float, 4>*>();
        gl::SamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR,
                               colors[sharp.border_color_ptr.Value()].data());
    }
    samplers.emplace(key, sampler);
    return sampler;
}

void Rasterizer::BindTexture(const Shader::Info& info,
                             const Shader::Backend::GLSL::CombinedSampler& combined, GLuint unit) {
    const auto& desc = info.images[combined.image];
    const AmdGpu::Image sharp = desc.GetSharp(info);
    const auto data_format = sharp.GetDataFmt();
    const auto number_format = sharp.GetNumberFmt();
    gl::ActiveTexture(GL_TEXTURE0 + unit);
    Image* image = nullptr;
    if (sharp.Address() != 0 && memory->IsValidGpuMapping(sharp.Address(), 0) &&
        data_format != AmdGpu::DataFormat::FormatInvalid &&
        Vulkan::LiverpoolToVK::TrySurfaceFormat(data_format, number_format) !=
            vk::Format::eUndefined) {
        image = texture_cache.FindTexture(sharp, desc);
    }
    if (!image) {
        const GLenum target = ImageTarget(sharp.GetViewType(desc.is_array));
        gl::BindTexture(target, target == GL_TEXTURE_2D ? black_texture : 0);
        gl::BindSampler(unit, 0);
        return;
    }
    if (image->is_srgb != (number_format == AmdGpu::NumberFormat::Srgb)) {
        Report("an image is sampled with the other sRGB encoding than it was rendered with");
    }
    if (sharp.base_array != 0 && image->target == GL_TEXTURE_2D) {
        Report("a texture view of a later array slice samples the first");
    }
    gl::ActiveTexture(GL_TEXTURE0 + unit);
    gl::BindTexture(image->target, image->handle);
    const auto mapping = Vulkan::LiverpoolToVK::ComponentMapping(sharp.DstSelect());
    const std::array<GLint, 4> swizzle{
        Swizzle(mapping.r, GL_RED),
        Swizzle(mapping.g, GL_GREEN),
        Swizzle(mapping.b, GL_BLUE),
        Swizzle(mapping.a, GL_ALPHA),
    };
    gl::TexParameteriv(image->target, GL_TEXTURE_SWIZZLE_RGBA, swizzle.data());
    const u32 last_level = std::max(image->info.resources.levels, 1u) - 1;
    gl::TexParameteri(image->target, GL_TEXTURE_BASE_LEVEL,
                      GLint(std::min<u32>(sharp.base_level, last_level)));
    gl::TexParameteri(image->target, GL_TEXTURE_MAX_LEVEL,
                      GLint(std::min<u32>(sharp.last_level, last_level)));
    if (combined.sampler == Shader::Backend::GLSL::NoSampler) {
        gl::BindSampler(unit, 0);
        return;
    }
    const auto sampler = info.samplers[combined.sampler].GetSharp(info);
    gl::BindSampler(unit, SamplerFor(sampler, combined.shadow, desc.is_depth));
}

void Rasterizer::BindStage(const Shader::Info& info, const StageLocations& locations,
                           const Shader::Backend::GLSL::Stage& glsl) {
    if (locations.user_data >= 0) {
        std::array<u32, Shader::NUM_USER_DATA_REGS> user_data{};
        const auto registers = info.UserData();
        u32 mask = info.ud_mask.mask;
        for (u32 slot = 0; mask != 0; ++slot) {
            const u32 index = std::countr_zero(mask);
            mask &= mask - 1;
            user_data[slot] = registers[index];
        }
        gl::Uniform4uiv(locations.user_data, 4, user_data.data());
    }
    if (locations.buffers >= 0) {
        std::array<std::array<u32, 2>, Shader::NUM_BUFFERS> table{};
        const auto& regs = liverpool->regs;
        for (u32 i = 0; i < info.buffers.size(); ++i) {
            const auto& desc = info.buffers[i];
            u32 offset = StreamRing::NoOffset;
            u32 size = 0;
            switch (desc.buffer_type) {
            case Shader::BufferType::Guest: {
                const auto sharp = desc.GetSharp(info);
                if (sharp.base_address != 0 && sharp.GetSize() != 0) {
                    size = u32(memory->ClampRangeSize(sharp.base_address, sharp.GetSize()));
                    offset = ring.PushGuest(sharp.base_address, size, DwordAlignment);
                }
                break;
            }
            case Shader::BufferType::Flatbuf: {
                const auto flat = info.FlatUserData();
                size = u32(flat.size_bytes());
                offset = ring.Push(flat.data(), size, DwordAlignment);
                break;
            }
            case Shader::BufferType::ClipPlanes: {
                std::array<float, AmdGpu::NUM_CLIP_PLANES * 4> planes{};
                for (u32 plane = 0; plane < AmdGpu::NUM_CLIP_PLANES; ++plane) {
                    const auto& data = regs.clip_user_data[plane];
                    planes[plane * 4 + 0] = std::bit_cast<float>(data.data_x);
                    planes[plane * 4 + 1] = std::bit_cast<float>(data.data_y);
                    planes[plane * 4 + 2] = std::bit_cast<float>(data.data_z);
                    planes[plane * 4 + 3] = std::bit_cast<float>(data.data_w);
                }
                size = u32(sizeof(planes));
                offset = ring.Push(planes.data(), size, DwordAlignment);
                break;
            }
            default:
                Report(fmt::format("buffer type {} is not bound", u32(desc.buffer_type)));
                break;
            }
            if (offset != StreamRing::NoOffset) {
                table[i] = {offset / DwordAlignment, (size + 3) / DwordAlignment};
            }
        }
        gl::Uniform2uiv(locations.buffers, GLsizei(std::max<size_t>(info.buffers.size(), 1)),
                        table[0].data());
    }
    for (u32 i = 0; i < glsl.samplers.size(); ++i) {
        BindTexture(info, glsl.samplers[i], locations.sampler_units[i]);
    }
}

bool Rasterizer::BindVertexInputs(const GraphicsProgram& program, u32 first_instance) {
    const auto& info = *program.vertex->info;
    const auto& fetch = program.vertex->spec.fetch_shader_data;
    gl::BindBuffer(GL_ARRAY_BUFFER, ring.Buffer());
    u32 wanted = 0;
    const auto& regs = liverpool->regs;
    if (fetch) {
        using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
        for (const auto& attrib : fetch->attributes) {
            if (attrib.semantic >= MaxVertexAttributes) {
                Report("a vertex attribute beyond the sixteen OpenGL guarantees was dropped");
                continue;
            }
            const auto buffer = attrib.GetSharp(info);
            if (buffer.base_address == 0 || buffer.GetSize() == 0) {
                continue;
            }
            const auto vk_format =
                Vulkan::LiverpoolToVK::TrySurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
            const auto* format = FindVertexFormat(vk_format);
            if (!format) {
                Report(fmt::format("unsupported vertex format {}", vk::to_string(vk_format)));
                continue;
            }
            const u32 size = u32(memory->ClampRangeSize(buffer.base_address, buffer.GetSize()));
            const u32 offset = ring.PushGuest(buffer.base_address, size, VertexAlignment);
            if (offset == StreamRing::NoOffset) {
                Report("vertex data larger than the stream ring was dropped");
                return false;
            }
            const u32 stride = buffer.GetStride();
            const auto step_rate = attrib.GetStepRate();
            u32 divisor = 0;
            if (step_rate != InstanceIdType::None) {
                divisor = step_rate == InstanceIdType::OverStepRate0   ? regs.vgt_instance_step_rate_0
                          : step_rate == InstanceIdType::OverStepRate1 ? regs.vgt_instance_step_rate_1
                                                                       : 1;
            }
            const u64 first = step_rate != InstanceIdType::None ? u64(first_instance) * stride : 0;
            const GLuint location = attrib.semantic;
            const auto* pointer = reinterpret_cast<const void*>(uintptr_t(offset + first));
            if (format->integer) {
                gl::VertexAttribIPointer(location, format->size, format->type, GLsizei(stride),
                                         pointer);
            } else {
                gl::VertexAttribPointer(location, format->size, format->type,
                                        format->normalized ? GL_TRUE : GL_FALSE, GLsizei(stride),
                                        pointer);
            }
            gl::VertexAttribDivisor(location, stride == 0 ? ConstantAttributeDivisor : divisor);
            wanted |= 1u << location;
        }
    }
    for (u32 changed = wanted ^ enabled_attributes; changed != 0; changed &= changed - 1) {
        const u32 location = std::countr_zero(changed);
        if (wanted & (1u << location)) {
            gl::EnableVertexAttribArray(location);
        } else {
            gl::DisableVertexAttribArray(location);
        }
    }
    enabled_attributes = wanted;
    return true;
}

void Rasterizer::ApplyViewportScissor(const GraphicsProgram& program) {
    const auto& regs = liverpool->regs;
    const auto& control = regs.viewport_control;
    const bool offset_enabled = !regs.window_scissor.window_offset_disable;
    const s32 window_x = offset_enabled ? regs.window_offset.window_x_offset : 0;
    const s32 window_y = offset_enabled ? regs.window_offset.window_y_offset : 0;
    s32 left = std::max({s32(regs.screen_scissor.top_left_x),
                         s32(regs.window_scissor.top_left_x) + window_x,
                         s32(regs.generic_scissor.top_left_x) + window_x});
    s32 top = std::max({s32(regs.screen_scissor.top_left_y),
                        s32(regs.window_scissor.top_left_y) + window_y,
                        s32(regs.generic_scissor.top_left_y) + window_y});
    s32 right = std::min({s32(regs.screen_scissor.bottom_right_x),
                          s32(regs.window_scissor.bottom_right_x) + window_x,
                          s32(regs.generic_scissor.bottom_right_x) + window_x});
    s32 bottom = std::min({s32(regs.screen_scissor.bottom_right_y),
                           s32(regs.window_scissor.bottom_right_y) + window_y,
                           s32(regs.generic_scissor.bottom_right_y) + window_y});

    float flip_y = 1.0f;
    const AmdGpu::ViewportBounds* viewport = nullptr;
    for (u32 i = 0; i < AmdGpu::NUM_VIEWPORTS; ++i) {
        if (regs.viewports[i].xscale != 0) {
            viewport = &regs.viewports[i];
            if (regs.mode_control.vport_scissor_enable) {
                const auto& scissor = regs.viewport_scissors[i];
                left = std::max(left, s32(scissor.top_left_x));
                top = std::max(top, s32(scissor.top_left_y));
                right = std::min(right, s32(scissor.bottom_right_x));
                bottom = std::min(bottom, s32(scissor.bottom_right_y));
            }
            break;
        }
    }
    const float xoffset = viewport && control.xoffset_enable ? viewport->xoffset : 0.0f;
    const float xscale = viewport && control.xscale_enable ? viewport->xscale : 1.0f;
    const float yoffset = viewport && control.yoffset_enable ? viewport->yoffset : 0.0f;
    const float yscale = viewport && control.yscale_enable ? viewport->yscale : 1.0f;
    if (!viewport) {
        gl::Viewport(0, 0, 1, 1);
        left = top = right = bottom = 0;
    } else if (regs.IsClipDisabled()) {
        gl::Viewport(0, 0, GLsizei(max_viewport_width), GLsizei(max_viewport_height));
    } else {
        // OpenGL needs a positive height: a negative one becomes a flip in the vertex stage.
        const float width = std::abs(2.0f * xscale);
        const float height = std::abs(2.0f * yscale);
        gl::Viewport(GLint(xoffset - std::abs(xscale)), GLint(yoffset - std::abs(yscale)),
                     GLsizei(width), GLsizei(height));
        flip_y = yscale < 0.0f ? -1.0f : 1.0f;
    }
    if (viewport) {
        const float zoffset = control.zoffset_enable ? viewport->zoffset : 0.0f;
        const float zscale = control.zscale_enable ? viewport->zscale : 1.0f;
        const bool symmetric = regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
        const float depth_min = std::clamp(symmetric ? zoffset - zscale : zoffset, 0.0f, 1.0f);
        const float depth_max = std::clamp(zoffset + zscale, 0.0f, 1.0f);
        gl::DepthRange(depth_min, depth_max);
    }
    if (program.flip_y >= 0) {
        gl::Uniform1f(program.flip_y, flip_y);
    }
    if (program.transform >= 0) {
        gl::Uniform4f(program.transform, xoffset, yoffset, xscale, yscale);
    }
    gl::Enable(GL_SCISSOR_TEST);
    gl::Scissor(left, top, std::max(right - left, 0), std::max(bottom - top, 0));
}

void Rasterizer::ApplyDepthStencil() {
    using namespace Vulkan::LiverpoolToVK;
    const auto& regs = liverpool->regs;
    const auto state = Vulkan::GetEffectiveDepthStencilState(regs);
    if (state.depth_test_enable) {
        gl::Enable(GL_DEPTH_TEST);
        gl::DepthFunc(CompareFunc(CompareOp(regs.depth_control.depth_func)));
    } else {
        gl::Disable(GL_DEPTH_TEST);
    }
    gl::DepthMask(state.depth_write_enable && !regs.depth_render_control.depth_clear_enable
                      ? GL_TRUE
                      : GL_FALSE);
    if (state.depth_bounds_enable) {
        Report("the depth bounds test is not available and was ignored");
    }
    if (!state.stencil_test_enable) {
        gl::Disable(GL_STENCIL_TEST);
        return;
    }
    gl::Enable(GL_STENCIL_TEST);
    const auto& control = regs.stencil_control;
    const bool two_sided = regs.depth_control.backface_enable;
    const auto replaces = [](AmdGpu::StencilFunc fail, AmdGpu::StencilFunc pass,
                             AmdGpu::StencilFunc zfail) {
        using enum AmdGpu::StencilFunc;
        return fail == ReplaceOp || pass == ReplaceOp || zfail == ReplaceOp;
    };
    const auto front_func = CompareFunc(CompareOp(regs.depth_control.stencil_ref_func));
    const auto back_func =
        two_sided ? CompareFunc(CompareOp(regs.depth_control.stencil_bf_func)) : front_func;
    const auto& front = regs.stencil_ref_front;
    const auto& back = two_sided ? regs.stencil_ref_back : regs.stencil_ref_front;
    const bool front_replaces = replaces(control.stencil_fail_front, control.stencil_zpass_front,
                                         control.stencil_zfail_front);
    const bool back_replaces = two_sided ? replaces(control.stencil_fail_back,
                                                    control.stencil_zpass_back,
                                                    control.stencil_zfail_back)
                                         : front_replaces;
    gl::StencilFuncSeparate(GL_FRONT, front_func,
                            GLint(front_replaces ? front.stencil_op_val : front.stencil_test_val),
                            front.stencil_mask);
    gl::StencilFuncSeparate(GL_BACK, back_func,
                            GLint(back_replaces ? back.stencil_op_val : back.stencil_test_val),
                            back.stencil_mask);
    const bool clearing = regs.depth_render_control.stencil_clear_enable;
    gl::StencilMaskSeparate(GL_FRONT, clearing ? 0u : front.stencil_write_mask);
    gl::StencilMaskSeparate(GL_BACK, clearing ? 0u : back.stencil_write_mask);
    const auto stencil_op = [](AmdGpu::StencilFunc func) {
        return OpenGL::StencilOp(Vulkan::LiverpoolToVK::StencilOp(func));
    };
    gl::StencilOpSeparate(GL_FRONT, stencil_op(control.stencil_fail_front),
                          stencil_op(control.stencil_zfail_front),
                          stencil_op(control.stencil_zpass_front));
    gl::StencilOpSeparate(GL_BACK,
                          stencil_op(two_sided ? control.stencil_fail_back
                                               : control.stencil_fail_front),
                          stencil_op(two_sided ? control.stencil_zfail_back
                                               : control.stencil_zfail_front),
                          stencil_op(two_sided ? control.stencil_zpass_back
                                               : control.stencil_zpass_front));
}

void Rasterizer::ApplyRasterState() {
    using namespace Vulkan::LiverpoolToVK;
    const auto& regs = liverpool->regs;
    const auto cull = IsPrimitiveCulled(regs.primitive_type) ? regs.polygon_control.CullingMode()
                                                             : AmdGpu::CullMode::None;
    if (cull == AmdGpu::CullMode::None) {
        gl::Disable(GL_CULL_FACE);
    } else {
        gl::Enable(GL_CULL_FACE);
        gl::CullFace(cull == AmdGpu::CullMode::Front  ? GL_FRONT
                     : cull == AmdGpu::CullMode::Back ? GL_BACK
                                                      : GL_FRONT_AND_BACK);
    }
    // Render targets keep Vulkan's window coordinates; its signed area rule is OpenGL's negated.
    gl::FrontFace(regs.polygon_control.front_face == AmdGpu::FrontFace::CounterClockwise ? GL_CW
                                                                                         : GL_CCW);
    const auto mode = regs.polygon_control.PolyMode();
    gl::PolygonMode(GL_FRONT_AND_BACK, mode == AmdGpu::PolygonMode::Point  ? GL_POINT
                                       : mode == AmdGpu::PolygonMode::Line ? GL_LINE
                                                                           : GL_FILL);
    if (regs.polygon_control.NeedsBias()) {
        const bool front = regs.polygon_control.enable_polygon_offset_front;
        gl::Enable(GL_POLYGON_OFFSET_FILL);
        gl::PolygonOffset((front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale) / 16.f,
                          front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset);
    } else {
        gl::Disable(GL_POLYGON_OFFSET_FILL);
    }
    if (regs.clipper_control.ZclipEnable()) {
        gl::Disable(GL_DEPTH_CLAMP);
    } else {
        gl::Enable(GL_DEPTH_CLAMP);
    }
    if (regs.clipper_control.dx_rasterization_kill) {
        gl::Enable(GL_RASTERIZER_DISCARD);
    } else {
        gl::Disable(GL_RASTERIZER_DISCARD);
    }
    gl::LineWidth(std::max(regs.line_control.Width(), 1.0f));
    gl::ProvokingVertex(regs.polygon_control.provoking_vtx_last == AmdGpu::ProvokingVtxLast::Last
                            ? GL_LAST_VERTEX_CONVENTION
                            : GL_FIRST_VERTEX_CONVENTION);
}

void Rasterizer::ApplyBlend(const GraphicsProgram& program, const AttachedTargets& targets) {
    using namespace Vulkan;
    const auto& regs = liverpool->regs;
    gl::BlendColor(regs.blend_constants[0], regs.blend_constants[1], regs.blend_constants[2],
                   regs.blend_constants[3]);
    const auto logic_op = regs.color_control.rop3;
    if (logic_op != AmdGpu::ColorControl::LogicOp::Copy) {
        gl::Enable(GL_COLOR_LOGIC_OP);
        gl::LogicOp(OpenGL::LogicOp(LiverpoolToVK::LogicOp(logic_op)));
    } else {
        gl::Disable(GL_COLOR_LOGIC_OP);
    }
    bool global_set = false;
    const auto apply = [&](u32 cb, GLenum src_color, GLenum dst_color, GLenum eq_color,
                           GLenum src_alpha, GLenum dst_alpha, GLenum eq_alpha) {
        if (separate_blend) {
            gl::BlendFuncSeparatei(cb, src_color, dst_color, src_alpha, dst_alpha);
            gl::BlendEquationSeparatei(cb, eq_color, eq_alpha);
            return;
        }
        if (global_set) {
            Report("different blending per render target needs OpenGL 4.0; the first one is used");
            return;
        }
        global_set = true;
        gl::BlendFuncSeparate(src_color, dst_color, src_alpha, dst_alpha);
        gl::BlendEquationSeparate(eq_color, eq_alpha);
    };
    for (u32 cb = 0; cb < targets.color.size(); ++cb) {
        const auto& buffer = regs.color_buffers[cb];
        if (!targets.color[cb]) {
            gl::Disablei(GL_BLEND, cb);
            continue;
        }
        const u32 mask = buffer.Swizzle().ApplyMask(regs.color_target_mask.GetMask(cb));
        gl::ColorMaski(cb, (mask & 1) != 0, (mask & 2) != 0, (mask & 4) != 0, (mask & 8) != 0);
        const auto& control = regs.blend_control[cb];
        if (!control.enable || buffer.info.blend_bypass) {
            gl::Disablei(GL_BLEND, cb);
            continue;
        }
        gl::Enablei(GL_BLEND, cb);
        Vulkan::BlendEquation color{control.color_src_factor, control.color_func, control.color_dst_factor};
        Vulkan::BlendEquation alpha{
            control.separate_alpha_blend ? control.alpha_src_factor : control.color_src_factor,
            control.separate_alpha_blend ? control.alpha_func : control.color_func,
            control.separate_alpha_blend ? control.alpha_dst_factor : control.color_dst_factor};
        const auto format = buffer.GetNumberFmt();
        const auto color_rewrite = RewriteScaledMinMaxBlend(color, format, BlendChannel::Color);
        const auto alpha_rewrite = RewriteScaledMinMaxBlend(alpha, format, BlendChannel::Alpha);
        if (color_rewrite == BlendRewriteResult::Squared ||
            alpha_rewrite == BlendRewriteResult::Squared ||
            color_rewrite == BlendRewriteResult::Unsupported ||
            alpha_rewrite == BlendRewriteResult::Unsupported) {
            Report("a scaled MIN/MAX blend has no OpenGL equivalent and blends unscaled");
        }
        // Hardware blends a masked alpha export as 1; the shader's output would be 0.
        const bool alpha_masked =
            (regs.color_shader_mask.GetMask(cb) & AmdGpu::ColorBufferMask::ComponentA) == 0;
        const auto color_factor = [&](AmdGpu::BlendControl::BlendFactor factor) {
            const GLenum mapped = OpenGL::BlendFactor(LiverpoolToVK::BlendFactor(factor));
            if (alpha_masked && mapped == GL_SRC_ALPHA) {
                return GLenum(GL_ONE);
            }
            if (alpha_masked && mapped == GL_ONE_MINUS_SRC_ALPHA) {
                return GLenum(GL_ZERO);
            }
            return mapped;
        };
        apply(cb, color_factor(color.src_factor), color_factor(color.dst_factor),
              OpenGL::BlendEquation(LiverpoolToVK::BlendOp(color.function)),
              OpenGL::BlendFactor(LiverpoolToVK::BlendFactor(alpha.src_factor)),
              OpenGL::BlendFactor(LiverpoolToVK::BlendFactor(alpha.dst_factor)),
              OpenGL::BlendEquation(LiverpoolToVK::BlendOp(alpha.function)));
    }
}

void Rasterizer::Draw(bool is_indexed, u32 index_offset, const Vulkan::PreparedDraw*) {
    ++draws;
    if (!FilterDraw()) {
        return;
    }
    const auto& regs = liverpool->regs;
    GraphicsProgram* program = shaders.GetGraphics(regs);
    if (!program) {
        return;
    }
    const GLenum mode = PrimitiveMode(regs.primitive_type);
    if (mode == 0) {
        Report(fmt::format("primitive type {} is not implemented", u32(regs.primitive_type)));
        return;
    }
    AttachedTargets targets{};
    if (!BindTargets(*program, targets)) {
        return;
    }
    gl::BindVertexArray(vertex_array);
    gl::UseProgram(program->handle);
    ring.BeginDraw();

    const auto& vs_info = *program->vertex->info;
    const auto [vertex_offset, instance_offset] =
        DrawOffsets(regs, vs_info, program->vertex->spec.fetch_shader_data);

    gl::ActiveTexture(GL_TEXTURE0);
    gl::BindSampler(0, 0);
    gl::BindTexture(GL_TEXTURE_BUFFER, ring.Texture());
    BindStage(vs_info, program->vs, program->vertex->glsl);
    if (program->fragment) {
        BindStage(*program->fragment->info, program->fs, program->fragment->glsl);
    }
    if (!BindVertexInputs(*program, instance_offset)) {
        return;
    }

    u32 index_ring_offset = 0;
    const bool index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    if (is_indexed) {
        const u32 index_size = index16 ? 2 : 4;
        const VAddr address =
            regs.index_base_address.Address<VAddr>() + VAddr(index_offset) * index_size;
        index_ring_offset = ring.PushGuest(address, regs.num_indices * index_size, index_size);
        if (index_ring_offset == StreamRing::NoOffset) {
            Report("index data larger than the stream ring was dropped");
            return;
        }
    }
    ring.Commit();

    if (program->base_vertex >= 0) {
        gl::Uniform1ui(program->base_vertex, vertex_offset);
    }
    if (program->base_instance >= 0) {
        gl::Uniform1ui(program->base_instance, instance_offset);
    }
    ClearTargets(targets);
    ApplyViewportScissor(*program);
    ApplyDepthStencil();
    ApplyRasterState();
    ApplyBlend(*program, targets);

    const u32 clip_distances = program->vertex->glsl.clip_distances;
    for (u32 i = clip_distances; i < enabled_clip_distances; ++i) {
        gl::Disable(GL_CLIP_DISTANCE0 + i);
    }
    for (u32 i = enabled_clip_distances; i < clip_distances; ++i) {
        gl::Enable(GL_CLIP_DISTANCE0 + i);
    }
    enabled_clip_distances = clip_distances;

    if (regs.enable_primitive_restart & 1) {
        gl::Enable(GL_PRIMITIVE_RESTART);
        gl::PrimitiveRestartIndex(index16 ? 0xFFFFu : 0xFFFFFFFFu);
    } else {
        gl::Disable(GL_PRIMITIVE_RESTART);
    }

    const GLsizei count = GLsizei(regs.num_indices);
    const GLsizei instances = GLsizei(regs.num_instances.NumInstances());
    if (is_indexed) {
        gl::BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ring.Buffer());
        gl::DrawElementsInstancedBaseVertex(
            mode, count, index16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
            reinterpret_cast<const void*>(uintptr_t(index_ring_offset)), instances,
            GLint(vertex_offset));
    } else {
        gl::DrawArraysInstanced(mode, GLint(vertex_offset), count, instances);
    }
    if (++draws_reaching_gl == 1) {
        std::printf("OpenGL: first guest draw submitted (vs %#llx, ps %#llx, %d indices)\n",
                    static_cast<unsigned long long>(vs_info.pgm_hash),
                    static_cast<unsigned long long>(
                        program->fragment ? program->fragment->info->pgm_hash : 0),
                    count);
    }
    // DEBUG_SAULO: temporary, driver errors of the first draws.
    if (draws_reaching_gl <= 256) {
        if (const GLenum error = gl::GetError(); error != GL_NO_ERROR) {
            std::printf("DEBUG_SAULO: OpenGL error %#x at draw %llu\n", error,
                        static_cast<unsigned long long>(draws_reaching_gl));
        }
    }
}

void Rasterizer::DrawIndirect(bool, VAddr, u32, u32, u32, VAddr, u16, u16) {
    Report("indirect draws are not implemented");
}

bool Rasterizer::TryKnownCompute(const Shader::Info& info) {
    bool reads_meta = false;
    const AmdGpu::Buffer* written_guest = nullptr;
    AmdGpu::Buffer written{};
    AmdGpu::Buffer read{};
    bool has_read = false;
    for (const auto& desc : info.buffers) {
        if (desc.IsSpecial()) {
            continue;
        }
        const auto sharp = desc.GetSharp(info);
        if (desc.is_written) {
            written = sharp;
            written_guest = &written;
        } else {
            read = sharp;
            has_read = true;
            reads_meta |= texture_cache.IsMeta(sharp.base_address);
        }
    }
    if (!written_guest) {
        return false;
    }
    if (!reads_meta && !info.has_bitwise_xor && texture_cache.ClearMeta(written.base_address)) {
        return true;
    }
    if (info.pgm_hash == BufferFillShader && has_read && info.images.empty() &&
        AmdGpu::NumBitsPerElement(written.GetDataFmt()) == 32) {
        const auto& cs = liverpool->GetCsRegs();
        const u64 threads = u64(cs.dim_x) * cs.num_thread_x.full;
        const u64 count = std::min<u64>(threads, written.GetSize() / 4);
        auto* out = reinterpret_cast<u32*>(written.base_address);
        std::fill(out, out + count, *reinterpret_cast<const u32*>(read.base_address));
        texture_cache.NoteGpuWrite(written.base_address, count * sizeof(u32));
        return true;
    }
    return false;
}

void Rasterizer::DispatchDirect() {
    const auto& cs = liverpool->GetCsRegs();
    const Shader::Info* info = shaders.GetCompute(cs);
    if (!TryKnownCompute(*info)) {
        Report(fmt::format("compute shader {:#x} was skipped: OpenGL 3.3 has no compute stage",
                           info->pgm_hash));
    }
}

void Rasterizer::DispatchIndirect(VAddr, u32, u32) {
    Report("indirect dispatches are not implemented");
}

void Rasterizer::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    if (is_gds) {
        if (u64(address) + num_bytes <= gds.size()) {
            auto* out = reinterpret_cast<u32*>(gds.data() + address);
            std::fill(out, out + num_bytes / 4, value);
        }
        return;
    }
    texture_cache.ClearMeta(address);
    auto* out = reinterpret_cast<u32*>(address);
    std::fill(out, out + num_bytes / 4, value);
    texture_cache.NoteGpuWrite(address, num_bytes);
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    const auto* source = src_gds ? gds.data() + src : reinterpret_cast<const u8*>(src);
    auto* target = dst_gds ? gds.data() + dst : reinterpret_cast<u8*>(dst);
    std::memmove(target, source, num_bytes);
    if (!dst_gds) {
        texture_cache.NoteGpuWrite(dst, num_bytes);
    }
}

u32 Rasterizer::ReadDataFromGds(u32 gds_offset) {
    u32 value = 0;
    std::memcpy(&value, gds.data() + gds_offset, sizeof(value));
    return value;
}

bool Rasterizer::RunInOrder(OrderedTask task, const void* data, u32, u64) {
    task(*this, static_cast<const u8*>(data));
    return false;
}

void Rasterizer::UnmapMemory(VAddr addr, u64 size) {
    texture_cache.QueueUnmap(addr, size);
}

u64 Rasterizer::Flush() {
    gl::Flush();
    return 0;
}

void Rasterizer::Finish() {
    gl::Finish();
}

void Rasterizer::OnSubmit() {
    ring.ForgetGuestCopies();
}

void Rasterizer::OnFlip() {
    texture_cache.NextFrame();
    ring.ForgetGuestCopies();
    // DEBUG_SAULO: temporary, how much of the guest's work reaches OpenGL.
    if (++flips % 600 == 0) {
        std::printf("DEBUG_SAULO: OpenGL %llu of %llu guest draws submitted after %llu flips\n",
                    static_cast<unsigned long long>(draws_reaching_gl),
                    static_cast<unsigned long long>(draws), static_cast<unsigned long long>(flips));
    }
}

void Rasterizer::ResetState() {
    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl::Disable(GL_SCISSOR_TEST);
    gl::Disable(GL_DEPTH_TEST);
    gl::Disable(GL_STENCIL_TEST);
    gl::Disable(GL_CULL_FACE);
    gl::Disable(GL_POLYGON_OFFSET_FILL);
    gl::Disable(GL_DEPTH_CLAMP);
    gl::Disable(GL_RASTERIZER_DISCARD);
    gl::Disable(GL_PRIMITIVE_RESTART);
    gl::Disable(GL_COLOR_LOGIC_OP);
    gl::Disable(GL_FRAMEBUFFER_SRGB);
    for (u32 i = 0; i < enabled_clip_distances; ++i) {
        gl::Disable(GL_CLIP_DISTANCE0 + i);
    }
    enabled_clip_distances = 0;
    for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        gl::Disablei(GL_BLEND, cb);
        gl::ColorMaski(cb, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }
    gl::PolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    gl::DepthRange(0.0, 1.0);
    gl::DepthMask(GL_TRUE);
}

} // namespace OpenGL
