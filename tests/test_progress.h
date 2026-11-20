// Progress lines for long-running test suites. Under tools\test.ps1 (DCP_TEST_PROGRESS set) a suite emits
// "@@dcp-progress checks=N ..." lines that the runner turns into its progress display; run on its own it prints a plain
// "RUNNING ..." line every few seconds. The GROUP|... result lines stay the machine-readable output.
#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace dcp::testprogress {

enum class State { kPass, kFail, kNoData };

inline const char* StateName(State state) {
    return state == State::kPass ? "PASS" : state == State::kFail ? "FAIL" : "NO_DATA";
}

inline std::string FormatGroupLine(const std::string& name, State state, long long checks, long long ms) {
    char line[512];
    std::snprintf(line, sizeof line, "GROUP|%s|%s|checks=%lld|ms=%lld", name.c_str(), StateName(state), checks, ms);
    return line;
}

inline std::string FormatCount(int64_t value) {
    std::string digits = std::to_string(value < 0 ? -value : value);
    for (int i = int(digits.size()) - 3; i > 0; i -= 3) digits.insert(size_t(i), ",");
    return value < 0 ? "-" + digits : digits;
}

// 0m 03s, 31m 42s, 1h 02m 03s
inline std::string FormatElapsed(int64_t ms) {
    const int64_t seconds = std::max<int64_t>(0, ms) / 1000;
    char text[48];
    if (seconds < 3600) std::snprintf(text, sizeof text, "%lldm %02llds", (long long)(seconds / 60), (long long)(seconds % 60));
    else std::snprintf(text, sizeof text, "%lldh %02lldm %02llds", (long long)(seconds / 3600), (long long)(seconds % 3600 / 60), (long long)(seconds % 60));
    return text;
}

// 2.1s under a minute, then as FormatElapsed.
inline std::string FormatDuration(int64_t ms) {
    if (ms < 0) ms = 0;
    if (ms >= 60'000) return FormatElapsed(ms);
    char text[32];
    std::snprintf(text, sizeof text, "%.1fs", double(ms) / 1000.0);
    return text;
}

// Rounded so the estimate does not flicker: 2 s steps under a minute, 30 s under ten minutes, whole minutes above.
inline std::string FormatEta(int64_t ms) {
    if (ms < 0) return "calculating";
    const double seconds = double(ms) / 1000.0;
    char text[48];
    if (seconds < 60) {
        std::snprintf(text, sizeof text, "~%llds", (long long)std::max<int64_t>(2, 2 * int64_t(seconds / 2 + 0.5)));
        return text;
    }
    const int64_t step = seconds < 600 ? 30 : 60;
    const int64_t rounded = step * int64_t(seconds / double(step) + 0.5);
    const int64_t hours = rounded / 3600, minutes = rounded % 3600 / 60, secs = rounded % 60;
    if (hours > 0) std::snprintf(text, sizeof text, "~%lldh %02lldm", (long long)hours, (long long)minutes);
    else if (secs > 0) std::snprintf(text, sizeof text, "~%lldm %02llds", (long long)minutes, (long long)secs);
    else std::snprintf(text, sizeof text, "~%lldm", (long long)minutes);
    return text;
}

struct Record {
    std::string name;
    State state = State::kPass;
    int64_t checks = 0;
    int64_t ms = 0;
};

struct HistoryEntry {
    int64_t checks = 0;
    int64_t ms = 0;
};
using History = std::map<std::string, HistoryEntry>;

inline History LoadHistory(const std::filesystem::path& path) {
    History history;
    std::ifstream in(path);
    std::string name;
    long long checks = 0, ms = 0;
    while (std::getline(in, name, '|') && in >> checks >> ms) {
        in.ignore(1, '\n');
        if (!name.empty() && ms > 0) history[name] = {checks, ms};
    }
    return history;
}

inline void SaveHistory(const std::filesystem::path& path, const History& history) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    for (const auto& [name, entry] : history) out << name << '|' << entry.checks << ' ' << entry.ms << '\n';
}

inline bool StdoutInteractive() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

