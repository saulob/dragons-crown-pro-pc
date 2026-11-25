#include "host.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "dcp_displays.h"
#include "launcher_renderer.h"
#include "startup_phase.h"

namespace dcp {
namespace {

Rect ToRect(const SDL_Rect& rect) {
    return {rect.x, rect.y, rect.x + rect.w, rect.y + rect.h};
}

#ifdef _WIN32
bool PinModule(std::string_view name) {
    const std::wstring wide(name.begin(), name.end());
    // Resolved like SDL_LoadObject resolves it, so the pinned module is the one the renderer uses.
    if (!LoadLibraryW(wide.c_str())) {
        return false;
    }
    HMODULE pinned = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, wide.c_str(), &pinned) != 0;
}

int64_t ThreadCpuNs() {
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        return -1;
    }
    const auto ticks = [](const FILETIME& time) {
        return (static_cast<int64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    return (ticks(kernel) + ticks(user)) * 100;
}
#else
bool PinModule(std::string_view) {
    return false;
}

int64_t ThreadCpuNs() {
    timespec time{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0) {
        return -1;
    }
    return static_cast<int64_t>(time.tv_sec) * kNsPerSecond + time.tv_nsec;
}
#endif

int64_t NowNs() {
    return static_cast<int64_t>(SDL_GetTicksNS());
}

// The first module that could not be kept loaded; empty when all are.
std::string PinRendererModules(std::string_view renderer, int& pinned) {
    for (const std::string_view module : RendererModulesToPin(renderer)) {
        if (!PinModule(module)) {
            return std::string(module);
        }
        ++pinned;
    }
    return {};
}

#ifdef _WIN32
constexpr bool kWindows = true;
#else
constexpr bool kWindows = false;
#endif

std::string SdlError(const char* what) {
    const char* error = SDL_GetError();
    return std::string(what) + ": " + (error && *error ? error : "unknown error");
}

// Flushed at once: the next step may be a driver call that ends the process.
void LogLine(const std::string& line) {
    std::printf("%s\n", line.c_str());
    std::fflush(nullptr);
}

} // namespace

#ifdef _WIN32
void ShowNativeError(const std::string& title, const std::string& text) {
    const auto wide = [](const std::string& utf8) {
        const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
        std::wstring result(static_cast<size_t>(length > 0 ? length : 1), L'\0');
        if (length > 0) {
            MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, result.data(), length);
        }
        return result;
    };
    MessageBoxW(nullptr, wide(text).c_str(), wide(title).c_str(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}
#else
void ShowNativeError(const std::string& title, const std::string& text) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), text.c_str(), nullptr);
}
#endif

struct LauncherHost::Geometry {
    const Display* target = nullptr;
    int x = SDL_WINDOWPOS_CENTERED, y = SDL_WINDOWPOS_CENTERED;
    int width = 0, height = 0;
};

bool LauncherHost::Fail(StartupFailure failure, const std::string& detail) {
    failure_text_ = detail;
    LogLine(StartupFailureLine(failure, detail) + " phase=" + dcp_startup_phase_current());
    Close();
    return false;
}

SDL_Window* LauncherHost::OpenWindow(const HostOptions& options, const Geometry& geometry) {
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, options.title.c_str());
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, geometry.x);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, geometry.y);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, geometry.width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, geometry.height);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
    SDL_Window* window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (window) {
        const float scale = geometry.target->scale;
        SDL_SetWindowMinimumSize(window, std::min(static_cast<int>(options.min_width * scale), geometry.width),
                                 std::min(static_cast<int>(options.min_height * scale), geometry.height));
    }
    return window;
}

