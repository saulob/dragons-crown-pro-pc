#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace dcp {

constexpr int kLauncherTargetHz = 60;
constexpr int64_t kNsPerSecond = 1'000'000'000;
constexpr int64_t kNsPerMs = 1'000'000;
constexpr int64_t kLauncherFrameNs = kNsPerSecond / kLauncherTargetHz;
constexpr int kMinimizedSleepMs = 30;

struct FrameWait {
    bool draw = false;
    int64_t wait_ns = 0;
};

// Deadline-based frame cap on a monotonic clock, so a high refresh rate VSync does not rebuild the UI at that rate.
// An interval of 0 never waits.
class FramePacer {
public:
    explicit FramePacer(int64_t interval_ns = kLauncherFrameNs) : interval_(interval_ns) {}

    FrameWait Next(int64_t now_ns) {
        if (interval_ <= 0) {
            return {true, 0};
        }
        if (!scheduled_) {
            scheduled_ = true;
            deadline_ = now_ns + interval_;
            return {true, 0};
        }
        if (deadline_ - now_ns > interval_) {
            deadline_ = now_ns + interval_;
        }
        if (now_ns < deadline_) {
            return {false, deadline_ - now_ns};
        }
        deadline_ += interval_;
        if (deadline_ <= now_ns) {
            deadline_ = now_ns + interval_;
        }
        return {true, 0};
    }

    void Reset() { scheduled_ = false; }
    int64_t interval_ns() const { return interval_; }

private:
    int64_t interval_;
    int64_t deadline_ = 0;
    bool scheduled_ = false;
};

struct VsyncState {
    bool requested = false;
    bool effective = false;
};

// Wall-clock time of one launcher frame: building the UI, recording draw commands, and presenting, which also executes
// the recorded commands (the renderer batches them) and may wait for VSync.
struct FrameTimes {
    int64_t build_ns = 0;
    int64_t submit_ns = 0;
    int64_t present_ns = 0;
};

struct LoopSample {
    int64_t frames = 0;
    int64_t elapsed_ns = 0;
    double hz = 0.0;
    FrameTimes total;
    int64_t wait_ns = 0;
    int64_t max_present_ns = 0;
    int64_t slow_presents = 0;
    int cpu_pct = -1;  // the UI thread's own CPU time, -1 when unavailable
    bool first = false;
    bool excessive_rate = false;
    bool present_stall = false;
};

// Aggregates drawn frames into wall-clock samples once the launcher has been visible long enough to be stable.
class LoopMonitor {
public:
    static constexpr int64_t kWarmupNs = 5 * kNsPerSecond;
    static constexpr int64_t kWindowNs = 5 * kNsPerSecond;
    static constexpr double kExcessiveHz = kLauncherTargetHz * 1.5;
    static constexpr int64_t kSlowPresentNs = 50 * kNsPerMs;
    static constexpr int64_t kStallMinFrames = 3;

    void Waited(int64_t ns) {
        if (windowed_) {
            wait_ += ns;
        }
    }

    std::optional<LoopSample> FrameDrawn(int64_t now_ns, const FrameTimes& times, int64_t thread_cpu_ns = -1) {
        if (!visible_) {
            visible_ = true;
            visible_since_ = now_ns;
        }
        if (!windowed_) {
            if (now_ns - visible_since_ >= kWarmupNs) {
                StartWindow(now_ns, thread_cpu_ns);
            }
            return std::nullopt;
        }
        ++frames_;
        total_.build_ns += times.build_ns;
        total_.submit_ns += times.submit_ns;
        total_.present_ns += times.present_ns;
        max_present_ = std::max(max_present_, times.present_ns);
        slow_presents_ += times.present_ns > kSlowPresentNs ? 1 : 0;
        const int64_t elapsed = now_ns - window_start_;
        if (elapsed < kWindowNs) {
            return std::nullopt;
        }
        LoopSample sample;
        sample.frames = frames_;
        sample.elapsed_ns = elapsed;
        sample.hz = double(frames_) * double(kNsPerSecond) / double(elapsed);
        sample.total = total_;
        sample.wait_ns = wait_;
        sample.max_present_ns = max_present_;
        sample.slow_presents = slow_presents_;
        if (thread_cpu_ns >= 0 && window_cpu_ >= 0) {
            sample.cpu_pct = int(std::clamp<int64_t>((thread_cpu_ns - window_cpu_) * 100 / elapsed, 0, 100));
        }
        sample.first = !sampled_;
        sample.excessive_rate = sample.hz > kExcessiveHz && !rate_reported_;
        sample.present_stall = frames_ >= kStallMinFrames && slow_presents_ * 2 >= frames_ && !stall_reported_;
        sampled_ = true;
        rate_reported_ = rate_reported_ || sample.excessive_rate;
        stall_reported_ = stall_reported_ || sample.present_stall;
        StartWindow(now_ns, thread_cpu_ns);
        return sample;
    }

