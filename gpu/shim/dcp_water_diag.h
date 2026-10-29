// SPDX-License-Identifier: GPL-2.0-or-later
// Issue #32 diagnostics: on AMD GPUs only, one log line per distinct draw that samples or aliases one of its own
// render targets, at most kMaxEvents lines per process (tests/water_diag_tests.cpp).
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>

namespace DcpWaterDiag {

inline constexpr std::uint32_t kAmdVendorId = 0x1002;
inline constexpr std::uint32_t kMaxEvents = 20;
inline constexpr std::uint8_t kDepthSlot = 8;

enum class Reason : std::uint8_t { ColorFeedback, DepthFeedback, ImageAlias };

struct Image {
    std::uint32_t id = 0;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::uint32_t format = 0; // VkFormat
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t layout = 0; // VkImageLayout
    bool aliased = false; // the texture cache copies between images at this guest address
};

struct Target : Image {
    std::uint8_t slot = 0; // color buffer, or kDepthSlot
    std::uint8_t mip = 0;
    bool sampled = false;            // the same image is also bound as a texture
    std::uint32_t sample_layout = 0; // layout once the textures were bound (the descriptors')
};

struct Draw {
    std::uint64_t vs = 0;
    std::uint64_t ps = 0;
    bool feedback_layout = false; // VK_EXT_attachment_feedback_loop_layout
    bool old_fix = false;         // DcpAmdWaterFix
    bool depth_writes = false;
    bool scaled = false; // reduced scene proxies
};

struct Event {
    Reason reason{};
    Draw draw;
    Target target;
    Image sample;
};

inline bool Overlaps(const Image& a, const Image& b) {
    return a.begin < b.end && b.begin < a.end;
}

/// Sampling a depth target is ordinary while its attachment is read-only and matches the descriptor layout.
inline bool SuspiciousDepthSample(const Draw& draw, const Target& depth) {
    return draw.depth_writes || depth.layout != depth.sample_layout;
}

inline std::optional<Event> Classify(const Draw& draw, std::span<const Target> targets,
                                     std::span<const Image> sampled) {
    for (const Target& target : targets) {
        if (!target.sampled) {
            continue;
        }
        if (target.slot != kDepthSlot) {
            return Event{Reason::ColorFeedback, draw, target, target};
        }
        if (SuspiciousDepthSample(draw, target)) {
            return Event{Reason::DepthFeedback, draw, target, target};
        }
    }
    for (const Target& target : targets) {
        for (const Image& image : sampled) {
            if (image.id != target.id && Overlaps(image, target)) {
                return Event{Reason::ImageAlias, draw, target, image};
            }
        }
    }
    return std::nullopt;
}

inline const char* ReasonName(Reason reason) {
    switch (reason) {
    case Reason::ColorFeedback:
        return "color-feedback";
    case Reason::DepthFeedback:
        return "depth-feedback";
    case Reason::ImageAlias:
        return "image-alias";
    }
    return "?";
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
    }
    return "Other";
}

inline const char* Relation(const Image& sample, const Image& target) {
    if (sample.id == target.id) {
        return "self";
    }
    if (sample.begin == target.begin) {
        return "same-base";
    }
    if (sample.begin >= target.begin && sample.end <= target.end) {
        return "inside";
    }
    if (target.begin >= sample.begin && target.end <= sample.end) {
        return "covers";
    }
    return "partial";
}

inline std::uint32_t SampleLayout(const Event& e) {
    return e.reason == Reason::ImageAlias ? e.sample.layout : e.target.sample_layout;
}

