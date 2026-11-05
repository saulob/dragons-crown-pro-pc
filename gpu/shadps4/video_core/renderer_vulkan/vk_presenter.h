// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>

#include "core/libraries/videoout/buffer.h"
#include "video_core/renderer_backend.h"
#include "video_core/renderer_vulkan/host_passes/fsr_pass.h"
#include "video_core/renderer_vulkan/host_passes/pp_pass.h"
#include "video_core/renderer_vulkan/host_passes/smaa_pass.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Frontend {
class WindowSDL;
}

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {

struct Frame : VideoCore::PresentFrame {
    u32 width;
    u32 height;
    VmaAllocation allocation;
    vk::Image image;
    vk::ImageView image_view;
    vk::Fence present_done;
    vk::Semaphore ready_semaphore;
    u64 ready_tick;
    bool is_hdr{false};
    u8 id{};

};

enum SchedulerType {
    Draw,
    Present,
    CpuFlip,
};

class Rasterizer;

class Presenter final : public VideoCore::PresenterBackend {
public:
    Presenter(Frontend::WindowSDL& window, AmdGpu::Liverpool* liverpool);
    ~Presenter() override;

    HostPasses::PostProcessingPass::Settings& GetPPSettingsRef() {
        return pp_settings;
    }

    HostPasses::FsrPass::Settings& GetFsrSettingsRef() {
        return fsr_settings;
    }

    Frontend::WindowSDL& GetWindow() const {
        return window;
    }

    Rasterizer& GetRasterizer() const {
        return *rasterizer.get();
    }

    bool IsHDRSupported() const override {
        return swapchain.HasHDR();
    }

    void SetHDR(bool enable) override {
        if (!IsHDRSupported()) {
            return;
        }
        swapchain.SetHDR(enable);
        pp_settings.hdr = enable ? 1 : 0;
    }

    void SetGamma(float gamma) override {
        pp_settings.gamma = gamma;
    }

    void RegisterDisplayBuffer(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) override {
        RegisterVideoOutSurface(attribute, cpu_address);
    }

    VideoCore::Image& RegisterVideoOutSurface(
        const Libraries::VideoOut::BufferAttributeGroup& attribute, VAddr cpu_address) {
        vo_buffers_addr.emplace_back(cpu_address);
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        const auto image_id = texture_cache.FindImage(desc);
        auto& image = texture_cache.GetImage(image_id);
        image.usage.vo_surface = 1u;
        return image;
    }

    bool IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const;

    Frame* PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                        VAddr cpu_address) override;

    Frame* PrepareBlankFrame(bool present_thread) override;

    void Present(VideoCore::PresentFrame* frame, bool is_reusing_frame,
                 bool is_game_frame) override {
        Present(static_cast<Frame*>(frame), is_reusing_frame, is_game_frame);
    }
    void Present(Frame* frame, bool is_reusing_frame = false, bool is_game_frame = true);
    void RetireFrame(VideoCore::PresentFrame* frame) override {
        RetireFrame(static_cast<Frame*>(frame));
    }
    /// Returns a rendered frame to the free queue without presenting it.
    void RetireFrame(Frame* frame);
    /// A black frame with the loading screen (DcpOverlay::SetLoading) only.
    void PresentLoadingFrame();
    Frame* PrepareLastFrame() override;
    void ReportCompilerStats() override;

private:
    Frame* GetRenderFrame();

    void RecreateFrame(Frame* frame, u32 width, u32 height);

    void SetExpectedGameSize(s32 width, s32 height);

private:
    std::atomic<float> expected_ratio{1920.0f / 1080.0f};
    std::atomic<u64> expected_frame_size{1920 | u64(1080) << 32}; ///< width | height << 32

    Frontend::WindowSDL& window;
    Instance instance;
    HostPasses::FsrPass fsr_pass;
    HostPasses::SmaaPass smaa_pass;
    HostPasses::FsrPass::Settings fsr_settings{};
    HostPasses::PostProcessingPass::Settings pp_settings{};
    HostPasses::PostProcessingPass pp_pass;
    AmdGpu::Liverpool* liverpool;
    Scheduler draw_scheduler;
    u64 previous_frame_tick{}; ///< PrepareFrame's bound: one frame ahead of the GPU
    Scheduler present_scheduler;
    Scheduler flip_scheduler;
    Swapchain swapchain;
    Runtime runtime;
    std::unique_ptr<Rasterizer> rasterizer;
    VideoCore::TextureCache& texture_cache;
    vk::UniqueCommandPool command_pool;
    std::vector<Frame> present_frames;
    std::deque<Frame*> free_queue;
    Frame* last_submit_frame{};
    std::mutex free_mutex;
    std::condition_variable free_cv;
    std::condition_variable_any frame_cv;
    std::vector<VAddr> vo_buffers_addr;
};

} // namespace Vulkan
