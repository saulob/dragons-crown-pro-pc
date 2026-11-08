// Focused tests of the windowed-mode fit: fits are kept, oversized windows shrink by one factor, centring.
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
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
