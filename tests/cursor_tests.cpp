// Cursor auto-hide tests: the idle timer and the visibility changes, with synthetic time. No window, no SDL.
#include <chrono>
#include <cstdio>
#include <optional>

#include "dcp_cursor.h"

namespace {

using Clock = DcpCursor::Hider::Clock;

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

Clock::time_point At(int ms) {
    return Clock::time_point{} + std::chrono::milliseconds(ms);
}

const std::optional<bool> hide = false, show = true;

void TestWindowedStaysVisible() {
    DcpCursor::Hider hider;
    CHECK(!hider.Update(false, At(0)));
    CHECK(!hider.Update(false, At(60000)));
}

void TestFullscreenHidesAfterIdleAndMotionShows() {
    DcpCursor::Hider hider;
    CHECK(!hider.Update(true, At(0)));
    CHECK(!hider.Update(true, At(4999)));
    CHECK(hider.Update(true, At(5000)) == hide);
    CHECK(!hider.Update(true, At(6000)));

    hider.MouseMoved(At(6000));
    CHECK(hider.Update(true, At(6000)) == show);
    hider.MouseMoved(At(8000));
    CHECK(!hider.Update(true, At(12999)));
    CHECK(hider.Update(true, At(13000)) == hide);
}

void TestLeavingFullscreenShowsAndReentryStartsFresh() {
    DcpCursor::Hider hider;
    CHECK(!hider.Update(true, At(0)));
    CHECK(hider.Update(true, At(5000)) == hide);

    CHECK(hider.Update(false, At(5100)) == show);
    CHECK(!hider.Update(true, At(20000)));
    CHECK(!hider.Update(true, At(24999)));
    CHECK(hider.Update(true, At(25000)) == hide);
}

void TestFocusLossShowsAndRegainStartsFresh() {
    DcpCursor::Hider hider;
    CHECK(!hider.Update(true, At(0)));
    CHECK(hider.Update(true, At(5000)) == hide);

    CHECK(hider.Update(false, At(25100)) == show);
    CHECK(!hider.Update(true, At(30000)));
    CHECK(!hider.Update(true, At(34999)));
    CHECK(hider.Update(true, At(35000)) == hide);
}

} // namespace

int main() {
    TestWindowedStaysVisible();
    TestFullscreenHidesAfterIdleAndMotionShows();
    TestLeavingFullscreenShowsAndReentryStartsFresh();
    TestFocusLossShowsAndRegainStartsFresh();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
