// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>

#include "common/alignment.h"
#include "shader_recompiler/backend/glsl/ring_layout.h"
#include "video_core/renderer_opengl/gl_guest_watch.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/renderer_opengl/gl_stream_ring.h"

namespace OpenGL {
namespace {

constexpr u32 RingBytes = 64 * 1024 * 1024;
constexpr GLbitfield PersistentFlags =
    GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
constexpr GLuint64 FenceWaitNanoseconds = 1'000'000'000;
constexpr u32 ShaderRowTexels = 1u << Shader::Backend::GLSL::RingRowBits;
constexpr u32 ShaderTexelBytes = sizeof(u32);

} // namespace

StreamRing::StreamRing(GlState& state_, RingStorage storage_)
    : state{state_}, storage{storage_}, guest_copies(GuestCopySlots) {
    if (storage == RingStorage::ClientMemory || storage == RingStorage::ShaderTexture) {
        capacity = RingBytes;
        segment_bytes = capacity / SegmentCount;
        client.resize(capacity);
        if (storage == RingStorage::ShaderTexture) {
            CreateShaderTexture();
        }
        return;
    }
    const bool persistent = storage == RingStorage::Persistent;
    GLint max_texels = 0;
    gl::GetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &max_texels);
    segment_bytes = u32(std::min<u64>(RingBytes, u64(max_texels) * 4) / SegmentCount);
    capacity = segment_bytes * SegmentCount;
    gl::GenBuffers(1, &buffer);
    state.BindArrayBuffer(buffer);
    if (persistent) {
        gl::BufferStorage(GL_ARRAY_BUFFER, capacity, nullptr, PersistentFlags);
        mapped = static_cast<u8*>(gl::MapBufferRange(GL_ARRAY_BUFFER, 0, capacity, PersistentFlags));
        if (!mapped) {
            state.BindArrayBuffer(0);
            gl::DeleteBuffers(1, &buffer);
            gl::GenBuffers(1, &buffer);
            state.BindArrayBuffer(buffer);
        }
    }
    if (!mapped) {
        gl::BufferData(GL_ARRAY_BUFFER, capacity, nullptr, GL_STREAM_DRAW);
    }
    gl::GenTextures(1, &texture);
    state.BindTexture(0, GL_TEXTURE_BUFFER, texture);
    gl::TexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, buffer);
}

void StreamRing::CreateShaderTexture() {
    gl::GenTextures(1, &texture);
    state.BindForEdit(GL_TEXTURE_2D, texture);
    const u32 rows = capacity / ShaderTexelBytes / ShaderRowTexels;
    gl::TexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, GLsizei(ShaderRowTexels), GLsizei(rows));
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

StreamRing::~StreamRing() {
    for (GLsync fence : fences) {
        if (fence) {
            gl::DeleteSync(fence);
        }
    }
    if (mapped) {
        state.BindArrayBuffer(buffer);
        gl::UnmapBuffer(GL_ARRAY_BUFFER);
    }
    gl::DeleteTextures(1, &texture);
    gl::DeleteBuffers(1, &buffer);
}

void StreamRing::FenceSegments(u32 end) {
    for (; fenced_segments < end; ++fenced_segments) {
        GLsync& fence = fences[fenced_segments];
        if (fence) {
            gl::DeleteSync(fence);
        }
        fence = gl::FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }
}

void StreamRing::AcquireThrough(u32 end) {
    for (; acquired_end < end; acquired_end += segment_bytes) {
        GLsync& fence = fences[acquired_end / segment_bytes];
        if (!fence) {
            continue;
        }
        GLenum result;
        do {
            result = gl::ClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, FenceWaitNanoseconds);
        } while (result == GL_TIMEOUT_EXPIRED);
        gl::DeleteSync(fence);
        fence = nullptr;
    }
}

void StreamRing::Wrap() {
    if (mapped) {
        FenceSegments((head + segment_bytes - 1) / segment_bytes);
        fenced_segments = 0;
        acquired_end = 0;
    } else if (client.empty()) {
        state.BindArrayBuffer(buffer);
        gl::BufferData(GL_ARRAY_BUFFER, capacity, nullptr, GL_STREAM_DRAW);
    }
    head = 0;
    pending_start = 0;
    ForgetGuestCopies();
}

