// The game window (SDL3) the Vulkan swapchain presents to.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <SDL3/SDL.h>
#include "common/assert.h"
#include "dcp_displays.h"
#include "dcp_window_fit.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "dcp_overlay.h"
#include "dcp_graphics.h"
#include "dcp_latency_trace.h"
#include "dcp_ping.h"
#include "dcp_present.h"
#include "dcp_settings.h"
#include "dcp_title.h"
#include "runtime_online_overlay.h"

extern "C" void runtime_log_flush(void);

namespace Frontend {
namespace {

// No window means no game: say why and exit with an error, not through UNREACHABLE (no crash, no abort).
[[noreturn]] void ExitNoWindow(const char* what, const char* sdl_error) {
    std::fprintf(stderr,
                 "Error: %s: %s\nNo display is available. Run from a graphical session "
                 "(DISPLAY for X11 or WAYLAND_DISPLAY for Wayland must be set).\n",
                 what, sdl_error && *sdl_error ? sdl_error : "unknown SDL error");
    std::fflush(stderr);
    runtime_log_flush();
    std::_Exit(1);
}

void TracePress(const SDL_Event& event) {
    if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat)) {
        DcpLatencyTrace::press_ns.store(event.common.timestamp, std::memory_order_relaxed);
    }
}

} // namespace

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        ExitNoWindow("Cannot initialize SDL video", SDL_GetError());
    }
    // The launcher's Window size and Monitor settings; the same display list as its Monitor row.
    const auto& settings = DcpSettings::Get();
    const std::vector<dcp::Display> displays = dcp::ListDisplays();
    const dcp::Display* display = dcp::DisplayForSetting(displays, settings.monitor);
    int window_width = settings.requested_width > 0 ? settings.requested_width.load() : width_;
    int window_height = settings.requested_height > 0 ? settings.requested_height.load() : height_;
    int window_x = SDL_WINDOWPOS_CENTERED, window_y = SDL_WINDOWPOS_CENTERED;
    if (display) {
        dcp::FitToDisplay(window_width, window_height, *display);
        window_x = window_y = SDL_WINDOWPOS_CENTERED_DISPLAY(display->id);
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, window_x);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, window_y);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, window_width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, window_height);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    base_title = title;
    title_shown = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!window) {
        ExitNoWindow("Cannot create the game window", SDL_GetError());
    }

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
#ifdef _WIN32
    if (driver && !std::strcmp(driver, "windows")) {
        window_info.type = WindowSystemType::Windows;
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    } else
#endif
    if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
    if (display) {
        // Hidden until the real title bar and frame are known, so the decorated window is fitted and centred once.
        dcp::WindowBorders borders;
        SDL_GetWindowBordersSize(window, &borders.top, &borders.left, &borders.bottom, &borders.right);
        const dcp::WindowFit fit = dcp::FitWindowToArea(window_width, window_height, borders, display->usable.x,
                                                        display->usable.y, display->usable.w, display->usable.h);
        window_width = fit.width;
        window_height = fit.height;
        SDL_SetWindowSize(window, fit.width, fit.height);
        SDL_SetWindowPosition(window, fit.x, fit.y);
    }
    ApplyDisplayMode(DcpSettings::Get().display_mode);
    SDL_ShowWindow(window);
    if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window))) {
        DcpSettings::Get().display_refresh_hz = mode->refresh_rate;
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
    const std::string display_name = display && !display->name.empty() ? " (" + display->name + ")" : "";
    std::printf("Window: display %d of %zu%s, requested %dx%d, VSync %s, frame rate in title %s, "
                "resolution in title %s\n",
                display ? int(display - displays.data()) + 1 : 0, displays.size(), display_name.c_str(),
                window_width, window_height, settings.vsync ? "on" : "off", settings.show_fps_title ? "on" : "off",
                settings.show_resolution_title ? "on" : "off");
    const SDL_DisplayMode* current = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    std::printf("Display: refresh %.2f Hz\n", current ? current->refresh_rate : 0.0f);
#ifndef _WIN32
    std::printf("Session: %s\n", DcpPresent::DescribeSession(DcpPresent::GamescopeSession(), std::getenv).c_str());
#endif
}

