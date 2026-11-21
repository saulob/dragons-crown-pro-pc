// Focused tests of the Steam Deck trackpad (bug report DCP-FHYNJM): the native gamepad touchpad route, the Gaming Mode
// fallback that drives the pointer from the mouse Steam Input makes of the trackpad, click routing, the opt-out, cursor
// ownership, telemetry, and that mouse, controller and touchscreen behavior stay as they were. Headless: no window, no SDL
// video, no Gamescope, no Steam, no hardware.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#define DCP_VIDEO_BACKEND_POLICY_ONLY
#include "dcp_cursor.h"
#include "dcp_keys.h"
#include "dcp_mouse.h"
#include "dcp_mouse_capture.h"
#include "dcp_pad_buttons.h"
#include "dcp_touch_gamescope.h"
#include "dcp_touch_route.h"
#include "dcp_touchscreen.h"
#include "dcp_trackpad.h"

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

std::size_t Pos(const std::string& text, const std::string& needle, std::size_t from = 0) {
    const std::size_t at = text.find(needle, from);
    CHECK(at != std::string::npos);
    return at;
}

constexpr std::uint64_t kMs = 1000000;
constexpr SDL_TouchID kScreen = 7;
constexpr SDL_MouseID kTrackpadMouse = 3; // what Steam Input's trackpad mouse and a plugged-in mouse share
constexpr DcpTouchArea kWhole{0.0f, 0.0f, 1.0f, 1.0f};
constexpr int kValve = DCP_TRACKPAD_VALVE_VENDOR;

struct Setup {
    bool linux_host = true;
    bool deck = true;
    bool gamescope = true;
    int setting = DCP_TRACKPAD_AUTO;
    int pointer_mode = DCP_POINTER_MODE_HOLD;
    bool controls = true;
    int native = 0;
    int mice = 1;
};

void Sink(const char* line, void* user) { static_cast<std::vector<std::string>*>(user)->emplace_back(line); }

// The window thread's handling of the events in the order WindowSDL::PollEvents applies it, and the pad sampler's calls in
// the order runtime_pad.c makes them.
struct Deck {
    DcpMouseState* mouse = dcp_mouse_state_new();
    DcpTouchscreen* screen = dcp_touchscreen_new();
    DcpTouchTelemetry* touch_telemetry = dcp_touch_telemetry_new();
    DcpCursor::State cursor;
    DcpMouseCapture::Tracker capture;
    std::vector<std::string> lines;
    std::vector<std::string> touch_lines;
    DcpMouseMap map{};
    Setup setup;
    bool focused = true, overlay = false, text = false, blocked = false;
    bool pad_touching = false; // a Valve or other controller finger is on its touchpad
    int pads = 0;
    std::uint64_t now = 1000 * kMs;

    explicit Deck(const Setup& s = {}) : setup(s), pads(s.native) {
        dcp_mouse_defaults(&map);
        map.pointer_mode = s.pointer_mode;
        map.trackpad = s.setting;
        map.controls = s.controls ? 1 : 0;
        dcp_mouse_set_log_sink(mouse, Sink, &lines);
        dcp_mouse_configure(mouse, &map);
        DcpTrackpadEnv env{s.linux_host, s.deck, s.gamescope};
        dcp_mouse_set_trackpad_env(mouse, &env, s.mice);
        dcp_mouse_trackpad_pads(mouse, pads, 0);
        Frame();
    }
    ~Deck() {
        dcp_mouse_state_free(mouse);
        dcp_touchscreen_free(screen);
        dcp_touch_telemetry_free(touch_telemetry);
    }
    Deck(const Deck&) = delete;
    Deck& operator=(const Deck&) = delete;

    // WindowSDL::UpdateMouseCapture and UpdateCursor.
    void Frame() {
        cursor.SetMouseControls(dcp_mouse_enabled(mouse) != 0);
        const DcpMouseCapture::Frame frame{dcp_mouse_capture_wanted(mouse) != 0, focused, overlay, text};
        const bool now_blocked = DcpMouseCapture::Blocked(frame);
        if (now_blocked != blocked) {
            blocked = now_blocked;
            dcp_mouse_block(mouse, blocked);
            dcp_touchscreen_block(screen, blocked);
        }
        capture.Update(frame);
        cursor.Update(true, focused, overlay, capture.Captured());
    }
    bool HostCursorVisible() const { return cursor.Visible(); }

    std::vector<std::string> Trackpad() const {
        std::vector<std::string> out;
        for (const std::string& line : lines) {
            if (Contains(line, "Trackpad telemetry:")) out.push_back(line);
        }
        return out;
    }
    bool HasTrackpadLine(const std::string& text) const {
        for (const std::string& line : Trackpad()) {
            if (line == "Trackpad telemetry: " + text) return true;
        }
        return false;
    }
    std::size_t CountTrackpad(const std::string& event) const {
        std::size_t n = 0;
        for (const std::string& line : Trackpad()) n += Contains(line, "event=" + event + " ") ? 1 : 0;
        return n;
    }

    void NoteTouch(DcpTouchEvent event) {
        char line[96];
        if (dcp_touch_telemetry_note(touch_telemetry, event, line, sizeof(line))) touch_lines.emplace_back(line);
    }

    void Dispatch(const SDL_Event& event) {
        const bool ui = overlay;
        if (DcpTouchRoute::IsFingerEvent(event)) {
            const SDL_TouchFingerEvent& finger = event.tfinger;
            if (!DcpTouchRoute::AcceptedTouch(finger.touchID, SDL_TOUCH_DEVICE_DIRECT)) return;
            switch (event.type) {
            case SDL_EVENT_FINGER_DOWN:
                if (ui) return;
                NoteTouch(DCP_TOUCH_EVENT_DETECTED);
                cursor.ControllerActivity();
                dcp_touchscreen_down(screen, finger.fingerID, kWhole, finger.x, finger.y, finger.timestamp);
                break;
            case SDL_EVENT_FINGER_MOTION:
                dcp_touchscreen_motion(screen, finger.fingerID, kWhole, finger.x, finger.y);
                break;
            case SDL_EVENT_FINGER_UP:
                dcp_touchscreen_up(screen, finger.fingerID, kWhole, finger.x, finger.y, finger.timestamp);
                break;
            default:
                dcp_touchscreen_cancel(screen, finger.fingerID);
                break;
            }
            return;
        }
        const bool touch_mouse = DcpTouchRoute::TouchGeneratedMouse(event);
        if (DcpTouchRoute::CountedEvent(event, false) == DcpTouchRoute::Counted::MouseSuppressed) {
            NoteTouch(DCP_TOUCH_EVENT_MOUSE_SUPPRESSED);
        }
        switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION:
            if (!ui && !touch_mouse) {
                cursor.MouseMoved();
                dcp_mouse_motion(mouse, event.motion.xrel, event.motion.yrel);
            }
            break;
        case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
            if (!ui) dcp_mouse_controller_touch(mouse);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (!ui && !touch_mouse) {
                cursor.MouseButtonPressed();
                dcp_mouse_button(mouse, event.button.button, 1);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (!touch_mouse) dcp_mouse_button(mouse, event.button.button, 0);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            dcp_mouse_release_all(mouse);
            dcp_touchscreen_release_all(screen);
            break;
        default:
            break;
        }
    }

