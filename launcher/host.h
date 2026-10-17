// The launcher's window: SDL3, an SDL renderer and Dear ImGui. It exists only until Play or Quit, before the loader
// creates the game window and its Vulkan presenter, so only one SDL window and one ImGui context are alive at a time.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>

#include "placement.h"

struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Window;

namespace dcp {

struct HostOptions {
    std::string title;
    int width = 1280;  // logical, scaled by the display's content scale
    int height = 720;
    int min_width = 900;
    int min_height = 560;
    std::optional<WindowPlacement> restore;  // used when it lies on a display
    bool hidden = false;                     // page screenshots without showing the window
};

class LauncherHost {
public:
    LauncherHost() = default;
    ~LauncherHost() { Close(); }
    LauncherHost(const LauncherHost&) = delete;
    LauncherHost& operator=(const LauncherHost&) = delete;

    // False when no window or renderer could be created; nothing is left open then.
    bool Open(const HostOptions& options);
    void Close();

    // Handles the window's events (F11 switches the launcher's own fullscreen). False once it was closed.
    bool PumpEvents();
    bool Minimized() const;

    // Between these two the caller draws the launcher with ImGui. A capture path saves the frame as a PNG.
    void BeginFrame();
    void EndFrame(const ImVec4& clear, const char* capture_path = nullptr);

    bool Fullscreen() const;
    void SetFullscreen(bool fullscreen);
    std::optional<WindowState> State() const;

    // A picture for ImGui to draw (RGBA8, width * height * 4 bytes). The host owns it until Close; 0 when it could not be made.
    ImTextureID CreateTexture(const uint8_t* rgba, int width, int height);

    // The saved placement the window was opened with, when it lay on a display.
    const std::optional<WindowPlacement>& Restored() const { return restored_; }

private:
    std::optional<WindowPlacement> restored_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    ImGuiContext* imgui_ = nullptr;
    std::vector<SDL_Texture*> textures_;
    bool video_ = false;
    bool gamepad_ = false;
    bool quit_ = false;
};

} // namespace dcp
