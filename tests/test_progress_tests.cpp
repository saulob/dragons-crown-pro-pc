// Focused tests of the shared test progress lines (tests/test_progress.h) on an injected clock, so nothing waits.
// `dcp-test-progress-tests simulate [runner]` prints a sample for a long synthetic run. No window, no game.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "test_progress.h"
#include "test_shards.h"

namespace fs = std::filesystem;
using namespace dcp::testprogress;
namespace testshards = dcp::testshards;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* what, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL line %d: %s\n", line, what);
    }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

bool Contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

struct Sim {
    int64_t now = 0;
    int64_t checks = 0;
    std::vector<std::string> lines;

    Reporter::Options Options(std::vector<std::string> groups, int64_t interval_ms, bool runner = false, History history = {}) {
        Reporter::Options options;
        options.suite = "Suite";
        options.groups = std::move(groups);
        options.interval_ms = interval_ms;
        options.runner = runner;
        options.history = std::move(history);
        options.clock = [this] { return now; };
        options.checks = [this] { return checks; };
        options.sink = [this](const std::string& text) { lines.push_back(text); };
        return options;
    }

    // One simulated second per step, with `per_second` checks each, calling Tick like the heartbeat would.
    void Run(Reporter& reporter, int64_t seconds, int64_t per_second) {
        for (int64_t i = 0; i < seconds; ++i) {
            now += 1000;
            checks += per_second;
            reporter.Tick();
        }
    }
};

