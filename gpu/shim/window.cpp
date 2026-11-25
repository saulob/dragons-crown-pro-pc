// The game window (SDL3) the renderer presents to.
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <SDL3/SDL.h>
#include "character_highlight.h"
#include "character_highlight_core.h"
#include "common/assert.h"
#include "dcp_displays.h"
#include "dcp_exit_shortcut.h"
#include "dcpgpu.h"
#include "dcp_window_fit.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "dcp_sdl_compat.h"
#include "dcp_video_backend.h"
#include "dcp_overlay.h"
#include "dcp_comms_wheel.h"
#include "dcp_graphics.h"
#include "dcp_latency_trace.h"
#include "dcp_mouse.h"
#include "dcp_ping.h"
#include "dcp_pointer_motion.h"
#include "dcp_present.h"
#include "dcp_settings.h"
#include "dcp_title.h"
#include "dcp_gamescope_touch.h"
#include "dcp_touch_route.h"
#include "dcp_trackpad.h"
#include "dcp_touchscreen.h"
#include "runtime_online_overlay.h"
#ifdef _WIN32
#include "video_core/renderer_opengl/gl_angle.h"
#endif

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

DcpTouchRouteLog g_touch_route_log;
DcpGamescopeTouch::Claim g_gamescope_touch;

int DirectTouchDevices() {
    int total = 0, direct = 0;
    SDL_TouchID* devices = SDL_GetTouchDevices(&total);
    for (int i = 0; devices && i < total; ++i) {
        direct += DcpTouchRoute::AcceptedTouch(devices[i], SDL_GetTouchDeviceType(devices[i])) ? 1 : 0;
    }
    SDL_free(devices);
    return direct;
}

void LogTouchRoute(DcpTouchRouteEvent event) {
    const DcpTouchRouteInfo route{SDL_GetCurrentVideoDriver(),
                                  DcpPresent::GamescopeSession() ? 1 : 0,
                                  DirectTouchDevices(),
                                  g_gamescope_touch.Roots(),
                                  g_gamescope_touch.CompositorRoot() ? 1 : 0,
                                  int(g_gamescope_touch.Failure())};
    char line[DCP_TOUCH_ROUTE_LINE_SIZE];
    if (dcp_touch_route_note(&g_touch_route_log, event, &route, line, sizeof(line))) {
        std::puts(line);
    }
}

bool GamescopeTouchWanted() {
    DcpTouchGamescopeEnv env{};
#ifdef __linux__
    env.linux_host = 1;
#endif
    env.gamescope = DcpPresent::GamescopeSession() ? 1 : 0;
    env.display = std::getenv("DISPLAY");
    env.opt_out = std::getenv("DCP_TOUCH_GAMESCOPE");
    return dcp_touch_gamescope_wanted(&env) != 0;
}

void ClaimGamescopeTouch() {
    if (!GamescopeTouchWanted()) {
        return;
    }
    switch (g_gamescope_touch.Acquire()) {
    case DcpGamescopeTouch::Result::Applied:
        LogTouchRoute(DCP_TOUCH_ROUTE_CLAIMED);
        break;
    case DcpGamescopeTouch::Result::Unavailable:
        LogTouchRoute(DCP_TOUCH_ROUTE_CLAIM_FAILED);
        break;
    case DcpGamescopeTouch::Result::Unchanged:
        break;
    }
}

void LogExitShortcut() {
    const char* key = SDL_GetScancodeName(SDL_Scancode(DcpSettings::Get().exit_key.load()));
    char line[DCP_EXIT_LINE_SIZE];
    dcp_exit_shortcut_ready_line(line, sizeof(line), key, DcpSettings::Get().controller_exit_shortcut);
    std::puts(line);
}

void PublishTrackpadEnv() {
    DcpTrackpadEnv env{};
#ifdef __linux__
    env.linux_host = 1;
#endif
    env.steam_deck = dcp_trackpad_steam_deck_value(std::getenv("SteamDeck")) ? 1 : 0;
    env.gamescope = DcpPresent::GamescopeSession() ? 1 : 0;
    int mice = 0;
    SDL_free(SDL_GetMice(&mice));
    dcp_mouse_set_trackpad_env(dcp_mouse_global(), &env, mice);
}