inline bool RunnerProgressRequested() { return std::getenv("DCP_TEST_PROGRESS") != nullptr; }

// The runner wants a line about once a second; run on its own a suite prints every 5 s (terminal) or 15 s (log).
// DCP_TEST_PROGRESS_SECONDS overrides the standalone interval.
inline int64_t DefaultIntervalMs(bool runner, bool interactive) {
    if (runner) return 1'000;
    if (const char* text = std::getenv("DCP_TEST_PROGRESS_SECONDS")) {
        const long seconds = std::atol(text);
        if (seconds > 0) return int64_t(seconds) * 1000;
    }
    return interactive ? 5'000 : 15'000;
}

struct Snapshot {
    std::string suite;
    std::string group;
    size_t completed = 0;
    size_t total = 0;
    int64_t group_ms = 0;
    int64_t checks = -1;
    int64_t expected_checks = 0;
    bool expected_exact = false;
    int64_t suite_checks = -1;
    int64_t suite_expected = 0;
    int step = 0;
    int steps = 0;
    std::string step_label;
    int64_t eta_ms = -1;
};

// group 18m 11s | ETA ~18m | checks 1,284,220 / ~2,039,231 | step 4/19 Appearance preparation
inline std::string FormatDetail(const Snapshot& s) {
    std::string text = "group " + FormatElapsed(s.group_ms) + " | ETA " + FormatEta(s.eta_ms);
    if (s.checks >= 0) {
        text += " | checks " + FormatCount(s.checks);
        if (s.expected_checks > 0) text += std::string(" / ") + (s.expected_exact ? "" : "~") + FormatCount(s.expected_checks);
    }
    if (s.steps > 0 && s.step > 0) {
        text += " | step " + std::to_string(std::min(s.step, s.steps)) + "/" + std::to_string(s.steps);
        if (!s.step_label.empty()) text += " " + s.step_label;
    }
    return text;
}

inline std::string FormatRunningLine(const Snapshot& s) {
    return "RUNNING " + s.suite + ": " + s.group + " " + std::to_string(s.completed + 1) + "/" + std::to_string(s.total) + " | " + FormatDetail(s) + "\n";
}

// Suite-wide counts for the runner's bar; the detail text is what the runner shows beside them.
inline std::string FormatRunnerLine(const Snapshot& s) {
    std::string line = "@@dcp-progress checks=" + std::to_string(std::max<int64_t>(0, s.suite_checks));
    if (s.suite_expected > 0) line += " total=" + std::to_string(s.suite_expected) + " estimate=1";
    line += " detail=" + s.group + " " + std::to_string(s.completed + 1) + "/" + std::to_string(s.total);
    if (s.steps > 0 && s.step > 0) {
        line += " | step " + std::to_string(std::min(s.step, s.steps)) + "/" + std::to_string(s.steps);
        if (!s.step_label.empty()) line += " " + s.step_label;
    }
    return line + "\n";
}

class Reporter {
public:
    struct Options {
        std::string suite;
        std::vector<std::string> groups;
        std::function<void(const std::string&)> sink;
        std::function<int64_t()> clock;
        std::function<int64_t()> checks;
        int64_t interval_ms = 5'000;
        bool runner = false;
        History history;
    };

    explicit Reporter(Options options) : options_(std::move(options)) {
        if (!options_.sink) options_.sink = [](const std::string& text) { std::fputs(text.c_str(), stdout); std::fflush(stdout); };
        if (!options_.clock) {
            options_.clock = [] {
                return int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            };
        }
        start_ = options_.clock();
        suite_checks_base_ = options_.checks ? options_.checks() : 0;
    }

    ~Reporter() { StopHeartbeat(); }
    Reporter(const Reporter&) = delete;
    Reporter& operator=(const Reporter&) = delete;

