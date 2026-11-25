// SPDX-License-Identifier: GPL-2.0-or-later
// The launcher's list of Vulkan devices the renderer can use (the Graphics card row), and the check that Vulkan can run
// the renderer at all.

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "common/types.h"
#include "dcp_gpu_probe.h"
#include "dcp_renderer.h"
#include "dcp_settings.h"
#include "dcp_vulkan_probe.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"

namespace {

// Kept for the life of the process: the game's own instance loads the same driver modules afterwards.
std::unique_ptr<vk::detail::DynamicLoader> g_loader;

// The probe loads its commands into the shared dispatcher; it is put back as found so no command of the destroyed probe
// instance stays in it.
class VulkanSource final : public DcpVulkanProbe::Source {
public:
    VulkanSource() : saved_(VULKAN_HPP_DEFAULT_DISPATCHER) {}
    ~VulkanSource() override {
        instance_.reset();
        VULKAN_HPP_DEFAULT_DISPATCHER = saved_;
    }

    bool OpenLoader() override {
        if (!g_loader || !g_loader->success()) {
            g_loader = std::make_unique<vk::detail::DynamicLoader>();
        }
        return g_loader->success();
    }

    bool LoadEntryPoints() override {
        const auto get_proc = g_loader->getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
        if (!get_proc) {
            return false;
        }
        VULKAN_HPP_DEFAULT_DISPATCHER.init(get_proc);
        return true;
    }

    bool QueryInstanceVersion(u32& version, int& code) override {
        version = VK_API_VERSION_1_0;
        if (!VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumerateInstanceVersion) {
            return true;
        }
        const auto [result, value] = vk::enumerateInstanceVersion();
        code = static_cast<int>(result);
        if (result != vk::Result::eSuccess) {
            return false;
        }
        version = value;
        return true;
    }

    DcpVulkanProbe::InstanceOutcome CreateInstance(int& code) override {
        auto [result, instance] = Vulkan::CreateProbeInstance();
        code = static_cast<int>(result);
        if (result == vk::Result::eErrorIncompatibleDriver) {
            return DcpVulkanProbe::InstanceOutcome::IncompatibleDriver;
        }
        if (result != vk::Result::eSuccess) {
            return DcpVulkanProbe::InstanceOutcome::Failed;
        }
        instance_ = std::move(instance);
        return DcpVulkanProbe::InstanceOutcome::Created;
    }

    bool EnumerateDevices(std::vector<DcpVulkanProbe::Device>& devices, int& code) override {
        const auto [result, physical_devices] = instance_->enumeratePhysicalDevices();
        code = static_cast<int>(result);
        if (result != vk::Result::eSuccess) {
            return false;
        }
        for (const vk::PhysicalDevice physical : physical_devices) {
            devices.push_back({physical.getProperties().deviceName.data(), Vulkan::IsDeviceUsable(physical)});
        }
        return true;
    }

private:
    vk::detail::DispatchLoaderDynamic saved_;
    vk::UniqueInstance instance_;
};

// A success is remembered; a failure is asked again, as the player may install a driver and press Play again.
DcpVulkanProbe::Report ProbeVulkan() {
    static std::mutex lock;
    static std::optional<DcpVulkanProbe::Report> usable;
    const std::lock_guard guard(lock);
    if (usable) {
        return *usable;
    }
    VulkanSource source;
    DcpVulkanProbe::Report report = DcpVulkanProbe::Run(source, Vulkan::TargetVulkanApiVersion);
    if (report.available()) {
        usable = report;
    }
    return report;
}

} // namespace

extern "C" int dcp_check_vulkan(char* cause, size_t cause_size, char* advice, size_t advice_size) {
    const DcpVulkanProbe::Report report = ProbeVulkan();
    if (report.available()) {
        return 1;
    }
    const DcpVulkanProbe::Problem problem = DcpVulkanProbe::Describe(report, Vulkan::TargetVulkanApiVersion);
    std::printf("Graphics: %s %s\n", problem.cause.c_str(), problem.advice.c_str());
    std::snprintf(cause, cause_size, "%s", problem.cause.c_str());
    std::snprintf(advice, advice_size, "%s", problem.advice.c_str());
    return 0;
}

// Vulkan stops the start only when the player's Renderer setting needs it.
extern "C" int dcp_check_renderer(const char* settings_file, char* cause, size_t cause_size,
                                  char* advice, size_t advice_size) {
    DcpSettings::Values values;
    DcpSettings::Read(settings_file ? settings_file : "", values);
    switch (DcpRenderer::Choice(values.renderer.load())) {
    case DcpRenderer::OpenGL:
    case DcpRenderer::Direct3D11:
        return 1;
    case DcpRenderer::Auto:
        dcp_check_vulkan(cause, cause_size, advice, advice_size);
        return 1;
    default:
        return dcp_check_vulkan(cause, cause_size, advice, advice_size);
    }
}

extern "C" void dcp_gpu_probe(DcpGpuProbe* out) {
    const DcpVulkanProbe::Report report = ProbeVulkan();
    std::printf("%s\n", DcpVulkanProbe::SummaryLine(report).c_str());
    std::fflush(nullptr);
    std::memset(out, 0, sizeof(*out));
    out->status = static_cast<int>(report.status);
    out->api_version = report.api_version;
    out->code = report.code;
    for (const std::string& name : DcpVulkanProbe::UsableNames(report, DCP_GPU_PROBE_MAX)) {
        std::snprintf(out->names[out->count++], sizeof(out->names[0]), "%s", name.c_str());
    }
}
