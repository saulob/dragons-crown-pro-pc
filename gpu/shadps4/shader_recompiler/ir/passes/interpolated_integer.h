// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace Shader::Optimization {

// Away from zero, so truncation of either sign lands on the integer the vertices held.
constexpr float InterpolatedIntegerScale = 1.0f + 1.0f / 65536.0f;

constexpr float NudgeInterpolatedInteger(float value) {
    return value * InterpolatedIntegerScale;
}

} // namespace Shader::Optimization