// Borderless is a desktop-sized fullscreen window, without a display mode change. Window thread only (SDL); the
// swapchain follows the size events.
void WindowSDL::ApplyDisplayMode(int mode) {
    display_mode = mode;
    const bool borderless = mode == DcpGraphics::Borderless;
    if (borderless && !SDL_SetWindowFullscreenMode(window, nullptr)) {
        LOG_WARNING(Frontend, "Borderless fullscreen: {}", SDL_GetError());
    }
    SDL_SetWindowFullscreen(window, borderless);
    SDL_SyncWindow(window);
    std::printf("Window: %s\n", DcpGraphics::DisplayModeNames[borderless ? DcpGraphics::Borderless : DcpGraphics::Windowed]);
}

WindowSDL::~WindowSDL() {
    SDL_ShowCursor();
    SDL_DestroyWindow(window);
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock{text_mutex};
    text = initial;
    text_prompt = prompt;
    text_state = 0;
    text_requested = true;
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

void WindowSDL::UpdateTextTitle() {
    RefreshTitle();
    DcpOverlay::SetTextEntry(text_active, text_prompt, text);
}

// Window thread. The title is the game's, then the render resolution, then the text being typed or the frame rate.
void WindowSDL::RefreshTitle() {
    const auto& settings = DcpSettings::Get();
    std::string entry;
    if (text_active) {
        entry = " \u2014 " + text_prompt + ": " + text + "_  (Enter = OK, Esc = cancel)";
    }
    const std::string title = DcpTitle::Compose({
        .base = base_title,
        .show_resolution = settings.show_resolution_title,
        .width = settings.image_width,
        .height = settings.image_height,
        .show_fps = settings.show_fps_title,
        .fps = fps,
        .entry = entry,
    });
    if (title != title_shown) {
        SDL_SetWindowTitle(window, title.c_str());
        title_shown = title;
    }
}

// The frame rate of the presented game frames, measured about once a second.
void WindowSDL::UpdateFpsTitle() {
    if (!DcpSettings::Get().show_fps_title) {
        fps = -1.0;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (fps_started == std::chrono::steady_clock::time_point{}) {
        fps_started = now;
        fps_frames = DcpOverlay::PresentedFrames();
        return;
    }
    const double elapsed = std::chrono::duration<double>(now - fps_started).count();
    if (elapsed < 1.0) {
        return;
    }
    const u64 frames = DcpOverlay::PresentedFrames();
    fps = double(frames - fps_frames) / elapsed;
    fps_started = now;
    fps_frames = frames;
}

// F11: Windowed and Borderless, saved as the Display mode like the launcher's row.
void WindowSDL::ToggleBorderless() {
    auto& mode = DcpSettings::Get().display_mode;
    mode = mode.load() == DcpGraphics::Windowed ? DcpGraphics::Borderless : DcpGraphics::Windowed;
    DcpSettings::SaveDisplayMode();
}

// Window thread. Visible while an overlay needs the mouse or focus is away; otherwise the last
// gameplay input device decides, in Windowed and Borderless alike.
void WindowSDL::UpdateCursor() {
    const bool focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    if (const auto visible = cursor.Update(true, focused, DcpOverlay::CapturesInput())) {
        if (*visible) {
            SDL_ShowCursor();
        } else {
            SDL_HideCursor();
        }
    }
}

// Window thread. Only the gamepads runtime_pad.c opened produce these events.
bool WindowSDL::TrackControllerEvent(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        pad_activity.erase(event.gdevice.which);
        return false;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        PadActivity& pad = pad_activity[event.gbutton.which];
        const u32 bit = event.gbutton.button < 32 ? 1u << event.gbutton.button : 0u;
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            pad.state.buttons |= bit;
        } else {
            pad.state.buttons &= ~bit;
        }
        return pad.filter.Update(pad.state);
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        PadActivity& pad = pad_activity[event.gaxis.which];
        switch (event.gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX: pad.state.left_x = event.gaxis.value; break;
        case SDL_GAMEPAD_AXIS_LEFTY: pad.state.left_y = event.gaxis.value; break;
        case SDL_GAMEPAD_AXIS_RIGHTX: pad.state.right_x = event.gaxis.value; break;
        case SDL_GAMEPAD_AXIS_RIGHTY: pad.state.right_y = event.gaxis.value; break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: pad.state.left_trigger = event.gaxis.value; break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: pad.state.right_trigger = event.gaxis.value; break;
        default: return false;
        }
        return pad.filter.Update(pad.state);
    }
    default:
        return false;
    }
}

