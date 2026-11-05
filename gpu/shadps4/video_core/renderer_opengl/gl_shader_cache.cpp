// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <span>
#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>

#include "common/assert.h"
#include "common/logging/log.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/ir/program.h"
#include "video_core/renderer_opengl/gl_shader_cache.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"

namespace OpenGL {

using Shader::HwStage;
using Shader::Output;
using Shader::SwStage;
using Shader::Backend::GLSL::PrimitiveExpansion;

namespace {

u32 MapOutputs(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl) {
    u32 num_outputs = 0;
    if (ctl.vs_out_misc_enable) {
        auto& misc_vec = outputs[num_outputs++];
        misc_vec[0] = ctl.use_vtx_point_size ? Output::PointSize : Output::None;
        misc_vec[1] = ctl.use_vtx_edge_flag
                          ? Output::EdgeFlag
                          : (ctl.use_vtx_gs_cut_flag ? Output::GsCutFlag : Output::None);
        misc_vec[2] =
            ctl.use_vtx_kill_flag
                ? Output::KillFlag
                : (ctl.use_vtx_render_target_idx ? Output::RenderTargetIndex : Output::None);
        misc_vec[3] = ctl.use_vtx_viewport_idx ? Output::ViewportIndex : Output::None;
    }
    const auto clip_or_cull = [&](u32 index, Output clip, Output cull) {
        return ctl.IsClipDistEnabled(index)
                   ? clip
                   : (ctl.IsCullDistEnabled(index) ? cull : Output::None);
    };
    if (ctl.vs_out_ccdist0_enable) {
        auto& ccdist0 = outputs[num_outputs++];
        ccdist0[0] = clip_or_cull(0, Output::ClipDist0, Output::CullDist0);
        ccdist0[1] = clip_or_cull(1, Output::ClipDist1, Output::CullDist1);
        ccdist0[2] = clip_or_cull(2, Output::ClipDist2, Output::CullDist2);
        ccdist0[3] = clip_or_cull(3, Output::ClipDist3, Output::CullDist3);
    }
    if (ctl.vs_out_ccdist1_enable) {
        auto& ccdist1 = outputs[num_outputs++];
        ccdist1[0] = clip_or_cull(4, Output::ClipDist4, Output::CullDist4);
        ccdist1[1] = clip_or_cull(5, Output::ClipDist5, Output::CullDist5);
        ccdist1[2] = clip_or_cull(6, Output::ClipDist6, Output::CullDist6);
        ccdist1[3] = clip_or_cull(7, Output::ClipDist7, Output::CullDist7);
    }
    return num_outputs;
}

GLuint CompileStage(GLenum type, const std::string& source, std::string& error) {
    const GLuint shader = gl::CreateShader(type);
    const GLchar* text = source.c_str();
    gl::ShaderSource(shader, 1, &text, nullptr);
    gl::CompileShader(shader);
    GLint compiled = GL_FALSE;
    gl::GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled) {
        return shader;
    }
    std::array<GLchar, 4096> log{};
    gl::GetShaderInfoLog(shader, GLsizei(log.size()), nullptr, log.data());
    error = log.data();
    gl::DeleteShader(shader);
    return 0;
}

void BindSamplerUnits(GLuint program, const Shader::Backend::GLSL::Stage* stage,
                      StageLocations& locations, GLint& next_unit) {
    if (!stage) {
        return;
    }
    for (const auto& sampler : stage->samplers) {
        const GLint location = gl::GetUniformLocation(program, sampler.name.c_str());
        const GLuint unit = GLuint(next_unit++);
        locations.sampler_units.push_back(unit);
        if (location >= 0) {
            gl::Uniform1i(location, GLint(unit));
        }
    }
}

} // namespace

PrimitiveExpansion ExpansionOf(AmdGpu::PrimitiveType type) {
    switch (type) {
    case AmdGpu::PrimitiveType::RectList:
        return PrimitiveExpansion::RectList;
    case AmdGpu::PrimitiveType::QuadList:
        return PrimitiveExpansion::QuadList;
    default:
        return PrimitiveExpansion::None;
    }
}

ShaderCache::ShaderCache(u32 max_viewport_width, u32 max_viewport_height) {
    profile.max_viewport_width = max_viewport_width;
    profile.max_viewport_height = max_viewport_height;
    profile.subgroup_size = 64;
}

