// What the Display page offers and how it labels it.
#pragma once

#include <string>
#include <vector>

#include "dcp_displays.h"
#include "dcp_resolution.h"

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

// "Key: Action" lines of the Controls page: the keyboard layout runtime_pad.c implements.
struct KeyBinding {
    const char* action;
    const char* keys;
};
const std::vector<KeyBinding>& KeyboardBindings();

} // namespace dcp
