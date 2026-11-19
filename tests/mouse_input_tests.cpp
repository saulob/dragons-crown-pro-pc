// Focused tests of the mouse and touchscreen input: bindings and their settings, Pointer Control's synthetic touch finger,
// touchscreen contacts and mapping, the capture rules and the source-level safety of the SDL handoff. No window, no SDL
// video, no game.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "dcp_cursor.h"
#include "dcp_keys.h"
#include "dcp_mouse.h"
#include "dcp_mouse_capture.h"
#include "dcp_pad_buttons.h"
#include "dcp_touch_route.h"
#include "dcp_touchscreen.h"

namespace fs = std::filesystem;

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

// A state with the given map; the tests drive it as the window thread and the pad sampler do.
struct Rig {
    DcpMouseState* state = dcp_mouse_state_new();
    DcpMouseMap map{};

    Rig() {
        dcp_mouse_defaults(&map);
        dcp_mouse_configure(state, &map);
    }
    ~Rig() { dcp_mouse_state_free(state); }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    void Configure() { dcp_mouse_configure(state, &map); }
    void Down(int button) { dcp_mouse_button(state, button, 1); }
    void Up(int button) { dcp_mouse_button(state, button, 0); }
    void Move(float dx, float dy) { dcp_mouse_motion(state, dx, dy); }
    DcpMouseSample Read() {
        DcpMouseSample sample;
        dcp_mouse_sample(state, &sample);
        return sample;
    }
};

// The game's rule for its pointer (CUSA10454 1.00 0x5a2b03): a finger moves it by the difference between its positions on
// two consecutive reads, and only when a finger was on the surface on both.
struct Game {
    bool previous = false;
    int x = 0, y = 0;
    double pointer_x = 0, pointer_y = 0;
    int segments = 0; // fingers the game has seen

    void Read(const DcpMouseSample& sample) {
        if (sample.touch && !previous) {
            ++segments;
        }
        if (sample.touch && previous) {
            pointer_x += sample.x - x;
            pointer_y += sample.y - y;
        }
        previous = sample.touch != 0;
        if (sample.touch) {
            x = sample.x;
            y = sample.y;
        }
    }
};

constexpr int kCenterX = DCP_TOUCH_MAX_X / 2;
constexpr int kCenterY = DCP_TOUCH_MAX_Y / 2;

void TestDefaultsAndNames() {
    DcpMouseMap map;
    dcp_mouse_defaults(&map);
    CHECK(map.button[DCP_MOUSE_SQUARE] == DCP_MOUSE_LEFT);
    CHECK(map.button[DCP_MOUSE_CIRCLE] == DCP_MOUSE_RIGHT);
    CHECK(map.button[DCP_MOUSE_POINTER_CONTROL] == DCP_MOUSE_MIDDLE);
    CHECK(map.button[DCP_MOUSE_POINTER_INTERACT] == DCP_MOUSE_LEFT);
    CHECK(map.sensitivity == 100 && map.wheel_up == DCP_WHEEL_NONE && map.wheel_down == DCP_WHEEL_NONE);

    // The numbers are SDL's own (SDL_BUTTON_LEFT 1, MIDDLE 2, RIGHT 3, X1 4, X2 5).
    CHECK(DCP_MOUSE_LEFT == 1 && DCP_MOUSE_MIDDLE == 2 && DCP_MOUSE_RIGHT == 3 && DCP_MOUSE_X1 == 4 && DCP_MOUSE_X2 == 5);

    CHECK(dcp_mouse_button_from_name("left") == DCP_MOUSE_LEFT && dcp_mouse_button_from_name("RIGHT") == DCP_MOUSE_RIGHT);
    CHECK(dcp_mouse_button_from_name("middle") == DCP_MOUSE_MIDDLE);
    CHECK(dcp_mouse_button_from_name("x1") == DCP_MOUSE_X1 && dcp_mouse_button_from_name("X2") == DCP_MOUSE_X2);
    CHECK(dcp_mouse_button_from_name("mouse4") == DCP_MOUSE_X1 && dcp_mouse_button_from_name("Mouse5") == DCP_MOUSE_X2);
    CHECK(dcp_mouse_button_from_name("mouse1") == DCP_MOUSE_LEFT && dcp_mouse_button_from_name("mouse2") == DCP_MOUSE_RIGHT);
    CHECK(dcp_mouse_button_from_name("mouse3") == DCP_MOUSE_MIDDLE);
    CHECK(dcp_mouse_button_from_name("none") == DCP_MOUSE_NONE && dcp_mouse_button_from_name("NONE") == DCP_MOUSE_NONE);
    CHECK(dcp_mouse_button_from_name("") == -1 && dcp_mouse_button_from_name(nullptr) == -1); // a missing value is no choice
    CHECK(dcp_mouse_button_from_name("mouse6") == -1 && dcp_mouse_button_from_name("button9") == -1);
    CHECK(dcp_mouse_button_from_name("left click") == -1);

    for (int button = DCP_MOUSE_LEFT; button < DCP_MOUSE_BUTTON_COUNT; ++button) {
        CHECK(dcp_mouse_button_from_name(dcp_mouse_button_name(button)) == button);
    }
    CHECK(std::string(dcp_mouse_button_name(DCP_MOUSE_NONE)) == "none" && std::string(dcp_mouse_button_name(99)) == "none");
    CHECK(std::string(dcp_mouse_button_label(DCP_MOUSE_LEFT)) == "Left Mouse");
    CHECK(std::string(dcp_mouse_button_label(DCP_MOUSE_RIGHT)) == "Right Mouse");
    CHECK(std::string(dcp_mouse_button_label(DCP_MOUSE_MIDDLE)) == "Middle Mouse");
    CHECK(std::string(dcp_mouse_button_label(DCP_MOUSE_X1)) == "Mouse 4" && std::string(dcp_mouse_button_label(DCP_MOUSE_X2)) == "Mouse 5");

    CHECK(std::string(dcp_mouse_actions[DCP_MOUSE_SQUARE].ini_key) == "mouse_square");
    CHECK(std::string(dcp_mouse_actions[DCP_MOUSE_CIRCLE].ini_key) == "mouse_circle");
    CHECK(std::string(dcp_mouse_actions[DCP_MOUSE_POINTER_CONTROL].ini_key) == "mouse_pointer_control");
    CHECK(std::string(dcp_mouse_actions[DCP_MOUSE_POINTER_INTERACT].ini_key) == "mouse_pointer_interact");
}

void TestSensitivitySteps() {
    CHECK(DCP_POINTER_SENSITIVITY_MIN == 25 && DCP_POINTER_SENSITIVITY_MAX == 200 && DCP_POINTER_SENSITIVITY_STEP == 5);
    CHECK(DCP_POINTER_SENSITIVITY_DEFAULT == 100);
    int steps = 0;
    for (int percent = 0; percent <= 300; ++percent) {
        const bool step = percent >= 25 && percent <= 200 && percent % 5 == 0;
        CHECK(dcp_pointer_sensitivity_valid(percent) == (step ? 1 : 0));
        const int normalized = dcp_pointer_sensitivity_normalize(percent);
        CHECK(normalized >= 25 && normalized <= 200 && normalized % 5 == 0);
        CHECK(!step || normalized == percent);
        steps += step;
    }
    CHECK(steps == 36);
    CHECK(dcp_pointer_sensitivity_normalize(-50) == 25 && dcp_pointer_sensitivity_normalize(0) == 25);
    CHECK(dcp_pointer_sensitivity_normalize(24) == 25 && dcp_pointer_sensitivity_normalize(9999) == 200);
    CHECK(dcp_pointer_sensitivity_normalize(102) == 100 && dcp_pointer_sensitivity_normalize(103) == 105);

    // Saved values: numbers are normalized, anything else is malformed.
    for (const char* ok : {"25", "75", "100", "5", "250", "33", "0"}) {
        CHECK(dcp_mouse_setting_value_valid("mouse_pointer_sensitivity", ok));
    }
    for (const char* bad : {"1.5", "100%", "", "-5", "abc", "99999999999"}) {
        CHECK(!dcp_mouse_setting_value_valid("mouse_pointer_sensitivity", bad));
    }
    DcpMouseMap map;
    dcp_mouse_defaults(&map);
    CHECK(map.sensitivity == 100);
    const std::vector<std::pair<const char*, int>> cases = {{"25", 25}, {"5", 25}, {"0", 25}, {"33", 35}, {"100", 100},
                                                            {"137", 135}, {"200", 200}, {"250", 200}, {"9000", 200}};
    for (const auto& [text, expected] : cases) {
        dcp_mouse_defaults(&map);
        CHECK(dcp_mouse_apply(&map, "mouse_pointer_sensitivity", text) && map.sensitivity == expected);
    }
    dcp_mouse_defaults(&map);
    map.sensitivity = 150;
    CHECK(!dcp_mouse_apply(&map, "mouse_pointer_sensitivity", "abc") && map.sensitivity == 150);
}

void TestSensitivityMultipliers() {
    float base_x = 0, base_y = 0;
    dcp_mouse_to_touch_units(100, 60.0f, 60.0f, &base_x, &base_y);
    CHECK(std::fabs(base_x - 100.0f) < 0.01f); // the tested default feel is unchanged
    for (const int percent : {25, 50, 75, 100, 125, 150, 175, 200, 30, 35, 195}) {
        float x = 0, y = 0;
        dcp_mouse_to_touch_units(percent, 60.0f, 60.0f, &x, &y);
        const float scale = static_cast<float>(percent) / 100.0f;
        CHECK(std::fabs(x - base_x * scale) < 0.001f && std::fabs(y - base_y * scale) < 0.001f);
    }

    // Live: the new percent applies to the next movement, normalized, with the held pointer undisturbed.
    Rig rig;
    rig.Down(DCP_MOUSE_MIDDLE);
    const DcpMouseSample landed = rig.Read();
    dcp_mouse_set_sensitivity(rig.state, 50);
    rig.Move(60.0f, 0.0f);
    CHECK(rig.Read().x == landed.x + 50);
    dcp_mouse_set_sensitivity(rig.state, 5000);
    rig.Move(6.0f, 0.0f);
    const DcpMouseSample capped = rig.Read();
    CHECK(capped.touch && capped.x == landed.x + 50 + 20);
    dcp_mouse_set_sensitivity(rig.state, 0);
    rig.Move(60.0f, 0.0f);
    CHECK(rig.Read().x == landed.x + 50 + 20 + 25);
}

