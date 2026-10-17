#include "dcp_launcher.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cfloat>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <imgui.h>

#include "trg/headers.h"
#include "trg/launcher.h"
#include "trg/platform.h"

#include "controller_prompts.h"
#include "dcp_accent.h"
#include "dcp_cheats.h"
#include "dcp_displays.h"
#include "dcp_resolution.h"
#include "game_files.h"
#include "game_install.h"
#include "host.h"
#include "options.h"
#include "package_browser.h"
#include "placement.h"
#include "play_gate.h"
#include "problem_dialog.h"
#include "prompts_patch.h"
#include "settings.h"
#include "shift_key.h"
#include "trophy_page.h"
#include "utf8.h"

#ifndef DCP_PORT_VERSION
#define DCP_PORT_VERSION "unknown"
#endif
#define DCP_TRG_LAUNCHER_VERSION "1.1.0"

namespace dcp {
namespace {

const ImVec4 kAccent = ImVec4(kAccentRed / 255.0f, kAccentGreen / 255.0f, kAccentBlue / 255.0f, 1.0f);
constexpr double kPlayFeedbackSeconds = 0.4;
constexpr char kPortTitle[] = "Dragon's Crown Pro PC";
std::string GameFilesHelp() {
    return "Select your own legally obtained Dragon's Crown Pro package (.pkg). Supported: " + SupportedBuildsText() +
           ". The Japanese 1.02 and 1.03 updates are installed over the 1.00 base package. The game is unpacked into the "
           "game folder next to the program (about 6 GB). No game files are included. Reinstalling keeps your saves and "
           "settings.";
}
#ifdef _WIN32
constexpr char kShowLauncherHelp[] = "Off starts the game directly. Hold Shift while starting to bring it back.";
#else
constexpr char kShowLauncherHelp[] =
    "Off starts the game directly. Hold Shift while starting, or start the program with --launcher, to bring it back.";
#endif
constexpr char kWindowSizeHelp[] =
    "The game window's size in pixels in Windowed mode. The picture keeps its shape: other window shapes get black bars.";
constexpr char kGraphicsCardHelp[] =
    "The graphics card the game runs on. Automatic prefers a discrete card. A card that is no longer there falls back "
    "to Automatic.";
constexpr char kMonitorHelp[] = "The display the game opens on.";
constexpr char kVSyncHelp[] = "On prevents tearing. Off allows it, for the lowest latency and G-Sync/FreeSync.";
constexpr char kUpscaleHelp[] =
    "Enlarges the game's picture to a bigger window with AMD FidelityFX Super Resolution 1. It has no effect when the "
    "window is the size of the render resolution or smaller.";
constexpr char kSharpenHelp[] =
    "Sharpens the final picture (AMD RCAS). It applies when the window is the size of the render resolution, or "
    "together with FSR 1 in a bigger window.";
constexpr char kAspectRatioHelp[] = "Sets the game's target rendering format and the Window size choices.";
constexpr char kRenderResolutionHelp[] =
    "The resolution the game renders at. It is separate from the window size: the picture is scaled to the window. "
    "1440p and above need a powerful graphics card.";
constexpr char kAntiAliasingHelp[] =
    "SMAA smooths jagged edges on the finished picture, at a small cost in performance.";
constexpr char kAnisotropyHelp[] =
    "Sharper textures on floors and walls seen at an angle. Game uses the game's own filtering.";
constexpr char kTextLanguageHelp[] = "Language used by game menus and subtitles.";
constexpr char kShowFpsTitleHelp[] = "Shows the frame rate in the window title: Dragon's Crown Pro | FPS: 60.00";
constexpr char kShowResolutionTitleHelp[] =
    "Shows the game's render resolution in the window title: Dragon's Crown Pro | 1920x1080";
constexpr char kFrameCounterHelp[] = "Shows the frame rate and frame time in a corner of the game.";
constexpr char kControllerPromptsHelp[] =
    "The buttons the game's prompts show. Xbox rewrites the button pictures in the installed game archives (a few "
    "seconds, reversible); the change is kept across reinstalls.";

std::vector<std::string> ListGpuNames() {
    char names[16][256] = {};
    const int count = dcp_gpu_names(names, 16);
    return {names, names + count};
}

trg::Theme LauncherTheme() {
    trg::Theme theme = trg::Theme::Midnight().WithAccent(kAccent);
    // Warnings stay red-orange so they are never mistaken for the gold accent.
    theme.warn = ImVec4(0.47f, 0.18f, 0.10f, 1.0f);
    theme.warn_dot = ImVec4(1.0f, 0.45f, 0.30f, 1.0f);
    return theme;
}

trg::HeaderPainter LauncherHeader() {
    return trg::headers::Gradient(IM_COL32(12, 18, 26, 255), IM_COL32(62, 42, 12, 255), IM_COL32(8, 34, 44, 255),
                                  IM_COL32(8, 10, 16, 255));
}

__attribute__((format(printf, 1, 2))) void Log(const char* format, ...) {
    std::printf("Launcher: ");
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::printf("\n");
}

const char* StatusTitle(GameFileState state) {
    switch (state) {
    case GameFileState::kValid: return "Ready to play";
    case GameFileState::kMissing: return "Game files needed";
    case GameFileState::kDifferentVersion: return "Different version of Dragon's Crown Pro";
    case GameFileState::kReadError: return "Could not read the game files";
    }
    return "";
}

std::string StatusDetail(GameFileState state) {
    switch (state) {
    case GameFileState::kValid: return "";
    case GameFileState::kMissing: return "Install the game from your own .pkg file.";
    case GameFileState::kDifferentVersion: return "Install a supported build from your own .pkg file.";
    case GameFileState::kReadError: return "Check that the game folder can be read, then install again.";
    }
    return "";
}

GameCheck CheckAndLog(const GamePaths& paths) {
    const GameCheck check = CheckGameFiles(paths);
    Log("game files in %s: %s%s%s%s", Utf8(paths.app0).c_str(), StateName(check.state), check.detail.empty() ? "" : " (",
        check.detail.c_str(), check.detail.empty() ? "" : ")");
    if (check.build) {
        Log("build %s, regional archive %s", check.build->label,
            check.build->regional_archive ? check.build->regional_archive : "none");
    }
    if (!check.eboot_sha256.empty()) {
        Log("eboot.bin SHA-256 %s", check.eboot_sha256.c_str());
    }
    return check;
}

std::filesystem::path SettingsPath(const DcpLauncherConfig& config) {
    if (config.settings_file && *config.settings_file) {
        return PathFromUtf8(config.settings_file);
    }
    return PathFromUtf8(config.user_dir ? config.user_dir : "user") / "dcp.ini";
}

enum class Outcome { kRunning, kPlay, kQuit };

class LauncherApp {
public:
    LauncherApp(const DcpLauncherConfig& config, LauncherSettings& settings, GamePaths paths, GameCheck check)
        : config_(config), settings_(settings), paths_(std::move(paths)), files_(check.state), build_(check.build) {}

