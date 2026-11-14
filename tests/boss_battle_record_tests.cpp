// Focused tests of Boss Battle's own data file: format (and the first one), outcomes, the attempt invariant, atomic
// replacement and refusal to touch a file it cannot read.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "boss_battle_record.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;
#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);  \
            ++failures;                                          \
        }                                                        \
    } while (0)

std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void TestFormat() {
    const std::string good = "DCP Boss Battle 2\nSaulo\tharpy\t5\t2\t1\t1\t95000\n\nAna\tmedusa\t1\t0\t1\t0\t0\n";
    CHECK(boss_battle_record_parse(good.data(), good.size()) == 1);
    const BossBattleStats saulo = boss_battle_record_stats("Saulo", "harpy");
    CHECK(saulo.attempts == 5 && saulo.wins == 2 && saulo.deaths == 1 && saulo.quits == 1 && saulo.best_ms == 95000);
    CHECK(boss_battle_record_stats("Saulo", "medusa").attempts == 0);
    char text[256];
    CHECK(boss_battle_record_format(text, sizeof(text)) == std::strlen(text));
    CHECK(std::string(text) == "DCP Boss Battle 3\nSaulo\tharpy\t5\t2\t1\t1\t95000\nAna\tmedusa\t1\t0\t1\t0\t0\n");

    // The first format had no quits; it is read and written back in the current one.
    const std::string v1 = "DCP Boss Battle 1\nSaulo\tharpy\t3\t2\t1\t95000\n";
    CHECK(boss_battle_record_parse(v1.data(), v1.size()) == 1);
    const BossBattleStats old = boss_battle_record_stats("Saulo", "harpy");
    CHECK(old.attempts == 3 && old.wins == 2 && old.deaths == 1 && old.quits == 0 && old.best_ms == 95000);
    CHECK(boss_battle_record_format(text, sizeof(text)) && std::string(text) == "DCP Boss Battle 3\nSaulo\tharpy\t3\t2\t1\t0\t95000\n");

    // The second format had no Fight All rows; its individual records survive the upgrade.
    const std::string v2 = "DCP Boss Battle 2\nSaulo\tharpy\t5\t2\t1\t1\t95000\nSaulo\tmedusa\t2\t1\t0\t0\t80000\n";
    CHECK(boss_battle_record_parse(v2.data(), v2.size()) == 1);
    CHECK(boss_battle_record_stats("Saulo", "harpy").attempts == 5 && boss_battle_record_stats("Saulo", "medusa").best_ms == 80000);
    CHECK(boss_battle_record_fight_all_stats("Saulo").runs == 0);
    CHECK(boss_battle_record_format(text, sizeof(text)) &&
          std::string(text) == "DCP Boss Battle 3\nSaulo\tharpy\t5\t2\t1\t1\t95000\nSaulo\tmedusa\t2\t1\t0\t0\t80000\n");

    // Fight All rows: runs, clears, deaths, quits, best progress, best time.
    const std::string v3 = "DCP Boss Battle 3\nSaulo\tharpy\t1\t1\t0\t0\t5000\nSaulo\t@fight_all\t4\t1\t1\t1\t20\t3600123\n";
    CHECK(boss_battle_record_parse(v3.data(), v3.size()) == 1);
    const BossBattleFightAllStats run = boss_battle_record_fight_all_stats("Saulo");
    CHECK(run.runs == 4 && run.clears == 1 && run.deaths == 1 && run.quits == 1 && run.best_progress == 20 &&
          run.best_ms == 3600123);
    CHECK(boss_battle_record_stats("Saulo", "harpy").attempts == 1 && boss_battle_record_fight_all_stats("Ana").runs == 0);
    CHECK(boss_battle_record_format(text, sizeof(text)) && std::string(text) == v3);

    for (const char* bad : {"", "DCP Boss Battle 4\n",
                            "DCP Boss Battle 3\nSaulo\t@fight_all\t1\t0\t0\t0\t0\n",
                            "DCP Boss Battle 3\nSaulo\t@fight_all\t1\t1\t1\t0\t5\t0\n",
                            "DCP Boss Battle 3\nSaulo\t@fight_all\t1\t0\t0\t0\t21\t0\n",
                            "DCP Boss Battle 3\nSaulo\t@fight_all\tx\t0\t0\t0\t0\t0\n",
                            "DCP Boss Battle 3\nSaulo\t@harpy\t1\t0\t0\t0\t0\n",
                            "DCP Boss Battle 2\nSaulo\t@fight_all\t1\t0\t0\t0\t0\t0\n", "DCP Boss Battle 2\nSaulo\tharpy\t1\t2\t3\n",
                            "DCP Boss Battle 2\nSaulo\tharpy\tx\t0\t0\t0\t0\n", "DCP Boss Battle 2\n\tharpy\t1\t0\t0\t0\t0\n",
                            "DCP Boss Battle 2\nSaulo\tharpy\t1\t0\t0\t0\t99999999999\n",
                            "DCP Boss Battle 2\nSaulo\tharpy\t1\t1\t1\t0\t0\n", "DCP Boss Battle 1\nSaulo\tharpy\t1\t1\t0\t0\t0\n"}) {
        CHECK(boss_battle_record_parse(bad, std::strlen(bad)) == 0);
    }
    CHECK(boss_battle_record_stats("Saulo", "harpy").attempts == 1 && boss_battle_record_fight_all_stats("Saulo").runs == 4);
    // a rejected text changes nothing
    boss_battle_record_clear();
}

