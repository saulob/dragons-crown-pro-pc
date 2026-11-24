// Focused Issue #84 regression tests (Qualcomm Adreno X2-45 profile):
// feedback-loop snapshot fallback, provoking-vertex pass breaks, static pipeline key audit,
// cache version invalidation, and bounded diagnostics.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "dcp_issue84_diag.h"
#include "video_core/pipeline_cache_policy.h"
#include "video_core/renderer_vulkan/vk_gpu_compat.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);          \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

using namespace Vulkan::GpuCompat;

Capabilities AdrenoX2_45() {
    return {
        .vendor_id = 0x5143,
        .nvidia_proprietary = false,
        .amd_explicit_vertex_parameter = false,
        .fragment_shader_barycentric = false,
        .dynamic_state3 = false,
        .dynamic_state3_color_write_mask = false,
        .depth_bounds = true,
        .vertex_input_dynamic_state = true,
        .feedback_loop = false,
        .mixed_depth_samples = false,
        .mixed_any_samples = false,
        .provoking_vertex = true,
    };
}

Capabilities ModernNvidia() {
    return {
        .vendor_id = 0x10de,
        .nvidia_proprietary = true,
        .fragment_shader_barycentric = true,
        .dynamic_state3 = true,
        .dynamic_state3_color_write_mask = true,
        .depth_bounds = true,
        .vertex_input_dynamic_state = true,
        .feedback_loop = true,
        .mixed_depth_samples = true,
        .provoking_vertex = true,
    };
}

Capabilities Amd() {
    return {
        .vendor_id = 0x1002,
        .amd_explicit_vertex_parameter = true,
        .dynamic_state3 = true,
        .dynamic_state3_color_write_mask = true,
        .depth_bounds = true,
        .vertex_input_dynamic_state = true,
        .feedback_loop = true,
        .mixed_depth_samples = true,
        .mixed_any_samples = true,
        .provoking_vertex = true,
    };
}

