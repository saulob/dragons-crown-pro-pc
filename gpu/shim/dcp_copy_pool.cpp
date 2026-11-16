// SPDX-License-Identifier: GPL-2.0-or-later
#include "dcp_copy_pool.h"
#include "dcp_copy_pool_forensics.h"
#include "dcp_threads.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dcp_toggles.h"
#include "common/thread.h"

namespace DcpCopyPool {
namespace {

namespace F = Forensics;

thread_local bool in_copy_thread = false;

std::uint32_t ThreadTag() {
    static std::atomic<std::uint32_t> next{1};
    thread_local const std::uint32_t tag = next.fetch_add(1);
    return tag;
}

void StopOnForensicFault(const std::string& dump) {
    std::puts(dump.c_str());
    std::fflush(stdout);
    __builtin_trap();
}

ForensicHandler forensic_handler = StopOnForensicFault;

class Pool {
public:
    Pool() {
        // Copies are on the critical path (fences wait for them): normal priority, a quarter
        // of the hardware threads available to the process.
        const unsigned count = std::clamp(DcpThreads::Available() / 4, 1u, 4u);
        epochs.emplace_back();
        for (unsigned i = 0; i < count; ++i) {
            threads.emplace_back([this, i] { Loop(i); });
        }
    }
    ~Pool() {
        {
            std::scoped_lock lk{mutex};
            stop = true;
        }
        cv.notify_all();
        for (auto& thread : threads) {
            thread.join();
        }
    }

    bool Enabled() const {
        return !threads.empty();
    }

    void Async(std::function<void()> run) {
        {
            std::scoped_lock lk{mutex};
            const u64 epoch = epoch_base + epochs.size() - 1;
            Task task;
            task.run = std::move(run);
            F::Seal(task, ++issued, epoch);
            ++epochs.back().pending;
            ++epochs.back().counted;
            ++pending;
            Check(ledger.Queue(task.id, epoch, ThreadTag()), &task);
            async_tasks.push_back(std::move(task));
        }
        cv.notify_one();
    }

    void AfterCopies(std::function<void()> callback) {
        {
            std::unique_lock lk{mutex};
            if (epochs.size() > 1 || epochs.back().pending != 0 || draining) {
                // Runs once every copy issued so far is done, in order with earlier ones.
                epochs.back().callbacks.push_back(std::move(callback));
                epochs.emplace_back();
                return;
            }
        }
        callback();
    }

    void WaitAll() {
        // The waiting thread helps instead of only spinning.
        while (true) {
            Task task;
            {
                std::scoped_lock lk{mutex};
                if (pending == 0 && epochs.size() == 1 && !draining) {
                    return;
                }
                if (!async_tasks.empty()) {
                    Take(task);
                }
            }
            if (task.run) {
                RunAsync(task);
            } else {
                std::this_thread::yield();
            }
        }
    }

    void Run(std::size_t count, const std::function<void(std::size_t)>& task) {
        auto job = std::make_shared<Job>();
        job->task = &task;
        job->count = count;
        {
            std::scoped_lock lk{mutex};
            current = job;
            ++generation;
        }
        cv.notify_all();
        Work(*job);
        // Workers only call the task for indices below count, all done before this returns.
        while (job->done.load(std::memory_order_acquire) < count) {
            std::this_thread::yield();
        }
        std::scoped_lock lk{mutex};
        if (current == job) {
            current.reset();
        }
    }

    std::size_t Faults() {
        std::scoped_lock lk{mutex};
        return faults;
    }

private:
    using u64 = unsigned long long;
    using Task = F::CopyTask;
    /// Copies issued between two AfterCopies() calls, and the callbacks waiting for them.
    struct Epoch {
        std::size_t pending = 0;
        u64 counted = 0;
        u64 finished = 0;
        std::vector<std::function<void()>> callbacks;
    };
    struct Job {
        const std::function<void(std::size_t)>* task{};
        std::size_t count{};
        std::atomic<std::size_t> next{0};
        std::atomic<std::size_t> done{0};
    };

    /// Moves the oldest queued task into `task`. Called with the mutex held.
    void Take(Task& task) {
        task = std::move(async_tasks.front());
        async_tasks.pop_front();
        Check(Intact(task), &task);
        Check(ledger.Take(task.id, ThreadTag()), &task);
    }

    void RunAsync(Task& task) {
        {
            std::scoped_lock lk{mutex};
            Check(ledger.Start(task.id, ThreadTag(), reinterpret_cast<std::uintptr_t>(&task)), &task);
        }
        const bool was = in_copy_thread;
        in_copy_thread = true;
        task.run();
        in_copy_thread = was;
        Complete(task);
    }

    /// Retires a task and runs the callbacks of epochs with nothing left, oldest first. One
    /// thread drains at a time so the callbacks keep their order.
    void Complete(const Task& task) {
        std::unique_lock lk{mutex};
        const F::Fault integrity = Intact(task);
        const F::Fault lifecycle = ledger.Complete(task.id, ThreadTag());
        if (lifecycle == F::Fault::CompletedTwice) {
            Check(lifecycle, &task);
            return;
        }
        --pending;
        ++completed;
        Check(integrity, &task);
        if (lifecycle != F::Fault::RecordRetired) {
            Check(lifecycle, &task);
        }
        // An overwritten task still counts against the epoch the ledger recorded when it was queued.
        const F::TaskRecord* record = ledger.Find(task.id);
        const u64 task_epoch = integrity == F::Fault::None || !record ? task.epoch : record->epoch;
        if (!EpochOpen(task_epoch, epoch_base, epochs.size())) {
            Check(F::Fault::StaleEpoch, &task);
            return;
        }
        Epoch& epoch = epochs[task_epoch - epoch_base];
        --epoch.pending;
        ++epoch.finished;
        if (draining) {
            return;
        }
        draining = true;
        while (true) {
            std::vector<std::function<void()>> ready;
            while (epochs.size() > 1 && epochs.front().pending == 0) {
                Epoch& front = epochs.front();
                retired.Record({epoch_base, front.counted, front.finished, front.pending, front.callbacks.size(),
                                issued, completed, ++retirements});
                for (auto& callback : front.callbacks) {
                    ready.push_back(std::move(callback));
                }
                epochs.pop_front();
                ++epoch_base;
            }
            if (ready.empty()) {
                break;
            }
            lk.unlock();
            for (auto& callback : ready) {
                callback();
            }
            lk.lock();
        }
        draining = false;
    }

