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
    CHECK(std::string(text) == "DCP Boss Battle 2\nSaulo\tharpy\t5\t2\t1\t1\t95000\nAna\tmedusa\t1\t0\t1\t0\t0\n");

    // The first format had no quits; it is read and written back in the current one.
    const std::string v1 = "DCP Boss Battle 1\nSaulo\tharpy\t3\t2\t1\t95000\n";
    CHECK(boss_battle_record_parse(v1.data(), v1.size()) == 1);
    const BossBattleStats old = boss_battle_record_stats("Saulo", "harpy");
    CHECK(old.attempts == 3 && old.wins == 2 && old.deaths == 1 && old.quits == 0 && old.best_ms == 95000);
    CHECK(boss_battle_record_format(text, sizeof(text)) && std::string(text) == "DCP Boss Battle 2\nSaulo\tharpy\t3\t2\t1\t0\t95000\n");

    for (const char* bad : {"", "DCP Boss Battle 3\n", "DCP Boss Battle 2\nSaulo\tharpy\t1\t2\t3\n",
                            "DCP Boss Battle 2\nSaulo\tharpy\tx\t0\t0\t0\t0\n", "DCP Boss Battle 2\n\tharpy\t1\t0\t0\t0\t0\n",
                            "DCP Boss Battle 2\nSaulo\tharpy\t1\t0\t0\t0\t99999999999\n",
                            "DCP Boss Battle 2\nSaulo\tharpy\t1\t1\t1\t0\t0\n", "DCP Boss Battle 1\nSaulo\tharpy\t1\t1\t0\t0\t0\n"}) {
        CHECK(boss_battle_record_parse(bad, std::strlen(bad)) == 0);
    }
    CHECK(boss_battle_record_stats("Saulo", "harpy").attempts == 3); // a rejected text changes nothing
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
    CHECK(Read(dir / "boss_battle.dat") == "DCP Boss Battle 2\nSaulo Knight\tharpy\t5\t2\t1\t1\t90000\n");
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

} // namespace

int main() {
    TestFormat();
    TestFile();
    if (failures) {
        std::printf("%d Boss Battle record check(s) failed\n", failures);
        return 1;
    }
    std::puts("Boss Battle record tests passed");
    return 0;
}
