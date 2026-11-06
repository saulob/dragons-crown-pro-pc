// Where assets/launcher/header-art.png goes in the launcher's header, and how it gets there.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dcp {

inline constexpr float kHeaderArtAspect = 3840.0f / 432.0f;
inline constexpr float kHeaderArtInkStart = 2312.0f / 3840.0f;  // the image is empty left of its first stroke
inline constexpr int kHeaderArtStripWidth = 128;
inline constexpr float kHeaderArtScale = 1.15f;
inline constexpr float kHeaderArtTopCrop = 0.12f;      // the image's top band is nearly empty
inline constexpr float kHeaderArtAlphaGain = 2.0f;
inline constexpr float kHeaderFade = 0.35f;            // of the header, over its gradient

struct HeaderArtRect {
    float min_x, min_y, max_x, max_y;
};

struct HeaderArtStrip {
    float min_x, max_x, min_u, max_u;
};

// Right edge of the window, whole pixels, at its own size. `bottom` is where the art is cut, and it is usually taller.
HeaderArtRect PlaceHeaderArt(float top, float right, float bottom, float viewport_height, float scale, float aspect);

// SDL's software renderer, the launcher's on Windows, drops triangles whose texture width times area overflows an int.
std::vector<HeaderArtStrip> SplitHeaderArt(const HeaderArtRect& rect);

// Area-averaged RGBA at the size it is shown: that renderer samples without filtering. Empty on bad input.
std::vector<uint8_t> ScaleArtRgba(const uint8_t* rgba, int width, int height, int out_width, int out_height,
                                  float alpha_gain = 1.0f);

} // namespace dcp
