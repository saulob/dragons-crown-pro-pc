// Ready-made header backgrounds. Each returns a HeaderPainter for
// Branding::background; games with their own art write a painter instead
// (it gets the draw list, the header rectangle, the time and the scale).
#pragma once

#include <cstdint>
#include <vector>

#include "trg/launcher.h"

namespace trg::headers {

// A four-corner gradient.
HeaderPainter Gradient(ImU32 top_left, ImU32 top_right, ImU32 bottom_right, ImU32 bottom_left);

// Twinkling stars over deep space with a planet rising on the right
// (Outpost Kaloki X).
struct SpaceOptions {
  ImVec4 planet = ImVec4(0.37f, 0.68f, 0.24f, 1.0f);  // lit side of the planet
  int stars = 150;
  bool planet_visible = true;
};
HeaderPainter Space(SpaceOptions options = {});

// Blue sky, a turning sun and moving waves (Super Mario Sunshine).
struct SkyOptions {
  ImU32 sky_top = IM_COL32(30, 136, 229, 255);
  ImU32 sky_bottom = IM_COL32(126, 211, 255, 255);
  ImU32 sea_top = IM_COL32(0, 172, 193, 255);
  ImU32 sea_bottom = IM_COL32(0, 96, 160, 255);
  ImU32 sun = IM_COL32(255, 214, 64, 255);
  float sea_level = 0.70f;  // fraction of the header's height
};
HeaderPainter SkyAndSea(SkyOptions options = {});

// A band of an image (a title screen capture, key art), scaled to the
// header's width. `v_offset` skips that fraction of the image's top. Until
// the texture exists, `fallback` is drawn.
HeaderPainter Image(const ImTextureID* texture, const float* aspect, float v_offset = 0.04f,
                    HeaderPainter fallback = {});

// Draws several painters in order.
HeaderPainter Layers(std::vector<HeaderPainter> painters);

// Pieces for writing painters.
void DrawStarfield(const HeaderContext& c, int count, uint32_t seed = 0x5841u);

}  // namespace trg::headers
