// What the Display page offers and how it labels it.
#pragma once

#include <string>
#include <vector>

#include "dcp_displays.h"
#include "dcp_keys.h"
#include "dcp_resolution.h"
#include "trg/settings.h"

namespace dcp {

struct WindowSize {
    int width = 0;
    int height = 0;
    bool operator==(const WindowSize&) const = default;
};

// In pixels. The game's own picture is 1920 x 1080.
inline constexpr int kWindowHeights[] = {720, 900, 1080, 1440, 2160};
inline constexpr WindowSize kGamePictureSize{1920, 1080};

WindowSize WindowSizeOf(int height, const DcpAspectRatio& aspect);

std::vector<WindowSize> WindowSizesFor(const DcpAspectRatio& aspect);

enum class PageId { kPlay, kDisplay, kGraphics, kGameplay, kCheats, kControls, kOnline, kTrophies, kEditor, kBugReport, kAbout };

inline constexpr PageId kPageOrder[] = {PageId::kPlay,   PageId::kDisplay,  PageId::kGraphics, PageId::kGameplay,
                                        PageId::kEditor, PageId::kOnline,   PageId::kCheats,   PageId::kControls,
                                        PageId::kTrophies, PageId::kBugReport, PageId::kAbout};

const char* PageName(PageId page);

// "Native (1080p) — 1920 × 1080".
std::string ResolutionLabel(const DcpResolution& resolution);

// "1920 × 1080", with a note on the game's own picture size.
std::string WindowSizeLabel(WindowSize size);

// "Display 2 (primary)   2560 × 1440   Name". Monitor settings are 1-based.
std::string MonitorLabel(int monitor, const std::vector<Display>& displays);

// The Controls page's bindings, stored one key name per action in the settings (dcp_keys.h).
// "Space", "Left Shift", "Enter", "F10".
std::string KeyDisplayName(int scancode);
// The saved key of an action; the default when the setting is missing or names no usable key.
int SavedKey(const trg::SettingsStore& settings, DcpKeyAction action);
void SetKey(trg::SettingsStore& settings, DcpKeyAction action, int scancode);
// Puts every keyboard control and port hotkey back to its default and nothing else.
void ResetKeys(trg::SettingsStore& settings);

// The mouse bindings, wheel steps and Pointer Control sensitivity saved in the settings (dcp_keys.h). A missing or unusable
// value reads as its default.
DcpMouseMap SavedMouse(const trg::SettingsStore& settings);
// Binds a mouse button (DCP_MOUSE_NONE clears the binding). False, and nothing saved, when the binding would leave Pointer
// Control and its click on one button or the click without a button.
bool SetMouseButton(trg::SettingsStore& settings, DcpMouseAction action, int button);
// Gives a mouse button the one PS4 control `target` (-1: none), taking it from the button that had it. `moved_from` is that
// button (DCP_MOUSE_NONE when the control had none). False, and nothing saved, for a button Pointer Control owns.
struct MouseTargetResult {
    bool applied = false;
    int moved_from = DCP_MOUSE_NONE;
};
MouseTargetResult SetMouseTarget(trg::SettingsStore& settings, int button, int target);
void SetPointerMode(trg::SettingsStore& settings, int mode);
void SetWheelTarget(trg::SettingsStore& settings, bool up, int target);
void SetPointerSensitivity(trg::SettingsStore& settings, int percent);
// The Mouse Sensitivity slider moves in steps: index 0 is the minimum.
inline constexpr int kSensitivityStepCount =
    (DCP_POINTER_SENSITIVITY_MAX - DCP_POINTER_SENSITIVITY_MIN) / DCP_POINTER_SENSITIVITY_STEP + 1;
int SensitivityStepOf(int percent);
int SensitivityOfStep(int step);
// Puts the mouse bindings, wheel steps and sensitivity back to their defaults and nothing else.
void ResetMouse(trg::SettingsStore& settings);

} // namespace dcp
