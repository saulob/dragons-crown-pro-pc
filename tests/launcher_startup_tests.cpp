// The launcher starts without Vulkan. Pure logic on scripted systems: no window, no SDL, no Vulkan and no game.
// The same file is compiled by tools/test-linux-launcher.sh, so the Linux renderer order is checked here too.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "dcp_vulkan_probe.h"
#include "graphics_cards.h"
#include "launcher_renderer.h"

namespace fs = std::filesystem;
using namespace dcp;
namespace probe = DcpVulkanProbe;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        ++g_checks;                                                                        \
        if (!(condition)) {                                                                \
            ++g_failures;                                                                  \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);             \
        }                                                                                  \
    } while (0)

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// ---- launcher renderer ----

struct ScriptedRenderers {
    std::vector<std::string> events;
    std::vector<std::string> failing;
    RendererStage stage = RendererStage::kCreate;

    std::optional<RendererFailure> Open(std::string_view name) {
        events.push_back("open:" + std::string(name));
        for (const std::string& bad : failing) {
            if (bad == name) {
                return RendererFailure{stage, "scripted failure of " + AttemptLabel(name)};
            }
        }
        return std::nullopt;
    }
    void Discard() { events.push_back("discard"); }

    RendererSelection Select(std::string_view requested, bool windows) {
        return SelectLauncherRenderer(requested, windows, [this](std::string_view name) { return Open(name); },
                                      [this] { Discard(); });
    }
};

void TestRendererValues() {
    CHECK(ParseLauncherRenderer("auto", true) && ParseLauncherRenderer("auto", false));
    CHECK(ParseLauncherRenderer("software", true) && ParseLauncherRenderer("software", false));
    CHECK(ParseLauncherRenderer("direct3d11", true));
    CHECK(!ParseLauncherRenderer("direct3d11", false));
    for (const char* invalid : {"", "Software", "AUTO", "d3d11", "vulkan", "opengl", "software ", "--launcher"}) {
        CHECK(!ParseLauncherRenderer(invalid, true));
        CHECK(!ParseLauncherRenderer(invalid, false));
    }
}

void TestRendererOrder() {
    const auto windows = LauncherRendererOrder(true);
    CHECK(windows.size() == 2 && windows[0] == "direct3d11" && windows[1] == "software");
    const auto linux_order = LauncherRendererOrder(false);
    CHECK(linux_order.size() == 2 && linux_order[0].empty() && linux_order[1] == "software");
    CHECK(LauncherRendererOrder(true, "auto") == windows && LauncherRendererOrder(false, "auto") == linux_order);

    const auto software = LauncherRendererOrder(true, "software");
    CHECK(software.size() == 1 && software[0] == "software");
    CHECK(LauncherRendererOrder(false, "software") == software);
    const auto direct3d11 = LauncherRendererOrder(true, "direct3d11");
    CHECK(direct3d11.size() == 1 && direct3d11[0] == "direct3d11");
    CHECK(LauncherRendererOrder(false, "direct3d11") == linux_order);

    // The launcher never draws through Vulkan or OpenGL, by default or by request.
    for (const bool is_windows : {true, false}) {
        for (const char* request : {"auto", "software", "direct3d11", "vulkan", "opengl", ""}) {
            for (const std::string_view name : LauncherRendererOrder(is_windows, request)) {
                CHECK(name.empty() || name == "software" || name == "direct3d11");
            }
        }
    }
    CHECK(RendererModulesToPin("direct3d11").size() == 2 && RendererModulesToPin("software").empty());
}