void TestFormatting() {
    CHECK(FormatCount(0) == "0" && FormatCount(999) == "999" && FormatCount(1000) == "1,000" && FormatCount(1284220) == "1,284,220");
    CHECK(FormatDuration(0) == "0.0s" && FormatDuration(2100) == "2.1s" && FormatDuration(-5) == "0.0s" && FormatDuration(59'900) == "59.9s");
    CHECK(FormatDuration(128'000) == "2m 08s" && FormatDuration(1'694'000) == "28m 14s");
    CHECK(FormatDuration(3'723'000) == "1h 02m 03s" && FormatDuration(36'000'000) == "10h 00m 00s");
    CHECK(FormatElapsed(0) == "0m 00s" && FormatElapsed(3'900) == "0m 03s" && FormatElapsed(1'902'000) == "31m 42s" && FormatElapsed(7'384'000) == "2h 03m 04s");
    CHECK(FormatElapsed(-1) == "0m 00s");
    CHECK(FormatEta(-1) == "calculating" && FormatEta(0) == "~2s" && FormatEta(3'000) == "~4s" && FormatEta(47'000) == "~48s");
    CHECK(FormatEta(270'000) == "~4m 30s" && FormatEta(240'000) == "~4m" && FormatEta(1'080'000) == "~18m" && FormatEta(3'900'000) == "~1h 05m");
    CHECK(FormatGroupLine("editor-real-parts", State::kPass, 1284220, 1694000) == "GROUP|editor-real-parts|PASS|checks=1284220|ms=1694000");
    CHECK(FormatGroupLine("a", State::kNoData, 0, 3) == "GROUP|a|NO_DATA|checks=0|ms=3");
    CHECK(FormatGroupLine("a", State::kFail, 5, 9) == "GROUP|a|FAIL|checks=5|ms=9");
}

void TestRunnerLines() {
    Sim sim;
    Reporter reporter(sim.Options({"one", "two", "three"}, 1000, true));
    reporter.BeginGroup("one");
    CHECK(sim.lines.size() == 1 && sim.lines[0] == "@@dcp-progress checks=0 detail=one 1/3\n");
    sim.checks = 12'845;
    sim.now = 4000;
    reporter.EndGroup(State::kPass, 12'845, 4000);
    reporter.BeginGroup("two");
    CHECK(sim.lines.back() == "@@dcp-progress checks=12845 detail=two 2/3\n"); // suite-wide count, group in the detail
    reporter.SetSteps(19);
    reporter.Step("Feather");
    reporter.Step("No headgear");
    sim.now += 1000;
    sim.checks += 5;
    reporter.Tick();
    CHECK(sim.lines.back() == "@@dcp-progress checks=12850 detail=two 2/3 | step 2/19 No headgear\n");

    Sim known;
    History history;
    history["a"] = {100, 1000};
    history["b"] = {900, 9000};
    Reporter withHistory(known.Options({"a", "b"}, 1000, true, history));
    withHistory.BeginGroup("a");
    CHECK(known.lines.back() == "@@dcp-progress checks=0 total=1000 estimate=1 detail=a 1/2\n");
    history.erase("b");
    Sim partial;
    Reporter missing(partial.Options({"a", "b"}, 1000, true, history));
    missing.BeginGroup("a");
    CHECK(partial.lines.back().find("total=") == std::string::npos);
}

void TestStandaloneLines() {
    Sim sim;
    Reporter reporter(sim.Options({"g"}, 1000));
    reporter.BeginGroup("g");
    CHECK(sim.lines.empty()); // nothing until the interval passes
    sim.checks = 12'845;
    sim.now = 1500;
    reporter.Tick();
    CHECK(sim.lines.size() == 1 && sim.lines[0] == "RUNNING Suite: g 1/1 | group 0m 01s | ETA calculating | checks 12,845\n");
    reporter.SetExpectedChecks(2'039'231, true);
    sim.checks = 1'284'220;
    sim.now = 3000;
    reporter.Tick();
    CHECK(Contains(sim.lines.back(), "| checks 1,284,220 / 2,039,231\n") && !Contains(sim.lines.back(), "/ ~"));
    reporter.SetExpectedChecks(2'039'231, false);
    sim.now = 4500;
    reporter.Tick();
    CHECK(Contains(sim.lines.back(), "| checks 1,284,220 / ~2,039,231\n"));

    Sim estimated;
    History history;
    history["g"] = {2'039'231, 100'000};
    Reporter fromHistory(estimated.Options({"g"}, 1000, false, history));
    fromHistory.BeginGroup("g");
    estimated.checks = 500;
    estimated.now = 2000;
    fromHistory.Tick();
    CHECK(Contains(estimated.lines.back(), "checks 500 / ~2,039,231"));
}

void TestEta() {
    Sim sim;
    History history;
    history["fast"] = {100, 10'000};
    history["slow"] = {1000, 90'000};
    Reporter reporter(sim.Options({"fast", "slow"}, 1000, false, history));
    reporter.BeginGroup("fast");
    CHECK(reporter.Current().eta_ms == 100'000);
    sim.Run(reporter, 10, 10);
    reporter.EndGroup(State::kPass, 100, 10'000);
    reporter.BeginGroup("slow");
    Snapshot s = reporter.Current();
    CHECK(s.completed == 1 && s.total == 2 && s.eta_ms == 90'000);
    sim.Run(reporter, 45, 10);
    s = reporter.Current();
    CHECK(s.eta_ms >= 44'000 && s.eta_ms <= 46'000 && s.expected_checks == 1000 && !s.expected_exact && s.checks == 450);
    sim.Run(reporter, 200, 10); // past the historical duration
    s = reporter.Current();
    CHECK(s.eta_ms >= 0 && s.group_ms == 245'000);
    reporter.EndGroup(State::kPass, 2450, 245'000);
    reporter.Finish();
    CHECK(Contains(sim.lines.back(), "Suite timing\n"));
}

void TestEtaWithoutHistory() {
    Sim sim;
    Reporter reporter(sim.Options({"a", "b", "c"}, 1000));
    reporter.BeginGroup("a");
    sim.Run(reporter, 5, 1);
    CHECK(reporter.Current().eta_ms == -1);
    reporter.EndGroup(State::kPass, 5, 5000);
    reporter.BeginGroup("b");
    sim.Run(reporter, 1, 1);
    CHECK(reporter.Current().eta_ms == 5000 - 1000 + 5000); // the rest against the mean of the finished groups

    Sim stepped;
    Reporter steps(stepped.Options({"only"}, 1000));
    steps.BeginGroup("only");
    steps.SetSteps(4);
    steps.Step("first");
    steps.Step("second");
    stepped.Run(steps, 20, 1);
    const Snapshot t = steps.Current();
    CHECK(t.step == 2 && t.steps == 4 && t.eta_ms == 60'000);
    CHECK(Contains(FormatDetail(t), "| step 2/4 second"));
}

void TestEdgeCases() {
    Sim sim;
    Reporter empty(sim.Options({}, 1000));
    empty.Tick();
    empty.BeginGroup("late"); // not in the plan: appended
    sim.Run(empty, 3, 0);
    CHECK(empty.Current().total == 1);
    empty.EndGroup(State::kPass, 0, 3000);

    Sim zero;
    History history;
    history["z"] = {0, 1};
    Reporter flat(zero.Options({"z"}, 1000, false, history));
    flat.BeginGroup("z");
    zero.Run(flat, 5, 0);
    CHECK(flat.Current().eta_ms >= 0);

    Sim backwards;
    backwards.now = 10'000;
    Reporter clock(backwards.Options({"a"}, 1000));
    backwards.now = 5000;
    clock.BeginGroup("a");
    backwards.now = 1000; // a clock that moved back never yields negative times
    CHECK(clock.Current().group_ms == 0);
}

void TestCadenceAndRedirect() {
    Sim sim;
    Reporter reporter(sim.Options({"a", "b"}, 5000));
    reporter.BeginGroup("a");
    sim.Run(reporter, 20, 1);
    CHECK(sim.lines.size() == 4); // one line every 5 simulated seconds
    Sim runner;
    Reporter fast(runner.Options({"a"}, 1000, true));
    fast.BeginGroup("a");
    runner.Run(fast, 20, 1);
    CHECK(runner.lines.size() == 21); // the start, then about one a second
    for (const std::string& text : sim.lines) {
        CHECK(text.find('\r') == std::string::npos && text.find('\x1b') == std::string::npos && text.find('\b') == std::string::npos);
        CHECK(std::count(text.begin(), text.end(), '\n') == 1 && text.rfind("RUNNING ", 0) == 0);
    }
    CHECK(DefaultIntervalMs(true, false) == 1000 && DefaultIntervalMs(false, false) >= DefaultIntervalMs(false, true));
}

void TestFailureAndTable() {
    Sim sim;
    Reporter reporter(sim.Options({"editor-save-parse", "editor-real-parts", "appearance-translation"}, 1000));
    reporter.BeginGroup("editor-save-parse");
    sim.now += 2100;
    reporter.EndGroup(State::kPass, 10, 2100);
    reporter.BeginGroup("editor-real-parts");
    sim.now += 1'694'000;
    reporter.EndGroup(State::kFail, 20, 1'694'000);
    reporter.BeginGroup("appearance-translation");
    sim.now += 271'000;
    reporter.EndGroup(State::kNoData, 0, 271'000);
    reporter.Finish();
    const std::string table = sim.lines.back();
    CHECK(table == reporter.TimingTable());
    CHECK(Contains(table, "Suite timing\n") && Contains(table, "  editor-save-parse") && Contains(table, "PASS") && Contains(table, "FAIL") &&
          Contains(table, "NO_DATA") && Contains(table, "28m 14s") && Contains(table, "  Total") && Contains(table, "10 checks") && !Contains(table, "Incomplete"));
    CHECK(table.find("1. editor-real-parts") < table.find("2. appearance-translation") && table.find("2. appearance-translation") < table.find("3. editor-save-parse"));
    CHECK(reporter.records().size() == 3 && reporter.records()[1].state == State::kFail);
    std::istringstream rows(table);
    std::string row;
    std::getline(rows, row);
    while (std::getline(rows, row)) CHECK(row.rfind("  ", 0) == 0); // the runner keeps an indented block together

    Sim partial;
    Reporter stopped(partial.Options({"a", "b"}, 1000));
    stopped.BeginGroup("a");
    stopped.EndGroup(State::kPass, 1, 1000);
    stopped.Finish();
    CHECK(Contains(partial.lines.back(), "Incomplete: 1/2 groups finished"));
}

void TestHistoryFile() {
    const fs::path dir = fs::temp_directory_path() / "dcp_test_progress_tests";
    fs::remove_all(dir);
    const fs::path file = dir / "nested" / "history.txt";
    History saved;
    saved["editor-real-parts"] = {2'039'231, 1'694'000};
    saved["editor-save-parse"] = {10, 2100};
    SaveHistory(file, saved);
    const History loaded = LoadHistory(file);
    CHECK(loaded.size() == 2 && loaded.at("editor-real-parts").checks == 2'039'231 && loaded.at("editor-real-parts").ms == 1'694'000);
    CHECK(LoadHistory(dir / "missing.txt").empty());
    {
        std::ofstream broken(dir / "broken.txt");
        broken << "garbage without numbers\nx|notanumber\n";
    }
    CHECK(LoadHistory(dir / "broken.txt").empty());

    Sim sim;
    Reporter reporter(sim.Options({"a", "b"}, 1000, false, saved));
    reporter.BeginGroup("a");
    reporter.EndGroup(State::kPass, 7, 500);
    reporter.BeginGroup("b");
    reporter.EndGroup(State::kFail, 7, 500);
    reporter.Finish(dir / "run.txt");
    const History after = LoadHistory(dir / "run.txt");
    CHECK(after.count("a") == 1 && after.count("b") == 0 && after.count("editor-real-parts") == 1);
    fs::remove_all(dir);
}

void TestHeartbeat() {
    std::mutex lock;
    std::vector<std::string> lines;
    Reporter::Options options;
    options.suite = "Real time";
    options.groups = {"slow"};
    options.interval_ms = 40;
    options.sink = [&](const std::string& text) {
        std::lock_guard<std::mutex> guard(lock);
        lines.push_back(text);
    };
    std::atomic<int64_t> checks{0};
    options.checks = [&] { return checks.load(); };
    Reporter reporter(std::move(options));
    reporter.StartHeartbeat();
    reporter.BeginGroup("slow");
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        checks += 1000;
    }
    reporter.EndGroup(State::kPass, 20'000, 400);
    reporter.Finish();
    std::lock_guard<std::mutex> guard(lock);
    CHECK(lines.size() >= 4); // several updates while the group ran, then the table
    CHECK(Contains(lines.front(), "RUNNING Real time: slow 1/1"));
}

// The group names and which of them need the game, read from the editor suite's own table.
std::vector<std::pair<std::string, bool>> EditorGroupTable() {
    std::vector<std::pair<std::string, bool>> table;
#ifdef DCP_SOURCE_DIR
    std::ifstream in(fs::path(DCP_SOURCE_DIR) / "tests" / "save_editor_tests.cpp");
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string source = buffer.str();
    const size_t start = source.find("std::vector<EditorGroup> EditorGroups(");
    const size_t end = source.find("// Runs the named groups", start);
    if (start == std::string::npos || end == std::string::npos) return table;
    const std::string body = source.substr(start, end - start);
    static const std::regex entry("(?:SingleUnit\\(|\\{)\"((?:editor-|appearance-)[a-z-]+)\",\\s*(true|false|[0-9]+)");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), entry); it != std::sregex_iterator(); ++it) {
        table.emplace_back((*it)[1].str(), (*it)[2].str() == "true");
    }
    const std::string source_main = source.substr(end);
    CHECK(source_main.find("FormatGroupLine(group.name, state, g_checks - checks, ms)") != std::string::npos);
    CHECK(source_main.find("reporter.BeginGroup(group.name)") != std::string::npos && source_main.find("reporter.Finish(") != std::string::npos);
    CHECK(source_main.find("RunnerProgressRequested()") != std::string::npos);
#endif
    return table;
}

void TestModeGroupCounts() {
    const auto table = EditorGroupTable();
#ifdef DCP_SOURCE_DIR
    CHECK(table.size() == 15); // release and all
    size_t quick = 0;
    for (const auto& [name, needs_game] : table) quick += needs_game ? 0 : 1;
    CHECK(quick > 0 && quick < table.size());
    for (const size_t count : {table.size(), quick}) {
        Sim sim;
        std::vector<std::string> names;
        for (const auto& [name, needs_game] : table) {
            if (count == table.size() || !needs_game) names.push_back(name);
        }
        Reporter reporter(sim.Options(names, 1000, true));
        reporter.BeginGroup(names.front());
        const std::string expected = " 1/" + std::to_string(count) + "\n";
        CHECK(Contains(sim.lines.back(), expected.c_str()));
    }
#endif
}

int Simulate(bool runner) {
    Sim sim;
    History history;
    history["editor-save-parse"] = {900, 2'100};
    history["editor-real-overlay"] = {5'000, 128'000};
    history["editor-real-parts"] = {2'039'231, 1'694'000};
    Reporter::Options options = sim.Options({"editor-save-parse", "editor-real-overlay", "editor-real-parts"}, runner ? 60'000 : 300'000, runner, history);
    options.suite = "Character Editor";
    options.sink = [](const std::string& text) { std::fputs(text.c_str(), stdout); };
    Reporter reporter(std::move(options));
    reporter.BeginGroup("editor-save-parse");
    sim.Run(reporter, 2, 450);
    reporter.EndGroup(State::kPass, 900, 2'100);
    reporter.BeginGroup("editor-real-overlay");
    sim.Run(reporter, 128, 39);
    reporter.EndGroup(State::kPass, 5'000, 128'000);
    reporter.BeginGroup("editor-real-parts");
    const int64_t started = sim.now;
    const int64_t base_checks = sim.checks;
    reporter.SetSteps(19);
    const char* labels[] = {"Feather", "No headgear", "Fighter horns on every build", "Appearance preparation", "Amazon parts", "Sorceress parts"};
    for (const char* label : labels) {
        reporter.Step(label);
        sim.Run(reporter, 280, 1200);
    }
    reporter.EndGroup(State::kPass, sim.checks - base_checks, sim.now - started);
    reporter.Finish();
    return 0;
}

// ---- tests/test_shards.h

// Units whose "work" only ends when the test says so, guarding a resource two of them must never hold at once.
void TestShardScheduling() {
    using testshards::Result;
    using testshards::Unit;
    std::vector<Unit> units;
    for (int i = 0; i < 12; ++i) units.push_back({"learn" + std::to_string(i % 3) + "/" + std::to_string(i), {}, i});
    units.push_back({"uses", {"learn0/0", "learn1/1"}, 1000});
    units.push_back({"throws", {}, 500});
    units.push_back({"after-throws", {"throws"}, 1});
    units.push_back({"a-waits-b", {"b-waits-a"}, 1});
    units.push_back({"b-waits-a", {"a-waits-b"}, 1});
    units.push_back({"unknown-dependency", {"not in the list"}, 1});

    for (const int jobs : {1, 3, 8, 64}) {
        std::mutex mutex;
        std::set<std::string> held, ended;
        std::vector<std::string> order;
        int running = 0, peak = 0, clashes = 0;
        const auto run = [&](const Unit& unit) {
            const std::string folder = "folder of " + unit.id;
            {
                std::lock_guard<std::mutex> lock(mutex);
                clashes += !held.insert(folder).second;
                for (const std::string& id : unit.after) {
                    if (id.find("waits") == std::string::npos && id != "not in the list") clashes += !ended.count(id);
                }
                order.push_back(unit.id);
                peak = std::max(peak, ++running);
            }
            std::this_thread::yield();
            std::lock_guard<std::mutex> lock(mutex);
            --running;
            held.erase(folder);
            ended.insert(unit.id);
            if (unit.id == "throws") throw std::runtime_error("boom");
            Result result;
            result.exit_code = 0;
            result.output = unit.id;
            return result;
        };
        size_t finished = 0;
        testshards::Scheduler scheduler(units, run, [&](size_t index, const Result& result) {
            ++finished;
            CHECK(index < units.size() && result.ran);
        });
        const std::vector<Result> results = scheduler.Run(jobs);
        CHECK(results.size() == units.size() && finished == units.size() && order.size() == units.size());
        CHECK(std::set<std::string>(order.begin(), order.end()).size() == units.size()); // each exactly once
        CHECK(peak <= std::min<int>(jobs, int(units.size())) && clashes == 0 && running == 0);
        CHECK(scheduler.Running().empty() && scheduler.Finished() == units.size());
        for (size_t i = 0; i < results.size(); ++i) {
            if (units[i].id == "throws") CHECK(results[i].exit_code == -1 && Contains(results[i].output, "boom"));
            else CHECK(results[i].exit_code == 0 && results[i].output == units[i].id); // a result stays with its unit
        }
        if (jobs == 1) CHECK(order.front() == "throws" && order[1] == "learn2/11"); // the heaviest ready unit first
    }
    // Memory: what runs together fits the budget, and a unit that never fits runs alone.
    std::vector<Unit> heavy;
    for (int i = 0; i < 10; ++i) heavy.push_back({"m" + std::to_string(i), {}, 1, i == 7 ? 5000 : 300 + i});
    std::mutex mutex;
    long long used = 0, peak = 0;
    int running = 0, alone = 0, over = 0;
    testshards::Scheduler bounded(heavy, [&](const Unit& unit) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++running;
            used += unit.memory_mb;
            if (unit.memory_mb > 1000) alone += running == 1;
            else over += used > 1000;
            peak = std::max(peak, used);
        }
        std::this_thread::yield();
        std::lock_guard<std::mutex> lock(mutex);
        --running;
        used -= unit.memory_mb;
        return Result{0, "", 0, false};
    });
    for (const Result& result : bounded.Run(8, 1000)) CHECK(result.ran && result.exit_code == 0);
    CHECK(over == 0 && alone == 1 && peak == 5000 && used == 0);
    const testshards::Memory memory = testshards::PhysicalMemoryMb();
    CHECK(memory.total > 500 && memory.available > 0 && memory.available <= memory.total);
    CHECK(testshards::MemoryBudgetMb({32000, 20000}) == 14000 && testshards::MemoryBudgetMb({32000, 1000}) == 8000 && testshards::MemoryBudgetMb({0, 0}) == 0);
    testshards::Scheduler none({}, [](const Unit&) { return Result{}; });
    CHECK(none.Run(4).empty());
}

// Two units overlap when two jobs are allowed: each waits for the other to have started.
void TestShardOverlap() {
    std::mutex mutex;
    std::condition_variable both;
    int started = 0, overlapped = 0;
    testshards::Scheduler scheduler({{"one", {}, 1}, {"two", {}, 1}}, [&](const testshards::Unit&) {
        std::unique_lock<std::mutex> lock(mutex);
        ++started;
        both.notify_all();
        overlapped += both.wait_for(lock, std::chrono::seconds(20), [&] { return started == 2; });
        return testshards::Result{0, "", 0, false};
    });
    scheduler.Run(2);
    CHECK(overlapped == 2);
}

void TestShardProcess(const char* self) {
    std::vector<std::string> lines;
    const int code = testshards::RunProcess(self, {"echo", "two words", "last"}, [&](const std::string& line) { lines.push_back(line); });
    CHECK(code == 7);
    CHECK(lines == (std::vector<std::string>{"out two words", "err last", "no line end"}));
    std::atomic<int> good{0};
    testshards::Scheduler scheduler({{"a", {}, 1}, {"b", {}, 1}, {"c", {}, 1}, {"d", {}, 1}}, [&](const testshards::Unit& unit) {
        std::vector<std::string> own;
        testshards::Result result;
        result.exit_code = testshards::RunProcess(self, {"echo", unit.id, unit.id}, [&](const std::string& line) { own.push_back(line); });
        good += own == std::vector<std::string>{"out " + unit.id, "err " + unit.id, "no line end"}; // no line of another child
        return result;
    });
    for (const testshards::Result& result : scheduler.Run(4)) CHECK(result.exit_code == 7);
    CHECK(good == 4);
}

void TestShardJobs() {
    const auto with = [](const char* value) {
#ifdef _WIN32
        _putenv_s("DCP_TEST_JOBS", value);
#else
        if (*value) setenv("DCP_TEST_JOBS", value, 1);
        else unsetenv("DCP_TEST_JOBS");
#endif
        return testshards::JobsFromEnvironment();
    };
    const char* before = std::getenv("DCP_TEST_JOBS");
    const std::string saved = before ? before : "";
    CHECK(with("") == 1 && with("1") == 1 && with("4") == 4 && with("99") == 8 && with("0") == 1 && with("-3") == 1 && with("many") == 1);
    with(saved.c_str());
}

// Every group keeps its units: what the sequential suite runs is what the processes run.
void TestEditorUnits() {
    std::ifstream in(fs::path(DCP_SOURCE_DIR) / "tests" / "save_editor_tests.cpp", std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    const std::string source = text.str();
    const auto body = [&](const char* signature) {
        const size_t from = source.find(signature);
        return from == std::string::npos ? std::string() : source.substr(from, source.find("\n}\n", from) - from);
    };
    for (const char* test : {"TestAmazonParts", "TestSorceressParts", "TestSorceressSplit", "TestFighterParts", "TestFighterHelmetAndHorns", "TestFighterBareHead",
                             "TestFighterHelmetContexts", "TestDwarfParts", "TestDwarfHeadAndGear", "TestWizardParts", "TestElfParts", "TestElfMaterials",
                             "TestClassWeapons", "TestEveryPartHides", "TestFeather", "TestNoHeadgear", "TestFighterHornBuild(", "TestFighterHornBuildsPresent",
                             "TestAppearancePrepare", "TestPartCacheVersion"}) {
        CHECK(Contains(body("RealPartsUnits("), test));
    }
    for (const char* test : {"TestRealCustomOverlay", "TestRealHelmetHiddenOverlay", "TestRemoteLookParity", "TestCustomBaseInvariant"}) {
        CHECK(Contains(body("RealOverlayUnits("), test));
    }
    for (const char* test : {"TestRealArt", "TestRealCustom"}) CHECK(Contains(body("RealArtUnits("), test));
    for (const char* test : {"TestCompositionRules", "TestCompositionMatrix", "SupportedBuilds()"}) CHECK(Contains(body("TranslationUnits("), test));
    CHECK(Contains(body("int RunEditorGroups("), "for (const EditorUnit& unit : group.units)") && Contains(body("int RunSharded("), "for (const EditorUnit& unit : groups[g].units)"));
}

// What TestShardProcess starts: both streams, a line without an end and an exit code of its own.
int Echo(const char* out, const char* err) {
    std::printf("out %s\n", out);
    std::fflush(stdout);
    std::fprintf(stderr, "err %s\n", err);
    std::fflush(stderr);
    std::printf("no line end");
    return 7;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "simulate") == 0) return Simulate(argc > 2 && std::strcmp(argv[2], "runner") == 0);
    if (argc > 3 && std::strcmp(argv[1], "echo") == 0) return Echo(argv[2], argv[3]);
    TestFormatting();
    TestRunnerLines();
    TestStandaloneLines();
    TestEta();
    TestEtaWithoutHistory();
    TestEdgeCases();
    TestCadenceAndRedirect();
    TestFailureAndTable();
    TestHistoryFile();
    TestHeartbeat();
    TestModeGroupCounts();
    TestShardScheduling();
    TestShardOverlap();
    TestShardProcess(argv[0]);
    TestShardJobs();
    TestEditorUnits();
    std::printf("GROUP|test-progress|%s|checks=%d|ms=0\n", g_failures ? "FAIL" : "PASS", g_checks);
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