void TestFile() {
    const fs::path dir = fs::temp_directory_path() / "dcp_boss_battle_record_tests";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string user = dir.string();
    boss_battle_record_configure(user.c_str());
    // Each terminal outcome closes one attempt; one with no open attempt is refused and never written.
    CHECK(boss_battle_record_note("Saulo\tKnight", "harpy", BOSS_BATTLE_WIN, 1000) == 0);
    CHECK(boss_battle_record_note("Saulo\tKnight", "harpy", BOSS_BATTLE_ATTEMPT, 0) == 1);
    CHECK(boss_battle_record_note("Saulo\tKnight", "harpy", BOSS_BATTLE_WIN, 120000) == 1);
    CHECK(boss_battle_record_note("Saulo\tKnight", "harpy", BOSS_BATTLE_WIN, 90000) == 0);
    CHECK(boss_battle_record_note("Saulo\tKnight", "harpy", BOSS_BATTLE_ATTEMPT, 0) == 1);
    CHECK(boss_battle_record_note("Saulo\tKnight", "harpy", BOSS_BATTLE_WIN, 90000) == 1);
    CHECK(boss_battle_record_note("Saulo\tKnight", "harpy", BOSS_BATTLE_ATTEMPT, 0) == 1);
    CHECK(boss_battle_record_note("Saulo\tKnight", "harpy", BOSS_BATTLE_DEATH, 5000) == 1);
    CHECK(boss_battle_record_note("Saulo\tKnight", "harpy", BOSS_BATTLE_ATTEMPT, 0) == 1);
    CHECK(boss_battle_record_note("Saulo\tKnight", "harpy", BOSS_BATTLE_QUIT, 0) == 1);
    CHECK(boss_battle_record_note("Saulo\tKnight", "harpy", BOSS_BATTLE_QUIT, 0) == 0);
    CHECK(boss_battle_record_note("Saulo\tKnight", "harpy", BOSS_BATTLE_ATTEMPT, 0) == 1); // a crash leaves it open
    CHECK(boss_battle_record_note("", "harpy", BOSS_BATTLE_ATTEMPT, 0) == 0);
    const BossBattleStats stats = boss_battle_record_stats("Saulo Knight", "harpy");
    CHECK(stats.attempts == 5 && stats.wins == 2 && stats.deaths == 1 && stats.quits == 1 && stats.best_ms == 90000);
    CHECK(stats.wins + stats.deaths + stats.quits <= stats.attempts);
    CHECK(boss_battle_record_note("Saulo Knight", "@fight_all", BOSS_BATTLE_ATTEMPT, 0) == 0); // never a boss id
    CHECK(Read(dir / "boss_battle.dat") == "DCP Boss Battle 3\nSaulo Knight\tharpy\t5\t2\t1\t1\t90000\n");
    CHECK(!fs::exists(dir / "boss_battle.dat.tmp"));

    boss_battle_record_configure(user.c_str()); // read back from disk
    CHECK(boss_battle_record_stats("Saulo Knight", "harpy").best_ms == 90000);

    const std::string foreign = "something else entirely\n";
    std::ofstream(dir / "boss_battle.dat", std::ios::binary) << foreign;
    boss_battle_record_configure(user.c_str());
    CHECK(boss_battle_record_note("Saulo Knight", "harpy", BOSS_BATTLE_ATTEMPT, 0) == 0);
    CHECK(Read(dir / "boss_battle.dat") == foreign);
    fs::remove_all(dir);
}

