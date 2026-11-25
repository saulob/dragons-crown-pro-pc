// Focused tests of the renderer capability routes (issue #84): interpolation path, integer fix, pipeline state
// fallbacks for missing extended dynamic state 3, feedback loop, mixed samples and provoking vertex.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "shader_recompiler/ir/passes/interpolated_integer.h"
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
using Shader::Optimization::NudgeInterpolatedInteger;

Capabilities ModernNvidia() {
    return {.vendor_id = 0x10de, .nvidia_proprietary = true, .fragment_shader_barycentric = true,
            .dynamic_state3 = true, .dynamic_state3_color_write_mask = true, .depth_bounds = true,
            .vertex_input_dynamic_state = true, .feedback_loop = true, .mixed_depth_samples = true,
            .provoking_vertex = true};
}

Capabilities PascalNvidia() {
    Capabilities caps = ModernNvidia();
    caps.fragment_shader_barycentric = false;
    return caps;
}

Capabilities Amd() {
    return {.vendor_id = 0x1002, .amd_explicit_vertex_parameter = true, .dynamic_state3 = true,
            .dynamic_state3_color_write_mask = true, .depth_bounds = true, .vertex_input_dynamic_state = true,
            .feedback_loop = true, .mixed_depth_samples = true, .mixed_any_samples = true,
            .provoking_vertex = true};
}

// Adreno X2-45, driver 512.879.0 (issue #84 log).
Capabilities Adreno() {
    return {.vendor_id = 0x5143, .depth_bounds = true, .vertex_input_dynamic_state = true,
            .provoking_vertex = true};
}

Capabilities GenericVulkan() {
    return {.vendor_id = 0x13b5};
}

Capabilities IntelWithBarycentric() {
    return {.vendor_id = 0x8086, .fragment_shader_barycentric = true, .dynamic_state3 = true,
            .vertex_input_dynamic_state = true};
}

// The rule before issue #84.
bool PreviousIntegerFix(const Capabilities& caps) {
    return !caps.fragment_shader_barycentric && caps.nvidia_proprietary;
}

void TestInterpolationRoutes() {
    const auto modern = ChooseInterpolation(ModernNvidia());
    CHECK(modern.interpolation == Interpolation::NativeBarycentric);
    CHECK(modern.manual_interpolation && !modern.integer_fix);

    const auto pascal = ChooseInterpolation(PascalNvidia());
    CHECK(pascal.interpolation == Interpolation::Fallback);
    CHECK(!pascal.manual_interpolation && pascal.integer_fix);

    const auto amd = ChooseInterpolation(Amd());
    CHECK(amd.interpolation == Interpolation::AmdExplicit);
    CHECK(!amd.manual_interpolation && !amd.integer_fix);

    const auto adreno = ChooseInterpolation(Adreno());
    CHECK(adreno.interpolation == Interpolation::Fallback);
    CHECK(!adreno.manual_interpolation && adreno.integer_fix);

    const auto generic = ChooseInterpolation(GenericVulkan());
    CHECK(generic.interpolation == Interpolation::Fallback);
    CHECK(!generic.manual_interpolation && generic.integer_fix);

    const auto intel = ChooseInterpolation(IntelWithBarycentric());
    CHECK(intel.interpolation == Interpolation::NativeBarycentric);
    CHECK(!intel.manual_interpolation && intel.integer_fix);
}

// The NVIDIA routes keep their previous behaviour; the Adreno is the profile the old vendor rule missed.
void TestPreviousRuleRegression() {
    for (const Capabilities& caps : {ModernNvidia(), PascalNvidia(), Amd()}) {
        CHECK(ChooseInterpolation(caps).integer_fix == PreviousIntegerFix(caps));
    }
    CHECK(!PreviousIntegerFix(Adreno()));
    CHECK(ChooseInterpolation(Adreno()).integer_fix);
}

