// SPDX-License-Identifier: GPL-2.0-or-later
// Guest primitives that OpenGL ES has no equivalent for; free of GL calls so it can be tested headless.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <vector>
#include "common/types.h"

namespace OpenGL {

inline constexpr int RectStripVertices = 4;
inline constexpr u32 RectVerticesPerRect = 3;
inline constexpr u32 QuadVertices = 4;
inline constexpr std::array<u32, 6> QuadTriangleCorners{0, 1, 3, 3, 1, 2};

/// Whole primitives of a list. A restart index drops the primitive it interrupts and begins the next.
template <u32 Vertices, typename Index, typename Emit>
void AssembleList(std::span<const Index> indices, bool restart, Emit emit) {
    std::array<u32, Vertices> primitive{};
    u32 held = 0;
    for (const Index index : indices) {
        if (restart && index == std::numeric_limits<Index>::max()) {
            held = 0;
            continue;
        }
        primitive[held++] = index;
        if (held == Vertices) {
            emit(primitive);
            held = 0;
        }
    }
}

/// The two triangles of the strip 0, 1, 3, 2: the order the desktop geometry shader emits.
template <typename Index>
void ExpandQuadIndices(std::span<const Index> quads, bool restart, std::vector<u32>& out) {
    out.clear();
    AssembleList<QuadVertices>(quads, restart, [&out](const std::array<u32, QuadVertices>& quad) {
        for (const u32 corner : QuadTriangleCorners) {
            out.push_back(quad[corner]);
        }
    });
}

inline void ExpandSequentialQuads(u32 vertices, std::vector<u32>& out) {
    out.clear();
    for (u32 first = 0; vertices - first >= QuadVertices; first += QuadVertices) {
        for (const u32 corner : QuadTriangleCorners) {
            out.push_back(first + corner);
        }
    }
}

/// Only indexed rectangle lists take the table path: non-indexed ones keep the attribute path.
inline constexpr bool RectListUsesTables(bool rect_list, bool is_indexed, bool tables_enabled) {
    return rect_list && is_indexed && tables_enabled;
}

/// Three guest vertices per rectangle, each `index + first_vertex` as Vulkan's vertex index is.
/// Indices past the last whole rectangle draw nothing, as in a three-vertex patch list.
template <typename Index>
void AssembleRectVertices(std::span<const Index> indices, bool restart, u32 first_vertex,
                          std::vector<u32>& out) {
    out.clear();
    AssembleList<RectVerticesPerRect>(indices, restart,
                                      [&](const std::array<u32, RectVerticesPerRect>& rect) {
                                          for (const u32 index : rect) {
                                              out.push_back(index + first_vertex);
                                          }
                                      });
}

/// What can be read of `requested` indices when only `readable_bytes` of them are mapped.
inline u32 ReadableIndexCount(u32 requested, u64 readable_bytes, u32 index_size) {
    return u32(std::min<u64>(requested, readable_bytes / index_size));
}

/// Empty when the draw has no index to read, or only restart indices.
template <typename Index>
std::optional<u32> HighestIndex(std::span<const Index> indices, bool restart) {
    std::optional<u32> highest;
    for (const Index index : indices) {
        if (restart && index == std::numeric_limits<Index>::max()) {
            continue;
        }
        highest = std::max<u32>(highest.value_or(0), index);
    }
    return highest;
}

/// The last element a read may name and stay inside `capacity`; empty when not even the first fits.
inline std::optional<u64> HighestElementInside(u64 start, u64 element_bytes, u64 stride, u64 capacity) {
    if (start + element_bytes > capacity) {
        return std::nullopt;
    }
    return stride == 0 ? std::numeric_limits<u64>::max() : (capacity - start - element_bytes) / stride;
}

/// One strip instance per rectangle of each guest instance; empty when a draw cannot hold that many.
inline std::optional<s32> RectDrawInstances(u32 rectangles, u32 guest_instances) {
    const u64 total = u64(rectangles) * guest_instances;
    if (total > u64(std::numeric_limits<s32>::max())) {
        return std::nullopt;
    }
    return s32(total);
}

/// Elements of a per-instance table. Never more than one past what guest memory stores: that last
/// one reads as missing data, and the shader stops there.
inline u32 RectInstanceElements(u32 guest_instances, u32 divisor, u64 stored_elements) {
    return u32(std::min<u64>((guest_instances - 1) / divisor, stored_elements)) + 1;
}

} // namespace OpenGL
