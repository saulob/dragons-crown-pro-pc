// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// The OpenGL context and the window's swap path. One context lives on the GPU command thread:
// EGL allows a surface to be current on only one thread, so the present thread hands its work over.
#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include "video_core/renderer_backend.h"
#include "video_core/renderer_opengl/gl_api.h"

typedef struct SDL_GLContextState* SDL_GLContext;

namespace OpenGL {

class Rasterizer;
struct Capabilities;

struct Frame : VideoCore::PresentFrame {
    GLuint framebuffer{};
    GLuint texture{};
    u32 width{};
    u32 height{};
    bool blank{};
};

class Presenter final : public VideoCore::PresenterBackend {
public:
    Presenter(Frontend::WindowSDL& window, AmdGpu::Liverpool* liverpool);
    ~Presenter() override;

    [[nodiscard]] bool IsHDRSupported() const override {
        return false;
    }
    void SetHDR(bool) override {}
    void SetGamma(float value) override {
        gamma.store(value, std::memory_order_relaxed);
    }

    void RegisterDisplayBuffer(const Libraries::VideoOut::BufferAttributeGroup&, VAddr) override {}
    Frame* PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                        VAddr cpu_address) override;
    Frame* PrepareBlankFrame(bool present_thread) override;
    Frame* PrepareLastFrame() override;
    void Present(VideoCore::PresentFrame* frame, bool is_reusing_frame,
                 bool is_game_frame) override;
    void RetireFrame(VideoCore::PresentFrame*) override {}
    void ReportCompilerStats() override {}

private:
    void InitializeOnGpuThread();
    void CheckSwap(bool swapped);
    void CreatePasses();
    void Show(const Frame& frame, bool is_game_frame);
    void ApplySwapInterval();

    Frontend::WindowSDL& window;
    AmdGpu::Liverpool* liverpool;
    SDL_GLContext context{};
    std::unique_ptr<Rasterizer> rasterizer;
    std::array<Frame, 3> frames{};
    Frame blank_frame{};
    u32 next_frame{};
    std::atomic<Frame*> last_frame{nullptr};
    std::atomic<u32> pending_shows{0};
    std::atomic<float> gamma{1.0f};
    GLuint vertex_array{};
    GLuint prepare_program{};
    GLuint show_program{};
    GLuint linear_sampler{};
    GLint prepare_gamma = -1;
    GLint prepare_swap = -1;
    GLint prepare_decode = -1;
    int swap_interval = -1;
    bool first_flip_logged = false;
    bool gles = false;
    bool swap_failure_logged = false;
};

} // namespace OpenGL
