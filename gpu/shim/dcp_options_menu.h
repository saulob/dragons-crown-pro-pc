// SPDX-License-Identifier: GPL-2.0-or-later
// The Options menu (F1, or L3 + R3): the dcp.ini settings it offers, how each applies and where it saves. Settings the
// launcher alone owns have no row.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <span>
#include <string>
#include <string_view>

#include <SDL3/SDL_scancode.h>

#include "dcp_graphics.h"
#include "dcp_menu_input.h"
#include "dcp_renderer.h"
#include "dcp_settings.h"

namespace DcpOptions {

inline constexpr int kKey = SDL_SCANCODE_F1;
inline constexpr char kTitle[] = "OPTIONS";

enum class Page { Display, Graphics, Gameplay, Online, Controls, Cheats };
inline constexpr std::array kPages{Page::Display, Page::Graphics, Page::Gameplay,
                                   Page::Online,  Page::Controls, Page::Cheats};

struct PageList {
    std::array<Page, kPages.size()> pages{};
    size_t count = 0;
};

// The pages on offer: Online Play replaces the Cheats page, which is locked then, with the Online page.
constexpr PageList VisiblePages(bool online_play) {
    PageList list;
    for (const Page page : kPages) {
        if (page == (online_play ? Page::Cheats : Page::Online)) {
            continue;
        }
        list.pages[list.count++] = page;
    }
    return list;
}

constexpr int PageIndex(const PageList& list, Page page) {
    for (size_t i = 0; i < list.count; ++i) {
        if (list.pages[i] == page) {
            return int(i);
        }
    }
    return -1;
}

// The index of the selected page, or of Controls when that page is no longer on offer.
constexpr int RecoverPage(const PageList& list, Page selected) {
    const int index = PageIndex(list, selected);
    return index >= 0 ? index : std::max(PageIndex(list, Page::Controls), 0);
}

constexpr const char* PageName(Page page) {
    switch (page) {
    case Page::Display: return "Display";
    case Page::Graphics: return "Graphics";
    case Page::Gameplay: return "Gameplay";
    case Page::Online: return "Online";
    case Page::Controls: return "Controls";
    case Page::Cheats: return "Cheats";
    }
    return "";
}

constexpr const char* PageNote(Page page) {
    switch (page) {
    case Page::Display:
        return "Window size, Monitor and Graphics card are launcher-only: change them before pressing Play.";
    case Page::Graphics:
        return "Renderer, render resolution and texture filtering apply after a restart.";
    case Page::Gameplay:
        return "Language and Arachnophobia Safe Mode are launcher-only.";
    case Page::Online:
        return "Online name, friends and friend requests are managed in the launcher.";
    case Page::Controls:
        return "Keyboard bindings, port hotkeys and controller prompts are launcher-only.";
    case Page::Cheats:
        return "Session only: nothing here is saved to the launcher.";
    }
    return "";
}

inline constexpr int kDefaultWindowHeight = 1080;

inline constexpr char kRestartNote[] = "Some changes apply after a restart.";

// Live: the game reads it every frame. Restart: saved now, read once at the start.
enum class Apply { Live, Restart };

struct Choice {
    std::string_view value;
    std::string_view label;
};

inline constexpr Choice kOffOn[] = {{"0", "Off"}, {"1", "On"}};
inline constexpr Choice kHiddenShown[] = {{"0", "Hidden"}, {"1", "Shown"}};
inline constexpr Choice kRenderers[] = {
    {"auto", "Auto"},
    {"vulkan", "Vulkan"},
    {"opengl", "OpenGL (experimental)"},
#ifdef _WIN32
    {"d3d11", "Direct3D 11 (experimental)"},
#endif
};
#ifdef _WIN32
inline constexpr char kRendererHelp[] =
    "Vulkan is the supported renderer. OpenGL and Direct3D 11 are experimental. Direct3D 11 is intended mainly for "
    "Windows systems where Vulkan or OpenGL has problems. Auto uses OpenGL only when Vulkan cannot run.";
#else
inline constexpr char kRendererHelp[] =
    "Vulkan is the supported renderer. OpenGL is experimental. Auto uses OpenGL only when Vulkan cannot run.";
#endif
inline constexpr Choice kRenderResolutions[] = {{"native", "Native (1080p)"}, {"480p", "480p"},   {"720p", "720p"},
                                                {"1440p", "1440p"},           {"1800p", "1800p"}, {"4K", "4K"}};
inline constexpr Choice kAspectRatios[] = {{"16:9", "16:9 Original"},
                                           {"16:10", "16:10"},
                                           {"21:9", "21:9 Ultrawide"},
                                           {"32:9", "32:9 Super Ultrawide"},
                                           {"4:3", "4:3"},
                                           {"5:4", "5:4"}};
inline constexpr Choice kUpscaling[] = {{"0", "Off"}, {"1", "FSR 1"}};
inline constexpr Choice kAntiAliasing[] = {{"0", "Off"}, {"1", "SMAA"}};
inline constexpr Choice kDisplayModes[] = {{"windowed", "Windowed"}, {"borderless", "Borderless Fullscreen"}};
inline constexpr Choice kPresentations[] = {{"letterbox", "Letterbox"}, {"stretch", "Stretch"}};
inline constexpr Choice kAnisotropy[] = {{"0", "Game"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}};
inline constexpr Choice kFrameRates[] = {{"30", "30 FPS"},   {"60", "60 FPS (Original)"}, {"90", "90 FPS"},
                                         {"120", "120 FPS"}, {"144", "144 FPS"},         {"165", "165 FPS"},
                                         {"240", "240 FPS"}, {"0", "Unlimited"}};
inline constexpr Choice kStrengths[] = {
    {"0", "0%"},   {"5", "5%"},   {"10", "10%"}, {"15", "15%"}, {"20", "20%"}, {"25", "25%"}, {"30", "30%"},
    {"35", "35%"}, {"40", "40%"}, {"45", "45%"}, {"50", "50%"}, {"55", "55%"}, {"60", "60%"}, {"65", "65%"},
    {"70", "70%"}, {"75", "75%"}, {"80", "80%"}, {"85", "85%"}, {"90", "90%"}, {"95", "95%"}, {"100", "100%"}};

enum class Id {
    DisplayMode,
    VSync,
    AspectRatio,
    TrueUltrawide,
    Renderer,
    RenderResolution,
    Presentation,
    Upscaling,
    Sharpening,
    SharpeningStrength,
    AntiAliasing,
    TextureFiltering,
    SkipIntro,
    JapaneseNpcVoices,
    JapaneseMenuButtons,
    FrameRate,
    FrameRateInTitle,
    ResolutionInTitle,
    FrameCounter,
    StorySkipOffer,
    TutorialSkipOffer,
    UnlockGallery,
};

struct Setting {
    Id id;
    Page page;
    const char* key; ///< the dcp.ini line the launcher writes
    const char* label;
    const char* help;
    Apply apply;
    std::span<const Choice> choices;
    unsigned feature = 0; ///< DcpSettings::Feature the game build must support; 0: every build
};

inline constexpr Setting kSettings[] = {
    {Id::DisplayMode, Page::Display, "display_mode", "Display mode",
     "Borderless Fullscreen fills the monitor. F11 also switches it.", Apply::Live, kDisplayModes},
    {Id::VSync, Page::Display, "vsync", "VSync",
     "On prevents tearing. Off allows it, for the lowest latency and G-Sync/FreeSync.", Apply::Live, kOffOn},
    {Id::AspectRatio, Page::Display, "aspect_ratio", "Aspect Ratio",
     "The shape the game renders in. The saved window width follows it.", Apply::Restart,
     kAspectRatios, DcpSettings::RenderResolution},
    {Id::TrueUltrawide, Page::Display, "true_ultrawide", "True Ultrawide",
     "Experimental. Expands gameplay beyond 16:9. Some scenes may show unintended background elements. Available with "
     "21:9 and 32:9.",
     Apply::Restart, kOffOn, DcpSettings::TrueUltrawide},
    {Id::Renderer, Page::Graphics, "renderer", "Renderer",
     kRendererHelp,
     Apply::Restart, kRenderers},
    {Id::RenderResolution, Page::Graphics, "render_resolution", "Render resolution",
     "The resolution the game renders at. It is separate from the window size: the picture is scaled to the window. "
     "1440p and above need a powerful graphics card.",
     Apply::Restart, kRenderResolutions, DcpSettings::RenderResolution},
    {Id::Presentation, Page::Graphics, "presentation", "Presentation",
     "How the picture fits a window or display with a different aspect ratio.", Apply::Live, kPresentations},
    {Id::Upscaling, Page::Graphics, "fsr1", "Upscaling",
     "Enlarges the picture to a bigger window with AMD FidelityFX Super Resolution 1.", Apply::Live, kUpscaling},
    {Id::Sharpening, Page::Graphics, "rcas", "Sharpening",
     "Sharpens the final picture (AMD RCAS), alone or together with FSR 1 in a bigger window.", Apply::Live, kOffOn},
    {Id::SharpeningStrength, Page::Graphics, "rcas_strength", "Sharpening strength", "How strong the sharpening is.",
     Apply::Live, kStrengths},
    {Id::AntiAliasing, Page::Graphics, "smaa", "Anti-aliasing",
     "SMAA smooths jagged edges on the finished picture, at a small cost in speed.", Apply::Live, kAntiAliasing},
    {Id::TextureFiltering, Page::Graphics, "anisotropy", "Texture filtering",
     "Sharper textures seen at an angle. Game uses the game's own filtering.", Apply::Restart, kAnisotropy},
    {Id::SkipIntro, Page::Gameplay, "skip_intro", "Skip Intro",
     "Skips the startup logo and starts at the title screen.", Apply::Restart, kOffOn},
    {Id::JapaneseNpcVoices, Page::Gameplay, "dcp_japanese_npc_voices", "Japanese NPC Voices",
     "Use Japanese voices for NPCs, enemies and AI allies", Apply::Restart, kOffOn, DcpSettings::JapaneseNpcVoices},
    {Id::JapaneseMenuButtons, Page::Controls, "japanese_menu_buttons", "Japanese Menu Buttons",
     "Swaps Confirm and Cancel in the menus, as in the Japanese release. Combat controls do not change.", Apply::Restart, kOffOn},
    {Id::FrameRate, Page::Gameplay, "frame_rate", "Frame rate",
     "60 FPS is the original game rate. Faster targets show the last frame again; game speed does not change.",
     Apply::Restart, kFrameRates},
    {Id::FrameRateInTitle, Page::Gameplay, "show_fps_title", "Frame rate in title",
     "Shows the frame rate in the window title.", Apply::Live, kOffOn},
    {Id::ResolutionInTitle, Page::Gameplay, "show_resolution_title", "Render resolution in title",
     "Shows the game's render resolution in the window title.", Apply::Live, kOffOn},
    {Id::FrameCounter, Page::Gameplay, "show_fps", "Frame counter",
     "Shows the frame rate and frame time in a corner of the game. F2 also switches it for this session.",
     Apply::Live, kHiddenShown},
    {Id::StorySkipOffer, Page::Gameplay, "story_skip_offer", "Story Skip Offer",
     "Offers the game's built-in story skip when creating a new character.", Apply::Restart, kOffOn},
    {Id::TutorialSkipOffer, Page::Gameplay, "tutorial_skip_offer", "Tutorial Skip Offer",
     "Lets new characters choose whether to begin the tutorial.", Apply::Restart, kOffOn},
    {Id::UnlockGallery, Page::Gameplay, "unlock_gallery", "Unlock Gallery",
     "Unlocks all Gallery content while enabled without changing save progress.", Apply::Restart, kOffOn,
     DcpSettings::UnlockGallery},
};

constexpr const Setting& SettingOf(Id id) {
    return kSettings[size_t(id)];
}

constexpr bool SettingsInOrder() {
    for (size_t i = 0; i < std::size(kSettings); ++i) {
        if (size_t(kSettings[i].id) != i) {
            return false;
        }
    }
    return true;
}
static_assert(SettingsInOrder());

inline int ChoiceIndex(const Setting& setting, std::string_view value) {
    for (size_t i = 0; i < setting.choices.size(); ++i) {
        if (setting.choices[i].value == value) {
            return int(i);
        }
    }
    return -1;
}

inline std::string Current(const DcpSettings::Values& v, Id id) {
    const auto flag = [](bool on) { return std::string(on ? "1" : "0"); };
    switch (id) {
    case Id::DisplayMode:
        return std::string(DcpGraphics::DisplayModeKeys[size_t(std::clamp(v.display_mode.load(), 0,
                                                                          int(DcpGraphics::DisplayModeCount) - 1))]);
    case Id::VSync: return flag(v.vsync);
    case Id::AspectRatio: return std::string(DcpGraphics::ParseAspectRatio(v.aspect_ratio).key);
    case Id::TrueUltrawide: return flag(v.true_ultrawide);
    case Id::Renderer:
        return std::string(DcpRenderer::ChoiceKeys[size_t(std::clamp(v.renderer.load(), 0, int(DcpRenderer::ChoiceCount) - 1))]);
    case Id::RenderResolution: return std::string(DcpGraphics::ParseRenderResolution(v.render_resolution));
    case Id::Presentation:
        return std::string(DcpGraphics::PresentationKeys[size_t(std::clamp(v.presentation.load(), 0,
                                                                           int(DcpGraphics::PresentationCount) - 1))]);
    case Id::Upscaling: return flag(v.fsr1);
    case Id::Sharpening: return flag(v.rcas);
    case Id::SharpeningStrength: return std::to_string(v.rcas_strength.load());
    case Id::AntiAliasing: return flag(v.smaa);
    case Id::TextureFiltering: return std::to_string(v.anisotropy.load());
    case Id::SkipIntro: return flag(v.skip_intro);
    case Id::JapaneseNpcVoices: return flag(v.japanese_npc_voices);
    case Id::JapaneseMenuButtons: return flag(v.japanese_menu_buttons);
    case Id::FrameRate: return std::to_string(v.frame_rate.load());
    case Id::FrameRateInTitle: return flag(v.show_fps_title);
    case Id::ResolutionInTitle: return flag(v.show_resolution_title);
    case Id::FrameCounter: return flag(v.show_fps);
    case Id::StorySkipOffer: return flag(v.story_skip_offer);
    case Id::TutorialSkipOffer: return flag(v.tutorial_skip_offer);
    case Id::UnlockGallery: return flag(v.unlock_gallery);
    }
    return {};
}

// Whether the running game build supports the setting; the menu shows no row for one it does not.
inline bool Available(const Setting& setting, const DcpSettings::Values& v) {
    return setting.feature == 0 || (v.features.load() & setting.feature) != 0;
}

// What the video core and the window read each frame. Only called for Live settings.
inline void ApplyLive(DcpSettings::Values& v, Id id, std::string_view value) {
    const bool on = value == "1";
    switch (id) {
    case Id::DisplayMode: v.display_mode = DcpGraphics::ParseDisplayMode(value); break;
    case Id::VSync: v.vsync = on; break;
    case Id::Presentation: v.presentation = DcpGraphics::ParsePresentation(value); break;
    case Id::Upscaling: v.fsr1 = on; break;
    case Id::Sharpening: v.rcas = on; break;
    case Id::SharpeningStrength: v.rcas_strength = DcpGraphics::ParseRcasStrength(std::atoi(std::string(value).c_str())); break;
    case Id::AntiAliasing: v.smaa = on; break;
    case Id::FrameRateInTitle: v.show_fps_title = on; break;
    case Id::ResolutionInTitle: v.show_resolution_title = on; break;
    case Id::FrameCounter: v.show_fps = on; break;
    case Id::AspectRatio:
    case Id::TrueUltrawide:
    case Id::Renderer:
    case Id::RenderResolution:
    case Id::TextureFiltering:
    case Id::SkipIntro:
    case Id::JapaneseNpcVoices:
    case Id::JapaneseMenuButtons:
    case Id::FrameRate:
    case Id::StorySkipOffer:
    case Id::TutorialSkipOffer:
    case Id::UnlockGallery: break;
    }
}

struct Outcome {
    bool changed = false;
    bool saved = true;
};

// The menu's settings over the running session: a Live choice changes the values the game reads and a Restart choice
// waits in pending_, shown as chosen but not applied. Both save to dcp.ini at once.
class Options {
public:
    explicit Options(DcpSettings::Values& values) : values_(values) {}