void TestRendererSelection() {
    {
        ScriptedRenderers system;
        const RendererSelection selection = system.Select("auto", true);
        CHECK(selection.opened && selection.effective == "direct3d11" && !selection.fallback);
        CHECK(selection.attempts.size() == 1 && !selection.attempts[0].failure);
        CHECK(system.events == std::vector<std::string>{"open:direct3d11"});
        CHECK(Contains(RendererResultLine(selection), "effective=direct3d11 fallback=0"));
    }
    {
        ScriptedRenderers system;
        system.failing = {"direct3d11"};
        const RendererSelection selection = system.Select("auto", true);
        CHECK(selection.opened && selection.effective == "software" && selection.fallback);
        CHECK(selection.attempts.size() == 2 && selection.attempts[0].failure && !selection.attempts[1].failure);
        CHECK((system.events == std::vector<std::string>{"open:direct3d11", "discard", "open:software"}));
        const std::string line = RendererResultLine(selection);
        CHECK(Contains(line, "effective=software fallback=1 fallback_reason=direct3d11_create"));
        CHECK(Contains(RendererAttemptLine(selection.attempts[0]), "attempt=direct3d11 result=failed stage=create"));
        CHECK(Contains(RendererAttemptLine(selection.attempts[1]), "attempt=software result=ok"));
    }
    for (const RendererStage stage : {RendererStage::kPin, RendererStage::kProbe, RendererStage::kBackend}) {
        // A failure after the renderer exists (first frame, ImGui backend) is cleaned up before software is tried.
        ScriptedRenderers system;
        system.failing = {"direct3d11"};
        system.stage = stage;
        const RendererSelection selection = system.Select("auto", true);
        CHECK(selection.opened && selection.effective == "software" && selection.fallback);
        CHECK((system.events == std::vector<std::string>{"open:direct3d11", "discard", "open:software"}));
        CHECK(Contains(RendererResultLine(selection), std::string("direct3d11_") + RendererStageName(stage)));
    }
    {
        ScriptedRenderers system;
        system.failing = {"direct3d11", "software"};
        const RendererSelection selection = system.Select("auto", true);
        CHECK(!selection.opened && selection.effective.empty());
        CHECK(selection.attempts.size() == 2);
        CHECK((system.events == std::vector<std::string>{"open:direct3d11", "discard", "open:software", "discard"}));
        CHECK(RendererResultLine(selection) == "Launcher: startup: renderer effective=none");
        const std::string text = RendererFailureText(selection);
        CHECK(Contains(text, "direct3d11: scripted failure") && Contains(text, "software: scripted failure"));
    }
    {
        ScriptedRenderers system;
        const RendererSelection selection = system.Select("software", true);
        CHECK(selection.opened && selection.effective == "software" && !selection.fallback && selection.requested == "software");
        CHECK(system.events == std::vector<std::string>{"open:software"});
        CHECK(Contains(RendererRequestLine("software", true), "requested=software order=software"));
    }
    {
        ScriptedRenderers system;
        system.failing = {"software"};
        const RendererSelection selection = system.Select("software", true);
        CHECK(!selection.opened && selection.attempts.size() == 1);
    }
    {
        // Asking for Direct3D 11 by name is a diagnostic: it does not fall back.
        ScriptedRenderers system;
        system.failing = {"direct3d11"};
        const RendererSelection selection = system.Select("direct3d11", true);
        CHECK(!selection.opened && selection.attempts.size() == 1);
        CHECK((system.events == std::vector<std::string>{"open:direct3d11", "discard"}));
    }
    {
        // Linux: SDL's own choice first, then software.
        ScriptedRenderers system;
        const RendererSelection selection = system.Select("auto", false);
        CHECK(selection.opened && selection.effective == "accelerated" && !selection.fallback);
        CHECK(system.events == std::vector<std::string>{"open:"});
        CHECK(Contains(RendererRequestLine("auto", false), "order=accelerated,software"));
    }
    {
        ScriptedRenderers system;
        system.failing = {""};
        const RendererSelection selection = system.Select("auto", false);
        CHECK(selection.opened && selection.effective == "software" && selection.fallback);
        CHECK((system.events == std::vector<std::string>{"open:", "discard", "open:software"}));
        CHECK(Contains(RendererResultLine(selection), "fallback_reason=accelerated_create"));
    }
    {
        ScriptedRenderers system;
        system.failing = {"", "software"};
        const RendererSelection selection = system.Select("auto", false);
        CHECK(!selection.opened && system.events.size() == 4);
    }
    CHECK(Contains(StartupFailureLine(StartupFailure::kSdlVideo, "x"), "kind=sdl_video"));
    CHECK(Contains(StartupFailureLine(StartupFailure::kNoDisplay, "x"), "kind=no_display"));
    CHECK(Contains(StartupFailureLine(StartupFailure::kWindow, "x"), "kind=window"));
    CHECK(Contains(StartupFailureLine(StartupFailure::kRenderer, "x"), "kind=renderer"));
}