void NoteInputEvent(uint64_t& counter, const char* event) {
    if (dcp_trackpad_count_due(++counter)) {
        std::printf("Input telemetry: event=%s count=%llu\n", event, static_cast<unsigned long long>(counter));
    }
}

uint64_t g_sdl_mouse_motion_count = 0;
uint64_t g_cursor_controller_owner_count = 0;

void NoteControllerCursorOwner(const DcpCursor::State& cursor) {
    if (cursor.LastDevice() == DcpCursor::Device::Mouse) {
        NoteInputEvent(g_cursor_controller_owner_count, "cursor_owner_controller");
    }
}

} // namespace

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title, GraphicsApi api)
    : width{width_}, height{height_} {
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    dcp_prepare_sdl_gamepad();
#ifndef _WIN32
    dcp_video_backend_prepare(0);
#endif
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        ExitNoWindow("Cannot initialize SDL video", SDL_GetError());
    }
    // The launcher's Window size and Monitor settings; the same display list as its Monitor row.
    const auto& settings = DcpSettings::Get();
    const std::vector<dcp::Display> displays = dcp::ListDisplays();
    const dcp::Display* display = dcp::DisplayForSetting(displays, settings.monitor);
    int window_width = settings.requested_width > 0 ? settings.requested_width.load() : width_;
    int window_height = settings.requested_height > 0 ? settings.requested_height.load() : height_;
    const int requested_width = window_width, requested_height = window_height;
    int window_x = SDL_WINDOWPOS_CENTERED, window_y = SDL_WINDOWPOS_CENTERED;
    if (display) {
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
#ifdef _WIN32
    if (api == GraphicsApi::Angle) {
        OpenGL::RequestAngleDirect3D11();
    }
#endif
    if (api == GraphicsApi::OpenGL) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    }
    SDL_SetBooleanProperty(props, api == GraphicsApi::Vulkan ? SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN
                                                             : SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN,
                           true);
    base_title = title;
    title_shown = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!window) {
#ifdef _WIN32
        if (api == GraphicsApi::Angle) {
            OpenGL::ExitAngleUnavailable(SDL_GetError());
        }
#endif
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
        if (!SDL_GetWindowBordersSize(window, &borders.top, &borders.left, &borders.bottom, &borders.right)) {
            borders = dcp::EstimatedBorders(display->scale);
        }
        const dcp::WindowFit fit = dcp::FitWindowToArea(window_width, window_height, borders, display->usable.x,
                                                        display->usable.y, display->usable.w, display->usable.h);
        window_width = fit.width;
        window_height = fit.height;
        std::printf("Window fit: bounds %dx%d, usable %dx%d, scale %.2f, borders %d+%d, requested %dx%d, fitted %dx%d\n",
                    display->bounds.w, display->bounds.h, display->usable.w, display->usable.h, display->scale,
                    borders.top, borders.bottom, requested_width, requested_height, fit.width, fit.height);
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
    PublishSize(w, h);
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
    LogGamescopeWindow();
    ClaimGamescopeTouch();
    LogTouchRoute(DCP_TOUCH_ROUTE_READY);
    PublishTrackpadEnv();
    LogExitShortcut();
    const std::string display_name = display && !display->name.empty() ? " (" + display->name + ")" : "";
    std::printf("Window: display %d of %zu%s, size %dx%d, VSync %s, frame rate in title %s, "
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

void WindowSDL::PublishSize(int w, int h) {
    width = w;
    height = h;
    DcpPresent::g_progress.window_size = DcpPresent::Pack({u32(w), u32(h)});
}

// Window thread. window_active stays the start-up watch's flag; the presenter reads the visibility itself.
void WindowSDL::ApplyWindowEvent(DcpPresent::WindowEvent event) {
    visibility.Apply(event);
    visibility_bits.store(visibility.Pack(), std::memory_order_relaxed);
    DcpPresent::g_progress.window_active = !visibility.minimized && !visibility.hidden;
}

void WindowSDL::LogGamescopeWindow() {
    if (!DcpPresent::GamescopeSession()) {
        return;
    }
    int logical_w = 0, logical_h = 0, pixel_w = 0, pixel_h = 0;
    SDL_GetWindowSize(window, &logical_w, &logical_h);
    SDL_GetWindowSizeInPixels(window, &pixel_w, &pixel_h);
    SDL_Rect bounds{};
    SDL_GetDisplayBounds(SDL_GetDisplayForWindow(window), &bounds);
    const bool fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    const std::array<int, 7> state{logical_w, logical_h, pixel_w, pixel_h, bounds.w, bounds.h, fullscreen};
    static std::array<int, 7> last{};
    static unsigned lines = 0;
    constexpr unsigned kMaxLines = 8;
    if (state == last || lines >= kMaxLines) {
        return;
    }
    last = state;
    std::printf("Display diagnostic: Gamescope window%s logical=%dx%d pixels=%dx%d display=%dx%d fullscreen=%s scale=%.2f\n",
                lines++ ? " changed" : "", logical_w, logical_h, pixel_w, pixel_h, bounds.w, bounds.h,
                fullscreen ? "borderless" : "windowed", SDL_GetWindowDisplayScale(window));
}

// Borderless is a desktop-sized fullscreen window, without a display mode change. Window thread only (SDL); the
// swapchain follows the size events.
void WindowSDL::ApplyDisplayMode(int mode) {
    display_mode = mode;
    const bool borderless = mode == DcpGraphics::Borderless;
    if (borderless && !SDL_SetWindowFullscreenMode(window, nullptr)) {
        LOG_WARNING(Frontend, "Borderless fullscreen: {}", SDL_GetError());
    }
    ReleaseMouse();
    SDL_SetWindowFullscreen(window, borderless);
    SDL_SyncWindow(window);
    mouse_settle_until = SDL_GetTicks() + kModeSettleMs;
    std::printf("Window: %s\n", DcpGraphics::DisplayModeNames[borderless ? DcpGraphics::Borderless : DcpGraphics::Windowed]);
}

WindowSDL::~WindowSDL() {
    ReleaseMouse();
    g_gamescope_touch.Release();
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
    if (const auto visible = cursor.Update(true, focused, DcpOverlay::CapturesInput(), mouse_capture.Captured())) {
        if (*visible) {
            SDL_ShowCursor();
        } else {
            SDL_HideCursor();
        }
    }
}

// Window thread. Relative mouse mode is on only while Pointer Control wants the mouse (held in Hold, always in Always On)
// and nothing else needs it; whenever the game stops taking mouse input, held buttons and the pointer are dropped.
void WindowSDL::UpdateMouseCapture() {
    DcpMouseState* const mouse = dcp_mouse_global();
    const bool focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    const bool settling = dcp_mouse_always_on(mouse) && SDL_GetTicks() < mouse_settle_until;
    const DcpMouseCapture::Frame frame{dcp_mouse_capture_wanted(mouse) != 0, focused,
                                       DcpOverlay::CapturesInput() || settling || DcpCommsWheel::MouseEngaged(),
                                       text_active};
    const bool blocked = DcpMouseCapture::Blocked(frame);
    if (blocked != mouse_blocked) {
        mouse_blocked = blocked;
        dcp_mouse_block(mouse, blocked);
        dcp_touchscreen_block(dcp_touchscreen_global(), blocked);
    }
    if (const auto relative = mouse_capture.Update(frame)) {
        if (!SDL_SetWindowRelativeMouseMode(window, *relative)) {
            LOG_WARNING(Frontend, "Relative mouse mode: {}", SDL_GetError());
        }
    }
}

// Window thread. A window mode switch and closing the game must not leave the mouse captured or a button held.
void WindowSDL::ReleaseMouse() {
    dcp_mouse_release_all(dcp_mouse_global());
    dcp_touchscreen_release_all(dcp_touchscreen_global());
    if (mouse_capture.Release()) {
        SDL_SetWindowRelativeMouseMode(window, false);
    }
}

// Window thread. Only the gamepads runtime_pad.c opened produce these events.
bool WindowSDL::TrackControllerEvent(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        pad_activity.erase(event.gdevice.which);
        dcp_exit_chord_forget(&exit_chord, event.gdevice.which);
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

// Window thread. Keyboard and controller close the game through dcpgpu_request_close(), as the title's Exit Game row does.
void WindowSDL::RequestExitShortcut(DcpExitEvent route, u32 held_ms) {
    if (!dcpgpu_request_close()) {
        return;
    }
    char line[DCP_EXIT_LINE_SIZE];
    if (dcp_exit_telemetry_note(&exit_telemetry, route, held_ms, line, sizeof(line))) {
        std::puts(line);
    }
}

// Window thread, once per pump. Each opened controller is judged alone from its own buttons; the hold survives overlays and
// dialogs opening, and ends with focus, the setting being off, or a button released.
void WindowSDL::UpdateExitChord() {
    if (!DcpSettings::Get().controller_exit_shortcut) {
        dcp_exit_chord_clear(&exit_chord);
        return;
    }
    if ((SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) == 0) {
        dcp_exit_chord_cancel(&exit_chord);
        return;
    }
    const u64 now_ms = SDL_GetTicks();
    for (const auto& entry : pad_activity) {
        SDL_Gamepad* const gamepad = SDL_GetGamepadFromID(entry.first);
        if (!gamepad) {
            dcp_exit_chord_forget(&exit_chord, entry.first);
            continue;
        }
        u32 held_ms = 0;
        switch (dcp_exit_chord_update(&exit_chord, entry.first, true, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER),
                                      SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER),
                                      SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START), now_ms, &held_ms)) {
        case DCP_EXIT_CHORD_STARTED: {
            char line[DCP_EXIT_LINE_SIZE];
            if (dcp_exit_telemetry_note(&exit_telemetry, DCP_EXIT_EVENT_CONTROLLER_HOLD_STARTED, 0, line, sizeof(line))) {
                std::puts(line);
            }
            break;
        }
        case DCP_EXIT_CHORD_COMPLETED:
            RequestExitShortcut(DCP_EXIT_EVENT_CONTROLLER_USED, held_ms);
            break;
        default:
            break;
        }
    }
}

