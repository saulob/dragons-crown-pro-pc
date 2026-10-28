// SPDX-License-Identifier: GPL-2.0-or-later
// DEBUG_SAULO: temporary F9 ultrawide frame capture, the parts without Vulkan
// (renderer_vulkan/vk_ultrawide_capture.cpp and tests/ultrawide_capture_tests.cpp).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace UltrawideCapture {

inline constexpr uint32_t kThumbBoxWidth = 128, kThumbBoxHeight = 72;
inline constexpr uint32_t kShotBoxWidth = 1280, kShotBoxHeight = 720;
inline constexpr uint32_t kPassesPerView = 0x2b;

struct Size {
    uint32_t width = 0, height = 0;
    bool operator==(const Size&) const = default;
};

/// [left, right) x [top, bottom); empty when nothing is inside.
struct Rect {
    uint32_t left = 0, top = 0, right = 0, bottom = 0;
    bool Empty() const {
        return right <= left || bottom <= top;
    }
    uint64_t Area() const {
        return Empty() ? 0 : uint64_t(right - left) * (bottom - top);
    }
};

inline bool Overlaps(const Rect& a, const Rect& b) {
    return !a.Empty() && !b.Empty() && a.left < b.right && b.left < a.right && a.top < b.bottom &&
           b.top < a.bottom;
}

inline Rect Union(const Rect& a, const Rect& b) {
    if (a.Empty()) {
        return b;
    }
    if (b.Empty()) {
        return a;
    }
    return {std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right),
            std::max(a.bottom, b.bottom)};
}

/// The largest size with the source's shape inside the box, never larger than the source, at least 1 x 1.
inline Size FitBox(Size source, uint32_t box_width, uint32_t box_height) {
    if (!source.width || !source.height) {
        return {};
    }
    double scale = std::min(double(box_width) / source.width, double(box_height) / source.height);
    scale = std::min(scale, 1.0);
    return {std::max(1u, uint32_t(std::lround(source.width * scale))),
            std::max(1u, uint32_t(std::lround(source.height * scale)))};
}

/// The cells where two RGBA8 pictures of `size` differ by more than `threshold` in some channel.
inline Rect ChangedCells(const uint8_t* before, const uint8_t* after, Size size, uint32_t threshold,
                         uint64_t* cells = nullptr) {
    Rect out;
    bool any = false;
    uint64_t count = 0;
    for (uint32_t y = 0; y < size.height; ++y) {
        for (uint32_t x = 0; x < size.width; ++x) {
            const uint8_t* a = before + (uint64_t(y) * size.width + x) * 4;
            const uint8_t* b = after + (uint64_t(y) * size.width + x) * 4;
            bool changed = false;
            for (int c = 0; c < 4; ++c) {
                changed |= uint32_t(std::abs(int(a[c]) - int(b[c]))) > threshold;
            }
            if (!changed) {
                continue;
            }
            ++count;
            if (!any) {
                out = {x, y, x + 1, y + 1};
                any = true;
            } else {
                out = Union(out, {x, y, x + 1, y + 1});
            }
        }
    }
    if (cells) {
        *cells = count;
    }
    return out;
}

/// Picture cells as target pixels: each cell covers the pixels that map into it.
inline Rect CellsToPixels(const Rect& cells, Size picture, Size target) {
    if (cells.Empty() || !picture.width || !picture.height) {
        return {};
    }
    const auto edge = [](uint32_t cell, uint32_t cells_total, uint32_t pixels) {
        return uint32_t((uint64_t(cell) * pixels + cells_total / 2) / cells_total);
    };
    return {edge(cells.left, picture.width, target.width), edge(cells.top, picture.height, target.height),
            std::min(target.width, edge(cells.right, picture.width, target.width)),
            std::min(target.height, edge(cells.bottom, picture.height, target.height))};
}

/// The parts of a target wider than 16:9 outside its centred 16:9 area; both empty for other shapes.
struct SideAreas {
    Rect left, right;
};

inline SideAreas SidesOf(Size target) {
    const uint32_t centre = uint32_t(std::lround(target.height * 16.0 / 9.0));
    if (target.width <= centre + 1) {
        return {};
    }
    const uint32_t side = (target.width - centre) / 2;
    return {{0, 0, side, target.height}, {target.width - side, 0, target.width, target.height}};
}

/// The pass family the game's pass setup gives a layer (pass id + 0x2b * view).
inline const char* PassFamily(int32_t layer) {
    if (layer < 0) {
        return "?";
    }
    const uint32_t id = uint32_t(layer) % kPassesPerView;
    if ((id >= 0x20 && id <= 0x22) || (id >= 0x25 && id <= 0x28)) {
        return "screen";
    }
    return id == 0x29 ? "4:3" : "world";
}

inline std::string LayerText(int32_t layer) {
    if (layer < 0) {
        return "-";
    }
    char text[24];
    const uint32_t view = uint32_t(layer) / kPassesPerView, id = uint32_t(layer) % kPassesPerView;
    if (view) {
        std::snprintf(text, sizeof(text), "v%u:0x%02x", view, id);
    } else {
        std::snprintf(text, sizeof(text), "0x%02x", id);
    }
    return text;
}

/// The canvas of an orthographic projection: 2 / m00 by 2 / m11.
inline Size Canvas(float m00, float m11) {
    if (!std::isfinite(m00) || !std::isfinite(m11) || std::fabs(m00) < 1e-6f || std::fabs(m11) < 1e-6f) {
        return {};
    }
    return {uint32_t(std::lround(2.0 / std::fabs(m00))), uint32_t(std::lround(2.0 / std::fabs(m11)))};
}

