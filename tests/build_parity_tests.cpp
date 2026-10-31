// The supported-build parity gate (tools/test.ps1), without the game or any window.
//
// Every supported build has one row of the contract below. The test fails when a build has no row, when the launcher's
// capability gates and the runtime mappings disagree, when an update loses anything its base offers, when a mapping is
// partial, ambiguous or overlapping, or when an unknown executable could reach a supported build's mapping. Local boot images and prepared folders under
// build/image and game/prepared are read only; a build without one is proven by synthetic images and reported as such.
//
// Stdout ends with PARITY-BUILD lines that tools/test.ps1 turns into its summary.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "cheat_hooks.h"
#include "dcp_launcher.h"
#include "dcp_resolution.h"
#include "game_files.h"
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

const char* MappingName(Mapping mapping) {
    return mapping == Mapping::kTable ? "table" : mapping == Mapping::kSignature ? "signature" : "none";
}

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

// What the launcher offers and the loader can install, derived from the profile and the runtime tables.
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

struct Contract {
    const char* label;
    bool cheats, xbox_prompts, npc_voices, true_ultrawide, custom_resolution;
    Mapping resolution, skip_offers, system_type, lzcnt, startup_save;
    const char* languages;
};

constexpr Mapping T = Mapping::kTable, S = Mapping::kSignature, N = Mapping::kNone;
const char* const kWestern = "english french german italian spanish";

// One row per supported build. Features every build offers regardless of its executable (frame rate options, frame rate
// and render resolution in the title, Story and Tutorial Skip Offers, Arachnophobia Safe Mode, graphics filters,
// presentation and window modes, keyboard and controller mapping, Online Play) have no column: the rows' mappings make
// them available, and TestLauncherGates fails if the launcher starts gating one by build.
const Contract kContracts[] = {
    {"CUSA10454 1.00 USA", true, true, true, true, true, T, T, T, T, N, kWestern},
    {"CUSA10454 1.01 USA", true, true, true, true, true, S, S, S, S, N, kWestern},
    {"CUSA07001 1.00 Japan", false, false, false, true, true, T, T, T, T, T, "japanese"},
    {"CUSA07001 1.02 Japan", false, false, false, true, true, T, T, T, T, T, "japanese"},
    {"CUSA07001 1.03 Japan", false, false, false, true, true, T, T, T, T, T, "japanese"},
    {"CUSA10209 1.00 Asia/Hong Kong", false, false, false, true, true, T, T, T, T, N, "traditional_chinese korean"},
    {"CUSA10209 1.02 Asia/Hong Kong", false, false, false, true, true, S, S, S, S, N, "traditional_chinese korean"},
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
        const Capabilities caps = CapabilitiesOf(build);
        const bool same = caps.cheats == row->cheats && caps.xbox_prompts == row->xbox_prompts &&
                          caps.npc_voices == row->npc_voices && caps.true_ultrawide == row->true_ultrawide &&
                          caps.custom_resolution == row->custom_resolution && caps.resolution == row->resolution &&
                          caps.skip_offers == row->skip_offers && caps.system_type == row->system_type &&
                          caps.lzcnt == row->lzcnt && caps.startup_save == row->startup_save &&
                          caps.languages == row->languages;
        g_scope = build.label;
        CHECK(same);
        if (!same) {
            std::printf("  %s: cheats %d xbox %d voices %d ultrawide %d custom size %d (%s) skip %s system type %s lzcnt %s "
                        "save %s languages %s\n",
                        build.label, caps.cheats, caps.xbox_prompts, caps.npc_voices, caps.true_ultrawide,
                        caps.custom_resolution, MappingName(caps.resolution), MappingName(caps.skip_offers),
                        MappingName(caps.system_type), MappingName(caps.lzcnt), MappingName(caps.startup_save),
                        caps.languages.c_str());
        }
        // Every build offers the executable-independent features through these mappings.
        CHECK(caps.skip_offers != Mapping::kNone && caps.system_type != Mapping::kNone && caps.lzcnt != Mapping::kNone);
        CHECK(caps.custom_resolution && caps.resolution != Mapping::kNone);
    }
    g_scope = "contract";
    for (const Contract& row : kContracts) {
        CHECK(FindBuild(row.label) != nullptr);
    }
}

