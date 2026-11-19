#include "sprite_render.h"

#include <algorithm>
#include <cmath>

namespace dcp {
namespace {

struct Vertex {
    float x, y; // pixels
    float u, v; // texels
    float r, g, b, a; // 0..1
};

Vertex MakeVertex(const MbsPoint& position, const MbsPoint& uv, const MbsColor& color, const SpriteImage& image,
                  const FtxSurface& surface, const MbsTransform& transform) {
    const float sx = position.x * transform.scale[0];
    const float sy = position.y * transform.scale[1];
    const float angle = transform.rotate[2] * 3.14159265358979f / 180.0f;
    const float px = sx * std::cos(angle) - sy * std::sin(angle) + transform.move[0];
    const float py = sx * std::sin(angle) + sy * std::cos(angle) + transform.move[1];
    return {(px - image.left) * image.scale, (py - image.top) * image.scale, uv.x * surface.width(), uv.y * surface.height(),
            color.r / 255.0f * transform.tint.r / 255.0f, color.g / 255.0f * transform.tint.g / 255.0f,
            color.b / 255.0f * transform.tint.b / 255.0f, color.a / 255.0f * transform.tint.a / 255.0f};
}

std::array<float, 4> Bilinear(const FtxSurface& surface, float u, float v, const SpriteRequest& request, uint32_t layer,
                              uint8_t texture) {
    const float fx = u - 0.5f, fy = v - 0.5f;
    const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
    const float tx = fx - float(x0), ty = fy - float(y0);
    std::array<float, 4> out{};
    for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
            std::array<uint8_t, 4> texel = surface.Texel(x0 + dx, y0 + dy);
            if (request.shade) request.shade(layer, texture, x0 + dx, y0 + dy, texel);
            const float w = (dx ? tx : 1.0f - tx) * (dy ? ty : 1.0f - ty);
            const float alpha = texel[3] / 255.0f;
            out[0] += w * texel[0] / 255.0f * alpha; // filtered premultiplied, so transparent texels do not bleed colour
            out[1] += w * texel[1] / 255.0f * alpha;
            out[2] += w * texel[2] / 255.0f * alpha;
            out[3] += w * alpha;
        }
    }
    return out;
}

void DrawTriangle(const Vertex& a, const Vertex& b, const Vertex& c, const FtxSurface& surface, bool additive,
                  uint32_t layer, uint8_t texture, const SpriteRequest& request, std::vector<float>& color, SpriteImage& image) {
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::fabs(area) < 1e-6f) return;
    const int min_x = std::max(0, int(std::floor(std::min({a.x, b.x, c.x}))));
    const int max_x = std::min(image.width - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
    const int min_y = std::max(0, int(std::floor(std::min({a.y, b.y, c.y}))));
    const int max_y = std::min(image.height - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            const float px = float(x) + 0.5f, py = float(y) + 0.5f;
            const float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
            const float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
            const float w2 = 1.0f - w0 - w1;
            // Top-left fill rule approximated by a half-open test, so shared edges are drawn once.
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f || (w0 == 0.0f && w1 == 0.0f)) continue;
            const float u = w0 * a.u + w1 * b.u + w2 * c.u;
            const float v = w0 * a.v + w1 * b.v + w2 * c.v;
            std::array<float, 4> s = Bilinear(surface, u, v, request, layer, texture);
            const float tr = w0 * a.r + w1 * b.r + w2 * c.r, tg = w0 * a.g + w1 * b.g + w2 * c.g;
            const float tb = w0 * a.b + w1 * b.b + w2 * c.b, ta = w0 * a.a + w1 * b.a + w2 * c.a;
            s[0] *= tr * ta;
            s[1] *= tg * ta;
            s[2] *= tb * ta;
            s[3] *= ta;
            if (s[3] <= 0.0f) continue;
            float* dst = color.data() + (size_t(y) * size_t(image.width) + size_t(x)) * 4;
            if (additive) {
                for (int i = 0; i < 3; ++i) dst[i] = std::min(1.0f, dst[i] + s[i]);
            } else {
                for (int i = 0; i < 4; ++i) dst[i] = s[i] + dst[i] * (1.0f - s[3]);
            }
            if (s[3] >= 0.5f) {
                const size_t at = size_t(y) * size_t(image.width) + size_t(x);
                image.layer[at] = layer;
                const int tx = std::clamp(int(u), 0, surface.width() - 1), ty = std::clamp(int(v), 0, surface.height() - 1);
                image.texel[at] = uint32_t(ty) * uint32_t(surface.width()) + uint32_t(tx);
                image.texture[at] = texture;
            }
        }
    }
}

} // namespace

