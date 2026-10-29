// Focused tests of the Frame rate setting: the mode table, which flips reach the screen, the repeat cadence above 60 and
// the limiter that schedules it (dcp_frame_pacing.h), plus source checks that the game's own pacing is left alone.
// No window, no GPU and no game files.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "dcp_frame_pacing.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                             \
    do {                                                        \
        if (!(cond)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond); \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

namespace Pacing = DcpFramePacing;
using Clock = std::chrono::steady_clock;

std::string ReadSource(const char* path) {
    std::ifstream file(std::string(DCP_SOURCE_DIR) + "/" + path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

bool InOrder(const std::string& text, const std::string& first, const std::string& second) {
    const std::size_t a = text.find(first);
    return a != std::string::npos && text.find(second, a + first.size()) != std::string::npos;
}

void TestModeTable() {
    const int expected[] = {30, 60, 90, 120, 144, 165, 240, 0};
    CHECK(Pacing::kFrameRateModes.size() == std::size(expected));
    for (std::size_t i = 0; i < std::size(expected); ++i) {
        CHECK(Pacing::kFrameRateModes[i] == expected[i]);
        CHECK(Pacing::IsFrameRateMode(expected[i]));
    }
    CHECK(!Pacing::IsFrameRateMode(-1) && !Pacing::IsFrameRateMode(45) && !Pacing::IsFrameRateMode(75) &&
          !Pacing::IsFrameRateMode(1000));
    CHECK(Pacing::RenderFps(45) == 60 && Pacing::RenderFps(-1) == 60);
    CHECK(Pacing::RenderFps(0) == 0 && Pacing::RenderFps(144) == 144);
}

void TestDefaultIsTheOriginalBehavior() {
    CHECK(!Pacing::IsHighRate(60) && !Pacing::IsHighRate(30) && !Pacing::IsHighRate(-1) && !Pacing::IsHighRate(75));
    for (const bool vsync : {false, true}) {
        for (const double display : {0.0, 59.94, 60.0, 144.0}) {
            CHECK(Pacing::RepeatCadenceHz(60, vsync, display) == 0);
            CHECK(Pacing::RepeatCadenceHz(30, vsync, display) == 0);
        }
    }
    for (std::uint64_t flip = 0; flip < 8; ++flip) {
        CHECK(Pacing::PresentsFlip(60, flip));
    }
}

void TestRepeatCadence() {
    for (const int fps : {90, 120, 144, 165, 240}) {
        CHECK(Pacing::IsHighRate(fps));
        CHECK(Pacing::RepeatCadenceHz(fps) == static_cast<unsigned>(fps));
        for (std::uint64_t flip = 0; flip < 8; ++flip) {
            CHECK(Pacing::PresentsFlip(fps, flip));
        }
    }
    CHECK(Pacing::IsHighRate(0) && Pacing::IsUnlimited(0));
    CHECK(Pacing::RepeatCadenceHz(0) == static_cast<unsigned>(Pacing::kUnlimitedCadenceHz));
    CHECK(Pacing::RepeatCadenceHz(0, false, 144.0) == static_cast<unsigned>(Pacing::kUnlimitedCadenceHz));
}

void TestThirtyShowsEverySecondFlip() {
    int shown = 0;
    for (std::uint64_t flip = 0; flip < 100; ++flip) {
        shown += Pacing::PresentsFlip(30, flip) ? 1 : 0;
    }
    CHECK(shown == 50);
    CHECK(Pacing::PresentsFlip(30, 0) && !Pacing::PresentsFlip(30, 1) && Pacing::PresentsFlip(30, 2));
}

void TestVsyncBoundsTheCadence() {
    CHECK(Pacing::EffectiveRenderHz(120, true, 60.0) == 60);
    CHECK(Pacing::EffectiveRenderHz(120, true, 59.94) == 60);
    CHECK(Pacing::EffectiveRenderHz(144, true, 143.856) == 144);
    CHECK(Pacing::EffectiveRenderHz(0, true, 165.0) == 165);
    CHECK(Pacing::EffectiveRenderHz(0, false, 165.0) == 0);
    CHECK(Pacing::EffectiveRenderHz(120, false, 60.0) == 120);
    CHECK(Pacing::EffectiveRenderHz(120, true, 0.0) == 120);
    // A display at or below the game's rate leaves nothing to repeat.
    CHECK(Pacing::RepeatCadenceHz(120, true, 60.0) == 0);
    CHECK(Pacing::RepeatCadenceHz(120, true, 59.94) == 0);
    CHECK(Pacing::RepeatCadenceHz(0, true, 60.0) == 0);
    CHECK(Pacing::RepeatCadenceHz(120, true, 144.0) == 120);
    CHECK(Pacing::RepeatCadenceHz(240, true, 143.856) == 144);
    CHECK(Pacing::RepeatCadenceHz(0, true, 165.0) == 165);
    CHECK(Pacing::RepeatCadenceHz(240, false, 144.0) == 240);
    CHECK(Pacing::RepeatCadenceHz(240, true, 0.0) == 240);
    CHECK(Pacing::RepeatCadenceHz(0, true, 1000.0) == static_cast<unsigned>(Pacing::kUnlimitedCadenceHz));
}

void TestLimiterSlots() {
    const auto start = Clock::now();
    Pacing::FrameLimiter off{0, start};
    CHECK(!off.Enabled() && off.Due(start) && off.Period().count() == 0);

    Pacing::FrameLimiter limiter{120, start};
    const auto period = limiter.Period();
    CHECK(limiter.Enabled() && period == std::chrono::nanoseconds(8333333));
    CHECK(limiter.Due(start));
    limiter.Release(start + std::chrono::microseconds(100));
    CHECK(limiter.NextSlot() == start + period);
    CHECK(!limiter.Due(start + period - std::chrono::nanoseconds(1)));
    CHECK(limiter.Due(start + period));
    // A late wake does not move the later slots.
    limiter.Release(start + period + std::chrono::milliseconds(3));
    CHECK(limiter.NextSlot() == start + 2 * period);
    // More than one period late re-anchors without a burst.
    const auto late = start + 10 * period;
    limiter.Release(late);
    CHECK(limiter.NextSlot() == late + period);
}

// Presentations the cadence schedules in one second of 60 Hz vblank ticks: each tick shows its new frame, then the
// repeats whose slots fall before the next tick, as the present thread does.
struct Second {
    int presents = 0;
    int new_frames = 0;
    int repeats = 0;
    long long longest_gap_ns = 0;
};

Second SimulateSecond(unsigned cadence) {
    Second second;
    const auto start = Clock::time_point{};
    const auto tick = std::chrono::nanoseconds(16'666'667);
    Pacing::FrameLimiter limiter{cadence, start};
    Clock::time_point last = start;
    bool first = true;
    const auto note = [&](Clock::time_point at) {
        ++second.presents;
        if (!first) {
            second.longest_gap_ns = std::max<long long>(second.longest_gap_ns, (at - last).count());
        }
        first = false;
        last = at;
    };
    for (int i = 0; i < 60; ++i) {
        const auto tick_start = start + i * tick;
        const auto deadline = tick_start + tick;
        limiter.Release(tick_start);
        ++second.new_frames;
        note(tick_start);
        while (limiter.Enabled()) {
            const auto slot = limiter.NextSlot();
            if (slot >= deadline) {
                break;
            }
            limiter.Release(slot);
            ++second.repeats;
            note(slot);
        }
    }
    return second;
}

void TestRepeatsFillTheTicks() {
    const Second off = SimulateSecond(0);
    CHECK(off.presents == 60 && off.repeats == 0 && off.new_frames == 60);
    for (const unsigned hz : {90u, 120u, 144u, 165u, 240u, 480u}) {
        const Second second = SimulateSecond(hz);
        CHECK(second.new_frames == 60);
        CHECK(second.presents >= static_cast<int>(hz) - 61 && second.presents <= static_cast<int>(hz) + 61);
        CHECK(second.presents == second.new_frames + second.repeats);
        // No gap longer than a tick, so a repeat never delays a new frame.
        CHECK(second.longest_gap_ns <= 16'666'667);
    }
    const Second twice = SimulateSecond(120);
    CHECK(twice.repeats >= 58 && twice.repeats <= 61); // one repeat per new frame, 8.33 ms after it
}

void TestModeName() {
    CHECK(Pacing::ModeName(0) == "Unlimited" && Pacing::ModeName(144) == "144");
}

// The game's own pacing stays as it was: the vblank is a steady 60 Hz and nothing in the guest is hooked for the
// Frame rate.
void TestGamePacingIsUntouched() {
    const std::string emulator = ReadSource("gpu/shim/core/emulator_settings.h");
    CHECK(Contains(emulator, "u32 GetVblankFrequency() { return 60; }"));
    CHECK(Contains(emulator, "u32 GetFrameLimit() { return 0; }"));
    const std::string loader = ReadSource("src/loader.c");
    CHECK(!Contains(loader, "frame_rate"));
}

void TestWiring() {
    const std::string driver = ReadSource("gpu/shadps4/core/libraries/videoout/driver.cpp");
    CHECK(InOrder(driver, "PresentsFlip(DcpSettings::Get().frame_rate.load(), flips_consumed++)", "RetireFrame(frame)"));
    CHECK(InOrder(driver, "limiter.Release(tick_start);", "Flip(request);"));
    CHECK(Contains(driver, "if (vblank_status.count % (main_port.flip_rate + 1) == 0 && !FlipWaitsForPresenter()) {"));
    // The repeats come after the vblank events and the status update of the tick, and only while the port is open.
    CHECK(InOrder(driver, "main_port.vblank_cv.notify_all();", "limiter.Enabled() && main_port.is_open"));
    CHECK(InOrder(driver, "limiter.Enabled() && main_port.is_open", "RepeatLastFrame();"));
    CHECK(InOrder(driver, "RepeatLastFrame();", "timer.End();"));

    const std::string settings = ReadSource("gpu/shim/dcp_settings.cpp");
    CHECK(Contains(settings, "key == \"frame_rate\""));
    CHECK(Contains(settings, "IsFrameRateMode(fps) ? fps : 60"));

    const std::string presenter = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    CHECK(InOrder(presenter, "void Presenter::RetireFrame(Frame* frame) {", "free_queue.push_back(frame);"));
}

} // namespace

int main() {
    TestModeTable();
    TestDefaultIsTheOriginalBehavior();
    TestRepeatCadence();
    TestThirtyShowsEverySecondFlip();
    TestVsyncBoundsTheCadence();
    TestLimiterSlots();
    TestRepeatsFillTheTicks();
    TestModeName();
    TestGamePacingIsUntouched();
    TestWiring();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("frame pacing tests passed\n");
    return 0;
}