void TestFightAllFile() {
    const fs::path dir = fs::temp_directory_path() / "dcp_boss_battle_fight_all_tests";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string user = dir.string();
    // An existing second-format file is upgraded in place without losing a boss record.
    std::ofstream(dir / "boss_battle.dat", std::ios::binary) << "DCP Boss Battle 2\nZak|Fighter\tharpy\t3\t2\t1\t0\t95000\n";
    boss_battle_record_configure(user.c_str());
    const char* who = "Zak|Fighter";
    // A run closes once; progress never goes down; a clear needs a run.
    CHECK(boss_battle_record_fight_all_note(who, BOSS_BATTLE_FIGHT_ALL_CLEAR, 1000) == 0);
    CHECK(boss_battle_record_fight_all_note(who, BOSS_BATTLE_FIGHT_ALL_PROGRESS, 3) == 0);
    CHECK(boss_battle_record_fight_all_note(who, BOSS_BATTLE_FIGHT_ALL_RUN, 0) == 1);
    CHECK(boss_battle_record_fight_all_note(who, BOSS_BATTLE_FIGHT_ALL_PROGRESS, 3) == 1);
    CHECK(boss_battle_record_fight_all_note(who, BOSS_BATTLE_FIGHT_ALL_PROGRESS, 2) == 1);
    CHECK(boss_battle_record_fight_all_stats(who).best_progress == 3);
    CHECK(boss_battle_record_fight_all_note(who, BOSS_BATTLE_FIGHT_ALL_DEATH, 7) == 1);
    CHECK(boss_battle_record_fight_all_note(who, BOSS_BATTLE_FIGHT_ALL_QUIT, 9) == 0); // no open run
    CHECK(boss_battle_record_fight_all_note(who, BOSS_BATTLE_FIGHT_ALL_RUN, 0) == 1);
    CHECK(boss_battle_record_fight_all_note(who, BOSS_BATTLE_FIGHT_ALL_QUIT, 5) == 1);
    CHECK(boss_battle_record_fight_all_note(who, BOSS_BATTLE_FIGHT_ALL_RUN, 0) == 1);
    CHECK(boss_battle_record_fight_all_note(who, BOSS_BATTLE_FIGHT_ALL_CLEAR, 4000000) == 1);
    CHECK(boss_battle_record_fight_all_note(who, BOSS_BATTLE_FIGHT_ALL_RUN, 0) == 1);
    CHECK(boss_battle_record_fight_all_note(who, BOSS_BATTLE_FIGHT_ALL_CLEAR, 5000000) == 1);
    BossBattleFightAllStats stats = boss_battle_record_fight_all_stats(who);
    CHECK(stats.runs == 4 && stats.clears == 2 && stats.deaths == 1 && stats.quits == 1 && stats.best_progress == 20 &&
          stats.best_ms == 4000000); // a slower clear keeps the best time
    CHECK(boss_battle_record_fight_all_stats("Tia|Elf").runs == 0);
    CHECK(boss_battle_record_stats(who, "harpy").attempts == 3);
    CHECK(!fs::exists(dir / "boss_battle.dat.tmp"));

    boss_battle_record_configure(user.c_str()); // read back from disk
    stats = boss_battle_record_fight_all_stats(who);
    CHECK(stats.runs == 4 && stats.clears == 2 && stats.best_ms == 4000000 && stats.best_progress == 20);
    CHECK(boss_battle_record_stats(who, "harpy").best_ms == 95000);

    // A file this build cannot read is left alone, Fight All rows included.
    const std::string foreign = "DCP Boss Battle 3\nZak|Fighter\t@fight_all\t1\t0\n";
    std::ofstream(dir / "boss_battle.dat", std::ios::binary) << foreign;
    boss_battle_record_configure(user.c_str());
    CHECK(boss_battle_record_fight_all_note(who, BOSS_BATTLE_FIGHT_ALL_RUN, 0) == 0);
    CHECK(Read(dir / "boss_battle.dat") == foreign);
    fs::remove_all(dir);
}

} // namespace

int main() {
    TestFormat();
    TestFile();
    TestFightAllFile();
    if (failures) {
        std::printf("%d Boss Battle record check(s) failed\n", failures);
        return 1;
    }
    std::puts("Boss Battle record tests passed");
    return 0;
}
