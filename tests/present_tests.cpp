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
    CHECK(InOrder(swapchain, "bool Swapchain::Recreate(u32 width_, u32 height_) {", "    return Create(width_, height_);"));
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
    CHECK(InOrder(swapchain, "bool Swapchain::Create(u32 width_, u32 height_) {", "    health.Recreated();"));
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

using DcpPresent::Lifecycle;
using DcpPresent::PresentGate;
using DcpPresent::SwapchainHealth;
using DcpPresent::WindowEvent;
using DcpPresent::WindowVisibility;

// Presenter::Present's lifecycle control flow, one call per swap-thread frame: gate, recreate when the window size
// changed or a recreation is pending (never against a surface without area), acquire, present.
struct LifecycleModel {
    WindowVisibility visibility;
    std::uint32_t window_width = 1920;
    std::uint32_t window_height = 1080;
    bool surface_zero = false;
    bool vsync_changed = false;

    PresentGate gate;
    SwapchainHealth health;
    std::uint32_t swapchain_width = 1920;
    std::uint32_t swapchain_height = 1080;
    int acquires = 0;
    int presents = 0;
    int recreations = 0;
    int zero_surface_recreations = 0;
    int acquires_while_unpresentable = 0;
    int resumed = 0;

    void Apply(WindowEvent event) {
        visibility.Apply(event);
    }

    bool Recreate() {
        if (surface_zero) {
            ++zero_surface_recreations;
        }
        health.Recreated();
        swapchain_width = window_width;
        swapchain_height = window_height;
        ++recreations;
        return true;
    }

    // Swapchain::Create refuses a surface without area and keeps a recreation pending.
    bool TryRecreate() {
        if (surface_zero) {
            gate.SurfaceUnavailable();
            health.RequestRecreation();
            return false;
        }
        return Recreate();
    }

    void Frame(SwapResult acquire = SwapResult::Success, SwapResult present = SwapResult::Success) {
        const bool check_surface = gate.Suspended() || health.NeedsRecreation();
        const auto step = gate.Update({visibility, window_width, window_height, check_surface && surface_zero});
        if (step.recreate) {
            health.RequestRecreation();
        }
        if (!step.present) {
            return;
        }
        if (vsync_changed) {
            vsync_changed = false;
            if (!TryRecreate()) {
                return;
            }
        }
        if (window_width != swapchain_width || window_height != swapchain_height || health.NeedsRecreation()) {
            if (!TryRecreate()) {
                return;
            }
        }
        ++acquires;
        if (visibility.minimized || visibility.hidden || surface_zero) {
            ++acquires_while_unpresentable;
        }
        if (!health.Acquire(acquire).usable) {
            if (!TryRecreate()) {
                return;
            }
            health.Acquire(SwapResult::Success);
        }
        const bool shown = health.Present(present).usable;
        presents += shown;
        if (health.NeedsRecreation()) {
            TryRecreate();
        }
        if (shown && gate.Presented()) {
            ++resumed;
        }
    }

    void Frames(int count) {
        for (int i = 0; i < count; ++i) {
            Frame();
        }
    }
};

void TestWindowVisibilityEvents() {
    WindowVisibility v;
    v.Apply(WindowEvent::Minimized);
    CHECK(v.minimized && !v.hidden);
    v.Apply(WindowEvent::Restored);
    CHECK(!v.minimized);
    v.Apply(WindowEvent::Hidden);
    CHECK(v.hidden && !v.minimized);
    v.Apply(WindowEvent::Shown);
    CHECK(!v.hidden);
    v.Apply(WindowEvent::Minimized);
    v.Apply(WindowEvent::Hidden);
    v.Apply(WindowEvent::Restored);
    v.Apply(WindowEvent::Shown);
    CHECK(!v.minimized && !v.hidden);
    for (int bits = 0; bits < 4; ++bits) {
        CHECK(WindowVisibility::Unpack(std::uint8_t(bits)).Pack() == bits);
    }
}

// Alt+Tab without a minimize is FOCUS_LOST / FOCUS_GAINED only: focus is not an input of the gate.
void TestFocusAloneKeepsPresenting() {
    LifecycleModel m;
    m.Frames(305);
    CHECK(m.presents == 305 && m.recreations == 0);
    CHECK(m.gate.Current() == Lifecycle::Presentable);
}

