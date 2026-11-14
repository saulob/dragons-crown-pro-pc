#include "boss_battle_record.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "utf8.h"

namespace {

constexpr char kHeader[] = "DCP Boss Battle 3";
constexpr char kHeaderV2[] = "DCP Boss Battle 2";
constexpr char kHeaderV1[] = "DCP Boss Battle 1";
constexpr char kFightAllId[] = "@fight_all";
constexpr char kFileName[] = "boss_battle.dat";
constexpr size_t kCharacterLimit = 64;

struct Row {
    std::string character, boss;
    BossBattleStats stats{};
};

struct FightAllRow {
    std::string character;
    BossBattleFightAllStats stats{};
};

std::mutex lock;
std::vector<Row> rows;
std::vector<FightAllRow> fight_all_rows;
std::filesystem::path file;
bool writable = false;

std::string Clean(const char* text, size_t limit) {
    std::string out(text ? text : "");
    for (char& c : out) {
        if (c == '\t' || c == '\r' || c == '\n') {
            c = ' ';
        }
    }
    return out.substr(0, limit);
}

Row* Find(const std::string& character, const std::string& boss) {
    for (Row& row : rows) {
        if (row.character == character && row.boss == boss) {
            return &row;
        }
    }
    return nullptr;
}

FightAllRow* FindFightAll(const std::string& character) {
    for (FightAllRow& row : fight_all_rows) {
        if (row.character == character) {
            return &row;
        }
    }
    return nullptr;
}

bool ParseNumber(const std::string& text, uint32_t* value) {
    if (text.empty() || text.size() > 10) {
        return false;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
    if (*end || parsed > 0xffffffffUL) {
        return false;
    }
    *value = uint32_t(parsed);
    return true;
}

bool ParseFightAll(const std::vector<std::string>& fields, FightAllRow* row) {
    uint32_t numbers[6] = {};
    for (size_t i = 0; i < 6; ++i) {
        if (!ParseNumber(fields[i + 2], &numbers[i])) {
            return false;
        }
    }
    row->character = fields[0];
    row->stats = {numbers[0], numbers[1], numbers[2], numbers[3], numbers[4], numbers[5]};
    const BossBattleFightAllStats& stats = row->stats;
    return uint64_t(stats.clears) + stats.deaths + stats.quits <= stats.runs &&
           stats.best_progress <= BOSS_BATTLE_FIGHT_ALL_BOSSES;
}

bool Parse(const std::string& text, std::vector<Row>* out, std::vector<FightAllRow>* fight_all_out) {
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || (line != kHeader && line != kHeaderV2 && line != kHeaderV1)) {
        return false;
    }
    const bool has_fight_all = line == kHeader;
    const size_t numbers_count = line == kHeaderV1 ? 4 : 5;
    std::vector<Row> parsed;
    std::vector<FightAllRow> parsed_fight_all;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        std::vector<std::string> fields;
        std::istringstream cells(line);
        for (std::string cell; std::getline(cells, cell, '\t');) {
            fields.push_back(cell);
        }
        if (fields.size() < 2 || fields[0].empty() || fields[1].empty()) {
            return false;
        }
        if (has_fight_all && fields[1] == kFightAllId) {
            FightAllRow fight_all;
            if (fields.size() != 8 || !ParseFightAll(fields, &fight_all)) {
                return false;
            }
            parsed_fight_all.push_back(fight_all);
            continue;
        }
        Row row;
        uint32_t numbers[5] = {};
        if (fields.size() != numbers_count + 2 || fields[1][0] == '@') {
            return false;
        }
        for (size_t i = 0; i < numbers_count; ++i) {
            if (!ParseNumber(fields[i + 2], &numbers[i])) {
                return false;
            }
        }
        row.character = fields[0];
        row.boss = fields[1];
        if (numbers_count == 5) {
            row.stats = {numbers[0], numbers[1], numbers[2], numbers[3], numbers[4]};
        } else {
            row.stats = {numbers[0], numbers[1], numbers[2], 0, numbers[3]};
        }
        if (row.stats.wins + row.stats.deaths + row.stats.quits > row.stats.attempts) {
            return false;
        }
        parsed.push_back(row);
    }
    *out = std::move(parsed);
    *fight_all_out = std::move(parsed_fight_all);
    return true;
}

