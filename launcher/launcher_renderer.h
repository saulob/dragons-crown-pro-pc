#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dcp {

inline constexpr std::string_view kLauncherRendererAuto = "auto";
inline constexpr std::string_view kLauncherRendererSoftware = "software";
inline constexpr std::string_view kLauncherRendererDirect3D11 = "direct3d11";

// The --launcher-renderer values: Direct3D 11 exists on Windows only.
inline std::optional<std::string_view> ParseLauncherRenderer(std::string_view value, bool windows) {
    if (value == kLauncherRendererAuto || value == kLauncherRendererSoftware) {
        return value;
    }
    if (windows && value == kLauncherRendererDirect3D11) {
        return value;
    }
    return std::nullopt;
}

// SDL renderer names to try in order; an empty name lets SDL pick its first working renderer. A renderer asked for by
// name is the only one tried.
inline std::vector<std::string_view> LauncherRendererOrder(bool windows, std::string_view request = kLauncherRendererAuto) {
    if (request == kLauncherRendererSoftware) {
        return {kLauncherRendererSoftware};
    }
    if (windows && request == kLauncherRendererDirect3D11) {
        return {kLauncherRendererDirect3D11};
    }
    if (windows) {
        return {kLauncherRendererDirect3D11, kLauncherRendererSoftware};
    }
    return {"", kLauncherRendererSoftware};
}

// System modules a renderer loads that must stay mapped until the process exits. SDL unloads d3d11.dll right after the
// device's last Release, while an NVIDIA driver thread can still be returning through it (issue #45).
inline std::vector<std::string_view> RendererModulesToPin(std::string_view renderer) {
    if (renderer == "direct3d11") {
        return {"d3d11.dll", "dxgi.dll"};
    }
    return {};
}

enum class RendererStage { kPin, kCreate, kProbe, kBackend };

inline const char* RendererStageName(RendererStage stage) {
    switch (stage) {
    case RendererStage::kPin: return "pin";
    case RendererStage::kCreate: return "create";
    case RendererStage::kProbe: return "first_frame";
    case RendererStage::kBackend: return "imgui_backend";
    }
    return "unknown";
}

struct RendererFailure {
    RendererStage stage = RendererStage::kCreate;
    std::string reason;
};

struct RendererAttempt {
    std::string name;
    std::optional<RendererFailure> failure;
};

struct RendererSelection {
    std::string requested;
    std::vector<RendererAttempt> attempts;
    std::string effective;
    bool opened = false;
    bool fallback = false;
};

inline std::string AttemptLabel(std::string_view name) {
    return name.empty() ? "accelerated" : std::string(name);
}

// Tries the renderers of the order for `requested` until `open` accepts one. A failed attempt is followed by `discard`:
// whatever it left open must be gone before the next one starts.
inline RendererSelection SelectLauncherRenderer(std::string_view requested, bool windows,
                                                const std::function<std::optional<RendererFailure>(std::string_view)>& open,
                                                const std::function<void()>& discard) {
    RendererSelection selection;
    selection.requested = std::string(requested);
    const std::vector<std::string_view> order = LauncherRendererOrder(windows, requested);
    for (size_t i = 0; i < order.size(); ++i) {
        RendererAttempt attempt{std::string(order[i]), open(order[i])};
        const bool failed = attempt.failure.has_value();
        selection.attempts.push_back(std::move(attempt));
        if (!failed) {
            selection.effective = AttemptLabel(order[i]);
            selection.opened = true;
            selection.fallback = i > 0;
            return selection;
        }
        discard();
    }
    return selection;
}

inline std::string RendererRequestLine(std::string_view requested, bool windows) {
    std::string order;
    for (const std::string_view name : LauncherRendererOrder(windows, requested)) {
        order += (order.empty() ? "" : ",") + AttemptLabel(name);
    }
    return "Launcher: startup: renderer requested=" + std::string(requested) + " order=" + order;
}

inline std::string RendererAttemptLine(const RendererAttempt& attempt) {
    std::string line = "Launcher: startup: renderer attempt=" + AttemptLabel(attempt.name);
    if (!attempt.failure) {
        return line + " result=ok";
    }
    return line + " result=failed stage=" + RendererStageName(attempt.failure->stage) + " reason=" + attempt.failure->reason;
}

inline std::string RendererResultLine(const RendererSelection& selection) {
    if (!selection.opened) {
        return "Launcher: startup: renderer effective=none";
    }
    std::string line = "Launcher: startup: renderer effective=" + selection.effective + " fallback=" + (selection.fallback ? "1" : "0");
    if (selection.fallback) {
        const RendererAttempt& first = selection.attempts.front();
        line += " fallback_reason=" + AttemptLabel(first.name) + "_" + RendererStageName(first.failure->stage);
    }
    return line;
}

// Which part of the launcher's own startup failed, for the log.
enum class StartupFailure { kSdlVideo, kNoDisplay, kWindow, kRenderer };

inline const char* StartupFailureName(StartupFailure failure) {
    switch (failure) {
    case StartupFailure::kSdlVideo: return "sdl_video";
    case StartupFailure::kNoDisplay: return "no_display";
    case StartupFailure::kWindow: return "window";
    case StartupFailure::kRenderer: return "renderer";
    }
    return "unknown";
}

inline std::string StartupFailureLine(StartupFailure failure, std::string_view detail) {
    return std::string("Launcher: startup: failed kind=") + StartupFailureName(failure) + " detail=" + std::string(detail);
}

// What the player is told when no renderer opens: each renderer tried and why it failed.
inline std::string RendererFailureText(const RendererSelection& selection) {
    std::string text = "The launcher could not start its window renderer.";
    for (const RendererAttempt& attempt : selection.attempts) {
        if (attempt.failure) {
            text += "\n" + AttemptLabel(attempt.name) + ": " + attempt.failure->reason;
        }
    }
    return text;
}

} // namespace dcp