bool WindowSDL::PollEvents() {
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) { // SDL text input must be toggled from the window thread
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
    }
    if (!text_active) {
        DcpOverlay::UpdateTextInput(window);
    }
    if (const int mode = DcpSettings::Get().display_mode; mode != display_mode) {
        ApplyDisplayMode(mode);
    }
    UpdateFpsTitle();
    RefreshTitle();
    DcpOverlay::SetDisplayScale(SDL_GetWindowDisplayScale(window));
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (DcpLatencyTrace::enabled) {
            TracePress(event);
        }
        const bool ui = DcpOverlay::CapturesInput();
        const bool controller_activity = TrackControllerEvent(event);
        if (text_active && (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{text_mutex};
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                text += event.text.text;
            } else if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                size_t cut = text.size() - 1; // drop one UTF-8 code point
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
                text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE) {
                text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                text_active = false;
                SDL_StopTextInput(window);
            }
            UpdateTextTitle();
            continue;
        }
        // The controller finishes the text dialog too: Cross (A) accepts, Circle (B) cancels.
        if (text_active && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
            (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
             event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST)) {
            std::scoped_lock lock{text_mutex};
            text_state = event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : 2;
            text_active = false;
            SDL_StopTextInput(window);
            UpdateTextTitle();
            continue;
        }
        if (DcpOverlay::HandleEvent(event)) {
            continue;
        }
        if (controller_activity && !ui) {
            cursor.ControllerActivity();
        }
        switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION:
            if (!ui) {
                cursor.MouseMoved();
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (!ui) {
                cursor.MouseButtonPressed();
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (!ui) {
                cursor.MouseWheelMoved();
            }
            break;
        case SDL_EVENT_KEY_DOWN: {
            const bool window_shortcut = event.key.scancode == DcpSettings::Get().window_mode_key;
            const bool hud_key = event.key.scancode == DcpPing::kFpsKey || event.key.scancode == DcpPing::kPingKey;
            const bool host_shortcut = window_shortcut || hud_key ||
                                       event.key.scancode == DcpSettings::Get().menu_key ||
                                       event.key.scancode == SDL_SCANCODE_INSERT;
            const DcpPing::HudKey hud =
                DcpPing::HudKeyFor(event.key.scancode, event.key.repeat, runtime_online_play_enabled() != 0);
            auto& settings = DcpSettings::Get();
            if (window_shortcut && !event.key.repeat) {
                ToggleBorderless();
            } else if (hud == DcpPing::HudKey::Fps) {
                settings.show_fps = !settings.show_fps;
            } else if (hud == DcpPing::HudKey::Ping) {
                settings.show_ping = !settings.show_ping;
            } else if (DcpCursor::IsGameplayKey(event.key.repeat, host_shortcut) && !ui) {
                cursor.KeyboardActivity();
            }
            break;
        }
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            break;
        }
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            is_open = false;
            break;
        default:
            break;
        }
    }
    UpdateCursor();
    return is_open;
}

} // namespace Frontend
