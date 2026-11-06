// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <xxhash.h>

#include "common/signal_context.h"
#include "core/memory.h"
#include "core/signals.h"
#include "dcp_settings.h"
#include "dcp_toggles.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_opengl/gl_primitives.h"
#include "video_core/renderer_opengl/gl_rasterizer.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_blend_rewrite.h"
#include "video_core/renderer_vulkan/vk_depth_stencil_state.h"

namespace OpenGL {
namespace {

constexpr u32 GdsBytes = 64 * 1024;
constexpr u32 DwordAlignment = 4;
constexpr u32 VertexAlignment = 16;
constexpr u32 MaxVertexAttributes = 16;
constexpr u32 ConstantAttributeDivisor = 0x7FFFFFFF;
/// Dragon's buffer clear: one value into dim_x * 64 dwords. There is no compute stage to run it on.
constexpr u64 BufferFillShader = 0x8d304694;
/// The SRT walker's handler comes after this one (priority 1).
constexpr u32 WatchFaultPriority = 0;

constexpr u32 RectInputElementBytes = sizeof(VertexElement);
constexpr u32 RectConstantDivisor = 0xFFFFFFFF;

GuestWriteWatch* fault_watch;

bool OnGuestFault(void* context, void* address) {
    using FaultResult = GuestWriteWatch::FaultResult;
    const auto fault = reinterpret_cast<VAddr>(address);
    const FaultResult result =
        Common::IsWriteError(context) ? fault_watch->OnWriteFault(fault) : FaultResult::NotOurs;
    if (result == FaultResult::Released) {
        ResetFaultRetries();
        return true;
    }
    // Carving a mapping on Windows unmaps the views around it for a moment: mapped memory that
    // faults is retried, and the lookup waits for the carve (as Vulkan's handler does).
    if (result == FaultResult::NotOurs && Core::Memory::Instance()->ClampRangeSize(fault, 1) == 0) {
        return false;
    }
    return RetryFault(fault);
}

void ProtectGuest(VAddr address, u64 size, bool writable) {
    Core::Memory::Instance()->GetAddressSpace().Protect(
        address, size, writable ? Core::MemoryPermission::ReadWrite : Core::MemoryPermission::Read);
}

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

// Rectangles draw as a strip of their own and quads as triangles; adjacency needs a geometry stage.
GLenum GlesPrimitiveMode(AmdGpu::PrimitiveType type) {
    switch (type) {
    case AmdGpu::PrimitiveType::RectList:
        return GL_TRIANGLE_STRIP;
    case AmdGpu::PrimitiveType::QuadList:
        return GL_TRIANGLES;
    case AmdGpu::PrimitiveType::AdjLineList:
    case AmdGpu::PrimitiveType::AdjLineStrip:
    case AmdGpu::PrimitiveType::AdjTriangleList:
    case AmdGpu::PrimitiveType::AdjTriangleStrip:
        return 0;
    default:
        return PrimitiveMode(type);
    }
}

bool IsConstantColorFactor(GLenum factor) {
    return factor == GL_CONSTANT_COLOR || factor == GL_ONE_MINUS_CONSTANT_COLOR;
}

bool IsConstantAlphaFactor(GLenum factor) {
    return factor == GL_CONSTANT_ALPHA || factor == GL_ONE_MINUS_CONSTANT_ALPHA;
}

GLenum ConstantAlphaAsColor(GLenum factor) {
    return factor == GL_CONSTANT_ALPHA         ? GLenum(GL_CONSTANT_COLOR)
           : factor == GL_ONE_MINUS_CONSTANT_ALPHA ? GLenum(GL_ONE_MINUS_CONSTANT_COLOR)
                                                   : factor;
}

template <typename Visit>
void VisitGuestIndices(VAddr address, u32 count, u32 index_size, Visit visit) {
    if (index_size == sizeof(u16)) {
        visit(std::span{reinterpret_cast<const u16*>(address), count});
    } else {
        visit(std::span{reinterpret_cast<const u32*>(address), count});
    }
}

void ClearColorAttachment(GlState& state, const AmdGpu::ColorBuffer& buffer, u32 index) {
    state.ColorMask(index, 0xF);
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

Rasterizer::Rasterizer(AmdGpu::Liverpool* liverpool_, const Capabilities& capabilities_)
    : liverpool{liverpool_}, memory{Core::Memory::Instance()}, capabilities{capabilities_},
      gles{capabilities_.gles}, separate_blend{capabilities_.draw_buffers_blend},
      anisotropy{capabilities_.anisotropy},
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
      state{capabilities_.gles, capabilities_.depth_clamp},
      // Never freed: guest threads can fault on armed pages until the process ends.
      watch{new GuestWriteWatch(ProtectGuest)},
      ring{state, capabilities_.gles                                       ? RingStorage::ClientMemory
                  : capabilities_.buffer_storage ? RingStorage::Persistent
                                                               : RingStorage::Mapped},
      gles_shader_ring{capabilities_.gles
                           ? std::make_unique<StreamRing>(state, RingStorage::ShaderTexture)
                           : nullptr},
      shader_ring{gles_shader_ring ? *gles_shader_ring : ring},
      texture_cache{state, *watch, capabilities},
      shaders{state, max_viewport_width, max_viewport_height,
              capabilities_.gles ? Shader::Backend::GLSL::Target::Gles300
                                 : Shader::Backend::GLSL::Target::Desktop330},
      gds(GdsBytes) {
    fault_watch = watch;
    Core::Signals::Instance()->RegisterAccessViolationHandler(OnGuestFault, WatchFaultPriority);
    std::printf("OpenGL: stream ring %s\n", gles            ? "client memory, shader data in a texture"
                                            : ring.Persistent() ? "persistent"
                                                                : "mapped per draw");
    if (anisotropy) {
        gl::GetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &max_anisotropy);
    }
    // Client memory is only allowed in the default vertex array.
    if (!gles) {
        gl::GenVertexArrays(1, &vertex_array);
    }
    gl::GenFramebuffers(2, resolve_framebuffers);
    gl::GenTextures(1, &black_texture);
    state.BindForEdit(GL_TEXTURE_2D, black_texture);
    constexpr u32 opaque_black = 0xFF000000;
    gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &opaque_black);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    texture_cache.SetDestroyCallback([this](GLuint texture) {
        state.ForgetFramebuffers();
        std::erase_if(framebuffers, [&](const auto& entry) {
            const auto& cached = entry.second;
            if (std::ranges::find(cached.textures, texture) == cached.textures.end()) {
                return false;
            }
            gl::DeleteFramebuffers(1, &cached.handle);
            return true;
        });
    });
    state.Enable(GlState::Cap::FramebufferSrgb, true);
    state.Enable(GlState::Cap::ProgramPointSize, true);
    liverpool->BindRasterizer(this);
    memory->SetRasterizer(this);
}

Rasterizer::~Rasterizer() {
    watch->ReleaseAll();
    for (const auto& [key, framebuffer] : framebuffers) {
        gl::DeleteFramebuffers(1, &framebuffer.handle);
    }
    for (const auto& [key, sampler] : samplers) {
        gl::DeleteSamplers(1, &sampler);
    }
    gl::DeleteFramebuffers(2, resolve_framebuffers);
    gl::DeleteTextures(1, &black_texture);
    if (vertex_array) {
        gl::DeleteVertexArrays(1, &vertex_array);
    }
}

void Rasterizer::Report(const std::string& what) {
    if (reported.insert(what).second) {
        std::printf("OpenGL: %s\n", what.c_str());
    }
}

void Rasterizer::Report(const char* literal) {
    if (reported_literals.insert(literal).second) {
        std::printf("OpenGL: %s\n", literal);
    }
}

void Rasterizer::ReportFirst(const char* cause, const std::string& details) {
    if (reported_literals.insert(cause).second) {
        std::printf("OpenGL: %s: %s\n", cause, details.c_str());
    }
}

template <typename Sent, typename Value>
bool Rasterizer::UniformChanged(Sent& sent, const Value& value) {
    if (sent == value) {
        return false;
    }
    sent = value;
    return true;
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
    state.BindFramebuffer(FramebufferFor(targets));
    state.Enable(GlState::Cap::ScissorTest, false);
    state.Enable(GlState::Cap::RasterizerDiscard, false);
    ClearColorAttachment(state, buffer, 0);
}

void Rasterizer::Resolve() {
    const auto& regs = liverpool->regs;
    Image* source = texture_cache.FindColorTarget(regs.color_buffers[0], liverpool->last_cb_extent[0]);
    Image* target = texture_cache.FindColorTarget(regs.color_buffers[1], liverpool->last_cb_extent[1]);
    if (!source || !target) {
        return;
    }
    state.BindReadFramebuffer(resolve_framebuffers[0]);
    gl::FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                             source->handle, 0);
    state.BindDrawFramebuffer(resolve_framebuffers[1]);
    gl::FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                             target->handle, 0);
    state.Enable(GlState::Cap::ScissorTest, false);
    state.Enable(GlState::Cap::RasterizerDiscard, false);
    state.ColorMask(0, 0xF);
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
        return it->second.handle;
    }
    CachedFramebuffer cached{};
    GLuint framebuffer = 0;
    gl::GenFramebuffers(1, &framebuffer);
    cached.handle = framebuffer;
    state.BindFramebuffer(framebuffer);
    std::array<GLenum, AmdGpu::NUM_COLOR_BUFFERS> draw_buffers{};
    u32 count = 0;
    for (u32 i = 0; i < targets.color.size(); ++i) {
        if (const Image* image = targets.color[i]) {
            gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D,
                                     image->handle, 0);
            draw_buffers[i] = GL_COLOR_ATTACHMENT0 + i;
            cached.textures[i] = image->handle;
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
        cached.textures.back() = depth->handle;
    }
    if (gl::CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        Report("a framebuffer combination is incomplete; its draws may be lost");
    }
    framebuffers.emplace(key, cached);
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
    state.BindFramebuffer(FramebufferFor(targets));
    return true;
}

