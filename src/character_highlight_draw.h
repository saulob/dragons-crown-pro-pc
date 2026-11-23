// Character Highlight outline: player colors, outline thickness and exterior coverage, kept free of Vulkan so tests can
// run it.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace dcp::highlight {

struct Offset {
    int x, y;
    bool operator==(const Offset&) const = default;
};

inline constexpr std::array<Offset, 8> kOutlineDirections = {{{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}}};

// Player colors: P1 red, P2 blue, P3 green, P4 yellow.
struct PlayerColor {
    bool r, g, b;
    bool operator==(const PlayerColor&) const = default;
};
inline constexpr std::array<PlayerColor, 4> kPlayerColors = {{{true, false, false}, {false, false, true}, {false, true, false}, {true, true, false}}};

inline PlayerColor ColorOfPlayer(int slot) {
    return kPlayerColors[static_cast<size_t>(slot >= 0 && slot < static_cast<int>(kPlayerColors.size()) ? slot : 0)];
}

inline std::array<float, 4> OutlineColorOfPlayer(int slot) {
    const PlayerColor color = ColorOfPlayer(slot);
    return {color.r ? 1.0f : 0.0f, color.g ? 1.0f : 0.0f, color.b ? 1.0f : 0.0f, 1.0f};
}

// Values match AmdGpu::CompSwizzle: 0 zero, 1 one, 4 red, 5 green, 6 blue, 7 alpha.
inline constexpr std::array<uint32_t, 4> kIdentityColorSwizzle = {4, 5, 6, 7};

inline std::array<float, 4> ApplyColorSwizzle(const std::array<float, 4>& color,
                                              const std::array<uint32_t, 4>& swizzle) {
    const auto component = [&](uint32_t channel) {
        if (channel == 0) return 0.0f;
        if (channel == 1) return 1.0f;
        return color[std::min<uint32_t>(channel - 4, 3)];
    };
    return {component(swizzle[0]), component(swizzle[1]), component(swizzle[2]),
            component(swizzle[3])};
}

// Must match character_highlight_outline.frag's push_constant block.
struct OutlineSettings {
    std::array<float, 4> color;
    std::array<float, 2> size;
    float pixels;
    float pad;
    std::array<uint32_t, 4> swizzle;
};
static_assert(sizeof(OutlineSettings) == 48);

inline OutlineSettings MakeOutlineSettings(int slot, float width, float height, float pixels,
                                           const std::array<uint32_t, 4>& swizzle) {
    return {
        .color = OutlineColorOfPlayer(slot),
        .size = {width, height},
        .pixels = pixels,
        .pad = 0.0f,
        .swizzle = swizzle,
    };
}

inline constexpr float kOutlinePixelsAt1080p = 2.35f;
inline constexpr float kOutlineMinPixels = 1.0f;
inline constexpr float kOutlineMaxPixels = 5.0f;
inline constexpr float kReferenceHeight = 1080.0f;

// Outline thickness in render target pixels for a target of `height` pixels; 0 for a size that cannot be right.
inline float OutlinePixels(float height) {
    height = std::fabs(height);
    if (!(height >= 1.0f) || height > 16384.0f) return 0.0f;
    return std::clamp(kOutlinePixelsAt1080p * height / kReferenceHeight, kOutlineMinPixels, kOutlineMaxPixels);
}

// Any non-zero coverage counts as the character itself: an outline pixel may only exist outside the mask.
inline constexpr float kCoverageThreshold = 0.5f / 255.0f;

inline float OutlineCoverage(float center, float around) {
    if (!(center <= kCoverageThreshold)) return 0.0f;
    return std::max(around - center, 0.0f);
}

inline float MaxCoverage(std::span<const float> values) {
    float result = 0.0f;
    for (const float value : values) result = std::max(result, value);
    return result;
}

} // namespace dcp::highlight
