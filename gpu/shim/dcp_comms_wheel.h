// SPDX-License-Identifier: GPL-2.0-or-later
// The Comms Wheel's input and layout. Driver is stepped by the game thread's pad read of player 1 (the only place that
// sees the pad before the game does); the window thread feeds it the mouse; the present thread draws what it published.
// No SDL or ImGui types here, so the logic is testable on its own.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>

#include "dcp_comms.h"

union SDL_Event;

namespace DcpCommsWheel {

// Mouse motion (window units) from the point the wheel opened that reaches the outer sector ring.
inline constexpr float kMouseRangeUnits = 80.0f;
// A step later than this after the previous one means the game stopped reading the pad (a menu, a load).
inline constexpr uint64_t kStaleMs = 500;
inline constexpr uint64_t kVisibleMs = 250;

struct Inputs {
    uint64_t now_ms = 0;
    bool available = false;
    bool pad_up = false;
    float stick_x = 0.0f, stick_y = 0.0f; // right stick, down positive
    bool confirm = false;                 // Cross held, from the pad or its key
    bool circle = false;
    bool key_down = false;
    uint8_t nav = 0;                      // dcp::comms::kNav* bits of the direction keys held
    bool click = false, cancel_click = false; // left mouse / Escape or right mouse pressed since the last step
    float mouse_x = 0.0f, mouse_y = 0.0f; // accumulated mouse motion over kMouseRangeUnits, length <= 1
};

struct Outputs {
    bool hide_pad_up = false;
    bool hide_confirm = false;
    bool hide_circle = false;
    bool neutral_right_stick = false;
    bool neutral_keyboard_axes = false;
    int send = -1;
};

struct View {
    bool open = false;
    int selected = -1;
    dcp::comms::Source source = dcp::comms::Source::kNone;
    uint64_t stamp_ms = 0;
};

inline float Clamp01Length(float& x, float& y) {
    const float length = std::hypot(x, y);
    if (length > 1.0f) {
        x /= length;
        y /= length;
    }
    return std::min(length, 1.0f);
}

class Driver {
public:
    Outputs Step(const Inputs& in) {
        std::scoped_lock lock{mutex_};
        if (controller_.Active() && in.now_ms - last_step_ms_ > kStaleMs) {
            controller_.Reset();
        }
        last_step_ms_ = in.now_ms;

        dcp::comms::WheelInput wheel;
        wheel.available = in.available;
        wheel.pad_up = in.pad_up;
        wheel.key_down = in.key_down;
        wheel.stick_x = in.stick_x;
        wheel.stick_y = in.stick_y;
        wheel.mouse_x = in.mouse_x;
        wheel.mouse_y = in.mouse_y;
        wheel.nav = in.nav;
        wheel.confirm_held = in.confirm;
        wheel.cancel_held = in.circle;
        wheel.click = in.click;
        wheel.cancel_click = in.cancel_click;
        const dcp::comms::WheelOutput out = controller_.Step(wheel);

        Outputs result;
        result.send = out.send;
        result.hide_pad_up = out.hide_pad_up;
        result.hide_confirm = out.hide_confirm;
        result.hide_circle = out.hide_cancel;
        result.neutral_right_stick = out.hide_right_stick;
        result.neutral_keyboard_axes = out.hide_keyboard_axes;

        view_ = View{out.open, out.selected, out.source, in.now_ms};
        engaged_ = out.open && out.source == dcp::comms::Source::kKeyboard;
        return result;
    }

    View Latest() const {
        std::scoped_lock lock{mutex_};
        return view_;
    }

    // A keyboard session is open: the mouse and Escape belong to the wheel.
    bool KeyboardEngaged() const { return engaged_.load(std::memory_order_relaxed); }

