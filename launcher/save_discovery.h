// Discovery of Dragon's Crown Pro saves under the user savedata hierarchy
// (<user>/savedata/<user id>/<title id>/<dir>/SAVE0.DAT), reusing the layout runtime_savedata.c establishes.
#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "save_data.h"

namespace dcp {

inline constexpr std::string_view kSavedataFolder = "savedata";

// The title ids the Pro builds write, in the order SupportedBuilds lists them.
inline constexpr std::string_view kProTitleIds[] = {"CUSA10454", "CUSA07001", "CUSA10209", "CUSA10487"};

// A known title id: the platform and region it belongs to, for import detection and the save list.
struct TitleInfo {
    std::string_view platform; // "PS4" (Pro), "Vita", "PS3"
    std::string_view region;
};

// "PS4" and the region for a Pro title id, or "" when it is not one.
const TitleInfo* ProTitleInfo(std::string_view title_id);

struct DiscoveredSave {
    std::filesystem::path save0_path; // <...>/SAVE0.DAT
    std::filesystem::path folder;     // the dir holding SAVE0.DAT
    std::string title_id;
    std::string dir_name; // the DATA-slot folder name (SAVEDATA00, ...)
    std::string region;
    std::filesystem::file_time_type modified;
    bool current = false; // matches the active build's title id
    std::string error;    // set when the file could not be parsed
    SaveModel model;
};

// Enumerates every SAVE0.DAT under <user_dir>/savedata, parsing each. The save matching `active_title_id` (when given)
// is marked current. Fails to parse are still listed with their error. Sorted by title id then folder name.
std::vector<DiscoveredSave> DiscoverSaves(const std::filesystem::path& user_dir, std::string_view active_title_id);

// The indexes of the saves that belong to `title_id`: the one definition of which saves a game build owns.
std::vector<size_t> SavesOfTitle(std::span<const DiscoveredSave> saves, std::string_view title_id);

// The index in `saves` to start on, among `visible`: the first of `preferred` that is visible, else the first valid
// visible save, else the first visible one, else -1.
int ChooseSave(std::span<const DiscoveredSave> saves, std::span<const size_t> visible,
               std::span<const std::filesystem::path> preferred);

// The folder the launcher writes its backups to.
std::filesystem::path BackupFolder(const std::filesystem::path& user_dir);

} // namespace dcp
