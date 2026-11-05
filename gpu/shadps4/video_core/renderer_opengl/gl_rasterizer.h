// SPDX-License-Identifier: GPL-2.0-or-later
// Executes Liverpool's draws through OpenGL on the GPU command thread, which owns the context.
#pragma once

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "video_core/amdgpu/regs.h"
#include "video_core/renderer_backend.h"
#include "video_core/renderer_opengl/gl_api.h"
#include "video_core/renderer_opengl/gl_shader_cache.h"
#include "video_core/renderer_opengl/gl_texture_cache.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace OpenGL {

struct Capabilities;

/// One buffer shared by vertex, index and shader data; shaders read it as a buffer texture
/// because OpenGL 3.3 has no storage buffers.
class StreamRing {
public:
    StreamRing();
    ~StreamRing();

    /// Before a draw's first Push: its offsets stay valid until the draw is issued.
    void BeginDraw();
    /// The offset of a copy of `data`, or NoOffset when it cannot fit.
    u32 Push(const void* data, u32 size, u32 alignment);
    /// A copy of guest memory, shared by the draws of one submission.
    u32 PushGuest(VAddr address, u32 size, u32 alignment);
    void Commit();
    void ForgetGuestCopies();

    [[nodiscard]] GLuint Buffer() const {
        return buffer;
    }
    [[nodiscard]] GLuint Texture() const {
        return texture;
    }

    static constexpr u32 NoOffset = ~0u;

private:
    void Wrap();

    GLuint buffer{};
    GLuint texture{};
    u32 capacity{};
    u32 head{};
    u32 pending_start{};
    std::vector<u8> pending;
    std::unordered_map<u64, u32> guest_copies;
};

class Rasterizer final : public VideoCore::RasterizerBackend {
public:
    Rasterizer(AmdGpu::Liverpool* liverpool, const Capabilities& capabilities);
    ~Rasterizer() override;

    void Draw(bool is_indexed, u32 index_offset, const Vulkan::PreparedDraw* prepared) override;
    void DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 size, u32 max_count,
                      VAddr count_address, u16 vertex_sgpr_offset,
                      u16 instance_sgpr_offset) override;
    void DispatchDirect() override;
    void DispatchIndirect(VAddr address, u32 offset, u32 size) override;

    bool HostMarkersEnabled() const override {
        return false;
    }
    void ScopeMarkerBegin(const std::string_view&, bool) override {}
    void ScopeMarkerEnd(bool) override {}
    void ScopedMarkerInsertColor(const std::string_view&, const u32, bool) override {}

    void FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) override;
    void CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) override;
    u32 ReadDataFromGds(u32 gds_offset) override;
    bool InvalidateMemory(VAddr, u64, bool) override {
        return false;
    }
    void WaitHostCopies() override {}

    void DrainDrawPipe(u32, u32, const char*) override {}
    bool RunInOrder(OrderedTask task, const void* data, u32 size, u64 toggle) override;
    [[nodiscard]] u64 DrawPipeHead() const override {
        return 0;
    }
    [[nodiscard]] bool DrawPipeReached(u64) const override {
        return true;
    }
    [[nodiscard]] bool DrawPipeIdle() const override {
        return true;
    }
    void RetireSubmission() override {}
    void NotePendingGpuWrite(VAddr, u64) override {}
    void WaitDeferredSignals() override {}
    void SignalAfterHostCopies(std::function<void()> signal) override {
        signal();
    }
    void ProcessDownloadImages() override {}

    void MapMemory(VAddr, u64) override {}
    void RegisterMemory(VAddr, u64) override {}
    void UnmapMemory(VAddr addr, u64 size) override;

    u64 Flush() override;
    void Finish() override;
    void OnSubmit() override;

    Vulkan::DrawPreparation* DrawPreparationOrNull() override {
        return nullptr;
    }

    TextureCache& GetTextureCache() {
        return texture_cache;
    }
    /// Plain state for the presenter's own passes.
    void ResetState();
    void OnFlip();

private:
    struct AttachedTargets {
        std::array<Image*, AmdGpu::NUM_COLOR_BUFFERS> color{};
        Image* depth{};
    };

    bool FilterDraw();
    void EliminateFastClear();
    void Resolve();
    void Report(const std::string& what);
    bool BindTargets(const GraphicsProgram& program, AttachedTargets& targets);
    GLuint FramebufferFor(const AttachedTargets& targets);
    void ClearTargets(const AttachedTargets& targets);
    void BindStage(const Shader::Info& info, const StageLocations& locations,
                   const Shader::Backend::GLSL::Stage& glsl);
    void BindTexture(const Shader::Info& info, const Shader::Backend::GLSL::CombinedSampler& combined,
                     GLuint unit);
    GLuint SamplerFor(AmdGpu::Sampler sharp, bool shadow, bool is_depth);
    bool BindVertexInputs(const GraphicsProgram& program, u32 first_instance);
    void ApplyViewportScissor(const GraphicsProgram& program);
    void ApplyDepthStencil();
    void ApplyRasterState();
    void ApplyBlend(const GraphicsProgram& program, const AttachedTargets& targets);
    bool TryKnownCompute(const Shader::Info& info);

    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    bool separate_blend;
    bool anisotropy;
    float max_anisotropy{1.0f};
    u32 max_viewport_width;
    u32 max_viewport_height;
    StreamRing ring;
    TextureCache texture_cache;
    ShaderCache shaders;
    GLuint vertex_array{};
    GLuint black_texture{};
    GLuint resolve_framebuffers[2]{};
    u32 enabled_attributes{};
    u32 enabled_clip_distances{};
    std::unordered_map<u64, GLuint> framebuffers;
    struct SamplerKey {
        u64 raw0;
        u64 raw1;
        bool shadow;
        bool operator==(const SamplerKey&) const = default;
    };
    struct SamplerKeyHash {
        size_t operator()(const SamplerKey& key) const {
            return size_t(key.raw0 ^ (key.raw1 * 31) ^ u64(key.shadow));
        }
    };
    std::unordered_map<SamplerKey, GLuint, SamplerKeyHash> samplers;
    std::vector<u8> gds;
    std::unordered_set<std::string> reported;
    u64 draws{};
    u64 draws_reaching_gl{};
    u64 flips{};
};

} // namespace OpenGL
