// SPDX-License-Identifier: GPL-2.0-or-later
// What the backend needs from an OpenGL context; free of GL calls so it can be tested headless.
#pragma once

#include <string>
#include <string_view>

namespace OpenGL {

struct Capabilities {
    int major{};
    int minor{};
    bool s3tc{};
    bool bptc{};
    bool anisotropy{};
    bool draw_buffers_blend{};
    bool buffer_storage{};
    int max_texture_buffer_texels{};
    bool gles{}; ///< OpenGL ES through ANGLE instead of desktop OpenGL
    bool s3tc_srgb{};
    bool rgtc{};
    bool norm16{};
    bool depth_clamp{};
    int max_texture_size{};
};

/// Dragon's ring buffer needs more than the 64 KiB texels OpenGL guarantees.
inline constexpr int MinimumRingTexels = 16 * 1024 * 1024 / 4;

/// The texture that stands in for a buffer texture on OpenGL ES.
inline constexpr int MinimumGlesTextureSize = 4096;

/// ANGLE's renderer string names the backend it really started on.
inline bool IsAngleDirect3D11(std::string_view renderer) {
    return renderer.find("ANGLE") != std::string_view::npos &&
           renderer.find("Direct3D11") != std::string_view::npos;
}

inline std::string MissingGlesRequirement(const Capabilities& caps) {
    if (caps.major < 3) {
        return "OpenGL ES 3.0 is required, which needs Direct3D feature level 10_1 or newer; this context is " +
               std::to_string(caps.major) + "." + std::to_string(caps.minor);
    }
    if (!caps.s3tc) {
        return "GL_EXT_texture_compression_s3tc is required for the game's BC1-BC3 textures";
    }
    if (!caps.draw_buffers_blend) {
        return "GL_EXT_draw_buffers_indexed is required for per-target blending";
    }
    if (caps.max_texture_size < MinimumGlesTextureSize) {
        return "textures of " + std::to_string(MinimumGlesTextureSize) +
               " texels are required, this device allows " + std::to_string(caps.max_texture_size);
    }
    return {};
}

/// Empty when the context can run the game, else what is missing.
inline std::string MissingRequirement(const Capabilities& caps) {
    if (caps.gles) {
        return MissingGlesRequirement(caps);
    }
    if (caps.major < 3 || (caps.major == 3 && caps.minor < 3)) {
        return "OpenGL 3.3 or newer is required, this context is " + std::to_string(caps.major) +
               "." + std::to_string(caps.minor);
    }
    if (!caps.s3tc) {
        return "GL_EXT_texture_compression_s3tc is required for the game's BC1-BC3 textures";
    }
    if (caps.max_texture_buffer_texels < MinimumRingTexels) {
        return "a texture buffer of at least " + std::to_string(MinimumRingTexels) +
               " texels is required, this driver allows " +
               std::to_string(caps.max_texture_buffer_texels);
    }
    return {};
}

} // namespace OpenGL
