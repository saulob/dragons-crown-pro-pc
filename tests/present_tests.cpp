// Focused tests of the swapchain image count, the present mode choice, Gamescope detection, the Gamescope-only flip
// pacing and the latency trace switch.
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>

#include "dcp_latency_trace.h"
#include "dcp_present.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);          \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

using DcpPresent::ImageCount;
using DcpPresent::Mode;

std::string ReadSource(const char* path) {
    std::ifstream file(std::string(DCP_SOURCE_DIR) + "/" + path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

bool InOrder(const std::string& text, const char* first, const char* second) {
    const std::size_t a = text.find(first);
    return a != std::string::npos && text.find(second, a) != std::string::npos;
}

void TestDefaultImageCount() {
    CHECK(ImageCount(2, 0, false) == 3);
    CHECK(ImageCount(3, 0, false) == 4); // the reporter's Gamescope surface, before
    CHECK(ImageCount(3, 8, false) == 4);
    CHECK(ImageCount(3, 3, false) == 3);
    CHECK(ImageCount(1, 0, false) == 2);
}

void TestGamescopeImageCount() {
    CHECK(ImageCount(3, 0, true) == 3);
    CHECK(ImageCount(4, 0, true) == 4);
    CHECK(ImageCount(2, 8, true) == 2);
    CHECK(ImageCount(1, 0, true) == 2);
    CHECK(ImageCount(1, 1, true) == 1);
    for (unsigned min = 1; min <= 8; ++min) {
        for (unsigned max = 0; max <= 8; ++max) {
            if (max && max < min) {
                continue;
            }
            for (const bool gamescope : {false, true}) {
                const unsigned count = ImageCount(min, max, gamescope);
                CHECK(count >= min);
                CHECK(!max || count <= max);
                CHECK(count <= ImageCount(min, max, false));
            }
        }
    }
}

void TestPresentMode() {
    for (const bool mailbox : {false, true}) {
        for (const bool immediate : {false, true}) {
            CHECK(DcpPresent::ChooseMode(true, mailbox, immediate) == Mode::Fifo);
        }
    }
    CHECK(DcpPresent::ChooseMode(false, true, true) == Mode::Mailbox);
    CHECK(DcpPresent::ChooseMode(false, true, false) == Mode::Mailbox);
    CHECK(DcpPresent::ChooseMode(false, false, true) == Mode::Immediate);
    CHECK(DcpPresent::ChooseMode(false, false, false) == Mode::Fifo);
    CHECK(int(Mode::Immediate) == 0 && int(Mode::Mailbox) == 1 && int(Mode::Fifo) == 2);
}

void TestGamescopeDetection() {
    CHECK(DcpPresent::IsGamescope("gamescope-0", false));
    CHECK(!DcpPresent::IsGamescope("gamescope-0", true));
    CHECK(!DcpPresent::IsGamescope("", false));
    CHECK(!DcpPresent::IsGamescope(nullptr, false));
    if (DcpPresent::kWindows) {
        CHECK(!DcpPresent::GamescopeSession());
    }
}

void TestLatencyTraceSwitch() {
    CHECK(!DcpLatencyTrace::Requested(nullptr));
    CHECK(!DcpLatencyTrace::Requested(""));
    CHECK(!DcpLatencyTrace::Requested("0"));
    CHECK(DcpLatencyTrace::Requested("1"));
}

const char* FakeEnv(const char* name) {
    static const struct {
        const char* name;
        const char* value;
    } env[] = {
        {"GAMESCOPE_WAYLAND_DISPLAY", "gamescope-0"},
        {"XDG_CURRENT_DESKTOP", "gamescope"},
        {"SteamDeck", "1"},
        {"GAMESCOPE_LIMITER_FILE", "/tmp/gamescope-limiter.123"},
        {"HOME", "/home/deck"},
        {"USER", "deck"},
        {"MESA_VK_WSI_PRESENT_MODE", "fifo"},
    };
    for (const auto& e : env) {
        if (!std::strcmp(e.name, name)) {
            return e.value;
        }
    }
    return nullptr;
}

void TestSessionDescription() {
    const std::string session = DcpPresent::DescribeSession(true, FakeEnv);
    CHECK(session.rfind("Gamescope yes", 0) == 0);
    CHECK(Contains(session, "GAMESCOPE_WAYLAND_DISPLAY=gamescope-0"));
    CHECK(Contains(session, "SteamDeck=1"));
    CHECK(Contains(session, "MESA_VK_WSI_PRESENT_MODE=fifo"));
    CHECK(Contains(session, "GAMESCOPE_LIMITER_FILE=(set)"));
    CHECK(session.find('/') == std::string::npos);
    CHECK(!Contains(session, "HOME") && !Contains(session, "USER") && !Contains(session, "deck"));
    CHECK(DcpPresent::DescribeSession(false, [](const char*) -> const char* { return nullptr; }) == "Gamescope no");
}

struct Delay {
    int worst_ticks = 0; ///< flip submit -> present
    int presents = 0;
};

// A model of the flip path in vblank ticks: the guest flips once a display buffer is free (its flip before last
// completed) and a presenter frame is free; the vblank thread completes a flip by moving it to the swap queue, and
// with Gamescope pacing only while that queue is empty; the swap thread presents one frame every `present_ticks`.
Delay FlipDelay(bool gamescope, int frames, int present_ticks, int display_buffers) {
    std::deque<int> requests, swap_queue;
    int free_frames = frames, pending_flips = 0, busy_until = -1, presenting = -1;
    Delay delay;
    for (int t = 0; t < 600; ++t) {
        if (presenting >= 0 && t >= busy_until) {
            if (t > 300) {
                delay.worst_ticks = std::max(delay.worst_ticks, t - presenting);
                ++delay.presents;
            }
            ++free_frames;
            presenting = -1;
        }
        if (presenting < 0 && !swap_queue.empty()) {
            presenting = swap_queue.front();
            swap_queue.pop_front();
            busy_until = t + present_ticks;
        }
        if (!(gamescope && !swap_queue.empty()) && !requests.empty()) {
            swap_queue.push_back(requests.front());
            requests.pop_front();
            --pending_flips;
        }
        if (pending_flips < display_buffers - 1 && free_frames > 0) {
            --free_frames;
            ++pending_flips;
            requests.push_back(t);
        }
    }
    return delay;
}

void TestFlipPacingModel() {
    for (const int buffers : {2, 3}) {
        for (const int frames : {3, 4}) {
            const Delay fast_old = FlipDelay(false, frames, 1, buffers), fast_new = FlipDelay(true, frames, 1, buffers);
            CHECK(fast_new.worst_ticks == fast_old.worst_ticks && fast_new.presents == fast_old.presents);
            const Delay slow_old = FlipDelay(false, frames, 2, buffers), slow_new = FlipDelay(true, frames, 2, buffers);
            CHECK(slow_new.worst_ticks <= slow_old.worst_ticks && slow_new.presents == slow_old.presents);
        }
    }
    // The reporter's case: 4 swapchain images (presenter frames) before, 3 with Gamescope pacing after.
    const Delay before = FlipDelay(false, 4, 2, 2), after = FlipDelay(true, 3, 2, 2);
    std::printf("30 FPS display, 60 Hz vblank: worst flip -> present %d ticks before, %d after\n", before.worst_ticks,
                after.worst_ticks);
    CHECK(after.worst_ticks < before.worst_ticks && after.presents == before.presents);
}

void TestFlipPacingIsGamescopeOnly() {
    const std::string driver = ReadSource("gpu/shadps4/core/libraries/videoout/driver.cpp");
    CHECK(Contains(driver, "bool VideoOutDriver::FlipWaitsForPresenter() {\n"
                           "    if (!DcpPresent::GamescopeSession()) {\n"
                           "        return false;\n"
                           "    }\n"));
    CHECK(Contains(driver, "&& !FlipWaitsForPresenter()) {"));
    // One Gamescope detection: nothing else reads its variable.
    const char* users[] = {"gpu/shadps4/core/libraries/videoout/driver.cpp",
                           "gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp", "gpu/shim/window.cpp",
                           "src/runtime_pad.c"};
    for (const char* path : users) {
        CHECK(!Contains(ReadSource(path), "GAMESCOPE_WAYLAND_DISPLAY"));
    }
    CHECK(Contains(ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp"),
                   "gamescope{DcpPresent::GamescopeSession()}"));
}

void TestSwapchainRecreation() {
    const std::string swapchain = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp");
    CHECK(Contains(swapchain, "DcpPresent::ImageCount(surface_min_images, surface_max_images, gamescope)"));
    // The returned images decide the count; the semaphores and frame_index follow it on every recreation.
    CHECK(Contains(swapchain, "image_count = static_cast<u32>(images.size());"));
    CHECK(InOrder(swapchain, "    SetupImages();\n    RefreshSemaphores();\n", "    frame_index = 0;"));
    CHECK(Contains(swapchain, "image_acquired.resize(image_count);"));
    CHECK(Contains(swapchain, "present_ready.resize(image_count);"));
    CHECK(Contains(swapchain, "frame_index = (frame_index + 1) % image_count;"));
    // VSync and HDR changes go through the same recreation.
    CHECK(InOrder(swapchain, "void Swapchain::UpdateVsync() {", "    FindPresentMode();\n    Recreate(width, height);"));
    CHECK(InOrder(swapchain, "void Swapchain::SetHDR(bool hdr) {", "    Recreate(width, height);"));
    CHECK(InOrder(swapchain, "void Swapchain::Recreate(u32 width_, u32 height_) {", "    Create(width_, height_);"));
}

using DcpPresent::SwapResult;
using DcpPresent::SwapStep;

void TestAcquireResults() {
    for (const SwapResult result : {SwapResult::Success, SwapResult::Suboptimal}) {
        DcpPresent::SwapchainHealth health;
        const SwapStep step = health.Acquire(result);
        CHECK(step.usable); // VK_SUBOPTIMAL_KHR is a success code: the image was acquired
        CHECK(step.recreate == (result == SwapResult::Suboptimal));
    }
    DcpPresent::SwapchainHealth out_of_date;
    const SwapStep stale = out_of_date.Acquire(SwapResult::OutOfDate);
    CHECK(!stale.usable && stale.recreate && out_of_date.NeedsRecreation());
    DcpPresent::SwapchainHealth failed;
    CHECK(!failed.Acquire(SwapResult::Failure).usable);
}

void TestPresentResults() {
    DcpPresent::SwapchainHealth health;
    const SwapStep ok = health.Present(SwapResult::Success);
    CHECK(ok.usable && !ok.recreate);
    const SwapStep suboptimal = health.Present(SwapResult::Suboptimal);
    CHECK(suboptimal.usable && suboptimal.recreate);
    DcpPresent::SwapchainHealth out_of_date;
    const SwapStep stale = out_of_date.Present(SwapResult::OutOfDate);
    CHECK(!stale.usable && stale.recreate);
    DcpPresent::SwapchainHealth failed;
    CHECK(!failed.Present(SwapResult::Failure).usable);
}

struct PresenterRun {
    int presented = 0;
    int skipped = 0;
    int recreations = 0;
};

// Presenter::Present's control flow: acquire, on failure recreate and acquire once more or skip the frame; present;
// recreate when Swapchain::Present asks for it. The scripts get the swapchain generation and the frame.
template <typename AcquireScript, typename PresentScript>
PresenterRun RunPresenter(int frames, AcquireScript acquire_result, PresentScript present_result) {
    DcpPresent::SwapchainHealth health;
    PresenterRun run;
    int generation = 0;
    const auto recreate = [&] {
        health.Recreated();
        ++run.recreations;
        ++generation;
    };
    for (int frame = 0; frame < frames; ++frame) {
        if (!health.Acquire(acquire_result(generation, frame)).usable) {
            recreate();
            if (!health.Acquire(acquire_result(generation, frame)).usable) {
                ++run.skipped;
                continue;
            }
        }
        run.presented += health.Present(present_result(generation, frame)).usable;
        if (health.NeedsRecreation()) {
            recreate();
        }
    }
    return run;
}

constexpr int kFrames = 120;

// DCP-LACF6H: a surface that reports suboptimal on every acquire and present, as on every replacement swapchain.
void TestPersistentSuboptimalPresents() {
    const auto suboptimal = [](int, int) { return SwapResult::Suboptimal; };
    const PresenterRun run = RunPresenter(kFrames, suboptimal, suboptimal);
    std::printf("persistent suboptimal surface: %d of %d frames presented, %d skipped, %d recreations\n",
                run.presented, kFrames, run.skipped, run.recreations);
    CHECK(run.presented == kFrames);
    CHECK(run.skipped == 0);
    CHECK(run.recreations == 1);
}

void TestSuboptimalRecovery() {
    const auto success = [](int, int) { return SwapResult::Success; };
    const auto first_suboptimal = [](int generation, int) {
        return generation == 0 ? SwapResult::Suboptimal : SwapResult::Success;
    };
    const PresenterRun recovered = RunPresenter(kFrames, first_suboptimal, success);
    CHECK(recovered.presented == kFrames && recovered.skipped == 0 && recovered.recreations == 1);

    const auto suboptimal = [](int, int) { return SwapResult::Suboptimal; };
    const PresenterRun present_only = RunPresenter(kFrames, success, suboptimal);
    CHECK(present_only.presented == kFrames && present_only.recreations == 1);

    // A clean frame ends the episode: a later suboptimal surface gets its one recreation again.
    const auto episodes = [](int, int frame) {
        return frame < 10 || frame >= 20 ? SwapResult::Suboptimal : SwapResult::Success;
    };
    const PresenterRun twice = RunPresenter(kFrames, episodes, episodes);
    CHECK(twice.presented == kFrames && twice.recreations == 2);
}

void TestOutOfDateAndFailures() {
    const auto success = [](int, int) { return SwapResult::Success; };
    const auto first_out_of_date = [](int generation, int) {
        return generation == 0 ? SwapResult::OutOfDate : SwapResult::Success;
    };
    const PresenterRun recovered = RunPresenter(kFrames, first_out_of_date, success);
    CHECK(recovered.presented == kFrames && recovered.skipped == 0 && recovered.recreations == 1);

    const PresenterRun present_stale = RunPresenter(kFrames, success, first_out_of_date);
    CHECK(present_stale.presented == kFrames - 1 && present_stale.recreations == 1);

    // Out-of-date and real failures are never turned into presented frames.
    for (const SwapResult hard : {SwapResult::OutOfDate, SwapResult::Failure}) {
        const auto always = [hard](int, int) { return hard; };
        const PresenterRun run = RunPresenter(kFrames, always, success);
        CHECK(run.presented == 0 && run.skipped == kFrames);
    }
    const auto out_of_date = [](int, int) { return SwapResult::OutOfDate; };
    CHECK(RunPresenter(kFrames, out_of_date, success).recreations >= kFrames);
}

void TestSwapchainUsesResultPolicy() {
    const std::string swapchain = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp");
    CHECK(Contains(swapchain, "    case vk::Result::eSuboptimalKHR:\n        return DcpPresent::SwapResult::Suboptimal;"));
    CHECK(InOrder(swapchain, "bool Swapchain::AcquireNextImage() {", "    const DcpPresent::SwapStep step = health.Acquire(kind);"));
    CHECK(InOrder(swapchain, "bool Swapchain::AcquireNextImage() {", "    return step.usable;\n}"));
    CHECK(InOrder(swapchain, "bool Swapchain::Present() {", "health.Present(kind)"));
    CHECK(InOrder(swapchain, "void Swapchain::Create(u32 width_, u32 height_) {", "    health.Recreated();"));
    CHECK(!Contains(swapchain, "needs_recreation"));
    const std::string presenter = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    CHECK(InOrder(presenter, "void Presenter::PresentLoadingFrame() {", "        swapchain.NeedsRecreation()) {"));
}

bool InsideSurface(const DcpPresent::Region& r, DcpPresent::Size surface) {
    return r.x >= 0 && r.y >= 0 && r.width > 0 && r.height > 0 && std::uint32_t(r.x) + r.width <= surface.width &&
           std::uint32_t(r.y) + r.height <= surface.height;
}

// DCP-2P6VZW: Gamescope forces a 3840x2160 swapchain under a 1920x1080 logical window. The frame size and the
// destination depend on the swapchain extent only, so the window size cannot reach them.
void TestPresentGeometryFollowsSurface() {
    using DcpPresent::FitRegion;
    using DcpPresent::GameFrameSize;
    using DcpPresent::Region;
    using DcpPresent::Size;
    const float wide = 16.0f / 9.0f;

    const Size gamescope_surface{3840, 2160};
    const Size frame = GameFrameSize(gamescope_surface, wide);
    CHECK((frame == Size{3840, 2160}));
    CHECK((FitRegion(frame, gamescope_surface) == Region{0, 0, 3840, 2160}));

    const Size desktop_surface{1920, 1080};
    const Size desktop_frame = GameFrameSize(desktop_surface, wide);
    CHECK((desktop_frame == Size{1920, 1080}));
    CHECK((FitRegion(desktop_frame, desktop_surface) == Region{0, 0, 1920, 1080}));

    // Same picture, different surface: the whole surface is used in both cases.
    CHECK(FitRegion(frame, gamescope_surface).width == 2 * FitRegion(desktop_frame, desktop_surface).width);

    CHECK((FitRegion(GameFrameSize({1280, 800}, wide), {1280, 800}) == Region{0, 40, 1280, 720}));
    CHECK((FitRegion(GameFrameSize({3440, 1440}, wide), {3440, 1440}) == Region{440, 0, 2560, 1440}));
}

void TestPresentGeometryAnySurface() {
    const DcpPresent::Size surfaces[] = {{1280, 720},  {1280, 800},  {1920, 1080}, {1920, 1200}, {2560, 1440},
                                         {3440, 1440}, {3840, 2160}, {5120, 1440}, {7680, 4320}, {1080, 1920}};
    const float ratios[] = {16.0f / 9.0f, 4.0f / 3.0f, 21.0f / 9.0f, 1.0f};
    for (const DcpPresent::Size surface : surfaces) {
        for (const float ratio : ratios) {
            const DcpPresent::Size frame = DcpPresent::GameFrameSize(surface, ratio);
            CHECK(frame.width > 0 && frame.height > 0);
            CHECK(frame.width <= surface.width && frame.height <= surface.height);
            const DcpPresent::Region region = DcpPresent::FitRegion(frame, surface);
            CHECK(InsideSurface(region, surface));
            // Letterboxed on one axis at most, centred on the other.
            CHECK(region.width == surface.width || region.height == surface.height);
            CHECK(std::uint32_t(region.x) * 2 + region.width <= surface.width + 1);
            CHECK(std::uint32_t(region.y) * 2 + region.height <= surface.height + 1);
        }
    }
}

void TestPresenterUsesSwapchainExtent() {
    const std::string presenter = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    const std::size_t at = presenter.find("void Presenter::Present(Frame* frame");
    CHECK(at != std::string::npos);
    const std::string body = presenter.substr(at);
    CHECK(Contains(body, "const vk::Extent2D extent = swapchain.GetExtent();\n        SetExpectedGameSize(s32(extent.width), s32(extent.height));"));
    CHECK(Contains(body, "MakeImageBlitStretch(frame->width, frame->height, extent.width, extent.height)"));
    CHECK(Contains(body, "MakeImageBlitFit(frame->width, frame->height, extent.width, extent.height)"));
    CHECK(Contains(presenter, "DcpPresent::GameFrameSize({u32(width), u32(height)}"));
    CHECK(Contains(presenter, "DcpPresent::FitRegion({u32(frame_width), u32(frame_height)}"));

    const std::string swapchain = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp");
    CHECK(Contains(swapchain, "    extent = capabilities.currentExtent;\n    if (capabilities.currentExtent.width == std::numeric_limits<u32>::max()) {"));
    CHECK(Contains(swapchain, ".imageExtent = extent,"));
}

DcpPresent::PresentGeometry GamescopeGeometry() {
    return {{3840, 2160}, {3840, 2160}, {3840, 2160}, {1920, 1080}, {0, 0, 3840, 2160}, false};
}

void TestGeometryReporter() {
    DcpPresent::GeometryReporter reporter;
    const DcpPresent::PresentGeometry geometry = GamescopeGeometry();
    unsigned lines = 0;
    for (std::uint64_t present = 1; present <= 5000; ++present) {
        lines += reporter.Observe(present, geometry);
    }
    CHECK(lines == 4); // presents 1, 10, 100 and 1000
    DcpPresent::PresentGeometry moved = geometry;
    moved.swapchain = {1920, 1080};
    CHECK(reporter.Observe(5001, moved));
    CHECK(!reporter.Observe(5002, moved));
    unsigned flapping = 0;
    for (std::uint64_t present = 6000; present < 6100; ++present) {
        moved.window.width = unsigned(present);
        flapping += reporter.Observe(present, moved);
    }
    CHECK(flapping <= 7); // changes are capped
}

void TestResultReporter() {
    DcpPresent::ResultReporter reporter;
    CHECK(reporter.Observe(0));
    CHECK(!reporter.Observe(0));
    CHECK(reporter.Observe(1000001003));
    CHECK(!reporter.Observe(1000001003));
    CHECK(reporter.Observe(0));
    unsigned lines = 0;
    for (int i = 0; i < 100; ++i) {
        lines += reporter.Observe(i % 2 ? 0 : 1000001003);
    }
    CHECK(lines == 5); // alternating results stop at 8 lines in all
    CHECK(std::string(DcpPresent::ResultName(0)) == "VK_SUCCESS");
    CHECK(std::string(DcpPresent::ResultName(1000001003)) == "VK_SUBOPTIMAL_KHR");
    CHECK(std::string(DcpPresent::ResultName(-1000001004)) == "VK_ERROR_OUT_OF_DATE_KHR");
    CHECK(std::string(DcpPresent::ResultName(DcpPresent::kNoResult)) == "none yet");
    CHECK(std::string(DcpPresent::ResultName(-77)) == "unexpected result");
}

void TestStallReporter() {
    using Action = DcpPresent::StallReporter::Action;
    DcpPresent::StallReporter reporter;
    CHECK(reporter.Update(false, 0) == Action::None);
    CHECK(reporter.Update(true, 3000) == Action::Report);
    CHECK(reporter.SinceMs() == 3000);
    for (std::int64_t now = 3250; now < 13000; now += 250) {
        CHECK(reporter.Update(true, now) == Action::None);
    }
    CHECK(reporter.Update(true, 13000) == Action::Report);
    CHECK(reporter.Update(true, 15000) == Action::None);
    CHECK(reporter.Update(false, 16000) == Action::Resumed);
    CHECK(reporter.Update(false, 16250) == Action::None);
    CHECK(reporter.Update(true, 20000) == Action::Report);
}

void TestProgressPacking() {
    const DcpPresent::Size size{3840, 2160};
    CHECK(DcpPresent::Unpack(DcpPresent::Pack(size)) == size);
    CHECK(DcpPresent::g_progress.last_present.load() == DcpPresent::kNoResult);
}

void TestTelemetryIsGamescopeOnlyAndShippable() {
    const char* files[] = {"gpu/shim/dcp_startup_watch.cpp", "gpu/shim/dcp_startup_watch.h",
                           "gpu/shim/window.cpp", "gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp",
                           "gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.h"};
    const std::string temporary = std::string("DEBUG_") + "SAULO";
    for (const char* file : files) {
        CHECK(!Contains(ReadSource(file), temporary));
    }
    const std::string window = ReadSource("gpu/shim/window.cpp");
    CHECK(InOrder(window, "void WindowSDL::LogGamescopeWindow() {", "if (!DcpPresent::GamescopeSession()) {"));
    const std::string swapchain = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp");
    CHECK(Contains(swapchain, "if (!gamescope || logged_creations >= 16) {"));
    CHECK(Contains(swapchain, "if (!gamescope || !(presenting ? present_reporter : acquire_reporter).Observe(code)) {"));
    const std::string watch = ReadSource("gpu/shim/dcp_startup_watch.cpp");
    CHECK(Contains(watch, "if (DcpPresent::GamescopeSession() && !started.exchange(true)) {"));
    CHECK(Contains(watch, "cursor < 0 || !DcpPresent::GamescopeSession()"));
    const std::string presenter = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    CHECK(InOrder(presenter, "void Presenter::ReportPresentGeometry(", "if (!DcpPresent::GamescopeSession()) {"));
}

void TestPadSamplingUntouched() {
    const std::string pad = ReadSource("src/runtime_pad.c");
    CHECK(!Contains(pad, "Gamescope") && !Contains(pad, "gamescope"));
    // The trace only reads the sampled state, after sampling, and only when switched on.
    CHECK(Contains(pad, "static void trace_read(const PadData *d) {"));
    CHECK(Contains(pad, "    if (!dcpgpu_latency_trace_enabled()) return;"));
    CHECK(Contains(pad, "    sample(slot,data); ++reads;\n    if (slot==0) {\n        trace_read(data);\n"));
    CHECK(!Contains(pad, "DCP_INPUT_LATENCY_TRACE\")"));
}

} // namespace

int main() {
    TestDefaultImageCount();
    TestGamescopeImageCount();
    TestPresentMode();
    TestGamescopeDetection();
    TestLatencyTraceSwitch();
    TestSessionDescription();
    TestFlipPacingModel();
    TestFlipPacingIsGamescopeOnly();
    TestSwapchainRecreation();
    TestPadSamplingUntouched();
    TestAcquireResults();
    TestPresentResults();
    TestPersistentSuboptimalPresents();
    TestSuboptimalRecovery();
    TestOutOfDateAndFailures();
    TestSwapchainUsesResultPolicy();
    TestPresentGeometryFollowsSurface();
    TestPresentGeometryAnySurface();
    TestPresenterUsesSwapchainExtent();
    TestGeometryReporter();
    TestResultReporter();
    TestStallReporter();
    TestProgressPacking();
    TestTelemetryIsGamescopeOnlyAndShippable();
    if (g_failures) {
        std::printf("%d present test(s) failed\n", g_failures);
        return 1;
    }
    std::puts("present tests passed");
    return 0;
}
