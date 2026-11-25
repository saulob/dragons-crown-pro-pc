// The touchscreen pointer: where the game's touch cursor option is read, the mode the pointer update sees while the screen
// is touched, the screen-to-touch-surface mapping against the game's own Absolute position arithmetic, and every refusal.
// DCP_TOUCH_ABSOLUTE_EBOOT=<flat image> also locates the sites in a real executable image.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "dcp_pad_buttons.h"
#include "guest_hook.h"
#include "touch_absolute.h"

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

// The game's Absolute position constants in CUSA10454: gain 1.5, offset -0.5 * 0.2 of the screen height.
constexpr float kGain = 1.5f, kOffsetA = -0.5f, kOffsetB = 0.2f;

constexpr uint64_t kModeGetter = 0x100, kOptionRecord = 0x120, kConstants = 0x180;
constexpr uint64_t kUpdate = 0x400, kAbsolute = 0x520, kStep = 0x900;

void Put(Bytes& image, uint64_t at, std::initializer_list<uint8_t> bytes) {
    std::copy(bytes.begin(), bytes.end(), image.begin() + at);
}
void PutRel(Bytes& image, uint64_t disp_at, uint64_t end, uint64_t target) {
    const int32_t rel = int32_t(int64_t(target) - int64_t(end));
    std::memcpy(image.data() + disp_at, &rel, 4);
}
void PutFloat(Bytes& image, uint64_t at, float value) { std::memcpy(image.data() + at, &value, 4); }

void PutRead(Bytes& image, uint64_t at, bool step) {
    Put(image, at, {0xe8, 0, 0, 0, 0, 0x48, 0x89, 0xc7, 0xe8, 0, 0, 0, 0});
    PutRel(image, at + 1, at + 5, kOptionRecord);
    PutRel(image, at + 9, at + 13, kModeGetter);
    if (step) Put(image, at + 13, {0x89, 0x85, 0x2c, 0xfe, 0xff, 0xff, 0x83, 0xf8, 0x04, 0x75, 0x09});
    else Put(image, at + 13, {0x41, 0x89, 0xc7, 0x41, 0x83, 0xff, 0x04, 0x0f, 0x84, 0x13, 0x03, 0, 0});
}

void PutAbsolute(Bytes& image, uint64_t at) {
    Put(image, at, {0xc5, 0xfa, 0x10, 0x0d, 0, 0, 0, 0, 0xc5, 0xfa, 0x59, 0xd1, 0xc5, 0xfa, 0x16, 0xc0, 0xc5, 0xfa, 0x59,
                    0xc1, 0xc5, 0xf8, 0x29, 0x55, 0xc0, 0xc5, 0xfa, 0x11, 0x45, 0xd0, 0xe8, 0, 0, 0, 0, 0x48, 0x8b, 0x40,
                    0x40, 0xc5, 0xf8, 0x28, 0x4d, 0xc0, 0xc5, 0xfa, 0x10, 0x40, 0x40, 0xc5, 0xfa, 0x59, 0x05, 0, 0, 0, 0,
                    0xc5, 0xfa, 0x59, 0x05, 0, 0, 0, 0});
    PutRel(image, at + 4, at + 8, kConstants);
    PutRel(image, at + 53, at + 57, kConstants + 4);
    PutRel(image, at + 61, at + 65, kConstants + 8);
}

