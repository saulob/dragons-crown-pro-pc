// The frame counter's panel, shared by the notices so both look the same. Corners are DcpSettings::overlay_corner
// values: 0 top left, 1 top right, 2 bottom left, 3 bottom right.
#pragma once

namespace DcpOverlayBox {

struct Rect {
    float x0, y0, x1, y1;
};

struct Box {
    Rect rect;
    float pad_x, pad_y, rounding;
};

inline constexpr float kBorderThickness = 1.0f;

// `size` is the text size; the panel sits one text size in from the corner of `work`.
constexpr Box Place(int corner, float text_width, float text_height, float size, Rect work) {
    const float pad_x = size * 0.6f, pad_y = size * 0.3f;
    const float width = text_width + pad_x * 2, height = text_height + pad_y * 2;
    const bool right = corner == 1 || corner == 3, bottom = corner >= 2;
    const float x0 = right ? work.x1 - size - width : work.x0 + size;
    const float y0 = bottom ? work.y1 - size - height : work.y0 + size;
    return {{x0, y0, x0 + width, y0 + height}, pad_x, pad_y, size * 0.5f};
}

// A notice takes the top corner the frame counter leaves free.
constexpr int NoticeCorner(int fps_corner) {
    return fps_corner == 0 ? 1 : 0;
}

constexpr bool Overlap(Rect a, Rect b) {
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

} // namespace DcpOverlayBox
