// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <SDL3/SDL.h>

#include "core/libraries/videoout/buffer.h"
#include "dcp_graphics.h"
#include "dcp_overlay.h"
#include "dcp_settings.h"
#include "dcp_ultrawide_sides.h"
#include "sdl_window.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_opengl/gl_capabilities.h"
#include "video_core/renderer_opengl/gl_presenter.h"
#include "video_core/renderer_opengl/gl_rasterizer.h"

extern "C" void runtime_log_flush(void);

namespace OpenGL {
namespace {

using PixelFormat = Libraries::VideoOut::PixelFormat;

constexpr u32 MaxPendingShows = 2;

constexpr const char* FullscreenVertex = R"(#version 330 core
out vec2 uv;
uniform float flip_v;
void main() {
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    uv = vec2(corner.x, flip_v != 0.0 ? 1.0 - corner.y : corner.y);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

// post_process.frag without HDR, which has no OpenGL path here.
constexpr const char* PrepareFragment = R"(#version 330 core
in vec2 uv;
out vec4 color;
uniform sampler2D source;
uniform float gamma;
uniform bool swap_red_blue;
uniform bool decode_input;
const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 encode(vec3 rgb) {
    return mix(a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - gamma))) - b, d * rgb / gamma,
               lessThan(rgb, vec3(cutoff)));
}
vec3 decode(vec3 rgb) {
    return mix(pow(max(rgb + b, 0.0) / a, vec3(2.4)), rgb / d, lessThan(rgb, vec3(d * cutoff)));
}
void main() {
    vec4 texel = texture(source, uv);
    if (swap_red_blue) texel = texel.bgra;
    if (decode_input) texel.rgb = decode(texel.rgb);
    color = vec4(encode(texel.rgb), 1.0);
}
)";

constexpr const char* ShowFragment = R"(#version 330 core
in vec2 uv;
out vec4 color;
uniform sampler2D source;
void main() {
    color = vec4(texture(source, uv).rgb, 1.0);
}
)";

[[noreturn]] void Fatal(const std::string& message) {
    std::printf("OpenGL: %s\n", message.c_str());
    std::fflush(stdout);
    runtime_log_flush();
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Dragon's Crown Pro",
                             ("The OpenGL renderer could not start.\n\n" + message).c_str(),
                             nullptr);
    std::_Exit(1);
}

GLuint BuildProgram(const char* vertex_source, const char* fragment_source) {
    const auto compile = [](GLenum type, const char* source) {
        const GLuint shader = gl::CreateShader(type);
        gl::ShaderSource(shader, 1, &source, nullptr);
        gl::CompileShader(shader);
        GLint compiled = GL_FALSE;
        gl::GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            std::array<GLchar, 1024> log{};
            gl::GetShaderInfoLog(shader, GLsizei(log.size()), nullptr, log.data());
            Fatal(std::string{"a presentation shader did not compile: "} + log.data());
        }
        return shader;
    };
    const GLuint vertex = compile(GL_VERTEX_SHADER, vertex_source);
    const GLuint fragment = compile(GL_FRAGMENT_SHADER, fragment_source);
    const GLuint program = gl::CreateProgram();
    gl::AttachShader(program, vertex);
    gl::AttachShader(program, fragment);
    gl::LinkProgram(program);
    gl::DeleteShader(vertex);
    gl::DeleteShader(fragment);
    GLint linked = GL_FALSE;
    gl::GetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        std::array<GLchar, 1024> log{};
        gl::GetProgramInfoLog(program, GLsizei(log.size()), nullptr, log.data());
        Fatal(std::string{"a presentation shader did not link: "} + log.data());
    }
    return program;
}

bool HasExtension(std::string_view name) {
    GLint count = 0;
    gl::GetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; ++i) {
        const auto* extension = reinterpret_cast<const char*>(gl::GetStringi(GL_EXTENSIONS, GLuint(i)));
        if (extension && name == extension) {
            return true;
        }
    }
    return false;
}

const char* Text(GLenum name) {
    const auto* text = reinterpret_cast<const char*>(gl::GetString(name));
    return text ? text : "unknown";
}

bool DisplayStoresBlueFirst(PixelFormat format) {
    return format != PixelFormat::A8B8G8R8Srgb;
}

bool DisplayIsSrgb(PixelFormat format) {
    return format == PixelFormat::A8B8G8R8Srgb || format == PixelFormat::A8R8G8B8Srgb ||
           format == PixelFormat::A2R10G10B10Srgb;
}

