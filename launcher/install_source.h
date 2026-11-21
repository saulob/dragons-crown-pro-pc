// Where the player installs the game from: a PKG file or an already extracted game folder. Both go to InstallGame.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "trg/platform.h"

namespace dcp {

enum class InstallSource { kPackage, kFolder };

inline constexpr char kInstallStartLabel[] = "Checking the game files...";

const char* InstallSourceLabel(InstallSource source);

// The desktop's choosers; the launcher passes the native ones.
struct SourceChoosers {
    std::function<std::string(const std::string& title, const std::vector<trg::FileFilter>& filters)> file;
    std::function<std::string(const std::string& title)> folder;
};

// The UTF-8 path the player chose; empty when the chooser was cancelled.
std::string ChooseInstallSource(InstallSource source, const SourceChoosers& choosers);

} // namespace dcp
