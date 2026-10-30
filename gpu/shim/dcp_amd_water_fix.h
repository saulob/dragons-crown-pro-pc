// SPDX-License-Identifier: GPL-2.0-or-later
// Experimental AMD workaround (not a proven fix) for black regions under characters standing in water (issue #32): on
// by default on AMD GPUs, off elsewhere (tests/amd_water_fix_tests.cpp).
#pragma once

#include <atomic>
#include <cstdint>

namespace DcpAmdWaterFix {

inline constexpr std::uint32_t kAmdVendorId = 0x1002;

inline bool IsAmdVendor(std::uint32_t vendor_id) {
    return vendor_id == kAmdVendorId;
}

inline bool DefaultOn(std::uint32_t vendor_id) {
    return IsAmdVendor(vendor_id);
}

class Switch {
public:
    /// Every process starts from the vendor default; nothing is saved.
    void Init(std::uint32_t vendor_id) {
        on_.store(DefaultOn(vendor_id), std::memory_order_relaxed);
    }
    bool On() const {
        return on_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<bool> on_{false};
};

inline Switch& State() {
    static Switch state;
    return state;
}

/// A texture linked to a depth-stencil image reads that image's stencil only through a stencil-sized format
/// (shadPS4 fecfbed0); with the fix on, a color texture that reuses the stencil memory keeps its own image.
inline bool FollowsStencilLink(bool fix_on, bool stencil_format) {
    return stencil_format || !fix_on;
}

/// A depth target that the same draw samples while writing depth or stencil: the attachment and the descriptor
/// then share the General layout instead of disagreeing.
inline bool SharesDepthLayout(bool fix_on, bool sampled, bool attachment_writes) {
    return fix_on && sampled && attachment_writes;
}

} // namespace DcpAmdWaterFix
