// SPDX-License-Identifier: GPL-2.0-or-later
// Guest images as OpenGL textures. Guest memory is checked by content, not by page protection:
// this path has no page tracking, so a texture is hashed the first time a frame uses it.
#pragma once

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
#include "video_core/texture_cache/image_info.h"

namespace Libraries::VideoOut {
struct BufferAttributeGroup;
}

namespace Shader {
struct ImageResource;
}

namespace OpenGL {

struct Image {
    VideoCore::ImageInfo info;
    GLuint handle{};
    GLenum target{};
    const TextureFormat* format{};
    bool is_srgb{};
    bool gpu_written{};
    u64 content_hash{};
    u64 checked_frame = ~0ull;
    u64 last_used_frame{};
};

GLenum ImageTarget(AmdGpu::ImageType type);

/// The whole of a small range, three windows of a large one: reused many times per frame.
u64 HashGuestRange(const u8* data, u64 size);

class TextureCache {
public:
    TextureCache();
    ~TextureCache();

    /// Null for a format the backend does not map (logged once).
    Image* FindTexture(const AmdGpu::Image& sharp, const Shader::ImageResource& desc);
    Image* FindColorTarget(const AmdGpu::ColorBuffer& buffer, AmdGpu::CbDbExtent hint);
    Image* FindDepthTarget(const AmdGpu::DepthBuffer& buffer, u32 num_slices, VAddr htile_address,
                           AmdGpu::CbDbExtent hint);
    Image* FindDisplayBuffer(const Libraries::VideoOut::BufferAttributeGroup& group, VAddr address);
    /// An image already at this address, whatever its format.
    Image* FindAt(VAddr address, u64 size);

    /// Any thread: guest memory went away; the images over it die on the GPU thread.
    void QueueUnmap(VAddr address, u64 size);
    /// A write made on the GPU's behalf, in order with the draws.
    void NoteGpuWrite(VAddr address, u64 size);
    void NextFrame();

    /// Called before an image's texture is deleted (framebuffers refer to it).
    void SetDestroyCallback(std::function<void()> callback) {
        on_destroy = std::move(callback);
    }

    void RegisterMeta(VAddr address);
    bool ClearMeta(VAddr address);
    bool IsMeta(VAddr address) const;
    bool IsMetaCleared(VAddr address) const;
    void TouchMeta(VAddr address);

private:
    Image* Find(const VideoCore::ImageInfo& info, GLenum target, bool is_target);
    Image* Create(const VideoCore::ImageInfo& info, GLenum target);
    void Allocate(Image& image);
    void Refresh(Image& image);
    void MarkTarget(Image& image);
    void Upload(Image& image);
    void UploadLevel(Image& image, u32 level, const u8* data, u32 pitch, u32 rows);
    void Sweep();

    std::unordered_map<VAddr, std::vector<std::unique_ptr<Image>>> images;
    std::vector<u8> scratch;
    std::vector<u8> tight;
    std::unordered_map<VAddr, bool> meta;
    std::unordered_set<u32> reported_formats;
    std::function<void()> on_destroy;
    u64 frame{};
    std::mutex unmap_mutex;
    std::vector<std::pair<VAddr, u64>> pending_unmaps;
};

} // namespace OpenGL
