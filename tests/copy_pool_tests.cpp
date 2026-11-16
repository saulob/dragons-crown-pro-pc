// Focused tests of the copy pool's epoch accounting (DCP-NUGRJ6): the open-epoch check, its
// report, and a ledger over real pool operations that no task runs twice and no callback runs
// before the tasks issued ahead of it.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dcp_copy_pool.h"
#include "dcp_copy_pool_forensics.h"

extern "C" {
std::uint64_t runtime_disabled_optimizations = 0;
}

namespace Common {
void SetCurrentThreadName(const char*) {}
std::string GetCurrentThreadName() {
    return "test";
}
} // namespace Common

namespace {

std::atomic<int> g_checks{0};
std::atomic<int> g_failures{0};

void Check(bool condition, const char* what, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL line %d: %s\n", line, what);
    }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

bool Contains(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

// The reported crash: the deque position start + (epoch - base) was 0xfffffffffffffa80, so the
// block index (position / 128) became 0x01fffffffffffff5 and the map read left the address space.
void TestReportedState() {
    constexpr unsigned long long kBase = 5000, kEpoch = kBase - 0x580;
    constexpr unsigned long long kPosition = 0 + (kEpoch - kBase);
    CHECK(kPosition == 0xfffffffffffffa80ull && kPosition / 128 == 0x01fffffffffffff5ull);
    CHECK(!DcpCopyPool::EpochOpen(kEpoch, kBase, 3));
}

void TestOpenEpochs() {
    using DcpCopyPool::EpochOpen;
    CHECK(EpochOpen(0, 0, 1));
    CHECK(EpochOpen(7, 7, 1) && !EpochOpen(8, 7, 1) && !EpochOpen(6, 7, 1));
    CHECK(EpochOpen(10, 10, 4) && EpochOpen(13, 10, 4) && !EpochOpen(14, 10, 4) && !EpochOpen(9, 10, 4));
    CHECK(!EpochOpen(0, 1, 1));
    CHECK(!EpochOpen(~0ull, 0, 1));
    CHECK(EpochOpen(~0ull - 1, ~0ull - 2, 2) && !EpochOpen(~0ull, ~0ull - 2, 2));
}

namespace F = DcpCopyPool::Forensics;

// A task's identity survives moves; an overwritten epoch, id or canary does not pass.
void TestSealAndCanaries() {
    F::CopyTask task;
    task.run = [] {};
    F::Seal(task, 41, 3620);
    CHECK(F::SealIntact(task) && F::CanariesIntact(task));
    F::CopyTask moved = std::move(task);
    CHECK(F::SealIntact(moved) && F::CanariesIntact(moved) && moved.id == 41 && moved.epoch == 3620);
    F::CopyTask overwritten = moved;
    overwritten.epoch -= 0x580; // mutant: the epoch field rewritten with an older epoch
    CHECK(!F::SealIntact(overwritten) && F::CanariesIntact(overwritten));
    F::CopyTask renamed = moved;
    renamed.id = 42;
    CHECK(!F::SealIntact(renamed));
    F::CopyTask smashed = moved;
    smashed.canary_back = 0;
    CHECK(!F::CanariesIntact(smashed) && F::SealIntact(smashed));
    CHECK(!F::SealIntact(F::CopyTask{})); // never sealed
}

// The ledger follows each id through queued, taken, running and completed, and refuses every
// other order: the double completion and stale-reference mutants among them.
void TestLedger() {
    auto ledger = std::make_unique<F::TaskLedger>();
    CHECK(ledger->Queue(1, 7, 1) == F::Fault::None && ledger->Take(1, 2) == F::Fault::None &&
          ledger->Start(1, 2, 0x1000) == F::Fault::None && ledger->Complete(1, 2) == F::Fault::None);
    CHECK(ledger->Find(1) && ledger->Find(1)->state == F::State::Completed && ledger->Find(1)->epoch == 7);
    CHECK(ledger->Complete(1, 3) == F::Fault::CompletedTwice); // mutant: double completion
    CHECK(ledger->Queue(1, 7, 1) == F::Fault::QueuedTwice);    // an id is never reused

    CHECK(ledger->Queue(2, 7, 1) == F::Fault::None);
    CHECK(ledger->Start(2, 2, 0x2000) == F::Fault::RunWithoutTake);
    CHECK(ledger->Complete(2, 2) == F::Fault::CompletedWithoutRunning);
    CHECK(ledger->Take(2, 2) == F::Fault::None && ledger->Take(2, 3) == F::Fault::TakenWithoutQueue);
    CHECK(ledger->Start(2, 2, 0x2000) == F::Fault::None && ledger->Start(2, 3, 0x2000) == F::Fault::RunWithoutTake);
    CHECK(ledger->Complete(2, 2) == F::Fault::None);

    // Mutant: task 3 runs from the storage task 2 used; a stale reference to task 2 completes again.
    CHECK(ledger->Queue(3, 8, 1) == F::Fault::None && ledger->Take(3, 2) == F::Fault::None &&
          ledger->Start(3, 2, 0x2000) == F::Fault::None);
    CHECK(ledger->Complete(2, 2) == F::Fault::CompletedTwice);
    CHECK(ledger->PreviousUser(0x2000, 3) == 2 && ledger->PreviousUser(0x2000, 2) == 3);
    CHECK(ledger->PreviousUser(0x9000, 3) == 0);
    CHECK(ledger->Complete(3, 2) == F::Fault::None);

    // A record is retired once a newer id takes its slot; that task is then older than the ledger.
    for (unsigned long long id = 4; id < 4 + F::TaskLedger::kCapacity; ++id) {
        CHECK(ledger->Queue(id, 9, 1) == F::Fault::None);
    }
    CHECK(!ledger->Find(3) && ledger->Take(3, 2) == F::Fault::RecordRetired);
}

// Retired epochs are remembered further back than the reported gap of about 1400 epochs.
void TestRetiredHistory() {
    auto history = std::make_unique<F::RetiredHistory>();
    for (unsigned long long epoch = 0; epoch < 5000; ++epoch) {
        history->Record({epoch, epoch % 5, epoch % 5, 0, 1, epoch * 10, epoch * 10, epoch + 1});
    }
    const F::RetiredEpoch* old = history->Find(5000 - 0x580);
    CHECK(old && old->counted == old->finished && old->pending == 0 && old->order == 5000 - 0x580 + 1);
    CHECK(history->Find(4999) && !history->Find(5000) && !history->Find(5000 - F::RetiredHistory::kCapacity - 1));
}

void TestDescribe() {
    F::CopyTask task;
    F::Seal(task, 91000, 3620);
    F::TaskRecord record{91000, 3620, 88000, 0x5000, 3, 1, 1, F::State::Running};
    F::RetiredEpoch retired{3620, 4, 4, 0, 1, 90990, 90990, 3621};
    F::Evidence evidence{F::Fault::StaleEpoch, &task, 5028, 2, 91000, 90997, &record, 90950, &retired, "dcp:GPU"};
    const std::string dump = F::Describe(evidence);
    CHECK(Contains(dump, "Copy pool forensic: epoch already retired on thread 'dcp:GPU'"));
    CHECK(Contains(dump, "task id 91000") && Contains(dump, "epoch 3620, seal intact, canaries intact"));
    CHECK(Contains(dump, "open epochs 5028..5029 (2), tasks issued 91000 completed 90997"));
    CHECK(Contains(dump, "state running queue #88000 threads 3/1/1, storage previously used by task 90950"));
    CHECK(Contains(dump, "epoch 3620 retired #3621 with 4 counted, 4 finished, pending 0, 1 callbacks"));
    task.epoch = 1;
    evidence.record = nullptr;
    evidence.retired = nullptr;
    const std::string broken = F::Describe(evidence);
    CHECK(Contains(broken, "seal BROKEN") && Contains(broken, "no record for this task id") &&
          Contains(broken, "not in the retired history"));
}

std::vector<std::string> g_dumps;
std::mutex g_dumps_mutex;
void CollectDump(const std::string& dump) {
    std::scoped_lock lock{g_dumps_mutex};
    std::puts(dump.c_str());
    g_dumps.push_back(dump);
}

// Thousands of epochs created and retired as fast as the pool allows, with copies between them.
void TestRapidEpochs() {
    std::vector<int> order;
    std::mutex order_mutex;
    std::atomic<int> copies{0};
    for (int i = 0; i < 6000; ++i) {
        if (i % 3 != 0) {
            DcpCopyPool::Async([&] { copies.fetch_add(1); });
        }
        DcpCopyPool::AfterCopies([&, i] {
            std::scoped_lock lock{order_mutex};
            order.push_back(i);
        });
    }
    DcpCopyPool::WaitAsync();
    bool ordered = order.size() == 6000;
    for (int i = 0; ordered && i < 6000; ++i) {
        ordered = order[size_t(i)] == i;
    }
    CHECK(ordered && copies.load() == 4000);
}

// The pool as the renderer uses it: tasks from one thread, callbacks from another, a third waiting.
// Every callback runs once and in registration order, and nothing is left when WaitAsync returns.
void TestPoolOrdering() {
    std::atomic<long> tasks_run{0};
    std::mutex order_mutex;
    std::vector<int> order;
    std::atomic<bool> stop{false};
    constexpr int kTasks = 20000, kCallbacks = 2000;
    std::thread producer([&] {
        for (int i = 0; i < kTasks; ++i) {
            DcpCopyPool::Async([&] { tasks_run.fetch_add(1, std::memory_order_relaxed); });
            if (i % 9 == 0) {
                DcpCopyPool::WaitAsync();
            }
        }
    });
    std::thread recorder([&] {
        for (int i = 0; i < kCallbacks; ++i) {
            DcpCopyPool::AfterCopies([&, i] {
                std::scoped_lock lock{order_mutex};
                order.push_back(i);
            });
        }
    });
    std::thread waiter([&] {
        while (!stop.load()) {
            DcpCopyPool::WaitAsync();
        }
    });
    producer.join();
    recorder.join();
    stop = true;
    waiter.join();
    DcpCopyPool::WaitAsync();
    CHECK(tasks_run.load() == kTasks);
    bool in_order = order.size() == kCallbacks;
    for (int i = 0; in_order && i < kCallbacks; ++i) {
        in_order = order[size_t(i)] == i;
    }
    CHECK(in_order);

    bool ran = false;
    DcpCopyPool::AfterCopies([&] { ran = true; });
    CHECK(ran); // nothing pending: the callback runs at once
}

// A ledger kept beside the pool: every task and callback is registered under one lock together
// with the pool call, so the ledger's order is the pool's. A callback may run only once every
// task registered before it has finished, which is what a retired epoch promises.
class Ledger {
public:
    explicit Ledger(std::size_t capacity) : runs(capacity), done(capacity), callback_runs(capacity) {}

    bool Async(std::function<void()> work) {
        std::scoped_lock lock{mutex};
        if (issued == runs.size()) {
            return false;
        }
        const std::size_t id = issued++;
        DcpCopyPool::Async([this, id, work = std::move(work)] {
            runs[id].fetch_add(1);
            work();
            done[id].store(true, std::memory_order_release);
        });
        return true;
    }

    bool AfterCopies(std::function<void()> work) {
        std::scoped_lock lock{mutex};
        if (registered == callback_runs.size()) {
            return false;
        }
        const std::size_t id = registered++, waits_for = issued;
        DcpCopyPool::AfterCopies([this, id, waits_for, work = std::move(work)] {
            if (callback_runs[id].fetch_add(1) != 0 || FinishedPrefix() < waits_for) {
                early.fetch_add(1);
            }
            {
                std::scoped_lock order_lock{order_mutex};
                order.push_back(id);
            }
            work();
        });
        return true;
    }

    std::size_t FinishedPrefix() {
        std::scoped_lock lock{prefix_mutex};
        while (prefix < issued_snapshot() && done[prefix].load(std::memory_order_acquire)) {
            ++prefix;
        }
        return prefix;
    }

    bool Consistent() {
        bool once = true;
        for (std::size_t id = 0; id < issued; ++id) {
            once = once && runs[id].load() == 1;
        }
        for (std::size_t id = 0; id < registered; ++id) {
            once = once && callback_runs[id].load() == 1;
        }
        bool ordered = order.size() == registered;
        for (std::size_t i = 0; ordered && i < order.size(); ++i) {
            ordered = order[i] == i;
        }
        return once && ordered && early.load() == 0;
    }

    std::size_t Issued() {
        std::scoped_lock lock{mutex};
        return issued;
    }

private:
    std::size_t issued_snapshot() const {
        return runs.size();
    }

    std::recursive_mutex mutex;
    std::size_t issued = 0, registered = 0;
    std::vector<std::atomic<int>> runs;
    std::vector<std::atomic<bool>> done;
    std::vector<std::atomic<int>> callback_runs;
    std::mutex prefix_mutex;
    std::size_t prefix = 0;
    std::mutex order_mutex;
    std::vector<std::size_t> order;
    std::atomic<int> early{0};
};

struct Random {
    unsigned long long state;
    unsigned Next(unsigned bound) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return unsigned(state >> 33) % bound;
    }
};

// The renderer's patterns, nested: copy tasks that issue more copies and register callbacks,
// callbacks that issue copies and register callbacks, empty epochs, a GPU thread that helps
// through WaitAsync, and parallel copies from inside tasks.
void TestNestedWorkload(unsigned long long seed) {
    Ledger ledger(60000);
    std::function<void(Random&, int)> nested = [&](Random& random, int depth) {
        const unsigned pick = random.Next(8);
        if (depth > 2) {
            return;
        }
        if (pick == 0) {
            ledger.Async([&nested, depth, s = random.state] {
                Random inner{s};
                nested(inner, depth + 1);
            });
        } else if (pick == 1) {
            ledger.AfterCopies([&nested, depth, s = random.state] {
                Random inner{s ^ 0x9e37u};
                nested(inner, depth + 1);
            });
        } else if (pick == 2) {
            std::atomic<int> parts{0};
            DcpCopyPool::ParallelFor(4, [&](std::size_t) { parts.fetch_add(1); });
            CHECK(parts.load() == 4);
        }
    };
    std::atomic<bool> stop{false};
    std::thread gpu([&] {
        Random random{seed};
        for (int i = 0; i < 15000; ++i) {
            const unsigned pick = random.Next(10);
            if (pick < 6) {
                ledger.Async([&nested, s = random.state] {
                    Random inner{s};
                    nested(inner, 0);
                });
            } else if (pick < 8) {
                ledger.AfterCopies([] {});
            } else {
                DcpCopyPool::WaitAsync();
            }
        }
    });
    std::thread recorder([&] {
        Random random{seed * 31 + 7};
        while (!stop.load()) {
            const unsigned pick = random.Next(4);
            if (pick == 0) {
                ledger.AfterCopies([&nested, s = random.state] {
                    Random inner{s};
                    nested(inner, 0);
                });
            } else if (pick == 1) {
                ledger.AfterCopies([] {});
                ledger.AfterCopies([] {});
            } else {
                std::this_thread::yield();
            }
        }
    });
    std::thread streamer([&] {
        Random random{seed * 17 + 3};
        for (int i = 0; i < 8000; ++i) {
            ledger.Async([] {});
            if (random.Next(16) == 0) {
                DcpCopyPool::WaitAsync();
            }
        }
    });
    gpu.join();
    streamer.join();
    stop = true;
    recorder.join();
    DcpCopyPool::WaitAsync();
    CHECK(ledger.Issued() > 0);
    CHECK(ledger.Consistent());
    CHECK(DcpCopyPool::ForensicFaults() == 0);
}

} // namespace

int main() {
    TestReportedState();
    TestOpenEpochs();
    TestSealAndCanaries();
    TestLedger();
    TestRetiredHistory();
    TestDescribe();
    DcpCopyPool::SetForensicHandler(CollectDump);
    TestPoolOrdering();
    for (unsigned long long seed = 1; seed <= 6; ++seed) {
        TestNestedWorkload(seed);
    }
    TestRapidEpochs();
    CHECK(DcpCopyPool::ForensicFaults() == 0 && g_dumps.empty());
    std::printf("%d checks, %d failed\n", g_checks.load(), g_failures.load());
    return g_failures.load() ? 1 : 0;
}