    // Runs the launcher window until Play or Quit. False when no window could be opened.
    bool Run(Outcome& outcome) {
        HostOptions options;
        options.title = kGameTitle;
        const WindowPlacement saved = ReadPlacement(settings_);
        if (PlacementIsSet(saved)) {
            options.restore = saved;
        }
        // DCP_LAUNCHER_SHOTS=<folder> (tests/launcher_shots.cpp): every page into the folder, hidden, then quit.
        const char* shots = std::getenv("DCP_LAUNCHER_SHOTS");
        options.hidden = shots != nullptr;
        if (!host_.Open(options)) {
            return false;
        }
        displays_ = ListDisplays();
        gpu_names_ = ListGpuNames();
        prompts_installed_ = InstalledPrompts();
        Log("controller prompts: %s installed", PromptStyleKey(prompts_installed_));
        placement_ = PlacementTracker(host_.Restored());
        fonts_ = trg::LoadSystemFonts(18.0f);
        trophy_page_ = std::make_unique<TrophyPage>(host_, UserDir());
        launcher_ = std::make_unique<trg::Launcher>(MakeConfig());
        Log("shown");

        outcome = Outcome::kRunning;
        while (outcome == Outcome::kRunning) {
            if (!host_.PumpEvents()) {
                outcome = Outcome::kQuit;
                break;
            }
            if (host_.Minimized()) {
                SDL_Delay(30);
                continue;
            }
            host_.BeginFrame();
            outcome = Frame();
            std::string shot;
            if (shots) {
                constexpr int kFramesPerPage = 8;
                const int page = frames_ / kFramesPerPage;
                if (page >= static_cast<int>(std::size(kPageOrder))) {
                    outcome = Outcome::kQuit;
                } else if (frames_ % kFramesPerPage == 0) {
                    launcher_->GoToPage(page);
                } else if (frames_ % kFramesPerPage == kFramesPerPage - 1) {
                    shot = std::string(shots) + "/page_" + std::to_string(page) + "_" + PageName(kPageOrder[page]) + ".png";
                }
                ++frames_;
            }
            host_.EndFrame(LauncherTheme().bg, shot.empty() ? nullptr : shot.c_str());
        }
        SavePlacement(placement_.Flush());
        launcher_.reset();
        trophy_page_.reset();
        host_.Close();
        Log(outcome == Outcome::kPlay ? "closed, starting the game" : "closed");
        return true;
    }

private:
    Outcome Frame() {
        TrackPlacement();
        PollInstall();
        PollPrompts();
        if (play_.starting() && ImGui::GetTime() - play_started_ >= kPlayFeedbackSeconds) {
            return Outcome::kPlay;
        }
        ImGuiIO& io = ImGui::GetIO();
        if (play_.starting()) {
            io.ClearInputKeys();
            io.ClearInputMouse();
        }
        ImGui::BeginDisabled(play_.starting());
        const trg::Result result = launcher_->Frame();
        ImGui::EndDisabled();
        if (const std::optional<std::filesystem::path> picked = browser_.Draw(fonts_.regular, fonts_.size, LauncherTheme())) {
            StartInstall(Utf8(*picked));
        }
        if (play_.starting()) {
            DrawStartingMessage();
        }
        problem_.Draw(fonts_, LauncherTheme());
        switch (result) {
        case trg::Result::kPlay:
            RequestPlay();
            break;
        case trg::Result::kQuit:
            if (!play_.starting()) {
                return Outcome::kQuit;
            }
            break;
        case trg::Result::kNone:
            break;
        }
        return Outcome::kRunning;
    }

