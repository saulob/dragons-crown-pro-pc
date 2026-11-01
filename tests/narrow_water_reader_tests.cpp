#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#include "dcp_narrow_water_reader.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                             \
    do {                                                        \
        if (!(cond)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond); \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

using namespace DcpNarrowWaterReader;

Draw Reader(int width, int height) {
    Draw d;
    d.ps = kPixelShader;
    d.vs = kVertexShader;
    d.format = kSceneFormat;
    d.width = width;
    d.height = height;
    d.partial = true;
    d.depth = true;
    return d;
}

std::string Source(const char* relative) {
    std::ifstream file(std::string(DCP_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

bool Contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

void TestNarrowerModesSkip() {
    for (const auto& size : {std::pair{1728, 1080}, std::pair{1920, 1200}, std::pair{2560, 1600}, std::pair{1440, 1080},
                             std::pair{1280, 800}, std::pair{3456, 2160}}) {
        CHECK(ShouldSkip(Reader(size.first, size.second)));
    }
}

void TestSixteenNineAndWiderAreUntouched() {
    for (const auto& size : {std::pair{1920, 1080}, std::pair{2560, 1440}, std::pair{3840, 2160}, std::pair{1280, 720},
                             std::pair{2520, 1080}, std::pair{2560, 1080}, std::pair{3440, 1440}, std::pair{3840, 1080},
                             std::pair{5120, 1440}}) {
        CHECK(!ShouldSkip(Reader(size.first, size.second)));
    }
}

void TestOnlyTheRecoveredSignatureMatches() {
    Draw d = Reader(1728, 1080);
    d.ps = 0xda1a1e68;
    CHECK(!ShouldSkip(d));
    d = Reader(1728, 1080);
    d.vs = 0xee0a8dd7;
    CHECK(!ShouldSkip(d));
    d = Reader(1728, 1080);
    d.format = 44;
    CHECK(!ShouldSkip(d));
    d = Reader(1728, 1080);
    d.partial = false;
    CHECK(!ShouldSkip(d));
    d = Reader(1728, 1080);
    d.depth = false;
    CHECK(!ShouldSkip(d));
    d = Reader(1728, 1080);
    d.sampled = 1;
    CHECK(!ShouldSkip(d));
    CHECK(!ShouldSkip(Reader(800, 600)));
    Draw sprite = Reader(1728, 1080);
    sprite.vs = 0xee0a8dd7;
    sprite.sampled = 1;
    sprite.depth = false;
    CHECK(!ShouldSkip(sprite));
    Draw other_water = Reader(1728, 1080);
    other_water.vs = 0x2b835df9;
    CHECK(!ShouldSkip(other_water));
}

void TestNothingElseDecidesTheMatch() {
    const Draw reference = Reader(1728, 1080);
    CHECK(sizeof(reference.ps) == 8);
    Draw other = Reader(1920, 1200);
    CHECK(ShouldSkip(other));
    other = Reader(2560, 1600);
    CHECK(ShouldSkip(other));
}

void TestDebugInfrastructureIsGone() {
    const std::string rasterizer = Source("gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp");
    const std::string window = Source("gpu/shim/window.cpp");
    const std::string overlay = Source("gpu/shim/dcp_overlay.cpp");
    const std::string saves = Source("src/runtime_savedata.c");
    const std::string pad = Source("src/runtime_pad.c");
    const std::string loader = Source("src/loader.c");
    CHECK(Contains(rasterizer, "DcpNarrowWaterReader::ShouldSkip"));
    for (const std::string* text : {&rasterizer, &window, &overlay, &saves, &pad, &loader}) {
        CHECK(!Contains(*text, "DCP_DEBUG_SAULO_ISSUE32"));
        CHECK(!Contains(*text, "Issue32"));
        CHECK(!Contains(*text, "dcp_issue32"));
        CHECK(!Contains(*text, "issue32_scene"));
    }
    CHECK(!Contains(window, "SDL_SCANCODE_F4") && !Contains(window, "SDL_SCANCODE_F5") &&
          !Contains(window, "SDL_SCANCODE_F8") && !Contains(window, "SDL_SCANCODE_F9"));
    CHECK(!Contains(pad, "debug_key_handler"));
    CHECK(!Contains(saves, "debug_save_protected"));
    CHECK(!Contains(Source("CMakeLists.txt"), "issue32"));
    CHECK(!Contains(Source("tools/test.ps1"), "issue32") && !Contains(Source("tools/build.sh"), "issue32"));
}

} // namespace

int main() {
    TestNarrowerModesSkip();
    TestSixteenNineAndWiderAreUntouched();
    TestOnlyTheRecoveredSignatureMatches();
    TestNothingElseDecidesTheMatch();
    TestDebugInfrastructureIsGone();
    std::printf("narrow water reader tests: %d failures\n", g_failures);
    return g_failures ? 1 : 0;
}