/// Wider than the game's own canvases of that height (812 x 456 world, 1280 x 720 screen): more than 2% over 16:9.
inline bool Widened(Size canvas) {
    return canvas.height && uint64_t(canvas.width) * 9 * 100 > uint64_t(canvas.height) * 16 * 102;
}

/// Image offsets of the queueing code, innermost first: "5812bb<581ac8<4e9b12". A return outside the image (a hook
/// stub replaying the call) is "?".
inline std::string CallersText(const uint32_t* callers, size_t count) {
    size_t used = count;
    while (used && !callers[used - 1]) {
        --used;
    }
    std::string out;
    for (size_t i = 0; i < used; ++i) {
        char text[16];
        if (callers[i]) {
            std::snprintf(text, sizeof(text), "%s%x", i ? "<" : "", callers[i]);
        } else {
            std::snprintf(text, sizeof(text), "%s?", i ? "<" : "");
        }
        out += text;
    }
    return out.empty() ? "-" : out;
}

inline std::string CaptureDirName(uint32_t index) {
    char name[32];
    std::snprintf(name, sizeof(name), "capture-%03u", index);
    return name;
}

/// One past the highest "capture-NNN" among `names`, starting at 1.
inline uint32_t NextCaptureIndex(const std::vector<std::string>& names) {
    uint32_t highest = 0;
    for (const auto& name : names) {
        unsigned value = 0;
        char tail = 0;
        if (name.size() > 8 && name.compare(0, 8, "capture-") == 0 &&
            std::sscanf(name.c_str() + 8, "%u%c", &value, &tail) == 1) {
            highest = std::max(highest, uint32_t(value));
        }
    }
    return highest + 1;
}

inline constexpr uint32_t kNoSlot = 0xffffffffu;

/// A colour target to picture: its texture-cache slot and the uid of the image in it (uids start at 1).
struct TargetRef {
    uint32_t slot = kNoSlot;
    uint64_t uid = 0;
    bool Valid() const {
        return slot != kNoSlot && uid != 0;
    }
};

/// The draw's colour target 0; none when the pipeline does not write it or nothing is bound there.
inline TargetRef ColourTarget(uint32_t mrt_mask, uint32_t slot, uint64_t uid) {
    return (mrt_mask & 1) && slot != kNoSlot && uid ? TargetRef{slot, uid} : TargetRef{};
}

enum class Skip : uint8_t { None, NoTarget, Proxy, Gone, Refused, Limit };

/// Why no picture of `target` is taken. `alive(slot, uid)` asks the texture cache and is only called with a valid
/// target.
template <typename Alive>
Skip PictureSkip(const TargetRef& target, bool proxy, size_t taken, size_t limit, Alive&& alive) {
    if (taken >= limit) {
        return Skip::Limit;
    }
    if (!target.Valid()) {
        return Skip::NoTarget;
    }
    if (proxy) {
        return Skip::Proxy;
    }
    return alive(target.slot, target.uid) ? Skip::None : Skip::Gone;
}

struct SkipCounts {
    uint32_t no_target = 0, proxy = 0, gone = 0, refused = 0, limit = 0;
    void Count(Skip skip) {
        switch (skip) {
        case Skip::NoTarget:
            ++no_target;
            break;
        case Skip::Proxy:
            ++proxy;
            break;
        case Skip::Gone:
            ++gone;
            break;
        case Skip::Refused:
            ++refused;
            break;
        case Skip::Limit:
            ++limit;
            break;
        case Skip::None:
            break;
        }
    }
    uint32_t Total() const {
        return no_target + proxy + gone + refused + limit;
    }
};

inline std::string SkipCountsText(const SkipCounts& c) {
    if (!c.Total()) {
        return "none";
    }
    std::string out;
    const auto add = [&](uint32_t value, const char* what) {
        if (value) {
            out += (out.empty() ? "" : ", ") + std::to_string(value) + " " + what;
        }
    };
    add(c.no_target, "no colour target");
    add(c.proxy, "proxy target");
    add(c.gone, "image gone");
    add(c.refused, "format or layout not copyable");
    add(c.limit, "over the limit");
    return out;
}

/// The source subresource of a picture copy.
struct SnapshotSource {
    bool backing = false, depth = false, multisample = false, integer = false;
    uint32_t mip = 0, levels = 0, slice = 0, layers = 0;
    bool layout_known = false, blit_source = false, rgba8 = false;
};

/// A direct copy needs RGBA8 or BGRA8; a scaled one needs a blit source format.
inline bool SnapshotPossible(const SnapshotSource& s, bool direct) {
    if (!s.backing || s.depth || s.multisample || s.integer || !s.layout_known || s.mip >= s.levels ||
        s.slice >= s.layers) {
        return false;
    }
    return direct ? s.rgba8 : s.blit_source;
}

/// A guest address column: "-" when there is none.
inline std::string AddressText(uint64_t address) {
    if (!address) {
        return "-";
    }
    char text[24];
    std::snprintf(text, sizeof(text), "%#llx", static_cast<unsigned long long>(address));
    return text;
}

/// The draws that get a picture of their own: those reaching the side areas, then those covering at
/// least `large` of their target, each group in draw order, `limit` in all.
struct Footprint {
    bool side = false;
    double coverage = 0.0;
};

inline std::vector<size_t> PickPictures(const std::vector<Footprint>& draws, double large, size_t limit) {
    std::vector<size_t> out;
    for (size_t i = 0; i < draws.size() && out.size() < limit; ++i) {
        if (draws[i].side) {
            out.push_back(i);
        }
    }
    for (size_t i = 0; i < draws.size() && out.size() < limit; ++i) {
        if (!draws[i].side && draws[i].coverage >= large) {
            out.push_back(i);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace UltrawideCapture
