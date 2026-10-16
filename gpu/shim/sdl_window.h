// The game window. It is created, and its events are pumped, on the window thread (dcpgpu.cpp).
#pragma once
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include "common/types.h"
#include "dcp_cursor.h"

struct SDL_Window;

namespace Frontend {

enum class WindowSystemType : u8 { Headless, Windows, X11, Wayland, Metal };

struct WindowSystemInfo {
    void* display_connection = nullptr;
    void* render_surface = nullptr;
    float render_surface_scale = 1.0f;
    WindowSystemType type = WindowSystemType::Headless;
};

class WindowSDL {
public:
    WindowSDL(s32 width, s32 height, const char* title);
    ~WindowSDL();
    s32 GetWidth() const { return width.load(std::memory_order_relaxed); }
    s32 GetHeight() const { return height.load(std::memory_order_relaxed); }
    SDL_Window* GetSDLWindow() const { return window; }
    WindowSystemInfo GetWindowInfo() const { return window_info; }
    bool IsOpen() const { return is_open.load(std::memory_order_relaxed); }
    /// Processes pending window events. Returns false once the user closed the window.
    bool PollEvents();
    /// Keyboard text entry for the system IME dialog; typed text shows in the title bar.
    void BeginTextInput(const std::string& initial, const std::string& prompt);
    /// 0 while typing, 1 confirmed (Enter), 2 cancelled (Escape); text is UTF-8.
    int PollTextInput(std::string& text);

private:
    std::atomic<s32> width, height;
    std::atomic<bool> is_open{true};
    std::mutex text_mutex;
    bool text_requested{}, text_active{};
    int display_mode = -1; ///< applied DcpSettings display mode (window thread)
    void ApplyDisplayMode(int mode);
    void ToggleBorderless();
    void UpdateFpsTitle();
    void UpdateCursor();
    DcpCursor::Hider cursor; ///< window thread
    void RefreshTitle();
    std::chrono::steady_clock::time_point fps_started{};
    u64 fps_frames = 0;
    double fps = -1.0; ///< frame rate shown in the title; negative: none
    std::string title_shown;
    int text_state{};
    std::string text, text_prompt, base_title;
    void UpdateTextTitle();
    SDL_Window* window{};
    WindowSystemInfo window_info{};
};

} // namespace Frontend
