// SPDX-License-Identifier: GPL-2.0-or-later
// The boundary between the GPU front end (Liverpool, VideoOut, guest memory hooks) and a renderer.
#pragma once

#include <functional>
#include <memory>
#include <string_view>
#include <fmt/format.h>
#include "common/types.h"
#include "dcp_toggles.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Frontend {
class WindowSDL;
}

namespace Libraries::VideoOut {
struct BufferAttributeGroup;
}

namespace Vulkan {
class DrawPreparation;
struct PreparedDraw;
} // namespace Vulkan

namespace VideoCore {

class RasterizerBackend {
public:
    virtual ~RasterizerBackend() = default;

    virtual void Draw(bool is_indexed, u32 index_offset = 0,
                      const Vulkan::PreparedDraw* prepared = nullptr) = 0;
    virtual void DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 size,
                              u32 max_count, VAddr count_address, u16 vertex_sgpr_offset,
                              u16 instance_sgpr_offset) = 0;
    virtual void DispatchDirect() = 0;
    virtual void DispatchIndirect(VAddr address, u32 offset, u32 size) = 0;

    void ScopeMarker(fmt::string_view fmt, fmt::format_args args, auto&& func) {
        if (HostMarkersEnabled()) {
            ScopeMarkerBegin(fmt::vformat(fmt, args));
            func();
            ScopeMarkerEnd();
        } else {
            func();
        }
    }
    virtual bool HostMarkersEnabled() const = 0;
    virtual void ScopeMarkerBegin(const std::string_view& str, bool from_guest = false) = 0;
    virtual void ScopeMarkerEnd(bool from_guest = false) = 0;
    virtual void ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                         bool from_guest = false) = 0;

    virtual void FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) = 0;
    virtual void CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) = 0;
    virtual u32 ReadDataFromGds(u32 gsd_offset) = 0;
    virtual bool InvalidateMemory(VAddr addr, u64 size, bool assume_locks = false) = 0;
    virtual void WaitHostCopies() = 0;

    virtual void DrainDrawPipe(u32 reason, u32 line = __builtin_LINE(),
                               const char* function = __builtin_FUNCTION()) = 0;
    using OrderedTask = void (*)(RasterizerBackend& rasterizer, const u8* data);
    virtual bool RunInOrder(OrderedTask task, const void* data, u32 size,
                            u64 toggle = DcpToggle::PipelinedTasks) = 0;
    [[nodiscard]] virtual u64 DrawPipeHead() const = 0;
    [[nodiscard]] virtual bool DrawPipeReached(u64 position) const = 0;
    [[nodiscard]] virtual bool DrawPipeIdle() const = 0;
    virtual void RetireSubmission() = 0;
    virtual void NotePendingGpuWrite(VAddr address, u64 size) = 0;
    virtual void WaitDeferredSignals() = 0;
    virtual void SignalAfterHostCopies(std::function<void()> signal) = 0;
    virtual void ProcessDownloadImages() = 0;

    virtual void MapMemory(VAddr addr, u64 size) = 0;
    virtual void RegisterMemory(VAddr addr, u64 size) = 0;
    virtual void UnmapMemory(VAddr addr, u64 size) = 0;

    virtual u64 Flush() = 0;
    virtual void Finish() = 0;
    virtual void OnSubmit() = 0;

    /// The draw preparation workers; null when a renderer has none.
    virtual Vulkan::DrawPreparation* DrawPreparationOrNull() = 0;
};

/// A finished picture waiting to be shown; each backend derives its own.
struct PresentFrame {};

class PresenterBackend {
public:
    virtual ~PresenterBackend() = default;

    [[nodiscard]] virtual bool IsHDRSupported() const = 0;
    virtual void SetHDR(bool enable) = 0;
    virtual void SetGamma(float gamma) = 0;

    virtual void RegisterDisplayBuffer(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                                       VAddr cpu_address) = 0;
    virtual PresentFrame* PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                                       VAddr cpu_address) = 0;
    virtual PresentFrame* PrepareBlankFrame(bool present_thread) = 0;
    virtual PresentFrame* PrepareLastFrame() = 0;
    virtual void Present(PresentFrame* frame, bool is_reusing_frame = false,
                         bool is_game_frame = true) = 0;
    virtual void RetireFrame(PresentFrame* frame) = 0;

    virtual void ReportCompilerStats() = 0;
};

/// Creates the renderer DcpRenderer::Active() names.
std::unique_ptr<PresenterBackend> CreatePresenter(Frontend::WindowSDL& window,
                                                  AmdGpu::Liverpool* liverpool);

} // namespace VideoCore
