// Focused tests of the mouse and touchscreen input: bindings and their settings, Pointer Control's synthetic touch finger,
// touchscreen contacts and mapping, the capture rules and the source-level safety of the SDL handoff. No window, no SDL
// video, no game.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "dcp_cursor.h"
#include "dcp_keys.h"
#include "dcp_mouse.h"
#include "dcp_mouse_capture.h"
#include "dcp_pad_buttons.h"
#include "dcp_pad_owner.h"
#include "dcp_pointer_motion.h"
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
    std::vector<std::string> lines; // the telemetry lines the state sent

    explicit Rig(int mode = DCP_POINTER_MODE_HOLD) {
        dcp_mouse_defaults(&map);
        map.pointer_mode = mode;
        dcp_mouse_configure(state, &map);
        dcp_mouse_set_log_sink(state, &Rig::Log, this);
    }
    ~Rig() { dcp_mouse_state_free(state); }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    static void Log(const char* line, void* user) { static_cast<Rig*>(user)->lines.emplace_back(line); }
    // The lines that are exactly "Mouse telemetry: <text>".
    int Lines(const std::string& text) const {
        return static_cast<int>(std::count(lines.begin(), lines.end(), "Mouse telemetry: " + text));
    }
    // The lines of one event, whatever the count.
    int Events(const std::string& event) const {
        return static_cast<int>(std::count_if(lines.begin(), lines.end(),
                                              [&](const std::string& line) { return Contains(line, "event=" + event + " "); }));
    }

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
    CHECK(DCP_KEY_COUNT == 30 && DCP_KEY_FIRST_HOTKEY == 25 && DCP_KEY_HOTKEY_EXIT_GAME == 28 && DCP_KEY_HOTKEY_CHARACTER_HIGHLIGHT == 29);
    DcpKeyMap keys;
    dcp_keys_defaults(&keys);
    CHECK(keys.scancode[DCP_KEY_LEFT_STICK_UP] == dcp_key_from_name("w") && keys.scancode[DCP_KEY_LEFT_STICK_LEFT] == dcp_key_from_name("a"));
    CHECK(keys.scancode[DCP_KEY_CROSS] == dcp_key_from_name("space") && keys.scancode[DCP_KEY_CIRCLE] == dcp_key_from_name("lshift"));
    CHECK(keys.scancode[DCP_KEY_SQUARE] == dcp_key_from_name("e") && keys.scancode[DCP_KEY_TRIANGLE] == dcp_key_from_name("q"));
    CHECK(keys.scancode[DCP_KEY_TOUCHPAD_LEFT] == dcp_key_from_name("tab") && keys.scancode[DCP_KEY_TOUCHPAD_RIGHT] == dcp_key_from_name("backspace"));
    CHECK(keys.scancode[DCP_KEY_HOTKEY_CHEATS] == dcp_key_from_name("f10") && keys.scancode[DCP_KEY_HOTKEY_WINDOW_MODE] == dcp_key_from_name("f11"));
    CHECK(keys.scancode[DCP_KEY_HOTKEY_COMMS_WHEEL] == dcp_key_from_name("t") && keys.scancode[DCP_KEY_HOTKEY_EXIT_GAME] == dcp_key_from_name("f9"));

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

