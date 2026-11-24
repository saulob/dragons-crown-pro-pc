// Focused tests of the window: the windowed-mode fit (fits are kept, oversized windows shrink by one factor, centring) and
// the Exit Game shortcuts (keyboard press rule, the L1 + R1 + Options hold of each controller, one close request).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "dcp_exit_shortcut.h"
#include "dcp_window_fit.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);          \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

struct Area {
    int x, y, w, h;
};

const dcp::WindowBorders kBars{31, 1, 1, 1};
const dcp::WindowBorders kWin10{31, 8, 8, 8};

dcp::WindowFit Fit(int w, int h, dcp::WindowBorders borders, const Area& a) {
    return dcp::FitWindowToArea(w, h, borders, a.x, a.y, a.w, a.h);
}

void TestFittingSizeIsKept() {
    // 1920x1200 monitor, 1160 usable rows: the reported case, with Windows 10's invisible side frames.
    const dcp::WindowFit f = Fit(1920, 1080, kWin10, {0, 0, 1920, 1160});
    CHECK(f.width == 1920 && f.height == 1080);
    CHECK(f.y == (1160 - (1080 + 39)) / 2 + 31);
    CHECK(Fit(1280, 720, kWin10, {0, 0, 1920, 1160}).width == 1280);
    CHECK(Fit(1280, 720, kBars, {0, 0, 3840, 2100}).height == 720);
    const dcp::WindowFit exact = Fit(1920, 1000, kBars, {0, 0, 1920, 1032});
    CHECK(exact.width == 1920 && exact.height == 1000);
}

void TestOversizedShrinksProportionally() {
    const Area areas[] = {{0, 0, 1366, 728}, {0, 0, 1920, 1040}, {0, 0, 2560, 1400}};
    const int sources[][2] = {{1920, 1080}, {2560, 1440}, {3840, 2160}, {5120, 1440}};
    for (const Area& a : areas) {
        for (const auto& s : sources) {
            const dcp::WindowFit f = Fit(s[0], s[1], kBars, a);
            CHECK(f.width >= 1 && f.height >= 1);
            CHECK(f.width <= s[0] && f.height <= s[1]);
            CHECK(f.width <= a.w && f.height + 32 <= a.h);
            CHECK(std::abs(static_cast<long long>(f.width) * s[1] - static_cast<long long>(f.height) * s[0]) <= s[0]);
            CHECK(f.y - 31 >= a.y && f.y + f.height + 1 <= a.y + a.h);
        }
    }
    const dcp::WindowFit f = Fit(1920, 1080, kBars, {0, 0, 1366, 728});
    CHECK(f.height + 32 <= 728 && f.height >= 690 - 32);
}

void TestUltrawideKeepsShape() {
    const dcp::WindowFit f = Fit(3440, 1440, kBars, {0, 0, 1920, 1040});
    CHECK(f.width == 1920);
    CHECK(std::abs(f.width * 1440 - f.height * 3440) <= 3440);
    const dcp::WindowFit tall = Fit(3440, 1440, kBars, {0, 0, 3840, 700});
    CHECK(tall.height + 32 <= 700);
    CHECK(std::abs(tall.width * 1440 - tall.height * 3440) <= 3440);
}

void TestScaleOnlyChangesTheEstimate() {
    CHECK(dcp::EstimatedBorders(1.0f).top == 40);
    CHECK(dcp::EstimatedBorders(1.25f).top == 50);
    CHECK(dcp::EstimatedBorders(1.5f).top == 60);
    CHECK(dcp::EstimatedBorders(2.0f).top == 80);
    CHECK(dcp::EstimatedBorders(0.0f).top == 40);
    for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        const dcp::WindowFit f = Fit(1920, 1080, dcp::EstimatedBorders(scale), {0, 0, 3840, 2100});
        CHECK(f.width == 1920 && f.height == 1080);
    }
}

