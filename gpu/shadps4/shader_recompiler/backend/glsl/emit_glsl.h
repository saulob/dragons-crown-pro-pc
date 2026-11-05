// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>
#include "common/types.h"
#include "shader_recompiler/info.h"

namespace Shader {
struct Profile;
struct RuntimeInfo;
} // namespace Shader

namespace Shader::IR {
struct Program;
}

namespace Shader::Backend::GLSL {

inline constexpr u32 NoSampler = 0xFFFF;

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

struct Stage {
    std::string declarations;
    std::string main;
    std::vector<CombinedSampler> samplers;
    std::vector<VaryingInput> inputs; ///< fragment stage
    u32 output_mask{};                ///< vertex stage: the parameters it writes
    u32 clip_distances{};             ///< vertex stage: how many gl_ClipDistance it writes
    std::string error;                ///< the first thing GLSL 3.30 cannot express here
};

/// Rect and quad lists have no OpenGL primitive: a geometry shader completes them.
enum class PrimitiveExpansion : u8 { None, RectList, QuadList };

struct Sources {
    std::string vertex;
    std::string geometry;
    std::string fragment;
};

Stage EmitGLSL(const Profile& profile, const RuntimeInfo& runtime_info, const IR::Program& program);

/// Joins two stages: GLSL 3.30 links varyings by name and needs their qualifiers to agree, and a
/// fragment input the vertex stage never writes is a link error (Vulkan reads it as undefined).
Sources LinkStages(const Stage& vertex, const Stage* fragment, PrimitiveExpansion expansion);

} // namespace Shader::Backend::GLSL
