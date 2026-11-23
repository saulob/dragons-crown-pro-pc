// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D maskSampler;

layout (push_constant) uniform settings {
    vec4 outline_color;
    vec2 mask_size;
    float pixels;
    uvec4 color_swizzle;
} highlight;

const vec2 directions[8] = vec2[](
    vec2(-1.0, 0.0), vec2(1.0, 0.0), vec2(0.0, -1.0), vec2(0.0, 1.0),
    vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0)
);

float swizzle_component(vec4 color, uint channel) {
    if (channel == 0u) return 0.0;
    if (channel == 1u) return 1.0;
    return color[channel - 4u];
}

void main() {
    const vec2 uv = gl_FragCoord.xy / highlight.mask_size;
    const float center = texture(maskSampler, uv).a;
    float around = center;
    for (int i = 0; i < 8; ++i) {
        around = max(around, texture(maskSampler, uv + directions[i] * highlight.pixels / highlight.mask_size).a);
    }
    float coverage = max(around - center, 0.0);
    if (center > (0.5 / 255.0)) {
        coverage = 0.0;
    }
    const vec4 logical = vec4(highlight.outline_color.rgb, coverage);
    color = vec4(
        swizzle_component(logical, highlight.color_swizzle.x),
        swizzle_component(logical, highlight.color_swizzle.y),
        swizzle_component(logical, highlight.color_swizzle.z),
        swizzle_component(logical, highlight.color_swizzle.w)
    );
}