std::string Format() {
    std::string text = std::string(kHeader) + "\n";
    for (const Row& row : rows) {
        text += row.character + "\t" + row.boss + "\t" + std::to_string(row.stats.attempts) + "\t" +
                std::to_string(row.stats.wins) + "\t" + std::to_string(row.stats.deaths) + "\t" +
                std::to_string(row.stats.quits) + "\t" + std::to_string(row.stats.best_ms) + "\n";
    }
    for (const FightAllRow& row : fight_all_rows) {
        const BossBattleFightAllStats& stats = row.stats;
        text += row.character + "\t" + kFightAllId + "\t" + std::to_string(stats.runs) + "\t" +
                std::to_string(stats.clears) + "\t" + std::to_string(stats.deaths) + "\t" + std::to_string(stats.quits) +
                "\t" + std::to_string(stats.best_progress) + "\t" + std::to_string(stats.best_ms) + "\n";
    }
    return text;
}

FILE* OpenForWrite(const std::filesystem::path& path) {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}

bool Replace(const std::filesystem::path& from, const std::filesystem::path& to) {
#ifdef _WIN32
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

// The new contents reach the disk under a temporary name first, so a crash leaves the old file or the new one.
bool WriteAtomically(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    FILE* out = OpenForWrite(temporary);
    if (!out) {
        return false;
    }
    bool ok = std::fwrite(text.data(), 1, text.size(), out) == text.size() && std::fflush(out) == 0;
#ifdef _WIN32
    ok = ok && FlushFileBuffers(HANDLE(_get_osfhandle(_fileno(out))));
#else
    ok = ok && fsync(fileno(out)) == 0;
#endif
    ok = std::fclose(out) == 0 && ok;
    if (ok && Replace(temporary, path)) {
        return true;
    }
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return false;
}

} // namespace

extern "C" void boss_battle_record_configure(const char* user_dir) {
    std::scoped_lock guard{lock};
    rows.clear();
    fight_all_rows.clear();
    file = dcp::PathFromUtf8(user_dir && *user_dir ? user_dir : "user") / kFileName;
    std::error_code error;
    if (!std::filesystem::exists(file, error)) {
        writable = !error;
        return;
    }
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    writable = in.good() || in.eof() ? Parse(text, &rows, &fight_all_rows) : false;
    if (!writable) {
        rows.clear();
        fight_all_rows.clear();
        std::printf("Boss Battle: %s is not readable; it is left untouched and nothing is recorded\n", kFileName);
    }
}

extern "C" int boss_battle_record_note(const char* character, const char* boss_id, BossBattleOutcome outcome, uint32_t ms) {
    std::scoped_lock guard{lock};
    if (!writable || file.empty()) {
        return 0;
    }
    const std::string who = Clean(character, kCharacterLimit), boss = Clean(boss_id, kCharacterLimit);
    if (who.empty() || boss.empty() || boss[0] == '@') {
        return 0;
    }
    Row* row = Find(who, boss);
    if (!row) {
        if (outcome != BOSS_BATTLE_ATTEMPT) {
            return 0;
        }
        rows.push_back({who, boss, {}});
        row = &rows.back();
    }
    BossBattleStats& stats = row->stats;
    if (outcome != BOSS_BATTLE_ATTEMPT && stats.wins + stats.deaths + stats.quits >= stats.attempts) {
        std::printf("Boss Battle: no open attempt for %s; outcome not recorded\n", boss.c_str());
        return 0;
    }
    switch (outcome) {
    case BOSS_BATTLE_ATTEMPT: ++stats.attempts; break;
    case BOSS_BATTLE_DEATH: ++stats.deaths; break;
    case BOSS_BATTLE_QUIT: ++stats.quits; break;
    case BOSS_BATTLE_WIN:
        ++stats.wins;
        if (ms && (!stats.best_ms || ms < stats.best_ms)) {
            stats.best_ms = ms;
        }
        break;
    }
    const bool written = WriteAtomically(file, Format());
    std::printf("Boss Battle: metadata %s (%s)\n", written ? "written" : "NOT written", kFileName);
    return written;
}

