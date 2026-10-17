// The one readiness check for the game: the launcher, Play and the installer all use it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

#include "game_profile.h"

namespace dcp {

enum class GameFileState { kMissing, kValid, kDifferentVersion, kReadError };

// The prepared /app0 tree (game\prepared\<folder>) and the loader's boot image (game\image\<folder>) of one build.
struct GamePaths {
    std::filesystem::path app0;
    std::filesystem::path image_dir;
};

// The two folders the builds' folders sit in: game\prepared and game\image.
struct GameRoots {
    std::filesystem::path prepared;
    std::filesystem::path image;
};

struct GameCheck {
    GameFileState state = GameFileState::kMissing;
    std::string detail;       // for the log, not the UI
    std::string eboot_sha256; // lower-case hex, set once eboot.bin was read
    const GameBuild* build = nullptr; // the supported build the files were identified as
    size_t layer = 0;                 // for a package: its index in build->layers
};

// The "scope" of a folder's preparation marker: "full", "core" (development) or empty.
std::string PreparationScope(const std::filesystem::path& app0);
GamePaths BuildPaths(const GameRoots& roots, const GameBuild& build);
// The build whose complete folder was prepared last, or nullptr when no supported build is complete.
const GameBuild* LastPreparedBuild(const GameRoots& roots, std::span<const GameBuild> builds = SupportedBuilds());
// The folders of the installed build: the complete game that was prepared last, else the first build's.
GamePaths ActiveBuildPaths(const GameRoots& roots, std::span<const GameBuild> builds = SupportedBuilds());
GameRoots RootsOf(const GamePaths& paths);

// Reads the build from param.sfo, then the preparation marker, the boot image report and eboot.bin (about 21 MB). The
// game data archives are only checked for presence: the marker records that their extraction finished.
GameCheck CheckGameFiles(const GamePaths& paths, std::span<const GameBuild> builds = SupportedBuilds());

// The game itself, without a preparation marker or boot image: param.sfo, the required files and eboot.bin. The
// installer checks what it unpacked or the folder the player chose with it.
GameCheck CheckGameTree(const std::filesystem::path& app0, std::span<const GameBuild> builds = SupportedBuilds());

// TITLE_ID, CONTENT_ID, APP_VER and VERSION of a finished game's param.sfo name a supported build.
GameCheck CheckSfoIdentity(const std::string& sfo, std::span<const GameBuild> builds = SupportedBuilds());

// The build whose finished game a package's param.sfo names, and the package's layer in that build.
GameCheck CheckPackageIdentity(const std::string& sfo, std::span<const GameBuild> builds = SupportedBuilds());

// A string value of param.sfo, empty when the key is absent or not a string.
std::string SfoString(const std::string& sfo, const char* key);
// An integer value of param.sfo: `absent` when the key is missing, nothing when it is not an integer.
std::optional<uint32_t> SfoInteger(const std::string& sfo, const char* key, uint32_t absent);

// Lower-case hex SHA-256 of a file, empty when it cannot be read.
std::string Sha256Hex(const std::filesystem::path& file);

// The string value of a top-level-looking "key": "value" pair in JSON text; empty when absent.
std::string JsonString(const std::string& text, const std::string& key);

const char* StateName(GameFileState state);

// The About page's name for the installed game, or "Not installed".
std::string InstalledGameName(GameFileState state, const GameBuild* build);

} // namespace dcp
