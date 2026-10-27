// SPDX-License-Identifier: GPL-2.0-or-later
// Experimental AMD workaround for black regions under characters standing in water (issue #32): on by default on AMD
// GPUs, off elsewhere; F3 switches it on any GPU for the running process only (tests/amd_water_fix_tests.cpp).
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

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
    /// One F3 press flips the fix; key repeats change nothing (false).
    bool Press(bool repeat) {
        if (repeat) {
            return false;
        }
        on_.store(!On(), std::memory_order_relaxed);
        return true;
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

enum class Hazard : std::size_t { StencilColor, StencilLink, DepthSampled, ColorFeedback, Count };

inline constexpr std::array<const char*, std::size_t(Hazard::Count)> kHazardNames = {
    "stencil_color", "stencil_link", "depth_sampled", "color_feedback"};

/// DEBUG_SAULO: how often each hazard was reached, and which of the first few distinct resources to describe in the
/// session log.
class HazardLog {
public:
    static constexpr std::size_t kDescribed = 4;

    /// Counts one occurrence; true when `key` is among the first kDescribed distinct keys of its kind.
    bool Record(Hazard kind, std::uint64_t key) {
        const std::size_t k = std::size_t(kind);
        counts_[k].fetch_add(1, std::memory_order_relaxed);
        std::scoped_lock lock{mutex_};
        auto& seen = keys_[k];
        for (std::size_t i = 0; i < described_[k]; ++i) {
            if (seen[i] == key) {
                return false;
            }
        }
        if (described_[k] == kDescribed) {
            return false;
        }
        seen[described_[k]++] = key;
        return true;
    }

    std::uint64_t Count(Hazard kind) const {
        return counts_[std::size_t(kind)].load(std::memory_order_relaxed);
    }

    std::string Summary() const {
        std::string text;
        for (std::size_t k = 0; k < kHazardNames.size(); ++k) {
            text += (k ? " " : "") + std::string(kHazardNames[k]) + "=" + std::to_string(Count(Hazard(k)));
        }
        return text;
    }

private:
    std::array<std::atomic<std::uint64_t>, std::size_t(Hazard::Count)> counts_{};
    std::mutex mutex_;
    std::array<std::array<std::uint64_t, kDescribed>, std::size_t(Hazard::Count)> keys_{};
    std::array<std::size_t, std::size_t(Hazard::Count)> described_{};
};

inline HazardLog& Hazards() {
    static HazardLog log;
    return log;
}

} // namespace DcpAmdWaterFix