    void Motion(float dx, float dy, SDL_MouseID which = kTrackpadMouse) {
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.which = which;
        event.motion.xrel = dx;
        event.motion.yrel = dy;
        Dispatch(event);
    }
    void Press(Uint8 button, bool down, SDL_MouseID which = kTrackpadMouse) {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.which = which;
        event.button.button = button;
        Dispatch(event);
    }
    void Finger(Uint32 type, float x, float y, std::uint64_t advance_ms = 0) {
        now += advance_ms * kMs;
        SDL_Event event{};
        event.type = type;
        event.tfinger.touchID = kScreen;
        event.tfinger.fingerID = 3;
        event.tfinger.x = x;
        event.tfinger.y = y;
        event.tfinger.timestamp = now;
        Dispatch(event);
    }
    void FocusLost() {
        SDL_Event event{};
        event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
        Dispatch(event);
        focused = false;
        Frame();
    }
    void FocusGained() {
        focused = true;
        Frame();
    }
    // A touchpad of the controller: SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN, then the contact held until Lift.
    void PadDown() {
        SDL_Event event{};
        event.type = SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN;
        Dispatch(event);
        pad_touching = true;
    }
    void PadUp() { pad_touching = false; }

    struct Read {
        std::uint32_t buttons = 0;
        bool pointer = false;
        bool pointer_touch = false;
        bool screen_touch = false;
        int x = 0, y = 0;
    };
    // The order of runtime_pad.c's sample_host for player 1.
    Read Sample() {
        const bool native_pad_touching = pad_touching && setup.native > 1;
        dcp_mouse_controller_surface(mouse, pad_touching);
        dcp_mouse_trackpad_pads(mouse, pads, native_pad_touching);
        DcpMouseSample m;
        dcp_mouse_sample(mouse, &m);
        DcpTouchSample t;
        dcp_touchscreen_sample(screen, &t);
        if (t.new_contact) NoteTouch(DCP_TOUCH_EVENT_TOUCHPAD_USED);
        dcp_mouse_screen_surface(mouse, t.touch);
        Read read;
        read.buttons = m.buttons | t.buttons;
        read.pointer = m.pointer != 0;
        read.pointer_touch = m.touch != 0 && !t.touch;
        read.screen_touch = t.touch != 0;
        read.x = t.touch ? t.x : m.x;
        read.y = t.touch ? t.y : m.y;
        return read;
    }
    // Reads until the pointer's finger has landed and returns that read.
    Read Land() {
        Read read;
        for (int i = 0; i < 4; ++i) {
            read = Sample();
            if (read.pointer_touch) break;
        }
        return read;
    }
    bool Idle() {
        const Read read = Sample();
        return !read.buttons && !read.screen_touch && !dcp_mouse_any_held(mouse);
    }
};

// Reads while the buttons are held for a few reads and ORs what the game saw.
std::uint32_t Seen(Deck& d, int reads = 4) {
    std::uint32_t seen = 0;
    for (int i = 0; i < reads; ++i) seen |= d.Sample().buttons;
    return seen;
}

bool Engaged(Deck& d) { return dcp_mouse_trackpad_mode(d.mouse) == DCP_TRACKPAD_MODE_MOUSE_FALLBACK; }

// ---- Detection and policy ----

void TestSteamDeckDetection() {
    CHECK(dcp_trackpad_steam_deck_value("1"));
    CHECK(!dcp_trackpad_steam_deck_value("0") && !dcp_trackpad_steam_deck_value("") && !dcp_trackpad_steam_deck_value(nullptr) &&
          !dcp_trackpad_steam_deck_value("true") && !dcp_trackpad_steam_deck_value("11"));

    struct Row {
        DcpTrackpadInput in;
        int mode;
        int reason;
    };
    const DcpTrackpadEnv deck{1, 1, 1};
    const Row rows[] = {
        {{deck, DCP_TRACKPAD_AUTO, 1, 0}, DCP_TRACKPAD_MODE_MOUSE_FALLBACK, DCP_TRACKPAD_REASON_NONE},
        {{deck, DCP_TRACKPAD_AUTO, 1, 2}, DCP_TRACKPAD_MODE_NATIVE, DCP_TRACKPAD_REASON_NONE},
        {{deck, DCP_TRACKPAD_OFF, 1, 2}, DCP_TRACKPAD_MODE_NATIVE, DCP_TRACKPAD_REASON_NONE},
        {{deck, DCP_TRACKPAD_OFF, 1, 0}, DCP_TRACKPAD_MODE_NONE, DCP_TRACKPAD_REASON_SETTING_OFF},
        {{deck, DCP_TRACKPAD_AUTO, 0, 0}, DCP_TRACKPAD_MODE_NONE, DCP_TRACKPAD_REASON_MOUSE_CONTROLS_OFF},
        {{{1, 1, 0}, DCP_TRACKPAD_AUTO, 1, 0}, DCP_TRACKPAD_MODE_NONE, DCP_TRACKPAD_REASON_NOT_GAMING_MODE},
        {{{1, 0, 1}, DCP_TRACKPAD_AUTO, 1, 0}, DCP_TRACKPAD_MODE_NONE, DCP_TRACKPAD_REASON_NONE},
        {{{1, 0, 0}, DCP_TRACKPAD_AUTO, 1, 0}, DCP_TRACKPAD_MODE_NONE, DCP_TRACKPAD_REASON_NONE},
        {{{0, 1, 1}, DCP_TRACKPAD_AUTO, 1, 0}, DCP_TRACKPAD_MODE_NONE, DCP_TRACKPAD_REASON_NONE}, // Windows never
        {{{0, 0, 0}, DCP_TRACKPAD_AUTO, 1, 3}, DCP_TRACKPAD_MODE_NONE, DCP_TRACKPAD_REASON_NONE},
    };
    for (const Row& row : rows) {
        const DcpTrackpadRoute route = dcp_trackpad_route(&row.in);
        CHECK(route.mode == row.mode && route.reason == row.reason);
    }
}

void TestVirtualGamepadContext() {
    // The Steam Virtual Gamepad 28de:11ff is an Xbox-shaped controller: no touchpad, so the trackpad is the mouse. Only a
    // Valve gamepad that SDL gives touchpads counts as native; a DualShock/DualSense touchpad is an ordinary controller one.
    CHECK(kValve == 0x28de);
    CHECK(dcp_trackpad_native_touchpads(0x28de, 0) == 0);
    CHECK(dcp_trackpad_native_touchpads(0x28de, 2) == 2);
    CHECK(dcp_trackpad_native_touchpads(0x054c, 1) == 0 && dcp_trackpad_native_touchpads(0x054c, 2) == 0);
    CHECK(dcp_trackpad_native_touchpads(0x045e, 0) == 0 && dcp_trackpad_native_touchpads(0x28de, -1) == 0);

    Deck d({.native = dcp_trackpad_native_touchpads(0x28de, 0)});
    CHECK(Engaged(d));
    Deck with_dualshock({.native = dcp_trackpad_native_touchpads(0x054c, 1)});
    CHECK(Engaged(with_dualshock));

    const std::string pad = ReadSource("src/runtime_pad.c");
    CHECK(Contains(pad, "steam_virtual(vendor,product)") && Contains(pad, "0x28de && product==0x11ff"));
    CHECK(Contains(pad, "dcp_trackpad_native_touchpads(SDL_GetGamepadVendor(g),SDL_GetNumGamepadTouchpads(g))"));
}

