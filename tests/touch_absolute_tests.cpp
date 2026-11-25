// PS Vita Direct Touch (DCP-T7M4KC): the touchscreen mode setting, the option-read sites in every local boot image, the mode
// the pointer update sees while the screen is touched, the screen-to-surface mapping against the game's own Absolute
// position arithmetic, the tap click, the wiring that keeps controllers and trackpads untouched, and every refusal.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "cheat_hooks.h"
#include "dcp_pad_buttons.h"
#include "dcp_pad_owner.h"
#include "dcp_touchscreen.h"
#include "guest_hook.h"
#include "touch_absolute.h"

namespace fs = std::filesystem;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(condition)) {                                             \
            ++g_failures;                                               \
            std::printf("FAIL line %d: %s\n", __LINE__, #condition);    \
        }                                                               \
    } while (0)

using Bytes = std::vector<uint8_t>;

// The game's Absolute position constants: gain 1.5, offset -0.5 * 0.2 of the screen height.
constexpr float kGain = 1.5f, kOffsetA = -0.5f, kOffsetB = 0.2f;

constexpr uint64_t kModeGetter = 0x100, kOptionRecord = 0x120, kConstants = 0x180, kEngineAccessor = 0x1a0, kTouchRecord = 0x1c0;
constexpr uint64_t kTouchFill = 0x200, kPadRead = 0x240, kPortWrites = 0x280, kEngine = 0x300;
constexpr uint64_t kUpdate = 0x400, kAbsolute = 0x520, kStep = 0x900, kSecond = 0xc00;
constexpr uint64_t kUpdateRead = kUpdate + 45, kStepRead = kStep + 46;

void Put(Bytes& image, uint64_t at, std::initializer_list<uint8_t> bytes) {
    std::copy(bytes.begin(), bytes.end(), image.begin() + std::ptrdiff_t(at));
}
void PutRel(Bytes& image, uint64_t disp_at, uint64_t end, uint64_t target) {
    const int32_t rel = int32_t(int64_t(target) - int64_t(end));
    std::memcpy(image.data() + disp_at, &rel, 4);
}
void PutFloat(Bytes& image, uint64_t at, float value) { std::memcpy(image.data() + at, &value, 4); }

void PutCall(Bytes& image, uint64_t at, uint64_t target) {
    image[at] = 0xe8;
    PutRel(image, at + 1, at + 5, target);
}

void PutRead(Bytes& image, uint64_t at, bool step) {
    if (step) Put(image, at, {0xbe, 0xff, 0, 0, 0, 0x41, 0x83, 0xff, 0x03, 0x77, 0x18, 0x48, 0x8b, 0x80, 0x80, 0, 0, 0, 0x49, 0x63,
                              0xcf, 0x48, 0x69, 0xc9, 0xb0, 0x23, 0, 0, 0x8b, 0xb4, 0x08, 0x84, 0x22, 0, 0, 0x4c, 0x89, 0xef});
    else Put(image, at, {0xbe, 0xff, 0, 0, 0, 0x83, 0xfb, 0x03, 0x77, 0x18, 0x48, 0x8b, 0x80, 0x80, 0, 0, 0, 0x48, 0x63, 0xcb,
                         0x48, 0x69, 0xc9, 0xb0, 0x23, 0, 0, 0x8b, 0xb4, 0x08, 0x84, 0x22, 0, 0, 0x4c, 0x89, 0xff});
    const uint64_t option = at + (step ? 38 : 37);
    PutCall(image, option, kOptionRecord);
    Put(image, option + 5, {0x48, 0x89, 0xc7});
    PutCall(image, option + 8, kModeGetter);
    if (step) Put(image, option + 13, {0x89, 0x85, 0x2c, 0xfe, 0xff, 0xff, 0x83, 0xf8, 0x04, 0x75, 0x09});
    else Put(image, option + 13, {0x41, 0x89, 0xc7, 0x41, 0x83, 0xff, 0x04, 0x0f, 0x84, 0x13, 0x03, 0, 0});
}

void PutPortWrite(Bytes& image, int port) {
    Put(image, kPortWrites + uint64_t(port) * 0x10, {0xc7, 0x83, uint8_t(0xbc + port * 0x34), 0x24, 0, 0, uint8_t(port), 0, 0, 0});
    const uint32_t field = 0x24bc + uint32_t(port) * 0x34;
    std::memcpy(image.data() + kPortWrites + size_t(port) * 0x10 + 2, &field, 4);
}

void PutPortPath(Bytes& image) {
    Put(image, kOptionRecord, {0x89, 0xf0, 0x83, 0xfe, 0x03, 0x77, 0x10, 0x48, 0x8d, 0x0d, 0x11, 0x22, 0x33, 0x44, 0x48, 0x63, 0x04,
                               0x81, 0x48, 0x01, 0xc8, 0xff, 0xe0, 0x48, 0x83, 0xc7, 0x02, 0x48, 0x89, 0xf8, 0xc3, 0x48, 0x83, 0xc7,
                               0x06, 0x48, 0x89, 0xf8, 0xc3, 0x48, 0x83, 0xc7, 0x0a, 0x48, 0x89, 0xf8, 0xc3, 0x48, 0x83, 0xc7, 0x0e,
                               0x48, 0x89, 0xf8, 0xc3});
    Put(image, kEngineAccessor, {0x48, 0x8b, 0x05, 0, 0, 0, 0, 0xc3});
    PutRel(image, kEngineAccessor + 3, kEngineAccessor + 7, kEngine);
    Put(image, kTouchRecord, {0x89, 0xf0, 0x48, 0x6b, 0xc8, 0x34, 0x31, 0xc0, 0x83, 0xfe, 0x03, 0x48, 0x8d, 0x8c, 0x0f, 0x8c, 0x24,
                              0, 0, 0x48, 0x0f, 0x46, 0xc1, 0xc3});
    Put(image, kTouchFill, {0x44, 0x8b, 0x73, 0x30, 0xe8, 0, 0, 0, 0, 0x48, 0x8b, 0x40, 0x68, 0x4d, 0x69, 0xf6, 0xd0, 0x01, 0, 0});
    PutCall(image, kTouchFill + 4, kEngineAccessor);
    Put(image, kPadRead, {0x43, 0x8b, 0x5c, 0x3d, 0x44, 0x85, 0xdb, 0x0f, 0x88, 1, 2, 3, 4, 0x89, 0xdf, 0x4c, 0x89, 0xf6, 0xe8});
    for (int port = 0; port < 4; ++port) PutPortWrite(image, port);
    std::fill(image.begin() + kEngine, image.begin() + kEngine + 8, uint8_t(0));
}