std::vector<uint32_t> FrameLayers(const Mbs& mbs, size_t animation, size_t frame) {
    std::vector<uint32_t> layers;
    if (animation >= mbs.animations.size()) return layers;
    const MbsAnimation& anim = mbs.animations[animation];
    for (size_t t = 0; t < anim.track_count; ++t) {
        const MbsTrack& track = mbs.tracks[anim.first_track + t];
        if (!track.frame_count) continue;
        const MbsFrame& f = mbs.frames[track.first_frame + std::min<size_t>(frame, track.frame_count - 1)];
        const MbsKey& key = mbs.keys[f.key];
        for (uint32_t i = 0; i < key.layer_count; ++i) layers.push_back(key.first_layer + i);
    }
    return layers;
}

SpriteImage RenderSprite(const SpriteRequest& request) {
    SpriteImage image;
    if (!request.mbs || request.animation >= request.mbs->animations.size() || request.scale <= 0.0f) return image;
    const Mbs& mbs = *request.mbs;
    const MbsAnimation& anim = mbs.animations[request.animation];
    std::array<float, 4> box = request.crop;
    if (box[0] == 0.0f && box[1] == 0.0f && box[2] == 0.0f && box[3] == 0.0f) box = anim.bounds;
    image.scale = request.scale;
    image.left = box[0];
    image.top = box[1];
    image.width = std::max(1, int(std::ceil((box[2] - box[0]) * request.scale)));
    image.height = std::max(1, int(std::ceil((box[3] - box[1]) * request.scale)));
    image.layer.assign(size_t(image.width) * size_t(image.height), kNoLayer);
    image.texel.assign(image.layer.size(), 0);
    image.texture.assign(image.layer.size(), 0);
    std::vector<float> color(size_t(image.width) * size_t(image.height) * 4, 0.0f);

    const MbsTransform identity;
    for (size_t t = 0; t < anim.track_count; ++t) {
        const MbsTrack& track = mbs.tracks[anim.first_track + t];
        if (!track.frame_count) continue;
        const MbsFrame& frame = mbs.frames[track.first_frame + std::min<size_t>(request.frame, track.frame_count - 1)];
        const MbsTransform& transform = frame.transform < mbs.transforms.size() ? mbs.transforms[frame.transform] : identity;
        const MbsKey& key = mbs.keys[frame.key];
        for (uint32_t i = 0; i < key.layer_count; ++i) {
            const uint32_t index = key.first_layer + i;
            const MbsLayer& layer = mbs.layers[index];
            const FtxSurface* surface = layer.texture < request.textures.size() ? request.textures[layer.texture] : nullptr;
            if (!surface || (request.visible && !request.visible(index))) continue;
            const MbsQuad& src = mbs.sources[layer.source];
            const MbsQuad& dst = mbs.destinations[layer.destination];
            const MbsColorQuad& tint = mbs.colors[layer.color];
            const Vertex center = MakeVertex(dst.center, src.center, tint.center, image, *surface, transform);
            for (size_t c = 0; c < 4; ++c) {
                const size_t n = (c + 1) % 4;
                const Vertex v0 = MakeVertex(dst.corners[c], src.corners[c], tint.corners[c], image, *surface, transform);
                const Vertex v1 = MakeVertex(dst.corners[n], src.corners[n], tint.corners[n], image, *surface, transform);
                DrawTriangle(center, v0, v1, *surface, layer.blend == 1, index, layer.texture, request, color, image);
            }
        }
    }

    image.rgba.resize(color.size());
    for (size_t p = 0; p < color.size(); p += 4) {
        float a = std::clamp(color[p + 3], 0.0f, 1.0f);
        if (std::lround(a * 255.0f) == 0) a = 0.0f;
        for (int i = 0; i < 3; ++i) {
            const float straight = a > 0.0f ? color[p + i] / a : 0.0f;
            image.rgba[p + i] = uint8_t(std::lround(std::clamp(straight, 0.0f, 1.0f) * 255.0f));
        }
        image.rgba[p + 3] = uint8_t(std::lround(a * 255.0f));
    }
    return image;
}

} // namespace dcp