// One texture, one clear, one draw and one present: what the first launcher frame needs from the renderer. The font
// atlas is a texture of the same kind.
std::optional<RendererFailure> LauncherHost::ProbeFirstFrame() {
    constexpr int kSize = 512;
    const std::vector<uint8_t> pixels(static_cast<size_t>(kSize) * kSize * 4, 255);
    SDL_Texture* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, kSize, kSize);
    if (!texture) {
        return RendererFailure{RendererStage::kProbe, SdlError("texture")};
    }
    std::string reason;
    if (!SDL_UpdateTexture(texture, nullptr, pixels.data(), kSize * 4)) {
        reason = SdlError("texture upload");
    } else if (!SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND) || !SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255) ||
               !SDL_RenderClear(renderer_) || !SDL_RenderTexture(renderer_, texture, nullptr, nullptr)) {
        reason = SdlError("draw");
    } else if (!SDL_RenderPresent(renderer_)) {
        reason = SdlError("present");
    }
    SDL_DestroyTexture(texture);
    if (!reason.empty()) {
        return RendererFailure{RendererStage::kProbe, reason};
    }
    return std::nullopt;
}

std::optional<RendererFailure> LauncherHost::OpenRenderer(const HostOptions& options, const Geometry& geometry,
                                                          std::string_view name) {
    const std::string unpinned = PinRendererModules(name, pinned_modules_);
    if (!unpinned.empty()) {
        return RendererFailure{RendererStage::kPin, "cannot keep " + unpinned + " loaded"};
    }
    window_ = OpenWindow(options, geometry);
    if (!window_) {
        return RendererFailure{RendererStage::kCreate, SdlError("window")};
    }
    const std::string sdl_name(name);
    renderer_ = SDL_CreateRenderer(window_, sdl_name.empty() ? nullptr : sdl_name.c_str());
    if (!renderer_) {
        return RendererFailure{RendererStage::kCreate, SdlError("renderer")};
    }
    const char* created = SDL_GetRendererName(renderer_);
    renderer_name_ = created ? created : "";
    if (std::optional<RendererFailure> failure = ProbeFirstFrame()) {
        return failure;
    }
    ConfigureVsync();

    IMGUI_CHECKVERSION();
    imgui_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(imgui_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    sdl_backend_ = ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
    if (!sdl_backend_) {
        return RendererFailure{RendererStage::kBackend, "ImGui SDL backend"};
    }
    renderer_backend_ = ImGui_ImplSDLRenderer3_Init(renderer_);
    if (!renderer_backend_) {
        return RendererFailure{RendererStage::kBackend, "ImGui renderer backend"};
    }
    return std::nullopt;
}

bool LauncherHost::Open(const HostOptions& options) {
    failure_text_.clear();
    dcp_startup_phase("launcher video init");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        return Fail(StartupFailure::kSdlVideo, SdlError("SDL video"));
    }
    video_ = true;
    gamepad_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);

    const std::vector<Display> displays = ListDisplays();
    if (displays.empty()) {
        return Fail(StartupFailure::kNoDisplay, "no display found");
    }
    std::vector<Rect> usable;
    for (const Display& display : displays) {
        usable.push_back(ToRect(display.usable));
    }

    const bool restore = options.restore && PlacementIsVisible(*options.restore, usable);
    restored_ = restore ? options.restore : std::nullopt;
    Geometry geometry;
    geometry.target = &displays.front();
    if (restore) {
        const Rect& normal = options.restore->normal;
        const SDL_Point center{(normal.left + normal.right) / 2, (normal.top + normal.bottom) / 2};
        const SDL_DisplayID id = SDL_GetDisplayForPoint(&center);
        for (const Display& display : displays) {
            if (display.id == id) {
                geometry.target = &display;
            }
        }
        geometry.x = normal.left;
        geometry.y = normal.top;
        geometry.width = normal.width();
        geometry.height = normal.height();
    } else {
        geometry.x = SDL_WINDOWPOS_CENTERED_DISPLAY(geometry.target->id);
        geometry.y = SDL_WINDOWPOS_CENTERED_DISPLAY(geometry.target->id);
        geometry.width = static_cast<int>(options.width * geometry.target->scale);
        geometry.height = static_cast<int>(options.height * geometry.target->scale);
        FitWindowSize(geometry.width, geometry.height, *geometry.target);
    }

    dcp_startup_phase("launcher renderer selection");
    LogLine(RendererRequestLine(options.renderer, kWindows));
    const RendererSelection selection = SelectLauncherRenderer(
        options.renderer, kWindows,
        [&](std::string_view name) {
            LogLine("Launcher: startup: renderer trying=" + AttemptLabel(name));
            std::optional<RendererFailure> failure = OpenRenderer(options, geometry, name);
            LogLine(RendererAttemptLine({std::string(name), failure}));
            return failure;
        },
        [this] { ReleaseWindow(); });
    LogLine(RendererResultLine(selection));
    if (!selection.opened) {
        return Fail(StartupFailure::kRenderer, RendererFailureText(selection));
    }
    std::printf("%s\n", RendererSelectedLine(renderer_name_, selection.fallback, pinned_modules_).c_str());

    hidden_ = options.hidden;
    pacer_ = FramePacer(hidden_ ? 0 : kLauncherFrameNs);
    if (restore && options.restore->maximized) {
        SDL_MaximizeWindow(window_);
    }
    if (restore && options.restore->fullscreen) {
        SDL_SetWindowFullscreen(window_, true);
    }
    std::printf("Launcher: window %dx%d at %.2fx display scale, %s renderer\n", geometry.width, geometry.height,
                geometry.target->scale, SDL_GetRendererName(renderer_));
    return true;
}

