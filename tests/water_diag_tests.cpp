// Focused tests of the issue #32 diagnostics: AMD-only activation, classification, deduplication and the cap.
#include <cstdio>
#include <string>
#include <vector>

#include "dcp_water_diag.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);          \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

using namespace DcpWaterDiag;

constexpr std::uint32_t kAmd = 0x1002;
constexpr std::uint32_t kNvidia = 0x10de;
constexpr std::uint32_t kIntel = 0x8086;
constexpr std::uint32_t kGeneral = 1;
constexpr std::uint32_t kColorAttachment = 2;
constexpr std::uint32_t kDepthStencilReadOnly = 4;
constexpr std::uint32_t kShaderReadOnly = 5;
constexpr std::uint32_t kDepthReadOnly = 1000241001;
constexpr std::uint32_t kFeedbackLoop = 1000339000;

struct Lines {
    std::vector<std::string> lines;
    void operator()(const char* line) {
        lines.emplace_back(line);
    }
};

Target ColorTarget(std::uint32_t id, std::uint64_t begin, bool sampled, std::uint32_t layout) {
    Target target;
    target.id = id;
    target.begin = begin;
    target.end = begin + 0x800000;
    target.format = 37;
    target.width = 1728;
    target.height = 1080;
    target.layout = layout;
    target.sampled = sampled;
    target.sample_layout = sampled ? layout : 0;
    return target;
}

Target DepthTarget(std::uint32_t id, bool sampled, std::uint32_t layout, std::uint32_t sample_layout) {
    Target target = ColorTarget(id, 0x40000000, sampled, layout);
    target.slot = kDepthSlot;
    target.format = 126;
    target.sample_layout = sample_layout;
    return target;
}

Image Texture(std::uint32_t id, std::uint64_t begin, std::uint64_t size) {
    Image image;
    image.id = id;
    image.begin = begin;
    image.end = begin + size;
    image.format = 37;
    image.width = 864;
    image.height = 540;
    image.layout = kShaderReadOnly;
    return image;
}

Draw AmdDraw(std::uint64_t ps, bool feedback_layout = false) {
    Draw draw;
    draw.vs = 0x1111;
    draw.ps = ps;
    draw.feedback_layout = feedback_layout;
    draw.old_fix = true;
    return draw;
}

Event ColorFeedback(std::uint64_t ps) {
    const Target targets[] = {ColorTarget(42, 0x10000000, true, kGeneral)};
    return *Classify(AmdDraw(ps), targets, {});
}

void Feed(Monitor& monitor, const Event& event, std::uint32_t frame, Lines& sink) {
    monitor.Report(event, frame, monitor.NextDraw(frame), sink);
}

bool Has(const std::string& line, const char* text) {
    return line.find(text) != std::string::npos;
}

void TestOtherVendorsReportNothing() {
    for (const std::uint32_t vendor : {kNvidia, kIntel, 0u}) {
        Monitor monitor;
        monitor.Init(vendor);
        CHECK(!monitor.Active());
        Lines sink;
        for (std::uint32_t i = 0; i < 3 * kMaxEvents; ++i) {
            Feed(monitor, ColorFeedback(i), i, sink);
        }
        CHECK(sink.lines.empty());
    }
}

void TestOrdinaryDraws() {
    const Target targets[] = {ColorTarget(1, 0x10000000, false, kColorAttachment),
                              DepthTarget(2, false, kDepthReadOnly, 0)};
    const Image textures[] = {Texture(3, 0x20000000, 0x100000), Texture(4, 0x30000000, 0x100000)};
    CHECK(!Classify(AmdDraw(7), targets, textures));
    CHECK(!Classify(AmdDraw(7), targets, {}));
    // Read-only depth sampled through a matching descriptor layout.
    const Target depth[] = {DepthTarget(2, true, kDepthStencilReadOnly, kDepthStencilReadOnly)};
    CHECK(!Classify(AmdDraw(7), depth, {}));
    // Adjacent ranges do not overlap.
    const Image adjacent[] = {Texture(5, 0x10800000, 0x1000)};
    CHECK(!Classify(AmdDraw(7), targets, adjacent));
}

