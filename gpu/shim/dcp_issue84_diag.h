// SPDX-License-Identifier: GPL-2.0-or-later
// Bounded Issue #84 draw diagnostics: auto-enabled on Qualcomm (0x5143) or via DCP_ISSUE84_DIAG=1.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

namespace DcpIssue84Diag {

inline constexpr std::uint32_t kQualcommVendorId = 0x5143;
inline constexpr std::uint32_t kMaxEvents = 32;

enum class Hazard : std::uint8_t { None, Color, Depth };

enum class Category : std::uint8_t {
    ColorSnapshot,
    ColorFeedback,
    DepthFeedback,
    InterpFallbackGap,
    ProvokingLast,
    FlatBlendDraw,
};

struct DrawInfo {
    std::uint32_t frame = 0;
    std::uint32_t draw = 0;
    std::uint64_t vs_hash = 0;
    std::uint64_t ps_hash = 0;
    std::uint64_t pipeline_hash = 0;
    std::uint32_t prim_type = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t color_write_masks = 0; // 4 bits per color attachment (up to 8)
    std::uint32_t target_layout = 0;
    std::uint32_t sample_layout = 0;
    std::uint8_t num_render_targets = 0;
    std::uint8_t blend_enable_mask = 0;
    std::uint8_t blend_cb0_src = 0;
    std::uint8_t blend_cb0_dst = 0;
    std::uint8_t blend_cb0_op = 0;
    std::uint8_t depth_stencil_bits = 0; // bit0=z_test bit1=z_write bit2=stencil bit3=z_bounds
    std::uint8_t interp_flat = 0;
    std::uint8_t interp_smooth = 0;
    std::uint8_t interp_nopersp = 0;
    std::uint8_t interp_centroid_or_sample = 0;
    std::uint8_t interp_integer_fix = 0;
    std::uint8_t interp_flags = 0; // bit0=mov_non_p0 bit1=mixed_param bit2=pull_model
    Hazard hazard = Hazard::None;
    bool indexed = false;
    bool provoking_last = false;
    bool feedback_loop_supported = false;
    bool snapshot_used = false;
};

struct Event {
    Category category{};
    DrawInfo draw{};
};

inline bool ShouldEnable(std::uint32_t vendor_id, const char* env_value) {
    if (vendor_id == kQualcommVendorId) {
        return true;
    }
    return env_value && env_value[0] == '1' && env_value[1] == '\0';
}

inline const char* CategoryName(Category c) {
    switch (c) {
    case Category::ColorSnapshot:
        return "color-snapshot";
    case Category::ColorFeedback:
        return "color-feedback";
    case Category::DepthFeedback:
        return "depth-feedback";
    case Category::InterpFallbackGap:
        return "interp-fallback-gap";
    case Category::ProvokingLast:
        return "provoking-last";
    case Category::FlatBlendDraw:
        return "flat-blend-draw";
    }
    return "unknown";
}

inline const char* HazardName(Hazard h) {
    switch (h) {
    case Hazard::None:
        return "none";
    case Hazard::Color:
        return "color";
    case Hazard::Depth:
        return "depth";
    }
    return "none";
}

inline const char* LayoutName(std::uint32_t layout) {
    switch (layout) {
    case 0:
        return "Undefined";
    case 1:
        return "General";
    case 2:
        return "ColorAttachment";
    case 3:
        return "DepthStencilAttachment";
    case 4:
        return "DepthStencilReadOnly";
    case 5:
        return "ShaderReadOnly";
    case 6:
        return "TransferSrc";
    case 7:
        return "TransferDst";
    case 1000117000:
        return "DepthReadOnlyStencilAttachment";
    case 1000117001:
        return "DepthAttachmentStencilReadOnly";
    case 1000241000:
        return "DepthAttachment";
    case 1000241001:
        return "DepthReadOnly";
    case 1000339000:
        return "FeedbackLoop";
    default:
        return "Other";
    }
}

inline const char* TopologyName(std::uint32_t prim_type) {
    switch (prim_type) {
    case 1:
        return "PointList";
    case 2:
        return "LineList";
    case 3:
        return "LineStrip";
    case 4:
        return "TriList";
    case 5:
        return "TriFan";
    case 6:
        return "TriStrip";
    case 9:
        return "Patch";
    case 17:
        return "RectList";
    case 19:
        return "QuadList";
    case 20:
        return "QuadStrip";
    case 21:
        return "Polygon";
    default:
        return "Other";
    }
}

inline std::optional<Event> Classify(const DrawInfo& draw) {
    if (draw.snapshot_used) {
        return Event{Category::ColorSnapshot, draw};
    }
    if (draw.hazard == Hazard::Color) {
        return Event{Category::ColorFeedback, draw};
    }
    if (draw.hazard == Hazard::Depth) {
        return Event{Category::DepthFeedback, draw};
    }
    if (draw.interp_flags != 0) {
        return Event{Category::InterpFallbackGap, draw};
    }
    if (draw.provoking_last &&
        (draw.interp_flat > 0 || draw.prim_type == 5 || draw.prim_type == 6 ||
         draw.prim_type == 17 || draw.prim_type == 19)) {
        return Event{Category::ProvokingLast, draw};
    }
    if (draw.interp_flat > 0 && draw.blend_enable_mask != 0) {
        return Event{Category::FlatBlendDraw, draw};
    }
    return std::nullopt;
}

inline std::uint64_t Signature(const Event& e) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    const auto mix = [&](std::uint64_t v) {
        hash ^= v;
        hash *= 0x100000001b3ull;
    };
    mix(std::uint64_t(e.category));
    mix(e.draw.vs_hash);
    mix(e.draw.ps_hash);
    mix(e.draw.prim_type);
    mix(e.draw.provoking_last);
    mix(e.draw.color_write_masks);
    mix(e.draw.blend_enable_mask);
    mix(e.draw.interp_flags);
    mix(std::uint64_t(e.draw.hazard));
    mix(e.draw.snapshot_used);
    mix(e.draw.target_layout);
    mix(e.draw.sample_layout);
    return hash;
}