    static F::Fault Intact(const Task& task) {
        if (!F::CanariesIntact(task)) {
            return F::Fault::CanaryBroken;
        }
        return F::SealIntact(task) ? F::Fault::None : F::Fault::SealBroken;
    }

    /// Reports an impossible transition with everything the ledgers know. Called with the mutex
    /// held; the production handler does not return.
    void Check(F::Fault fault, const Task* task) {
        if (fault == F::Fault::None || fault == F::Fault::RecordRetired) {
            return;
        }
        ++faults;
        F::Evidence evidence;
        evidence.fault = fault;
        evidence.task = task;
        evidence.epoch_base = epoch_base;
        evidence.open_epochs = epochs.size();
        evidence.issued = issued;
        evidence.completed = completed;
        evidence.record = ledger.Find(task->id);
        evidence.previous_storage_user =
            evidence.record ? ledger.PreviousUser(evidence.record->storage, task->id) : 0;
        evidence.retired = retired.Find(task->epoch);
        evidence.thread = Common::GetCurrentThreadName();
        forensic_handler(F::Describe(evidence));
    }

    static void Work(Job& job) {
        const bool was = in_copy_thread;
        in_copy_thread = true;
        for (std::size_t i; (i = job.next.fetch_add(1, std::memory_order_relaxed)) < job.count;) {
            (*job.task)(i);
            job.done.fetch_add(1, std::memory_order_release);
        }
        in_copy_thread = was;
    }

    void Loop(unsigned index) {
        Common::SetCurrentThreadName(("dcp:Copy" + std::to_string(index)).c_str());
        u64 seen = 0;
        std::unique_lock lk{mutex};
        while (true) {
            cv.wait(lk, [&] {
                return stop || !async_tasks.empty() || (generation != seen && current);
            });
            if (stop) {
                return;
            }
            if (!async_tasks.empty()) {
                Task task;
                Take(task);
                lk.unlock();
                RunAsync(task);
                lk.lock();
                continue;
            }
            seen = generation;
            const auto job = current;
            lk.unlock();
            Work(*job);
            lk.lock();
        }
    }

    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable cv;
    std::shared_ptr<Job> current;
    u64 generation = 0;
    bool stop = false;
    std::deque<Task> async_tasks;
    std::deque<Epoch> epochs; ///< back() is open for new copies
    u64 epoch_base = 0;       ///< epoch number of epochs.front()
    std::size_t pending = 0;
    bool draining = false;
    u64 issued = 0;
    u64 completed = 0;
    u64 retirements = 0;
    std::size_t faults = 0;
    F::TaskLedger ledger;
    F::RetiredHistory retired;
};

Pool& GetPool() {
    static Pool pool;
    return pool;
}

struct Batch {
    std::vector<Item> items;
    unsigned long long bytes = 0;
};
thread_local Batch batch;

} // namespace

bool EpochOpen(unsigned long long epoch, unsigned long long base, std::size_t open) {
    return epoch >= base && epoch - base < open;
}

ForensicHandler SetForensicHandler(ForensicHandler handler) {
    const ForensicHandler previous = forensic_handler;
    forensic_handler = handler ? handler : StopOnForensicFault;
    return previous;
}

std::size_t ForensicFaults() {
    return GetPool().Faults();
}

bool Enabled() {
    return GetPool().Enabled() && !DcpToggle::Disabled(DcpToggle::ParallelCopies);
}

void Async(std::function<void()> task) {
    if (!Enabled()) {
        task();
        return;
    }
    GetPool().Async(std::move(task));
}

void FlushBatch() {
    if (batch.items.empty()) {
        return;
    }
    auto items = std::make_shared<std::vector<Item>>(std::move(batch.items));
    batch.items = {};
    batch.items.reserve(256);
    batch.bytes = 0;
    Async([items] {
        for (const auto& item : *items) {
            item.run(item);
        }
    });
}

void QueueCopy(const Item& item) {
    if (!Enabled()) {
        item.run(item);
        return;
    }
    // Smaller batches (32 items / 64 KiB, done before the next fence) measured slower.
    batch.items.push_back(item);
    batch.bytes += item.size;
    if (batch.bytes >= 512 * 1024 || batch.items.size() >= 256) {
        FlushBatch();
    }
}

void AfterCopies(std::function<void()> callback) {
    FlushBatch();
    GetPool().AfterCopies(std::move(callback));
}

void WaitAsync() {
    // The caller's own batch runs here: handing it over only to wait for it costs a wakeup.
    if (!batch.items.empty()) {
        for (const auto& item : batch.items) {
            item.run(item);
        }
        batch.items.clear();
        batch.bytes = 0;
    }
    GetPool().WaitAll();
}

void ParallelFor(std::size_t count, const std::function<void(std::size_t)>& task) {
    if (count <= 1 || in_copy_thread || !Enabled()) {
        for (std::size_t i = 0; i < count; ++i) {
            task(i);
        }
        return;
    }
    GetPool().Run(count, task);
}

} // namespace DcpCopyPool
