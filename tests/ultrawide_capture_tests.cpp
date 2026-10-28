// DEBUG_SAULO: focused tests of the F9 ultrawide capture's parts without Vulkan: box sizes, footprints, side areas,
// pass and canvas names, capture folder numbering, the choice of draw pictures and the skipping of absent or
// uncopyable targets.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dcp_ultrawide_capture.h"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        ++g_checks;                                                      \
        if (!(cond)) {                                                   \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);          \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

using namespace UltrawideCapture;

void TestFitBox() {
    CHECK((FitBox({2520, 1080}, kThumbBoxWidth, kThumbBoxHeight) == Size{128, 55}));
    CHECK((FitBox({3840, 1080}, kThumbBoxWidth, kThumbBoxHeight) == Size{128, 36}));
    CHECK((FitBox({1920, 1080}, kThumbBoxWidth, kThumbBoxHeight) == Size{128, 72}));
    CHECK((FitBox({1024, 1024}, kThumbBoxWidth, kThumbBoxHeight) == Size{72, 72}));
    CHECK((FitBox({2520, 1080}, kShotBoxWidth, kShotBoxHeight) == Size{1280, 549}));
    CHECK((FitBox({640, 360}, kShotBoxWidth, kShotBoxHeight) == Size{640, 360}));
    CHECK((FitBox({4000, 1}, kThumbBoxWidth, kThumbBoxHeight) == Size{128, 1}));
    CHECK((FitBox({0, 1080}, kThumbBoxWidth, kThumbBoxHeight) == Size{}));
}

std::vector<uint8_t> Picture(Size size, uint8_t value) {
    std::vector<uint8_t> pixels(size_t(size.width) * size.height * 4, value);
    return pixels;
}

void Paint(std::vector<uint8_t>& pixels, Size size, uint32_t x, uint32_t y, uint8_t value) {
    std::memset(&pixels[(size_t(y) * size.width + x) * 4], value, 4);
}

void TestChangedCells() {
    const Size size{128, 55};
    auto before = Picture(size, 10);
    auto after = before;
    uint64_t cells = 99;
    CHECK(ChangedCells(before.data(), after.data(), size, 6, &cells).Empty() && cells == 0);

    Paint(after, size, 3, 4, 200);
    Paint(after, size, 10, 20, 200);
    const Rect changed = ChangedCells(before.data(), after.data(), size, 6, &cells);
    CHECK(changed.left == 3 && changed.top == 4 && changed.right == 11 && changed.bottom == 21 && cells == 2);

    // Small differences (filtering, dithering) are not changes.
    auto faint = before;
    Paint(faint, size, 50, 30, 15);
    CHECK(ChangedCells(before.data(), faint.data(), size, 6).Empty());

    // Alpha alone counts: a dim may only change it in an intermediate target.
    auto alpha = before;
    alpha[(size_t(7) * size.width + 9) * 4 + 3] = 255;
    const Rect a = ChangedCells(before.data(), alpha.data(), size, 6);
    CHECK(a.left == 9 && a.top == 7 && a.Area() == 1);
}

void TestCellsToPixels() {
    const Size thumb{128, 55}, target{2520, 1080};
    const Rect all = CellsToPixels({0, 0, 128, 55}, thumb, target);
    CHECK(all.left == 0 && all.top == 0 && all.right == 2520 && all.bottom == 1080);
    const Rect left = CellsToPixels({0, 0, 15, 55}, thumb, target);
    CHECK(left.right == 295);
    CHECK(CellsToPixels({}, thumb, target).Empty());
}

void TestSides() {
    const SideAreas wide = SidesOf({2520, 1080});
    CHECK(wide.left.left == 0 && wide.left.right == 300 && wide.right.left == 2220 && wide.right.right == 2520);
    const SideAreas super = SidesOf({3840, 1080});
    CHECK(super.left.right == 960 && super.right.left == 2880);
    CHECK(SidesOf({1920, 1080}).left.Empty() && SidesOf({1920, 1080}).right.Empty());
    CHECK(SidesOf({1440, 1080}).left.Empty());

    // A footprint reaching the left side area, one inside the centre only.
    CHECK(Overlaps({0, 900, 2520, 1080}, wide.left) && Overlaps({0, 900, 2520, 1080}, wide.right));
    CHECK(!Overlaps({300, 0, 2220, 1080}, wide.left) && !Overlaps({300, 0, 2220, 1080}, wide.right));
    CHECK(Overlaps({299, 0, 301, 1}, wide.left));
}

