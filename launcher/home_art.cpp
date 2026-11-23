#include "home_art.h"

#include <algorithm>
#include <cmath>

#include "header_art.h"

namespace dcp::home {
namespace {

constexpr float kNavy[3] = {7.0f, 12.0f, 26.0f};

float Smooth(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

std::vector<uint8_t> Enlarge(const uint8_t* rgba, int width, int height, int out_width, int out_height) {
    std::vector<uint8_t> out(size_t(out_width) * size_t(out_height) * 4);
    for (int y = 0; y < out_height; ++y) {
        const float fy = std::max(0.0f, (y + 0.5f) * float(height) / float(out_height) - 0.5f);
        const int y0 = std::min(int(fy), height - 1), y1 = std::min(y0 + 1, height - 1);
        const float wy = fy - float(y0);
        for (int x = 0; x < out_width; ++x) {
            const float fx = std::max(0.0f, (x + 0.5f) * float(width) / float(out_width) - 0.5f);
            const int x0 = std::min(int(fx), width - 1), x1 = std::min(x0 + 1, width - 1);
            const float wx = fx - float(x0);
            for (int c = 0; c < 3; ++c) {
                const auto texel = [&](int tx, int ty) { return float(rgba[(size_t(ty) * size_t(width) + size_t(tx)) * 4 + size_t(c)]); };
                const float top = texel(x0, y0) + (texel(x1, y0) - texel(x0, y0)) * wx;
                const float bottom = texel(x0, y1) + (texel(x1, y1) - texel(x0, y1)) * wx;
                out[(size_t(y) * size_t(out_width) + size_t(x)) * 4 + size_t(c)] = uint8_t(top + (bottom - top) * wy + 0.5f);
            }
            out[(size_t(y) * size_t(out_width) + size_t(x)) * 4 + 3] = 255;
        }
    }
    return out;
}

} // namespace

ArtCrop CoverCrop(int source_width, int source_height, int target_width, int target_height, float anchor_x, float anchor_y) {
    if (source_width <= 0 || source_height <= 0 || target_width <= 0 || target_height <= 0) {
        return {};
    }
    const long long wide = (long long)source_width * target_height, tall = (long long)source_height * target_width;
    ArtCrop crop{0, 0, source_width, source_height};
    if (wide > tall) {
        crop.width = std::clamp(int(tall / target_height), 1, source_width);
        crop.x = int(std::lround(float(source_width - crop.width) * std::clamp(anchor_x, 0.0f, 1.0f)));
    } else {
        crop.height = std::clamp(int(wide / target_width), 1, source_height);
        crop.y = int(std::lround(float(source_height - crop.height) * std::clamp(anchor_y, 0.0f, 1.0f)));
    }
    return crop;
}

float ShadeAt(float across, float down) {
    const float left = 0.84f - 0.58f * Smooth(0.0f, 0.68f, across);
    const float edges = 0.10f * Smooth(0.55f, 1.0f, down) + 0.06f * (1.0f - Smooth(0.0f, 0.25f, down));
    return std::clamp(left + edges, 0.0f, 0.95f);
}

std::vector<uint8_t> RenderHomeBackground(const uint8_t* rgba, int width, int height, int out_width, int out_height) {
    if (!rgba || width <= 0 || height <= 0 || out_width <= 0 || out_height <= 0) {
        return {};
    }
    const ArtCrop crop = CoverCrop(width, height, out_width, out_height);
    std::vector<uint8_t> cropped(size_t(crop.width) * size_t(crop.height) * 4);
    for (int y = 0; y < crop.height; ++y) {
        const uint8_t* row = rgba + (size_t(crop.y + y) * size_t(width) + size_t(crop.x)) * 4;
        std::copy(row, row + size_t(crop.width) * 4, cropped.begin() + std::ptrdiff_t(size_t(y) * size_t(crop.width) * 4));
    }
    std::vector<uint8_t> out = out_width <= crop.width && out_height <= crop.height
                                   ? ScaleArtRgba(cropped.data(), crop.width, crop.height, out_width, out_height)
                                   : Enlarge(cropped.data(), crop.width, crop.height, out_width, out_height);
    for (int y = 0; y < out_height; ++y) {
        const float down = (y + 0.5f) / float(out_height);
        for (int x = 0; x < out_width; ++x) {
            const float shade = ShadeAt((x + 0.5f) / float(out_width), down);
            uint8_t* pixel = out.data() + (size_t(y) * size_t(out_width) + size_t(x)) * 4;
            for (int c = 0; c < 3; ++c) {
                pixel[c] = uint8_t(float(pixel[c]) + (kNavy[c] - float(pixel[c])) * shade + 0.5f);
            }
            pixel[3] = 255;
        }
    }
    return out;
}

} // namespace dcp::home
