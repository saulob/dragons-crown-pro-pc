// SPDX-License-Identifier: GPL-2.0-or-later
// DEBUG_SAULO: temporary OpenGL frame profile; delete this file before release.

#include <algorithm>
#include <cstdio>

#include "video_core/renderer_opengl/gl_perf.h"

namespace OpenGL::Perf {
namespace {

constexpr u64 ReportFlips = 600;
constexpr double SlowFrameMs = 18.0;
/// Upper edges in ms: 60 FPS, between, 30 FPS, slower.
constexpr std::array<double, 3> CadenceEdges{18.0, 28.0, 38.0};

using Clock = std::chrono::steady_clock;

Clock::time_point window_time;
Clock::time_point last_flip;
u64 window_ticks{};
double worst_ms{};
u64 slow_frames{};
u64 flips{};
u64 window_idle_ns{};
u64 window_dropped{};
std::array<u64, CadenceEdges.size() + 1> flip_cadence{};
std::array<u64, CadenceEdges.size() + 1> present_cadence{};
Clock::time_point last_present;
double present_ms_total{};
double present_ms_worst{};
u64 presents{};

void Bucket(std::array<u64, CadenceEdges.size() + 1>& cadence, double interval_ms) {
    u32 bucket = 0;
    while (bucket < CadenceEdges.size() && interval_ms > CadenceEdges[bucket]) {
        ++bucket;
    }
    ++cadence[bucket];
}

} // namespace

void OnFlip(const FlipTotals& totals) {
    const u64 now = Now();
    const auto time = Clock::now();
    if (flips == 0) {
        window_time = time;
        window_ticks = now;
        window_idle_ns = totals.gpu_idle_ns;
        window_dropped = dropped_shows.load(std::memory_order_relaxed);
        totals.watch->TakeStats();
        last_flip = time;
        ++flips;
        timers = {};
        counters = {};
        return;
    }
    const double interval_ms = std::chrono::duration<double, std::milli>(time - last_flip).count();
    last_flip = time;
    worst_ms = std::max(worst_ms, interval_ms);
    slow_frames += interval_ms > SlowFrameMs ? 1 : 0;
    Bucket(flip_cadence, interval_ms);
    if (++flips <= ReportFlips) {
        return;
    }

    const double wall_ms = std::chrono::duration<double, std::milli>(time - window_time).count();
    const double ticks_per_ms = double(now - window_ticks) / wall_ms;
    const double frames = double(ReportFlips);
    const auto ms = [&](Timer timer) { return double(timers[timer]) / ticks_per_ms / frames; };
    const auto per_frame = [&](Counter counter) { return double(counters[counter]) / frames; };
    const auto mb = [&](Counter counter) { return per_frame(counter) / (1024.0 * 1024.0); };
    const double idle_ms = double(totals.gpu_idle_ns - window_idle_ns) / 1e6;
    const u64 dropped = dropped_shows.load(std::memory_order_relaxed);

    const auto watch = totals.watch->TakeStats();
    const auto watch_rate = [&](u64 value) { return double(value) / frames; };

    std::printf("DEBUG_SAULO: OpenGL perf cadence frame_interval_ms=%.2f flips_at_16/22/33/slower=%llu/"
                "%llu/%llu/%llu present_interval_ms=%.2f present_worst_ms=%.1f "
                "presents_at_16/22/33/slower=%llu/%llu/%llu/%llu\n",
                wall_ms / frames, static_cast<unsigned long long>(flip_cadence[0]),
                static_cast<unsigned long long>(flip_cadence[1]),
                static_cast<unsigned long long>(flip_cadence[2]),
                static_cast<unsigned long long>(flip_cadence[3]),
                presents ? present_ms_total / double(presents) : 0.0, present_ms_worst,
                static_cast<unsigned long long>(present_cadence[0]),
                static_cast<unsigned long long>(present_cadence[1]),
                static_cast<unsigned long long>(present_cadence[2]),
                static_cast<unsigned long long>(present_cadence[3]));
    std::printf("DEBUG_SAULO: OpenGL perf watch faults=%.1f gpu_thread=%.1f guest=%.1f "
                "unique_pages=%llu repeat=%.1f unique_64k=%llu shared_page=%.1f released=%.1f "
                "armed=%.1f rearmed=%.1f dirty=%.1f textures_dirty=%.2f textures_unwatched=%.1f "
                "arms=%.2f arms_deferred=%.1f armed_pages_now=%llu host_writes=%.1f "
                "host_pages=%.1f forgets=%llu forget_pages=%llu invalidates=%llu retries=%llu\n",
                watch_rate(watch.faults), watch_rate(watch.faults_gpu_thread),
                watch_rate(watch.faults - watch.faults_gpu_thread),
                static_cast<unsigned long long>(watch.unique_pages), watch_rate(watch.faults_repeat),
                static_cast<unsigned long long>(watch.unique_blocks),
                watch_rate(watch.faults_shared_page), watch_rate(watch.pages_released),
                watch_rate(watch.pages_armed), watch_rate(watch.pages_rearmed),
                watch_rate(watch.pages_dirty), per_frame(TexturesDirtied),
                per_frame(TexturesUnwatched), per_frame(WatchArms), watch_rate(watch.arms_deferred),
                static_cast<unsigned long long>(watch.armed_pages_now), watch_rate(watch.host_writes),
                watch_rate(watch.host_write_pages), static_cast<unsigned long long>(watch.forgets),
                static_cast<unsigned long long>(watch.forget_pages),
                static_cast<unsigned long long>(watch.invalidates),
                static_cast<unsigned long long>(watch.retries));
    std::printf("DEBUG_SAULO: OpenGL perf frames=%llu fps=%.1f worst_ms=%.1f slow=%llu busy_ms=%.2f "
                "idle_ms=%.2f draw_ms=%.2f shader_ms=%.2f link_ms=%.2f targets_ms=%.2f tex_ms=%.2f "
                "tex_hash_ms=%.2f detile_ms=%.2f upload_ms=%.2f arm_ms=%.2f buffer_ms=%.2f "
                "buf_hash_ms=%.2f ring_wait_ms=%.2f state_ms=%.2f gl_draw_ms=%.2f prepare_ms=%.2f "
                "show_ms=%.2f overlay_ms=%.2f swap_ms=%.2f show_wait_ms=%.2f finish_ms=%.2f "
                "flush_ms=%.2f gpu_write_ms=%.2f\n",
                static_cast<unsigned long long>(ReportFlips), frames * 1000.0 / wall_ms,
                worst_ms, static_cast<unsigned long long>(slow_frames),
                (wall_ms - idle_ms) / frames, idle_ms / frames, ms(DrawTotal), ms(ShaderLookup),
                ms(ShaderLink), ms(BindTargets), ms(TextureLookup), ms(TextureHash), ms(TextureDetile),
                ms(TextureUpload), ms(WatchArm), ms(BufferPush), ms(BufferHash), ms(RingWait),
                ms(ApplyState), ms(DriverDraw), ms(PrepareFrame), ms(ShowFrame) - ms(SwapWindow),
                ms(Overlay), ms(SwapWindow), ms(ShowWait), ms(GlFinish), ms(GlFlush), ms(GpuWrites));
    std::printf("DEBUG_SAULO: OpenGL perf per_frame draws=%.0f checked=%.0f hashed=%.1f uploads=%.2f "
                "detiled=%.2f tex_mb=%.2f vb_mb=%.2f ib_mb=%.2f cb_mb=%.2f buf_hash_mb=%.1f reused=%.0f "
                "programs=%.0f fbos=%.0f fbos_new=%.2f tex_binds=%.0f samplers=%.0f tex_params=%.0f "
                "vao=%.0f buffers=%.0f uniforms=%.0f attribs=%.0f blend=%.0f masks=%.0f viewport=%.0f "
                "scissor=%.0f depth=%.0f stencil=%.0f raster=%.0f links=%.2f modules=%.2f arms=%.2f "
                "lookup_hit=%.0f lookup_miss=%.1f gpu_writes=%.1f gpu_write_uploads=%.2f "
                "fill_mb=%.1f copy_mb=%.1f compute_fill_mb=%.1f gpu_write_max_mb=%.1f finishes=%.2f "
                "faults=%.1f dropped_shows=%llu guest_draws_total=%llu gl_draws_total=%llu\n",
                per_frame(DrawCalls), per_frame(TexturesChecked), per_frame(TexturesHashed),
                per_frame(TexturesUploaded), per_frame(TexturesDetiled), mb(TextureBytes),
                mb(VertexBytes), mb(IndexBytes), mb(ConstantBytes), mb(BufferHashedBytes),
                per_frame(CopiesReused), per_frame(Programs), per_frame(Framebuffers),
                per_frame(FramebuffersCreated), per_frame(TextureBinds), per_frame(SamplerBinds),
                per_frame(TextureParams), per_frame(VertexArrays), per_frame(BufferBinds),
                per_frame(Uniforms), per_frame(Attributes), per_frame(BlendChanges),
                per_frame(MaskChanges), per_frame(Viewports), per_frame(Scissors),
                per_frame(DepthChanges), per_frame(StencilChanges), per_frame(RasterChanges),
                per_frame(Links), per_frame(Modules), per_frame(WatchArms), per_frame(LookupHits),
                per_frame(LookupMisses), per_frame(GpuWriteCalls), per_frame(GpuWriteUploads),
                mb(GpuFillBytes), mb(GpuCopyBytes), mb(GpuComputeBytes),
                double(largest_gpu_write) / (1024.0 * 1024.0), per_frame(Finishes),
                watch_rate(watch.faults),
                static_cast<unsigned long long>(dropped - window_dropped),
                static_cast<unsigned long long>(totals.guest_draws),
                static_cast<unsigned long long>(totals.gl_draws));

    timers = {};
    counters = {};
    window_time = time;
    window_ticks = now;
    window_idle_ns = totals.gpu_idle_ns;
    window_dropped = dropped;
    worst_ms = 0.0;
    slow_frames = 0;
    flips = 1;
    largest_gpu_write = 0;
    flip_cadence = {};
    present_cadence = {};
    present_ms_total = 0.0;
    present_ms_worst = 0.0;
    presents = 0;
}

void OnPresent() {
    const auto time = Clock::now();
    if (last_present != Clock::time_point{}) {
        const double interval_ms =
            std::chrono::duration<double, std::milli>(time - last_present).count();
        present_ms_total += interval_ms;
        present_ms_worst = std::max(present_ms_worst, interval_ms);
        ++presents;
        Bucket(present_cadence, interval_ms);
    }
    last_present = time;
}

} // namespace OpenGL::Perf
