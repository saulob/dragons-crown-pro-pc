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
    CHECK(std::string(DCP_GAMESCOPE_TOUCH_MARKER_ATOM) == "GAMESCOPE_INPUT_COUNTER");

    // Steam can take the mode back at any time, so the rewrite is fast but never per frame.
    CHECK(DCP_GAMESCOPE_TOUCH_RECHECK_MS <= 1000u && DCP_GAMESCOPE_TOUCH_RECHECK_MS >= 250u);
    CHECK(DCP_GAMESCOPE_TOUCH_MAX_ROOTS >= 2);
}

// Gamescope keeps one touch mode for the whole compositor: a write on any of its Xwayland roots wins, whatever the other
// roots still hold. DCP-Y9F9QZ is exactly this: the game's root reads Passthrough while Steam's root reads Left.
struct GamescopeRoots {
    std::vector<std::uint32_t> value;
    std::vector<bool> open;
    std::vector<DcpTouchModeRoot> mode;
    std::uint32_t effective = DCP_GAMESCOPE_TOUCH_LEFT;
    bool attached = false; // the game holds an X connection to the roots

    explicit GamescopeRoots(std::uint32_t game = DCP_GAMESCOPE_TOUCH_LEFT,
                            std::uint32_t steam = DCP_GAMESCOPE_TOUCH_LEFT)
        : value{game, steam}, open(2, true), mode(2) {}

    void Write(std::size_t root, std::uint32_t mode_value) {
        value[root] = mode_value;
        effective = mode_value;
    }
    void Steam(std::size_t root, std::uint32_t mode_value) { Write(root, mode_value); }

    // The window's Acquire: the reachable roots are claimed and written; the claim always writes Passthrough.
    int Claim() {
        int written = 0;
        attached = true;
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (!open[i]) continue;
            dcp_touch_mode_claim(&mode[i], 1, value[i]);
            Write(i, DCP_GAMESCOPE_TOUCH_PASSTHROUGH);
            ++written;
        }
        return written;
    }

    bool Maintain() {
        if (!attached) return false;
        bool written = false;
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (!open[i]) continue;
            dcp_touch_mode_claim(&mode[i], 1, value[i]);
            Write(i, DCP_GAMESCOPE_TOUCH_PASSTHROUGH);
            written = true;
        }
        return written;
    }

    int Release() {
        if (!attached) return 0;
        attached = false;
        int restored = 0;
        for (std::size_t i = 0; i < value.size(); ++i) {
            std::uint32_t next = 0;
            if (dcp_touch_mode_release(&mode[i], open[i] ? 1 : 0, value[i], &next)) {
                Write(i, next);
                ++restored;
            }
        }
        return restored;
    }
};

