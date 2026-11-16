// SPDX-License-Identifier: GPL-2.0-or-later
// Swapchain and flip pacing choices that need no Vulkan device (tests/present_tests.cpp).
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace DcpPresent {

#ifdef _WIN32
inline constexpr bool kWindows = true;
#else
inline constexpr bool kWindows = false;
#endif

/// Values of VkPresentModeKHR.
enum class Mode : int { Immediate = 0, Mailbox = 1, Fifo = 2 };

/// VSync on is Fifo (no tearing, waits for the display); off is Mailbox when the surface has it, else Immediate
/// (tearing allowed), else Fifo.
inline Mode ChooseMode(bool vsync, bool has_mailbox, bool has_immediate) {
    if (vsync) {
        return Mode::Fifo;
    }
    if (has_mailbox) {
        return Mode::Mailbox;
    }
    return has_immediate ? Mode::Immediate : Mode::Fifo;
}

/// Gamescope (Steam Deck Gaming Mode) sets GAMESCOPE_WAYLAND_DISPLAY for the processes it runs.
inline bool IsGamescope(const char* gamescope_wayland_display, bool windows) {
    return !windows && gamescope_wayland_display && *gamescope_wayland_display;
}

/// This process runs under Gamescope: the only switch for the low-latency swapchain and flip pacing.
inline bool GamescopeSession() {
    static const bool session = IsGamescope(std::getenv("GAMESCOPE_WAYLAND_DISPLAY"), kWindows);
    return session;
}

/// One image beyond the surface minimum to work on. Gamescope queues frames in its compositor and already reports a
/// 3-image minimum, so there the extra image only adds a frame of latency; never a single image.
inline std::uint32_t ImageCount(std::uint32_t surface_min, std::uint32_t surface_max, bool gamescope) {
    std::uint32_t count = gamescope ? std::max(surface_min, 2u) : surface_min + 1;
    if (surface_max > 0) {
        count = std::min(count, surface_max);
    }
    return count;
}

/// A swapchain acquire or present result as the frame sees it. OutOfDate also covers a lost surface.
enum class SwapResult { Success, Suboptimal, OutOfDate, Failure };

struct SwapStep {
    bool usable;   ///< acquire: the image can be drawn and presented; present: the image was queued for display
    bool recreate; ///< the swapchain is to be recreated before the next acquire
};

/// Suboptimal is a Vulkan success code: the image was acquired or presented and the frame goes on. It asks for one
/// recreation; a replacement that is suboptimal again is kept until a clean frame, or a compositor that always reports
/// it (Gamescope) would get a new swapchain every frame.
class SwapchainHealth {
public:
    SwapStep Acquire(SwapResult result) {
        clean_acquire = result == SwapResult::Success;
        switch (result) {
        case SwapResult::Success:
            return {true, recreate};
        case SwapResult::Suboptimal:
            RequestForSuboptimal();
            return {true, recreate};
        case SwapResult::OutOfDate:
            RequestRequired();
            return {false, true};
        case SwapResult::Failure:
            break;
        }
        return {false, recreate};
    }

    SwapStep Present(SwapResult result) {
        switch (result) {
        case SwapResult::Success:
            if (clean_acquire) {
                replaced_suboptimal = false;
            }
            return {true, recreate};
        case SwapResult::Suboptimal:
            RequestForSuboptimal();
            return {true, recreate};
        case SwapResult::OutOfDate:
            RequestRequired();
            return {false, true};
        case SwapResult::Failure:
            break;
        }
        return {false, recreate};
    }

    void Recreated() {
        replaced_suboptimal = recreate && suboptimal_only;
        recreate = false;
        suboptimal_only = false;
        clean_acquire = false;
    }

    bool NeedsRecreation() const {
        return recreate;
    }

private:
    void RequestForSuboptimal() {
        if (!replaced_suboptimal && !recreate) {
            recreate = true;
            suboptimal_only = true;
        }
    }

    void RequestRequired() {
        recreate = true;
        suboptimal_only = false;
    }

    bool recreate = false;
    bool suboptimal_only = false;     ///< only a suboptimal result asked for the pending recreation
    bool replaced_suboptimal = false; ///< this swapchain replaced a suboptimal one and stays even if suboptimal
    bool clean_acquire = false;
};

struct Size {
    std::uint32_t width;
    std::uint32_t height;
    bool operator==(const Size&) const = default;
};

struct Region {
    std::int32_t x;
    std::int32_t y;
    std::uint32_t width;
    std::uint32_t height;
    bool operator==(const Region&) const = default;
};

/// The game frame is sized from the swapchain extent, never from the window: the surface decides (Gamescope forces its
/// own), and the frame keeps the VideoOut image's aspect ratio inside it.
inline Size GameFrameSize(Size surface, float image_ratio) {
    const float ratio = static_cast<float>(surface.width) / static_cast<float>(surface.height);
    Size frame = surface;
    if (ratio > image_ratio) {
        frame.width = static_cast<std::uint32_t>(surface.height * image_ratio);
    } else {
        frame.height = static_cast<std::uint32_t>(surface.width / image_ratio);
    }
    return frame;
}

/// Where a frame lands inside the swapchain image, letterboxed and centred.
inline Region FitRegion(Size frame, Size surface) {
    const float frame_aspect = static_cast<float>(frame.width) / static_cast<float>(frame.height);
    const float surface_aspect = static_cast<float>(surface.width) / static_cast<float>(surface.height);
    Size dst = surface;
    if (frame_aspect > surface_aspect) {
        dst.height = static_cast<std::uint32_t>(static_cast<std::int32_t>(surface.width / frame_aspect));
    } else {
        dst.width = static_cast<std::uint32_t>(static_cast<std::int32_t>(surface.height * frame_aspect));
    }
    return {static_cast<std::int32_t>(surface.width - dst.width) / 2,
            static_cast<std::int32_t>(surface.height - dst.height) / 2, dst.width, dst.height};
}

struct PresentGeometry {
    Size source;
    Size frame;
    Size swapchain;
    Size window;
    Region destination;
    bool stretch;
    bool operator==(const PresentGeometry&) const = default;
};

/// Present 1, 10, 100 and 1000, then only a changed geometry (a few times at most).
class GeometryReporter {
public:
    bool Observe(std::uint64_t present, const PresentGeometry& geometry) {
        const bool milestone = present == 1 || present == 10 || present == 100 || present == 1000;
        const bool changed = seen && !(geometry == last);
        seen = true;
        last = geometry;
        return milestone || (changed && changes++ < kMaxChanges);
    }

private:
    static constexpr unsigned kMaxChanges = 8;
    bool seen = false;
    unsigned changes = 0;
    PresentGeometry last{};
};

/// A Vulkan result is reported when it is the first one or differs from the previous one, at most `max_lines` times.
class ResultReporter {
public:
    bool Observe(int result) {
        const bool report = !seen || result != last;
        seen = true;
        last = result;
        return report && lines++ < max_lines;
    }

private:
    static constexpr unsigned max_lines = 8;
    bool seen = false;
    int last = 0;
    unsigned lines = 0;
};

/// One report when presenting stalls, another every 10 s while it lasts, and one when it resumes.
class StallReporter {
public:
    enum class Action { None, Report, Resumed };
    static constexpr std::int64_t kRepeatMs = 10000;

    Action Update(bool stalled, std::int64_t now_ms) {
        if (!stalled) {
            const bool was_stalled = active;
            active = false;
            return was_stalled ? Action::Resumed : Action::None;
        }
        if (!active) {
            active = true;
            since_ms = last_report_ms = now_ms;
            return Action::Report;
        }
        if (now_ms - last_report_ms >= kRepeatMs) {
            last_report_ms = now_ms;
            return Action::Report;
        }
        return Action::None;
    }

    std::int64_t SinceMs() const {
        return since_ms;
    }

private:
    bool active = false;
    std::int64_t since_ms = 0;
    std::int64_t last_report_ms = 0;
};

inline constexpr int kNoResult = INT32_MIN;

inline const char* ResultName(int result) {
    switch (result) {
    case kNoResult:
        return "none yet";
    case 0:
        return "VK_SUCCESS";
    case 1000001003:
        return "VK_SUBOPTIMAL_KHR";
    case -1000001004:
        return "VK_ERROR_OUT_OF_DATE_KHR";
    case -1000000000:
        return "VK_ERROR_SURFACE_LOST_KHR";
    case -4:
        return "VK_ERROR_DEVICE_LOST";
    case -13:
        return "VK_ERROR_UNKNOWN";
    default:
        return "unexpected result";
    }
}

inline std::uint64_t Pack(Size size) {
    return std::uint64_t(size.width) | std::uint64_t(size.height) << 32;
}

inline Size Unpack(std::uint64_t packed) {
    return {static_cast<std::uint32_t>(packed), static_cast<std::uint32_t>(packed >> 32)};
}

/// What the presenter has done so far, read by the stall watchdog and the title checkpoint.
struct Progress {
    std::atomic<std::uint64_t> videoout_images{0}; ///< guest images prepared for display
    std::atomic<std::uint64_t> blitted{0};         ///< presentation frames recorded into a swapchain image
    std::atomic<std::uint64_t> presented{0};       ///< vkQueuePresentKHR calls that returned success or suboptimal
    std::atomic<int> last_acquire{kNoResult};
    std::atomic<int> last_present{kNoResult};
    std::atomic<std::uint64_t> swapchain_size{0};
    std::atomic<std::uint64_t> source_size{0};
    std::atomic<std::uint64_t> destination_size{0};
    std::atomic<bool> window_active{true};
};

inline Progress g_progress;

/// Linux session facts that change presentation; values that look like paths are not shown.
template <typename GetEnv>
std::string DescribeSession(bool gamescope, GetEnv get_env) {
    static constexpr const char* names[] = {
        "GAMESCOPE_WAYLAND_DISPLAY", "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE", "SteamDeck", "SteamGamepadUI",
        "ENABLE_GAMESCOPE_WSI", "DISABLE_GAMESCOPE_WSI", "GAMESCOPE_WSI_MIN_IMAGE_COUNT",
        "GAMESCOPE_WSI_ENSURE_MIN_IMAGE_COUNT", "GAMESCOPE_WSI_FRAME_LIMITER_AWARE", "GAMESCOPE_LIMITER_FILE",
        "MESA_VK_WSI_PRESENT_MODE", "vk_x11_override_min_image_count", "vk_xwayland_wait_ready", "MANGOHUD",
        "LD_PRELOAD", "VK_INSTANCE_LAYERS", "VK_LOADER_LAYERS_ENABLE", "VK_LOADER_LAYERS_DISABLE", "OBS_VKCAPTURE",
    };
    std::string text = gamescope ? "Gamescope yes (flip pacing on)" : "Gamescope no";
    for (const char* name : names) {
        const char* value = get_env(name);
        if (!value) {
            continue;
        }
        const bool hidden = std::strpbrk(value, "/\\") || std::strlen(value) > 40;
        text += std::string(", ") + name + "=" + (hidden ? "(set)" : value);
    }
    return text;
}

} // namespace DcpPresent