// The route depends on capabilities only: the same capabilities under another vendor id choose the same path.
void TestNoVendorGate() {
    Capabilities relabelled = Adreno();
    relabelled.vendor_id = 0x1234;
    const auto a = ChooseInterpolation(Adreno());
    const auto b = ChooseInterpolation(relabelled);
    CHECK(a.interpolation == b.interpolation && a.integer_fix == b.integer_fix &&
          a.manual_interpolation == b.manual_interpolation);
}

void TestAdrenoPipelineState() {
    const auto state = ChoosePipelineState(Adreno());
    CHECK(!state.dynamic_color_write_mask);
    CHECK(state.static_depth_stencil_state);
    CHECK(state.dynamic_vertex_input);
    CHECK(state.dynamic_depth_bounds);
    CHECK(!state.feedback_loop);
    CHECK(state.provoking_vertex_state);
    CHECK(!state.mixed_samples_info);

    // Dynamic state 3 present without the color write mask feature keeps the static mask.
    Capabilities partial = Adreno();
    partial.dynamic_state3 = true;
    CHECK(!ChoosePipelineState(partial).dynamic_color_write_mask);
    CHECK(!ChoosePipelineState(partial).static_depth_stencil_state);

    const auto nvidia = ChoosePipelineState(ModernNvidia());
    CHECK(nvidia.dynamic_color_write_mask && !nvidia.static_depth_stencil_state && nvidia.feedback_loop &&
          nvidia.mixed_samples_info);
}

void TestMixedSamplesFallback() {
    const Capabilities adreno = Adreno();
    CHECK(ForceColorSamplesToDepth(adreno, true, true));
    CHECK(ForceColorSamplesToDepth(adreno, false, false));
    CHECK(!ForceColorSamplesToDepth(adreno, true, false));

    const Capabilities nvidia = ModernNvidia();
    CHECK(!ForceColorSamplesToDepth(nvidia, true, true));
    CHECK(ForceColorSamplesToDepth(nvidia, false, false));
    CHECK(!ForceColorSamplesToDepth(Amd(), false, true));
}

void TestMixedSamplesPNextPolicy() {
    const auto amd_route = ChoosePipelineState(Amd());
    const auto nvidia_route = ChoosePipelineState(ModernNvidia());
    const auto adreno_route = ChoosePipelineState(Adreno());

    const std::uint32_t single_1x[1] = {1};
    const std::uint32_t dual_1x[2] = {1, 1};
    const std::uint32_t dual_4x[2] = {4, 4};
    const std::uint32_t mixed_1x_4x[2] = {1, 4};

    // Uniform 1x attachments omit VkAttachmentSampleCountInfoAMD.
    CHECK(!NeedsMixedSamplesInfo(amd_route, 1, single_1x, 1, true, 1));
    CHECK(!NeedsMixedSamplesInfo(amd_route, 1, dual_1x, 2, false, 1));
    CHECK(!NeedsMixedSamplesInfo(nvidia_route, 1, single_1x, 1, true, 1));

    // Uniform multisample attachments (color == depth == raster) omit pNext.
    CHECK(!NeedsMixedSamplesInfo(amd_route, 4, dual_4x, 2, true, 4));
    CHECK(!NeedsMixedSamplesInfo(amd_route, 4, dual_4x, 2, false, 1));
    CHECK(!NeedsMixedSamplesInfo(nvidia_route, 4, dual_4x, 2, true, 4));

    // Depth-only pass with matching rasterization samples omits pNext.
    CHECK(!NeedsMixedSamplesInfo(amd_route, 4, nullptr, 0, true, 4));
    CHECK(!NeedsMixedSamplesInfo(amd_route, 1, nullptr, 0, false, 1));

    // Genuine color/depth sample count difference requires pNext on AMD and NVIDIA.
    CHECK(NeedsMixedSamplesInfo(amd_route, 4, single_1x, 1, true, 4));
    CHECK(NeedsMixedSamplesInfo(amd_route, 4, dual_4x, 2, true, 1));
    CHECK(NeedsMixedSamplesInfo(amd_route, 4, mixed_1x_4x, 2, true, 4));
    CHECK(NeedsMixedSamplesInfo(nvidia_route, 4, single_1x, 1, true, 4));

    // Without capability, pNext is never attached and fallback forcing remains intact.
    CHECK(!NeedsMixedSamplesInfo(adreno_route, 4, single_1x, 1, true, 4));
    CHECK(!NeedsMixedSamplesInfo(adreno_route, 4, mixed_1x_4x, 2, true, 4));

    // NVIDIA forces differing color samples to depth before pipeline creation, resulting in uniform samples.
    CHECK(ForceColorSamplesToDepth(ModernNvidia(), false, false));
    CHECK(!NeedsMixedSamplesInfo(nvidia_route, 4, dual_4x, 2, true, 4));
}

