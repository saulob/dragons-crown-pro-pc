// SPDX-License-Identifier: GPL-2.0-or-later
// Graphics options as dcp.ini stores them, shared by the in-game menu, the window and the renderer.
#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace DcpGraphics {

enum DisplayMode : int { Windowed = 0, Borderless = 1, DisplayModeCount };
inline constexpr std::array<std::string_view, DisplayModeCount> DisplayModeKeys = {"windowed", "borderless"};
inline constexpr std::array<const char*, DisplayModeCount> DisplayModeNames = {"Windowed", "Borderless Fullscreen"};

template <typename T, std::size_t N>
constexpr int IndexOf(const std::array<T, N>& values, const T& value, int fallback) {
    for (std::size_t i = 0; i < N; ++i) {
        if (values[i] == value) return int(i);
    }
    return fallback;
}

inline int ParseDisplayMode(std::string_view text) {
    return IndexOf(DisplayModeKeys, text, Windowed);
}

/// Letterbox keeps the frame's aspect ratio in the window; Stretch fills the window with it.
enum Presentation : int { Letterbox = 0, Stretch = 1, PresentationCount };
inline constexpr std::array<std::string_view, PresentationCount> PresentationKeys = {"letterbox", "stretch"};

inline int ParsePresentation(std::string_view text) {
    return IndexOf(PresentationKeys, text, Letterbox);
}
/// RCAS strength in percent; invalid values fall back to 75.
inline int ParseRcasStrength(int value) {
    return value >= 0 && value <= 100 ? value : 75;
}
/// Forced anisotropic filtering: 0 is the game's own, else 2, 4, 8 or 16 times.
inline constexpr std::array<int, 5> Anisotropy = {0, 2, 4, 8, 16};
inline int ParseAnisotropy(int value) {
    return IndexOf(Anisotropy, value, -1) >= 0 ? value : 0;
}
/// Index of the device named `request` among `names` (an unusable device has an empty name); -1 for
/// Automatic, an empty request or a name that is not there.
inline int FindGpuByName(const std::vector<std::string>& names, std::string_view request) {
    if (request.empty() || request == "automatic") return -1;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (!names[i].empty() && names[i] == request) return int(i);
    }
    return -1;
}
/// FSR RCAS attenuation for a strength: 0 is the sharpest, 2 the softest.
inline float RcasAttenuation(int strength) {
    return 2.0f * (1.0f - float(ParseRcasStrength(strength)) / 100.0f);
}
/// Whether the FSR pass runs: FSR 1 when the window is larger than the game's image, RCAS
/// alone when they are the same size. A smaller window is left to the regular scaling.
inline bool FsrPassRuns(bool fsr1, bool rcas, int in_w, int in_h, int out_w, int out_h) {
    const bool upscale = in_w < out_w && in_h < out_h;
    const bool same = in_w == out_w && in_h == out_h;
    return (fsr1 && upscale) || (rcas && same);
}

} // namespace DcpGraphics
