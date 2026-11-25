#include "build_contract.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <regex>
#include <set>
#include <string_view>

#include "appearance_hooks.h"
#include "boss_battle.h"
#include "boss_battle_online.h"
#include "cheat_hooks.h"
#include "comms_bridge.h"
#include "cyrillic_glyphs.h"
#include "dcp_launcher.h"
#include "dcp_resolution.h"
#include "default_message_hook.h"
#include "early_story_online.h"
#include "endgame_unlocks.h"
#include "exit_game_hook.h"
#include "gallery_unlock.h"
#include "game_files.h"
#include "game_profile.h"
#include "hangul_glyphs.h"
#include "hp_override_hooks.h"
#include "hydeland_town.h"
#include "item_attack_cap.h"
#include "kraken_fix.h"
#include "latin_glyphs.h"
#include "lzcnt_compat.h"
#include "menu_buttons.h"
#include "network_access.h"
#include "party_hook.h"
#include "runtime_cheats.h"
#include "scan_fixture.h"
#include "skip_intro.h"
#include "skip_offers.h"
#include "startup_save_hook.h"
#include "system_type_hook.h"
#include "touch_absolute.h"
#include "ultrawide_hooks.h"
#include "voice_hooks.h"

namespace parity {
namespace {

using namespace dcp;

constexpr uint64_t kFixtureRegion = 0xd00000;
constexpr size_t kPage = 0x1000;
constexpr size_t kStubs = 0x40000;

ContractResult g_result;
std::string g_scope, g_feature;

void Check(bool ok, const std::string& what) {
    ++g_result.checks;
    if (ok) return;
    ++g_result.failures;
    ++g_result.build_failures[g_scope];
    if (!g_feature.empty()) ++g_result.feature_failures[g_feature];
    std::printf("FAIL [%s] contract: %s\n", g_scope.c_str(), what.c_str());
}

DcpGameBuild LoaderView(const GameBuild& build) {
    DcpGameBuild game{};
    game.label = build.label;
    game.cheats = build.cheats;
    game.voice_language = build.voice_language;
    game.true_ultrawide = build.true_ultrawide;
    game.resolution_sites = build.resolution_sites.data();
    game.resolution_site_count = build.resolution_sites.size();
    game.resolution_by_signature = build.resolution_by_signature;
    game.features_by_signature = build.features_by_signature;
    game.exit_game = build.exit_game;
    game.unlock_gallery = build.unlock_gallery;
    game.title_id = build.title_id;
    game.version = build.version;
    return game;
}

const GameBuild& BuildOf(std::string_view key) {
    for (const GameBuild& build : SupportedBuilds()) {
        if (BuildKey(build) == key) return build;
    }
    return SupportedBuilds().front();
}

// CUSA10454 1.01's image has 1.00's size and entry, so 1.00's records are its own.
const GameBuild& RecordsOf(const GameBuild& build) {
    return BuildKey(build) == "CUSA10454-1.01" ? BuildOf("CUSA10454-1.00") : build;
}

struct ImageId {
    uint64_t size, entry;
};

ImageId ImageOf(const GameBuild& build) {
    const std::string_view label = RecordsOf(build).label;
    for (const LzcntBuild& record : lzcnt_builds) {
        if (label == record.build) return {record.image.size, record.image.entry};
    }
    for (const StartupSaveRecord& record : startup_save_records) {
        if (label == record.build) return {record.image.size, record.image.entry};
    }
    return {0x1239000, 0x54b120};
}

bool Listed(const GameBuild& build) {
    return boss_battle_build_listed(BuildKey(build).c_str()) != 0;
}

// The synthetic executable of `build` with the sites laid out as `layout` has them, moved by `shift`.
struct Subject {
    const GameBuild* build = nullptr;
    const GameBuild* layout = nullptr;
    uint64_t shift = 0, size = 0, entry = 0;
    std::unique_ptr<Fx, void (*)(Fx*)> fx{nullptr, fx_destroy};
    DcpGameBuild game{};
    std::string key;

    size_t base() const { return (size_t(size) + kPage - 1) / kPage * kPage; }
    Subject Copy() const {
        Subject copy;
        copy.build = build, copy.layout = layout, copy.shift = shift, copy.size = size, copy.entry = entry;
        copy.game = game, copy.key = key;
        copy.fx.reset(fx_clone(fx.get()));
        return copy;
    }
    std::vector<uint8_t> Memory() const {
        std::vector<uint8_t> memory(base() + kStubs + kPage, 0);
        std::memcpy(memory.data(), fx_image(fx.get()), size);
        return memory;
    }
};

Subject MakeSubject(const GameBuild& build, const GameBuild& layout, uint64_t shift, bool tables_move = false) {
    Subject subject;
    subject.build = &build;
    subject.layout = &layout;
    subject.shift = shift;
    subject.key = BuildKey(build);
    subject.game = LoaderView(build);
    const ImageId id = ImageOf(build);
    subject.size = id.size;
    subject.entry = id.entry;
    Fx* fx = fx_create(id.size, kFixtureRegion);
    subject.fx.reset(fx);
    const GameBuild& records = RecordsOf(layout);
    const std::string key = BuildKey(layout);
    PlantScanned(fx, records, *build.translation, records.label, ReferenceOf(records.label));
    fixture_boss_battle(fx, key.c_str(), TextChangedSite(fx));
    fixture_item_attack_cap(fx, key.c_str());
    fixture_kraken(fx, key.c_str());
    fixture_skip_intro(fx, key.c_str());
    fixture_skip_offers(fx, key.c_str());
    fixture_gallery(fx, key.c_str());
    fixture_menu_buttons(fx, key.c_str());
    fixture_endgame(fx, key.c_str());
    fixture_system_type(fx, records.label);
    fixture_touch(fx);
    fixture_appearance(fx);
    fixture_character_override(fx);
    fixture_default_messages(fx);
    fixture_comms(fx);
    if (tables_move) PlantStartupSave(fx, build.label);
    if (shift) fx_shift(fx, shift);
    if (!tables_move) PlantStartupSave(fx, build.label);
    Check(fx_conflicts(fx) == 0, "the synthetic executable is consistent (" + key + " layout)");
    return subject;
}

// ---- the features

struct Run {
    const Subject& subject;
    uint8_t* image;
    uint8_t* stubs;
    uint64_t size, entry;
    const char* key;
    const DcpGameBuild* game;