void TestDisplayScales() {
    CHECK(dcp::EstimatedBorders(2.25f).top == 90);
    for (const float scale : {1.0f, 1.5f, 2.25f}) {
        const dcp::WindowBorders borders = dcp::EstimatedBorders(scale);
        const dcp::WindowFit roomy = Fit(1920, 1080, borders, {0, 0, 3840, 2100});
        CHECK(roomy.width == 1920 && roomy.height == 1080);
        const dcp::WindowFit tight = Fit(1920, 1080, borders, {0, 0, 1920, 1040});
        CHECK(tight.width <= 1920 && tight.height + borders.top <= 1040);
        CHECK(std::abs(tight.width * 1080 - tight.height * 1920) <= 1920);
        CHECK(tight.y - borders.top >= 0 && tight.y + tight.height <= 1040);
    }
}

void TestCentring() {
    const dcp::WindowFit f = Fit(1280, 720, kBars, {0, 0, 1920, 1040});
    const int outer_w = f.width + 2, outer_h = f.height + 32;
    CHECK(f.x - 1 == (1920 - outer_w) / 2);
    CHECK(f.y - 31 == (1040 - outer_h) / 2);

    // A monitor left of and above the primary has negative origin coordinates.
    const dcp::WindowFit n = Fit(1920, 1080, kBars, {-2560, -200, 2560, 1400});
    CHECK(n.x - 1 >= -2560 && n.x + n.width + 1 <= 0);
    CHECK(n.y - 31 >= -200 && n.y + n.height + 1 <= 1200);
    CHECK((n.x - 1) - (-2560) == (2560 - (n.width + 2)) / 2);
}

void TestDegenerateInput() {
    const dcp::WindowFit empty_area = Fit(1920, 1080, kBars, {0, 0, 0, 0});
    CHECK(empty_area.width == 1920 && empty_area.height == 1080);
    const dcp::WindowFit tiny = Fit(1920, 1080, kBars, {0, 0, 100, 20});
    CHECK(tiny.width >= 1 && tiny.height >= 1);
    const dcp::WindowFit zero = Fit(0, 0, kBars, {0, 0, 1920, 1040});
    CHECK(zero.width >= 1 && zero.height >= 1);
}

void TestConfiguredSizeUntouched() {
    const int configured_w = 1920, configured_h = 1080;
    const dcp::WindowFit f = Fit(configured_w, configured_h, kBars, {0, 0, 1920, 1040});
    CHECK(f.height < configured_h);
    CHECK(configured_w == 1920 && configured_h == 1080);
}

struct Chord {
    DcpExitChord state{};
    int closes = 0;
    DcpExitChordEvent last = DCP_EXIT_CHORD_NONE;
    uint32_t held_ms = 0;

    // One sample of one controller; a completed hold requests the close once, as the window does.
    DcpExitChordEvent Step(uint32_t pad, bool l1, bool r1, bool options, uint64_t now_ms, bool enabled = true) {
        last = dcp_exit_chord_update(&state, pad, enabled, l1, r1, options, now_ms, &held_ms);
        if (last == DCP_EXIT_CHORD_COMPLETED) {
            ++closes;
        }
        return last;
    }
    DcpExitChordEvent Hold(uint32_t pad, uint64_t now_ms) { return Step(pad, true, true, true, now_ms); }
    DcpExitChordEvent Release(uint32_t pad, uint64_t now_ms) { return Step(pad, false, false, false, now_ms); }
};

void TestExitKeyboardRoute() {
    constexpr int kF9 = 66, kF10 = 67; // SDL scancodes: the rule never looks at what a number means
    int closes = 0;
    const auto press = [&](int scancode, bool repeat, int bound) {
        if (dcp_exit_key_pressed(scancode, repeat, bound)) {
            ++closes;
        }
    };
    press(kF9, false, kF9);
    CHECK(closes == 1);
    press(kF9, true, kF9);
    press(kF9, true, kF9);
    CHECK(closes == 1); // a held key repeats; only the press counts
    press(kF9, false, kF9);
    CHECK(closes == 2); // the next press is the next request
    press(kF10, false, kF9);
    CHECK(closes == 2); // another key
    press(kF9, false, kF10);
    CHECK(closes == 2); // a rebound key: the old one no longer closes
    press(kF10, false, kF10);
    CHECK(closes == 3);
    CHECK(!dcp_exit_key_pressed(0, false, 0) && !dcp_exit_key_pressed(kF9, false, 0) && !dcp_exit_key_pressed(kF9, false, -1));

    // The keyboard route does not look at the controller shortcut at all.
    Chord off;
    CHECK(off.Step(1, true, true, true, 0, false) == DCP_EXIT_CHORD_NONE &&
          off.Step(1, true, true, true, 5000, false) == DCP_EXIT_CHORD_NONE);
    CHECK(off.closes == 0 && dcp_exit_key_pressed(kF9, false, kF9));
}