void TestConfigFile() {
    const fs::path directory = fs::temp_directory_path() / "dcp_mouse_tests";
    fs::create_directories(directory);
    const std::string file = (directory / "dcp.ini").string();

    DcpMouseMap map;
    dcp_mouse_load((directory / "missing.ini").string().c_str(), &map);
    CHECK(map.button[DCP_MOUSE_SQUARE] == DCP_MOUSE_LEFT && map.sensitivity == 100);

    {
        std::ofstream(file) << "# comment\nvsync = 1\nmouse_square = x1\nmouse_circle = Mouse5 # note\n"
                               "mouse_pointer_control = right\nmouse_pointer_interact = middle\n"
                               "mouse_pointer_sensitivity = 150\nmouse_wheel_up = l1\nmouse_wheel_down = cross\n";
    }
    dcp_mouse_load(file.c_str(), &map);
    CHECK(map.button[DCP_MOUSE_SQUARE] == DCP_MOUSE_X1 && map.button[DCP_MOUSE_CIRCLE] == DCP_MOUSE_X2);
    CHECK(map.button[DCP_MOUSE_POINTER_CONTROL] == DCP_MOUSE_RIGHT && map.button[DCP_MOUSE_POINTER_INTERACT] == DCP_MOUSE_MIDDLE);
    CHECK(map.sensitivity == 150 && map.wheel_up == DCP_WHEEL_L1 && map.wheel_down == DCP_WHEEL_CROSS);

    // Bad values keep the default; the click cannot be unbound; unknown mouse keys are ignored.
    {
        std::ofstream(file) << "mouse_square = nobutton\nmouse_pointer_interact = none\nmouse_pointer_sensitivity = abc\n"
                               "mouse_wheel_up = start\nmouse_unknown = left\nmouse_circle = none\n";
    }
    dcp_mouse_load(file.c_str(), &map);
    CHECK(map.button[DCP_MOUSE_SQUARE] == DCP_MOUSE_LEFT && map.button[DCP_MOUSE_POINTER_INTERACT] == DCP_MOUSE_LEFT);
    CHECK(map.sensitivity == 100 && map.wheel_up == DCP_WHEEL_NONE && map.button[DCP_MOUSE_CIRCLE] == DCP_MOUSE_NONE);

    // An empty value is no choice: the default stays.
    {
        std::ofstream(file) << "mouse_square =\nmouse_wheel_up =\nmouse_pointer_sensitivity =\n";
    }
    dcp_mouse_load(file.c_str(), &map);
    CHECK(map.button[DCP_MOUSE_SQUARE] == DCP_MOUSE_LEFT && map.wheel_up == DCP_WHEEL_NONE && map.sensitivity == 100);

    // A file that gave Pointer Control and its click one button falls back to the defaults for both, in any line order.
    for (const char* text : {"mouse_pointer_control = left\nmouse_pointer_interact = left\n",
                             "mouse_pointer_interact = right\nmouse_pointer_control = right\n"}) {
        std::ofstream(file) << text;
        dcp_mouse_load(file.c_str(), &map);
        CHECK(map.button[DCP_MOUSE_POINTER_CONTROL] == DCP_MOUSE_MIDDLE && map.button[DCP_MOUSE_POINTER_INTERACT] == DCP_MOUSE_LEFT);
    }
    // Custom pair applied in either order is kept when the final pair differs.
    {
        std::ofstream(file) << "mouse_pointer_control = left\nmouse_pointer_interact = right\n";
    }
    dcp_mouse_load(file.c_str(), &map);
    CHECK(map.button[DCP_MOUSE_POINTER_CONTROL] == DCP_MOUSE_LEFT && map.button[DCP_MOUSE_POINTER_INTERACT] == DCP_MOUSE_RIGHT);

    // Binding rules the launcher uses.
    dcp_mouse_defaults(&map);
    CHECK(!dcp_mouse_binding_allowed(&map, DCP_MOUSE_POINTER_CONTROL, DCP_MOUSE_LEFT)); // the click's button
    CHECK(dcp_mouse_binding_allowed(&map, DCP_MOUSE_POINTER_CONTROL, DCP_MOUSE_X1));
    CHECK(dcp_mouse_binding_allowed(&map, DCP_MOUSE_POINTER_CONTROL, DCP_MOUSE_NONE));
    CHECK(!dcp_mouse_binding_allowed(&map, DCP_MOUSE_POINTER_INTERACT, DCP_MOUSE_MIDDLE));
    CHECK(!dcp_mouse_binding_allowed(&map, DCP_MOUSE_POINTER_INTERACT, DCP_MOUSE_NONE));
    CHECK(dcp_mouse_binding_allowed(&map, DCP_MOUSE_POINTER_INTERACT, DCP_MOUSE_X2));
    CHECK(dcp_mouse_binding_allowed(&map, DCP_MOUSE_SQUARE, DCP_MOUSE_MIDDLE) && dcp_mouse_binding_allowed(&map, DCP_MOUSE_CIRCLE, DCP_MOUSE_NONE));
    CHECK(!dcp_mouse_binding_allowed(&map, DCP_MOUSE_SQUARE, 17));

    fs::remove_all(directory);
}

void TestKeyboardMappingsUnchanged() {
    // The keyboard layout and the saved key names are what they were before the mouse bindings existed.
    CHECK(DCP_KEY_COUNT == 28 && DCP_KEY_FIRST_HOTKEY == 25);
    DcpKeyMap keys;
    dcp_keys_defaults(&keys);
    CHECK(keys.scancode[DCP_KEY_LEFT_STICK_UP] == dcp_key_from_name("w") && keys.scancode[DCP_KEY_LEFT_STICK_LEFT] == dcp_key_from_name("a"));
    CHECK(keys.scancode[DCP_KEY_CROSS] == dcp_key_from_name("space") && keys.scancode[DCP_KEY_CIRCLE] == dcp_key_from_name("lshift"));
    CHECK(keys.scancode[DCP_KEY_SQUARE] == dcp_key_from_name("e") && keys.scancode[DCP_KEY_TRIANGLE] == dcp_key_from_name("q"));
    CHECK(keys.scancode[DCP_KEY_TOUCHPAD_LEFT] == dcp_key_from_name("tab") && keys.scancode[DCP_KEY_TOUCHPAD_RIGHT] == dcp_key_from_name("backspace"));
    CHECK(keys.scancode[DCP_KEY_HOTKEY_CHEATS] == dcp_key_from_name("f10") && keys.scancode[DCP_KEY_HOTKEY_WINDOW_MODE] == dcp_key_from_name("f11"));

    // Mouse lines, whose values (left, right...) are also arrow key names, never touch the keyboard map, and keyboard
    // lines never touch the mouse map.
    const fs::path directory = fs::temp_directory_path() / "dcp_mouse_tests_keys";
    fs::create_directories(directory);
    const std::string file = (directory / "dcp.ini").string();
    {
        std::ofstream(file) << "mouse_square = left\nmouse_circle = right\nkey_cross = q\nmouse_wheel_up = l1\n";
    }
    DcpKeyMap loaded;
    dcp_keys_load(file.c_str(), &loaded);
    CHECK(loaded.scancode[DCP_KEY_CROSS] == dcp_key_from_name("q"));
    for (int i = 0; i < DCP_KEY_COUNT; ++i) {
        if (i != DCP_KEY_CROSS) {
            CHECK(loaded.scancode[i] == keys.scancode[i]);
        }
    }
    DcpMouseMap mouse;
    dcp_mouse_load(file.c_str(), &mouse);
    CHECK(mouse.button[DCP_MOUSE_SQUARE] == DCP_MOUSE_LEFT && mouse.wheel_up == DCP_WHEEL_L1);
    CHECK(!dcp_keys_apply(&keys, "mouse_square", "left") && !dcp_mouse_apply(&mouse, "key_cross", "q"));
    for (const DcpKeyInfo& action : dcp_key_actions) {
        CHECK(!dcp_mouse_setting_key(action.ini_key));
    }
    fs::remove_all(directory);
}

void TestNormalMouseButtons() {
    Rig rig;
    DcpMouseSample idle = rig.Read();
    CHECK(idle.buttons == 0 && !idle.touch && !idle.pointer);

    rig.Down(DCP_MOUSE_LEFT);
    CHECK(rig.Read().buttons == BTN_SQUARE); // Mouse Left -> Square
    rig.Up(DCP_MOUSE_LEFT);
    CHECK(rig.Read().buttons == 0);

    rig.Down(DCP_MOUSE_RIGHT);
    CHECK(rig.Read().buttons == BTN_CIRCLE); // Mouse Right -> Circle
    rig.Down(DCP_MOUSE_LEFT);
    CHECK(rig.Read().buttons == (BTN_CIRCLE | BTN_SQUARE));
    rig.Up(DCP_MOUSE_RIGHT);
    rig.Up(DCP_MOUSE_LEFT);

    // Movement alone moves nothing; the unused buttons and out-of-range numbers do nothing.
    rig.Move(500, 500);
    rig.Down(DCP_MOUSE_X1);
    rig.Down(DCP_MOUSE_X2);
    rig.Down(0);
    rig.Down(DCP_MOUSE_BUTTON_COUNT);
    const DcpMouseSample moved = rig.Read();
    CHECK(moved.buttons == 0 && !moved.touch && !moved.pointer);
    rig.Up(DCP_MOUSE_X1);
    rig.Up(DCP_MOUSE_X2);

    // Custom bindings: X buttons for the actions, and the old default buttons do nothing.
    rig.map.button[DCP_MOUSE_SQUARE] = DCP_MOUSE_X1;
    rig.map.button[DCP_MOUSE_CIRCLE] = DCP_MOUSE_X2;
    rig.Configure();
    rig.Down(DCP_MOUSE_LEFT);
    CHECK(rig.Read().buttons == 0);
    rig.Up(DCP_MOUSE_LEFT);
    rig.Down(DCP_MOUSE_X1);
    CHECK(rig.Read().buttons == BTN_SQUARE);
    rig.Down(DCP_MOUSE_X2);
    CHECK(rig.Read().buttons == (BTN_SQUARE | BTN_CIRCLE));
    rig.Up(DCP_MOUSE_X1);
    rig.Up(DCP_MOUSE_X2);

    // An unbound action presses nothing.
    rig.map.button[DCP_MOUSE_SQUARE] = DCP_MOUSE_NONE;
    rig.Configure();
    rig.Down(DCP_MOUSE_LEFT);
    CHECK(rig.Read().buttons == 0);
}

