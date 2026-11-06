#include "header_art.h"

#include <algorithm>
#include <cmath>

namespace dcp {
namespace {

struct Weight {
    int source;
    float amount;
};

std::vector<std::vector<Weight>> AreaWeights(int source, int target) {
    std::vector<std::vector<Weight>> weights(static_cast<size_t>(target));
    const double ratio = double(source) / double(target);
    for (int i = 0; i < target; ++i) {
        const double begin = i * ratio, end = (i + 1) * ratio;
        for (int s = int(begin); s < source && s < end; ++s) {
            const double covered = std::min(end, s + 1.0) - std::max(begin, double(s));
            if (covered > 0.0) {
                weights[size_t(i)].push_back({s, float(covered / ratio)});
            }
        }
    }
    return weights;
}

} // namespace

HeaderArtRect PlaceHeaderArt(float top, float right, float bottom, float viewport_height, float scale, float aspect) {
    const float art_height = std::clamp(viewport_height * 0.2f, 120.0f * scale, 190.0f * scale) * kHeaderArtScale;
    const float height = std::ceil(std::max(bottom - top, art_height));
    const float width = std::round(height * aspect);
    const float above = std::floor(std::min(height - (bottom - top), height * kHeaderArtTopCrop));
    return {right - width, top - above, right, top - above + height};
}

std::vector<HeaderArtStrip> SplitHeaderArt(const HeaderArtRect& rect) {
    std::vector<HeaderArtStrip> strips;
    const float width = rect.max_x - rect.min_x;
    for (float x = 0.0f; x < width; x += float(kHeaderArtStripWidth)) {
        const float end = std::min(x + float(kHeaderArtStripWidth), width);
        strips.push_back({rect.min_x + x, rect.min_x + end, x / width, end / width});
    }
    return strips;
}

std::vector<uint8_t> ScaleArtRgba(const uint8_t* rgba, int width, int height, int out_width, int out_height,
                                  float alpha_gain) {
    if (!rgba || width <= 0 || height <= 0 || out_width <= 0 || out_height <= 0) {
        return {};
    }
    const auto across = AreaWeights(width, out_width);
    const auto down = AreaWeights(height, out_height);
    const size_t out_w = size_t(out_width), src_w = size_t(width);

    // Premultiplied, so that transparent texels add no colour to a stroke's edge.
    std::vector<float> rows(out_w * size_t(height) * 4);
    for (size_t y = 0; y < size_t(height); ++y) {
        const uint8_t* source = rgba + y * src_w * 4;
        float* row = rows.data() + y * out_w * 4;
        for (size_t x = 0; x < out_w; ++x) {
            float* pixel = row + x * 4;
            for (const Weight& weight : across[x]) {
                const uint8_t* texel = source + size_t(weight.source) * 4;
                const float alpha = texel[3] / 255.0f * weight.amount;
                pixel[0] += texel[0] * alpha;
                pixel[1] += texel[1] * alpha;
                pixel[2] += texel[2] * alpha;
                pixel[3] += alpha;
            }
        }
    }

    std::vector<uint8_t> out(out_w * size_t(out_height) * 4);
    std::vector<float> sum(out_w * 4);
    for (size_t y = 0; y < size_t(out_height); ++y) {
        std::fill(sum.begin(), sum.end(), 0.0f);
        for (const Weight& weight : down[y]) {
            const float* row = rows.data() + size_t(weight.source) * out_w * 4;
            for (size_t i = 0; i < sum.size(); ++i) {
                sum[i] += row[i] * weight.amount;
            }
        }
        uint8_t* target = out.data() + y * out_w * 4;
        for (size_t x = 0; x < out_w; ++x) {
            const float* pixel = sum.data() + x * 4;
            const float alpha = std::clamp(pixel[3], 0.0f, 1.0f);
            for (size_t c = 0; c < 3; ++c) {
                target[x * 4 + c] = alpha > 0.0f ? uint8_t(std::clamp(pixel[c] / alpha + 0.5f, 0.0f, 255.0f)) : 0;
            }
            target[x * 4 + 3] = uint8_t(std::min(alpha * alpha_gain, 1.0f) * 255.0f + 0.5f);
        }
    }
    return out;
}

} // namespace dcp