void TestMinimizeRestore() {
    LifecycleModel m;
    m.Frames(10);
    m.Apply(WindowEvent::Minimized);
    m.surface_zero = true;
    m.Frames(120);
    CHECK(m.presents == 10 && m.acquires == 10 && m.acquires_while_unpresentable == 0);
    CHECK(m.recreations == 0 && m.gate.Current() == Lifecycle::Minimized);
    m.Apply(WindowEvent::Restored);
    m.Apply(WindowEvent::Shown);
    m.surface_zero = false;
    m.Frames(120);
    CHECK(m.presents == 130 && m.recreations == 1 && m.resumed == 1 && m.zero_surface_recreations == 0);
    CHECK(m.gate.Current() == Lifecycle::Presentable);
}

void TestHiddenShown() {
    LifecycleModel m;
    m.Frames(5);
    m.Apply(WindowEvent::Hidden);
    m.Frames(50);
    CHECK(m.presents == 5 && m.gate.Current() == Lifecycle::Hidden);
    m.Apply(WindowEvent::Shown);
    m.Frames(50);
    CHECK(m.presents == 55 && m.recreations == 1);
}

// SDL keeps the last pixel size while minimized on Windows, so a window size check alone never fires: the Vulkan
// surface decides, even before the window thread has pumped the minimize.
void TestSurfaceZeroBeforeMinimizeEvent() {
    LifecycleModel m;
    m.Frames(3);
    m.surface_zero = true;
    m.Frame(SwapResult::OutOfDate);
    CHECK(m.recreations == 0 && m.zero_surface_recreations == 0 && m.gate.Current() == Lifecycle::SurfaceSizeZero);
    m.Frames(100);
    CHECK(m.presents == 3 && m.recreations == 0);
    m.surface_zero = false;
    m.Frames(10);
    CHECK(m.presents == 13 && m.recreations == 1);
}

void TestZeroWindowSizeThenValid() {
    LifecycleModel m;
    m.Frames(5);
    m.window_width = 0;
    m.window_height = 0;
    m.Frames(30);
    CHECK(m.presents == 5 && m.recreations == 0 && m.gate.Current() == Lifecycle::WindowSizeZero);
    m.window_width = 1280;
    m.window_height = 720;
    m.Frames(30);
    CHECK(m.presents == 35 && m.recreations == 1 && m.swapchain_width == 1280);
}

void TestRestoreEventsBeforeValidSize() {
    LifecycleModel m;
    m.Frames(5);
    m.Apply(WindowEvent::Minimized);
    m.surface_zero = true;
    m.Frames(10);
    for (int i = 0; i < 5; ++i) {
        m.Apply(WindowEvent::Restored);
        m.Apply(WindowEvent::Shown);
        m.Frames(5);
    }
    CHECK(m.recreations == 0 && m.zero_surface_recreations == 0 && m.presents == 5);
    CHECK(m.acquires_while_unpresentable == 0 && m.gate.Current() == Lifecycle::SurfaceSizeZero);
    m.surface_zero = false;
    m.Frames(20);
    CHECK(m.recreations == 1 && m.presents == 25 && m.resumed == 1);
}

void TestRestoreClusterRecreatesOnce() {
    LifecycleModel m;
    m.Frames(5);
    m.Apply(WindowEvent::Minimized);
    m.Frames(5);
    // RESTORED + SHOWN + RESIZED + PIXEL_SIZE_CHANGED between two frames: one recreation, not four.
    m.Apply(WindowEvent::Restored);
    m.Apply(WindowEvent::Shown);
    m.window_width = 1920;
    m.window_height = 1080;
    m.Frames(60);
    CHECK(m.recreations == 1 && m.presents == 65);
}

void TestMinimizeWithPendingRecreation() {
    LifecycleModel m;
    m.Frames(5);
    m.Frame(SwapResult::Suboptimal, SwapResult::Suboptimal);
    CHECK(m.recreations == 1);
    m.Apply(WindowEvent::Minimized);
    m.surface_zero = true;
    m.health.RequestRecreation();
    m.Frames(40);
    CHECK(m.recreations == 1 && m.acquires_while_unpresentable == 0);
    m.Apply(WindowEvent::Restored);
    m.surface_zero = false;
    m.Frames(40);
    CHECK(m.recreations == 2 && m.presents == 5 + 1 + 40);
}

void TestRestoreAfterOutOfDate() {
    LifecycleModel m;
    m.Frames(5);
    m.Frame(SwapResult::Success, SwapResult::OutOfDate);
    CHECK(m.recreations == 1);
    m.Apply(WindowEvent::Minimized);
    m.surface_zero = true;
    m.Frame(SwapResult::OutOfDate);
    m.Frames(20);
    m.Apply(WindowEvent::Restored);
    m.surface_zero = false;
    m.Frames(20);
    CHECK(m.recreations == 2 && m.presents == 5 + 20 && m.zero_surface_recreations == 0);
}

