// Vanillaware FMBS sprite models (version 0x72, little-endian on PS4): vertex colours, source and destination quads,
// layers, keys, frames, tracks and animations. Section strides and count fields are the game's own layout.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dcp {

struct MbsColor {
    uint8_t r = 0, g = 0, b = 0, a = 0;
    bool operator==(const MbsColor&) const = default;
};

struct MbsPoint {
    float x = 0.0f, y = 0.0f;
};

// A quad is drawn as four triangles around its centre.
struct MbsQuad {
    MbsPoint center;
    std::array<MbsPoint, 4> corners;
};

struct MbsColorQuad {
    MbsColor center;
    std::array<MbsColor, 4> corners;
};

struct MbsLayer {
    uint32_t id = 0;
    uint8_t flags = 0;
    uint8_t blend = 0;
    uint8_t texture = 0;
    uint16_t attributes = 0;
    uint16_t source = 0;      // index into source quads (texture coordinates, 0..1)
    uint16_t color = 0;       // index into colour quads
    uint16_t destination = 0; // index into destination quads (model units)
};

struct MbsKey {
    std::array<float, 4> bounds{}; // left, top, right, bottom
    uint32_t first_layer = 0;
    uint16_t layer_count = 0;
};

struct MbsTransform {
    std::array<float, 3> move{};
    std::array<float, 3> rotate{};
    std::array<float, 2> scale{1.0f, 1.0f};
    MbsColor tint{255, 255, 255, 255};
};

struct MbsFrame {
    uint16_t key = 0;
    uint16_t transform = 0;
    uint16_t duration = 0;
};

struct MbsTrack {
    uint16_t first_frame = 0;
    uint16_t frame_count = 0;
};

struct MbsAnimation {
    std::array<float, 4> bounds{};
    std::string name;
    uint16_t first_track = 0;
    uint8_t track_count = 0;
};

struct Mbs {
    std::string name;
    std::vector<MbsColorQuad> colors;
    std::vector<MbsQuad> sources;
    std::vector<MbsQuad> destinations;
    std::vector<MbsLayer> layers;
    std::vector<MbsKey> keys;
    std::vector<MbsTransform> transforms;
    std::vector<MbsFrame> frames;
    std::vector<MbsAnimation> animations;
    std::vector<MbsTrack> tracks;
};

// Every cross-reference (frame -> key, key -> layers, layer -> quads, animation -> tracks -> frames) is bounds-checked, so
// a parsed model can be walked without further checks.
std::optional<Mbs> ParseMbs(std::span<const uint8_t> bytes, std::string* error = nullptr);

std::optional<Mbs> LoadGameMbs(const std::filesystem::path& cpk, std::string_view entry, std::string* error = nullptr);

} // namespace dcp