/// Addresses and image ids are left out: the same pass on another buffer of a swap chain is the same event.
inline std::uint64_t Signature(const Event& e) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    const auto mix = [&](std::uint64_t value) {
        hash ^= value;
        hash *= 0x100000001b3ull;
    };
    mix(std::uint64_t(e.reason));
    mix(e.draw.vs);
    mix(e.draw.ps);
    mix(std::uint64_t(e.draw.feedback_layout) | std::uint64_t(e.draw.old_fix) << 1 |
        std::uint64_t(e.draw.depth_writes) << 2 | std::uint64_t(e.draw.scaled) << 3);
    mix(std::uint64_t(e.target.slot) | std::uint64_t(e.target.mip) << 8);
    mix(e.target.format);
    mix(std::uint64_t(e.target.width) << 32 | e.target.height);
    mix(std::uint64_t(e.target.layout) << 32 | SampleLayout(e));
    mix(e.sample.format);
    mix(std::uint64_t(e.sample.width) << 32 | e.sample.height);
    mix(e.sample.begin - e.target.begin);
    mix(e.sample.aliased);
    return hash;
}

inline int Format(const Event& e, std::uint32_t frame, std::uint32_t draw, char* out, std::size_t size) {
    const Image& s = e.sample;
    const Target& t = e.target;
    return std::snprintf(
        out, size,
        "WaterDiag: f=%u d=%u reason=%s vs=%016llx ps=%016llx rt=%u slot=%u mip=%u fmt=%u extent=%ux%u "
        "guest=%#llx size=%#llx sample=%u sfmt=%u sextent=%ux%u sguest=%#llx ssize=%#llx overlap=%s aliased=%d "
        "layout=%s slayout=%s feedback=%d dwrite=%d scaled=%d oldfix=%d",
        frame, draw, ReasonName(e.reason), static_cast<unsigned long long>(e.draw.vs),
        static_cast<unsigned long long>(e.draw.ps), t.id, unsigned(t.slot), unsigned(t.mip), t.format, t.width,
        t.height, static_cast<unsigned long long>(t.begin), static_cast<unsigned long long>(t.end - t.begin), s.id,
        s.format, s.width, s.height, static_cast<unsigned long long>(s.begin),
        static_cast<unsigned long long>(s.end - s.begin), Relation(s, t), int(s.aliased), LayoutName(t.layout),
        LayoutName(SampleLayout(e)), int(e.draw.feedback_layout), int(e.draw.depth_writes), int(e.draw.scaled),
        int(e.draw.old_fix));
}

/// Draw recording only (one thread at a time); frames are counted by the presenter.
class Monitor {
public:
    void Init(std::uint32_t vendor_id) {
        enabled_ = vendor_id == kAmdVendorId;
    }
    /// Until the limit notice: afterwards draws are not even classified.
    bool Active() const {
        return enabled_ && !suppressed_;
    }

    std::uint32_t NextDraw(std::uint32_t frame) {
        if (frame != frame_) {
            frame_ = frame;
            draw_ = 0;
        }
        return draw_++;
    }

    template <typename Sink>
    void Report(const Event& event, std::uint32_t frame, std::uint32_t draw, Sink&& sink) {
        if (!Active()) {
            return;
        }
        const std::uint64_t signature = Signature(event);
        if (std::find(seen_.begin(), seen_.begin() + count_, signature) != seen_.begin() + count_) {
            return;
        }
        if (count_ == kMaxEvents) {
            suppressed_ = true;
            sink("WaterDiag: limit reached, further events suppressed");
            return;
        }
        seen_[count_++] = signature;
        char line[512];
        Format(event, frame, draw, line, sizeof(line));
        sink(static_cast<const char*>(line));
    }

    std::uint32_t Count() const {
        return count_;
    }

private:
    bool enabled_ = false;
    bool suppressed_ = false;
    std::uint32_t count_ = 0;
    std::array<std::uint64_t, kMaxEvents> seen_{};
    std::uint32_t frame_ = 0;
    std::uint32_t draw_ = 0;
};

inline std::atomic<std::uint32_t>& Frames() {
    static std::atomic<std::uint32_t> frames{0};
    return frames;
}

inline void CountFrame() {
    Frames().fetch_add(1, std::memory_order_relaxed);
}

inline std::uint32_t Frame() {
    return Frames().load(std::memory_order_relaxed);
}

} // namespace DcpWaterDiag