void StreamRing::BeginDraw() {
    if (mapped) {
        FenceSegments(head / segment_bytes);
    }
    if (head > capacity - capacity / 3) {
        Wrap();
    }
}

u32 StreamRing::Push(const void* data, u32 size, u32 alignment) {
    const u32 offset = Common::AlignUp(head, alignment);
    if (u64(offset) + size > capacity) {
        return NoOffset;
    }
    if (!client.empty()) {
        std::memcpy(client.data() + offset, data, size);
    } else if (mapped) {
        AcquireThrough(offset + size);
        std::memcpy(mapped + offset, data, size);
    } else {
        pending.resize(offset + size - pending_start);
        std::memcpy(pending.data() + (offset - pending_start), data, size);
    }
    head = offset + size;
    return offset;
}

u8* StreamRing::Reserve(u32 size, u32 alignment, u32& offset) {
    offset = Common::AlignUp(head, alignment);
    if (client.empty() || u64(offset) + size > capacity) {
        offset = NoOffset;
        return nullptr;
    }
    head = offset + size;
    return client.data() + offset;
}

u32 StreamRing::PushGuest(VAddr address, u32 size, u32 alignment) {
    // By content: the guest and the command stream can rewrite a buffer between two draws.
    const auto* data = reinterpret_cast<const u8*>(address);
    u64 hash;
    hash = HashGuestRange(data, size);
    const u64 key = hash ^ (address * 0x9E3779B97F4A7C15ull) ^ (u64(size) << 32);
    GuestCopy& copy = guest_copies[key & (GuestCopySlots - 1)];
    // A fence over the copy's segment predates this draw, so it would not protect the copy.
    if (copy.key == key && copy.generation == copy_generation && copy.offset % alignment == 0 &&
        (!mapped || copy.offset >= fenced_segments * segment_bytes)) {
        return copy.offset;
    }
    const u32 offset = Push(data, size, alignment);
    if (offset != NoOffset) {
        copy = {key, offset, copy_generation};
    }
    return offset;
}

// Uploads are in order with the draws, so a draw already issued keeps what it read and no fence is needed.
void StreamRing::UploadPending() {
    if (head == pending_start) {
        return;
    }
    const u32 first = pending_start / ShaderTexelBytes;
    const u32 last = (head + ShaderTexelBytes - 1) / ShaderTexelBytes;
    const u32 first_row = first / ShaderRowTexels;
    const u32 last_row = (last - 1) / ShaderRowTexels;
    // Several rows go up whole: the ring's memory holds what the rest of each row already has.
    const u32 column = first_row == last_row ? first % ShaderRowTexels : 0;
    const u32 width = first_row == last_row ? last - first : ShaderRowTexels;
    state.BindForEdit(GL_TEXTURE_2D, texture);
    gl::TexSubImage2D(GL_TEXTURE_2D, 0, GLint(column), GLint(first_row), GLsizei(width),
                      GLsizei(last_row - first_row + 1), GL_RED_INTEGER, GL_UNSIGNED_INT,
                      client.data() + (size_t(first_row) * ShaderRowTexels + column) * ShaderTexelBytes);
    pending_start = head;
}

void StreamRing::Commit() {
    if (storage == RingStorage::ClientMemory) {
        return;
    }
    if (storage == RingStorage::ShaderTexture) {
        UploadPending();
        return;
    }
    if (pending.empty()) {
        return;
    }
    state.BindArrayBuffer(buffer);
    void* target = gl::MapBufferRange(GL_ARRAY_BUFFER, pending_start, GLsizeiptr(pending.size()),
                                      GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT |
                                          GL_MAP_INVALIDATE_RANGE_BIT);
    if (target) {
        std::memcpy(target, pending.data(), pending.size());
        gl::UnmapBuffer(GL_ARRAY_BUFFER);
    }
    pending_start = head;
    pending.clear();
}

void StreamRing::ForgetGuestCopies() {
    ++copy_generation;
}

} // namespace OpenGL