void TestPassNames() {
    CHECK(std::string(PassFamily(0x05)) == "world");
    CHECK(std::string(PassFamily(0x1f)) == "world");
    CHECK(std::string(PassFamily(0x20)) == "screen");
    CHECK(std::string(PassFamily(0x23)) == "world");
    CHECK(std::string(PassFamily(0x26)) == "screen");
    CHECK(std::string(PassFamily(0x28)) == "screen");
    CHECK(std::string(PassFamily(0x29)) == "4:3");
    CHECK(std::string(PassFamily(0x2a)) == "world");
    CHECK(std::string(PassFamily(0x27 + 0x2b)) == "screen");
    CHECK(std::string(PassFamily(-1)) == "?");
    CHECK(LayerText(0x26) == "0x26");
    CHECK(LayerText(0x27 + 0x2b) == "v1:0x27");
    CHECK(LayerText(-1) == "-");
}

void TestCanvas() {
    CHECK((Canvas(2.0f / 812.0f, -2.0f / 456.0f) == Size{812, 456}));
    CHECK((Canvas(2.0f / 1280.0f, 2.0f / 720.0f) == Size{1280, 720}));
    CHECK(!Widened({812, 456}) && !Widened({1280, 720}) && !Widened({960, 720}));
    CHECK(Widened({1066, 456}) && Widened({1680, 720}) && Widened({2560, 720}));
    CHECK((Canvas(0.0f, 1.0f) == Size{}));
}

void TestNames() {
    const uint32_t callers[4] = {0x5812bb, 0x581ac8, 0x4e9b12, 0};
    CHECK(CallersText(callers, 4) == "5812bb<581ac8<4e9b12");
    const uint32_t none[4] = {};
    CHECK(CallersText(none, 4) == "-");
    const uint32_t relocated[4] = {0, 0x579baa, 0x4ec2f1, 0};
    CHECK(CallersText(relocated, 4) == "?<579baa<4ec2f1");
    CHECK(CaptureDirName(1) == "capture-001");
    CHECK(CaptureDirName(42) == "capture-042");
    CHECK(NextCaptureIndex({}) == 1);
    CHECK(NextCaptureIndex({"capture-001", "capture-003", "notes.txt", "capture-x", "capture-002b"}) == 4);
    CHECK(NextCaptureIndex({"capture-999"}) == 1000);
}

void TestPickPictures() {
    std::vector<Footprint> draws(10);
    draws[2].side = true;
    draws[7].side = true;
    draws[4].coverage = 0.5;
    draws[5].coverage = 0.14;
    draws[9].coverage = 0.9;
    const auto all = PickPictures(draws, 0.15, 10);
    CHECK((all == std::vector<size_t>{2, 4, 7, 9}));
    // The side ones come first when the limit is short.
    const auto two = PickPictures(draws, 0.15, 2);
    CHECK((two == std::vector<size_t>{2, 7}));
    const auto three = PickPictures(draws, 0.15, 3);
    CHECK((three == std::vector<size_t>{2, 4, 7}));
}

void TestColourTarget() {
    CHECK(ColourTarget(1, 7, 42).Valid());
    CHECK((ColourTarget(1, 7, 42).slot == 7 && ColourTarget(1, 7, 42).uid == 42));
    CHECK(!ColourTarget(0, 7, 42).Valid());       // pipeline writes no colour target 0
    CHECK(!ColourTarget(2, 7, 42).Valid());       // only target 1
    CHECK(!ColourTarget(1, kNoSlot, 42).Valid()); // nothing bound: the invalid slot id
    CHECK(!ColourTarget(1, 7, 0).Valid());        // no image generation
    CHECK(!TargetRef{}.Valid());
}

