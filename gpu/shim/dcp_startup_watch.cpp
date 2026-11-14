// SPDX-License-Identifier: GPL-2.0-or-later
// DEBUG_SAULO: temporary startup and stall telemetry (dcp_startup_watch.h).
#include "dcp_startup_watch.h"

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
constexpr std::array<const char*, WaitCount> kWaitNames{"free frame", "frame fence", "acquire", "queue present"};

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

void Report(const char* reason, std::int64_t now, std::int64_t stalled_since,
            const std::array<std::uint64_t, StageCount>& seen,
            const std::array<std::int64_t, StageCount>& advanced) {
    char ages[512];
    int used = 0;
    for (unsigned s = 0; s < StageCount && used < int(sizeof(ages)); ++s) {
        used += seen[s] ? std::snprintf(ages + used, sizeof(ages) - used, "%s%s %lld ms", s ? ", " : "",
                                        kStageNames[s], static_cast<long long>(now - advanced[s]))
                        : std::snprintf(ages + used, sizeof(ages) - used, "%s%s never", s ? ", " : "",
                                        kStageNames[s]);
    }
    std::printf("DEBUG_SAULO stall: %s for %.1f s; since last: %s\n", reason, (now - stalled_since) / 1000.0, ages);

    char waiting[256];
    used = 0;
    for (unsigned w = 0; w < WaitCount && used < int(sizeof(waiting)); ++w) {
        const auto since = waits[w].since_ms.load(std::memory_order_relaxed);
        if (since) {
            used += std::snprintf(waiting + used, sizeof(waiting) - used, " %s %lld ms (thread %u);", kWaitNames[w],
                                  static_cast<long long>(now - since), waits[w].tid.load(std::memory_order_relaxed));
        }
    }
    std::printf("DEBUG_SAULO stall: counts submit %llu, GPU %llu, packets %llu, prepared %llu, taken %llu, "
                "presents %llu, vblanks %llu, title ticks %llu; free frames %lld, pending flips %lld, swap queue %lld, "
                "stage B in packet %s, title cursor %lld; waiting:%s\n",
                static_cast<unsigned long long>(seen[GuestSubmit]), static_cast<unsigned long long>(seen[GpuSubmission]),
                static_cast<unsigned long long>(seen[StageBPacket]), static_cast<unsigned long long>(seen[FlipPrepared]),
                static_cast<unsigned long long>(seen[FlipTaken]), static_cast<unsigned long long>(seen[Present]),
                static_cast<unsigned long long>(seen[Vblank]), static_cast<unsigned long long>(seen[TitleTick]),
                static_cast<long long>(gauges[FreeFrames].load(std::memory_order_relaxed)),
                static_cast<long long>(gauges[PendingFlips].load(std::memory_order_relaxed)),
                static_cast<long long>(gauges[SwapQueue].load(std::memory_order_relaxed)),
                gauges[StageBBusy].load(std::memory_order_relaxed) ? "yes" : "no",
                static_cast<long long>(gauges[TitleCursor].load(std::memory_order_relaxed)),
                used ? waiting : " nothing");
}

void WatchLoop() {
    constexpr std::int64_t kStallMs = 3000, kRepeatMs = 10000, kFirstPresentMs = 20000;
    std::array<std::uint64_t, StageCount> seen{};
    std::array<std::int64_t, StageCount> advanced{};
    const std::int64_t start = NowMs();
    advanced.fill(start);
    std::int64_t stalled_since = 0, last_report = 0;
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
        const char* reason = nullptr;
        if (!seen[Present]) {
            reason = now - start >= kFirstPresentMs ? "no present since the video core started" : nullptr;
        } else if (now - advanced[Present] >= kStallMs) {
            reason = "no present";
        } else if (now - advanced[GuestSubmit] >= kStallMs) {
            reason = "no guest GPU submission";
        } else if (now - advanced[Vblank] >= kStallMs) {
            reason = "no vblank";
        }
        if (!reason) {
            if (stalled_since) {
                std::printf("DEBUG_SAULO stall: resumed after %.1f s\n", (now - stalled_since) / 1000.0);
                stalled_since = 0;
            }
            continue;
        }
        if (!stalled_since) {
            stalled_since = now;
            last_report = 0;
        }
        if (last_report && now - last_report < kRepeatMs) {
            continue;
        }
        last_report = now;
        Report(reason, now, stalled_since, seen, advanced);
    }
}

} // namespace

void Mark(Stage stage) {
    if (counts[stage].fetch_add(1, std::memory_order_relaxed) == 0) {
        std::printf("DEBUG_SAULO startup: first %s (thread %u)\n", kStageNames[stage], ThreadId());
    }
}

void SetGauge(Gauge gauge, std::int64_t value) {
    gauges[gauge].store(value, std::memory_order_relaxed);
}

void StartWatch() {
    if (!started.exchange(true)) {
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
    const auto previous = gauges[TitleCursor].exchange(cursor, std::memory_order_relaxed);
    static std::atomic<int> logged{0};
    if (previous != cursor && counts[TitleTick].load(std::memory_order_relaxed) > 1 &&
        logged.fetch_add(1, std::memory_order_relaxed) < 16) {
        std::printf("DEBUG_SAULO title: cursor %lld -> %d\n", static_cast<long long>(previous), cursor);
    }
}
