// SPDX-License-Identifier: GPL-2.0-or-later
// The copy pool's forensic layer (DCP-NUGRJ6): every queued task carries a sealed identity, a
// ledger keyed by task id follows its lifecycle, and recently retired epochs are remembered, so
// an impossible completion can say whether the task was stale, reused, overwritten or finished
// twice.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace DcpCopyPool::Forensics {

using u64 = unsigned long long;

inline constexpr u64 kCanary = 0xC0B1E5A1D0CA7A11ull;
inline constexpr u64 kSeal = 0x5EA1ED7A5C0FFEEull;

struct CopyTask {
    u64 canary_front = kCanary;
    std::function<void()> run;
    u64 id = 0;
    u64 epoch = 0;
    u64 check = 0;
    u64 canary_back = kCanary;
};

inline u64 SealOf(u64 id, u64 epoch) {
    return id ^ (epoch * 0x9E3779B97F4A7C15ull) ^ kSeal;
}

inline void Seal(CopyTask& task, u64 id, u64 epoch) {
    task.id = id;
    task.epoch = epoch;
    task.check = SealOf(id, epoch);
}

inline bool CanariesIntact(const CopyTask& task) {
    return task.canary_front == kCanary && task.canary_back == kCanary;
}

inline bool SealIntact(const CopyTask& task) {
    return task.id != 0 && task.check == SealOf(task.id, task.epoch);
}

enum class State : std::uint8_t { Free, Queued, Taken, Running, Completed };

enum class Fault : std::uint8_t {
    None,
    QueuedTwice,
    TakenWithoutQueue,
    RunWithoutTake,
    CompletedTwice,
    CompletedWithoutRunning,
    RecordRetired,
    CanaryBroken,
    SealBroken,
    StaleEpoch,
};

const char* FaultName(Fault fault);
const char* StateName(State state);

struct TaskRecord {
    u64 id = 0;
    u64 epoch = 0;
    u64 queue_sequence = 0;
    std::uintptr_t storage = 0;
    std::uint32_t issuer = 0, starter = 0, finisher = 0;
    State state = State::Free;
};

/// The lifecycle of the most recent tasks, one record per task id modulo the capacity. A record
/// that a newer id took over is "retired": its task is older than the ledger remembers.
class TaskLedger {
public:
    static constexpr std::size_t kCapacity = 1u << 14;

    Fault Queue(u64 id, u64 epoch, std::uint32_t thread);
    Fault Take(u64 id, std::uint32_t thread);
    Fault Start(u64 id, std::uint32_t thread, std::uintptr_t storage);
    Fault Complete(u64 id, std::uint32_t thread);

    /// The record of `id`, or nullptr once a newer task took its slot.
    const TaskRecord* Find(u64 id) const;
    /// The newest other task id that ran from `storage`, or 0.
    u64 PreviousUser(std::uintptr_t storage, u64 id) const;

private:
    TaskRecord& Slot(u64 id) { return records[id % kCapacity]; }
    std::array<TaskRecord, kCapacity> records{};
    u64 queued = 0;
};

struct RetiredEpoch {
    u64 epoch = 0;
    u64 counted = 0;   // tasks queued into it
    u64 finished = 0;  // its tasks that completed
    u64 pending = 0;   // its pending count when it retired
    u64 callbacks = 0;
    u64 issued_total = 0, completed_total = 0;
    u64 order = 0;     // retirement sequence
};

/// The last kCapacity retired epochs (more than the 1400 epochs of the reported crash).
class RetiredHistory {
public:
    static constexpr std::size_t kCapacity = 4096;

    void Record(const RetiredEpoch& epoch);
    const RetiredEpoch* Find(u64 epoch) const;

private:
    std::array<RetiredEpoch, kCapacity> entries{};
    u64 count = 0;
};

struct Evidence {
    Fault fault = Fault::None;
    const CopyTask* task = nullptr;
    u64 epoch_base = 0;
    std::size_t open_epochs = 0;
    u64 issued = 0, completed = 0;
    const TaskRecord* record = nullptr;
    u64 previous_storage_user = 0;
    const RetiredEpoch* retired = nullptr;
    std::string thread;
};

/// The forensic dump for an impossible transition, one line per subject.
std::string Describe(const Evidence& evidence);

} // namespace DcpCopyPool::Forensics
