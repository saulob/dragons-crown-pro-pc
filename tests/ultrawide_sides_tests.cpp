// Focused tests of the True Ultrawide side areas the final presenter blacks out during a fade.
#include <cstdio>

#include "dcp_ultrawide_sides.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);          \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

using namespace DcpUltrawide;

void TestSides() {
    const SideAreas wide = SidesOf({2520, 1080});
    CHECK(wide.left.left == 0 && wide.left.right == 300 && wide.right.left == 2220 && wide.right.right == 2520);
    CHECK(wide.left.top == 0 && wide.left.bottom == 1080 && wide.right.bottom == 1080);
    const SideAreas super = SidesOf({3840, 1080});
    CHECK(super.left.right == 960 && super.right.left == 2880);
    CHECK(SidesOf({1920, 1080}).left.Empty() && SidesOf({1920, 1080}).right.Empty());
    CHECK(SidesOf({1440, 1080}).left.Empty());
    CHECK(SidesOf({1921, 1080}).left.Empty()); // one pixel of rounding slack is not a side area
    CHECK(SidesOf({}).left.Empty());
}

} // namespace

int main() {
    TestSides();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ultrawide sides tests passed\n");
    return 0;
}