// The shapes of CUSA10454's pointer update, pointer step, mode getter and constants, at made-up offsets.
Bytes MakeImage() {
    Bytes image(0x1000, 0xcc);
    Put(image, kModeGetter, {0x0f, 0xb6, 0x47, 0x03, 0x83, 0xe0, 0x0f, 0xc3});
    Put(image, kOptionRecord, {0x89, 0xf0, 0xc3});
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

void TestMode() {
    CHECK(touch_absolute_mode(TOUCH_ABSOLUTE_MODE_RELATIVE, 0) == TOUCH_ABSOLUTE_MODE_RELATIVE);
    CHECK(touch_absolute_mode(TOUCH_ABSOLUTE_MODE_RELATIVE, 1) == TOUCH_ABSOLUTE_MODE_ABSOLUTE);
    for (int aligned : {2, 3}) {
        CHECK(touch_absolute_mode(aligned, 0) == aligned);
        CHECK(touch_absolute_mode(aligned, 1) == TOUCH_ABSOLUTE_MODE_ABSOLUTE);
    }
    CHECK(touch_absolute_mode(TOUCH_ABSOLUTE_MODE_DISABLED, 1) == TOUCH_ABSOLUTE_MODE_DISABLED);
    CHECK(touch_absolute_mode(TOUCH_ABSOLUTE_MODE_DISABLED, 0) == TOUCH_ABSOLUTE_MODE_DISABLED);
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

    // Dragging from the top-right corner to the centre moves the surface point the same way, monotonically.
    uint16_t px = 0xffff, py = 0;
    for (int step = 0; step <= 16; ++step) {
        const uint16_t x = uint16_t(DCP_TOUCH_MAX_X - (DCP_TOUCH_MAX_X / 2) * step / 16), y = uint16_t((DCP_TOUCH_MAX_Y / 2) * step / 16);
        uint16_t tx = 0, ty = 0;
        touch_absolute_map_point(&map, x, y, &tx, &ty);
        CHECK(tx <= px && ty >= py);
        px = tx;
        py = ty;
    }

    // A gain below 1 would ask for points past the surface: they are clamped.
    const TouchAbsoluteMap wide{0.5f, 0.5f, 0.0f};
    touch_absolute_map_point(&wide, 0, DCP_TOUCH_MAX_Y, &lx, &ly);
    CHECK(lx == 0 && ly == DCP_TOUCH_MAX_Y);
}

void TestLocate() {
    const Bytes image = MakeImage();
    TouchAbsoluteSites sites{};
    CHECK(touch_absolute_locate(image.data(), image.size(), &sites) == 1);
    CHECK(sites.sites[0] == kUpdate + 8 && sites.sites[1] == kStep + 8);
    CHECK(sites.map.gain_x == kGain && sites.map.gain_y == kGain);
    CHECK(std::fabs(sites.map.offset_y - kOffsetA * kOffsetB) < 1e-6f);

    // A second pointer update, a getter that reads another field, Absolute position outside the update, constants that are
    // not the game's: nothing is found.
    Bytes twice = image;
    PutRead(twice, 0xc00, false);
    CHECK(touch_absolute_locate(twice.data(), twice.size(), &sites) == 0 && sites.sites[0] == 0);
    Bytes other_getter = image;
    other_getter[kModeGetter + 3] = 0x02;
    CHECK(touch_absolute_locate(other_getter.data(), other_getter.size(), &sites) == 0);
    Bytes far = image;
    std::fill(far.begin() + kAbsolute, far.begin() + kAbsolute + 65, 0xcc);
    PutAbsolute(far, kUpdate + 0x400);
    CHECK(touch_absolute_locate(far.data(), far.size(), &sites) == 0);
    Bytes no_step = image;
    no_step[kStep + 13 + 9] = 0x74;
    CHECK(touch_absolute_locate(no_step.data(), no_step.size(), &sites) == 0);
    Bytes bad_gain = image;
    PutFloat(bad_gain, kConstants, 40.0f);
    CHECK(touch_absolute_locate(bad_gain.data(), bad_gain.size(), &sites) == 0);
    Bytes nan_offset = image;
    PutFloat(nan_offset, kConstants + 8, std::nanf(""));
    CHECK(touch_absolute_locate(nan_offset.data(), nan_offset.size(), &sites) == 0);
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

void TestInstallAndHandler() {
    const Bytes image = MakeImage();

    Patched unknown = Prepare(image);
    CHECK(touch_absolute_install(unknown.memory.data(), unknown.size, 0, nullptr, unknown.stubs, touch_absolute_stub_bytes()) == 0);
    CHECK(std::equal(image.begin(), image.end(), unknown.memory.begin()));
    Bytes broken = image;
    broken[kModeGetter] = 0x90;
    Patched refused = Prepare(broken);
    CHECK(touch_absolute_install(refused.memory.data(), refused.size, 0, "CUSA10454-1.00", refused.stubs, touch_absolute_stub_bytes()) == 0);
    CHECK(std::equal(broken.begin(), broken.end(), refused.memory.begin()));
    CHECK(touch_absolute_ready() == 0);

    // Not ready: the contact keeps its screen point.
    uint16_t x = 100, y = 200;
    touch_absolute_remap(&x, &y);
    CHECK(x == 100 && y == 200);

    Patched patched = Prepare(image);
    CHECK(touch_absolute_install(patched.memory.data(), patched.size, 0, "CUSA10454-1.00", patched.stubs,
                                 touch_absolute_stub_bytes()) == 1);
    CHECK(patched.memory[kUpdate + 8] == 0xe9 && patched.memory[kStep + 8] == 0xe9);
    CHECK(std::equal(image.begin(), image.begin() + kUpdate + 8, patched.memory.begin()));
    CHECK(touch_absolute_ready() == 1);

    const TouchAbsoluteMap map{kGain, kGain, kOffsetA * kOffsetB};
    uint16_t ex = 0, ey = 0;
    touch_absolute_map_point(&map, 1700, 100, &ex, &ey);
    x = 1700;
    y = 100;
    touch_absolute_remap(&x, &y);
    CHECK(x == ex && y == ey);

    // The handler answers the read in place of the getter, from the record it was given.
    uint8_t record[4] = {0, 0, 0x35, TOUCH_ABSOLUTE_MODE_RELATIVE | 0x50};
    GuestRegs regs{};
    regs.rdi = uint64_t(uintptr_t(record));
    touch_absolute_set_contact(0);
    touch_absolute_read_mode(&regs);
    CHECK(regs.rax == TOUCH_ABSOLUTE_MODE_RELATIVE && regs.skip == 1);
    touch_absolute_set_contact(1);
    regs = GuestRegs{};
    regs.rdi = uint64_t(uintptr_t(record));
    touch_absolute_read_mode(&regs);
    CHECK(regs.rax == TOUCH_ABSOLUTE_MODE_ABSOLUTE && regs.skip == 1);
    CHECK(record[3] == (TOUCH_ABSOLUTE_MODE_RELATIVE | 0x50)); // the saved option is never written
    record[3] = TOUCH_ABSOLUTE_MODE_DISABLED;
    regs = GuestRegs{};
    regs.rdi = uint64_t(uintptr_t(record));
    touch_absolute_read_mode(&regs);
    CHECK(regs.rax == TOUCH_ABSOLUTE_MODE_DISABLED);
    touch_absolute_set_contact(0);
}

std::string ReadSource(const char* path) {
    std::ifstream file(std::string(DCP_SOURCE_DIR) + "/" + path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// The pad sampler reports the contact on every read of player 1 (also while a menu holds the input) and remaps only the
// touchscreen's point; the loader installs the hooks for a supported build.
void TestWiring() {
    const std::string pad = ReadSource("src/runtime_pad.c");
    CHECK(pad.find("if (slot==0) touch_absolute_set_contact(0);\n    if (dcpgpu_overlay_captures_input()) return;") != std::string::npos);
    CHECK(pad.find("touch_absolute_set_contact(screen.touch);") != std::string::npos);
    const size_t merge = pad.find("static void merge_touchscreen");
    CHECK(merge != std::string::npos && pad.find("touch_absolute_remap(&x,&y);", merge) < pad.find("static void key_axis", merge));
    CHECK(pad.find("touch_absolute_remap") == pad.rfind("touch_absolute_remap"));
    const std::string loader = ReadSource("src/loader.c");
    CHECK(loader.find("else touch_absolute_install(image,size,entry,known_build ? build_key : NULL,") != std::string::npos);
    CHECK(loader.find("known_build && touchscreen_direct ? touch_absolute_stub_bytes() : 0") != std::string::npos);
    CHECK(loader.find("kraken_fix_stub_area + touch_absolute_stub_area;") != std::string::npos);
}

// A flat image of a real executable (segment bytes at their addresses), when one is given.
void TestRealImage() {
    const char* path = std::getenv("DCP_TOUCH_ABSOLUTE_EBOOT");
    if (!path || !*path) return;
    std::ifstream file(path, std::ios::binary);
    const Bytes image((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    TouchAbsoluteSites sites{};
    CHECK(touch_absolute_locate(image.data(), image.size(), &sites) == 1);
    std::printf("  %s: sites 0x%llx 0x%llx, gain %.3f, offset %.3f\n", path, (unsigned long long)sites.sites[0],
                (unsigned long long)sites.sites[1], sites.map.gain_x, sites.map.offset_y);
}

} // namespace

int main() {
    TestMode();
    TestMappingPutsPointerUnderFinger();
    TestLocate();
    TestInstallAndHandler();
    TestWiring();
    TestRealImage();
    std::printf("touch absolute tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