    void BeginGroup(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::find(options_.groups.begin(), options_.groups.end(), name) == options_.groups.end()) options_.groups.push_back(name);
        group_ = name;
        in_group_ = true;
        group_start_ = options_.clock();
        last_emit_ = group_start_;
        group_checks_base_ = options_.checks ? options_.checks() : 0;
        expected_checks_ = 0;
        expected_exact_ = false;
        step_ = steps_ = 0;
        step_label_.clear();
        if (options_.runner) Emit(group_start_);
    }

    void SetExpectedChecks(int64_t expected, bool exact) {
        std::lock_guard<std::mutex> lock(mutex_);
        expected_checks_ = expected;
        expected_exact_ = exact;
    }

    void SetSteps(int total) {
        std::lock_guard<std::mutex> lock(mutex_);
        steps_ = total;
        step_ = 0;
    }

    void Step(const std::string& label) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++step_;
        step_label_ = label;
    }

    void EndGroup(State state, int64_t checks, int64_t ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.push_back({group_, state, checks, ms});
        in_group_ = false;
    }

    // Prints a line when one is due; cheap enough to call often.
    void Tick() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!in_group_ || finished_) return;
        const int64_t now = options_.clock();
        if (now - last_emit_ >= options_.interval_ms) Emit(now);
    }

    void StartHeartbeat() {
        if (heartbeat_.joinable()) return;
        stop_ = false;
        heartbeat_ = std::thread([this] {
            const auto poll = std::chrono::milliseconds(std::clamp<int64_t>(options_.interval_ms / 4, 20, 500));
            std::unique_lock<std::mutex> wait(heartbeat_mutex_);
            while (!stop_) {
                heartbeat_cv_.wait_for(wait, poll);
                if (stop_) break;
                Tick();
            }
        });
    }

    void StopHeartbeat() {
        if (!heartbeat_.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(heartbeat_mutex_);
            stop_ = true;
        }
        heartbeat_cv_.notify_all();
        heartbeat_.join();
    }

    // Timing table; the baseline is refreshed from groups that passed.
    void Finish(const std::filesystem::path& history_path = {}) {
        StopHeartbeat();
        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
        in_group_ = false;
        options_.sink(TimingTable());
        if (!history_path.empty() && records_.size() >= options_.groups.size()) {
            History merged = options_.history;
            for (const Record& record : records_) {
                if (record.state == State::kPass && record.ms > 0) merged[record.name] = {record.checks, record.ms};
            }
            SaveHistory(history_path, merged);
        }
    }

    Snapshot Current() {
        std::lock_guard<std::mutex> lock(mutex_);
        return Capture(options_.clock());
    }

    // Indented so the runner can keep the table together: name, state, time, checks; Total; Slowest.
    std::string TimingTable() const {
        std::ostringstream out;
        out << options_.suite << " timing\n";
        for (const Record& record : records_) {
            char row[256];
            std::snprintf(row, sizeof row, "  %-36s %-8s %8s  %s checks\n", record.name.c_str(), StateName(record.state), FormatDuration(record.ms).c_str(),
                          FormatCount(record.checks).c_str());
            out << row;
        }
        char total[256];
        std::snprintf(total, sizeof total, "  %-36s %-8s %8s\n", "Total", "", FormatElapsed(options_.clock() - start_).c_str());
        out << total;
        if (records_.size() < options_.groups.size()) out << "  Incomplete: " << records_.size() << '/' << options_.groups.size() << " groups finished\n";
        std::vector<Record> slowest = records_;
        std::stable_sort(slowest.begin(), slowest.end(), [](const Record& a, const Record& b) { return a.ms > b.ms; });
        if (slowest.size() >= 3) {
            out << "  Slowest:\n";
            for (size_t i = 0; i < 3; ++i) {
                char row[256];
                std::snprintf(row, sizeof row, "  %zu. %-32s %s\n", i + 1, slowest[i].name.c_str(), FormatElapsed(slowest[i].ms).c_str());
                out << row;
            }
        }
        return out.str();
    }

    const std::vector<Record>& records() const { return records_; }

