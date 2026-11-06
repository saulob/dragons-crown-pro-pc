// SPDX-License-Identifier: GPL-2.0-or-later
// One buffer shared by vertex, index and shader data; shaders read it as a buffer texture because
// OpenGL 3.3 has no storage buffers. OpenGL ES has neither, and ANGLE's Direct3D 11 backend copies a
// whole buffer between its internal copies when any of it changes: there vertices and indices are
// client memory it streams itself, and a second ring feeds shaders from a 2D integer texture.
#pragma once

#include <array>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_opengl/gl_api.h"

namespace OpenGL {

class GlState;

/// Persistent needs ARB_buffer_storage: data goes straight into a buffer that stays mapped, and
/// fences keep it from overtaking the GPU. Mapped: each draw maps what it wrote.
enum class RingStorage : u8 { Persistent, Mapped, ClientMemory, ShaderTexture };

class StreamRing {
public:
    StreamRing(GlState& state, RingStorage storage);
    StreamRing(GlState& state, bool persistent)
        : StreamRing(state, persistent ? RingStorage::Persistent : RingStorage::Mapped) {}
    ~StreamRing();

    /// Before a draw's first Push: its offsets stay valid until the draw is issued.
    void BeginDraw();
    /// The offset of a copy of `data`, or NoOffset when it cannot fit.
    u32 Push(const void* data, u32 size, u32 alignment);
    /// A copy of guest memory, shared by the draws of one submission.
    u32 PushGuest(VAddr address, u32 size, u32 alignment);
    /// Room to write into, for a ring in client memory; null when it cannot fit.
    u8* Reserve(u32 size, u32 alignment, u32& offset);
    void Commit();
    void ForgetGuestCopies();

    [[nodiscard]] u32 Capacity() const {
        return capacity;
    }
    [[nodiscard]] GLuint Buffer() const {
        return buffer;
    }
    [[nodiscard]] GLuint Texture() const {
        return texture;
    }
    [[nodiscard]] GLenum TextureTarget() const {
        return storage == RingStorage::ShaderTexture ? GL_TEXTURE_2D : GL_TEXTURE_BUFFER;
    }
    /// Added to an offset to get what vertex and index pointers take: client memory has no buffer.
    [[nodiscard]] u64 PointerBase() const {
        return storage == RingStorage::ClientMemory ? u64(reinterpret_cast<uintptr_t>(client.data())) : 0;
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
    void CreateShaderTexture();
    void UploadPending();

    GlState& state;
    RingStorage storage;
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
    std::vector<u8> client;
    std::vector<GuestCopy> guest_copies;
    u32 copy_generation{1};
};

} // namespace OpenGL
