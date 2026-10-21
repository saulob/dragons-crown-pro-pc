// Cursor visibility tests: the last-device state machine and the controller activity edges. No window, no SDL.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "dcp_cursor.h"

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

using DcpCursor::ControllerState;

constexpr std::uint32_t Bit(int button) {
    return 1u << button;
}

constexpr std::uint32_t kCross = Bit(0);
constexpr std::uint32_t kCircle = Bit(1);
constexpr std::uint32_t kDpadUp = Bit(2);

int TriggerAt(int units) {
    return units * 128;
}

bool Apply(DcpCursor::State& state, bool gameplay = true, bool focused = true, bool overlay = false) {
    state.Update(gameplay, focused, overlay);
    return state.Visible();
}

std::string ReadSource(const char* path) {
    std::ifstream file(std::string(DCP_SOURCE_DIR) + "/" + path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

int Count(const std::string& text, const std::string& needle) {
    int count = 0;
    for (std::size_t pos = 0; (pos = text.find(needle, pos)) != std::string::npos; pos += needle.size()) {
        ++count;
    }
    return count;
}

void TestInitialVisible() {
    DcpCursor::State state;
    CHECK(state.Visible());
    CHECK(state.LastDevice() == DcpCursor::Device::Mouse);
    CHECK(!state.Update(true, true, false).has_value());
    CHECK(state.Visible());
}

void TestMouseActivityShows() {
    DcpCursor::State state;
    state.KeyboardActivity();
    CHECK(!Apply(state));

    state.MouseMoved();
    CHECK(Apply(state));
    state.KeyboardActivity();
    CHECK(!Apply(state));

    state.MouseButtonPressed();
    CHECK(Apply(state));
    state.KeyboardActivity();
    CHECK(!Apply(state));

    state.MouseWheelMoved();
    CHECK(Apply(state));
}

void TestKeyboardActivityHides() {
    DcpCursor::State state;
    CHECK(Apply(state));
    state.KeyboardActivity();
    CHECK(!Apply(state));
}

void TestKeyRepeatAndHostShortcuts() {
    CHECK(DcpCursor::IsGameplayKey(false, false));
    CHECK(!DcpCursor::IsGameplayKey(true, false));
    CHECK(!DcpCursor::IsGameplayKey(false, true));
    CHECK(!DcpCursor::IsGameplayKey(true, true));

    DcpCursor::State state;
    state.KeyboardActivity();
    CHECK(!Apply(state));
    state.KeyboardActivity();
    CHECK(!Apply(state));
    state.MouseMoved();
    CHECK(Apply(state));
}

void TestControllerButtonHides() {
    DcpCursor::State state;
    DcpCursor::ControllerActivityFilter filter;
    ControllerState pad;
    pad.buttons = kCross;
    CHECK(filter.Update(pad));
    state.ControllerActivity();
    CHECK(!Apply(state));

    CHECK(!filter.Update(pad));
    state.MouseMoved();
    CHECK(Apply(state));
    CHECK(!filter.Update(pad));
    CHECK(Apply(state));

    pad.buttons |= kCircle;
    CHECK(filter.Update(pad));
    state.ControllerActivity();
    CHECK(!Apply(state));

    pad.buttons = 0;
    CHECK(!filter.Update(pad));
    pad.buttons = kDpadUp;
    CHECK(filter.Update(pad));
    state.ControllerActivity();
    CHECK(!Apply(state));
}

void TestStickCrossing() {
    DcpCursor::ControllerActivityFilter filter;
    ControllerState pad;
    pad.left_x = DcpCursor::kStickDeadzone - 1;
    CHECK(!filter.Update(pad));
    pad.left_x = DcpCursor::kStickDeadzone;
    CHECK(filter.Update(pad));

    pad.left_x = 0;
    CHECK(!filter.Update(pad));
    pad.left_x = -DcpCursor::kStickDeadzone;
    CHECK(filter.Update(pad));

    pad.left_x = 0;
    CHECK(!filter.Update(pad));
    pad.right_y = DcpCursor::kStickDeadzone;
    CHECK(filter.Update(pad));
}

void TestHeldStickDoesNotOverrideMouse() {
    DcpCursor::State state;
    DcpCursor::ControllerActivityFilter filter;
    ControllerState pad;
    pad.left_x = DcpCursor::kStickDeadzone;
    CHECK(filter.Update(pad));
    state.ControllerActivity();
    CHECK(!Apply(state));

    state.MouseMoved();
    CHECK(Apply(state));
    CHECK(!filter.Update(pad));
    CHECK(Apply(state));

    pad.left_x = 0;
    CHECK(!filter.Update(pad));
    pad.left_x = DcpCursor::kStickDeadzone;
    CHECK(filter.Update(pad));
    state.ControllerActivity();
    CHECK(!Apply(state));
}

void TestTriggerCrossing() {
    DcpCursor::ControllerActivityFilter filter;
    ControllerState pad;
    pad.left_trigger = TriggerAt(DcpCursor::kTriggerThreshold - 1);
    CHECK(!filter.Update(pad));
    pad.left_trigger = TriggerAt(DcpCursor::kTriggerThreshold);
    CHECK(filter.Update(pad));

    pad.left_trigger = 0;
    CHECK(!filter.Update(pad));
    pad.right_trigger = TriggerAt(DcpCursor::kTriggerThreshold);
    CHECK(filter.Update(pad));
}

void TestHeldTriggerDoesNotOverrideMouse() {
    DcpCursor::State state;
    DcpCursor::ControllerActivityFilter filter;
    ControllerState pad;
    pad.left_trigger = TriggerAt(DcpCursor::kTriggerThreshold);
    CHECK(filter.Update(pad));
    state.ControllerActivity();
    CHECK(!Apply(state));

    state.MouseMoved();
    CHECK(Apply(state));
    CHECK(!filter.Update(pad));
    CHECK(Apply(state));

    pad.left_trigger = TriggerAt(DcpCursor::kTriggerThreshold - 1);
    CHECK(!filter.Update(pad));
    pad.left_trigger = TriggerAt(DcpCursor::kTriggerThreshold);
    CHECK(filter.Update(pad));
    state.ControllerActivity();
    CHECK(!Apply(state));
}

void TestOverlayForcesVisible() {
    DcpCursor::State state;
    state.ControllerActivity();
    CHECK(!Apply(state));
    CHECK(Apply(state, true, true, true));
    CHECK(Apply(state, true, true, true));
    CHECK(!Apply(state, true, true, false));
    CHECK(state.LastDevice() == DcpCursor::Device::Controller);
}

void TestUiInputKeepsLastDevice() {
    DcpCursor::State state;
    state.ControllerActivity();
    CHECK(!Apply(state));
    CHECK(Apply(state, true, true, true));

    state.MouseMoved();
    state.MouseButtonPressed();
    state.KeyboardActivity();
    CHECK(Apply(state, true, true, true));
    CHECK(!Apply(state, true, true, false));
    CHECK(state.LastDevice() == DcpCursor::Device::Controller);

    DcpCursor::State keyboard;
    keyboard.KeyboardActivity();
    CHECK(!Apply(keyboard));
    CHECK(Apply(keyboard, true, true, true));
    keyboard.MouseMoved();
    CHECK(Apply(keyboard, true, true, true));
    CHECK(!Apply(keyboard, true, true, false));
    CHECK(keyboard.LastDevice() == DcpCursor::Device::Keyboard);
}

void TestFocus() {
    DcpCursor::State state;
    state.ControllerActivity();
    CHECK(!Apply(state));
    CHECK(Apply(state, true, false, false));
    CHECK(!Apply(state, true, true, false));

    CHECK(Apply(state, true, false, true));
    CHECK(Apply(state, true, true, true));
    CHECK(!Apply(state, true, true, false));
}

void TestWindowedAndBorderless() {
    // Both modes share the same path.
    DcpCursor::State windowed;
    windowed.ControllerActivity();
    CHECK(!Apply(windowed));
    windowed.MouseMoved();
    CHECK(Apply(windowed));

    DcpCursor::State borderless;
    borderless.ControllerActivity();
    CHECK(!Apply(borderless));
    borderless.MouseMoved();
    CHECK(Apply(borderless));

    const std::string window = ReadSource("gpu/shim/window.cpp");
    CHECK(!window.empty());
    CHECK(!Contains(window, "SDL_WINDOW_FULLSCREEN"));
}

void TestLauncherUnaffected() {
    DcpCursor::State state;
    state.KeyboardActivity();
    CHECK(Apply(state, false, true, false));
    state.ControllerActivity();
    CHECK(Apply(state, false, true, false));
    CHECK(state.LastDevice() == DcpCursor::Device::Mouse);

    const std::string launcher = ReadSource("launcher/host.cpp");
    CHECK(!launcher.empty());
    CHECK(!Contains(launcher, "DcpCursor"));
}

void TestControllersIndependent() {
    DcpCursor::ControllerActivityFilter first;
    DcpCursor::ControllerActivityFilter second;
    ControllerState a;
    ControllerState b;
    a.buttons = kCross;
    CHECK(first.Update(a));
    CHECK(!first.Update(a));

    b.buttons = kCircle;
    CHECK(second.Update(b));
    CHECK(!second.Update(b));

    a.buttons |= kCircle;
    CHECK(first.Update(a));
    CHECK(!second.Update(b));
}

void TestNoTimeout() {
    DcpCursor::State state;
    state.MouseMoved();
    for (int i = 0; i < 10000; ++i) {
        state.Update(true, true, false);
    }
    CHECK(state.Visible());

    state.KeyboardActivity();
    CHECK(!Apply(state));
    for (int i = 0; i < 10000; ++i) {
        state.Update(true, true, false);
    }
    CHECK(!state.Visible());

    const std::string header = ReadSource("gpu/shim/dcp_cursor.h");
    CHECK(!header.empty());
    CHECK(!Contains(header, "chrono"));
    CHECK(!Contains(header, "kHideDelay"));
    CHECK(!Contains(header, "last_motion"));

    const std::string window = ReadSource("gpu/shim/window.cpp");
    CHECK(!window.empty());
    CHECK(!Contains(window, "kHideDelay"));
    CHECK(!Contains(window, "last_motion"));
}

void TestNoSecondGamepadPath() {
    const std::string header = ReadSource("gpu/shim/dcp_cursor.h");
    CHECK(!header.empty());
    CHECK(!Contains(header, "SDL_"));

    const std::string window = ReadSource("gpu/shim/window.cpp");
    CHECK(!window.empty());
    CHECK(!Contains(window, "SDL_GetGamepads"));
    CHECK(!Contains(window, "SDL_OpenGamepad"));
    CHECK(Count(window, "SDL_InitSubSystem") == 1);
    CHECK(Count(window, "SDL_INIT_GAMEPAD") == 1);
}

} // namespace

int main() {
    TestInitialVisible();
    TestMouseActivityShows();
    TestKeyboardActivityHides();
    TestKeyRepeatAndHostShortcuts();
    TestControllerButtonHides();
    TestStickCrossing();
    TestHeldStickDoesNotOverrideMouse();
    TestTriggerCrossing();
    TestHeldTriggerDoesNotOverrideMouse();
    TestOverlayForcesVisible();
    TestUiInputKeepsLastDevice();
    TestFocus();
    TestWindowedAndBorderless();
    TestLauncherUnaffected();
    TestControllersIndependent();
    TestNoTimeout();
    TestNoSecondGamepadPath();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