void TestExitChordHold() {
    Chord c;
    CHECK(c.Hold(1, 1000) == DCP_EXIT_CHORD_STARTED);
    CHECK(c.Hold(1, 1500) == DCP_EXIT_CHORD_NONE && c.Hold(1, 2999) == DCP_EXIT_CHORD_NONE && c.closes == 0);
    CHECK(c.Hold(1, 3000) == DCP_EXIT_CHORD_COMPLETED && c.closes == 1 && c.held_ms == DCP_EXIT_CHORD_HOLD_MS);
    CHECK(DCP_EXIT_CHORD_HOLD_MS == 2000u);

    // One request per hold, however long the chord stays down.
    for (uint64_t t = 3001; t < 9000; t += 250) {
        CHECK(c.Hold(1, t) == DCP_EXIT_CHORD_NONE);
    }
    CHECK(c.closes == 1);

    // Released and held again: a new hold, a new request.
    CHECK(c.Release(1, 9000) == DCP_EXIT_CHORD_NONE);
    CHECK(c.Hold(1, 9100) == DCP_EXIT_CHORD_STARTED && c.Hold(1, 11099) == DCP_EXIT_CHORD_NONE);
    CHECK(c.Hold(1, 11100) == DCP_EXIT_CHORD_COMPLETED && c.closes == 2);

    // A late sample reports the real time held.
    Chord late;
    late.Hold(1, 0);
    CHECK(late.Hold(1, 2600) == DCP_EXIT_CHORD_COMPLETED && late.held_ms == 2600);
}

void TestExitChordReleaseResets() {
    Chord c;
    c.Hold(1, 0);
    c.Hold(1, 1900);
    CHECK(c.Release(1, 1950) == DCP_EXIT_CHORD_NONE);
    CHECK(c.Hold(1, 2000) == DCP_EXIT_CHORD_STARTED); // the clock restarts: 2000 ms after the first press is not enough
    CHECK(c.Hold(1, 3999) == DCP_EXIT_CHORD_NONE && c.closes == 0);
    CHECK(c.Hold(1, 4000) == DCP_EXIT_CHORD_COMPLETED && c.closes == 1);

    // Releasing any one of the three ends the hold.
    for (int released = 0; released < 3; ++released) {
        Chord each;
        each.Hold(1, 0);
        const bool l1 = released != 0, r1 = released != 1, options = released != 2;
        CHECK(each.Step(1, l1, r1, options, 1000) == DCP_EXIT_CHORD_NONE);
        CHECK(each.Step(1, l1, r1, options, 5000) == DCP_EXIT_CHORD_NONE && each.closes == 0);
        CHECK(each.Hold(1, 5100) == DCP_EXIT_CHORD_STARTED); // the three together again start over
        CHECK(each.Hold(1, 7099) == DCP_EXIT_CHORD_NONE && each.Hold(1, 7100) == DCP_EXIT_CHORD_COMPLETED);
    }

    // The timer starts only when all three are down: pressing them one after another does not count the early ones.
    Chord staged;
    CHECK(staged.Step(1, true, false, false, 0) == DCP_EXIT_CHORD_NONE &&
          staged.Step(1, true, true, false, 900) == DCP_EXIT_CHORD_NONE);
    CHECK(staged.Hold(1, 1000) == DCP_EXIT_CHORD_STARTED);
    CHECK(staged.Hold(1, 2999) == DCP_EXIT_CHORD_NONE && staged.Hold(1, 3000) == DCP_EXIT_CHORD_COMPLETED);

    // Any two buttons never count.
    Chord partial;
    for (uint64_t t = 0; t < 10000; t += 100) {
        partial.Step(1, true, true, false, t);
        partial.Step(1, true, false, true, t);
        partial.Step(1, false, true, true, t);
    }
    CHECK(partial.closes == 0);
    CHECK(!dcp_exit_chord_down(1, 1, 1, 0) && !dcp_exit_chord_down(1, 0, 1, 1) && !dcp_exit_chord_down(1, 1, 0, 1));
    CHECK(dcp_exit_chord_down(1, 1, 1, 1) && !dcp_exit_chord_down(0, 1, 1, 1));
}