void TestModeOwnership() {
    // A fresh root (Gamescope's default, property absent) is claimed and given back as Left.
    DcpTouchModeRoot fresh{};
    dcp_touch_mode_claim(&fresh, 0, 0);
    CHECK(fresh.owned && fresh.saved == DCP_GAMESCOPE_TOUCH_LEFT);
    uint32_t value = 0;
    CHECK(dcp_touch_mode_release(&fresh, 1, DCP_GAMESCOPE_TOUCH_PASSTHROUGH, &value) == 1 && value == DCP_GAMESCOPE_TOUCH_LEFT);
    CHECK(!fresh.owned);
    CHECK(dcp_touch_mode_release(&fresh, 1, DCP_GAMESCOPE_TOUCH_PASSTHROUGH, &value) == 0); // a second release is harmless

    // Steam's own mode is remembered and restored.
    DcpTouchModeRoot steam{};
    dcp_touch_mode_claim(&steam, 1, 2u);
    CHECK(steam.saved == 2u);
    CHECK(dcp_touch_mode_release(&steam, 1, DCP_GAMESCOPE_TOUCH_PASSTHROUGH, &value) == 1 && value == 2u);

    // Steam changes the mode while the game runs: the game takes it back and later restores Steam's value.
    DcpTouchModeRoot race{};
    dcp_touch_mode_claim(&race, 1, 1u);
    dcp_touch_mode_claim(&race, 1, 0u);
    CHECK(race.saved == 0u && race.owned);
    CHECK(dcp_touch_mode_release(&race, 1, DCP_GAMESCOPE_TOUCH_PASSTHROUGH, &value) == 1 && value == 0u);

    // A root that already reads Passthrough is still claimed and rewritten, but the release writes nothing back:
    // Passthrough was not a value the game took away (this is the DCP-Y9F9QZ stale-root case).
    DcpTouchModeRoot already{};
    dcp_touch_mode_claim(&already, 1, DCP_GAMESCOPE_TOUCH_PASSTHROUGH);
    CHECK(already.owned && already.saved == DCP_GAMESCOPE_TOUCH_PASSTHROUGH);
    CHECK(dcp_touch_mode_release(&already, 1, DCP_GAMESCOPE_TOUCH_PASSTHROUGH, &value) == 0 && !already.owned);

    // Someone else changed it away from Passthrough: nothing stale is written back.
    DcpTouchModeRoot changed{};
    dcp_touch_mode_claim(&changed, 1, 1u);
    CHECK(dcp_touch_mode_release(&changed, 1, 6u, &value) == 0 && !changed.owned);
    CHECK(dcp_touch_mode_release(&changed, 0, 0, &value) == 0);

    // Each root keeps its own value: two roots that started differently are restored independently.
    DcpTouchModeRoot game{};
    DcpTouchModeRoot compositor{};
    dcp_touch_mode_claim(&game, 1, 1u);
    dcp_touch_mode_claim(&compositor, 1, 6u);
    CHECK(dcp_touch_mode_release(&game, 1, DCP_GAMESCOPE_TOUCH_PASSTHROUGH, &value) == 1 && value == 1u);
    CHECK(dcp_touch_mode_release(&compositor, 1, DCP_GAMESCOPE_TOUCH_PASSTHROUGH, &value) == 1 && value == 6u);
}

// DCP-Y9F9QZ: Steam reaches its own root while the game's root already reads Passthrough.
void TestCompetingWriteOnAnotherRootIsReasserted() {
    GamescopeRoots session;
    CHECK(session.Claim() == 2 && session.effective == DCP_GAMESCOPE_TOUCH_PASSTHROUGH);

    session.Steam(1, DCP_GAMESCOPE_TOUCH_LEFT);
    CHECK(session.value[0] == DCP_GAMESCOPE_TOUCH_PASSTHROUGH && session.effective == DCP_GAMESCOPE_TOUCH_LEFT);

    CHECK(session.Maintain());
    CHECK(session.effective == DCP_GAMESCOPE_TOUCH_PASSTHROUGH);
    CHECK(session.value[0] == DCP_GAMESCOPE_TOUCH_PASSTHROUGH && session.value[1] == DCP_GAMESCOPE_TOUCH_PASSTHROUGH);
    CHECK(session.mode[1].saved == DCP_GAMESCOPE_TOUCH_LEFT); // the release gives Steam's own choice back

    // Steam keeps rewriting its root: every later maintenance pass restores Passthrough.
    for (int i = 0; i < 3; ++i) {
        session.Steam(1, DCP_GAMESCOPE_TOUCH_LEFT);
        CHECK(session.Maintain());
        CHECK(session.effective == DCP_GAMESCOPE_TOUCH_PASSTHROUGH);
    }
}