void TestPointerActivationAndConsumption() {
    Rig rig;
    CHECK(!dcp_mouse_pointer_active(rig.state));
    rig.Down(DCP_MOUSE_MIDDLE);
    CHECK(dcp_mouse_pointer_active(rig.state));
    DcpMouseSample sample = rig.Read();
    CHECK(sample.pointer && sample.touch && sample.buttons == 0); // the pointer button itself presses nothing

    // Mouse Left is the pointer interaction and does not send Square.
    rig.Down(DCP_MOUSE_LEFT);
    sample = rig.Read();
    CHECK(sample.buttons == BTN_TOUCHPAD && (sample.buttons & BTN_SQUARE) == 0);
    rig.Up(DCP_MOUSE_LEFT);
    CHECK(rig.Read().buttons == 0);

    // The right button keeps its normal action while the pointer is held.
    rig.Down(DCP_MOUSE_RIGHT);
    CHECK(rig.Read().buttons == BTN_CIRCLE);
    rig.Up(DCP_MOUSE_RIGHT);

    // Left held, pointer released: the press stays consumed, so no attack leaks out of the click.
    rig.Down(DCP_MOUSE_LEFT);
    rig.Read();
    rig.Up(DCP_MOUSE_MIDDLE);
    sample = rig.Read();
    CHECK(!dcp_mouse_pointer_active(rig.state) && !sample.pointer && !sample.touch && sample.buttons == 0);
    rig.Up(DCP_MOUSE_LEFT);
    CHECK(rig.Read().buttons == 0);
    rig.Down(DCP_MOUSE_LEFT); // the next press is the normal attack again
    CHECK(rig.Read().buttons == BTN_SQUARE);
    rig.Up(DCP_MOUSE_LEFT);

    // Left pressed first, then the pointer: the held press becomes the click, not an attack.
    rig.Down(DCP_MOUSE_LEFT);
    CHECK(rig.Read().buttons == BTN_SQUARE);
    rig.Down(DCP_MOUSE_MIDDLE);
    sample = rig.Read();
    CHECK(sample.buttons == BTN_TOUCHPAD);
    rig.Up(DCP_MOUSE_MIDDLE);
    rig.Up(DCP_MOUSE_LEFT);

    // Releasing the pointer button exits pointer mode on the next read, with the finger lifting at once.
    rig.Down(DCP_MOUSE_MIDDLE);
    CHECK(rig.Read().touch);
    rig.Up(DCP_MOUSE_MIDDLE);
    sample = rig.Read();
    CHECK(!sample.touch && !sample.pointer);

    // With Pointer Control unbound nothing activates it.
    rig.map.button[DCP_MOUSE_POINTER_CONTROL] = DCP_MOUSE_NONE;
    rig.Configure();
    rig.Down(DCP_MOUSE_MIDDLE);
    CHECK(!dcp_mouse_pointer_active(rig.state) && !rig.Read().touch);
}

void TestPointerClickButtonCanBeRemapped() {
    Rig rig;
    rig.map.button[DCP_MOUSE_POINTER_CONTROL] = DCP_MOUSE_X1;
    rig.map.button[DCP_MOUSE_POINTER_INTERACT] = DCP_MOUSE_RIGHT;
    rig.map.button[DCP_MOUSE_CIRCLE] = DCP_MOUSE_NONE;
    rig.Configure();
    rig.Down(DCP_MOUSE_X1);
    rig.Down(DCP_MOUSE_RIGHT);
    DcpMouseSample sample = rig.Read();
    CHECK(sample.buttons == BTN_TOUCHPAD);
    rig.Down(DCP_MOUSE_LEFT); // the primary action is not consumed by the pointer
    CHECK(rig.Read().buttons == (BTN_TOUCHPAD | BTN_SQUARE));
}

void TestMovementScaling() {
    float dx = 0, dy = 0;
    dcp_mouse_to_touch_units(100, 60.0f, 0.0f, &dx, &dy);
    CHECK(std::fabs(dx - 100.0f) < 0.01f && dy == 0.0f); // 60 game pixels at the game's default cursor gain of 0.6
    dcp_mouse_to_touch_units(100, 0.0f, 60.0f, &dx, &dy);
    CHECK(std::fabs(dy - 60.0f / (0.6f * (1080.0f / 943.0f))) < 0.01f); // the surface is 1920 x 943 for a 1920 x 1080 game
    dcp_mouse_to_touch_units(50, 60.0f, 0.0f, &dx, &dy);
    CHECK(std::fabs(dx - 50.0f) < 0.01f);
    dcp_mouse_to_touch_units(200, 60.0f, 0.0f, &dx, &dy);
    CHECK(std::fabs(dx - 200.0f) < 0.01f);
    dcp_mouse_to_touch_units(25, -60.0f, 0.0f, &dx, &dy);
    CHECK(std::fabs(dx + 25.0f) < 0.01f);

    // The same movement through a real state: the finger lands in the middle, then follows the movement.
    for (const int percent : {25, 50, 75, 100, 125, 150, 175, 200}) {
        Rig rig;
        rig.map.sensitivity = percent;
        rig.Configure();
        rig.Down(DCP_MOUSE_MIDDLE);
        const DcpMouseSample landed = rig.Read();
        CHECK(landed.touch && landed.x == kCenterX && landed.y == kCenterY);
        rig.Move(6.0f, -3.0f);
        const DcpMouseSample moved = rig.Read();
        float expected_x = 0, expected_y = 0;
        dcp_mouse_to_touch_units(percent, 6.0f, -3.0f, &expected_x, &expected_y);
        CHECK(moved.touch && std::fabs((moved.x - landed.x) - expected_x) <= 1.0f);
        CHECK(std::fabs((moved.y - landed.y) - expected_y) <= 1.0f);
        CHECK(moved.id == landed.id);
    }
    // A bad sensitivity in the map is the default.
    Rig rig;
    rig.map.sensitivity = 133;
    rig.Configure();
    rig.Down(DCP_MOUSE_MIDDLE);
    rig.Read();
    rig.Move(6.0f, 0.0f);
    CHECK(std::fabs(rig.Read().x - kCenterX - 10) <= 1);
}

void TestGameScaleFollowsThePresentedPicture() {
    CHECK(dcp_mouse_game_scale(1920, 1080) == 1.0f);
    CHECK(dcp_mouse_game_scale(3840, 2160) == 0.5f);
    CHECK(dcp_mouse_game_scale(1280, 720) == 1.5f);
    CHECK(dcp_mouse_game_scale(2560, 1080) == 1.0f); // ultrawide window: height fits
    CHECK(dcp_mouse_game_scale(1920, 1200) == 1.0f); // 16:10: width fits, bars above and below
    CHECK(dcp_mouse_game_scale(1280, 1024) == 1.5f); // 5:4: the picture is 1280 x 720
    CHECK(dcp_mouse_game_scale(0, 0) == 1.0f && dcp_mouse_game_scale(-1, 720) == 1.0f);
}

void TestCoordinatesStayValid() {
    Rig rig;
    rig.Down(DCP_MOUSE_MIDDLE);
    uint32_t seed = 12345;
    auto next = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(static_cast<int>(seed >> 16) % 4001 - 2000) / 2.0f;
    };
    int touches = 0;
    for (int read = 0; read < 5000; ++read) {
        rig.Move(next(), next());
        const DcpMouseSample sample = rig.Read();
        if (sample.touch) {
            ++touches;
            CHECK(sample.x <= DCP_TOUCH_MAX_X && sample.y <= DCP_TOUCH_MAX_Y);
            CHECK(sample.id < 128);
        }
    }
    CHECK(touches > 2000);
}

void TestFingerLifetime() {
    Rig rig;
    rig.Down(DCP_MOUSE_MIDDLE);
    const DcpMouseSample first = rig.Read();
    CHECK(first.touch);
    for (int i = 0; i < 20; ++i) {
        const DcpMouseSample held = rig.Read();
        CHECK(held.touch && held.id == first.id); // the same finger for as long as the button is held
    }
    rig.Up(DCP_MOUSE_MIDDLE);
    CHECK(!rig.Read().touch);                      // lifted on the first read after release
    CHECK(!rig.Read().touch && !rig.Read().pointer);
    rig.Down(DCP_MOUSE_MIDDLE);
    const DcpMouseSample second = rig.Read();
    CHECK(second.touch && second.id != first.id && second.x == kCenterX && second.y == kCenterY); // a new finger

    // The lift is observed by the game before a new finger begins, and a press shorter than a read never reaches it.
    Game game;
    game.Read(second);
    rig.Up(DCP_MOUSE_MIDDLE);
    game.Read(rig.Read());
    CHECK(!game.previous);
    rig.Down(DCP_MOUSE_MIDDLE);
    rig.Up(DCP_MOUSE_MIDDLE);
    CHECK(!rig.Read().touch);
}

