// SPDX-License-Identifier: GPL-2.0-or-later
// DEBUG_SAULO: temporary startup and stall telemetry. Logs the first advance of each stage of the frame pipeline and, when
// presenting or the guest's GPU work stops for 3 s, how long ago every stage last advanced and what is waiting.
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