    void Reset() {
        visible_ = false;
        windowed_ = false;
    }

private:
    void StartWindow(int64_t now_ns, int64_t thread_cpu_ns) {
        windowed_ = true;
        window_start_ = now_ns;
        window_cpu_ = thread_cpu_ns;
        frames_ = 0;
        total_ = {};
        wait_ = 0;
        max_present_ = 0;
        slow_presents_ = 0;
    }

    bool visible_ = false;
    bool windowed_ = false;
    bool sampled_ = false;
    bool rate_reported_ = false;
    bool stall_reported_ = false;
    int64_t visible_since_ = 0;
    int64_t window_start_ = 0;
    int64_t window_cpu_ = -1;
    int64_t frames_ = 0;
    FrameTimes total_;
    int64_t wait_ = 0;
    int64_t max_present_ = 0;
    int64_t slow_presents_ = 0;
};

inline std::string RendererLabel(std::string_view name) {
    std::string label;
    for (const char c : name) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        label += plain ? c : (c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : '_');
    }
    return label.empty() ? "unknown" : label;
}

inline std::string PacingModeLine(std::string_view renderer, const VsyncState& vsync) {
    char line[192];
    std::snprintf(line, sizeof(line),
                  "Launcher telemetry: frame_pacing renderer=%s requested_vsync=%d effective_vsync=%d pacing=%s target_hz=%d",
                  RendererLabel(renderer).c_str(), vsync.requested ? 1 : 0, vsync.effective ? 1 : 0,
                  vsync.effective ? "vsync" : "fallback", kLauncherTargetHz);
    return line;
}

inline std::string RendererSelectedLine(std::string_view renderer, bool fallback, int pinned_modules) {
    char line[160];
    std::snprintf(line, sizeof(line), "Launcher telemetry: renderer_selected renderer=%s fallback=%d pinned_modules=%d",
                  RendererLabel(renderer).c_str(), fallback ? 1 : 0, pinned_modules);
    return line;
}

namespace loop_detail {
inline double AverageMs(int64_t total_ns, int64_t frames) {
    return frames > 0 ? double(total_ns) / double(frames) / double(kNsPerMs) : 0.0;
}
inline int Percent(int64_t part_ns, int64_t whole_ns) {
    return whole_ns > 0 ? int(std::clamp<int64_t>(part_ns * 100 / whole_ns, 0, 100)) : 0;
}
inline std::string Body(std::string_view renderer, const LoopSample& s) {
    char body[320];
    std::snprintf(body, sizeof(body),
                  "renderer=%s frames=%lld elapsed_ms=%lld effective_hz=%.1f avg_build_ms=%.1f avg_submit_ms=%.1f "
                  "avg_present_ms=%.1f avg_wait_ms=%.1f present_pct=%d wait_pct=%d",
                  RendererLabel(renderer).c_str(), static_cast<long long>(s.frames),
                  static_cast<long long>(s.elapsed_ns / kNsPerMs), s.hz, AverageMs(s.total.build_ns, s.frames),
                  AverageMs(s.total.submit_ns, s.frames), AverageMs(s.total.present_ns, s.frames),
                  AverageMs(s.wait_ns, s.frames), Percent(s.total.present_ns, s.elapsed_ns), Percent(s.wait_ns, s.elapsed_ns));
    std::string text = body;
    if (s.cpu_pct >= 0) {
        text += " ui_thread_cpu_pct=" + std::to_string(s.cpu_pct);
    }
    return text;
}
} // namespace loop_detail

inline std::string LoopSampleLine(std::string_view renderer, const LoopSample& sample) {
    return "Launcher telemetry: loop_sample " + loop_detail::Body(renderer, sample);
}

inline std::string ExcessiveRateLine(std::string_view renderer, const LoopSample& sample) {
    return "Launcher telemetry: event=launcher_frame_pacing_excessive target_hz=" + std::to_string(kLauncherTargetHz) + " " +
           loop_detail::Body(renderer, sample);
}

inline std::string PresentStallLine(std::string_view renderer, const LoopSample& sample) {
    char extra[96];
    std::snprintf(extra, sizeof(extra), " max_present_ms=%.1f slow_presents=%lld threshold_ms=%lld",
                  double(sample.max_present_ns) / double(kNsPerMs), static_cast<long long>(sample.slow_presents),
                  static_cast<long long>(LoopMonitor::kSlowPresentNs / kNsPerMs));
    return "Launcher telemetry: event=launcher_present_stall " + loop_detail::Body(renderer, sample) + extra;
}

} // namespace dcp