ShaderCache::~ShaderCache() {
    for (const auto& [key, program] : graphics) {
        if (program) {
            gl::DeleteProgram(program->handle);
        }
    }
}

void ShaderCache::ReportOnce(u64 key, const std::string& what) {
    if (reported.insert(key).second) {
        std::printf("OpenGL: skipping draws: %s\n", what.c_str());
    }
}

Shader::RuntimeInfo ShaderCache::BuildRuntimeInfo(const AmdGpu::Regs& regs, HwStage hw_stage,
                                                  SwStage sw_stage) const {
    Shader::RuntimeInfo info{};
    info.Initialize(hw_stage, sw_stage);
    const auto common = [&](const auto& program) {
        info.props.num_user_data = program.settings.num_user_regs;
        info.props.num_input_vgprs = program.settings.vgpr_comp_cnt;
        info.props.num_allocated_vgprs = program.NumVgprs();
        info.props.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = program.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = program.settings.fp_round_mode64;
    };
    if (hw_stage == HwStage::Vertex) {
        common(regs.vs_program);
        info.hw.vs.user_clip_plane_mask = regs.clipper_control.user_clip_plane_enable;
        info.hw.vs.num_outputs = MapOutputs(info.hw.vs.outputs, regs.vs_output_control);
        info.hw.vs.emulate_depth_negative_one_to_one =
            regs.clipper_control.clip_space == AmdGpu::ClipSpace::ZeroToW;
        info.hw.vs.clip_disable = regs.IsClipDisabled();
        info.sw.vs.step_rate_0 = regs.vgt_instance_step_rate_0;
        info.sw.vs.step_rate_1 = regs.vgt_instance_step_rate_1;
        info.sw.vs.tess_emulated_primitive =
            regs.primitive_type == AmdGpu::PrimitiveType::RectList ||
            regs.primitive_type == AmdGpu::PrimitiveType::QuadList;
        return info;
    }
    common(regs.ps_program);
    info.hw.fs.en_flags = regs.ps_input_ena;
    info.hw.fs.addr_flags = regs.ps_input_addr;
    info.hw.fs.num_inputs = regs.num_interp;
    info.hw.fs.front_face_all_bits = regs.barycentric_control.front_face_all_bits;
    info.hw.fs.num_samples =
        regs.ps_input_addr.sample_coverage_ena && regs.ps_input_ena.sample_coverage_ena
            ? regs.aa_config.NumSamples()
            : 1;
    info.hw.fs.z_export_format = regs.z_export_format;
    const u8 stencil_ref_export = regs.depth_shader_control.stencil_op_val_export_enable |
                                  regs.depth_shader_control.stencil_test_val_export_enable;
    info.hw.fs.mrtz_mask = regs.depth_shader_control.z_export_enable | (stencil_ref_export << 1) |
                           (regs.depth_shader_control.mask_export_enable << 2) |
                           (regs.depth_shader_control.coverage_to_mask_enable << 3);
    const auto& blend = regs.blend_control[0];
    if (blend.enable) {
        using Vulkan::LiverpoolToVK::IsDualSourceBlendFactor;
        info.hw.fs.dual_source_blending = IsDualSourceBlendFactor(blend.color_dst_factor) ||
                                          IsDualSourceBlendFactor(blend.color_src_factor);
        if (blend.separate_alpha_blend) {
            info.hw.fs.dual_source_blending |= IsDualSourceBlendFactor(blend.alpha_dst_factor) ||
                                               IsDualSourceBlendFactor(blend.alpha_src_factor);
        }
    }
    for (u32 i = 0; i < regs.num_interp; ++i) {
        info.hw.fs.inputs[i] = {
            .param_index = u8(regs.ps_inputs[i].input_offset),
            .is_default = bool(regs.ps_inputs[i].use_default),
            .is_flat = bool(regs.ps_inputs[i].flat_shade),
            .default_value = u8(regs.ps_inputs[i].default_value),
        };
    }
    const bool skip_cb = regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    for (u32 cb = 0; cb < Shader::MaxColorBuffers && !skip_cb; ++cb) {
        const auto& buffer = regs.color_buffers[cb];
        if (!buffer || !regs.color_target_mask.GetMask(cb)) {
            continue;
        }
        auto& color = info.hw.fs.color_buffers[cb];
        color.data_format = buffer.GetDataFmt();
        color.num_format = buffer.GetNumberFmt();
        color.num_conversion = buffer.GetNumberConversion();
        color.export_format = regs.color_export_format.GetFormat(cb);
        color.swizzle = buffer.Swizzle();
    }
    return info;
}

