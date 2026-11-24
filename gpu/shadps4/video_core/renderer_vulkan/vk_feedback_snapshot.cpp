// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_feedback_snapshot.h"

#include <algorithm>

#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {

vk::ImageViewType SnapshotViewType(AmdGpu::ImageType type) {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
        return vk::ImageViewType::e1D;
    case AmdGpu::ImageType::Color1DArray:
        return vk::ImageViewType::e1DArray;
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
        return vk::ImageViewType::e2D;
    case AmdGpu::ImageType::Color2DArray:
    case AmdGpu::ImageType::Color2DMsaaArray:
        return vk::ImageViewType::e2DArray;
    case AmdGpu::ImageType::Color3D:
        return vk::ImageViewType::e3D;
    default:
        return vk::ImageViewType::e2D;
    }
}

vk::ImageCreateFlags CompatibleSnapshotFlags(vk::ImageCreateFlags source_flags) {
    constexpr auto mask = vk::ImageCreateFlagBits::eMutableFormat |
                          vk::ImageCreateFlagBits::eCubeCompatible |
                          vk::ImageCreateFlagBits::e2DArrayCompatible;
    return (source_flags & mask) | vk::ImageCreateFlagBits::eMutableFormat;
}

} // namespace

FeedbackSnapshots::FeedbackSnapshots(const Instance& instance_, Scheduler& scheduler_,
                                     Runtime& runtime_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_} {}

FeedbackSnapshots::~FeedbackSnapshots() {
    if (!entries.empty()) {
        scheduler.Finish();
    }
}

FeedbackSnapshots::Entry& FeedbackSnapshots::AcquireEntry(const Key& key,
                                                          VideoCore::ImageId source_id,
                                                          bool& already_captured) {
    for (auto& entry : entries) {
        if (entry->last_draw_serial == draw_serial && entry->source_id == source_id &&
            entry->key == key) {
            already_captured = true;
            return *entry;
        }
    }
    already_captured = false;
    for (auto& entry : entries) {
        if (entry->last_draw_serial != draw_serial && entry->key == key) {
            return *entry;
        }
    }
    auto entry = std::make_unique<Entry>();
    entry->key = key;
    entry->image = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
    const vk::ImageCreateInfo ci{
        .flags = key.flags,
        .imageType = key.image_type,
        .format = key.format,
        .extent = key.extent,
        .mipLevels = key.mip_levels,
        .arrayLayers = key.array_layers,
        .samples = key.samples,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    entry->image.Create(ci);
    entries.push_back(std::move(entry));
    return *entries.back();
}

vk::ImageView FeedbackSnapshots::FindOrCreateView(Entry& entry, const VideoCore::Image& source,
                                                  const VideoCore::ImageViewInfo& info) {
    for (const auto& [cached_info, view] : entry.views) {
        if (cached_info == info) {
            return *view;
        }
    }
    vk::ImageViewMinLodCreateInfoEXT min_lod_ci{};
    const u32 base_level = std::min(info.range.base.level, entry.key.mip_levels - 1);
    const u32 level_count =
        std::min(std::max(1u, info.range.extent.levels), entry.key.mip_levels - base_level);
    const u32 base_layer = std::min(info.range.base.layer, entry.key.array_layers - 1);
    const u32 layer_count =
        std::min(std::max(1u, info.range.extent.layers), entry.key.array_layers - base_layer);

    void* pnext = nullptr;
    if (info.min_lod != 0 && instance.IsImageViewMinLodSupported()) {
        const float last_level = static_cast<float>(base_level + level_count - 1);
        min_lod_ci.minLod = std::min(static_cast<float>(info.min_lod) / 256.f, last_level);
        pnext = &min_lod_ci;
    }

    const vk::Format format = instance.GetSupportedFormat(info.format, source.format_features);
    const vk::ImageViewCreateInfo ci{
        .pNext = pnext,
        .image = entry.image,
        .viewType = SnapshotViewType(info.type),
        .format = format,
        .components = info.mapping,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = base_level,
            .levelCount = level_count,
            .baseArrayLayer = base_layer,
            .layerCount = layer_count,
        },
    };
    auto [result, view] = instance.GetDevice().createImageViewUnique(ci);
    ASSERT_MSG(result == vk::Result::eSuccess, "Failed creating feedback snapshot image view: {}",
               vk::to_string(result));
    const vk::ImageView handle = *view;
    entry.views.emplace_back(info, std::move(view));
    return handle;
}

vk::ImageView FeedbackSnapshots::Capture(VideoCore::Image& source, VideoCore::ImageId source_id,
                                         const VideoCore::ImageViewInfo& view_info) {
    const auto& src_ci = source.backing->image.image_ci;
    const Key key{
        .image_type = src_ci.imageType,
        .format = src_ci.format,
        .extent = src_ci.extent,
        .mip_levels = std::max(1u, src_ci.mipLevels),
        .array_layers = std::max(1u, src_ci.arrayLayers),
        .samples = src_ci.samples,
        .flags = CompatibleSnapshotFlags(src_ci.flags),
    };
    bool already_captured = false;
    Entry& entry = AcquireEntry(key, source_id, already_captured);
    if (already_captured) {
        return FindOrCreateView(entry, source, view_info);
    }

    scheduler.EndRendering();
    runtime.Transit(&source, vk::ImageLayout::eTransferSrcOptimal,
                    vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead);
    runtime.FlushBarriers();

    const vk::Image src_image = source.GetImage();
    const vk::Image dst_image = entry.image;
    const vk::ImageLayout old_layout = entry.layout;

    boost::container::small_vector<vk::ImageCopy, 16> copies;
    copies.reserve(key.mip_levels);
    for (u32 mip = 0; mip < key.mip_levels; ++mip) {
        copies.push_back(vk::ImageCopy{
            .srcSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = key.array_layers,
            },
            .dstSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = key.array_layers,
            },
            .extent{
                .width = std::max(1u, key.extent.width >> mip),
                .height = std::max(1u, key.extent.height >> mip),
                .depth = std::max(1u, key.extent.depth >> mip),
            },
        });
    }

    const vk::ImageMemoryBarrier2 pre_copy{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllGraphics | vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask =
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = old_layout,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = dst_image,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = key.mip_levels,
            .baseArrayLayer = 0,
            .layerCount = key.array_layers,
        },
    };
    const vk::ImageMemoryBarrier2 post_copy{
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllGraphics,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = dst_image,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = key.mip_levels,
            .baseArrayLayer = 0,
            .layerCount = key.array_layers,
        },
    };
    scheduler.Record([src_image, dst_image, pre_copy, post_copy,
                      copies = std::move(copies)](vk::CommandBuffer cmdbuf) {
        cmdbuf.pipelineBarrier2({
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &pre_copy,
        });
        cmdbuf.copyImage(src_image, vk::ImageLayout::eTransferSrcOptimal, dst_image,
                         vk::ImageLayout::eTransferDstOptimal, copies);
        cmdbuf.pipelineBarrier2({
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &post_copy,
        });
    });

    entry.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
    entry.last_draw_serial = draw_serial;
    entry.source_id = source_id;
    return FindOrCreateView(entry, source, view_info);
}

} // namespace Vulkan