    // PLAY saves the settings once here, then starts the transition. A failed save keeps the launcher open.
    void RequestPlay() {
        if (play_.starting()) {
            return;
        }
        Recheck();
        if (files_ == GameFileState::kValid && !GraphicsUsable()) {
            return;
        }
        const bool busy = prompts_.running();
        switch (play_.Request(busy, files_ == GameFileState::kValid, [this] { return launcher_->Save(); })) {
        case PlayRequest::kStarting:
            play_started_ = ImGui::GetTime();
            launcher_->SetStatus("", 0.0);
            Log("settings saved, starting the game");
            break;
        case PlayRequest::kFilesNotReady:
            launcher_->SetStatus(std::string(StatusTitle(files_)) + ". Install the game files first.", 6.0);
            break;
        case PlayRequest::kBusy:
            launcher_->SetStatus("Wait for the controller prompts to finish.", 6.0);
            break;
        case PlayRequest::kSaveFailed:
            Log("could not save the settings");
            break;
        }
    }

    bool GraphicsUsable() {
        if (!config_.check_graphics) {
            return true;
        }
        char cause[320] = "", advice[320] = "";
        if (config_.check_graphics(cause, sizeof(cause), advice, sizeof(advice))) {
            return true;
        }
        problem_.Show(cause, advice);
        Log("graphics unavailable, staying open");
        return false;
    }

    void Recheck() {
        const GameCheck check = CheckAndLog(paths_);
        files_ = check.state;
        build_ = check.build;
    }

    PromptStyle InstalledPrompts() const {
        return build_ ? InstalledPromptStyle(paths_.app0, *build_) : PromptStyle::kPlayStation;
    }

    void TrackPlacement() {
        const auto state = host_.State();
        if (!state) {
            return;
        }
        placement_.Observe(*state, host_.Fullscreen(), ImGui::GetTime());
        SavePlacement(placement_.TakeSettled(ImGui::GetTime()));
    }

    void SavePlacement(const std::optional<WindowPlacement>& placement) {
        if (placement && !settings_.SaveNow(PlacementValues(*placement))) {
            Log("could not save the launcher window placement");
        }
    }

    void DrawStartingMessage() {
        constexpr const char* kText = "Settings saved. Starting game...";
        ImGui::PushFont(fonts_.semibold, fonts_.size * 1.25f);
        const float size = ImGui::GetFontSize();
        const ImVec2 text_size = ImGui::CalcTextSize(kText);
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 pos(viewport->Pos.x + (viewport->Size.x - text_size.x) * 0.5f,
                         viewport->Pos.y + (viewport->Size.y - text_size.y) * 0.5f);
        ImGui::GetForegroundDrawList()->AddText(ImGui::GetFont(), size, pos, ImGui::GetColorU32(kAccent), kText);
        ImGui::PopFont();
    }

    trg::LauncherConfig MakeConfig() {
        trg::LauncherConfig c;
        c.settings = &settings_;
        c.save_on_play = false;
        c.theme = LauncherTheme();
        c.fonts = fonts_;
        c.branding.title = "DRAGON'S CROWN PRO";
        c.branding.subtitle = "PC PORT  \xC2\xB7  LAUNCHER";
        c.branding.subtitle_color = kAccent;
        c.branding.background = LauncherHeader();
        c.note = "Play applies the changes. Save keeps them without starting.";
        for (const PageId page : kPageOrder) {
            c.pages.push_back(MakePage(page));
        }
        c.can_play = [this] {
            if (install_.running()) {
                return trg::PlayCheck{false, "Wait for the installation to finish.", 0};
            }
            if (prompts_.running()) {
                return trg::PlayCheck{false, "Wait for the controller prompts to finish.", 0};
            }
            if (files_ == GameFileState::kValid) {
                return trg::PlayCheck{};
            }
            return trg::PlayCheck{false, "The game files are not ready: see the Play page.", 0};
        };
        return c;
    }