    void Reset() {
        std::scoped_lock lock{mutex_};
        controller_.Reset();
        view_ = View{};
        engaged_ = false;
    }

private:
    mutable std::mutex mutex_;
    dcp::comms::WheelController controller_{dcp::comms::kStickDeadZone};
    View view_;
    uint64_t last_step_ms_ = 0;
    std::atomic<bool> engaged_{false};
};

// Mouse motion kept by the window thread while a keyboard session is open; the game thread clears it between sessions.
class MouseAccumulator {
public:
    void Add(float dx, float dy) {
        std::scoped_lock lock{mutex_};
        x_ += dx / kMouseRangeUnits;
        y_ += dy / kMouseRangeUnits;
        Clamp01Length(x_, y_);
    }
    void Clear() {
        std::scoped_lock lock{mutex_};
        x_ = y_ = 0.0f;
    }
    void Read(float& x, float& y) const {
        std::scoped_lock lock{mutex_};
        x = x_;
        y = y_;
    }

private:
    mutable std::mutex mutex_;
    float x_ = 0.0f, y_ = 0.0f;
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool Overlaps(const Rect& o) const { return x < o.x + o.w && o.x < x + w && y < o.y + o.h && o.y < y + h; }
    bool Inside(float cx, float cy, float radius) const {
        const float xs[2] = {x, x + w}, ys[2] = {y, y + h};
        for (float px : xs) {
            for (float py : ys) {
                if (std::hypot(px - cx, py - cy) > radius) {
                    return false;
                }
            }
        }
        return true;
    }
};

// Pixels of text of `font_px` for a UTF-8 string.
using Measure = std::function<float(const char* utf8, float font_px)>;

struct SectorLayout {
    Rect icon;
    Rect label;
    float font_px = 0.0f;
};

struct Layout {
    float center_x = 0, center_y = 0;
    float outer_radius = 0, inner_radius = 0;
    std::array<SectorLayout, dcp::comms::kSectors> sectors;
};

// `scale`: 1 at 1080 lines of display height, the overlay's own UI scale. Labels shrink to fit their sector and never
// below 80% of the base size.
inline Layout ComputeLayout(float center_x, float center_y, float scale, const std::array<const char*, dcp::comms::kSectors>& labels,
                            const Measure& measure) {
    constexpr float kPi = 3.14159265358979f;
    Layout layout;
    layout.center_x = center_x;
    layout.center_y = center_y;
    layout.outer_radius = 300.0f * scale;
    layout.inner_radius = 82.0f * scale;
    const float content_radius = 196.0f * scale;
    const float icon_px = 60.0f * scale;
    const float base_font = 22.0f * scale;
    const float label_gap = 8.0f * scale;
    const float max_width = 150.0f * scale;
    for (int i = 0; i < dcp::comms::kSectors; ++i) {
        const float angle = static_cast<float>(i) * kPi / 4.0f;
        const float ax = center_x + std::sin(angle) * content_radius;
        const float ay = center_y - std::cos(angle) * content_radius;
        SectorLayout& s = layout.sectors[static_cast<size_t>(i)];
        float font = base_font;
        float width = measure(labels[static_cast<size_t>(i)], font);
        if (width > max_width) {
            font = std::max(base_font * 0.8f, base_font * max_width / width);
            width = measure(labels[static_cast<size_t>(i)], font);
        }
        const float block = icon_px + label_gap + font * 1.25f;
        const float top = ay - block * 0.5f;
        s.font_px = font;
        s.icon = {ax - icon_px * 0.5f, top, icon_px, icon_px};
        s.label = {ax - width * 0.5f, top + icon_px + label_gap, width, font * 1.25f};
    }
    return layout;
}

// A label the overlay font can draw: Latin, Greek and Cyrillic. Anything else is shown in English.
inline bool DrawableByOverlayFont(const char* utf8) {
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8); *p;) {
        if (*p >= 0xe0) {
            return false;
        }
        const bool two = *p >= 0xc0;
        const unsigned codepoint = two ? ((*p & 0x1fu) << 6) | (p[1] & 0x3fu) : *p;
        if (codepoint >= 0x530) {
            return false;
        }
        p += two ? 2 : 1;
    }
    return true;
}

// Window thread, from DcpOverlay::HandleEvent: true when the event belongs to the wheel.
bool HandleEvent(const SDL_Event& event);

// Present thread, with the overlay's ImGui lock held: the icon texture before NewFrame, the wheel inside the frame.
void PrepareTextures();
void Draw(float display_width, float display_height, float ui_scale);
bool Visible();

} // namespace DcpCommsWheel