// ---- Native SDL gamepad touchpad ----

void TestNativeTouchpadPick() {
    CHECK(dcp_trackpad_pick(0, 1, 1) == 1 && dcp_trackpad_pick(1, 0, 1) == 0 && dcp_trackpad_pick(1, 1, 1) == 1);
    CHECK(dcp_trackpad_pick(1, 1, 0) == 0); // a touched pad stays chosen
    CHECK(dcp_trackpad_pick(0, 1, 0) == 1 && dcp_trackpad_pick(0, 0, 0) == 0 && dcp_trackpad_pick(0, 0, 1) == 1);
    CHECK(dcp_trackpad_pick(0, 0, 7) == 1);

    const std::string pad = ReadSource("src/runtime_pad.c");
    // One touchpad of a Deck feeds the game's touch surface; every other pad keeps touchpad 0 and its finger loop.
    CHECK(Contains(pad, "if (valve_touchpads(g)<2) return 0;"));
    CHECK(Contains(pad, "deck_pad=dcp_trackpad_pick(touchpad_touched(g,0),touchpad_touched(g,1),deck_pad);"));
    CHECK(Contains(pad, "SDL_GetGamepadTouchpadFinger(g,touchpad,finger,&down,&x,&y,NULL)"));
    CHECK(Contains(pad, ".x=touch_axis(x,1919)") && Contains(pad, ".y=touch_axis(y,942)"));
    CHECK(Contains(pad, "dcp_mouse_trackpad_pads(dcp_mouse_global(),native_trackpads(),"));
}

void TestNativeRoute() {
    Deck d({.native = 2});
    CHECK(dcp_mouse_trackpad_mode(d.mouse) == DCP_TRACKPAD_MODE_NATIVE && !Engaged(d));
    CHECK(d.HasTrackpadLine("event=trackpad_route_ready steamdeck=yes gamescope=yes native_touchpads=2 mice=1 mode=native count=1"));

    // The native pad owns the touch surface: a mouse is an ordinary mouse again (Hold needs its button).
    d.Motion(20.0f, 0.0f);
    Deck::Read read = d.Sample();
    CHECK(!read.pointer && !read.pointer_touch);

    // Touch down, motion and up: SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN, then the contact is held, then released.
    d.PadDown();
    read = d.Sample();
    CHECK(!read.pointer_touch && !dcp_mouse_pointer_active(d.mouse));
    CHECK(d.CountTrackpad("trackpad_native_used") == 1 && d.HasTrackpadLine("event=trackpad_native_used count=1"));
    for (int i = 0; i < 5; ++i) d.Sample();
    d.PadUp();
    CHECK(d.Idle());
    CHECK(d.CountTrackpad("trackpad_native_used") == 1);
    CHECK(d.CountTrackpad("trackpad_mouse_fallback_used") == 0);

    // The pad's physical press is a contact, not a mouse button: left click stays Square.
    d.Press(SDL_BUTTON_LEFT, true);
    CHECK((Seen(d) & BTN_SQUARE) != 0 && (Seen(d) & BTN_TOUCHPAD) == 0);
    d.Press(SDL_BUTTON_LEFT, false);

    // A DualShock's touchpad, one touchpad on a non-Valve pad, is not counted native.
    Deck ds4({.native = 0});
    ds4.PadDown();
    ds4.Sample();
    CHECK(ds4.CountTrackpad("trackpad_native_used") == 0);
}

// ---- Mouse fallback ----

void TestPointerWithoutHold() {
    Deck d;
    CHECK(Engaged(d) && dcp_mouse_pointer_active(d.mouse) && dcp_mouse_always_on(d.mouse));
    CHECK(d.map.pointer_mode == DCP_POINTER_MODE_HOLD && d.map.button[DCP_MOUSE_POINTER_CONTROL] == DCP_MOUSE_MIDDLE);

    d.Motion(40.0f, 20.0f);
    Deck::Read first = d.Sample();
    CHECK(first.pointer && !first.pointer_touch && first.buttons == 0); // the landing read has no finger: no jump
    Deck::Read landed = d.Sample();
    CHECK(landed.pointer && landed.pointer_touch);
    d.Motion(60.0f, 0.0f);
    Deck::Read right = d.Sample();
    CHECK(right.pointer_touch && right.x > landed.x);
    d.Motion(0.0f, 60.0f);
    Deck::Read down = d.Sample();
    CHECK(down.pointer_touch && down.y > right.y);
    d.Motion(-30.0f, 0.0f);
    CHECK(d.Sample().x < down.x);
    CHECK(d.CountTrackpad("trackpad_mouse_fallback_used") == 1);

    // Movement alone is not a button: nothing is pressed.
    for (int i = 0; i < 4; ++i) CHECK(d.Sample().buttons == 0);

    // The same without any Pointer Control button bound at all.
    Setup none;
    Deck unbound(none);
    unbound.map.button[DCP_MOUSE_POINTER_CONTROL] = DCP_MOUSE_NONE;
    dcp_mouse_configure(unbound.mouse, &unbound.map);
    unbound.Motion(30.0f, 0.0f);
    CHECK(unbound.Land().pointer_touch);
}

void TestClickIsPointerInteraction() {
    Deck d;
    d.Motion(10.0f, 0.0f);
    d.Land();
    d.Press(SDL_BUTTON_LEFT, true);
    const Deck::Read click = d.Sample();
    CHECK((click.buttons & BTN_TOUCHPAD) != 0);
    CHECK((click.buttons & BTN_SQUARE) == 0);
    CHECK((Seen(d) & BTN_SQUARE) == 0);
    CHECK(d.CountTrackpad("trackpad_pointer_interaction") == 1);
    CHECK(d.HasTrackpadLine("event=trackpad_pointer_interaction count=1"));
    d.Press(SDL_BUTTON_LEFT, false);
    CHECK(d.Idle());

    // A click before any movement is the interaction too, never Square.
    Deck early;
    early.Press(SDL_BUTTON_LEFT, true);
    const std::uint32_t seen = Seen(early);
    CHECK((seen & BTN_TOUCHPAD) != 0 && (seen & BTN_SQUARE) == 0);
    early.Press(SDL_BUTTON_LEFT, false);

    // The other trackpad's click is the secondary action; the held click does not count again until released.
    d.Press(SDL_BUTTON_RIGHT, true);
    CHECK((Seen(d) & BTN_CIRCLE) != 0);
    d.Press(SDL_BUTTON_RIGHT, false);
    d.Press(SDL_BUTTON_LEFT, true);
    for (int i = 0; i < 20; ++i) d.Sample();
    CHECK(d.CountTrackpad("trackpad_pointer_interaction") == 1);
    d.Press(SDL_BUTTON_LEFT, false);
    d.Sample();
    d.Press(SDL_BUTTON_LEFT, true);
    d.Sample();
    d.Press(SDL_BUTTON_LEFT, false);
    d.Sample();
    CHECK(d.CountTrackpad("trackpad_pointer_interaction") == 1); // the 3rd click is not a power of ten

    // The middle button is an ordinary button here: Pointer Control needs no button.
    Deck mid;
    mid.Press(SDL_BUTTON_MIDDLE, true);
    CHECK((Seen(mid) & ~BTN_TOUCHPAD) == 0);
}

