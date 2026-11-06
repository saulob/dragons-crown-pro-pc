// SPDX-License-Identifier: GPL-2.0-or-later
// One buffer shared by vertex, index and shader data; shaders read it as a buffer texture because
// OpenGL 3.3 has no storage buffers.
#pragma once

#include <array>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_opengl/gl_api.h"

namespace OpenGL {

class GlState;

class StreamRing {
public:
    /// `persistent` needs ARB_buffer_storage: data goes straight into a buffer that stays mapped,
    /// and fences keep it from overtaking the GPU. Otherwise each draw maps what it wrote.
    StreamRing(GlState& state, bool persistent);
    ~StreamRing();

    /// Before a draw's first Push: its offsets stay valid until the draw is issued.
    void BeginDraw();
    /// The offset of a copy of `data`, or NoOffset when it cannot fit.
    u32 Push(const void* data, u32 size, u32 alignment);
    /// A copy of guest memory, shared by the draws of one submission.
    u32 PushGuest(VAddr address, u32 size, u32 alignment);
    void Commit();
    void ForgetGuestCopies();

    [[nodiscard]] GLuint Buffer() const {
        return buffer;
    }
    [[nodiscard]] GLuint Texture() const {
        return texture;
    }
    [[nodiscard]] bool Persistent() const {
        return mapped != nullptr;
    }

    static constexpr u32 NoOffset = ~0u;
    static constexpr u32 SegmentCount = 16;

private:
    struct GuestCopy {
        u64 key;
        u32 offset;
        u32 generation;
    };
    static constexpr u32 GuestCopySlots = 1u << 14;

    void Wrap();
    void FenceSegments(u32 end);
    void AcquireThrough(u32 end);

    GlState& state;
    GLuint buffer{};
    GLuint texture{};
    u8* mapped{};
    u32 capacity{};
    u32 segment_bytes{};
    u32 head{};
    u32 pending_start{};
    u32 fenced_segments{};
    u32 acquired_end{};
    std::array<GLsync, SegmentCount> fences{};
    std::vector<u8> pending;
    std::vector<GuestCopy> guest_copies;
    u32 copy_generation{1};
};

} // namespace OpenGL