// DCP-Y9F9QZ as the report log shows it: the game's root still held a Passthrough left by an earlier session, so the old
// claim kept that value and wrote nothing while Gamescope's mode had already been set to Left from Steam's root.
void TestStalePassthroughRootIsRewritten() {
    GamescopeRoots session(DCP_GAMESCOPE_TOUCH_PASSTHROUGH, DCP_GAMESCOPE_TOUCH_LEFT);
    session.effective = DCP_GAMESCOPE_TOUCH_LEFT;
    CHECK(session.value[0] == DCP_GAMESCOPE_TOUCH_PASSTHROUGH && session.effective == DCP_GAMESCOPE_TOUCH_LEFT);

    CHECK(session.Claim() == 2); // the claim writes even though the game's own root already read Passthrough
    CHECK(session.effective == DCP_GAMESCOPE_TOUCH_PASSTHROUGH);

    // Only the root that actually held Left is given back; the stale Passthrough is not re-imposed.
    CHECK(session.Release() == 1);
    CHECK(session.value[0] == DCP_GAMESCOPE_TOUCH_PASSTHROUGH && session.value[1] == DCP_GAMESCOPE_TOUCH_LEFT);
    CHECK(session.effective == DCP_GAMESCOPE_TOUCH_LEFT);
}

void TestBothRootsStartLeft() {
    GamescopeRoots session;
    CHECK(session.Claim() == 2);
    CHECK(session.value[0] == DCP_GAMESCOPE_TOUCH_PASSTHROUGH && session.value[1] == DCP_GAMESCOPE_TOUCH_PASSTHROUGH);
    CHECK(session.mode[0].saved == DCP_GAMESCOPE_TOUCH_LEFT && session.mode[1].saved == DCP_GAMESCOPE_TOUCH_LEFT);
}

void TestOneRootUnavailable() {
    GamescopeRoots session;
    session.open[0] = false;
    CHECK(session.Claim() == 1);
    CHECK(session.effective == DCP_GAMESCOPE_TOUCH_PASSTHROUGH);
    session.Steam(1, DCP_GAMESCOPE_TOUCH_LEFT);
    CHECK(session.Maintain() && session.effective == DCP_GAMESCOPE_TOUCH_PASSTHROUGH);

    // The reachable root is still restored on release; the closed one is left alone.
    CHECK(session.Release() == 1 && session.value[1] == DCP_GAMESCOPE_TOUCH_LEFT);
}

void TestNoRootAvailable() {
    GamescopeRoots session;
    session.open[0] = false;
    session.open[1] = false;
    CHECK(session.Claim() == 0);
    CHECK(session.effective == DCP_GAMESCOPE_TOUCH_LEFT); // Gamescope's own handling remains
    CHECK(!session.Maintain());
    CHECK(session.Release() == 0);
}

