#include "boss_battle_record.h"

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

constexpr char kHeader[] = "DCP Boss Battle 2";
constexpr char kHeaderV1[] = "DCP Boss Battle 1";
constexpr char kFileName[] = "boss_battle.dat";
constexpr size_t kCharacterLimit = 64;

struct Row {
    std::string character, boss;
    BossBattleStats stats{};
};

std::mutex lock;
std::vector<Row> rows;
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

bool Parse(const std::string& text, std::vector<Row>* out) {
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || (line != kHeader && line != kHeaderV1)) {
        return false;
    }
    const size_t numbers_count = line == kHeader ? 5 : 4;
    std::vector<Row> parsed;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        std::vector<std::string> fields;
        std::istringstream cells(line);
        for (std::string cell; std::getline(cells, cell, '\t');) {
            fields.push_back(cell);
        }
        Row row;
        uint32_t numbers[5] = {};
        if (fields.size() != numbers_count + 2 || fields[0].empty() || fields[1].empty()) {
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
    return true;
}

std::string Format() {
    std::string text = std::string(kHeader) + "\n";
    for (const Row& row : rows) {
        text += row.character + "\t" + row.boss + "\t" + std::to_string(row.stats.attempts) + "\t" +
                std::to_string(row.stats.wins) + "\t" + std::to_string(row.stats.deaths) + "\t" +
                std::to_string(row.stats.quits) + "\t" + std::to_string(row.stats.best_ms) + "\n";
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
    file = dcp::PathFromUtf8(user_dir && *user_dir ? user_dir : "user") / kFileName;
    std::error_code error;
    if (!std::filesystem::exists(file, error)) {
        writable = !error;
        return;
    }
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    writable = in.good() || in.eof() ? Parse(text, &rows) : false;
    if (!writable) {
        rows.clear();
        std::printf("Boss Battle: %s is not readable; it is left untouched and nothing is recorded\n", kFileName);
    }
}

extern "C" int boss_battle_record_note(const char* character, const char* boss_id, BossBattleOutcome outcome, uint32_t ms) {
    std::scoped_lock guard{lock};
    if (!writable || file.empty()) {
        return 0;
    }
    const std::string who = Clean(character, kCharacterLimit), boss = Clean(boss_id, kCharacterLimit);
    if (who.empty() || boss.empty()) {
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

extern "C" BossBattleStats boss_battle_record_stats(const char* character, const char* boss_id) {
    std::scoped_lock guard{lock};
    const Row* row = Find(Clean(character, kCharacterLimit), Clean(boss_id, kCharacterLimit));
    return row ? row->stats : BossBattleStats{};
}

extern "C" int boss_battle_record_parse(const char* text, size_t length) {
    std::scoped_lock guard{lock};
    std::vector<Row> parsed;
    if (!Parse(std::string(text ? text : "", text ? length : 0), &parsed)) {
        return 0;
    }
    rows = std::move(parsed);
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
}

namespace {

constexpr char kProgressHeader[] = "DCP Boss Battle Progress 1";
constexpr char kProgressFileName[] = "boss_battle_progress.dat";
constexpr int32_t kMaxRecordLevel = 0xfe;

struct ProgressRow {
    std::string character;
    BossBattleProgress progress{};
};

std::vector<ProgressRow> progress_rows;
std::filesystem::path progress_file;
bool progress_writable = false;

bool ValidProgress(const BossBattleProgress& p) {
    return p.level >= 0 && p.level <= kMaxRecordLevel && p.xp_pool >= 0 && p.xp_progress >= 0;
}

bool ParseProgress(const std::string& text, std::vector<ProgressRow>* out) {
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line != kProgressHeader) {
        return false;
    }
    std::vector<ProgressRow> parsed;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        std::vector<std::string> fields;
        std::istringstream cells(line);
        for (std::string cell; std::getline(cells, cell, '\t');) {
            fields.push_back(cell);
        }
        uint32_t numbers[3] = {};
        if (fields.size() != 4 || fields[0].empty()) {
            return false;
        }
        for (size_t i = 0; i < 3; ++i) {
            if (!ParseNumber(fields[i + 1], &numbers[i]) || numbers[i] > INT32_MAX) {
                return false;
            }
        }
        ProgressRow row{fields[0], {int32_t(numbers[0]), int32_t(numbers[1]), int32_t(numbers[2])}};
        if (!ValidProgress(row.progress)) {
            return false;
        }
        parsed.push_back(row);
    }
    *out = std::move(parsed);
    return true;
}

std::string FormatProgress() {
    std::string text = std::string(kProgressHeader) + "\n";
    for (const ProgressRow& row : progress_rows) {
        text += row.character + "\t" + std::to_string(row.progress.level) + "\t" + std::to_string(row.progress.xp_pool) +
                "\t" + std::to_string(row.progress.xp_progress) + "\n";
    }
    return text;
}

ProgressRow* FindProgress(const std::string& character) {
    for (ProgressRow& row : progress_rows) {
        if (row.character == character) {
            return &row;
        }
    }
    return nullptr;
}

} // namespace

extern "C" void boss_battle_progress_configure(const char* user_dir) {
    std::scoped_lock guard{lock};
    progress_rows.clear();
    progress_file = dcp::PathFromUtf8(user_dir && *user_dir ? user_dir : "user") / kProgressFileName;
    std::error_code error;
    if (!std::filesystem::exists(progress_file, error)) {
        progress_writable = !error;
        return;
    }
    std::ifstream in(progress_file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    progress_writable = in.good() || in.eof() ? ParseProgress(text, &progress_rows) : false;
    if (!progress_writable) {
        progress_rows.clear();
        std::printf("Boss Battle: %s is not readable; it is left untouched and the campaign level is used\n",
                    kProgressFileName);
    }
}

extern "C" int boss_battle_progress_get(const char* character, BossBattleProgress* out) {
    std::scoped_lock guard{lock};
    const ProgressRow* row = FindProgress(Clean(character, kCharacterLimit));
    if (!row || !out) {
        return 0;
    }
    *out = row->progress;
    return 1;
}

extern "C" int boss_battle_progress_put(const char* character, const BossBattleProgress* progress) {
    std::scoped_lock guard{lock};
    const std::string who = Clean(character, kCharacterLimit);
    if (!progress_writable || progress_file.empty() || who.empty() || !progress || !ValidProgress(*progress)) {
        return 0;
    }
    ProgressRow* row = FindProgress(who);
    if (row) {
        row->progress = *progress;
    } else {
        progress_rows.push_back({who, *progress});
    }
    const bool written = WriteAtomically(progress_file, FormatProgress());
    std::printf("Boss Battle: progression %s (%s)\n", written ? "written" : "NOT written", kProgressFileName);
    return written;
}

extern "C" int boss_battle_progress_parse(const char* text, size_t length) {
    std::scoped_lock guard{lock};
    std::vector<ProgressRow> parsed;
    if (!ParseProgress(std::string(text ? text : "", text ? length : 0), &parsed)) {
        return 0;
    }
    progress_rows = std::move(parsed);
    return 1;
}

extern "C" size_t boss_battle_progress_format(char* out, size_t size) {
    std::scoped_lock guard{lock};
    const std::string text = FormatProgress();
    if (out && size) {
        std::snprintf(out, size, "%s", text.c_str());
    }
    return text.size();
}