void TestExitGameHotkey() {
    CHECK(DCP_KEY_HOTKEY_EXIT_GAME == DCP_KEY_HOTKEY_COMMS_WHEEL + 1 && DCP_KEY_HOTKEY_EXIT_GAME == DCP_KEY_COUNT - 2);
    CHECK(std::string(dcp_key_actions[DCP_KEY_HOTKEY_EXIT_GAME].ini_key) == "hotkey_exit_game" &&
          std::string(dcp_key_actions[DCP_KEY_HOTKEY_EXIT_GAME].label) == "Exit Game");
    DcpKeyMap keys;
    dcp_keys_defaults(&keys);
    CHECK(keys.scancode[DCP_KEY_HOTKEY_EXIT_GAME] == dcp_key_from_name("f9") && keys.scancode[DCP_KEY_HOTKEY_EXIT_GAME] != dcp_key_from_name("f12"));
    for (int i = 0; i < DCP_KEY_HOTKEY_EXIT_GAME; ++i) {
        CHECK(keys.scancode[i] != keys.scancode[DCP_KEY_HOTKEY_EXIT_GAME]);
    }

    const fs::path directory = fs::temp_directory_path() / "dcp_mouse_tests_exit_key";
    fs::create_directories(directory);
    const std::string file = (directory / "dcp.ini").string();
    DcpKeyMap loaded;
    std::ofstream(file) << "hotkey_exit_game = Pause # custom\n";
    dcp_keys_load(file.c_str(), &loaded);
    CHECK(loaded.scancode[DCP_KEY_HOTKEY_EXIT_GAME] == dcp_key_from_name("pause"));
    CHECK(loaded.scancode[DCP_KEY_HOTKEY_CHEATS] == keys.scancode[DCP_KEY_HOTKEY_CHEATS]);
    for (const char* bad : {"notakey", "escape", "", "none"}) {
        std::ofstream(file) << "hotkey_exit_game = " << bad << "\n";
        dcp_keys_load(file.c_str(), &loaded);
        CHECK(loaded.scancode[DCP_KEY_HOTKEY_EXIT_GAME] == dcp_key_from_name("f9"));
        CHECK(!dcp_keys_apply(&loaded, "hotkey_exit_game", bad));
    }
    CHECK(dcp_keys_apply(&loaded, "hotkey_exit_game", "x") && loaded.scancode[DCP_KEY_HOTKEY_EXIT_GAME] == dcp_key_from_name("x"));
    dcp_keys_load((directory / "missing.ini").string().c_str(), &loaded);
    CHECK(loaded.scancode[DCP_KEY_HOTKEY_EXIT_GAME] == dcp_key_from_name("f9"));
    CHECK(!dcp_mouse_setting_key("hotkey_exit_game") && !dcp_mouse_setting_key("controller_exit_shortcut"));
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

void TestControllerTouchEndsPointer() {
    Rig rig;
    rig.Down(DCP_MOUSE_MIDDLE);
    CHECK(rig.Read().touch);
    dcp_mouse_controller_touch(rig.state);
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

    // A controller touch with no pointer held changes nothing.
    Rig idle;
    dcp_mouse_controller_touch(idle.state);
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
    // Only a touch on an opened controller's own touchpad ends Pointer Control; buttons, sticks and triggers do not.
    CHECK(Count(window, "dcp_mouse_controller_touch(") == 1);
    const std::size_t pad_touch = window.find("case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:");
    CHECK(pad_touch != std::string::npos &&
          window.find("dcp_mouse_controller_touch(dcp_mouse_global());", pad_touch) < window.find("break;", pad_touch));
    const std::size_t activity = window.find("if (controller_activity && !ui) {");
    CHECK(activity != std::string::npos);
    const std::string activity_block = window.substr(activity, window.find("}", activity) - activity);
    CHECK(Contains(activity_block, "cursor.ControllerActivity();") && !Contains(activity_block, "dcp_mouse_"));
    for (const char* source : {"gpu/shim/window.cpp", "src/runtime_pad.c", "src/dcp_mouse.c", "src/dcp_mouse.h"}) {
        CHECK(!Contains(ReadSource(source), "dcp_mouse_controller_activity"));
    }
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

DcpPadDevice Pad(uint32_t instance, uint64_t identity, int player_index = -1) {
    return DcpPadDevice{instance, identity, player_index};
}

std::vector<DcpPadAssignment> Scan(DcpPadOwners& owners, std::vector<DcpPadDevice> devices) {
    DcpPadAssignment out[DCP_PAD_OWNER_SLOTS];
    const size_t n = dcp_pad_owners_assign(&owners, devices.data(), devices.size(), out, DCP_PAD_OWNER_SLOTS);
    return std::vector<DcpPadAssignment>(out, out + n);
}

DcpPadOwners FreshOwners() {
    DcpPadOwners owners;
    dcp_pad_owners_init(&owners);
    return owners;
}

constexpr uint64_t kIdA = 0xa11ce, kIdB = 0xb0b, kIdC = 0xc0ffee;

void TestPadOwnershipStartupOrders() {
    {   // one controller before pad 1 opens
        DcpPadOwners owners = FreshOwners();
        const auto a = Scan(owners, {Pad(3, kIdA, 0)});
        CHECK(a.size() == 1 && a[0].slot == 0 && a[0].reason == DCP_PAD_ASSIGN_NEW);
    }
    {   // pad 1 opens before any controller is discovered
        DcpPadOwners owners = FreshOwners();
        CHECK(Scan(owners, {}).empty());
        CHECK(dcp_pad_owners_free_slot(&owners) == 0);
        const auto a = Scan(owners, {Pad(3, kIdA, 0)});
        CHECK(a.size() == 1 && a[0].slot == 0);
    }
    {   // both discovered in one scan before pad 2 opens
        DcpPadOwners owners = FreshOwners();
        const auto a = Scan(owners, {Pad(3, kIdA, 0), Pad(4, kIdB, 1)});
        CHECK(a.size() == 2);
        CHECK(dcp_pad_owners_slot_of(&owners, 3) == 0);
        CHECK(dcp_pad_owners_slot_of(&owners, 4) == 1);
    }
    {   // A first, B in a later scan; pad 2 opening and later scans keep both
        DcpPadOwners owners = FreshOwners();
        Scan(owners, {Pad(3, kIdA, 0)});
        const auto b = Scan(owners, {Pad(3, kIdA, 0), Pad(4, kIdB, 1)});
        CHECK(b.size() == 1 && b[0].slot == 1 && b[0].device.instance == 4);
        for (int i = 0; i < 20; ++i) CHECK(Scan(owners, {Pad(4, kIdB, 1), Pad(3, kIdA, 0)}).empty());
        CHECK(dcp_pad_owners_slot_of(&owners, 3) == 0);
        CHECK(dcp_pad_owners_slot_of(&owners, 4) == 1);
    }
}

void TestPadOwnershipEnumerationOrder() {
    {   // one scan listing B before A: the player index decides, not the list position
        DcpPadOwners owners = FreshOwners();
        Scan(owners, {Pad(8, kIdB, 1), Pad(7, kIdA, 0)});
        CHECK(dcp_pad_owners_slot_of(&owners, 7) == 0);
        CHECK(dcp_pad_owners_slot_of(&owners, 8) == 1);
    }
    {   // without player indices the list order is kept, and devices with one come first
        DcpPadOwners owners = FreshOwners();
        Scan(owners, {Pad(5, kIdA), Pad(6, kIdB), Pad(9, kIdC, 2)});
        CHECK(dcp_pad_owners_slot_of(&owners, 9) == 0);
        CHECK(dcp_pad_owners_slot_of(&owners, 5) == 1);
        CHECK(dcp_pad_owners_slot_of(&owners, 6) == 2);
    }
}

void TestPadOwnershipHotplug() {
    {   // B disconnects: A stays; B returns to P2
        DcpPadOwners owners = FreshOwners();
        Scan(owners, {Pad(3, kIdA, 0), Pad(4, kIdB, 1)});
        dcp_pad_owners_release(&owners, 1);
        CHECK(dcp_pad_owners_slot_of(&owners, 3) == 0);
        CHECK(Scan(owners, {Pad(3, kIdA, 0)}).empty());
        const auto b = Scan(owners, {Pad(3, kIdA, 0), Pad(11, kIdB, 1)});
        CHECK(b.size() == 1 && b[0].slot == 1 && b[0].reason == DCP_PAD_ASSIGN_RECLAIM);
    }
    {   // A disconnects and returns to P1; B never moves
        DcpPadOwners owners = FreshOwners();
        Scan(owners, {Pad(3, kIdA, 0), Pad(4, kIdB, 1)});
        dcp_pad_owners_release(&owners, 0);
        CHECK(dcp_pad_owners_slot_of(&owners, 4) == 1);
        const auto a = Scan(owners, {Pad(4, kIdB, 1), Pad(12, kIdA, 0)});
        CHECK(a.size() == 1 && a[0].slot == 0 && a[0].reason == DCP_PAD_ASSIGN_RECLAIM);
        CHECK(dcp_pad_owners_slot_of(&owners, 4) == 1);
    }
    {   // both drop (virtual pads recreated) and come back B first, in separate scans
        DcpPadOwners owners = FreshOwners();
        Scan(owners, {Pad(3, kIdA, 0), Pad(4, kIdB, 1)});
        dcp_pad_owners_release(&owners, 0);
        dcp_pad_owners_release(&owners, 1);
        const auto b = Scan(owners, {Pad(20, kIdB, 0)});
        CHECK(b.size() == 1 && b[0].slot == 1 && b[0].reason == DCP_PAD_ASSIGN_RECLAIM);
        const auto a = Scan(owners, {Pad(20, kIdB, 0), Pad(21, kIdA, 1)});
        CHECK(a.size() == 1 && a[0].slot == 0 && a[0].reason == DCP_PAD_ASSIGN_RECLAIM);
    }
    {   // both drop and come back in one scan, listed in the other order
        DcpPadOwners owners = FreshOwners();
        Scan(owners, {Pad(3, kIdA, 0), Pad(4, kIdB, 1)});
        dcp_pad_owners_release(&owners, 0);
        dcp_pad_owners_release(&owners, 1);
        Scan(owners, {Pad(31, kIdB), Pad(30, kIdA)});
        CHECK(dcp_pad_owners_slot_of(&owners, 30) == 0);
        CHECK(dcp_pad_owners_slot_of(&owners, 31) == 1);
    }
}

void TestPadOwnershipSteamVirtualPads() {
    // Steam Input pads share VID/PID and GUID; only the per-device identity and player index tell them apart.
    DcpPadOwners owners = FreshOwners();
    const uint64_t steam0 = 0x5157ea0, steam1 = 0x5157ea1;
    Scan(owners, {Pad(1, steam0, 0), Pad(2, steam1, 1)});
    CHECK(dcp_pad_owners_slot_of(&owners, 1) == 0);
    CHECK(dcp_pad_owners_slot_of(&owners, 2) == 1);
    dcp_pad_owners_release(&owners, 0);
    const auto back = Scan(owners, {Pad(2, steam1, 1), Pad(5, steam0, 0)});
    CHECK(back.size() == 1 && back[0].slot == 0 && back[0].reason == DCP_PAD_ASSIGN_RECLAIM);

    // Two devices with the same identity cannot claim a remembered player or report a move.
    DcpPadOwners twins = FreshOwners();
    Scan(twins, {Pad(1, 0x7, 0)});
    dcp_pad_owners_release(&twins, 0);
    const auto both = Scan(twins, {Pad(8, 0x7), Pad(9, 0x7)});
    CHECK(both.size() == 2);
    for (const auto& a : both) CHECK(a.reason == DCP_PAD_ASSIGN_NEW && a.swapped_with < 0);
}

void TestPadOwnershipSinglePlayer() {
    DcpPadOwners owners = FreshOwners();
    Scan(owners, {Pad(3, kIdA, 0)});
    dcp_pad_owners_release(&owners, 0);
    CHECK(Scan(owners, {Pad(4, kIdA, 0)})[0].slot == 0);
    dcp_pad_owners_release(&owners, 0);
    const auto other = Scan(owners, {Pad(5, kIdC, 0)});
    CHECK(other.size() == 1 && other[0].slot == 0 && other[0].reason == DCP_PAD_ASSIGN_NEW);
}

void TestPadOwnershipNoDuplicatesOrJumps() {
    DcpPadOwners owners = FreshOwners();
    const auto a = Scan(owners, {Pad(3, kIdA, 0), Pad(3, kIdA, 0)});
    CHECK(a.size() == 1);
    Scan(owners, {Pad(4, kIdB, 1)});
    for (int i = 0; i < 50; ++i) {
        const auto again = i % 2 ? Scan(owners, {Pad(3, kIdA, 0), Pad(4, kIdB, 1)})
                                 : Scan(owners, {Pad(4, kIdB, 1), Pad(3, kIdA, 0)});
        CHECK(again.empty());
    }
    CHECK(dcp_pad_owners_slot_of(&owners, 3) == 0);
    CHECK(dcp_pad_owners_slot_of(&owners, 4) == 1);
    int holders = 0;
    for (int i = 0; i < DCP_PAD_OWNER_SLOTS; ++i) holders += owners.active[i].used && owners.active[i].device.instance == 3;
    CHECK(holders == 1);

    // A full table assigns nothing and keeps everyone.
    Scan(owners, {Pad(5, 0x55, 2), Pad(6, 0x66, 3)});
    CHECK(Scan(owners, {Pad(7, 0x77)}).empty());
    CHECK(dcp_pad_owners_slot_of(&owners, 7) < 0);
}

bool TelemetryLineIsSafe(const std::string& line) {
    static const std::regex shape(
        "Local controller telemetry: event=local_controller_(assignment|reassignment|swap_detected) player=[1-4] count=[0-9]+");
    return std::regex_match(line, shape);
}

std::vector<std::string> TelemetryFor(DcpPadTelemetry* t, const std::vector<DcpPadAssignment>& assignments) {
    std::vector<std::string> out;
    for (const auto& a : assignments) {
        char lines[DCP_PAD_EVENT_COUNT][DCP_PAD_TELEMETRY_LINE];
        const size_t n = dcp_pad_telemetry_lines(t, &a, lines);
        for (size_t i = 0; i < n; ++i) out.emplace_back(lines[i]);
    }
    return out;
}

size_t CountEvent(const std::vector<std::string>& lines, const std::string& event) {
    return static_cast<size_t>(std::count_if(lines.begin(), lines.end(),
                                              [&](const std::string& l) { return Contains(l, "event=" + event + " "); }));
}

void TestPadOwnershipTelemetry() {
    DcpPadTelemetry* t = dcp_pad_telemetry_new();
    DcpPadOwners owners = FreshOwners();
    std::vector<std::string> lines = TelemetryFor(t, Scan(owners, {Pad(3, kIdA, 0), Pad(4, kIdB, 1)}));
    CHECK(lines.size() == 2);
    CHECK(Contains(lines[0], "event=local_controller_assignment player=1 count=1"));
    CHECK(Contains(lines[1], "event=local_controller_assignment player=2 count=1"));

    // Stable operation: rescans and reclaims give no reassignment or swap.
    for (int i = 0; i < 30; ++i) {
        dcp_pad_owners_release(&owners, i % 2);
        const uint32_t instance = 100 + static_cast<uint32_t>(i);
        auto more = TelemetryFor(t, Scan(owners, {Pad(instance, i % 2 ? kIdB : kIdA, i % 2)}));
        lines.insert(lines.end(), more.begin(), more.end());
    }
    CHECK(CountEvent(lines, "local_controller_reassignment") == 0);
    CHECK(CountEvent(lines, "local_controller_swap_detected") == 0);
    CHECK(Contains(lines.back(), "count=10")); // player 1 and 2 have 16 assignments each: lines at 1 and 10 only
    CHECK(lines.size() == 4);

    // A genuine move, then a proven exchange: A 0->1 while C held P1, then B 1->0.
    DcpPadOwners swap = FreshOwners();
    DcpPadTelemetry* s = dcp_pad_telemetry_new();
    TelemetryFor(s, Scan(swap, {Pad(3, kIdA, 0), Pad(4, kIdB, 1)}));
    dcp_pad_owners_release(&swap, 0);
    TelemetryFor(s, Scan(swap, {Pad(4, kIdB, 1), Pad(5, kIdC, 0)}));
    dcp_pad_owners_release(&swap, 1);
    const auto moved = Scan(swap, {Pad(5, kIdC, 0), Pad(6, kIdA, 1)});
    CHECK(moved.size() == 1 && moved[0].slot == 1 && moved[0].reason == DCP_PAD_ASSIGN_MOVED && moved[0].previous_slot == 0);
    CHECK(moved[0].swapped_with < 0);
    const auto moved_lines = TelemetryFor(s, moved);
    CHECK(CountEvent(moved_lines, "local_controller_reassignment") == 1);
    CHECK(CountEvent(moved_lines, "local_controller_swap_detected") == 0);
    dcp_pad_owners_release(&swap, 0);
    const auto exchanged = Scan(swap, {Pad(6, kIdA, 1), Pad(7, kIdB, 0)});
    CHECK(exchanged.size() == 1 && exchanged[0].slot == 0 && exchanged[0].previous_slot == 1 && exchanged[0].swapped_with == 1);
    const auto swap_lines = TelemetryFor(s, exchanged);
    CHECK(CountEvent(swap_lines, "local_controller_swap_detected") == 1);
    CHECK(Contains(swap_lines.back(), "event=local_controller_swap_detected player=1 count=1"));

    for (const auto& line : lines) CHECK(TelemetryLineIsSafe(line));
    for (const auto& line : swap_lines) CHECK(TelemetryLineIsSafe(line));
    dcp_pad_telemetry_free(t);
    dcp_pad_telemetry_free(s);
}

std::string FunctionBody(const std::string& source, const std::string& signature) {
    const size_t start = source.find(signature);
    if (start == std::string::npos) return {};
    const size_t end = source.find("\n}\n", start);
    return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

void TestPadOwnershipSource() {
    const std::string pad = ReadSource("src/runtime_pad.c");
    const std::string owner = ReadSource("src/dcp_pad_owner.c");
    const std::string refresh = FunctionBody(pad, "static void refresh_gamepads(void) {");
    CHECK(Contains(refresh, "dcp_pad_owners_assign(&owners,"));
    CHECK(Contains(refresh, "SDL_GetGamepadPlayerIndexForID"));
    CHECK(!Contains(refresh, "SDL_OpenGamepad")); // opened only after a player is chosen
    for (const char* online : {"online", "network", "comms", "early_story", "lobby"}) {
        CHECK(!Contains(refresh, online));
        CHECK(!Contains(owner, online));
    }
    // The keyboard, mouse and touchscreen stay player 1's.
    CHECK(Contains(pad, "const bool *k=slot==0 && SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;"));
    CHECK(Contains(pad, "static bool slot_connected(int slot) { return slot==0 || slots[slot].gamepad; }"));
    // Opening a pad handle never changes who owns it.
    const std::string open = FunctionBody(pad, "static ABI int32_t pad_open(");
    CHECK(!open.empty());
    CHECK(!Contains(open, "dcp_pad_owners_assign") && !Contains(open, "dcp_pad_owners_release"));
    // Telemetry carries no device names, VID/PID, GUID, path or serial.
    const std::string telemetry = owner.substr(owner.find("static const char *const telemetry_names"));
    for (const char* sensitive :
         {"SDL_", "device", "vendor", "product", "guid", "path", "serial", "instance", "identity", "player_index"})
        CHECK(!Contains(telemetry.substr(telemetry.find("struct DcpPadTelemetry")), sensitive));
    CHECK(!Contains(owner, "#include <SDL3"));
    CHECK(!Contains(pad, "XInput Controller #"));
}

// A Pointer Control stroke: the same motion before every read, with `other` held from read `press_at` up to read
// `release_at`. The pointer is `pointer` and the click is `click`, as a Steam Deck layout may bind them.
struct Stroke {
    Game game;
    std::vector<DcpMouseSample> samples;
    double input_x = 0, input_y = 0;
};

Stroke RunStroke(int pointer, int click, int other, int press_at, int release_at, int reads = 300, bool always_on = false) {
    Rig rig(always_on ? DCP_POINTER_MODE_ALWAYS_ON : DCP_POINTER_MODE_HOLD);
    rig.map.button[DCP_MOUSE_POINTER_CONTROL] = pointer;
    rig.map.button[DCP_MOUSE_POINTER_INTERACT] = click;
    rig.Configure();
    if (!always_on) {
        rig.Down(pointer);
    }
    Stroke stroke;
    for (int read = 0; read < reads; ++read) {
        if (other && read == press_at) {
            rig.Down(other);
        }
        if (other && read == release_at) {
            rig.Up(other);
        }
        rig.Move(7.0f, 3.0f);
        float dx = 0, dy = 0;
        dcp_mouse_to_touch_units(100, 7.0f, 3.0f, &dx, &dy);
        stroke.input_x += dx;
        stroke.input_y += dy;
        stroke.samples.push_back(rig.Read());
        stroke.game.Read(stroke.samples.back());
    }
    for (int read = 0; read < 20; ++read) {
        stroke.samples.push_back(rig.Read());
        stroke.game.Read(stroke.samples.back());
    }
    return stroke;
}

bool SameFinger(const Stroke& a, const Stroke& b) {
    if (a.samples.size() != b.samples.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.samples.size(); ++i) {
        const DcpMouseSample &x = a.samples[i], &y = b.samples[i];
        if (x.touch != y.touch || x.pointer != y.pointer || x.x != y.x || x.y != y.y || x.id != y.id) {
            return false;
        }
    }
    return true;
}

// DCP-2TG3WE: a held button never changes the finger the game reads.
void TestMotionUnaffectedByHeldButtons() {
    const Stroke base = RunStroke(DCP_MOUSE_MIDDLE, DCP_MOUSE_LEFT, 0, 0, 0);
    CHECK(std::fabs(base.game.pointer_x - base.input_x) <= 2.0 * base.game.segments);
    CHECK(std::fabs(base.game.pointer_y - base.input_y) <= 2.0 * base.game.segments);
    for (int other : {DCP_MOUSE_LEFT, DCP_MOUSE_RIGHT, DCP_MOUSE_X1}) {
        CHECK(SameFinger(base, RunStroke(DCP_MOUSE_MIDDLE, DCP_MOUSE_LEFT, other, 0, 1000))); // held throughout
        CHECK(SameFinger(base, RunStroke(DCP_MOUSE_MIDDLE, DCP_MOUSE_LEFT, other, 100, 200))); // pressed mid-stroke
    }
    // Pointer Control on the left button (a trackpad click) with the right button held.
    const Stroke left = RunStroke(DCP_MOUSE_LEFT, DCP_MOUSE_RIGHT, 0, 0, 0);
    CHECK(SameFinger(left, RunStroke(DCP_MOUSE_LEFT, DCP_MOUSE_RIGHT, DCP_MOUSE_RIGHT, 0, 1000)));
    CHECK(SameFinger(left, RunStroke(DCP_MOUSE_LEFT, DCP_MOUSE_RIGHT, DCP_MOUSE_X2, 50, 250)));
    CHECK(SameFinger(base, left));

    // Sustained: the finger moves on every read but the recentres, whatever is held.
    const Stroke held = RunStroke(DCP_MOUSE_MIDDLE, DCP_MOUSE_LEFT, DCP_MOUSE_RIGHT, 0, 1000);
    int still = 0, lifted = 0;
    for (std::size_t i = 1; i < held.samples.size(); ++i) {
        const DcpMouseSample &a = held.samples[i - 1], &b = held.samples[i];
        lifted += !b.touch;
        still += i < 300 && a.touch && b.touch && a.x == b.x && a.y == b.y;
    }
    CHECK(still == 0);
    CHECK(lifted >= 3 && (lifted == held.game.segments - 1 || lifted == held.game.segments));
    CHECK(std::fabs(held.game.pointer_x - held.input_x) <= 2.0 * held.game.segments);
}

double Travel(Rig& rig, Game& game, int reads) {
    const double before = game.pointer_x;
    for (int i = 0; i < reads; ++i) {
        game.Read(rig.Read());
    }
    return game.pointer_x - before;
}

void TestMotionAndButtonOrdering() {
    const double unit = 30.0 / 0.6; // 30 game pixels at 100% in touch units
    // Pointer Control down and motion before the same read: delivered once the finger is down.
    {
        Rig rig;
        Game game;
        rig.Down(DCP_MOUSE_MIDDLE);
        rig.Move(30.0f, 0.0f);
        CHECK(std::fabs(Travel(rig, game, 3) - unit) < 1.0);
    }
    // Motion before Pointer Control is pressed is host cursor motion: the game never sees it (unchanged behaviour).
    {
        Rig rig;
        Game game;
        rig.Move(30.0f, 0.0f);
        rig.Down(DCP_MOUSE_MIDDLE);
        CHECK(std::fabs(Travel(rig, game, 3)) < 1.0);
    }
    // Another button around motion while the pointer is held: before down, after down, before up, right after up.
    for (int other : {DCP_MOUSE_LEFT, DCP_MOUSE_RIGHT}) {
        Rig rig;
        Game game;
        rig.Down(DCP_MOUSE_MIDDLE);
        Travel(rig, game, 2);
        rig.Move(30.0f, 0.0f);
        rig.Down(other);
        CHECK(std::fabs(Travel(rig, game, 1) - unit) < 1.0);
        rig.Down(other);
        rig.Move(30.0f, 0.0f);
        CHECK(std::fabs(Travel(rig, game, 1) - unit) < 1.0);
        rig.Move(30.0f, 0.0f);
        rig.Up(other);
        CHECK(std::fabs(Travel(rig, game, 1) - unit) < 1.0);
        rig.Up(other);
        rig.Move(30.0f, 0.0f);
        CHECK(std::fabs(Travel(rig, game, 1) - unit) < 1.0);
        rig.Down(other);
        rig.Move(15.0f, 0.0f);
        rig.Up(other);
        rig.Move(15.0f, 0.0f);
        CHECK(std::fabs(Travel(rig, game, 1) - unit) < 1.0);
    }
    // Releasing Pointer Control itself ends the stroke: motion queued before the release is dropped (unchanged).
    {
        Rig rig;
        Game game;
        rig.Down(DCP_MOUSE_MIDDLE);
        Travel(rig, game, 2);
        rig.Move(30.0f, 0.0f);
        rig.Up(DCP_MOUSE_MIDDLE);
        CHECK(std::fabs(Travel(rig, game, 2)) < 1.0);
    }
}

void TestCaptureModesWithHeldButtons() {
    using DcpMouseCapture::Frame;
    DcpMouseCapture::Tracker tracker;
    // Absolute (host cursor) until Pointer Control; other held buttons never flip the mode.
    CHECK(!tracker.Update(Frame{false, true, false, false}).has_value());
    CHECK(tracker.Update(Frame{true, true, false, false}) == std::optional<bool>(true));
    for (int i = 0; i < 100; ++i) {
        CHECK(!tracker.Update(Frame{true, true, false, false}).has_value());
    }
    CHECK(tracker.Update(Frame{false, true, false, false}) == std::optional<bool>(false));

    // A held button does not change what Pointer Control or capture sees.
    Rig rig;
    rig.Down(DCP_MOUSE_MIDDLE);
    rig.Down(DCP_MOUSE_RIGHT);
    CHECK(dcp_mouse_pointer_active(rig.state) && dcp_mouse_any_held(rig.state));
    rig.Up(DCP_MOUSE_RIGHT);
    rig.Up(DCP_MOUSE_MIDDLE);
    CHECK(!dcp_mouse_any_held(rig.state));
    rig.Down(DCP_MOUSE_LEFT);
    CHECK(!dcp_mouse_pointer_active(rig.state) && dcp_mouse_any_held(rig.state));
    dcp_mouse_block(rig.state, 1);
    CHECK(!dcp_mouse_any_held(rig.state));

    // The window forwards the same relative motion in either mode, once per event, with no button gate.
    const std::string window = ReadSource("gpu/shim/window.cpp");
    CHECK(Count(window, "dcp_mouse_motion(") == 1);
    CHECK(Contains(window, "dcp_mouse_motion(dcp_mouse_global(), event.motion.xrel * to_game, event.motion.yrel * to_game);"));
    const std::size_t motion = window.find("case SDL_EVENT_MOUSE_MOTION:");
    const std::string motion_case = window.substr(motion, window.find("break;", motion) - motion);
    CHECK(!Contains(motion_case, "button") && !Contains(motion_case, "SDL_GetMouseState"));
    CHECK(!Contains(window, "SDL_CaptureMouse") && !Contains(window, "SDL_SetWindowMouseGrab"));
}

// Controller sticks and the Steam Input virtual pad keep their path: the sampler reads the axes directly on each read and
// telemetry only observes.
void TestControllerPathUnchanged() {
    const std::string pad = ReadSource("src/runtime_pad.c");
    const std::string window = ReadSource("gpu/shim/window.cpp");
    CHECK(Contains(pad, "d->right_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX));"));
    CHECK(Contains(pad, "d->left_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTX));"));
    CHECK(Count(pad, "dcp_motion_record_read(") == 1);
    const std::size_t read_state = pad.find("static ABI int32_t pad_read_state(");
    CHECK(read_state != std::string::npos && pad.find("dcp_motion_record_read(SDL_GetTicksNS());", read_state) <
                                                 pad.find("static ABI int32_t pad_read(", read_state));
    CHECK(Count(window, "dcp_motion_record_host(") == 2);
    const std::size_t axis = window.find("case SDL_EVENT_GAMEPAD_AXIS_MOTION:\n            if (!ui && RightStickMoved(event)) {");
    CHECK(axis != std::string::npos);
    const std::string header = ReadSource("src/dcp_pointer_motion.h");
    CHECK(!Contains(header, "PadData") && !Contains(header, "DcpMouseSample"));
    // The pointer keeps the touch surface over pad fingers, and a touchscreen finger over the pointer (unchanged).
    const std::size_t host = pad.find("static void sample_host(int slot, PadData *d)");
    CHECK(pad.find("merge_mouse(d,&mouse);", host) < pad.find("merge_touchscreen(d,&screen);", host));
}

struct Motion {
    DcpMotionTelemetry* state = dcp_motion_telemetry_new();
    std::vector<std::string> lines;
    std::uint64_t now = 1000000000ull;

    ~Motion() { dcp_motion_telemetry_free(state); }
    Motion() = default;
    Motion(const Motion&) = delete;
    Motion& operator=(const Motion&) = delete;

    void Collect(char out[][DCP_MOTION_LINE_SIZE], std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            lines.emplace_back(out[i]);
        }
    }
    void Host(DcpMotionSource source, unsigned held) {
        char out[DCP_MOTION_MAX_LINES][DCP_MOTION_LINE_SIZE];
        Collect(out, dcp_motion_host(state, source, held, now, out));
    }
    void Read() {
        char out[DCP_MOTION_MAX_LINES][DCP_MOTION_LINE_SIZE];
        Collect(out, dcp_motion_read(state, now, out));
    }
    // Host motion every `event_ms` for `ms`, with a game read every 16 ms unless `reading` is false.
    void Stroke(DcpMotionSource source, unsigned held, int ms, int event_ms = 4, bool reading = true) {
        for (int t = 0; t < ms; ++t) {
            now += 1000000ull;
            if (t % event_ms == 0) {
                Host(source, held);
            }
            if (reading && t % 16 == 0) {
                Read();
            }
        }
    }
    void Idle(int ms) {
        for (int t = 0; t < ms; ++t) {
            now += 1000000ull;
            if (t % 16 == 0) {
                Read();
            }
        }
    }
    int With(const std::string& needle) const {
        return static_cast<int>(std::count_if(lines.begin(), lines.end(),
                                              [&](const std::string& line) { return Contains(line, needle); }));
    }
};

void TestPointerMotionTelemetry() {
    // Short or sparse motion is not sustained; idle reads say nothing.
    {
        Motion m;
        m.Idle(2000);
        m.Stroke(DCP_MOTION_SOURCE_MOUSE_POINTER, DCP_MOTION_HELD_MOUSE, 150);
        m.Idle(500);
        m.Stroke(DCP_MOTION_SOURCE_MOUSE_POINTER, DCP_MOTION_HELD_MOUSE, 2000, 200);
        CHECK(m.lines.empty());
    }
    // Sustained motion with nothing held: active once, nothing else.
    {
        Motion m;
        m.Stroke(DCP_MOTION_SOURCE_MOUSE, 0, 3000);
        CHECK(m.lines.size() == 1 && m.lines[0] == "Pointer telemetry: event=pointer_motion_active source=mouse count=1");
    }
    // Motion while a button is held: one line per stroke, naming the held class; no stall while the game reads.
    {
        Motion m;
        m.Stroke(DCP_MOTION_SOURCE_MOUSE_POINTER, DCP_MOTION_HELD_MOUSE, 3000);
        CHECK(m.lines.size() == 2);
        CHECK(m.lines[1] == "Pointer telemetry: event=pointer_motion_while_button_held source=mouse_pointer held=mouse_button count=1");
        m.Idle(500);
        m.Stroke(DCP_MOTION_SOURCE_GAMEPAD_AXIS, DCP_MOTION_HELD_GAMEPAD, 1000, 8);
        CHECK(m.With("source=gamepad_axis held=gamepad_button count=1") == 1);
        m.Idle(500);
        m.Stroke(DCP_MOTION_SOURCE_MOUSE, DCP_MOTION_HELD_MOUSE | DCP_MOTION_HELD_GAMEPAD, 1000);
        CHECK(m.With("source=mouse held=both count=1") == 1);
        CHECK(m.With("stall") == 0 && m.With("recovered") == 0);
    }
    // A button pressed partway through the stroke is still reported for that stroke.
    {
        Motion m;
        m.Stroke(DCP_MOTION_SOURCE_MOUSE_POINTER, 0, 500);
        m.Stroke(DCP_MOTION_SOURCE_MOUSE_POINTER, DCP_MOTION_HELD_MOUSE, 500);
        CHECK(m.With("event=pointer_motion_active") == 1 && m.With("event=pointer_motion_while_button_held") == 1);
    }
    // A stall: the host keeps sending motion while a button is held but the game stops reading.
    {
        Motion m;
        m.Stroke(DCP_MOTION_SOURCE_MOUSE_POINTER, DCP_MOTION_HELD_MOUSE, 500);
        m.Stroke(DCP_MOTION_SOURCE_MOUSE_POINTER, DCP_MOTION_HELD_MOUSE, 180, 4, false);
        CHECK(m.With("event=pointer_motion_stall_while_button_held source=mouse_pointer held=mouse_button count=1") == 1);
        m.Stroke(DCP_MOTION_SOURCE_MOUSE_POINTER, DCP_MOTION_HELD_MOUSE, 400);
        CHECK(m.With("event=pointer_motion_recovered source=mouse_pointer stall=lt250ms count=1") == 1);
        CHECK(m.With("stall_while_button_held") == 1); // one line per stall, not per event
        m.Stroke(DCP_MOTION_SOURCE_GAMEPAD_AXIS, DCP_MOTION_HELD_GAMEPAD, 300, 8);
        m.Stroke(DCP_MOTION_SOURCE_GAMEPAD_AXIS, DCP_MOTION_HELD_GAMEPAD, 1500, 8, false);
        m.Read();
        CHECK(m.With("event=pointer_motion_recovered source=gamepad_axis stall=ge1000ms count=1") == 1);
    }
    // Not a stall: ordinary reads with the Pointer Control recentre cadence, inactivity, no held button, host cursor
    // motion the game does not use, or no read seen yet.
    {
        Motion m;
        m.Stroke(DCP_MOTION_SOURCE_MOUSE_POINTER, DCP_MOTION_HELD_MOUSE, 500);
        m.now += 5000000000ull; // a long pause with nothing moving
        m.Stroke(DCP_MOTION_SOURCE_MOUSE_POINTER, DCP_MOTION_HELD_MOUSE, 90, 4, false);
        m.Stroke(DCP_MOTION_SOURCE_MOUSE_POINTER, 0, 600, 4, false);
        m.Stroke(DCP_MOTION_SOURCE_MOUSE, DCP_MOTION_HELD_MOUSE, 600, 4, false);
        CHECK(m.With("stall") == 0);
        Motion fresh;
        fresh.Stroke(DCP_MOTION_SOURCE_MOUSE_POINTER, DCP_MOTION_HELD_MOUSE, 1000, 4, false);
        CHECK(fresh.With("stall") == 0);
    }
    // Rate limit: a long session logs only the first line and every power of ten.
    {
        Motion m;
        for (int stroke = 0; stroke < 1000; ++stroke) {
            m.Stroke(DCP_MOTION_SOURCE_MOUSE_POINTER, DCP_MOTION_HELD_MOUSE, 250);
            m.Idle(200);
        }
        CHECK(m.With("event=pointer_motion_active") == 4 && m.With("event=pointer_motion_while_button_held") == 4);
        CHECK(m.With("count=1000") == 2 && m.lines.size() == 8);
    }
    // Bad input logs nothing.
    {
        char out[DCP_MOTION_MAX_LINES][DCP_MOTION_LINE_SIZE];
        CHECK(dcp_motion_host(nullptr, DCP_MOTION_SOURCE_MOUSE, 0, 1, out) == 0);
        CHECK(dcp_motion_read(nullptr, 1, out) == 0);
        Motion m;
        m.Host(DCP_MOTION_SOURCE_COUNT, DCP_MOTION_HELD_MOUSE);
        m.Host(static_cast<DcpMotionSource>(-1), DCP_MOTION_HELD_MOUSE);
        CHECK(m.lines.empty());
    }
}

void TestPointerMotionTelemetryPayload() {
    Motion m;
    m.Stroke(DCP_MOTION_SOURCE_MOUSE, DCP_MOTION_HELD_MOUSE, 500);
    m.Stroke(DCP_MOTION_SOURCE_MOUSE_POINTER, DCP_MOTION_HELD_MOUSE | DCP_MOTION_HELD_GAMEPAD, 500);
    m.Stroke(DCP_MOTION_SOURCE_MOUSE_POINTER, DCP_MOTION_HELD_MOUSE, 300, 4, false);
    m.Stroke(DCP_MOTION_SOURCE_GAMEPAD_AXIS, DCP_MOTION_HELD_GAMEPAD, 500, 8);
    m.Stroke(DCP_MOTION_SOURCE_GAMEPAD_AXIS, DCP_MOTION_HELD_GAMEPAD, 600, 8, false);
    m.Read();
    CHECK(m.lines.size() == 10);
    const std::regex safe("Pointer telemetry: event=pointer_motion_(active|while_button_held|stall_while_button_held|recovered) "
                          "source=(mouse|mouse_pointer|gamepad_axis)( held=(mouse_button|gamepad_button|both))?"
                          "( stall=(lt250ms|lt1000ms|ge1000ms))? count=[0-9]+");
    for (const std::string& line : m.lines) {
        CHECK(std::regex_match(line, safe));
    }
    // The module never sees a position, a delta, a device or an account.
    const std::string source = ReadSource("src/dcp_pointer_motion.c") + ReadSource("src/dcp_pointer_motion.h");
    for (const char* sensitive : {"%f", "%g", "SDL_", "name=", "vid", "pid", "steam", "account", "guid"}) {
        CHECK(!Contains(source, sensitive));
    }
    CHECK(!Contains(source, "float") && !Contains(source, "double"));
}

// ---- Full mouse rebinding and Pointer Control Mode ----

constexpr int kButtons[] = {DCP_MOUSE_LEFT, DCP_MOUSE_RIGHT, DCP_MOUSE_MIDDLE, DCP_MOUSE_X1, DCP_MOUSE_X2};

struct TargetExpectation {
    int target;
    const char* name;
    std::uint32_t buttons;
    int side; // the touchpad click side, 0 for none
};

// Written out independently of the tables under test.
constexpr TargetExpectation kTargets[] = {
    {DCP_PAD_TARGET_CROSS, "cross", BTN_CROSS, 0},
    {DCP_PAD_TARGET_CIRCLE, "circle", BTN_CIRCLE, 0},
    {DCP_PAD_TARGET_SQUARE, "square", BTN_SQUARE, 0},
    {DCP_PAD_TARGET_TRIANGLE, "triangle", BTN_TRIANGLE, 0},
    {DCP_PAD_TARGET_L1, "l1", BTN_L1, 0},
    {DCP_PAD_TARGET_R1, "r1", BTN_R1, 0},
    {DCP_PAD_TARGET_L2, "l2", BTN_L2, 0},
    {DCP_PAD_TARGET_R2, "r2", BTN_R2, 0},
    {DCP_PAD_TARGET_L3, "l3", BTN_L3, 0},
    {DCP_PAD_TARGET_R3, "r3", BTN_R3, 0},
    {DCP_PAD_TARGET_DPAD_UP, "dpad_up", BTN_UP, 0},
    {DCP_PAD_TARGET_DPAD_DOWN, "dpad_down", BTN_DOWN, 0},
    {DCP_PAD_TARGET_DPAD_LEFT, "dpad_left", BTN_LEFT, 0},
    {DCP_PAD_TARGET_DPAD_RIGHT, "dpad_right", BTN_RIGHT, 0},
    {DCP_PAD_TARGET_OPTIONS, "options", BTN_OPTIONS, 0},
    {DCP_PAD_TARGET_TOUCHPAD_LEFT, "touchpad_left", BTN_TOUCHPAD, 1},
    {DCP_PAD_TARGET_TOUCHPAD_RIGHT, "touchpad_right", BTN_TOUCHPAD, 2},
};
constexpr int kTargetCount = static_cast<int>(sizeof(kTargets) / sizeof(kTargets[0]));

int ActionOf(int target) {
    for (int action = 0; action < DCP_MOUSE_ACTION_COUNT; ++action) {
        if (dcp_mouse_actions[action].target == target) {
            return action;
        }
    }
    return -1;
}

bool SameMap(const DcpMouseMap& a, const DcpMouseMap& b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

void TestTargetTable() {
    CHECK(DCP_PAD_TARGET_COUNT == 17 && DCP_MOUSE_ACTION_COUNT == 19 && DCP_WHEEL_TARGET_COUNT == 18 && kTargetCount == 17);
    CHECK(DCP_MOUSE_SQUARE == 0 && DCP_MOUSE_CIRCLE == 1 && DCP_MOUSE_POINTER_CONTROL == 2 && DCP_MOUSE_POINTER_INTERACT == 3);
    CHECK(DCP_MOUSE_FIRST_EXTENDED == DCP_MOUSE_CROSS && DCP_WHEEL_CROSS == 1 && DCP_WHEEL_R2 == 8);
    unsigned seen = 0;
    for (const TargetExpectation& t : kTargets) {
        CHECK(std::string(dcp_pad_target_name(t.target)) == t.name && dcp_pad_target_from_name(t.name) == t.target);
        CHECK(dcp_pad_target_from_name(dcp_pad_target_name(t.target)) == t.target);
        // One list: the keyboard's action, the mouse's and the wheel's use the same name and label.
        CHECK(std::string(dcp_key_actions[DCP_KEY_CROSS + t.target].ini_key) == std::string("key_") + t.name);
        CHECK(std::string(dcp_pad_target_label(t.target)) == dcp_key_actions[DCP_KEY_CROSS + t.target].label);
        const int action = ActionOf(t.target);
        CHECK(action >= 0 && std::string(dcp_mouse_actions[action].ini_key) == std::string("mouse_") + t.name);
        CHECK(dcp_wheel_target_from_name(t.name) == t.target + 1 && std::string(dcp_wheel_target_name(t.target + 1)) == t.name);
        CHECK(std::string(dcp_wheel_target_label(t.target + 1)) == dcp_pad_target_label(t.target));
        CHECK(!(seen & (1u << t.target)));
        seen |= 1u << t.target;
    }
    CHECK(seen == (1u << DCP_PAD_TARGET_COUNT) - 1);
    CHECK(dcp_mouse_actions[DCP_MOUSE_POINTER_CONTROL].target == -1 && dcp_mouse_actions[DCP_MOUSE_POINTER_INTERACT].target == -1);
    CHECK(dcp_mouse_actions[DCP_MOUSE_SQUARE].target == DCP_PAD_TARGET_SQUARE && dcp_mouse_actions[DCP_MOUSE_CIRCLE].target == DCP_PAD_TARGET_CIRCLE);
    for (const char* bad : {"start", "", "none", "jump", "dodge", "f1", "comms_wheel", "hotkey_cheats_menu", "cheats", "touchpad"}) {
        CHECK(dcp_pad_target_from_name(bad) == -1);
    }
    CHECK(dcp_pad_target_from_name(nullptr) == -1);
    CHECK(std::string(dcp_pad_target_name(-1)).empty() && std::string(dcp_pad_target_name(DCP_PAD_TARGET_COUNT)).empty());
    CHECK(std::string(dcp_pad_target_label(-1)).empty() && std::string(dcp_wheel_target_name(0)) == "none");
    CHECK(std::string(dcp_wheel_target_name(DCP_WHEEL_TARGET_COUNT)) == "none" && std::string(dcp_wheel_target_label(0)) == "(none)");
    // The port's own hotkeys are not PS4 controls and are not offered.
    for (int action = 0; action < DCP_MOUSE_ACTION_COUNT; ++action) {
        const std::string key = dcp_mouse_actions[action].ini_key;
        CHECK(key.find("hotkey") == std::string::npos && key.find("comms") == std::string::npos && key.find("cheats") == std::string::npos);
    }

    // Defaults are the 1.4.0 ones; every added control is unbound and Pointer Control Mode is Hold.
    DcpMouseMap map;
    dcp_mouse_defaults(&map);
    for (int action = 0; action < DCP_MOUSE_ACTION_COUNT; ++action) {
        const int expected = action == DCP_MOUSE_SQUARE ? DCP_MOUSE_LEFT
                             : action == DCP_MOUSE_CIRCLE ? DCP_MOUSE_RIGHT
                             : action == DCP_MOUSE_POINTER_CONTROL ? DCP_MOUSE_MIDDLE
                             : action == DCP_MOUSE_POINTER_INTERACT ? DCP_MOUSE_LEFT
                                                                    : DCP_MOUSE_NONE;
        CHECK(map.button[action] == expected && dcp_mouse_actions[action].default_button == expected);
    }
    CHECK(map.pointer_mode == DCP_POINTER_MODE_HOLD && map.controls == 1 && map.sensitivity == 100);
    CHECK(map.wheel_up == DCP_WHEEL_NONE && map.wheel_down == DCP_WHEEL_NONE);
    CHECK(dcp_mouse_button_targets(&map, DCP_MOUSE_LEFT) == 1u << DCP_PAD_TARGET_SQUARE);
    CHECK(dcp_mouse_button_targets(&map, DCP_MOUSE_RIGHT) == 1u << DCP_PAD_TARGET_CIRCLE);
    CHECK(dcp_mouse_button_targets(&map, DCP_MOUSE_MIDDLE) == 0 && dcp_mouse_button_targets(&map, DCP_MOUSE_X1) == 0);
    CHECK(dcp_mouse_button_targets(&map, DCP_MOUSE_NONE) == 0 && dcp_mouse_button_targets(&map, 17) == 0);

    // One model on both platforms: the tables live in portable sources with no platform branch.
    for (const char* source : {"src/dcp_keys.c", "src/dcp_keys.h", "src/dcp_mouse.c", "src/dcp_mouse.h", "launcher/options.cpp",
                               "launcher/options.h"}) {
        const std::string text = ReadSource(source);
        CHECK(!text.empty() && !Contains(text, "_WIN32") && !Contains(text, "__linux__") && !Contains(text, "__unix__") &&
              !Contains(text, "_MSC_VER") && !Contains(text, "__APPLE__"));
    }
}

// A dcp.ini written by 1.4.0 or earlier loads to the same bindings and behaves the same.
void TestLegacyFilesStayIdentical() {
    const fs::path directory = fs::temp_directory_path() / "dcp_mouse_tests_legacy";
    fs::create_directories(directory);
    const std::string file = (directory / "dcp.ini").string();
    DcpMouseMap defaults;
    dcp_mouse_defaults(&defaults);
    DcpMouseMap map;
    DcpMouseLoadInfo info;
    {
        std::ofstream(file) << "# Dragon's Crown Pro user settings\nvsync = 1\nkey_cross = q\nmouse_controls = 1\nmouse_square = left\n"
                               "mouse_circle = right\nmouse_pointer_control = middle\nmouse_pointer_interact = left\n"
                               "mouse_pointer_sensitivity = 100\nmouse_wheel_up = none\nmouse_wheel_down = none\n";
    }
    dcp_mouse_load_info(file.c_str(), &map, &info);
    CHECK(SameMap(map, defaults) && info.rejected == 0 && info.extended_bound == 0);

    {
        std::ofstream(file) << "mouse_controls = 1\nmouse_square = x1\nmouse_circle = Mouse5\nmouse_pointer_control = right\n"
                               "mouse_pointer_interact = middle\nmouse_pointer_sensitivity = 150\nmouse_wheel_up = l1\nmouse_wheel_down = cross\n";
    }
    dcp_mouse_load_info(file.c_str(), &map, &info);
    CHECK(info.rejected == 0 && info.extended_bound == 0 && map.pointer_mode == DCP_POINTER_MODE_HOLD);
    CHECK(map.button[DCP_MOUSE_SQUARE] == DCP_MOUSE_X1 && map.button[DCP_MOUSE_CIRCLE] == DCP_MOUSE_X2);
    CHECK(map.button[DCP_MOUSE_POINTER_CONTROL] == DCP_MOUSE_RIGHT && map.button[DCP_MOUSE_POINTER_INTERACT] == DCP_MOUSE_MIDDLE);
    CHECK(map.sensitivity == 150 && map.wheel_up == DCP_WHEEL_L1 && map.wheel_down == DCP_WHEEL_CROSS);
    for (int action = DCP_MOUSE_FIRST_EXTENDED; action < DCP_MOUSE_ACTION_COUNT; ++action) {
        CHECK(map.button[action] == DCP_MOUSE_NONE); // an old file never gains a binding
    }
    Rig rig;
    rig.map = map;
    rig.Configure();
    rig.Down(DCP_MOUSE_X1);
    CHECK(rig.Read().buttons == BTN_SQUARE);
    rig.Down(DCP_MOUSE_X2);
    CHECK(rig.Read().buttons == (BTN_SQUARE | BTN_CIRCLE));
    rig.Up(DCP_MOUSE_X1);
    rig.Up(DCP_MOUSE_X2);
    rig.Down(DCP_MOUSE_LEFT);
    CHECK(rig.Read().buttons == 0);
    rig.Up(DCP_MOUSE_LEFT);
    rig.Down(DCP_MOUSE_RIGHT); // Pointer Control moved to the right button
    CHECK(rig.Read().touch && dcp_mouse_pointer_active(rig.state));
    rig.Down(DCP_MOUSE_MIDDLE); // and its click to the middle one
    CHECK(rig.Read().buttons == BTN_TOUCHPAD);
    CHECK(rig.lines.empty()); // nothing was migrated, so nothing is reported

    // The default map: every button does what it did.
    Rig fresh;
    for (const int button : kButtons) {
        fresh.Down(button);
        const DcpMouseSample sample = fresh.Read();
        const std::uint32_t expected = button == DCP_MOUSE_LEFT ? BTN_SQUARE : button == DCP_MOUSE_RIGHT ? BTN_CIRCLE : 0;
        CHECK(sample.buttons == expected && sample.touch == (button == DCP_MOUSE_MIDDLE) && sample.pointer == (button == DCP_MOUSE_MIDDLE));
        fresh.Up(button);
        CHECK(fresh.Read().buttons == 0);
    }
    fs::remove_all(directory);
}

void TestEveryButtonEveryTarget() {
    for (const int button : kButtons) {
        for (const TargetExpectation& t : kTargets) {
            Rig rig;
            // The default buttons are cleared so that the one under test is the only binding.
            rig.map.button[DCP_MOUSE_SQUARE] = DCP_MOUSE_NONE;
            rig.map.button[DCP_MOUSE_CIRCLE] = DCP_MOUSE_NONE;
            rig.map.button[DCP_MOUSE_POINTER_CONTROL] = DCP_MOUSE_NONE;
            rig.map.button[ActionOf(t.target)] = button;
            rig.Configure();
            CHECK(rig.Read().buttons == 0);
            rig.Down(button);
            const DcpMouseSample pressed = rig.Read();
            CHECK(pressed.buttons == t.buttons && pressed.touchpad_side == t.side && !pressed.touch && !pressed.pointer);
            CHECK(rig.Read().buttons == t.buttons); // held across reads
            rig.Up(button);
            const DcpMouseSample released = rig.Read();
            CHECK(released.buttons == 0 && released.touchpad_side == 0 && !released.touch); // nothing stays held
            for (const int other : kButtons) {
                if (other == button) {
                    continue;
                }
                rig.Down(other);
                CHECK(rig.Read().buttons == 0); // the other buttons stay unbound
                rig.Up(other);
            }
        }
    }
}

void TestRepresentativeBindings() {
    // Mouse 4 -> Cross and Mouse 5 -> R1, with every default kept.
    Rig rig;
    rig.map.button[DCP_MOUSE_CROSS] = DCP_MOUSE_X1;
    rig.map.button[DCP_MOUSE_R1] = DCP_MOUSE_X2;
    rig.Configure();
    rig.Down(DCP_MOUSE_X1);
    CHECK(rig.Read().buttons == BTN_CROSS);
    rig.Down(DCP_MOUSE_X2); // two buttons held together
    CHECK(rig.Read().buttons == (BTN_CROSS | BTN_R1));
    rig.Down(DCP_MOUSE_LEFT);
    CHECK(rig.Read().buttons == (BTN_CROSS | BTN_R1 | BTN_SQUARE));
    rig.Up(DCP_MOUSE_X1);
    CHECK(rig.Read().buttons == (BTN_R1 | BTN_SQUARE)); // releasing one leaves the other
    rig.Up(DCP_MOUSE_X2);
    rig.Up(DCP_MOUSE_LEFT);
    for (int i = 0; i < 100; ++i) {
        CHECK(rig.Read().buttons == 0);
    }
    for (int i = 0; i < 100; ++i) {
        rig.Down(DCP_MOUSE_X2);
        CHECK(rig.Read().buttons == BTN_R1);
        rig.Up(DCP_MOUSE_X2);
        CHECK(rig.Read().buttons == 0);
    }

    // A mouse button to a shoulder button, replacing the Circle it had.
    Rig shoulder;
    shoulder.map.button[DCP_MOUSE_CIRCLE] = DCP_MOUSE_NONE;
    shoulder.map.button[DCP_MOUSE_L1] = DCP_MOUSE_RIGHT;
    shoulder.Configure();
    shoulder.Down(DCP_MOUSE_RIGHT);
    CHECK(shoulder.Read().buttons == BTN_L1);

    // Changing one binding leaves the others as they were.
    DcpMouseMap before;
    dcp_mouse_defaults(&before);
    before.button[DCP_MOUSE_CROSS] = DCP_MOUSE_X1;
    before.button[DCP_MOUSE_R1] = DCP_MOUSE_X2;
    DcpMouseMap after = before;
    after.button[DCP_MOUSE_L2] = DCP_MOUSE_MIDDLE;
    for (int action = 0; action < DCP_MOUSE_ACTION_COUNT; ++action) {
        CHECK((before.button[action] == after.button[action]) == (action != DCP_MOUSE_L2));
    }
    Rig other;
    other.map = after;
    other.map.button[DCP_MOUSE_POINTER_CONTROL] = DCP_MOUSE_NONE;
    other.Configure();
    other.Down(DCP_MOUSE_X1);
    other.Down(DCP_MOUSE_X2);
    other.Down(DCP_MOUSE_MIDDLE);
    CHECK(other.Read().buttons == (BTN_CROSS | BTN_R1 | BTN_L2));

    // Touchpad left and right are clicks on that side of the pad.
    Rig pad;
    pad.map.button[DCP_MOUSE_TOUCHPAD_LEFT] = DCP_MOUSE_X1;
    pad.map.button[DCP_MOUSE_TOUCHPAD_RIGHT] = DCP_MOUSE_X2;
    pad.Configure();
    pad.Down(DCP_MOUSE_X1);
    DcpMouseSample click = pad.Read();
    CHECK(click.buttons == BTN_TOUCHPAD && click.touchpad_side == 1 && !click.touch);
    pad.Up(DCP_MOUSE_X1);
    pad.Down(DCP_MOUSE_X2);
    click = pad.Read();
    CHECK(click.buttons == BTN_TOUCHPAD && click.touchpad_side == 2);
    pad.Down(DCP_MOUSE_X1);
    CHECK(pad.Read().touchpad_side == 2); // both together: the right side

    // The wheel offers the same controls.
    Rig wheel;
    wheel.map.wheel_up = DCP_WHEEL_NONE + 1 + DCP_PAD_TARGET_DPAD_UP;
    wheel.map.wheel_down = DCP_WHEEL_NONE + 1 + DCP_PAD_TARGET_TOUCHPAD_RIGHT;
    wheel.Configure();
    dcp_mouse_wheel(wheel.state, 1.0f);
    dcp_mouse_wheel(wheel.state, -1.0f);
    const DcpMouseSample step = wheel.Read();
    CHECK(step.buttons == (BTN_UP | BTN_TOUCHPAD) && step.touchpad_side == 2);
    for (int i = 0; i < 3; ++i) {
        wheel.Read();
    }
    CHECK(wheel.Read().buttons == 0);
}

void TestDuplicateAndConflictPolicy() {
    // A hand-edited file may give one button several controls: every one of them presses, and nothing else.
    Rig multi;
    multi.map.button[DCP_MOUSE_CROSS] = DCP_MOUSE_X1;
    multi.map.button[DCP_MOUSE_R1] = DCP_MOUSE_X1;
    multi.Configure();
    multi.Down(DCP_MOUSE_X1);
    CHECK(multi.Read().buttons == (BTN_CROSS | BTN_R1));

    // The launcher's rule gives a button exactly one control, taking it from the button that had it.
    DcpMouseMap map;
    dcp_mouse_defaults(&map);
    map.button[DCP_MOUSE_CROSS] = DCP_MOUSE_X1;
    map.button[DCP_MOUSE_R1] = DCP_MOUSE_X1;
    CHECK(dcp_mouse_button_targets(&map, DCP_MOUSE_X1) == ((1u << DCP_PAD_TARGET_CROSS) | (1u << DCP_PAD_TARGET_R1)));
    int moved = -1;
    CHECK(dcp_mouse_assign_target(&map, DCP_MOUSE_X1, DCP_PAD_TARGET_TRIANGLE, &moved) && moved == DCP_MOUSE_NONE);
    CHECK(dcp_mouse_button_targets(&map, DCP_MOUSE_X1) == 1u << DCP_PAD_TARGET_TRIANGLE);
    CHECK(map.button[DCP_MOUSE_CROSS] == DCP_MOUSE_NONE && map.button[DCP_MOUSE_R1] == DCP_MOUSE_NONE);
    CHECK(dcp_mouse_assign_target(&map, DCP_MOUSE_X2, DCP_PAD_TARGET_TRIANGLE, &moved) && moved == DCP_MOUSE_X1);
    CHECK(dcp_mouse_button_targets(&map, DCP_MOUSE_X1) == 0 && dcp_mouse_button_targets(&map, DCP_MOUSE_X2) == 1u << DCP_PAD_TARGET_TRIANGLE);
    CHECK(dcp_mouse_assign_target(&map, DCP_MOUSE_X2, DCP_PAD_TARGET_TRIANGLE, &moved) && moved == DCP_MOUSE_NONE); // already there
    CHECK(dcp_mouse_assign_target(&map, DCP_MOUSE_LEFT, -1, nullptr) && dcp_mouse_button_targets(&map, DCP_MOUSE_LEFT) == 0);
    CHECK(map.button[DCP_MOUSE_SQUARE] == DCP_MOUSE_NONE && map.button[DCP_MOUSE_CIRCLE] == DCP_MOUSE_RIGHT); // only Left's control went
    CHECK(map.button[DCP_MOUSE_POINTER_INTERACT] == DCP_MOUSE_LEFT && map.button[DCP_MOUSE_POINTER_CONTROL] == DCP_MOUSE_MIDDLE);

    // Pointer Control owns its button in Hold and Pointer Interaction owns its button in Always On.
    DcpMouseMap hold;
    dcp_mouse_defaults(&hold);
    DcpMouseMap kept = hold;
    CHECK(dcp_mouse_button_locked(&hold, DCP_MOUSE_MIDDLE) && !dcp_mouse_button_locked(&hold, DCP_MOUSE_LEFT));
    CHECK(!dcp_mouse_assign_target(&hold, DCP_MOUSE_MIDDLE, DCP_PAD_TARGET_CROSS, &moved) && SameMap(hold, kept));
    CHECK(dcp_mouse_assign_target(&hold, DCP_MOUSE_LEFT, DCP_PAD_TARGET_CROSS, &moved) && moved == DCP_MOUSE_NONE);
    DcpMouseMap always = kept;
    always.pointer_mode = DCP_POINTER_MODE_ALWAYS_ON;
    kept = always;
    CHECK(dcp_mouse_button_locked(&always, DCP_MOUSE_LEFT) && !dcp_mouse_button_locked(&always, DCP_MOUSE_MIDDLE));
    CHECK(!dcp_mouse_assign_target(&always, DCP_MOUSE_LEFT, DCP_PAD_TARGET_CROSS, &moved) && SameMap(always, kept));
    CHECK(dcp_mouse_assign_target(&always, DCP_MOUSE_MIDDLE, DCP_PAD_TARGET_CROSS, &moved));
    CHECK(always.button[DCP_MOUSE_POINTER_CONTROL] == DCP_MOUSE_MIDDLE); // the saved Pointer Control button stays
    CHECK(!dcp_mouse_assign_target(&always, DCP_MOUSE_NONE, DCP_PAD_TARGET_CROSS, &moved));
    CHECK(!dcp_mouse_assign_target(&always, 17, DCP_PAD_TARGET_CROSS, &moved));
    CHECK(!dcp_mouse_assign_target(&always, DCP_MOUSE_X1, DCP_PAD_TARGET_COUNT, &moved));
    CHECK(!dcp_mouse_assign_target(&always, DCP_MOUSE_X1, -2, &moved) && !dcp_mouse_button_locked(&always, DCP_MOUSE_NONE));

    // Pointer Control's own button never presses a control in Hold, even when a file binds one to it.
    Rig control;
    control.map.button[DCP_MOUSE_POINTER_CONTROL] = DCP_MOUSE_X1;
    control.map.button[DCP_MOUSE_CROSS] = DCP_MOUSE_X1;
    control.Configure();
    control.Down(DCP_MOUSE_X1);
    DcpMouseSample sample = control.Read();
    CHECK(sample.pointer && sample.touch && sample.buttons == 0);
    control.Up(DCP_MOUSE_X1);
    CHECK(control.Read().buttons == 0);

    // Pointer Interaction owns its click: no control bound to that button leaks out of it.
    for (const int mode : {DCP_POINTER_MODE_HOLD, DCP_POINTER_MODE_ALWAYS_ON}) {
        Rig click(mode);
        click.map.button[DCP_MOUSE_CROSS] = DCP_MOUSE_LEFT; // Left also performs Square, and now Cross
        click.Configure();
        if (mode == DCP_POINTER_MODE_HOLD) {
            click.Down(DCP_MOUSE_LEFT);
            CHECK(click.Read().buttons == (BTN_SQUARE | BTN_CROSS)); // outside Pointer Control it is an ordinary button
            click.Up(DCP_MOUSE_LEFT);
            CHECK(click.Read().buttons == 0);
            click.Down(DCP_MOUSE_MIDDLE);
        }
        click.Read();
        click.Read();
        click.Down(DCP_MOUSE_LEFT);
        sample = click.Read();
        CHECK(sample.buttons == BTN_TOUCHPAD && sample.pointer);
        click.Down(DCP_MOUSE_RIGHT);
        CHECK(click.Read().buttons == (BTN_TOUCHPAD | BTN_CIRCLE)); // the other buttons keep their controls
        click.Up(DCP_MOUSE_LEFT);
        CHECK(click.Read().buttons == BTN_CIRCLE); // and the release leaks nothing
        click.Up(DCP_MOUSE_RIGHT);
        click.Down(DCP_MOUSE_LEFT);
        CHECK(click.Read().buttons == BTN_TOUCHPAD);
    }
}

void TestInvalidConfigurationFallsBack() {
    const fs::path directory = fs::temp_directory_path() / "dcp_mouse_tests_invalid";
    fs::create_directories(directory);
    const std::string file = (directory / "dcp.ini").string();
    DcpMouseMap map;
    DcpMouseLoadInfo info;
    {
        std::ofstream(file) << "mouse_cross = bogus\nmouse_l1 = mouse9\nmouse_pointer_mode = sideways\nmouse_r1 = x2\n"
                               "mouse_dpad_up =\nmouse_jump = left\nmouse_pointer_interact = none\nmouse_l2 = LEFT\n";
    }
    dcp_mouse_load_info(file.c_str(), &map, &info);
    CHECK(info.rejected == 4); // cross, l1, the mode and the unbound click; an empty value and an unknown key are not counted
    CHECK(map.button[DCP_MOUSE_CROSS] == DCP_MOUSE_NONE && map.button[DCP_MOUSE_L1] == DCP_MOUSE_NONE);
    CHECK(map.button[DCP_MOUSE_R1] == DCP_MOUSE_X2 && map.button[DCP_MOUSE_L2] == DCP_MOUSE_LEFT && map.button[DCP_MOUSE_DPAD_UP] == DCP_MOUSE_NONE);
    CHECK(map.pointer_mode == DCP_POINTER_MODE_HOLD && map.button[DCP_MOUSE_POINTER_INTERACT] == DCP_MOUSE_LEFT);
    CHECK(info.extended_bound == 2);

    // Pointer Control and its click on one button: both go back to their defaults, and nothing else is touched.
    {
        std::ofstream(file) << "mouse_pointer_control = x1\nmouse_pointer_interact = x1\nmouse_cross = x1\nmouse_pointer_mode = always_on\n";
    }
    dcp_mouse_load_info(file.c_str(), &map, &info);
    CHECK(info.rejected == 1 && map.button[DCP_MOUSE_POINTER_CONTROL] == DCP_MOUSE_MIDDLE && map.button[DCP_MOUSE_POINTER_INTERACT] == DCP_MOUSE_LEFT);
    CHECK(map.button[DCP_MOUSE_CROSS] == DCP_MOUSE_X1 && map.pointer_mode == DCP_POINTER_MODE_ALWAYS_ON);

    // A clean file reports nothing.
    {
        std::ofstream(file) << "mouse_cross = x1\nmouse_pointer_mode = hold\n";
    }
    dcp_mouse_load_info(file.c_str(), &map, &info);
    CHECK(info.rejected == 0 && info.extended_bound == 1);
    dcp_mouse_load_info((directory / "missing.ini").string().c_str(), &map, &info);
    CHECK(info.rejected == 0 && info.extended_bound == 0);

    // A value that is not usable changes nothing and is never read as another action.
    DcpMouseMap base;
    dcp_mouse_defaults(&base);
    map = base;
    for (int action = 0; action < DCP_MOUSE_ACTION_COUNT; ++action) {
        const char* key = dcp_mouse_actions[action].ini_key;
        CHECK(dcp_mouse_setting_key(key) && dcp_mouse_setting_value_valid(key, "x2") && dcp_mouse_setting_value_valid(key, "Mouse4"));
        CHECK(!dcp_mouse_setting_value_valid(key, "bogus") && !dcp_mouse_setting_value_valid(key, "") && !dcp_mouse_setting_value_valid(key, "cross"));
        CHECK(dcp_mouse_setting_value_valid(key, "none") == (action != DCP_MOUSE_POINTER_INTERACT));
        CHECK(!dcp_mouse_apply(&map, key, "bogus") && SameMap(map, base));
    }
    fs::remove_all(directory);

    // Telemetry for a rejected configuration: one line with a bounded count.
    Rig rig;
    dcp_mouse_note_invalid(rig.state, 0);
    dcp_mouse_note_invalid(rig.state, -3);
    CHECK(rig.lines.empty());
    dcp_mouse_note_invalid(rig.state, 3);
    CHECK(rig.lines.size() == 1 && rig.lines[0] == "Mouse telemetry: event=mouse_binding_invalid_fallback count=3");
    dcp_mouse_note_invalid(rig.state, 5000);
    CHECK(rig.lines.size() == 2 && rig.lines[1] == "Mouse telemetry: event=mouse_binding_invalid_fallback count=99");
}

void TestPointerModeSetting() {
    CHECK(std::string(dcp_pointer_mode_key) == "mouse_pointer_mode" && dcp_mouse_setting_key("mouse_pointer_mode"));
    CHECK(DCP_POINTER_MODE_HOLD == 0 && DCP_POINTER_MODE_ALWAYS_ON == 1 && DCP_POINTER_MODE_COUNT == 2);
    CHECK(dcp_pointer_mode_from_name("hold") == DCP_POINTER_MODE_HOLD && dcp_pointer_mode_from_name("ALWAYS_ON") == DCP_POINTER_MODE_ALWAYS_ON);
    for (const char* bad : {"", "always", "on", "always on", "toggle", "0", "1"}) {
        CHECK(dcp_pointer_mode_from_name(bad) == -1 && !dcp_mouse_setting_value_valid("mouse_pointer_mode", bad));
    }
    CHECK(dcp_pointer_mode_from_name(nullptr) == -1);
    for (int mode = 0; mode < DCP_POINTER_MODE_COUNT; ++mode) {
        CHECK(dcp_pointer_mode_from_name(dcp_pointer_mode_name(mode)) == mode);
    }
    CHECK(std::string(dcp_pointer_mode_label(DCP_POINTER_MODE_HOLD)) == "Hold" && std::string(dcp_pointer_mode_label(DCP_POINTER_MODE_ALWAYS_ON)) == "Always On");
    CHECK(std::string(dcp_pointer_mode_name(7)) == "hold" && std::string(dcp_pointer_mode_label(-1)) == "Hold");

    DcpMouseMap map;
    dcp_mouse_defaults(&map);
    CHECK(!dcp_mouse_apply(&map, "mouse_pointer_mode", "sideways") && map.pointer_mode == DCP_POINTER_MODE_HOLD);
    CHECK(dcp_mouse_apply(&map, "mouse_pointer_mode", "always_on") && map.pointer_mode == DCP_POINTER_MODE_ALWAYS_ON);
    CHECK(!dcp_mouse_apply(&map, "mouse_pointer_mode", "") && map.pointer_mode == DCP_POINTER_MODE_ALWAYS_ON);
    CHECK(dcp_mouse_apply(&map, "mouse_pointer_mode", "Hold") && map.pointer_mode == DCP_POINTER_MODE_HOLD);

    // A state given an impossible mode behaves as Hold.
    Rig rig;
    rig.map.pointer_mode = 7;
    rig.Configure();
    rig.Move(30, 0);
    DcpMouseSample sample = rig.Read();
    CHECK(!sample.touch && !sample.pointer && !dcp_mouse_pointer_active(rig.state));
    dcp_mouse_set_pointer_mode(rig.state, 9);
    dcp_mouse_set_pointer_mode(rig.state, -1);
    CHECK(!dcp_mouse_pointer_active(rig.state));

    // Hold -> Always On -> Hold with a custom Pointer Control button: the button comes back and works as before.
    Rig live;
    live.map.button[DCP_MOUSE_POINTER_CONTROL] = DCP_MOUSE_X1;
    live.Configure();
    live.Down(DCP_MOUSE_X1);
    CHECK(dcp_mouse_pointer_active(live.state));
    live.Up(DCP_MOUSE_X1);
    CHECK(!dcp_mouse_pointer_active(live.state) && !live.Read().touch);
    dcp_mouse_set_pointer_mode(live.state, DCP_POINTER_MODE_ALWAYS_ON);
    CHECK(dcp_mouse_pointer_active(live.state));
    CHECK(!live.Read().touch);
    live.Move(20.0f, 0.0f);
    CHECK(live.Read().touch);
    dcp_mouse_set_pointer_mode(live.state, DCP_POINTER_MODE_HOLD);
    CHECK(!dcp_mouse_pointer_active(live.state));
    sample = live.Read();
    CHECK(!sample.touch && !sample.pointer);
    live.Down(DCP_MOUSE_X1);
    CHECK(dcp_mouse_pointer_active(live.state) && live.Read().touch);
}

// The finger Always On makes is the finger Hold makes once it has landed.
std::vector<DcpMouseSample> StrokeSamples(Rig& rig, int reads) {
    std::vector<DcpMouseSample> samples;
    for (int read = 0; read < reads; ++read) {
        rig.Move(7.0f, 3.0f);
        samples.push_back(rig.Read());
    }
    return samples;
}

void TestAlwaysOnMovesThePointer() {
    Rig rig(DCP_POINTER_MODE_ALWAYS_ON);
    CHECK(dcp_mouse_pointer_active(rig.state) && dcp_mouse_capture_wanted(rig.state));
    // Idle Always On leaves the touch surface free so the game's right-stick pointer still works; motion lands it.
    DcpMouseSample sample = rig.Read();
    CHECK(sample.pointer && !sample.touch && sample.buttons == 0);
    rig.Move(20.0f, 0.0f);
    sample = rig.Read();
    CHECK(sample.pointer && sample.touch && sample.x == kCenterX && sample.y == kCenterY && sample.buttons == 0);

    // Hold needs its button for the same thing.
    Rig hold;
    CHECK(!dcp_mouse_pointer_active(hold.state) && !dcp_mouse_capture_wanted(hold.state));
    hold.Move(30, 0);
    CHECK(!hold.Read().touch);

    // Motion moves the pointer by the same amount as Hold does, sensitivity included.
    const double unit = 30.0 / 0.6;
    {
        Game game;
        game.Read(rig.Read());
        rig.Move(30.0f, 0.0f);
        CHECK(std::fabs(Travel(rig, game, 3) - unit) < 1.0);
        dcp_mouse_set_sensitivity(rig.state, 50);
        rig.Move(30.0f, 0.0f);
        CHECK(std::fabs(Travel(rig, game, 3) - unit / 2) < 1.0);
        dcp_mouse_set_sensitivity(rig.state, 200);
        rig.Move(30.0f, 0.0f);
        CHECK(std::fabs(Travel(rig, game, 3) - unit * 2) < 1.0);
        dcp_mouse_set_sensitivity(rig.state, 100);
    }

    // The same finger as Hold, read for read, once both have landed.
    Rig always(DCP_POINTER_MODE_ALWAYS_ON);
    Rig held;
    held.Down(DCP_MOUSE_MIDDLE);
    always.Read();
    always.Move(1.0f, 0.0f);
    always.Read();
    always.Read();
    held.Move(1.0f, 0.0f);
    held.Read();
    held.Read();
    const std::vector<DcpMouseSample> a = StrokeSamples(always, 400);
    const std::vector<DcpMouseSample> h = StrokeSamples(held, 400);
    int differences = 0, lifted = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        differences += a[i].touch != h[i].touch || a[i].pointer != h[i].pointer || a[i].x != h[i].x || a[i].y != h[i].y || a[i].id != h[i].id;
        lifted += !a[i].touch;
    }
    CHECK(differences == 0 && lifted >= 3);

    // Movement has no end: the host cursor never reaches an edge because the window holds it in relative mode, so a long
    // stroke in one direction keeps arriving in full.
    Rig far(DCP_POINTER_MODE_ALWAYS_ON);
    Game game;
    game.Read(far.Read());
    double input = 0;
    for (int read = 0; read < 2000; ++read) {
        far.Move(12.0f, 0.0f);
        float dx = 0, dy = 0;
        dcp_mouse_to_touch_units(100, 12.0f, 0.0f, &dx, &dy);
        input += dx;
        game.Read(far.Read());
    }
    CHECK(std::fabs(game.pointer_x - input) <= 2.0 * game.segments && game.pointer_x > 10.0 * DCP_TOUCH_RES_X);

    // The window keeps relative mode on without a button while gameplay owns the mouse, and drops it for everything else.
    using DcpMouseCapture::Frame;
    DcpMouseCapture::Tracker tracker;
    CHECK(tracker.Update(Frame{dcp_mouse_capture_wanted(rig.state) != 0, true, false, false}) == std::optional<bool>(true));
    CHECK(!tracker.Update(Frame{dcp_mouse_capture_wanted(rig.state) != 0, true, false, false}).has_value());
    CHECK(tracker.Update(Frame{dcp_mouse_capture_wanted(rig.state) != 0, true, true, false}) == std::optional<bool>(false));
}

void TestAlwaysOnInteractionAndButtons() {
    Rig rig(DCP_POINTER_MODE_ALWAYS_ON);
    rig.Read();
    rig.Read();
    // Left is the Pointer Interaction: it never becomes Square while Always On owns the mouse.
    rig.Down(DCP_MOUSE_LEFT);
    DcpMouseSample sample = rig.Read();
    CHECK(sample.buttons == BTN_TOUCHPAD && !sample.touch);
    rig.Up(DCP_MOUSE_LEFT);
    CHECK(rig.Read().buttons == 0);
    rig.Down(DCP_MOUSE_LEFT);
    CHECK(rig.Read().buttons == BTN_TOUCHPAD);
    // Other configured buttons keep working at the same time.
    rig.Down(DCP_MOUSE_RIGHT);
    CHECK(rig.Read().buttons == (BTN_TOUCHPAD | BTN_CIRCLE));
    rig.Up(DCP_MOUSE_LEFT);
    rig.Up(DCP_MOUSE_RIGHT);

    // Pointer Control's own button is not needed and is an ordinary button: unbound it does nothing, bound it presses.
    rig.Down(DCP_MOUSE_MIDDLE);
    sample = rig.Read();
    CHECK(sample.buttons == 0 && !sample.touch && dcp_mouse_pointer_active(rig.state));
    rig.Up(DCP_MOUSE_MIDDLE);
    Rig bound(DCP_POINTER_MODE_ALWAYS_ON);
    bound.map.button[DCP_MOUSE_CROSS] = DCP_MOUSE_MIDDLE;
    bound.map.button[DCP_MOUSE_SQUARE] = DCP_MOUSE_X1;
    bound.Configure();
    bound.Read();
    bound.Down(DCP_MOUSE_MIDDLE);
    bound.Down(DCP_MOUSE_X1);
    sample = bound.Read();
    CHECK(sample.buttons == (BTN_CROSS | BTN_SQUARE) && !sample.touch);

    // The saved Pointer Control button can be anything, even nothing, in Always On.
    Rig none(DCP_POINTER_MODE_ALWAYS_ON);
    none.map.button[DCP_MOUSE_POINTER_CONTROL] = DCP_MOUSE_NONE;
    none.Configure();
    none.Read();
    none.Move(15.0f, 0.0f);
    CHECK(none.Read().touch);

    // A remapped Pointer Interaction owns the new button, and Left is ordinary again.
    Rig moved(DCP_POINTER_MODE_ALWAYS_ON);
    moved.map.button[DCP_MOUSE_POINTER_INTERACT] = DCP_MOUSE_X2;
    moved.Configure();
    moved.Read();
    moved.Down(DCP_MOUSE_LEFT);
    CHECK(moved.Read().buttons == BTN_SQUARE);
    moved.Down(DCP_MOUSE_X2);
    CHECK(moved.Read().buttons == (BTN_SQUARE | BTN_TOUCHPAD));
}

void TestAlwaysOnSuspendsAndResumes() {
    using DcpMouseCapture::Blocked;
    using DcpMouseCapture::Frame;
    struct Case {
        const char* name;
        bool focused, overlay, text;
    };
    // focus away, Options/Cheats/a game dialog (and the Comms Wheel's mouse session), text entry
    for (const Case& c : {Case{"focus", false, false, false}, Case{"overlay", true, true, false}, Case{"text", true, false, true}}) {
        Rig rig(DCP_POINTER_MODE_ALWAYS_ON);
        DcpMouseCapture::Tracker tracker;
        rig.Read();
        rig.Read();
        const Frame open_frame{dcp_mouse_capture_wanted(rig.state) != 0, true, false, false};
        CHECK(tracker.Update(open_frame) == std::optional<bool>(true));
        const Frame busy{dcp_mouse_capture_wanted(rig.state) != 0, c.focused, c.overlay, c.text};
        CHECK(Blocked(busy));
        dcp_mouse_block(rig.state, 1);
        CHECK(tracker.Update(busy) == std::optional<bool>(false)); // capture released
        CHECK(!dcp_mouse_pointer_active(rig.state));
        rig.Move(100, 100);
        rig.Down(DCP_MOUSE_LEFT);
        rig.Down(DCP_MOUSE_RIGHT);
        dcp_mouse_wheel(rig.state, 1.0f);
        for (int i = 0; i < 3; ++i) {
            const DcpMouseSample sample = rig.Read();
            CHECK(sample.buttons == 0 && !sample.touch && !sample.pointer); // nothing reaches the game, no finger stays
        }
        dcp_mouse_block(rig.state, 0); // gameplay owns the mouse again
        CHECK(tracker.Update(open_frame) == std::optional<bool>(true));
        CHECK(dcp_mouse_pointer_active(rig.state));
        DcpMouseSample sample = rig.Read();
        CHECK(sample.pointer && !sample.touch && sample.buttons == 0); // buttons held through the block are not pressed
        rig.Up(DCP_MOUSE_LEFT);
        rig.Up(DCP_MOUSE_RIGHT);
        rig.Move(30, 0);
        sample = rig.Read();
        CHECK(sample.touch && sample.pointer); // resumed by itself, with no toggling
        CHECK(rig.Lines("event=pointer_control_suspended reason=blocked count=1") == 1);
        CHECK(rig.Lines("event=pointer_control_resumed count=1") == 1);
    }

    // A window mode switch: capture and the finger are dropped at once, nothing is taken while the window settles, and the
    // pointer lands again afterwards.
    Rig rig(DCP_POINTER_MODE_ALWAYS_ON);
    DcpMouseCapture::Tracker tracker;
    rig.Read();
    rig.Read();
    CHECK(tracker.Update(Frame{true, true, false, false}) == std::optional<bool>(true));
    dcp_mouse_release_all(rig.state);
    CHECK(tracker.Release() == std::optional<bool>(false) && !tracker.Captured());
    const Frame settling{true, true, true, false};
    CHECK(Blocked(settling));
    dcp_mouse_block(rig.state, 1);
    CHECK(!tracker.Update(settling).has_value() && !tracker.Captured());
    CHECK(!rig.Read().touch && !rig.Read().pointer);
    dcp_mouse_block(rig.state, 0);
    CHECK(tracker.Update(Frame{true, true, false, false}) == std::optional<bool>(true));
    CHECK(!rig.Read().touch);
    rig.Move(20.0f, 0.0f);
    CHECK(rig.Read().touch);

    // A release with nothing blocking (closing, a mode switch before the settle starts) lands the finger again, not as a jump.
    Rig again(DCP_POINTER_MODE_ALWAYS_ON);
    again.Read();
    again.Move(200, 0);
    CHECK(again.Read().touch);
    dcp_mouse_release_all(again.state);
    DcpMouseSample sample = again.Read();
    CHECK(sample.pointer && !sample.touch);
    again.Move(10, 0);
    CHECK(again.Read().touch && again.Read().x > kCenterX);
}

void TestAlwaysOnWithMouseControlsOff() {
    for (const int mode : {DCP_POINTER_MODE_HOLD, DCP_POINTER_MODE_ALWAYS_ON}) {
        Rig off(mode);
        off.map.controls = 0;
        off.map.wheel_up = DCP_WHEEL_L1;
        off.map.button[DCP_MOUSE_CROSS] = DCP_MOUSE_X1;
        off.Configure();
        CHECK(!dcp_mouse_enabled(off.state) && !dcp_mouse_pointer_active(off.state) && !dcp_mouse_capture_wanted(off.state));
        for (const int button : kButtons) {
            off.Down(button);
            const DcpMouseSample sample = off.Read();
            CHECK(sample.buttons == 0 && !sample.touch && !sample.pointer);
            off.Up(button);
        }
        dcp_mouse_wheel(off.state, 1.0f);
        off.Move(400, 400);
        for (int i = 0; i < 5; ++i) {
            const DcpMouseSample sample = off.Read();
            CHECK(sample.buttons == 0 && !sample.touch && !sample.pointer);
        }
    }

    // Turning it Off while Always On is moving the pointer releases everything at once, and On starts clean.
    Rig live(DCP_POINTER_MODE_ALWAYS_ON);
    live.map.button[DCP_MOUSE_CROSS] = DCP_MOUSE_X1;
    live.map.wheel_up = DCP_WHEEL_R1;
    live.Configure();
    DcpMouseCapture::Tracker tracker;
    live.Read();
    live.Down(DCP_MOUSE_X1);
    live.Down(DCP_MOUSE_LEFT);
    dcp_mouse_wheel(live.state, 1.0f);
    live.Move(50, 50);
    DcpMouseSample sample = live.Read();
    CHECK(sample.touch && (sample.buttons & BTN_TOUCHPAD) && (sample.buttons & BTN_CROSS) && (sample.buttons & BTN_R1));
    CHECK(tracker.Update({dcp_mouse_capture_wanted(live.state) != 0, true, false, false}) == std::optional<bool>(true));
    dcp_mouse_set_enabled(live.state, 0);
    CHECK(!dcp_mouse_pointer_active(live.state) && !dcp_mouse_capture_wanted(live.state) && !dcp_mouse_any_held(live.state));
    CHECK(tracker.Update({dcp_mouse_capture_wanted(live.state) != 0, true, false, false}) == std::optional<bool>(false));
    for (int i = 0; i < 5; ++i) {
        sample = live.Read();
        CHECK(sample.buttons == 0 && !sample.touch && !sample.pointer);
    }
    live.Up(DCP_MOUSE_LEFT);
    live.Down(DCP_MOUSE_X1);
    dcp_mouse_set_enabled(live.state, 1);
    CHECK(live.Read().buttons == 0); // the press during Off is not a press now
    live.Up(DCP_MOUSE_X1);
    live.Read();
    live.Move(30, 0);
    CHECK(live.Read().touch && dcp_mouse_pointer_active(live.state));
    live.Down(DCP_MOUSE_X1);
    CHECK((live.Read().buttons & BTN_CROSS) != 0);
    CHECK(live.Lines("event=pointer_control_suspended reason=disabled count=1") == 1);
    CHECK(live.Lines("event=pointer_control_resumed count=1") == 1);
}

struct PadView {
    bool touch;
    int x, y;
};

// What the pad sampler hands the game: Pointer Control's finger replaces the controller's while it owns the surface.
PadView Merge(const DcpMouseSample& mouse, bool controller_touching, int controller_x, int controller_y) {
    if (mouse.pointer) {
        return {mouse.touch != 0, mouse.x, mouse.y};
    }
    return {controller_touching, controller_x, controller_y};
}

struct PadGame {
    bool previous = false;
    int x = 0, y = 0;
    double travelled = 0, biggest_step = 0;
    void Read(const PadView& view) {
        if (view.touch && previous) {
            const double step = std::fabs(static_cast<double>(view.x - x)) + std::fabs(static_cast<double>(view.y - y));
            travelled += step;
            biggest_step = std::max(biggest_step, step);
        }
        previous = view.touch;
        if (view.touch) {
            x = view.x;
            y = view.y;
        }
    }
};

void TestControllerTouchpadCoexistence() {
    // Hold: unchanged. A controller touch ends the pointer until its button is released; the surface reports do not matter.
    {
        Rig rig;
        rig.Down(DCP_MOUSE_MIDDLE);
        dcp_mouse_controller_surface(rig.state, 1);
        dcp_mouse_screen_surface(rig.state, 1);
        rig.Move(30, 0);
        CHECK(dcp_mouse_pointer_active(rig.state) && rig.Read().touch);
        dcp_mouse_controller_touch(rig.state);
        CHECK(!dcp_mouse_pointer_active(rig.state));
        dcp_mouse_controller_surface(rig.state, 0);
        CHECK(!dcp_mouse_pointer_active(rig.state)); // still off until the button is released
        rig.Up(DCP_MOUSE_MIDDLE);
        rig.Down(DCP_MOUSE_MIDDLE);
        CHECK(dcp_mouse_pointer_active(rig.state));
        CHECK(rig.lines.empty()); // Hold has no Always On telemetry
    }

    // Always On: the controller wins while it touches, and the pointer comes back by itself, without a hand-over jump.
    Rig rig(DCP_POINTER_MODE_ALWAYS_ON);
    PadGame game;
    for (int read = 0; read < 30; ++read) {
        rig.Move(7, 0);
        game.Read(Merge(rig.Read(), false, 0, 0));
    }
    CHECK(game.travelled > 100.0);
    game.biggest_step = 0;
    dcp_mouse_controller_touch(rig.state); // the window's event
    CHECK(!dcp_mouse_pointer_active(rig.state));
    const int controller_x = 150, controller_y = 80; // far from the pointer's finger in the middle of the surface
    for (int read = 0; read < 10; ++read) {
        dcp_mouse_controller_surface(rig.state, 1); // the sampler's poll
        rig.Move(7, 0);                              // dropped: the controller has the surface
        const DcpMouseSample sample = rig.Read();
        CHECK(sample.buttons == 0);
        game.Read(Merge(sample, true, controller_x + 5 * read, controller_y));
        if (read > 0) {
            CHECK(!sample.pointer); // after the one finger-less read, the controller's finger is the game's
        }
        CHECK(!dcp_mouse_pointer_active(rig.state));
    }
    CHECK(game.biggest_step <= 5.5); // the controller's own movement only: no jump from the pointer's finger to the controller's
    game.biggest_step = 0;
    for (int read = 0; read < 30; ++read) {
        dcp_mouse_controller_surface(rig.state, 0);
        rig.Move(7, 0);
        game.Read(Merge(rig.Read(), false, controller_x, controller_y));
        CHECK(dcp_mouse_pointer_active(rig.state));
    }
    CHECK(game.biggest_step < 40.0); // and none back
    CHECK(rig.Lines("event=pointer_control_suspended reason=controller_touch count=1") == 1);
    CHECK(rig.Lines("event=pointer_control_resumed count=1") == 1);

    // The player does not have to touch a setting again: a second touch does the same.
    dcp_mouse_controller_surface(rig.state, 1);
    CHECK(!dcp_mouse_pointer_active(rig.state));
    dcp_mouse_controller_surface(rig.state, 0);
    rig.Read();
    rig.Move(20, 0);
    CHECK(dcp_mouse_pointer_active(rig.state) && rig.Read().touch);

    // The controller's buttons, sticks and triggers never reach the pointer: it only hears about touches. A touchscreen
    // contact takes the surface the same way, and gives it back.
    Rig screen(DCP_POINTER_MODE_ALWAYS_ON);
    screen.Read();
    screen.Move(20, 0);
    CHECK(screen.Read().touch);
    dcp_mouse_screen_surface(screen.state, 1);
    CHECK(!dcp_mouse_pointer_active(screen.state));
    screen.Move(40, 0);
    CHECK(!screen.Read().touch);
    dcp_mouse_controller_surface(screen.state, 1); // both at once
    dcp_mouse_screen_surface(screen.state, 0);
    CHECK(!dcp_mouse_pointer_active(screen.state)); // the controller still holds it
    dcp_mouse_controller_surface(screen.state, 0);
    CHECK(dcp_mouse_pointer_active(screen.state));
}

void TestSteamDeckTrackpadWithAlwaysOn() {
    // The trackpad is mouse motion; Pointer Control Mode is Always On; buttons held on the controller or the mouse at any
    // moment change nothing about the finger the game reads.
    const Stroke base = RunStroke(DCP_MOUSE_MIDDLE, DCP_MOUSE_LEFT, 0, 0, 0, 300, true);
    CHECK(std::fabs(base.game.pointer_x - base.input_x) <= 2.0 * base.game.segments);
    CHECK(std::fabs(base.game.pointer_y - base.input_y) <= 2.0 * base.game.segments);
    CHECK(base.game.pointer_x > 300.0);
    for (const int other : {DCP_MOUSE_LEFT, DCP_MOUSE_RIGHT, DCP_MOUSE_X1}) {
        CHECK(SameFinger(base, RunStroke(DCP_MOUSE_MIDDLE, DCP_MOUSE_LEFT, other, 0, 1000, 300, true)));
        CHECK(SameFinger(base, RunStroke(DCP_MOUSE_MIDDLE, DCP_MOUSE_LEFT, other, 100, 200, 300, true)));
    }
    // The trackpad click bound to Pointer Interaction while its other button is the secondary action.
    const Stroke click = RunStroke(DCP_MOUSE_X2, DCP_MOUSE_LEFT, DCP_MOUSE_LEFT, 20, 280, 300, true);
    CHECK(SameFinger(base, click));
    const Stroke held = RunStroke(DCP_MOUSE_MIDDLE, DCP_MOUSE_LEFT, DCP_MOUSE_RIGHT, 0, 1000, 300, true);
    int still = 0, lifted = 0;
    for (std::size_t i = 1; i < held.samples.size(); ++i) {
        const DcpMouseSample &a = held.samples[i - 1], &b = held.samples[i];
        lifted += !b.touch;
        still += i < 300 && a.touch && b.touch && a.x == b.x && a.y == b.y;
    }
    CHECK(still == 0 && lifted >= 3 && std::fabs(held.game.pointer_x - held.input_x) <= 2.0 * held.game.segments);

    // The window reports Always On motion as pointer motion, with the buttons held on the controller or the mouse.
    Motion m;
    m.Stroke(DCP_MOTION_SOURCE_MOUSE_POINTER, DCP_MOTION_HELD_GAMEPAD, 3000);
    CHECK(m.With("event=pointer_motion_while_button_held source=mouse_pointer held=gamepad_button count=1") == 1 && m.With("stall") == 0);
}

int CountLines(const std::vector<std::string>& lines, const std::string& needle) {
    return static_cast<int>(std::count_if(lines.begin(), lines.end(), [&](const std::string& line) { return Contains(line, needle); }));
}

void TestMouseTelemetry() {
    // Extended bindings: counted per press, logged on the first and each power of ten, never for Square and Circle.
    {
        Rig rig;
        rig.map.button[DCP_MOUSE_CROSS] = DCP_MOUSE_X1;
        rig.Configure();
        for (int press = 1; press <= 1000; ++press) {
            rig.Down(DCP_MOUSE_X1);
            rig.Read();
            rig.Read(); // held: not a second press
            rig.Up(DCP_MOUSE_X1);
            rig.Read();
        }
        CHECK(rig.lines == (std::vector<std::string>{"Mouse telemetry: event=mouse_extended_binding_used count=1",
                                                      "Mouse telemetry: event=mouse_extended_binding_used count=10",
                                                      "Mouse telemetry: event=mouse_extended_binding_used count=100",
                                                      "Mouse telemetry: event=mouse_extended_binding_used count=1000"}));
    }
    {
        Rig rig;
        for (int press = 0; press < 50; ++press) {
            rig.Down(DCP_MOUSE_LEFT);
            rig.Down(DCP_MOUSE_RIGHT);
            rig.Read();
            rig.Up(DCP_MOUSE_LEFT);
            rig.Up(DCP_MOUSE_RIGHT);
            rig.Read();
        }
        CHECK(rig.lines.empty()); // the 1.4.0 buttons are not the new path
    }

    // Always On: used once per start, suspended with its reason, resumed.
    {
        Rig rig(DCP_POINTER_MODE_ALWAYS_ON);
        rig.Read();
        CHECK(rig.lines.empty()); // active, but nothing has used it
        rig.Move(5, 0);
        CHECK(rig.lines.size() == 1 && rig.lines[0] == "Mouse telemetry: event=pointer_control_always_on_used count=1");
        rig.Move(5, 5);
        rig.Move(5, 0);
        CHECK(rig.lines.size() == 1); // not per movement
        rig.Move(0, 0);
        dcp_mouse_block(rig.state, 1);
        CHECK(rig.lines.size() == 2 && rig.lines[1] == "Mouse telemetry: event=pointer_control_suspended reason=blocked count=1");
        dcp_mouse_block(rig.state, 1);
        CHECK(rig.lines.size() == 2); // a repeated block is no new suspension
        dcp_mouse_block(rig.state, 0);
        CHECK(rig.lines.size() == 3 && rig.lines[2] == "Mouse telemetry: event=pointer_control_resumed count=1");
        rig.Move(5, 0);
        CHECK(rig.Events("pointer_control_always_on_used") == 1); // the second start: counted, not logged
        for (int cycle = 0; cycle < 9; ++cycle) {
            dcp_mouse_block(rig.state, 1);
            dcp_mouse_block(rig.state, 0);
            rig.Move(5, 0);
        }
        CHECK(rig.Lines("event=pointer_control_always_on_used count=10") == 1);
        CHECK(rig.Lines("event=pointer_control_suspended reason=blocked count=10") == 1);
        CHECK(rig.Lines("event=pointer_control_resumed count=10") == 1);
    }
    {
        Rig rig(DCP_POINTER_MODE_ALWAYS_ON);
        rig.Read();
        dcp_mouse_controller_surface(rig.state, 1);
        dcp_mouse_controller_surface(rig.state, 1);
        dcp_mouse_controller_surface(rig.state, 0);
        dcp_mouse_set_enabled(rig.state, 0);
        dcp_mouse_set_enabled(rig.state, 1);
        CHECK(rig.lines.size() == 3);
        CHECK(rig.Lines("event=pointer_control_suspended reason=controller_touch count=1") == 1);
        CHECK(rig.Lines("event=pointer_control_suspended reason=disabled count=1") == 1);
        CHECK(rig.Lines("event=pointer_control_resumed count=1") == 1 && rig.Lines("event=pointer_control_resumed count=2") == 0);
    }
    // Hold never reports any of the Always On events, however Pointer Control is used.
    {
        Rig rig;
        for (int use = 0; use < 20; ++use) {
            rig.Down(DCP_MOUSE_MIDDLE);
            rig.Move(30, 30);
            rig.Read();
            dcp_mouse_controller_touch(rig.state);
            rig.Up(DCP_MOUSE_MIDDLE);
            dcp_mouse_block(rig.state, 1);
            dcp_mouse_block(rig.state, 0);
            dcp_mouse_release_all(rig.state);
        }
        dcp_mouse_set_enabled(rig.state, 0);
        dcp_mouse_set_enabled(rig.state, 1);
        CHECK(rig.lines.empty());
    }

    // Nothing sensitive and nothing frequent: every line is a name, a reason class and a count; motion and presses by the
    // thousand log a handful of lines.
    Rig busy(DCP_POINTER_MODE_ALWAYS_ON);
    busy.map.button[DCP_MOUSE_R1] = DCP_MOUSE_X2;
    busy.Configure();
    for (int i = 0; i < 20000; ++i) {
        busy.Move(static_cast<float>(i % 17), static_cast<float>(i % 5));
        if (i % 4 == 0) {
            busy.Read();
        }
        if (i % 50 == 0) {
            busy.Down(DCP_MOUSE_X2);
        } else if (i % 50 == 25) {
            busy.Up(DCP_MOUSE_X2);
        }
        if (i % 2000 == 0) {
            dcp_mouse_block(busy.state, 1);
            dcp_mouse_block(busy.state, 0);
        }
    }
    CHECK(busy.lines.size() <= 16);
    dcp_mouse_note_invalid(busy.state, 2);
    const std::regex safe("Mouse telemetry: event=(mouse_extended_binding_used|pointer_control_always_on_used|pointer_control_suspended|"
                          "pointer_control_resumed|mouse_binding_invalid_fallback)( reason=(blocked|controller_touch|disabled))? count=[0-9]+");
    for (const std::string& line : busy.lines) {
        CHECK(std::regex_match(line, safe));
    }
    const std::string source = ReadSource("src/dcp_mouse.c");
    CHECK(Count(source, "snprintf(") == 2 && Count(source, "Mouse telemetry: event=") == 2);
    for (const char* sensitive : {"%f", "%g", "%s,%s", "name=", "path", "account", "guid", "SDL_"}) {
        const std::size_t at = source.find("Mouse telemetry: event=");
        CHECK(source.substr(at, 200).find(sensitive) == std::string::npos);
    }
    CHECK(CountLines(busy.lines, "x=") == 0 && CountLines(busy.lines, "dx") == 0 && CountLines(busy.lines, "Left") == 0);
    CHECK(!Contains(source, "migrated")); // no legacy setting is rewritten, so there is nothing to report
}

void TestWindowAndRuntimeWiring() {
    const std::string window = ReadSource("gpu/shim/window.cpp");
    const std::string pad = ReadSource("src/runtime_pad.c");
    const std::string capture = ReadSource("gpu/shim/dcp_mouse_capture.h");

    // The window holds relative mode for what the mouse input wants, so an edge never stops Always On, and everything that
    // needs the mouse (dialogs, Options, text entry, the Comms Wheel, focus, a mode switch) blocks it.
    const std::size_t update = window.find("void WindowSDL::UpdateMouseCapture() {");
    CHECK(update != std::string::npos);
    const std::string body = window.substr(update, window.find("\n}\n", update) - update);
    CHECK(Contains(body, "dcp_mouse_capture_wanted(mouse)") && !Contains(body, "dcp_mouse_pointer_active("));
    CHECK(Contains(body, "DcpCommsWheel::MouseEngaged()") && Contains(body, "settling") && Contains(body, "DcpOverlay::CapturesInput()"));
    CHECK(Contains(body, "dcp_mouse_always_on(mouse)") && Contains(body, "mouse_settle_until"));
    CHECK(Contains(body, "dcp_mouse_block(mouse, blocked);") && Contains(body, "dcp_touchscreen_block(dcp_touchscreen_global(), blocked);"));
    const std::size_t apply = window.find("void WindowSDL::ApplyDisplayMode(int mode)");
    CHECK(window.find("ReleaseMouse();", apply) < window.find("SDL_SetWindowFullscreen(window, borderless);", apply));
    CHECK(window.find("SDL_SyncWindow(window);", apply) < window.find("mouse_settle_until = SDL_GetTicks() + kModeSettleMs;", apply));
    CHECK(Contains(ReadSource("gpu/shim/sdl_window.h"), "u64 mouse_settle_until"));
    CHECK(Contains(ReadSource("gpu/shim/dcp_comms_wheel.cpp"), "bool MouseEngaged() {\n    return g_driver.KeyboardEngaged();"));
    CHECK(Contains(capture, "the mouse input wants the pointer captured"));
    CHECK(Contains(window, "const bool pointer = dcp_mouse_pointer_active(dcp_mouse_global()) != 0;")); // Always On motion is pointer motion
    // A controller's touchpad event still reaches the mouse state, once, and the other controller events do not.
    CHECK(Count(window, "dcp_mouse_controller_touch(") == 1 && Count(window, "dcp_mouse_controller_surface(") == 0);

    // The sampler tells the mouse about the touch surface before the mouse takes its sample and the touchscreen after its own.
    const std::size_t host = pad.find("static void sample_host(int slot, PadData *d)");
    const std::size_t surface = pad.find("dcp_mouse_controller_surface(dcp_mouse_global(),pad_surface_touched(g));", host);
    const std::size_t sampled = pad.find("dcp_mouse_sample(dcp_mouse_global(),&mouse);", host);
    const std::size_t screen_sample = pad.find("dcp_touchscreen_sample(dcp_touchscreen_global(),&screen);", host);
    const std::size_t screen_surface = pad.find("dcp_mouse_screen_surface(dcp_mouse_global(),screen.touch);", host);
    CHECK(surface != std::string::npos && surface < sampled && sampled < screen_sample && screen_sample < screen_surface);
    CHECK(Count(pad, "dcp_mouse_controller_surface(") == 1 && Count(pad, "dcp_mouse_screen_surface(") == 1);
    CHECK(Contains(pad, "if (mouse->touchpad_side) touch_click(d,mouse->touchpad_side==2);"));
    CHECK(Contains(pad, "dcp_mouse_load_info(keys_file,&mouse,&loaded);") && Contains(pad, "dcp_mouse_note_invalid(dcp_mouse_global(),loaded.rejected);"));
    CHECK(Contains(pad, "pointer control %s, %d extra buttons"));

    // F1 offers Pointer Control Mode live, and applies it to the running mouse input.
    const std::string menu = ReadSource("gpu/shim/dcp_options_menu.h");
    CHECK(Contains(menu, "\"mouse_pointer_mode\", \"Pointer Control Mode\"") && Contains(menu, "Apply::Live, kPointerModes}"));
    CHECK(Contains(ReadSource("gpu/shim/dcp_options_menu.cpp"), "dcp_mouse_set_pointer_mode(dcp_mouse_global(), DcpSettings::Get().mouse_pointer_mode);"));
    CHECK(Contains(ReadSource("gpu/shim/dcp_settings.cpp"), "key == \"mouse_pointer_mode\""));
    CHECK(Contains(ReadSource("launcher/settings.h"), "kMousePointerModeKey[] = \"mouse_pointer_mode\""));

    // The gate names what this suite covers, and both platforms run it.
    const std::string gate = ReadSource("tools/test.ps1");
    for (const char* topic : {"full mouse rebinding", "Pointer Control Hold / Always On", "Steam Deck trackpad", "touchscreen", "controller ownership"}) {
        CHECK(Contains(gate, topic));
    }
    CHECK(Contains(gate, "'dcp-mouse-tests'"));
    const std::string build = ReadSource("tools/build.sh");
    CHECK(Contains(build, "\"$build_dir/bin/dcp-mouse-tests\"") && Contains(build, "full mouse rebinding"));
}

} // namespace

int main() {
    TestDefaultsAndNames();
    TestSensitivitySteps();
    TestSensitivityMultipliers();
    TestConfigFile();
    TestKeyboardMappingsUnchanged();
    TestExitGameHotkey();
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
    TestControllerTouchEndsPointer();
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
    TestPadOwnershipStartupOrders();
    TestPadOwnershipEnumerationOrder();
    TestPadOwnershipHotplug();
    TestPadOwnershipSteamVirtualPads();
    TestPadOwnershipSinglePlayer();
    TestPadOwnershipNoDuplicatesOrJumps();
    TestPadOwnershipTelemetry();
    TestPadOwnershipSource();
    TestMotionUnaffectedByHeldButtons();
    TestMotionAndButtonOrdering();
    TestCaptureModesWithHeldButtons();
    TestControllerPathUnchanged();
    TestPointerMotionTelemetry();
    TestPointerMotionTelemetryPayload();
    TestTargetTable();
    TestLegacyFilesStayIdentical();
    TestEveryButtonEveryTarget();
    TestRepresentativeBindings();
    TestDuplicateAndConflictPolicy();
    TestInvalidConfigurationFallsBack();
    TestPointerModeSetting();
    TestAlwaysOnMovesThePointer();
    TestAlwaysOnInteractionAndButtons();
    TestAlwaysOnSuspendsAndResumes();
    TestAlwaysOnWithMouseControlsOff();
    TestControllerTouchpadCoexistence();
    TestSteamDeckTrackpadWithAlwaysOn();
    TestMouseTelemetry();
    TestWindowAndRuntimeWiring();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