void TestEdgeRecenter() {
    Rig rig;
    rig.Down(DCP_MOUSE_MIDDLE);
    Game game;
    double input_x = 0, input_y = 0;
    int lifts = 0, landings = 0;
    bool previous = false;
    DcpMouseSample last{};
    // Steady movement to the right and down, much further than the surface is wide.
    for (int read = 0; read < 600; ++read) {
        rig.Move(30.0f, 12.0f);
        float dx = 0, dy = 0;
        dcp_mouse_to_touch_units(100, 30.0f, 12.0f, &dx, &dy);
        input_x += dx;
        input_y += dy;
        const DcpMouseSample sample = rig.Read();
        CHECK(sample.pointer);
        if (!sample.touch && previous) {
            ++lifts;
        }
        if (sample.touch && !previous) {
            ++landings;
            CHECK(sample.x == kCenterX && sample.y == kCenterY); // recentred, never at the edge
        }
        if (sample.touch && previous) {
            CHECK(std::abs(sample.x - last.x) <= DCP_TOUCH_RES_X / 5 + 1 && std::abs(sample.y - last.y) <= DCP_TOUCH_RES_Y / 5 + 1);
        }
        game.Read(sample);
        previous = sample.touch != 0;
        last = sample;
    }
    // Let the rest of the movement through.
    for (int read = 0; read < 100; ++read) {
        const DcpMouseSample sample = rig.Read();
        game.Read(sample);
    }
    CHECK(lifts >= 3 && (landings == lifts || landings == lifts + 1));
    // The game saw one finger per segment: no jump at a recentre, and the pointer travelled what the mouse travelled.
    CHECK(game.segments >= 4);
    CHECK(std::fabs(game.pointer_x - input_x) <= 2.0 * game.segments);
    CHECK(std::fabs(game.pointer_y - input_y) <= 2.0 * game.segments);

    // The reverse direction recenters the same way.
    Rig back;
    back.Down(DCP_MOUSE_MIDDLE);
    Game back_game;
    double back_input = 0;
    for (int read = 0; read < 400; ++read) {
        back.Move(-40.0f, 0.0f);
        back_input -= 40.0 / 0.6;
        back_game.Read(back.Read());
    }
    for (int read = 0; read < 100; ++read) {
        back_game.Read(back.Read());
    }
    CHECK(back_game.segments >= 3 && std::fabs(back_game.pointer_x - back_input) <= 2.0 * back_game.segments);

    // Movement during the recentre waits for the new finger and is not lost.
    Rig wait;
    wait.Down(DCP_MOUSE_MIDDLE);
    Game wait_game;
    double wait_input = 0;
    for (int read = 0; read < 80; ++read) {
        wait.Move(100.0f, 0.0f);
        wait_input += 100.0 / 0.6;
        wait_game.Read(wait.Read());
    }
    for (int read = 0; read < 80; ++read) {
        wait_game.Read(wait.Read());
    }
    CHECK(wait_game.segments >= 5 && std::fabs(wait_game.pointer_x - wait_input) <= 2.0 * wait_game.segments);
}

void TestStalledReadsAreBounded() {
    Rig rig;
    rig.Down(DCP_MOUSE_MIDDLE);
    Game game;
    game.Read(rig.Read());
    for (int i = 0; i < 100000; ++i) {
        rig.Move(1000.0f, 0.0f);
    }
    // The pad was not read for a while: the queued movement is capped, not unbounded.
    for (int reads = 0; reads < 300; ++reads) {
        game.Read(rig.Read());
    }
    CHECK(game.pointer_x <= 4.0 * DCP_TOUCH_RES_X + 2.0 * game.segments && game.pointer_x >= 3.9 * DCP_TOUCH_RES_X);
}

void TestFocusLossAndBlocking() {
    Rig rig;
    rig.Down(DCP_MOUSE_MIDDLE);
    rig.Down(DCP_MOUSE_LEFT);
    CHECK(rig.Read().touch);
    dcp_mouse_release_all(rig.state); // focus lost, a window mode switch, closing
    CHECK(!dcp_mouse_pointer_active(rig.state));
    DcpMouseSample sample = rig.Read();
    CHECK(!sample.touch && !sample.pointer && sample.buttons == 0);
    rig.Move(100, 100);
    CHECK(rig.Read().buttons == 0);
    rig.Down(DCP_MOUSE_RIGHT);
    CHECK(rig.Read().buttons == BTN_CIRCLE); // new presses after the loss work

    // A held normal button is released too.
    rig.Down(DCP_MOUSE_LEFT);
    dcp_mouse_release_all(rig.state);
    CHECK(rig.Read().buttons == 0);

    // While an overlay or the text dialog owns the mouse, presses are dropped and nothing stays held afterwards.
    rig.Down(DCP_MOUSE_MIDDLE);
    dcp_mouse_block(rig.state, 1);
    CHECK(!dcp_mouse_pointer_active(rig.state));
    rig.Down(DCP_MOUSE_LEFT);
    rig.Down(DCP_MOUSE_MIDDLE);
    rig.Move(50, 50);
    dcp_mouse_wheel(rig.state, 1.0f);
    sample = rig.Read();
    CHECK(sample.buttons == 0 && !sample.touch && !sample.pointer);
    dcp_mouse_block(rig.state, 0);
    sample = rig.Read();
    CHECK(sample.buttons == 0 && !sample.touch); // the buttons held through the block are not seen as pressed
    rig.Up(DCP_MOUSE_LEFT);
    rig.Up(DCP_MOUSE_MIDDLE);
    rig.Down(DCP_MOUSE_LEFT);
    CHECK(rig.Read().buttons == BTN_SQUARE);
}

void TestControllerActivityEndsPointer() {
    Rig rig;
    rig.Down(DCP_MOUSE_MIDDLE);
    CHECK(rig.Read().touch);
    dcp_mouse_controller_activity(rig.state);
    CHECK(!dcp_mouse_pointer_active(rig.state));
    DcpMouseSample sample = rig.Read();
    CHECK(!sample.touch && !sample.pointer);
    rig.Move(100, 100);
    rig.Down(DCP_MOUSE_LEFT);
    CHECK(rig.Read().buttons == BTN_SQUARE); // the held pointer button does not hide the left click's normal use
    rig.Up(DCP_MOUSE_LEFT);
    // The button stays off until released, then works again.
    CHECK(!dcp_mouse_pointer_active(rig.state));
    rig.Up(DCP_MOUSE_MIDDLE);
    rig.Down(DCP_MOUSE_MIDDLE);
    CHECK(dcp_mouse_pointer_active(rig.state) && rig.Read().touch);

    // Controller activity with no pointer held changes nothing.
    Rig idle;
    dcp_mouse_controller_activity(idle.state);
    idle.Down(DCP_MOUSE_MIDDLE);
    CHECK(dcp_mouse_pointer_active(idle.state));
}

void TestWheel() {
    Rig rig;
    dcp_mouse_wheel(rig.state, 1.0f);
    dcp_mouse_wheel(rig.state, -1.0f);
    CHECK(rig.Read().buttons == 0); // no hard-coded gameplay action

    rig.map.wheel_up = DCP_WHEEL_L1;
    rig.map.wheel_down = DCP_WHEEL_TRIANGLE;
    rig.Configure();
    dcp_mouse_wheel(rig.state, 1.0f);
    for (int i = 0; i < 4; ++i) {
        CHECK(rig.Read().buttons == BTN_L1);
    }
    CHECK(rig.Read().buttons == 0);
    dcp_mouse_wheel(rig.state, -2.0f);
    CHECK(rig.Read().buttons == BTN_TRIANGLE);
    dcp_mouse_wheel(rig.state, 0.0f);
    for (int i = 0; i < 4; ++i) {
        rig.Read();
    }
    CHECK(rig.Read().buttons == 0);
    for (const int target : {DCP_WHEEL_CROSS, DCP_WHEEL_CIRCLE, DCP_WHEEL_SQUARE, DCP_WHEEL_R1, DCP_WHEEL_L2, DCP_WHEEL_R2}) {
        CHECK(dcp_wheel_target_from_name(dcp_wheel_target_name(target)) == target);
    }
    CHECK(dcp_wheel_target_from_name("start") == -1 && dcp_wheel_target_from_name("") == -1);
    CHECK(dcp_wheel_target_from_name("none") == DCP_WHEEL_NONE);
}

void TestOverlayAndTextEntryBlockGameplayMouse() {
    using DcpMouseCapture::Blocked;
    using DcpMouseCapture::Frame;
    CHECK(!Blocked(Frame{true, true, false, false}));
    CHECK(Blocked(Frame{true, false, false, false})); // focus away
    CHECK(Blocked(Frame{true, true, true, false}));   // Options, Cheats or a game dialog
    CHECK(Blocked(Frame{true, true, false, true}));   // text entry

    DcpMouseCapture::Tracker tracker;
    CHECK(!tracker.Captured());
    CHECK(!tracker.Update(Frame{false, true, false, false}).has_value());
    CHECK(tracker.Update(Frame{true, true, false, false}) == std::optional<bool>(true));
    CHECK(tracker.Captured());
    CHECK(!tracker.Update(Frame{true, true, false, false}).has_value()); // no repeated SDL calls
    CHECK(tracker.Update(Frame{true, true, true, false}) == std::optional<bool>(false)); // Options opens
    CHECK(!tracker.Captured());
    CHECK(!tracker.Update(Frame{true, true, true, false}).has_value());
    CHECK(tracker.Update(Frame{true, true, false, false}) == std::optional<bool>(true));
    CHECK(tracker.Update(Frame{true, true, false, true}) == std::optional<bool>(false)); // text entry
    CHECK(tracker.Update(Frame{true, true, false, false}) == std::optional<bool>(true));
    CHECK(tracker.Update(Frame{true, false, false, false}) == std::optional<bool>(false)); // Alt+Tab
    CHECK(!tracker.Captured());
    CHECK(tracker.Update(Frame{true, true, false, false}) == std::optional<bool>(true));
    CHECK(tracker.Update(Frame{false, true, false, false}) == std::optional<bool>(false)); // button released
    CHECK(!tracker.Captured());

    // A window mode switch or closing leaves capture at once, and only once.
    CHECK(tracker.Update(Frame{true, true, false, false}) == std::optional<bool>(true));
    CHECK(tracker.Release() == std::optional<bool>(false));
    CHECK(!tracker.Captured() && !tracker.Release().has_value());
    // The next frame with the pointer still held captures again, after the mode switch finished.
    CHECK(tracker.Update(Frame{true, true, false, false}) == std::optional<bool>(true));

    // The state follows the same frames: a blocked frame drops the pointer and every held button.
    Rig rig;
    rig.Down(DCP_MOUSE_MIDDLE);
    rig.Down(DCP_MOUSE_LEFT);
    const Frame overlay{dcp_mouse_pointer_active(rig.state) != 0, true, true, false};
    dcp_mouse_block(rig.state, Blocked(overlay));
    CHECK(!tracker.Update(overlay).value_or(false) && rig.Read().buttons == 0 && !dcp_mouse_pointer_active(rig.state));
}

