// SPDX-License-Identifier: GPL-2.0-or-later
// dcp: capability-driven renderer routes, kept free of Vulkan headers so they can be tested headless.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace Vulkan::GpuCompat {

struct Capabilities {
    std::uint32_t vendor_id{};
    bool nvidia_proprietary{};
    bool amd_explicit_vertex_parameter{};
    bool fragment_shader_barycentric{};
    bool dynamic_state3{};
    bool dynamic_state3_color_write_mask{};
    bool depth_bounds{};
    bool vertex_input_dynamic_state{};
    bool feedback_loop{};
    bool mixed_depth_samples{};
    bool mixed_any_samples{};
    bool provoking_vertex{};
};

enum class Interpolation { AmdExplicit, NativeBarycentric, Fallback };

struct InterpolationRoute {
    Interpolation interpolation{};
    bool manual_interpolation{};
    bool integer_fix{};
};

constexpr InterpolationRoute ChooseInterpolation(const Capabilities& caps) {
    InterpolationRoute route{};
    if (caps.amd_explicit_vertex_parameter) {
        route.interpolation = Interpolation::AmdExplicit;
    } else if (caps.fragment_shader_barycentric) {
        route.interpolation = Interpolation::NativeBarycentric;
    } else {
        route.interpolation = Interpolation::Fallback;
    }
    route.manual_interpolation =
        route.interpolation == Interpolation::NativeBarycentric && caps.nvidia_proprietary;
    // Only GCN-style interpolation (AMD) and the manual FMA path reproduce equal vertex values
    // exactly; Vulkan leaves driver interpolation precision implementation-defined.
    route.integer_fix =
        route.interpolation != Interpolation::AmdExplicit && !route.manual_interpolation;
    return route;
}

struct PipelineStateRoute {
    bool dynamic_color_write_mask{};
    bool dynamic_depth_bounds{};
    bool dynamic_vertex_input{};
    bool static_depth_stencil_state{};
    bool feedback_loop{};
    bool provoking_vertex_state{};
    bool mixed_samples_info{};
};

constexpr PipelineStateRoute ChoosePipelineState(const Capabilities& caps) {
    return {
        .dynamic_color_write_mask = caps.dynamic_state3 && caps.dynamic_state3_color_write_mask,
        .dynamic_depth_bounds = caps.depth_bounds,
        .dynamic_vertex_input = caps.vertex_input_dynamic_state,
        .static_depth_stencil_state = !caps.dynamic_state3,
        .feedback_loop = caps.feedback_loop,
        .provoking_vertex_state = caps.provoking_vertex,
        .mixed_samples_info = caps.mixed_depth_samples,
    };
}

constexpr bool ForceColorSamplesToDepth(const Capabilities& caps, bool all_color_samples_same,
                                        bool depth_mismatch) {
    return (!all_color_samples_same && !caps.mixed_any_samples) ||
           (all_color_samples_same && depth_mismatch && !caps.mixed_depth_samples);
}

constexpr bool NeedsMixedSamplesInfo(const PipelineStateRoute& route,
                                     std::uint32_t rasterization_samples,
                                     const std::uint32_t* color_samples,
                                     std::uint32_t color_attachment_count,
                                     bool has_depth_stencil, std::uint32_t depth_samples) {
    if (!route.mixed_samples_info) {
        return false;
    }
    const std::uint32_t raster = rasterization_samples ? rasterization_samples : 1u;
    for (std::uint32_t i = 0; i < color_attachment_count; ++i) {
        const std::uint32_t samples = color_samples[i] ? color_samples[i] : 1u;
        if (samples != raster) {
            return true;
        }
    }
    if (has_depth_stencil) {
        const std::uint32_t depth = depth_samples ? depth_samples : 1u;
        if (depth != raster) {
            return true;
        }
    }
    return false;
}

constexpr const char* InterpolationName(Interpolation interpolation) {
    switch (interpolation) {
    case Interpolation::AmdExplicit:
        return "amd_explicit";
    case Interpolation::NativeBarycentric:
        return "native_barycentric";
    case Interpolation::Fallback:
        return "fallback";
    }
    return "unknown";
}

inline std::string Summary(const Capabilities& caps) {
    const InterpolationRoute route = ChooseInterpolation(caps);
    const auto yes_no = [](bool value) { return value ? "yes" : "no"; };
    char line[256];
    std::snprintf(line, sizeof(line),
                  "GPU compatibility: vendor=%#06x interpolation=%s manual_interpolation=%s "
                  "integer_fix=%s dynamic_state3=%s feedback_loop=%s mixed_samples=%s "
                  "provoking_vertex=%s",
                  static_cast<unsigned>(caps.vendor_id), InterpolationName(route.interpolation),
                  route.manual_interpolation ? "on" : "off", route.integer_fix ? "on" : "off",
                  yes_no(caps.dynamic_state3), yes_no(caps.feedback_loop),
                  yes_no(caps.mixed_depth_samples), yes_no(caps.provoking_vertex));
    return line;
}

} // namespace Vulkan::GpuCompat