// Vulkan clears through the render pass load operation; here it runs before the draw that asked.
void Rasterizer::ClearTargets(const AttachedTargets& targets) {
    const auto& regs = liverpool->regs;
    const auto unclip = [this] {
        state.Enable(GlState::Cap::ScissorTest, false);
        state.Enable(GlState::Cap::RasterizerDiscard, false);
    };
    for (u32 cb = 0; cb < targets.color.size(); ++cb) {
        if (!targets.color[cb]) {
            continue;
        }
        const auto& buffer = regs.color_buffers[cb];
        if (!texture_cache.IsMetaCleared(buffer.CmaskAddress())) {
            continue;
        }
        texture_cache.TouchMeta(buffer.CmaskAddress());
        unclip();
        ClearColorAttachment(state, buffer, cb);
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
        unclip();
        state.DepthMask(GL_TRUE);
        const float value = regs.depth_clear;
        gl::ClearBufferfv(GL_DEPTH, 0, &value);
    }
    if (stencil_clear) {
        unclip();
        state.StencilMask(GL_FRONT, 0xFF);
        state.StencilMask(GL_BACK, 0xFF);
        const GLint value = GLint(regs.stencil_clear);
        gl::ClearBufferiv(GL_STENCIL, 0, &value);
    }
}

GLuint Rasterizer::SamplerFor(AmdGpu::Sampler sharp, bool shadow, bool is_depth, bool nearest_only) {
    using namespace Vulkan::LiverpoolToVK;
    const int forced = DcpSettings::Get().anisotropy;
    bool force_aniso = false;
    if (forced > 1 && !is_depth && sharp.mip_filter != AmdGpu::MipFilter::None &&
        (sharp.xy_min_filter == AmdGpu::Filter::Bilinear ||
         sharp.xy_min_filter == AmdGpu::Filter::AnisoLinear)) {
        force_aniso = !AmdGpu::IsAnisoFilter(sharp.xy_min_filter) || sharp.MaxAniso() < float(forced);
    }
    const SamplerKey key{sharp.raw0, sharp.raw1 ^ (force_aniso ? u64(forced) << 44 : 0), shadow,
                         nearest_only};
    if (const auto it = samplers.find(key); it != samplers.end()) {
        return it->second;
    }
    GLuint sampler = 0;
    gl::GenSamplers(1, &sampler);
    const bool linear_min = !nearest_only && Filter(sharp.xy_min_filter) == vk::Filter::eLinear;
    GLint min_filter = linear_min ? GL_LINEAR : GL_NEAREST;
    if (sharp.mip_filter != AmdGpu::MipFilter::None && !nearest_only) {
        const bool linear_mip = MipFilter(sharp.mip_filter) == vk::SamplerMipmapMode::eLinear;
        min_filter = linear_min ? (linear_mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST)
                                : (linear_mip ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
    }
    gl::SamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, min_filter);
    gl::SamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER,
                          !nearest_only && Filter(sharp.xy_mag_filter) == vk::Filter::eLinear
                              ? GL_LINEAR
                              : GL_NEAREST);
    gl::SamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GLint(WrapMode(ClampMode(sharp.clamp_x))));
    gl::SamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GLint(WrapMode(ClampMode(sharp.clamp_y))));
    gl::SamplerParameteri(sampler, GL_TEXTURE_WRAP_R, GLint(WrapMode(ClampMode(sharp.clamp_z))));
    if (!gles) {
        gl::SamplerParameterf(sampler, GL_TEXTURE_LOD_BIAS, sharp.LodBias());
    } else if (sharp.LodBias() != 0.0f) {
        Report("a sampler's LOD bias has no OpenGL ES equivalent and was ignored");
    }
    gl::SamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, sharp.MinLod());
    gl::SamplerParameterf(sampler, GL_TEXTURE_MAX_LOD, sharp.MaxLod());
    if (anisotropy && !nearest_only) {
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
    Image* image = nullptr;
    if (sharp.Address() != 0 && memory->IsValidGpuMapping(sharp.Address(), 0) &&
        data_format != AmdGpu::DataFormat::FormatInvalid &&
        Vulkan::LiverpoolToVK::TrySurfaceFormat(data_format, number_format) !=
            vk::Format::eUndefined) {
        image = texture_cache.FindTexture(sharp, desc);
    }
    if (!image) {
        const GLenum target = ImageTarget(sharp.GetViewType(desc.is_array));
        state.BindTexture(unit, target, target == GL_TEXTURE_2D ? black_texture : 0);
        state.BindSampler(unit, 0);
        return;
    }
    if (image->is_srgb != (number_format == AmdGpu::NumberFormat::Srgb)) {
        Report("an image is sampled with the other sRGB encoding than it was rendered with");
    }
    if (sharp.base_array != 0 && image->target == GL_TEXTURE_2D) {
        Report("a texture view of a later array slice samples the first");
    }
    state.BindTexture(unit, image->target, image->handle);
    const auto mapping = Vulkan::LiverpoolToVK::ComponentMapping(sharp.DstSelect());
    const u32 last_level = std::max(image->info.resources.levels, 1u) - 1;
    texture_cache.ApplyView(*image, unit,
                            SampledView{
                                .swizzle = {Swizzle(mapping.r, GL_RED), Swizzle(mapping.g, GL_GREEN),
                                            Swizzle(mapping.b, GL_BLUE), Swizzle(mapping.a, GL_ALPHA)},
                                .base_level = GLint(std::min<u32>(sharp.base_level, last_level)),
                                .max_level = GLint(std::min<u32>(sharp.last_level, last_level)),
                            });
    if (combined.sampler == Shader::Backend::GLSL::NoSampler) {
        state.BindSampler(unit, 0);
        return;
    }
    const auto sampler = info.samplers[combined.sampler].GetSharp(info);
    // OpenGL ES calls these incomplete under a linear filter, where desktop drivers filter anyway.
    const auto number_class = AmdGpu::GetNumberClass(number_format);
    const bool nearest_only = gles && ((image->info.props.is_depth && !combined.shadow) ||
                                       number_class == AmdGpu::NumberClass::Uint ||
                                       number_class == AmdGpu::NumberClass::Sint);
    state.BindSampler(unit, SamplerFor(sampler, combined.shadow, desc.is_depth, nearest_only));
}

