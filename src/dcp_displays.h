// The displays as the Monitor setting numbers them (1-based): the primary display first, then SDL's order.
// The launcher's Monitor row and the game window both use this list.
#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <string>
#include <vector>

namespace dcp {

struct Display {
    SDL_DisplayID id = 0;
    std::string name;
    SDL_Rect bounds{};
    SDL_Rect usable{};
    float scale = 1.0f;
    bool primary = false;
};

// Needs the SDL video subsystem.
inline std::vector<Display> ListDisplays() {
    std::vector<Display> displays;
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    for (int i = 0; ids && i < count; ++i) {
        Display display;
        display.id = ids[i];
        const char* name = SDL_GetDisplayName(ids[i]);
        display.name = name ? name : "";
        SDL_GetDisplayBounds(ids[i], &display.bounds);
        if (!SDL_GetDisplayUsableBounds(ids[i], &display.usable)) {
            display.usable = display.bounds;
        }
        const float scale = SDL_GetDisplayContentScale(ids[i]);
        display.scale = scale > 0.0f ? scale : 1.0f;
        display.primary = ids[i] == primary;
        displays.push_back(std::move(display));
    }
    SDL_free(ids);
    std::stable_partition(displays.begin(), displays.end(), [](const Display& d) { return d.primary; });
    return displays;
}

// Settings 0 and out-of-range values name the primary display.
inline const Display* DisplayForSetting(const std::vector<Display>& displays, int monitor) {
    if (displays.empty()) {
        return nullptr;
    }
    if (monitor >= 1 && monitor <= static_cast<int>(displays.size())) {
        return &displays[static_cast<size_t>(monitor - 1)];
    }
    return &displays.front();
}

// The launcher window stays within this share of the display's usable area, in pixels.
inline constexpr double kWindowDisplayShare = 0.9;

inline void FitWindowSize(int& width, int& height, const Display& display) {
    if (display.usable.w <= 0 || display.usable.h <= 0) {
        return;
    }
    width = std::min(width, static_cast<int>(display.usable.w * kWindowDisplayShare));
    height = std::min(height, static_cast<int>(display.usable.h * kWindowDisplayShare));
}

} // namespace dcp
