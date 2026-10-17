#include "trophies/trophy_store.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

#include "trophies/trophy_data.h"

namespace fs = std::filesystem;

namespace dcp::trophies {
namespace {

constexpr char kHeader[] = "DCP-TROPHIES 1";

std::string ReadText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool ParseProgress(const std::string& text, const std::string& np_comm_id, std::map<int, int64_t>& out) {
    std::istringstream lines(text);
    std::string line;
    const auto next = [&] {
        const bool ok = static_cast<bool>(std::getline(lines, line));
        if (ok && !line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        return ok;
    };
    if (!next() || line != kHeader || !next() || line != "npcommid " + np_comm_id) {
        return false;
    }
    while (next()) {
        int id = 0;
        long long time = 0;
        char tail = 0;
        if (std::sscanf(line.c_str(), "unlocked %d %lld %c", &id, &time, &tail) == 2 && id >= 0 && id < kMaxTrophies &&
            time >= 0) {
            out.emplace(id, time);
        }
    }
    return true;
}

} // namespace

fs::path TrophyDirectory(const fs::path& user_dir, const std::string& np_comm_id) {
    return user_dir / "trophy" / np_comm_id;
}

bool IsNpCommId(const std::string& text) {
    if (text.size() != 12 || text.compare(0, 4, "NPWR") != 0 || text[9] != '_') {
        return false;
    }
    for (size_t i = 4; i < 12; ++i) {
        if (i != 9 && (text[i] < '0' || text[i] > '9')) {
            return false;
        }
    }
    return true;
}

TrophyStore::TrophyStore(fs::path trophy_dir, std::string np_comm_id, int user)
    : file_(trophy_dir / ("progress_user" + std::to_string(user) + ".txt")), np_comm_id_(std::move(np_comm_id)) {}

void TrophyStore::Load() {
    unlocked_.clear();
    std::error_code ec;
    if (file_.empty() || !fs::exists(file_, ec)) {
        return;
    }
    std::map<int, int64_t> parsed;
    if (ParseProgress(ReadText(file_), np_comm_id_, parsed)) {
        unlocked_ = std::move(parsed);
        return;
    }
    fs::path aside = file_;
    aside += ".corrupt";
    fs::rename(file_, aside, ec);
    std::printf("Trophies: progress file %s is not readable, set aside\n", file_.filename().string().c_str());
}

bool TrophyStore::Save() const {
    std::error_code ec;
    fs::create_directories(file_.parent_path(), ec);
    fs::path temp = file_;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << kHeader << "\nnpcommid " << np_comm_id_ << "\n";
        for (const auto& [id, time] : unlocked_) {
            out << "unlocked " << id << ' ' << time << "\n";
        }
        out.flush();
        if (!out) {
            out.close();
            fs::remove(temp, ec);
            return false;
        }
    }
    fs::rename(temp, file_, ec);
    if (ec) {
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

int64_t TrophyStore::UnlockTime(int id) const {
    const auto it = unlocked_.find(id);
    return it == unlocked_.end() ? 0 : it->second;
}

bool TrophyStore::Unlock(int id, int64_t unix_time) {
    if (id < 0 || id >= kMaxTrophies) {
        return false;
    }
    return unlocked_.emplace(id, unix_time < 0 ? 0 : unix_time).second;
}

} // namespace dcp::trophies
