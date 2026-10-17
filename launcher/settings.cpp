#include "settings.h"

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <system_error>
#include <utility>

#include "dcp_launcher.h"
#include "dcp_resolution.h"
#include "utf8.h"

namespace dcp {
namespace {

constexpr ManagedSetting kManagedSettings[] = {
    {kShowLauncherKey, "1"},
    {kDisplayModeKey, "windowed"},
    {kWindowWidthKey, "1920"},
    {kWindowHeightKey, "1080"},
    {kMonitorKey, "1"},
    {kVSyncKey, "1"},
    {kUpscaleKey, "0"},
    {kSharpenKey, "1"},
    {kSharpenStrengthKey, "75"},
    {kRenderResolutionKey, kNativeResolution},
    {kAspectRatioKey, kDefaultAspectRatio},
    {kGraphicsCardKey, kAutomaticGpu},
    {kAntiAliasingKey, "0"},
    {kAnisotropyKey, "0"},
    {kTextLanguageKey, "english"},
    {kShowFpsTitleKey, "0"},
    {kShowResolutionTitleKey, "0"},
    {kShowFrameCounterKey, "0"},
    {kControllerPromptsKey, "playstation"},
    {cheats::kInfiniteHealthKey, "0"},
    {cheats::kInfiniteManaKey, "0"},
    {cheats::kInfiniteLivesKey, "0"},
    {cheats::kInfiniteDurabilityKey, "0"},
    {cheats::kInfiniteArrowsKey, "0"},
    {cheats::kInfiniteSkillPointsKey, "0"},
    {cheats::kInfiniteGoldKey, "0"},
    {cheats::kMovementSpeedKey, "0"},
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
    return kManagedSettings;
}

bool IsValidSetting(std::string_view key, std::string_view value) {
    const auto one_of = [&](std::initializer_list<std::string_view> values) {
        return std::find(values.begin(), values.end(), value) != values.end();
    };
    if (key == kDisplayModeKey) {
        return one_of({"windowed", "borderless"});
    }
    if (key == kAnisotropyKey) {
        return one_of({"0", "2", "4", "8", "16"});
    }
    if (key == kTextLanguageKey) {
        return std::any_of(std::begin(kTextLanguages), std::end(kTextLanguages),
                           [&](const TextLanguage& language) { return value == language.key; });
    }
    if (key == kControllerPromptsKey) {
        return one_of({"playstation", "xbox"});
    }
    if (key == cheats::kExperienceMultiplierKey) {
        return std::any_of(std::begin(cheats::kExperienceChoices), std::end(cheats::kExperienceChoices),
                           [&](const cheats::ExperienceChoice& choice) { return value == choice.saved; });
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

int TextLanguageId(std::string_view key) {
    for (const TextLanguage& language : kTextLanguages) {
        if (language.key == key) {
            return language.ps4_id;
        }
    }
    return kTextLanguages[0].ps4_id;
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
    for (const ManagedSetting& setting : kManagedSettings) {
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
    for (const ManagedSetting& setting : kManagedSettings) {
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

bool LauncherWanted(bool game_ready, bool show_setting, bool shift_held, bool skip_once) {
    if (!game_ready) {
        return true;
    }
    return !skip_once && (shift_held || show_setting);
}

} // namespace dcp

extern "C" int dcp_render_resolution(const char* settings_file) {
    using namespace dcp;
    const LauncherSettings settings(PathFromUtf8(settings_file ? settings_file : ""));
    return dcp_resolution_index(settings.Get(kRenderResolutionKey, kNativeResolution).c_str());
}

extern "C" int dcp_aspect_ratio(const char* settings_file) {
    using namespace dcp;
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

extern "C" int dcp_text_language(const char* settings_file) {
    using namespace dcp;
    const LauncherSettings settings(PathFromUtf8(settings_file ? settings_file : ""));
    return TextLanguageId(settings.Get(kTextLanguageKey, kTextLanguages[0].key));
}
