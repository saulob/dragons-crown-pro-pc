// CPU rasterizer for one FMBS animation frame: every layer of every track, in model order, each quad as four textured
// triangles around its centre, tinted by its vertex colours. Alongside the RGBA image it keeps, per pixel, the layer that
// covers it last, so a click or a recolour can be traced back to the model.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "game_mbs.h"
#include "game_texture.h"

namespace dcp {

inline constexpr uint32_t kNoLayer = 0xFFFFFFFFu;

struct SpriteImage {
    int width = 0;
    int height = 0;
    float scale = 1.0f;          // pixels per model unit
    float left = 0.0f, top = 0.0f; // model coordinates of pixel (0, 0)
    std::vector<uint8_t> rgba;   // straight alpha
    std::vector<uint32_t> layer; // per pixel: index into Mbs::layers, or kNoLayer
    std::vector<uint32_t> texel; // per pixel: the texel (y * width + x) the top layer samples there
    std::vector<uint8_t> texture; // and the texture id it samples
};

struct SpriteRequest {
    const Mbs* mbs = nullptr;
    size_t animation = 0;
    size_t frame = 0; // index within each track, clamped to the track length
    float scale = 1.0f;
    // The surface each MBS texture id samples; a layer whose surface is missing is skipped.
    std::array<const FtxSurface*, 16> textures{};
    // Optional crop in model units (left, top, right, bottom); the animation bounds when all zero.
    std::array<float, 4> crop{};
    // Optional per-layer filter (false hides the layer) and texel shader (recolours a sampled texel in place).
    std::function<bool(uint32_t layer)> visible;
    std::function<void(uint32_t layer, uint8_t texture, int x, int y, std::array<uint8_t, 4>& texel)> shade;
};

// Layers drawn by the request's frame, in draw order.
std::vector<uint32_t> FrameLayers(const Mbs& mbs, size_t animation, size_t frame);

SpriteImage RenderSprite(const SpriteRequest& request);

} // namespace dcp
