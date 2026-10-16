// SPDX-License-Identifier: GPL-2.0-or-later
// The game window's title: the game's title, then the render resolution and the frame rate when they are shown.
#pragma once

#include <cstdio>
#include <string>
#include <string_view>

namespace DcpTitle {

struct Content {
    std::string_view base;
    bool show_resolution = false;
    int width = 0, height = 0; ///< the render resolution; 0 until a frame is presented
    bool show_fps = false;
    double fps = -1.0; ///< negative until measured
    std::string_view entry{}; ///< text being typed; it takes the place of the frame rate
};

inline std::string Compose(const Content& content) {
    std::string title{content.base};
    if (content.show_resolution && content.width > 0 && content.height > 0) {
        title += " | " + std::to_string(content.width) + "x" + std::to_string(content.height);
    }
    if (!content.entry.empty()) {
        title += content.entry;
    } else if (content.show_fps && content.fps >= 0.0) {
        char digits[32];
        std::snprintf(digits, sizeof(digits), "%.2f", content.fps);
        title += std::string(" | FPS: ") + digits;
    }
    return title;
}

} // namespace DcpTitle
