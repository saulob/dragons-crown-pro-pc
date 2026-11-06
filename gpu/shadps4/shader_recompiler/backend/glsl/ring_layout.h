// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace Shader::Backend::GLSL {

/// OpenGL ES has no buffer textures: there the shared buffer is a 2D texture this many bits wide.
inline constexpr unsigned RingRowBits = 12;

} // namespace Shader::Backend::GLSL