void PutAbsolute(Bytes& image, uint64_t at) {
    Put(image, at, {0xc5, 0xfa, 0x10, 0x0d, 0, 0, 0, 0, 0xc5, 0xfa, 0x59, 0xd1, 0xc5, 0xfa, 0x16, 0xc0, 0xc5, 0xfa, 0x59,
                    0xc1, 0xc5, 0xf8, 0x29, 0x55, 0xc0, 0xc5, 0xfa, 0x11, 0x45, 0xd0, 0xe8, 0, 0, 0, 0, 0x48, 0x8b, 0x40,
                    0x40, 0xc5, 0xf8, 0x28, 0x4d, 0xc0, 0xc5, 0xfa, 0x10, 0x40, 0x40, 0xc5, 0xfa, 0x59, 0x05, 0, 0, 0, 0,
                    0xc5, 0xfa, 0x59, 0x05, 0, 0, 0, 0});
    PutRel(image, at + 4, at + 8, kConstants);
    PutRel(image, at + 53, at + 57, kConstants + 4);
    PutRel(image, at + 61, at + 65, kConstants + 8);
    PutCall(image, at + 30, kEngineAccessor);
}

// The shapes of the pointer update and step, the controller port path, the mode getter and the constants, at made-up offsets.
Bytes MakeImage() {
    Bytes image(0x1000, 0xcc);
    Put(image, kModeGetter, {0x0f, 0xb6, 0x47, 0x03, 0x83, 0xe0, 0x0f, 0xc3});
    PutPortPath(image);
    PutFloat(image, kConstants, kGain);
    PutFloat(image, kConstants + 4, kOffsetA);
    PutFloat(image, kConstants + 8, kOffsetB);
    PutRead(image, kUpdate, false);
    PutAbsolute(image, kAbsolute);
    PutRead(image, kStep, true);
    return image;
}

// The pointer the game computes in Absolute position from a touch surface point, as a fraction of the screen from its
// top-left corner, after its clamp to the screen.
float GamePointer(float gain, float offset, uint16_t touch, int resolution) {
    const float centred = gain * (float(touch) / float(resolution) - 0.5f) + offset;
    return std::fmin(0.5f, std::fmax(-0.5f, centred)) + 0.5f;
}

