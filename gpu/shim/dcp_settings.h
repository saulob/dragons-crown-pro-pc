// SPDX-License-Identifier: GPL-2.0-or-later
// The video core's view of dcp.ini: read once at start, changed live by the in-game menu (dcp_overlay.h) and F11.
// Readers load the atomics every frame.

#pragma once

#include <atomic>
#include <string>

namespace DcpSettings {

struct Values {
    // Frame counter overlay: font size in percent and corner (0 top left, 1 top right, 2 bottom left, 3 bottom right).
    std::atomic<bool> show_fps{false};
    std::atomic<int> overlay_scale{100}, overlay_corner{1};
    std::atomic<int> menu_tab{0};
    std::atomic<int> display_mode{0};
    std::atomic<bool> vsync{true};
    std::atomic<bool> fsr1{false};
    std::atomic<bool> rcas{true};
    std::atomic<int> rcas_strength{75};
    std::atomic<bool> smaa{false};
    std::string graphics_card; ///< exact Vulkan device name; empty: Automatic. Set by Load() only
    std::atomic<int> anisotropy{0}; ///< 0: the game's own; 2, 4, 8 or 16 times
    // Set by the presenter: sharpening ran on the last frame, the game's image and the window it is shown in.
    std::atomic<bool> rcas_applied{false};
    std::atomic<int> image_width{0}, image_height{0}, window_width{0}, window_height{0};
    // The game window: size in pixels (0: the loader's size), 1-based monitor (dcp_displays.h), frame rate and render
    // resolution in the title.
    std::atomic<int> requested_width{0}, requested_height{0};
    std::atomic<int> monitor{1};
    std::atomic<bool> show_fps_title{false}, show_resolution_title{false};
};

Values& Get();

/// Reads the file into `values`; a missing file, key or invalid value keeps what `values` holds. False when the
/// file cannot be read.
bool Read(const std::string& path, Values& values);
/// Read() into Get() and the file Save() writes. Once, before the window and the renderer start.
void Load(const std::string& path);
/// Writes the settings the in-game menu changes; every other line of the file is kept.
bool Save();
/// Writes the display mode alone (F11).
bool SaveDisplayMode();

/// One "key = value" line as the launcher's settings file reads it; false for comments and other lines.
bool ParseLine(const std::string& line, std::string& key, std::string& value);
/// "1", "on", "yes" and "true" are true, "0", "off", "no" and "false" are false; anything else is `fallback`.
bool ParseBool(const std::string& value, bool fallback);

} // namespace DcpSettings
