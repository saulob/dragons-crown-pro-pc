// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <xxhash.h>

#include "core/libraries/videoout/buffer.h"
#include "core/memory.h"
#include "shader_recompiler/resource.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_opengl/gl_capabilities.h"
#include "video_core/renderer_opengl/gl_detile.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/renderer_opengl/gl_texture_cache.h"

namespace OpenGL {
namespace {

constexpr u64 UnusedFramesBeforeEviction = 1800;
constexpr u32 TileRows = 8;

enum class ResolvedKind : u64 { Texture = 1, Color = 2, Depth = 3 };

u64 ResolveKey(ResolvedKind kind, const void* descriptor, size_t size, u64 extra) {
    return XXH3_64bits_withSeed(descriptor, size, extra << 2 | u64(kind));
}

bool IsSrgbFormat(vk::Format format) {
    return vk::to_string(format).find("Srgb") != std::string::npos;
}

u64 ReadableSize(VAddr address, u64 size) {
    return Core::Memory::Instance()->ClampRangeSize(address, size);
}

} // namespace

GLenum ImageTarget(AmdGpu::ImageType type) {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
        return GL_TEXTURE_1D;
    case AmdGpu::ImageType::Color1DArray:
        return GL_TEXTURE_1D_ARRAY;
    case AmdGpu::ImageType::Color2DArray:
        return GL_TEXTURE_2D_ARRAY;
    case AmdGpu::ImageType::Color3D:
        return GL_TEXTURE_3D;
    default:
        return GL_TEXTURE_2D;
    }
}

TextureCache::TextureCache(GlState& state_, GuestWriteWatch& watch_, const Capabilities& capabilities_)
    : state{state_}, watch{watch_}, capabilities{capabilities_}, gles{capabilities_.gles},
      resolved(ResolvedSlots) {}

TextureCache::~TextureCache() {
    for (auto& [address, list] : images) {
        for (auto& image : list) {
            gl::DeleteTextures(1, &image->handle);
        }
    }
}