std::string ReadSource(const char* path) {
    std::ifstream file(fs::path(DCP_SOURCE_DIR) / path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

void TestModeSetting() {
    CHECK(dcp_touchscreen_mode_from_value(nullptr) == DCP_TOUCHSCREEN_MODE_PS4_TOUCHPAD);
    CHECK(dcp_touchscreen_mode_from_value("") == DCP_TOUCHSCREEN_MODE_PS4_TOUCHPAD);
    CHECK(dcp_touchscreen_mode_from_value("banana") == DCP_TOUCHSCREEN_MODE_PS4_TOUCHPAD);
    CHECK(dcp_touchscreen_mode_from_value("touchscreen") == DCP_TOUCHSCREEN_MODE_PS4_TOUCHPAD);
    CHECK(dcp_touchscreen_mode_from_value("PS_VITA_DIRECT_TOUCH") == DCP_TOUCHSCREEN_MODE_PS4_TOUCHPAD);
    CHECK(dcp_touchscreen_mode_from_value("ps4_touchpad") == DCP_TOUCHSCREEN_MODE_PS4_TOUCHPAD);
    CHECK(dcp_touchscreen_mode_from_value("ps_vita_direct_touch") == DCP_TOUCHSCREEN_MODE_PS_VITA_DIRECT_TOUCH);
    CHECK(std::string(dcp_touchscreen_mode_key) == "touchscreen_mode");
    CHECK(std::string(dcp_touchscreen_mode_name(DCP_TOUCHSCREEN_MODE_PS4_TOUCHPAD)) == "ps4_touchpad");
    CHECK(std::string(dcp_touchscreen_mode_label(DCP_TOUCHSCREEN_MODE_PS4_TOUCHPAD)) == "PS4 Touchpad");
    CHECK(std::string(dcp_touchscreen_mode_name(DCP_TOUCHSCREEN_MODE_PS_VITA_DIRECT_TOUCH)) == "ps_vita_direct_touch");
    CHECK(std::string(dcp_touchscreen_mode_label(DCP_TOUCHSCREEN_MODE_PS_VITA_DIRECT_TOUCH)) == "PS Vita Direct Touch");
    CHECK(std::string(dcp_touchscreen_mode_name(-1)) == "ps4_touchpad" && std::string(dcp_touchscreen_mode_name(99)) == "ps4_touchpad");
    for (int mode = 0; mode < DCP_TOUCHSCREEN_MODE_COUNT; ++mode) {
        CHECK(dcp_touchscreen_mode_from_value(dcp_touchscreen_mode_name(mode)) == mode);
    }
}

void TestMode() {
    CHECK(touch_absolute_mode(TOUCH_ABSOLUTE_MODE_RELATIVE, 0) == TOUCH_ABSOLUTE_MODE_RELATIVE);
    CHECK(touch_absolute_mode(TOUCH_ABSOLUTE_MODE_RELATIVE, 1) == TOUCH_ABSOLUTE_MODE_ABSOLUTE);
    for (int aligned : {2, 3}) {
        CHECK(touch_absolute_mode(aligned, 0) == aligned);
        CHECK(touch_absolute_mode(aligned, 1) == TOUCH_ABSOLUTE_MODE_ABSOLUTE);
    }
    CHECK(touch_absolute_mode(TOUCH_ABSOLUTE_MODE_ABSOLUTE, 1) == TOUCH_ABSOLUTE_MODE_ABSOLUTE);
    CHECK(touch_absolute_mode(TOUCH_ABSOLUTE_MODE_DISABLED, 1) == TOUCH_ABSOLUTE_MODE_DISABLED);
    CHECK(touch_absolute_mode(TOUCH_ABSOLUTE_MODE_DISABLED, 0) == TOUCH_ABSOLUTE_MODE_DISABLED);
    for (int unknown : {5, 9, 15}) {
        CHECK(touch_absolute_mode(unknown, 1) == unknown && touch_absolute_mode(unknown, 0) == unknown);
    }
}

// The pointer lands where the finger is, within a pixel of a 1920x1080 picture, everywhere on the screen.
void TestMappingPutsPointerUnderFinger() {
    const TouchAbsoluteMap map{kGain, kGain, kOffsetA * kOffsetB};
    float worst_x = 0, worst_y = 0;
    for (int step = 0; step <= 64; ++step) {
        const uint16_t x = uint16_t(std::lround(DCP_TOUCH_MAX_X * step / 64.0)), y = uint16_t(std::lround(DCP_TOUCH_MAX_Y * step / 64.0));
        uint16_t tx = 0, ty = 0;
        touch_absolute_map_point(&map, x, y, &tx, &ty);
        CHECK(tx <= DCP_TOUCH_MAX_X && ty <= DCP_TOUCH_MAX_Y);
        worst_x = std::fmax(worst_x, std::fabs(GamePointer(map.gain_x, 0.0f, tx, DCP_TOUCH_RES_X) - float(x) / DCP_TOUCH_MAX_X));
        worst_y = std::fmax(worst_y, std::fabs(GamePointer(map.gain_y, map.offset_y, ty, DCP_TOUCH_RES_Y) - float(y) / DCP_TOUCH_MAX_Y));
    }
    CHECK(worst_x * 1920.0f <= 1.0f);
    CHECK(worst_y * 1080.0f <= 2.0f);

    uint16_t cx = 0, cy = 0;
    touch_absolute_map_point(&map, DCP_TOUCH_MAX_X / 2, DCP_TOUCH_MAX_Y / 2, &cx, &cy);
    CHECK(std::abs(int(cx) - DCP_TOUCH_RES_X / 2) <= 1);
    CHECK(cy > DCP_TOUCH_RES_Y / 2); // the game lifts the pointer: the surface point sits lower
    uint16_t lx = 0, ly = 0, rx = 0, ry = 0;
    touch_absolute_map_point(&map, 0, 0, &lx, &ly);
    touch_absolute_map_point(&map, DCP_TOUCH_MAX_X, DCP_TOUCH_MAX_Y, &rx, &ry);
    CHECK(lx > 0 && rx < DCP_TOUCH_MAX_X && lx < DCP_TOUCH_RES_X / 2 && rx > DCP_TOUCH_RES_X / 2);
    CHECK(ly < ry && ry <= DCP_TOUCH_MAX_Y);

    // A gain below 1 would ask for points past the surface: they are clamped.
    const TouchAbsoluteMap wide{0.5f, 0.5f, 0.0f};
    touch_absolute_map_point(&wide, 0, DCP_TOUCH_MAX_Y, &lx, &ly);
    CHECK(lx == 0 && ly == DCP_TOUCH_MAX_Y);
}

// Dragging moves the surface point the same way, one step at a time and never backwards.
void TestDragIsMonotonic() {
    const TouchAbsoluteMap map{kGain, kGain, kOffsetA * kOffsetB};
    uint16_t px = 0xffff, py = 0, qx = 0, qy = 0xffff;
    for (int step = 0; step <= 64; ++step) {
        uint16_t tx = 0, ty = 0;
        touch_absolute_map_point(&map, uint16_t(DCP_TOUCH_MAX_X - (DCP_TOUCH_MAX_X / 2) * step / 64),
                                 uint16_t((DCP_TOUCH_MAX_Y / 2) * step / 64), &tx, &ty);
        CHECK(tx <= px && ty >= py);
        px = tx;
        py = ty;
        touch_absolute_map_point(&map, uint16_t(DCP_TOUCH_MAX_X * step / 64), uint16_t(DCP_TOUCH_MAX_Y - DCP_TOUCH_MAX_Y * step / 64),
                                 &tx, &ty);
        CHECK(tx >= qx && ty <= qy);
        qx = tx;
        qy = ty;
    }
}

// The existing window-to-picture mapping stays first: a letterboxed, pillarboxed or non-native window still puts the
// pointer under the finger, and a touch on a black bar is still ignored.
void TestLetterboxedWindows() {
    const TouchAbsoluteMap map{kGain, kGain, kOffsetA * kOffsetB};
    struct Window {
        int width, height;
        int picture_x, picture_y, picture_width, picture_height;
    };
    const Window windows[] = {
        {1920, 1080, 0, 0, 1920, 1080},      // native
        {1280, 800, 0, 40, 1280, 720},       // 16:10 window: bars above and below
        {3440, 1440, 440, 0, 2560, 1440},    // ultrawide window: bars at the sides
        {1024, 768, 0, 96, 1024, 576},       // 4:3 window
        {800, 480, 0, 15, 800, 450},         // handheld screen
    };
    for (const Window& window : windows) {
        const DcpTouchArea area = dcp_touch_area_from_pixels(window.picture_x, window.picture_y, window.picture_width,
                                                             window.picture_height, window.width, window.height);
        for (int step = 0; step <= 8; ++step) {
            const float u = float(step) / 8.0f, v = float(8 - step) / 8.0f;
            uint16_t x = 0, y = 0;
            CHECK(dcp_touch_point(area, area.x + u * area.width, area.y + v * area.height, &x, &y) == 1);
            uint16_t tx = 0, ty = 0;
            touch_absolute_map_point(&map, x, y, &tx, &ty);
            CHECK(std::fabs(GamePointer(map.gain_x, 0.0f, tx, DCP_TOUCH_RES_X) - u) * 1920.0f <= 1.5f);
            CHECK(std::fabs(GamePointer(map.gain_y, map.offset_y, ty, DCP_TOUCH_RES_Y) - v) * 1080.0f <= 2.5f);
        }
        if (area.x > 0.0f) {
            uint16_t x = 0, y = 0;
            CHECK(dcp_touch_point(area, area.x * 0.5f, 0.5f, &x, &y) == 0);
        }
        if (area.y > 0.0f) {
            uint16_t x = 0, y = 0;
            CHECK(dcp_touch_point(area, 0.5f, area.y * 0.5f, &x, &y) == 0);
        }
    }
    CHECK(ReadSource("src/touch_absolute.c").find("SDL_") == std::string::npos);
    CHECK(ReadSource("src/touch_absolute.c").find("window") == std::string::npos);
}

// The tap click is still the touchpad click at the touched point, and nothing the touchscreen does is Square.
void TestTapClick() {
    const TouchAbsoluteMap map{kGain, kGain, kOffsetA * kOffsetB};
    DcpTouchscreen* screen = dcp_touchscreen_new();
    const DcpTouchArea area = dcp_touch_area_from_pixels(0, 0, 1920, 1080, 1920, 1080);
    dcp_touchscreen_down(screen, 7, area, 0.25f, 0.75f, 1000000000ull);
    DcpTouchSample sample{};
    dcp_touchscreen_sample(screen, &sample);
    CHECK(sample.touch == 1 && sample.new_contact == 1 && sample.buttons == 0);
    dcp_touchscreen_up(screen, 7, area, 0.25f, 0.75f, 1100000000ull);
    int clicks = 0;
    for (int read = 0; read < 6; ++read) {
        dcp_touchscreen_sample(screen, &sample);
        if (sample.buttons) {
            ++clicks;
            CHECK(sample.buttons == BTN_TOUCHPAD && (sample.buttons & BTN_SQUARE) == 0 && sample.touch == 1);
            uint16_t tx = 0, ty = 0;
            touch_absolute_map_point(&map, sample.x, sample.y, &tx, &ty);
            CHECK(std::fabs(GamePointer(map.gain_x, 0.0f, tx, DCP_TOUCH_RES_X) - 0.25f) * 1920.0f <= 1.5f);
            CHECK(std::fabs(GamePointer(map.gain_y, map.offset_y, ty, DCP_TOUCH_RES_Y) - 0.75f) * 1080.0f <= 2.5f);
        }
    }
    CHECK(clicks == 3);
    dcp_touchscreen_sample(screen, &sample);
    CHECK(sample.touch == 0 && sample.buttons == 0);

    dcp_touchscreen_down(screen, 8, area, 0.2f, 0.2f, 2000000000ull);
    dcp_touchscreen_sample(screen, &sample);
    dcp_touchscreen_motion(screen, 8, area, 0.6f, 0.4f);
    dcp_touchscreen_up(screen, 8, area, 0.6f, 0.4f, 2100000000ull);
    for (int read = 0; read < 6; ++read) {
        dcp_touchscreen_sample(screen, &sample);
        CHECK(sample.buttons == 0);
    }
    dcp_touchscreen_free(screen);
}

void TestLocate() {
    const Bytes image = MakeImage();
    TouchAbsoluteSites sites{};
    CHECK(touch_absolute_locate(image.data(), image.size(), &sites) == 1);
    CHECK(sites.sites[0] == kUpdateRead && sites.sites[1] == kStepRead && sites.engine == kEngine);
    CHECK(sites.map.gain_x == kGain && sites.map.gain_y == kGain);
    CHECK(std::fabs(sites.map.offset_y - kOffsetA * kOffsetB) < 1e-6f);

    Bytes twice = image;
    PutRead(twice, kSecond, false);
    CHECK(touch_absolute_locate(twice.data(), twice.size(), &sites) == 0 && sites.sites[0] == 0 && sites.sites[1] == 0);
    Bytes step_twice = image;
    PutRead(step_twice, kSecond, true);
    CHECK(touch_absolute_locate(step_twice.data(), step_twice.size(), &sites) == 0);
    Bytes no_update = image;
    std::fill(no_update.begin() + kUpdate, no_update.begin() + kUpdate + 13, 0xcc);
    CHECK(touch_absolute_locate(no_update.data(), no_update.size(), &sites) == 0);
    Bytes other_getter = image;
    other_getter[kModeGetter + 3] = 0x02;
    CHECK(touch_absolute_locate(other_getter.data(), other_getter.size(), &sites) == 0);
    Bytes far = image;
    std::fill(far.begin() + kAbsolute, far.begin() + kAbsolute + 65, 0xcc);
    PutAbsolute(far, kUpdate + 0x400);
    CHECK(touch_absolute_locate(far.data(), far.size(), &sites) == 0);
    Bytes no_absolute = image;
    std::fill(no_absolute.begin() + kAbsolute, no_absolute.begin() + kAbsolute + 65, 0xcc);
    CHECK(touch_absolute_locate(no_absolute.data(), no_absolute.size(), &sites) == 0);
    Bytes two_absolute = image;
    PutAbsolute(two_absolute, kAbsolute + 0x80);
    CHECK(touch_absolute_locate(two_absolute.data(), two_absolute.size(), &sites) == 0);
    Bytes no_step = image;
    no_step[kStep + 38 + 13 + 9] = 0x74;
    CHECK(touch_absolute_locate(no_step.data(), no_step.size(), &sites) == 0);
    Bytes bad_gain = image;
    PutFloat(bad_gain, kConstants, 40.0f);
    CHECK(touch_absolute_locate(bad_gain.data(), bad_gain.size(), &sites) == 0);
    Bytes nan_offset = image;
    PutFloat(nan_offset, kConstants + 8, std::nanf(""));
    CHECK(touch_absolute_locate(nan_offset.data(), nan_offset.size(), &sites) == 0);
    Bytes big_offset = image;
    PutFloat(big_offset, kConstants + 8, 9.0f);
    CHECK(touch_absolute_locate(big_offset.data(), big_offset.size(), &sites) == 0);
    // Any broken or repeated link between the read and the pad handle refuses the whole locator.
    for (const uint64_t at : {kOptionRecord + 1, kOptionRecord + 23, kTouchRecord + 5, kTouchFill + 3, kTouchFill + 16, kPadRead + 4,
                              kEngineAccessor + 2, kPortWrites + 6, kPortWrites + 0x36, kUpdate + 30, kStep + 31}) {
        Bytes broken = image;
        broken[at] ^= 0x01;
        CHECK(touch_absolute_locate(broken.data(), broken.size(), &sites) == 0 && sites.engine == 0);
    }
    for (const auto& [from, length] : {std::pair{kTouchRecord, 24}, std::pair{kTouchFill, 20}, std::pair{kPadRead, 19}, std::pair{kPortWrites, 10}}) {
        Bytes repeated = image;
        std::copy_n(image.begin() + std::ptrdiff_t(from), length, repeated.begin() + kSecond);
        CHECK(touch_absolute_locate(repeated.data(), repeated.size(), &sites) == 0);
    }
    Bytes other_engine = image;
    Put(other_engine, kSecond, {0x48, 0x8b, 0x05, 0x11, 0, 0, 0, 0xc3});
    PutCall(other_engine, kAbsolute + 30, kSecond);
    CHECK(touch_absolute_locate(other_engine.data(), other_engine.size(), &sites) == 0);
    Bytes outside_engine = image;
    PutRel(outside_engine, kEngineAccessor + 3, kEngineAccessor + 7, outside_engine.size() - 4);
    CHECK(touch_absolute_locate(outside_engine.data(), outside_engine.size(), &sites) == 0);
    Bytes truncated(image.begin(), image.begin() + std::ptrdiff_t(kStep + 20));
    CHECK(touch_absolute_locate(truncated.data(), truncated.size(), &sites) == 0);
    CHECK(touch_absolute_locate(nullptr, 0, &sites) == 0);
}

struct Patched {
    Bytes memory;
    uint64_t size = 0;
    uint8_t* stubs = nullptr;
};

Patched Prepare(const Bytes& image) {
    Patched patched;
    patched.size = image.size();
    const size_t base = (image.size() + 0xfff) / 0x1000 * 0x1000;
    patched.memory.assign(base + touch_absolute_stub_bytes() + 0x1000, 0);
    std::copy(image.begin(), image.end(), patched.memory.begin());
    patched.stubs = patched.memory.data() + base;
    return patched;
}

bool Untouched(const Patched& patched, const Bytes& image) {
    return std::equal(image.begin(), image.end(), patched.memory.begin());
}

void TestRefusalsLeaveTheImageUntouched() {
    const Bytes image = MakeImage();
    CHECK(touch_absolute_stub_bytes() == TOUCH_ABSOLUTE_SITES * guest_hook_stub_size());

    Patched unknown = Prepare(image);
    CHECK(touch_absolute_install(unknown.memory.data(), unknown.size, 0, nullptr, unknown.stubs, touch_absolute_stub_bytes()) == 0);
    CHECK(Untouched(unknown, image));
    Bytes broken = image;
    broken[kModeGetter] = 0x90;
    Patched refused = Prepare(broken);
    CHECK(touch_absolute_install(refused.memory.data(), refused.size, 0, "CUSA10454-1.00", refused.stubs, touch_absolute_stub_bytes()) == 0);
    CHECK(Untouched(refused, broken));
    Bytes twice = image;
    PutRead(twice, kSecond, false);
    Patched ambiguous = Prepare(twice);
    CHECK(touch_absolute_install(ambiguous.memory.data(), ambiguous.size, 0, "CUSA10454-1.00", ambiguous.stubs, touch_absolute_stub_bytes()) == 0);
    CHECK(Untouched(ambiguous, twice));
    Patched cramped = Prepare(image);
    CHECK(touch_absolute_install(cramped.memory.data(), cramped.size, 0, "CUSA10454-1.00", cramped.stubs, touch_absolute_stub_bytes() - 1) == 0);
    CHECK(Untouched(cramped, image) && touch_absolute_ready() == 0);
    Patched none = Prepare(image);
    CHECK(touch_absolute_install(none.memory.data(), none.size, 0, "CUSA10454-1.00", none.stubs, 0) == 0);
    CHECK(Untouched(none, image) && touch_absolute_ready() == 0);

    // PS4 Touchpad: nothing installed, so the contact keeps its screen point and the game keeps its own option.
    uint16_t x = 100, y = 200;
    touch_absolute_remap(1, &x, &y);
    CHECK(x == 100 && y == 200);
    touch_absolute_set_contact(1);
    for (uint32_t port = 0; port < 5; ++port) CHECK(touch_absolute_port_owns_contact(port) == 0);
    touch_absolute_set_contact(0);
}

// The hooks and the stubs they use meet nothing else in the image: two sites, apart from each other and from a hook
// that already holds the same bytes.
void TestOverlapAndStubs() {
    const Bytes image = MakeImage();
    const size_t stub = guest_hook_stub_size();
    GuestHook hooks[2] = {{"update", uint32_t(kUpdateRead), 5, {0}, touch_absolute_read_mode},
                          {"step", uint32_t(kStepRead), 5, {0}, touch_absolute_read_mode}};
    for (int i = 0; i < 2; ++i) std::memcpy(hooks[i].original, image.data() + (i ? kStepRead : kUpdateRead), 5);
    size_t bad = 0;
    CHECK(guest_hooks_verify(image.data(), image.size(), 0, nullptr, hooks, 2, &bad) == GUEST_HOOK_OK);
    GuestHook same[3] = {hooks[0], hooks[1], hooks[0]};
    CHECK(guest_hooks_verify(image.data(), image.size(), 0, nullptr, same, 3, &bad) == GUEST_HOOK_OVERLAP);
    GuestHook beside[3] = {hooks[0], hooks[1], hooks[0]};
    beside[2].offset += 4;
    CHECK(guest_hooks_verify(image.data(), image.size(), 0, nullptr, beside, 3, &bad) != GUEST_HOOK_OK);

    Patched patched = Prepare(image);
    CHECK(patched.memory.size() >= patched.size + touch_absolute_stub_bytes());
    CHECK(touch_absolute_install(patched.memory.data(), patched.size, 0, "CUSA10454-1.00", patched.stubs,
                                 touch_absolute_stub_bytes()) == 1);
    const size_t stub_used = std::count_if(patched.stubs, patched.stubs + touch_absolute_stub_bytes() + stub,
                                           [](uint8_t byte) { return byte != 0; });
    CHECK(stub_used > 0);
    CHECK(std::all_of(patched.stubs + touch_absolute_stub_bytes(), patched.stubs + touch_absolute_stub_bytes() + 0x100,
                      [](uint8_t byte) { return byte == 0; }));
}

// The game's controller ports as the pad reader sees them: engine + 0x68 -> four records of 0x1d0 bytes, handle at 0x44.
struct FakePorts {
    Bytes engine = Bytes(0x80, 0), pads = Bytes(4 * 0x1d0, 0);

    void Attach(uint8_t* image, uint64_t engine_offset) {
        const uint8_t* pads_at = pads.data();
        const uint8_t* engine_at = engine.data();
        std::memcpy(engine.data() + 0x68, &pads_at, 8);
        std::memcpy(image + engine_offset, &engine_at, 8);
    }
    void Handles(int32_t a, int32_t b, int32_t c, int32_t d) {
        const int32_t handles[] = {a, b, c, d};
        for (size_t port = 0; port < 4; ++port) std::memcpy(pads.data() + port * 0x1d0 + 0x44, &handles[port], 4);
    }
};

constexpr int kModes[] = {TOUCH_ABSOLUTE_MODE_RELATIVE, TOUCH_ABSOLUTE_MODE_ABSOLUTE, 2, 3, TOUCH_ABSOLUTE_MODE_DISABLED};

// What the pointer update of `port` gets for a saved mode; the record must come back untouched.
int ReadMode(uint32_t port, int saved) {
    uint8_t record[4] = {0x12, 0x34, 0x56, uint8_t(0xa0 | saved)};
    const uint8_t before[4] = {record[0], record[1], record[2], record[3]};
    GuestRegs regs{};
    regs.rdi = uint64_t(uintptr_t(record));
    regs.rsi = port;
    touch_absolute_read_mode(&regs);
    CHECK(regs.skip == 1 && regs.rsi == port && std::memcmp(record, before, sizeof(record)) == 0);
    return int(regs.rax);
}

bool KeepsSaved(uint32_t port) {
    for (int saved : kModes) {
        if (ReadMode(port, saved) != saved) return false;
    }
    return true;
}

bool FollowsTouchscreen(uint32_t port) {
    for (int saved : kModes) {
        if (ReadMode(port, saved) != (saved == TOUCH_ABSOLUTE_MODE_DISABLED ? saved : TOUCH_ABSOLUTE_MODE_ABSOLUTE)) return false;
    }
    return true;
}

// Two local players, player 1 on the touchscreen and player 2 on a DS4 or DualSense touchpad, in every port layout the
// game can end up with. `image` already carries the hooks.
void CheckTwoPlayers(uint8_t* image, uint64_t engine_offset) {
    FakePorts ports;
    ports.Attach(image, engine_offset);
    ports.Handles(1, 2, -1, -1);

    touch_absolute_set_contact(0);
    for (uint32_t port = 0; port < 4; ++port) CHECK(KeepsSaved(port));

    touch_absolute_set_contact(1);
    CHECK(FollowsTouchscreen(0) && KeepsSaved(1) && KeepsSaved(2) && KeepsSaved(3));
    for (int read = 0; read < 8; ++read) {
        for (int saved : kModes) {
            CHECK(ReadMode(0, TOUCH_ABSOLUTE_MODE_RELATIVE) == TOUCH_ABSOLUTE_MODE_ABSOLUTE);
            CHECK(ReadMode(1, saved) == saved);
        }
    }
    CHECK(ReadMode(0, TOUCH_ABSOLUTE_MODE_DISABLED) == TOUCH_ABSOLUTE_MODE_DISABLED && ReadMode(1, 2) == 2);
    CHECK(ReadMode(0, 2) == TOUCH_ABSOLUTE_MODE_ABSOLUTE && ReadMode(1, TOUCH_ABSOLUTE_MODE_DISABLED) == TOUCH_ABSOLUTE_MODE_DISABLED);
    for (uint32_t none : {4u, 5u, 0xffu, 0xffffffffu}) CHECK(KeepsSaved(none));

    touch_absolute_set_contact(0);
    for (uint32_t port = 0; port < 4; ++port) CHECK(KeepsSaved(port));

    // Player 1's pad in the game's second port: the override follows the handle, not the port number.
    touch_absolute_set_contact(1);
    ports.Handles(2, 1, -1, -1);
    CHECK(KeepsSaved(0) && FollowsTouchscreen(1) && KeepsSaved(2) && KeepsSaved(3));
    ports.Handles(3, 2, 4, 1);
    CHECK(KeepsSaved(0) && KeepsSaved(1) && KeepsSaved(2) && FollowsTouchscreen(3));

    // Player 1's pad closed, or never opened: nobody inherits the contact and the point keeps its PS4 Touchpad place.
    for (const int32_t closed : {-1, 0, int32_t(0x80920003)}) {
        ports.Handles(closed, 2, -1, -1);
        for (uint32_t port = 0; port < 4; ++port) CHECK(KeepsSaved(port));
        uint16_t x = 1700, y = 100;
        touch_absolute_remap(1, &x, &y);
        CHECK(x == 1700 && y == 100);
    }

    // No engine or no ports yet.
    ports.Handles(1, 2, -1, -1);
    const uint64_t null = 0;
    std::memcpy(ports.engine.data() + 0x68, &null, 8);
    CHECK(KeepsSaved(0) && KeepsSaved(1));
    std::memcpy(image + engine_offset, &null, 8);
    CHECK(KeepsSaved(0) && KeepsSaved(1));
    uint16_t x = 1700, y = 100;
    touch_absolute_remap(1, &x, &y);
    CHECK(x == 1700 && y == 100);

    // A handle that is not a pad never matches a port without one.
    ports.Attach(image, engine_offset);
    for (const int32_t contact : {-1, 0}) {
        ports.Handles(contact, contact, contact, contact);
        touch_absolute_set_contact(contact);
        for (uint32_t port = 0; port < 4; ++port) CHECK(KeepsSaved(port));
    }
    touch_absolute_set_contact(0);
    std::memcpy(image + engine_offset, &null, 8);
}

// The devices behind the players move between slots; the touchscreen stays with player 1's handle.
void CheckControllerSwap(uint8_t* image, uint64_t engine_offset) {
    FakePorts ports;
    ports.Attach(image, engine_offset);
    ports.Handles(1, 2, -1, -1);
    DcpPadOwners owners;
    dcp_pad_owners_init(&owners);
    const DcpPadDevice xbox{10, 100, 0}, ds4{11, 200, 1};
    const DcpPadDevice both[] = {xbox, ds4};
    DcpPadAssignment assigned[4];
    CHECK(dcp_pad_owners_assign(&owners, both, 2, assigned, 4) == 2);
    touch_absolute_set_contact(1);
    CHECK(dcp_pad_owners_slot_of(&owners, ds4.instance) == 1 && KeepsSaved(1) && FollowsTouchscreen(0));
    dcp_pad_owners_release(&owners, 0);
    CHECK(dcp_pad_owners_slot_of(&owners, ds4.instance) == 1 && KeepsSaved(1) && FollowsTouchscreen(0));
    touch_absolute_set_contact(0);
    CHECK(KeepsSaved(0) && KeepsSaved(1));
    const uint64_t null = 0;
    std::memcpy(image + engine_offset, &null, 8);
}

void TestInstallAndHandler() {
    const Bytes image = MakeImage();
    Patched patched = Prepare(image);
    CHECK(touch_absolute_install(patched.memory.data(), patched.size, 0, "CUSA10454-1.00", patched.stubs,
                                 touch_absolute_stub_bytes()) == 1);
    CHECK(patched.memory[kUpdateRead] == 0xe9 && patched.memory[kStepRead] == 0xe9);
    CHECK(std::equal(image.begin(), image.begin() + kUpdateRead, patched.memory.begin()));
    CHECK(std::equal(image.begin() + kUpdateRead + 5, image.begin() + kStepRead, patched.memory.begin() + kUpdateRead + 5));
    CHECK(std::equal(image.begin() + kStepRead + 5, image.end(), patched.memory.begin() + kStepRead + 5));
    CHECK(touch_absolute_ready() == 1);

    FakePorts ports;
    ports.Attach(patched.memory.data(), kEngine);
    ports.Handles(1, 2, -1, -1);
    const TouchAbsoluteMap map{kGain, kGain, kOffsetA * kOffsetB};
    uint16_t ex = 0, ey = 0;
    touch_absolute_map_point(&map, 1700, 100, &ex, &ey);
    uint16_t x = 1700, y = 100;
    touch_absolute_remap(1, &x, &y);
    CHECK(x == ex && y == ey);
    x = 1700;
    y = 100;
    touch_absolute_remap(3, &x, &y);
    CHECK(x == 1700 && y == 100);

    CheckTwoPlayers(patched.memory.data(), kEngine);
    CheckControllerSwap(patched.memory.data(), kEngine);
}

// The pad sampler maps only the touchscreen's contact and reports it on every read of player 1 (also while a menu holds
// the input); controller touchpads and the trackpads never reach the remap; the loader installs the hooks only for
// PS Vita Direct Touch on a supported build and the stub area follows.
void TestWiring() {
    const std::string pad = ReadSource("src/runtime_pad.c");
    CHECK(pad.find("pointer_owns_touch=0;\n    if (slot==0) touchscreen_owns_touch=0;\n    if (dcpgpu_overlay_captures_input()) return;") != std::string::npos);
    CHECK(pad.find("sample_host(slot,d);\n    if (slot==0) touch_absolute_set_contact(touchscreen_owns_touch ? TOUCHSCREEN_PAD : 0);") !=
          std::string::npos);
    CHECK(pad.find("touch_absolute_set_contact(") == pad.rfind("touch_absolute_set_contact("));
    CHECK(pad.find("enum { TOUCHSCREEN_PAD = 1 };") != std::string::npos);
    CHECK(pad.find("touchscreen_owns_touch=screen.touch;") != std::string::npos);
    CHECK(pad.find("handle>=1 && handle<=MAX_PADS && slots[handle-1].opened ? handle-1 : -1") != std::string::npos);
    const size_t sampler = pad.find("static void sample_host(");
    const size_t player1 = pad.find("if (slot==0) {", sampler);
    CHECK(player1 < pad.find("dcp_touchscreen_sample(", sampler) && pad.find("dcp_touchscreen_sample(") == pad.rfind("dcp_touchscreen_sample("));
    const size_t merge = pad.find("static void merge_touchscreen");
    const size_t merge_end = pad.find("static void key_axis", merge);
    CHECK(merge != std::string::npos && merge_end != std::string::npos);
    CHECK(pad.find("touch_absolute_remap(TOUCHSCREEN_PAD,&x,&y);", merge) < merge_end);
    CHECK(pad.find("touch_absolute_remap") == pad.rfind("touch_absolute_remap"));
    const size_t controller_touch = pad.find("const int touchpad=surface_touchpad(g);", pad.find("static void sample_host("));
    const size_t controller_end = pad.find("merge_mouse(d,&mouse);", controller_touch);
    CHECK(controller_touch != std::string::npos && controller_end != std::string::npos);
    CHECK(pad.substr(controller_touch, controller_end - controller_touch).find("touch_absolute") == std::string::npos);
    CHECK(pad.find("merge_touchscreen(d,&screen);", controller_end) != std::string::npos);

    const std::string loader = ReadSource("src/loader.c");
    CHECK(loader.find("const int touchscreen_direct_touch=dcp_touchscreen_direct_touch(settings_file);") != std::string::npos);
    CHECK(loader.find("known_build && touchscreen_direct_touch ? touch_absolute_stub_bytes() : 0") != std::string::npos);
    CHECK(loader.find("kraken_fix_stub_area + touch_absolute_stub_area;") != std::string::npos);
    const size_t gate = loader.find("if (!touchscreen_direct_touch) puts(\"Touchscreen mode: PS4 Touchpad\");");
    CHECK(gate != std::string::npos && loader.find("else touch_absolute_install(image,size,entry,known_build ? build_key : NULL,", gate) > gate);
    CHECK(loader.find("touch_absolute_install(") == loader.rfind("touch_absolute_install("));
    CHECK(loader.find("touch_absolute_set_contact") == std::string::npos);
    CHECK(ReadSource("src/touch_absolute.c").find("getenv") == std::string::npos);
}

// ---- the local boot images ----

bool ReadBootImage(const fs::path& path, Bytes* image) {
    std::ifstream in(path, std::ios::binary);
    auto read64 = [&in]() {
        uint64_t value = 0;
        in.read(reinterpret_cast<char*>(&value), 8);
        return value;
    };
    char magic[8] = {};
    in.read(magic, 8);
    if (!in || std::memcmp(magic, "DCPBOOT1", 8) != 0) return false;
    const uint64_t size = read64();
    read64();
    const uint64_t segments = read64(), relocations = read64(), imports = read64();
    read64();
    for (int i = 0; i < 5; ++i) read64();
    const uint64_t modules = read64();
    for (uint64_t i = 0; i < modules * 7; ++i) read64();
    const uint64_t bindings = read64();
    for (uint64_t i = 0; i < bindings * 3 + segments * 3; ++i) read64();
    in.seekg(static_cast<std::streamoff>(imports * 128 + relocations * 32), std::ios::cur);
    image->resize(size);
    in.read(reinterpret_cast<char*>(image->data()), static_cast<std::streamsize>(size));
    return static_cast<bool>(in);
}

struct Build {
    const char* key;
    const char* folder; // under build/image, then build/research/image; null: no local executable is known
    uint64_t entry;
};

constexpr Build kBuilds[] = {
    {"CUSA10454-1.00", "v1.03", 0x54c690},
    {"CUSA10454-1.01", "CUSA10454-1.01", 0x54c690},
    {"CUSA07001-1.00", "CUSA07001-1.00", 0x54a800},
    {"CUSA07001-1.02", "CUSA07001-1.02", 0x54a660},
    {"CUSA07001-1.03", "CUSA07001-1.03", 0x54a720},
    {"CUSA10209-1.00", "CUSA10209-1.00", 0x54a790},
    {"CUSA10209-1.02", nullptr, 0},
    {"CUSA10487-1.03", nullptr, 0},
};

bool LocalImage(const Build& build, Bytes* image) {
    if (!build.folder) return false;
    for (const char* root : {"build/image", "build/research/image"}) {
        if (ReadBootImage(fs::path(DCP_SOURCE_DIR) / root / build.folder / "boot.bin", image)) return true;
    }
    return false;
}

// The sites in a real executable, read only: found once each, a real call to the option getter, the game's own
// constants, hooks that install on a private copy and meet no cheat hook.
bool CheckRealImage(const Build& build, const Bytes& image) {
    const int before = g_failures;
    TouchAbsoluteSites sites{};
    CHECK(touch_absolute_locate(image.data(), image.size(), &sites) == 1);
    CHECK(sites.sites[0] != 0 && sites.sites[1] != 0 && sites.sites[0] != sites.sites[1]);
    for (uint64_t site : sites.sites) {
        CHECK(site + 5 <= image.size() && image[site] == 0xe8);
        int32_t rel = 0;
        std::memcpy(&rel, image.data() + site + 1, 4);
        const int64_t getter = int64_t(site) + 5 + rel;
        CHECK(getter > 0 && uint64_t(getter) + 8 <= image.size());
        const uint8_t body[] = {0x0f, 0xb6, 0x47, 0x03, 0x83, 0xe0, 0x0f, 0xc3};
        CHECK(getter > 0 && std::memcmp(image.data() + getter, body, sizeof(body)) == 0);
    }
    CHECK(sites.map.gain_x >= 0.5f && sites.map.gain_x <= 4.0f && std::fabs(sites.map.offset_y) <= 0.5f);

    Patched patched = Prepare(image);
    CHECK(touch_absolute_install(patched.memory.data(), patched.size, build.entry, build.key, patched.stubs,
                                 touch_absolute_stub_bytes()) == 1);
    CHECK(patched.memory[sites.sites[0]] == 0xe9 && patched.memory[sites.sites[1]] == 0xe9);
    size_t changed = 0;
    for (size_t i = 0; i < image.size(); ++i) changed += patched.memory[i] != image[i];
    CHECK(changed <= 2 * TOUCH_ABSOLUTE_SITE_BYTES);

    if (std::string(build.key) == "CUSA10454-1.00") {
        std::vector<GuestHook> all(dcp::cheats::Hooks().begin(), dcp::cheats::Hooks().end());
        for (const GuestHook& hook : all) {
            for (uint64_t site : sites.sites) CHECK(hook.offset + hook.length <= site || hook.offset >= site + 5);
        }
        for (int i = 0; i < TOUCH_ABSOLUTE_SITES; ++i) {
            GuestHook hook = {"touch pointer mode", uint32_t(sites.sites[i]), 5, {0}, touch_absolute_read_mode};
            std::memcpy(hook.original, image.data() + sites.sites[i], 5);
            all.push_back(hook);
        }
        size_t bad = 0;
        CHECK(guest_hooks_verify(image.data(), image.size(), build.entry, &dcp::cheats::kSupportedImage, all.data(), all.size(),
                                 &bad) == GUEST_HOOK_OK);
    }
    if (touch_absolute_ready() && sites.engine) CheckTwoPlayers(patched.memory.data(), sites.engine);
    std::printf("TOUCH-REAL|%s|%s|sites=0x%llx,0x%llx engine=0x%llx gain=%.3f offset=%.3f\n", build.key,
                g_failures == before ? "PASS" : "FAIL", (unsigned long long)sites.sites[0], (unsigned long long)sites.sites[1],
                (unsigned long long)sites.engine, sites.map.gain_x, sites.map.offset_y);
    return g_failures == before;
}

void TestRealImages() {
    int found = 0;
    for (const Build& build : kBuilds) {
        Bytes image;
        if (!LocalImage(build, &image)) {
            std::printf("TOUCH-REAL|%s|BLOCKED|no local boot image; the locator is not proven on this build\n", build.key);
            continue;
        }
        ++found;
        CheckRealImage(build, image);
    }
    std::printf("touch absolute: %d of %zu supported builds validated against a local boot image\n", found,
                sizeof(kBuilds) / sizeof(*kBuilds));
}

} // namespace

int main() {
    TestModeSetting();
    TestMode();
    TestMappingPutsPointerUnderFinger();
    TestDragIsMonotonic();
    TestLetterboxedWindows();
    TestTapClick();
    TestLocate();
    TestRefusalsLeaveTheImageUntouched();
    TestOverlapAndStubs();
    TestInstallAndHandler();
    TestWiring();
    TestRealImages();
    std::printf("touch absolute tests: %d checks, %d failures\n", g_checks, g_failures);
    std::printf("GROUP|touchscreen-hook|%s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures ? 1 : 0;
}