void TestRemappedInteractionStillRoutes() {
    // The player's own Pointer Interaction button is the click, and left may be Square again.
    Deck d;
    d.map.button[DCP_MOUSE_POINTER_INTERACT] = DCP_MOUSE_X1;
    dcp_mouse_configure(d.mouse, &d.map);
    d.Press(SDL_BUTTON_X1, true);
    CHECK((Seen(d) & BTN_TOUCHPAD) != 0);
    d.Press(SDL_BUTTON_X1, false);
    d.Press(SDL_BUTTON_LEFT, true);
    const std::uint32_t seen = Seen(d);
    CHECK((seen & BTN_SQUARE) != 0 && (seen & BTN_TOUCHPAD) == 0);
}

void TestOrdinaryMouseOutsideTheRoute() {
    const Setup cases[] = {
        {.deck = false},
        {.gamescope = false},
        {.linux_host = false},
        {.setting = DCP_TRACKPAD_OFF},
        {.linux_host = false, .deck = false, .gamescope = false},
    };
    for (const Setup& setup : cases) {
        Deck d(setup);
        CHECK(!Engaged(d) && !dcp_mouse_always_on(d.mouse));
        d.Motion(40.0f, 10.0f);
        Deck::Read read = d.Sample();
        CHECK(!read.pointer && !read.pointer_touch && d.Sample().buttons == 0);
        d.Press(SDL_BUTTON_LEFT, true);
        const std::uint32_t seen = Seen(d);
        CHECK((seen & BTN_SQUARE) != 0 && (seen & BTN_TOUCHPAD) == 0);
        d.Press(SDL_BUTTON_LEFT, false);
        d.Press(SDL_BUTTON_RIGHT, true);
        CHECK((Seen(d) & BTN_CIRCLE) != 0);
        d.Press(SDL_BUTTON_RIGHT, false);

        // Hold still works with its button, as before.
        d.Press(SDL_BUTTON_MIDDLE, true);
        d.Motion(30.0f, 0.0f);
        CHECK(d.Land().pointer_touch);
        d.Press(SDL_BUTTON_MIDDLE, false);
        CHECK(d.Idle());
        CHECK(d.CountTrackpad("trackpad_mouse_fallback_used") == 0 && d.CountTrackpad("trackpad_pointer_interaction") == 0);
    }

    // A player's own Always On choice is untouched off the Deck.
    Deck always({.deck = false, .pointer_mode = DCP_POINTER_MODE_ALWAYS_ON});
    CHECK(dcp_mouse_always_on(always.mouse) && !Engaged(always));
    always.Motion(20.0f, 0.0f);
    CHECK(always.Land().pointer_touch);
    CHECK(always.Trackpad().empty());
}

void TestExternalMouseSafety() {
    // Gamescope has one seat pointer: Steam Input's trackpad mouse and a plugged-in mouse reach SDL as the same kind of
    // mouse, so nothing here tells them apart. The setting is the safety valve; the mouse count only goes into telemetry.
    for (const int mice : {0, 1, 2, 9, 40}) {
        Deck d({.mice = mice});
        CHECK(Engaged(d));
    }
    Deck two({.mice = 2});
    CHECK(two.HasTrackpadLine("event=trackpad_route_ready steamdeck=yes gamescope=yes native_touchpads=0 mice=2 mode=mouse_fallback count=1"));
    Deck many({.mice = 40});
    CHECK(many.HasTrackpadLine("event=trackpad_route_ready steamdeck=yes gamescope=yes native_touchpads=0 mice=9 mode=mouse_fallback count=1"));

    // Every mouse id is handled alike.
    for (const SDL_MouseID which : {SDL_MouseID(0), SDL_MouseID(3), SDL_MouseID(99), SDL_PEN_MOUSEID}) {
        Deck d;
        d.Motion(25.0f, 0.0f, which);
        CHECK(d.Land().pointer_touch);
        d.Press(SDL_BUTTON_LEFT, true, which);
        CHECK((Seen(d) & BTN_SQUARE) == 0);
        d.Press(SDL_BUTTON_LEFT, false, which);
    }

    // Opting out gives a Steam Deck an ordinary mouse, and the cursor stays the host's.
    Deck off({.setting = DCP_TRACKPAD_OFF});
    off.Motion(10.0f, 0.0f);
    off.Frame();
    CHECK(off.HostCursorVisible());
    off.Press(SDL_BUTTON_LEFT, true);
    CHECK((Seen(off) & BTN_SQUARE) != 0);

    const std::string window = ReadSource("gpu/shim/window.cpp");
    const std::size_t motion = Pos(window, "case SDL_EVENT_MOUSE_MOTION:");
    const std::size_t end = Pos(window, "case SDL_EVENT_WINDOW_FOCUS_LOST:", motion);
    CHECK(!Contains(window.substr(motion, end - motion), "which"));
    const std::string policy = ReadSource("src/dcp_trackpad.h");
    CHECK(!Contains(policy, "SDL_") && !Contains(policy, "getenv"));
}

// ---- Controller, right stick, touchscreen ----

void TestControllerUnchanged() {
    Deck d;
    d.Motion(20.0f, 0.0f);
    CHECK(d.Land().pointer_touch);

    // A controller's own touch surface takes the touch surface; the pointer comes back, recentred, when it lifts.
    d.PadDown();
    Deck::Read read = d.Sample();
    CHECK(!read.pointer_touch);
    for (int i = 0; i < 3; ++i) CHECK(!d.Sample().pointer_touch);
    d.PadUp();
    d.Motion(10.0f, 0.0f);
    CHECK(d.Land().pointer_touch);
    CHECK(Engaged(d));

    // Controller buttons and cursor activity never end the route, and a controller press is not a mouse press.
    d.cursor.ControllerActivity();
    d.Frame();
    CHECK(!d.HostCursorVisible() && Engaged(d) && dcp_mouse_pointer_active(d.mouse));
    CHECK(d.Sample().buttons == 0);

    // The mouse state never reads a stick, a trigger or a button of a gamepad: the right stick is the game's alone.
    const std::string mouse_c = ReadSource("src/dcp_mouse.c");
    for (const char* word : {"right_x", "right_y", "left_x", "SDL_GAMEPAD_AXIS", "SDL_Gamepad", "trackpad_axis"}) {
        CHECK(!Contains(mouse_c, word));
    }
    const std::string pad = ReadSource("src/runtime_pad.c");
    CHECK(Contains(pad, "d->right_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX)); d->right_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTY));"));
    const std::size_t axes = Pos(pad, "d->left_x=axis(");
    const std::size_t axes_end = Pos(pad, "\n", axes + 100);
    CHECK(!Contains(pad.substr(axes, axes_end - axes), "trackpad") && !Contains(pad.substr(axes, axes_end - axes), "sensitivity"));
    // The joystick-mode trackpad is a gamepad axis the window cannot tell from the physical stick, and stays as it is.
    const std::string motion = ReadSource("src/dcp_pointer_motion.c");
    CHECK(Contains(motion, "gamepad_axis") && Contains(motion, "mouse_pointer"));
    const std::string window = ReadSource("gpu/shim/window.cpp");
    CHECK(Contains(window, "dcp_motion_record_host(DCP_MOTION_SOURCE_GAMEPAD_AXIS, HeldButtons(), SDL_GetTicksNS());"));
    CHECK(Contains(window, "dcp_motion_record_host(pointer ? DCP_MOTION_SOURCE_MOUSE_POINTER : DCP_MOTION_SOURCE_MOUSE, HeldButtons(),"));
}

