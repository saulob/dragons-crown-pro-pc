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
        return std::any_of(builds.begin(), builds.end(), [&](const GameBuild& build) {
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
        if (!IsLauncherPlacementSetting(setting.key)) {
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

extern "C" int dcp_detailed_logging(const char* settings_file) {
    using namespace dcp;
    if (!settings_file || !*settings_file) {
        return 1;
    }
    const LauncherSettings settings(PathFromUtf8(settings_file));
    return settings.GetBool(kDetailedLoggingKey, true) ? 1 : 0;
}

extern "C" void dcp_log_gameplay_settings(const char* settings_file, const char* image_dir) {
    using namespace dcp;
    const LauncherSettings settings(PathFromUtf8(settings_file ? settings_file : ""));
    const GameBuild* build = BuildOfImageDir(image_dir);
    const TextLanguage& language = ResolveTextLanguage(build, settings.Get(kTextLanguageKey));
    std::printf("Gameplay: Menu & Subtitles %.*s\n", static_cast<int>(language.name.size()), language.name.data());
    std::printf("Gameplay: Frame rate %s\n",
                FrameRateLabel(std::atoi(settings.Get(kFrameRateKey, "60").c_str())).c_str());
    std::printf("Gameplay: Frame rate in title %s\n", settings.GetBool(kShowFpsTitleKey, false) ? "on" : "off");
    std::printf("Gameplay: Render resolution in title %s\n",
                settings.GetBool(kShowResolutionTitleKey, false) ? "on" : "off");
    std::printf("Gameplay: Frame counter %s\n", settings.GetBool(kShowFrameCounterKey, false) ? "on" : "off");
    std::printf("Gameplay: Arachnophobia Safe Mode %s\n",
                settings.GetBool(kArachnophobiaSafeModeKey, false) ? "on" : "off");
    const bool supported = SupportsJapaneseNpcVoices(build);
    const bool voices = supported && settings.GetBool(kJapaneseNpcVoicesKey, false);
    std::printf("Gameplay: Japanese NPC Voices %s\n", supported ? (voices ? "on" : "off") : "unsupported");
    std::printf("Gameplay: Allow overlay programs %s\n",
                settings.GetBool(kAllowOverlayProgramsKey, false) ? "on" : "off");
}

extern "C" int dcp_text_language(const char* settings_file, const char* image_dir) {
    using namespace dcp;
    const GameBuild* build = BuildOfImageDir(image_dir);
    const LauncherSettings settings(PathFromUtf8(settings_file ? settings_file : ""));
    return ResolveTextLanguage(build, settings.Get(kTextLanguageKey)).ps4_id;
}