inline std::string Format(const Event& e) {
    const auto& d = e.draw;
    char interp_flags_str[32];
    if (d.interp_flags == 0) {
        std::snprintf(interp_flags_str, sizeof(interp_flags_str), "none");
    } else {
        std::snprintf(interp_flags_str, sizeof(interp_flags_str), "%s%s%s",
                      (d.interp_flags & 1u) ? "P" : "", (d.interp_flags & 2u) ? "M" : "",
                      (d.interp_flags & 4u) ? "L" : "");
    }
    char line[512];
    std::snprintf(
        line, sizeof(line),
        "Issue84 diag [%s] f=%u d=%u vs=%016llx ps=%016llx pipe=%016llx topo=%s idx=%u verts=%u "
        "prov=%s rts=%u wmask=%#x blend=%#x(%u/%u/%u) ds=%#x fb_ext=%s hazard=%s t_lay=%s "
        "s_lay=%s snap=%s interp=f%u/s%u/n%u/c%u flags=%s intfix=%u",
        CategoryName(e.category), d.frame, d.draw, static_cast<unsigned long long>(d.vs_hash),
        static_cast<unsigned long long>(d.ps_hash),
        static_cast<unsigned long long>(d.pipeline_hash), TopologyName(d.prim_type),
        d.indexed ? 1u : 0u, d.vertex_count, d.provoking_last ? "last" : "first",
        unsigned(d.num_render_targets), unsigned(d.color_write_masks),
        unsigned(d.blend_enable_mask), unsigned(d.blend_cb0_src), unsigned(d.blend_cb0_dst),
        unsigned(d.blend_cb0_op), unsigned(d.depth_stencil_bits),
        d.feedback_loop_supported ? "yes" : "no", HazardName(d.hazard),
        LayoutName(d.target_layout), LayoutName(d.sample_layout), d.snapshot_used ? "yes" : "no",
        unsigned(d.interp_flat), unsigned(d.interp_smooth), unsigned(d.interp_nopersp),
        unsigned(d.interp_centroid_or_sample), interp_flags_str, unsigned(d.interp_integer_fix));
    return line;
}

class Monitor {
public:
    void Init(std::uint32_t vendor_id, const char* env_override = nullptr) {
        const char* env = env_override ? env_override : std::getenv("DCP_ISSUE84_DIAG");
        active_ = ShouldEnable(vendor_id, env);
        emitted_ = 0;
        last_frame_ = 0;
        draw_in_frame_ = 0;
    }

    bool Active() const noexcept {
        return active_ && emitted_ < kMaxEvents;
    }

    bool Enabled() const noexcept {
        return active_;
    }

    std::uint32_t NextDraw(std::uint32_t frame) noexcept {
        if (frame != last_frame_) {
            last_frame_ = frame;
            draw_in_frame_ = 0;
        }
        return ++draw_in_frame_;
    }

    template <typename Emit>
    bool Report(const Event& event, Emit&& emit) {
        if (!Active()) {
            return false;
        }
        const std::uint64_t sig = Signature(event);
        for (std::uint32_t i = 0; i < emitted_; ++i) {
            if (seen_[i] == sig) {
                return false;
            }
        }
        seen_[emitted_++] = sig;
        const std::string line = Format(event);
        emit(line.c_str());
        return true;
    }

    std::uint32_t Emitted() const noexcept {
        return emitted_;
    }

private:
    bool active_ = false;
    std::uint32_t emitted_ = 0;
    std::uint32_t last_frame_ = 0;
    std::uint32_t draw_in_frame_ = 0;
    std::array<std::uint64_t, kMaxEvents> seen_{};
};

} // namespace DcpIssue84Diag
