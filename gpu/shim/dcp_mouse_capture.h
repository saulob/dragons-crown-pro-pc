// SPDX-License-Identifier: GPL-2.0-or-later
// When the game window holds the mouse for Pointer Control. Window thread only; the SDL calls stay in window.cpp.
#pragma once

#include <optional>

namespace DcpMouseCapture {

struct Frame {
    bool pointer_active; // the mouse input wants the pointer captured: Pointer Control is held (Hold) or on (Always On)
    bool focused;        // the game window has input focus
    bool overlay;        // Options or Cheats menu, or a game dialog, owns the mouse
    bool text_entry;     // the game's text dialog is taking keys
};

// Mouse input reaches the game only while the window is focused and nothing else needs the mouse.
constexpr bool Blocked(const Frame& frame) {
    return !frame.focused || frame.overlay || frame.text_entry;
}

class Tracker {
public:
    // Returns the relative mouse mode to apply when it changes, otherwise nullopt.
    std::optional<bool> Update(const Frame& frame) {
        return Set(frame.pointer_active && !Blocked(frame));
    }

    // Leaves capture at once: window mode switch, closing the game.
    std::optional<bool> Release() { return Set(false); }

    bool Captured() const { return captured_; }

private:
    std::optional<bool> Set(bool captured) {
        if (captured == captured_) {
            return std::nullopt;
        }
        captured_ = captured;
        return captured;
    }

    bool captured_ = false;
};

} // namespace DcpMouseCapture
