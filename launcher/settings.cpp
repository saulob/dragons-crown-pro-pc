#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <initializer_list>
#include <system_error>
#include <utility>
#include <vector>

#include "dcp_keys.h"
#include "dcp_launcher.h"
#include "dcp_resolution.h"
#include "game_files.h"
#include "game_process.h"
#include "net/online_version.h"
#include "online_settings.h"
#include "text_languages.h"
#include "utf8.h"

namespace dcp {
namespace {

constexpr ManagedSetting kBaseSettings[] = {
    {kShowLauncherKey, "1"},
    {kDisplayModeKey, "windowed"},
    {kWindowWidthKey, "1920"},
    {kWindowHeightKey, "1080"},
    {kMonitorKey, "1"},
    {kVSyncKey, "1"},
    {kPresentationKey, kDefaultPresentation},
    {kUpscaleKey, "0"},
    {kSharpenKey, "1"},
    {kSharpenStrengthKey, "75"},
    {kRenderResolutionKey, kNativeResolution},
    {kAspectRatioKey, kDefaultAspectRatio},
    {kTrueUltrawideKey, "0"},
    {kGraphicsCardKey, ""},
    {kAntiAliasingKey, "0"},
    {kAnisotropyKey, "0"},
    {kTextLanguageKey, "english"},
    {kFrameRateKey, "60"},
    {kShowFpsTitleKey, "0"},
    {kShowResolutionTitleKey, "0"},
    {kShowFrameCounterKey, "0"},
    {kDetailedLoggingKey, "1"},
    {kControllerPromptsKey, "playstation"},
    {kArachnophobiaSafeModeKey, "0"},
    {kJapaneseNpcVoicesKey, "0"},
    {kAllowOverlayProgramsKey, "0"},
    {kStorySkipOfferKey, "0"},
    {kTutorialSkipOfferKey, "0"},
    {kOnlineEnabledKey, "0"},
    {kOnlineNameKey, net::kDefaultOnlineName},
    {kOnlineUdpPortKey, "0"},
    {kOnlineStunKey, net::kDefaultStunServers},
    {kPingDisplayKey, "0"},
    {cheats::kInfiniteHealthKey, "0"},
    {cheats::kInfiniteAllyHealthKey, "0"},
    {cheats::kInfiniteManaKey, "0"},
    {cheats::kInfiniteAllyManaKey, "0"},
    {cheats::kInfiniteLivesKey, "0"},
    {cheats::kInfiniteDurabilityKey, "0"},
    {cheats::kInfiniteAllyDurabilityKey, "0"},
    {cheats::kInfiniteArrowsKey, "0"},
    {cheats::kInfiniteSkillPointsKey, "0"},
    {cheats::kInfiniteSkillUsesKey, "0"},
    {cheats::kInfiniteGoldKey, "0"},
    {cheats::kMovementSpeedKey, "1"},
    {cheats::kLegacyMovementSpeedKey, "0"},
    {cheats::kExperienceMultiplierKey, "1"},
    {kLauncherXKey, ""},
    {kLauncherYKey, ""},
    {kLauncherWidthKey, ""},
    {kLauncherHeightKey, ""},
    {kLauncherMaximizedKey, "0"},
    {kLauncherFullscreenKey, "0"},
};

constexpr std::string_view kPlacementKeys[] = {kLauncherXKey,         kLauncherYKey,         kLauncherWidthKey,
                                                kLauncherHeightKey,    kLauncherMaximizedKey, kLauncherFullscreenKey};

constexpr char kFileHeader[] =
    "# Dragon's Crown Pro user settings (the launcher and the in-game menu write this file).\n"
    "# Remove a line to use the default.\n";

} // namespace

std::span<const ManagedSetting> ManagedSettings() {
    static const std::vector<ManagedSetting> settings = [] {
        static std::deque<std::string> default_names;
        std::vector<ManagedSetting> all(std::begin(kBaseSettings), std::end(kBaseSettings));
        for (const DcpKeyInfo& action : dcp_key_actions) {
            char name[32];
            default_names.emplace_back(dcp_key_name(action.default_scancode, name, sizeof(name)));
            all.push_back({action.ini_key, default_names.back()});
        }
        return all;
    }();
    return settings;
}

bool IsValidSetting(std::string_view key, std::string_view value) {
    const auto one_of = [&](std::initializer_list<std::string_view> values) {
        return std::find(values.begin(), values.end(), value) != values.end();
    };
    if (std::any_of(std::begin(dcp_key_actions), std::end(dcp_key_actions),
                    [&](const DcpKeyInfo& action) { return key == action.ini_key; })) {
        return dcp_key_from_name(std::string(value).c_str()) != 0;
    }
    if (key == kDisplayModeKey) {
        return one_of({"windowed", "borderless"});
    }
    if (key == kAnisotropyKey) {
        return one_of({"0", "2", "4", "8", "16"});
    }
    if (key == kTextLanguageKey) {
        const std::span<const GameBuild> builds = SupportedBuilds();
        return value == kRussianKey || std::any_of(builds.begin(), builds.end(), [&](const GameBuild& build) {
            return std::any_of(build.text_languages.begin(), build.text_languages.end(),
                               [&](const TextLanguage& language) { return value == language.key; });
        });
    }
    if (key == kFrameRateKey) {
        return one_of({"30", "60", "90", "120", "144", "165", "240", "0"});
    }
    if (key == kPresentationKey) {
        return std::any_of(std::begin(kPresentationChoices), std::end(kPresentationChoices),
                           [&](const PresentationChoice& choice) { return value == choice.key; });
    }
    if (key == kControllerPromptsKey) {
        return one_of({"playstation", "xbox"});
    }
    if (key == cheats::kExperienceMultiplierKey) {
        return std::any_of(std::begin(cheats::kExperienceChoices), std::end(cheats::kExperienceChoices),
                           [&](const cheats::Choice& choice) { return value == choice.saved; });
    }
    if (key == cheats::kMovementSpeedKey) {
        return std::any_of(std::begin(cheats::kMovementSpeedChoices), std::end(cheats::kMovementSpeedChoices),
                           [&](const cheats::Choice& choice) { return value == choice.saved; });
    }
    if (key == kRenderResolutionKey) {
        return std::any_of(std::begin(dcp_resolutions), std::end(dcp_resolutions),
                           [&](const DcpResolution& resolution) { return value == resolution.key; });
    }
    if (key == kAspectRatioKey) {
        return std::any_of(std::begin(dcp_aspect_ratios), std::end(dcp_aspect_ratios),
                           [&](const DcpAspectRatio& aspect) { return value == aspect.key; });
    }
    if (IsOnlineSetting(key)) {
        return IsValidOnlineSetting(key, value);
    }
    return true;
}

bool IsLauncherPlacementSetting(std::string_view key) {
    for (const std::string_view placement_key : kPlacementKeys) {
        if (placement_key == key) {
            return true;
        }
    }
    return false;
}

LauncherSettings::LauncherSettings(std::filesystem::path file) : file_(std::move(file)) {
    true_value = "1";
    false_value = "0";
    const trg::TextFileSettings stored(Utf8(file_));
    for (const ManagedSetting& setting : ManagedSettings()) {
        if (stored.Has(setting.key)) {
            saved_[std::string(setting.key)] = stored.Get(setting.key);
        }
    }
    pending_ = saved_;
}

std::string LauncherSettings::Get(std::string_view key, std::string_view def) const {
    const auto it = pending_.find(key);
    if (it == pending_.end() || it->second.empty() || !IsValidSetting(key, it->second)) {
        return std::string(def);
    }
    return it->second;
}

void LauncherSettings::Set(std::string_view key, std::string value) {
    pending_[std::string(key)] = std::move(value);
}

bool LauncherSettings::Save() {
    Values changes;
    for (const auto& [key, value] : pending_) {
        const auto saved = saved_.find(key);
        if (saved == saved_.end() || saved->second != value) {
            changes.emplace(key, value);
        }
    }
    if (changes.empty()) {
        return true;
    }
    if (!WriteFile(changes)) {
        return false;
    }
    for (const auto& [key, value] : changes) {
        saved_[key] = value;
    }
    return true;
}

bool LauncherSettings::SaveNow(const Values& values) {
    if (!WriteFile(values)) {
        return false;
    }
    for (const auto& [key, value] : values) {
        saved_[key] = value;
        pending_[key] = value;
    }
    return true;
}

void LauncherSettings::ResetToDefaults() {
    for (const ManagedSetting& setting : ManagedSettings()) {
        if (!IsLauncherPlacementSetting(setting.key) && !IsOnlineSetting(setting.key)) {
            pending_[std::string(setting.key)] = std::string(setting.default_value);
        }
    }
}

bool LauncherSettings::WriteFile(const Values& changes) {
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    if (!std::filesystem::exists(file_, ec)) {
        std::ofstream(file_) << kFileHeader;
    }
    trg::TextFileSettings file(Utf8(file_));
    for (const auto& [key, value] : changes) {
        file.Set(key, value);
    }
    return file.Save();
}

std::string FrameRateLabel(int fps) {
    if (fps == 0) {
        return "Unlimited";
    }
    if (fps == 60) {
        return "60 FPS (Original)";
    }
    return std::to_string(fps) + " FPS";
}

bool TurnCheatsOff(trg::SettingsStore& settings) {
    bool changed = false;
    for (const cheats::CheatInfo& cheat : cheats::kCheats) {
        if (settings.GetBool(cheat.key, cheats::kCheatDefault)) {
            settings.SetBool(cheat.key, false);
            changed = true;
        }
    }
    if (settings.GetBool(cheats::kLegacyMovementSpeedKey, false)) {
        settings.SetBool(cheats::kLegacyMovementSpeedKey, false);
        changed = true;
    }
    for (const char* key : {cheats::kMovementSpeedKey, cheats::kExperienceMultiplierKey}) {
        if (settings.Get(key, "1") != "1") {
            settings.Set(key, "1");
            changed = true;
        }
    }
    return changed;
}

bool NormalizeTextLanguage(trg::SettingsStore& settings, std::span<const TextLanguage> languages) {
    const std::string saved = settings.Get(kTextLanguageKey, languages.front().key);
    const std::string_view effective = ResolveTextLanguage(languages, saved).key;
    if (saved == effective) {
        return false;
    }
    settings.Set(kTextLanguageKey, std::string(effective));
    return true;
}

bool LauncherWanted(bool game_ready, bool show_setting, bool shift_held, bool skip_once) {
    if (!game_ready) {
        return true;
    }
    return !skip_once && (shift_held || show_setting);
}

} // namespace dcp