extern "C" int runtime_pad_player_of_instance(uint32_t instance);

namespace {

SDL_GamepadButton SdlButton(dcp::highlight::PadButton button) {
    using dcp::highlight::PadButton;
    switch (button) {
    case PadButton::Back: return SDL_GAMEPAD_BUTTON_BACK;
    case PadButton::LeftStick: return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    case PadButton::RightStick: return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    case PadButton::LeftShoulder: return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    case PadButton::RightShoulder: return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    }
    return SDL_GAMEPAD_BUTTON_INVALID;
}

} // namespace

// Window thread. The chosen two-button chord on a controller toggles the highlight of the local player that controller drives.
void WindowSDL::UpdateHighlightChord() {
    const int chord = DcpSettings::Get().character_highlight_chord;
    if (chord < 0 || chord >= dcp::highlight::kChordOff || (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) == 0) {
        highlight_chord_down.clear();
        return;
    }
    const auto& buttons = dcp::highlight::kChords[static_cast<size_t>(chord)];
    for (const auto& entry : pad_activity) {
        SDL_Gamepad* const gamepad = SDL_GetGamepadFromID(entry.first);
        if (!gamepad) {
            highlight_chord_down.erase(entry.first);
            continue;
        }
        const bool down = SDL_GetGamepadButton(gamepad, SdlButton(buttons.first)) &&
                          SDL_GetGamepadButton(gamepad, SdlButton(buttons.second));
        bool& was_down = highlight_chord_down[entry.first];
        if (down && !was_down) {
            character_highlight_toggle_pad(runtime_pad_player_of_instance(entry.first));
        }
        was_down = down;
    }
}

