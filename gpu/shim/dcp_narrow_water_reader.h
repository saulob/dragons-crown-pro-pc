// Narrower than 16:9, the water scene draws a partial depth-only pass whose output is black rectangles; it is skipped.
#pragma once

#include <cstdint>

namespace DcpNarrowWaterReader {

inline constexpr std::uint64_t kPixelShader = 0xec957f1d;
inline constexpr std::uint64_t kVertexShader = 0xa17e81a;
inline constexpr std::uint32_t kSceneFormat = 43;
inline constexpr double kAspectEpsilon = 0.01;

struct Draw {
    std::uint64_t ps = 0, vs = 0;
    std::uint32_t format = 0;
    int width = 0, height = 0;
    bool partial = false, depth = false;
    unsigned sampled = 0;
};

inline bool NarrowerThan16x9(int width, int height) {
    return height > 0 && double(width) / double(height) < 16.0 / 9.0 - kAspectEpsilon;
}

inline bool ShouldSkip(const Draw& d) {
    return d.ps == kPixelShader && d.vs == kVertexShader && d.format == kSceneFormat && d.width >= 1280 &&
           d.height >= 720 && NarrowerThan16x9(d.width, d.height) && d.partial && d.depth && d.sampled == 0;
}

} // namespace DcpNarrowWaterReader