    DcpImageView View(DcpExecRange& range) const {
        range = {0, size};
        return {image, size, &range, 1};
    }
};

const char* SystemTypeText(const char*) { return "text"; }

bool Resolution(Run& run) {
    if (!run.game) return false;
    const DcpResolutionSite* sites = run.game->resolution_sites;
    size_t count = run.game->resolution_site_count;
    DcpResolutionSite found[DCP_RENDER_SIZE_SIGNATURE_COUNT];
    DcpResolutionSignatureResult results[DCP_RENDER_SIZE_SIGNATURE_COUNT];
    if (!count && run.game->resolution_by_signature) {
        count = DCP_RENDER_SIZE_SIGNATURE_COUNT;
        if (!dcp_find_resolution_sites(run.image, run.size, dcp_render_size_signatures, count, found, results)) return false;
        sites = found;
    }
    const DcpResolution& k4k = dcp_resolutions[dcp_resolution_index("4K")];
    if (k4k.width != 3840 || !count) return false;
    const DcpResolution target = dcp_render_target(&k4k, &dcp_aspect_ratios[0]);
    char error[320];
    return dcp_apply_resolution(run.image, run.size, sites, count, &target, error, sizeof(error)) == 1;
}

bool Ultrawide(Run& run) {
    if (!run.game || !run.game->true_ultrawide) return false;
    char detail[720] = {}, error[320] = {};
    const size_t slots = run.subject.base() + kStubs;
    if (!run.game->features_by_signature) {
        return dcp_apply_true_ultrawide(run.image, run.size, slots, dcp_pass_ortho_sites, DCP_PASS_ORTHO_SITE_COUNT, 1.3125, error,
                                        sizeof(error)) == 1 &&
               ultrawide_hooks_install(run.image, run.size, run.entry, run.stubs, ultrawide_hooks_stub_bytes(), 1.3125, detail,
                                       sizeof(detail)) == 1 &&
               std::string_view(detail).find("off (") == std::string_view::npos;
    }
    DcpExecRange range;
    const DcpImageView view = run.View(range);
    DcpFeatureSites located;
    dcp_resolve_features(run.game, &view, 0, 1, &located);
    if (!located.ultrawide) return false;
    DcpOrthoSite loads[DCP_ORTHO_LOADS];
    for (size_t i = 0; i < DCP_ORTHO_LOADS; ++i) loads[i] = {located.ultrawide_sites.ortho_loads[i], dcp_pass_ortho_sites[i].value};
    return ultrawide_hooks_install_located(run.image, run.size, run.entry, &located.ultrawide_sites, run.stubs,
                                           ultrawide_hooks_stub_bytes(), 1.3125, detail, sizeof(detail)) == 1 &&
           dcp_apply_true_ultrawide(run.image, run.size, slots, loads, DCP_ORTHO_LOADS, 1.3125, error, sizeof(error)) == 1;
}

bool Lzcnt(Run& run) {
    const char* name = nullptr;
    const LzcntCompatStatus status =
        lzcnt_compat_install(run.image, run.size, run.entry, run.stubs, lzcnt_compat_stub_bytes(), &name);
    if (status != LZCNT_COMPAT_UNKNOWN_IMAGE || !run.game) return status == LZCNT_COMPAT_OK;
    uint32_t offsets[LZCNT_SITE_COUNT];
    return lzcnt_compat_locate(run.image, run.size, offsets) == LZCNT_COMPAT_OK &&
           lzcnt_compat_apply(run.image, run.size, offsets, lzcnt_shapes, LZCNT_SITE_COUNT, run.stubs, lzcnt_compat_stub_bytes()) ==
               LZCNT_COMPAT_OK;
}

bool Cheats(Run& run) {
    if (!run.game || !runtime_cheats_hooks_wanted(run.game->cheats)) return false;
    bool installed;
    if (run.game->features_by_signature) {
        DcpExecRange range;
        const DcpImageView view = run.View(range);
        DcpFeatureSites located;
        dcp_resolve_features(run.game, &view, 1, 0, &located);
        installed = located.cheats && runtime_cheat_hooks_install_located(run.image, run.size, run.entry, &located.cheat_sites,
                                                                          run.stubs, kStubs) == 1;
        for (const uint32_t hook : located.cheat_sites.hooks) installed = installed && run.image[hook] == 0xe9;
    } else {
        installed = runtime_cheat_hooks_install(run.image, run.size, run.entry, run.stubs, kStubs) == 1;
        for (const GuestHook& hook : cheats::Hooks()) installed = installed && run.image[hook.offset] == 0xe9;
    }
    runtime_cheats_set_available(0);
    return installed;
}

bool Voice(Run& run) {
    if (!run.game) return false;
    DcpExecRange range;
    const DcpImageView view = run.View(range);
    DcpVoiceSites sites{};
    char failure[192];
    if (!dcp_resolve_voice_language(run.game, &view, &sites, failure, sizeof(failure))) return false;
    if (sites.narrator_route != DCP_NARRATOR_ROUTE_OK) return false;
    return voice_hooks_install(run.image, run.size, run.entry, &sites, run.stubs, voice_hooks_stub_bytes()) == 1;
}

bool Party(Run& run) {
    if (!run.game) return false;
    DcpExecRange range;
    const DcpImageView view = run.View(range);
    DcpPartySites sites{};
    char failure[192];
    return dcp_resolve_party(run.game, &view, &sites, failure, sizeof(failure)) &&
           party_hook_install_located(run.image, run.size, run.entry, &sites, run.stubs, party_hook_stub_bytes()) == 1;
}

bool SystemType(Run& run) {
    if (!run.game) return false;
    return system_type_hook_install(run.image, run.size, run.entry, run.stubs, system_type_hook_stub_bytes(), SystemTypeText) == 1;
}

bool StartupSave(Run& run) {
    return run.game && startup_save_hook_install(run.image, run.size, run.entry, run.stubs, startup_save_hook_stub_bytes()) == 1;
}

bool ExitGame(Run& run) {
    if (!run.game) return false;
    DcpExecRange range;
    const DcpImageView view = run.View(range);
    DcpExitGameSites sites{};
    char failure[192];
    return dcp_resolve_exit_game(run.game, &view, &sites, failure, sizeof(failure)) &&
           exit_game_hook_install_located(run.image, run.size, run.entry, &sites, run.stubs, exit_game_hook_stub_bytes()) == 1;
}

bool SkipIntro(Run& run) {
    const bool installed = skip_intro_install(run.image, run.size, run.entry, run.key, 1, run.stubs, skip_intro_stub_bytes()) == 1;
    skip_intro_claim();
    return installed;
}

bool Gallery(Run& run) {
    return gallery_unlock_install(run.image, run.size, run.entry, run.key, 1, run.stubs, gallery_unlock_stub_bytes()) == 1;
}

bool ItemCap(Run& run) { return item_attack_cap_install(run.image, run.size, run.key) == 1; }

bool MenuButtonsWestern(Run& run) { return run.key && menu_buttons_install(run.image, run.size, run.key, 0) == 1; }

bool MenuButtonsJapanese(Run& run) { return menu_buttons_install(run.image, run.size, run.key, 1) == 1; }

bool StoryOffer(Run& run) { return skip_offers_install(run.image, run.size, run.key, SKIP_OFFER_STORY) == SKIP_OFFER_STORY; }

bool TutorialOffer(Run& run) { return skip_offers_install(run.image, run.size, run.key, SKIP_OFFER_TUTORIAL) == SKIP_OFFER_TUTORIAL; }

bool TownSites(const Run& run, const uint8_t* bytes, DcpEarlyStorySites& town) {
    if (!run.game) return false;
    const DcpExecRange range{0, run.size};
    const DcpImageView view{bytes, run.size, &range, 1};
    char failure[192];
    return dcp_resolve_early_story(run.game, &view, &town, failure, sizeof(failure)) == 1;
}

void ResetOnline() {
    early_story_set_active(0);
    hydeland_town_set_installed(0);
    network_access_set_installed(0);
    network_access_set(NETWORK_ACCESS_BOSS_BATTLE, nullptr);
    network_menu_set_create(nullptr);
    boss_battle_online_set_available(0);
    boss_battle_set_available(0);
}

bool EarlyStory(Run& run) {
    DcpEarlyStorySites town{};
    const bool installed = TownSites(run, run.image, town) && town.hint_create &&
                           early_story_online_install(run.image, run.size, run.entry, &town, run.stubs,
                                                      early_story_online_stub_bytes()) == 1;
    ResetOnline();
    return installed;
}

bool BossBattleOffline(Run& run) {
    DcpEarlyStorySites town{};
    const bool town_found = TownSites(run, fx_image(run.subject.fx.get()), town);
    const bool installed = boss_battle_install(run.image, run.size, run.entry, run.key, 0, town_found ? &town : nullptr, run.stubs,
                                               boss_battle_stub_bytes()) == 1 &&
                           boss_battle_available() && boss_battle_startable_count() == BOSS_BATTLE_ENCOUNTERS;
    if (installed) {
        // Off: the Inn keeps its rows and a frame of a scene that is not the game's leaves its registers alone.
        boss_battle_set_enabled(0);
        GuestRegs regs{}, before{};
        static uint8_t scene[0x10000];
        regs.r15 = before.r15 = uint64_t(uintptr_t(scene));
        boss_battle_scene_hook(&regs);
        Check(!boss_battle_inn_row_wanted() && !boss_battle_session_active() && std::memcmp(&regs, &before, sizeof(regs)) == 0,
              "Boss Battle Off leaves the game's own path alone");
    }
    ResetOnline();
    return installed;
}

// Online Play: Early Story's hooks go in first and own the Start check.
bool BossBattleOnline(Run& run) {
    DcpEarlyStorySites town{};
    const bool installed = TownSites(run, run.image, town) &&
                           early_story_online_install(run.image, run.size, run.entry, &town, run.stubs,
                                                      early_story_online_stub_bytes()) == 1 &&
                           boss_battle_install(run.image, run.size, run.entry, run.key, 1, &town,
                                               run.stubs + early_story_online_stub_bytes(), boss_battle_stub_bytes()) == 1 &&
                           boss_battle_online_available() && network_menu_request_available();
    ResetOnline();
    return installed;
}

bool Endgame(Run& run) {
    const unsigned both = ENDGAME_UNLOCK_LABYRINTH | ENDGAME_UNLOCK_ULTIMATE_TOWER;
    const bool installed =
        endgame_unlocks_install(run.image, run.size, run.entry, run.key, both, run.stubs, endgame_unlocks_stub_bytes()) == both;
    endgame_unlocks_set(0);
    return installed;
}

bool Kraken(Run& run) {
    return kraken_fix_install(run.image, run.size, run.entry, run.key, run.stubs, kraken_fix_stub_bytes()) == 1;
}

bool Touch(Run& run) {
    return touch_absolute_install(run.image, run.size, run.entry, run.key, run.stubs, touch_absolute_stub_bytes()) == 1;
}

const char* MessageText(unsigned) { return "message"; }

bool DefaultMessages(Run& run) {
    if (!run.game) return false;
    return default_message_install(run.image, run.size, run.entry, MessageText, MessageText, run.stubs, default_message_stub_bytes()) == 1;
}

bool Appearance(Run& run) {
    if (!run.game) return false;
    AppearanceSites sites{};
    const bool installed = appearance_hooks_locate(run.image, run.size, &sites) &&
                           appearance_hooks_install(run.image, run.size, run.entry, &sites, run.stubs, appearance_hooks_stub_bytes()) == 1;
    appearance_hooks_set_weapon_names(nullptr, nullptr);
    return installed;
}

bool CharacterOverride(Run& run) {
    if (!run.game) return false;
    const DcpExecRange range{0, run.size};
    const DcpImageView view{fx_image(run.subject.fx.get()), run.size, &range, 1};
    DcpPartySites party{};
    char failure[192];
    CharacterOverrideSites sites{};
    if (!dcp_resolve_party(run.game, &view, &party, failure, sizeof(failure)) ||
        !character_override_hooks_locate(run.image, run.size, &sites))
        return false;
    sites.player_update = party.tick;
    return character_override_hooks_install(run.image, run.size, run.entry, &sites, run.stubs, character_override_hooks_stub_bytes()) == 1;
}

bool QuickChat(Run& run) {
    if (!run.game) return false;
    CommsBridgeSites sites{};
    const bool installed = comms_bridge_locate(run.image, run.size, &sites) &&
                           comms_bridge_install(run.image, run.size, run.entry, &sites, run.stubs, comms_bridge_stub_bytes()) == 1;
    comms_bridge_attach(nullptr, nullptr);
    return installed;
}

bool Fonts(Run& run) {
    if (!run.game) return false;
    const TranslationProfile& profile = *run.subject.build->translation;
    const uint8_t* pages = font_glyph_pages(run.image, run.size, profile.cyrillic.probe);
    static const CyrillicMetrics metrics[CYRILLIC_GLYPH_COUNT] = {};
    if (!pages || !cyrillic_glyphs_install(run.image, run.size, pages, CyrillicCells(profile.cyrillic.cells), metrics)) return false;
    if (profile.font_probe) {
        pages = font_glyph_pages(run.image, run.size, profile.font_probe);
        if (!pages || !latin_glyphs_install(run.image, run.size, font_glyph_page(run.image, run.size, pages, 0))) return false;
    }
    if (profile.hangul.probe) {
        const HangulGlyph glyph{0xac00, kFontProbeCell, 30, 2};
        pages = font_glyph_pages(run.image, run.size, profile.hangul.probe);
        if (!pages || !hangul_glyphs_install(run.image, run.size, pages, &glyph, 1)) return false;
    }
    return true;
}

// How a feature reaches a build's code: a table of its own, a signature search, or one locator for every build.
using Expect = std::function<Path(const GameBuild&)>;

Path TableOrSignature(const GameBuild& build) { return Listed(build) ? Path::kStatic : Path::kSignature; }
Path Shared(const GameBuild&) { return Path::kShared; }
Path CheatPath(const GameBuild& build) { return build.features_by_signature ? Path::kSignature : Path::kStatic; }

Path ResolutionPath(const GameBuild& build) { return build.resolution_by_signature ? Path::kSignature : Path::kStatic; }

template <typename Records>
Path RecordPath(const Records& records, const GameBuild& build) {
    const std::string_view label = RecordsOf(build).label;
    return std::any_of(std::begin(records), std::end(records), [&](const auto& record) { return label == record.build; })
               ? Path::kStatic
               : Path::kSignature;
}

// The save data step of these builds asks a question the runtime answers; the others' never does (docs/LAUNCHER.md).
Path StartupPath(const GameBuild& build) {
    const std::string_view title = build.title_id;
    return title == "CUSA07001" || title == "CUSA10487" ? Path::kStatic : Path::kNotNeeded;
}

enum Flags : unsigned {
    kNone = 0,
    kParts = 1,       // installs in steps that each check their own sites
    kIndependent = 2, // a table's sites install one by one: a refused one is left alone, the others are not
    kAnyImage = 4,    // keyed by the image itself: an unknown build never has one of its images
    kOptional = 8,    // some of its sites only add to it: a refused one is left alone
    kData = 16,       // patches data found by content, not instructions
    kMayKeep = 32,    // some builds already have what it asks for
};

struct Feature {
    const char* name;
    bool (*install)(Run&);
    Expect expect;
    unsigned flags;
    void (*again)(Fx*) = nullptr; // plants all of its code a second time
};

const std::vector<Feature>& Features() {
    static const std::vector<Feature> features = {
        {"Render resolution and 4K", Resolution, ResolutionPath, kNone},
        {"True Ultrawide", Ultrawide, CheatPath, kParts},
        {"LZCNT compatibility", Lzcnt, [](const GameBuild& b) { return RecordPath(lzcnt_builds, b); }, kAnyImage},
        {"Cheats", Cheats, CheatPath, kIndependent},
        {"Voice Language", Voice, Shared, kParts},
        {"Party hooks", Party, Shared, kNone},
        {"Online System type", SystemType, [](const GameBuild& b) { return RecordPath(system_type_records, b); }, kNone},
        {"Startup save", StartupSave, StartupPath, kNone},
        {"Exit Game", ExitGame, Shared, kNone},
        {"Skip Intro", SkipIntro, TableOrSignature, kNone, fixture_skip_intro_again},
        {"Unlock Gallery", Gallery, TableOrSignature, kNone},
        {"Item attack cap", ItemCap,
         [](const GameBuild& b) { return fixture_lists_item_attack_cap(BuildKey(b).c_str()) == 1 ? Path::kStatic : Path::kSignature; }, kNone},
        {"Menu Buttons (Western)", MenuButtonsWestern, TableOrSignature, kMayKeep, fixture_menu_buttons_again},
        {"Menu Buttons (Japanese)", MenuButtonsJapanese, TableOrSignature, kMayKeep, fixture_menu_buttons_again},
        {"Story Skip Offer", StoryOffer, TableOrSignature, kNone, fixture_skip_offers_again},
        {"Tutorial Skip Offer", TutorialOffer, TableOrSignature, kNone, fixture_skip_offers_again},
        {"Early Story Online", EarlyStory, Shared, kNone},
        {"Boss Battle", BossBattleOffline, TableOrSignature, kOptional},
        {"Boss Battle Online", BossBattleOnline, TableOrSignature, kParts | kOptional},
        {"Endgame unlocks", Endgame, TableOrSignature, kNone},
        {"Kraken fix", Kraken,
         [](const GameBuild& b) { return fixture_lists_kraken(BuildKey(b).c_str()) == 1 ? Path::kStatic : Path::kSignature; }, kNone},
        {"Touchscreen mode", Touch, Shared, kNone, fixture_touch},
        {"Default messages", DefaultMessages, Shared, kOptional, fixture_default_messages},
        {"Appearance Editor hooks", Appearance, Shared, kNone, fixture_appearance},
        {"Character stat hooks", CharacterOverride, Shared, kNone, fixture_character_override},
        {"Quick Chat", QuickChat, Shared, kNone, fixture_comms},
        {"Text fonts", Fonts, Shared, kData | kParts},
    };
    return features;
}

bool Install(const Feature& feature, const Subject& subject, std::vector<uint8_t>& memory, uint64_t size, bool known = true) {
    Run run{subject, memory.data(), memory.data() + subject.base(), size, subject.entry, known ? subject.key.c_str() : nullptr,
            known ? &subject.game : nullptr};
    return feature.install(run);
}

struct Range {
    uint64_t begin, end;
};

std::vector<Range> Changes(const uint8_t* before, const uint8_t* after, uint64_t size) {
    std::vector<Range> out;
    constexpr uint64_t kBlock = 0x1000;
    for (uint64_t at = 0; at < size;) {
        if (at % kBlock == 0 && at + kBlock <= size && !std::memcmp(before + at, after + at, kBlock)) {
            at += kBlock;
            continue;
        }
        if (before[at] == after[at]) {
            ++at;
            continue;
        }
        const uint64_t begin = at;
        while (at < size && before[at] != after[at]) ++at;
        if (!out.empty() && begin - out.back().end < 3) out.back().end = at;
        else out.push_back({begin, at});
    }
    return out;
}

// Wrong, missing and cut-off instructions at each patched place, a second copy of the code, and an unknown build.
void CheckRefusals(const Feature& feature, const Subject& subject, Path path, const std::vector<Range>& sites) {
    const std::string name = feature.name;
    const bool parts = feature.flags & kParts, independent = path == Path::kStatic && (feature.flags & kIndependent);
    const std::vector<uint8_t> original = subject.Memory();
    std::vector<uint8_t> memory = original;
    const auto refused = [&](uint64_t size, bool known) {
        const bool installed = Install(feature, subject, memory, size, known);
        const bool clean = parts || independent || std::memcmp(memory.data(), original.data(), subject.size) == 0;
        memory = original;
        return !installed && clean;
    };
    if (!(feature.flags & kAnyImage)) Check(refused(subject.size, false), name + " refuses an unknown build");
    Check(refused(0x40, true), name + " refuses a truncated image");
    if (feature.flags & kData) return;
    Check(refused(sites.front().begin + 1, true), name + " refuses code cut off at its first site");

    // The site is changed, the feature must not install, and nothing but the change may differ.
    std::vector<uint8_t> broken = original;
    const auto stands = [&](const Range& site, size_t length, const std::function<void(uint8_t*)>& change) {
        change(&broken[site.begin]);
        std::memcpy(&memory[site.begin], &broken[site.begin], length);
        const bool installed = Install(feature, subject, memory, subject.size);
        const bool left_alone = std::memcmp(&memory[site.begin], &broken[site.begin], length) == 0;
        bool good;
        if (independent || (installed && (feature.flags & kOptional))) good = left_alone;
        else good = !installed && (parts || std::memcmp(memory.data(), broken.data(), subject.size) == 0);
        std::memcpy(&broken[site.begin], &original[site.begin], length);
        if (installed || parts || independent || !good) memory = original;
        else std::memcpy(&memory[site.begin], &original[site.begin], length);
        return good;
    };
    const size_t step = sites.size() > 40 ? ((feature.flags & kParts) ? 6 : 2) : 1;
    size_t wrong = 0, missing = 0;
    for (size_t i = 0; i < sites.size(); i += step) {
        const size_t length = size_t(std::min<uint64_t>(sites[i].end - sites[i].begin, 16));
        wrong += !stands(sites[i], length, [](uint8_t* at) { *at ^= 0x10; });
        if (i % (2 * step) == 0) missing += !stands(sites[i], length, [&](uint8_t* at) { std::memset(at, 0xcc, length); });
    }
    Check(wrong == 0, name + " refuses wrong bytes (accepted at " + std::to_string(wrong) + " site(s))");
    Check(missing == 0, name + " refuses missing code (accepted at " + std::to_string(missing) + " site(s))");
    if (path == Path::kStatic) return;
    size_t copies = 0, stopped = 0, strayed = 0;
    for (const Range& site : sites) {
        const Subject copy = subject.Copy();
        if (feature.again) feature.again(copy.fx.get());
        else if (!fx_duplicate(copy.fx.get(), site.begin)) continue;
        ++copies;
        memory = copy.Memory();
        const bool installed = Install(feature, copy, memory, copy.size);
        stopped += !installed;
        for (const Range& change : Changes(fx_image(copy.fx.get()), memory.data(), copy.size))
            strayed += std::none_of(sites.begin(), sites.end(), [&](const Range& s) { return change.begin < s.end && s.begin < change.end; });
        if (copies == 3 || feature.again) break;
    }
    Check(stopped > 0, name + " refuses a second copy of its code");
    Check(strayed == 0, name + " never writes to a second copy of its code");
}

// Every feature in the loader's order on one image: all install, and none writes a byte another wrote.
void CheckTogether(const Subject& subject, bool online) {
    std::vector<uint8_t> memory = subject.Memory();
    std::vector<uint8_t> writer(subject.size, 0), before;
    uint8_t* stubs = memory.data() + subject.base();
    for (size_t i = 0; i < Features().size(); ++i) {
        const Feature& feature = Features()[i];
        const std::string_view name = feature.name;
        if (feature.expect(*subject.build) == Path::kNotNeeded || name == "Menu Buttons (Japanese)") continue;
        const bool online_only = name == "Party hooks" || name == "Early Story Online" || name == "Boss Battle Online" ||
                                 name == "Quick Chat" || name == "Online System type" || name == "Character stat hooks";
        if (name == "Early Story Online" || (online ? name == "Cheats" || name == "Boss Battle" : online_only)) continue;
        g_feature = feature.name;
        before.assign(memory.begin(), memory.begin() + std::ptrdiff_t(subject.size));
        Run run{subject, memory.data(), stubs, subject.size, subject.entry, subject.key.c_str(), &subject.game};
        Check(feature.install(run), std::string(feature.name) + " installs beside the other features" + (online ? " online" : ""));
        bool overlap = false;
        for (const Range& change : Changes(before.data(), memory.data(), subject.size)) {
            for (uint64_t at = change.begin; at < change.end; ++at) {
                if (before[at] == memory[at]) continue;
                overlap |= writer[at] != 0;
                writer[at] = uint8_t(i + 1);
            }
        }
        Check(!overlap, std::string(feature.name) + " overlaps no other feature" + (online ? " online" : ""));
        stubs += kStubs / Features().size() / 0x100 * 0x100;
    }
}

void CheckFeature(const Feature& feature, const Subject& subject, bool refusals) {
    g_feature = feature.name;
    const GameBuild& build = *subject.build;
    const Path path = feature.expect(build);
    const std::string name = feature.name;
    std::vector<uint8_t> memory = subject.Memory();
    const std::vector<uint8_t> before = memory;
    const bool installed = Install(feature, subject, memory, subject.size);
    if (path == Path::kNotNeeded) {
        Check(!installed && std::memcmp(memory.data(), before.data(), subject.size) == 0, name + " is not needed and changes nothing");
        return;
    }
    Check(installed, name + " installs (" + PathName(path) + ", " + BuildKey(*subject.layout) + " layout)");
    if (!installed) return;
    const std::vector<Range> sites = Changes(before.data(), memory.data(), subject.size);
    Check(!sites.empty() || (feature.flags & kMayKeep), name + " changes its sites");
    for (const Range& site : sites) {
        bool planted = false;
        for (uint64_t at = site.begin; at < site.end; ++at) planted |= fx_planted(subject.fx.get(), at) != 0;
        Check(planted || (feature.flags & kData), name + " writes only where its code is");
    }
    if (refusals && !sites.empty()) CheckRefusals(feature, subject, path, sites);
}

// A search answers wherever the code is; a table only where it lists it.
void CheckPaths(const GameBuild& build, const Subject& moved) {
    for (const Feature& feature : Features()) {
        const Path path = feature.expect(build);
        if (path == Path::kNotNeeded) continue;
        g_feature = feature.name;
        std::vector<uint8_t> memory = moved.Memory();
        const bool follows = Install(feature, moved, memory, moved.size);
        const bool expected = path != Path::kStatic;
        Check(follows == expected,
              std::string(feature.name) + (expected ? " follows its code when it moves" : " uses its table only where its code is"));
    }
}

std::string ReadSource(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The installers the loader calls and the contract feature that runs each; nullptr is no game feature.
struct Installer {
    const char* name;
    const char* feature;
};
constexpr Installer kInstallers[] = {
    {"appearance_hooks_install", "Appearance Editor hooks"},
    {"boss_battle_install", "Boss Battle"},
    {"character_override_hooks_install", "Character stat hooks"},
    {"comms_bridge_install", "Quick Chat"},
    {"cyrillic_glyphs_install", "Text fonts"},
    {"dcp_crash_dump_install", nullptr},
    {"dcp_apply_resolution", "Render resolution and 4K"},
    {"dcp_apply_true_ultrawide", "True Ultrawide"},
    {"default_message_install", "Default messages"},
    {"early_story_online_install", "Early Story Online"},
    {"endgame_unlocks_install", "Endgame unlocks"},
    {"exit_game_hook_install_located", "Exit Game"},
    {"gallery_unlock_install", "Unlock Gallery"},
    {"hangul_glyphs_install", "Text fonts"},
    {"item_attack_cap_install", "Item attack cap"},
    {"kraken_fix_install", "Kraken fix"},
    {"latin_glyphs_install", "Text fonts"},
    {"lzcnt_compat_install", "LZCNT compatibility"},
    {"menu_buttons_install", "Menu Buttons (Western)"},
    {"party_hook_install_located", "Party hooks"},
    {"runtime_cheat_hooks_install", "Cheats"},
    {"runtime_cheat_hooks_install_located", "Cheats"},
    {"runtime_log_install_cleanup", nullptr},
    {"skip_intro_install", "Skip Intro"},
    {"skip_offers_install", "Story Skip Offer"},
    {"startup_save_hook_install", "Startup save"},
    {"system_type_hook_install", "Online System type"},
    {"touch_absolute_install", "Touchscreen mode"},
    {"ultrawide_hooks_install", "True Ultrawide"},
    {"ultrawide_hooks_install_located", "True Ultrawide"},
    {"voice_hooks_install", "Voice Language"},
};

void CheckRegistration() {
    g_scope = "contract";
    const std::filesystem::path source(DCP_SOURCE_DIR);
    const std::string loader = ReadSource(source / "src" / "loader.c");
    const std::regex call("\\b([a-z_0-9]+_install[a-z_]*|dcp_apply_[a-z_]+)\\(");
    std::set<std::string> called;
    for (std::sregex_iterator it(loader.begin(), loader.end(), call), end; it != end; ++it) called.insert((*it)[1]);
    for (const std::string& name : called) {
        const Installer* known = std::find_if(std::begin(kInstallers), std::end(kInstallers), [&](const Installer& i) { return name == i.name; });
        Check(known != std::end(kInstallers), "the loader calls " + name + ", which no contract feature runs");
        if (known == std::end(kInstallers) || !known->feature) continue;
        Check(std::any_of(Features().begin(), Features().end(), [&](const Feature& f) { return std::string_view(f.name) == known->feature; }),
              name + " belongs to a contract feature");
    }
    for (const Installer& installer : kInstallers) Check(called.count(installer.name) == 1, std::string(installer.name) + " is called by the loader");
}

// A source's build keys: every build, or exactly the builds with tables (the others are searched for).
void CheckBuildTables() {
    g_scope = "contract";
    std::set<std::string> all, tabled, records;
    for (const GameBuild& build : SupportedBuilds()) {
        all.insert(BuildKey(build));
        if (Listed(build)) tabled.insert(BuildKey(build));
        if (Listed(build) && BuildKey(build) != "CUSA10454-1.01") records.insert(BuildKey(build));
    }
    const std::set<std::string> startup = {"CUSA07001-1.00", "CUSA07001-1.02", "CUSA07001-1.03", "CUSA10487-1.03"};
    const std::regex key("\"(CUSA[0-9]{5})[- ]([0-9][.][0-9]{2})[^\"]*\"");
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(DCP_SOURCE_DIR) / "src")) {
        const std::string name = entry.path().filename().string(), extension = entry.path().extension().string();
        if (extension != ".c" && extension != ".cpp") continue;
        const std::string text = ReadSource(entry.path());
        std::set<std::string> found;
        for (std::sregex_iterator it(text.begin(), text.end(), key), end; it != end; ++it) found.insert((*it)[1].str() + "-" + (*it)[2].str());
        if (found.empty()) continue;
        const bool complete = found == all || found == tabled || found == records || (name == "startup_save_hook.c" && found == startup);
        Check(complete, name + " lists every build, or every build that has tables");
    }
}

std::string BuildsWith(const std::function<bool(const GameBuild&)>& test) {
    std::string out;
    for (const GameBuild& build : SupportedBuilds()) {
        if (test(build)) out += (out.empty() ? "" : ", ") + BuildKey(build);
    }
    return out;
}

void ListDifferences() {
    const auto add = [](const char* feature, const std::string& builds, const char* reason) {
        g_result.differences.push_back(std::string(feature) + "|" + builds + "|" + reason);
    };
    add("Startup save hook", BuildsWith([](const GameBuild& b) { return StartupPath(b) == Path::kNotNeeded; }),
        "not needed: their startup creates the first save without it");
    add("Startup notice of Skip Intro", BuildsWith([](const GameBuild& b) { return std::string_view(b.title_id) == "CUSA07001"; }),
        "only the Japanese builds show the notice Skip Intro passes");
    add("Japanese menu buttons", BuildsWith([](const GameBuild& b) { return std::string_view(b.title_id) == "CUSA07001" || std::string_view(b.title_id) == "CUSA10209"; }),
        "these builds start with Circle confirming; the port's Western default patches them instead");
    add("Port English", BuildsWith([](const GameBuild& b) { return OffersPortEnglish(&b); }), "the other builds have an English slot of their own");
    add("Port Korean", BuildsWith([](const GameBuild& b) { return b.translation->hangul.probe != 0; }),
        "Asia/Hong Kong has its own Korean; the corpus is keyed to the English records, and the Japanese sheets have no map to them");
    add("Native text languages", "by region", "each build offers the slots its own archive holds, and the port's languages beside them");
    add("Cheat and True Ultrawide tables", BuildsWith([](const GameBuild& b) { return !b.features_by_signature; }),
        "the reference build keeps its tables; every other build is located");
}

void CheckMetadata() {
    g_scope = "contract";
    const size_t builds = SupportedBuilds().size();
    Check(builds == 8, "eight supported builds have contract metadata");
    size_t listed = 0, searched = 0;
    for (const GameBuild& build : SupportedBuilds()) {
        const std::string key = BuildKey(build);
        listed += boss_battle_build_listed(key.c_str());
        searched += boss_battle_build_searched(key.c_str());
        Check(boss_battle_build_listed(key.c_str()) + boss_battle_build_searched(key.c_str()) == 1, key + " has one Boss Battle path");
        Check(fixture_lists_item_attack_cap(key.c_str()) >= 0 && fixture_lists_kraken(key.c_str()) >= 0,
              key + " is in the item attack cap and Kraken tables");
        Check(build.cheats && build.voice_language && build.true_ultrawide && build.exit_game && build.unlock_gallery &&
                  build.boss_battle && build.xbox_prompts && !RestrictedToNativeResolution(&build),
              key + " has every capability flag");
        Check(build.resolution_by_signature == build.resolution_sites.empty(), key + " has one render size path");
        Check(SitesOnlySearched(build) == build.resolution_by_signature, key + " reports its searched sites");
        const auto offers = [&](std::string_view language) {
            const auto languages = TextLanguagesOf(&build);
            return std::count_if(languages.begin(), languages.end(), [&](const TextLanguage& l) { return l.key == language; }) == 1;
        };
        const bool japanese = std::string_view(build.title_id) == "CUSA07001";
        Check(offers(kEnglishKey) && offers(kPortugueseBrazilKey) && offers(kRussianKey), key + " offers English, Portuguese and Russian");
        Check(offers(kKoreanKey) != japanese, key + " offers Korean where it has a Korean slot or English records");
        Check((build.translation->hangul.probe != 0) == build.translation->source.empty(), key + " draws Korean over English records only");
    }
    Check(listed + searched == builds, "every build has a path");
    Check(!std::filesystem::exists(std::filesystem::path(DCP_SOURCE_DIR) / "localization" / "ko" / "sources"),
          "the Korean corpus has no record map for another region's sheets");
    Check(dcp_aspect_index("21:9") >= 0 && DCP_ASPECT_COUNT == 6 && DCP_RESOLUTION_COUNT == 6, "the same shapes for every build");
}

} // namespace