void Rasterizer::BindStage(const Shader::Info& info, StageLocations& locations,
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
        if (UniformChanged(locations.sent_user_data, user_data)) {
            gl::Uniform4uiv(locations.user_data, 4, user_data.data());
        }
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
                    offset = shader_ring.PushGuest(sharp.base_address, size, DwordAlignment);
                }
                break;
            }
            case Shader::BufferType::Flatbuf: {
                const auto flat = info.FlatUserData();
                size = u32(flat.size_bytes());
                offset = shader_ring.Push(flat.data(), size, DwordAlignment);
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
                offset = shader_ring.Push(planes.data(), size, DwordAlignment);
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
        if (UniformChanged(locations.sent_buffers, table)) {
            gl::Uniform2uiv(locations.buffers, GLsizei(std::max<size_t>(info.buffers.size(), 1)),
                            table[0].data());
        }
    }
    for (u32 i = 0; i < glsl.samplers.size(); ++i) {
        BindTexture(info, glsl.samplers[i], locations.sampler_units[i]);
    }
}

Rasterizer::GuestIndices Rasterizer::GuestIndexRange(u32 index_offset) {
    const auto& regs = liverpool->regs;
    const u32 size = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16 ? 2 : 4;
    const VAddr address = regs.index_base_address.Address<VAddr>() + VAddr(index_offset) * size;
    const u64 readable = memory->ClampRangeSize(address, u64(regs.num_indices) * size);
    const u32 count = ReadableIndexCount(regs.num_indices, readable, size);
    if (count < regs.num_indices) {
        ReportFirst("an index buffer runs past mapped guest memory and was cut short",
                    fmt::format("{} of {} indices at {:#x}", count, u32(regs.num_indices), address));
    }
    return {address, count, size};
}