extern "C" int dcp_render_resolution(const char* settings_file, const char* image_dir) {
    using namespace dcp;
    if (RestrictedToNativeResolution(BuildOfImageDir(image_dir))) {
        return 0;
    }
    const LauncherSettings settings(PathFromUtf8(settings_file ? settings_file : ""));
    return dcp_resolution_index(settings.Get(kRenderResolutionKey, kNativeResolution).c_str());
}

extern "C" int dcp_aspect_ratio(const char* settings_file, const char* image_dir) {
    using namespace dcp;
    if (RestrictedToNativeResolution(BuildOfImageDir(image_dir))) {
        return 0;
    }
    const LauncherSettings settings(PathFromUtf8(settings_file ? settings_file : ""));
    return dcp_aspect_index(settings.Get(kAspectRatioKey, kDefaultAspectRatio).c_str());
}

extern "C" unsigned dcp_saved_cheats(const char* settings_file) {
    using namespace dcp;
    const LauncherSettings settings(PathFromUtf8(settings_file ? settings_file : ""));
    unsigned mask = 0;
    for (const cheats::CheatInfo& cheat : cheats::kCheats) {
        if (settings.GetBool(cheat.key, cheats::kCheatDefault)) {
            mask |= cheats::Bit(cheat.cheat);
        }
    }
    return mask;
}

