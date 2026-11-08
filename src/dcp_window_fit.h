// Windowed-mode sizing: pure arithmetic, no SDL.
#pragma once

#include <algorithm>
#include <cmath>

namespace dcp {

// Room the title bar takes at 100% scaling, used when the window system cannot report real borders.
inline constexpr int kEstimatedTitleBarHeight = 40;

struct WindowBorders {
    int top = 0, left = 0, bottom = 0, right = 0;
};

struct WindowFit {
    int width = 0, height = 0; // client size
    int x = 0, y = 0;          // client origin that centres the decorated window in the area
};

inline WindowBorders EstimatedBorders(float display_scale) {
    const float scale = display_scale > 0.0f ? display_scale : 1.0f;
    return {static_cast<int>(kEstimatedTitleBarHeight * scale + 0.5f), 0, 0, 0};
}

// Keeps the client size when the decorated window fits the area; otherwise shrinks it by one common factor. Only the
// vertical borders limit the fit: Windows reports invisible side frames that may overhang the screen edge.
inline WindowFit FitWindowToArea(int width, int height, WindowBorders borders, int area_x, int area_y, int area_w,
                                 int area_h) {
    const int extra_w = borders.left + borders.right;
    const int extra_h = borders.top + borders.bottom;
    double scale = 1.0;
    if (width > 0 && height > 0 && area_w > 0 && area_h > 0) {
        const double room_h = std::max(1.0, static_cast<double>(area_h - extra_h));
        scale = std::min({1.0, static_cast<double>(area_w) / width, room_h / height});
    }
    WindowFit fit;
    fit.width = std::max(1, scale < 1.0 ? static_cast<int>(std::lround(width * scale)) : width);
    fit.height = std::max(1, scale < 1.0 ? static_cast<int>(std::lround(height * scale)) : height);
    fit.x = area_x + (area_w - (fit.width + extra_w)) / 2 + borders.left;
    fit.y = area_y + (area_h - (fit.height + extra_h)) / 2 + borders.top;
    return fit;
}

} // namespace dcp