void TestExitChordPerController() {
    // Player 1 holds L1, player 2 holds R1, player 3 holds Options: no controller holds the chord.
    Chord c;
    for (uint64_t t = 0; t < 6000; t += 100) {
        c.Step(1, true, false, false, t);
        c.Step(2, false, true, false, t);
        c.Step(3, false, false, true, t);
    }
    CHECK(c.closes == 0);

    // Each controller is timed alone; a second one starting later does not borrow the first one's time.
    Chord two;
    CHECK(two.Hold(1, 0) == DCP_EXIT_CHORD_STARTED && two.Hold(2, 1500) == DCP_EXIT_CHORD_STARTED);
    CHECK(two.Hold(1, 1999) == DCP_EXIT_CHORD_NONE && two.Hold(2, 1999) == DCP_EXIT_CHORD_NONE);
    CHECK(two.Hold(1, 2000) == DCP_EXIT_CHORD_COMPLETED && two.closes == 1);
    CHECK(two.Hold(2, 3499) == DCP_EXIT_CHORD_NONE);
    two.Release(2, 3500);
    CHECK(two.Hold(2, 3600) == DCP_EXIT_CHORD_STARTED && two.closes == 1);

    // Any connected controller may trigger it.
    Chord any;
    any.Hold(7, 0);
    CHECK(any.Hold(7, 2000) == DCP_EXIT_CHORD_COMPLETED && any.closes == 1);
    Chord more;
    for (uint32_t pad = 1; pad <= DCP_EXIT_CHORD_SLOTS; ++pad) {
        CHECK(more.Hold(pad, 0) == DCP_EXIT_CHORD_STARTED);
    }
    CHECK(more.Hold(DCP_EXIT_CHORD_SLOTS + 1, 0) == DCP_EXIT_CHORD_NONE); // a table that is full ignores the extra pad
    more.Release(3, 100);
    CHECK(more.Hold(99, 200) == DCP_EXIT_CHORD_STARTED); // and a released controller frees its slot
}

void TestExitChordDisconnectAndFocus() {
    Chord c;
    c.Hold(1, 0);
    c.Hold(1, 1900);
    dcp_exit_chord_forget(&c.state, 1); // unplugged
    CHECK(c.Hold(1, 2000) == DCP_EXIT_CHORD_STARTED && c.closes == 0); // a controller that comes back starts over
    CHECK(c.Hold(1, 3999) == DCP_EXIT_CHORD_NONE && c.Hold(1, 4000) == DCP_EXIT_CHORD_COMPLETED);
    dcp_exit_chord_forget(&c.state, 42); // never seen: nothing happens

    // Focus lost: the hold ends, and a chord still held when focus returns must be released first.
    Chord focus;
    focus.Hold(1, 0);
    focus.Hold(1, 1500);
    dcp_exit_chord_cancel(&focus.state);
    CHECK(focus.Hold(1, 2500) == DCP_EXIT_CHORD_NONE && focus.Hold(1, 9000) == DCP_EXIT_CHORD_NONE && focus.closes == 0);
    focus.Release(1, 9100);
    CHECK(focus.Hold(1, 9200) == DCP_EXIT_CHORD_STARTED);
    CHECK(focus.Hold(1, 11200) == DCP_EXIT_CHORD_COMPLETED && focus.closes == 1);

    // Cancelling with nothing held leaves nothing behind.
    Chord idle;
    dcp_exit_chord_cancel(&idle.state);
    CHECK(idle.Hold(1, 0) == DCP_EXIT_CHORD_STARTED && idle.Hold(1, 2000) == DCP_EXIT_CHORD_COMPLETED);

    // The setting turned Off ends every hold; On again starts a new one.
    Chord setting;
    setting.Hold(1, 0);
    CHECK(setting.Step(1, true, true, true, 1000, false) == DCP_EXIT_CHORD_NONE);
    CHECK(setting.Step(1, true, true, true, 5000, false) == DCP_EXIT_CHORD_NONE && setting.closes == 0);
    CHECK(setting.Hold(1, 5100) == DCP_EXIT_CHORD_STARTED);
    dcp_exit_chord_clear(&setting.state);
    CHECK(setting.Hold(1, 5200) == DCP_EXIT_CHORD_STARTED);

    // A clock that steps back never completes a hold.
    Chord clock;
    clock.Hold(1, 5000);
    CHECK(clock.Hold(1, 100) == DCP_EXIT_CHORD_NONE);
}

