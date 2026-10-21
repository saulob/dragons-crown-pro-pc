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

enum class PageId { kPlay, kDisplay, kGraphics, kGameplay, kCheats, kControls, kTrophies, kAbout };

inline constexpr PageId kPageOrder[] = {PageId::kPlay,   PageId::kDisplay, PageId::kGraphics, PageId::kGameplay,
                                        PageId::kCheats, PageId::kControls, PageId::kTrophies,
                                        PageId::kAbout};

const char* PageName(PageId page);

// The window a requested size becomes on this display: unchanged when it fits, else shrunk to fit, keeping its shape.
WindowSize FitGameWindow(WindowSize requested, const Display& display);

// Whether the size fits the display, so the game window is not larger than the screen.
bool WindowSizeFits(WindowSize size, const Display& display);

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

} // namespace dcp
