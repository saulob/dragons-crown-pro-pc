#include "host.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <vector>

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "dcp_displays.h"

namespace dcp {
namespace {

Rect ToRect(const SDL_Rect& rect) {
    return {rect.x, rect.y, rect.x + rect.w, rect.y + rect.h};
}

} // namespace

bool LauncherHost::Open(const HostOptions& options) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        std::printf("Launcher: SDL video failed: %s\n", SDL_GetError());
        return false;
    }
    video_ = true;
    gamepad_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);

    const std::vector<Display> displays = ListDisplays();
    if (displays.empty()) {
        std::printf("Launcher: no display found\n");
        Close();
        return false;
    }
    std::vector<Rect> usable;
    for (const Display& display : displays) {
        usable.push_back(ToRect(display.usable));
    }

    const bool restore = options.restore && PlacementIsVisible(*options.restore, usable);
    restored_ = restore ? options.restore : std::nullopt;
    const Display* target = &displays.front();
    int x = SDL_WINDOWPOS_CENTERED, y = SDL_WINDOWPOS_CENTERED;
    int width = 0, height = 0;
    if (restore) {
        const Rect& normal = options.restore->normal;
        const SDL_Point center{(normal.left + normal.right) / 2, (normal.top + normal.bottom) / 2};
        const SDL_DisplayID id = SDL_GetDisplayForPoint(&center);
        for (const Display& display : displays) {
            if (display.id == id) {
                target = &display;
            }
        }
        x = normal.left;
        y = normal.top;
        width = normal.width();
        height = normal.height();
    } else {
        x = SDL_WINDOWPOS_CENTERED_DISPLAY(target->id);
        y = SDL_WINDOWPOS_CENTERED_DISPLAY(target->id);
        width = static_cast<int>(options.width * target->scale);
        height = static_cast<int>(options.height * target->scale);
        FitWindowSize(width, height, *target);
    }

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, options.title.c_str());
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, x);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, y);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
    window_ = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!window_) {
        std::printf("Launcher: cannot create the window: %s\n", SDL_GetError());
        Close();
        return false;
    }
    SDL_SetWindowMinimumSize(window_, std::min(static_cast<int>(options.min_width * target->scale), width),
                             std::min(static_cast<int>(options.min_height * target->scale), height));

    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (!renderer_) {
        std::printf("Launcher: no accelerated renderer (%s), using the software one\n", SDL_GetError());
        renderer_ = SDL_CreateRenderer(window_, SDL_SOFTWARE_RENDERER);
    }
    if (!renderer_) {
        std::printf("Launcher: cannot create a renderer: %s\n", SDL_GetError());
        Close();
        return false;
    }
    SDL_SetRenderVSync(renderer_, 1);

    IMGUI_CHECKVERSION();
    imgui_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(imgui_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
    ImGui_ImplSDLRenderer3_Init(renderer_);

    if (!options.hidden) {
        SDL_ShowWindow(window_);
        SDL_RaiseWindow(window_);
    }
    if (restore && options.restore->maximized) {
        SDL_MaximizeWindow(window_);
    }
    if (restore && options.restore->fullscreen) {
        SDL_SetWindowFullscreen(window_, true);
    }
    std::printf("Launcher: window %dx%d at %.2fx display scale, %s renderer\n", width, height, target->scale,
                SDL_GetRendererName(renderer_));
    return true;
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

void LauncherHost::Close() {
    for (SDL_Texture* texture : textures_) {
        SDL_DestroyTexture(texture);
    }
    textures_.clear();
    if (imgui_) {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext(imgui_);
        imgui_ = nullptr;
    }
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    if (video_ || gamepad_) {
        // Nothing else uses SDL yet: the game window thread starts it again from scratch.
        SDL_Quit();
        video_ = gamepad_ = false;
    }
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

void LauncherHost::BeginFrame() {
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    const float scale = SDL_GetWindowDisplayScale(window_);
    ImGui::GetStyle().FontScaleDpi = scale > 0.0f ? scale : 1.0f;
    ImGui::NewFrame();
}

void LauncherHost::EndFrame(const ImVec4& clear, const char* capture_path) {
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
    SDL_RenderPresent(renderer_);
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
