// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <utility>
#include <vector>
#include <boost/container/small_vector.hpp>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/types.h"

namespace Vulkan {

class Instance;
class Runtime;
class Scheduler;

/// Scratch snapshot pool used when a draw samples its own color render target on a device without
/// VK_EXT_attachment_feedback_loop_layout + dynamic state support.
class FeedbackSnapshots {
public:
    FeedbackSnapshots(const Instance& instance, Scheduler& scheduler, Runtime& runtime);
    ~FeedbackSnapshots();

    FeedbackSnapshots(const FeedbackSnapshots&) = delete;
    FeedbackSnapshots& operator=(const FeedbackSnapshots&) = delete;

    void BeginDraw() noexcept {
        ++draw_serial;
    }

    vk::ImageView Capture(VideoCore::Image& source, VideoCore::ImageId source_id,
                          const VideoCore::ImageViewInfo& view_info);

private:
    struct Key {
        vk::ImageType image_type{};
        vk::Format format{};
        vk::Extent3D extent{};
        u32 mip_levels{};
        u32 array_layers{};
        vk::SampleCountFlagBits samples{};
        vk::ImageCreateFlags flags{};

        bool operator==(const Key&) const = default;
    };

    struct Entry {
        Key key{};
        VideoCore::UniqueImage image;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
        u64 last_draw_serial = 0;
        VideoCore::ImageId source_id{};
        boost::container::small_vector<std::pair<VideoCore::ImageViewInfo, vk::UniqueImageView>, 4>
            views;
    };

    Entry& AcquireEntry(const Key& key, VideoCore::ImageId source_id, bool& already_captured);
    vk::ImageView FindOrCreateView(Entry& entry, const VideoCore::Image& source,
                                   const VideoCore::ImageViewInfo& info);

    const Instance& instance;
    Scheduler& scheduler;
    Runtime& runtime;
    std::vector<std::unique_ptr<Entry>> entries;
    u64 draw_serial = 0;
};

} // namespace Vulkan