// On a Windows GDI window the software renderer's VSync is only simulated by SDL, so it is turned off there and the
// frame cap alone paces the loop.
void LauncherHost::ConfigureVsync() {
    VsyncState state;
    state.requested = true;
    const bool set = SDL_SetRenderVSync(renderer_, 1);
    int value = 0;
    SDL_GetRenderVSync(renderer_, &value);
    const bool software = renderer_name_ == SDL_SOFTWARE_RENDERER;
    int surface_value = 0;
    const bool real = !software || (SDL_GetWindowSurfaceVSync(window_, &surface_value) && surface_value != 0);
    state.effective = set && value == 1 && real;
    if (set && !state.effective) {
        SDL_SetRenderVSync(renderer_, 0);
    }
    std::printf("%s\n", PacingModeLine(renderer_name_, state).c_str());
}

void LauncherHost::Show() {
    if (!window_ || hidden_) {
        return;
    }
    SDL_ShowWindow(window_);
    SDL_RaiseWindow(window_);
}

ImTextureID LauncherHost::CreateTexture(const uint8_t* rgba, int width, int height) {
    if (!renderer_ || !rgba || width <= 0 || height <= 0) {
        return ImTextureID{};
    }
    SDL_Texture* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
    if (!texture) {
        return ImTextureID{};
    }
    if (!SDL_UpdateTexture(texture, nullptr, rgba, width * 4)) {
        SDL_DestroyTexture(texture);
        return ImTextureID{};
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    textures_.push_back(texture);
    return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture));
}

void LauncherHost::DestroyTexture(ImTextureID texture) {
    SDL_Texture* sdl_texture = reinterpret_cast<SDL_Texture*>(static_cast<uintptr_t>(texture));
    const auto found = std::find(textures_.begin(), textures_.end(), sdl_texture);
    if (found != textures_.end()) {
        SDL_DestroyTexture(sdl_texture);
        textures_.erase(found);
    }
}

// Whatever a renderer attempt opened, in the reverse of the order it was opened. Safe after a partial attempt.
void LauncherHost::ReleaseWindow() {
    for (SDL_Texture* texture : textures_) {
        SDL_DestroyTexture(texture);
    }
    textures_.clear();
    if (renderer_backend_) {
        ImGui_ImplSDLRenderer3_Shutdown();
        renderer_backend_ = false;
    }
    if (sdl_backend_) {
        ImGui_ImplSDL3_Shutdown();
        sdl_backend_ = false;
    }
    if (imgui_) {
        ImGui::DestroyContext(imgui_);
        imgui_ = nullptr;
    }
    dcp_startup_phase("launcher destroying renderer");
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    dcp_startup_phase("launcher destroying window");
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    renderer_name_.clear();
}

