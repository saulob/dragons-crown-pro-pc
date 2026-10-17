// The supported game builds. scripts/game_profiles.py mirrors the identity fields; tests/launcher_tests.cpp checks they agree.
#pragma once

#include <cstddef>
#include <span>
#include <string>

#include "dcp_resolution.h"

namespace dcp {

inline constexpr char kGameTitle[] = "Dragon's Crown Pro";
inline constexpr char kPortRepository[] = "github.com/saulob/dragons-crown-pro-pc";

// A package of a build, by its param.sfo APP_VER and VERSION. An update (CATEGORY gp) is applied over the packages
// before it.
struct PackageLayer {
    const char* app_ver;
    const char* sfo_version;
    bool update;
};

struct GameBuild {
    const char* title_id;
    const char* content_id;
    const char* version;          // the profile version: the preparation marker's "version"
    const char* folder;           // under game\prepared and game\image; distinct for each build
    const char* eboot_sha256;     // lower-case hex of the finished game's eboot.bin
    const char* regional_archive; // nullptr when the build has none
    std::span<const char* const> extra_files; // the other files of the build's update, required like the archive
    const char* label;            // the name the log, the launcher and the loader show
    const char* region;
    bool cheats;                  // the native cheat hooks are validated for this build
    bool xbox_prompts;            // the Xbox prompt patch is validated for this build's archives
    std::span<const PackageLayer> layers; // base package first; the last layer is the finished game
    std::span<const DcpResolutionSite> resolution_sites;
};

// The supported builds, US first.
std::span<const GameBuild> SupportedBuilds();

// The finished game's layer: its param.sfo is the one of the installed game.
const PackageLayer& FinishedLayer(const GameBuild& build);

// The build whose base package a build's update is laid over, or nullptr when the build has no update.
const GameBuild* BaseBuildOf(const GameBuild& build, std::span<const GameBuild> builds = SupportedBuilds());

std::string SupportedBuildsText();

// Files every complete game folder has, whatever the build. The build's regional archive is required too.
inline constexpr const char* kSharedGameFiles[] = {
    "eboot.bin",
    "sce_discmap.plt",
    "DRACRO.CPK",
    "sce_module/libc.prx",
    "sce_module/libSceFios2.prx",
    "sce_module/libSceNpToolkit2.prx",
    "sce_sys/param.sfo",
};

// Linked after the eboot in this order: libc.prx needs libSceFios2.prx.
inline constexpr const char* kBundledModules[] = {"libc.prx", "libSceFios2.prx", "libSceNpToolkit2.prx"};

// Written last into a prepared game folder (scripts/game_view.py writes the same one).
inline constexpr char kPreparedMarker[] = ".dcp_view.json";

} // namespace dcp
