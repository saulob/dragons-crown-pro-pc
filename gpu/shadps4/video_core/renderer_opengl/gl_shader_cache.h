// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "shader_recompiler/backend/glsl/emit_glsl.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/amdgpu/regs.h"
#include "video_core/renderer_opengl/gl_api.h"

namespace OpenGL {

class GlState;

/// One recompilation of a guest shader: a permutation chosen by its specialization.
struct StageModule {
    std::unique_ptr<Shader::Info> info;
    Shader::StageSpecialization spec;
    Shader::Backend::GLSL::Stage glsl;
};

struct StageLocations {
    GLint user_data = -1;
    GLint buffers = -1;
    std::vector<GLuint> sampler_units;
    std::optional<std::array<u32, Shader::NUM_USER_DATA_REGS>> sent_user_data;
    std::optional<std::array<std::array<u32, 2>, Shader::NUM_BUFFERS>> sent_buffers;
};

struct GraphicsProgram {
    GLuint handle{};
    StageModule* vertex{};
    StageModule* fragment{};
    StageLocations vs;
    StageLocations fs;
    GLint transform = -1;
    GLint flip_y = -1;
    GLint base_vertex = -1;
    GLint base_instance = -1;
    GLint vertex_id_base = -1;
    GLint rect_draw = -1;
    GLint rect_inputs = -1;
    std::optional<std::array<float, 4>> sent_transform;
    std::optional<float> sent_flip_y;
    std::optional<u32> sent_base_vertex;
    std::optional<u32> sent_base_instance;
    std::optional<u32> sent_vertex_id_base;
};

class ShaderCache {
public:
    ShaderCache(GlState& state, u32 max_viewport_width, u32 max_viewport_height,
                Shader::Backend::GLSL::Target target);
    ~ShaderCache();

    /// Null when the draw needs something this backend cannot do yet (logged once per cause).
    GraphicsProgram* GetGraphics(const AmdGpu::Regs& regs, bool rect_tables);
    /// Compute shaders are not run, but their resources identify known fill and clear passes.
    Shader::Info* GetCompute(const AmdGpu::ComputeProgram& program);

private:
    using Programs = std::unordered_map<u64, std::vector<std::unique_ptr<StageModule>>>;

    StageModule* GetModule(Shader::HwStage hw_stage, Shader::SwStage sw_stage,
                           const Shader::ShaderParams& params,
                           Shader::RuntimeInfo runtime_info);
    Shader::RuntimeInfo BuildRuntimeInfo(const AmdGpu::Regs& regs, Shader::HwStage hw_stage,
                                         Shader::SwStage sw_stage) const;
    GraphicsProgram* Link(StageModule* vertex, StageModule* fragment,
                          Shader::Backend::GLSL::PrimitiveExpansion expansion);
    void ReportOnce(u64 key, const std::string& what);

    GlState& state;
    Shader::Backend::GLSL::Target target;
    Shader::Profile profile{};
    Shader::Pools pools;
    Programs programs;
    using ProgramKey =
        std::tuple<StageModule*, StageModule*, Shader::Backend::GLSL::PrimitiveExpansion>;
    std::map<ProgramKey, std::unique_ptr<GraphicsProgram>> graphics;
    std::unordered_set<u64> reported;
};

/// `target` decides whether a list needs a program of its own: GLSL ES quads are plain triangles.
Shader::Backend::GLSL::PrimitiveExpansion ExpansionOf(AmdGpu::PrimitiveType type,
                                                      Shader::Backend::GLSL::Target target,
                                                      bool rect_tables = false);

} // namespace OpenGL
