#include "trg/headers.h"

#include <cmath>

namespace trg::headers {

HeaderPainter Gradient(ImU32 top_left, ImU32 top_right, ImU32 bottom_right, ImU32 bottom_left) {
  return [=](const HeaderContext& c) {
    c.draw->AddRectFilledMultiColor(c.min, c.max, top_left, top_right, bottom_right, bottom_left);
  };
}

void DrawStarfield(const HeaderContext& c, int count, uint32_t seed) {
  auto rnd = [&seed] {
    seed = seed * 1664525u + 1013904223u;
    return float(seed >> 8) / float(1 << 24);
  };
  for (int i = 0; i < count; ++i) {
    const ImVec2 p(c.min.x + rnd() * (c.max.x - c.min.x), c.min.y + rnd() * (c.max.y - c.min.y));
    const float size = (0.6f + rnd() * 1.4f) * c.scale;
    const float speed = 0.6f + rnd() * 1.8f, phase = rnd() * 6.28f;
    const float twinkle = 0.55f + 0.45f * std::sin(c.time * speed + phase);
    c.draw->AddCircleFilled(p, size, IM_COL32(255, 255, 255, int(200 * twinkle)));
  }
}

HeaderPainter Space(SpaceOptions o) {
  return [o](const HeaderContext& c) {
    ImDrawList* dl = c.draw;
    const float w = c.max.x - c.min.x, h = c.max.y - c.min.y;
    dl->AddRectFilledMultiColor(c.min, c.max, IM_COL32(9, 16, 40, 255), IM_COL32(4, 7, 20, 255),
                                IM_COL32(6, 22, 24, 255), IM_COL32(10, 14, 34, 255));
    DrawStarfield(c, o.stars);
    if (!o.planet_visible) return;
    const ImVec2 centre(c.min.x + w - h * 0.95f, c.min.y + h * 1.18f);
    const float r = h * 1.05f;
    const ImVec4 lit = o.planet;
    const ImVec4 dark(lit.x * 0.32f, lit.y * 0.44f, lit.z * 0.5f, 1.0f);
    dl->AddCircleFilled(centre, r * 1.07f, Col(lit, 0.10f), 96);
    dl->AddCircleFilled(centre, r * 1.03f, Col(lit, 0.16f), 96);
    for (int i = 0; i < 10; ++i) {
      const float k = float(i) / 9.0f;
      const ImVec4 col(dark.x + (lit.x - dark.x) * k, dark.y + (lit.y - dark.y) * k, dark.z + (lit.z - dark.z) * k, 1);
      dl->AddCircleFilled(ImVec2(centre.x - r * 0.18f * k, centre.y - r * 0.2f * k), r * (1.0f - 0.55f * k), Col(col),
                          96);
    }
  };
}

HeaderPainter SkyAndSea(SkyOptions o) {
  return [o](const HeaderContext& c) {
    ImDrawList* dl = c.draw;
    const float h = c.max.y - c.min.y, w = c.max.x - c.min.x;
    const float sea_y = c.min.y + h * o.sea_level;
    dl->AddRectFilledMultiColor(c.min, ImVec2(c.max.x, sea_y), o.sky_top, o.sky_top, o.sky_bottom, o.sky_bottom);
    // The sun, with slowly turning rays.
    const ImVec2 sun(c.max.x - w * 0.11f, c.min.y + h * 0.40f);
    const float r = h * 0.22f;
    const ImU32 glow = (o.sun & ~IM_COL32_A_MASK) | (60u << IM_COL32_A_SHIFT);
    for (int i = 0; i < 12; ++i) {
      const float a = c.time * 0.15f + float(i) * 3.14159265f / 6.0f;
      dl->AddTriangleFilled(ImVec2(sun.x + std::cos(a - 0.10f) * r * 1.25f, sun.y + std::sin(a - 0.10f) * r * 1.25f),
                            ImVec2(sun.x + std::cos(a) * r * 2.0f, sun.y + std::sin(a) * r * 2.0f),
                            ImVec2(sun.x + std::cos(a + 0.10f) * r * 1.25f, sun.y + std::sin(a + 0.10f) * r * 1.25f),
                            glow);
    }
    dl->AddCircleFilled(sun, r * 1.18f, glow, 48);
    dl->AddCircleFilled(sun, r, o.sun, 48);
    // The sea, with moving waves.
    dl->AddRectFilledMultiColor(ImVec2(c.min.x, sea_y), c.max, o.sea_top, o.sea_top, o.sea_bottom, o.sea_bottom);
    for (int band = 0; band < 3; ++band) {
      const float y = sea_y + float(band) * h * 0.09f + h * 0.03f;
      const float amp = h * 0.012f * float(band + 1);
      ImVec2 pts[64];
      for (int i = 0; i < 64; ++i) {
        const float x = c.min.x + w * float(i) / 63.0f;
        pts[i] = ImVec2(x, y + std::sin(x * 0.02f / c.scale + c.time * (1.2f + float(band) * 0.4f) + float(band)) * amp);
      }
      dl->AddPolyline(pts, 64, IM_COL32(255, 255, 255, 70 - band * 18), 0, 2.0f * c.scale);
    }
  };
}

HeaderPainter Image(const ImTextureID* texture, const float* aspect, float v_offset, HeaderPainter fallback) {
  return [=](const HeaderContext& c) {
    if (!texture || *texture == ImTextureID{} || !aspect || *aspect <= 0.0f) {
      if (fallback) fallback(c);
      return;
    }
    const float w = c.max.x - c.min.x, h = c.max.y - c.min.y;
    const float band = std::fmin(1.0f - v_offset, (h / w) * *aspect);
    c.draw->AddImage(ImTextureRef(*texture), c.min, c.max, ImVec2(0, v_offset), ImVec2(1, v_offset + band));
  };
}

HeaderPainter Layers(std::vector<HeaderPainter> painters) {
  return [painters = std::move(painters)](const HeaderContext& c) {
    for (const HeaderPainter& p : painters)
      if (p) p(c);
  };
}

}  // namespace trg::headers
