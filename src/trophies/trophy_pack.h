#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "trophies/trophy_data.h"
#include "trophies/trp_archive.h"

namespace dcp::trophies {

enum class PackStatus {
    kReady,
    kNoNpBind,    // sce_sys/npbind.dat is missing or names no NP Communication ID
    kNoPack,      // sce_sys/trophy/trophy00.trp is missing
    kKeyMissing,  // the lists are encrypted and user/keys.json holds no valid trophy key
    kKeyRejected, // the trophy key does not decrypt this game's lists
    kDamaged,
};

struct PackResult {
    PackStatus status = PackStatus::kNoNpBind;
    std::string np_comm_id;
};

const char* DescribePack(PackStatus status);

// The NP Communication ID of the title's first trophy set.
std::optional<std::string> ReadNpCommId(const std::filesystem::path& npbind);

// ReleaseTrophyKey of <user dir>/keys.json: exactly 32 hex digits in a string; nullopt for anything else.
std::optional<TrophyKey> LoadTrophyKey(const std::filesystem::path& user_dir);

PackResult PrepareTrophyPack(const std::filesystem::path& app0, const std::filesystem::path& user_dir);

std::optional<TrophySet> LoadTrophySet(const std::filesystem::path& user_dir, const std::string& np_comm_id);

std::filesystem::path TrophyIconPath(const std::filesystem::path& user_dir, const std::string& np_comm_id, int id);

} // namespace dcp::trophies