// ---- Vulkan probe ----

struct ScriptedVulkan final : probe::Source {
    bool loader = true;
    bool entry_points = true;
    bool version_ok = true;
    uint32_t version = probe::MakeVersion(1, 3);
    probe::InstanceOutcome instance = probe::InstanceOutcome::Created;
    bool enumerates = true;
    std::vector<probe::Device> devices;
    int code = 0;
    std::vector<std::string> calls;

    bool OpenLoader() override { calls.push_back("loader"); return loader; }
    bool LoadEntryPoints() override { calls.push_back("entry"); return entry_points; }
    bool QueryInstanceVersion(uint32_t& out, int& result) override {
        calls.push_back("version");
        out = version;
        result = version_ok ? 0 : code;
        return version_ok;
    }
    probe::InstanceOutcome CreateInstance(int& result) override {
        calls.push_back("instance");
        result = instance == probe::InstanceOutcome::Created ? 0 : code;
        return instance;
    }
    bool EnumerateDevices(std::vector<probe::Device>& out, int& result) override {
        calls.push_back("devices");
        result = enumerates ? 0 : code;
        if (enumerates) {
            out = devices;
        }
        return enumerates;
    }
};

constexpr uint32_t kRequired = probe::MakeVersion(1, 3);

void TestVulkanProbe() {
    {
        ScriptedVulkan vulkan;
        vulkan.devices = {{"AMD Radeon RX 580", true}};
        const probe::Report report = probe::Run(vulkan, kRequired);
        CHECK(report.available() && report.usable_count() == 1);
        CHECK(probe::UsableNames(report, 16) == std::vector<std::string>{"AMD Radeon RX 580"});
        CHECK(probe::Describe(report, kRequired).cause.empty());
    }
    {
        ScriptedVulkan vulkan;
        vulkan.loader = false;
        const probe::Report report = probe::Run(vulkan, kRequired);
        CHECK(report.status == probe::Status::LoaderMissing && report.devices.empty());
        CHECK(vulkan.calls == std::vector<std::string>{"loader"});
        CHECK(Contains(probe::Describe(report, kRequired).cause, "not installed"));
    }
    {
        ScriptedVulkan vulkan;
        vulkan.entry_points = false;
        const probe::Report report = probe::Run(vulkan, kRequired);
        CHECK(report.status == probe::Status::EntryPointMissing);
        CHECK((vulkan.calls == std::vector<std::string>{"loader", "entry"}));
    }
    {
        // vkEnumerateInstanceVersion missing: the source answers 1.0.
        ScriptedVulkan vulkan;
        vulkan.version = probe::MakeVersion(1, 0);
        const probe::Report report = probe::Run(vulkan, kRequired);
        CHECK(report.status == probe::Status::ApiTooOld && report.api_version == probe::MakeVersion(1, 0));
        CHECK((vulkan.calls == std::vector<std::string>{"loader", "entry", "version"}));
        CHECK(Contains(probe::Describe(report, kRequired).cause, "supports only 1.0, but Vulkan 1.3"));
    }
    for (const uint32_t minor : {1u, 2u}) {
        // An old runtime never reaches instance creation, so nothing can assert or end the process.
        ScriptedVulkan vulkan;
        vulkan.version = probe::MakeVersion(1, minor);
        vulkan.devices = {{"Integrated GPU", true}};
        const probe::Report report = probe::Run(vulkan, kRequired);
        CHECK(report.status == probe::Status::ApiTooOld && report.devices.empty());
        CHECK(vulkan.calls.size() == 3);
        CHECK(Contains(probe::Describe(report, kRequired).cause, "supports only 1." + std::to_string(minor)));
        CHECK(Contains(probe::SummaryLine(report), "status=api_too_old api=1." + std::to_string(minor)));
    }
    {
        ScriptedVulkan vulkan;
        vulkan.version = probe::MakeVersion(1, 3) | 275; // a 1.3 patch level is still 1.3
        vulkan.devices = {{"GPU", true}};
        CHECK(probe::Run(vulkan, kRequired).available());
        vulkan.version = probe::MakeVersion(1, 4);
        CHECK(probe::Run(vulkan, kRequired).available());
    }
    {
        ScriptedVulkan vulkan;
        vulkan.version_ok = false;
        vulkan.code = -3;
        const probe::Report report = probe::Run(vulkan, kRequired);
        CHECK(report.status == probe::Status::VersionQueryFailed && report.code == -3);
        CHECK(Contains(probe::Describe(report, kRequired).cause, "error -3"));
    }
    {
        ScriptedVulkan vulkan;
        vulkan.instance = probe::InstanceOutcome::IncompatibleDriver;
        vulkan.code = -9;
        const probe::Report report = probe::Run(vulkan, kRequired);
        CHECK(report.status == probe::Status::NoDriver);
        CHECK(Contains(probe::Describe(report, kRequired).cause, "No Vulkan graphics driver"));
    }
    {
        ScriptedVulkan vulkan;
        vulkan.instance = probe::InstanceOutcome::Failed;
        vulkan.code = -3;
        const probe::Report report = probe::Run(vulkan, kRequired);
        CHECK(report.status == probe::Status::InstanceFailed && report.code == -3);
        CHECK(vulkan.calls.back() == "instance");
    }
    {
        ScriptedVulkan vulkan;
        vulkan.enumerates = false;
        vulkan.code = -4;
        const probe::Report report = probe::Run(vulkan, kRequired);
        CHECK(report.status == probe::Status::EnumerationFailed && report.devices.empty() && report.code == -4);
    }
    {
        ScriptedVulkan vulkan; // zero devices
        const probe::Report report = probe::Run(vulkan, kRequired);
        CHECK(report.status == probe::Status::NoDriver && probe::UsableNames(report, 16).empty());
    }
    {
        // Devices that lack the extensions or features the game needs: found, not usable.
        ScriptedVulkan vulkan;
        vulkan.devices = {{"Old GPU", false}, {"Older GPU", false}};
        const probe::Report report = probe::Run(vulkan, kRequired);
        CHECK(report.status == probe::Status::NoUsableDevice && report.devices.size() == 2 && report.usable_count() == 0);
        CHECK(Contains(probe::Describe(report, kRequired).cause, "No graphics card here supports Vulkan 1.3"));
    }
    {
        ScriptedVulkan vulkan;
        vulkan.devices = {{"Integrated", false}, {"NVIDIA GeForce RTX 4070", true}, {"NVIDIA GeForce RTX 4070", true},
                          {"AMD Radeon 780M", true}};
        const probe::Report report = probe::Run(vulkan, kRequired);
        CHECK(report.available() && report.usable_count() == 3);
        CHECK((probe::UsableNames(report, 16) == std::vector<std::string>{"NVIDIA GeForce RTX 4070", "AMD Radeon 780M"}));
        CHECK(probe::UsableNames(report, 1).size() == 1);
        const std::string line = probe::SummaryLine(report);
        CHECK(Contains(line, "status=available") && Contains(line, "devices=4 usable=3"));
        CHECK(!Contains(line, "NVIDIA") && !Contains(line, "AMD"));
    }
    for (const probe::Status status :
         {probe::Status::LoaderMissing, probe::Status::EntryPointMissing, probe::Status::VersionQueryFailed, probe::Status::ApiTooOld,
          probe::Status::NoDriver, probe::Status::InstanceFailed, probe::Status::EnumerationFailed, probe::Status::NoUsableDevice}) {
        probe::Report report;
        report.status = status;
        const probe::Problem problem = probe::Describe(report, kRequired);
        CHECK(!problem.cause.empty() && !problem.advice.empty());
    }
}