void TestFocusReleaseAndRegain() {
    GamescopeRoots session;
    CHECK(session.Claim() == 2);

    // Focus loss gives every root its own value back.
    CHECK(session.Release() == 2);
    CHECK(session.value[0] == DCP_GAMESCOPE_TOUCH_LEFT && session.value[1] == DCP_GAMESCOPE_TOUCH_LEFT);
    CHECK(!session.mode[0].owned && !session.mode[1].owned);
    CHECK(!session.Maintain()); // nothing to reassert while the game is not the owner

    // Steam's newer choice is not overwritten by a stale saved value.
    session.Steam(0, 2u);
    CHECK(session.Release() == 0 && session.value[0] == 2u);

    // Focus regain claims again from the current values.
    CHECK(session.Claim() == 2);
    CHECK(session.effective == DCP_GAMESCOPE_TOUCH_PASSTHROUGH);
    CHECK(session.mode[0].saved == 2u && session.mode[1].saved == DCP_GAMESCOPE_TOUCH_LEFT);
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
        "^Touchscreen telemetry: event=gamescope_touch_claim roots=[0-9] compositor=(yes|no) count=1$|"
        "^Touchscreen telemetry: event=gamescope_touch_claim_failed reason=(x11|display) count=1$|"
        "^Touchscreen telemetry: event=gamescope_touch_reassert roots=[0-9] count=[0-9]{1,10}$");
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
    const DcpTouchRouteInfo none{"wayland", 1, 0, 0, 0, DCP_TOUCH_CLAIM_NO_X11};
    const auto ready = Route(log, DCP_TOUCH_ROUTE_READY, none);
    CHECK(ready.size() == 1 &&
          ready[0] == "Touchscreen telemetry: event=touch_route_ready video=wayland gamescope=yes direct_devices=0 count=1");
    CHECK(Route(log, DCP_TOUCH_ROUTE_READY, none).empty()); // once

    // The first touch finds a device SDL had not listed: one update, and no more.
    CHECK(Route(log, DCP_TOUCH_ROUTE_UPDATED, none).empty());
    const DcpTouchRouteInfo found{"wayland", 1, 1, 0, 0, DCP_TOUCH_CLAIM_NO_X11};
    const auto updated = Route(log, DCP_TOUCH_ROUTE_UPDATED, found);
    CHECK(updated.size() == 1 &&
          updated[0] == "Touchscreen telemetry: event=touch_route_updated video=wayland gamescope=yes direct_devices=1 count=1");
    CHECK(Route(log, DCP_TOUCH_ROUTE_UPDATED, DcpTouchRouteInfo{"wayland", 1, 2, 0, 0, DCP_TOUCH_CLAIM_NO_X11}).empty());

    DcpTouchRouteLog known{};
    Route(known, DCP_TOUCH_ROUTE_READY, DcpTouchRouteInfo{"x11", 1, 1, 0, 0, DCP_TOUCH_CLAIM_NO_X11});
    CHECK(Route(known, DCP_TOUCH_ROUTE_UPDATED, DcpTouchRouteInfo{"x11", 1, 1, 0, 0, DCP_TOUCH_CLAIM_NO_X11}).empty());

    // A failed claim names why, once, and no reassert is reported without one.
    DcpTouchRouteLog failed{};
    const DcpTouchRouteInfo unavailable{"x11", 1, 1, 0, 0, DCP_TOUCH_CLAIM_NO_DISPLAY};
    CHECK(Route(failed, DCP_TOUCH_ROUTE_REASSERT, unavailable).empty());
    const auto failed_line = Route(failed, DCP_TOUCH_ROUTE_CLAIM_FAILED, unavailable);
    CHECK(failed_line.size() == 1 &&
          failed_line[0] == "Touchscreen telemetry: event=gamescope_touch_claim_failed reason=display count=1");
    CHECK(Route(failed, DCP_TOUCH_ROUTE_CLAIM_FAILED, unavailable).empty());
    CHECK(Route(failed, DCP_TOUCH_ROUTE_REASSERT, unavailable).empty());
    DcpTouchRouteLog no_x11{};
    CHECK(Route(no_x11, DCP_TOUCH_ROUTE_CLAIM_FAILED, DcpTouchRouteInfo{"x11", 1, 1, 0, 0, DCP_TOUCH_CLAIM_NO_X11})[0] ==
          "Touchscreen telemetry: event=gamescope_touch_claim_failed reason=x11 count=1");

    // The claim says how many roots answered and whether Gamescope's own root was among them.
    DcpTouchRouteLog claimed{};
    const DcpTouchRouteInfo both{"x11", 1, 1, 2, 1, DCP_TOUCH_CLAIM_NO_X11};
    const auto claim = Route(claimed, DCP_TOUCH_ROUTE_CLAIMED, both);
    CHECK(claim.size() == 1 &&
          claim[0] == "Touchscreen telemetry: event=gamescope_touch_claim roots=2 compositor=yes count=1");
    CHECK(Route(claimed, DCP_TOUCH_ROUTE_CLAIMED, both).empty());

    // The periodic reassert is reported at the first and every tenth power, never per interval.
    const std::vector<std::string> reasserts = [&claimed, &both] {
        std::vector<std::string> out;
        for (int i = 0; i < 10; ++i) {
            for (const std::string& line : Route(claimed, DCP_TOUCH_ROUTE_REASSERT, both)) {
                out.push_back(line);
            }
        }
        return out;
    }();
    CHECK(reasserts.size() == 2);
    CHECK(reasserts[0] == "Touchscreen telemetry: event=gamescope_touch_reassert roots=2 count=1");
    CHECK(reasserts[1] == "Touchscreen telemetry: event=gamescope_touch_reassert roots=2 count=10");

    DcpTouchRouteLog single{};
    CHECK(Route(single, DCP_TOUCH_ROUTE_CLAIMED, DcpTouchRouteInfo{"x11", 1, 1, 1, 0, DCP_TOUCH_CLAIM_NO_X11})[0] ==
          "Touchscreen telemetry: event=gamescope_touch_claim roots=1 compositor=no count=1");

    // Only known driver names reach a line; a device count is clamped; nothing else is formatted.
    CHECK(std::string(dcp_touch_video_name("wayland")) == "wayland" && std::string(dcp_touch_video_name("x11")) == "x11" &&
          std::string(dcp_touch_video_name("windows")) == "windows");
    CHECK(std::string(dcp_touch_video_name("kmsdrm")) == "other" && std::string(dcp_touch_video_name("/home/someone")) == "other" &&
          std::string(dcp_touch_video_name(nullptr)) == "none");
    DcpTouchRouteLog odd{};
    const auto clamped = Route(odd, DCP_TOUCH_ROUTE_READY, DcpTouchRouteInfo{"Xbox Ally X", 0, 12345, 0, 0, 0});
    CHECK(clamped.size() == 1 && Contains(clamped[0], "video=other") && Contains(clamped[0], "direct_devices=99") &&
          !Contains(clamped[0], "Ally"));
    DcpTouchRouteLog negative{};
    CHECK(Contains(Route(negative, DCP_TOUCH_ROUTE_READY, DcpTouchRouteInfo{"x11", 0, -4, 0, 0, 0})[0], "direct_devices=0"));
    DcpTouchRouteLog many{};
    CHECK(Contains(Route(many, DCP_TOUCH_ROUTE_CLAIMED, DcpTouchRouteInfo{"x11", 1, 1, 99, 1, 0})[0], "roots=9"));

    for (const std::vector<std::string>* lines : {&ready, &updated, &claim, &failed_line, &reasserts, &clamped}) {
        for (const std::string& line : *lines) {
            CHECK(RouteLineIsSafe(line));
        }
    }
    CHECK(Route(claimed, static_cast<DcpTouchRouteEvent>(DCP_TOUCH_ROUTE_EVENT_COUNT), both).empty());
    CHECK(Route(claimed, static_cast<DcpTouchRouteEvent>(-1), both).empty());
    CHECK(!RouteLineIsSafe("Touchscreen telemetry: event=touch_route_ready video=wayland gamescope=yes direct_devices=1 x=480 count=1"));
    CHECK(!RouteLineIsSafe("Touchscreen telemetry: event=touch_route_ready video=Xbox gamescope=yes direct_devices=1 count=1"));
    CHECK(!RouteLineIsSafe("Touchscreen telemetry: event=gamescope_touch_claim roots=2 compositor=yes display=:1 count=1"));
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

    // The mode is reasserted on a timer only while the window has input focus.
    CHECK(Contains(window, "if ((SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0 &&\n"
                           "        g_gamescope_touch.Maintain(SDL_GetTicks())) {\n"
                           "        LogTouchRoute(DCP_TOUCH_ROUTE_REASSERT);"));
    CHECK(Contains(window, "LogTouchRoute(DCP_TOUCH_ROUTE_CLAIMED);") &&
          Contains(window, "LogTouchRoute(DCP_TOUCH_ROUTE_CLAIM_FAILED);"));

    // Only the environment decides; the route telemetry uses SDL's real driver, not the session variable.
    CHECK(Contains(window, "return dcp_touch_gamescope_wanted(&env) != 0;") && !Contains(window, "GAMESCOPE_WAYLAND_DISPLAY"));
    CHECK(Contains(window, "SDL_GetCurrentVideoDriver(),") && Contains(window, "g_gamescope_touch.Roots(),") &&
          Contains(window, "g_gamescope_touch.CompositorRoot()"));
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

    // The X side is Linux only, loads libX11 on demand, discovers local displays and names no device or path in a log.
    CHECK(Contains(header, "#ifdef __linux__") && Contains(header, "SDL_LoadObject(\"libX11.so.6\")"));
    CHECK(Contains(header, "/tmp/.X11-unix") && Contains(header, "S_ISSOCK") && Contains(header, "getuid()"));
    CHECK(Contains(header, "DCP_GAMESCOPE_TOUCH_MARKER_ATOM"));
    for (const char* shell : {"xprop", "system(", "popen(", "fork(", "execl", "/dev/input"}) {
        CHECK(!Contains(header, shell));
        CHECK(!Contains(policy, shell));
    }
    // No display number is assumed: the game's DISPLAY and the sockets found locally are the only candidates.
    CHECK(!Contains(header, "\":0\"") && !Contains(header, "\":1\""));
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