    std::string Shown(Id id) const {
        const std::string& pending = pending_[size_t(id)];
        return pending.empty() ? Current(values_, id) : pending;
    }

    bool Pending(Id id) const { return !pending_[size_t(id)].empty(); }

    bool RestartPending() const {
        for (const std::string& pending : pending_) {
            if (!pending.empty()) {
                return true;
            }
        }
        return false;
    }

    Outcome Choose(Id id, std::string_view value) {
        const Setting& setting = SettingOf(id);
        if (ChoiceIndex(setting, value) < 0 || Shown(id) == value) {
            return {};
        }
        if (setting.apply == Apply::Live) {
            ApplyLive(values_, id, value);
        } else {
            pending_[size_t(id)] = value == Current(values_, id) ? std::string() : std::string(value);
        }
        return {true, DcpSettings::SaveEntries(EntriesFor(setting, value))};
    }

    Outcome Step(Id id, int step) {
        const Setting& setting = SettingOf(id);
        const int next = dcp::menu::StepChoice(ChoiceIndex(setting, Shown(id)), int(setting.choices.size()), step);
        return next < 0 ? Outcome{} : Choose(id, setting.choices[size_t(next)].value);
    }

    Outcome Cycle(Id id) {
        const Setting& setting = SettingOf(id);
        const int next = dcp::menu::NextChoiceWrapping(ChoiceIndex(setting, Shown(id)), int(setting.choices.size()));
        return next < 0 ? Outcome{} : Choose(id, setting.choices[size_t(next)].value);
    }

private:
    // An aspect ratio also saves the window width that keeps the window the same height, as the launcher does.
    DcpSettings::Entries EntriesFor(const Setting& setting, std::string_view value) const {
        DcpSettings::Entries entries{{setting.key, std::string(value)}};
        if (setting.id == Id::AspectRatio) {
            const int height = values_.requested_height > 0 ? values_.requested_height.load() : kDefaultWindowHeight;
            entries.emplace_back("window_width",
                                 std::to_string(DcpGraphics::WindowWidthFor(height, DcpGraphics::ParseAspectRatio(value))));
        }
        return entries;
    }

    DcpSettings::Values& values_;
    std::array<std::string, std::size(kSettings)> pending_;
};

} // namespace DcpOptions