bool Rasterizer::VerticesReachable(u64 highest_vertex) {
    if (highest_vertex > vertex_reach) {
        ReportFirst("a draw naming a vertex past the vertex stream was dropped",
                    fmt::format("vertex {}, last reachable {}", highest_vertex, vertex_reach));
        return false;
    }
    return true;
}

void Rasterizer::SetEnabledAttributes(u32 wanted) {
    for (u32 changed = wanted ^ enabled_attributes; changed != 0; changed &= changed - 1) {
        const u32 location = std::countr_zero(changed);
        if (wanted & (1u << location)) {
            gl::EnableVertexAttribArray(location);
        } else {
            gl::DisableVertexAttribArray(location);
        }
    }
    enabled_attributes = wanted;
}

// `first_vertex` moves the attributes themselves when the draw has no way to say it (OpenGL ES).
bool Rasterizer::BindVertexInputs(const GraphicsProgram& program, u32 first_instance, u32 first_vertex,
                                  bool rect_list) {
    using Shader::Backend::GLSL::RectVertexCount;
    const auto& info = *program.vertex->info;
    const auto& fetch = program.vertex->spec.fetch_shader_data;
    state.BindArrayBuffer(ring.Buffer());
    u32 wanted = 0;
    const auto& regs = liverpool->regs;
    vertex_reach = std::numeric_limits<u64>::max();
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
            const u32 pushed = ring.PushGuest(buffer.base_address, size, VertexAlignment);
            if (pushed == StreamRing::NoOffset) {
                Report("vertex data larger than the stream ring was dropped");
                return false;
            }
            const u64 offset = ring.PointerBase() + pushed;
            const u32 stride = buffer.GetStride();
            const auto step_rate = attrib.GetStepRate();
            u32 divisor = 0;
            if (step_rate != InstanceIdType::None) {
                divisor = step_rate == InstanceIdType::OverStepRate0   ? regs.vgt_instance_step_rate_0
                          : step_rate == InstanceIdType::OverStepRate1 ? regs.vgt_instance_step_rate_1
                                                                       : 1;
            }
            const bool per_vertex = step_rate == InstanceIdType::None && stride != 0;
            const u64 first = step_rate != InstanceIdType::None ? u64(first_instance) * stride : 0;
            if (rect_list) {
                const auto& inputs = program.vertex->glsl.vertex_inputs;
                const auto input = std::ranges::find(inputs, attrib.semantic,
                                                     &Shader::Backend::GLSL::VertexInput::semantic);
                const u32 slot = u32(input - inputs.begin());
                if (input == inputs.end() || (slot + 1) * RectVertexCount > MaxVertexAttributes) {
                    Report("a rectangle list reads too many vertex attributes and was dropped");
                    return false;
                }
                // One instance per rectangle: each attribute advances three vertices at a time.
                for (u32 corner = 0; corner < RectVertexCount; ++corner) {
                    const GLuint location = slot * RectVertexCount + corner;
                    const u64 start = per_vertex ? u64(first_vertex + corner) * stride : first;
                    state.VertexAttrib(location, format->integer, format->size, format->type,
                                       format->normalized, GLsizei(stride * RectVertexCount),
                                       offset + start);
                    state.VertexDivisor(location, per_vertex ? 1 : ConstantAttributeDivisor);
                    wanted |= 1u << location;
                }
                continue;
            }
            const u64 start = per_vertex ? u64(first_vertex) * stride : first;
            // ANGLE copies client memory by what the draw names, so nothing may name past the ring.
            if (gles) {
                const u32 instances = regs.num_instances.NumInstances();
                const u64 instance_step = u64(stride) * ((instances - 1) / std::max(divisor, 1u));
                const auto reach = HighestElementInside(u64(pushed) + start + (divisor ? instance_step : 0),
                                                        VertexFormatBytes(*format), divisor ? 0 : stride,
                                                        ring.Capacity());
                if (!reach) {
                    Report("a vertex attribute past the vertex stream was dropped");
                    return false;
                }
                vertex_reach = std::min(vertex_reach, *reach);
            }
            const GLuint location = attrib.semantic;
            state.VertexAttrib(location, format->integer, format->size, format->type,
                               format->normalized, GLsizei(stride), offset + start);
            state.VertexDivisor(location, stride == 0 ? ConstantAttributeDivisor : divisor);
            wanted |= 1u << location;
        }
    }
    SetEnabledAttributes(wanted);
    return true;
}