void TestCursorVisibilityIntegration() {
    DcpCursor::State state;
    // Moving the mouse shows the cursor as before; Pointer Control hides it, and ending it brings it back.
    state.MouseMoved();
    CHECK(!state.Update(true, true, false).has_value() && state.Visible());
    CHECK(state.Update(true, true, false, true) == std::optional<bool>(false));
    CHECK(!state.Visible());
    state.MouseMoved(); // relative motion keeps arriving
    CHECK(!state.Update(true, true, false, true).has_value() && !state.Visible());
    CHECK(state.Update(true, true, false, false) == std::optional<bool>(true));

    // Anything that needs the OS cursor wins over the pointer.
    CHECK(state.Update(true, true, false, true) == std::optional<bool>(false));
    CHECK(state.Update(true, true, true, true) == std::optional<bool>(true)); // Options or Cheats
    CHECK(state.Update(true, true, false, true) == std::optional<bool>(false));
    CHECK(state.Update(true, false, false, true) == std::optional<bool>(true)); // focus lost
    CHECK(state.Update(true, true, false, true) == std::optional<bool>(false));
    CHECK(state.Update(false, true, false, true) == std::optional<bool>(true)); // the launcher

    // The default is unchanged for callers that do not pass the pointer.
    DcpCursor::State plain;
    plain.KeyboardActivity();
    CHECK(plain.Update(true, true, false) == std::optional<bool>(false));
    plain.MouseButtonPressed();
    CHECK(plain.Update(true, true, false) == std::optional<bool>(true));
    // A controller becoming active while the pointer is held shows nothing new: the pointer ends through the mouse state.
    plain.ControllerActivity();
    CHECK(!plain.Update(true, true, false, false).value_or(true));
}

void TestMouseControlsSetting() {
    DcpMouseMap map;
    dcp_mouse_defaults(&map);
    CHECK(map.controls == 1);
    CHECK(std::string(dcp_mouse_controls_key) == "mouse_controls" && dcp_mouse_setting_key("mouse_controls"));
    for (const char* valid : {"1", "on", "yes", "true", "ON", "0", "off", "no", "false"}) {
        CHECK(dcp_mouse_setting_value_valid("mouse_controls", valid));
    }
    for (const char* bad : {"", "maybe", "2", "-1", "enabled"}) {
        CHECK(!dcp_mouse_setting_value_valid("mouse_controls", bad));
    }

    // Save and load, and a bad value keeps the default On.
    const fs::path directory = fs::temp_directory_path() / "dcp_mouse_tests_controls";
    fs::create_directories(directory);
    const std::string file = (directory / "dcp.ini").string();
    DcpMouseMap loaded;
    dcp_mouse_load((directory / "missing.ini").string().c_str(), &loaded);
    CHECK(loaded.controls == 1);
    const std::vector<std::pair<std::string, int>> cases = {
        {"mouse_controls = 0", 0}, {"mouse_controls = off", 0}, {"mouse_controls = 1", 1}, {"mouse_controls = On", 1},
        {"mouse_controls = maybe", 1}, {"mouse_controls =", 1}, {"vsync = 1", 1}};
    for (const auto& [text, expected] : cases) {
        std::ofstream(file) << text << "\n";
        dcp_mouse_load(file.c_str(), &loaded);
        CHECK(loaded.controls == expected);
    }
    {
        std::ofstream(file) << "mouse_controls = 0\nmouse_square = x1\nmouse_pointer_sensitivity = 150\n";
    }
    dcp_mouse_load(file.c_str(), &loaded);
    CHECK(loaded.controls == 0 && loaded.button[DCP_MOUSE_SQUARE] == DCP_MOUSE_X1 && loaded.sensitivity == 150);
    fs::remove_all(directory);

    // On keeps the behaviour.
    Rig on;
    CHECK(dcp_mouse_enabled(on.state));
    on.Down(DCP_MOUSE_LEFT);
    CHECK(on.Read().buttons == BTN_SQUARE);
    on.Up(DCP_MOUSE_LEFT);
    on.Down(DCP_MOUSE_MIDDLE);
    CHECK(on.Read().touch && dcp_mouse_pointer_active(on.state));

    // Off blocks every gameplay mouse input.
    Rig off;
    off.map.controls = 0;
    off.map.wheel_up = DCP_WHEEL_L1;
    off.map.button[DCP_MOUSE_CIRCLE] = DCP_MOUSE_X2;
    off.map.button[DCP_MOUSE_SQUARE] = DCP_MOUSE_X1;
    off.Configure();
    CHECK(!dcp_mouse_enabled(off.state));
    for (const int button : {DCP_MOUSE_LEFT, DCP_MOUSE_RIGHT, DCP_MOUSE_X1, DCP_MOUSE_X2, DCP_MOUSE_MIDDLE}) {
        off.Down(button);
        const DcpMouseSample pressed = off.Read();
        CHECK(pressed.buttons == 0 && !pressed.touch && !pressed.pointer);
        off.Up(button);
    }
    dcp_mouse_wheel(off.state, 1.0f);
    off.Move(300, 300);
    off.Down(DCP_MOUSE_MIDDLE);
    off.Move(300, 300);
    for (int i = 0; i < 6; ++i) {
        const DcpMouseSample idle = off.Read();
        CHECK(idle.buttons == 0 && !idle.touch && !idle.pointer);
    }
    CHECK(!dcp_mouse_pointer_active(off.state));

    // Turning it Off during Pointer Control drops the finger, the capture, the click and every held button at once.
    Rig live;
    live.map.button[DCP_MOUSE_SQUARE] = DCP_MOUSE_X1;
    live.map.button[DCP_MOUSE_CIRCLE] = DCP_MOUSE_X2;
    live.map.wheel_up = DCP_WHEEL_R1;
    live.Configure();
    DcpMouseCapture::Tracker tracker;
    live.Down(DCP_MOUSE_MIDDLE);
    live.Down(DCP_MOUSE_LEFT);
    live.Down(DCP_MOUSE_X1);
    live.Down(DCP_MOUSE_X2);
    dcp_mouse_wheel(live.state, 1.0f);
    DcpMouseSample sample = live.Read();
    CHECK(sample.touch && (sample.buttons & BTN_TOUCHPAD) && (sample.buttons & BTN_SQUARE));
    CHECK(tracker.Update({dcp_mouse_pointer_active(live.state) != 0, true, false, false}) == std::optional<bool>(true));
    dcp_mouse_set_enabled(live.state, 0);
    CHECK(!dcp_mouse_pointer_active(live.state));
    CHECK(tracker.Update({dcp_mouse_pointer_active(live.state) != 0, true, false, false}) == std::optional<bool>(false));
    CHECK(!tracker.Captured());
    for (int i = 0; i < 6; ++i) {
        sample = live.Read();
        CHECK(sample.buttons == 0 && !sample.touch && !sample.pointer);
    }
    // Presses and releases while Off leave nothing behind, so On starts clean with the custom bindings kept.
    live.Up(DCP_MOUSE_X1);
    live.Down(DCP_MOUSE_X2);
    dcp_mouse_set_enabled(live.state, 1);
    CHECK(live.Read().buttons == 0 && !dcp_mouse_pointer_active(live.state));
    live.Down(DCP_MOUSE_X1);
    CHECK(live.Read().buttons == BTN_SQUARE);
    live.Down(DCP_MOUSE_X2);
    CHECK(live.Read().buttons == (BTN_SQUARE | BTN_CIRCLE));
    live.Down(DCP_MOUSE_MIDDLE);
    CHECK(live.Read().touch);
    dcp_mouse_wheel(live.state, 1.0f);
    CHECK((live.Read().buttons & BTN_R1) != 0);

    // With Mouse Controls off the mouse never becomes the cursor's last device; menus still show the cursor.
    DcpCursor::State cursor;
    cursor.KeyboardActivity();
    cursor.SetMouseControls(false);
    cursor.MouseMoved();
    cursor.MouseButtonPressed();
    cursor.MouseWheelMoved();
    cursor.Update(true, true, false);
    CHECK(cursor.LastDevice() == DcpCursor::Device::Keyboard && !cursor.Visible());
    CHECK(cursor.Update(true, true, true) == std::optional<bool>(true));
    CHECK(cursor.Update(true, true, false) == std::optional<bool>(false));
    cursor.ControllerActivity();
    cursor.MouseMoved();
    CHECK(cursor.LastDevice() == DcpCursor::Device::Controller);
    cursor.SetMouseControls(true);
    cursor.MouseMoved();
    CHECK(cursor.LastDevice() == DcpCursor::Device::Mouse);

    // Menus, dialogs and text entry see every event before the gameplay path, which is the only place the setting acts.
    const std::string window = ReadSource("gpu/shim/window.cpp");
    const std::size_t poll = window.find("while (SDL_PollEvent(&event))");
    const std::size_t overlay = window.find("DcpOverlay::HandleEvent(event)", poll);
    const std::size_t motion = window.find("case SDL_EVENT_MOUSE_MOTION:", poll);
    CHECK(poll != std::string::npos && overlay < motion);
    CHECK(!Contains(window.substr(poll, overlay - poll), "dcp_mouse_enabled"));
    CHECK(Contains(window, "cursor.SetMouseControls(dcp_mouse_enabled(dcp_mouse_global()) != 0);"));
    const std::string overlay_source = ReadSource("gpu/shim/dcp_overlay.cpp");
    CHECK(!Contains(overlay_source, "mouse_controls") && !Contains(overlay_source, "dcp_mouse_enabled"));
    CHECK(!Contains(ReadSource("launcher/launcher.cpp"), "dcp_mouse_enabled"));
}

