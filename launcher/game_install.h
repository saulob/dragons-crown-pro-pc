// Installs the game from the player's own fake PKG (or an already extracted game folder): checks it, unpacks or lays
// it out in staging folders next to the prepared game, builds the boot image, checks the result and only then swaps it
// in. Settings, saves, logs and the shader cache live elsewhere and are never touched.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "game_files.h"

namespace dcp {

enum class InstallStatus {
    kInstalled,
    kActivated,
    kCancelled,
    kNotPackage,
    kUnsupportedPackage,
    kNotGameFolder,
    kWrongGame,
    kWrongVersion,
    kIncomplete,
    kReadFailed,
    kOverlapping,
    kForeignFolder,
    kNoSpace,
    kWriteFailed,
    kPrepareFailed,
    kUpdateNeedsBase,
    kUpToDate,
    kUnsupportedBuild,
};

struct InstallResult {
    InstallStatus status = InstallStatus::kInstalled;
    std::string detail; // for the log, not the UI
    std::vector<std::string> diagnostics; // kUnsupportedBuild: the block for the log
    std::vector<std::string> site_report; // a build whose sites are only searched for: the block for the log
    std::vector<std::string> language_report; // an installed update: its base's, its own and the finished language slots
    uint64_t bytes = 0;
    size_t files = 0, linked = 0;
    const GameBuild* build = nullptr; // the build that was installed
    bool ok() const {
        return status == InstallStatus::kInstalled || status == InstallStatus::kActivated || status == InstallStatus::kUpToDate;
    }
};

struct InstallHooks {
    std::function<void(const char* phase)> phase;
    // Returns false to cancel. Cancelling is possible until the new files are activated.
    std::function<bool(uint64_t done, uint64_t total)> progress;
    bool replace_active = false; // install the package even when its build is the active, ready one
};

// The build is the one the source identifies as; it is installed into BuildPaths(roots, build). An update is laid over the
// installed base build of its title (kUpdateNeedsBase when that is not installed). On any failure or cancellation that
// build's current installation is left as it was. The source is a .pkg file or a game folder.
InstallResult InstallGame(const std::filesystem::path& source, const GameRoots& roots, const InstallHooks& hooks = {},
                          std::span<const GameBuild> builds = SupportedBuilds());
InstallResult InstallFromPackage(const std::filesystem::path& package, const GameRoots& roots,
                                 const InstallHooks& hooks = {}, std::span<const GameBuild> builds = SupportedBuilds());
InstallResult InstallFromFolder(const std::filesystem::path& source, const GameRoots& roots,
                                const InstallHooks& hooks = {}, std::span<const GameBuild> builds = SupportedBuilds());

// Lays an unpacked update over an installed base game in `staged_app0`, which must not exist yet. The base game is not
// changed: its files are linked, and the update's files replace the links. An update archive that lacks some of the
// base archive's entries is composed instead: the base's with the update's entries laid over it, or, for a build that
// keeps the base's languages, with only the update's entries outside the language slots.
InstallResult StageUpdate(const std::filesystem::path& base_app0, const std::filesystem::path& update_dir,
                          const std::filesystem::path& staged_app0, const GameBuild* build = nullptr);

struct InstallMessage {
    std::string title;
    std::string text;
};
InstallMessage DescribeInstall(InstallStatus status);

} // namespace dcp