void TestTouchscreenUnchanged() {
    Deck d;
    // Gamescope Passthrough: a tap arrives as finger events plus SDL's own mouse copy of the touch.
    d.Finger(SDL_EVENT_FINGER_DOWN, 0.5f, 0.5f);
    SDL_Event copy{};
    copy.type = SDL_EVENT_MOUSE_MOTION;
    copy.motion.which = SDL_TOUCH_MOUSEID;
    copy.motion.xrel = 5.0f;
    d.Dispatch(copy);
    SDL_Event press{};
    press.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    press.button.which = SDL_TOUCH_MOUSEID;
    press.button.button = SDL_BUTTON_LEFT;
    d.Dispatch(press);
    std::uint32_t seen = d.Sample().buttons;
    d.Finger(SDL_EVENT_FINGER_UP, 0.5f, 0.5f, 60);
    press.type = SDL_EVENT_MOUSE_BUTTON_UP;
    d.Dispatch(press);
    for (int i = 0; i < 8; ++i) seen |= d.Sample().buttons;
    CHECK((seen & BTN_TOUCHPAD) != 0 && (seen & BTN_SQUARE) == 0);
    CHECK(d.cursor.LastDevice() == DcpCursor::Device::Controller);
    CHECK(d.touch_lines.size() == 3 && d.touch_lines[0] == "Touchscreen telemetry: event=touchscreen_detected count=1" &&
          d.touch_lines[1] == "Touchscreen telemetry: event=touchscreen_synthetic_mouse_suppressed count=1" &&
          d.touch_lines[2] == "Touchscreen telemetry: event=touchscreen_touchpad_used count=1");
    CHECK(d.Idle());
    // The touch-made mouse events never reached the trackpad pointer.
    CHECK(d.CountTrackpad("trackpad_mouse_fallback_used") == 0 && d.CountTrackpad("trackpad_pointer_interaction") == 0);

    // The route lines of DCP-8G6HDU keep their exact form.
    DcpTouchRouteLog log{};
    char line[DCP_TOUCH_ROUTE_LINE_SIZE];
    const DcpTouchRouteInfo route{"x11", 1, 1};
    CHECK(dcp_touch_route_note(&log, DCP_TOUCH_ROUTE_READY, &route, line, sizeof(line)) &&
          std::string(line) == "Touchscreen telemetry: event=touch_route_ready video=x11 gamescope=yes direct_devices=1 count=1");
    CHECK(dcp_touch_route_note(&log, DCP_TOUCH_ROUTE_FALLBACK_USED, &route, line, sizeof(line)) &&
          std::string(line) == "Touchscreen telemetry: event=touchscreen_fallback_used source=gamescope count=1");

    // Passthrough ownership is the touchscreen route's alone.
    DcpTouchGamescopeEnv env{1, 1, ":1", nullptr};
    CHECK(dcp_touch_gamescope_wanted(&env));
    uint32_t value = 0;
    DcpTouchModeState mode{};
    CHECK(dcp_touch_mode_claim(&mode, 0, 0, &value) == DCP_TOUCH_MODE_WRITE && value == DCP_GAMESCOPE_TOUCH_PASSTHROUGH);
    CHECK(dcp_touch_mode_release(&mode, 1, DCP_GAMESCOPE_TOUCH_PASSTHROUGH, &value) == DCP_TOUCH_MODE_WRITE &&
          value == DCP_GAMESCOPE_TOUCH_LEFT);
    const std::string gamescope_touch = ReadSource("src/dcp_touch_gamescope.h");
    CHECK(!Contains(gamescope_touch, "trackpad") && !Contains(gamescope_touch, "SteamDeck"));
    const std::string route_header = ReadSource("gpu/shim/dcp_touch_route.h");
    CHECK(!Contains(route_header, "dcp_trackpad") && !Contains(route_header, "dcp_mouse"));
}

void TestTouchscreenAndTrackpadTogether() {
    Deck d;
    d.Motion(15.0f, 0.0f);
    CHECK(d.Land().pointer_touch);

    // trackpad -> touchscreen: the finger owns the surface while it is down.
    d.Finger(SDL_EVENT_FINGER_DOWN, 0.3f, 0.6f);
    Deck::Read read = d.Sample();
    CHECK(read.screen_touch && !read.pointer_touch);
    d.Motion(50.0f, 50.0f);
    read = d.Sample();
    CHECK(read.screen_touch && !read.pointer_touch);
    d.Finger(SDL_EVENT_FINGER_MOTION, 0.4f, 0.6f, 16);
    CHECK(d.Sample().screen_touch);

    // touchscreen -> trackpad: the pointer lands again, without a jump, once the finger is gone.
    d.Finger(SDL_EVENT_FINGER_UP, 0.4f, 0.6f, 600);
    for (int i = 0; i < 6; ++i) d.Sample();
    d.Motion(25.0f, 0.0f);
    read = d.Land();
    CHECK(read.pointer_touch && !read.screen_touch);
    CHECK(Engaged(d) && d.Idle());

    // A click while the finger is down is not the trackpad interaction of the pointer.
    d.Finger(SDL_EVENT_FINGER_DOWN, 0.5f, 0.5f);
    d.Sample();
    d.Press(SDL_BUTTON_LEFT, true);
    d.Sample();
    d.Press(SDL_BUTTON_LEFT, false);
    d.Finger(SDL_EVENT_FINGER_UP, 0.5f, 0.5f, 700);
    for (int i = 0; i < 6; ++i) d.Sample();
    CHECK(d.Idle());
}

// ---- Cursor ownership, overlays, focus ----

void TestCursorOwnership() {
    Deck d;
    d.Frame();
    CHECK(!d.HostCursorVisible()); // the game's pointer is the cursor
    d.Motion(30.0f, 5.0f);
    d.Press(SDL_BUTTON_LEFT, true);
    d.Frame();
    CHECK(!d.HostCursorVisible() && d.cursor.LastDevice() == DcpCursor::Device::Mouse);
    d.Press(SDL_BUTTON_LEFT, false);

    // trackpad -> controller -> trackpad: the cursor stays hidden throughout, the pointer follows the last input.
    d.cursor.ControllerActivity();
    d.Frame();
    CHECK(!d.HostCursorVisible());
    d.PadDown();
    d.Sample();
    d.Frame();
    CHECK(!d.HostCursorVisible() && !dcp_mouse_pointer_active(d.mouse));
    d.PadUp();
    d.Motion(8.0f, 0.0f);
    d.Frame();
    CHECK(!d.HostCursorVisible() && d.Land().pointer_touch);

    // trackpad -> touchscreen -> trackpad.
    d.Finger(SDL_EVENT_FINGER_DOWN, 0.2f, 0.2f);
    d.Frame();
    CHECK(!d.HostCursorVisible() && d.cursor.LastDevice() == DcpCursor::Device::Controller);
    d.Finger(SDL_EVENT_FINGER_UP, 0.2f, 0.2f, 700);
    for (int i = 0; i < 6; ++i) d.Sample();
    d.Motion(8.0f, 0.0f);
    d.Frame();
    CHECK(!d.HostCursorVisible() && d.Land().pointer_touch);

    // An ordinary mouse (route off, opt-out, or a mouse outside Gaming Mode) keeps the host cursor.
    for (const Setup& setup : {Setup{.setting = DCP_TRACKPAD_OFF}, Setup{.deck = false}, Setup{.gamescope = false}}) {
        Deck plain(setup);
        plain.Motion(10.0f, 0.0f);
        plain.Frame();
        CHECK(plain.HostCursorVisible());
    }
}