void TestColorFeedbackGeneral() {
    Monitor monitor;
    monitor.Init(kAmd);
    Lines sink;
    const Event event = ColorFeedback(0x8a31c4);
    CHECK(event.reason == Reason::ColorFeedback && event.sample.id == 42);
    Feed(monitor, event, 1842, sink);
    CHECK(sink.lines.size() == 1);
    const std::string& line = sink.lines[0];
    CHECK(line.rfind("WaterDiag: f=1842 d=0 reason=color-feedback ", 0) == 0);
    CHECK(Has(line, " ps=00000000008a31c4 ") && Has(line, " rt=42 ") && Has(line, " sample=42 "));
    CHECK(Has(line, " extent=1728x1080 ") && Has(line, " overlap=self "));
    CHECK(Has(line, " layout=General slayout=General feedback=0 "));
    CHECK(Has(line, " oldfix=1") && line.size() < 512);
}

void TestColorFeedbackLayoutSupported() {
    const Target targets[] = {ColorTarget(42, 0x10000000, true, kFeedbackLoop)};
    const auto event = Classify(AmdDraw(9, true), targets, {});
    CHECK(event && event->reason == Reason::ColorFeedback);
    char line[512];
    Format(*event, 1, 2, line, sizeof(line));
    CHECK(Has(line, " layout=FeedbackLoop slayout=FeedbackLoop feedback=1 "));
    CHECK(Signature(*event) != Signature(ColorFeedback(9)));
}

void TestDepthFeedback() {
    Draw writes = AmdDraw(5);
    writes.depth_writes = true;
    const Target shared[] = {DepthTarget(2, true, kGeneral, kGeneral)};
    const auto written = Classify(writes, shared, {});
    CHECK(written && written->reason == Reason::DepthFeedback);
    char line[512];
    Format(*written, 3, 4, line, sizeof(line));
    CHECK(Has(line, " reason=depth-feedback ") && Has(line, " slot=8 ") && Has(line, " dwrite=1 "));
    // Read-only attachment whose layout moved away from the descriptors'.
    const Target moved[] = {DepthTarget(2, true, kDepthReadOnly, kDepthStencilReadOnly)};
    const auto mismatch = Classify(AmdDraw(5), moved, {});
    CHECK(mismatch && mismatch->reason == Reason::DepthFeedback);
    Format(*mismatch, 3, 4, line, sizeof(line));
    CHECK(Has(line, " layout=DepthReadOnly slayout=DepthStencilReadOnly ") && Has(line, " dwrite=0 "));
}

void TestImageAlias() {
    const Target targets[] = {ColorTarget(42, 0x12340000, false, kColorAttachment)};
    const Image textures[] = {Texture(3, 0x20000000, 0x1000), Texture(57, 0x12340000 + 0x1000, 0x10000)};
    const auto event = Classify(AmdDraw(6), targets, textures);
    CHECK(event && event->reason == Reason::ImageAlias && event->sample.id == 57);
    char line[512];
    Format(*event, 10, 11, line, sizeof(line));
    CHECK(Has(line, " reason=image-alias "));
    CHECK(Has(line, " rt=42 ") && Has(line, " sample=57 ") && Has(line, " sguest=0x12341000 "));
    CHECK(Has(line, " overlap=inside ") && Has(line, " layout=ColorAttachment slayout=ShaderReadOnly "));

    Image base = Texture(58, 0x12340000, 0x800000);
    base.aliased = true;
    const Image same_base[] = {base};
    const auto aliased = Classify(AmdDraw(6), targets, same_base);
    CHECK(aliased);
    Format(*aliased, 10, 11, line, sizeof(line));
    CHECK(Has(line, " overlap=same-base aliased=1 "));
    CHECK(Signature(*aliased) != Signature(*event));
}