void TestRestoreWithVsyncChange() {
    LifecycleModel m;
    m.Frames(5);
    m.Apply(WindowEvent::Minimized);
    m.surface_zero = true;
    m.vsync_changed = true;
    m.Frames(10);
    m.Apply(WindowEvent::Restored);
    m.surface_zero = false;
    m.Frames(20);
    CHECK(m.presents == 25 && m.zero_surface_recreations == 0);
    CHECK(m.recreations <= 2); // the vsync change and the restore may each ask once, never a storm
}

// F11 changes the window size; Mailbox, Immediate and FIFO all follow the same lifecycle.
void TestDisplayModeChangeAndModes() {
    for (const bool vsync : {true, false}) {
        for (const bool has_mailbox : {true, false}) {
            const Mode mode = DcpPresent::ChooseMode(vsync, has_mailbox, true);
            CHECK(vsync ? mode == Mode::Fifo : mode == (has_mailbox ? Mode::Mailbox : Mode::Immediate));
            LifecycleModel m;
            m.Frames(10);
            m.window_width = 2560;
            m.window_height = 1440;
            m.Frames(10);
            m.Apply(WindowEvent::Minimized);
            m.surface_zero = true;
            m.Frames(10);
            m.Apply(WindowEvent::Restored);
            m.surface_zero = false;
            m.Frames(10);
            CHECK(m.recreations == 2 && m.presents == 30);
        }
    }
}

void TestRepeatedMinimizeRestoreNoStorm() {
    LifecycleModel m;
    for (int cycle = 0; cycle < 25; ++cycle) {
        m.Frames(5);
        m.Apply(WindowEvent::Minimized);
        m.surface_zero = true;
        m.Frames(30);
        m.Apply(WindowEvent::Restored);
        m.Apply(WindowEvent::Shown);
        m.surface_zero = false;
        m.Frames(5);
    }
    CHECK(m.recreations == 25 && m.resumed == 25 && m.acquires_while_unpresentable == 0);
}

void TestSurfaceArea() {
    using DcpPresent::SurfaceHasArea;
    CHECK(SurfaceHasArea(1920, 1080, 16384, 16384));
    CHECK(!SurfaceHasArea(0, 0, 16384, 16384));
    CHECK(!SurfaceHasArea(1920, 0, 16384, 16384));
    CHECK(!SurfaceHasArea(1920, 1080, 0, 0));
    CHECK(SurfaceHasArea(UINT32_MAX, UINT32_MAX, 4096, 4096));
    CHECK(!SurfaceHasArea(UINT32_MAX, UINT32_MAX, 0, 0));
}

void TestSwapchainRefusesZeroSurface() {
    const std::string swapchain = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp");
    CHECK(InOrder(swapchain, "bool Swapchain::Create(u32 width_, u32 height_) {", "if (!SurfaceHasArea()) {"));
    CHECK(InOrder(swapchain, "if (!SurfaceHasArea()) {", "        return false;"));
    CHECK(InOrder(swapchain, "        return false;", "    Destroy();\n"));
    CHECK(Contains(swapchain, "health.RequestRecreation();\n        PublishHealth();\n        return false;"));
}

