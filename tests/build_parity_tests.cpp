// The supported-build parity gate (tools/test.ps1), without the game or any window.
//
// Every supported build must offer every GLOBAL feature of CUSA10454 1.00, and the launcher, the runtime and the contract
// below must agree on it. The test also fails when a build has no row, when an update loses anything its base offers,
// when a mapping is partial, ambiguous or overlapping, or when an unknown executable could reach a supported build's
// mapping. Local boot images and prepared folders under build/image and game/prepared are read only; a build without
// one is proven by synthetic images and reported as such.
//
// Stdout ends with PARITY-BUILD and PARITY-FEATURE lines that tools/test.ps1 turns into its summary.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "cheat_hooks.h"
#include "controller_prompts.h"
#include "cpk_patch.h"
#include "dcp_launcher.h"
#include "dcp_resolution.h"
#include "exit_game_hook.h"
#include "exit_game_text.h"
#include "feature_sites.h"
#include "game_files.h"
#include "game_patches.h"
#include "game_profile.h"
#include "lzcnt_compat.h"
#include "npc_voice_resources.h"
#include "net/online_version.h"
#include "runtime_cheats.h"
#include "skip_offers.h"
#include "startup_save_hook.h"
#include "system_type_hook.h"
#include "text_languages.h"
#include "ultrawide_hooks.h"
#include "voice_hooks.h"

namespace fs = std::filesystem;
using namespace dcp;

namespace {

int g_checks = 0, g_failures = 0;
std::string g_scope = "contract";
std::map<std::string, int> g_scope_failures;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(cond)) {                                                                \
            std::printf("FAIL [%s] line %d: %s\n", g_scope.c_str(), __LINE__, #cond); \
            ++g_failures;                                                             \
            ++g_scope_failures[g_scope];                                              \
        }                                                                             \
    } while (0)

enum class Mapping { kNone, kTable, kSignature };

std::string Key(const GameBuild& build) {
    return std::string(build.title_id) + "-" + build.version;
}