void TestSourceSafety() {
    const std::string window = ReadSource("gpu/shim/window.cpp");
    const std::string pad = ReadSource("src/runtime_pad.c");
    const std::string mouse = ReadSource("src/dcp_mouse.c");
    const std::string header = ReadSource("src/dcp_mouse.h");

    // One event pump, on the window thread; the pad sampler only samples.
    CHECK(Count(window, "SDL_PollEvent(") == 1);
    for (const std::string& source : {pad, mouse, header}) {
        CHECK(!Contains(source, "SDL_PollEvent") && !Contains(source, "SDL_PumpEvents") && !Contains(source, "SDL_WaitEvent"));
        CHECK(!Contains(source, "SDL_SetWindowRelativeMouseMode") && !Contains(source, "SDL_ShowCursor") && !Contains(source, "SDL_HideCursor"));
    }
    CHECK(!Contains(mouse, "#include <SDL3") && !Contains(header, "#include <SDL3"));
    // Relative mouse mode is only set by the window, and only through the tracker and the release path.
    CHECK(Count(window, "SDL_SetWindowRelativeMouseMode(") == 2);
    for (const char* other : {"gpu/shim/dcpgpu.cpp", "gpu/shim/dcp_overlay.cpp", "src/loader.c", "launcher/launcher.cpp"}) {
        CHECK(!Contains(ReadSource(other), "SDL_SetWindowRelativeMouseMode"));
    }

    // Everything that stops the game from taking the mouse releases it.
    const std::size_t apply = window.find("void WindowSDL::ApplyDisplayMode(int mode)");
    CHECK(apply != std::string::npos);
    CHECK(window.find("ReleaseMouse();", apply) < window.find("SDL_SetWindowFullscreen(window, borderless);", apply));
    const std::size_t destructor = window.find("WindowSDL::~WindowSDL()");
    CHECK(window.find("ReleaseMouse();", destructor) < window.find("SDL_DestroyWindow(window);", destructor));
    const std::size_t focus = window.find("case SDL_EVENT_WINDOW_FOCUS_LOST:");
    CHECK(focus != std::string::npos && window.find("ReleaseMouse();", focus) < window.find("break;", focus));
    CHECK(Contains(window, "dcp_mouse_controller_activity(dcp_mouse_global());"));
    CHECK(Contains(window, "mouse_capture.Captured()"));
    // Overlay input stays isolated: gameplay presses and motion are forwarded only when no overlay owns the mouse.
    const std::size_t motion = window.find("case SDL_EVENT_MOUSE_MOTION:");
    const std::size_t down = window.find("case SDL_EVENT_MOUSE_BUTTON_DOWN:");
    const std::size_t wheel = window.find("case SDL_EVENT_MOUSE_WHEEL:");
    CHECK(window.find("if (!ui && !touch_mouse) {", motion) < window.find("dcp_mouse_motion(", motion));
    CHECK(window.find("if (!ui && !touch_mouse) {", down) < window.find("dcp_mouse_button(dcp_mouse_global(), event.button.button, 1)", down));
    CHECK(window.find("if (!ui && !touch_mouse) {", wheel) < window.find("dcp_mouse_wheel(", wheel));
    CHECK(Contains(window, "DcpOverlay::CapturesInput(),") && Contains(window, "text_active};"));

    // The pad sampler takes one mouse sample per read of player 1, before anything that could add a touch, and Pointer
    // Control's finger is not replaced by an invented click finger.
    CHECK(Count(pad, "dcp_mouse_sample(") == 1);
    const std::size_t host = pad.find("static void sample_host(int slot, PadData *d)");
    CHECK(host != std::string::npos);
    const std::size_t sampled = pad.find("dcp_mouse_sample(dcp_mouse_global(),&mouse);", host);
    const std::size_t gamepad = pad.find("if (g) {", host);
    const std::size_t merged = pad.find("merge_mouse(d,&mouse);", host);
    const std::size_t keyboard = pad.find("if (!k) return;", host);
    CHECK(sampled < gamepad && gamepad < merged && merged < keyboard);
    CHECK(pad.find("if (slot==0) {", pad.find("DcpMouseSample mouse={0};", host)) < sampled);
    const std::size_t click = pad.find("static void touch_click(PadData *d, int right) {");
    CHECK(pad.find("if (pointer_owns_touch) return;", click) < pad.find("d->touch_count=1;", click));
    CHECK(pad.find("if (dcpgpu_overlay_captures_input()) return;", host) < sampled);
    CHECK(Contains(pad, "runtime_pad_set_settings(const char *settings_file) {") && Contains(pad, "dcp_mouse_configure(dcp_mouse_global(),&mouse);"));

    // Online play sees ordinary pad data: no mouse code in the online protocol, the lobby or the Worker.
    for (const char* online : {"src/runtime_online.cpp", "src/runtime_matching2.cpp", "src/net/lobby_link.cpp", "src/net/peer_socket.cpp",
                               "src/net/relay_frame.cpp"}) {
        const std::string text = ReadSource(online);
        CHECK(!Contains(text, "dcp_mouse") && !Contains(text, "mouse_"));
    }
}

bool Near(int value, int expected) { return value >= expected - 1 && value <= expected + 1; }

constexpr DcpTouchArea kWhole{0.0f, 0.0f, 1.0f, 1.0f};
constexpr std::uint64_t kMs = 1000000;

// Drives a touchscreen state as the window thread and the pad sampler do. Every read is checked to carry no button but
// the touchpad click.
struct Screen {
    DcpTouchscreen* state = dcp_touchscreen_new();
    DcpTouchArea area = kWhole;
    std::uint64_t now = 0;

    ~Screen() { dcp_touchscreen_free(state); }
    Screen() = default;
    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;

    void Down(std::uint64_t finger, float x, float y) { dcp_touchscreen_down(state, finger, area, x, y, now); }
    void Move(std::uint64_t finger, float x, float y) { dcp_touchscreen_motion(state, finger, area, x, y); }
    void Up(std::uint64_t finger, float x, float y) { dcp_touchscreen_up(state, finger, area, x, y, now); }
    DcpTouchSample Read() {
        DcpTouchSample sample;
        dcp_touchscreen_sample(state, &sample);
        CHECK((sample.buttons & ~static_cast<std::uint32_t>(BTN_TOUCHPAD)) == 0);
        CHECK(sample.touch || sample.buttons == 0);
        return sample;
    }
    bool Idle() {
        const DcpTouchSample sample = Read();
        return !sample.touch && sample.buttons == 0;
    }
};

void TestTouchMapping() {
    std::uint16_t x = 0, y = 0;
    CHECK(dcp_touch_point(kWhole, 0.0f, 0.0f, &x, &y) && x == 0 && y == 0);
    CHECK(dcp_touch_point(kWhole, 1.0f, 1.0f, &x, &y) && x == DCP_TOUCH_MAX_X && y == DCP_TOUCH_MAX_Y);
    CHECK(dcp_touch_point(kWhole, 0.5f, 0.5f, &x, &y) && x == 960 && y == 471);

    // Unknown geometry (nothing presented yet) is the whole window.
    const DcpTouchArea unknown = dcp_touch_area_from_pixels(0, 0, 0, 0, 0, 0);
    CHECK(unknown.x == 0.0f && unknown.y == 0.0f && unknown.width == 1.0f && unknown.height == 1.0f);
    const DcpTouchArea stretch = dcp_touch_area_from_pixels(0, 0, 1920, 1080, 1920, 1080);
    CHECK(stretch.x == 0.0f && stretch.y == 0.0f && stretch.width == 1.0f && stretch.height == 1.0f);

    // A 16:9 picture letterboxed on a 1280x800 screen (FitRegion: 0,40 1280x720).
    const DcpTouchArea letterbox = dcp_touch_area_from_pixels(0, 40, 1280, 720, 1280, 800);
    CHECK(dcp_touch_point(letterbox, 0.5f, 0.5f, &x, &y) && x == 960 && Near(y, 471));
    CHECK(dcp_touch_point(letterbox, 0.0f, 41.0f / 800.0f, &x, &y) && x == 0 && Near(y, 1));
    CHECK(dcp_touch_point(letterbox, 1.0f, 759.0f / 800.0f, &x, &y) && x == DCP_TOUCH_MAX_X && Near(y, DCP_TOUCH_MAX_Y - 1));
    CHECK(!dcp_touch_point(letterbox, 0.5f, 0.02f, &x, &y) && y == 0);
    CHECK(!dcp_touch_point(letterbox, 0.5f, 0.98f, &x, &y) && y == DCP_TOUCH_MAX_Y);

    // A 16:9 picture pillarboxed on a 2560x1080 screen.
    const DcpTouchArea pillarbox = dcp_touch_area_from_pixels(320, 0, 1920, 1080, 2560, 1080);
    CHECK(dcp_touch_point(pillarbox, 0.125f, 0.0f, &x, &y) && x == 0 && y == 0);
    CHECK(dcp_touch_point(pillarbox, 0.875f, 1.0f, &x, &y) && x == DCP_TOUCH_MAX_X && y == DCP_TOUCH_MAX_Y);
    CHECK(dcp_touch_point(pillarbox, 0.5f, 0.5f, &x, &y) && x == 960 && y == 471);
    CHECK(!dcp_touch_point(pillarbox, 0.05f, 0.5f, &x, &y) && x == 0);
    CHECK(!dcp_touch_point(pillarbox, 0.95f, 0.5f, &x, &y) && x == DCP_TOUCH_MAX_X);

    // The same physical point maps the same way at any surface resolution of the same shape.
    const DcpTouchArea deck = dcp_touch_area_from_pixels(0, 40, 1280, 720, 1280, 800);
    const DcpTouchArea deck_scaled = dcp_touch_area_from_pixels(0, 60, 1920, 1080, 1920, 1200);
    std::uint16_t x2 = 0, y2 = 0;
    dcp_touch_point(deck, 0.3f, 0.7f, &x, &y);
    dcp_touch_point(deck_scaled, 0.3f, 0.7f, &x2, &y2);
    CHECK(Near(x, x2) && Near(y, y2));
}

