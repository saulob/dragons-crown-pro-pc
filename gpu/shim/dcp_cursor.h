// SPDX-License-Identifier: GPL-2.0-or-later
// Cursor auto-hide timer for the game window: hidden after kHideDelay without mouse motion while `active`.
#pragma once

#include <chrono>
#include <optional>

namespace DcpCursor {

inline constexpr std::chrono::milliseconds kHideDelay{5000};

class Hider {
public:
    using Clock = std::chrono::steady_clock;

    void MouseMoved(Clock::time_point now) { last_motion = now; }

    // The new visibility when it changes, otherwise nullopt.
    std::optional<bool> Update(bool active, Clock::time_point now) {
        if (active && !was_active) {
            last_motion = now;
        }
        was_active = active;
        const bool wanted = !active || now - last_motion < kHideDelay;
        if (wanted == visible) {
            return std::nullopt;
        }
        visible = wanted;
        return visible;
    }

private:
    Clock::time_point last_motion{};
    bool was_active = false;
    bool visible = true;
};

} // namespace DcpCursor
