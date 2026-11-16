// SPDX-License-Identifier: GPL-2.0-or-later
// Presenter progress telemetry for Gamescope sessions (dcp_startup_watch.h).
#include "dcp_startup_watch.h"

#include "dcp_present.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace DcpStartupWatch {
namespace {

constexpr std::array<const char*, StageCount> kStageNames{
    "guest submit", "GPU submission", "stage B packet", "flip prepared",
    "flip taken",   "present",        "vblank",         "title tick",
};

struct WaitSlot {
    std::atomic<std::int64_t> since_ms{0};
    std::atomic<std::uint32_t> tid{0};
};

std::array<std::atomic<std::uint64_t>, StageCount> counts{};
std::array<std::atomic<std::int64_t>, GaugeCount> gauges{};
std::array<WaitSlot, WaitCount> waits{};
std::atomic<bool> started{false};

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::uint32_t ThreadId() {
    return static_cast<std::uint32_t>(gettid());
}

const char* Blocked(Wait wait, std::int64_t now, char* text, std::size_t size) {
    const auto since = waits[wait].since_ms.load(std::memory_order_relaxed);
    if (!since) {
        return "no";
    }
    std::snprintf(text, size, "yes, %lld ms", static_cast<long long>(now - since));
    return text;
}

void Report(const char* reason, std::int64_t now, std::int64_t stalled_since, const std::array<std::uint64_t, StageCount>& seen) {
    const auto& progress = DcpPresent::g_progress;
    const DcpPresent::Size swapchain = DcpPresent::Unpack(progress.swapchain_size.load());
    char acquire_wait[32], present_wait[32];
    std::printf("Presenter diagnostic: %s for %.1f s, last successful present %llu, last acquire %s, last present %s, "
                "swapchain %ux%u, acquire blocked %s, present blocked %s\n",
                reason, (now - stalled_since) / 1000.0, static_cast<unsigned long long>(progress.presented.load()),
                DcpPresent::ResultName(progress.last_acquire.load()), DcpPresent::ResultName(progress.last_present.load()),
                swapchain.width, swapchain.height, Blocked(Acquire, now, acquire_wait, sizeof(acquire_wait)),
                Blocked(QueuePresent, now, present_wait, sizeof(present_wait)));
    std::printf("Presenter diagnostic: guest submits %llu, flips prepared %llu, flips taken %llu, presenter calls %llu, "
                "vblanks %llu, pending flips %lld, swap queue %lld, free frames %lld\n",
                static_cast<unsigned long long>(seen[GuestSubmit]), static_cast<unsigned long long>(seen[FlipPrepared]),
                static_cast<unsigned long long>(seen[FlipTaken]), static_cast<unsigned long long>(seen[Present]),
                static_cast<unsigned long long>(seen[Vblank]),
                static_cast<long long>(gauges[PendingFlips].load(std::memory_order_relaxed)),
                static_cast<long long>(gauges[SwapQueue].load(std::memory_order_relaxed)),
                static_cast<long long>(gauges[FreeFrames].load(std::memory_order_relaxed)));
}

void WatchLoop() {
    constexpr std::int64_t kStallMs = 3000, kFirstPresentMs = 20000;
    std::array<std::uint64_t, StageCount> seen{};
    std::array<std::int64_t, StageCount> advanced{};
    const std::int64_t start = NowMs();
    advanced.fill(start);
    std::uint64_t presented_seen = 0;
    std::int64_t presented_advanced = start;
    DcpPresent::StallReporter reporter;
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const std::int64_t now = NowMs();
        for (unsigned s = 0; s < StageCount; ++s) {
            const auto count = counts[s].load(std::memory_order_relaxed);
            if (count != seen[s]) {
                seen[s] = count;
                advanced[s] = now;
            }
        }
        const auto& progress = DcpPresent::g_progress;
        if (const auto presented = progress.presented.load(); presented != presented_seen) {
            presented_seen = presented;
            presented_advanced = now;
        }
        const bool submitting = now - advanced[GuestSubmit] < kStallMs;
        const bool active = progress.window_active.load();
        const std::int64_t limit = presented_seen ? kStallMs : kFirstPresentMs;
        const bool stalled = submitting && active && now - presented_advanced >= limit;
        switch (reporter.Update(stalled, now)) {
        case DcpPresent::StallReporter::Action::Report:
            Report(presented_seen ? "no successful present" : "no successful present since start", now,
                   reporter.SinceMs(), seen);
            break;
        case DcpPresent::StallReporter::Action::Resumed:
            std::printf("Presenter diagnostic: presenting resumed, present %llu\n",
                        static_cast<unsigned long long>(presented_seen));
            break;
        case DcpPresent::StallReporter::Action::None:
            break;
        }
    }
}

} // namespace

void Mark(Stage stage) {
    if (counts[stage].fetch_add(1, std::memory_order_relaxed) == 0 && DcpPresent::GamescopeSession()) {
        std::printf("Presenter diagnostic: first %s\n", kStageNames[stage]);
    }
}

void SetGauge(Gauge gauge, std::int64_t value) {
    gauges[gauge].store(value, std::memory_order_relaxed);
}

void StartWatch() {
    if (DcpPresent::GamescopeSession() && !started.exchange(true)) {
        std::thread(WatchLoop).detach();
    }
}

WaitScope::WaitScope(Wait wait_) : wait{wait_}, tid{ThreadId()} {
    waits[wait].tid.store(tid, std::memory_order_relaxed);
    waits[wait].since_ms.store(NowMs(), std::memory_order_relaxed);
}

WaitScope::~WaitScope() {
    if (waits[wait].tid.load(std::memory_order_relaxed) == tid) {
        waits[wait].since_ms.store(0, std::memory_order_relaxed);
    }
}

} // namespace DcpStartupWatch

extern "C" void dcpgpu_debug_title_tick(int cursor) {
    using namespace DcpStartupWatch;
    Mark(TitleTick);
    gauges[TitleCursor].store(cursor, std::memory_order_relaxed);
    static std::atomic<bool> reported{false};
    if (cursor < 0 || !DcpPresent::GamescopeSession() || reported.exchange(true)) {
        return;
    }
    const auto& progress = DcpPresent::g_progress;
    const DcpPresent::Size source = DcpPresent::Unpack(progress.source_size.load());
    const DcpPresent::Size destination = DcpPresent::Unpack(progress.destination_size.load());
    std::printf("Presenter diagnostic: title reached, VideoOut images=%llu blitted=%llu presents=%llu source=%ux%u "
                "destination=%ux%u last_acquire=%s last_present=%s\n",
                static_cast<unsigned long long>(progress.videoout_images.load()),
                static_cast<unsigned long long>(progress.blitted.load()),
                static_cast<unsigned long long>(progress.presented.load()), source.width, source.height,
                destination.width, destination.height, DcpPresent::ResultName(progress.last_acquire.load()),
                DcpPresent::ResultName(progress.last_present.load()));
}
