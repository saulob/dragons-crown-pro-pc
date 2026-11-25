// The Home page's background: assets/launcher/home_background.png cropped to the panel, scaled to the pixels it is
// shown at and darkened. SDL's software renderer, the launcher's on Windows, samples without filtering and drops
// triangles whose texture width times area overflows an int, so the picture is made at its exact size and drawn in strips.
#pragma once

#include <cstdint>
#include <vector>

namespace dcp::home {

inline constexpr float kArtAnchorX = 1.0f;  // the kingdom is on the right: a narrow panel crops the left
inline constexpr float kArtAnchorY = 0.5f;

struct ArtCrop {
    int x = 0, y = 0, width = 0, height = 0;
};

// The largest part of the source with the target's aspect, positioned by the anchors (0 left/top, 1 right/bottom).
ArtCrop CoverCrop(int source_width, int source_height, int target_width, int target_height, float anchor_x = kArtAnchorX,
                  float anchor_y = kArtAnchorY);

// Navy tint over the whole picture, stronger on the left where the text is, lighter on the right so the kingdom shows.
float ShadeAt(float across, float down);

// Opaque RGBA of out_width x out_height: the cover crop, scaled, then shaded. Empty on bad input.
std::vector<uint8_t> RenderHomeBackground(const uint8_t* rgba, int width, int height, int out_width, int out_height);

} // namespace dcp::home