SDL_Rect FitInside(u32 picture_width, u32 picture_height, int window_width, int window_height) {
    const float picture_aspect = float(picture_width) / float(picture_height);
    const float window_aspect = float(window_width) / float(window_height);
    int width = window_width, height = window_height;
    if (picture_aspect > window_aspect) {
        height = int(float(window_width) / picture_aspect);
    } else {
        width = int(float(window_height) * picture_aspect);
    }
    return {(window_width - width) / 2, (window_height - height) / 2, width, height};
}

} // namespace

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_} {
    blank_frame.blank = true;
    SDL_Window* sdl_window = window.GetSDLWindow();
    context = SDL_GL_CreateContext(sdl_window);
    if (!context) {
        Fatal(std::string{"no OpenGL 3.3 core context: "} + SDL_GetError());
    }
    SDL_GL_MakeCurrent(sdl_window, nullptr);
    liverpool->SendCommand<true>([this] { InitializeOnGpuThread(); });
}

Presenter::~Presenter() = default;

void Presenter::InitializeOnGpuThread() {
    SDL_Window* sdl_window = window.GetSDLWindow();
    if (!SDL_GL_MakeCurrent(sdl_window, context)) {
        Fatal(std::string{"the context could not be made current: "} + SDL_GetError());
    }
    std::string missing_function;
    if (!gl::Load(missing_function)) {
        Fatal("the driver does not export " + missing_function);
    }
    Capabilities capabilities;
    gl::GetIntegerv(GL_MAJOR_VERSION, &capabilities.major);
    gl::GetIntegerv(GL_MINOR_VERSION, &capabilities.minor);
    capabilities.s3tc = HasExtension("GL_EXT_texture_compression_s3tc");
    capabilities.bptc = capabilities.major > 4 || (capabilities.major == 4 && capabilities.minor >= 2) ||
                        HasExtension("GL_ARB_texture_compression_bptc");
    capabilities.anisotropy = HasExtension("GL_EXT_texture_filter_anisotropic") ||
                              HasExtension("GL_ARB_texture_filter_anisotropic");
    capabilities.draw_buffers_blend =
        capabilities.major >= 4 && gl::BlendFuncSeparatei && gl::BlendEquationSeparatei;
    gl::GetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &capabilities.max_texture_buffer_texels);
    std::printf("OpenGL: vendor %s, renderer %s, version %s, GLSL %s\n", Text(GL_VENDOR),
                Text(GL_RENDERER), Text(GL_VERSION), Text(GL_SHADING_LANGUAGE_VERSION));
    std::printf("OpenGL: BC6/BC7 %s, anisotropy %s, per-target blending %s, texture buffer %d "
                "texels\n",
                capabilities.bptc ? "yes" : "no", capabilities.anisotropy ? "yes" : "no",
                capabilities.draw_buffers_blend ? "yes" : "no",
                capabilities.max_texture_buffer_texels);
    if (const auto missing = MissingRequirement(capabilities); !missing.empty()) {
        Fatal(missing);
    }
    CreatePasses();
    rasterizer = std::make_unique<Rasterizer>(liverpool, capabilities);
    DcpOverlay::InitOpenGL();
    ApplySwapInterval();
}