// Window thread. A DCP_MOTION_HELD_* mask of the mouse and opened-gamepad buttons held right now.
unsigned WindowSDL::HeldButtons() const {
    unsigned held = dcp_mouse_any_held(dcp_mouse_global()) ? DCP_MOTION_HELD_MOUSE : 0u;
    for (const auto& [id, pad] : pad_activity) {
        const auto& s = pad.state;
        if (s.buttons || s.left_trigger / 128 >= DcpCursor::kTriggerThreshold ||
            s.right_trigger / 128 >= DcpCursor::kTriggerThreshold) {
            held |= DCP_MOTION_HELD_GAMEPAD;
        }
    }
    return held;
}

// Window thread, after TrackControllerEvent recorded the axis.
bool WindowSDL::RightStickMoved(const SDL_Event& event) const {
    const SDL_GamepadAxisEvent& axis = event.gaxis;
    if (axis.axis != SDL_GAMEPAD_AXIS_RIGHTX && axis.axis != SDL_GAMEPAD_AXIS_RIGHTY) {
        return false;
    }
    const auto pad = pad_activity.find(axis.which);
    return pad != pad_activity.end() && (std::abs(pad->second.state.right_x) >= DcpCursor::kStickDeadzone ||
                                         std::abs(pad->second.state.right_y) >= DcpCursor::kStickDeadzone);
}