void TestExitTelemetry() {
    DcpExitTelemetry telemetry{};
    char line[DCP_EXIT_LINE_SIZE];
    CHECK(dcp_exit_telemetry_note(&telemetry, DCP_EXIT_EVENT_KEYBOARD_USED, 0, line, sizeof(line)) &&
          std::string(line) == "Exit shortcut telemetry: event=keyboard_used count=1");
    CHECK(dcp_exit_telemetry_note(&telemetry, DCP_EXIT_EVENT_CONTROLLER_HOLD_STARTED, 0, line, sizeof(line)) &&
          std::string(line) == "Exit shortcut telemetry: event=controller_hold_started count=1");
    CHECK(dcp_exit_telemetry_note(&telemetry, DCP_EXIT_EVENT_CONTROLLER_USED, 2004, line, sizeof(line)) &&
          std::string(line) == "Exit shortcut telemetry: event=controller_used hold_ms=2000 count=1");
    CHECK(dcp_exit_telemetry_note(&telemetry, DCP_EXIT_EVENT_CONTROLLER_USED, 2006, line, sizeof(line)) &&
          std::string(line) == "Exit shortcut telemetry: event=controller_used hold_ms=2010 count=2");
    int started_lines = 1;
    for (int i = 0; i < 100; ++i) {
        started_lines += dcp_exit_telemetry_note(&telemetry, DCP_EXIT_EVENT_CONTROLLER_HOLD_STARTED, 0, line, sizeof(line));
    }
    CHECK(started_lines == DCP_EXIT_START_LINES_MAX); // restarts do not flood the log
    CHECK(!dcp_exit_telemetry_note(&telemetry, DCP_EXIT_EVENT_COUNT, 0, line, sizeof(line)));
    dcp_exit_shortcut_ready_line(line, sizeof(line), "F9", 1);
    CHECK(std::string(line) == "Exit shortcut: keyboard key=F9 controller=on");
    dcp_exit_shortcut_ready_line(line, sizeof(line), "", 0);
    CHECK(std::string(line) == "Exit shortcut: keyboard key=none controller=off");
}

