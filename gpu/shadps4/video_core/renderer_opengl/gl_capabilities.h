// SPDX-License-Identifier: GPL-2.0-or-later
// What the backend needs from an OpenGL context; free of GL calls so it can be tested headless.
#pragma once

#include <string>

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
};

/// Dragon's ring buffer needs more than the 64 KiB texels OpenGL guarantees.
inline constexpr int MinimumRingTexels = 16 * 1024 * 1024 / 4;

/// Empty when the context can run the game, else what is missing.
inline std::string MissingRequirement(const Capabilities& caps) {
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