void TestPresentOrdering() {
    const std::string presenter = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    const std::size_t at = presenter.find("void Presenter::Present(Frame* frame");
    CHECK(at != std::string::npos);
    const std::string body = presenter.substr(at, presenter.find("DcpPresent::WindowVisibility Presenter::GateVisibility") - at);
    // Suspend, then recreate, then acquire; nothing is acquired while suspended.
    CHECK(InOrder(body, "if (!CanPresent()) {", "swapchain.UpdateVsync();"));
    CHECK(InOrder(body, "swapchain.UpdateVsync();", "if (!RecreateSwapchain()) {"));
    CHECK(InOrder(body, "if (!RecreateSwapchain()) {", "acquired = swapchain.AcquireNextImage();"));
    CHECK(InOrder(body, "acquired = swapchain.AcquireNextImage();", "presented = swapchain.Present();"));
    // After the acquire retry, up to the present, one frame never recreates the swapchain: no frame meets a newer
    // swapchain generation than the one it acquired from.
    const std::size_t reset = body.find("const auto reset_result");
    const std::size_t present = body.find("presented = swapchain.Present();");
    CHECK(reset != std::string::npos && present != std::string::npos && reset < present);
    CHECK(body.substr(reset, present - reset).find("Recreate") == std::string::npos);
    CHECK(!Contains(body, "swapchain.Recreate("));
    CHECK(InOrder(body, "std::scoped_lock submit_lock{Scheduler::submit_mutex};", "presented = swapchain.Present();"));
    const std::string swapchain = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp");
    CHECK(InOrder(swapchain, "static void WaitDeviceIdle(", "std::scoped_lock lock{Scheduler::submit_mutex};"));
    CHECK(InOrder(swapchain, "void Swapchain::Destroy() {", "WaitDeviceIdle(instance, \"before destroying the swapchain\");"));
    CHECK(InOrder(swapchain, "WaitDeviceIdle(instance, \"before destroying the swapchain\");", "device.destroySwapchainKHR(swapchain);"));
    const std::size_t loading = presenter.find("void Presenter::PresentLoadingFrame() {");
    CHECK(loading != std::string::npos);
    CHECK(InOrder(presenter.substr(loading), "if (!swapchain.Recreate(window.GetWidth(), window.GetHeight())) {",
                  "if (!swapchain.AcquireNextImage()) {"));
}

void TestPresentResultMapping() {
    const std::string swapchain = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp");
    const std::size_t at = swapchain.find("DcpPresent::SwapResult Swapchain::PresentResult(vk::Result result) {");
    CHECK(at != std::string::npos);
    const std::string body = swapchain.substr(at, swapchain.find("void Swapchain::ReportFatalResult") - at);
    CHECK(Contains(body, "    case vk::Result::eErrorSurfaceLostKHR:\n    case vk::Result::eErrorOutOfDateKHR:\n        return DcpPresent::SwapResult::OutOfDate;"));
    CHECK(!Contains(body, "eErrorDeviceLost"));
    CHECK(InOrder(swapchain, "if (kind == DcpPresent::SwapResult::Failure) {\n        ReportFatalResult(result);",
                  "ASSERT_MSG(kind != DcpPresent::SwapResult::Failure, \"Swapchain presentation failed"));
}

void TestDeviceLostReport() {
    DcpPresent::Progress progress;
    progress.lifecycle = int(Lifecycle::Restoring);
    progress.window_size = DcpPresent::Pack({1920, 1080});
    progress.swapchain_size = DcpPresent::Pack({1920, 1080});
    progress.swapchain_generation = 3;
    progress.recreation_pending = false;
    progress.last_acquire = 0;
    progress.last_present = 1000001003;
    const std::string line = DcpPresent::DescribeDeviceLost("submit role=present", progress);
    CHECK(line == "GPU device lost: submit role=present lifecycle=restoring window=1920x1080 swapchain=1920x1080 "
                  "generation=3 recreation_pending=no acquire=VK_SUCCESS present=VK_SUBOPTIMAL_KHR");
    const std::string scheduler = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp");
    CHECK(InOrder(scheduler, "if (submit_result == vk::Result::eErrorDeviceLost) {",
                  "ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, \"Device lost during submit\");"));
}