StageModule* ShaderCache::GetModule(HwStage hw_stage, SwStage sw_stage,
                                    const Shader::ShaderParams& params,
                                    Shader::RuntimeInfo runtime_info) {
    auto& modules = programs[params.hash];
    for (auto& module : modules) {
        auto& info = *module->info;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        info.RefreshFlatBuf();
        const Shader::StageSpecialization spec(info, runtime_info, profile, {});
        if (module->spec == spec) {
            return module.get();
        }
    }
    auto module = std::make_unique<StageModule>();
    module->info = std::make_unique<Shader::Info>(hw_stage, sw_stage, params);
    const auto program =
        Shader::TranslateProgram(params.code, pools, *module->info, runtime_info, profile);
    module->spec = Shader::StageSpecialization(*module->info, runtime_info, profile, {});
    if (sw_stage != SwStage::Compute) {
        module->glsl = Shader::Backend::GLSL::EmitGLSL(profile, runtime_info, program);
    }
    LOG_INFO(Render, "OpenGL: recompiled {} shader {:#x} (permutation {})",
             magic_enum::enum_name(hw_stage), params.hash, modules.size());
    modules.push_back(std::move(module));
    return modules.back().get();
}

GraphicsProgram* ShaderCache::Link(StageModule* vertex, StageModule* fragment,
                                   PrimitiveExpansion expansion) {
    const auto sources = Shader::Backend::GLSL::LinkStages(
        vertex->glsl, fragment ? &fragment->glsl : nullptr, expansion);
    std::string error;
    const u64 report_key = vertex->info->pgm_hash ^ (fragment ? fragment->info->pgm_hash << 1 : 0);
    const GLuint vs = CompileStage(GL_VERTEX_SHADER, sources.vertex, error);
    const GLuint gs = vs && !sources.geometry.empty()
                          ? CompileStage(GL_GEOMETRY_SHADER, sources.geometry, error)
                          : 0;
    const GLuint fs = vs ? CompileStage(GL_FRAGMENT_SHADER, sources.fragment, error) : 0;
    const GLuint program = gl::CreateProgram();
    if (vs && fs && (sources.geometry.empty() || gs)) {
        gl::AttachShader(program, vs);
        if (gs) {
            gl::AttachShader(program, gs);
        }
        gl::AttachShader(program, fs);
        gl::LinkProgram(program);
    }
    GLint linked = GL_FALSE;
    gl::GetProgramiv(program, GL_LINK_STATUS, &linked);
    for (const GLuint shader : {vs, gs, fs}) {
        if (shader) {
            gl::DeleteShader(shader);
        }
    }
    if (!linked) {
        if (error.empty()) {
            std::array<GLchar, 4096> log{};
            gl::GetProgramInfoLog(program, GLsizei(log.size()), nullptr, log.data());
            error = log.data();
        }
        gl::DeleteProgram(program);
        graphics.emplace(ProgramKey{vertex, fragment, expansion}, nullptr);
        ReportOnce(report_key, fmt::format("shaders {:#x}/{:#x} did not compile: {}",
                                           vertex->info->pgm_hash,
                                           fragment ? fragment->info->pgm_hash : 0, error));
        // DEBUG_SAULO: temporary, the rejected GLSL for the first test rounds.
        std::printf("DEBUG_SAULO: vertex source\n%s\nDEBUG_SAULO: geometry source\n%s\n"
                    "DEBUG_SAULO: fragment source\n%s\n",
                    sources.vertex.c_str(), sources.geometry.c_str(), sources.fragment.c_str());
        return nullptr;
    }

    auto linked_program = std::make_unique<GraphicsProgram>();
    auto& out = *linked_program;
    out.handle = program;
    out.vertex = vertex;
    out.fragment = fragment;
    out.vs.user_data = gl::GetUniformLocation(program, "vs_ud");
    out.vs.buffers = gl::GetUniformLocation(program, "vs_buffers");
    out.fs.user_data = gl::GetUniformLocation(program, "fs_ud");
    out.fs.buffers = gl::GetUniformLocation(program, "fs_buffers");
    out.transform = gl::GetUniformLocation(program, "vs_transform");
    out.flip_y = gl::GetUniformLocation(program, "vs_flip_y");
    out.base_vertex = gl::GetUniformLocation(program, "vs_base_vertex");
    out.base_instance = gl::GetUniformLocation(program, "vs_base_instance");
    gl::UseProgram(program);
    if (const GLint ring = gl::GetUniformLocation(program, "dcp_ring"); ring >= 0) {
        gl::Uniform1i(ring, 0);
    }
    GLint next_unit = 1;
    BindSamplerUnits(program, &vertex->glsl, out.vs, next_unit);
    BindSamplerUnits(program, fragment ? &fragment->glsl : nullptr, out.fs, next_unit);
    return graphics.emplace(ProgramKey{vertex, fragment, expansion}, std::move(linked_program))
        .first->second.get();
}