void LauncherHost::Close() {
    dcp_startup_phase("launcher host closing");
    ReleaseWindow();
    dcp_startup_phase("launcher SDL shutdown");
    if (video_ || gamepad_) {
        // Nothing else uses SDL yet: the game window thread starts it again from scratch.
        SDL_Quit();
        video_ = gamepad_ = false;
    }
    dcp_startup_phase("launcher host closed");
}

bool LauncherHost::PumpEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        switch (event.type) {
        case SDL_EVENT_QUIT:
            quit_ = true;
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (event.window.windowID == SDL_GetWindowID(window_)) {
                quit_ = true;
            }
            break;
        case SDL_EVENT_KEY_DOWN:
            if (event.key.key == SDLK_F11 && !event.key.repeat) {
                SetFullscreen(!Fullscreen());
            }
            break;
        default:
            break;
        }
    }
    return !quit_;
}

bool LauncherHost::Minimized() const {
    return (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0;
}

bool LauncherHost::WaitForFrame() {
    for (;;) {
        if (!PumpEvents()) {
            return false;
        }
        const int64_t polled = NowNs();
        if (Minimized()) {
            pacer_.Reset();
            monitor_.Reset();
            SDL_Delay(kMinimizedSleepMs);
            continue;
        }
        const FrameWait next = pacer_.Next(polled);
        if (next.draw) {
            return true;
        }
        SDL_DelayNS(static_cast<Uint64>(next.wait_ns));
        const int64_t waited = NowNs() - polled;
        monitor_.Waited(waited);
    }
}

void LauncherHost::NoteFrameDrawn(int64_t now_ns, const FrameTimes& times) {
    if (pacer_.interval_ns() > 0) {
        if (const std::optional<LoopSample> sample = monitor_.FrameDrawn(now_ns, times, ThreadCpuNs())) {
            if (sample->first) {
                std::printf("%s\n", LoopSampleLine(renderer_name_, *sample).c_str());
            }
            if (sample->excessive_rate) {
                std::printf("%s\n", ExcessiveRateLine(renderer_name_, *sample).c_str());
            }
            if (sample->present_stall) {
                std::printf("%s\n", PresentStallLine(renderer_name_, *sample).c_str());
            }
        }
    }
}

void LauncherHost::BeginFrame() {
    frame_begin_ns_ = NowNs();
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    const float scale = SDL_GetWindowDisplayScale(window_);
    ImGui::GetStyle().FontScaleDpi = scale > 0.0f ? scale : 1.0f;
    ImGui::NewFrame();
}

void LauncherHost::EndFrame(const ImVec4& clear, const char* capture_path) {
    const int64_t submitting = NowNs();
    ImGui::Render();
    const ImGuiIO& io = ImGui::GetIO();
    SDL_SetRenderScale(renderer_, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColorFloat(renderer_, clear.x, clear.y, clear.z, 1.0f);
    SDL_RenderClear(renderer_);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
    if (capture_path) {
        if (SDL_Surface* frame = SDL_RenderReadPixels(renderer_, nullptr)) {
            SDL_SavePNG(frame, capture_path);
            SDL_DestroySurface(frame);
        }
    }
    const int64_t presenting = NowNs();
    SDL_RenderPresent(renderer_);
    const int64_t presented = NowNs();
    NoteFrameDrawn(presented, {submitting - frame_begin_ns_, presenting - submitting, presented - presenting});
}

bool LauncherHost::Fullscreen() const {
    return (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
}

void LauncherHost::SetFullscreen(bool fullscreen) {
    SDL_SetWindowFullscreen(window_, fullscreen);
}

std::optional<WindowState> LauncherHost::State() const {
    if (!window_) {
        return std::nullopt;
    }
    int x = 0, y = 0, width = 0, height = 0;
    SDL_GetWindowPosition(window_, &x, &y);
    SDL_GetWindowSize(window_, &width, &height);
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window_);
    return WindowState{{x, y, x + width, y + height}, (flags & SDL_WINDOW_MAXIMIZED) != 0,
                       (flags & SDL_WINDOW_MINIMIZED) != 0};
}

} // namespace dcp