extern "C" int boss_battle_record_fight_all_note(const char* character, BossBattleFightAllEvent event, unsigned value) {
    std::scoped_lock guard{lock};
    if (!writable || file.empty()) {
        return 0;
    }
    const std::string who = Clean(character, kCharacterLimit);
    if (who.empty()) {
        return 0;
    }
    FightAllRow* row = FindFightAll(who);
    if (!row) {
        if (event != BOSS_BATTLE_FIGHT_ALL_RUN) {
            return 0;
        }
        fight_all_rows.push_back({who, {}});
        row = &fight_all_rows.back();
    }
    BossBattleFightAllStats& stats = row->stats;
    const bool closes_run = event == BOSS_BATTLE_FIGHT_ALL_CLEAR || event == BOSS_BATTLE_FIGHT_ALL_DEATH ||
                            event == BOSS_BATTLE_FIGHT_ALL_QUIT;
    if (closes_run && stats.clears + stats.deaths + stats.quits >= stats.runs) {
        std::printf("Boss Battle: no open Fight All run; outcome not recorded\n");
        return 0;
    }
    const unsigned progress = std::min<unsigned>(value, BOSS_BATTLE_FIGHT_ALL_BOSSES);
    switch (event) {
    case BOSS_BATTLE_FIGHT_ALL_RUN: ++stats.runs; break;
    case BOSS_BATTLE_FIGHT_ALL_PROGRESS:
        if (!stats.runs || progress <= stats.best_progress) {
            return 1;
        }
        stats.best_progress = progress;
        break;
    case BOSS_BATTLE_FIGHT_ALL_CLEAR:
        ++stats.clears;
        stats.best_progress = BOSS_BATTLE_FIGHT_ALL_BOSSES;
        if (value && (!stats.best_ms || value < stats.best_ms)) {
            stats.best_ms = value;
        }
        break;
    case BOSS_BATTLE_FIGHT_ALL_DEATH:
    case BOSS_BATTLE_FIGHT_ALL_QUIT:
        ++(event == BOSS_BATTLE_FIGHT_ALL_DEATH ? stats.deaths : stats.quits);
        stats.best_progress = std::max(stats.best_progress, progress);
        break;
    }
    const bool written = WriteAtomically(file, Format());
    std::printf("Boss Battle: metadata %s (%s)\n", written ? "written" : "NOT written", kFileName);
    return written;
}

extern "C" BossBattleFightAllStats boss_battle_record_fight_all_stats(const char* character) {
    std::scoped_lock guard{lock};
    const FightAllRow* row = FindFightAll(Clean(character, kCharacterLimit));
    return row ? row->stats : BossBattleFightAllStats{};
}

extern "C" BossBattleStats boss_battle_record_stats(const char* character, const char* boss_id) {
    std::scoped_lock guard{lock};
    const Row* row = Find(Clean(character, kCharacterLimit), Clean(boss_id, kCharacterLimit));
    return row ? row->stats : BossBattleStats{};
}

extern "C" int boss_battle_record_parse(const char* text, size_t length) {
    std::scoped_lock guard{lock};
    std::vector<Row> parsed;
    std::vector<FightAllRow> parsed_fight_all;
    if (!Parse(std::string(text ? text : "", text ? length : 0), &parsed, &parsed_fight_all)) {
        return 0;
    }
    rows = std::move(parsed);
    fight_all_rows = std::move(parsed_fight_all);
    return 1;
}

extern "C" size_t boss_battle_record_format(char* out, size_t size) {
    std::scoped_lock guard{lock};
    const std::string text = Format();
    if (out && size) {
        std::snprintf(out, size, "%s", text.c_str());
    }
    return text.size();
}

extern "C" void boss_battle_record_clear(void) {
    std::scoped_lock guard{lock};
    rows.clear();
    fight_all_rows.clear();
}