void TestPictureSkip() {
    int lookups = 0;
    const auto alive = [&](uint32_t slot, uint64_t uid) {
        ++lookups;
        CHECK(slot != kNoSlot);
        return slot == 7 && uid == 42;
    };
    const TargetRef live{7, 42};
    CHECK(PictureSkip(live, false, 0, 64, alive) == Skip::None);
    CHECK(PictureSkip(TargetRef{}, false, 0, 64, alive) == Skip::NoTarget);
    CHECK(PictureSkip(TargetRef{kNoSlot, 42}, false, 0, 64, alive) == Skip::NoTarget);
    CHECK(PictureSkip(live, true, 0, 64, alive) == Skip::Proxy);
    CHECK(PictureSkip(TargetRef{7, 43}, false, 0, 64, alive) == Skip::Gone); // slot reused by another image
    CHECK(PictureSkip(TargetRef{8, 42}, false, 0, 64, alive) == Skip::Gone); // image destroyed
    CHECK(PictureSkip(live, false, 64, 64, alive) == Skip::Limit);
    CHECK(lookups == 3); // never asked about a missing target, a proxy or past the limit
}

void TestSkipCounts() {
    SkipCounts counts;
    CHECK(SkipCountsText(counts) == "none");
    counts.Count(Skip::None);
    CHECK(counts.Total() == 0);
    counts.Count(Skip::NoTarget);
    counts.Count(Skip::Refused);
    counts.Count(Skip::Refused);
    CHECK(counts.Total() == 3);
    CHECK(SkipCountsText(counts) == "1 no colour target, 2 format or layout not copyable");
}

void TestSnapshotPossible() {
    SnapshotSource s;
    s.backing = true;
    s.levels = 1;
    s.layers = 1;
    s.layout_known = true;
    s.blit_source = true;
    s.rgba8 = true;
    CHECK(SnapshotPossible(s, false) && SnapshotPossible(s, true));
    auto changed = [&](auto&& edit) {
        SnapshotSource t = s;
        edit(t);
        return t;
    };
    CHECK(!SnapshotPossible(changed([](SnapshotSource& t) { t.backing = false; }), false));
    CHECK(!SnapshotPossible(changed([](SnapshotSource& t) { t.depth = true; }), false));
    CHECK(!SnapshotPossible(changed([](SnapshotSource& t) { t.multisample = true; }), false));
    CHECK(!SnapshotPossible(changed([](SnapshotSource& t) { t.integer = true; }), false));
    CHECK(!SnapshotPossible(changed([](SnapshotSource& t) { t.layout_known = false; }), false));
    CHECK(!SnapshotPossible(changed([](SnapshotSource& t) { t.mip = 1; }), false));
    CHECK(!SnapshotPossible(changed([](SnapshotSource& t) { t.slice = 0xffffffffu; }), false));
    CHECK(!SnapshotPossible(changed([](SnapshotSource& t) { t.levels = 0; }), false));
    // Unsupported formats: no blit source for a scaled copy, not RGBA8/BGRA8 for a direct one.
    CHECK(!SnapshotPossible(changed([](SnapshotSource& t) { t.blit_source = false; }), false));
    CHECK(SnapshotPossible(changed([](SnapshotSource& t) { t.blit_source = false; }), true));
    CHECK(!SnapshotPossible(changed([](SnapshotSource& t) { t.rgba8 = false; }), true));
}

void TestAddressText() {
    CHECK(AddressText(0) == "-"); // no colour or depth target
    CHECK(AddressText(0x108e540000ull) == "0x108e540000");
}

} // namespace

int main() {
    TestFitBox();
    TestChangedCells();
    TestCellsToPixels();
    TestSides();
    TestPassNames();
    TestCanvas();
    TestNames();
    TestPickPictures();
    TestColourTarget();
    TestPictureSkip();
    TestSkipCounts();
    TestSnapshotPossible();
    TestAddressText();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