std::string ReadText(const char* relative) {
    std::ifstream in(fs::path(DCP_SOURCE_DIR) / relative, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

template <typename Records>
bool Listed(const Records& records, const GameBuild& build) {
    return std::any_of(std::begin(records), std::end(records),
                       [&](const auto& record) { return std::string_view(record.build) == build.label; });
}

const GuestImageId* ImageOf(const GameBuild& build) {
    for (const LzcntBuild& record : lzcnt_builds) {
        if (std::string_view(record.build) == build.label) {
            return &record.image;
        }
    }
    return nullptr;
}

bool HasSkipTable(const GameBuild& build) {
    uint64_t story = 0, tutorial = 0;
    const unsigned char none[1] = {0};
    return skip_offers_locate(none, 0, Key(build).c_str(), &story, &tutorial) == 1;
}

bool IsUs100(const GameBuild& build) {
    const GuestImageId* id = ImageOf(build);
    return id && id->size == cheats::kSupportedImage.size && id->entry == cheats::kSupportedImage.entry;
}

// What the profile and the runtime tables provide, feature by feature.
struct Capabilities {
    bool cheats = false, xbox_prompts = false, npc_voices = false, true_ultrawide = false, custom_resolution = false;
    Mapping resolution = Mapping::kNone, skip_offers = Mapping::kNone, system_type = Mapping::kNone,
            lzcnt = Mapping::kNone, startup_save = Mapping::kNone;
    std::string languages;
};

Capabilities CapabilitiesOf(const GameBuild& build) {
    Capabilities caps;
    caps.cheats = build.cheats;
    caps.xbox_prompts = build.xbox_prompts;
    caps.npc_voices = SupportsJapaneseNpcVoices(&build);
    caps.true_ultrawide = TrueUltrawideAvailable(&build);
    caps.custom_resolution = !RestrictedToNativeResolution(&build);
    caps.resolution = !build.resolution_sites.empty() ? Mapping::kTable
                      : build.resolution_by_signature ? Mapping::kSignature
                                                      : Mapping::kNone;
    const auto table_or_search = [&](bool listed) { return listed ? Mapping::kTable : Mapping::kSignature; };
    caps.skip_offers = table_or_search(HasSkipTable(build));
    caps.system_type = table_or_search(Listed(system_type_records, build));
    caps.lzcnt = table_or_search(Listed(lzcnt_builds, build));
    caps.startup_save = Listed(startup_save_records, build) ? Mapping::kTable : Mapping::kNone;
    for (const TextLanguage& language : TextLanguagesOf(&build)) {
        caps.languages += (caps.languages.empty() ? "" : " ") + std::string(language.key);
    }
    return caps;
}

// The PC-port features CUSA10454 1.00 defines. GLOBAL: every supported build must offer it. Japanese NPC Voices is
// REGION-SPECIFIC: a toggle where the game speaks English to NPCs, NOT APPLICABLE where it already speaks Japanese.
enum Feature : size_t {
    kCheats,
    kOnlinePlay,
    kTrueUltrawide,
    kRenderResolution,
    kAspectRatio,
    kStorySkip,
    kTutorialSkip,
    kLzcnt,
    kArachnophobia,
    kController,
    kKeyboard,
    kGraphics,
    kSaveLoad,
    kXboxPrompts,
    kJapaneseNpcVoices,
    kExitGame,
    kFeatureCount
};

const char* const kFeatureNames[kFeatureCount] = {
    "Cheats",     "Online Play", "True Ultrawide", "Render resolution",       "Aspect Ratio",
    "Story Skip", "Tutorial Skip", "LZCNT compatibility", "Arachnophobia Safe Mode", "Controller",
    "Keyboard",   "Graphics",    "Save/load",      "Xbox prompts",            "Japanese NPC Voices", "Exit Game"};

// One row per supported build: only what may differ by region. Every other feature is GLOBAL and has no column.
struct Contract {
    const char* label;
    bool native_japanese_voices; // Japanese NPC Voices NOT APPLICABLE: the build already speaks Japanese
    Mapping startup_save;        // the Japanese builds' startup save step, part of their save handling
    const char* languages;       // the regional base's languages; an update keeps all of them
};

constexpr Mapping T = Mapping::kTable, N = Mapping::kNone;
const char* const kWestern = "english french german italian spanish";
const char* const kAsia = "traditional_chinese korean";

const Contract kContracts[] = {
    {"CUSA10454 1.00 USA", false, N, kWestern},
    {"CUSA10454 1.01 USA", false, N, kWestern},
    {"CUSA07001 1.00 Japan", true, T, "japanese"},
    {"CUSA07001 1.02 Japan", true, T, "japanese"},
    {"CUSA07001 1.03 Japan", true, T, "japanese"},
    {"CUSA10209 1.00 Asia/Hong Kong", false, N, kAsia},
    {"CUSA10209 1.02 Asia/Hong Kong", false, N, kAsia},
};

// Each update's chain: the base it must match, then the siblings that must match each other.
struct Chain {
    const char* update;
    const char* base;
};
const Chain kChains[] = {
    {"CUSA10454 1.01 USA", "CUSA10454 1.00 USA"},
    {"CUSA07001 1.02 Japan", "CUSA07001 1.00 Japan"},
    {"CUSA07001 1.03 Japan", "CUSA07001 1.00 Japan"},
    {"CUSA10209 1.02 Asia/Hong Kong", "CUSA10209 1.00 Asia/Hong Kong"},
};

const GameBuild* FindBuild(std::string_view label) {
    for (const GameBuild& build : SupportedBuilds()) {
        if (label == build.label) {
            return &build;
        }
    }
    return nullptr;
}

const Contract* FindContract(std::string_view label) {
    for (const Contract& row : kContracts) {
        if (label == row.label) {
            return &row;
        }
    }
    return nullptr;
}

// What the loader reads for a build, filled as dcp_game_build fills it.
DcpGameBuild LoaderView(const GameBuild& build) {
    DcpGameBuild game{};
    game.label = build.label;
    game.cheats = build.cheats;
    game.japanese_npc_voices = build.japanese_npc_voices;
    game.true_ultrawide = build.true_ultrawide;
    game.resolution_sites = build.resolution_sites.data();
    game.resolution_site_count = build.resolution_sites.size();
    game.resolution_by_signature = build.resolution_by_signature;
    game.features_by_signature = build.features_by_signature;
    game.exit_game = build.exit_game;
    game.title_id = build.title_id;
    game.version = build.version;
    return game;
}

// The loader's own view, from an image.json naming the build.
bool LoaderBuild(const GameBuild& build, DcpGameBuild& game) {
    const fs::path dir = fs::temp_directory_path() / "dcp_build_parity_gate";
    fs::create_directories(dir);
    std::ofstream(dir / "image.json") << "{\n  \"title_id\": \"" << build.title_id << "\",\n  \"eboot_sha256\": \""
                                      << build.eboot_sha256 << "\",\n  \"app_ver\": \"" << FinishedLayer(build).app_ver
                                      << "\"\n}";
    game = {};
    const bool found = dcp_game_build(dir.string().c_str(), &game) == 1;
    fs::remove_all(dir);
    return found;
}

bool SameView(const DcpGameBuild& a, const DcpGameBuild& b) {
    return std::string_view(a.label) == b.label && a.cheats == b.cheats && a.japanese_npc_voices == b.japanese_npc_voices &&
           a.true_ultrawide == b.true_ultrawide && a.resolution_sites == b.resolution_sites &&
           a.resolution_site_count == b.resolution_site_count && a.resolution_by_signature == b.resolution_by_signature &&
           a.features_by_signature == b.features_by_signature && a.exit_game == b.exit_game &&
           std::string_view(a.title_id) == b.title_id &&
           std::string_view(a.version) == b.version;
}

// What the launcher shows for the build, through the helpers launcher.cpp gates with. A feature without a helper has no
// build gate in the launcher, which TestGateConsistency enforces.
bool LauncherOffers(const GameBuild& build, Feature feature) {
    switch (feature) {
    case kCheats:
        return CheatsAvailable(&build);
    case kTrueUltrawide:
        return TrueUltrawideAvailable(&build);
    case kRenderResolution:
    case kAspectRatio:
        return !RestrictedToNativeResolution(&build);
    case kXboxPrompts:
        return build.xbox_prompts;
    case kJapaneseNpcVoices:
        return SupportsJapaneseNpcVoices(&build);
    default:
        return true;
    }
}

// Whether the loader has a way to install the feature for the build: a table for this exact image or a locator its
// profile asks for. The real images prove the locators in CheckFixtures, synthetic images in dcp-feature-sites-tests.
bool RuntimeImplements(const GameBuild& build, const DcpGameBuild& game, const Contract& row, Feature feature) {
    const bool known = game.title_id && *game.title_id && game.version && *game.version;
    switch (feature) {
    case kCheats:
        return known && runtime_cheats_hooks_wanted(game.cheats) && (IsUs100(build) || game.features_by_signature);
    case kOnlinePlay:
        return known && CapabilitiesOf(build).system_type != Mapping::kNone;
    case kTrueUltrawide:
        return known && game.true_ultrawide && (Listed(ultrawide_records, build) || game.features_by_signature);
    case kRenderResolution:
    case kAspectRatio:
        return known && (game.resolution_site_count == DCP_RENDER_SIZE_SIGNATURE_COUNT || game.resolution_by_signature) &&
               !(game.resolution_site_count && game.resolution_by_signature);
    case kStorySkip:
    case kTutorialSkip:
        return known && CapabilitiesOf(build).skip_offers != Mapping::kNone;
    case kLzcnt:
        return known && CapabilitiesOf(build).lzcnt != Mapping::kNone;
    case kArachnophobia:
        return std::any_of(std::begin(kSharedGameFiles), std::end(kSharedGameFiles),
                           [](const char* file) { return std::string_view(file) == "DRACRO.CPK"; });
    case kSaveLoad:
        return known && CapabilitiesOf(build).startup_save == row.startup_save;
    case kXboxPrompts:
        return build.xbox_prompts;
    case kJapaneseNpcVoices:
        return known && game.japanese_npc_voices && (IsUs100(build) || game.features_by_signature);
    case kExitGame:
        return known && game.exit_game;
    default:
        return known;
    }
}

// A GLOBAL feature passes only when the launcher offers it and the runtime implements it; Japanese NPC Voices passes as
// NOT APPLICABLE only on a build the contract says already speaks Japanese, which then offers no toggle.
bool FeaturePasses(const GameBuild& build, const DcpGameBuild& game, const Contract& row, Feature feature,
                   std::string* why) {
    const bool offered = LauncherOffers(build, feature);
    const bool implemented = RuntimeImplements(build, game, row, feature);
    const bool native = feature == kJapaneseNpcVoices && row.native_japanese_voices;
    const bool pass = native ? !offered && !implemented && NativeJapaneseNpcVoices(&build) : offered && implemented;
    if (!pass && why) {
        *why = std::string(native ? "expected native, " : "expected on, ") + "launcher " + (offered ? "on" : "off") +
               ", runtime " + (implemented ? "on" : "off");
    }
    return pass;
}

struct FeatureTally {
    size_t passed = 0, native = 0;
    std::string failures;
};
FeatureTally g_tally[kFeatureCount];

void TestContract() {
    g_scope = "contract";
    CHECK(SupportedBuilds().size() == std::size(kContracts));
    for (const GameBuild& build : SupportedBuilds()) {
        const Contract* row = FindContract(build.label);
        CHECK(row != nullptr);
        if (!row) {
            std::printf("  %s has no parity contract row\n", build.label);
            continue;
        }
        g_scope = build.label;
        const Capabilities caps = CapabilitiesOf(build);
        CHECK(caps.startup_save == row->startup_save);
        CHECK(caps.languages == row->languages);
        CHECK(NativeJapaneseNpcVoices(&build) == row->native_japanese_voices);
        g_scope = "contract";
    }
    for (const Contract& row : kContracts) {
        CHECK(FindBuild(row.label) != nullptr);
    }
}

// Every build against the reference feature set: launcher, runtime and contract must agree for each feature.
void TestGlobalFeatures() {
    runtime_cheats_set_online(0);
    for (const GameBuild& build : SupportedBuilds()) {
        g_scope = build.label;
        const Contract* row = FindContract(build.label);
        DcpGameBuild game{};
        CHECK(row && LoaderBuild(build, game) && SameView(game, LoaderView(build)));
        if (!row || !game.label) {
            continue;
        }
        for (size_t f = 0; f < kFeatureCount; ++f) {
            std::string why;
            const bool pass = FeaturePasses(build, game, *row, Feature(f), &why);
            CHECK(pass);
            if (pass) {
                ++g_tally[f].passed;
                g_tally[f].native += f == kJapaneseNpcVoices && row->native_japanese_voices;
            } else {
                std::printf("  %s: %s %s\n", build.label, kFeatureNames[f], why.c_str());
                g_tally[f].failures += std::string(g_tally[f].failures.empty() ? "" : ", ") + build.label;
            }
        }
    }
}

// Every selectable language of every build has Exit Game text, the table holds no language no build can select, and a
// build that offers Exit Game has the languages its contract row lists.
void TestExitGameLanguages() {
    std::set<std::string> used;
    for (const GameBuild& build : SupportedBuilds()) {
        g_scope = build.label;
        const Contract* row = FindContract(build.label);
        CHECK(build.exit_game && row);
        std::string joined;
        for (const TextLanguage& language : TextLanguagesOf(&build)) {
            const ExitGameLanguage* text = nullptr;
            for (const ExitGameLanguage& candidate : exit_game_languages) {
                if (language.key == candidate.key) text = &candidate;
            }
            CHECK(text && text->row && *text->row && text->help && *text->help);
            used.insert(std::string(language.key));
            joined += (joined.empty() ? "" : " ") + std::string(language.key);
        }
        CHECK(row && joined == row->languages);
    }
    g_scope = "contract";
    CHECK(used.size() == EXIT_GAME_LANGUAGE_COUNT);
}

// The case an earlier contract let through: "CUSA10209 1.00 Asia/Hong Kong PASS real" while the launcher said "Cheats
// are not available for CUSA10209 1.00 Asia/Hong Kong". A supported build with any GLOBAL feature switched off fails.
void TestDisabledFeatureCannotPass() {
    g_scope = "contract";
    for (const GameBuild& build : SupportedBuilds()) {
        const Contract* row = FindContract(build.label);
        if (!row) {
            continue;
        }
        const auto refuses = [&](GameBuild copy, Feature feature) {
            return !FeaturePasses(copy, LoaderView(copy), *row, feature, nullptr);
        };
        GameBuild copy = build;
        copy.cheats = false;
        CHECK(refuses(copy, kCheats));
        copy = build;
        copy.true_ultrawide = false;
        CHECK(refuses(copy, kTrueUltrawide));
        copy = build;
        copy.xbox_prompts = false;
        CHECK(refuses(copy, kXboxPrompts));
        copy = build;
        copy.resolution_sites = {};
        copy.resolution_by_signature = false;
        CHECK(refuses(copy, kRenderResolution) && refuses(copy, kAspectRatio));
        copy = build;
        copy.features_by_signature = false;
        CHECK(IsUs100(build) || refuses(copy, kCheats));
        copy = build;
        copy.japanese_npc_voices = !build.japanese_npc_voices;
        CHECK(refuses(copy, kJapaneseNpcVoices));
        copy = build;
        copy.exit_game = false;
        CHECK(refuses(copy, kExitGame));
        CHECK(CheatsAvailable(&build));
    }
    const GameBuild* hk = FindBuild("CUSA10209 1.00 Asia/Hong Kong");
    CHECK(hk && hk->cheats && CheatsAvailable(hk));

    const std::string launcher = ReadText("launcher/launcher.cpp");
    const std::string message = "Cheats are not available for ";
    const size_t at = launcher.find(message);
    CHECK(at != std::string::npos && launcher.find(message, at + 1) == std::string::npos);
    const size_t gate = launcher.rfind("const bool blocked = !CheatsAvailable(build_);", at);
    CHECK(gate != std::string::npos && at - gate < 200 && launcher.find("if (blocked)", gate) < at);
}

// An update offers everything its base offers, and the same languages.
void TestUpdateParity() {
    g_scope = "contract";
    for (const Chain& chain : kChains) {
        const GameBuild* update = FindBuild(chain.update);
        const GameBuild* base = FindBuild(chain.base);
        CHECK(update && base && BaseBuildOf(*update) == base);
        if (!update || !base) {
            continue;
        }
        g_scope = update->label;
        CHECK(std::string_view(base->title_id) == update->title_id && base->layers.size() == 1);
        const Capabilities b = CapabilitiesOf(*base), u = CapabilitiesOf(*update);
        const auto lost = [&](const char* feature, bool in_base, bool in_update) {
            if (in_base && !in_update) {
                std::printf("  %s lost %s of %s\n", update->label, feature, base->label);
            }
            CHECK(!in_base || in_update);
        };
        lost("cheats", b.cheats, u.cheats);
        lost("xbox prompts", b.xbox_prompts, u.xbox_prompts);
        lost("japanese npc voices", b.npc_voices, u.npc_voices);
        lost("true ultrawide", b.true_ultrawide, u.true_ultrawide);
        lost("render resolution and aspect ratio", b.custom_resolution, u.custom_resolution);
        lost("skip offers", b.skip_offers != Mapping::kNone, u.skip_offers != Mapping::kNone);
        lost("system type", b.system_type != Mapping::kNone, u.system_type != Mapping::kNone);
        lost("lzcnt rewrite", b.lzcnt != Mapping::kNone, u.lzcnt != Mapping::kNone);
        lost("startup save", b.startup_save != Mapping::kNone, u.startup_save != Mapping::kNone);
        CHECK(u.languages == b.languages);
        CHECK(update->features_by_signature && base->features_by_signature == !IsUs100(*base));
        CHECK(update->cheats == base->cheats && update->japanese_npc_voices == base->japanese_npc_voices &&
              update->true_ultrawide == base->true_ultrawide);
    }
    g_scope = "contract";
    // 1.02 and 1.03 are independent builds with the same contract.
    const GameBuild* jp102 = FindBuild("CUSA07001 1.02 Japan");
    const GameBuild* jp103 = FindBuild("CUSA07001 1.03 Japan");
    CHECK(jp102 && jp103 && std::string_view(jp102->eboot_sha256) != jp103->eboot_sha256);
    if (jp102 && jp103) {
        const Capabilities a = CapabilitiesOf(*jp102), c = CapabilitiesOf(*jp103);
        CHECK(a.cheats == c.cheats && a.true_ultrawide == c.true_ultrawide && a.resolution == c.resolution &&
              a.startup_save == c.startup_save && a.languages == c.languages);
        CHECK(jp102->resolution_sites.data() != jp103->resolution_sites.data());
    }
}

// Launcher and runtime must agree in both directions: nothing is offered without its mapping, and nothing mapped is hidden.
void TestGateConsistency() {
    for (const GameBuild& build : SupportedBuilds()) {
        g_scope = build.label;
        const Capabilities caps = CapabilitiesOf(build);
        const GuestImageId* image = ImageOf(build);
        CHECK(caps.custom_resolution == (caps.resolution != Mapping::kNone));
        // US 1.00 has the static cheat and voice tables; every other build finds its sites by signature.
        CHECK(build.features_by_signature == !IsUs100(build));
        CHECK(caps.true_ultrawide == (Listed(ultrawide_records, build) || build.features_by_signature));
        CHECK(!Listed(ultrawide_records, build) || caps.true_ultrawide);
        CHECK(caps.cheats && (IsUs100(build) || build.features_by_signature));
        CHECK(caps.npc_voices == !NativeJapaneseNpcVoices(&build));
        CHECK(caps.startup_save == (std::string_view(build.title_id) == "CUSA07001" ? Mapping::kTable : Mapping::kNone));
        CHECK(caps.xbox_prompts);
        CHECK(!build.keep_base_languages || BaseBuildOf(build) != nullptr);
        CHECK(!Key(build).empty());
        for (const SystemTypeRecord& record : system_type_records) {
            CHECK(std::string_view(record.build) != build.label || (image && record.image.size == image->size && record.image.entry == image->entry));
        }
        for (const UltrawideRecord& record : ultrawide_records) {
            CHECK(std::string_view(record.build) != build.label || (image && record.image.size == image->size && record.image.entry == image->entry));
        }
        for (const StartupSaveRecord& record : startup_save_records) {
            CHECK(std::string_view(record.build) != build.label || (image && record.image.size == image->size && record.image.entry == image->entry));
        }
    }
    g_scope = "contract";
    std::set<std::string> keys;
    for (const GameBuild& build : SupportedBuilds()) {
        CHECK(keys.insert(Key(build)).second);
    }
    for (size_t i = 0; i < std::size(lzcnt_builds); ++i) {
        for (size_t j = i + 1; j < std::size(lzcnt_builds); ++j) {
            CHECK(lzcnt_builds[i].image.size != lzcnt_builds[j].image.size ||
                  lzcnt_builds[i].image.entry != lzcnt_builds[j].image.entry);
        }
    }

    // The launcher gates a feature by build only through these helpers; any other use of the active build is a new gate
    // the contract does not know.
    const std::string launcher = ReadText("launcher/launcher.cpp");
    const char* const allowed[] = {"RestrictedToNativeResolution(build_)", "TrueUltrawideAvailable(build_)",
                                   "JapaneseNpcVoicesOffered(paths_.app0, build_)", "CheatsAvailable(build_)",
                                   "xbox_available_",                      "XboxPromptsAvailable",
                                   "build_->xbox_prompts",                 "InstalledTextLanguages(paths_.app0, build_)",
                                   "InstalledPromptStyle(paths_.app0, *build_", "PromptArchives(paths_.app0, *build_",
                                   "build_->label",                        "BuildMetadataText(*build_)",
                                   "InstalledGameName(files_, build_)",    "build_ = check.build",
                                   "const GameBuild* build_",              "|| !build_)",
                                   "build_(check.build)",                  "build = *build_"};
    std::map<std::string, int> seen;
    size_t line_start = 0, unknown = 0;
    const std::regex word("\\bbuild_\\b");
    while (line_start < launcher.size()) {
        size_t line_end = launcher.find('\n', line_start);
        if (line_end == std::string::npos) line_end = launcher.size();
        const std::string line = launcher.substr(line_start, line_end - line_start);
        line_start = line_end + 1;
        if (!std::regex_search(line, word)) continue;
        bool known = false;
        for (const char* pattern : allowed) {
            if (line.find(pattern) != std::string::npos) {
                known = true;
                ++seen[pattern];
            }
        }
        if (!known) {
            ++unknown;
            std::printf("  launcher.cpp gates on the active build outside the contract: %s\n", line.c_str());
        }
    }
    CHECK(unknown == 0);
    for (const char* required : {"RestrictedToNativeResolution(build_)", "TrueUltrawideAvailable(build_)",
                                 "JapaneseNpcVoicesOffered(paths_.app0, build_)", "CheatsAvailable(build_)",
                                 "xbox_available_"}) {
        CHECK(seen[required] > 0);
    }
}

// Table or signature, never empty, partial, copied from another build or shared.
void TestResolutionStrategy() {
    for (const GameBuild& build : SupportedBuilds()) {
        g_scope = build.label;
        const Capabilities caps = CapabilitiesOf(build);
        if (caps.resolution == Mapping::kTable) {
            CHECK(build.resolution_sites.size() == DCP_RENDER_SIZE_SIGNATURE_COUNT && !build.resolution_by_signature);
            std::set<uint32_t> offsets;
            for (const DcpResolutionSite& site : build.resolution_sites) {
                offsets.insert(site.offset);
                CHECK(site.opcode_length == 1 || site.opcode_length == 2);
            }
            CHECK(offsets.size() == build.resolution_sites.size());
            std::vector<DcpResolutionSite> ordered(build.resolution_sites.begin(), build.resolution_sites.end());
            std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
            for (size_t i = 1; i < ordered.size(); ++i) {
                CHECK(ordered[i].offset >= ordered[i - 1].offset + ordered[i - 1].opcode_length + 4u);
            }
            for (const GameBuild& other : SupportedBuilds()) {
                if (&other == &build || other.resolution_sites.empty()) continue;
                std::set<uint32_t> theirs;
                for (const DcpResolutionSite& site : other.resolution_sites) theirs.insert(site.offset);
                CHECK(theirs != offsets);
            }
        } else if (caps.resolution == Mapping::kSignature) {
            CHECK(build.resolution_sites.empty() && build.resolution_by_signature);
            CHECK(DCP_RENDER_SIZE_SIGNATURE_COUNT == 23);
        }
        CHECK(caps.true_ultrawide == (Listed(ultrawide_records, build) || build.features_by_signature));
    }
}

void PlantSite(std::vector<unsigned char>& image, uint64_t at, const DcpResolutionSignature& signature) {
    std::memcpy(&image[at + signature.lead], signature.opcode, signature.opcode_length);
    std::memcpy(&image[at + signature.lead + signature.opcode_length], &signature.value, sizeof(signature.value));
}

void PlantSignature(std::vector<unsigned char>& image, uint64_t at, const DcpResolutionSignature& signature) {
    for (unsigned k = 0; k < signature.length; ++k) {
        if (signature.compare[k]) image[at + k] = signature.pattern[k];
    }
    PlantSite(image, at, signature);
}

int MatchesIn(const std::vector<unsigned char>& image, const DcpResolutionSignature& signature) {
    DcpResolutionSite site;
    DcpResolutionSignatureResult result{};
    dcp_find_resolution_sites(image.data(), image.size(), &signature, 1, &site, &result);
    return result.matches;
}

// Some signatures are windows of others (one code region), so, as in a real image, the longest are planted first and a
// signature that already matches once is not planted again.
std::vector<unsigned char> SyntheticRenderImage() {
    std::vector<unsigned char> image(0x20000, 0);
    std::vector<size_t> order(DCP_RENDER_SIZE_SIGNATURE_COUNT);
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [](size_t a, size_t b) {
        return dcp_render_size_signatures[a].length > dcp_render_size_signatures[b].length;
    });
    uint64_t at = 0x1000;
    for (const size_t i : order) {
        if (MatchesIn(image, dcp_render_size_signatures[i]) == 1) continue;
        PlantSignature(image, at, dcp_render_size_signatures[i]);
        at += 0x400;
    }
    return image;
}

void TestSyntheticRenderLocator() {
    g_scope = "synthetic";
    std::vector<unsigned char> image = SyntheticRenderImage();
    DcpResolutionSite sites[DCP_RENDER_SIZE_SIGNATURE_COUNT];
    DcpResolutionSignatureResult results[DCP_RENDER_SIZE_SIGNATURE_COUNT];
    const auto find = [&](const std::vector<unsigned char>& bytes) {
        return dcp_find_resolution_sites(bytes.data(), bytes.size(), dcp_render_size_signatures,
                                         DCP_RENDER_SIZE_SIGNATURE_COUNT, sites, results);
    };
    CHECK(find(image) == 1);
    std::vector<DcpResolutionSignatureResult> good(results, results + DCP_RENDER_SIZE_SIGNATURE_COUNT);
    for (const DcpResolutionSignatureResult& result : good) {
        CHECK(result.matches == 1 && result.valid);
    }

    // A site missing, ambiguous or changed makes the whole set refuse.
    for (size_t i = 0; i < DCP_RENDER_SIZE_SIGNATURE_COUNT; i += 5) {
        const DcpResolutionSignature& signature = dcp_render_size_signatures[i];
        const uint64_t start = good[i].offset - signature.lead;
        std::vector<unsigned char> broken = image;
        const size_t first = size_t(std::find(signature.compare, signature.compare + signature.length, 1) - signature.compare);
        broken[start + first] ^= 0xff;
        CHECK(find(broken) == 0 && results[i].matches == 0);

        broken = image;
        PlantSignature(broken, 0x1f000, signature);
        CHECK(find(broken) == 0 && results[i].matches == 2);

        broken = image;
        broken[good[i].offset + signature.opcode_length] ^= 0x01;
        CHECK(find(broken) == 0 && !results[i].valid);
    }
    CHECK(find(std::vector<unsigned char>(0x400, 0)) == 0);
    CHECK(dcp_find_resolution_sites(image.data(), image.size(), dcp_render_size_signatures, 0, sites, results) == 0);
}

size_t CountLzcnt(const uint8_t* image, uint64_t size) {
    size_t count = 0;
    for (uint64_t i = 0; i + 3 < size; ++i) {
        if (image[i] != 0xf3) continue;
        const uint64_t at = (image[i + 1] & 0xf0) == 0x40 ? i + 2 : i + 1;
        count += at + 1 < size && image[at] == 0x0f && image[at + 1] == 0xbd;
    }
    return count;
}

// Synthetic LZCNT image: the shapes at spread offsets stand for a build without a table.
void TestSyntheticLzcnt() {
    g_scope = "synthetic";
    const size_t size = 0x10000, stubs = lzcnt_compat_stub_bytes();
    std::vector<uint8_t> memory(size + stubs, 0);
    uint32_t planted[LZCNT_SITE_COUNT];
    for (size_t i = 0; i < LZCNT_SITE_COUNT; ++i) {
        planted[i] = uint32_t(0x400 + i * 0x80);
        std::memcpy(memory.data() + planted[i], lzcnt_shapes[i].bytes, lzcnt_shapes[i].length);
    }
    uint32_t located[LZCNT_SITE_COUNT];
    CHECK(lzcnt_compat_locate(memory.data(), size, located) == LZCNT_COMPAT_OK);
    CHECK(std::equal(std::begin(located), std::end(located), std::begin(planted)));
    CHECK(lzcnt_compat_apply(memory.data(), size, located, lzcnt_shapes, LZCNT_SITE_COUNT, memory.data() + size, stubs) ==
          LZCNT_COMPAT_OK);
    CHECK(CountLzcnt(memory.data(), size) == 0);

    std::vector<uint8_t> shifted(size + stubs, 0);
    for (size_t i = 0; i < LZCNT_SITE_COUNT; ++i)
        std::memcpy(shifted.data() + planted[i], lzcnt_shapes[i].bytes, lzcnt_shapes[i].length);
    const uint8_t extra[] = {0xf3, 0x48, 0x0f, 0xbd, 0xc0};
    std::memcpy(shifted.data() + 0x8000, extra, sizeof(extra));
    CHECK(lzcnt_compat_locate(shifted.data(), size, located) == LZCNT_COMPAT_UNKNOWN_IMAGE);
    std::memset(shifted.data() + 0x8000, 0, sizeof(extra));
    shifted[planted[0] + lzcnt_shapes[0].length - 1] ^= 0x01;
    CHECK(lzcnt_compat_locate(shifted.data(), size, located) == LZCNT_COMPAT_UNKNOWN_IMAGE);
}

void TestOnline() {
    g_scope = "contract";
    CHECK(net::kOnlineProtocolVersion >= 1 && net::online_compatible(net::kOnlineProtocolVersion));
    CHECK(!net::online_compatible(0) && std::string_view(net::kOnlineGame) == "DragonsCrownPro");
    for (const GameBuild& build : SupportedBuilds()) {
        g_scope = build.label;
        // No per-build allowlist: Online Play needs a build key and the System type hook's mapping, nothing else.
        CHECK(!Key(build).empty());
        CHECK(CapabilitiesOf(build).system_type != Mapping::kNone);

        runtime_cheats_set_online(1);
        runtime_cheats_start(~0u, 10, 4);
        CHECK(runtime_cheats_online_locked() == 1);
        CHECK(runtime_cheats_log_state(build.cheats) == RUNTIME_CHEATS_ONLINE_LOCKED);
        CHECK(runtime_cheats_hooks_wanted(build.cheats) == 0);
        runtime_cheats_set_online(0);
        runtime_cheats_start(~0u, 10, 4);
        CHECK(runtime_cheats_online_locked() == 0);
        CHECK(runtime_cheats_log_state(build.cheats) == (build.cheats ? RUNTIME_CHEATS_LISTED : RUNTIME_CHEATS_UNAVAILABLE));
        CHECK((runtime_cheats_hooks_wanted(build.cheats) != 0) == build.cheats);
    }
    runtime_cheats_set_online(0);
}

// The loader reaches every mapping only through a supported build; an unknown executable is diagnosed and never patched.
void TestUnknownBuilds() {
    g_scope = "unknown";
    const std::set<std::string> fixture_labels = {"CUSA10454 1.00 USA", "CUSA07001 1.00 Japan", "CUSA07001 1.02 Japan",
                                                  "CUSA07001 1.03 Japan", "CUSA10209 1.00 Asia/Hong Kong"};
    const auto labels = [](const auto& records) {
        std::set<std::string> out;
        for (const auto& record : records) out.insert(record.build);
        return out;
    };
    CHECK(labels(lzcnt_builds) == fixture_labels);
    CHECK(labels(system_type_records) == fixture_labels);
    CHECK(labels(ultrawide_records) == fixture_labels);
    for (const StartupSaveRecord& record : startup_save_records) {
        CHECK(std::string_view(record.build).starts_with("CUSA07001 "));
    }

    std::vector<uint8_t> image(0x20000, 0x90);
    const std::vector<uint8_t> before = image;
    const uint64_t size = image.size(), entry = 0x1000;
    std::vector<uint8_t> stubs(1 << 16);
    CHECK(skip_offers_install(image.data(), size, nullptr, SKIP_OFFER_STORY | SKIP_OFFER_TUTORIAL) == 0);
    CHECK(!startup_save_hook_install(image.data(), size, entry, stubs.data(), stubs.size()));
    CHECK(!voice_hooks_install(image.data(), size, entry, stubs.data(), stubs.size()));
    char detail[200];
    CHECK(!ultrawide_hooks_install(image.data(), size, entry, stubs.data(), stubs.size(), 1.3125, detail, sizeof(detail)));
    CHECK(lzcnt_compat_install(image.data(), size, entry, stubs.data(), stubs.size(), nullptr) == LZCNT_COMPAT_UNKNOWN_IMAGE);
    uint32_t offsets[LZCNT_SITE_COUNT];
    CHECK(lzcnt_compat_locate(image.data(), size, offsets) == LZCNT_COMPAT_UNKNOWN_IMAGE);
    SystemTypeRecord located;
    CHECK(!system_type_locate(image.data(), size, entry, &located));
    CHECK(guest_hooks_verify(image.data(), size, entry, &cheats::kSupportedImage, nullptr, 0, nullptr) == GUEST_HOOK_WRONG_IMAGE);
    const DcpExecRange whole{0, size};
    const DcpImageView unknown_view{image.data(), size, &whole, 1};
    DcpExitGameSites no_sites{};
    DcpLocateReport no_report{};
    char no_failure[200];
    CHECK(!dcp_locate_exit_game_sites(&unknown_view, &no_sites, &no_report) && no_report.failure[0]);
    CHECK(!dcp_resolve_exit_game(nullptr, &unknown_view, &no_sites, no_failure, sizeof(no_failure)));
    CHECK(!exit_game_hook_install_located(image.data(), size, entry, &no_sites, stubs.data(), stubs.size()));
    CHECK(image == before);

    // An image.json that names a supported hash with another version, title or hash is no supported build.
    const fs::path dir = fs::temp_directory_path() / "dcp_build_parity_unknown";
    for (const GameBuild& build : SupportedBuilds()) {
        const std::pair<const char*, const char*> mismatches[] = {
            {build.title_id, "01.99"},
            {"CUSA00000", FinishedLayer(build).app_ver},
            {build.title_id, std::string_view(FinishedLayer(build).app_ver) == "01.00" ? "01.01" : "01.00"}};
        for (const auto& [title, app_ver] : mismatches) {
            fs::create_directories(dir);
            std::ofstream(dir / "image.json") << "{\n  \"title_id\": \"" << title << "\",\n  \"eboot_sha256\": \""
                                              << build.eboot_sha256 << "\",\n  \"app_ver\": \"" << app_ver << "\"\n}";
            DcpGameBuild unknown{};
            CHECK(dcp_game_build(dir.string().c_str(), &unknown) == 0);
        }
        fs::remove_all(dir);
    }
    fs::create_directories(dir);
    std::ofstream(dir / "image.json") << "{\n  \"title_id\": \"CUSA10454\",\n  \"eboot_sha256\": \"" << std::string(64, '0')
                                      << "\",\n  \"app_ver\": \"01.02\"\n}";
    DcpGameBuild unknown{};
    CHECK(dcp_game_build(dir.string().c_str(), &unknown) == 0);
    fs::remove_all(dir);

    const std::string loader = ReadText("src/loader.c");
    for (const char* gate : {"if (!known_build) fail_running(\"render resolution and aspect ratio need a supported game build\");",
                             "status==LZCNT_COMPAT_UNKNOWN_IMAGE && known_build",
                             "if (known_build && runtime_online_active())",
                             "skip_offers_install(image,size,known_build ? build_key : NULL,",
                             "if (known_build) startup_save_hook_install(",
                             "if (known_build && game_build.japanese_npc_voices && voice_hooks_enabled() &&",
                             "dcp_resolve_features(known_build ? &game_build : NULL,",
                             "runtime_cheats_hooks_wanted(known_build && game_build.cheats)",
                             "known_build && game_build.true_ultrawide",
                             "dcp_resolve_exit_game(&game_build,&view,&exit_game_sites,",
                             "if (known_build && game_build.exit_game) {",
                             "exit_game_set_handler(dcpgpu_request_close);"}) {
        const bool present = loader.find(gate) != std::string::npos;
        if (!present) {
            std::printf("  loader gate missing: %s\n", gate);
        }
        CHECK(present);
    }
}

void TestLauncherAgreesWithLoader() {
    const fs::path dir = fs::temp_directory_path() / "dcp_build_parity_gate";
    for (const GameBuild& build : SupportedBuilds()) {
        g_scope = build.label;
        fs::create_directories(dir);
        std::ofstream(dir / "image.json") << "{\n  \"title_id\": \"" << build.title_id << "\",\n  \"eboot_sha256\": \""
                                          << build.eboot_sha256 << "\",\n  \"app_ver\": \""
                                          << FinishedLayer(build).app_ver << "\"\n}";
        DcpGameBuild game{};
        CHECK(dcp_game_build(dir.string().c_str(), &game) == 1);
        CHECK(std::string_view(game.label) == build.label);
        CHECK(game.cheats == build.cheats && game.japanese_npc_voices == build.japanese_npc_voices);
        CHECK(game.true_ultrawide == build.true_ultrawide && game.resolution_by_signature == build.resolution_by_signature);
        CHECK(game.resolution_site_count == build.resolution_sites.size());
        CHECK(!RestrictedToNativeResolution(&build) == (game.resolution_site_count > 0 || game.resolution_by_signature));
        CHECK(!(game.resolution_site_count > 0 && game.resolution_by_signature));
        fs::remove_all(dir);
    }
}

struct BootImage {
    std::vector<uint8_t> bytes;
    uint64_t entry = 0;
};

uint64_t Read64(std::ifstream& in) {
    uint8_t b[8] = {};
    in.read(reinterpret_cast<char*>(b), 8);
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i) value = value << 8 | b[i];
    return value;
}