extern "C" int dcp_saved_experience_multiplier(const char* settings_file) {
    using namespace dcp;
    const LauncherSettings settings(PathFromUtf8(settings_file ? settings_file : ""));
    return cheats::ExperienceMultiplierFromSaved(settings.Get(cheats::kExperienceMultiplierKey, "1"));
}

extern "C" int dcp_saved_movement_speed(const char* settings_file) {
    using namespace dcp;
    const LauncherSettings settings(PathFromUtf8(settings_file ? settings_file : ""));
    if (settings.Has(cheats::kMovementSpeedKey)) {
        return cheats::MovementSpeedFromSaved(settings.Get(cheats::kMovementSpeedKey));
    }
    return cheats::MovementSpeedFromLegacy(settings.Get(cheats::kLegacyMovementSpeedKey));
}

extern "C" int dcp_japanese_npc_voices(const char* settings_file) {
    using namespace dcp;
    if (!settings_file || !*settings_file) {
        return 0;
    }
    const LauncherSettings settings(PathFromUtf8(settings_file));
    return settings.GetBool(kJapaneseNpcVoicesKey, false) ? 1 : 0;
}

extern "C" int dcp_true_ultrawide(const char* settings_file) {
    using namespace dcp;
    if (!settings_file || !*settings_file) {
        return 0;
    }
    const LauncherSettings settings(PathFromUtf8(settings_file));
    return settings.GetBool(kTrueUltrawideKey, false) ? 1 : 0;
}

extern "C" unsigned dcp_skip_offers(const char* settings_file) {
    using namespace dcp;
    const LauncherSettings settings(PathFromUtf8(settings_file ? settings_file : ""));
    return (settings.GetBool(kStorySkipOfferKey, false) ? 1u : 0u) | (settings.GetBool(kTutorialSkipOfferKey, false) ? 2u : 0u);
}

extern "C" int dcp_detailed_logging(const char* settings_file) {
    using namespace dcp;
    if (!settings_file || !*settings_file) {
        return 1;
    }
    const LauncherSettings settings(PathFromUtf8(settings_file));
    return settings.GetBool(kDetailedLoggingKey, true) ? 1 : 0;
}