void TestTouchContactLifetime() {
    Screen screen;
    CHECK(screen.Idle());

    // Down creates contact 0 at the touched point; motion follows it; a drag lifts with no click.
    screen.Down(1, 0.25f, 0.5f);
    DcpTouchSample sample = screen.Read();
    CHECK(sample.touch && sample.x == 480 && sample.y == 471 && sample.buttons == 0);
    const std::uint8_t first_id = sample.id;
    screen.Move(1, 0.75f, 0.25f);
    sample = screen.Read();
    CHECK(sample.touch && sample.x == 1439 && Near(sample.y, 236) && sample.id == first_id);
    screen.now += 100 * kMs;
    screen.Up(1, 0.75f, 0.25f);
    CHECK(screen.Idle());
    CHECK(screen.Idle());

    // A tap clicks the touchpad at the touched point for a few reads, then lets go of both.
    screen.now = 1000 * kMs;
    screen.Down(2, 0.5f, 0.5f);
    sample = screen.Read();
    CHECK(sample.touch && sample.buttons == 0 && sample.id != first_id);
    screen.now += 120 * kMs;
    screen.Up(2, 0.5f, 0.5f);
    int clicks = 0;
    for (int read = 0; read < 3; ++read) {
        sample = screen.Read();
        clicks += sample.touch && sample.buttons == BTN_TOUCHPAD && sample.x == 960 && sample.y == 471;
    }
    CHECK(clicks == 3);
    CHECK(screen.Idle());

    // A tap between two reads still shows the contact once before the click.
    screen.now = 2000 * kMs;
    screen.Down(3, 0.1f, 0.1f);
    screen.now += 10 * kMs;
    screen.Up(3, 0.1f, 0.1f);
    sample = screen.Read();
    CHECK(sample.touch && sample.buttons == 0);
    CHECK(screen.Read().buttons == BTN_TOUCHPAD);

    // A long press does not click.
    Screen held;
    held.Down(1, 0.5f, 0.5f);
    CHECK(held.Read().touch);
    held.now += 800 * kMs;
    held.Up(1, 0.5f, 0.5f);
    CHECK(held.Idle());

    // A finger dragged off the picture stays on its edge.
    Screen edge;
    edge.area = dcp_touch_area_from_pixels(0, 40, 1280, 720, 1280, 800);
    edge.Down(1, 0.5f, 0.5f);
    edge.Move(1, 0.5f, 0.99f);
    sample = edge.Read();
    CHECK(sample.touch && sample.y == DCP_TOUCH_MAX_Y);
}

void TestTouchOutsidePicture() {
    Screen screen;
    screen.area = dcp_touch_area_from_pixels(0, 40, 1280, 720, 1280, 800);
    screen.Down(1, 0.5f, 0.02f); // on the black band
    CHECK(screen.Idle());
    screen.Move(1, 0.5f, 0.5f);
    CHECK(screen.Idle());
    screen.Up(1, 0.5f, 0.5f);
    CHECK(screen.Idle());
}

void TestSecondFinger() {
    Screen screen;
    screen.Down(1, 0.25f, 0.5f);
    screen.Down(2, 0.75f, 0.5f);
    DcpTouchSample sample = screen.Read();
    CHECK(sample.touch && sample.x == 480);
    screen.Move(2, 0.9f, 0.9f);
    screen.Up(2, 0.9f, 0.9f);
    sample = screen.Read();
    CHECK(sample.touch && sample.x == 480 && sample.buttons == 0);
    screen.Up(1, 0.25f, 0.5f);
    CHECK(screen.Read().buttons == BTN_TOUCHPAD);

    // The second finger never takes over once the first one lifts.
    Screen other;
    other.Down(1, 0.25f, 0.5f);
    other.Down(2, 0.75f, 0.5f);
    CHECK(other.Read().x == 480);
    other.now += 900 * kMs;
    other.Up(1, 0.25f, 0.5f);
    CHECK(other.Idle());
    other.Move(2, 0.8f, 0.5f);
    CHECK(other.Idle());
    other.Up(2, 0.8f, 0.5f);
    CHECK(other.Idle());
}

void TestTouchCancellation() {
    Screen focus;
    focus.Down(1, 0.5f, 0.5f);
    CHECK(focus.Read().touch);
    dcp_touchscreen_release_all(focus.state); // focus loss, window mode switch, shutdown
    CHECK(focus.Idle());
    focus.Up(1, 0.5f, 0.5f); // the stale lift changes nothing
    CHECK(focus.Idle());

    Screen click;
    click.Down(1, 0.5f, 0.5f);
    click.Read();
    click.Up(1, 0.5f, 0.5f);
    CHECK(click.Read().buttons == BTN_TOUCHPAD);
    dcp_touchscreen_release_all(click.state);
    CHECK(click.Idle());

    Screen cancel;
    cancel.Down(4, 0.5f, 0.5f);
    dcp_touchscreen_cancel(cancel.state, 5);
    CHECK(cancel.Read().touch);
    dcp_touchscreen_cancel(cancel.state, 4);
    CHECK(cancel.Idle());

    // Overlay, text entry or focus away: no contact starts and a held one ends.
    Screen blocked;
    blocked.Down(1, 0.5f, 0.5f);
    dcp_touchscreen_block(blocked.state, 1);
    CHECK(blocked.Idle());
    blocked.Down(2, 0.5f, 0.5f);
    CHECK(blocked.Idle());
    dcp_touchscreen_block(blocked.state, 0);
    blocked.Up(2, 0.5f, 0.5f);
    CHECK(blocked.Idle());
    blocked.Down(3, 0.5f, 0.5f);
    CHECK(blocked.Read().touch);
}

void TestTouchRouting() {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.which = SDL_TOUCH_MOUSEID;
    event.button.button = SDL_BUTTON_LEFT;
    CHECK(DcpTouchRoute::TouchGeneratedMouse(event));
    event.button.which = 1; // a physical mouse
    CHECK(!DcpTouchRoute::TouchGeneratedMouse(event));
    event.button.which = 0; // the default mouse
    CHECK(!DcpTouchRoute::TouchGeneratedMouse(event));
    event.button.which = SDL_PEN_MOUSEID;
    CHECK(!DcpTouchRoute::TouchGeneratedMouse(event));
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.which = SDL_TOUCH_MOUSEID;
    CHECK(DcpTouchRoute::TouchGeneratedMouse(event));

    event = SDL_Event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.which = SDL_TOUCH_MOUSEID;
    CHECK(DcpTouchRoute::TouchGeneratedMouse(event));
    event.motion.which = 2;
    CHECK(!DcpTouchRoute::TouchGeneratedMouse(event));
    event = SDL_Event{};
    event.type = SDL_EVENT_MOUSE_WHEEL;
    event.wheel.which = 2;
    CHECK(!DcpTouchRoute::TouchGeneratedMouse(event));
    event = SDL_Event{};
    event.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    CHECK(!DcpTouchRoute::TouchGeneratedMouse(event) && !DcpTouchRoute::IsFingerEvent(event));

    for (const Uint32 type : {SDL_EVENT_FINGER_DOWN, SDL_EVENT_FINGER_MOTION, SDL_EVENT_FINGER_UP, SDL_EVENT_FINGER_CANCELED}) {
        event = SDL_Event{};
        event.type = type;
        CHECK(DcpTouchRoute::IsFingerEvent(event) && !DcpTouchRoute::TouchGeneratedMouse(event));
    }

    CHECK(DcpTouchRoute::AcceptedTouch(7, SDL_TOUCH_DEVICE_DIRECT));
    CHECK(!DcpTouchRoute::AcceptedTouch(7, SDL_TOUCH_DEVICE_INDIRECT_ABSOLUTE));
    CHECK(!DcpTouchRoute::AcceptedTouch(7, SDL_TOUCH_DEVICE_INDIRECT_RELATIVE));
    CHECK(!DcpTouchRoute::AcceptedTouch(7, SDL_TOUCH_DEVICE_INVALID));
    CHECK(!DcpTouchRoute::AcceptedTouch(SDL_MOUSE_TOUCHID, SDL_TOUCH_DEVICE_DIRECT));
    CHECK(!DcpTouchRoute::AcceptedTouch(SDL_PEN_TOUCHID, SDL_TOUCH_DEVICE_DIRECT));

    // A physical left click is still Square.
    Rig rig;
    rig.Down(DCP_MOUSE_LEFT);
    CHECK(rig.Read().buttons == BTN_SQUARE);
}

