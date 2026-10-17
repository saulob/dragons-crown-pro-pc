// The launcher's view of user\dcp.ini, the file the video core reads at start and its in-game menu writes.
// The video core reads it the same way (gpu/shim/dcp_settings.cpp); booleans are written as 1/0.
#pragma once

#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>

#include "dcp_cheats.h"
#include "trg/settings.h"

namespace dcp {

inline constexpr char kShowLauncherKey[] = "show_launcher";
inline constexpr char kDisplayModeKey[] = "display_mode";
inline constexpr char kWindowWidthKey[] = "window_width";
inline constexpr char kWindowHeightKey[] = "window_height";
inline constexpr char kMonitorKey[] = "monitor";
inline constexpr char kVSyncKey[] = "vsync";
inline constexpr char kUpscaleKey[] = "fsr1";
inline constexpr char kSharpenKey[] = "rcas";
inline constexpr char kSharpenStrengthKey[] = "rcas_strength";
inline constexpr char kRenderResolutionKey[] = "render_resolution";
inline constexpr char kNativeResolution[] = "native";
inline constexpr char kAspectRatioKey[] = "aspect_ratio";
inline constexpr char kDefaultAspectRatio[] = "16:9";
inline constexpr char kGraphicsCardKey[] = "graphics_card";
inline constexpr char kAutomaticGpu[] = "automatic";
inline constexpr char kAntiAliasingKey[] = "smaa";
inline constexpr char kAnisotropyKey[] = "anisotropy";
inline constexpr char kTextLanguageKey[] = "text_language";
inline constexpr char kShowFpsTitleKey[] = "show_fps_title";
inline constexpr char kShowResolutionTitleKey[] = "show_resolution_title";
inline constexpr char kShowFrameCounterKey[] = "show_fps";
// Which controller's buttons the game's prompts show; the launcher keeps the installed archives in step with it.
inline constexpr char kControllerPromptsKey[] = "controller_prompts";
inline constexpr char kLauncherXKey[] = "launcher_x";
inline constexpr char kLauncherYKey[] = "launcher_y";
inline constexpr char kLauncherWidthKey[] = "launcher_width";
inline constexpr char kLauncherHeightKey[] = "launcher_height";
inline constexpr char kLauncherMaximizedKey[] = "launcher_maximized";
inline constexpr char kLauncherFullscreenKey[] = "launcher_fullscreen";

inline constexpr int kDefaultWindowWidth = 1920;
inline constexpr int kDefaultWindowHeight = 1080;

struct TextLanguage {
    std::string_view key;
    std::string_view name;
    int ps4_id;
};

// The languages the game has text for, in launcher order; the first is the default.
inline constexpr TextLanguage kTextLanguages[] = {
    {"english", "English", 1}, {"french", "French", 2}, {"german", "German", 4},
    {"italian", "Italian", 5}, {"spanish", "Spanish", 3},
};

// The system language the game is told for a text_language value; English when the value is not listed.
int TextLanguageId(std::string_view key);

using Values = std::map<std::string, std::string, std::less<>>;

struct ManagedSetting {
    std::string_view key;
    std::string_view default_value;
};

// Every setting the launcher stores, with the value the game uses when the file does not set it.
std::span<const ManagedSetting> ManagedSettings();

// The launcher window's own placement, kept apart from the game's window settings and from Reset.
bool IsLauncherPlacementSetting(std::string_view key);

// False for a value the game would not accept for the key (it uses the default then, and so does the launcher).
bool IsValidSetting(std::string_view key, std::string_view value);

// Pending launcher edits over the saved values. Nothing is written until Save() or SaveNow(); lines of the file
// the launcher does not manage (the in-game menu's keys, comments) are kept as they are.
class LauncherSettings final : public trg::SettingsStore {
public:
    explicit LauncherSettings(std::filesystem::path file);

    std::string Get(std::string_view key, std::string_view def = {}) const override;
    void Set(std::string_view key, std::string value) override;
    bool Save() override;
    void ResetToDefaults() override;

    // Writes some settings now and leaves the other pending edits pending.
    bool SaveNow(const Values& values);

    const std::filesystem::path& file() const { return file_; }

private:
    bool WriteFile(const Values& changes);

    std::filesystem::path file_;
    Values saved_;
    Values pending_;
};

// The launcher opens when the game files are not ready, or when the setting is On or Shift is held, unless it was
// skipped once. A game that is not ready always shows it: it is where the files are installed.
bool LauncherWanted(bool game_ready, bool show_setting, bool shift_held, bool skip_once);

} // namespace dcp