// An update offers everything its base offers; there is no list of exceptions. Items that do not apply to a region are
// not parity gaps and are not listed (a Japanese build has no English NPC voices to replace).
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
        // What an update has beyond the table-based mappings comes from the locators its own profile asks for.
        const bool by_locator = update->features_by_signature;
        CHECK(by_locator == (std::string_view(update->title_id) != "CUSA07001"));
        if (by_locator) {
            CHECK(!ImageOf(*update) && !Listed(ultrawide_records, *update));
            CHECK(update->cheats == base->cheats && update->japanese_npc_voices == base->japanese_npc_voices &&
                  update->true_ultrawide == base->true_ultrawide);
        }
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
        // A feature has a table-based mapping (US 1.00's cheats and voices, the ultrawide records) or its profile's locators.
        CHECK(caps.true_ultrawide == (Listed(ultrawide_records, build) || build.features_by_signature));
        CHECK(!Listed(ultrawide_records, build) || caps.true_ultrawide);
        CHECK(caps.cheats == (IsUs100(build) || (build.features_by_signature && std::string_view(build.title_id) == "CUSA10454")));
        CHECK(caps.npc_voices == caps.cheats);
        CHECK(!build.features_by_signature || !ImageOf(build));
        CHECK(caps.startup_save == (std::string_view(build.title_id) == "CUSA07001" ? Mapping::kTable : Mapping::kNone));
        CHECK(caps.xbox_prompts == (std::string_view(build.title_id) == "CUSA10454" && build.regional_archive != nullptr));
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
                                   "SupportsJapaneseNpcVoices(build_)",    "build_->cheats",
                                   "xbox_available_",                      "XboxPromptsAvailable",
                                   "build_->xbox_prompts",                 "InstalledTextLanguages(paths_.app0, build_)",
                                   "InstalledPromptStyle(paths_.app0, *build_", "PromptArchives(paths_.app0, *build_",
                                   "build_->label",                        "BuildMetadataText(*build_)",
                                   "InstalledGameName(files_, build_)",    "build_ = check.build",
                                   "const GameBuild* build_",              "|| !build_)",
                                   "build_(check.build)",                  "build = *build_",
                                   "BaseBuildOf(*build_)"};
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
                                 "SupportsJapaneseNpcVoices(build_)", "build_->cheats", "xbox_available_"}) {
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
                             "known_build && game_build.true_ultrawide"}) {
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

// Every range the loader may write in this build's image when every feature is on.
std::vector<Patched> PatchedRanges(const GameBuild& build, const BootImage& image) {
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
    for (const UltrawideRecord& record : ultrawide_records) {
        if (std::string_view(record.build) != build.label) continue;
        GuestHook hooks[ULTRAWIDE_HOOK_COUNT];
        ultrawide_record_hooks(&record, hooks);
        for (const GuestHook& hook : hooks) add(hook.offset, hook.length, std::string("ultrawide ") + hook.name);
        for (const DcpOrthoSite& site : dcp_pass_ortho_sites) add(site.offset, 8, "pass ortho load");
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

const char* SystemTypeText(const char*) {
    return "text";
}

// Installs every feature the build has on a private copy of its image, in the loader's order. Each fingerprint must hold
// and no installed patch may meet another's bytes; a feature the build does not have must refuse.
void InstallEverything(const GameBuild& build, const BootImage& image) {
    const Capabilities caps = CapabilitiesOf(build);
    const uint64_t size = image.bytes.size(), entry = image.entry;
    const size_t page = 0x1000, base = (size_t(size) + page - 1) / page * page;
    const size_t cheat_stubs = cheats::Hooks().size() * guest_hook_stub_size();
    const size_t room = cheat_stubs + startup_save_hook_stub_bytes() + voice_hooks_stub_bytes() +
                        system_type_hook_stub_bytes() + ultrawide_hooks_stub_bytes() + lzcnt_compat_stub_bytes();
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
    uint8_t* image_bytes = memory.data();

    const char* name = nullptr;
    CHECK(lzcnt_compat_install(image_bytes, size, entry, lzcnt_stubs, lzcnt_compat_stub_bytes(), &name) == LZCNT_COMPAT_OK);
    CHECK(name && std::string_view(name) == build.label && CountLzcnt(image_bytes, size) == 0);

    const std::span<const GuestHook> cheat_hooks = cheats::Hooks();
    if (caps.cheats) {
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
    CHECK((voice_hooks_install(image_bytes, size, entry, voice_area, voice_hooks_stub_bytes()) != 0) == caps.npc_voices);
    CHECK(system_type_hook_install(image_bytes, size, entry, system_area, system_type_hook_stub_bytes(), SystemTypeText) == 1);
    CHECK(skip_offers_install(image_bytes, size, Key(build).c_str(), SKIP_OFFER_STORY | SKIP_OFFER_TUTORIAL) ==
          (SKIP_OFFER_STORY | SKIP_OFFER_TUTORIAL));

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
    const int ultrawide = ultrawide_hooks_install(image_bytes, size, entry, ultrawide_area, ultrawide_hooks_stub_bytes(), 1.3125,
                                                  detail, sizeof(detail));
    if (caps.true_ultrawide) {
        CHECK(ultrawide == 1 && std::string_view(detail).find("off (") == std::string_view::npos);
        CHECK(dcp_apply_true_ultrawide(image_bytes, size, base + room, dcp_pass_ortho_sites, DCP_PASS_ORTHO_SITE_COUNT, 1.3125,
                                       error, sizeof(error)) == 1);
    } else {
        CHECK(ultrawide == 0);
    }

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

        std::vector<Patched> ranges = PatchedRanges(build, image);
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

        InstallEverything(build, image);
        report.note = std::to_string(ranges.size()) + " patched ranges, no overlap";
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
    }
    return report;
}

} // namespace

int main() {
    TestContract();
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
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