// ---- graphics card detection ----

DcpGpuProbe Answer(probe::Status status, std::vector<std::string> names) {
    DcpGpuProbe result{};
    result.status = static_cast<int>(status);
    for (const std::string& name : names) {
        std::snprintf(result.names[result.count++], sizeof(result.names[0]), "%s", name.c_str());
    }
    return result;
}

void TestGraphicsCards() {
    const fs::path folder = fs::temp_directory_path() / "dcp_launcher_startup_tests";
    fs::remove_all(folder);
    fs::create_directories(folder);
    const fs::path marker = folder / kGpuProbeMarker;

    {
        bool marked_while_probing = false;
        int before = 0;
        const GraphicsCards cards = DetectGraphicsCards(
            false, marker, [&] { ++before; },
            [&] {
                marked_while_probing = fs::exists(marker);
                return Answer(probe::Status::Available, {"AMD Radeon RX 580", "", "Intel(R) UHD Graphics 630"});
            });
        CHECK(marked_while_probing && !fs::exists(marker) && before == 1);
        CHECK(cards.state == GraphicsCards::State::kProbed && cards.vulkan == probe::Status::Available);
        CHECK((cards.names == std::vector<std::string>{"AMD Radeon RX 580", "Intel(R) UHD Graphics 630"}));
        CHECK(std::string(GraphicsCardsProblem(cards)).empty());
    }
    {
        // Vulkan 1.1 only: the list is empty, the launcher says why, and the marker is gone.
        const GraphicsCards cards =
            DetectGraphicsCards(false, marker, nullptr, [] { return Answer(probe::Status::ApiTooOld, {}); });
        CHECK(cards.state == GraphicsCards::State::kProbed && cards.names.empty() && !fs::exists(marker));
        CHECK(std::string(GraphicsCardsProblem(cards)) == "This system does not support Vulkan 1.3");
    }
    {
        const GraphicsCards cards =
            DetectGraphicsCards(false, marker, nullptr, [] { return Answer(probe::Status::LoaderMissing, {}); });
        CHECK(cards.names.empty() && std::string(GraphicsCardsProblem(cards)) == "Vulkan is not available on this system");
    }
    {
        const GraphicsCards cards =
            DetectGraphicsCards(false, marker, nullptr, [] { return Answer(probe::Status::NoUsableDevice, {}); });
        CHECK(std::string(GraphicsCardsProblem(cards)) == "No graphics card found");
    }
    {
        // Software recovery never touches Vulkan.
        int probes = 0;
        const GraphicsCards cards = DetectGraphicsCards(true, marker, nullptr, [&] { ++probes; return Answer(probe::Status::Available, {"X"}); });
        CHECK(probes == 0 && cards.state == GraphicsCards::State::kSkipped && cards.names.empty() && !fs::exists(marker));
        CHECK(Contains(GraphicsCardsProblem(cards), "software recovery mode"));
    }
    {
        // A probe that never returned left its marker: the next start does not ask the driver again.
        { std::ofstream(marker) << "left behind\n"; }
        int probes = 0;
        const GraphicsCards cards = DetectGraphicsCards(false, marker, nullptr, [&] { ++probes; return Answer(probe::Status::Available, {"X"}); });
        CHECK(probes == 0 && cards.state == GraphicsCards::State::kInterrupted && fs::exists(marker));
        CHECK(Contains(GraphicsCardsProblem(cards), "did not finish last time"));
        fs::remove(marker);
    }
    fs::remove_all(folder);
}

// The probe freezes the UI thread, so it waits for a few complete frames after the window is shown.
void TestProbeWaitsForShownFrames() {
    CHECK(!GraphicsProbeDue(0, 0.0) && !GraphicsProbeDue(1, 0.02));
    CHECK(!GraphicsProbeDue(kProbeAfterFrames - 1, kProbeAfterSeconds + 1.0));
    CHECK(!GraphicsProbeDue(kProbeAfterFrames + 100, kProbeAfterSeconds - 0.001));
    CHECK(GraphicsProbeDue(kProbeAfterFrames, kProbeAfterSeconds));
    CHECK(GraphicsProbeDue(kProbeAfterFrames + 1, kProbeAfterSeconds + 3.0));
}

} // namespace

int main() {
    TestRendererValues();
    TestRendererOrder();
    TestRendererSelection();
    TestVulkanProbe();
    TestGraphicsCards();
    TestProbeWaitsForShownFrames();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    std::printf("GROUP|launcher-startup-logic|%s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures ? 1 : 0;
}