void TestOverlayAndTextInputBlocking() {
    for (const int kind : {0, 1}) {
        Deck d;
        d.Motion(10.0f, 0.0f);
        CHECK(d.Land().pointer_touch);
        d.Press(SDL_BUTTON_LEFT, true);
        (kind == 0 ? d.overlay : d.text) = true;
        d.Frame();
        CHECK(d.HostCursorVisible());
        Deck::Read read = d.Sample();
        CHECK(!read.pointer && !read.pointer_touch && read.buttons == 0 && !dcp_mouse_any_held(d.mouse));
        d.Motion(100.0f, 0.0f);
        d.Press(SDL_BUTTON_LEFT, true);
        d.Press(SDL_BUTTON_RIGHT, true);
        read = d.Sample();
        CHECK(!read.pointer && !read.pointer_touch && read.buttons == 0);
        d.Press(SDL_BUTTON_LEFT, false);
        d.Press(SDL_BUTTON_RIGHT, false);

        (kind == 0 ? d.overlay : d.text) = false;
        d.Frame();
        CHECK(d.Idle());
        d.Motion(10.0f, 0.0f);
        CHECK(d.Land().pointer_touch && Engaged(d));
        CHECK(d.Sample().buttons == 0);
    }
}

void TestFocusLoss() {
    Deck d;
    d.Motion(10.0f, 0.0f);
    d.Land();
    d.Press(SDL_BUTTON_LEFT, true);
    d.Press(SDL_BUTTON_RIGHT, true);
    d.Finger(SDL_EVENT_FINGER_DOWN, 0.5f, 0.5f);
    d.FocusLost();
    CHECK(d.HostCursorVisible());
    CHECK(d.Idle() && !dcp_mouse_pointer_active(d.mouse));
    d.Motion(10.0f, 0.0f); // still delivered after the loss: ignored while blocked
    CHECK(d.Sample().buttons == 0);

    d.FocusGained();
    CHECK(!d.HostCursorVisible() && !dcp_mouse_any_held(d.mouse));
    for (int i = 0; i < 4; ++i) CHECK(d.Sample().buttons == 0);
    d.Motion(10.0f, 0.0f);
    CHECK(d.Land().pointer_touch);
    d.Press(SDL_BUTTON_LEFT, false);
    d.Press(SDL_BUTTON_RIGHT, false);
    CHECK(d.Idle());

    // A native pad contact that is still down while focus leaves is released with the rest.
    Deck native({.native = 2});
    native.PadDown();
    native.Sample();
    native.FocusLost();
    native.PadUp();
    native.FocusGained();
    CHECK(native.Idle());
}

void TestLiveSettingChanges() {
    Deck d;
    CHECK(Engaged(d));
    dcp_mouse_set_enabled(d.mouse, 0);
    CHECK(!Engaged(d) && !dcp_mouse_pointer_active(d.mouse) && !dcp_mouse_always_on(d.mouse));
    d.Press(SDL_BUTTON_LEFT, true);
    CHECK(d.Sample().buttons == 0);
    dcp_mouse_set_enabled(d.mouse, 1);
    CHECK(Engaged(d) && dcp_mouse_pointer_active(d.mouse));

    // The saved Pointer Control Mode is kept underneath: the route off gives it back.
    Deck mode;
    dcp_mouse_set_pointer_mode(mode.mouse, DCP_POINTER_MODE_ALWAYS_ON);
    CHECK(dcp_mouse_always_on(mode.mouse));
    dcp_mouse_set_pointer_mode(mode.mouse, DCP_POINTER_MODE_HOLD);
    CHECK(dcp_mouse_always_on(mode.mouse) && Engaged(mode));
    mode.map.trackpad = DCP_TRACKPAD_OFF;
    dcp_mouse_configure(mode.mouse, &mode.map);
    CHECK(!dcp_mouse_always_on(mode.mouse) && !Engaged(mode));
    mode.map.trackpad = DCP_TRACKPAD_AUTO;
    dcp_mouse_configure(mode.mouse, &mode.map);
    CHECK(Engaged(mode));
}

// ---- Telemetry ----

void TestRouteTelemetry() {
    Deck d;
    CHECK(d.Trackpad().size() == 1 &&
          d.Trackpad()[0] == "Trackpad telemetry: event=trackpad_route_ready steamdeck=yes gamescope=yes native_touchpads=0 mice=1 mode=mouse_fallback count=1");
    d.Motion(10.0f, 0.0f);
    d.Land();
    d.Press(SDL_BUTTON_LEFT, true);
    d.Sample();
    CHECK(d.HasTrackpadLine("event=trackpad_mouse_fallback_used count=1"));
    CHECK(d.HasTrackpadLine("event=trackpad_pointer_interaction count=1"));
    CHECK(d.Trackpad().size() == 3);

    // The route changes once if a native pad appears, and is not repeated.
    d.pads = 2;
    d.Sample();
    CHECK(d.HasTrackpadLine("event=trackpad_route_updated steamdeck=yes gamescope=yes native_touchpads=2 mice=1 mode=native count=1"));
    d.Sample();
    d.Sample();
    CHECK(d.CountTrackpad("trackpad_route_ready") == 1 && d.CountTrackpad("trackpad_route_updated") == 1);

    // Not a Steam Deck, and not Linux: no trackpad line at all, whatever the mouse does.
    for (const Setup& setup : {Setup{.deck = false}, Setup{.linux_host = false, .deck = false}, Setup{.linux_host = false}}) {
        Deck other(setup);
        other.Motion(10.0f, 0.0f);
        other.Press(SDL_BUTTON_LEFT, true);
        Seen(other);
        other.Press(SDL_BUTTON_LEFT, false);
        CHECK(other.Trackpad().empty());
    }
}