std::string ReadSource(const char* relative) {
    std::ifstream in(std::string(DCP_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

size_t CountOf(const std::string& text, const std::string& what) {
    size_t count = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) {
        ++count;
    }
    return count;
}

// The one clean close path, and what the shortcut code must never do.
void TestExitSourcesShareTheCloseRequest() {
    const std::string window = ReadSource("gpu/shim/window.cpp");
    const std::string glue = ReadSource("gpu/shim/dcpgpu.cpp");
    const std::string loader = ReadSource("src/loader.c");
    CHECK(CountOf(window, "if (!dcpgpu_request_close()) {") == 1); // both routes go through RequestExitShortcut
    CHECK(window.find("RequestExitShortcut(DCP_EXIT_EVENT_KEYBOARD_USED, 0);") != std::string::npos);
    CHECK(window.find("RequestExitShortcut(DCP_EXIT_EVENT_CONTROLLER_USED, held_ms);") != std::string::npos);
    CHECK(loader.find("exit_game_set_handler(dcpgpu_request_close);") != std::string::npos);
    CHECK(glue.find("g_window->RequestClose()") != std::string::npos &&
          glue.find("Exit Game: host exit scheduled") != std::string::npos);
    CHECK(window.find("Exit Game: hard exit") != std::string::npos);

    const size_t begin = window.find("void WindowSDL::RequestExitShortcut");
    const size_t end = window.find("unsigned WindowSDL::HeldButtons");
    CHECK(begin != std::string::npos && begin < end);
    const std::string shortcut = window.substr(begin, end - begin);
    for (const char* forbidden : {"exit(", "_Exit(", "abort(", "TerminateProcess", "SDL_Quit", "SDL_Delay", "sleep"}) {
        CHECK(shortcut.find(forbidden) == std::string::npos);
    }
    const std::string policy = ReadSource("src/dcp_exit_shortcut.h");
    for (const char* forbidden : {"exit(", "_Exit(", "abort(", "TerminateProcess", "SDL_", "frame"}) {
        CHECK(policy.find(forbidden) == std::string::npos);
    }

    // The keyboard route runs on the window thread after the text entry, before the menus, on the press edge.
    const size_t text = window.find("if (text_active && (event.type == SDL_EVENT_TEXT_INPUT");
    const size_t key = window.find("dcp_exit_key_pressed(event.key.scancode, event.key.repeat, DcpSettings::Get().exit_key)");
    const size_t overlay = window.find("if (DcpOverlay::HandleEvent(event)) {");
    CHECK(text != std::string::npos && text < key && key < overlay);
    CHECK(window.find("SDL_EVENT_WINDOW_FOCUS_LOST") != std::string::npos &&
          window.find("dcp_exit_chord_cancel(&exit_chord);") != std::string::npos);
    CHECK(window.find("dcp_exit_chord_forget(&exit_chord, event.gdevice.which);") != std::string::npos);
    CHECK(window.find("dcp_exit_chord_clear(&exit_chord);") != std::string::npos);
    CHECK(window.find("SDL_GetTicks()") != std::string::npos);
    CHECK(window.find("    UpdateExitChord();\n    UpdateHighlightChord();\n    UpdateMouseCapture();") != std::string::npos);
    // It judges each controller from its own buttons, by SDL's standard names: no vendor ids, no Guide, Share or Touchpad.
    const size_t update = window.find("void WindowSDL::UpdateExitChord()");
    const size_t update_end = window.find("\n}\n", update);
    CHECK(update != std::string::npos && update_end != std::string::npos && update_end < end);
    const std::string tick = window.substr(update, update_end - update);
    CHECK(CountOf(tick, "SDL_GetGamepadButton(gamepad, ") == 3);
    CHECK(tick.find("SDL_GAMEPAD_BUTTON_LEFT_SHOULDER") != std::string::npos &&
          tick.find("SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER") != std::string::npos &&
          tick.find("SDL_GAMEPAD_BUTTON_START") != std::string::npos);
    for (const char* forbidden : {"GUIDE", "MISC1", "TOUCHPAD", "BACK", "Vendor", "0x28de", "SDL_GetGamepadName", "GUID"}) {
        CHECK(tick.find(forbidden) == std::string::npos);
    }
}

void TestExitPadHidesTheChord() {
    const std::string pad = ReadSource("src/runtime_pad.c");
    const size_t chord = pad.find("const bool exit_chord=dcp_exit_chord_down(dcpgpu_controller_exit_shortcut(),");
    CHECK(chord != std::string::npos);
    const size_t loop = pad.find("for (size_t i=0;i<pad_map.count;++i) {", chord);
    const std::string head = pad.substr(chord, loop - chord);
    // One controller's own three buttons, by SDL's names; while all are down the guest sees none of them.
    CHECK(CountOf(head, "SDL_GetGamepadButton(g,") == 3);
    CHECK(head.find("SDL_GAMEPAD_BUTTON_LEFT_SHOULDER") != std::string::npos &&
          head.find("SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER") != std::string::npos &&
          head.find("SDL_GAMEPAD_BUTTON_START") != std::string::npos);
    CHECK(head.find("if (exit_chord) slots[slot].hold_after_capture=1;") != std::string::npos);
    const size_t skip = pad.find("if (exit_chord && (source==SDL_GAMEPAD_BUTTON_LEFT_SHOULDER", loop);
    CHECK(skip != std::string::npos && skip < pad.find("SDL_GetGamepadButton(g,(SDL_GamepadButton)source)", loop));
    // Nothing is skipped unless the chord is down, and the Options menu chord keeps its own rule.
    CHECK(CountOf(pad, "source==SDL_GAMEPAD_BUTTON_LEFT_SHOULDER") == 1);
    CHECK(CountOf(pad, "if (chord && (source==SDL_GAMEPAD_BUTTON_LEFT_STICK || source==SDL_GAMEPAD_BUTTON_RIGHT_STICK)) continue;") == 1);
    CHECK(pad.find("L3 and R3 together belong to the Options menu") != std::string::npos);
    // What is still held after an aborted chord stays hidden until released, like after a menu closes.
    const size_t sample = pad.find("static void sample(int slot, PadData *d) {");
    const size_t gate = pad.find("if (s->hold_after_capture) {", sample);
    CHECK(sample != std::string::npos && gate != std::string::npos);
    CHECK(pad.find("if (d->buttons) d->buttons=0;", gate) < pad.find("else s->hold_after_capture=0;", gate));
    for (const char* forbidden : {"GUIDE", "0x28de"}) {
        CHECK(head.find(forbidden) == std::string::npos);
    }
    // The setting is read from the video core, never a fixed value.
    CHECK(ReadSource("gpu/shim/dcpgpu.cpp").find("return DcpSettings::Get().controller_exit_shortcut ? 1 : 0;") != std::string::npos);
    CHECK(ReadSource("src/dcpgpu.h").find("int dcpgpu_controller_exit_shortcut(void);") != std::string::npos);
}

void TestExitSettingsAreHostLevel() {
    const std::string settings = ReadSource("gpu/shim/dcp_settings.h");
    CHECK(settings.find("exit_key{SDL_SCANCODE_F9}") != std::string::npos &&
          settings.find("controller_exit_shortcut{true}") != std::string::npos);
    const std::string reader = ReadSource("gpu/shim/dcp_settings.cpp");
    CHECK(reader.find("key == \"controller_exit_shortcut\"") != std::string::npos);
    CHECK(reader.find("&v.window_mode_key, &v.comms_key, &v.exit_key, &v.character_highlight_key}") != std::string::npos);
    // Not a game feature: no build, locator, hook, language or Online Play test anywhere in the shortcut code.
    const std::string policy = ReadSource("src/dcp_exit_shortcut.h");
    for (const char* forbidden : {"cusa", "CUSA", "locator", "online", "language", "feature", "rva", "guest"}) {
        CHECK(policy.find(forbidden) == std::string::npos);
    }
    const std::string window = ReadSource("gpu/shim/window.cpp");
    const size_t begin = window.find("void WindowSDL::RequestExitShortcut");
    const size_t end = window.find("unsigned WindowSDL::HeldButtons");
    const std::string shortcut = window.substr(begin, end - begin);
    for (const char* forbidden : {"build", "locator", "runtime_online_play_enabled", "language", "feature"}) {
        CHECK(shortcut.find(forbidden) == std::string::npos);
    }
}

} // namespace

int main() {
    TestFittingSizeIsKept();
    TestOversizedShrinksProportionally();
    TestUltrawideKeepsShape();
    TestScaleOnlyChangesTheEstimate();
    TestDisplayScales();
    TestCentring();
    TestDegenerateInput();
    TestConfiguredSizeUntouched();
    TestExitKeyboardRoute();
    TestExitChordHold();
    TestExitChordReleaseResets();
    TestExitChordPerController();
    TestExitChordDisconnectAndFocus();
    TestExitTelemetry();
    TestExitSourcesShareTheCloseRequest();
    TestExitPadHidesTheChord();
    TestExitSettingsAreHostLevel();
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