void TestFeedbackBeforeAlias() {
    const Target targets[] = {ColorTarget(42, 0x10000000, true, kGeneral)};
    const Image textures[] = {Texture(57, 0x10000000, 0x1000)};
    const auto event = Classify(AmdDraw(6), targets, textures);
    CHECK(event && event->reason == Reason::ColorFeedback);
}

void TestDeduplication() {
    Monitor monitor;
    monitor.Init(kAmd);
    Lines sink;
    for (std::uint32_t frame = 0; frame < 500; ++frame) {
        Feed(monitor, ColorFeedback(0x8a31c4), frame, sink);
    }
    CHECK(sink.lines.size() == 1 && monitor.Count() == 1);
    // Another buffer of the same pass: other id and address, same relationship.
    const Target other[] = {ColorTarget(43, 0x18000000, true, kGeneral)};
    Feed(monitor, *Classify(AmdDraw(0x8a31c4), other, {}), 501, sink);
    CHECK(sink.lines.size() == 1);
    // Another shader, or another layout, is a new event.
    Feed(monitor, ColorFeedback(0x8a31c5), 502, sink);
    const Target loop[] = {ColorTarget(42, 0x10000000, true, kFeedbackLoop)};
    Feed(monitor, *Classify(AmdDraw(0x8a31c4, true), loop, {}), 503, sink);
    CHECK(sink.lines.size() == 3);
}

void TestCap() {
    Monitor monitor;
    monitor.Init(kAmd);
    Lines sink;
    for (std::uint32_t i = 0; i < kMaxEvents; ++i) {
        Feed(monitor, ColorFeedback(0x1000 + i), i, sink);
    }
    CHECK(sink.lines.size() == kMaxEvents && monitor.Active());
    // A repeat of a logged event at the cap is not a new one.
    Feed(monitor, ColorFeedback(0x1000), 100, sink);
    CHECK(sink.lines.size() == kMaxEvents && monitor.Active());
    for (std::uint32_t frame = 200; frame < 10200; ++frame) {
        Feed(monitor, ColorFeedback(0x9000 + frame), frame, sink);
    }
    CHECK(sink.lines.size() == kMaxEvents + 1);
    CHECK(sink.lines.back() == "WaterDiag: limit reached, further events suppressed");
    CHECK(!monitor.Active());
    std::uint32_t suppressed = 0;
    for (const auto& line : sink.lines) {
        suppressed += Has(line, "limit reached");
    }
    CHECK(suppressed == 1);
}

void TestDrawNumbering() {
    Monitor monitor;
    monitor.Init(kAmd);
    CHECK(monitor.NextDraw(5) == 0 && monitor.NextDraw(5) == 1 && monitor.NextDraw(5) == 2);
    CHECK(monitor.NextDraw(6) == 0 && monitor.NextDraw(6) == 1);
}

void TestOldFixState() {
    Draw off = AmdDraw(0x77);
    off.old_fix = false;
    const Target targets[] = {ColorTarget(42, 0x10000000, true, kGeneral)};
    const auto event_off = Classify(off, targets, {});
    const auto event_on = Classify(AmdDraw(0x77), targets, {});
    char line[512];
    Format(*event_off, 1, 1, line, sizeof(line));
    CHECK(Has(line, " oldfix=0"));
    Format(*event_on, 1, 1, line, sizeof(line));
    CHECK(Has(line, " oldfix=1"));
    CHECK(Signature(*event_off) != Signature(*event_on));
}

} // namespace

int main() {
    TestOtherVendorsReportNothing();
    TestOrdinaryDraws();
    TestColorFeedbackGeneral();
    TestColorFeedbackLayoutSupported();
    TestDepthFeedback();
    TestImageAlias();
    TestFeedbackBeforeAlias();
    TestDeduplication();
    TestCap();
    TestDrawNumbering();
    TestOldFixState();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("water diag tests passed\n");
    return 0;
}
