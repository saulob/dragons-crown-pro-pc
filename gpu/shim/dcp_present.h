// SPDX-License-Identifier: GPL-2.0-or-later
// Swapchain and flip pacing choices that need no Vulkan device (tests/present_tests.cpp).
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
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

    /// The swapchain is stale for a reason Vulkan has not reported yet (the window came back from minimized).
    void RequestRecreation() {
        RequestRequired();
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

/// What the window thread learns from SDL. Focus is not part of it: an unfocused window still has a surface.
enum class WindowEvent { Minimized, Restored, Hidden, Shown };

struct WindowVisibility {
    bool minimized = false;
    bool hidden = false;

    void Apply(WindowEvent event) {
        switch (event) {
        case WindowEvent::Minimized:
            minimized = true;
            break;
        case WindowEvent::Restored:
            minimized = false;
            break;
        case WindowEvent::Hidden:
            hidden = true;
            break;
        case WindowEvent::Shown:
            hidden = false;
            break;
        }
    }

    std::uint8_t Pack() const {
        return std::uint8_t((minimized ? 1 : 0) | (hidden ? 2 : 0));
    }

    static WindowVisibility Unpack(std::uint8_t bits) {
        return {(bits & 1) != 0, (bits & 2) != 0};
    }
};

enum class Lifecycle : int { Presentable, Minimized, Hidden, WindowSizeZero, SurfaceSizeZero, Restoring };

inline const char* LifecycleName(int lifecycle) {
    switch (static_cast<Lifecycle>(lifecycle)) {
    case Lifecycle::Presentable:
        return "presentable";
    case Lifecycle::Minimized:
        return "minimized";
    case Lifecycle::Hidden:
        return "hidden";
    case Lifecycle::WindowSizeZero:
        return "window size zero";
    case Lifecycle::SurfaceSizeZero:
        return "surface size zero";
    case Lifecycle::Restoring:
        return "restoring";
    }
    return "unknown";
}

/// A surface can hold a swapchain only with a non-zero extent. Windows reports 0x0 for a minimized window while SDL
/// keeps the last pixel size; a surface that follows the swapchain (width UINT32_MAX) is judged by the maximum.
inline bool SurfaceHasArea(std::uint32_t current_width, std::uint32_t current_height, std::uint32_t max_width,
                           std::uint32_t max_height) {
    if (max_width == 0 || max_height == 0) {
        return false;
    }
    return current_width == UINT32_MAX || (current_width > 0 && current_height > 0);
}

/// Whether the presenter may acquire, record and queue a present. Swap thread only.
class PresentGate {
public:
    struct Inputs {
        WindowVisibility visibility;
        std::uint32_t window_width;
        std::uint32_t window_height;
        bool surface_zero; ///< only consulted while suspended or a recreation is pending
    };

    struct Step {
        bool present;     ///< acquire and present may go on
        bool recreate;    ///< presentation just became possible again: one recreation before the next acquire
        bool transition;  ///< the lifecycle changed: log it
        Lifecycle lifecycle;
    };

    bool Suspended() const {
        return suspended;
    }

    Step Update(const Inputs& in) {
        const Lifecycle reason = ReasonFor(in);
        if (reason != Lifecycle::Presentable) {
            return Suspend(reason);
        }
        if (suspended) {
            suspended = false;
            restoring = true;
            current = Lifecycle::Restoring;
            return {true, true, true, current};
        }
        return {true, false, false, current};
    }

    /// A recreation found no usable surface.
    Step SurfaceUnavailable() {
        return Suspend(Lifecycle::SurfaceSizeZero);
    }

    /// A frame was presented: the restore is over.
    bool Presented() {
        if (!restoring) {
            return false;
        }
        restoring = false;
        current = Lifecycle::Presentable;
        return true;
    }

    Lifecycle Current() const {
        return current;
    }

private:
    static Lifecycle ReasonFor(const Inputs& in) {
        if (in.visibility.minimized) {
            return Lifecycle::Minimized;
        }
        if (in.visibility.hidden) {
            return Lifecycle::Hidden;
        }
        if (in.window_width == 0 || in.window_height == 0) {
            return Lifecycle::WindowSizeZero;
        }
        return in.surface_zero ? Lifecycle::SurfaceSizeZero : Lifecycle::Presentable;
    }

    Step Suspend(Lifecycle reason) {
        const bool changed = !suspended || current != reason;
        suspended = true;
        restoring = false;
        current = reason;
        return {false, false, changed, reason};
    }

    bool suspended = false;
    bool restoring = false;
    Lifecycle current = Lifecycle::Presentable;
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
    std::atomic<std::uint64_t> destination_origin{0};
    std::atomic<std::uint64_t> destination_surface{0}; ///< the image the destination is placed in
    std::atomic<bool> window_active{true};
    std::atomic<int> lifecycle{static_cast<int>(Lifecycle::Presentable)};
    std::atomic<std::uint64_t> window_size{0};
    std::atomic<std::uint32_t> swapchain_generation{0};
    std::atomic<bool> recreation_pending{false};
};

inline Progress g_progress;

inline constexpr std::int64_t kStallNotableMs = 500;
inline constexpr std::int64_t kStallSevereMs = 2000;

constexpr bool ShouldLogCount(std::uint64_t count) {
    if (count == 0) {
        return false;
    }
    for (std::uint64_t mark = 1; mark <= count; mark *= 10) {
        if (count == mark) {
            return true;
        }
        if (mark > UINT64_MAX / 10) {
            break;
        }
    }
    return false;
}

constexpr const char* StallSeverity(std::int64_t duration_ms) {
    if (duration_ms >= kStallSevereMs) {
        return "severe";
    }
    if (duration_ms >= kStallNotableMs) {
        return "notable";
    }
    return nullptr;
}

class StallMonitor {
public:
    std::string ObservePipelineStall(std::int64_t now_ms, const char* event,
                                     std::int64_t duration_ms, std::int64_t driver_ms,
                                     bool mixed_samples, bool preloading) {
        const char* severity = StallSeverity(duration_ms);
        if (!severity) {
            return {};
        }
        last_pipeline_stall_at_ms = now_ms;
        last_pipeline_stall_duration_ms = duration_ms;
        last_pipeline_stall_mixed_samples = mixed_samples;
        const bool is_cache = std::strcmp(event, "pipeline_cache_compile") == 0;
        auto& counter = is_cache ? cache_stall_count : pipeline_stall_count;
        auto& severe_counter = is_cache ? cache_severe_count : pipeline_severe_count;
        const std::uint64_t count = ++counter;
        const bool severe = duration_ms >= kStallSevereMs;
        const std::uint64_t severe_count = severe ? ++severe_counter : 0;
        if (!ShouldLogCount(count) && !(severe && ShouldLogCount(severe_count))) {
            return {};
        }
        char line[320];
        if (driver_ms >= 0) {
            std::snprintf(line, sizeof(line),
                          "GPU stall telemetry: event=%s severity=%s duration_ms=%lld "
                          "driver_ms=%lld mixed_samples=%s preloading=%s count=%llu",
                          event, severity, static_cast<long long>(duration_ms),
                          static_cast<long long>(driver_ms), mixed_samples ? "yes" : "no",
                          preloading ? "yes" : "no", static_cast<unsigned long long>(count));
        } else {
            std::snprintf(line, sizeof(line),
                          "GPU stall telemetry: event=%s severity=%s duration_ms=%lld "
                          "mixed_samples=%s count=%llu",
                          event, severity, static_cast<long long>(duration_ms),
                          mixed_samples ? "yes" : "no", static_cast<unsigned long long>(count));
        }
        return line;
    }

    std::string ObservePresent(std::int64_t now_ms, std::uint64_t videoout_images,
                               std::uint32_t swapchain_generation, int last_acquire,
                               int last_present) {
        if (!has_present) {
            has_present = true;
            last_present_ms = now_ms;
            last_present_videoout_images = videoout_images;
            last_present_swapchain_gen = swapchain_generation;
            return {};
        }
        const std::int64_t prev_ms = last_present_ms;
        const std::int64_t gap_ms = now_ms - prev_ms;
        const bool source_changed = videoout_images != last_present_videoout_images;
        const bool swapchain_changed = swapchain_generation != last_present_swapchain_gen;
        last_present_ms = now_ms;
        last_present_videoout_images = videoout_images;
        last_present_swapchain_gen = swapchain_generation;

        const char* severity = StallSeverity(gap_ms);
        if (!severity) {
            return {};
        }
        const std::uint64_t count = ++present_gap_count;
        const bool severe = gap_ms >= kStallSevereMs;
        const std::uint64_t severe_count = severe ? ++present_severe_count : 0;
        if (!ShouldLogCount(count) && !(severe && ShouldLogCount(severe_count))) {
            return {};
        }
        const std::int64_t correlated_ms = CorrelatedPipelineStallMs(prev_ms, now_ms);
        char line[384];
        std::snprintf(line, sizeof(line),
                      "Presenter telemetry: event=present_gap severity=%s duration_ms=%lld "
                      "acquire=%s present=%s source_changed=%s swapchain_changed=%s "
                      "swapchain_gen=%u recent_pipeline_stall_ms=%lld count=%llu",
                      severity, static_cast<long long>(gap_ms), ResultName(last_acquire),
                      ResultName(last_present), source_changed ? "yes" : "no",
                      swapchain_changed ? "yes" : "no", swapchain_generation,
                      static_cast<long long>(correlated_ms),
                      static_cast<unsigned long long>(count));
        return line;
    }

    std::string ObserveVideoOut(std::int64_t now_ms, std::uint32_t swapchain_generation,
                                int last_acquire, int last_present) {
        if (!has_videoout) {
            has_videoout = true;
            last_videoout_ms = now_ms;
            last_videoout_swapchain_gen = swapchain_generation;
            return {};
        }
        const std::int64_t prev_ms = last_videoout_ms;
        const std::int64_t gap_ms = now_ms - prev_ms;
        const bool swapchain_changed = swapchain_generation != last_videoout_swapchain_gen;
        last_videoout_ms = now_ms;
        last_videoout_swapchain_gen = swapchain_generation;

        const char* severity = StallSeverity(gap_ms);
        if (!severity) {
            return {};
        }
        const std::uint64_t count = ++videoout_gap_count;
        const bool severe = gap_ms >= kStallSevereMs;
        const std::uint64_t severe_count = severe ? ++videoout_severe_count : 0;
        if (!ShouldLogCount(count) && !(severe && ShouldLogCount(severe_count))) {
            return {};
        }
        const std::int64_t correlated_ms = CorrelatedPipelineStallMs(prev_ms, now_ms);
        char line[384];
        std::snprintf(line, sizeof(line),
                      "Presenter telemetry: event=videoout_gap severity=%s duration_ms=%lld "
                      "acquire=%s present=%s swapchain_changed=%s swapchain_gen=%u "
                      "recent_pipeline_stall_ms=%lld count=%llu",
                      severity, static_cast<long long>(gap_ms), ResultName(last_acquire),
                      ResultName(last_present), swapchain_changed ? "yes" : "no",
                      swapchain_generation, static_cast<long long>(correlated_ms),
                      static_cast<unsigned long long>(count));
        return line;
    }

    std::string ObserveGuestSubmit(std::int64_t now_ms, std::uint64_t videoout_images,
                                   std::uint32_t swapchain_generation) {
        if (!has_guest_submit) {
            has_guest_submit = true;
            last_guest_submit_ms = now_ms;
            last_guest_videoout_images = videoout_images;
            last_guest_swapchain_gen = swapchain_generation;
            return {};
        }
        const std::int64_t prev_ms = last_guest_submit_ms;
        const std::int64_t gap_ms = now_ms - prev_ms;
        const bool source_changed = videoout_images != last_guest_videoout_images;
        const bool swapchain_changed = swapchain_generation != last_guest_swapchain_gen;
        last_guest_submit_ms = now_ms;
        last_guest_videoout_images = videoout_images;
        last_guest_swapchain_gen = swapchain_generation;

        const char* severity = StallSeverity(gap_ms);
        if (!severity) {
            return {};
        }
        const std::uint64_t count = ++guest_submit_gap_count;
        const bool severe = gap_ms >= kStallSevereMs;
        const std::uint64_t severe_count = severe ? ++guest_submit_severe_count : 0;
        if (!ShouldLogCount(count) && !(severe && ShouldLogCount(severe_count))) {
            return {};
        }
        const std::int64_t correlated_ms = CorrelatedPipelineStallMs(prev_ms, now_ms);
        char line[384];
        std::snprintf(line, sizeof(line),
                      "Presenter telemetry: event=guest_submit_gap severity=%s duration_ms=%lld "
                      "source_changed=%s swapchain_changed=%s swapchain_gen=%u "
                      "recent_pipeline_stall_ms=%lld count=%llu",
                      severity, static_cast<long long>(gap_ms), source_changed ? "yes" : "no",
                      swapchain_changed ? "yes" : "no", swapchain_generation,
                      static_cast<long long>(correlated_ms),
                      static_cast<unsigned long long>(count));
        return line;
    }

private:
    std::int64_t CorrelatedPipelineStallMs(std::int64_t window_start_ms,
                                           std::int64_t window_end_ms) const {
        if (last_pipeline_stall_duration_ms <= 0) {
            return 0;
        }
        if (last_pipeline_stall_at_ms >= window_start_ms &&
            last_pipeline_stall_at_ms <= window_end_ms) {
            return last_pipeline_stall_duration_ms;
        }
        return 0;
    }

    bool has_present = false;
    std::int64_t last_present_ms = 0;
    std::uint64_t last_present_videoout_images = 0;
    std::uint32_t last_present_swapchain_gen = 0;
    std::uint64_t present_gap_count = 0;
    std::uint64_t present_severe_count = 0;

    bool has_videoout = false;
    std::int64_t last_videoout_ms = 0;
    std::uint32_t last_videoout_swapchain_gen = 0;
    std::uint64_t videoout_gap_count = 0;
    std::uint64_t videoout_severe_count = 0;

    bool has_guest_submit = false;
    std::int64_t last_guest_submit_ms = 0;
    std::uint64_t last_guest_videoout_images = 0;
    std::uint32_t last_guest_swapchain_gen = 0;
    std::uint64_t guest_submit_gap_count = 0;
    std::uint64_t guest_submit_severe_count = 0;

    std::int64_t last_pipeline_stall_at_ms = 0;
    std::int64_t last_pipeline_stall_duration_ms = 0;
    bool last_pipeline_stall_mixed_samples = false;
    std::uint64_t pipeline_stall_count = 0;
    std::uint64_t pipeline_severe_count = 0;
    std::uint64_t cache_stall_count = 0;
    std::uint64_t cache_severe_count = 0;
};

inline std::mutex g_stall_mutex;
inline StallMonitor g_stall_monitor;

inline std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline void RecordPipelineStall(const char* event, std::int64_t duration_ms,
                                std::int64_t driver_ms, bool mixed_samples, bool preloading) {
    if (duration_ms < kStallNotableMs) {
        return;
    }
    std::string line;
    {
        std::scoped_lock lock{g_stall_mutex};
        line = g_stall_monitor.ObservePipelineStall(NowMs(), event, duration_ms, driver_ms,
                                                    mixed_samples, preloading);
    }
    if (!line.empty()) {
        std::printf("%s\n", line.c_str());
    }
}

inline void RecordPresent() {
    const auto now = NowMs();
    std::string line;
    {
        std::scoped_lock lock{g_stall_mutex};
        line = g_stall_monitor.ObservePresent(
            now, g_progress.videoout_images.load(std::memory_order_relaxed),
            g_progress.swapchain_generation.load(std::memory_order_relaxed),
            g_progress.last_acquire.load(std::memory_order_relaxed),
            g_progress.last_present.load(std::memory_order_relaxed));
    }
    if (!line.empty()) {
        std::printf("%s\n", line.c_str());
    }
}

inline void RecordVideoOut() {
    const auto now = NowMs();
    std::string line;
    {
        std::scoped_lock lock{g_stall_mutex};
        line = g_stall_monitor.ObserveVideoOut(
            now, g_progress.swapchain_generation.load(std::memory_order_relaxed),
            g_progress.last_acquire.load(std::memory_order_relaxed),
            g_progress.last_present.load(std::memory_order_relaxed));
    }
    if (!line.empty()) {
        std::printf("%s\n", line.c_str());
    }
}

inline void RecordGuestSubmit() {
    const auto now = NowMs();
    std::string line;
    {
        std::scoped_lock lock{g_stall_mutex};
        line = g_stall_monitor.ObserveGuestSubmit(
            now, g_progress.videoout_images.load(std::memory_order_relaxed),
            g_progress.swapchain_generation.load(std::memory_order_relaxed));
    }
    if (!line.empty()) {
        std::printf("%s\n", line.c_str());
    }
}

/// One line for the log before the process stops on VK_ERROR_DEVICE_LOST.
inline std::string DescribeDeviceLost(const char* where, const Progress& progress) {
    const Size window = Unpack(progress.window_size.load());
    const Size swapchain = Unpack(progress.swapchain_size.load());
    char text[320];
    std::snprintf(text, sizeof(text),
                  "GPU device lost: %s lifecycle=%s window=%ux%u swapchain=%ux%u generation=%u "
                  "recreation_pending=%s acquire=%s present=%s",
                  where, LifecycleName(progress.lifecycle.load()), window.width, window.height, swapchain.width,
                  swapchain.height, progress.swapchain_generation.load(),
                  progress.recreation_pending.load() ? "yes" : "no", ResultName(progress.last_acquire.load()),
                  ResultName(progress.last_present.load()));
    return text;
}

inline void ReportDeviceLost(const char* where) {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        std::printf("%s\n", DescribeDeviceLost(where, g_progress).c_str());
        std::fflush(stdout);
    }
}

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
