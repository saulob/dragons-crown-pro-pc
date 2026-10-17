#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace dcp::trophies {

// <user dir>/trophy/<NP Communication ID>: the pack's extracted files and the users' progress.
std::filesystem::path TrophyDirectory(const std::filesystem::path& user_dir, const std::string& np_comm_id);
// Real NP Communication IDs only (NPWRnnnnn_nn): the id becomes a folder name.
bool IsNpCommId(const std::string& text);

class TrophyStore {
public:
    TrophyStore() = default;
    TrophyStore(std::filesystem::path trophy_dir, std::string np_comm_id, int user);

    void Load();
    bool Save() const;

    bool IsUnlocked(int id) const { return unlocked_.count(id) != 0; }
    // Seconds since the Unix epoch, 0 when locked.
    int64_t UnlockTime(int id) const;
    // True only when the trophy was locked; unlocking twice keeps the first time.
    bool Unlock(int id, int64_t unix_time);
    int UnlockedCount() const { return static_cast<int>(unlocked_.size()); }
    const std::map<int, int64_t>& Unlocked() const { return unlocked_; }

    const std::filesystem::path& file() const { return file_; }

private:
    std::filesystem::path file_;
    std::string np_comm_id_;
    std::map<int, int64_t> unlocked_;
};

} // namespace dcp::trophies
