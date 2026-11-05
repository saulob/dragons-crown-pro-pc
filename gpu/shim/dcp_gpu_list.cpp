// SPDX-License-Identifier: GPL-2.0-or-later
// The launcher's list of Vulkan devices the renderer can use (the Graphics card row), and the check that Vulkan can run
// the renderer at all.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

#include <fmt/format.h>

#include "common/types.h"
#include "dcp_renderer.h"
#include "dcp_settings.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"

namespace {

struct VulkanProblem {
    std::string cause;
    std::string advice;
};

// The renderer's own requirements, asked without its assertions, which end the process with no word to the player.
std::optional<VulkanProblem> FindVulkanProblem() {
    using namespace Vulkan;
    static std::unique_ptr<vk::detail::DynamicLoader> loader;
    if (!loader || !loader->success()) {
        loader = std::make_unique<vk::detail::DynamicLoader>();
    }
    if (!loader->success()) {
        return VulkanProblem{"Vulkan is not installed on this system.",
                             "Install your graphics driver's Vulkan support and try again."};
    }
    VULKAN_HPP_DEFAULT_DISPATCHER.init(loader->getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    u32 version = VK_API_VERSION_1_0;
    if (VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumerateInstanceVersion) {
        const auto [result, value] = vk::enumerateInstanceVersion();
        if (result == vk::Result::eSuccess) {
            version = value;
        }
    }
    const u32 major = VK_VERSION_MAJOR(TargetVulkanApiVersion), minor = VK_VERSION_MINOR(TargetVulkanApiVersion);
    if (version < TargetVulkanApiVersion) {
        return VulkanProblem{fmt::format("This system's Vulkan runtime supports only {}.{}, but Vulkan {}.{} or newer "
                                         "is required.",
                                         VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version), major, minor),
                             "Update your graphics driver and Vulkan runtime, and try again."};
    }
    const VulkanProblem no_driver{"No Vulkan graphics driver was found.",
                                  "Install or update your graphics driver and try again."};
    auto [result, instance] = CreateProbeInstance();
    if (result == vk::Result::eErrorIncompatibleDriver) {
        return no_driver;
    }
    if (result != vk::Result::eSuccess) {
        return VulkanProblem{fmt::format("Vulkan could not be started ({}).", vk::to_string(result)),
                             "Update your graphics driver and try again."};
    }
    const auto [devices_result, devices] = instance->enumeratePhysicalDevices();
    if (devices_result != vk::Result::eSuccess || devices.empty()) {
        return no_driver;
    }
    if (std::ranges::none_of(devices, IsDeviceUsable)) {
        return VulkanProblem{fmt::format("No graphics card here supports Vulkan {}.{} with the features the game "
                                         "needs.",
                                         major, minor),
                             "Update your graphics driver and try again."};
    }
    return std::nullopt;
}

// A success is remembered; a failure is asked again, as the player may install a driver and press Play again.
std::optional<VulkanProblem> CheckVulkan() {
    static bool usable = false;
    if (usable) {
        return std::nullopt;
    }
    auto problem = FindVulkanProblem();
    usable = !problem;
    return problem;
}

} // namespace

extern "C" int dcp_check_vulkan(char* cause, size_t cause_size, char* advice, size_t advice_size) {
    const auto problem = CheckVulkan();
    if (!problem) {
        return 1;
    }
    std::printf("Graphics: %s %s\n", problem->cause.c_str(), problem->advice.c_str());
    std::snprintf(cause, cause_size, "%s", problem->cause.c_str());
    std::snprintf(advice, advice_size, "%s", problem->advice.c_str());
    return 0;
}

// Vulkan stops the start only when the player's Renderer setting needs it.
extern "C" int dcp_check_renderer(const char* settings_file, char* cause, size_t cause_size,
                                  char* advice, size_t advice_size) {
    DcpSettings::Values values;
    DcpSettings::Read(settings_file ? settings_file : "", values);
    switch (DcpRenderer::Choice(values.renderer.load())) {
    case DcpRenderer::OpenGL:
        return 1;
    case DcpRenderer::Auto:
        dcp_check_vulkan(cause, cause_size, advice, advice_size);
        return 1;
    default:
        return dcp_check_vulkan(cause, cause_size, advice, advice_size);
    }
}

extern "C" int dcp_gpu_names(char (*names)[256], int max) {
    using namespace Vulkan;
#ifndef _WIN32
    if (CheckVulkan()) {
        return 0;
    }
#endif
    int count = 0;
    {
        auto instance = CreateInstance(Frontend::WindowSystemType::Headless, false, false);
        if (!instance) {
            return 0;
        }
        const auto [result, devices] = instance->enumeratePhysicalDevices();
        if (result != vk::Result::eSuccess) {
            return 0;
        }
        for (const auto& device : devices) {
            if (count == max || !IsDeviceUsable(device)) {
                continue;
            }
            const auto properties = device.getProperties();
            const char* name = properties.deviceName.data();
            bool known = false;
            for (int i = 0; i < count; ++i) {
                known |= !std::strcmp(names[i], name);
            }
            if (!known) {
                std::snprintf(names[count++], 256, "%s", name);
            }
        }
    }
    return count;
}