static DcpTouchArea GameTouchArea() {
    const auto& progress = DcpPresent::g_progress;
    const DcpPresent::Size origin = DcpPresent::Unpack(progress.destination_origin.load(std::memory_order_relaxed));
    const DcpPresent::Size size = DcpPresent::Unpack(progress.destination_size.load(std::memory_order_relaxed));
    const DcpPresent::Size surface = DcpPresent::Unpack(progress.destination_surface.load(std::memory_order_relaxed));
    return dcp_touch_area_from_pixels(int(origin.width), int(origin.height), int(size.width), int(size.height),
                                      int(surface.width), int(surface.height));
}

void WindowSDL::HandleTouch(const SDL_Event& event, bool ui) {
    const SDL_TouchFingerEvent& finger = event.tfinger;
    if (!DcpTouchRoute::AcceptedTouch(finger.touchID, SDL_GetTouchDeviceType(finger.touchID))) {
        return;
    }
    DcpTouchscreen* const screen = dcp_touchscreen_global();
    const DcpTouchArea area = GameTouchArea();
    switch (event.type) {
    case SDL_EVENT_FINGER_DOWN:
        if (ui) {
            return;
        }
        LogTouchRoute(DCP_TOUCH_ROUTE_UPDATED);
        dcp_touch_telemetry_record(DCP_TOUCH_EVENT_DETECTED);
        cursor.ControllerActivity();
        dcp_touchscreen_down(screen, finger.fingerID, area, finger.x, finger.y, finger.timestamp);
        break;
    case SDL_EVENT_FINGER_MOTION:
        dcp_touchscreen_motion(screen, finger.fingerID, area, finger.x, finger.y);
        break;
    case SDL_EVENT_FINGER_UP:
        dcp_touchscreen_up(screen, finger.fingerID, area, finger.x, finger.y, finger.timestamp);
        break;
    default:
        dcp_touchscreen_cancel(screen, finger.fingerID);
        break;
    }
}