std::string ReadSource(const char* path) {
    std::ifstream file(std::string(DCP_SOURCE_DIR) + "/" + path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

bool Contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

void TestFeedbackLoopFallbackRouting() {
    const auto adreno = ChoosePipelineState(AdrenoX2_45());
    const auto nvidia = ChoosePipelineState(ModernNvidia());
    const auto amd = ChoosePipelineState(Amd());

    CHECK(!adreno.feedback_loop);
    CHECK(nvidia.feedback_loop);
    CHECK(amd.feedback_loop);

    // Supported extension route keeps the fast feedback-loop layout unchanged.
    CHECK(ChooseTargetSampleAction(nvidia, true, false, false, false) ==
          TargetSampleAction::FeedbackLoopLayout);
    CHECK(ChooseTargetSampleAction(amd, true, false, false, false) ==
          TargetSampleAction::FeedbackLoopLayout);
    CHECK(UsesFeedbackLoopAttachment(nvidia, true, false));
    CHECK(UsesFeedbackLoopAttachment(amd, true, false));

    // Unsupported extension route snapshots same-target color draws instead of using General layout.
    CHECK(ChooseTargetSampleAction(adreno, true, false, false, false) ==
          TargetSampleAction::SnapshotCopy);
    CHECK(!UsesFeedbackLoopAttachment(adreno, true, true));

    // Unrelated sampled texture takes normal read-only layout on all GPUs.
    CHECK(ChooseTargetSampleAction(adreno, false, false, false, false) ==
          TargetSampleAction::NormalSample);
    CHECK(ChooseTargetSampleAction(nvidia, false, false, false, false) ==
          TargetSampleAction::NormalSample);

    // Storage image bindings (written) always stay in General layout and are never snapshotted.
    CHECK(ChooseTargetSampleAction(adreno, false, true, false, false) ==
          TargetSampleAction::StorageGeneral);
    CHECK(ChooseTargetSampleAction(adreno, true, true, false, false) ==
          TargetSampleAction::StorageGeneral);
    CHECK(ChooseTargetSampleAction(adreno, false, false, true, false) ==
          TargetSampleAction::StorageGeneral);
    CHECK(ChooseTargetSampleAction(adreno, true, false, true, false) ==
          TargetSampleAction::StorageGeneral);

    // Depth images are handled by depth layout logic, never color snapshot.
    CHECK(ChooseTargetSampleAction(adreno, true, false, false, true) ==
          TargetSampleAction::NormalSample);
    CHECK(ChooseTargetSampleAction(nvidia, true, false, false, true) ==
          TargetSampleAction::NormalSample);

    // Next draw state restoration: once binding resets, target is normal attachment and normal sample.
    CHECK(!UsesFeedbackLoopAttachment(adreno, false, false));
    CHECK(ChooseTargetSampleAction(adreno, false, false, false, false) ==
          TargetSampleAction::NormalSample);
}

void TestProvokingVertexPassBreak() {
    // Per-pipeline supported: no render pass break needed.
    CHECK(!NeedsProvokingVertexPassBreak(true, true, true, false, true));
    CHECK(!NeedsProvokingVertexPassBreak(true, true, true, true, false));

    // Per-pipeline unsupported: break pass only when active rendering switches mode.
    CHECK(NeedsProvokingVertexPassBreak(true, false, true, false, true));
    CHECK(NeedsProvokingVertexPassBreak(true, false, true, true, false));
    CHECK(!NeedsProvokingVertexPassBreak(true, false, true, false, false));
    CHECK(!NeedsProvokingVertexPassBreak(true, false, true, true, true));
    CHECK(!NeedsProvokingVertexPassBreak(true, false, false, false, true));

    // Extension unavailable: never breaks.
    CHECK(!NeedsProvokingVertexPassBreak(false, false, true, false, true));
}

void TestStaticPipelineKeyAudit() {
    const std::string key_h =
        ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_graphics_pipeline.h");
    const std::string pipe_cpp =
        ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_graphics_pipeline.cpp");
    const std::string cache_cpp =
        ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp");

    // Verify memcmp equality and XXH3 hashing over the entire GraphicsPipelineKey struct.
    CHECK(Contains(key_h, "std::memcmp(this, &key, sizeof(key)) == 0"));
    CHECK(Contains(key_h, "XXH3_64bits(&key, sizeof(key))"));
    CHECK(Contains(cache_cpp, "std::memset(&sel.graphics_key, 0, sizeof(GraphicsPipelineKey));"));

    // Verify every static state under the Adreno X2-45 profile is keyed, populated, and consumed.
    const char* key_fields[] = {
        "stage_hashes",       "patch_control_points", "num_color_attachments",
        "color_buffers",      "blend_controls",       "write_masks",
        "cb_shader_mask",     "logic_op",             "num_samples",
        "depth_samples",      "color_samples",        "mrt_mask",
        "z_format",           "stencil_format",       "depth_clamp_enable",
        "prim_type",          "polygon_mode",         "clip_space",
        "provoking_vtx_last", "depth_clip_enable",
    };
    for (const char* field : key_fields) {
        CHECK(Contains(key_h, field));
    }

    // Static color write mask and static depth-stencil struct on !dynamic_state3.
    CHECK(Contains(cache_cpp, "key.write_masks[cb] ="));
    CHECK(Contains(pipe_cpp, "state_route.dynamic_color_write_mask\n                    ? vk::ColorComponentFlagBits::eR"));
    CHECK(Contains(pipe_cpp, ": key.write_masks[i],"));
    CHECK(Contains(pipe_cpp, ".pDepthStencilState = state_route.static_depth_stencil_state ? &depth_stencil_info : nullptr"));

    // Provoking vertex First/Last mapping and per-pipeline property query.
    CHECK(Contains(cache_cpp, "key.provoking_vtx_last = regs.polygon_control.provoking_vtx_last;"));
    CHECK(Contains(pipe_cpp, "key.provoking_vtx_last == AmdGpu::ProvokingVtxLast::First"));
    CHECK(Contains(pipe_cpp, "? vk::ProvokingVertexModeEXT::eFirstVertex"));
    CHECK(Contains(pipe_cpp, ": vk::ProvokingVertexModeEXT::eLastVertex"));
}

void TestRasterizerAndInstanceWiring() {
    const std::string raster =
        ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp");
    const std::string instance =
        ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp");
    const std::string image_h = ReadSource("gpu/shadps4/video_core/texture_cache/image.h");

    CHECK(Contains(image_h, "u32 sampled_snapshot : 1;"));
    CHECK(Contains(raster, "GpuCompat::ChooseTargetSampleAction("));
    CHECK(Contains(raster, "TargetSampleAction::SnapshotCopy"));
    CHECK(Contains(raster, "feedback_snapshots->Capture(image, image_id, desc.view_info)"));
    CHECK(Contains(raster, "image.binding.sampled_snapshot = 1u;"));
    CHECK(Contains(raster, "GpuCompat::UsesFeedbackLoopAttachment(state_route, image->binding.is_bound,"));
    CHECK(Contains(raster, "GpuCompat::NeedsProvokingVertexPassBreak("));
    CHECK(Contains(raster, "ReportIssue84Diag(pipeline, is_indexed,"));
    CHECK(Contains(instance, "PhysicalDeviceProvokingVertexPropertiesEXT"));
    CHECK(Contains(instance, "provoking_vertex_mode_per_pipeline"));
    CHECK(Contains(instance, "attachment_feedback_loop_layout_only = true;"));
}

void TestCacheVersionInvalidation() {
    using namespace PipelineCachePolicy;
    const StoreIdentity v141{
        .vendor_id = 0x5143,
        .device_id = 0x1234,
        .driver_id = 10,
        .shader_binary_version = 8u,
        .shader_meta_version = 7u,
        .pipeline_key_version = 5u,
    };
    const StoreIdentity current{
        .vendor_id = 0x5143,
        .device_id = 0x1234,
        .driver_id = 10,
        .shader_binary_version = 8u,
        .shader_meta_version = 8u,
        .pipeline_key_version = 6u,
    };
    const auto old_blob = EncodeStoreIdentity(v141);
    const auto cur_blob = EncodeStoreIdentity(current);
    CHECK(CheckStore(old_blob, current) == StoreVerdict::Incompatible);
    CHECK(CheckStore(cur_blob, current) == StoreVerdict::Reuse);
}

void TestDiagnostics() {
    using namespace DcpIssue84Diag;

    // Auto-enabled only for Qualcomm (0x5143), opt-in via DCP_ISSUE84_DIAG=1 elsewhere.
    CHECK(ShouldEnable(0x5143, nullptr));
    CHECK(ShouldEnable(0x5143, "0"));
    CHECK(!ShouldEnable(0x10de, nullptr));
    CHECK(!ShouldEnable(0x1002, "0"));
    CHECK(!ShouldEnable(0x8086, ""));
    CHECK(ShouldEnable(0x10de, "1"));
    CHECK(ShouldEnable(0x1002, "1"));

    // Ordinary smooth non-hazard draw produces no event.
    DrawInfo normal{};
    normal.vs_hash = 0x1111;
    normal.ps_hash = 0x2222;
    normal.prim_type = 4; // TriList
    normal.num_render_targets = 1;
    normal.interp_smooth = 2;
    CHECK(!Classify(normal).has_value());

    // Color snapshot draw classifies as ColorSnapshot and formats all required markers.
    DrawInfo snap = normal;
    snap.frame = 42;
    snap.draw = 7;
    snap.pipeline_hash = 0xdeadbeefcafebabeULL;
    snap.indexed = true;
    snap.vertex_count = 6;
    snap.provoking_last = true;
    snap.color_write_masks = 0xf;
    snap.blend_enable_mask = 1;
    snap.blend_cb0_src = 4;
    snap.blend_cb0_dst = 5;
    snap.blend_cb0_op = 0;
    snap.depth_stencil_bits = 3;
    snap.target_layout = 2; // ColorAttachment
    snap.sample_layout = 5; // ShaderReadOnly
    snap.snapshot_used = true;
    snap.interp_flat = 1;
    const auto snap_event = Classify(snap);
    CHECK(snap_event.has_value() && snap_event->category == Category::ColorSnapshot);
    const std::string line = Format(*snap_event);
    CHECK(Contains(line, "Issue84 diag [color-snapshot]"));
    CHECK(Contains(line, "f=42 d=7"));
    CHECK(Contains(line, "vs=0000000000001111 ps=0000000000002222"));
    CHECK(Contains(line, "pipe=deadbeefcafebabe"));
    CHECK(Contains(line, "topo=TriList idx=1 verts=6"));
    CHECK(Contains(line, "prov=last"));
    CHECK(Contains(line, "wmask=0xf"));
    CHECK(Contains(line, "fb_ext=no hazard=none t_lay=ColorAttachment s_lay=ShaderReadOnly snap=yes"));
    CHECK(Contains(line, "interp=f1/s2/n0/c0 flags=none intfix=0"));
    CHECK(!Contains(line, "C:\\"));
    CHECK(!Contains(line, "/home/"));

    // Depth feedback, fallback interpolation gap, provoking-last, and flat-blend classifications.
    DrawInfo depth = normal;
    depth.hazard = Hazard::Depth;
    CHECK(Classify(depth)->category == Category::DepthFeedback);

    DrawInfo interp_gap = normal;
    interp_gap.interp_flags = 1u; // mov_non_p0
    CHECK(Classify(interp_gap)->category == Category::InterpFallbackGap);
    CHECK(Contains(Format(*Classify(interp_gap)), "flags=P"));

    DrawInfo prov = normal;
    prov.provoking_last = true;
    prov.prim_type = 6; // TriStrip
    CHECK(Classify(prov)->category == Category::ProvokingLast);

    DrawInfo flat_blend = normal;
    flat_blend.interp_flat = 1;
    flat_blend.blend_enable_mask = 1;
    CHECK(Classify(flat_blend)->category == Category::FlatBlendDraw);

    // Monitor deduplication and kMaxEvents cap.
    Monitor mon;
    mon.Init(0x5143, "");
    CHECK(mon.Active());
    std::vector<std::string> lines;
    const auto sink = [&](const char* s) { lines.emplace_back(s); };
    CHECK(mon.Report(*snap_event, sink));
    CHECK(!mon.Report(*snap_event, sink)); // duplicate suppressed
    CHECK(lines.size() == 1);

    for (std::uint32_t i = 0; i < kMaxEvents + 10; ++i) {
        DrawInfo d = flat_blend;
        d.ps_hash = 0x10000 + i;
        if (const auto ev = Classify(d)) {
            mon.Report(*ev, sink);
        }
    }
    CHECK(mon.Emitted() == kMaxEvents);
    CHECK(lines.size() == kMaxEvents);
    CHECK(!mon.Active());
}

} // namespace

int main() {
    TestFeedbackLoopFallbackRouting();
    TestProvokingVertexPassBreak();
    TestStaticPipelineKeyAudit();
    TestRasterizerAndInstanceWiring();
    TestCacheVersionInvalidation();
    TestDiagnostics();
    if (g_failures) {
        std::printf("issue #84 tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("issue #84 tests: all passed\n");
    return 0;
}
