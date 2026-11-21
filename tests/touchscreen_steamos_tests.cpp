// Focused tests of the touchscreen route under SteamOS / Gamescope (bug report DCP-8G6HDU): when Gamescope's touch handling
// is replaced by Passthrough, what the game receives from a touch, a mouse, a trackpad and a controller, and the route
// telemetry. Headless: no window, no SDL video, no Gamescope, no X11.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#define DCP_VIDEO_BACKEND_POLICY_ONLY
#include "dcp_cursor.h"
#include "dcp_mouse.h"
#include "dcp_pad_buttons.h"
#include "dcp_touch_gamescope.h"
#include "dcp_touch_route.h"
#include "dcp_touchscreen.h"
#include "dcp_video_backend.h"

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                                       \
    do {                                                                                                       \
        ++g_checks;                                                                                            \
        if (!(condition)) {                                                                                    \
            ++g_failures;                                                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                  \
        }                                                                                                      \
    } while (0)

std::string ReadSource(const char* path) {
    std::ifstream file(std::string(DCP_SOURCE_DIR) + "/" + path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

bool Contains(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

constexpr std::uint64_t kMs = 1000000;
constexpr SDL_TouchID kScreen = 7;      // a direct touchscreen
constexpr SDL_TouchID kTrackpad = 8;    // an indirect touch surface
constexpr SDL_MouseID kPhysical = 3;    // an ordinary mouse
constexpr SDL_MouseID kGamescopePointer = 5; // the pointer Gamescope itself moves
constexpr DcpTouchArea kWhole{0.0f, 0.0f, 1.0f, 1.0f};

// The window thread's handling of one SDL event, in the order WindowSDL::PollEvents applies it, with the real state each
// handler feeds.
struct Window {
    DcpCursor::State cursor;
    DcpMouseState* mouse = dcp_mouse_state_new();
    DcpTouchscreen* screen = dcp_touchscreen_new();
    DcpTouchTelemetry* telemetry = dcp_touch_telemetry_new();
    std::vector<std::string> lines;
    std::map<SDL_TouchID, SDL_TouchDeviceType> devices{{kScreen, SDL_TOUCH_DEVICE_DIRECT},
                                                       {kTrackpad, SDL_TOUCH_DEVICE_INDIRECT_ABSOLUTE}};
    DcpTouchArea area = kWhole;
    std::uint64_t now = 1000 * kMs;
    DcpMouseMap map{};
    std::uint32_t seen = 0; // buttons the game read while a tap was down

    Window() {
        dcp_mouse_defaults(&map);
        dcp_mouse_configure(mouse, &map);
    }
    ~Window() {
        dcp_mouse_state_free(mouse);
        dcp_touchscreen_free(screen);
        dcp_touch_telemetry_free(telemetry);
    }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    void Note(DcpTouchEvent event) {
        char line[96];
        if (dcp_touch_telemetry_note(telemetry, event, line, sizeof(line))) {
            lines.emplace_back(line);
        }
    }

    void HandleTouch(const SDL_Event& event, bool ui) {
        const SDL_TouchFingerEvent& finger = event.tfinger;
        const auto device = devices.find(finger.touchID);
        const SDL_TouchDeviceType type = device == devices.end() ? SDL_TOUCH_DEVICE_INVALID : device->second;
        if (!DcpTouchRoute::AcceptedTouch(finger.touchID, type)) {
            return;
        }
        switch (event.type) {
        case SDL_EVENT_FINGER_DOWN:
            if (ui) {
                return;
            }
            Note(DCP_TOUCH_EVENT_DETECTED);
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

    void Dispatch(const SDL_Event& event, bool ui = false) {
        if (DcpTouchRoute::IsFingerEvent(event)) {
            HandleTouch(event, ui);
            return;
        }
        const bool touch_mouse = DcpTouchRoute::TouchGeneratedMouse(event);
        if (DcpTouchRoute::CountedEvent(event, false) == DcpTouchRoute::Counted::MouseSuppressed) {
            Note(DCP_TOUCH_EVENT_MOUSE_SUPPRESSED);
        }
        switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION:
            if (!ui && !touch_mouse) {
                cursor.MouseMoved();
                dcp_mouse_motion(mouse, event.motion.xrel, event.motion.yrel);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (!ui && !touch_mouse) {
                cursor.MouseButtonPressed();
                dcp_mouse_button(mouse, event.button.button, 1);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (!touch_mouse) {
                dcp_mouse_button(mouse, event.button.button, 0);
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            dcp_mouse_release_all(mouse);
            dcp_touchscreen_release_all(screen);
            break;
        default:
            break;
        }
    }

    // What the window does each frame once an overlay or text entry owns the input.
    void Block(bool blocked) {
        dcp_mouse_block(mouse, blocked);
        dcp_touchscreen_block(screen, blocked);
    }

    // One game read of player 1: the mouse's buttons and the touchscreen's, as the pad sampler merges them.
    struct Read {
        std::uint32_t buttons = 0;
        bool touch = false;
        bool mouse_touch = false;
    };
    Read Sample() {
        DcpMouseSample m;
        dcp_mouse_sample(mouse, &m);
        DcpTouchSample t;
        dcp_touchscreen_sample(screen, &t);
        return {m.buttons | t.buttons, t.touch != 0, m.touch != 0};
    }

    bool Idle() {
        const Read read = Sample();
        return !read.buttons && !read.touch && !read.mouse_touch && !dcp_mouse_any_held(mouse);
    }
    bool HostCursorOwnedByMouse() const { return cursor.LastDevice() == DcpCursor::Device::Mouse; }

    std::uint64_t Stamp(std::uint64_t advance_ms) {
        now += advance_ms * kMs;
        return now;
    }
    void Finger(Uint32 type, float x, float y, std::uint64_t advance_ms = 0, SDL_TouchID touch = kScreen, bool ui = false) {
        SDL_Event event{};
        event.type = type;
        event.tfinger.touchID = touch;
        event.tfinger.fingerID = 3;
        event.tfinger.x = x;
        event.tfinger.y = y;
        event.tfinger.timestamp = Stamp(advance_ms);
        Dispatch(event, ui);
    }
    void Press(SDL_MouseID which, Uint8 button, bool down, bool ui = false) {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.which = which;
        event.button.button = button;
        Dispatch(event, ui);
    }
    void Motion(SDL_MouseID which, float dx, float dy, bool ui = false) {
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.which = which;
        event.motion.xrel = dx;
        event.motion.yrel = dy;
        Dispatch(event, ui);
    }
    void FocusLost() {
        SDL_Event event{};
        event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
        Dispatch(event);
    }
    // SDL's own mouse events for a finger (touch_mouse_events is on by default).
    void SdlTouchMouse(bool down) {
        Motion(SDL_TOUCH_MOUSEID, 4.0f, 2.0f);
        Press(SDL_TOUCH_MOUSEID, SDL_BUTTON_LEFT, down);
    }
};

// What SDL delivers for one touchscreen tap, given Gamescope's touch click mode (wlserver_touchdown: Passthrough sends
// wl_touch, every other mode warps a pointer and presses a button).
enum class GamescopeMode { Left, Passthrough };

void Tap(Window& w, GamescopeMode mode, bool ui = false) {
    if (mode == GamescopeMode::Passthrough) {
        w.Finger(SDL_EVENT_FINGER_DOWN, 0.5f, 0.5f, 0, kScreen, ui);
        w.SdlTouchMouse(true);
        w.seen |= w.Sample().buttons;
        w.Finger(SDL_EVENT_FINGER_UP, 0.5f, 0.5f, 60, kScreen, ui);
        w.SdlTouchMouse(false);
    } else {
        w.Motion(kGamescopePointer, 90.0f, 40.0f, ui);
        w.Press(kGamescopePointer, SDL_BUTTON_LEFT, true, ui);
        w.seen |= w.Sample().buttons;
        w.Stamp(60);
        w.Press(kGamescopePointer, SDL_BUTTON_LEFT, false, ui);
    }
}

std::uint32_t TapButtons(Window& w) {
    std::uint32_t seen = w.seen;
    for (int i = 0; i < 8; ++i) {
        seen |= w.Sample().buttons;
    }
    return seen;
}

void TestGamescopeLeftModeIsAMousePress() {
    // The DCP-8G6HDU behaviour: with Gamescope's default Left mode a touch arrives as an ordinary mouse press.
    Window w;
    Tap(w, GamescopeMode::Left);
    CHECK(w.HostCursorOwnedByMouse());
    CHECK((TapButtons(w) & BTN_SQUARE) != 0);
    CHECK(w.lines.empty());
}

void TestPassthroughTapIsTouchpad() {
    Window w;
    Tap(w, GamescopeMode::Passthrough);
    CHECK(!w.HostCursorOwnedByMouse());
    CHECK(w.cursor.LastDevice() == DcpCursor::Device::Controller);
    const std::uint32_t seen = TapButtons(w);
    CHECK((seen & BTN_TOUCHPAD) != 0);
    CHECK((seen & BTN_SQUARE) == 0);
    CHECK(w.Idle());
    CHECK(!dcp_mouse_any_held(w.mouse));
    CHECK(w.lines.size() == 2 && w.lines[0] == "Touchscreen telemetry: event=touchscreen_detected count=1" &&
          w.lines[1] == "Touchscreen telemetry: event=touchscreen_synthetic_mouse_suppressed count=1");
}

void TestDirectFingerAlone() {
    // Real finger events without SDL's mouse copy (SDL_HINT_TOUCH_MOUSE_EVENTS off, or a backend that omits it).
    Window w;
    w.Finger(SDL_EVENT_FINGER_DOWN, 0.25f, 0.5f);
    Window::Read first = w.Sample();
    CHECK(first.touch && first.buttons == 0 && !first.mouse_touch);
    w.Finger(SDL_EVENT_FINGER_MOTION, 0.75f, 0.5f, 16);
    CHECK(w.Sample().touch);
    w.Finger(SDL_EVENT_FINGER_UP, 0.75f, 0.5f, 500);
    CHECK((TapButtons(w) & BTN_TOUCHPAD) == 0); // a long drag is not a tap
    CHECK(w.Idle());
    CHECK(!w.HostCursorOwnedByMouse());
}

void TestTouchMovementUpdatesPosition() {
    Window w;
    w.Finger(SDL_EVENT_FINGER_DOWN, 0.2f, 0.2f);
    const DcpTouchSample before = [&] { DcpTouchSample s; dcp_touchscreen_sample(w.screen, &s); return s; }();
    w.Finger(SDL_EVENT_FINGER_MOTION, 0.8f, 0.6f, 16);
    DcpTouchSample after;
    dcp_touchscreen_sample(w.screen, &after);
    CHECK(before.touch && after.touch && after.x > before.x && after.y > before.y && before.id == after.id);
}

void TestMouseUnchanged() {
    Window w;
    w.Motion(kPhysical, 5.0f, 5.0f);
    CHECK(w.HostCursorOwnedByMouse());
    w.Press(kPhysical, SDL_BUTTON_LEFT, true);
    CHECK((w.Sample().buttons & BTN_SQUARE) != 0);
    w.Press(kPhysical, SDL_BUTTON_LEFT, false);
    CHECK(w.Idle());
    CHECK(w.lines.empty());

    // Remapped buttons keep working and the left button no longer does Square.
    Window remapped;
    remapped.map.button[DCP_MOUSE_SQUARE] = DCP_MOUSE_X1;
    dcp_mouse_configure(remapped.mouse, &remapped.map);
    remapped.Press(kPhysical, SDL_BUTTON_X1, true);
    CHECK((remapped.Sample().buttons & BTN_SQUARE) != 0);
    remapped.Press(kPhysical, SDL_BUTTON_X1, false);
    remapped.Press(kPhysical, SDL_BUTTON_LEFT, true);
    CHECK((remapped.Sample().buttons & BTN_SQUARE) == 0);

    // A mouse the window cannot classify (default id, pen, unknown) is a mouse.
    for (const SDL_MouseID which : {SDL_MouseID(0), SDL_MouseID(99), SDL_PEN_MOUSEID}) {
        Window other;
        other.Press(which, SDL_BUTTON_LEFT, true);
        CHECK((other.Sample().buttons & BTN_SQUARE) != 0 && other.HostCursorOwnedByMouse());
    }
}

void TestTrackpadUnchanged() {
    // The Steam Deck trackpads reach the game as Steam Input mouse events; a trackpad's own touch surface is never a touchscreen.
    Window w;
    w.Motion(kPhysical, 12.0f, -3.0f);
    w.Press(kPhysical, SDL_BUTTON_LEFT, true);
    CHECK(w.HostCursorOwnedByMouse() && (w.Sample().buttons & BTN_SQUARE) != 0);
    w.Press(kPhysical, SDL_BUTTON_LEFT, false);

    Window pad;
    pad.Finger(SDL_EVENT_FINGER_DOWN, 0.5f, 0.5f, 0, kTrackpad);
    pad.Finger(SDL_EVENT_FINGER_UP, 0.5f, 0.5f, 40, kTrackpad);
    CHECK(pad.Idle() && pad.lines.empty() && pad.cursor.LastDevice() != DcpCursor::Device::Controller);
    CHECK(!DcpTouchRoute::AcceptedTouch(SDL_MOUSE_TOUCHID, SDL_TOUCH_DEVICE_DIRECT));
    CHECK(!DcpTouchRoute::AcceptedTouch(kTrackpad, SDL_TOUCH_DEVICE_INDIRECT_RELATIVE));
}

void TestVirtualGamepadUnchanged() {
    // A Steam Virtual Gamepad is a controller: the window's controller path owns it, and a touch afterwards only takes
    // the touch surface.
    Window w;
    DcpCursor::ControllerState state;
    DcpCursor::ControllerActivityFilter filter;
    state.buttons = 1u << 0;
    CHECK(filter.Update(state));
    w.cursor.ControllerActivity();
    Tap(w, GamescopeMode::Passthrough);
    CHECK(w.cursor.LastDevice() == DcpCursor::Device::Controller);
    CHECK((TapButtons(w) & BTN_TOUCHPAD) != 0);

    const std::string touch = ReadSource("src/dcp_touchscreen.c");
    CHECK(!Contains(touch, "pad_owner") && !Contains(touch, "slot") && !Contains(touch, "SDL_Gamepad"));
}

void TestMixedSources() {
    // Touch followed immediately by a mouse, and a mouse followed immediately by touch.
    Window a;
    Tap(a, GamescopeMode::Passthrough);
    a.Motion(kPhysical, 3.0f, 0.0f);
    a.Press(kPhysical, SDL_BUTTON_LEFT, true);
    CHECK(a.HostCursorOwnedByMouse());
    const std::uint32_t after_mouse = a.Sample().buttons;
    CHECK((after_mouse & BTN_SQUARE) != 0);
    a.Press(kPhysical, SDL_BUTTON_LEFT, false);

    Window b;
    b.Motion(kPhysical, 3.0f, 0.0f);
    b.Press(kPhysical, SDL_BUTTON_LEFT, true);
    b.Press(kPhysical, SDL_BUTTON_LEFT, false);
    CHECK(b.HostCursorOwnedByMouse());
    Tap(b, GamescopeMode::Passthrough);
    CHECK(!b.HostCursorOwnedByMouse());
    CHECK((TapButtons(b) & BTN_SQUARE) == 0);

    // A touch while a physical button is held: the touch adds only its own bits.
    Window c;
    c.Press(kPhysical, SDL_BUTTON_LEFT, true);
    c.Finger(SDL_EVENT_FINGER_DOWN, 0.5f, 0.5f);
    const Window::Read both = c.Sample();
    CHECK((both.buttons & BTN_SQUARE) != 0 && both.touch);
    c.Finger(SDL_EVENT_FINGER_UP, 0.5f, 0.5f, 40);
    c.Press(kPhysical, SDL_BUTTON_LEFT, false);
    CHECK((TapButtons(c) & BTN_SQUARE) == 0);
}

void TestOverlayAndTextInputBlocking() {
    Window w;
    Tap(w, GamescopeMode::Passthrough, true); // an overlay captures the input
    CHECK(w.Idle() && w.lines.size() == 1 && w.cursor.LastDevice() != DcpCursor::Device::Controller);

    Window held;
    held.Finger(SDL_EVENT_FINGER_DOWN, 0.5f, 0.5f);
    held.Press(kPhysical, SDL_BUTTON_LEFT, true);
    CHECK(held.Sample().touch);
    held.Block(true); // text entry starts
    CHECK(held.Idle());
    held.Finger(SDL_EVENT_FINGER_DOWN, 0.4f, 0.4f);
    held.Press(kPhysical, SDL_BUTTON_LEFT, true);
    CHECK(held.Idle());
    held.Block(false);
    CHECK(held.Idle()); // nothing stale survives the block
    held.Finger(SDL_EVENT_FINGER_UP, 0.4f, 0.4f, 30);
    held.Press(kPhysical, SDL_BUTTON_LEFT, false);
    CHECK(held.Idle());
    held.Finger(SDL_EVENT_FINGER_DOWN, 0.4f, 0.4f);
    CHECK(held.Sample().touch);
}

void TestFocusLossReleasesEverything() {
    Window w;
    w.Finger(SDL_EVENT_FINGER_DOWN, 0.5f, 0.5f);
    w.Press(kPhysical, SDL_BUTTON_LEFT, true);
    CHECK(w.Sample().touch && dcp_mouse_any_held(w.mouse));
    w.FocusLost();
    CHECK(w.Idle());
}

void TestCoordinates() {
    std::uint16_t x = 0, y = 0;
    // 16:9 picture filling the window and the same picture on a 2x pixel density window: window fractions are the same.
    const DcpTouchArea full = dcp_touch_area_from_pixels(0, 0, 1920, 1080, 1920, 1080);
    const DcpTouchArea dense = dcp_touch_area_from_pixels(0, 0, 3840, 2160, 3840, 2160);
    CHECK(full.x == dense.x && full.y == dense.y && full.width == dense.width && full.height == dense.height);
    CHECK(dcp_touch_point(full, 0.5f, 0.5f, &x, &y) && x == 960);

    // Letterboxed on 1280x800 and pillarboxed on 1920x1200 (fitted output, borderless fullscreen).
    const DcpTouchArea letterbox = dcp_touch_area_from_pixels(0, 40, 1280, 720, 1280, 800);
    CHECK(dcp_touch_point(letterbox, 0.5f, 0.5f, &x, &y) && x == 960);
    CHECK(!dcp_touch_point(letterbox, 0.5f, 0.01f, &x, &y));
    const DcpTouchArea pillar = dcp_touch_area_from_pixels(240, 0, 1440, 810, 1920, 810);
    CHECK(!dcp_touch_point(pillar, 0.02f, 0.5f, &x, &y) && dcp_touch_point(pillar, 0.5f, 0.5f, &x, &y));

    // A touch outside the presented picture never starts a contact.
    Window w;
    w.area = letterbox;
    w.Finger(SDL_EVENT_FINGER_DOWN, 0.5f, 0.01f);
    CHECK(w.Idle());
    w.Finger(SDL_EVENT_FINGER_UP, 0.5f, 0.01f, 30);
    w.Finger(SDL_EVENT_FINGER_DOWN, 0.5f, 0.5f);
    CHECK(w.Sample().touch);
}

void TestGamescopePolicy() {
    DcpTouchGamescopeEnv env{1, 1, ":1", nullptr};
    CHECK(dcp_touch_gamescope_wanted(&env));
    env.opt_out = "0";
    CHECK(!dcp_touch_gamescope_wanted(&env));
    env.opt_out = "1";
    CHECK(dcp_touch_gamescope_wanted(&env));
    env.opt_out = "";
    CHECK(dcp_touch_gamescope_wanted(&env));

    // Non-Gamescope Linux, a session without an X server to ask, and Windows keep their own handling.
    CHECK(!dcp_touch_gamescope_wanted(&(env = {1, 0, ":0", nullptr})));
    CHECK(!dcp_touch_gamescope_wanted(&(env = {1, 1, nullptr, nullptr})));
    CHECK(!dcp_touch_gamescope_wanted(&(env = {1, 1, "", nullptr})));
    CHECK(!dcp_touch_gamescope_wanted(&(env = {0, 1, ":1", nullptr})));

    CHECK(DCP_GAMESCOPE_TOUCH_PASSTHROUGH == 4u && DCP_GAMESCOPE_TOUCH_LEFT == 1u);
    CHECK(std::string(DCP_GAMESCOPE_TOUCH_ATOM) == "STEAM_TOUCH_CLICK_MODE");
}

void TestModeOwnership() {
    uint32_t value = 0;
    // Gamescope's default (property absent): claim, then give back Left.
    DcpTouchModeState state{};
    CHECK(dcp_touch_mode_claim(&state, 0, 0, &value) == DCP_TOUCH_MODE_WRITE && value == DCP_GAMESCOPE_TOUCH_PASSTHROUGH);
    CHECK(dcp_touch_mode_claim(&state, 1, DCP_GAMESCOPE_TOUCH_PASSTHROUGH, &value) == DCP_TOUCH_MODE_KEEP);
    CHECK(dcp_touch_mode_release(&state, 1, DCP_GAMESCOPE_TOUCH_PASSTHROUGH, &value) == DCP_TOUCH_MODE_WRITE &&
          value == DCP_GAMESCOPE_TOUCH_LEFT && !state.applied);

    // Steam's own mode is remembered and restored.
    DcpTouchModeState steam{};
    CHECK(dcp_touch_mode_claim(&steam, 1, 2u, &value) == DCP_TOUCH_MODE_WRITE && steam.saved == 2u);
    CHECK(dcp_touch_mode_release(&steam, 1, DCP_GAMESCOPE_TOUCH_PASSTHROUGH, &value) == DCP_TOUCH_MODE_WRITE && value == 2u);

    // Steam changes the mode while the game runs: the game takes it back, and later restores Steam's value.
    DcpTouchModeState race{};
    dcp_touch_mode_claim(&race, 1, 1u, &value);
    CHECK(dcp_touch_mode_claim(&race, 1, 0u, &value) == DCP_TOUCH_MODE_WRITE && race.saved == 0u);
    CHECK(dcp_touch_mode_release(&race, 1, DCP_GAMESCOPE_TOUCH_PASSTHROUGH, &value) == DCP_TOUCH_MODE_WRITE && value == 0u);

    // Someone else already set Passthrough, or changed it away from the game's: nothing is written back.
    DcpTouchModeState other{};
    CHECK(dcp_touch_mode_claim(&other, 1, DCP_GAMESCOPE_TOUCH_PASSTHROUGH, &value) == DCP_TOUCH_MODE_KEEP && !other.applied);
    CHECK(dcp_touch_mode_release(&other, 1, DCP_GAMESCOPE_TOUCH_PASSTHROUGH, &value) == DCP_TOUCH_MODE_KEEP);
    DcpTouchModeState changed{};
    dcp_touch_mode_claim(&changed, 1, 1u, &value);
    CHECK(dcp_touch_mode_release(&changed, 1, 6u, &value) == DCP_TOUCH_MODE_KEEP && !changed.applied);
    CHECK(dcp_touch_mode_release(&changed, 0, 0, &value) == DCP_TOUCH_MODE_KEEP); // a second release is harmless
}

void TestVideoBackendDecision() {
    // The DCP-8G6HDU session: Steam launch, Gamescope, XDG_SESSION_TYPE=x11. The backend is left to SDL, as before.
    DcpVideoEnvironment env{};
    env.linux_host = 1;
    env.steam_game_id = "123";
    env.session_type = "x11";
    env.display = ":1";
    env.gamescope_display = "gamescope-0";
    env.x11_driver_listed = 1;
    CHECK(dcp_video_choose(&env) == DCP_VIDEO_KEEP_GAMESCOPE);
    env.session_type = "wayland";
    CHECK(dcp_video_choose(&env) == DCP_VIDEO_KEEP_GAMESCOPE);
    char lines[DCP_VIDEO_MAX_LINES][DCP_VIDEO_LINE_SIZE];
    CHECK(dcp_video_telemetry(DCP_VIDEO_KEEP_GAMESCOPE, lines) == 1);
    CHECK(std::string(lines[0]) == "Steam Input telemetry: event=steam_input_native_wayland_retained session=wayland backend=default "
                                   "gamescope=yes reason=gamescope count=1");

    // Outside Gamescope the Steam Wayland policy is unchanged.
    env.gamescope_display = nullptr;
    CHECK(dcp_video_choose(&env) == DCP_VIDEO_USE_X11);
    env.linux_host = 0;
    CHECK(dcp_video_choose(&env) == DCP_VIDEO_UNCHANGED);

    // Touch routing never selects a backend or sets a SDL touch/mouse hint.
    for (const char* path : {"src/dcp_video_backend.h", "src/dcp_sdl_compat.h", "gpu/shim/window.cpp", "src/loader.c",
                             "src/dcp_touch_gamescope.h", "gpu/shim/dcp_gamescope_touch.h"}) {
        const std::string text = ReadSource(path);
        CHECK(!Contains(text, "SDL_HINT_MOUSE_TOUCH_EVENTS") && !Contains(text, "SDL_HINT_TOUCH_MOUSE_EVENTS"));
    }
    CHECK(Contains(ReadSource("gpu/shim/window.cpp"), "dcp_video_backend_prepare(0);"));
}

bool RouteLineIsSafe(const std::string& line) {
    static const std::regex pattern(
        "^Touchscreen telemetry: event=(touch_route_ready|touch_route_updated) video=(wayland|x11|windows|other|none) "
        "gamescope=(yes|no) direct_devices=[0-9]{1,2} count=1$|"
        "^Touchscreen telemetry: event=touchscreen_fallback_(used|unavailable) source=gamescope count=1$");
    return std::regex_match(line, pattern);
}

std::vector<std::string> Route(DcpTouchRouteLog& log, DcpTouchRouteEvent event, const DcpTouchRouteInfo& route) {
    char line[DCP_TOUCH_ROUTE_LINE_SIZE];
    std::vector<std::string> out;
    if (dcp_touch_route_note(&log, event, &route, line, sizeof(line))) {
        out.emplace_back(line);
    }
    return out;
}

void TestRouteTelemetry() {
    DcpTouchRouteLog log{};
    const DcpTouchRouteInfo none{"wayland", 1, 0};
    const auto ready = Route(log, DCP_TOUCH_ROUTE_READY, none);
    CHECK(ready.size() == 1 &&
          ready[0] == "Touchscreen telemetry: event=touch_route_ready video=wayland gamescope=yes direct_devices=0 count=1");
    CHECK(Route(log, DCP_TOUCH_ROUTE_READY, none).empty()); // once

    // The first touch finds a device SDL had not listed: one update, and no more.
    CHECK(Route(log, DCP_TOUCH_ROUTE_UPDATED, none).empty());
    const DcpTouchRouteInfo found{"wayland", 1, 1};
    const auto updated = Route(log, DCP_TOUCH_ROUTE_UPDATED, found);
    CHECK(updated.size() == 1 &&
          updated[0] == "Touchscreen telemetry: event=touch_route_updated video=wayland gamescope=yes direct_devices=1 count=1");
    CHECK(Route(log, DCP_TOUCH_ROUTE_UPDATED, DcpTouchRouteInfo{"wayland", 1, 2}).empty());

    DcpTouchRouteLog known{};
    Route(known, DCP_TOUCH_ROUTE_READY, DcpTouchRouteInfo{"x11", 1, 1});
    CHECK(Route(known, DCP_TOUCH_ROUTE_UPDATED, DcpTouchRouteInfo{"x11", 1, 1}).empty());

    DcpTouchRouteLog fallback{};
    const auto used = Route(fallback, DCP_TOUCH_ROUTE_FALLBACK_USED, found);
    CHECK(used.size() == 1 && used[0] == "Touchscreen telemetry: event=touchscreen_fallback_used source=gamescope count=1");
    CHECK(Route(fallback, DCP_TOUCH_ROUTE_FALLBACK_USED, found).empty());
    const auto unavailable = Route(fallback, DCP_TOUCH_ROUTE_FALLBACK_FAILED, found);
    CHECK(unavailable.size() == 1 &&
          unavailable[0] == "Touchscreen telemetry: event=touchscreen_fallback_unavailable source=gamescope count=1");
    CHECK(Route(fallback, static_cast<DcpTouchRouteEvent>(DCP_TOUCH_ROUTE_EVENT_COUNT), found).empty());
    CHECK(Route(fallback, static_cast<DcpTouchRouteEvent>(-1), found).empty());

    // Only known driver names reach a line; a device count is clamped; nothing else is formatted.
    CHECK(std::string(dcp_touch_video_name("wayland")) == "wayland" && std::string(dcp_touch_video_name("x11")) == "x11" &&
          std::string(dcp_touch_video_name("windows")) == "windows");
    CHECK(std::string(dcp_touch_video_name("kmsdrm")) == "other" && std::string(dcp_touch_video_name("/home/someone")) == "other" &&
          std::string(dcp_touch_video_name(nullptr)) == "none");
    DcpTouchRouteLog odd{};
    const auto clamped = Route(odd, DCP_TOUCH_ROUTE_READY, DcpTouchRouteInfo{"Xbox Ally X", 0, 12345});
    CHECK(clamped.size() == 1 && Contains(clamped[0], "video=other") && Contains(clamped[0], "direct_devices=99") &&
          !Contains(clamped[0], "Ally"));
    DcpTouchRouteLog negative{};
    CHECK(Contains(Route(negative, DCP_TOUCH_ROUTE_READY, DcpTouchRouteInfo{"x11", 0, -4})[0], "direct_devices=0"));

    for (const std::vector<std::string>* lines : {&ready, &updated, &used, &unavailable, &clamped}) {
        for (const std::string& line : *lines) {
            CHECK(RouteLineIsSafe(line));
        }
    }
    CHECK(!RouteLineIsSafe("Touchscreen telemetry: event=touch_route_ready video=wayland gamescope=yes direct_devices=1 x=480 count=1"));
    CHECK(!RouteLineIsSafe("Touchscreen telemetry: event=touch_route_ready video=Xbox gamescope=yes direct_devices=1 count=1"));
}

void TestTelemetryStaysBounded() {
    // Many touches and touch-made mouse events log each legacy line only at the first and at powers of ten.
    Window w;
    for (int i = 0; i < 100; ++i) {
        Tap(w, GamescopeMode::Passthrough);
        w.Sample();
    }
    std::size_t detected = 0, suppressed = 0;
    for (const std::string& line : w.lines) {
        detected += Contains(line, "event=touchscreen_detected") ? 1 : 0;
        suppressed += Contains(line, "event=touchscreen_synthetic_mouse_suppressed") ? 1 : 0;
    }
    CHECK(detected == 1 && suppressed == 3); // 1, 10, 100
    for (const std::string& line : w.lines) {
        CHECK(!Contains(line, "x=") && !Contains(line, "finger") && !Contains(line, "device") && !Contains(line, "0.5"));
    }
}

std::size_t Pos(const std::string& text, const std::string& needle, std::size_t from = 0) {
    const std::size_t at = text.find(needle, from);
    CHECK(at != std::string::npos);
    return at;
}

void TestWindowSource() {
    const std::string window = ReadSource("gpu/shim/window.cpp");
    const std::string header = ReadSource("gpu/shim/dcp_gamescope_touch.h");
    const std::string policy = ReadSource("src/dcp_touch_gamescope.h");

    // Claimed after the window exists and before the route is first reported; given back on exit and on focus loss.
    const std::size_t claim = Pos(window, "    ClaimGamescopeTouch();\n    LogTouchRoute(DCP_TOUCH_ROUTE_READY);");
    CHECK(Pos(window, "DcpTouchRoute::AcceptedTouch(devices[i]") < claim && Pos(window, "LogGamescopeWindow();") < claim);
    const std::size_t destructor = Pos(window, "WindowSDL::~WindowSDL()");
    CHECK(Pos(window, "g_gamescope_touch.Release();", destructor) < Pos(window, "SDL_DestroyWindow(window);", destructor));
    const std::size_t lost = Pos(window, "case SDL_EVENT_WINDOW_FOCUS_LOST:");
    CHECK(Pos(window, "g_gamescope_touch.Release();", lost) < Pos(window, "case SDL_EVENT_WINDOW_FOCUS_GAINED:", lost));
    CHECK(Pos(window, "ClaimGamescopeTouch();", Pos(window, "case SDL_EVENT_WINDOW_FOCUS_GAINED:")) <
          Pos(window, "break;", Pos(window, "case SDL_EVENT_WINDOW_FOCUS_GAINED:")));
    CHECK(Contains(window, "if ((SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0) {\n        g_gamescope_touch.Maintain("));

    // Only the environment decides; the route telemetry uses SDL's real driver, not the session variable.
    CHECK(Contains(window, "return dcp_touch_gamescope_wanted(&env) != 0;") && !Contains(window, "GAMESCOPE_WAYLAND_DISPLAY"));
    CHECK(Contains(window, "SDL_GetCurrentVideoDriver(), DcpPresent::GamescopeSession()"));
    CHECK(!Contains(window, "XDG_SESSION_TYPE"));
    const std::size_t down = Pos(window, "LogTouchRoute(DCP_TOUCH_ROUTE_UPDATED);\n        dcp_touch_telemetry_record(DCP_TOUCH_EVENT_DETECTED);");
    CHECK(down != std::string::npos);

    // A touch is controller activity; the touch handler never reaches the mouse or the host cursor's mouse state.
    const std::size_t handle = Pos(window, "void WindowSDL::HandleTouch(");
    const std::string body = window.substr(handle, Pos(window, "\n}\n", handle) - handle);
    CHECK(Contains(body, "cursor.ControllerActivity();"));
    for (const char* forbidden : {"cursor.MouseMoved", "cursor.MouseButtonPressed", "cursor.MouseWheelMoved", "dcp_mouse_"}) {
        CHECK(!Contains(body, forbidden));
    }

    // The X side is Linux only, loads libX11 on demand and names no device.
    CHECK(Contains(header, "#ifdef __linux__") && Contains(header, "SDL_LoadObject(\"libX11.so.6\")"));
    for (const std::string* text : {&header, &policy}) {
        for (const char* name : {"Ally", "Xbox", "Steam Deck", "Legion", "ROG", "28de"}) {
            CHECK(!Contains(*text, name));
        }
    }
    CHECK(!Contains(policy, "SDL_") && !Contains(policy, "X11/"));
    CHECK(!Contains(header, "XSetErrorHandler"));

    // The pure policy builds on both platforms.
    const std::string cmake = ReadSource("CMakeLists.txt");
    CHECK(Contains(cmake, "add_executable(dcp-touchscreen-steamos-tests EXCLUDE_FROM_ALL tests/touchscreen_steamos_tests.cpp)"));

    // The gates run this suite as its own group, on both platforms.
    const std::string gate = ReadSource("tools/test.ps1");
    CHECK(Contains(gate, "'dcp-touchscreen-steamos-tests'") && Contains(gate, "SteamOS touchscreen tests"));
    CHECK(Contains(gate, " dcp-touchscreen-steamos-tests "));
    const std::string build = ReadSource("tools/build.sh");
    CHECK(Contains(build, "\"$build_dir/bin/dcp-touchscreen-steamos-tests\"") && Contains(build, " dcp-touchscreen-steamos-tests "));
}

void TestDocs() {
    const std::string linux_doc = ReadSource("docs/LINUX.md");
    CHECK(Contains(linux_doc, "touchscreen") && Contains(linux_doc, "DCP_TOUCH_GAMESCOPE") &&
          Contains(linux_doc, "STEAM_TOUCH_CLICK_MODE"));
}

} // namespace

int main() {
    TestGamescopeLeftModeIsAMousePress();
    TestPassthroughTapIsTouchpad();
    TestDirectFingerAlone();
    TestTouchMovementUpdatesPosition();
    TestMouseUnchanged();
    TestTrackpadUnchanged();
    TestVirtualGamepadUnchanged();
    TestMixedSources();
    TestOverlayAndTextInputBlocking();
    TestFocusLossReleasesEverything();
    TestCoordinates();
    TestGamescopePolicy();
    TestModeOwnership();
    TestVideoBackendDecision();
    TestRouteTelemetry();
    TestTelemetryStaysBounded();
    TestWindowSource();
    TestDocs();
    std::printf("touchscreen steamos tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
