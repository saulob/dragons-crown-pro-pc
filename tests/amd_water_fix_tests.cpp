// Focused tests of the experimental AMD water fix switch (issue #32): vendor defaults, F3 presses on every GPU, the
// stencil/depth decisions and the absence of an environment switch.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "dcp_amd_water_fix.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);          \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

using namespace DcpAmdWaterFix;

constexpr std::uint32_t kNvidia = 0x10de;
constexpr std::uint32_t kIntel = 0x8086;
constexpr std::uint32_t kOther = 0x13b5; // Arm

void TestVendorDefaults() {
    CHECK(IsAmdVendor(0x1002));
    CHECK(!IsAmdVendor(kNvidia) && !IsAmdVendor(kIntel) && !IsAmdVendor(kOther));
    CHECK(DefaultOn(0x1002));
    CHECK(!DefaultOn(kNvidia) && !DefaultOn(kIntel) && !DefaultOn(kOther));
}

void TestPresses(std::uint32_t vendor) {
    const bool start = DefaultOn(vendor);
    Switch fix;
    fix.Init(vendor);
    CHECK(fix.On() == start);
    CHECK(fix.Press(false) && fix.On() == !start);
    CHECK(!fix.Press(true) && fix.On() == !start); // holding F3 repeats the key: no flip
    CHECK(!fix.Press(true) && fix.On() == !start);
    CHECK(fix.Press(false) && fix.On() == start);
    CHECK(fix.Press(false) && fix.On() == !start);
    fix.Init(vendor); // a new process starts from the default again
    CHECK(fix.On() == start);
}

void TestAllVendors() {
    for (const std::uint32_t vendor : {std::uint32_t(0x1002), kNvidia, kIntel, kOther}) {
        TestPresses(vendor);
    }
}

// F3 on a non-AMD GPU reaches the same decisions as AMD's default.
void TestNonAmdTurnedOn() {
    Switch amd, nvidia;
    amd.Init(0x1002);
    nvidia.Init(kNvidia);
    CHECK(nvidia.Press(false) && nvidia.On() == amd.On());
    for (const bool flag : {false, true}) {
        CHECK(FollowsStencilLink(nvidia.On(), flag) == FollowsStencilLink(amd.On(), flag));
        CHECK(SharesDepthLayout(nvidia.On(), true, flag) == SharesDepthLayout(amd.On(), true, flag));
    }
    CHECK(!FollowsStencilLink(nvidia.On(), false));
    CHECK(SharesDepthLayout(nvidia.On(), true, true));
}

std::string ReadSource(const char* path) {
    std::ifstream file(std::string(DCP_SOURCE_DIR) + "/" + path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

void TestNoEnvironmentSwitch() {
    const std::string force_name = std::string("DCP_AMD_WATER_FIX") + "_FORCE";
    for (const char* path : {"gpu/shim/dcp_amd_water_fix.h", "gpu/shim/window.cpp",
                             "gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp",
                             "gpu/shadps4/video_core/texture_cache/texture_cache.cpp"}) {
        const std::string text = ReadSource(path);
        CHECK(!text.empty());
        CHECK(text.find(force_name) == std::string::npos);
    }
    const std::string header = ReadSource("gpu/shim/dcp_amd_water_fix.h");
    CHECK(header.find("getenv") == std::string::npos && header.find("Flag(") == std::string::npos);
}

void TestStencilLink() {
    CHECK(FollowsStencilLink(true, true));
    CHECK(!FollowsStencilLink(true, false));
    CHECK(FollowsStencilLink(false, true));
    CHECK(FollowsStencilLink(false, false)); // fix off: the previous behaviour
}

void TestDepthLayout() {
    CHECK(SharesDepthLayout(true, true, true));
    CHECK(!SharesDepthLayout(true, true, false)); // read-only attachment already matches the descriptor
    CHECK(!SharesDepthLayout(true, false, true));
    CHECK(!SharesDepthLayout(false, true, true));
}

} // namespace

int main() {
    TestVendorDefaults();
    TestAllVendors();
    TestNonAmdTurnedOn();
    TestNoEnvironmentSwitch();
    TestStencilLink();
    TestDepthLayout();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("amd water fix tests passed\n");
    return 0;
}
