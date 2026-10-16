// SPDX-License-Identifier: GPL-2.0-or-later
#include "dcp_settings.h"
#include "dcp_graphics.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace DcpSettings {

namespace {

std::filesystem::path file_path;
std::mutex file_mutex; // the menu (present thread) and F11 (window thread) both write

constexpr const char* FileHeader[] = {
    "# Dragon's Crown Pro user settings (the launcher and the in-game menu write this file).",
    "# Remove a line to use the default.",
};

std::filesystem::path PathFromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string Utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

std::string Trim(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? std::string{}
                                      : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

int ParseInt(const std::string& value, int fallback) {
    char* end = nullptr;
    const long number = std::strtol(value.c_str(), &end, 10);
    return end == value.c_str() ? fallback : int(std::clamp<long>(number, INT_MIN, INT_MAX));
}

void Set(Values& v, const std::string& key, const std::string& value) {
    if (key == "show_fps") {
        v.show_fps = ParseBool(value, v.show_fps);
    } else if (key == "overlay_scale") {
        v.overlay_scale = std::clamp(ParseInt(value, 100), 50, 300);
    } else if (key == "overlay_corner") {
        const int corner = ParseInt(value, 1);
        v.overlay_corner = corner >= 0 && corner <= 3 ? corner : 1;
    } else if (key == "menu_tab") {
        const int tab = ParseInt(value, 0);
        v.menu_tab = tab >= 0 && tab <= 1 ? tab : 0;
    } else if (key == "display_mode") {
        v.display_mode = DcpGraphics::ParseDisplayMode(value);
    } else if (key == "vsync") {
        v.vsync = ParseBool(value, v.vsync);
    } else if (key == "fsr1") {
        v.fsr1 = ParseBool(value, v.fsr1);
    } else if (key == "rcas") {
        v.rcas = ParseBool(value, v.rcas);
    } else if (key == "rcas_strength") {
        v.rcas_strength = DcpGraphics::ParseRcasStrength(ParseInt(value, -1));
    } else if (key == "smaa") {
        v.smaa = ParseBool(value, v.smaa);
    } else if (key == "graphics_card") {
        v.graphics_card = value == "automatic" ? std::string() : value;
    } else if (key == "anisotropy") {
        v.anisotropy = DcpGraphics::ParseAnisotropy(ParseInt(value, 0));
    } else if (key == "window_width") {
        v.requested_width = std::clamp(ParseInt(value, 0), 0, 16384);
    } else if (key == "window_height") {
        v.requested_height = std::clamp(ParseInt(value, 0), 0, 16384);
    } else if (key == "monitor") {
        v.monitor = std::max(ParseInt(value, 1), 1);
    } else if (key == "show_fps_title") {
        v.show_fps_title = ParseBool(value, v.show_fps_title);
    } else if (key == "show_resolution_title") {
        v.show_resolution_title = ParseBool(value, v.show_resolution_title);
    }
}

using Entries = std::vector<std::pair<std::string, std::string>>;

// Every line holding one of `entries` is rewritten in place and missing ones are appended; all other lines stay.
bool WriteEntries(const Entries& entries) {
    std::scoped_lock lock{file_mutex};
    if (file_path.empty()) {
        return false;
    }
    std::vector<std::string> lines;
    if (std::ifstream in{file_path}) {
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            lines.push_back(std::move(line));
        }
    } else {
        lines.assign(std::begin(FileHeader), std::end(FileHeader));
    }
    std::vector<bool> written(entries.size(), false);
    for (std::string& line : lines) {
        std::string key, value;
        if (!ParseLine(line, key, value)) {
            continue;
        }
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].first == key) {
                line = key + " = " + entries[i].second;
                written[i] = true;
            }
        }
    }
    bool header = false;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (written[i]) {
            continue;
        }
        if (!header) {
            if (!lines.empty() && !Trim(lines.back()).empty()) {
                lines.emplace_back();
            }
            lines.emplace_back("# Set from the in-game menu");
            header = true;
        }
        lines.push_back(entries[i].first + " = " + entries[i].second);
    }
    std::error_code ec;
    if (file_path.has_parent_path()) {
        std::filesystem::create_directories(file_path.parent_path(), ec);
    }
    std::filesystem::path temporary = file_path;
    temporary += ".tmp";
    {
        std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
        for (const std::string& line : lines) {
            out << line << '\n';
        }
        if (!out.flush()) {
            std::printf("Settings: cannot write %s\n", Utf8(file_path).c_str());
            return false;
        }
    }
    std::filesystem::rename(temporary, file_path, ec);
    if (ec) {
        std::printf("Settings: cannot replace %s: %s\n", Utf8(file_path).c_str(), ec.message().c_str());
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

std::string DisplayModeKey() {
    return std::string(DcpGraphics::DisplayModeKeys[std::clamp(Get().display_mode.load(), 0,
                                                               int(DcpGraphics::DisplayModeCount) - 1)]);
}

} // namespace

bool ParseLine(const std::string& line, std::string& key, std::string& value) {
    const std::string text = Trim(line);
    const size_t eq = text.find('=');
    if (text.empty() || text[0] == '#' || eq == std::string::npos) {
        return false;
    }
    key = Trim(text.substr(0, eq));
    if (key.empty() || key.find_first_of(" \t") != std::string::npos) {
        return false;
    }
    value = Trim(text.substr(eq + 1));
    if (const size_t comment = value.find(" #"); comment != std::string::npos) {
        value = Trim(value.substr(0, comment));
    }
    return true;
}

bool ParseBool(const std::string& value, bool fallback) {
    if (value == "1" || value == "on" || value == "yes" || value == "true") {
        return true;
    }
    if (value == "0" || value == "off" || value == "no" || value == "false") {
        return false;
    }
    return fallback;
}

Values& Get() {
    static Values values;
    return values;
}

bool Read(const std::string& path, Values& values) {
    std::ifstream in{PathFromUtf8(path)};
    if (!in) {
        return false;
    }
    for (std::string line; std::getline(in, line);) {
        std::string key, value;
        if (ParseLine(line, key, value)) {
            Set(values, key, value);
        }
    }
    return true;
}

void Load(const std::string& path) {
    auto& v = Get();
    {
        std::scoped_lock lock{file_mutex};
        file_path = PathFromUtf8(path);
    }
    if (!Read(path, v)) {
        std::printf("Settings: %s not found, using the defaults\n", path.c_str());
        return;
    }
    std::printf("Settings: %s\n", path.c_str());
    if (v.smaa) {
        std::printf("GPU: anti-aliasing SMAA\n");
    }
    if (v.anisotropy) {
        std::printf("GPU: texture filtering %dx\n", v.anisotropy.load());
    }
}

bool Save() {
    const auto& v = Get();
    const auto flag = [](bool value) { return std::string(value ? "1" : "0"); };
    return WriteEntries({
        {"display_mode", DisplayModeKey()},
        {"vsync", flag(v.vsync)},
        {"fsr1", flag(v.fsr1)},
        {"rcas", flag(v.rcas)},
        {"rcas_strength", std::to_string(v.rcas_strength.load())},
        {"show_fps", flag(v.show_fps)},
        {"overlay_scale", std::to_string(v.overlay_scale.load())},
        {"overlay_corner", std::to_string(v.overlay_corner.load())},
        {"menu_tab", std::to_string(v.menu_tab.load())},
    });
}

bool SaveDisplayMode() {
    return WriteEntries({{"display_mode", DisplayModeKey()}});
}

} // namespace DcpSettings