void TestUnavailableReasons() {
    Deck off({.setting = DCP_TRACKPAD_OFF});
    CHECK(off.HasTrackpadLine("event=trackpad_unavailable reason=setting_off count=1"));
    CHECK(off.HasTrackpadLine("event=trackpad_route_ready steamdeck=yes gamescope=yes native_touchpads=0 mice=1 mode=none count=1"));
    Deck desktop({.gamescope = false});
    CHECK(desktop.HasTrackpadLine("event=trackpad_unavailable reason=not_gaming_mode count=1"));
    CHECK(desktop.HasTrackpadLine("event=trackpad_route_ready steamdeck=yes gamescope=no native_touchpads=0 mice=1 mode=none count=1"));
    Deck controls({.controls = false});
    CHECK(controls.HasTrackpadLine("event=trackpad_unavailable reason=mouse_controls_off count=1"));

    // Repeated evaluation does not repeat the line.
    for (int i = 0; i < 50; ++i) off.Sample();
    CHECK(off.CountTrackpad("trackpad_unavailable") == 1);
    // Turning Mouse Controls on and off logs each reason on its own, bounded.
    Deck toggled;
    for (int i = 0; i < 40; ++i) {
        dcp_mouse_set_enabled(toggled.mouse, 0);
        toggled.Sample();
        dcp_mouse_set_enabled(toggled.mouse, 1);
        toggled.Sample();
    }
    CHECK(toggled.CountTrackpad("trackpad_unavailable") == 2); // the 1st and the 10th
    CHECK(Engaged(toggled));
}

void TestTelemetryBoundsAndPrivacy() {
    Deck d({.native = 0, .mice = 3});
    for (int round = 0; round < 1200; ++round) {
        d.Motion(float(round % 17) - 8.0f, float(round % 5));
        d.Sample();
        d.Press(SDL_BUTTON_LEFT, true);
        d.Sample();
        d.Press(SDL_BUTTON_LEFT, false);
        d.Sample();
        d.overlay = true;
        d.Frame();
        d.overlay = false;
        d.Frame();
    }
    const std::vector<std::string> lines = d.Trackpad();
    CHECK(lines.size() <= 12);
    CHECK(d.CountTrackpad("trackpad_route_ready") == 1);
    CHECK(d.CountTrackpad("trackpad_mouse_fallback_used") >= 3 && d.CountTrackpad("trackpad_mouse_fallback_used") <= 4);
    CHECK(d.CountTrackpad("trackpad_pointer_interaction") >= 3 && d.CountTrackpad("trackpad_pointer_interaction") <= 4);
    std::set<std::string> unique(lines.begin(), lines.end());
    CHECK(unique.size() == lines.size());

    const std::regex allowed(
        "^Trackpad telemetry: event=trackpad_(route_ready|route_updated) steamdeck=(yes|no) gamescope=(yes|no) native_touchpads=[0-9] "
        "mice=[0-9] mode=(none|native|mouse_fallback) count=1$|"
        "^Trackpad telemetry: event=trackpad_(native_used|mouse_fallback_used|pointer_interaction) count=[1-9]0*$|"
        "^Trackpad telemetry: event=trackpad_unavailable reason=(not_gaming_mode|setting_off|mouse_controls_off) count=[1-9]0*$");
    for (const std::string& line : lines) {
        CHECK(std::regex_match(line, allowed));
        CHECK(line.size() < DCP_TRACKPAD_LINE_SIZE && line.find('\n') == std::string::npos);
        for (const char* forbidden : {"x=", "y=", "dx=", "dy=", "finger", "id=", "serial", "name=", "user", "account", "28de", "11ff", "pos"}) {
            CHECK(!Contains(line, forbidden));
        }
    }

    // The counting rule on its own: the first and every tenth power.
    DcpTrackpadLog log{};
    char line[DCP_TRACKPAD_LINE_SIZE];
    int logged = 0;
    for (int i = 0; i < 100000; ++i) {
        logged += dcp_trackpad_note(&log, DCP_TRACKPAD_EVENT_NATIVE_USED, DCP_TRACKPAD_REASON_NONE, line, sizeof(line)) ? 1 : 0;
    }
    CHECK(logged == 6);
    CHECK(!dcp_trackpad_note(&log, (DcpTrackpadEvent)99, DCP_TRACKPAD_REASON_NONE, line, sizeof(line)));
    CHECK(!dcp_trackpad_note(&log, DCP_TRACKPAD_EVENT_UNAVAILABLE, (DcpTrackpadReason)99, line, sizeof(line)));

    // Route updates are capped.
    DcpTrackpadLog route{};
    const DcpTrackpadEnv env{1, 1, 1};
    int route_lines = 0;
    for (int i = 0; i < 100; ++i) {
        route_lines += dcp_trackpad_route_note(&route, &env, i % 4, i % 3, i % 3, line, sizeof(line)) ? 1 : 0;
    }
    CHECK(route_lines == 1 + DCP_TRACKPAD_MAX_UPDATES);

    // The existing Pointer and Mouse telemetry keep their names.
    const std::string motion = ReadSource("src/dcp_pointer_motion.c");
    CHECK(Contains(motion, "Pointer telemetry: event=") && Contains(motion, "pointer_motion_while_button_held"));
    const std::string mouse_c = ReadSource("src/dcp_mouse.c");
    CHECK(Contains(mouse_c, "pointer_control_always_on_used") && Contains(mouse_c, "Mouse telemetry: event="));
}

// ---- Settings ----

void TestSetting() {
    CHECK(DCP_TRACKPAD_AUTO == 0 && DCP_TRACKPAD_OFF == 1 && DCP_TRACKPAD_SETTING_COUNT == 2);
    CHECK(std::string(dcp_trackpad_key) == "mouse_steam_deck_trackpad");
    CHECK(dcp_trackpad_setting_from_name("auto") == DCP_TRACKPAD_AUTO && dcp_trackpad_setting_from_name("OFF") == DCP_TRACKPAD_OFF);
    CHECK(dcp_trackpad_setting_from_name("on") == -1 && dcp_trackpad_setting_from_name("") == -1 && dcp_trackpad_setting_from_name(nullptr) == -1);
    CHECK(std::string(dcp_trackpad_setting_name(DCP_TRACKPAD_OFF)) == "off" && std::string(dcp_trackpad_setting_label(DCP_TRACKPAD_AUTO)) == "Auto");
    CHECK(std::string(dcp_trackpad_setting_name(7)) == "auto");

    DcpMouseMap map;
    dcp_mouse_defaults(&map);
    CHECK(map.trackpad == DCP_TRACKPAD_AUTO && map.pointer_mode == DCP_POINTER_MODE_HOLD);
    CHECK(dcp_mouse_setting_key("mouse_steam_deck_trackpad"));
    CHECK(dcp_mouse_apply(&map, "mouse_steam_deck_trackpad", "off") && map.trackpad == DCP_TRACKPAD_OFF);
    CHECK(!dcp_mouse_apply(&map, "mouse_steam_deck_trackpad", "sideways") && map.trackpad == DCP_TRACKPAD_OFF);
    CHECK(!dcp_mouse_apply(&map, "mouse_steam_deck_trackpad", "") && map.trackpad == DCP_TRACKPAD_OFF);
    CHECK(dcp_mouse_apply(&map, "mouse_steam_deck_trackpad", "Auto") && map.trackpad == DCP_TRACKPAD_AUTO);

    // From a dcp.ini: off, an unusable value (counted as rejected, default kept) and no line at all.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "dcp-steam-deck-trackpad-tests";
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "dcp.ini").string();
    for (const auto& [text, expected, rejected] : std::vector<std::tuple<std::string, int, int>>{
             {"mouse_steam_deck_trackpad = off\n", DCP_TRACKPAD_OFF, 0},
             {"mouse_steam_deck_trackpad = banana\n", DCP_TRACKPAD_AUTO, 1},
             {"mouse_controls = 1\n", DCP_TRACKPAD_AUTO, 0}}) {
        std::ofstream(path, std::ios::binary) << text;
        DcpMouseMap loaded;
        DcpMouseLoadInfo info;
        dcp_mouse_load_info(path.c_str(), &loaded, &info);
        CHECK(loaded.trackpad == expected && info.rejected == rejected);
    }
    std::filesystem::remove_all(dir);

    // The launcher keeps the key, shows the row only on a Steam Deck, and resets it with the other mouse controls.
    const std::string settings = ReadSource("launcher/settings.cpp");
    CHECK(Contains(settings, "{kMouseSteamDeckTrackpadKey, \"auto\"},"));
    const std::string options = ReadSource("launcher/options.cpp");
    CHECK(Contains(options, "dcp_mouse_apply(&map, dcp_trackpad_key, settings.Get(dcp_trackpad_key).c_str());"));
    CHECK(Contains(options, "settings.Set(dcp_trackpad_key, dcp_trackpad_setting_name(DCP_TRACKPAD_AUTO));"));
    const std::string launcher = ReadSource("launcher/launcher.cpp");
    const std::size_t row = Pos(launcher, "ui.Choice(\"Steam Deck Trackpad\"");
    CHECK(Contains(launcher.substr(row - 800, 800), "dcp_trackpad_steam_deck_value(std::getenv(\"SteamDeck\"))"));
    const std::string portuguese = ReadSource("launcher/localization/pt_br_ui_text.cpp");
    CHECK(Contains(portuguese, "\nSteam Deck Trackpad\t") && Contains(portuguese, "\nAuto\t"));
}