    trg::Page MakePage(PageId page) {
        const std::string name(PageName(page));
        switch (page) {
        case PageId::kPlay:
            return {name, "Check the game files and start playing.", [this](trg::Ui& ui) { PagePlay(ui); },
                    [this] { return files_ != GameFileState::kValid; }};
        case PageId::kDisplay:
            return {name, "Window, monitor, scaling and presentation.", [this](trg::Ui& ui) { PageDisplay(ui); },
                    nullptr};
        case PageId::kGraphics:
            return {name, "Resolution, anti-aliasing and texture filtering.", [this](trg::Ui& ui) { PageGraphics(ui); },
                    nullptr};
        case PageId::kGameplay:
            return {name, "Frame rate readouts.", [this](trg::Ui& ui) { PageGameplay(ui); }, nullptr};
        case PageId::kCheats:
            return {name, cheats::kLauncherPageSubtitle, [this](trg::Ui& ui) { PageCheats(ui); }, nullptr};
        case PageId::kControls:
            return {name, "Controller and keyboard.", [this](trg::Ui& ui) { PageControls(ui); }, nullptr};
        case PageId::kTrophies:
            return {name, "Your trophies on this PC.", [this](trg::Ui& ui) { PageTrophies(ui); }, nullptr};
        case PageId::kAbout:
            return {name, "This port, and where your files and settings live.", [this](trg::Ui& ui) { PageAbout(ui); },
                    nullptr};
        }
        return {};
    }

    void PagePlay(trg::Ui& ui) {
        const bool ready = files_ == GameFileState::kValid;
        const std::string dir = Utf8(paths_.app0);
        const std::string title = ready && build_ ? std::string(StatusTitle(files_)) + " \xC2\xB7 " + build_->label
                                                  : StatusTitle(files_);
        const std::string detail = ready ? dir : StatusDetail(files_);
        ui.StatusCard(ready ? trg::Status::kReady : trg::Status::kAttention, title.c_str(), detail.c_str());
        if (!install_failure_.empty()) {
            ui.Spacer(4);
            ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().warn_dot);
            ImGui::TextUnformatted(install_failure_.c_str());
            ImGui::PopStyleColor();
            ui.Help(install_failure_text_.c_str());
            ui.Spacer(4);
        }
        const std::string files_help = GameFilesHelp();
        ui.Row("Game files", files_help.c_str());
        if (!ui.TaskProgress(install_)) {
            const bool clicked = ready ? ImGui::Button("Reinstall game...", ImVec2(-FLT_MIN, 0))
                                       : ui.AccentButton("Install game...");
            if (clicked) {
                BrowseAndInstall();
            }
        }
        ui.Toggle("Show this launcher", kShowLauncherHelp, kShowLauncherKey, true, "Off", "On");
        ui.EndRows();
    }

    // The desktop's chooser (Windows' own, zenity or kdialog) unless there is none or DCP_FILE_CHOOSER=builtin.
    static bool UseBuiltInChooser() {
#ifdef _WIN32
        return false;
#else
        const std::string_view preference = FileChooserPreference();
        if (preference != "") {
            return preference == "builtin";
        }
        return std::system("command -v zenity >/dev/null 2>&1 || command -v kdialog >/dev/null 2>&1") != 0;
#endif
    }

    void BrowseAndInstall() {
        if (install_.running() || browser_.active() || prompts_.running()) {
            return;
        }
        if (UseBuiltInChooser()) {
            browser_.Open(paths_.app0.parent_path().parent_path().parent_path());
            return;
        }
        StartInstall(trg::BrowseForFile("Select your Dragon's Crown Pro package", {{"PS4 packages (*.pkg)", "*.pkg"}}));
    }

    void StartInstall(const std::string& picked) {
        if (picked.empty() || prompts_.running()) {
            return;
        }
        install_failure_.clear();
        install_failure_text_.clear();
        outcome_ = std::make_shared<InstallResult>();
        const bool started = install_.Start(
            "Checking the package...",
            [outcome = outcome_, source = PathFromUtf8(picked), roots = RootsOf(paths_),
             replace = files_ == GameFileState::kValid](trg::Task& task) {
                InstallHooks hooks;
                hooks.replace_active = replace;
                hooks.phase = [&task](const char* text) {
                    task.SetLabel(text);
                    task.Progress(0, 0);
                };
                hooks.progress = [&task](uint64_t done, uint64_t total) {
                    task.Progress(static_cast<long long>(done), static_cast<long long>(total));
                    return !task.cancelled();
                };
                *outcome = InstallGame(source, roots, hooks);
                return outcome->ok() ? std::string() : std::string(DescribeInstall(outcome->status).title);
            });
        if (started) {
            Log("installing the game from %s", picked.c_str());
        }
    }

