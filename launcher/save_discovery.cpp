#include "save_discovery.h"

#include <algorithm>
#include <iterator>
#include <system_error>

namespace dcp {
namespace {

struct RegionEntry {
    std::string_view title_id;
    std::string_view region;
};

// The Pro builds and their regions, mirroring game_profile.cpp.
constexpr RegionEntry kProRegions[] = {
    {"CUSA10454", "USA"},
    {"CUSA07001", "Japan"},
    {"CUSA10209", "Asia/Hong Kong"},
    {"CUSA10487", "Europe"},
};

bool IsDirectory(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

} // namespace

const TitleInfo* ProTitleInfo(std::string_view title_id) {
    static const TitleInfo kInfos[] = {
        {"PS4", "USA"},
        {"PS4", "Japan"},
        {"PS4", "Asia/Hong Kong"},
        {"PS4", "Europe"},
    };
    for (size_t i = 0; i < std::size(kProRegions); ++i) {
        if (title_id == kProRegions[i].title_id) {
            return &kInfos[i];
        }
    }
    return nullptr;
}

std::vector<DiscoveredSave> DiscoverSaves(const std::filesystem::path& user_dir, std::string_view active_title_id) {
    std::vector<DiscoveredSave> saves;
    std::error_code ec;
    const std::filesystem::path root = user_dir / std::string(kSavedataFolder);
    if (!IsDirectory(root)) {
        return saves;
    }
    for (std::filesystem::directory_iterator user(root, ec); !ec && user != std::filesystem::directory_iterator();
         user.increment(ec)) {
        if (!IsDirectory(user->path())) {
            continue;
        }
        for (std::filesystem::directory_iterator title(user->path(), ec);
             !ec && title != std::filesystem::directory_iterator(); title.increment(ec)) {
            if (!IsDirectory(title->path())) {
                continue;
            }
            for (std::filesystem::directory_iterator dir(title->path(), ec);
                 !ec && dir != std::filesystem::directory_iterator(); dir.increment(ec)) {
                if (!IsDirectory(dir->path())) {
                    continue;
                }
                const std::filesystem::path save0 = dir->path() / "SAVE0.DAT";
                if (!std::filesystem::is_regular_file(save0, ec)) {
                    continue;
                }
                DiscoveredSave save;
                save.save0_path = save0;
                save.folder = dir->path();
                save.title_id = title->path().filename().string();
                save.dir_name = dir->path().filename().string();
                if (const TitleInfo* info = ProTitleInfo(save.title_id)) {
                    save.region = info->region;
                }
                save.modified = std::filesystem::last_write_time(save0, ec);
                save.current = !active_title_id.empty() && save.title_id == active_title_id;
                save.model = SaveModel::FromFile(save0);
                if (!save.model.valid()) {
                    save.error = save.model.errors().empty() ? "could not parse" : save.model.errors().front();
                }
                saves.push_back(std::move(save));
            }
        }
    }
    std::sort(saves.begin(), saves.end(), [](const DiscoveredSave& a, const DiscoveredSave& b) {
        if (a.title_id != b.title_id) {
            return a.title_id < b.title_id;
        }
        return a.dir_name < b.dir_name;
    });
    return saves;
}

std::filesystem::path BackupFolder(const std::filesystem::path& user_dir) { return user_dir / "save_backups"; }

} // namespace dcp
