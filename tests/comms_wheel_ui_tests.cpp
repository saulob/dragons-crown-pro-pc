// Focused tests of the Comms Wheel's input glue, layout and key binding. No window, no game.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include <SDL3/SDL.h>

#include "comms_text.h"
#include "dcp_comms_wheel.h"
#include "dcp_keys.h"

using namespace DcpCommsWheel;
namespace comms = dcp::comms;

namespace {

int g_checks = 0, g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                        \
    } while (0)

std::string ReadSource(const char* relative) {
    std::ifstream in(std::string(DCP_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

Inputs Base(uint64_t now) {
    Inputs in;
    in.now_ms = now;
    in.available = true;
    return in;
}

Inputs Pad(uint64_t now, bool up, float sx = 0.0f, float sy = 0.0f) {
    Inputs in = Base(now);
    in.pad_up = up;
    in.stick_x = sx;
    in.stick_y = sy;
    return in;
}

Inputs Keyboard(uint64_t now, bool down, float mx = 0.0f, float my = 0.0f) {
    Inputs in = Base(now);
    in.key_down = down;
    in.mouse_x = mx;
    in.mouse_y = my;
    return in;
}

// Sector 2 is right: Need help.
void TestControllerLatches() {
    Driver driver;
    Outputs out = driver.Step(Pad(1000, true));
    CHECK(driver.Latest().open && out.hide_pad_up && out.hide_confirm && out.hide_circle && out.neutral_right_stick);
    out = driver.Step(Pad(1016, false));
    CHECK(driver.Latest().open && out.send < 0 && out.hide_pad_up);
    out = driver.Step(Pad(1300, false, 1.0f, 0.0f));
    CHECK(driver.Latest().selected == 2 && driver.Latest().open && out.neutral_right_stick);
    out = driver.Step(Pad(1400, false, 0.0f, 0.0f));
    CHECK(driver.Latest().open && driver.Latest().selected == 2 && out.send < 0);
    out = driver.Step(Pad(1500, false, 0.0f, 1.0f));
    CHECK(driver.Latest().selected == 4);
    Inputs cross = Pad(1600, false);
    cross.confirm = true;
    out = driver.Step(cross);
    CHECK(out.send == static_cast<int>(comms::Message::kGroupUp) && !driver.Latest().open && out.hide_confirm);
    out = driver.Step(cross);
    CHECK(out.send < 0 && out.hide_confirm);
    out = driver.Step(Pad(1700, false));
    CHECK(!out.hide_confirm && !out.hide_pad_up && !out.hide_circle && !out.neutral_right_stick);
}

void TestControllerCancel() {
    Driver driver;
    driver.Step(Pad(0, true));
    driver.Step(Pad(100, false, 0.0f, -1.0f));
    Inputs circle = Pad(200, false, 0.0f, -1.0f);
    circle.circle = true;
    Outputs out = driver.Step(circle);
    CHECK(!driver.Latest().open && out.send < 0 && out.hide_circle);
    out = driver.Step(Pad(300, false));
    CHECK(!out.hide_circle);
}

void TestNothingSelectedKeepsOpen() {
    Driver driver;
    driver.Step(Pad(0, true));
    Inputs cross = Pad(100, false, 0.2f, 0.1f);
    cross.confirm = true;
    const Outputs out = driver.Step(cross);
    CHECK(out.send < 0 && driver.Latest().open);
}

void TestKeyboardOpenAndMouseConfirm() {
    Driver driver;
    Outputs out = driver.Step(Keyboard(0, true));
    CHECK(driver.Latest().open && driver.KeyboardEngaged() && out.neutral_keyboard_axes);
    out = driver.Step(Keyboard(16, false));
    CHECK(driver.Latest().open && driver.KeyboardEngaged() && out.send < 0);
    driver.Step(Keyboard(100, false, -1.0f, 0.0f));
    CHECK(driver.Latest().selected == 6);
    Inputs click = Keyboard(200, false, -1.0f, 0.0f);
    click.click = true;
    out = driver.Step(click);
    CHECK(out.send == static_cast<int>(comms::Message::kGoodJob) && !driver.Latest().open && !driver.KeyboardEngaged());
    CHECK(!driver.Step(Keyboard(300, false)).neutral_keyboard_axes);
}

void TestKeyboardCancel() {
    for (const int how : {0, 1}) {
        Driver driver;
        driver.Step(Keyboard(0, true));
        driver.Step(Keyboard(100, false, 0.0f, 1.0f));
        Inputs cancel = Keyboard(200, false, 0.0f, 1.0f);
        cancel.cancel_click = how == 0;
        cancel.circle = how == 1;
        const Outputs out = driver.Step(cancel);
        CHECK(!driver.Latest().open && out.send < 0 && !driver.KeyboardEngaged());
    }
}

void TestKeyboardNavigation() {
    Driver driver;
    driver.Step(Keyboard(0, true));
    Inputs up = Keyboard(100, false);
    up.nav = comms::kNavUp;
    driver.Step(up);
    CHECK(driver.Latest().selected == 0);
    up.nav = comms::kNavUp | comms::kNavRight;
    driver.Step(up);
    CHECK(driver.Latest().selected == 1);
    up.nav = comms::kNavRight;
    driver.Step(up);
    CHECK(driver.Latest().selected == 1);
    up.nav = 0;
    driver.Step(up);
    CHECK(driver.Latest().selected == 1);
    Inputs cross = Keyboard(300, false);
    cross.confirm = true;
    const Outputs out = driver.Step(cross);
    CHECK(out.send == static_cast<int>(comms::Message::kHello) && out.hide_confirm && !driver.Latest().open);
}

void TestAvailability() {
    Driver driver;
    Inputs solo = Pad(0, true);
    solo.available = false;
    Outputs out = driver.Step(solo);
    CHECK(!out.hide_pad_up && !driver.Latest().open);
    Inputs solo_key = Keyboard(500, true);
    solo_key.available = false;
    driver.Step(solo_key);
    CHECK(!driver.Latest().open && !driver.KeyboardEngaged());

    Driver room;
    room.Step(Pad(0, true));
    room.Step(Pad(300, false, 0.0f, 1.0f));
    CHECK(room.Latest().open);
    Inputs left_room = Pad(350, false, 0.0f, 1.0f);
    left_room.available = false;
    out = room.Step(left_room);
    CHECK(!room.Latest().open && out.send < 0);
}

void TestStaleStepDropsTheWheel() {
    Driver driver;
    driver.Step(Pad(0, true));
    driver.Step(Pad(300, false, 1.0f, 0.0f));
    CHECK(driver.Latest().open);
    const Outputs out = driver.Step(Pad(300 + kStaleMs + 1, false));
    CHECK(out.send < 0 && !out.hide_pad_up && !driver.Latest().open);
}

void TestMouseAccumulator() {
    MouseAccumulator mouse;
    float x = 0, y = 0;
    mouse.Add(kMouseRangeUnits * 0.25f, 0.0f);
    mouse.Read(x, y);
    CHECK(std::abs(x - 0.25f) < 1e-5f && y == 0.0f);
    mouse.Add(kMouseRangeUnits * 4.0f, kMouseRangeUnits * 4.0f);
    mouse.Read(x, y);
    CHECK(std::abs(std::hypot(x, y) - 1.0f) < 1e-4f);
    CHECK(comms::SectorAt(x, y, comms::kStickDeadZone) == 3);
    mouse.Clear();
    mouse.Read(x, y);
    CHECK(x == 0.0f && y == 0.0f);
}

int Codepoints(const char* text) {
    int count = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
        count += (*p & 0xc0) != 0x80;
    }
    return count;
}

void TestFontFallbackAndLayout() {
    CHECK(DrawableByOverlayFont("Preciso de ajuda") && DrawableByOverlayFont("Bien jou\xc3\xa9") &&
          DrawableByOverlayFont("\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82"));
    CHECK(!DrawableByOverlayFont("\xe3\x81\x93\xe3\x82\x93") && !DrawableByOverlayFont("\xed\x95\x9c\xea\xb5\xad"));

    int fallbacks = 0;
    for (int language = 0; language < COMMS_TEXT_LANGUAGES; ++language) {
        const char* key = comms_text_language_key(language);
        bool all = true, none = true;
        for (int id = 0; id < COMMS_TEXT_MESSAGES; ++id) {
            const bool drawable = DrawableByOverlayFont(comms_text_lookup(key, id));
            all = all && drawable;
            none = none && !drawable;
        }
        const bool cjk = !strcmp(key, "japanese") || !strcmp(key, "traditional_chinese") || !strcmp(key, "korean");
        CHECK(cjk ? none : all);
        fallbacks += cjk;
    }
    CHECK(fallbacks == 3);

    const Measure measure = [](const char* text, float px) { return static_cast<float>(Codepoints(text)) * px * 0.62f; };
    for (const float scale : {1.0f, 1.5f, 2.25f}) {
        for (int language = 0; language < COMMS_TEXT_LANGUAGES; ++language) {
            std::array<const char*, comms::kSectors> labels{};
            for (int i = 0; i < comms::kSectors; ++i) {
                const char* text = comms_text_lookup(comms_text_language_key(language), comms::MessageAtSector(i));
                labels[static_cast<size_t>(i)] = DrawableByOverlayFont(text) ? text : comms_text_lookup("english", comms::MessageAtSector(i));
            }
            const Layout layout = ComputeLayout(960.0f * scale, 540.0f * scale, scale, labels, measure);
            for (size_t a = 0; a < layout.sectors.size(); ++a) {
                const SectorLayout& s = layout.sectors[a];
                CHECK(s.font_px >= 22.0f * scale * 0.8f - 0.01f && s.font_px >= 17.0f);
                CHECK(s.icon.w >= 48.0f && !s.icon.Overlaps(s.label));
                CHECK(s.icon.Inside(layout.center_x, layout.center_y, layout.outer_radius));
                CHECK(s.label.Inside(layout.center_x, layout.center_y, layout.outer_radius));
                for (const Rect* r : {&s.icon, &s.label}) {
                    for (const float px : {r->x, r->x + r->w}) {
                        for (const float py : {r->y, r->y + r->h}) {
                            CHECK(std::hypot(px - layout.center_x, py - layout.center_y) > layout.inner_radius);
                        }
                    }
                }
                for (size_t b = a + 1; b < layout.sectors.size(); ++b) {
                    const SectorLayout& t = layout.sectors[b];
                    CHECK(!s.icon.Overlaps(t.icon) && !s.icon.Overlaps(t.label) && !s.label.Overlaps(t.icon) &&
                          !s.label.Overlaps(t.label));
                }
            }
        }
    }
}

void TestHitMapping() {
    for (int sector = 0; sector < comms::kSectors; ++sector) {
        const float angle = static_cast<float>(sector) * 3.14159265f / 4.0f;
        CHECK(comms::SectorAt(std::sin(angle), -std::cos(angle), comms::kStickDeadZone) == sector);
    }
    CHECK(comms::SectorAt(0.1f, 0.1f, comms::kStickDeadZone) == -1);
}

void TestKeyBinding() {
    CHECK(DCP_KEY_HOTKEY_COMMS_WHEEL == DCP_KEY_HOTKEY_EXIT_GAME - 1 && DCP_KEY_HOTKEY_EXIT_GAME == DCP_KEY_COUNT - 2 &&
          DCP_KEY_HOTKEY_CHARACTER_HIGHLIGHT == DCP_KEY_COUNT - 1 && DCP_KEY_HOTKEY_COMMS_WHEEL >= DCP_KEY_FIRST_HOTKEY);
    CHECK(!std::strcmp(dcp_key_actions[DCP_KEY_HOTKEY_COMMS_WHEEL].ini_key, "hotkey_comms_wheel"));
    CHECK(!std::strcmp(dcp_key_actions[DCP_KEY_HOTKEY_COMMS_WHEEL].label, "Comms Wheel"));
    DcpKeyMap map;
    dcp_keys_defaults(&map);
    CHECK(map.scancode[DCP_KEY_HOTKEY_COMMS_WHEEL] == SDL_SCANCODE_T);
    for (int i = 0; i < DCP_KEY_HOTKEY_COMMS_WHEEL; ++i) {
        CHECK(map.scancode[i] != SDL_SCANCODE_T);
    }
    CHECK(dcp_keys_apply(&map, "hotkey_comms_wheel", "g") && map.scancode[DCP_KEY_HOTKEY_COMMS_WHEEL] == SDL_SCANCODE_G);
    CHECK(!dcp_keys_apply(&map, "hotkey_comms_wheel", "escape") && !dcp_keys_apply(&map, "hotkey_comms_wheel", "notakey"));

    const std::string settings = ReadSource("gpu/shim/dcp_settings.h");
    CHECK(settings.find("comms_key{SDL_SCANCODE_T}") != std::string::npos);
    const std::string settings_cpp = ReadSource("gpu/shim/dcp_settings.cpp");
    CHECK(settings_cpp.find("&v.window_mode_key, &v.comms_key, &v.exit_key}") != std::string::npos);
    for (const char* file : {"gpu/shim/dcp_comms_wheel.cpp", "gpu/shim/dcp_comms_wheel.h", "src/runtime_pad.c", "gpu/shim/dcp_overlay.cpp"}) {
        CHECK(ReadSource(file).find("SDL_SCANCODE_T") == std::string::npos);
    }
    CHECK(ReadSource("gpu/shim/dcp_comms_wheel.cpp").find("DcpSettings::Get().comms_key") != std::string::npos);
}

void TestWiring() {
    const std::string pad = ReadSource("src/runtime_pad.c");
    const size_t shortcut = pad.find("early_story_shortcut_pad_filter(");
    const size_t wheel = pad.find("if (slot==0) apply_comms_wheel(d);");
    CHECK(shortcut != std::string::npos && wheel != std::string::npos && shortcut < wheel);
    const size_t captured = pad.find("if (dcpgpu_overlay_captures_input()) { s->hold_after_capture=1; return; }");
    CHECK(captured != std::string::npos && captured < wheel);
    CHECK(pad.find("comms_session_available()") != std::string::npos && pad.find("comms_session_send(out.send)") != std::string::npos);

    const std::string overlay = ReadSource("gpu/shim/dcp_overlay.cpp");
    CHECK(overlay.find("if (DcpCommsWheel::HandleEvent(event)) {") != std::string::npos);
    CHECK(overlay.find("DcpCommsWheel::PrepareTextures();") < overlay.find("new_frame();\n    ImGui::NewFrame();"));
    CHECK(overlay.find("DcpCommsWheel::Draw(") < overlay.find("ImGui::Render();"));

    const std::string wheel_cpp = ReadSource("gpu/shim/dcp_comms_wheel.cpp");
    CHECK(wheel_cpp.find("GetForegroundDrawList") != std::string::npos);
    CHECK(wheel_cpp.find("comms_text_message(") != std::string::npos);
    CHECK(wheel_cpp.find("Begin(") == std::string::npos && wheel_cpp.find("ImGui::Text") == std::string::npos);
    CHECK(wheel_cpp.find("dot_radius") == std::string::npos && wheel_cpp.find("AddCircleFilled(dot") == std::string::npos);
    CHECK(wheel_cpp.find("AddCircleFilled(center, inner") != std::string::npos);
    CHECK(wheel_cpp.find("SDL_BUTTON_LEFT") != std::string::npos && wheel_cpp.find("g_captured_buttons") != std::string::npos);
    CHECK(pad.find("out.hide_cross") != std::string::npos && pad.find("neutral_keyboard_axes") != std::string::npos);
}

void TestNeedHelpIcon() {
    const std::string svg = ReadSource("assets/comms/need-help.svg");
    CHECK(svg.find("<circle") != std::string::npos && svg.find("stroke-linecap=\"round\"") != std::string::npos);
    CHECK(svg.find("svgrepo") == std::string::npos && svg.find("912.1") == std::string::npos);
    const std::string sources = ReadSource("assets/comms/SOURCES.md");
    CHECK(sources.find("Raise Your Hand To Ask") == std::string::npos && sources.find("101551") == std::string::npos);
    CHECK(sources.find("| 5 Need help | `need-help.svg` |") != std::string::npos);
    CHECK(ReadSource("scripts/comms_icon_atlas.py").find("\"need-help\"") != std::string::npos);
    CHECK(ReadSource("gpu/CMakeLists.txt").find("assets/comms/icons.png") != std::string::npos);
    CHECK(ReadSource("gpu/shim/dcp_comms_wheel.cpp").find("id % kAtlasColumns") != std::string::npos);
}

} // namespace

int main() {
    TestControllerLatches();
    TestControllerCancel();
    TestNothingSelectedKeepsOpen();
    TestKeyboardOpenAndMouseConfirm();
    TestKeyboardCancel();
    TestKeyboardNavigation();
    TestAvailability();
    TestStaleStepDropsTheWheel();
    TestMouseAccumulator();
    TestFontFallbackAndLayout();
    TestHitMapping();
    TestKeyBinding();
    TestWiring();
    TestNeedHelpIcon();
    std::printf("comms wheel ui tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