// ---- Window and wiring ----

void TestWindowAndPadWiring() {
    const std::string window = ReadSource("gpu/shim/window.cpp");
    const std::size_t publish = Pos(window, "    LogTouchRoute(DCP_TOUCH_ROUTE_READY);\n    PublishTrackpadEnv();");
    CHECK(Pos(window, "    ClaimGamescopeTouch();\n    LogTouchRoute(DCP_TOUCH_ROUTE_READY);") < publish);
    const std::size_t fn = Pos(window, "void PublishTrackpadEnv() {");
    const std::string body = window.substr(fn, Pos(window, "\n}\n", fn) - fn);
    CHECK(Contains(body, "dcp_trackpad_steam_deck_value(std::getenv(\"SteamDeck\"))"));
    CHECK(Contains(body, "DcpPresent::GamescopeSession()") && Contains(body, "SDL_GetMice(&mice)"));
    CHECK(Contains(body, "#ifdef __linux__") && !Contains(body, "GAMESCOPE_WAYLAND_DISPLAY"));
    CHECK(Contains(window, "const bool settling = dcp_mouse_always_on(mouse) && SDL_GetTicks() < mouse_settle_until;"));
    CHECK(Contains(window, "dcp_mouse_capture_wanted(mouse)"));
    CHECK(Contains(window, "dcp_touchscreen_block(dcp_touchscreen_global(), blocked);"));
    const std::size_t lost = Pos(window, "case SDL_EVENT_WINDOW_FOCUS_LOST:");
    CHECK(Pos(window, "ReleaseMouse();", lost) < Pos(window, "case SDL_EVENT_WINDOW_FOCUS_GAINED:", lost));
    // The touch handler still never reaches the mouse.
    const std::size_t handle = Pos(window, "void WindowSDL::HandleTouch(");
    CHECK(!Contains(window.substr(handle, Pos(window, "\n}\n", handle) - handle), "dcp_mouse_"));

    const std::string mouse_h = ReadSource("src/dcp_mouse.h");
    CHECK(Contains(mouse_h, "void dcp_mouse_trackpad_pads(DcpMouseState *state, int native_touchpads, int native_touching);"));
    const std::string pad = ReadSource("src/runtime_pad.c");
    const std::size_t surface = Pos(pad, "dcp_mouse_controller_surface(dcp_mouse_global(),pad_surface_touched(g));");
    CHECK(surface < Pos(pad, "dcp_mouse_trackpad_pads(") && Pos(pad, "dcp_mouse_trackpad_pads(") < Pos(pad, "dcp_mouse_sample(dcp_mouse_global(),&mouse);"));

    const std::string cmake = ReadSource("CMakeLists.txt");
    CHECK(Contains(cmake, "add_executable(dcp-steam-deck-trackpad-tests EXCLUDE_FROM_ALL tests/steam_deck_trackpad_tests.cpp)"));
    CHECK(Contains(cmake, "add_executable(dcp-touchscreen-steamos-tests EXCLUDE_FROM_ALL tests/touchscreen_steamos_tests.cpp)"));
    CHECK(Contains(cmake, "add_executable(dcp-mouse-tests EXCLUDE_FROM_ALL tests/mouse_input_tests.cpp)"));

    const std::string gate = ReadSource("tools/test.ps1");
    CHECK(Contains(gate, "'dcp-steam-deck-trackpad-tests'") && Contains(gate, "Steam Deck trackpad tests (native touchpad, automatic pointer"));
    CHECK(Contains(gate, " dcp-steam-deck-trackpad-tests ") && Contains(gate, "'dcp-touchscreen-steamos-tests'") && Contains(gate, "'dcp-mouse-tests'"));
    const std::string build = ReadSource("tools/build.sh");
    CHECK(Contains(build, "\"$build_dir/bin/dcp-steam-deck-trackpad-tests\"") && Contains(build, " dcp-steam-deck-trackpad-tests "));
    CHECK(Contains(build, "\"$build_dir/bin/dcp-touchscreen-steamos-tests\"") && Contains(build, "\"$build_dir/bin/dcp-mouse-tests\""));
}

void TestDocs() {
    const std::string linux_doc = ReadSource("docs/LINUX.md");
    CHECK(Contains(linux_doc, "Steam Deck") && Contains(linux_doc, "trackpad") && Contains(linux_doc, "mouse_steam_deck_trackpad"));
    const std::string launcher_doc = ReadSource("docs/LAUNCHER.md");
    CHECK(Contains(launcher_doc, "Steam Deck Trackpad"));
    for (const std::string* doc : {&linux_doc, &launcher_doc}) {
        CHECK(!Contains(*doc, "DCP-FHYNJM"));
    }
}

} // namespace

int main() {
    TestSteamDeckDetection();
    TestVirtualGamepadContext();
    TestNativeTouchpadPick();
    TestNativeRoute();
    TestPointerWithoutHold();
    TestClickIsPointerInteraction();
    TestRemappedInteractionStillRoutes();
    TestOrdinaryMouseOutsideTheRoute();
    TestExternalMouseSafety();
    TestControllerUnchanged();
    TestTouchscreenUnchanged();
    TestTouchscreenAndTrackpadTogether();
    TestCursorOwnership();
    TestOverlayAndTextInputBlocking();
    TestFocusLoss();
    TestLiveSettingChanges();
    TestRouteTelemetry();
    TestUnavailableReasons();
    TestTelemetryBoundsAndPrivacy();
    TestSetting();
    TestWindowAndPadWiring();
    TestDocs();
    std::printf("steam deck trackpad tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