// The memory image inside boot.bin (layout written by launcher/boot_image.cpp, read as tests/isa_tests.c does).
bool LoadBootImage(const GameBuild& build, BootImage& out) {
    std::ifstream in(fs::path(DCP_SOURCE_DIR) / "build" / "image" / build.folder / "boot.bin", std::ios::binary);
    char magic[8] = {};
    if (!in.read(magic, 8) || std::memcmp(magic, "DCPBOOT1", 8)) {
        return false;
    }
    const uint64_t size = Read64(in);
    out.entry = Read64(in);
    const uint64_t segments = Read64(in), relocs = Read64(in), names = Read64(in);
    Read64(in), Read64(in);
    for (int i = 0; i < 4; ++i) Read64(in);
    const uint64_t modules = Read64(in);
    in.seekg(std::streamoff(modules * 56), std::ios::cur);
    const uint64_t bindings = Read64(in);
    in.seekg(std::streamoff(bindings * 24 + segments * 24 + names * 128 + relocs * 32), std::ios::cur);
    out.bytes.resize(size);
    return size && in.read(reinterpret_cast<char*>(out.bytes.data()), std::streamsize(size)).good();
}

struct Patched {
    uint64_t begin, end;
    std::string what;
};

// Every range the loader may write in this build's image when every feature is on; `located` holds the sites the
// loader's locators found when the build's profile asks for them.
std::vector<Patched> PatchedRanges(const GameBuild& build, const BootImage& image, const DcpFeatureSites& located) {
    std::vector<Patched> out;
    const auto add = [&](uint64_t at, uint64_t length, std::string what) { out.push_back({at, at + length, std::move(what)}); };
    const uint64_t size = image.bytes.size();
    for (const DcpResolutionSite& site : build.resolution_sites) {
        add(site.offset, site.opcode_length + 4u, "render size");
    }
    uint64_t story = 0, tutorial = 0;
    skip_offers_locate(image.bytes.data(), size, Key(build).c_str(), &story, &tutorial);
    if (story) add(story, 47, "story skip window");
    if (tutorial) add(tutorial, 12, "tutorial skip window");
    for (const SystemTypeRecord& record : system_type_records) {
        if (std::string_view(record.build) == build.label) {
            for (uint32_t site : record.sites) add(site, 5, "system type");
        }
    }
    for (const StartupSaveRecord& record : startup_save_records) {
        if (std::string_view(record.build) == build.label) add(record.step_offset, STARTUP_SAVE_SITE_LENGTH, "startup save");
    }
    for (const LzcntBuild& record : lzcnt_builds) {
        if (std::string_view(record.build) != build.label) continue;
        for (size_t i = 0; i < LZCNT_SITE_COUNT; ++i) add(record.offsets[i], lzcnt_shapes[i].length, "lzcnt");
    }
    {
        const DcpExecRange executable{0, size};
        const DcpImageView view{image.bytes.data(), size, &executable, 1};
        DcpExitGameSites exit_game{};
        DcpLocateReport report{};
        if (dcp_locate_exit_game_sites(&view, &exit_game, &report)) {
            add(exit_game.sheet_fill, 10, "exit game sheet");
            add(exit_game.row_widget, 5, "exit game row");
            add(exit_game.help_limits[0], 1, "exit game help limit");
            add(exit_game.help_limits[1], 1, "exit game help limit");
            add(exit_game.part_table_loads[0], 7, "exit game part table load");
            add(exit_game.part_table_loads[1], 7, "exit game part table load");
            add(exit_game.menu_tick, 6, "exit game menu tick");
        }
    }
    const auto add_ultrawide = [&](const UltrawideRecord& record, const uint32_t* loads) {
        GuestHook hooks[ULTRAWIDE_HOOK_COUNT];
        ultrawide_record_hooks(&record, hooks);
        for (const GuestHook& hook : hooks) add(hook.offset, hook.length, std::string("ultrawide ") + hook.name);
        for (size_t i = 0; i < DCP_ORTHO_LOADS; ++i) add(loads[i], 8, "pass ortho load");
    };
    if (build.features_by_signature) {
        if (located.ultrawide) {
            add_ultrawide(ultrawide_record_from_sites(&located.ultrawide_sites, size, image.entry), located.ultrawide_sites.ortho_loads);
        }
        if (build.japanese_npc_voices && located.voices) {
            add(located.voice_sites.npc_cue, VOICE_NPC_SITE_LENGTH, "npc voice cue");
            add(located.voice_sites.resource_acquire, VOICE_RES_ACQUIRE_SITE_LENGTH, "npc voice acquire");
        }
        if (build.cheats && located.cheats) {
            for (const GuestHook& hook : cheats::HooksAt(located.cheat_sites.hooks, image.bytes.data(), size)) {
                add(hook.offset, hook.length, std::string("cheat ") + hook.name);
            }
        }
        return out;
    }
    for (const UltrawideRecord& record : ultrawide_records) {
        if (std::string_view(record.build) != build.label) continue;
        uint32_t loads[DCP_ORTHO_LOADS];
        for (size_t i = 0; i < DCP_ORTHO_LOADS; ++i) loads[i] = uint32_t(dcp_pass_ortho_sites[i].offset);
        add_ultrawide(record, loads);
    }
    if (build.japanese_npc_voices) {
        add(VOICE_NPC_SITE_OFFSET, VOICE_NPC_SITE_LENGTH, "npc voice cue");
        add(VOICE_RES_ACQUIRE_SITE_OFFSET, VOICE_RES_ACQUIRE_SITE_LENGTH, "npc voice acquire");
    }
    if (build.cheats) {
        for (const GuestHook& hook : cheats::Hooks()) add(hook.offset, hook.length, std::string("cheat ") + hook.name);
    }
    return out;
}

