// SPDX-License-Identifier: GPL-2.0-or-later
#include "dcp_copy_pool_forensics.h"

#include <cstdarg>
#include <cstdio>

namespace DcpCopyPool::Forensics {

const char* FaultName(Fault fault) {
    switch (fault) {
    case Fault::None: return "none";
    case Fault::QueuedTwice: return "queued twice";
    case Fault::TakenWithoutQueue: return "taken while not queued";
    case Fault::RunWithoutTake: return "started while not taken";
    case Fault::CompletedTwice: return "completed twice";
    case Fault::CompletedWithoutRunning: return "completed without running";
    case Fault::RecordRetired: return "record retired";
    case Fault::CanaryBroken: return "canary overwritten";
    case Fault::SealBroken: return "id/epoch seal broken";
    case Fault::StaleEpoch: return "epoch already retired";
    }
    return "?";
}

const char* StateName(State state) {
    switch (state) {
    case State::Free: return "free";
    case State::Queued: return "queued";
    case State::Taken: return "taken";
    case State::Running: return "running";
    case State::Completed: return "completed";
    }
    return "?";
}

Fault TaskLedger::Queue(u64 id, u64 epoch, std::uint32_t thread) {
    TaskRecord& record = Slot(id);
    if (record.id == id && record.state != State::Free) {
        return Fault::QueuedTwice;
    }
    record = {id, epoch, ++queued, 0, thread, 0, 0, State::Queued};
    return Fault::None;
}

Fault TaskLedger::Take(u64 id, std::uint32_t) {
    TaskRecord& record = Slot(id);
    if (record.id != id) {
        return Fault::RecordRetired;
    }
    if (record.state != State::Queued) {
        return Fault::TakenWithoutQueue;
    }
    record.state = State::Taken;
    return Fault::None;
}

Fault TaskLedger::Start(u64 id, std::uint32_t thread, std::uintptr_t storage) {
    TaskRecord& record = Slot(id);
    if (record.id != id) {
        return Fault::RecordRetired;
    }
    if (record.state != State::Taken) {
        return Fault::RunWithoutTake;
    }
    record.state = State::Running;
    record.starter = thread;
    record.storage = storage;
    return Fault::None;
}

Fault TaskLedger::Complete(u64 id, std::uint32_t thread) {
    TaskRecord& record = Slot(id);
    if (record.id != id) {
        return Fault::RecordRetired;
    }
    if (record.state == State::Completed) {
        return Fault::CompletedTwice;
    }
    if (record.state != State::Running) {
        return Fault::CompletedWithoutRunning;
    }
    record.state = State::Completed;
    record.finisher = thread;
    return Fault::None;
}

const TaskRecord* TaskLedger::Find(u64 id) const {
    const TaskRecord& record = records[id % kCapacity];
    return record.id == id && record.state != State::Free ? &record : nullptr;
}

u64 TaskLedger::PreviousUser(std::uintptr_t storage, u64 id) const {
    u64 newest = 0;
    for (const TaskRecord& record : records) {
        if (storage != 0 && record.storage == storage && record.id != id && record.id > newest) {
            newest = record.id;
        }
    }
    return newest;
}

void RetiredHistory::Record(const RetiredEpoch& epoch) {
    entries[epoch.epoch % kCapacity] = epoch;
    ++count;
}

const RetiredEpoch* RetiredHistory::Find(u64 epoch) const {
    const RetiredEpoch& entry = entries[epoch % kCapacity];
    return count != 0 && entry.epoch == epoch && entry.order != 0 ? &entry : nullptr;
}

namespace {

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void Append(std::string& text, const char* format, ...) {
    char line[384];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    text += line;
}

} // namespace

std::string Describe(const Evidence& e) {
    std::string text;
    const CopyTask* task = e.task;
    Append(text, "Copy pool forensic: %s on thread '%s'\n", FaultName(e.fault), e.thread.c_str());
    if (task != nullptr) {
        Append(text, "Copy pool forensic: task id %llu at %p, epoch %llu, seal %s, canaries %s\n", task->id,
            static_cast<const void*>(task), task->epoch, SealIntact(*task) ? "intact" : "BROKEN",
            CanariesIntact(*task) ? "intact" : "BROKEN");
    }
    Append(text, "Copy pool forensic: open epochs %llu..%llu (%zu), tasks issued %llu completed %llu\n", e.epoch_base,
        e.epoch_base + e.open_epochs - 1, e.open_epochs, e.issued, e.completed);
    if (e.record != nullptr) {
        Append(text, "Copy pool forensic: record id %llu epoch %llu state %s queue #%llu threads %u/%u/%u, "
            "storage previously used by task %llu\n",
            e.record->id, e.record->epoch, StateName(e.record->state), e.record->queue_sequence,
            e.record->issuer, e.record->starter, e.record->finisher, e.previous_storage_user);
    } else {
        Append(text, "Copy pool forensic: no record for this task id (older than the ledger)\n");
    }
    if (e.retired != nullptr) {
        Append(text, "Copy pool forensic: epoch %llu retired #%llu with %llu counted, %llu finished, pending %llu, "
            "%llu callbacks, at issued %llu completed %llu",
            e.retired->epoch, e.retired->order, e.retired->counted, e.retired->finished, e.retired->pending,
            e.retired->callbacks, e.retired->issued_total, e.retired->completed_total);
    } else {
        text += "Copy pool forensic: the task's epoch is not in the retired history";
    }
    return text;
}

} // namespace DcpCopyPool::Forensics