// Each input becomes a table of decoded elements in the shader ring: a vertex stage that has to run
// the guest for three vertices at once cannot get them from attributes, which give it one.
bool Rasterizer::WriteRectInputs(const GraphicsProgram& program, u32 first_instance, u32 guest_instances) {
    using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
    constexpr u32 NoTable = ~0u;
    const auto& info = *program.vertex->info;
    const auto& fetch = program.vertex->spec.fetch_shader_data;
    const auto& inputs = program.vertex->glsl.vertex_inputs;
    const auto& regs = liverpool->regs;
    const auto reserve = [this](u32 elements, u32& offset) {
        const u64 bytes = u64(elements) * RectInputElementBytes;
        return bytes > shader_ring.Capacity() ? nullptr : shader_ring.Reserve(u32(bytes), DwordAlignment, offset);
    };
    rect_input_table.assign(inputs.size(), {NoTable, RectConstantDivisor, 0, 0});
    const u32 fetched = fetch ? u32(fetch->attributes.size()) : 0;
    for (u32 index = 0; index < fetched; ++index) {
        const auto& attrib = fetch->attributes[index];
        const auto input = std::ranges::find(inputs, attrib.semantic,
                                             &Shader::Backend::GLSL::VertexInput::semantic);
        if (input == inputs.end()) {
            continue;
        }
        const auto buffer = attrib.GetSharp(info);
        const auto vk_format =
            Vulkan::LiverpoolToVK::TrySurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        const auto* format = FindVertexFormat(vk_format);
        if (!format) {
            Report(fmt::format("unsupported vertex format {}", vk::to_string(vk_format)));
            continue;
        }
        const u64 size = buffer.base_address != 0
                             ? memory->ClampRangeSize(buffer.base_address, buffer.GetSize())
                             : 0;
        const auto* data = reinterpret_cast<const u8*>(buffer.base_address);
        const u32 stride = buffer.GetStride();
        const auto step_rate = attrib.GetStepRate();
        auto& table = rect_input_table[size_t(input - inputs.begin())];
        u32 offset = 0;
        u8* out = nullptr;
        if (stride != 0 && step_rate == InstanceIdType::None) {
            out = reserve(u32(expanded_indices.size()), offset);
            for (size_t position = 0; out && position < expanded_indices.size(); ++position) {
                const auto element = ReadVertexElement(*format, data, size, stride, expanded_indices[position]);
                std::memcpy(out + position * RectInputElementBytes, element.data(), RectInputElementBytes);
            }
            table = {offset / DwordAlignment, 0, 0, 0};
        } else {
            const u32 rate = step_rate == InstanceIdType::OverStepRate0   ? regs.vgt_instance_step_rate_0
                             : step_rate == InstanceIdType::OverStepRate1 ? regs.vgt_instance_step_rate_1
                                                                          : 1;
            const u32 divisor = stride == 0 ? RectConstantDivisor : std::max(rate, 1u);
            const u64 stored = stride == 0 ? 0 : size / stride;
            const u32 elements = RectInstanceElements(guest_instances, divisor, stored);
            out = reserve(elements, offset);
            for (u32 element = 0; out && element < elements; ++element) {
                const auto value = ReadVertexElement(*format, data, size, stride,
                                                     stride == 0 ? 0 : u64(first_instance) + element);
                std::memcpy(out + element * RectInputElementBytes, value.data(), RectInputElementBytes);
            }
            table = {offset / DwordAlignment, divisor, elements - 1, 0};
        }
        if (!out) {
            Report("a rectangle list's vertex data larger than the stream ring was dropped");
            return false;
        }
    }
    for (size_t slot = 0; slot < rect_input_table.size(); ++slot) {
        if (rect_input_table[slot][0] != NoTable) {
            continue;
        }
        u32 offset = 0;
        u8* out = reserve(1, offset);
        if (!out) {
            Report("a rectangle list's vertex data larger than the stream ring was dropped");
            return false;
        }
        const VertexElement missing{0, 0, 0,
                                    inputs[slot].number_class == AmdGpu::NumberClass::Float
                                        ? std::bit_cast<u32>(1.0f)
                                        : 1u};
        std::memcpy(out, missing.data(), RectInputElementBytes);
        rect_input_table[slot][0] = offset / DwordAlignment;
    }
    return true;
}

bool Rasterizer::PrepareIndexedRectangles(const GraphicsProgram& program, u32 index_offset, u32 first_vertex,
                                          u32 first_instance, GLsizei& strip_instances) {
    const auto& regs = liverpool->regs;
    const bool restart = (regs.enable_primitive_restart & 1) != 0;
    if (regs.num_indices > shader_ring.Capacity() / sizeof(u32)) {
        Report("a rectangle list larger than the stream ring was dropped");
        return false;
    }
    const auto indices = GuestIndexRange(index_offset);
    VisitGuestIndices(indices.address, indices.count, indices.size, [&](auto span) {
        AssembleRectVertices(span, restart, first_vertex, expanded_indices);
    });
    if (expanded_indices.empty()) {
        return false;
    }
    const u32 rectangles = u32(expanded_indices.size()) / RectVerticesPerRect;
    const u32 guest_instances = regs.num_instances.NumInstances();
    const auto instances = RectDrawInstances(rectangles, guest_instances);
    if (!instances) {
        ReportFirst("a rectangle list with too many instances was dropped",
                    fmt::format("{} rectangles, {} instances", rectangles, guest_instances));
        return false;
    }
    SetEnabledAttributes(0);
    const u32 vertices = shader_ring.Push(expanded_indices.data(),
                                          u32(expanded_indices.size() * sizeof(u32)), DwordAlignment);
    if (vertices == StreamRing::NoOffset) {
        Report("a rectangle list larger than the stream ring was dropped");
        return false;
    }
    if (!WriteRectInputs(program, first_instance, guest_instances)) {
        return false;
    }
    const std::array<u32, 2> draw{vertices / DwordAlignment, rectangles};
    if (program.rect_draw >= 0) {
        gl::Uniform2uiv(program.rect_draw, 1, draw.data());
    }
    if (program.rect_inputs >= 0 && !rect_input_table.empty()) {
        gl::Uniform4uiv(program.rect_inputs, GLsizei(rect_input_table.size()), rect_input_table[0].data());
    }
    strip_instances = *instances;
    return true;
}

void Rasterizer::ApplyViewportScissor(GraphicsProgram& program) {
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
        state.Viewport(0, 0, 1, 1);
        left = top = right = bottom = 0;
    } else if (regs.IsClipDisabled()) {
        state.Viewport(0, 0, GLsizei(max_viewport_width), GLsizei(max_viewport_height));
    } else {
        // OpenGL needs a positive height: a negative one becomes a flip in the vertex stage.
        const float width = std::abs(2.0f * xscale);
        const float height = std::abs(2.0f * yscale);
        state.Viewport(GLint(xoffset - std::abs(xscale)), GLint(yoffset - std::abs(yscale)),
                       GLsizei(width), GLsizei(height));
        flip_y = yscale < 0.0f ? -1.0f : 1.0f;
    }
    if (viewport) {
        const float zoffset = control.zoffset_enable ? viewport->zoffset : 0.0f;
        const float zscale = control.zscale_enable ? viewport->zscale : 1.0f;
        const bool symmetric = regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
        const float depth_min = std::clamp(symmetric ? zoffset - zscale : zoffset, 0.0f, 1.0f);
        const float depth_max = std::clamp(zoffset + zscale, 0.0f, 1.0f);
        state.DepthRange(depth_min, depth_max);
    }
    if (program.flip_y >= 0 && UniformChanged(program.sent_flip_y, flip_y)) {
        gl::Uniform1f(program.flip_y, flip_y);
    }
    if (program.transform >= 0 &&
        UniformChanged(program.sent_transform,
                       std::array<float, 4>{xoffset, yoffset, xscale, yscale})) {
        gl::Uniform4f(program.transform, xoffset, yoffset, xscale, yscale);
    }
    state.Enable(GlState::Cap::ScissorTest, true);
    state.Scissor(left, top, std::max(right - left, 0), std::max(bottom - top, 0));
}

