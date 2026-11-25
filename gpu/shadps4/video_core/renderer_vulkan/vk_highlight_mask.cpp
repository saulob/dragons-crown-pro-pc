// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_highlight_mask.h"

#include <cstdio>

#include <boost/container/static_vector.hpp>

#include "character_highlight_draw.h"
#include "video_core/host_shaders/character_highlight_outline_frag.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/renderer_vulkan/vk_gpu_compat.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {
namespace {
constexpr vk::ImageSubresourceRange kColorRange = {
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .levelCount = 1,
    .layerCount = 1,
};
} // namespace

CharacterHighlightMask::CharacterHighlightMask(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    const vk::Device device = instance.GetDevice();
    sampler = Check<"create Character Highlight sampler">(device.createSamplerUnique({
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
    }));

    const vk::DescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
    };
    set_layout = Check<"create Character Highlight descriptor set layout">(
        device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = 1,
            .pBindings = &binding,
        }));
    const vk::PushConstantRange push_constants = {
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(dcp::highlight::OutlineSettings),
    };
    const vk::DescriptorSetLayout layout = *set_layout;
    pipeline_layout = Check<"create Character Highlight pipeline layout">(
        device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push_constants,
        }));

    vertex_module = vk::UniqueShaderModule{CompileSPV(FS_TRI_VERT, device)};
    fragment_module = vk::UniqueShaderModule{CompileSPV(CHARACTER_HIGHLIGHT_OUTLINE_FRAG, device)};
}

CharacterHighlightMask::~CharacterHighlightMask() {
    scheduler.Finish();
}

CharacterHighlightMask::Extent* CharacterHighlightMask::Acquire(u32 width, u32 height) {
    if (width == 0 || height == 0) {
        return nullptr;
    }
    for (Extent& extent : extents) {
        if (extent.width == width && extent.height == height) {
            extent.used = ++use_counter;
            return &extent;
        }
    }
    for (Extent& extent : extents) {
        if (extent.width != 0) {
            continue;
        }
        extent.width = width;
        extent.height = height;
        extent.layout = vk::ImageLayout::eUndefined;
        extent.used = ++use_counter;
        extent.image = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
        extent.image.Create({
            .flags = {},
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR8G8B8A8Unorm,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        extent.view = Check<"create Character Highlight mask view">(
            instance.GetDevice().createImageViewUnique({
                .image = extent.image,
                .viewType = vk::ImageViewType::e2D,
                .format = vk::Format::eR8G8B8A8Unorm,
                .subresourceRange = kColorRange,
            }));
        SetObjectName(instance.GetDevice(), extent.image.image, "Character Highlight mask {}x{}", width,
                      height);
        return &extent;
    }
    return nullptr;
}

void CharacterHighlightMask::TransitionToAttachment(Extent& extent) {
    if (extent.layout == vk::ImageLayout::eColorAttachmentOptimal) {
        return;
    }
    const bool undefined = extent.layout == vk::ImageLayout::eUndefined;
    const vk::ImageMemoryBarrier2 barrier = {
        .srcStageMask = undefined ? vk::PipelineStageFlags2{vk::PipelineStageFlagBits2::eNone}
                                  : vk::PipelineStageFlags2{vk::PipelineStageFlagBits2::eAllCommands},
        .srcAccessMask = undefined ? vk::AccessFlags2{vk::AccessFlagBits2::eNone}
                                   : vk::AccessFlags2{vk::AccessFlagBits2::eMemoryRead |
                                                      vk::AccessFlagBits2::eMemoryWrite},
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = extent.layout,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = extent.image,
        .subresourceRange = kColorRange,
    };
    scheduler.Record([barrier](vk::CommandBuffer cmdbuf) {
        cmdbuf.pipelineBarrier2({
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &barrier,
        });
    });
    extent.layout = vk::ImageLayout::eColorAttachmentOptimal;
}

void CharacterHighlightMask::TransitionToSample(Extent& extent) {
    if (extent.layout == vk::ImageLayout::eShaderReadOnlyOptimal) {
        return;
    }
    const vk::ImageMemoryBarrier2 barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = extent.layout,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = extent.image,
        .subresourceRange = kColorRange,
    };
    scheduler.Record([barrier](vk::CommandBuffer cmdbuf) {
        cmdbuf.pipelineBarrier2({
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &barrier,
        });
    });
    extent.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
}

vk::Pipeline CharacterHighlightMask::CompositePipeline(PipelineKey key) {
    for (size_t i = 0; i < pipeline_count; ++i) {
        if (pipelines[i].first == key) {
            return *pipelines[i].second;
        }
    }
    if (pipeline_count == pipelines.size()) {
        return {};
    }
    const vk::Device device = instance.GetDevice();
    const std::array stages = {
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = *vertex_module,
            .pName = "main",
        },
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = *fragment_module,
            .pName = "main",
        },
    };
    const vk::PipelineRenderingCreateInfo rendering = {
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &key.format,
    };
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::Viewport viewport = {
        .width = 1.0f,
        .height = 1.0f,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    const vk::Rect2D scissor = {{0, 0}, {1, 1}};
    const vk::PipelineViewportStateCreateInfo viewport_info = {
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };
    const vk::PipelineRasterizationStateCreateInfo raster = {
        .depthClampEnable = false,
        .rasterizerDiscardEnable = false,
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eClockwise,
        .depthBiasEnable = false,
        .lineWidth = 1.0f,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling = {
        .rasterizationSamples = key.samples,
    };
    const vk::PipelineColorBlendAttachmentState attachment = {
        .blendEnable = true,
        .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
        .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
        .colorBlendOp = vk::BlendOp::eAdd,
        .srcAlphaBlendFactor = vk::BlendFactor::eOne,
        .dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
        .alphaBlendOp = vk::BlendOp::eAdd,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };
    const vk::PipelineColorBlendStateCreateInfo color_blending = {
        .attachmentCount = 1,
        .pAttachments = &attachment,
    };
    boost::container::static_vector<vk::DynamicState, 16> dynamic_states = {
        vk::DynamicState::eViewportWithCount,          vk::DynamicState::eScissorWithCount,
        vk::DynamicState::eDepthTestEnable,            vk::DynamicState::eDepthWriteEnable,
        vk::DynamicState::eStencilTestEnable,          vk::DynamicState::eRasterizerDiscardEnable,
        vk::DynamicState::ePrimitiveRestartEnable,     vk::DynamicState::eDepthBiasEnable,
        vk::DynamicState::eCullMode,                   vk::DynamicState::eFrontFace,
    };
    if (instance.IsDepthBoundsSupported()) {
        dynamic_states.push_back(vk::DynamicState::eDepthBoundsTestEnable);
    }
    if (instance.IsDynamicColorWriteMaskSupported()) {
        dynamic_states.push_back(vk::DynamicState::eColorWriteMaskEXT);
    }
    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };
    constexpr vk::PipelineDepthStencilStateCreateInfo depth_stencil{};
    const vk::GraphicsPipelineCreateInfo pipeline_info = {
        .pNext = &rendering,
        .stageCount = static_cast<u32>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = GpuCompat::ChoosePipelineState(instance.GetGpuCapabilities())
                                      .static_depth_stencil_state
                                  ? &depth_stencil
                                  : nullptr,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *pipeline_layout,
    };
    auto [result, pipeline] = device.createGraphicsPipelineUnique(nullptr, pipeline_info);
    if (result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "Character Highlight outline pipeline failed: {}",
                    vk::to_string(result));
        return {};
    }
    pipelines[pipeline_count].first = key;
    pipelines[pipeline_count].second = std::move(pipeline);
    return *pipelines[pipeline_count++].second;
}