std::span<const uint8_t> AsBytes(const std::string& text) {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

const char* SystemTypeText(const char*) {
    return "text";
}

// Installs every feature the build has on a private copy of its image, in the loader's order and through the loader's
// path (tables for CUSA10454 1.00, the located sites otherwise). Each fingerprint must hold and no installed patch may
// meet another's bytes; a feature the build does not have must refuse.
void InstallEverything(const GameBuild& build, const BootImage& image, const DcpFeatureSites& located) {
    const Capabilities caps = CapabilitiesOf(build);
    const uint64_t size = image.bytes.size(), entry = image.entry;
    const size_t page = 0x1000, base = (size_t(size) + page - 1) / page * page;
    const size_t cheat_stubs = cheats::Hooks().size() * guest_hook_stub_size();
    const size_t room = cheat_stubs + startup_save_hook_stub_bytes() + voice_hooks_stub_bytes() +
                        system_type_hook_stub_bytes() + ultrawide_hooks_stub_bytes() + lzcnt_compat_stub_bytes() +
                        exit_game_hook_stub_bytes();
    std::vector<uint8_t> memory(base + room + page, 0);
    std::memcpy(memory.data(), image.bytes.data(), size);
    uint8_t* area = memory.data() + base;
    const auto take = [&](size_t bytes) {
        uint8_t* at = area;
        area += bytes;
        return at;
    };
    uint8_t* lzcnt_stubs = take(lzcnt_compat_stub_bytes());
    uint8_t* cheat_area = take(cheat_stubs);
    uint8_t* save_area = take(startup_save_hook_stub_bytes());
    uint8_t* voice_area = take(voice_hooks_stub_bytes());
    uint8_t* system_area = take(system_type_hook_stub_bytes());
    uint8_t* ultrawide_area = take(ultrawide_hooks_stub_bytes());
    uint8_t* exit_game_area = take(exit_game_hook_stub_bytes());
    uint8_t* image_bytes = memory.data();

    const char* name = nullptr;
    CHECK(lzcnt_compat_install(image_bytes, size, entry, lzcnt_stubs, lzcnt_compat_stub_bytes(), &name) == LZCNT_COMPAT_OK);
    CHECK(name && std::string_view(name) == build.label && CountLzcnt(image_bytes, size) == 0);

    const std::span<const GuestHook> cheat_hooks = cheats::Hooks();
    if (build.features_by_signature) {
        CHECK(caps.cheats && located.cheats);
        CHECK(runtime_cheat_hooks_install_located(image_bytes, size, entry, &located.cheat_sites, cheat_area, cheat_stubs) == 1);
        CHECK(runtime_cheats_available() == 1);
        runtime_cheats_set_available(0);
    } else if (caps.cheats) {
        size_t installed = 0;
        for (size_t i = 0; i < cheat_hooks.size(); ++i) {
            size_t bad = 0;
            installed += guest_hooks_install(image_bytes, size, entry, &cheats::kSupportedImage, &cheat_hooks[i], 1,
                                             cheat_area + i * guest_hook_stub_size(), guest_hook_stub_size(), &bad) == GUEST_HOOK_OK;
        }
        CHECK(installed == cheat_hooks.size());
    } else {
        CHECK(guest_hooks_verify(image_bytes, size, entry, &cheats::kSupportedImage, cheat_hooks.data(), cheat_hooks.size(), nullptr) !=
              GUEST_HOOK_OK);
    }

    CHECK((startup_save_hook_install(image_bytes, size, entry, save_area, startup_save_hook_stub_bytes()) != 0) ==
          (caps.startup_save != Mapping::kNone));
    if (build.features_by_signature) {
        CHECK(located.voices == caps.npc_voices);
        CHECK(!caps.npc_voices || voice_hooks_install_located(image_bytes, size, entry, &located.voice_sites, voice_area,
                                                             voice_hooks_stub_bytes()) == 1);
    } else {
        CHECK((voice_hooks_install(image_bytes, size, entry, voice_area, voice_hooks_stub_bytes()) != 0) == caps.npc_voices);
    }
    CHECK(system_type_hook_install(image_bytes, size, entry, system_area, system_type_hook_stub_bytes(), SystemTypeText) == 1);
    CHECK(skip_offers_install(image_bytes, size, Key(build).c_str(), SKIP_OFFER_STORY | SKIP_OFFER_TUTORIAL) ==
          (SKIP_OFFER_STORY | SKIP_OFFER_TUTORIAL));

    {
        const DcpExecRange executable{0, size};
        const DcpImageView original{image.bytes.data(), size, &executable, 1};
        DcpExitGameSites sites{};
        char failure[200];
        const DcpGameBuild game = LoaderView(build);
        CHECK(dcp_resolve_exit_game(&game, &original, &sites, failure, sizeof(failure)) == 1);
        CHECK(exit_game_hook_install_located(image_bytes, size, entry, &sites, exit_game_area, exit_game_hook_stub_bytes()) == 1);
        CHECK(image_bytes[sites.help_limits[0]] == EXIT_GAME_MENU_ITEMS && image_bytes[sites.help_limits[1]] == EXIT_GAME_MENU_ITEMS);
        DcpGameBuild off = game;
        off.exit_game = 0;
        CHECK(dcp_resolve_exit_game(&off, &original, &sites, failure, sizeof(failure)) == 0);
    }

    const DcpResolution target = dcp_render_target(&dcp_resolutions[5], &dcp_aspect_ratios[0]);
    char error[320];
    DcpResolutionSite found[DCP_RENDER_SIZE_SIGNATURE_COUNT];
    DcpResolutionSignatureResult results[DCP_RENDER_SIZE_SIGNATURE_COUNT];
    const DcpResolutionSite* sites = build.resolution_sites.data();
    size_t site_count = build.resolution_sites.size();
    if (site_count == 0) {
        CHECK(dcp_find_resolution_sites(image_bytes, size, dcp_render_size_signatures, DCP_RENDER_SIZE_SIGNATURE_COUNT, found, results) == 1);
        sites = found;
        site_count = DCP_RENDER_SIZE_SIGNATURE_COUNT;
    }
    CHECK(dcp_apply_resolution(image_bytes, size, sites, site_count, &target, error, sizeof(error)) == 1);

    char detail[720] = {};
    if (build.features_by_signature) {
        CHECK(caps.true_ultrawide && located.ultrawide);
        CHECK(ultrawide_hooks_install_located(image_bytes, size, entry, &located.ultrawide_sites, ultrawide_area,
                                              ultrawide_hooks_stub_bytes(), 1.3125, detail, sizeof(detail)) == 1);
        DcpOrthoSite loads[DCP_ORTHO_LOADS];
        for (size_t i = 0; i < DCP_ORTHO_LOADS; ++i) {
            loads[i].offset = located.ultrawide_sites.ortho_loads[i];
            loads[i].value = dcp_pass_ortho_sites[i].value;
        }
        CHECK(dcp_apply_true_ultrawide(image_bytes, size, base + room, loads, DCP_ORTHO_LOADS, 1.3125, error, sizeof(error)) == 1);
    } else {
        const int ultrawide = ultrawide_hooks_install(image_bytes, size, entry, ultrawide_area, ultrawide_hooks_stub_bytes(),
                                                      1.3125, detail, sizeof(detail));
        CHECK(ultrawide == (caps.true_ultrawide ? 1 : 0));
        CHECK(!caps.true_ultrawide ||
              dcp_apply_true_ultrawide(image_bytes, size, base + room, dcp_pass_ortho_sites, DCP_PASS_ORTHO_SITE_COUNT, 1.3125,
                                       error, sizeof(error)) == 1);
    }
    CHECK(std::string_view(detail).find("off (") == std::string_view::npos);

    // Near misses: the same bytes with another entry or size are no supported image to any mapping.
    std::vector<uint8_t> near(memory.begin(), memory.begin() + std::ptrdiff_t(base + room + page));
    std::memcpy(near.data(), image.bytes.data(), size);
    for (const uint64_t other_entry : {entry + 0x10, entry ^ 0x1000}) {
        CHECK(lzcnt_compat_install(near.data(), size, other_entry, near.data() + base, room, nullptr) == LZCNT_COMPAT_UNKNOWN_IMAGE);
        CHECK(!startup_save_hook_install(near.data(), size, other_entry, near.data() + base, room));
        CHECK(!voice_hooks_install(near.data(), size, other_entry, near.data() + base, room));
        CHECK(!ultrawide_hooks_install(near.data(), size, other_entry, near.data() + base, room, 1.3125, detail, sizeof(detail)));
        CHECK(guest_hooks_verify(near.data(), size, other_entry, &cheats::kSupportedImage, nullptr, 0, nullptr) != GUEST_HOOK_OK);
    }
    CHECK(!ultrawide_hooks_install(near.data(), size - 0x10, entry, near.data() + base, room, 1.3125, detail, sizeof(detail)));
    CHECK(std::equal(near.begin(), near.begin() + std::ptrdiff_t(size), image.bytes.begin()));
}

struct FixtureReport {
    bool executable = false;
    bool prepared = false;
    bool image_identity = false;
    std::string note;
};

FixtureReport CheckFixtures(const GameBuild& build) {
    FixtureReport report;
    const fs::path root(DCP_SOURCE_DIR);
    BootImage image;
    if (LoadBootImage(build, image)) {
        report.executable = true;
        const uint8_t* bytes = image.bytes.data();
        const uint64_t size = image.bytes.size();

        // The loader's own entry, with the loader's view of the build, before any patch.
        DcpGameBuild game{};
        CHECK(LoaderBuild(build, game));
        const DcpExecRange executable{0, size};
        const DcpImageView view{bytes, size, &executable, 1};
        DcpFeatureSites located{};
        const int resolved = dcp_resolve_features(&game, &view, 1, 1, 1, &located);
        if (build.features_by_signature) {
            if (!located.cheats || !located.ultrawide || located.voices != build.japanese_npc_voices) {
                std::printf("  %s: cheats %s | voices %s | ultrawide %s\n", build.label, located.cheat_failure,
                            located.voice_failure, located.ultrawide_failure);
            }
            CHECK(resolved == 1 && located.cheats && located.ultrawide && located.voices == build.japanese_npc_voices);
            std::string mismatch;
            CHECK(cheats::LayoutMatches(located.cheat_sites.layout, &mismatch));
            std::vector<DcpExecRange> hooks;
            for (const GuestHook& hook : cheats::HooksAt(located.cheat_sites.hooks, bytes, size)) {
                hooks.push_back({hook.offset, hook.offset + hook.length});
            }
            CHECK(hooks.size() == DCP_CHEAT_HOOKS && !dcp_ranges_overlap(hooks.data(), hooks.size()));
            std::set<uint32_t> functions(std::begin(located.cheat_sites.functions), std::end(located.cheat_sites.functions));
            CHECK(functions.size() == DCP_CHEAT_FUNCTIONS && !functions.count(0));
        } else {
            CHECK(resolved == 0 && !located.cheats && !located.voices && !located.ultrawide);
        }

        DcpExitGameSites exit_game{};
        char exit_failure[200];
        if (!dcp_resolve_exit_game(&game, &view, &exit_game, exit_failure, sizeof(exit_failure))) {
            std::printf("  %s: Exit Game: %s\n", build.label, exit_failure);
        }
        CHECK(dcp_resolve_exit_game(&game, &view, &exit_game, exit_failure, sizeof(exit_failure)) == 1);

        std::vector<Patched> ranges = PatchedRanges(build, image, located);
        std::sort(ranges.begin(), ranges.end(), [](const Patched& a, const Patched& b) { return a.begin < b.begin; });
        size_t overlaps = 0, outside = 0;
        for (size_t i = 0; i < ranges.size(); ++i) {
            outside += ranges[i].end > size;
            if (i && ranges[i].begin < ranges[i - 1].end) {
                ++overlaps;
                std::printf("  %s: %s at 0x%llx overlaps %s at 0x%llx\n", build.label, ranges[i].what.c_str(),
                            static_cast<unsigned long long>(ranges[i].begin), ranges[i - 1].what.c_str(),
                            static_cast<unsigned long long>(ranges[i - 1].begin));
            }
        }
        CHECK(overlaps == 0 && outside == 0);

        const GuestImageId* id = ImageOf(build);
        CHECK(id && id->size == size && id->entry == image.entry);

        uint64_t story = 0, tutorial = 0, found_story = 0, found_tutorial = 0;
        CHECK(skip_offers_locate(bytes, size, Key(build).c_str(), &story, &tutorial) == 1 && story && tutorial);
        CHECK(skip_offers_locate(bytes, size, "unlisted", &found_story, &found_tutorial) == 0);
        CHECK(found_story == story && found_tutorial == tutorial);

        SystemTypeRecord table{}, searched{};
        CHECK(system_type_locate(bytes, size, image.entry, &table) == 1 && std::string_view(table.build) == build.label);
        CHECK(system_type_locate(bytes, size, image.entry + 1, &searched) == 2);
        CHECK(searched.sites[0] == table.sites[0] && searched.sites[1] == table.sites[1] && searched.set_text == table.set_text);

        uint32_t lzcnt[LZCNT_SITE_COUNT];
        const LzcntBuild* lzcnt_table = nullptr;
        for (const LzcntBuild& record : lzcnt_builds) {
            if (std::string_view(record.build) == build.label) lzcnt_table = &record;
        }
        CHECK(lzcnt_compat_locate(bytes, size, lzcnt) == LZCNT_COMPAT_OK);
        CHECK(lzcnt_table && std::equal(std::begin(lzcnt), std::end(lzcnt), lzcnt_table->offsets));

        DcpResolutionSite sites[DCP_RENDER_SIZE_SIGNATURE_COUNT];
        DcpResolutionSignatureResult results[DCP_RENDER_SIZE_SIGNATURE_COUNT];
        CHECK(dcp_find_resolution_sites(bytes, size, dcp_render_size_signatures, DCP_RENDER_SIZE_SIGNATURE_COUNT, sites,
                                        results) == 1);
        std::set<uint32_t> expected, recovered;
        for (const DcpResolutionSite& site : build.resolution_sites) expected.insert(site.offset);
        for (const DcpResolutionSite& site : sites) recovered.insert(site.offset);
        CHECK(expected == recovered);

        InstallEverything(build, image, located);
        report.note = std::to_string(ranges.size()) + " patched ranges, no overlap";
        if (build.features_by_signature) {
            report.note += "; cheats " + std::to_string(DCP_CHEAT_HOOKS) + " hooks, " + std::to_string(DCP_CHEAT_FUNCTIONS) +
                           " functions, " + std::to_string(DCP_LAYOUT_COUNT) + " layout fields located";
        }
    }

    const fs::path image_dir = root / "build" / "image" / build.folder;
    if (fs::is_directory(image_dir)) {
        report.image_identity = BuildOfImageDir(image_dir.string().c_str()) == &build;
        CHECK(report.image_identity);
    }
    const fs::path prepared = root / "game" / "prepared" / build.folder;
    if (fs::is_directory(prepared)) {
        report.prepared = true;
        const GameCheck check = CheckGameTree(prepared);
        if (check.state != GameFileState::kValid || check.build != &build) {
            std::printf("  %s: prepared folder: %s\n", build.label, check.detail.c_str());
        }
        CHECK(check.state == GameFileState::kValid && check.build == &build);
        CHECK(check.eboot_sha256 == build.eboot_sha256);
        const std::vector<TextLanguage> installed = InstalledTextLanguages(prepared, &build);
        CHECK(installed.size() == TextLanguagesOf(&build).size());
        CHECK(SameLanguages(installed, TextLanguagesOf(&build)));
        CHECK(build.regional_archive == nullptr || fs::is_regular_file(prepared / build.regional_archive));
        CHECK(!build.japanese_npc_voices || NpcVoiceResourcesPresent(prepared));
        CHECK(JapaneseNpcVoicesOffered(prepared, &build) == build.japanese_npc_voices);

        const std::string xbox = ReadText("launcher/prompts/xbox-prompts.dcpx");
        const std::optional<BlockPatch> prompts = ParseBlockPatch(AsBytes(xbox));
        std::string why;
        const bool fits = prompts && PromptPatchFits(prepared, build, *prompts, &why);
        if (!fits) {
            std::printf("  %s: Xbox prompts: %s\n", build.label, why.c_str());
        }
        CHECK(fits && XboxPromptsAvailable(prepared, build, AsBytes(xbox)));
        CHECK(!PromptArchives(prepared, build).empty());

        const std::optional<BlockPatch> spider = ParseBlockPatch(AsBytes(ReadText("launcher/prompts/arachnophobia-spider.dcpx")));
        CpkArchive shared;
        const CpkEntry* atlas = shared.Open(prepared / "DRACRO.CPK") ? shared.Find(kSpiderEntry) : nullptr;
        CHECK(spider && spider->targets.size() == 1 && spider->targets[0].name == kSpiderEntry && atlas);
        if (spider && atlas) {
            const std::vector<uint8_t> data = shared.Read(*atlas);
            const uint32_t crc = Crc32(data);
            const BlockPatchTarget& target = spider->targets[0];
            CHECK(data.size() == target.size && (crc == target.crc_before || crc == target.crc_after));
        }
    }
    return report;
}

} // namespace