namespace dcp {
namespace {

const char* OnOff(bool on) { return on ? "on" : "off"; }

} // namespace

std::string SettingsSummary(const LauncherSettings& settings, const GameBuild* build,
                            std::span<const TextLanguage> languages, const DcpRunSettings* run) {
    std::string out;
    const auto line = [&out](const std::string& text) { out += "Settings: " + text + "\n"; };
    const char* build_label = run ? run->game_build : build ? build->label : nullptr;
    line("Dragon's Crown Pro PC v" + net::port_version() + ", " + net::host_platform() + ", online protocol " +
         std::to_string(net::kOnlineProtocolVersion) + ", game build " +
         (build_label ? build_label : "not a supported build"));

    if (!SameLanguages(languages, TextLanguagesOf(build))) {
        line(std::string("Menu & Subtitles languages in ") + build->regional_archive + ": " + LanguageNames(languages));
    }
    const std::string saved_language = settings.Get(kTextLanguageKey);
    const TextLanguage& language = ResolveTextLanguage(languages, saved_language);
    if (const std::string note = UnavailableLanguageNote(saved_language, language); !note.empty()) {
        line(note);
    }

    bool online_play = run && run->online_play;
    if (!run) {
        net::OnlineConfig online = ReadOnlineConfig(settings.file());
        net::apply_environment(online);
        online_play = online.enabled;
    }
    line(online_play ? std::string("Online Play on, Ping display ") + OnOff(settings.GetBool(kPingDisplayKey, false)) +
                           ", cheats locked for Online Play"
                     : std::string("Online Play off"));

    const std::string mode = settings.Get(kDisplayModeKey, "windowed");
    line(std::string("Window ") + (mode == "borderless" ? "Borderless Fullscreen" : "Windowed") + ", frame rate " +
         FrameRateLabel(std::atoi(settings.Get(kFrameRateKey, "60").c_str())) + ", frame counter " +
         OnOff(settings.GetBool(kShowFrameCounterKey, false)) + ", frame rate in title " +
         OnOff(settings.GetBool(kShowFpsTitleKey, false)) + ", render resolution in title " +
         OnOff(settings.GetBool(kShowResolutionTitleKey, false)));

    const bool ultrawide = settings.GetBool(kTrueUltrawideKey, false);
    if (run) {
        line("Render resolution " + std::to_string(run->render_width) + "x" + std::to_string(run->render_height) + " (" +
             run->render_resolution + "), aspect ratio " + run->aspect_ratio + ", True Ultrawide " +
             OnOff(run->true_ultrawide_effective) + (ultrawide && !run->true_ultrawide_effective ? " (requested on)" : ""));
    } else {
        line("Render resolution " + settings.Get(kRenderResolutionKey, kNativeResolution) + ", aspect ratio " +
             settings.Get(kAspectRatioKey, kDefaultAspectRatio) + ", True Ultrawide requested " + OnOff(ultrawide));
    }

    const bool voices_supported = SupportsJapaneseNpcVoices(build);
    line(std::string("Menu & Subtitles ") + std::string(language.name) + ", Japanese NPC Voices " +
         (voices_supported ? OnOff(settings.GetBool(kJapaneseNpcVoicesKey, false)) : "unsupported") +
         ", Arachnophobia Safe Mode " + OnOff(settings.GetBool(kArachnophobiaSafeModeKey, false)));

    const bool story = run ? (run->skip_offers & 1u) != 0 : settings.GetBool(kStorySkipOfferKey, false);
    const bool tutorial = run ? (run->skip_offers & 2u) != 0 : settings.GetBool(kTutorialSkipOfferKey, false);
    line(std::string("Story Skip Offer ") + OnOff(story) + ", Tutorial Skip Offer " + OnOff(tutorial) +
         ", overlay programs " +
         (kSeparateGameProcessAvailable ? OnOff(settings.GetBool(kAllowOverlayProgramsKey, false)) : "unsupported"));
    return out;
}

} // namespace dcp

extern "C" void dcp_log_settings_summary(const char* settings_file, const char* image_dir, const char* app0,
                                         const DcpRunSettings* run) {
    using namespace dcp;
    const LauncherSettings settings(PathFromUtf8(settings_file ? settings_file : ""));
    const GameBuild* build = BuildOfImageDir(image_dir);
    const std::vector<TextLanguage> languages = InstalledTextLanguages(PathFromUtf8(app0 ? app0 : ""), build);
    std::fputs(SettingsSummary(settings, build, languages, run).c_str(), stdout);
}

extern "C" int dcp_text_language(const char* settings_file, const char* image_dir, const char* app0) {
    using namespace dcp;
    const GameBuild* build = BuildOfImageDir(image_dir);
    const LauncherSettings settings(PathFromUtf8(settings_file ? settings_file : ""));
    const std::vector<TextLanguage> languages = InstalledTextLanguages(PathFromUtf8(app0 ? app0 : ""), build);
    return ResolveTextLanguage(languages, settings.Get(kTextLanguageKey)).ps4_id;
}