bool CharacterHighlightMask::Composite(const RenderState& target, vk::Format color_format,
                                       vk::SampleCountFlagBits samples, Extent& extent, int slot,
                                       const std::array<uint32_t, 4>& color_swizzle, float pixels) {
    if (!extent.view || target.num_color_attachments == 0 || pixels <= 0.0f) {
        return false;
    }
    const vk::Pipeline pipeline = CompositePipeline({color_format, samples});
    if (!pipeline) {
        return false;
    }
    scheduler.EndRendering();
    TransitionToSample(extent);

    RenderState state{};
    state.width = target.width;
    state.height = target.height;
    state.num_layers = 1;
    state.num_color_attachments = 1;
    state.color_attachments[0] = target.color_attachments[0];
    state.color_attachments[0].is_clear = 0;
    scheduler.BeginRendering(state);

    const dcp::highlight::OutlineSettings settings = dcp::highlight::MakeOutlineSettings(
        slot, static_cast<float>(extent.width), static_cast<float>(extent.height), pixels,
        color_swizzle);
    const vk::Sampler mask_sampler = *sampler;
    const vk::ImageView mask_view = *extent.view;
    const vk::PipelineLayout layout = *pipeline_layout;
    const bool dynamic_color_write_mask = instance.IsDynamicColorWriteMaskSupported();
    const bool depth_bounds = instance.IsDepthBoundsSupported();
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        const std::array viewports = {
            vk::Viewport{
                .width = static_cast<float>(state.width),
                .height = static_cast<float>(state.height),
                .minDepth = 0.0f,
                .maxDepth = 1.0f,
            },
        };
        cmdbuf.setViewportWithCount(viewports);
        cmdbuf.setScissorWithCount(
            std::array{vk::Rect2D{{0, 0}, {state.width, state.height}}});
        cmdbuf.setDepthTestEnable(false);
        cmdbuf.setDepthWriteEnable(false);
        cmdbuf.setStencilTestEnable(false);
        cmdbuf.setRasterizerDiscardEnable(false);
        cmdbuf.setPrimitiveRestartEnable(false);
        cmdbuf.setDepthBiasEnable(false);
        if (depth_bounds) {
            cmdbuf.setDepthBoundsTestEnable(false);
        }
        cmdbuf.setCullMode(vk::CullModeFlagBits::eNone);
        cmdbuf.setFrontFace(vk::FrontFace::eClockwise);
        std::array<vk::ColorComponentFlags, 8> masks{};
        masks.fill(vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                   vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
        if (dynamic_color_write_mask) {
            cmdbuf.setColorWriteMaskEXT(0, masks);
        }
        const vk::DescriptorImageInfo image_info = {
            .sampler = mask_sampler,
            .imageView = mask_view,
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        };
        const std::array writes = {
            vk::WriteDescriptorSet{
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                .pImageInfo = &image_info,
            },
        };
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, layout, 0, writes);
        cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(settings),
                             &settings);
        cmdbuf.draw(3, 1, 0, 0);
    });
    scheduler.GetDynamicState().Invalidate();
    return true;
}

} // namespace Vulkan
