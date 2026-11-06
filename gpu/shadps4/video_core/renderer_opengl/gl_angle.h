// SPDX-License-Identifier: GPL-2.0-or-later
// Direct3D 11 through ANGLE on Windows: what makes a window's OpenGL ES context land on that backend.
#pragma once

#include <string>

namespace OpenGL {

/// Before the window exists, with SDL video running. Ends the process when ANGLE is not beside the game.
void RequestAngleDirect3D11();

/// The window could not be created: says why and ends the process, no other renderer takes over.
[[noreturn]] void ExitAngleUnavailable(const char* reason);

/// With the context current: the feature level ANGLE got and the library that was loaded.
std::string AngleDeviceDescription();

} // namespace OpenGL
