// SPDX-License-Identifier: GPL-2.0-or-later
// The last values sent to the driver. All tracked OpenGL state goes through here, so a call that
// changes nothing never reaches it.
#pragma once

#include <array>
#include <cstring>
#include "common/types.h"
#include "video_core/renderer_opengl/gl_api.h"
#include "video_core/renderer_opengl/gl_perf.h"

namespace OpenGL {

class GlState {
public:
    enum class Cap : u32 {
        ScissorTest,
        DepthTest,
        StencilTest,
        CullFace,
        PolygonOffsetFill,
        DepthClamp,
        RasterizerDiscard,
        PrimitiveRestart,
        ColorLogicOp,
        FramebufferSrgb,
        ProgramPointSize,
        CapCount,
    };

    static constexpr u32 MaxUnits = 48;
    /// OpenGL 3.3 guarantees 48 combined units: the last is kept for uploads, so that one never
    /// disturbs what a draw has bound.
    static constexpr GLuint EditUnit = MaxUnits - 1;
    static constexpr u32 MaxAttributes = 16;
    static constexpr u32 MaxColorTargets = 8;

    GlState() {
        // DEBUG_SAULO: the switch goes with the rest of the profile.
        filter = Perf::Optimizations().state;
        std::memset(&attributes, 0xFF, sizeof(attributes));
        Invalidate();
    }

    /// After code that does not go through here has touched the context.
    void Invalidate() {
        // 0xFF never equals a real value: floats become NaN, handles and enums an impossible one.
        std::memset(&shadow, 0xFF, sizeof(shadow));
        known_caps = 0;
    }

    [[nodiscard]] bool Filtering() const {
        return filter;
    }

    void Enable(Cap cap, bool on) {
        static constexpr std::array<GLenum, u32(Cap::CapCount)> names{
            GL_SCISSOR_TEST,        GL_DEPTH_TEST,        GL_STENCIL_TEST,
            GL_CULL_FACE,           GL_POLYGON_OFFSET_FILL, GL_DEPTH_CLAMP,
            GL_RASTERIZER_DISCARD,  GL_PRIMITIVE_RESTART, GL_COLOR_LOGIC_OP,
            GL_FRAMEBUFFER_SRGB,    GL_PROGRAM_POINT_SIZE,
        };
        const u32 bit = 1u << u32(cap);
        if (filter && (known_caps & bit) != 0 && ((enabled_caps & bit) != 0) == on) {
            return;
        }
        known_caps |= bit;
        enabled_caps = on ? enabled_caps | bit : enabled_caps & ~bit;
        if (on) {
            gl::Enable(names[u32(cap)]);
        } else {
            gl::Disable(names[u32(cap)]);
        }
        Perf::Count(cap == Cap::DepthTest     ? Perf::DepthChanges
                    : cap == Cap::StencilTest ? Perf::StencilChanges
                                              : Perf::RasterChanges);
    }

    void UseProgram(GLuint program) {
        if (Changed(shadow.program, program)) {
            gl::UseProgram(program);
            Perf::Count(Perf::Programs);
        }
    }

    void BindFramebuffer(GLuint framebuffer) {
        const bool read = Changed(shadow.read_framebuffer, framebuffer);
        const bool draw = Changed(shadow.draw_framebuffer, framebuffer);
        if (read || draw) {
            gl::BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            Perf::Count(Perf::Framebuffers);
        }
    }

    void BindReadFramebuffer(GLuint framebuffer) {
        if (Changed(shadow.read_framebuffer, framebuffer)) {
            gl::BindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
            Perf::Count(Perf::Framebuffers);
        }
    }

    void BindDrawFramebuffer(GLuint framebuffer) {
        if (Changed(shadow.draw_framebuffer, framebuffer)) {
            gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
            Perf::Count(Perf::Framebuffers);
        }
    }

    /// Before deleting framebuffers: their names can come back as new objects.
    void ForgetFramebuffers() {
        shadow.read_framebuffer = ~0u;
        shadow.draw_framebuffer = ~0u;
    }