// DCP-Y9F9QZ: the supplied report log, modelled exactly. Gamescope was effectively in Left mode, so the touch was a
// pointer move and a left click and no finger event ever reached SDL, while the route line still looked healthy.
void TestFailingReportLogIsDistinguishable() {
    DcpTouchRouteLog log{};
    const DcpTouchRouteInfo route{"x11", 1, 1, 0, 0, DCP_TOUCH_CLAIM_NO_DISPLAY};

    Window w;
    Route(log, DCP_TOUCH_ROUTE_READY, route); // direct_devices=1: the touchscreen exists and SDL lists it
    Tap(w, GamescopeMode::Left);
    const std::uint32_t seen = TapButtons(w);

    CHECK(w.HostCursorOwnedByMouse()); // pointer_motion_active source=mouse
    CHECK((seen & BTN_SQUARE) != 0);   // pointer_motion_while_button_held source=mouse -> Square
    CHECK(w.lines.empty());            // no touchscreen_detected, no touchpad_used, no suppression
    CHECK((seen & BTN_TOUCHPAD) == 0);

    // The same route line after the fix: the finger events arrive, so the same session is distinguishable.
    Window fixed;
    Route(log, DCP_TOUCH_ROUTE_CLAIMED, DcpTouchRouteInfo{"x11", 1, 1, 2, 1, DCP_TOUCH_CLAIM_NO_X11});
    Tap(fixed, GamescopeMode::Passthrough);
    const std::uint32_t fixed_seen = TapButtons(fixed);
    CHECK(!fixed.HostCursorOwnedByMouse());
    CHECK((fixed_seen & BTN_TOUCHPAD) != 0 && (fixed_seen & BTN_SQUARE) == 0);
    CHECK(fixed.lines.size() == 2 && Contains(fixed.lines[0], "event=touchscreen_detected") &&
          Contains(fixed.lines[1], "event=touchscreen_synthetic_mouse_suppressed"));
}

void TestReassertStaysBounded() {
    DcpTouchRouteLog log{};
    const DcpTouchRouteInfo both{"x11", 1, 1, 2, 1, DCP_TOUCH_CLAIM_NO_X11};
    Route(log, DCP_TOUCH_ROUTE_CLAIMED, both);
    std::size_t lines = 0;
    for (int i = 0; i < 1000; ++i) {
        lines += Route(log, DCP_TOUCH_ROUTE_REASSERT, both).size();
    }
    CHECK(lines == 4); // 1, 10, 100, 1000
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
    TestCompetingWriteOnAnotherRootIsReasserted();
    TestStalePassthroughRootIsRewritten();
    TestBothRootsStartLeft();
    TestOneRootUnavailable();
    TestNoRootAvailable();
    TestFocusReleaseAndRegain();
    TestVideoBackendDecision();
    TestRouteTelemetry();
    TestTelemetryStaysBounded();
    TestReassertStaysBounded();
    TestWindowSource();
    TestFailingReportLogIsDistinguishable();
    TestDocs();
    std::printf("touchscreen steamos tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