void Rasterizer::ApplyDepthStencil() {
    using namespace Vulkan::LiverpoolToVK;
    const auto& regs = liverpool->regs;
    const auto effective = Vulkan::GetEffectiveDepthStencilState(regs);
    using Cap = GlState::Cap;
    state.Enable(Cap::DepthTest, effective.depth_test_enable);
    if (effective.depth_test_enable) {
        state.DepthFunc(CompareFunc(CompareOp(regs.depth_control.depth_func)));
    }
    state.DepthMask(effective.depth_write_enable && !regs.depth_render_control.depth_clear_enable
                        ? GL_TRUE
                        : GL_FALSE);
    if (effective.depth_bounds_enable) {
        Report("the depth bounds test is not available and was ignored");
    }
    state.Enable(Cap::StencilTest, effective.stencil_test_enable);
    if (!effective.stencil_test_enable) {
        return;
    }
    const auto& control = regs.stencil_control;
    const bool two_sided = regs.depth_control.backface_enable;
    // Direct3D has one set of stencil masks and references: ANGLE rejects a draw whose faces differ.
    const bool shared_face_state = gles;
    const auto replaces = [](AmdGpu::StencilFunc fail, AmdGpu::StencilFunc pass,
                             AmdGpu::StencilFunc zfail) {
        using enum AmdGpu::StencilFunc;
        return fail == ReplaceOp || pass == ReplaceOp || zfail == ReplaceOp;
    };
    const auto front_func = CompareFunc(CompareOp(regs.depth_control.stencil_ref_func));
    const auto back_func =
        two_sided ? CompareFunc(CompareOp(regs.depth_control.stencil_bf_func)) : front_func;
    const auto& front = regs.stencil_ref_front;
    const auto& back = two_sided && !shared_face_state ? regs.stencil_ref_back : regs.stencil_ref_front;
    const bool front_replaces = replaces(control.stencil_fail_front, control.stencil_zpass_front,
                                         control.stencil_zfail_front);
    const bool back_replaces = two_sided ? replaces(control.stencil_fail_back,
                                                    control.stencil_zpass_back,
                                                    control.stencil_zfail_back)
                                         : front_replaces;
    state.StencilFunc(GL_FRONT, front_func,
                      GLint(front_replaces ? front.stencil_op_val : front.stencil_test_val),
                      front.stencil_mask);
    const GLint front_ref = GLint(front_replaces ? front.stencil_op_val : front.stencil_test_val);
    const GLint back_ref = GLint(back_replaces ? back.stencil_op_val : back.stencil_test_val);
    if (shared_face_state && two_sided &&
        (front_ref != back_ref || front.stencil_mask != regs.stencil_ref_back.stencil_mask ||
         front.stencil_write_mask != regs.stencil_ref_back.stencil_write_mask)) {
        Report("two-sided stencil with different masks or references uses the front face's");
    }
    state.StencilFunc(GL_BACK, back_func, shared_face_state ? front_ref : back_ref, back.stencil_mask);
    const bool clearing = regs.depth_render_control.stencil_clear_enable;
    state.StencilMask(GL_FRONT, clearing ? 0u : front.stencil_write_mask);
    state.StencilMask(GL_BACK, clearing ? 0u : back.stencil_write_mask);
    const auto stencil_op = [](AmdGpu::StencilFunc func) {
        return OpenGL::StencilOp(Vulkan::LiverpoolToVK::StencilOp(func));
    };
    state.StencilOp(GL_FRONT, stencil_op(control.stencil_fail_front),
                    stencil_op(control.stencil_zfail_front),
                    stencil_op(control.stencil_zpass_front));
    state.StencilOp(GL_BACK,
                    stencil_op(two_sided ? control.stencil_fail_back : control.stencil_fail_front),
                    stencil_op(two_sided ? control.stencil_zfail_back : control.stencil_zfail_front),
                    stencil_op(two_sided ? control.stencil_zpass_back
                                         : control.stencil_zpass_front));
}

void Rasterizer::ApplyRasterState() {
    using namespace Vulkan::LiverpoolToVK;
    const auto& regs = liverpool->regs;
    const auto cull = IsPrimitiveCulled(regs.primitive_type) ? regs.polygon_control.CullingMode()
                                                             : AmdGpu::CullMode::None;
    using Cap = GlState::Cap;
    state.Enable(Cap::CullFace, cull != AmdGpu::CullMode::None);
    if (cull != AmdGpu::CullMode::None) {
        state.CullFace(cull == AmdGpu::CullMode::Front  ? GL_FRONT
                       : cull == AmdGpu::CullMode::Back ? GL_BACK
                                                        : GL_FRONT_AND_BACK);
    }
    // Render targets keep Vulkan's window coordinates; its signed area rule is OpenGL's negated.
    state.FrontFace(regs.polygon_control.front_face == AmdGpu::FrontFace::CounterClockwise ? GL_CW
                                                                                           : GL_CCW);
    const auto mode = regs.polygon_control.PolyMode();
    if (gles && mode == AmdGpu::PolygonMode::Point) {
        Report("point polygon mode has no OpenGL ES equivalent and fills");
    } else {
        state.PolygonMode(mode == AmdGpu::PolygonMode::Point  ? GL_POINT
                          : mode == AmdGpu::PolygonMode::Line ? GL_LINE
                                                              : GL_FILL);
    }
    const bool bias = regs.polygon_control.NeedsBias();
    state.Enable(Cap::PolygonOffsetFill, bias);
    if (bias) {
        const bool front = regs.polygon_control.enable_polygon_offset_front;
        state.PolygonOffset((front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale) / 16.f,
                            front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset);
    }
    state.Enable(Cap::DepthClamp, !regs.clipper_control.ZclipEnable());
    state.Enable(Cap::RasterizerDiscard, regs.clipper_control.dx_rasterization_kill != 0);
    state.LineWidth(std::max(regs.line_control.Width(), 1.0f));
    state.ProvokingVertex(regs.polygon_control.provoking_vtx_last == AmdGpu::ProvokingVtxLast::Last
                              ? GL_LAST_VERTEX_CONVENTION
                              : GL_FIRST_VERTEX_CONVENTION);
}

