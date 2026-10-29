// SPDX-License-Identifier: GPL-2.0-or-later
// Swapchain and flip pacing choices that need no Vulkan device (tests/present_tests.cpp).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace DcpPresent {

#ifdef _WIN32
inline constexpr bool kWindows = true;
#else
inline constexpr bool kWindows = false;
#endif

/// Values of VkPresentModeKHR.
enum class Mode : int { Immediate = 0, Mailbox = 1, Fifo = 2 };

/// VSync on is Fifo (no tearing, waits for the display); off is Mailbox when the surface has it, else Immediate
/// (tearing allowed), else Fifo.
inline Mode ChooseMode(bool vsync, bool has_mailbox, bool has_immediate) {
    if (vsync) {
        return Mode::Fifo;
    }
    if (has_mailbox) {
        return Mode::Mailbox;
    }
    return has_immediate ? Mode::Immediate : Mode::Fifo;
}

/// Gamescope (Steam Deck Gaming Mode) sets GAMESCOPE_WAYLAND_DISPLAY for the processes it runs.
inline bool IsGamescope(const char* gamescope_wayland_display, bool windows) {
    return !windows && gamescope_wayland_display && *gamescope_wayland_display;
}

/// This process runs under Gamescope: the only switch for the low-latency swapchain and flip pacing.
inline bool GamescopeSession() {
    static const bool session = IsGamescope(std::getenv("GAMESCOPE_WAYLAND_DISPLAY"), kWindows);
    return session;
}

/// One image beyond the surface minimum to work on. Gamescope queues frames in its compositor and already reports a
/// 3-image minimum, so there the extra image only adds a frame of latency; never a single image.
inline std::uint32_t ImageCount(std::uint32_t surface_min, std::uint32_t surface_max, bool gamescope) {
    std::uint32_t count = gamescope ? std::max(surface_min, 2u) : surface_min + 1;
    if (surface_max > 0) {
        count = std::min(count, surface_max);
    }
    return count;
}

/// Linux session facts that change presentation; values that look like paths are not shown.
template <typename GetEnv>
std::string DescribeSession(bool gamescope, GetEnv get_env) {
    static constexpr const char* names[] = {
        "GAMESCOPE_WAYLAND_DISPLAY", "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE", "SteamDeck", "SteamGamepadUI",
        "ENABLE_GAMESCOPE_WSI", "DISABLE_GAMESCOPE_WSI", "GAMESCOPE_WSI_MIN_IMAGE_COUNT",
        "GAMESCOPE_WSI_ENSURE_MIN_IMAGE_COUNT", "GAMESCOPE_WSI_FRAME_LIMITER_AWARE", "GAMESCOPE_LIMITER_FILE",
        "MESA_VK_WSI_PRESENT_MODE", "vk_x11_override_min_image_count", "vk_xwayland_wait_ready", "MANGOHUD",
        "LD_PRELOAD", "VK_INSTANCE_LAYERS", "VK_LOADER_LAYERS_ENABLE", "VK_LOADER_LAYERS_DISABLE", "OBS_VKCAPTURE",
    };
    std::string text = gamescope ? "Gamescope yes (flip pacing on)" : "Gamescope no";
    for (const char* name : names) {
        const char* value = get_env(name);
        if (!value) {
            continue;
        }
        const bool hidden = std::strpbrk(value, "/\\") || std::strlen(value) > 40;
        text += std::string(", ") + name + "=" + (hidden ? "(set)" : value);
    }
    return text;
}

} // namespace DcpPresent