int main() {
    TestContract();
    TestGlobalFeatures();
    TestExitGameLanguages();
    TestDisabledFeatureCannotPass();
    TestUpdateParity();
    TestGateConsistency();
    TestResolutionStrategy();
    TestSyntheticRenderLocator();
    TestSyntheticLzcnt();
    TestOnline();
    TestUnknownBuilds();
    TestLauncherAgreesWithLoader();

    std::map<std::string, FixtureReport> reports;
    for (const GameBuild& build : SupportedBuilds()) {
        g_scope = build.label;
        reports[build.label] = CheckFixtures(build);
    }

    std::printf("\n");
    int shared_failures = g_scope_failures["contract"] + g_scope_failures["synthetic"] + g_scope_failures["unknown"];
    for (const GameBuild& build : SupportedBuilds()) {
        const FixtureReport& report = reports[build.label];
        const bool failed = g_scope_failures[build.label] > 0 || shared_failures > 0;
        const char* status = failed ? "FAIL" : report.executable ? "PASS real" : "PASS synthetic";
        std::string detail = report.executable ? "executable" : "real fixture unavailable";
        if (report.executable && !report.note.empty()) detail += ": " + report.note;
        detail += report.prepared ? "; prepared folder" : "; no prepared folder";
        std::printf("PARITY-BUILD|%s|%s|%s\n", build.label, status, detail.c_str());
    }
    const size_t builds = SupportedBuilds().size();
    for (size_t f = 0; f < kFeatureCount; ++f) {
        const FeatureTally& tally = g_tally[f];
        std::string status = (tally.passed == builds && shared_failures == 0 ? "PASS " : "FAIL ") +
                             std::to_string(tally.passed) + "/" + std::to_string(builds);
        if (tally.native) {
            status += " (" + std::to_string(tally.passed - tally.native) + " toggle, " + std::to_string(tally.native) +
                      " native)";
        }
        if (!tally.failures.empty()) status += " (" + tally.failures + ")";
        std::printf("PARITY-FEATURE|%s|%s\n", kFeatureNames[f], status.c_str());
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
