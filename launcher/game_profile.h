// The supported game builds. scripts/game_profiles.py mirrors the identity fields; tests/launcher_tests.cpp checks they agree.
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "dcp_resolution.h"

namespace dcp {

inline constexpr char kGameTitle[] = "Dragon's Crown Pro";
inline constexpr char kPortRepository[] = "github.com/saulob/dragons-crown-pro-pc";
inline constexpr char kAuthorUrl[] = "https://github.com/saulob";
inline constexpr char kSponsorUrl[] = "https://github.com/sponsors/saulob";

// A package of a build, by its param.sfo APP_VER and VERSION. An update (CATEGORY gp) is applied over the packages
// before it.
struct PackageLayer {
    const char* app_ver;
    const char* sfo_version;
    bool update;
};

inline constexpr std::string_view kPortugueseBrazilKey = "brazilian_portuguese";

// A system language the game can be told for its menus and subtitles; a build lists the ones its executable maps.
struct TextLanguage {
    std::string_view key;
    std::string_view name;
    int ps4_id;
    std::string_view suffix; // of its files in the regional archive (msgsheet/SystemMsg_FR.fms); empty when not there
    std::string_view translation = {}; // the port's translation laid over the suffix's sheets (localization/<translation>)
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
    bool xbox_prompts;            // the Xbox prompt patch fits this build's button atlases; an update's are checked too
    bool japanese_npc_voices;     // the NPC voice routing hook is validated for this build
    bool true_ultrawide;          // dcp_pass_ortho_sites are verified in this build's executable
    std::span<const PackageLayer> layers; // base package first; the last layer is the finished game
    std::span<const DcpResolutionSite> resolution_sites;
    std::span<const TextLanguage> text_languages; // the first is the default
    bool resolution_by_signature = false;
    bool keep_base_languages = false; // an update's regional archive is laid over the base's, which keeps its language slots
    bool features_by_signature = false; // cheats, NPC voices and True Ultrawide are found by structural locators, not tables
    bool exit_game = true;            // the title menu's Exit Game row is installed from structural locators
    bool unlock_gallery = true;       // the Gallery list builder is found by signature (src/gallery_unlock.h)
};

// The supported builds, US first.
std::span<const GameBuild> SupportedBuilds();

// The finished game's layer: its param.sfo is the one of the installed game.
const PackageLayer& FinishedLayer(const GameBuild& build);

// The finished package's APP_VER and VERSION, formatted for display (01.00 becomes 1.00).
std::string BuildMetadataText(const GameBuild& build);

// The build whose base package a build's update is laid over, or nullptr when the build has no update.
const GameBuild* BaseBuildOf(const GameBuild& build, std::span<const GameBuild> builds = SupportedBuilds());

// True when the build's executable has no verified render resolution sites: it then runs only at its native 16:9 size.
// The launcher hides and the loader refuses the custom Render resolution and Aspect Ratio for such a build; the unknown
// build (nullptr) is not restricted here, and the loader refuses to patch it anyway.
bool RestrictedToNativeResolution(const GameBuild* build);

// The build's languages; the Western ones when the build is not known.
std::span<const TextLanguage> TextLanguagesOf(const GameBuild* build);

// True when the build offers the Japanese NPC Voices toggle.
inline bool SupportsJapaneseNpcVoices(const GameBuild* build) {
    return build != nullptr && build->japanese_npc_voices;
}

// The cue mapper picks the Japanese NPC voices for system language 0, the only language a Japanese build runs with.
inline bool NativeJapaneseNpcVoices(const GameBuild* build) {
    if (build == nullptr) {
        return false;
    }
    for (const TextLanguage& language : build->text_languages) {
        if (language.ps4_id != 0) {
            return false;
        }
    }
    return !build->text_languages.empty();
}

// False only for a known build without the cheat hooks; the unknown build is not restricted here, and the loader
// installs no hook in it anyway.
inline bool CheatsAvailable(const GameBuild* build) {
    return build == nullptr || build->cheats;
}

// False only for a known build without a validated Gallery list builder; the unknown build is not restricted here, and
// the loader installs no hook in it anyway.
inline bool UnlockGalleryAvailable(const GameBuild* build) {
    return build == nullptr || build->unlock_gallery;
}

// False only for a known build whose pass projection sites are not verified; the unknown build is not restricted here,
// and the loader refuses to patch it anyway.
inline bool TrueUltrawideAvailable(const GameBuild* build) {
    return build == nullptr || build->true_ultrawide;
}

// The language of a saved key among `languages` (not empty); the first when the key is not one of them.
const TextLanguage& ResolveTextLanguage(std::span<const TextLanguage> languages, std::string_view key);
// The build's language for a saved key; the build's default when the key is not one of its languages.
const TextLanguage& ResolveTextLanguage(const GameBuild* build, std::string_view key);

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
