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
inline constexpr char kPresentationKey[] = "presentation";
inline constexpr char kUpscaleKey[] = "fsr1";
inline constexpr char kSharpenKey[] = "rcas";
inline constexpr char kSharpenStrengthKey[] = "rcas_strength";
inline constexpr char kRenderResolutionKey[] = "render_resolution";
inline constexpr char kNativeResolution[] = "native";
inline constexpr char kAspectRatioKey[] = "aspect_ratio";
inline constexpr char kDefaultAspectRatio[] = "16:9";
// Widens the scene's horizontal view (Hor+) for the aspect ratios wider than 16:9 (dcp_true_ultrawide_scale).
inline constexpr char kTrueUltrawideKey[] = "true_ultrawide";
inline constexpr char kGraphicsCardKey[] = "graphics_card";
inline constexpr char kAutomaticGpu[] = "automatic";
inline constexpr char kAntiAliasingKey[] = "smaa";
inline constexpr char kAnisotropyKey[] = "anisotropy";
inline constexpr char kTextLanguageKey[] = "text_language";
inline constexpr char kShowFpsTitleKey[] = "show_fps_title";
inline constexpr char kShowResolutionTitleKey[] = "show_resolution_title";
inline constexpr char kShowFrameCounterKey[] = "show_fps";
// Keeps the run log of normal sessions; crash logs are kept whatever it says.
inline constexpr char kDetailedLoggingKey[] = "detailed_logging";
// Which controller's buttons the game's prompts show; the launcher keeps the installed archives in step with it.
inline constexpr char kControllerPromptsKey[] = "controller_prompts";
// Replaces the Giant Spider's pictures with a simplified creature; the launcher keeps the installed archive in step with it.
inline constexpr char kArachnophobiaSafeModeKey[] = "arachnophobia_safe_mode";
// Routes NPC voice cues to the Japanese banks already present in the USA installation.
inline constexpr char kJapaneseNpcVoicesKey[] = "dcp_japanese_npc_voices";
// Windows, on: Play starts the game as a separate process that overlay programs can attach to (game_process.h).
inline constexpr char kAllowOverlayProgramsKey[] = "allow_overlay_programs";
inline constexpr char kLauncherXKey[] = "launcher_x";
inline constexpr char kLauncherYKey[] = "launcher_y";
inline constexpr char kLauncherWidthKey[] = "launcher_width";
inline constexpr char kLauncherHeightKey[] = "launcher_height";
inline constexpr char kLauncherMaximizedKey[] = "launcher_maximized";
inline constexpr char kLauncherFullscreenKey[] = "launcher_fullscreen";

inline constexpr int kDefaultWindowWidth = 1920;
inline constexpr int kDefaultWindowHeight = 1080;

struct PresentationChoice {
    std::string_view key;
    std::string_view name;
};

// How the rendered frame fits the window or display, in launcher order; the first is the default.
inline constexpr PresentationChoice kPresentationChoices[] = {
    {"letterbox", "Letterbox"},
    {"stretch", "Stretch"},
};
inline constexpr char kDefaultPresentation[] = "letterbox";

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
    bool Has(std::string_view key) const { return pending_.find(key) != pending_.end(); }
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