void TestTouchSourceSafety() {
    const std::string window = ReadSource("gpu/shim/window.cpp");
    const std::string pad = ReadSource("src/runtime_pad.c");
    const std::string touch = ReadSource("src/dcp_touchscreen.c");

    // Finger events go to the touchscreen and never reach the mouse handlers; touch-made mouse events skip the game.
    const std::size_t overlay = window.find("if (DcpOverlay::HandleEvent(event)) {");
    const std::size_t finger = window.find("if (DcpTouchRoute::IsFingerEvent(event)) {");
    const std::size_t routed = window.find("const bool touch_mouse = DcpTouchRoute::TouchGeneratedMouse(event);");
    const std::size_t sw = window.find("switch (event.type) {", routed);
    CHECK(overlay < finger && finger < window.find("HandleTouch(event, ui);", finger) &&
          window.find("continue;", finger) < routed && routed < sw);
    const std::size_t up = window.find("case SDL_EVENT_MOUSE_BUTTON_UP:");
    CHECK(window.find("if (!touch_mouse) {", up) < window.find("dcp_mouse_button(dcp_mouse_global(), event.button.button, 0)", up));
    const std::size_t release = window.find("void WindowSDL::ReleaseMouse() {");
    CHECK(window.find("dcp_touchscreen_release_all(dcp_touchscreen_global());", release) < window.find("}", release));
    const std::size_t capture = window.find("void WindowSDL::UpdateMouseCapture() {");
    CHECK(window.find("dcp_touchscreen_block(dcp_touchscreen_global(), blocked);", capture) < release);
    CHECK(Contains(window, "AcceptedTouch(finger.touchID, SDL_GetTouchDeviceType(finger.touchID))"));

    // The pad keeps the controller's and the mouse's buttons and adds the touchscreen's after both.
    const std::size_t host = pad.find("static void sample_host(int slot, PadData *d)");
    CHECK(Count(pad, "dcp_touchscreen_sample(") == 1);
    CHECK(pad.find("if (dcpgpu_overlay_captures_input()) return;", host) < pad.find("dcp_touchscreen_sample(", host));
    CHECK(pad.find("if (g) {", host) < pad.find("merge_mouse(d,&mouse);", host) &&
          pad.find("merge_mouse(d,&mouse);", host) < pad.find("merge_touchscreen(d,&screen);", host));
    const std::size_t merge = pad.find("static void merge_touchscreen(PadData *d, const DcpTouchSample *screen) {");
    const std::string body = pad.substr(merge, pad.find("\n}\n", merge) - merge);
    CHECK(Contains(body, "d->buttons|=screen->buttons;") && !Contains(body, "d->buttons=") && !Contains(body, "d->buttons&="));
    CHECK(Contains(pad, "pointer_owns_touch=mouse.pointer || screen.touch;"));

    CHECK(!Contains(touch, "#include <SDL3") && !Contains(touch, "SDL_"));
    CHECK(Contains(ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp"), "progress.destination_origin ="));
    CHECK(Contains(ReadSource("gpu/shadps4/video_core/renderer_opengl/gl_presenter.cpp"), "progress.destination_origin ="));
}

// Notes each event as the window and the pad do and collects the lines that would be logged.
struct Telemetry {
    DcpTouchTelemetry* state = dcp_touch_telemetry_new();
    std::vector<std::string> lines;

    ~Telemetry() { dcp_touch_telemetry_free(state); }
    Telemetry() = default;
    Telemetry(const Telemetry&) = delete;
    Telemetry& operator=(const Telemetry&) = delete;

    void Note(DcpTouchEvent event) {
        char line[96];
        if (dcp_touch_telemetry_note(state, event, line, sizeof(line))) {
            lines.emplace_back(line);
        }
    }
    void Window(const SDL_Event& event, bool accepted) {
        switch (DcpTouchRoute::CountedEvent(event, accepted)) {
        case DcpTouchRoute::Counted::Detected: Note(DCP_TOUCH_EVENT_DETECTED); break;
        case DcpTouchRoute::Counted::MouseSuppressed: Note(DCP_TOUCH_EVENT_MOUSE_SUPPRESSED); break;
        case DcpTouchRoute::Counted::None: break;
        }
    }
    void Pad(const DcpTouchSample& sample) {
        if (sample.new_contact) {
            Note(DCP_TOUCH_EVENT_TOUCHPAD_USED);
        }
    }
};

SDL_Event FingerEvent(Uint32 type) {
    SDL_Event event{};
    event.type = type;
    event.tfinger.touchID = 7;
    event.tfinger.fingerID = 3;
    event.tfinger.x = 0.25f;
    event.tfinger.y = 0.75f;
    return event;
}

SDL_Event MouseDown(SDL_MouseID which) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.which = which;
    event.button.button = SDL_BUTTON_LEFT;
    return event;
}

bool OnlyNameAndCount(const std::string& line) {
    const std::string prefix = "Touchscreen telemetry: event=";
    if (line.rfind(prefix, 0) != 0) {
        return false;
    }
    const std::size_t count = line.find(" count=");
    if (count == std::string::npos) {
        return false;
    }
    const std::string name = line.substr(prefix.size(), count - prefix.size());
    const std::string number = line.substr(count + 7);
    return name.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") == std::string::npos && !number.empty() &&
           number.find_first_not_of("0123456789") == std::string::npos;
}

void TestTouchscreenTelemetry() {
    Telemetry detected;
    const SDL_Event down = FingerEvent(SDL_EVENT_FINGER_DOWN);
    detected.Window(down, true);
    detected.Window(down, true);
    detected.Window(down, true);
    CHECK(detected.lines.size() == 1 && detected.lines[0] == "Touchscreen telemetry: event=touchscreen_detected count=1");

    Telemetry quiet;
    quiet.Window(down, false);
    for (const Uint32 type : {SDL_EVENT_FINGER_MOTION, SDL_EVENT_FINGER_UP, SDL_EVENT_FINGER_CANCELED}) {
        quiet.Window(FingerEvent(type), true);
    }
    CHECK(quiet.lines.empty());

    Screen screen;
    Telemetry pad;
    screen.Down(1, 0.5f, 0.5f);
    pad.Pad(screen.Read());
    CHECK(pad.lines.size() == 1 && pad.lines[0] == "Touchscreen telemetry: event=touchscreen_touchpad_used count=1");
    for (int i = 0; i < 200; ++i) {
        screen.Move(1, 0.5f + 0.001f * static_cast<float>(i % 50), 0.5f);
        pad.Pad(screen.Read());
    }
    CHECK(pad.lines.size() == 1);
    screen.now += 900 * kMs;
    screen.Up(1, 0.5f, 0.5f);
    pad.Pad(screen.Read());
    CHECK(pad.lines.size() == 1);
    screen.Down(2, 0.1f, 0.1f);
    pad.Pad(screen.Read());
    CHECK(pad.lines.size() == 1);
    for (int contact = 3; contact <= 10; ++contact) {
        screen.Up(static_cast<std::uint64_t>(contact - 1), 0.1f, 0.1f);
        screen.Read();
        screen.Read();
        screen.Down(static_cast<std::uint64_t>(contact), 0.1f, 0.1f);
        pad.Pad(screen.Read());
    }
    CHECK(pad.lines.size() == 2 && pad.lines[1] == "Touchscreen telemetry: event=touchscreen_touchpad_used count=10");

    Telemetry suppressed;
    for (int i = 0; i < 1000; ++i) {
        suppressed.Window(MouseDown(SDL_TOUCH_MOUSEID), true);
    }
    CHECK(suppressed.lines.size() == 4);
    CHECK(suppressed.lines[0] == "Touchscreen telemetry: event=touchscreen_synthetic_mouse_suppressed count=1");
    CHECK(suppressed.lines[3] == "Touchscreen telemetry: event=touchscreen_synthetic_mouse_suppressed count=1000");

    Telemetry other;
    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.which = SDL_TOUCH_MOUSEID;
    SDL_Event release = MouseDown(SDL_TOUCH_MOUSEID);
    release.type = SDL_EVENT_MOUSE_BUTTON_UP;
    other.Window(motion, true);
    other.Window(release, true);
    CHECK(other.lines.empty());

    Telemetry physical;
    physical.Window(MouseDown(1), true);
    physical.Window(MouseDown(0), true);
    physical.Window(MouseDown(SDL_PEN_MOUSEID), true);
    motion.motion.which = 1;
    physical.Window(motion, true);
    SDL_Event wheel{};
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.which = 1;
    physical.Window(wheel, true);
    for (const Uint32 type : {SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_EVENT_GAMEPAD_BUTTON_UP}) {
        SDL_Event button{};
        button.type = type;
        button.gbutton.button = SDL_GAMEPAD_BUTTON_WEST;
        physical.Window(button, true);
        button.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
        physical.Window(button, true);
    }
    Rig rig;
    rig.Down(DCP_MOUSE_LEFT);
    const DcpMouseSample square = rig.Read();
    physical.Pad(DcpTouchSample{square.buttons, 0, 0, 0, 0, 0});
    CHECK(square.buttons == BTN_SQUARE && physical.lines.empty());

    char line[96];
    CHECK(!dcp_touch_telemetry_note(detected.state, DCP_TOUCH_EVENT_COUNT, line, sizeof(line)));
    CHECK(!dcp_touch_telemetry_note(detected.state, static_cast<DcpTouchEvent>(-1), line, sizeof(line)));
    for (const Telemetry* source : {&detected, &pad, &suppressed}) {
        for (const std::string& text : source->lines) {
            CHECK(OnlyNameAndCount(text));
        }
    }
    CHECK(!OnlyNameAndCount("Touchscreen telemetry: event=touchscreen_detected count=1 x=480"));
    CHECK(!OnlyNameAndCount("Touchscreen telemetry: event=touchscreen_detected finger=3 count=1"));

    Screen contact;
    contact.Down(1, 0.3f, 0.3f);
    const DcpTouchSample first = contact.Read();
    const DcpTouchSample second = contact.Read();
    CHECK(first.new_contact && first.x == second.x && first.y == second.y && !second.new_contact);
    CHECK(!Screen().Read().new_contact);
}

void TestTouchTelemetrySource() {
    const std::string window = ReadSource("gpu/shim/window.cpp");
    const std::string pad = ReadSource("src/runtime_pad.c");
    CHECK(Count(window, "dcp_touch_telemetry_record(") == 2 && Count(pad, "dcp_touch_telemetry_record(") == 1);
    const std::size_t handle = window.find("void WindowSDL::HandleTouch(");
    const std::size_t down = window.find("case SDL_EVENT_FINGER_DOWN:", handle);
    const std::size_t record = window.find("dcp_touch_telemetry_record(DCP_TOUCH_EVENT_DETECTED)", handle);
    CHECK(window.find("AcceptedTouch(", handle) < record && window.find("if (ui) {", down) < record);
    const std::size_t counted = window.find("CountedEvent(event, false) == DcpTouchRoute::Counted::MouseSuppressed");
    CHECK(counted != std::string::npos && counted < window.find("switch (event.type) {", counted));
    CHECK(Contains(pad, "if (screen.new_contact) dcp_touch_telemetry_record(DCP_TOUCH_EVENT_TOUCHPAD_USED);"));
    for (const char* other : {"src/runtime_online.cpp", "src/net/lobby_link.cpp", "src/comms_session.cpp"}) {
        CHECK(!Contains(ReadSource(other), "dcp_touch"));
    }
}

} // namespace

int main() {
    TestDefaultsAndNames();
    TestSensitivitySteps();
    TestSensitivityMultipliers();
    TestConfigFile();
    TestKeyboardMappingsUnchanged();
    TestNormalMouseButtons();
    TestPointerActivationAndConsumption();
    TestPointerClickButtonCanBeRemapped();
    TestMovementScaling();
    TestGameScaleFollowsThePresentedPicture();
    TestCoordinatesStayValid();
    TestFingerLifetime();
    TestEdgeRecenter();
    TestStalledReadsAreBounded();
    TestFocusLossAndBlocking();
    TestMouseControlsSetting();
    TestControllerActivityEndsPointer();
    TestWheel();
    TestOverlayAndTextEntryBlockGameplayMouse();
    TestCursorVisibilityIntegration();
    TestSourceSafety();
    TestTouchMapping();
    TestTouchContactLifetime();
    TestTouchOutsidePicture();
    TestSecondFinger();
    TestTouchCancellation();
    TestTouchRouting();
    TestTouchSourceSafety();
    TestTouchscreenTelemetry();
    TestTouchTelemetrySource();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
