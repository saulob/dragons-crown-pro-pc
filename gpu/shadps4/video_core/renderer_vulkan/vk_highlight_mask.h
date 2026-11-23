// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

class Instance;

// Character Highlight: one reusable coverage image per render-target extent, and the exterior outline composite.
class CharacterHighlightMask {
public:
    CharacterHighlightMask(const Instance& instance, Scheduler& scheduler);
    ~CharacterHighlightMask();

    struct Extent {
        u32 width = 0;
        u32 height = 0;
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
        u64 used = 0;
    };

    Extent* Acquire(u32 width, u32 height);
    void TransitionToAttachment(Extent& extent);
    void TransitionToSample(Extent& extent);
    bool Composite(const RenderState& target, vk::Format color_format,
                   vk::SampleCountFlagBits samples, Extent& extent, int slot,
                   const std::array<uint32_t, 4>& color_swizzle, float pixels);

private:
    struct PipelineKey {
        vk::Format format;
        vk::SampleCountFlagBits samples;
        bool operator==(const PipelineKey&) const = default;
    };

    vk::Pipeline CompositePipeline(PipelineKey key);

    static constexpr size_t kMaxExtents = 2;
    static constexpr size_t kMaxPipelines = 8;

    const Instance& instance;
    Scheduler& scheduler;
    std::array<Extent, kMaxExtents> extents;
    u64 use_counter = 0;
    vk::UniqueSampler sampler;
    vk::UniqueDescriptorSetLayout set_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniqueShaderModule vertex_module;
    vk::UniqueShaderModule fragment_module;
    std::array<std::pair<PipelineKey, vk::UniquePipeline>, kMaxPipelines> pipelines;
    size_t pipeline_count = 0;
};

} // namespace Vulkan