const char* PathName(Path path) {
    switch (path) {
    case Path::kStatic: return "static";
    case Path::kSignature: return "signature";
    case Path::kShared: return "shared locator";
    case Path::kNotNeeded: return "not needed";
    }
    return "";
}

ContractResult RunBuildContract() {
    g_result = {};
    boss_battle_set_save_guard(+[] { return 1; }, +[] {});
    runtime_cheats_set_online(0);
    g_feature.clear();
    CheckMetadata();
    CheckRegistration();
    CheckBuildTables();
    ListDifferences();
    for (const Feature& feature : Features()) g_result.features.push_back(feature.name);
    for (const GameBuild& build : SupportedBuilds()) {
        g_scope = build.label;
        g_feature.clear();
        std::vector<const GameBuild*> layouts;
        if (Listed(build)) {
            layouts.push_back(&build);
        } else {
            // The unavailable executable's own layout is unknown: each compile the listed builds have stands for it.
            for (const char* key : {"CUSA07001-1.03", "CUSA10209-1.00", "CUSA10454-1.00"}) {
                if (ImageOf(build).size >= ImageOf(BuildOf(key)).size) layouts.push_back(&BuildOf(key));
            }
        }
        for (size_t i = 0; i < layouts.size(); ++i) {
            const uint64_t shift = Listed(build) ? 0 : 0x340;
            g_feature.clear();
            const Subject subject = MakeSubject(build, *layouts[i], shift);
            for (const Feature& feature : Features()) CheckFeature(feature, subject, i == 0);
            CheckTogether(subject, false);
            CheckTogether(subject, true);
            if (i == 0) CheckPaths(build, MakeSubject(build, *layouts[i], shift + 0x1240, true));
        }
        for (const Feature& feature : Features()) g_result.paths[build.label][feature.name] = feature.expect(build);
    }
    return g_result;
}

} // namespace parity