bool WindowSDL::PollEvents() {
    if (close_requested.load(std::memory_order_relaxed)) {
        if (is_open.exchange(false)) {
            std::printf("Exit Game: hard exit\n");
            ReleaseMouse();
        }
    }
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) { // SDL text input must be toggled from the window thread
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
    }
    if (const int mode = DcpSettings::Get().display_mode; mode != display_mode) {
        ApplyDisplayMode(mode);
    }
    UpdateFpsTitle();
    RefreshTitle();
    DcpOverlay::SetDisplayScale(SDL_GetWindowDisplayScale(window));
    cursor.SetMouseControls(dcp_mouse_enabled(dcp_mouse_global()) != 0);
    if ((SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0 &&
        g_gamescope_touch.Maintain(SDL_GetTicks())) {
        LogTouchRoute(DCP_TOUCH_ROUTE_REASSERT);
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (DcpLatencyTrace::enabled) {
            TracePress(event);
        }
        const bool ui = DcpOverlay::CapturesInput();
        const bool controller_activity = TrackControllerEvent(event);
        if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            dcp_exit_chord_cancel(&exit_chord);
        }
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
        // The keyboard route also works with the F1 and F10 menus and the game's dialogs open; typing never reaches it.
        if (event.type == SDL_EVENT_KEY_DOWN &&
            dcp_exit_key_pressed(event.key.scancode, event.key.repeat, DcpSettings::Get().exit_key)) {
            RequestExitShortcut(DCP_EXIT_EVENT_KEYBOARD_USED, 0);
            continue;
        }
        if (DcpOverlay::HandleEvent(event)) {
            continue;
        }
        if (controller_activity && !ui) {
            NoteControllerCursorOwner(cursor);
            cursor.ControllerActivity();
        }
        if (DcpTouchRoute::IsFingerEvent(event)) {
            HandleTouch(event, ui);
            continue;
        }
        const bool touch_mouse = DcpTouchRoute::TouchGeneratedMouse(event);
        if (DcpTouchRoute::CountedEvent(event, false) == DcpTouchRoute::Counted::MouseSuppressed) {
            LogTouchRoute(DCP_TOUCH_ROUTE_UPDATED);
            dcp_touch_telemetry_record(DCP_TOUCH_EVENT_MOUSE_SUPPRESSED);
        }
        switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION:
            if (!ui && !touch_mouse) {
                NoteInputEvent(g_sdl_mouse_motion_count, "sdl_mouse_motion_received");
                cursor.MouseMoved();
                const float to_game = SDL_GetWindowPixelDensity(window) * dcp_mouse_game_scale(GetWidth(), GetHeight());
                dcp_mouse_motion(dcp_mouse_global(), event.motion.xrel * to_game, event.motion.yrel * to_game);
                const bool pointer = dcp_mouse_pointer_active(dcp_mouse_global()) != 0;
                dcp_motion_record_host(pointer ? DCP_MOTION_SOURCE_MOUSE_POINTER : DCP_MOTION_SOURCE_MOUSE, HeldButtons(),
                                       SDL_GetTicksNS());
            }
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (!ui && RightStickMoved(event)) {
                dcp_motion_record_host(DCP_MOTION_SOURCE_GAMEPAD_AXIS, HeldButtons(), SDL_GetTicksNS());
            }
            break;
        case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
            if (!ui) {
                dcp_mouse_controller_touch(dcp_mouse_global());
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (!ui && !touch_mouse) {
                cursor.MouseButtonPressed();
                dcp_mouse_button(dcp_mouse_global(), event.button.button, 1);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (!touch_mouse) {
                dcp_mouse_button(dcp_mouse_global(), event.button.button, 0);
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (!ui && !touch_mouse) {
                cursor.MouseWheelMoved();
                dcp_mouse_wheel(dcp_mouse_global(), event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y
                                                                                                   : event.wheel.y);
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            ReleaseMouse();
            g_gamescope_touch.Release();
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            ClaimGamescopeTouch();
            break;
        case SDL_EVENT_KEY_DOWN: {
            const bool window_shortcut = event.key.scancode == DcpSettings::Get().window_mode_key;
            const bool hud_key = event.key.scancode == DcpPing::kFpsKey || event.key.scancode == DcpPing::kPingKey;
            const bool highlight_shortcut = event.key.scancode == DcpSettings::Get().character_highlight_key;
            const bool host_shortcut = window_shortcut || hud_key || highlight_shortcut ||
                                       event.key.scancode == DcpSettings::Get().menu_key;
            const DcpPing::HudKey hud =
                DcpPing::HudKeyFor(event.key.scancode, event.key.repeat, runtime_online_play_enabled() != 0);
            auto& settings = DcpSettings::Get();
            if (window_shortcut && !event.key.repeat) {
                ToggleBorderless();
            } else if (highlight_shortcut && !event.key.repeat) {
                character_highlight_toggle_local(0);
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
            PublishSize(w, h);
            LogGamescopeWindow();
            break;
        }
        case SDL_EVENT_WINDOW_MINIMIZED:
            ApplyWindowEvent(DcpPresent::WindowEvent::Minimized);
            break;
        case SDL_EVENT_WINDOW_HIDDEN:
            ApplyWindowEvent(DcpPresent::WindowEvent::Hidden);
            break;
        case SDL_EVENT_WINDOW_RESTORED:
            ApplyWindowEvent(DcpPresent::WindowEvent::Restored);
            break;
        case SDL_EVENT_WINDOW_SHOWN:
            ApplyWindowEvent(DcpPresent::WindowEvent::Shown);
            break;
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            is_open = false;
            ReleaseMouse();
            g_gamescope_touch.Release(); // the window thread ends the process: no destructor runs
            break;
        default:
            break;
        }
    }
    UpdateExitChord();
    UpdateHighlightChord();
    UpdateMouseCapture();
    UpdateCursor();
    return is_open;
}

} // namespace Frontend