// DCP-TKXB2V: Linux SteamOS, AMD Ryzen Z2 Go (RADV REMBRANDT), Gamescope, Vulkan with
// VK_AMD_mixed_attachment_samples and Steam Fossilize (which rejects sType 1000044008).
void TestDcpTkxb2vRegression() {
    const Capabilities radv_rembrandt = Amd();
    const auto route = ChoosePipelineState(radv_rembrandt);
    CHECK(route.mixed_samples_info);

    const std::uint32_t uniform_1x[2] = {1, 1};
    const std::uint32_t uniform_4x[1] = {4};
    const std::uint32_t mixed_color_depth[1] = {1};

    // Standard gameplay pipelines avoid attaching sType 1000044008 so Fossilize can record them.
    CHECK(!NeedsMixedSamplesInfo(route, 1, uniform_1x, 1, true, 1));
    CHECK(!NeedsMixedSamplesInfo(route, 1, uniform_1x, 2, false, 1));
    CHECK(!NeedsMixedSamplesInfo(route, 4, uniform_4x, 1, true, 4));

    // Genuinely mixed-sample pipelines preserve VK_AMD_mixed_attachment_samples on RADV.
    CHECK(NeedsMixedSamplesInfo(route, 4, mixed_color_depth, 1, true, 4));
}

void TestSummary() {
    const std::string adreno = Summary(Adreno());
    CHECK(adreno == "GPU compatibility: vendor=0x5143 interpolation=fallback manual_interpolation=off integer_fix=on "
                    "dynamic_state3=no feedback_loop=no mixed_samples=no provoking_vertex=yes");
    CHECK(Summary(ModernNvidia()).find("interpolation=native_barycentric manual_interpolation=on integer_fix=off") !=
          std::string::npos);
    CHECK(Summary(Amd()).find("interpolation=amd_explicit") != std::string::npos);
}

struct Vertex {
    float value;
    float w;
};

// GCN V_INTERP_P1/P2 and the manual path: P0 + I * (P1 - P0) + J * (P2 - P0).
float GcnInterpolate(const Vertex (&v)[3], float i, float j) {
    const float p1 = std::fma(v[1].value - v[0].value, i, v[0].value);
    return std::fma(v[2].value - v[0].value, j, p1);
}

// A driver that interpolates value / w linearly in screen space and divides by the interpolated 1 / w.
float PerspectiveInterpolate(const Vertex (&v)[3], float l1, float l2) {
    const float l0 = 1.0f - l1 - l2;
    const float num = l0 * (v[0].value / v[0].w) + l1 * (v[1].value / v[1].w) + l2 * (v[2].value / v[2].w);
    const float den = l0 / v[0].w + l1 / v[1].w + l2 / v[2].w;
    return num / den;
}