void TestWindowActiveUntouched() {
    const std::string window = ReadSource("gpu/shim/window.cpp");
    CHECK(Contains(window, "DcpPresent::g_progress.window_active = !visibility.minimized && !visibility.hidden;"));
    CHECK(Contains(ReadSource("gpu/shim/dcp_startup_watch.cpp"), "progress.window_active.load()"));
    CHECK(Contains(ReadSource("gpu/shim/sdl_window.h"), "DcpPresent::WindowVisibility Visibility() const {"));
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

void TestPresentGapTelemetry() {
    DcpPresent::StallMonitor monitor;

    // Normal 60/120 FPS cadence stays completely silent.
    for (int i = 0; i < 120; ++i) {
        const std::int64_t t = 1000 + i * 8;
        CHECK(monitor.ObservePresent(t, static_cast<std::uint64_t>(i / 2), 1, 0, 0).empty());
        CHECK(monitor.ObserveVideoOut(t, 1, 0, 0).empty());
        CHECK(monitor.ObserveGuestSubmit(t, static_cast<std::uint64_t>(i / 2), 1).empty());
    }
    CHECK(monitor.ObservePipelineStall(2000, "pipeline_create", 120, 90, false, false).empty());

    // A >500 ms present gap logs once with acquire/present results and source/swapchain change state.
    const std::string gap1 = monitor.ObservePresent(2600, 60, 1, 0, 0);
    CHECK(Contains(gap1, "Presenter telemetry: event=present_gap severity=notable duration_ms=648"));
    CHECK(Contains(gap1, "acquire=VK_SUCCESS present=VK_SUCCESS"));
    CHECK(Contains(gap1, "source_changed=yes swapchain_changed=no swapchain_gen=1"));
    CHECK(Contains(gap1, "recent_pipeline_stall_ms=0 count=1"));

    // Repeated notable gaps are bounded (powers of ten: 2..9 silent, 10 logged).
    std::int64_t t = 2600;
    for (int n = 2; n <= 9; ++n) {
        t += 600;
        CHECK(monitor.ObservePresent(t, 60, 1, 0, 0).empty());
    }
    t += 600;
    const std::string gap10 = monitor.ObservePresent(t, 60, 1, 0, 0);
    CHECK(Contains(gap10, "event=present_gap severity=notable"));
    CHECK(Contains(gap10, "source_changed=no"));
    CHECK(Contains(gap10, "count=10"));

    // A severe >=2000 ms pipeline creation stall is logged and correlated with the following present gap.
    const std::string pipe_stall =
        monitor.ObservePipelineStall(t + 2400, "pipeline_create", 2400, 2350, false, false);
    CHECK(Contains(pipe_stall, "GPU stall telemetry: event=pipeline_create severity=severe duration_ms=2400"));
    CHECK(Contains(pipe_stall, "driver_ms=2350 mixed_samples=no preloading=no count=1"));

    const std::string severe_gap = monitor.ObservePresent(t + 2500, 61, 2, 1000001003, 0);
    CHECK(Contains(severe_gap, "Presenter telemetry: event=present_gap severity=severe duration_ms=2500"));
    CHECK(Contains(severe_gap, "acquire=VK_SUBOPTIMAL_KHR present=VK_SUCCESS"));
    CHECK(Contains(severe_gap, "source_changed=yes swapchain_changed=yes swapchain_gen=2"));
    CHECK(Contains(severe_gap, "recent_pipeline_stall_ms=2400"));

    // VideoOut and guest submit gaps also correlate with recent pipeline stalls.
    const std::string vo_gap = monitor.ObserveVideoOut(t + 2500, 2, 0, 0);
    CHECK(Contains(vo_gap, "Presenter telemetry: event=videoout_gap severity=severe"));
    CHECK(Contains(vo_gap, "recent_pipeline_stall_ms=2400"));

    const std::string submit_gap = monitor.ObserveGuestSubmit(t + 2500, 61, 2);
    CHECK(Contains(submit_gap, "Presenter telemetry: event=guest_submit_gap severity=severe"));
    CHECK(Contains(submit_gap, "recent_pipeline_stall_ms=2400"));

    // Verify call sites are wired in renderer and VideoOut.
    const std::string pipeline = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_graphics_pipeline.cpp");
    CHECK(Contains(pipeline, "DcpPresent::RecordPipelineStall(\"pipeline_create\""));
    const std::string cache = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp");
    CHECK(Contains(cache, "DcpPresent::RecordPipelineStall(\"pipeline_cache_compile\""));
    const std::string presenter = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    CHECK(Contains(presenter, "DcpPresent::RecordVideoOut();"));
    CHECK(Contains(presenter, "DcpPresent::RecordPresent();"));
    const std::string driver = ReadSource("gpu/shadps4/core/libraries/videoout/driver.cpp");
    CHECK(Contains(driver, "DcpPresent::RecordGuestSubmit();"));
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
    TestWindowVisibilityEvents();
    TestFocusAloneKeepsPresenting();
    TestMinimizeRestore();
    TestHiddenShown();
    TestSurfaceZeroBeforeMinimizeEvent();
    TestZeroWindowSizeThenValid();
    TestRestoreEventsBeforeValidSize();
    TestRestoreClusterRecreatesOnce();
    TestMinimizeWithPendingRecreation();
    TestRestoreAfterOutOfDate();
    TestRestoreWithVsyncChange();
    TestDisplayModeChangeAndModes();
    TestRepeatedMinimizeRestoreNoStorm();
    TestSurfaceArea();
    TestSwapchainRefusesZeroSurface();
    TestPresentOrdering();
    TestPresentResultMapping();
    TestDeviceLostReport();
    TestWindowActiveUntouched();
    TestPresentGapTelemetry();
    if (g_failures) {
        std::printf("%d present test(s) failed\n", g_failures);
        return 1;
    }
    std::puts("present tests passed");
    return 0;
}
