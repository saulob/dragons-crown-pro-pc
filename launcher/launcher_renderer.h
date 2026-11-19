#pragma once

#include <string_view>
#include <vector>

namespace dcp {

// SDL renderer names to try in order; an empty name lets SDL pick its first working renderer.
inline std::vector<std::string_view> LauncherRendererOrder(bool windows) {
    if (windows) {
        return {"direct3d11", "software"};
    }
    return {"", "software"};
}

// System modules a renderer loads that must stay mapped until the process exits. SDL unloads d3d11.dll right after the
// device's last Release, while an NVIDIA driver thread can still be returning through it (issue #45).
inline std::vector<std::string_view> RendererModulesToPin(std::string_view renderer) {
    if (renderer == "direct3d11") {
        return {"d3d11.dll", "dxgi.dll"};
    }
    return {};
}

} // namespace dcp