GraphicsProgram* ShaderCache::GetGraphics(const AmdGpu::Regs& regs) {
    if (regs.stage_enable.raw != AmdGpu::ShaderStageEnable::VgtStages::Vs) {
        ReportOnce(u64(regs.stage_enable.raw) | 1ull << 63,
                   fmt::format("shader stage set {:#x} (only VS + PS is implemented)",
                               u32(regs.stage_enable.raw)));
        return nullptr;
    }
    const auto* vs_program = regs.ProgramForStage(u32(HwStage::Vertex));
    if (!vs_program || !vs_program->Address<u32*>()) {
        return nullptr;
    }
    const auto vs_params = AmdGpu::GetParams(*vs_program);
    StageModule* vertex =
        GetModule(HwStage::Vertex, SwStage::Vertex, vs_params,
                  BuildRuntimeInfo(regs, HwStage::Vertex, SwStage::Vertex));

    StageModule* fragment = nullptr;
    const auto* ps_program = regs.ProgramForStage(u32(HwStage::Fragment));
    if (regs.stage_enable.IsStageEnabled(u32(HwStage::Fragment)) && ps_program &&
        ps_program->Address<u32*>()) {
        fragment = GetModule(HwStage::Fragment, SwStage::Fragment, AmdGpu::GetParams(*ps_program),
                             BuildRuntimeInfo(regs, HwStage::Fragment, SwStage::Fragment));
    }

    for (const StageModule* module : {vertex, fragment}) {
        if (module && !module->glsl.error.empty()) {
            ReportOnce(module->info->pgm_hash,
                       fmt::format("shader {:#x}: GLSL 3.30 cannot express {}",
                                   module->info->pgm_hash, module->glsl.error));
            return nullptr;
        }
    }
    const auto expansion = ExpansionOf(regs.primitive_type);
    const auto it = graphics.find(ProgramKey{vertex, fragment, expansion});
    return it != graphics.end() ? it->second.get() : Link(vertex, fragment, expansion);
}

Shader::Info* ShaderCache::GetCompute(const AmdGpu::ComputeProgram& program) {
    const auto params = AmdGpu::GetParams(program);
    Shader::RuntimeInfo runtime_info{};
    runtime_info.Initialize(HwStage::Compute, SwStage::Compute);
    runtime_info.props.num_user_data = program.settings.num_user_regs;
    runtime_info.props.num_allocated_vgprs = program.settings.num_vgprs * 4;
    runtime_info.props.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
    runtime_info.props.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
    runtime_info.props.fp_round_mode32 = program.settings.fp_round_mode32;
    runtime_info.props.fp_round_mode16_64 = program.settings.fp_round_mode64;
    runtime_info.hw.cs.workgroup_size = {program.num_thread_x.full, program.num_thread_y.full,
                                         program.num_thread_z.full};
    runtime_info.hw.cs.tgid_enable = {program.IsTgidEnabled(0), program.IsTgidEnabled(1),
                                      program.IsTgidEnabled(2)};
    runtime_info.hw.cs.shared_memory_size = program.SharedMemSize();
    return GetModule(HwStage::Compute, SwStage::Compute, params, runtime_info)->info.get();
}

} // namespace OpenGL