void TestIntegerInterpolation() {
    int driver_errors = 0;
    int nudged_errors = 0;
    int gcn_errors = 0;
    for (const float integer : {1.0f, 3.0f, 7.0f, 41.0f, 255.0f, 1023.0f}) {
        for (const float w1 : {0.3f, 1.7f, 13.0f}) {
            const Vertex v[3] = {{integer, 1.0f}, {integer, w1}, {integer, 37.0f}};
            for (int a = 0; a <= 64; ++a) {
                for (int b = 0; a + b <= 64; ++b) {
                    const float l1 = a / 64.0f;
                    const float l2 = b / 64.0f;
                    const float driver = PerspectiveInterpolate(v, l1, l2);
                    driver_errors += std::trunc(driver) != integer;
                    nudged_errors += std::trunc(NudgeInterpolatedInteger(driver)) != integer;
                    gcn_errors += std::trunc(GcnInterpolate(v, l1, l2)) != integer;
                }
            }
        }
    }
    CHECK(driver_errors > 0);
    CHECK(nudged_errors == 0);
    CHECK(gcn_errors == 0);

    for (const float n : {-5.0f, -1.0f}) {
        CHECK(std::trunc(NudgeInterpolatedInteger(std::nextafter(n, 0.0f))) == n);
    }
    // Values away from an integer move by about 2^-16 of their magnitude (plus the product's rounding).
    for (const float x : {0.25f, 2.5f, 100.75f, -3.5f}) {
        CHECK(std::fabs(NudgeInterpolatedInteger(x) - x) <= std::fabs(x) * 2e-5f);
        CHECK(std::trunc(NudgeInterpolatedInteger(x)) == std::trunc(x));
    }
}

