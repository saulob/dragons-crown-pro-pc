// SPDX-License-Identifier: GPL-2.0-or-later
// Presenter progress telemetry, active in Gamescope sessions only: the first event of each pipeline stage, a report when
// presenting stalls while the guest keeps submitting, and a checkpoint when the title screen is reached.
#pragma once

#include <cstdint>

namespace DcpStartupWatch {

enum Stage : unsigned {
    GuestSubmit,
    GpuSubmission,
    StageBPacket,
    FlipPrepared,
    FlipTaken,
    Present,
    Vblank,
    TitleTick,
    StageCount,
};

enum Wait : unsigned {
    FreeFrame,
    FrameFence,
    Acquire,
    QueuePresent,
    WaitCount,
};

enum Gauge : unsigned {
    FreeFrames,
    PendingFlips,
    SwapQueue,
    StageBBusy,
    TitleCursor,
    GaugeCount,
};

void Mark(Stage stage);
void SetGauge(Gauge gauge, std::int64_t value);
void StartWatch();

class WaitScope {
public:
    explicit WaitScope(Wait wait);
    ~WaitScope();
    WaitScope(const WaitScope&) = delete;
    WaitScope& operator=(const WaitScope&) = delete;

private:
    Wait wait;
    std::uint32_t tid;
};

} // namespace DcpStartupWatch
