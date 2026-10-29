// SPDX-License-Identifier: GPL-2.0-or-later
// The Frame rate setting as presentation timing: which guest flips reach the screen and how often the last frame is
// shown again between two flips. The game's own pacing (a 59.94 Hz tick that also builds the frame) is never changed.
// Kept apart from VideoOutDriver so it is testable without a Vulkan presenter.
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>

namespace DcpFramePacing {

// The values the launcher stores; 0 is Unlimited.
inline constexpr std::array kFrameRateModes{30, 60, 90, 120, 144, 165, 240, 0};

// The ceiling of the repeat cadence when the setting is Unlimited and nothing else bounds it.
inline constexpr int kUnlimitedCadenceHz = 480;
inline constexpr int kGameHz = 60;

constexpr bool IsFrameRateMode(int setting) {
    for (const int mode : kFrameRateModes) {
        if (mode == setting) {
            return true;
        }
    }
    return false;
}

constexpr bool IsUnlimited(int setting) {
    return setting == 0;
}

// The setting a dcp.ini value stands for; anything but a stored value exactly is the original 60, as in the launcher.
inline int ParseFrameRate(const std::string& value) {
    for (const int mode : kFrameRateModes) {
        if (value == std::to_string(mode)) {
            return mode;
        }
    }
    return kGameHz;
}

// The rate the setting asks for; 0 = uncapped.
constexpr int RenderFps(int setting) {
    if (IsUnlimited(setting)) {
        return 0;
    }
    return IsFrameRateMode(setting) ? setting : kGameHz;
}

// Above the game's own 60 Hz: the setting that shows frames again.
constexpr bool IsHighRate(int setting) {
    return IsUnlimited(setting) || RenderFps(setting) > kGameHz;
}

// The display's refresh rounded to the whole rate its modes are named by (59.94 -> 60, 143.856 -> 144).
constexpr int NominalHz(double hz) {
    return static_cast<int>(hz + 0.5);
}

constexpr bool VsyncBoundsRate(bool vsync, double display_hz) {
    return vsync && display_hz > 0.0;
}

// The rate frames can actually reach the screen at; 0 = uncapped. A VSync present queue never goes past the display.
constexpr int EffectiveRenderHz(int setting, bool vsync = false, double display_hz = 0.0) {
    if (!IsHighRate(setting) || !VsyncBoundsRate(vsync, display_hz)) {
        return RenderFps(setting);
    }
    const int display = NominalHz(display_hz);
    return IsUnlimited(setting) ? display : std::min(RenderFps(setting), display);
}

// Presentations per second the host schedules, new frames and repeats together; 0 = only the game's own flips (the
// setting is not above 60, or the display leaves nothing to repeat).
constexpr unsigned RepeatCadenceHz(int setting, bool vsync = false, double display_hz = 0.0) {
    if (!IsHighRate(setting)) {
        return 0u;
    }
    const int hz = EffectiveRenderHz(setting, vsync, display_hz);
    const int cadence = hz == 0 ? kUnlimitedCadenceHz : std::min(hz, kUnlimitedCadenceHz);
    return cadence > kGameHz ? static_cast<unsigned>(cadence) : 0u;
}

// 30 keeps the 60 Hz vblank and presents every second flip; all flips are still consumed.
constexpr bool PresentsFlip(int setting, std::uint64_t flip_index) {
    return RenderFps(setting) != 30 || flip_index % 2 == 0;
}

// Releases one presentation per period on absolute slots of a monotonic clock, so a late wake does not shift
// later slots. Only a presentation more than one period late re-anchors to now + period, so lateness is
// never repaid with a burst.
class FrameLimiter {
public:
    using Clock = std::chrono::steady_clock;

    FrameLimiter(unsigned fps, Clock::time_point start)
        : period_(fps ? std::chrono::nanoseconds(1'000'000'000LL / fps) : std::chrono::nanoseconds(0)),
          next_(start) {}

    bool Enabled() const { return period_.count() != 0; }
    bool Due(Clock::time_point now) const { return !Enabled() || now >= next_; }
    Clock::time_point NextSlot() const { return next_; }
    std::chrono::nanoseconds Period() const { return period_; }

    void Release(Clock::time_point now) {
        next_ = now - next_ > period_ ? now + period_ : next_ + period_;
    }

private:
    std::chrono::nanoseconds period_;
    Clock::time_point next_;
};

inline std::string ModeName(int setting) {
    return setting == 0 ? "Unlimited" : std::to_string(setting);
}

} // namespace DcpFramePacing