    void BindVertexArray(GLuint vertex_array) {
        if (Changed(shadow.vertex_array, vertex_array)) {
            gl::BindVertexArray(vertex_array);
            // The element buffer binding belongs to the vertex array.
            shadow.element_buffer = ~0u;
            Perf::Count(Perf::VertexArrays);
        }
    }

    void BindArrayBuffer(GLuint buffer) {
        if (Changed(shadow.array_buffer, buffer)) {
            gl::BindBuffer(GL_ARRAY_BUFFER, buffer);
            Perf::Count(Perf::BufferBinds);
        }
    }

    void BindElementBuffer(GLuint buffer) {
        if (Changed(shadow.element_buffer, buffer)) {
            gl::BindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
            Perf::Count(Perf::BufferBinds);
        }
    }

    void ActiveTexture(GLuint unit) {
        if (Changed(shadow.active_unit, unit)) {
            gl::ActiveTexture(GL_TEXTURE0 + unit);
        }
    }

    void BindTexture(GLuint unit, GLenum target, GLuint texture) {
        if (unit < MaxUnits && !Changed(shadow.textures[unit][TargetSlot(target)], texture)) {
            return;
        }
        ActiveTexture(unit);
        gl::BindTexture(target, texture);
        Perf::Count(Perf::TextureBinds);
    }

    /// Binds a texture to change it; the calls that follow act on the active unit.
    void BindForEdit(GLenum target, GLuint texture) {
        BindTexture(EditUnit, target, texture);
        ActiveTexture(EditUnit);
    }

    /// Before deleting textures: their names can come back as new objects.
    void ForgetTextures() {
        std::memset(&shadow.textures, 0xFF, sizeof(shadow.textures));
    }

    void BindSampler(GLuint unit, GLuint sampler) {
        if (unit >= MaxUnits || Changed(shadow.samplers[unit], sampler)) {
            gl::BindSampler(unit, sampler);
            Perf::Count(Perf::SamplerBinds);
        }
    }

    void Viewport(GLint x, GLint y, GLsizei width, GLsizei height) {
        if (Changed(shadow.viewport, std::array<GLint, 4>{x, y, width, height})) {
            gl::Viewport(x, y, width, height);
            Perf::Count(Perf::Viewports);
        }
    }

    void Scissor(GLint x, GLint y, GLsizei width, GLsizei height) {
        if (Changed(shadow.scissor, std::array<GLint, 4>{x, y, width, height})) {
            gl::Scissor(x, y, width, height);
            Perf::Count(Perf::Scissors);
        }
    }

    void DepthRange(GLdouble near_value, GLdouble far_value) {
        if (Changed(shadow.depth_range, std::array<GLdouble, 2>{near_value, far_value})) {
            gl::DepthRange(near_value, far_value);
            Perf::Count(Perf::Viewports);
        }
    }

    void DepthFunc(GLenum func) {
        if (Changed(shadow.depth_func, func)) {
            gl::DepthFunc(func);
            Perf::Count(Perf::DepthChanges);
        }
    }

    void DepthMask(GLboolean mask) {
        if (Changed(shadow.depth_mask, u8(mask))) {
            gl::DepthMask(mask);
            Perf::Count(Perf::DepthChanges);
        }
    }

    void StencilFunc(GLenum face, GLenum func, GLint reference, GLuint mask) {
        const std::array<u32, 3> value{func, u32(reference), mask};
        if (Changed(shadow.stencil_func[Face(face)], value)) {
            gl::StencilFuncSeparate(face, func, reference, mask);
            Perf::Count(Perf::StencilChanges);
        }
    }

    void StencilMask(GLenum face, GLuint mask) {
        if (Changed(shadow.stencil_mask[Face(face)], mask)) {
            gl::StencilMaskSeparate(face, mask);
            Perf::Count(Perf::StencilChanges);
        }
    }

    void StencilOp(GLenum face, GLenum stencil_fail, GLenum depth_fail, GLenum pass) {
        const std::array<GLenum, 3> value{stencil_fail, depth_fail, pass};
        if (Changed(shadow.stencil_op[Face(face)], value)) {
            gl::StencilOpSeparate(face, stencil_fail, depth_fail, pass);
            Perf::Count(Perf::StencilChanges);
        }
    }

