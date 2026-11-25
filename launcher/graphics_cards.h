#pragma once

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

#include "dcp_gpu_probe.h"
#include "dcp_graphics.h"
#include "dcp_vulkan_probe.h"

namespace dcp {

// What the Graphics card row can show. The launcher never needs Vulkan to open: a card list that could not be made is
// empty, and the saved choice is left as it is.
struct GraphicsCards {
    enum class State { kNotAsked, kSkipped, kInterrupted, kProbed };
    State state = State::kNotAsked;
    DcpVulkanProbe::Status vulkan = DcpVulkanProbe::Status::LoaderMissing;
    std::vector<std::string> names;
};

inline constexpr char kGpuProbeMarker[] = "gpu_probe.pending";

// Asks Vulkan for the cards once. `skip` leaves Vulkan alone (software recovery). The marker file exists only while the
// probe runs: a driver that ends the process inside it leaves the marker, and the next start does not ask again.
inline GraphicsCards DetectGraphicsCards(bool skip, const std::filesystem::path& marker, const std::function<void()>& before,
                                         const std::function<DcpGpuProbe()>& probe) {
    GraphicsCards cards;
    if (skip) {
        cards.state = GraphicsCards::State::kSkipped;
        return cards;
    }
    std::error_code ec;
    if (std::filesystem::exists(marker, ec)) {
        cards.state = GraphicsCards::State::kInterrupted;
        return cards;
    }
    {
        std::ofstream pending(marker, std::ios::binary | std::ios::trunc);
        pending << "graphics card detection started\n";
    }
    if (before) {
        before();
    }
    const DcpGpuProbe result = probe();
    std::filesystem::remove(marker, ec);
    cards.state = GraphicsCards::State::kProbed;
    cards.vulkan = static_cast<DcpVulkanProbe::Status>(result.status);
    cards.names = DcpGraphics::GpuChoices({result.names, result.names + result.count});
    return cards;
}

// The English text the Graphics card row shows when there is no card to choose; empty when there are cards.
inline const char* GraphicsCardsProblem(const GraphicsCards& cards) {
    using State = GraphicsCards::State;
    using Status = DcpVulkanProbe::Status;
    if (!cards.names.empty()) {
        return "";
    }
    switch (cards.state) {
    case State::kNotAsked: return "No graphics card found";
    case State::kSkipped: return "Graphics card detection is off in software recovery mode";
    case State::kInterrupted: return "Graphics card detection was skipped (it did not finish last time)";
    case State::kProbed: break;
    }
    switch (cards.vulkan) {
    case Status::ApiTooOld: return "This system does not support Vulkan 1.3";
    case Status::NoUsableDevice: return "No graphics card found";
    case Status::Available: return "No graphics card found";
    default: return "Vulkan is not available on this system";
    }
}

} // namespace dcp
