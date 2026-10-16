// Windowed-mode sizing: pure arithmetic, no SDL.
#pragma once

#include <algorithm>
#include <cmath>

namespace dcp {

// A decorated window stays within this share of the display's usable area.
inline constexpr double kWindowAreaShare = 0.8;

struct WindowBorders {
    int top = 0, left = 0, bottom = 0, right = 0;
};

struct WindowFit {
    int width = 0, height = 0; // client size
    int x = 0, y = 0;          // client origin that centres the decorated window in the area
};

// Shrinks the client size by one common factor so the decorated window fits the share of the area, never enlarges.
inline WindowFit FitWindowToArea(int width, int height, WindowBorders borders, int area_x, int area_y, int area_w,
                                 int area_h, double share = kWindowAreaShare) {
    const int extra_w = borders.left + borders.right;
    const int extra_h = borders.top + borders.bottom;
    double scale = 1.0;
    if (width > 0 && height > 0 && area_w > 0 && area_h > 0) {
        const double room_w = std::max(1.0, area_w * share - extra_w);
        const double room_h = std::max(1.0, area_h * share - extra_h);
        scale = std::min({1.0, room_w / width, room_h / height});
    }
    WindowFit fit;
    fit.width = std::max(1, static_cast<int>(std::lround(width * scale)));
    fit.height = std::max(1, static_cast<int>(std::lround(height * scale)));
    fit.x = area_x + (area_w - (fit.width + extra_w)) / 2 + borders.left;
    fit.y = area_y + (area_h - (fit.height + extra_h)) / 2 + borders.top;
    return fit;
}

} // namespace dcp