    // Runs every frame so a finished install is picked up whichever page is open.
    void PollInstall() {
        if (install_.Finish() == trg::Task::State::kIdle) {
            return;
        }
        const std::shared_ptr<InstallResult> outcome = std::move(outcome_);
        if (!outcome) {
            return;
        }
        const InstallMessage message = DescribeInstall(outcome->status);
        if (outcome->ok()) {
            if (outcome->status == InstallStatus::kUpToDate) {
                Log("game already up to date: %s", outcome->detail.c_str());
            } else if (outcome->status == InstallStatus::kActivated) {
                Log("game activated: %s", outcome->build->label);
            } else {
                Log("game installed: %zu files (%zu linked), %llu bytes", outcome->files, outcome->linked,
                    static_cast<unsigned long long>(outcome->bytes));
            }
            paths_ = ActiveBuildPaths(RootsOf(paths_));
            Recheck();
            if (trophy_page_) {
                trophy_page_->Invalidate();
            }
            launcher_->SetStatus(std::string(message.title) + ". " + message.text, 6.0);
            prompts_installed_ = InstalledPrompts();
            // The fresh archives show the game's own prompts; put the chosen ones back.
            if (SavedPromptStyle() == PromptStyle::kXbox && build_ && build_->xbox_prompts &&
                prompts_installed_ != PromptStyle::kXbox && files_ == GameFileState::kValid) {
                Log("controller prompts: re-applying xbox after the install");
                StartPrompts(PromptStyle::kXbox);
            }
            return;
        }
        if (outcome->status == InstallStatus::kCancelled) {
            Log("game install cancelled");
            launcher_->SetStatus(std::string(message.title) + ". " + message.text, 6.0);
            return;
        }
        Log("game install failed: %s (%s)", message.title.c_str(), outcome->detail.c_str());
        install_failure_ = message.title;
        install_failure_text_ = message.text;
        launcher_->SetStatus(std::string("Install failed: ") + install_failure_, 8.0);
    }

    const Display* SelectedDisplay() const { return DisplayForSetting(displays_, settings_.GetInt(kMonitorKey, 1)); }

    void PageDisplay(trg::Ui& ui) {
        ui.Choice("Display mode", "Borderless Fullscreen fills the monitor. F11 switches while playing.", kDisplayModeKey,
                  "windowed", {{"windowed", "Windowed"}, {"borderless", "Borderless Fullscreen"}}, 2);
        const DcpAspectRatio& previous_aspect = SavedAspectRatio();
        ui.Row("Window size", kWindowSizeHelp);
        WindowSizeCombo();
        ui.Row("Monitor", kMonitorHelp);
        MonitorCombo();
        std::vector<trg::Option> aspects;
        for (const DcpAspectRatio& aspect : dcp_aspect_ratios) {
            aspects.push_back({aspect.key, aspect.name});
        }
        ui.Combo("Aspect Ratio", kAspectRatioHelp, kAspectRatioKey, kDefaultAspectRatio, aspects);
        if (&SavedAspectRatio() != &previous_aspect) {
            const int height = settings_.GetInt(kWindowHeightKey, kDefaultWindowHeight);
            settings_.SetInt(kWindowWidthKey, WindowSizeOf(height, SavedAspectRatio()).width);
        }
        ui.Row("Graphics card", kGraphicsCardHelp);
        GraphicsCardCombo();
        ui.Toggle("VSync", kVSyncHelp, kVSyncKey, true, "Off", "On");
        ui.Heading("Scaling and sharpening");
        ui.Toggle("Upscaling", kUpscaleHelp, kUpscaleKey, false, "Off", "FSR 1");
        ui.Toggle("Sharpening", kSharpenHelp, kSharpenKey, true, "Off", "On");
        ImGui::BeginDisabled(!settings_.GetBool(kSharpenKey, true));
        ui.SliderInt("Sharpening strength", "How strong the sharpening is.", kSharpenStrengthKey, 75, 0, 100, "%d%%", 5);
        ImGui::EndDisabled();
        ui.EndRows();
    }

    const DcpAspectRatio& SavedAspectRatio() const {
        return dcp_aspect_ratios[dcp_aspect_index(settings_.Get(kAspectRatioKey, kDefaultAspectRatio).c_str())];
    }

    void WindowSizeCombo() {
        int width = settings_.GetInt(kWindowWidthKey, kDefaultWindowWidth);
        int height = settings_.GetInt(kWindowHeightKey, kDefaultWindowHeight);
        if (width <= 0 || height <= 0) {
            width = kDefaultWindowWidth;
            height = kDefaultWindowHeight;
        }
        const WindowSize current{width, height};
        const Display* display = SelectedDisplay();
        if (ImGui::BeginCombo("##window_size", WindowSizeLabel(current).c_str())) {
            for (const WindowSize size : WindowSizesFor(SavedAspectRatio())) {
                const bool selected = size == current;
                if (!selected && display && !WindowSizeFits(size, *display)) {
                    continue;
                }
                if (ImGui::Selectable(WindowSizeLabel(size).c_str(), selected) && !selected) {
                    settings_.SetInt(kWindowWidthKey, size.width);
                    settings_.SetInt(kWindowHeightKey, size.height);
                }
            }
            ImGui::EndCombo();
        }
    }

