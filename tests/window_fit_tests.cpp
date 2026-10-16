// Focused tests of the windowed-mode fit: share of the usable area, one common factor, centring.
#include <cstdio>
#include <cstdlib>

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

void TestSizes() {
    const Area areas[] = {{0, 0, 1920, 1040}, {0, 0, 2560, 1400}, {0, 0, 3840, 2100}};
    const int sources[][2] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    for (const Area& a : areas) {
        for (const auto& s : sources) {
            const dcp::WindowFit f = dcp::FitWindowToArea(s[0], s[1], kBars, a.x, a.y, a.w, a.h);
            CHECK(f.width + 2 <= a.w * 0.8 + 1 && f.height + 32 <= a.h * 0.8 + 1);
            CHECK(std::abs(f.width * 9 - f.height * 16) <= 16); // 16:9 kept within rounding
            CHECK(f.width <= s[0] && f.height <= s[1]);
            CHECK(f.x - 1 >= a.x && f.y - 31 >= a.y);
            CHECK(f.x + f.width + 1 <= a.x + a.w && f.y + f.height + 1 <= a.y + a.h);
        }
    }
    const dcp::WindowFit small = dcp::FitWindowToArea(1280, 720, kBars, 0, 0, 3840, 2100);
    CHECK(small.width == 1280 && small.height == 720);
    const dcp::WindowFit big = dcp::FitWindowToArea(3840, 2160, kBars, 0, 0, 1920, 1040);
    CHECK(big.width < 1920 * 0.8 && big.height < 1040 * 0.8);
}

void TestCentring() {
    const dcp::WindowFit f = dcp::FitWindowToArea(1280, 720, kBars, 0, 0, 1920, 1040);
    const int outer_w = f.width + 2, outer_h = f.height + 32;
    CHECK(f.x - 1 == (1920 - outer_w) / 2);
    CHECK(f.y - 31 == (1040 - outer_h) / 2);

    // A monitor left of and above the primary has negative origin coordinates.
    const dcp::WindowFit n = dcp::FitWindowToArea(1920, 1080, kBars, -2560, -200, 2560, 1400);
    CHECK(n.x - 1 >= -2560 && n.x + n.width + 1 <= 0);
    CHECK(n.y - 31 >= -200 && n.y + n.height + 1 <= 1200);
    CHECK((n.x - 1) - (-2560) == 0 + (2560 - (n.width + 2)) / 2);
}

void TestScaling() {
    // 225% scaling: 3840x2160 desktop reports a 1707x960 logical area; the fit works in whatever unit it is given.
    const dcp::WindowFit logical = dcp::FitWindowToArea(1920, 1080, {31, 1, 1, 1}, 0, 0, 1707, 880);
    CHECK(logical.width + 2 <= 1707 * 0.8 + 1 && logical.height + 32 <= 880 * 0.8 + 1);
    const dcp::WindowFit physical = dcp::FitWindowToArea(1920, 1080, {70, 2, 2, 2}, 0, 0, 3840, 2100);
    CHECK(physical.width + 4 <= 3840 * 0.8 + 1 && physical.height + 72 <= 2100 * 0.8 + 1);
}

void TestConfiguredSizeUntouched() {
    const int configured_w = 1920, configured_h = 1080;
    const dcp::WindowFit f = dcp::FitWindowToArea(configured_w, configured_h, kBars, 0, 0, 1920, 1040);
    CHECK(f.width < configured_w && f.height < configured_h);
    CHECK(configured_w == 1920 && configured_h == 1080);
}

} // namespace

int main() {
    TestSizes();
    TestCentring();
    TestScaling();
    TestConfiguredSizeUntouched();
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
