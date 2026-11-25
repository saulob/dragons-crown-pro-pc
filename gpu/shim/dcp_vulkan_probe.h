// SPDX-License-Identifier: GPL-2.0-or-later
// What Vulkan offers, asked without assertions. The launcher's graphics card list and the pre-game check both read it.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace DcpVulkanProbe {

constexpr uint32_t MakeVersion(uint32_t major, uint32_t minor) {
    return (major << 22) | (minor << 12);
}
constexpr uint32_t Major(uint32_t version) {
    return (version >> 22) & 0x7f;
}
constexpr uint32_t Minor(uint32_t version) {
    return (version >> 12) & 0x3ff;
}

enum class Status {
    Available,
    LoaderMissing,
    EntryPointMissing,
    VersionQueryFailed,
    ApiTooOld,
    NoDriver,
    InstanceFailed,
    EnumerationFailed,
    NoUsableDevice,
};

enum class InstanceOutcome { Created, IncompatibleDriver, Failed };

struct Device {
    std::string name;
    bool usable = false;
};

/// The system under test: the real Vulkan loader in the shim, a scripted one in the tests. Every failure is a return
/// value; `code` is the VkResult the system gave.
class Source {
public:
    virtual ~Source() = default;
    virtual bool OpenLoader() = 0;
    virtual bool LoadEntryPoints() = 0;
    virtual bool QueryInstanceVersion(uint32_t& version, int& code) = 0;
    virtual InstanceOutcome CreateInstance(int& code) = 0;
    virtual bool EnumerateDevices(std::vector<Device>& devices, int& code) = 0;
};

struct Report {
    Status status = Status::LoaderMissing;
    uint32_t api_version = 0;
    int code = 0;
    std::vector<Device> devices;

    bool available() const { return status == Status::Available; }
    int usable_count() const {
        return int(std::count_if(devices.begin(), devices.end(), [](const Device& device) { return device.usable; }));
    }
};

inline const char* StatusName(Status status) {
    switch (status) {
    case Status::Available: return "available";
    case Status::LoaderMissing: return "loader_missing";
    case Status::EntryPointMissing: return "entry_point_missing";
    case Status::VersionQueryFailed: return "version_query_failed";
    case Status::ApiTooOld: return "api_too_old";
    case Status::NoDriver: return "no_driver";
    case Status::InstanceFailed: return "instance_failed";
    case Status::EnumerationFailed: return "enumeration_failed";
    case Status::NoUsableDevice: return "no_usable_device";
    }
    return "unknown";
}

/// Stops at the first step that fails; the Source is not touched after that.
inline Report Run(Source& source, uint32_t required) {
    Report report;
    if (!source.OpenLoader()) {
        report.status = Status::LoaderMissing;
        return report;
    }
    if (!source.LoadEntryPoints()) {
        report.status = Status::EntryPointMissing;
        return report;
    }
    if (!source.QueryInstanceVersion(report.api_version, report.code)) {
        report.status = Status::VersionQueryFailed;
        return report;
    }
    if (report.api_version < required) {
        report.status = Status::ApiTooOld;
        return report;
    }
    switch (source.CreateInstance(report.code)) {
    case InstanceOutcome::IncompatibleDriver:
        report.status = Status::NoDriver;
        return report;
    case InstanceOutcome::Failed:
        report.status = Status::InstanceFailed;
        return report;
    case InstanceOutcome::Created:
        break;
    }
    if (!source.EnumerateDevices(report.devices, report.code)) {
        report.devices.clear();
        report.status = Status::EnumerationFailed;
        return report;
    }
    if (report.devices.empty()) {
        report.status = Status::NoDriver;
        return report;
    }
    report.status = report.usable_count() > 0 ? Status::Available : Status::NoUsableDevice;
    return report;
}

/// The distinct names of the usable devices, in enumeration order, at most `max`.
inline std::vector<std::string> UsableNames(const Report& report, size_t max) {
    std::vector<std::string> names;
    for (const Device& device : report.devices) {
        if (names.size() == max) {
            break;
        }
        if (device.usable && std::find(names.begin(), names.end(), device.name) == names.end()) {
            names.push_back(device.name);
        }
    }
    return names;
}

struct Problem {
    std::string cause;
    std::string advice;
};

inline std::string VersionText(uint32_t version) {
    return std::to_string(Major(version)) + "." + std::to_string(Minor(version));
}

/// What to tell the player when Vulkan cannot run the renderer.
inline Problem Describe(const Report& report, uint32_t required) {
    static const char kUpdate[] = "Update your graphics driver and try again.";
    switch (report.status) {
    case Status::Available:
        return {};
    case Status::LoaderMissing:
        return {"Vulkan is not installed on this system.", "Install your graphics driver's Vulkan support and try again."};
    case Status::EntryPointMissing:
        return {"This system's Vulkan loader is damaged.", "Reinstall your graphics driver and try again."};
    case Status::ApiTooOld:
        return {"This system's Vulkan runtime supports only " + VersionText(report.api_version) + ", but Vulkan " +
                    VersionText(required) + " or newer is required.",
                "Update your graphics driver and Vulkan runtime, and try again."};
    case Status::NoDriver:
        return {"No Vulkan graphics driver was found.", "Install or update your graphics driver and try again."};
    case Status::NoUsableDevice:
        return {"No graphics card here supports Vulkan " + VersionText(required) + " with the features the game needs.",
                kUpdate};
    case Status::VersionQueryFailed:
    case Status::InstanceFailed:
    case Status::EnumerationFailed:
        break;
    }
    return {"Vulkan could not be started (error " + std::to_string(report.code) + ").", kUpdate};
}

/// One log line: the outcome of the probe, never a device name.
inline std::string SummaryLine(const Report& report) {
    char line[160];
    std::snprintf(line, sizeof(line), "Launcher: startup: vulkan probe status=%s api=%u.%u code=%d devices=%zu usable=%d",
                  StatusName(report.status), Major(report.api_version), Minor(report.api_version), report.code,
                  report.devices.size(), report.usable_count());
    return line;
}

} // namespace DcpVulkanProbe