    void GraphicsCardCombo() {
        const std::string current = settings_.Get(kGraphicsCardKey, kAutomaticGpu);
        const bool listed = current == kAutomaticGpu ||
                            std::find(gpu_names_.begin(), gpu_names_.end(), current) != gpu_names_.end();
        const std::string shown = current == kAutomaticGpu ? "Automatic" : listed ? current : current + " (not available)";
        if (ImGui::BeginCombo("##graphics_card", shown.c_str())) {
            if (ImGui::Selectable("Automatic", current == kAutomaticGpu) && current != kAutomaticGpu) {
                settings_.Set(kGraphicsCardKey, kAutomaticGpu);
            }
            for (const std::string& name : gpu_names_) {
                if (ImGui::Selectable(name.c_str(), name == current) && name != current) {
                    settings_.Set(kGraphicsCardKey, name);
                }
            }
            ImGui::EndCombo();
        }
    }

    void MonitorCombo() {
        const int current = settings_.GetInt(kMonitorKey, 1);
        if (ImGui::BeginCombo("##monitor", MonitorLabel(current, displays_).c_str())) {
            for (int i = 1; i <= static_cast<int>(displays_.size()); ++i) {
                if (ImGui::Selectable(MonitorLabel(i, displays_).c_str(), i == current) && i != current) {
                    settings_.SetInt(kMonitorKey, i);
                }
            }
            ImGui::EndCombo();
        }
    }

    void PageGraphics(trg::Ui& ui) {
        std::vector<trg::Option> options;
        for (const DcpResolution& resolution : dcp_resolutions) {
            options.push_back({resolution.key, ResolutionLabel(resolution)});
        }
        ui.Combo("Render resolution", kRenderResolutionHelp, kRenderResolutionKey, kNativeResolution, options);
        ui.Toggle("Anti-aliasing", kAntiAliasingHelp, kAntiAliasingKey, false, "Off", "SMAA");
        ui.Combo("Texture filtering", kAnisotropyHelp, kAnisotropyKey, "0",
                 {{"0", "Game"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}});
        ui.EndRows();
    }

    void PageGameplay(trg::Ui& ui) {
        std::vector<trg::Option> languages;
        for (const TextLanguage& language : kTextLanguages) {
            languages.push_back({std::string(language.key), std::string(language.name)});
        }
        ui.Combo("Menu & Subtitles", kTextLanguageHelp, kTextLanguageKey, kTextLanguages[0].key.data(), languages);
        ui.Toggle("Frame rate in title", kShowFpsTitleHelp, kShowFpsTitleKey, false, "Off", "On");
        ui.Toggle("Render resolution in title", kShowResolutionTitleHelp, kShowResolutionTitleKey, false, "Off", "On");
        ui.Toggle("Frame counter", kFrameCounterHelp, kShowFrameCounterKey, false, "Hidden", "Shown");
        ui.EndRows();
    }

    void PageCheats(trg::Ui& ui) {
        ui.Help(cheats::kLauncherHelp);
        ui.Spacer(12);
        const bool blocked = build_ && !build_->cheats;
        if (blocked) {
            ui.Help((std::string("Cheats are not available for ") + build_->label + ".").c_str());
            ui.Spacer(8);
        }
        ImGui::BeginDisabled(blocked);
        for (const cheats::CheatInfo& cheat : cheats::kCheats) {
            ui.Toggle(cheat.label, cheat.description, cheat.key, cheats::kCheatDefault, "Off", "On");
        }
        std::vector<trg::Option> experience;
        for (const cheats::ExperienceChoice& choice : cheats::kExperienceChoices) {
            experience.push_back({choice.saved, choice.label});
        }
        ui.Combo(cheats::kExperienceMultiplierName, cheats::kExperienceMultiplierHelp, cheats::kExperienceMultiplierKey,
                 "1", experience);
        ImGui::EndDisabled();
        ui.EndRows();
    }

    void PageControls(trg::Ui& ui) {
        ui.Info("Controller", "Automatic: Xbox controllers work without setup.");
        ui.Row("Controller prompts", kControllerPromptsHelp);
        ControllerPromptsCombo(ui);
        if (build_ && !build_->xbox_prompts) {
            ui.Help("Xbox prompts are not available for this game build.");
        }
        ui.Info("Keyboard", "Always on, together with the controller.");
        ui.Info("Window mode", "F11 switches Windowed and Borderless Fullscreen.");
        ui.Info("Cheats menu", "F10 opens it while playing.");
        if (ui.Section("Keyboard keys", true)) {
            for (const KeyBinding& binding : KeyboardBindings()) {
                ui.Info(binding.action, binding.keys);
            }
        }
    }