private:
    const HistoryEntry* Known(const std::string& name) const {
        const auto found = options_.history.find(name);
        return found != options_.history.end() && found->second.ms > 0 ? &found->second : nullptr;
    }

    double GroupFraction(int64_t checks, int64_t group_ms, int64_t expected) const {
        double fraction = 0.0;
        if (expected > 0 && checks >= 0) fraction = double(checks) / double(expected);
        else if (steps_ > 0) fraction = double(std::max(0, step_ - 1)) / double(steps_);
        else if (const HistoryEntry* known = Known(group_)) fraction = double(group_ms) / double(known->ms);
        return std::clamp(fraction, 0.0, 0.99);
    }

    int64_t RemainingMs(int64_t group_ms, double group_fraction, size_t current) const {
        int64_t mean = -1;
        if (!records_.empty()) {
            int64_t sum = 0;
            for (const Record& record : records_) sum += record.ms;
            mean = sum / int64_t(records_.size());
        }
        int64_t remaining = 0;
        if (const HistoryEntry* known = Known(group_)) {
            remaining = known->ms - group_ms;
            if (remaining <= 0) remaining = group_fraction > 0.02 ? int64_t(double(group_ms) * (1.0 - group_fraction) / group_fraction) : group_ms / 10;
        } else if (group_fraction > 0.02) {
            remaining = int64_t(double(group_ms) * (1.0 - group_fraction) / group_fraction);
        } else if (mean >= 0) {
            remaining = std::max<int64_t>(0, mean - group_ms);
        } else {
            return -1;
        }
        for (size_t i = current + 1; i < options_.groups.size(); ++i) {
            if (const HistoryEntry* known = Known(options_.groups[i])) remaining += known->ms;
            else if (mean >= 0) remaining += mean;
            else return -1;
        }
        return remaining;
    }

    Snapshot Capture(int64_t now) {
        Snapshot s;
        s.suite = options_.suite;
        s.group = group_;
        s.total = options_.groups.size();
        s.completed = std::min(records_.size(), s.total);
        s.group_ms = in_group_ ? std::max<int64_t>(0, now - group_start_) : 0;
        const int64_t running_checks = options_.checks ? options_.checks() : -1;
        if (running_checks >= 0 && in_group_) {
            s.checks = std::max<int64_t>(0, running_checks - group_checks_base_);
            s.suite_checks = std::max<int64_t>(0, running_checks - suite_checks_base_);
        }
        s.expected_checks = expected_checks_;
        s.expected_exact = expected_exact_;
        if (s.expected_checks <= 0 && !expected_exact_) {
            if (const HistoryEntry* known = Known(group_)) s.expected_checks = known->checks;
        }
        int64_t suite_expected = 0;
        for (const std::string& name : options_.groups) {
            const HistoryEntry* known = Known(name);
            if (!known || known->checks <= 0) {
                suite_expected = 0;
                break;
            }
            suite_expected += known->checks;
        }
        s.suite_expected = suite_expected;
        s.step = step_;
        s.steps = steps_;
        s.step_label = step_label_;
        const auto found = std::find(options_.groups.begin(), options_.groups.end(), group_);
        const size_t current = size_t(found - options_.groups.begin());
        const double group_fraction = in_group_ ? GroupFraction(s.checks, s.group_ms, s.expected_checks) : 0.0;
        s.eta_ms = in_group_ ? RemainingMs(s.group_ms, group_fraction, current) : -1;
        return s;
    }

    void Emit(int64_t now) {
        const Snapshot snapshot = Capture(now);
        last_emit_ = now;
        options_.sink(options_.runner ? FormatRunnerLine(snapshot) : FormatRunningLine(snapshot));
    }

    Options options_;
    std::mutex mutex_;
    std::vector<Record> records_;
    std::string group_;
    std::string step_label_;
    bool in_group_ = false;
    bool finished_ = false;
    int64_t start_ = 0;
    int64_t group_start_ = 0;
    int64_t group_checks_base_ = 0;
    int64_t suite_checks_base_ = 0;
    int64_t last_emit_ = 0;
    int64_t expected_checks_ = 0;
    bool expected_exact_ = false;
    int step_ = 0;
    int steps_ = 0;
    std::thread heartbeat_;
    std::mutex heartbeat_mutex_;
    std::condition_variable heartbeat_cv_;
    bool stop_ = false;
};

} // namespace dcp::testprogress
