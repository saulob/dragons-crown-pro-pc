// Runs a suite's independent units on a bounded number of threads, each in a process of its own (RunProcess), so one
// unit can never see another's globals or temporary files.
#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fstream>
#include <sys/wait.h>
#endif

namespace dcp::testshards {

struct Unit {
    std::string id;
    std::vector<std::string> after; // ids that must have finished, whatever their result
    int weight = 1;                 // heavier units start first
    int memory_mb = 0;              // what it may need at its peak
};

struct Result {
    int exit_code = -1;
    std::string output;
    long long ms = 0;
    bool ran = false;
};

// DCP_TEST_JOBS, at most 8; 1 when it is not set.
inline int JobsFromEnvironment() {
    const char* text = std::getenv("DCP_TEST_JOBS");
    return text ? std::clamp(std::atoi(text), 1, 8) : 1;
}

// Physical memory in MB: all of it, and what is free to use now; zeros when it is not known.
struct Memory {
    long long total = 0;
    long long available = 0;
};

inline Memory PhysicalMemoryMb() {
    Memory memory;
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof status;
    if (GlobalMemoryStatusEx(&status)) memory = {static_cast<long long>(status.ullTotalPhys >> 20), static_cast<long long>(status.ullAvailPhys >> 20)};
#else
    std::ifstream in("/proc/meminfo");
    std::string name, unit;
    long long kb = 0;
    while ((!memory.total || !memory.available) && in >> name >> kb >> unit) {
        if (name == "MemTotal:") memory.total = kb >> 10;
        else if (name == "MemAvailable:") memory.available = kb >> 10;
    }
#endif
    return memory;
}

// What the units may use together: most of what is free, but never less than a quarter of the machine.
inline long long MemoryBudgetMb(const Memory& memory = PhysicalMemoryMb()) { return std::max(memory.available * 7 / 10, memory.total / 4); }

class Scheduler {
public:
    Scheduler(std::vector<Unit> units, std::function<Result(const Unit&)> run, std::function<void(size_t, const Result&)> finished = {})
        : units_(std::move(units)), run_(std::move(run)), finished_(std::move(finished)), state_(units_.size(), kWaiting), results_(units_.size()) {}

    // At most `jobs` units at a time, and no more than fit into `memory_mb` (0: any); one unit always runs.
    std::vector<Result> Run(int jobs, long long memory_mb = 0) {
        memory_mb_ = memory_mb;
        std::vector<std::thread> workers;
        const int count = std::min<int>(std::max(1, jobs), int(std::max<size_t>(1, units_.size())));
        for (int i = 0; i < count; ++i) workers.emplace_back([this] { Work(); });
        for (std::thread& worker : workers) worker.join();
        return results_;
    }

    std::vector<std::string> Running() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> ids;
        for (size_t i = 0; i < units_.size(); ++i) {
            if (state_[i] == kRunning) ids.push_back(units_[i].id);
        }
        return ids;
    }

    size_t Finished() {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_t(std::count(state_.begin(), state_.end(), kFinished));
    }

private:
    enum State { kWaiting, kRunning, kFinished };

    bool Ready(size_t index) const {
        for (const std::string& id : units_[index].after) {
            for (size_t i = 0; i < units_.size(); ++i) {
                if (units_[i].id == id && state_[i] != kFinished) return false;
            }
        }
        return true;
    }

    size_t Pick() const {
        size_t best = units_.size();
        bool running = false;
        long long used = 0;
        for (size_t i = 0; i < units_.size(); ++i) {
            if (state_[i] != kRunning) continue;
            running = true;
            used += units_[i].memory_mb;
        }
        for (size_t i = 0; i < units_.size(); ++i) {
            if (state_[i] != kWaiting || !Ready(i)) continue;
            if (running && memory_mb_ > 0 && used + units_[i].memory_mb > memory_mb_) continue;
            if (best == units_.size() || units_[i].weight > units_[best].weight) best = i;
        }
        if (best != units_.size() || running) return best;
        for (size_t i = 0; i < units_.size(); ++i) { // units that wait for each other still run
            if (state_[i] == kWaiting) return i;
        }
        return units_.size();
    }

    void Work() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            if (std::none_of(state_.begin(), state_.end(), [](State s) { return s == kWaiting; })) return;
            const size_t index = Pick();
            if (index == units_.size()) {
                changed_.wait(lock);
                continue;
            }
            state_[index] = kRunning;
            lock.unlock();
            const auto started = std::chrono::steady_clock::now();
            Result result;
            try {
                result = run_(units_[index]);
            } catch (const std::exception& error) {
                result.exit_code = -1;
                result.output += std::string("unit threw: ") + error.what() + "\n";
            }
            result.ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            result.ran = true;
            lock.lock();
            results_[index] = result;
            state_[index] = kFinished;
            if (finished_) finished_(index, result);
            changed_.notify_all();
        }
    }

    std::vector<Unit> units_;
    std::function<Result(const Unit&)> run_;
    std::function<void(size_t, const Result&)> finished_;
    std::vector<State> state_;
    std::vector<Result> results_;
    long long memory_mb_ = 0;
    std::mutex mutex_;
    std::condition_variable changed_;
};

// Runs a program with stdout and stderr merged, handing over each line; returns its exit code (128 + signal on POSIX).
inline int RunProcess(const std::string& program, const std::vector<std::string>& arguments, const std::function<void(const std::string&)>& line) {
    static std::mutex open_mutex; // a pipe opened while another child starts would be inherited and never reach its end
    std::string command = "\"" + program + "\"";
    for (const std::string& argument : arguments) command += " \"" + argument + "\"";
    command += " 2>&1";
    std::FILE* pipe = nullptr;
    {
        std::lock_guard<std::mutex> lock(open_mutex);
        std::fflush(stdout);
#ifdef _WIN32
        pipe = _popen(("\"" + command + "\"").c_str(), "r");
#else
        pipe = popen(command.c_str(), "r");
#endif
    }
    if (!pipe) return -1;
    std::string text;
    char buffer[4096];
    while (std::fgets(buffer, sizeof buffer, pipe)) {
        text += buffer;
        if (text.back() != '\n') continue;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        line(text);
        text.clear();
    }
    if (!text.empty()) line(text);
#ifdef _WIN32
    return _pclose(pipe);
#else
    const int status = pclose(pipe);
    return WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
#endif
}

} // namespace dcp::testshards