    void CullFace(GLenum face) {
        if (Changed(shadow.cull_face, face)) {
            gl::CullFace(face);
            Perf::Count(Perf::RasterChanges);
        }
    }

    void FrontFace(GLenum mode) {
        if (Changed(shadow.front_face, mode)) {
            gl::FrontFace(mode);
            Perf::Count(Perf::RasterChanges);
        }
    }

    void PolygonMode(GLenum mode) {
        if (Changed(shadow.polygon_mode, mode)) {
            gl::PolygonMode(GL_FRONT_AND_BACK, mode);
            Perf::Count(Perf::RasterChanges);
        }
    }

    void PolygonOffset(GLfloat factor, GLfloat units) {
        if (Changed(shadow.polygon_offset, std::array<GLfloat, 2>{factor, units})) {
            gl::PolygonOffset(factor, units);
            Perf::Count(Perf::RasterChanges);
        }
    }

    void LineWidth(GLfloat width) {
        if (Changed(shadow.line_width, width)) {
            gl::LineWidth(width);
            Perf::Count(Perf::RasterChanges);
        }
    }

    void ProvokingVertex(GLenum mode) {
        if (Changed(shadow.provoking_vertex, mode)) {
            gl::ProvokingVertex(mode);
            Perf::Count(Perf::RasterChanges);
        }
    }

    void PrimitiveRestartIndex(GLuint index) {
        if (Changed(shadow.restart_index, index)) {
            gl::PrimitiveRestartIndex(index);
            Perf::Count(Perf::RasterChanges);
        }
    }

    void BlendColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
        if (Changed(shadow.blend_color, std::array<GLfloat, 4>{red, green, blue, alpha})) {
            gl::BlendColor(red, green, blue, alpha);
            Perf::Count(Perf::BlendChanges);
        }
    }

    void LogicOp(GLenum op) {
        if (Changed(shadow.logic_op, op)) {
            gl::LogicOp(op);
            Perf::Count(Perf::BlendChanges);
        }
    }

    void BlendEnable(u32 target, bool on) {
        if (Changed(shadow.blend_enable[target], u8(on))) {
            if (on) {
                gl::Enablei(GL_BLEND, target);
            } else {
                gl::Disablei(GL_BLEND, target);
            }
            Perf::Count(Perf::BlendChanges);
        }
    }

    /// `mask` holds red in bit 0 up to alpha in bit 3.
    void ColorMask(u32 target, u32 mask) {
        if (Changed(shadow.color_mask[target], u8(mask))) {
            gl::ColorMaski(target, (mask & 1) != 0, (mask & 2) != 0, (mask & 4) != 0,
                           (mask & 8) != 0);
            Perf::Count(Perf::MaskChanges);
        }
    }

    /// `per_target` needs OpenGL 4.0; otherwise the one blend state is kept in slot 0.
    void BlendFunc(u32 target, bool per_target, GLenum src_color, GLenum dst_color,
                   GLenum src_alpha, GLenum dst_alpha) {
        const std::array<GLenum, 4> value{src_color, dst_color, src_alpha, dst_alpha};
        if (!Changed(shadow.blend_func[per_target ? target : 0], value)) {
            return;
        }
        if (per_target) {
            gl::BlendFuncSeparatei(target, src_color, dst_color, src_alpha, dst_alpha);
        } else {
            gl::BlendFuncSeparate(src_color, dst_color, src_alpha, dst_alpha);
        }
        Perf::Count(Perf::BlendChanges);
    }

    void BlendEquation(u32 target, bool per_target, GLenum color, GLenum alpha) {
        const std::array<GLenum, 2> value{color, alpha};
        if (!Changed(shadow.blend_equation[per_target ? target : 0], value)) {
            return;
        }
        if (per_target) {
            gl::BlendEquationSeparatei(target, color, alpha);
        } else {
            gl::BlendEquationSeparate(color, alpha);
        }
        Perf::Count(Perf::BlendChanges);
    }