void Presenter::CreatePasses() {
    gl::GenVertexArrays(1, &vertex_array);
    prepare_program = BuildProgram(FullscreenVertex, PrepareFragment);
    show_program = BuildProgram(FullscreenVertex, ShowFragment);
    prepare_gamma = gl::GetUniformLocation(prepare_program, "gamma");
    prepare_swap = gl::GetUniformLocation(prepare_program, "swap_red_blue");
    prepare_decode = gl::GetUniformLocation(prepare_program, "decode_input");
    for (const GLuint program : {prepare_program, show_program}) {
        gl::UseProgram(program);
        gl::Uniform1i(gl::GetUniformLocation(program, "source"), 0);
        gl::Uniform1f(gl::GetUniformLocation(program, "flip_v"), program == show_program ? 1.0f : 0.0f);
    }
    gl::GenSamplers(1, &linear_sampler);
    gl::SamplerParameteri(linear_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl::SamplerParameteri(linear_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl::SamplerParameteri(linear_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl::SamplerParameteri(linear_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    for (Frame& frame : frames) {
        gl::GenFramebuffers(1, &frame.framebuffer);
        gl::GenTextures(1, &frame.texture);
    }
}

void Presenter::ApplySwapInterval() {
    const int wanted = DcpSettings::Get().vsync ? 1 : 0;
    if (wanted != swap_interval) {
        swap_interval = wanted;
        SDL_GL_SetSwapInterval(wanted);
    }
}

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) {
    rasterizer->OnFlip();
    Image* image = rasterizer->GetTextureCache().FindDisplayBuffer(attribute, cpu_address);
    if (!image) {
        return PrepareBlankFrame(false);
    }
    Frame& frame = frames[next_frame];
    next_frame = (next_frame + 1) % u32(frames.size());
    const u32 width = image->info.size.width, height = image->info.size.height;
    rasterizer->ResetState();
    gl::BindFramebuffer(GL_FRAMEBUFFER, frame.framebuffer);
    if (frame.width != width || frame.height != height) {
        frame.width = width;
        frame.height = height;
        gl::BindTexture(GL_TEXTURE_2D, frame.texture);
        gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(width), GLsizei(height), 0, GL_RGBA,
                       GL_UNSIGNED_BYTE, nullptr);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, frame.texture,
                                 0);
        const GLenum attachment = GL_COLOR_ATTACHMENT0;
        gl::DrawBuffers(1, &attachment);
    }
    gl::Viewport(0, 0, GLsizei(width), GLsizei(height));
    gl::BindVertexArray(vertex_array);
    gl::UseProgram(prepare_program);
    gl::ActiveTexture(GL_TEXTURE0);
    gl::BindTexture(GL_TEXTURE_2D, image->handle);
    gl::BindSampler(0, linear_sampler);
    constexpr std::array<GLint, 4> identity{GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
    gl::TexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, identity.data());
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    const auto pixel_format = attribute.attrib.pixel_format;
    const bool image_blue_first = image->format->format == GL_BGRA;
    gl::Uniform1f(prepare_gamma, gamma.load(std::memory_order_relaxed));
    gl::Uniform1i(prepare_swap, DisplayStoresBlueFirst(pixel_format) != image_blue_first);
    gl::Uniform1i(prepare_decode, DisplayIsSrgb(pixel_format) && !image->is_srgb);
    gl::DrawArrays(GL_TRIANGLES, 0, 3);
    gl::Enable(GL_FRAMEBUFFER_SRGB);

    auto& settings = DcpSettings::Get();
    settings.image_width = int(width);
    settings.image_height = int(height);
    settings.window_width = window.GetWidth();
    settings.window_height = window.GetHeight();
    settings.rcas_applied = false;
    if (!first_flip_logged) {
        first_flip_logged = true;
        std::printf("OpenGL: first guest frame presented (%ux%u, display buffer %#llx)\n", width,
                    height, static_cast<unsigned long long>(cpu_address));
    }
    last_frame.store(&frame, std::memory_order_release);
    return &frame;
}

Frame* Presenter::PrepareBlankFrame(bool) {
    return &blank_frame;
}

Frame* Presenter::PrepareLastFrame() {
    return last_frame.load(std::memory_order_acquire);
}

void Presenter::Present(VideoCore::PresentFrame* present_frame, bool, bool is_game_frame) {
    if (pending_shows.load(std::memory_order_relaxed) >= MaxPendingShows) {
        return;
    }
    pending_shows.fetch_add(1, std::memory_order_relaxed);
    auto* frame = static_cast<Frame*>(present_frame);
    liverpool->SendCommand([this, frame, is_game_frame] {
        Show(*frame, is_game_frame);
        pending_shows.fetch_sub(1, std::memory_order_relaxed);
    });
}

void Presenter::Show(const Frame& frame, bool is_game_frame) {
    const int window_width = std::max(window.GetWidth(), 1);
    const int window_height = std::max(window.GetHeight(), 1);
    ApplySwapInterval();
    rasterizer->ResetState();
    gl::Viewport(0, 0, window_width, window_height);
    gl::ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl::Clear(GL_COLOR_BUFFER_BIT);
    if (!frame.blank && frame.width != 0) {
        const bool stretch = DcpSettings::Get().presentation == DcpGraphics::Stretch;
        const SDL_Rect area = stretch ? SDL_Rect{0, 0, window_width, window_height}
                                      : FitInside(frame.width, frame.height, window_width,
                                                  window_height);
        gl::Viewport(area.x, area.y, area.w, area.h);
        gl::BindVertexArray(vertex_array);
        gl::UseProgram(show_program);
        gl::ActiveTexture(GL_TEXTURE0);
        gl::BindTexture(GL_TEXTURE_2D, frame.texture);
        gl::BindSampler(0, linear_sampler);
        gl::DrawArrays(GL_TRIANGLES, 0, 3);
        if (DcpUltrawide::SideMaskWanted()) {
            const auto sides = DcpUltrawide::SidesOf({u32(area.w), u32(area.h)});
            gl::Enable(GL_SCISSOR_TEST);
            for (const auto& side : {sides.left, sides.right}) {
                if (!side.Empty()) {
                    gl::Scissor(area.x + GLint(side.left), area.y, GLsizei(side.right - side.left),
                                area.h);
                    gl::Clear(GL_COLOR_BUFFER_BIT);
                }
            }
            gl::Disable(GL_SCISSOR_TEST);
        }
    }
    gl::Viewport(0, 0, window_width, window_height);
    DcpOverlay::RenderOpenGL(u32(window_width), u32(window_height));
    if (is_game_frame) {
        DcpOverlay::LogPresent();
    }
    SDL_GL_SwapWindow(window.GetSDLWindow());
    gl::Enable(GL_FRAMEBUFFER_SRGB);
}

} // namespace OpenGL
