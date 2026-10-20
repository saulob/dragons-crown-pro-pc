// SPDX-License-Identifier: GPL-2.0-or-later
// Cursor visibility from the last meaningful gameplay input device.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <optional>

namespace DcpCursor {

enum class Device { Mouse, Keyboard, Controller };

class State {
public:
    // gameplay: the game window is accepting input (false in the launcher).
    // overlay: a mouse-driven UI is open and needs the cursor.
    // Returns the new visibility when it changes, otherwise nullopt.
    std::optional<bool> Update(bool gameplay, bool focused, bool overlay) {
        if (!gameplay) {
            device_ = Device::Mouse;
            device_before_overlay_ = Device::Mouse;
            overlay_ = false;
        } else if (overlay && !overlay_) {
            device_before_overlay_ = device_;
            overlay_ = true;
        } else if (!overlay && overlay_) {
            device_ = device_before_overlay_;
            overlay_ = false;
        }
        const bool visible = !gameplay || !focused || overlay || device_ == Device::Mouse;
        if (visible == visible_) {
            return std::nullopt;
        }
        visible_ = visible;
        return visible;
    }

    void MouseMoved() { device_ = Device::Mouse; }
    void MouseButtonPressed() { device_ = Device::Mouse; }
    void MouseWheelMoved() { device_ = Device::Mouse; }
    void KeyboardActivity() { device_ = Device::Keyboard; }
    void ControllerActivity() { device_ = Device::Controller; }

    Device LastDevice() const { return device_; }
    bool Visible() const { return visible_; }

private:
    Device device_ = Device::Mouse;
    Device device_before_overlay_ = Device::Mouse;
    bool overlay_ = false;
    bool visible_ = true;
};

// A key press that counts as gameplay input: not a repeat, and not a host shortcut.
constexpr bool IsGameplayKey(bool repeat, bool host_shortcut) {
    return !repeat && !host_shortcut;
}

// Stick threshold in the gamepad's raw -32768..32767 units.
inline constexpr int kStickDeadzone = 7849;
// Trigger threshold in the runtime's 0..255 trigger units.
inline constexpr int kTriggerThreshold = 30;

struct ControllerState {
    std::uint32_t buttons = 0;
    int left_x = 0, left_y = 0, right_x = 0, right_y = 0;
    int left_trigger = 0, right_trigger = 0;
};

// Reports only the start of meaningful controller input, not a held button,
// stick, or trigger, so mouse movement is not overridden by input already down.
class ControllerActivityFilter {
public:
    bool Update(const ControllerState& state) {
        bool started = (state.buttons & ~buttons_) != 0;
        buttons_ = state.buttons;
        started |= StickCrossed(state.left_x, left_x_);
        started |= StickCrossed(state.left_y, left_y_);
        started |= StickCrossed(state.right_x, right_x_);
        started |= StickCrossed(state.right_y, right_y_);
        started |= TriggerCrossed(state.left_trigger, left_trigger_);
        started |= TriggerCrossed(state.right_trigger, right_trigger_);
        return started;
    }

private:
    static bool StickCrossed(int value, bool& was_active) {
        const bool active = std::abs(value) >= kStickDeadzone;
        const bool started = active && !was_active;
        was_active = active;
        return started;
    }

    static bool TriggerCrossed(int value, bool& was_active) {
        const bool active = value / 128 >= kTriggerThreshold; // raw 0..32767 to 0..255
        const bool started = active && !was_active;
        was_active = active;
        return started;
    }

    std::uint32_t buttons_ = 0;
    bool left_x_ = false, left_y_ = false, right_x_ = false, right_y_ = false;
    bool left_trigger_ = false, right_trigger_ = false;
};

} // namespace DcpCursor