    void ClipDistances(u32 count) {
        for (u32 i = count; i < clip_distances; ++i) {
            gl::Disable(GL_CLIP_DISTANCE0 + i);
        }
        for (u32 i = clip_distances; i < count; ++i) {
            gl::Enable(GL_CLIP_DISTANCE0 + i);
        }
        clip_distances = count;
    }

    /// For the one vertex array draws use; its contents outlive Invalidate.
    void VertexAttrib(GLuint location, bool integer, GLint size, GLenum type, bool normalized,
                      GLsizei stride, u64 offset) {
        Attribute value = attributes[location];
        value.integer = integer;
        value.size = size;
        value.type = type;
        value.normalized = normalized;
        value.stride = stride;
        value.offset = offset;
        if (filter && value == attributes[location]) {
            return;
        }
        attributes[location] = value;
        const auto* pointer = reinterpret_cast<const void*>(uintptr_t(offset));
        if (integer) {
            gl::VertexAttribIPointer(location, size, type, stride, pointer);
        } else {
            gl::VertexAttribPointer(location, size, type, normalized ? GL_TRUE : GL_FALSE, stride,
                                    pointer);
        }
        Perf::Count(Perf::Attributes);
    }

    void VertexDivisor(GLuint location, GLuint divisor) {
        if (filter && attributes[location].divisor == divisor) {
            return;
        }
        attributes[location].divisor = divisor;
        gl::VertexAttribDivisor(location, divisor);
        Perf::Count(Perf::Attributes);
    }

private:
    struct Attribute {
        u64 offset;
        GLint size;
        GLenum type;
        GLsizei stride;
        GLuint divisor;
        u8 integer;
        u8 normalized;
        bool operator==(const Attribute&) const = default;
    };

    static constexpr u32 TextureTargetSlots = 6;

    struct Shadow {
        GLuint program;
        GLuint read_framebuffer;
        GLuint draw_framebuffer;
        GLuint vertex_array;
        GLuint array_buffer;
        GLuint element_buffer;
        GLuint active_unit;
        std::array<std::array<GLuint, TextureTargetSlots>, MaxUnits> textures;
        std::array<GLuint, MaxUnits> samplers;
        std::array<GLint, 4> viewport;
        std::array<GLint, 4> scissor;
        std::array<GLdouble, 2> depth_range;
        GLenum depth_func;
        u8 depth_mask;
        std::array<std::array<u32, 3>, 2> stencil_func;
        std::array<GLuint, 2> stencil_mask;
        std::array<std::array<GLenum, 3>, 2> stencil_op;
        GLenum cull_face;
        GLenum front_face;
        GLenum polygon_mode;
        std::array<GLfloat, 2> polygon_offset;
        GLfloat line_width;
        GLenum provoking_vertex;
        GLuint restart_index;
        std::array<GLfloat, 4> blend_color;
        GLenum logic_op;
        std::array<u8, MaxColorTargets> blend_enable;
        std::array<u8, MaxColorTargets> color_mask;
        std::array<std::array<GLenum, 4>, MaxColorTargets> blend_func;
        std::array<std::array<GLenum, 2>, MaxColorTargets> blend_equation;
    };

    static constexpr u32 TargetSlot(GLenum target) {
        switch (target) {
        case GL_TEXTURE_1D:
            return 0;
        case GL_TEXTURE_1D_ARRAY:
            return 1;
        case GL_TEXTURE_2D_ARRAY:
            return 3;
        case GL_TEXTURE_3D:
            return 4;
        case GL_TEXTURE_BUFFER:
            return 5;
        default:
            return 2;
        }
    }

    static constexpr u32 Face(GLenum face) {
        return face == GL_FRONT ? 0 : 1;
    }

    template <typename T>
    bool Changed(T& cached, const T& value) {
        if (filter && cached == value) {
            return false;
        }
        cached = value;
        return true;
    }

    Shadow shadow;
    std::array<Attribute, MaxAttributes> attributes;
    u32 known_caps{};
    u32 enabled_caps{};
    u32 clip_distances{};
    bool filter{true};
};

} // namespace OpenGL
