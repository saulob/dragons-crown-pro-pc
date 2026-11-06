// SPDX-License-Identifier: GPL-2.0-or-later
// Guest images as OpenGL textures. A write watch says whether guest memory may have changed, and
// only then is it hashed; the hash decides whether the texture is uploaded again.
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "video_core/amdgpu/cb_db_extent.h"
#include "video_core/amdgpu/regs.h"
#include "video_core/renderer_opengl/gl_api.h"
#include "video_core/renderer_opengl/gl_format.h"
#include "video_core/renderer_opengl/gl_guest_watch.h"
#include "video_core/texture_cache/image_info.h"

namespace Libraries::VideoOut {
struct BufferAttributeGroup;
}

namespace Shader {
struct ImageResource;
}

namespace OpenGL {

class GlState;
struct Capabilities;

/// Per-object sampling state: a texture has one, whatever view of it a draw asks for.
struct SampledView {
    std::array<GLint, 4> swizzle{GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
    GLint base_level{};
    GLint max_level{};
    bool operator==(const SampledView&) const = default;
};

struct Image {
    VideoCore::ImageInfo info;
    GLuint handle{};
    GLenum target{};
    const TextureFormat* format{};
    bool is_srgb{};
    bool gpu_written{};
    bool watched{};
    u64 watch_stamp{};
    u64 content_hash{};
    u64 checked_frame = ~0ull;
    u64 last_used_frame{};
    SampledView view{};
};

GLenum ImageTarget(AmdGpu::ImageType type);

class TextureCache {
public:
    TextureCache(GlState& state, GuestWriteWatch& watch, const Capabilities& capabilities);
    ~TextureCache();

    /// Null for a format the backend does not map (logged once).
    Image* FindTexture(const AmdGpu::Image& sharp, const Shader::ImageResource& desc);
    Image* FindColorTarget(const AmdGpu::ColorBuffer& buffer, AmdGpu::CbDbExtent hint);
    Image* FindDepthTarget(const AmdGpu::DepthBuffer& buffer, u32 num_slices, VAddr htile_address,
                           AmdGpu::CbDbExtent hint);
    Image* FindDisplayBuffer(const Libraries::VideoOut::BufferAttributeGroup& group, VAddr address);
    /// An image already at this address, whatever its format.
    Image* FindAt(VAddr address, u64 size);

    /// `image` must be bound to `unit`.
    void ApplyView(Image& image, GLuint unit, const SampledView& view);

    /// Any thread: guest memory went away; the images over it die on the GPU thread.
    void QueueUnmap(VAddr address, u64 size);
    /// A write made on the GPU's behalf, in order with the draws.
    void NoteGpuWrite(VAddr address, u64 size);
    void NextFrame();

    /// Called before an image's texture is deleted (framebuffers refer to it).
    void SetDestroyCallback(std::function<void(GLuint texture)> callback) {
        on_destroy = std::move(callback);
    }

    void RegisterMeta(VAddr address);
    bool ClearMeta(VAddr address);
    bool IsMeta(VAddr address) const;
    bool IsMetaCleared(VAddr address) const;
    void TouchMeta(VAddr address);

private:
    /// What a guest descriptor resolved to: building an ImageInfo per bind is the expensive part.
    struct ResolvedImage {
        u64 key;
        u64 generation;
        Image* image;
    };
    static constexpr u32 ResolvedSlots = 1u << 12;

    Image* Resolved(u64 key);
    void Remember(u64 key, Image* image);
    Image* Find(const VideoCore::ImageInfo& info, GLenum target, bool is_target);
    Image* Create(const VideoCore::ImageInfo& info, GLenum target);
    void Allocate(Image& image);
    void Refresh(Image& image);
    void MarkTarget(Image& image);
    void Upload(Image& image);
    void UploadLevel(Image& image, u32 level, const u8* data, u32 pitch, u32 rows);
    void Sweep();

    GlState& state;
    GuestWriteWatch& watch;
    const Capabilities& capabilities;
    bool gles;
    std::unordered_map<VAddr, std::vector<std::unique_ptr<Image>>> images;
    std::vector<ResolvedImage> resolved;
    u64 resolve_generation{1};
    std::vector<u8> scratch;
    std::vector<u8> tight;
    std::vector<u8> converted;
    std::unordered_map<VAddr, bool> meta;
    std::unordered_set<u32> reported_formats;
    std::function<void(GLuint)> on_destroy;
    u64 frame{};
    std::mutex unmap_mutex;
    std::vector<std::pair<VAddr, u64>> pending_unmaps;
};

} // namespace OpenGL