// Pull model: I/W, J/W, 1/W. Without the AMD built-in, I/W = perspective I * FragCoord.w.
void TestPullModelReconstruction() {
    const float w[3] = {2.0f, 5.0f, 0.5f};
    for (const float l1 : {0.1f, 0.33f, 0.6f}) {
        for (const float l2 : {0.05f, 0.25f, 0.3f}) {
            const float l0 = 1.0f - l1 - l2;
            const float inv_w = l0 / w[0] + l1 / w[1] + l2 / w[2];
            const float persp_i = (l1 / w[1]) / inv_w;
            const float persp_j = (l2 / w[2]) / inv_w;
            const float pull_i = persp_i * inv_w;
            const float pull_j = persp_j * inv_w;
            CHECK(std::fabs(pull_i - l1 / w[1]) < 1e-6f);
            CHECK(std::fabs(pull_j - l2 / w[2]) < 1e-6f);
            CHECK(std::fabs(pull_i / inv_w - persp_i) < 1e-6f);
        }
    }
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

void TestRendererUsesRoutes() {
    const std::string cache = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp");
    CHECK(Contains(cache, ".needs_manual_interpolation = interpolation.manual_interpolation"));
    CHECK(Contains(cache, ".needs_integer_interpolation_fix = interpolation.integer_fix"));
    CHECK(Contains(cache, "GpuCompat::Summary(gpu_caps)"));
    CHECK(Contains(cache, "GpuCompat::ForceColorSamplesToDepth(gpu_caps"));
    CHECK(!Contains(cache, "0x5143"));

    const std::string pipeline = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_graphics_pipeline.cpp");
    CHECK(Contains(pipeline, "if (state_route.dynamic_color_write_mask) {\n        dynamic_states.push_back(vk::DynamicState::eColorWriteMaskEXT);"));
    CHECK(Contains(pipeline, "if (state_route.dynamic_depth_bounds) {\n        dynamic_states.push_back(vk::DynamicState::eDepthBoundsTestEnable);"));
    CHECK(Contains(pipeline, "if (state_route.dynamic_vertex_input) {\n        dynamic_states.push_back(vk::DynamicState::eVertexInputEXT);"));
    CHECK(Contains(pipeline, ".pDepthStencilState = state_route.static_depth_stencil_state ? &depth_stencil_info : nullptr"));
    CHECK(Contains(pipeline, "state_route.dynamic_color_write_mask\n                    ? vk::ColorComponentFlagBits::eR"));
    CHECK(Contains(pipeline, "if (!state_route.provoking_vertex_state) {\n        raster_chain.unlink<vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT>();"));
    CHECK(Contains(pipeline, "GpuCompat::NeedsMixedSamplesInfo(\n        state_route,"));
    CHECK(Contains(pipeline, ".pNext = attach_mixed_samples ? &mixed_samples : nullptr"));

    const std::string instance = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp");
    CHECK(Contains(instance, "feature_chain.get<vk::PhysicalDeviceProvokingVertexFeaturesEXT>().provokingVertexLast &&\n        add_extension(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME)"));
    CHECK(Contains(instance, "add_extension(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME);\n        if (!attachment_feedback_loop) {\n            // We want both extensions so remove the first if the second isn't available\n            enabled_extensions.pop_back();"));

    const std::string scheduler = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.h");
    CHECK(Contains(scheduler, "if (dirty_state.feedback_loop_enabled && feedback_loop_supported) {"));
}

void TestShaderPaths() {
    const std::string pass = ReadSource("gpu/shadps4/shader_recompiler/ir/passes/interpolated_integer_pass.cpp");
    CHECK(Contains(pass, "return primary != Qualifier::Flat && primary != Qualifier::PerVertex;"));
    CHECK(Contains(pass, "InterpolatedIntegerScale"));

    const std::string recompiler = ReadSource("gpu/shadps4/shader_recompiler/recompiler.cpp");
    CHECK(Contains(recompiler, "if (info.sw_stage == SwStage::Fragment && profile.needs_integer_interpolation_fix) {"));

    // Without either barycentric mechanism the guest I/J VGPRs are not loaded; V_INTERP uses driver interpolation.
    const std::string translate = ReadSource("gpu/shadps4/shader_recompiler/frontend/translate/translate.cpp");
    CHECK(Contains(translate, "if (profile.supports_amd_shader_explicit_vertex_parameter ||\n                    profile.supports_fragment_shader_barycentric) {\n                    ir.SetVectorReg(IR::VectorReg(vreg), ir.GetAttribute(attrib, comp));"));
    const std::string vintrp = ReadSource("gpu/shadps4/shader_recompiler/frontend/translate/vector_interpolation.cpp");
    CHECK(Contains(vintrp, "const auto next = GetInterpolation(vgpr_to_interp[inst.src[0].code]);"));
    CHECK(Contains(vintrp, "SetDst(inst.dst[0], ir.GetAttribute(attrib, inst.control.vintrp.chan));"));
    CHECK(Contains(vintrp, "interp.primary = Qualifier::Flat;"));

    const std::string emit = ReadSource("gpu/shadps4/shader_recompiler/backend/spirv/emit_spirv_context_get_set.cpp");
    CHECK(Contains(emit, "ctx.OpCompositeExtract(ctx.F32[1], ctx.OpLoad(ctx.F32[3], ctx.bary_coord), comp + 1);\n        return ctx.OpFMul(ctx.F32[1], ij, inv_w);"));

    const std::string serialization =
        ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp");
    CHECK(Contains(serialization, "ShaderBinaryVersion = 8u;"));
    CHECK(Contains(serialization, "ShaderMetaVersion = 8u;"));
    CHECK(Contains(serialization, "PipelineKeyVersion = 6u;"));
}

} // namespace

int main() {
    TestInterpolationRoutes();
    TestPreviousRuleRegression();
    TestNoVendorGate();
    TestAdrenoPipelineState();
    TestMixedSamplesFallback();
    TestMixedSamplesPNextPolicy();
    TestDcpTkxb2vRegression();
    TestSummary();
    TestIntegerInterpolation();
    TestPullModelReconstruction();
    TestRendererUsesRoutes();
    TestShaderPaths();
    if (g_failures) {
        std::printf("gpu compat tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("gpu compat tests: all passed\n");
    return 0;
}
