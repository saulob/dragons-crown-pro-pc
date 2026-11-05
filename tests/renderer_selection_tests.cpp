// Renderer selection tests: the Renderer setting, Auto's fallback and the OpenGL requirements. Pure logic plus the
// sources that must agree with it: no window, no OpenGL or Vulkan context, no game files.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "dcp_renderer.h"
#include "video_core/renderer_opengl/gl_capabilities.h"

namespace {

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

std::string ReadAll(const std::string& relative) {
    std::ifstream file(std::string(DCP_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

void TestParse() {
    using namespace DcpRenderer;
    CHECK(ParseChoice("vulkan") == Vulkan);
    CHECK(ParseChoice("auto") == Auto);
    CHECK(ParseChoice("opengl") == DcpRenderer::OpenGL);
    CHECK(ParseChoice("") == Vulkan);
    CHECK(ParseChoice("OpenGL") == Vulkan);
    CHECK(ParseChoice("direct3d") == Vulkan);
}

void TestResolve() {
    using namespace DcpRenderer;
    CHECK(Resolve(Vulkan, true, "").backend == Backend::Vulkan);
    CHECK(Resolve(DcpRenderer::OpenGL, true, "").backend == Backend::OpenGL);
    CHECK(Resolve(DcpRenderer::OpenGL, true, "").fallback_reason.empty());
    CHECK(Resolve(Auto, true, "").backend == Backend::Vulkan);
    CHECK(Resolve(Auto, true, "").fallback_reason.empty());

    const auto fallback = Resolve(Auto, false, "No Vulkan graphics driver was found.");
    CHECK(fallback.backend == Backend::OpenGL);
    CHECK(fallback.fallback_reason == "No Vulkan graphics driver was found.");

    // A renderer chosen by name is never replaced, working or not.
    const auto explicit_vulkan = Resolve(Vulkan, false, "No Vulkan graphics driver was found.");
    CHECK(explicit_vulkan.backend == Backend::Vulkan);
    CHECK(explicit_vulkan.fallback_reason.empty());
    CHECK(Resolve(DcpRenderer::OpenGL, false, "anything").backend == Backend::OpenGL);
    CHECK(Resolve(DcpRenderer::OpenGL, false, "anything").fallback_reason.empty());
}

void TestLauncherAgrees() {
    const std::string settings = ReadAll("launcher/settings.h");
    for (const auto key : DcpRenderer::ChoiceKeys) {
        CHECK(settings.find("{\"" + std::string(key) + "\", ") != std::string::npos);
    }
    CHECK(settings.find("kRendererKey[] = \"renderer\"") != std::string::npos);
    CHECK(settings.find("kDefaultRenderer[] = \"vulkan\"") != std::string::npos);
    CHECK(settings.find("\"OpenGL (experimental)\"") != std::string::npos);

    const std::string shim = ReadAll("gpu/shim/dcp_settings.cpp");
    CHECK(shim.find("key == \"renderer\"") != std::string::npos);

    const std::string launcher = ReadAll("launcher/launcher.cpp");
    const size_t renderer = launcher.find("ui.Combo(\"Renderer\", kRendererHelp, kRendererKey, kDefaultRenderer");
    const size_t card = launcher.find("ui.Row(\"Graphics card\"");
    CHECK(renderer != std::string::npos && card != std::string::npos && renderer < card);

    const std::string portuguese = ReadAll("launcher/localization/pt_br_ui_text.cpp");
    CHECK(portuguese.find("Renderer\tRenderizador") != std::string::npos);
}

// The game code stays out of renderer choices: only the factory and the window know both.
void TestBoundary() {
    const std::string liverpool = ReadAll("gpu/shadps4/video_core/amdgpu/liverpool.cpp");
    CHECK(liverpool.find("Vulkan::Rasterizer") == std::string::npos);
    CHECK(liverpool.find("OpenGL") == std::string::npos);
    const std::string driver = ReadAll("gpu/shadps4/core/libraries/videoout/driver.cpp");
    CHECK(driver.find("Vulkan::Presenter") == std::string::npos);
    CHECK(driver.find("OpenGL") == std::string::npos);
    const std::string factory = ReadAll("gpu/shadps4/video_core/renderer_factory.cpp");
    CHECK(factory.find("make_unique<Vulkan::Presenter>") != std::string::npos);
    CHECK(factory.find("make_unique<OpenGL::Presenter>") != std::string::npos);
}

void TestRequirements() {
    OpenGL::Capabilities caps;
    caps.major = 3;
    caps.minor = 3;
    caps.s3tc = true;
    caps.max_texture_buffer_texels = OpenGL::MinimumRingTexels;
    CHECK(OpenGL::MissingRequirement(caps).empty());

    auto old_context = caps;
    old_context.minor = 2;
    CHECK(OpenGL::MissingRequirement(old_context).find("3.3") != std::string::npos);
    auto no_s3tc = caps;
    no_s3tc.s3tc = false;
    CHECK(OpenGL::MissingRequirement(no_s3tc).find("s3tc") != std::string::npos);
    auto small_buffers = caps;
    small_buffers.max_texture_buffer_texels = 65536;
    CHECK(OpenGL::MissingRequirement(small_buffers).find("texture buffer") != std::string::npos);

    auto newer = caps;
    newer.major = 4;
    newer.minor = 0;
    CHECK(OpenGL::MissingRequirement(newer).empty());
}

} // namespace

int main() {
    TestParse();
    TestResolve();
    TestLauncherAgrees();
    TestBoundary();
    TestRequirements();
    std::printf("renderer selection tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