    // The installed state is the truth here; the setting only remembers the choice for the next install.
    void ControllerPromptsCombo(trg::Ui& ui) {
        if (ui.TaskProgress(prompts_)) {
            return;
        }
        const bool ready = files_ == GameFileState::kValid && !install_.running();
        const bool xbox_allowed = !build_ || build_->xbox_prompts;
        ImGui::BeginDisabled(!ready || !xbox_allowed);
        const char* shown = ready ? PromptStyleLabel(prompts_installed_) : "Install the game first";
        if (ImGui::BeginCombo("##controller_prompts", shown)) {
            for (const PromptStyle style : {PromptStyle::kPlayStation, PromptStyle::kXbox}) {
                const bool selected = style == prompts_installed_;
                if (ImGui::Selectable(PromptStyleLabel(style), selected) && !selected) {
                    StartPrompts(style);
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
    }

    PromptStyle SavedPromptStyle() const {
        const std::string saved = settings_.Get(kControllerPromptsKey, kPromptsPlayStation);
        return ParsePromptStyle(saved).value_or(PromptStyle::kPlayStation);
    }

    void StartPrompts(PromptStyle style) {
        if (prompts_.running() || install_.running() || !build_) {
            return;
        }
        prompts_outcome_ = std::make_shared<PromptsResult>();
        const bool started = prompts_.Start(
            style == PromptStyle::kXbox ? "Switching to Xbox prompts..." : "Restoring PlayStation prompts...",
            [outcome = prompts_outcome_, style, app0 = paths_.app0, build = *build_](trg::Task& task) {
                PatchHooks hooks;
                hooks.phase = [&task](const char* text) {
                    task.SetLabel(text);
                    task.Progress(0, 0);
                };
                hooks.progress = [&task](uint64_t done, uint64_t total) {
                    task.Progress(static_cast<long long>(done), static_cast<long long>(total));
                    return !task.cancelled();
                };
                *outcome = SetPromptStyle(app0, build, style, XboxPromptsPatch(), hooks);
                if (outcome->ok) {
                    return std::string();
                }
                return std::string(outcome->cancelled ? "Cancelled" : "Could not change the controller prompts");
            });
        if (started) {
            Log("controller prompts: switching to %s", PromptStyleKey(style));
        }
    }

    // Runs every frame so a finished switch is picked up whichever page is open.
    void PollPrompts() {
        if (prompts_.Finish() == trg::Task::State::kIdle) {
            return;
        }
        const std::shared_ptr<PromptsResult> outcome = std::move(prompts_outcome_);
        if (!outcome) {
            return;
        }
        prompts_installed_ = InstalledPrompts();
        if (outcome->ok) {
            Log("controller prompts: %s (%s)", PromptStyleKey(prompts_installed_), outcome->detail.c_str());
            if (!settings_.SaveNow({{kControllerPromptsKey, PromptStyleKey(prompts_installed_)}})) {
                Log("could not save the controller prompts setting");
            }
            launcher_->SetStatus(std::string("Controller prompts: ") + PromptStyleLabel(prompts_installed_) + ".", 6.0);
            return;
        }
        if (outcome->cancelled) {
            Log("controller prompts: cancelled, %s installed", PromptStyleKey(prompts_installed_));
            launcher_->SetStatus("Controller prompts unchanged.", 6.0);
            return;
        }
        Log("controller prompts: failed: %s", outcome->detail.c_str());
        launcher_->SetStatus("Could not change the controller prompts. See the log.", 8.0);
    }

    std::filesystem::path UserDir() const { return PathFromUtf8(config_.user_dir ? config_.user_dir : "user"); }

    void PageTrophies(trg::Ui& ui) {
        trophy_page_->SetGame(files_ == GameFileState::kValid ? paths_.app0 : std::filesystem::path());
        trophy_page_->Draw(ui);
    }

    void PageAbout(trg::Ui& ui) {
        ui.Paragraph("Dragon's Crown Pro for PlayStation 4, running natively on this PC. No game files are included; "
                     "the game runs from the user's own copy.");
        ui.Spacer(4);
        std::error_code ec;
        if (std::filesystem::is_directory(paths_.app0, ec)) {
            ui.FolderRow("Game files", "Where the prepared game is read from.", "Open game folder", Utf8(paths_.app0));
        }
        const std::filesystem::path user = PathFromUtf8(config_.user_dir ? config_.user_dir : "user");
        std::filesystem::create_directories(user, ec);
        ui.FolderRow("Saves and settings", "Settings, saves and caches.", "Open user folder", Utf8(user));
        if (config_.logs_dir && *config_.logs_dir && std::filesystem::is_directory(PathFromUtf8(config_.logs_dir), ec)) {
            ui.FolderRow("Logs", "One numbered log per run.", "Open logs folder", config_.logs_dir);
        }
        if (ui.ButtonRow("Settings file", "Launcher choices stored as plain text.", "Show settings file") &&
            launcher_->Save()) {
            trg::OpenInFileManager(Utf8(std::filesystem::exists(settings_.file(), ec) ? settings_.file() : user));
        }
        if (ui.ConfirmRow("Reset settings", "Put the launcher and display settings back to their defaults.",
                          "Reset all settings", "Put every setting back to its default?", "Reset")) {
            settings_.ResetToDefaults();
            launcher_->SetStatus("Settings reset to defaults. Save or press Play to keep them.", 6.0);
            Log("settings reset to defaults (pending)");
        }
        ui.Info("Port version", DCP_PORT_VERSION);
        const std::string game = InstalledGameName(files_, build_);
        ui.Info("Game", game.c_str());
        ui.Info("Renderer", "Vulkan");
#ifdef _WIN32
        ui.Info("Platform", "Windows x64");
#else
        ui.Info("Platform", "Linux x86-64 (experimental)");
#endif
        char libraries[96];
        std::snprintf(libraries, sizeof(libraries), "SDL %d.%d.%d  \xC2\xB7  Dear ImGui %s  \xC2\xB7  TRG Launcher %s",
                      SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
                      SDL_VERSIONNUM_MICRO(SDL_GetVersion()), IMGUI_VERSION, DCP_TRG_LAUNCHER_VERSION);
        ui.Info("Libraries", libraries);
        ui.Help(kPortRepository);
    }

    const DcpLauncherConfig& config_;
    LauncherSettings& settings_;
    GamePaths paths_;
    GameFileState files_;
    const GameBuild* build_ = nullptr;
    LauncherHost host_;
    std::vector<Display> displays_;
    std::vector<std::string> gpu_names_;
    trg::Fonts fonts_;
    std::unique_ptr<trg::Launcher> launcher_;
    std::unique_ptr<TrophyPage> trophy_page_;
    PlayGate play_;
    ProblemDialog problem_;
    PlacementTracker placement_;
    int frames_ = 0;
    double play_started_ = 0.0;
    std::shared_ptr<InstallResult> outcome_;
    trg::Task install_;
    PackageBrowser browser_;
    std::string install_failure_;
    std::string install_failure_text_;
    PromptStyle prompts_installed_ = PromptStyle::kPlayStation;
    std::shared_ptr<PromptsResult> prompts_outcome_;
    trg::Task prompts_;
};

// Play without the launcher on screen: the same check, explained in a window of its own.
DcpLauncherResult PlayOrExplain(const DcpLauncherConfig& config) {
    char cause[320] = "", advice[320] = "";
    if (!config.check_graphics || config.check_graphics(cause, sizeof(cause), advice, sizeof(advice))) {
        return DCP_LAUNCHER_PLAY;
    }
    LauncherHost host;
    HostOptions options;
    options.title = kPortTitle;
    options.width = 640;
    options.height = 300;
    options.min_width = 360;
    options.min_height = 220;
    if (!host.Open(options)) {
        std::fprintf(stderr, "Graphics: %s %s\n", cause, advice);
        return DCP_LAUNCHER_BLOCKED;
    }
    const trg::Fonts fonts = trg::LoadSystemFonts(18.0f);
    const trg::Theme theme = LauncherTheme();
    ProblemDialog dialog;
    dialog.Show(cause, advice);
    while (dialog.open() && host.PumpEvents()) {
        if (host.Minimized()) {
            SDL_Delay(30);
            continue;
        }
        host.BeginFrame();
        dialog.Draw(fonts, theme);
        host.EndFrame(theme.bg);
    }
    return DCP_LAUNCHER_BLOCKED;
}

} // namespace
} // namespace dcp

extern "C" DcpLauncherResult dcp_launcher_run(const DcpLauncherConfig* config) {
    using namespace dcp;
    const GamePaths paths{PathFromUtf8(config->app0 ? config->app0 : ""), PathFromUtf8(config->image_dir ? config->image_dir : "")};
    LauncherSettings settings(SettingsPath(*config));
    const GameCheck check = CheckAndLog(paths);
    const bool ready = check.state == GameFileState::kValid;
    const bool show_setting = settings.GetBool(kShowLauncherKey, true);
    const bool shift = ShiftHeldAtStart() || config->force_show != 0;
    if (!LauncherWanted(ready, show_setting, shift, config->skip_once != 0)) {
        Log("skipped (show_launcher %s%s)", show_setting ? "on" : "off", config->skip_once ? ", skip requested" : "");
        return PlayOrExplain(*config);
    }
    Log("settings file %s", Utf8(settings.file()).c_str());
    Log("opening: %s%s%s", ready ? "game ready" : "game files not ready", show_setting ? ", show_launcher on" : "",
        shift ? ", Shift held or --launcher" : "");

    LauncherApp app(*config, settings, paths, check);
    Outcome outcome = Outcome::kRunning;
    if (!app.Run(outcome)) {
        std::fprintf(stderr, "Launcher: could not open its window; %s\n", ready ? "starting the game" : "game files are not ready");
        return ready ? PlayOrExplain(*config) : DCP_LAUNCHER_QUIT;
    }
    return outcome == Outcome::kPlay ? DCP_LAUNCHER_PLAY : DCP_LAUNCHER_QUIT;
}