void Rasterizer::ApplyBlend(const GraphicsProgram& program, const AttachedTargets& targets) {
    using namespace Vulkan;
    const auto& regs = liverpool->regs;
    state.BlendColor(regs.blend_constants[0], regs.blend_constants[1], regs.blend_constants[2],
                     regs.blend_constants[3]);
    const auto logic_op = regs.color_control.rop3;
    const bool uses_logic_op = logic_op != AmdGpu::ColorControl::LogicOp::Copy;
    state.Enable(GlState::Cap::ColorLogicOp, uses_logic_op);
    if (uses_logic_op) {
        state.LogicOp(OpenGL::LogicOp(LiverpoolToVK::LogicOp(logic_op)));
        if (gles) {
            Report("a logic operation has no OpenGL ES equivalent and was ignored");
        }
    }
    bool global_set = false;
    const auto apply = [&](u32 cb, GLenum src_color, GLenum dst_color, GLenum eq_color,
                           GLenum src_alpha, GLenum dst_alpha, GLenum eq_alpha) {
        if (!separate_blend && global_set) {
            Report("different blending per render target needs OpenGL 4.0; the first one is used");
            return;
        }
        global_set = true;
        const bool mixes_constants = (IsConstantColorFactor(src_color) || IsConstantColorFactor(dst_color)) &&
                                     (IsConstantAlphaFactor(src_color) || IsConstantAlphaFactor(dst_color));
        if (gles && mixes_constants) {
            Report("blending with constant colour and alpha together uses the colour");
            src_color = ConstantAlphaAsColor(src_color);
            dst_color = ConstantAlphaAsColor(dst_color);
        }
        state.BlendFunc(cb, separate_blend, src_color, dst_color, src_alpha, dst_alpha);
        state.BlendEquation(cb, separate_blend, eq_color, eq_alpha);
    };
    for (u32 cb = 0; cb < targets.color.size(); ++cb) {
        const auto& buffer = regs.color_buffers[cb];
        if (!targets.color[cb]) {
            state.BlendEnable(cb, false);
            continue;
        }
        state.ColorMask(cb, buffer.Swizzle().ApplyMask(regs.color_target_mask.GetMask(cb)));
        const auto& control = regs.blend_control[cb];
        if (!control.enable || buffer.info.blend_bypass) {
            state.BlendEnable(cb, false);
            continue;
        }
        state.BlendEnable(cb, true);
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
    const bool rect_list = gles && regs.primitive_type == AmdGpu::PrimitiveType::RectList;
    const bool rect_tables = RectListUsesTables(rect_list, is_indexed, indexed_rect_tables);
    GraphicsProgram* program = shaders.GetGraphics(regs, rect_tables);
    if (!program) {
        return;
    }
    const GLenum mode = gles ? GlesPrimitiveMode(regs.primitive_type) : PrimitiveMode(regs.primitive_type);
    if (mode == 0) {
        Report(fmt::format("primitive type {} is not implemented", u32(regs.primitive_type)));
        return;
    }
    const bool quad_list = gles && regs.primitive_type == AmdGpu::PrimitiveType::QuadList;
    if (rect_list && is_indexed && !rect_tables) {
        Report("indexed rectangle lists are not implemented");
        return;
    }
    AttachedTargets targets{};
    if (!BindTargets(*program, targets)) {
        return;
    }
    state.BindVertexArray(vertex_array);
    state.UseProgram(program->handle);
    BeginStreams();

    const auto& vs_info = *program->vertex->info;
    const auto [vertex_offset, instance_offset] =
        DrawOffsets(regs, vs_info, program->vertex->spec.fetch_shader_data);

    state.BindSampler(0, 0);
    state.BindTexture(0, shader_ring.TextureTarget(), shader_ring.Texture());
    BindStage(vs_info, program->vs, program->vertex->glsl);
    if (program->fragment) {
        BindStage(*program->fragment->info, program->fs, program->fragment->glsl);
    }
    // Elements on OpenGL ES have no base vertex here, and gl_VertexID would not include it anyway.
    const u32 moved_vertices = gles && (is_indexed || quad_list || rect_list) ? vertex_offset : 0;
    GLsizei rect_strips = 0;
    const bool prepared =
        rect_tables ? PrepareIndexedRectangles(*program, index_offset, vertex_offset, instance_offset, rect_strips)
                    : BindVertexInputs(*program, instance_offset, moved_vertices, rect_list);
    if (!prepared) {
        return;
    }

    u32 index_ring_offset = 0;
    bool index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    GLsizei count = GLsizei(regs.num_indices);
    if (quad_list) {
        if (regs.num_indices > ring.Capacity() / sizeof(u32) / 2) {
            Report("a quad list larger than the stream ring was dropped");
            return;
        }
        if (is_indexed) {
            const auto indices = GuestIndexRange(index_offset);
            const bool restart = (regs.enable_primitive_restart & 1) != 0;
            VisitGuestIndices(indices.address, indices.count, indices.size,
                              [&](auto span) { ExpandQuadIndices(span, restart, expanded_indices); });
        } else {
            ExpandSequentialQuads(regs.num_indices, expanded_indices);
        }
        const auto highest = HighestIndex(std::span<const u32>{expanded_indices}, false);
        if (!highest || !VerticesReachable(*highest)) {
            return;
        }
        index16 = false;
        count = GLsizei(expanded_indices.size());
        index_ring_offset = ring.Push(expanded_indices.data(),
                                      u32(expanded_indices.size() * sizeof(u32)), sizeof(u32));
        if (index_ring_offset == StreamRing::NoOffset) {
            Report("index data larger than the stream ring was dropped");
            return;
        }
    } else if (is_indexed && !rect_list) {
        const u32 index_size = index16 ? 2 : 4;
        const VAddr address =
            regs.index_base_address.Address<VAddr>() + VAddr(index_offset) * index_size;
        const u32 index_count = gles ? GuestIndexRange(index_offset).count : u32(regs.num_indices);
        count = GLsizei(index_count);
        index_ring_offset = ring.PushGuest(address, index_count * index_size, index_size);
        if (index_ring_offset == StreamRing::NoOffset) {
            Report("index data larger than the stream ring was dropped");
            return;
        }
        // The ring's copy, not guest memory: it is what the driver will read.
        if (gles) {
            std::optional<u32> highest;
            const bool restart = (regs.enable_primitive_restart & 1) != 0;
            VisitGuestIndices(ring.PointerBase() + index_ring_offset, index_count, index_size,
                              [&](auto span) { highest = HighestIndex(span, restart); });
            if (!highest || !VerticesReachable(*highest)) {
                return;
            }
        }
    } else if (gles && !rect_list && !VerticesReachable(u64(vertex_offset) + regs.num_indices)) {
        return;
    }
    CommitStreams();

    if (program->vertex_id_base >= 0 &&
        UniformChanged(program->sent_vertex_id_base, moved_vertices)) {
        gl::Uniform1ui(program->vertex_id_base, moved_vertices);
    }
    if (program->base_vertex >= 0 && UniformChanged(program->sent_base_vertex, vertex_offset)) {
        gl::Uniform1ui(program->base_vertex, vertex_offset);
    }
    if (program->base_instance >= 0 &&
        UniformChanged(program->sent_base_instance, instance_offset)) {
        gl::Uniform1ui(program->base_instance, instance_offset);
    }
    ClearTargets(targets);
    ApplyViewportScissor(*program);
    ApplyDepthStencil();
    ApplyRasterState();
    ApplyBlend(*program, targets);
    state.ClipDistances(program->vertex->glsl.clip_distances);
    const bool restart = (regs.enable_primitive_restart & 1) != 0;
    state.Enable(GlState::Cap::PrimitiveRestart, restart);
    if (restart) {
        state.PrimitiveRestartIndex(index16 ? 0xFFFFu : 0xFFFFFFFFu);
    }

    const GLsizei instances = GLsizei(regs.num_instances.NumInstances());
    if (rect_tables) {
        gl::DrawArraysInstanced(mode, 0, RectStripVertices, rect_strips);
    } else if (rect_list) {
        if (instances > 1) {
            Report("instanced rectangle lists draw their first instance");
        }
        gl::DrawArraysInstanced(mode, 0, RectStripVertices, count / GLsizei(RectVerticesPerRect));
    } else if (gles && (is_indexed || quad_list)) {
        state.BindElementBuffer(ring.Buffer());
        gl::DrawElementsInstanced(
            mode, count, index16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
            reinterpret_cast<const void*>(uintptr_t(ring.PointerBase() + index_ring_offset)), instances);
    } else if (is_indexed) {
        state.BindElementBuffer(ring.Buffer());
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
        watch->BeforeHostWrite(written.base_address, count * sizeof(u32));
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
    watch->BeforeHostWrite(address, num_bytes);
    std::fill(out, out + num_bytes / 4, value);
    texture_cache.NoteGpuWrite(address, num_bytes);
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    const auto* source = src_gds ? gds.data() + src : reinterpret_cast<const u8*>(src);
    auto* target = dst_gds ? gds.data() + dst : reinterpret_cast<u8*>(dst);
    if (!dst_gds) {
        watch->BeforeHostWrite(dst, num_bytes);
    }
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
    watch->Forget(addr, size);
    texture_cache.QueueUnmap(addr, size);
}

bool Rasterizer::InvalidateMemory(VAddr address, u64 size, bool) {
    watch->NoteWritten(address, size);
    return true;
}

u64 Rasterizer::Flush() {
    gl::Flush();
    return 0;
}

void Rasterizer::Finish() {
    gl::Finish();
}

void Rasterizer::OnSubmit() {
    ForgetStreamCopies();
}

void Rasterizer::BeginStreams() {
    ring.BeginDraw();
    if (gles_shader_ring) {
        gles_shader_ring->BeginDraw();
    }
}

void Rasterizer::CommitStreams() {
    ring.Commit();
    if (gles_shader_ring) {
        gles_shader_ring->Commit();
    }
}

void Rasterizer::ForgetStreamCopies() {
    ring.ForgetGuestCopies();
    if (gles_shader_ring) {
        gles_shader_ring->ForgetGuestCopies();
    }
}

void Rasterizer::OnFlip() {
    watch->AdvanceFrame();
    texture_cache.NextFrame();
    ForgetStreamCopies();
}

void Rasterizer::ResetState() {
    using Cap = GlState::Cap;
    state.BindFramebuffer(0);
    for (const Cap cap : {Cap::ScissorTest, Cap::DepthTest, Cap::StencilTest, Cap::CullFace,
                          Cap::PolygonOffsetFill, Cap::DepthClamp, Cap::RasterizerDiscard,
                          Cap::PrimitiveRestart, Cap::ColorLogicOp, Cap::FramebufferSrgb}) {
        state.Enable(cap, false);
    }
    state.ClipDistances(0);
    for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        state.BlendEnable(cb, false);
        state.ColorMask(cb, 0xF);
    }
    state.PolygonMode(GL_FILL);
    state.DepthRange(0.0, 1.0);
    state.DepthMask(GL_TRUE);
}

} // namespace OpenGL
