// SPDX-License-Identifier: GPL-2.0-or-later
// Which renderer runs the game: the Renderer setting as dcp.ini stores it, and how it resolves.
#pragma once

#include <array>
#include <atomic>
#include <string>
#include <string_view>

namespace DcpRenderer {

enum Choice : int { Vulkan = 0, Auto = 1, OpenGL = 2, ChoiceCount };
inline constexpr std::array<std::string_view, ChoiceCount> ChoiceKeys = {"vulkan", "auto", "opengl"};

/// An unknown value keeps the renderer that has always run the game.
inline Choice ParseChoice(std::string_view text) {
    for (std::size_t i = 0; i < ChoiceKeys.size(); ++i) {
        if (ChoiceKeys[i] == text) {
            return Choice(i);
        }
    }
    return Vulkan;
}

enum class Backend { Vulkan, OpenGL };

inline const char* BackendName(Backend backend) {
    return backend == Backend::OpenGL ? "OpenGL" : "Vulkan";
}

struct Resolution {
    Backend backend;
    std::string fallback_reason; ///< empty unless Auto left Vulkan
};

/// Only Auto falls back: a renderer chosen by name starts or fails as itself.
inline Resolution Resolve(Choice choice, bool vulkan_usable, std::string_view vulkan_problem) {
    if (choice == OpenGL) {
        return {Backend::OpenGL, {}};
    }
    if (choice == Auto && !vulkan_usable) {
        return {Backend::OpenGL, std::string{vulkan_problem}};
    }
    return {Backend::Vulkan, {}};
}

inline std::atomic<Backend> active_backend{Backend::Vulkan};

inline Backend Active() {
    return active_backend.load(std::memory_order_relaxed);
}

inline void SetActive(Backend backend) {
    active_backend.store(backend, std::memory_order_relaxed);
}

} // namespace DcpRenderer