void TextureCache::Allocate(Image& image) {
    const auto& info = image.info;
    const auto* format = image.format;
    gl::GenTextures(1, &image.handle);
    state.BindForEdit(image.target, image.handle);
    const u32 levels = std::max(info.resources.levels, 1u);
    gl::TexParameteri(image.target, GL_TEXTURE_BASE_LEVEL, 0);
    gl::TexParameteri(image.target, GL_TEXTURE_MAX_LEVEL, GLint(levels - 1));
    image.view.max_level = GLint(levels - 1);
    if (gles) {
        // A fetch has no sampler: OpenGL ES calls integer and depth images incomplete under the defaults.
        gl::TexParameteri(image.target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl::TexParameteri(image.target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    const u32 layers = image.target == GL_TEXTURE_3D ? info.size.depth : info.resources.layers;
    for (u32 level = 0; level < levels; ++level) {
        const GLsizei width = GLsizei(std::max(info.size.width >> level, 1u));
        const GLsizei height = GLsizei(std::max(info.size.height >> level, 1u));
        const GLsizei depth = image.target == GL_TEXTURE_3D
                                  ? GLsizei(std::max(info.size.depth >> level, 1u))
                                  : GLsizei(layers);
        if (format->compressed) {
            const GLsizei blocks = ((width + 3) / 4) * ((height + 3) / 4);
            const GLsizei bytes = blocks * GLsizei(info.num_bits / 8);
            if (image.target == GL_TEXTURE_2D) {
                gl::CompressedTexImage2D(GL_TEXTURE_2D, GLint(level), format->internal_format,
                                         width, height, 0, bytes, nullptr);
            } else {
                gl::CompressedTexImage3D(image.target, GLint(level), format->internal_format, width,
                                         height, depth, 0, bytes * depth, nullptr);
            }
            continue;
        }
        const GLint internal = GLint(format->internal_format);
        switch (image.target) {
        case GL_TEXTURE_1D:
            gl::TexImage1D(GL_TEXTURE_1D, GLint(level), internal, width, 0, format->format,
                           format->type, nullptr);
            break;
        case GL_TEXTURE_1D_ARRAY:
            gl::TexImage2D(GL_TEXTURE_1D_ARRAY, GLint(level), internal, width, depth, 0,
                           format->format, format->type, nullptr);
            break;
        case GL_TEXTURE_2D:
            gl::TexImage2D(GL_TEXTURE_2D, GLint(level), internal, width, height, 0, format->format,
                           format->type, nullptr);
            break;
        default:
            gl::TexImage3D(image.target, GLint(level), internal, width, height, depth, 0,
                           format->format, format->type, nullptr);
            break;
        }
    }
}

Image* TextureCache::Create(const VideoCore::ImageInfo& info, GLenum target) {
    const auto* format = FindTextureFormat(info.pixel_format, gles);
    // ANGLE's Direct3D backends refuse a block compressed base level that is not whole blocks.
    const bool unusable = gles && format &&
                          (!FormatAvailable(*format, capabilities) ||
                           (format->compressed && (info.size.width % 4 != 0 || info.size.height % 4 != 0)));
    if (!format || unusable) {
        if (reported_formats.insert(u32(info.pixel_format)).second) {
            std::printf("OpenGL: unsupported image format %s at %#llx; its draws use a black "
                        "texture\n",
                        vk::to_string(info.pixel_format).c_str(),
                        static_cast<unsigned long long>(info.guest_address));
        }
        return nullptr;
    }
    auto image = std::make_unique<Image>();
    image->info = info;
    image->target = target;
    image->format = format;
    image->is_srgb = IsSrgbFormat(info.pixel_format);
    Allocate(*image);
    image->last_used_frame = frame;
    auto& list = images[info.guest_address];
    list.push_back(std::move(image));
    ++resolve_generation;
    return list.back().get();
}

Image* TextureCache::Resolved(u64 key) {
    const ResolvedImage& slot = resolved[key & (ResolvedSlots - 1)];
    const bool hit = slot.key == key && slot.generation == resolve_generation;
    return hit ? slot.image : nullptr;
}

void TextureCache::Remember(u64 key, Image* image) {
    resolved[key & (ResolvedSlots - 1)] = {key, resolve_generation, image};
}

Image* TextureCache::Find(const VideoCore::ImageInfo& info, GLenum target, bool is_target) {
    const auto it = images.find(info.guest_address);
    if (it != images.end()) {
        Image* exact = nullptr;
        for (auto& candidate : it->second) {
            const auto& known = candidate->info;
            if (known.size.width != info.size.width || candidate->target != target ||
                (is_target && known.props.is_depth != info.props.is_depth)) {
                continue;
            }
            if (known.size.height == info.size.height &&
                known.pixel_format == info.pixel_format &&
                known.resources.levels == info.resources.levels) {
                if (candidate->gpu_written) {
                    return candidate.get();
                }
                exact = candidate.get();
                continue;
            }
            // A target's height can be the tile-aligned one while its texture view has the real.
            const u32 height_gap = std::max(known.size.height, info.size.height) -
                                   std::min(known.size.height, info.size.height);
            const bool depth_view = known.props.is_depth && info.props.is_depth;
            if (candidate->gpu_written && height_gap <= TileRows &&
                (known.num_bits == info.num_bits || depth_view)) {
                return candidate.get();
            }
        }
        if (exact) {
            return exact;
        }
    }
    return Create(info, target);
}

void TextureCache::Refresh(Image& image) {
    image.last_used_frame = frame;
    if (image.checked_frame == frame) {
        return;
    }
    image.checked_frame = frame;
    const auto& info = image.info;
    const u64 size = ReadableSize(info.guest_address, info.guest_size);
    if (size == 0) {
        return;
    }
    // A partly mapped image could gain pages the watch never saw: those stay hashed each frame.
    const bool watchable = size == info.guest_size &&
                           info.guest_address + size <= GuestWriteWatch::AddressLimit;
    if (watchable && image.watched &&
        !watch.WrittenSince(info.guest_address, size, image.watch_stamp)) {
        return;
    }
    image.watched = false;
    if (watchable) {
        if (const auto stamp = watch.TryArm(info.guest_address, size)) {
            image.watch_stamp = *stamp;
            image.watched = true;
        }
    }
    const auto* guest = reinterpret_cast<const u8*>(info.guest_address);
    const u64 hash = image.gpu_written ? HashGuestRange(guest, size) : XXH3_64bits(guest, size);
    if (hash == image.content_hash) {
        return;
    }
    image.content_hash = hash;
    if (!info.props.is_depth) {
        Upload(image);
    }
    if (image.gpu_written) {
        ++resolve_generation;
    }
    image.gpu_written = false;
}

// From here on the texture, not guest memory, holds the image; only a write the guest makes
// over it afterwards brings guest memory back (Refresh).
void TextureCache::MarkTarget(Image& image) {
    if (image.gpu_written) {
        return;
    }
    image.gpu_written = true;
    ++resolve_generation;
    const auto& info = image.info;
    const u64 size = ReadableSize(info.guest_address, info.guest_size);
    image.content_hash =
        size ? HashGuestRange(reinterpret_cast<const u8*>(info.guest_address), size) : 0;
}

// Refresh would see this only at the next frame's first use, or not at all when the bytes match
// last time's, and would then replace whatever was rendered after the write.
void TextureCache::NoteGpuWrite(VAddr address, u64 size) {
    const VAddr end = address + size;
    for (auto& [base, list] : images) {
        for (auto& image : list) {
            const auto& info = image->info;
            const VAddr image_end = info.guest_address + info.guest_size;
            if (info.guest_address >= end || image_end <= address) {
                continue;
            }
            if (!image->gpu_written) {
                image->checked_frame = ~0ull;
                image->content_hash = 0;
                image->watched = false;
                continue;
            }
            if (address <= info.guest_address && end >= image_end && !info.props.is_depth) {
                Upload(*image);
            }
            const u64 readable = ReadableSize(info.guest_address, info.guest_size);
            image->content_hash =
                readable ? HashGuestRange(reinterpret_cast<const u8*>(info.guest_address), readable)
                         : 0;
            image->checked_frame = frame;
            image->watched = false;
        }
    }
}

void TextureCache::UploadLevel(Image& image, u32 level, const u8* data, u32 pitch, u32 rows) {
    const auto& info = image.info;
    const auto* format = image.format;
    const u32 element_bytes = info.num_bits / 8;
    const GLsizei width = GLsizei(std::max(info.size.width >> level, 1u));
    const GLsizei height = GLsizei(std::max(info.size.height >> level, 1u));
    const u32 layers = image.target == GL_TEXTURE_3D ? std::max(info.size.depth >> level, 1u)
                       : image.target == GL_TEXTURE_2D ? 1u
                                                       : u32(info.resources.layers);
    if (format->compressed) {
        const u32 block_columns = (u32(width) + 3) / 4, block_rows = (u32(height) + 3) / 4;
        const size_t row_bytes = size_t(block_columns) * element_bytes;
        tight.resize(row_bytes * block_rows * layers);
        for (u32 layer = 0; layer < layers; ++layer) {
            const u8* source = data + size_t(pitch) * rows * element_bytes * layer;
            u8* out = tight.data() + row_bytes * block_rows * layer;
            for (u32 row = 0; row < block_rows; ++row) {
                std::memcpy(out + row * row_bytes, source + size_t(row) * pitch * element_bytes,
                            row_bytes);
            }
        }
        const GLsizei bytes = GLsizei(tight.size());
        if (image.target == GL_TEXTURE_2D) {
            gl::CompressedTexSubImage2D(GL_TEXTURE_2D, GLint(level), 0, 0, width, height,
                                        format->internal_format, bytes, tight.data());
        } else {
            gl::CompressedTexSubImage3D(image.target, GLint(level), 0, 0, 0, width, height,
                                        GLsizei(layers), format->internal_format, bytes,
                                        tight.data());
        }
        return;
    }
    if (format->conversion != TexelConversion::None) {
        const size_t bytes = size_t(pitch) * rows * layers * element_bytes;
        converted.assign(data, data + bytes);
        ConvertTexels(format->conversion, converted.data(), bytes);
        data = converted.data();
    }
    gl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl::PixelStorei(GL_UNPACK_ROW_LENGTH, GLint(pitch));
    gl::PixelStorei(GL_UNPACK_IMAGE_HEIGHT, GLint(rows));
    switch (image.target) {
    case GL_TEXTURE_1D:
        gl::TexSubImage1D(GL_TEXTURE_1D, GLint(level), 0, width, format->format, format->type,
                          data);
        break;
    case GL_TEXTURE_1D_ARRAY:
        gl::TexSubImage2D(GL_TEXTURE_1D_ARRAY, GLint(level), 0, 0, width, GLsizei(layers),
                          format->format, format->type, data);
        break;
    case GL_TEXTURE_2D:
        gl::TexSubImage2D(GL_TEXTURE_2D, GLint(level), 0, 0, width, height, format->format,
                          format->type, data);
        break;
    default:
        gl::TexSubImage3D(image.target, GLint(level), 0, 0, 0, width, height, GLsizei(layers),
                          format->format, format->type, data);
        break;
    }
    gl::PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl::PixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
}

void TextureCache::Upload(Image& image) {
    const auto& info = image.info;
    const u32 block = info.props.is_block ? 4 : 1;
    const u32 element_bytes = info.num_bits / 8;
    const auto* guest = reinterpret_cast<const u8*>(info.guest_address);
    const u32 slices = info.props.is_volume ? info.size.depth : info.resources.layers;
    const TilingParams tiling{info.tile_mode, info.array_mode, info.num_bits, info.num_samples,
                              info.alt_tile,  info.bank_swizzle};
    state.BindForEdit(image.target, image.handle);
    for (u32 level = 0; level < info.resources.levels; ++level) {
        const auto& layout = info.mips_layout[level];
        const u32 pitch = std::max((layout.pitch + block - 1) / block, 1u);
        u32 rows = std::max((layout.height + block - 1) / block, 1u);
        if (!info.props.is_tiled) {
            // layout.height is padded for block images; layout.size has the real row count.
            const u32 level_slices =
                info.props.is_volume ? std::max(info.size.depth >> level, 1u) : info.resources.layers;
            rows = std::max(u32(layout.size / (u64(pitch) * element_bytes * level_slices)), 1u);
        }
        const u8* source = guest + layout.offset;
        const u64 size = ReadableSize(VAddr(source), layout.size);
        if (size == 0) {
            continue;
        }
        if (info.props.is_tiled) {
            scratch.resize(size_t(pitch) * rows * element_bytes * slices);
            SurfaceTiler{tiling, pitch, rows}.Detile(source, size, scratch.data(), slices);
            UploadLevel(image, level, scratch.data(), pitch, rows);
        } else {
            if (size < u64(pitch) * rows * element_bytes) {
                continue;
            }
            UploadLevel(image, level, source, pitch, rows);
        }
    }
}

Image* TextureCache::FindTexture(const AmdGpu::Image& sharp, const Shader::ImageResource& desc) {
    const u64 key = ResolveKey(ResolvedKind::Texture, &sharp, sizeof(sharp),
                               u64(desc.is_depth) | u64(desc.is_array) << 1);
    Image* image = Resolved(key);
    if (!image) {
        const VideoCore::ImageInfo info{sharp, desc};
        image = Find(info, ImageTarget(sharp.GetViewType(desc.is_array)), false);
        if (image) {
            Remember(key, image);
        }
    }
    if (image) {
        Refresh(*image);
    }
    return image;
}

void TextureCache::ApplyView(Image& image, GLuint unit, const SampledView& view) {
    if (image.view == view) {
        return;
    }
    state.ActiveTexture(unit);
    if (image.view.swizzle != view.swizzle) {
        if (gles) {
            gl::TexParameteri(image.target, GL_TEXTURE_SWIZZLE_R, view.swizzle[0]);
            gl::TexParameteri(image.target, GL_TEXTURE_SWIZZLE_G, view.swizzle[1]);
            gl::TexParameteri(image.target, GL_TEXTURE_SWIZZLE_B, view.swizzle[2]);
            gl::TexParameteri(image.target, GL_TEXTURE_SWIZZLE_A, view.swizzle[3]);
        } else {
            gl::TexParameteriv(image.target, GL_TEXTURE_SWIZZLE_RGBA, view.swizzle.data());
        }
    }
    if (image.view.base_level != view.base_level) {
        gl::TexParameteri(image.target, GL_TEXTURE_BASE_LEVEL, view.base_level);
    }
    if (image.view.max_level != view.max_level) {
        gl::TexParameteri(image.target, GL_TEXTURE_MAX_LEVEL, view.max_level);
    }
    image.view = view;
}

Image* TextureCache::FindColorTarget(const AmdGpu::ColorBuffer& buffer, AmdGpu::CbDbExtent hint) {
    const u64 key = ResolveKey(ResolvedKind::Color, &buffer, sizeof(buffer), hint.raw);
    Image* image = Resolved(key);
    if (!image) {
        const VideoCore::ImageInfo info{buffer, hint};
        image = Find(info, GL_TEXTURE_2D, true);
        if (image) {
            Remember(key, image);
        }
    }
    if (image) {
        Refresh(*image);
        MarkTarget(*image);
    }
    return image;
}

Image* TextureCache::FindDepthTarget(const AmdGpu::DepthBuffer& buffer, u32 num_slices,
                                     VAddr htile_address, AmdGpu::CbDbExtent hint) {
    const u64 key = ResolveKey(ResolvedKind::Depth, &buffer, sizeof(buffer),
                               (u64(hint.raw) | u64(num_slices) << 32) ^ htile_address);
    Image* image = Resolved(key);
    if (!image) {
        const VideoCore::ImageInfo info{buffer, num_slices, htile_address, hint};
        image = Find(info, GL_TEXTURE_2D, true);
        if (image) {
            Remember(key, image);
        }
    }
    if (image) {
        Refresh(*image);
        MarkTarget(*image);
    }
    return image;
}

Image* TextureCache::FindDisplayBuffer(const Libraries::VideoOut::BufferAttributeGroup& group,
                                       VAddr address) {
    const VideoCore::ImageInfo info{group, address};
    Image* image = nullptr;
    if (const auto it = images.find(address); it != images.end()) {
        for (auto& candidate : it->second) {
            if (candidate->gpu_written && candidate->target == GL_TEXTURE_2D) {
                image = candidate.get();
            }
        }
    }
    image = image ? image : Find(info, GL_TEXTURE_2D, false);
    if (image) {
        Refresh(*image);
    }
    return image;
}

Image* TextureCache::FindAt(VAddr address, u64 size) {
    const auto it = images.find(address);
    if (it == images.end()) {
        return nullptr;
    }
    for (auto& image : it->second) {
        if (image->gpu_written && image->info.guest_size == size) {
            return image.get();
        }
    }
    return nullptr;
}

void TextureCache::QueueUnmap(VAddr address, u64 size) {
    std::scoped_lock lock{unmap_mutex};
    pending_unmaps.emplace_back(address, size);
}

void TextureCache::Sweep() {
    std::vector<std::pair<VAddr, u64>> unmaps;
    {
        std::scoped_lock lock{unmap_mutex};
        unmaps.swap(pending_unmaps);
    }
    const auto dead = [&](const Image& image) {
        if (frame - image.last_used_frame > UnusedFramesBeforeEviction) {
            return true;
        }
        const VAddr begin = image.info.guest_address;
        const VAddr end = begin + image.info.guest_size;
        return std::ranges::any_of(unmaps, [&](const auto& range) {
            return begin < range.first + range.second && range.first < end;
        });
    };
    bool destroyed = false;
    for (auto it = images.begin(); it != images.end();) {
        auto& list = it->second;
        for (auto image = list.begin(); image != list.end();) {
            if (!dead(**image)) {
                ++image;
                continue;
            }
            if (on_destroy) {
                on_destroy((*image)->handle);
            }
            destroyed = true;
            gl::DeleteTextures(1, &(*image)->handle);
            image = list.erase(image);
        }
        it = list.empty() ? images.erase(it) : std::next(it);
    }
    if (destroyed) {
        ++resolve_generation;
        state.ForgetTextures();
    }
}

void TextureCache::NextFrame() {
    ++frame;
    bool has_unmaps;
    {
        std::scoped_lock lock{unmap_mutex};
        has_unmaps = !pending_unmaps.empty();
    }
    if (has_unmaps || (frame & 255) == 0) {
        Sweep();
    }
}

void TextureCache::RegisterMeta(VAddr address) {
    if (address != 0) {
        meta.try_emplace(address, false);
    }
}

bool TextureCache::ClearMeta(VAddr address) {
    const auto it = meta.find(address);
    if (it == meta.end()) {
        return false;
    }
    it->second = true;
    return true;
}

bool TextureCache::IsMeta(VAddr address) const {
    return meta.contains(address);
}

bool TextureCache::IsMetaCleared(VAddr address) const {
    const auto it = meta.find(address);
    return it != meta.end() && it->second;
}

void TextureCache::TouchMeta(VAddr address) {
    if (const auto it = meta.find(address); it != meta.end()) {
        it->second = false;
    }
}

} // namespace OpenGL
