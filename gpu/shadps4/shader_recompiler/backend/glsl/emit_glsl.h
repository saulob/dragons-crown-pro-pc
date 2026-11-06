// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>
#include "common/types.h"
#include "shader_recompiler/backend/glsl/ring_layout.h"
#include "shader_recompiler/info.h"
#include "video_core/amdgpu/pixel_format.h"

namespace Shader {
struct Profile;
struct RuntimeInfo;
} // namespace Shader

namespace Shader::IR {
struct Program;
}

namespace Shader::Backend::GLSL {

inline constexpr u32 NoSampler = 0xFFFF;

/// Only the spelling differs: both targets share one translation of the IR.
enum class Target : u8 { Desktop330, Gles300 };

/// GLSL ES has no geometry stage: a rectangle's vertex shader takes the attributes of its three vertices.
inline constexpr u32 RectVertexCount = 3;


/// GLSL has no separate images and samplers: every pair a shader uses is one sampler uniform.
struct CombinedSampler {
    u32 image;
    u32 sampler;
    bool shadow;
    std::string name;
};

struct VaryingInput {
    u32 location;
    Info::Interpolation interpolation;
};

struct VertexInput {
    u32 semantic;
    AmdGpu::NumberClass number_class;
};

struct Stage {
    Target target{};
    std::string declarations;
    std::string main;
    std::vector<CombinedSampler> samplers;
    std::vector<VaryingInput> inputs; ///< fragment stage
    std::vector<VertexInput> vertex_inputs; ///< vertex stage, Gles300: declared when the stages link
    u32 output_mask{};                ///< vertex stage: the parameters it writes
    u32 clip_distances{};             ///< vertex stage: how many gl_ClipDistance it writes
    bool dual_source{};               ///< fragment stage: writes a second blend colour
    bool uses_ring{};                 ///< reads dcp_ring
    std::string error;                ///< the first thing the target cannot express here
};

/// Rect and quad lists have no OpenGL primitive. Desktop: a geometry shader completes them. Gles300: quads become
/// indexed triangles on the CPU, and a rectangle is a four vertex strip the vertex stage works out alone.
/// IndexedRectList is Gles300 only: the same strip, reading its vertices from tables in the shader ring.
enum class PrimitiveExpansion : u8 { None, RectList, QuadList, IndexedRectList };

struct Sources {
    std::string vertex;
    std::string geometry;
    std::string fragment;
};

Stage EmitGLSL(const Profile& profile, const RuntimeInfo& runtime_info, const IR::Program& program,
              Target target = Target::Desktop330);

/// Joins two stages: GLSL 3.30 links varyings by name and needs their qualifiers to agree, and a
/// fragment input the vertex stage never writes is a link error (Vulkan reads it as undefined).
Sources LinkStages(const Stage& vertex, const Stage* fragment, PrimitiveExpansion expansion);

} // namespace Shader::Backend::GLSL
