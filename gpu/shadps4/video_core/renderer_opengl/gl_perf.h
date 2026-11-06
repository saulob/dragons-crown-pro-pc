// SPDX-License-Identifier: GPL-2.0-or-later
// DEBUG_SAULO: temporary OpenGL frame profile and A/B switches; delete this file before release.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string_view>
#include "common/types.h"
#include "video_core/renderer_opengl/gl_guest_watch.h"

namespace OpenGL::Perf {

enum Timer : u32 {
    DrawTotal,
    ShaderLookup,
    ShaderLink,
    BindTargets,
    TextureLookup,
    TextureHash,
    TextureDetile,
    TextureUpload,
    WatchArm,
    BufferPush,
    BufferHash,
    ApplyState,
    DriverDraw,
    PrepareFrame,
    ShowFrame,
    Overlay,
    SwapWindow,
    ShowWait,
    RingWait,
    GlFinish,
    GlFlush,
    GpuWrites,
    TimerCount,
};

enum Counter : u32 {
    DrawCalls,
    TexturesChecked,
    TexturesHashed,
    TexturesDirtied,
    TexturesUnwatched,
    TexturesUploaded,
    TexturesDetiled,
    TextureBytes,
    VertexBytes,
    IndexBytes,
    ConstantBytes,
    BufferHashedBytes,
    CopiesReused,
    Programs,
    Framebuffers,
    FramebuffersCreated,
    TextureBinds,
    SamplerBinds,
    TextureParams,
    VertexArrays,
    BufferBinds,
    Uniforms,
    Attributes,
    BlendChanges,
    MaskChanges,
    Viewports,
    Scissors,
    DepthChanges,
    StencilChanges,
    RasterChanges,
    Links,
    Modules,
    WatchArms,
    LookupHits,
    LookupMisses,
    GpuWriteCalls,
    GpuWriteUploads,
    GpuFillBytes,
    GpuCopyBytes,
    GpuComputeBytes,
    Finishes,
    CounterCount,
};

inline std::array<u64, TimerCount> timers{};
inline std::array<u64, CounterCount> counters{};
inline std::atomic<u64> dropped_shows{0};
inline u64 largest_gpu_write{};

inline u64 Now() {
#if defined(__x86_64__) || defined(__i386__)
    return __builtin_ia32_rdtsc();
#else
    return u64(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

inline void Count(Counter counter, u64 amount = 1) {
    counters[counter] += amount;
}

struct Scope {
    Timer timer;
    u64 start = Now();
    ~Scope() {
        timers[timer] += Now() - start;
    }
};

/// DCP_GL_PERF_OFF lists what runs as before this work: watch, ring, state, lookup.
struct Switches {
    bool watch;
    bool ring;
    bool state;
    bool lookup;
};

inline const Switches& Optimizations() {
    static const Switches switches = [] {
        const char* off = std::getenv("DCP_GL_PERF_OFF");
        const std::string_view text = off ? off : "";
        const auto on = [&](std::string_view name) {
            return text.find(name) == std::string_view::npos;
        };
        return Switches{on("watch"), on("ring"), on("state"), on("lookup")};
    }();
    return switches;
}

struct FlipTotals {
    u64 guest_draws;
    u64 gl_draws;
    u64 gpu_idle_ns;
    GuestWriteWatch* watch;
};

/// Once per guest flip, on the GPU thread: prints a summary every 600 flips.
void OnFlip(const FlipTotals& totals);
/// After each swap, on the GPU thread.
void OnPresent();

inline void NoteGpuWrite(Counter kind, u64 bytes) {
    Count(kind, bytes);
    largest_gpu_write = bytes > largest_gpu_write ? bytes : largest_gpu_write;
}

} // namespace OpenGL::Perf
