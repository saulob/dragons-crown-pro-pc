// Options menu tests: the L3 + R3 chord, the close gate, mutual exclusion with the cheat menu, stepping through rows,
// pages and choices, which settings apply live or after a restart, what is saved to dcp.ini, and that Online Play
// keeps the Cheats page locked. Logic and temporary files only: no window, no game files.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "dcp_cheat_menu.h"
#include "dcp_frame_pacing.h"
#include "game_profile.h"
#include "net/online_version.h"
#include "dcp_menu_input.h"
#include "dcp_options_menu.h"
#include "dcp_options_online.h"
#include "localization/ui_text.h"
#include "runtime_cheats.h"
#include "runtime_online_page.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void Check(bool passed, const char* name) {
    if (!passed) {
        ++failures;
        std::printf("FAIL: %s\n", name);
    }
}

std::string ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string Source(const char* relative) {
    return ReadAll(fs::path(DCP_SOURCE_DIR) / relative);
}

bool Contains(const std::string& text, const std::string& what) {
    return text.find(what) != std::string::npos;
}

void WriteFile(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

constexpr uint32_t kL3 = 1u << 7;
constexpr uint32_t kR3 = 1u << 8;
constexpr uint32_t kOther = 1u << 1;

void TestChord() {
    dcp::menu::Chord chord(kL3 | kR3);
    Check(!chord.Poll(0), "neither button down does not press");
    Check(!chord.Poll(kL3), "only L3 down does not press");
    Check(chord.Poll(kL3 | kR3), "both together press");
    Check(!chord.Poll(kL3 | kR3), "a held chord presses only once");
    Check(!chord.Poll(kL3 | kR3 | kOther), "another button held alongside does not press again");
    Check(!chord.Poll(kR3), "letting go of one button is not a press");
    Check(!chord.Poll(kL3 | kR3), "pressing it back before a full release does not press");
    Check(!chord.Poll(0), "letting go of both is not a press");
    Check(chord.Poll(kL3 | kR3), "pressing again after a full release presses again");

    dcp::menu::Chord other(kL3 | kR3);
    Check(!other.Poll(kOther) && !other.Poll(kOther | kL3) && !other.Poll(kOther | kR3),
          "unrelated bits and one of the pair do not press");
    Check(other.Poll(kOther | kL3 | kR3), "unrelated bits set alongside the pair still press");
    Check(!other.Poll(kOther), "releasing the pair keeps unrelated bits from pressing");

    dcp::menu::Chord first_poll(kL3 | kR3);
    Check(first_poll.Poll(kL3 | kR3), "a chord already down at the first poll presses once");
    Check(!first_poll.Poll(kL3 | kR3), "and not again while held");
}

void TestToggleAndMutualExclusion() {
    using dcp::menu::ToggleAction;
    using dcp::menu::TogglePress;
    Check(TogglePress(false, false, false) == ToggleAction::Open, "a closed menu opens");
    Check(TogglePress(false, true, false) == ToggleAction::Close, "an open menu closes");
    Check(TogglePress(false, true, true) == ToggleAction::Close, "an open menu closes even when something else is up");
    Check(TogglePress(true, false, false) == ToggleAction::None, "a held key does not open");
    Check(TogglePress(true, true, false) == ToggleAction::None, "a held key does not close");
    Check(TogglePress(false, false, true) == ToggleAction::None, "the menu stays away from another menu or dialog");

    // The Options menu is busy while the cheat menu is open, and the cheat menu while the Options menu is.
    const bool cheat_open = true, options_open = false;
    Check(TogglePress(false, options_open, cheat_open) == ToggleAction::None, "F1 does not open over the cheat menu");
    Check(dcp::cheats::MenuKeyPress(false, false, false, true) == dcp::cheats::MenuKeyAction::None,
          "F10 does not open over the Options menu");
    Check(dcp::cheats::MenuKeyPress(false, false, true, false) == dcp::cheats::MenuKeyAction::Close,
          "F10 still closes its own menu");

    const std::string overlay = Source("gpu/shim/dcp_overlay.cpp");
    Check(Contains(overlay, "const bool busy = text_entry_active || choice_active || loading || options_open;"),
          "the cheat key treats the Options menu as busy");
    Check(Contains(overlay, "const bool busy = cheat_open || text_entry_active || choice_active || loading;"),
          "the Options menu treats the cheat menu and the game's dialogs as busy");
    Check(Contains(overlay, "return cheat_open || options_open || text_entry_active || choice_active;"),
          "an open Options menu captures the game's input");
}

void TestCloseGate() {
    dcp::menu::CloseGate gate;
    Check(!gate.Step(false, 0.0), "the gate does nothing before a close is requested");
    gate.Request();
    Check(gate.requested(), "a request is remembered");
    Check(!gate.Step(true, 1.0), "the menu stays while something it received is held");
    Check(!gate.Step(true, 1.5), "and while it is still held after half the timeout");
    Check(gate.Step(false, 1.6), "it goes once everything is released");

    dcp::menu::CloseGate stuck;
    stuck.Request();
    Check(!stuck.Step(true, 10.0), "the wait starts at the first step");
    Check(stuck.Step(true, 10.0 + dcp::menu::kReleaseTimeoutSeconds), "a button never released times out");

    Check(dcp::menu::InputReleased(false, 0.0, 1.0) && !dcp::menu::InputReleased(true, 0.5, 1.0),
          "InputReleased follows what is held and the wait");
    Check(dcp::cheats::kReleaseTimeoutSeconds == dcp::menu::kReleaseTimeoutSeconds,
          "the cheat menu shares the release timeout");
}

void TestStepping() {
    using namespace dcp::menu;
    Check(WrapIndex(0, 5, -1) == 4, "rows wrap backward from the first");
    Check(WrapIndex(4, 5, 1) == 0, "rows wrap forward from the last");
    Check(WrapIndex(2, 5, 1) == 3 && WrapIndex(2, 5, -1) == 1, "rows move one in the middle");
    Check(WrapIndex(0, 0, 1) == 0, "no rows stay at 0");
    Check(WrapIndex(0, int(DcpOptions::kPages.size()), -1) == int(DcpOptions::kPages.size()) - 1,
          "pages wrap backward from the first");
    Check(WrapIndex(int(DcpOptions::kPages.size()) - 1, int(DcpOptions::kPages.size()), 1) == 0,
          "pages wrap forward from the last");

    Check(StepChoice(1, 4, 1) == 2 && StepChoice(1, 4, -1) == 0, "a choice steps one either way");
    Check(StepChoice(0, 4, -1) == 0 && StepChoice(3, 4, 1) == 3, "a choice stops at the ends");
    Check(StepChoice(-1, 4, 1) == 0 && StepChoice(-1, 4, -1) == 0, "an unknown value steps to the first choice");
    Check(StepChoice(0, 0, 1) == -1, "no choices is -1");

    Check(NextChoiceWrapping(0, 2) == 1 && NextChoiceWrapping(1, 2) == 0, "cycling flips a toggle");
    Check(NextChoiceWrapping(3, 4) == 0 && NextChoiceWrapping(-1, 4) == 0, "cycling wraps and recovers from unknown");
    Check(NextChoiceWrapping(0, 0) == -1, "cycling no choices is -1");

    Check(ScrollToShow(0, 0, 5, 20) == 0, "a selection at the top shows the top");
    Check(ScrollToShow(5, 0, 5, 20) == 1, "scrolling down keeps the selection in view");
    Check(ScrollToShow(19, 0, 5, 20) == 15, "the last row ends the list");
    Check(ScrollToShow(2, 10, 5, 20) == 2, "scrolling up keeps the selection in view");
    Check(ScrollToShow(3, 0, 5, 5) == 0 && ScrollToShow(3, 7, 8, 5) == 0, "a list that fits does not scroll");
}

void TestSettingsTable() {
    using namespace DcpOptions;
    std::string keys;
    for (const Setting& setting : kSettings) {
        const std::string key = setting.key;
        Check(keys.find("|" + key + "|") == std::string::npos, "every setting has its own dcp.ini key");
        keys += "|" + key + "|";
        Check(!key.starts_with("cheat"), "the Options menu saves no cheat");
        Check(!setting.choices.empty() && setting.label[0] && setting.help[0], "every setting is labelled and has choices");
    }
    for (const char* launcher_only : {"window_width", "window_height", "monitor", "graphics_card", "text_language",
                                      "controller_prompts", "arachnophobia_safe_mode", "allow_overlay_programs"}) {
        Check(keys.find(std::string("|") + launcher_only + "|") == std::string::npos,
              "launcher-only settings are not offered");
    }

    int restart = 0;
    for (const Setting& setting : kSettings) {
        restart += setting.apply == Apply::Restart;
    }
    Check(restart == 12 && SettingOf(Id::TextureFiltering).apply == Apply::Restart &&
              SettingOf(Id::Renderer).apply == Apply::Restart && SettingOf(Id::RenderResolution).apply == Apply::Restart &&
              SettingOf(Id::AspectRatio).apply == Apply::Restart && SettingOf(Id::TrueUltrawide).apply == Apply::Restart &&
              SettingOf(Id::JapaneseNpcVoices).apply == Apply::Restart &&
              SettingOf(Id::FrameRate).apply == Apply::Restart && SettingOf(Id::SkipIntro).apply == Apply::Restart &&
              SettingOf(Id::JapaneseMenuButtons).apply == Apply::Restart &&
              SettingOf(Id::StorySkipOffer).apply == Apply::Restart &&
              SettingOf(Id::TutorialSkipOffer).apply == Apply::Restart &&
              SettingOf(Id::UnlockGallery).apply == Apply::Restart,
          "the renderer, the render shape, texture filtering, the frame rate, Japanese NPC Voices, Skip Intro, the menu "
          "buttons, the skip offers and Unlock Gallery are the settings read once");
    for (const Id live : {Id::DisplayMode, Id::VSync, Id::Presentation, Id::Upscaling, Id::Sharpening,
                          Id::SharpeningStrength, Id::AntiAliasing, Id::FrameRateInTitle, Id::ResolutionInTitle,
                          Id::FrameCounter}) {
        Check(SettingOf(live).apply == Apply::Live, "the settings read every frame apply at once");
    }

    for (const Choice& rate : SettingOf(Id::FrameRate).choices) {
        Check(DcpFramePacing::ParseFrameRate(std::string(rate.value)) == std::stoi(std::string(rate.value)),
              "every frame rate choice is a mode the video core accepts");
    }
    Check(SettingOf(Id::TextureFiltering).choices.size() == DcpGraphics::Anisotropy.size(),
          "texture filtering offers the video core's values");
}

fs::path MakeTemporaryFolder() {
    fs::path folder = fs::temp_directory_path() / "dcp_options_menu_tests";
    fs::remove_all(folder);
    fs::create_directories(folder);
    return folder;
}

void TestPersistence(const fs::path& folder) {
    using namespace DcpOptions;
    const fs::path file = folder / "dcp.ini";
    WriteFile(file,
              "# my comment\nwindow_width = 1600\nshow_launcher = 1\nvsync = 1\nanisotropy = 0\nframe_rate = 60\n"
              "# trailing comment\nfuture_setting = keep me\n");
    DcpSettings::Load(file.string());
    auto& live = DcpSettings::Get();
    Options options(live);

    Check(!options.RestartPending(), "nothing waits for a restart at the start");
    Check(options.Shown(Id::VSync) == "1" && options.Shown(Id::FrameRate) == "60", "the menu shows the loaded values");

    Outcome outcome = options.Choose(Id::VSync, "0");
    Check(outcome.changed && outcome.saved && !live.vsync, "a live setting changes the value the game reads");
    Check(!options.RestartPending() && !options.Pending(Id::VSync), "a live setting never waits for a restart");

    outcome = options.Choose(Id::TextureFiltering, "8");
    Check(outcome.changed && outcome.saved, "a restart setting is saved");
    Check(live.anisotropy == 0, "a restart setting leaves the value the game reads alone");
    Check(options.Shown(Id::TextureFiltering) == "8" && options.Pending(Id::TextureFiltering) &&
              options.RestartPending(),
          "a restart setting shows as chosen and waits for the restart");

    outcome = options.Choose(Id::FrameRate, "144");
    Check(outcome.changed && live.frame_rate == 60 && options.Pending(Id::FrameRate),
          "the frame rate is saved and applies at the next start");
    Check(!options.Choose(Id::FrameRate, "144").changed, "choosing what is shown changes nothing");
    Check(!options.Choose(Id::FrameRate, "77").changed && !options.Choose(Id::VSync, "maybe").changed,
          "a value that is not a choice is refused");

    options.Choose(Id::FrameRate, "60");
    Check(!options.Pending(Id::FrameRate) && options.RestartPending(),
          "choosing the running value again clears its pending change");
    options.Choose(Id::TextureFiltering, "0");
    Check(!options.RestartPending(), "no pending change is left once every restart setting is back");

    options.Choose(Id::TextureFiltering, "4");
    options.Choose(Id::DisplayMode, "borderless");
    options.Choose(Id::Presentation, "stretch");
    options.Choose(Id::Upscaling, "1");
    options.Choose(Id::Sharpening, "0");
    options.Choose(Id::SharpeningStrength, "40");
    options.Choose(Id::AntiAliasing, "1");
    options.Choose(Id::FrameRateInTitle, "1");
    options.Choose(Id::ResolutionInTitle, "1");
    options.Choose(Id::FrameCounter, "1");
    Check(live.display_mode == DcpGraphics::Borderless && live.presentation == DcpGraphics::Stretch && live.fsr1 &&
              !live.rcas && live.rcas_strength == 40 && live.smaa && live.show_fps_title &&
              live.show_resolution_title && live.show_fps,
          "every live setting reaches the value the game reads");

    const std::string text = ReadAll(file);
    Check(Contains(text, "# my comment\n") && Contains(text, "# trailing comment\n") &&
              Contains(text, "window_width = 1600\n") && Contains(text, "show_launcher = 1\n") &&
              Contains(text, "future_setting = keep me\n"),
          "comments and settings the menu does not own are kept");
    Check(Contains(text, "vsync = 0\n") && Contains(text, "anisotropy = 4\n") && Contains(text, "frame_rate = 60\n") &&
              Contains(text, "display_mode = borderless\n") && Contains(text, "presentation = stretch\n") &&
              Contains(text, "rcas_strength = 40\n") && Contains(text, "show_fps = 1\n"),
          "the changes are written under the launcher's keys");

    DcpSettings::Values restarted;
    Check(DcpSettings::Read(file.string(), restarted), "the saved file reads back");
    Check(restarted.vsync == false && restarted.anisotropy == 4 && restarted.frame_rate == 60 &&
              restarted.display_mode == DcpGraphics::Borderless && restarted.presentation == DcpGraphics::Stretch &&
              restarted.fsr1 && !restarted.rcas && restarted.rcas_strength == 40 && restarted.smaa &&
              restarted.show_fps_title && restarted.show_resolution_title && restarted.show_fps,
          "a restart starts with what the menu saved");

    Check(DcpSettings::SaveDisplayMode(), "F11's save still works");
    Check(Contains(ReadAll(file), "display_mode = borderless\n"), "F11 saves the display mode the menu set");
}

void TestSkipOffers(const fs::path& folder) {
    using namespace DcpOptions;
    int story = 0, tutorial = 0;
    for (const Setting& setting : kSettings) {
        story += setting.page == Page::Gameplay && std::string(setting.label) == "Story Skip Offer";
        tutorial += setting.page == Page::Gameplay && std::string(setting.label) == "Tutorial Skip Offer";
    }
    Check(story == 1 && tutorial == 1, "each skip offer is in the Gameplay page exactly once");

    const std::string launcher = Source("launcher/settings.h");
    const std::string defaults = Source("launcher/settings.cpp");
    for (const Id id : {Id::StorySkipOffer, Id::TutorialSkipOffer}) {
        const Setting& setting = SettingOf(id);
        Check(Contains(launcher, std::string("= \"") + setting.key + "\";"), "the key is the launcher's");
        Check(setting.apply == Apply::Restart && setting.choices.size() == 2, "a skip offer is a restart-required Off/On");
        Check(Current(DcpSettings::Values{}, id) == "0", "a skip offer is Off by default");
    }
    Check(Contains(defaults, "{kStorySkipOfferKey, \"0\"}") && Contains(defaults, "{kTutorialSkipOfferKey, \"0\"}"),
          "the launcher's defaults are Off");

    const fs::path file = folder / "skip.ini";
    WriteFile(file, "# keep\nstory_skip_offer = 0\nfuture_setting = 1\n");
    DcpSettings::Load(file.string());
    DcpSettings::Values& live = DcpSettings::Get();
    live.story_skip_offer = false;
    live.tutorial_skip_offer = false;
    Options options(live);

    Outcome outcome = options.Choose(Id::StorySkipOffer, "1");
    Check(outcome.changed && outcome.saved && options.Pending(Id::StorySkipOffer) && options.RestartPending(),
          "a skip offer waits for the restart");
    Check(!live.story_skip_offer && options.Shown(Id::StorySkipOffer) == "1", "and the running game keeps its value");
    outcome = options.Choose(Id::TutorialSkipOffer, "1");
    Check(outcome.changed && !live.tutorial_skip_offer, "the tutorial offer behaves the same");

    const std::string text = ReadAll(file);
    Check(Contains(text, "story_skip_offer = 1\n") && Contains(text, "tutorial_skip_offer = 1\n") &&
              Contains(text, "# keep\n") && Contains(text, "future_setting = 1\n"),
          "the offers are saved under the launcher's keys and the rest of the file is kept");
    DcpSettings::Values restarted;
    Check(DcpSettings::Read(file.string(), restarted) && restarted.story_skip_offer && restarted.tutorial_skip_offer,
          "a restart reads what the menu saved");

    DcpSettings::Values untouched;
    ApplyLive(untouched, Id::StorySkipOffer, "1");
    ApplyLive(untouched, Id::TutorialSkipOffer, "1");
    Check(!untouched.story_skip_offer && !untouched.tutorial_skip_offer, "nothing applies a skip offer live");

    const std::string menu = Source("gpu/shim/dcp_options_menu.h") + Source("gpu/shim/dcp_options_menu.cpp");
    Check(!Contains(menu, "skip_offers") && !Contains(menu, "guest"), "the menu does not reach the skip offer hooks");
    Check(Contains(Source("gpu/shim/dcp_settings.cpp"), "key == \"story_skip_offer\"") &&
              !Contains(Source("gpu/shim/dcp_options_menu.cpp"), "story_skip_offer"),
          "the skip offers have one reader and one table row");
}

void TestSkipIntro(const fs::path& folder) {
    using namespace DcpOptions;
    int rows = 0;
    size_t intro = 0, frame_rate = 0, position = 0;
    for (const Setting& setting : kSettings) {
        if (setting.page != Page::Gameplay) continue;
        rows += std::string(setting.label) == "Skip Intro";
        intro = std::string(setting.label) == "Skip Intro" ? position : intro;
        frame_rate = setting.id == Id::FrameRate ? position : frame_rate;
        ++position;
    }
    Check(rows == 1 && intro < frame_rate, "Skip Intro is in the Gameplay page exactly once, before the frame rate");

    const Setting& setting = SettingOf(Id::SkipIntro);
    Check(std::string(setting.key) == "skip_intro" && Contains(Source("launcher/settings.h"), "kSkipIntroKey[] = \"skip_intro\";"),
          "the key is the launcher's");
    Check(setting.apply == Apply::Restart && setting.choices.size() == 2, "Skip Intro is a restart-required Off/On");
    Check(Current(DcpSettings::Values{}, Id::SkipIntro) == "0", "Skip Intro is Off by default");
    Check(Contains(Source("launcher/settings.cpp"), "{kSkipIntroKey, \"0\"}"), "the launcher's default is Off");

    const fs::path file = folder / "intro.ini";
    WriteFile(file, "# keep\nfuture_setting = 1\n");
    DcpSettings::Load(file.string());
    DcpSettings::Values& live = DcpSettings::Get();
    live.skip_intro = false;
    Options options(live);
    const Outcome outcome = options.Choose(Id::SkipIntro, "1");
    Check(outcome.changed && outcome.saved && options.Pending(Id::SkipIntro) && options.RestartPending(),
          "Skip Intro waits for the restart");
    Check(!live.skip_intro && options.Shown(Id::SkipIntro) == "1", "and the running game keeps its value");

    const std::string text = ReadAll(file);
    Check(Contains(text, "skip_intro = 1\n") && Contains(text, "# keep\n") && Contains(text, "future_setting = 1\n"),
          "it is saved under the launcher's key and the rest of the file is kept");
    DcpSettings::Values restarted;
    Check(DcpSettings::Read(file.string(), restarted) && restarted.skip_intro, "a restart reads what the menu saved");

    DcpSettings::Values untouched;
    ApplyLive(untouched, Id::SkipIntro, "1");
    Check(!untouched.skip_intro, "nothing applies Skip Intro live");
    Check(Contains(Source("gpu/shim/dcp_settings.cpp"), "key == \"skip_intro\"") &&
              !Contains(Source("gpu/shim/dcp_options_menu.cpp"), "skip_intro"),
          "Skip Intro has one reader and one table row");
}

void TestUnlockGallery(const fs::path& folder) {
    using namespace DcpOptions;
    int rows = 0;
    size_t gallery = 0, tutorial = 0, position = 0;
    for (const Setting& setting : kSettings) {
        if (setting.page != Page::Gameplay) continue;
        rows += std::string(setting.label) == "Unlock Gallery";
        gallery = setting.id == Id::UnlockGallery ? position : gallery;
        tutorial = setting.id == Id::TutorialSkipOffer ? position : tutorial;
        ++position;
    }
    Check(rows == 1 && gallery == tutorial + 1, "Unlock Gallery is in the Gameplay page exactly once, after the skip offers");

    const Setting& setting = SettingOf(Id::UnlockGallery);
    Check(std::string(setting.key) == "unlock_gallery" &&
              Contains(Source("launcher/settings.h"), "kUnlockGalleryKey[] = \"unlock_gallery\";"),
          "the key is the launcher's");
    Check(std::string(setting.help) ==
              "Unlocks all Gallery content while enabled without changing save progress.",
          "the help text says it does not change the save");
    Check(setting.apply == Apply::Restart && setting.choices.size() == 2, "Unlock Gallery is a restart-required Off/On");
    Check(Current(DcpSettings::Values{}, Id::UnlockGallery) == "0", "Unlock Gallery is Off by default");
    Check(Contains(Source("launcher/settings.cpp"), "{kUnlockGalleryKey, \"0\"}"), "the launcher's default is Off");

    const fs::path file = folder / "gallery.ini";
    WriteFile(file, "# keep\nfuture_setting = 1\n");
    DcpSettings::Load(file.string());
    DcpSettings::Values& live = DcpSettings::Get();
    live.unlock_gallery = false;
    Options options(live);
    const Outcome outcome = options.Choose(Id::UnlockGallery, "1");
    Check(outcome.changed && outcome.saved && options.Pending(Id::UnlockGallery) && options.RestartPending(),
          "Unlock Gallery waits for the restart");
    Check(!live.unlock_gallery && options.Shown(Id::UnlockGallery) == "1", "and the running game keeps its value");

    const std::string text = ReadAll(file);
    Check(Contains(text, "unlock_gallery = 1\n") && Contains(text, "# keep\n") && Contains(text, "future_setting = 1\n"),
          "it is saved under the launcher's key and the rest of the file is kept");
    DcpSettings::Values restarted;
    Check(DcpSettings::Read(file.string(), restarted) && restarted.unlock_gallery, "a restart reads what the menu saved");

    DcpSettings::Values untouched;
    ApplyLive(untouched, Id::UnlockGallery, "1");
    Check(!untouched.unlock_gallery, "nothing applies Unlock Gallery live");
    Check(Contains(Source("gpu/shim/dcp_settings.cpp"), "key == \"unlock_gallery\"") &&
              !Contains(Source("gpu/shim/dcp_options_menu.cpp"), "unlock_gallery"),
          "Unlock Gallery has one reader and one table row");
}

void TestControllerRumble(const fs::path& folder) {
    using namespace DcpOptions;
    int rows = 0;
    for (const Setting& setting : kSettings) {
        rows += std::string(setting.label) == "Controller Rumble";
    }
    const Setting& setting = SettingOf(Id::ControllerRumble);
    Check(rows == 1 && setting.page == Page::Controls && setting.apply == Apply::Live && setting.choices.size() == 2 &&
              std::string(setting.key) == "controller_rumble" && setting.feature == 0,
          "Controller Rumble is one live Off/On row in the Controls page for every build");
    Check(Current(DcpSettings::Values{}, Id::ControllerRumble) == "1", "it reads On by default");

    const fs::path file = folder / "rumble.ini";
    WriteFile(file, "# keep\ncontroller_rumble = 0\n");
    DcpSettings::Values seeded;
    Check(DcpSettings::Read(file.string(), seeded) && !seeded.controller_rumble &&
              Current(seeded, Id::ControllerRumble) == "0",
          "the saved launcher value seeds what F1 shows");

    DcpSettings::Load(file.string());
    DcpSettings::Values& live = DcpSettings::Get();
    live.controller_rumble = true;
    Options options(live);
    Check(options.Shown(Id::ControllerRumble) == "1", "F1 reads the current runtime state");
    Outcome outcome = options.Choose(Id::ControllerRumble, "0");
    Check(outcome.changed && outcome.saved && !live.controller_rumble && !options.Pending(Id::ControllerRumble) &&
              !options.RestartPending() && options.Shown(Id::ControllerRumble) == "0",
          "On to Off takes effect at once, with no restart");
    outcome = options.Choose(Id::ControllerRumble, "1");
    Check(outcome.changed && live.controller_rumble && options.Shown(Id::ControllerRumble) == "1",
          "Off to On takes effect at once");
    live.controller_rumble = false;
    Check(options.Shown(Id::ControllerRumble) == "0", "F1 follows the runtime state when something else changed it");
    Check(Contains(ReadAll(file), "controller_rumble = 1\n") && Contains(ReadAll(file), "# keep\n"),
          "like every Options row it saves under the launcher's key and keeps the rest of the file");
    live.controller_rumble = true;

    const std::string menu = Source("gpu/shim/dcp_options_menu.cpp");
    const size_t change = menu.find("outcome.changed && id == DcpOptions::Id::ControllerRumble");
    Check(change != std::string::npos && menu.find("runtime_pad_rumble_changed();", change) < change + 120,
          "F1 tells the pad when the setting changes");
    const std::string pad = Source("src/runtime_pad.c");
    const size_t vibration = pad.find("static ABI int32_t pad_vibration(");
    const size_t end = pad.find("return 0;", vibration);
    Check(pad.find("if (g && dcpgpu_controller_rumble()) SDL_RumbleGamepad(", vibration) < end &&
              pad.find("SDL_RumbleGamepad(") == pad.find("SDL_RumbleGamepad(", pad.find("dcpgpu_controller_rumble()", vibration)) &&
              !Contains(pad, "static int rumble_enabled"),
          "the pad reads the one shared state on every request, for every slot");
    const size_t changed = pad.find("void runtime_pad_rumble_changed(void) {");
    Check(changed != std::string::npos && pad.find("SDL_RumbleGamepad(slots[i].gamepad,0,0,0)", changed) < changed + 400,
          "switching Off stops a rumble in progress on every connected pad");
    Check(Contains(Source("src/loader.c"), "runtime_pad_set_rumble(dcp_controller_rumble(settings_file));") &&
              Contains(pad, "dcpgpu_set_controller_rumble(enabled);"),
          "the launcher value still seeds the same state at start");
    Check(Contains(pad, "if (dcpgpu_overlay_captures_input()) return; /* settings menu open: neutral input */"),
          "input handling is as before");
    const std::string portuguese = Source("launcher/localization/pt_br_ui_text.cpp");
    Check(Contains(portuguese, "Controller Rumble\tVibração do controle\n"), "the Portuguese (Brazil) label is present");
}

void TestJapaneseMenuButtons(const fs::path& folder) {
    using namespace DcpOptions;
    int rows = 0;
    for (const Setting& setting : kSettings) {
        rows += std::string(setting.label) == "Japanese Menu Buttons";
    }
    Check(rows == 1 && SettingOf(Id::JapaneseMenuButtons).page == Page::Controls,
          "Japanese Menu Buttons is in the Controls page exactly once, and not in Gameplay");
    Check(Contains(Source("gpu/shim/dcp_options_menu.cpp"), "if (page == Page::Controls) {\n        for (const DcpOptions::Setting& setting"),
          "the Controls page lists its settings before the hotkeys");

    const Setting& setting = SettingOf(Id::JapaneseMenuButtons);
    Check(std::string(setting.key) == "japanese_menu_buttons" &&
              Contains(Source("launcher/settings.h"), "kJapaneseMenuButtonsKey[] = \"japanese_menu_buttons\";"),
          "the key is the launcher's");
    Check(setting.apply == Apply::Restart && setting.choices.size() == 2, "it is a restart-required Off/On");
    Check(Current(DcpSettings::Values{}, Id::JapaneseMenuButtons) == "0", "it is Off by default");
    Check(Contains(Source("launcher/settings.cpp"), "{kJapaneseMenuButtonsKey, \"0\"}"), "the launcher's default is Off");

    const fs::path file = folder / "buttons.ini";
    WriteFile(file, "# keep\nfuture_setting = 1\n");
    DcpSettings::Load(file.string());
    DcpSettings::Values& live = DcpSettings::Get();
    live.japanese_menu_buttons = false;
    Options options(live);
    const Outcome outcome = options.Choose(Id::JapaneseMenuButtons, "1");
    Check(outcome.changed && outcome.saved && options.Pending(Id::JapaneseMenuButtons) && options.RestartPending(),
          "it waits for the restart");
    Check(!live.japanese_menu_buttons && options.Shown(Id::JapaneseMenuButtons) == "1", "and the running game keeps its value");

    const std::string text = ReadAll(file);
    Check(Contains(text, "japanese_menu_buttons = 1\n") && Contains(text, "# keep\n") && Contains(text, "future_setting = 1\n"),
          "it is saved under the launcher's key and the rest of the file is kept");
    DcpSettings::Values restarted;
    Check(DcpSettings::Read(file.string(), restarted) && restarted.japanese_menu_buttons && !restarted.skip_intro,
          "a restart reads what the menu saved and nothing else");

    DcpSettings::Values untouched;
    ApplyLive(untouched, Id::JapaneseMenuButtons, "1");
    Check(!untouched.japanese_menu_buttons, "nothing applies it live");
    Check(Contains(Source("gpu/shim/dcp_settings.cpp"), "key == \"japanese_menu_buttons\"") &&
              !Contains(Source("gpu/shim/dcp_options_menu.cpp"), "japanese_menu_buttons"),
          "it has one reader and one table row");
}

// Every key the launcher stores is an Options row or is named here as launcher-only.
void TestLauncherParity() {
    using namespace DcpOptions;
    const std::string launcher = Source("launcher/settings.h") + Source("launcher/online_settings.h");
    std::vector<std::string> launcher_keys;
    std::istringstream lines(launcher);
    for (std::string line; std::getline(lines, line);) {
        const size_t at = line.find("Key[] = \"");
        if (line.find("inline constexpr char k") == std::string::npos || at == std::string::npos) {
            continue;
        }
        const size_t begin = at + 9;
        launcher_keys.push_back(line.substr(begin, line.find('"', begin) - begin));
    }
    Check(launcher_keys.size() > 30, "the launcher's keys are read from its header");

    const std::vector<std::string> launcher_only = {
        "show_launcher",
        "window_width",
        "window_height",
        "monitor",
        "graphics_card",
        "text_language",
        "detailed_logging",
        "controller_prompts",
        "arachnophobia_safe_mode",
        "allow_overlay_programs",
        "launcher_x",
        "launcher_y",
        "launcher_width",
        "launcher_height",
        "launcher_maximized",
        "launcher_fullscreen",
        "online_enabled",
        "online_name",
        "online_udp_port",
        "online_stun",
        "online_connection_test_port",
        "show_ping",
        "show_in_lobby"};
    const auto has = [](const std::vector<std::string>& list, const std::string& key) {
        return std::find(list.begin(), list.end(), key) != list.end();
    };
    std::vector<std::string> row_keys;
    for (const Setting& setting : kSettings) {
        row_keys.push_back(setting.key);
    }
    for (const std::string& key : launcher_keys) {
        Check(has(row_keys, key) != has(launcher_only, key), ("the launcher key " + key + " is a row or launcher-only").c_str());
    }
    for (const std::string& key : row_keys) {
        Check(has(launcher_keys, key), ("the row key " + key + " is the launcher's").c_str());
    }
    for (const std::string& key : launcher_only) {
        Check(has(launcher_keys, key), ("the launcher-only key " + key + " is still the launcher's").c_str());
    }
}

void TestRendererRow(const fs::path& folder) {
    using namespace DcpOptions;
    const Setting& setting = SettingOf(Id::Renderer);
    Check(std::string(setting.key) == "renderer" && setting.page == Page::Graphics && setting.apply == Apply::Restart &&
              setting.feature == 0,
          "Renderer is a restart-required Graphics row on every build");
    const size_t renderers = DcpRenderer::Direct3D11Available ? 4 : 3;
    Check(setting.choices.size() == renderers && setting.choices[0].value == "auto" &&
              setting.choices[1].value == "vulkan" && setting.choices[2].value == "opengl",
          "Renderer offers Auto, Vulkan, OpenGL, and Direct3D 11 only on Windows");
    const std::string launcher = Source("launcher/settings.h");
    for (const Choice& choice : setting.choices) {
        Check(DcpRenderer::ChoiceKeys[size_t(DcpRenderer::ParseChoice(choice.value))] == choice.value,
              "every renderer choice is a value the video core parses");
        Check(Contains(launcher, "{\"" + std::string(choice.value) + "\","), "every renderer choice is the launcher's");
    }

    const fs::path file = folder / "renderer.ini";
    WriteFile(file, "# keep\nrenderer = vulkan\nfuture_setting = 1\n");
    DcpSettings::Load(file.string());
    DcpSettings::Values& live = DcpSettings::Get();
    Options options(live);
    Check(options.Shown(Id::Renderer) == "vulkan", "the menu shows the renderer the file names");

    const Outcome outcome = options.Choose(Id::Renderer, "opengl");
    Check(outcome.changed && outcome.saved && options.Pending(Id::Renderer) && options.RestartPending(),
          "choosing a renderer saves it and waits for the restart");
    Check(live.renderer == DcpRenderer::Vulkan && options.Shown(Id::Renderer) == "opengl",
          "the running renderer is left alone");
    const std::string text = ReadAll(file);
    Check(Contains(text, "renderer = opengl\n") && Contains(text, "# keep\n") && Contains(text, "future_setting = 1\n"),
          "it is saved under the launcher's key and the rest of the file is kept");
    DcpSettings::Values restarted;
    Check(DcpSettings::Read(file.string(), restarted) && restarted.renderer == DcpRenderer::OpenGL,
          "a restart reads what the menu saved");

    DcpSettings::Values untouched;
    ApplyLive(untouched, Id::Renderer, "opengl");
    Check(untouched.renderer == DcpRenderer::Vulkan, "nothing switches the renderer live");
#ifdef _WIN32
    Check(setting.choices[3].value == "d3d11" && setting.choices[3].label == "Direct3D 11 (experimental)",
          "Direct3D 11 is named in full");
    const Outcome direct3d = options.Choose(Id::Renderer, "d3d11");
    Check(direct3d.changed && direct3d.saved && options.RestartPending(), "Direct3D 11 waits for the restart too");
    Check(live.renderer == DcpRenderer::Vulkan, "and leaves the running renderer alone");
    DcpSettings::Values restarted_d3d11;
    Check(Contains(ReadAll(file), "renderer = d3d11\n") && DcpSettings::Read(file.string(), restarted_d3d11) &&
              restarted_d3d11.renderer == DcpRenderer::Direct3D11,
          "the same key holds it");
#endif
    options.Choose(Id::Renderer, "vulkan");
    Check(!options.Pending(Id::Renderer) && !options.RestartPending(), "choosing the running renderer clears the wait");
    const std::string menu = Source("gpu/shim/dcp_options_menu.cpp");
    Check(!Contains(menu, "renderer") && !Contains(menu, "Renderer"), "the menu code has no renderer switch of its own");
}

void TestRenderShapeRows(const fs::path& folder) {
    using namespace DcpOptions;
    const std::string resolution_source = Source("launcher/resolution.c");
    Check(SettingOf(Id::RenderResolution).choices.size() == DcpGraphics::RenderResolutionKeys.size(),
          "Render resolution offers the launcher's resolutions");
    size_t index = 0;
    for (const Choice& choice : SettingOf(Id::RenderResolution).choices) {
        Check(choice.value == DcpGraphics::RenderResolutionKeys[index++] &&
                  Contains(resolution_source, "{\"" + std::string(choice.value) + "\", \"" + std::string(choice.label) + "\","),
              "every render resolution choice is one the launcher lists, with its name");
    }
    Check(SettingOf(Id::AspectRatio).choices.size() == DcpGraphics::AspectRatios.size(),
          "Aspect Ratio offers the launcher's shapes");
    index = 0;
    for (const Choice& choice : SettingOf(Id::AspectRatio).choices) {
        const DcpGraphics::AspectRatio& aspect = DcpGraphics::AspectRatios[index++];
        Check(choice.value == aspect.key &&
                  Contains(resolution_source, "{\"" + std::string(choice.value) + "\", \"" + std::string(choice.label) + "\", " +
                                                  std::to_string(aspect.width) + ", " + std::to_string(aspect.height) + "}"),
              "every aspect ratio choice is a shape the launcher lists, with its name and size");
    }
    Check(Contains(Source("launcher/options.cpp"),
                   "(static_cast<long>(height) * aspect.width * 2 + aspect.height) / (2 * aspect.height)"),
          "the window width an aspect ratio saves is the launcher's formula");
    Check(DcpGraphics::WindowWidthFor(1080, DcpGraphics::ParseAspectRatio("16:9")) == 1920 &&
              DcpGraphics::WindowWidthFor(1080, DcpGraphics::ParseAspectRatio("21:9")) == 2520 &&
              DcpGraphics::WindowWidthFor(1080, DcpGraphics::ParseAspectRatio("32:9")) == 3840 &&
              DcpGraphics::WindowWidthFor(1080, DcpGraphics::ParseAspectRatio("16:10")) == 1728 &&
              DcpGraphics::WindowWidthFor(1080, DcpGraphics::ParseAspectRatio("4:3")) == 1440 &&
              DcpGraphics::WindowWidthFor(1080, DcpGraphics::ParseAspectRatio("5:4")) == 1350,
          "window widths follow the aspect ratio");
    Check(DcpGraphics::WiderThanOriginal(DcpGraphics::ParseAspectRatio("21:9")) &&
              DcpGraphics::WiderThanOriginal(DcpGraphics::ParseAspectRatio("32:9")) &&
              !DcpGraphics::WiderThanOriginal(DcpGraphics::ParseAspectRatio("16:9")) &&
              !DcpGraphics::WiderThanOriginal(DcpGraphics::ParseAspectRatio("16:10")) &&
              !DcpGraphics::WiderThanOriginal(DcpGraphics::ParseAspectRatio("4:3")),
          "True Ultrawide applies to the shapes wider than 16:9");
    Check(DcpGraphics::ParseRenderResolution("8K") == "native" && DcpGraphics::ParseAspectRatio("2:1").key == "16:9",
          "an unknown resolution or shape is the original one");

    const fs::path file = folder / "shape.ini";
    WriteFile(file, "# keep\nwindow_height = 1080\nwindow_width = 1920\nfuture_setting = 1\n");
    DcpSettings::Load(file.string());
    DcpSettings::Values& live = DcpSettings::Get();
    Options options(live);
    Check(options.Shown(Id::AspectRatio) == "16:9" && options.Shown(Id::RenderResolution) == "native" &&
              options.Shown(Id::TrueUltrawide) == "0" && options.Shown(Id::JapaneseNpcVoices) == "0",
          "the new rows start at the launcher's defaults");

    const Outcome outcome = options.Choose(Id::AspectRatio, "21:9");
    Check(outcome.changed && outcome.saved && options.Pending(Id::AspectRatio) && live.aspect_ratio == "16:9",
          "an aspect ratio waits for the restart");
    options.Choose(Id::RenderResolution, "1440p");
    options.Choose(Id::TrueUltrawide, "1");
    options.Choose(Id::JapaneseNpcVoices, "1");
    std::string text = ReadAll(file);
    Check(Contains(text, "aspect_ratio = 21:9\n") && Contains(text, "window_width = 2520\n") &&
              Contains(text, "render_resolution = 1440p\n") && Contains(text, "true_ultrawide = 1\n") &&
              Contains(text, "dcp_japanese_npc_voices = 1\n") && Contains(text, "# keep\n") &&
              Contains(text, "window_height = 1080\n") && Contains(text, "future_setting = 1\n"),
          "the choices are saved under the launcher's keys with the window width that follows the shape");
    DcpSettings::Values restarted;
    Check(DcpSettings::Read(file.string(), restarted) && restarted.aspect_ratio == "21:9" &&
              restarted.render_resolution == "1440p" && restarted.true_ultrawide && restarted.japanese_npc_voices &&
              restarted.requested_width == 2520,
          "a restart reads what the menu saved");
    Check(live.render_resolution == "native" && !live.true_ultrawide && !live.japanese_npc_voices,
          "the running session keeps its values");

    options.Choose(Id::AspectRatio, "16:9");
    text = ReadAll(file);
    Check(!options.Pending(Id::AspectRatio) && Contains(text, "aspect_ratio = 16:9\n") && Contains(text, "window_width = 1920\n"),
          "going back to the running shape clears the wait and restores the width");

    DcpSettings::Values untouched;
    for (const Id id : {Id::AspectRatio, Id::RenderResolution, Id::TrueUltrawide, Id::JapaneseNpcVoices}) {
        ApplyLive(untouched, id, id == Id::AspectRatio ? "21:9" : id == Id::RenderResolution ? "4K" : "1");
    }
    Check(untouched.aspect_ratio == "16:9" && untouched.render_resolution == "native" && !untouched.true_ultrawide &&
              !untouched.japanese_npc_voices,
          "nothing applies them live");
}

void TestBuildFeatureRows() {
    using namespace DcpOptions;
    DcpSettings::Values none;
    none.features = 0;
    DcpSettings::Values all;
    const Id gated[] = {Id::AspectRatio, Id::RenderResolution, Id::TrueUltrawide, Id::JapaneseNpcVoices, Id::UnlockGallery};
    for (const Setting& setting : kSettings) {
        const bool is_gated = std::find(std::begin(gated), std::end(gated), setting.id) != std::end(gated);
        Check(Available(setting, all), "a build with every feature offers every row");
        Check(Available(setting, none) == !is_gated, "a build with no feature offers only the rows every build has");
    }
    DcpSettings::Values resolution_only;
    resolution_only.features = DcpSettings::RenderResolution;
    Check(Available(SettingOf(Id::RenderResolution), resolution_only) && Available(SettingOf(Id::AspectRatio), resolution_only) &&
              !Available(SettingOf(Id::TrueUltrawide), resolution_only) &&
              !Available(SettingOf(Id::JapaneseNpcVoices), resolution_only),
          "each feature opens its own rows");

    const std::string loader = Source("src/loader.c");
    for (const char* feature : {"DCP_GPU_FEATURE_RENDER_RESOLUTION", "DCP_GPU_FEATURE_TRUE_ULTRAWIDE",
                                "DCP_GPU_FEATURE_JAPANESE_NPC_VOICES", "DCP_GPU_FEATURE_UNLOCK_GALLERY"}) {
        Check(Contains(loader, feature) && Contains(Source("src/dcpgpu.h"), feature), "the loader reports every feature");
    }
    Check(Contains(loader, "game_build.japanese_npc_voices && (!game_build.features_by_signature || dcp_npc_voice_resources(app0))"),
          "the voices row follows the launcher's rule for offering them");
    Check(Contains(Source("gpu/shim/dcpgpu.cpp"), "DcpSettings::Get().features = config->features;"),
          "the video core keeps what the loader reported");
    const std::string menu = Source("gpu/shim/dcp_options_menu.cpp");
    size_t offered = 0;
    for (size_t at = menu.find("&& Offered(setting)"); at != std::string::npos; at = menu.find("&& Offered(setting)", at + 1)) {
        ++offered;
    }
    Check(offered == 2, "both row lists leave out what the build does not support");
}

void TestEveryChoiceRoundTrips(const fs::path& folder) {
    using namespace DcpOptions;
    const fs::path file = folder / "round_trip.ini";
    WriteFile(file,
              "display_mode = windowed\nvsync = 1\npresentation = letterbox\nfsr1 = 0\nrcas = 1\nrcas_strength = 75\n"
              "smaa = 0\nanisotropy = 0\nframe_rate = 60\nshow_fps_title = 0\nshow_resolution_title = 0\nshow_fps = 0\n");
    DcpSettings::Load(file.string());
    Options options(DcpSettings::Get());
    for (const Setting& setting : kSettings) {
        for (const Choice& choice : setting.choices) {
            options.Choose(setting.id, choice.value);
            DcpSettings::Values read;
            DcpSettings::Read(file.string(), read);
            const std::string saved = Current(read, setting.id);
            if (saved != choice.value) {
                std::printf("%s: chose %s, read back %s\n", setting.key, std::string(choice.value).c_str(),
                            saved.c_str());
            }
            Check(saved == choice.value, "a saved choice reads back as the same choice");
            Check(ChoiceIndex(setting, saved) >= 0, "a saved value is one of the choices");
        }
    }
}

void TestStepAndCycle(const fs::path& folder) {
    using namespace DcpOptions;
    const fs::path file = folder / "step.ini";
    WriteFile(file, "frame_rate = 60\nrcas_strength = 75\nsmaa = 0\n");
    DcpSettings::Load(file.string());
    Options options(DcpSettings::Get());

    Check(options.Cycle(Id::AntiAliasing).changed && options.Shown(Id::AntiAliasing) == "1", "cycling flips a toggle on");
    Check(options.Cycle(Id::AntiAliasing).changed && options.Shown(Id::AntiAliasing) == "0", "and back off");
    Check(options.Step(Id::SharpeningStrength, 1).changed && options.Shown(Id::SharpeningStrength) == "80",
          "a step moves to the next value");
    Check(options.Step(Id::SharpeningStrength, -1).changed && options.Shown(Id::SharpeningStrength) == "75",
          "and back");
    options.Choose(Id::SharpeningStrength, "100");
    Check(!options.Step(Id::SharpeningStrength, 1).changed && options.Shown(Id::SharpeningStrength) == "100",
          "a step stops at the last value");
    options.Choose(Id::FrameRate, "0");
    Check(options.Cycle(Id::FrameRate).changed && options.Shown(Id::FrameRate) == "30", "cycling wraps to the first value");
    Check(options.Pending(Id::FrameRate) == true, "stepping a restart setting keeps it pending");

    DcpSettings::Get().rcas_strength = 77;
    Check(options.Step(Id::SharpeningStrength, 1).changed && options.Shown(Id::SharpeningStrength) == "0",
          "a value that is not a choice steps to the first choice");
}

void TestOnlinePlayLocksCheats() {
    using namespace dcp::cheats;
    Check(RowsUsable(true, false), "the cheat rows act with hooks and no Online Play");
    Check(!RowsUsable(true, true), "Online Play locks the rows");
    Check(!RowsUsable(false, false), "a build without hooks has no rows");

    runtime_cheats_set_online(1);
    runtime_cheats_set_available(1);
    Check(runtime_cheats_online_locked() == 1, "Online Play is on");
    Check(!RowsUsable(runtime_cheats_available() != 0, runtime_cheats_online_locked() != 0),
          "the Options page sees the rows locked");
    for (const MenuRow& row : kMenuRows) {
        if (row.kind == RowKind::Toggle) {
            runtime_cheat_toggle(int(row.cheat));
            Check(runtime_cheat_enabled(int(row.cheat)) == 0, "a toggle cannot turn a cheat on during Online Play");
        } else if (row.kind == RowKind::Action) {
            runtime_cheat_action(int(row.action));
            Check(runtime_cheat_action_pending(int(row.action)) == 0, "an action cannot be filed during Online Play");
        }
    }
    Check(runtime_cheat_movement_cycle() == kMovementSpeedOff && runtime_cheat_movement_speed() == kMovementSpeedOff,
          "Movement Speed stays Off during Online Play");
    Check(runtime_cheat_experience_cycle() == kExperienceMultiplierOff &&
              runtime_cheat_experience_multiplier() == kExperienceMultiplierOff,
          "Experience Multiplier stays Off during Online Play");

    runtime_cheats_set_online(0);
    runtime_cheats_start(0, kExperienceMultiplierOff, kMovementSpeedOff);
    Check(RowsUsable(true, runtime_cheats_online_locked() != 0), "the rows act again once Online Play is off");

    Check(StepActivates(RowKind::Toggle, 1, false) && !StepActivates(RowKind::Toggle, 1, true),
          "right turns a cheat on and leaves an On cheat alone");
    Check(StepActivates(RowKind::Toggle, -1, true) && !StepActivates(RowKind::Toggle, -1, false),
          "left turns a cheat off and leaves an Off cheat alone");
    Check(!StepActivates(RowKind::Toggle, 0, true), "no direction does nothing to a toggle");
    Check(StepActivates(RowKind::MovementSpeed, 1, false) && !StepActivates(RowKind::MovementSpeed, -1, false) &&
              StepActivates(RowKind::Experience, 1, false) && !StepActivates(RowKind::Experience, -1, false),
          "a multiplier moves on the right only");
    Check(!StepActivates(RowKind::Action, 1, false) && !StepActivates(RowKind::Action, -1, false),
          "an action runs on a press alone");
}

void TestSharedCheatState() {
    const std::string overlay = Source("gpu/shim/dcp_overlay.cpp");
    const std::string menu = Source("gpu/shim/dcp_options_menu.cpp") + Source("gpu/shim/dcp_options_menu.h") +
                             Source("gpu/shim/dcp_options_ui.h");
    Check(!Contains(menu, "runtime_cheat") && !Contains(menu, "SessionCheats"),
          "the Options menu holds no cheat state of its own");
    Check(Contains(overlay, "void ActivateCheatRow(const dcp::cheats::MenuRow& row)"),
          "one function runs a cheat row");
    Check(Contains(overlay, "ActivateCheatRow(dcp::cheats::kMenuRows[size_t(cheat_list.selected)]);") &&
              Contains(overlay, "ActivateCheatRow(dcp::cheats::kMenuRows[size_t(clicked)]);"),
          "the F10 menu runs its rows through it");
    Check(Contains(overlay, "ActivateCheatRow(row);\n    }\n}\n\nconst char* OptionsCheatNote"),
          "the Options menu runs its rows through it");
    Check(Contains(overlay, "CheatRowText CheatRowState(const dcp::cheats::MenuRow& row)"),
          "one function states a cheat row");
    Check(!Contains(Source("gpu/shim/dcp_settings.cpp"), "cheat"), "the settings file code knows no cheat");
    Check(!Contains(overlay, "\"Locked\""), "no cheat row says Locked");
    Check(Contains(overlay, "dcp::cheats::UnusableRowState(runtime_cheats_online_locked() != 0)"),
          "both menus take an unusable row's status from one function");
    Check(Contains(overlay, "bool OptionsOnlinePlay() {\n    return runtime_online_play_enabled() != 0;"),
          "the Options pages follow the shared Online Play state");
    Check(std::string(dcp::cheats::kOnlineLockoutText) == "Cheats are disabled while Online Play is enabled",
          "the F10 Online Play notice is unchanged");
}

void TestInputWiring() {
    Check(DcpOptions::kKey == SDL_SCANCODE_F1, "F1 opens the Options menu");
    const std::string overlay = Source("gpu/shim/dcp_overlay.cpp");
    Check(Contains(overlay, "if (event.key.scancode == DcpOptions::kKey) {"), "the window thread watches F1");
    Check(Contains(overlay, "DcpOptionsUi::TrackChord(event.gbutton.which, event.gbutton.button, down)"),
          "the window thread watches the controller chord");
    Check(Contains(overlay, "case SDL_EVENT_GAMEPAD_REMOVED:\n        DcpOptionsUi::ForgetPad("),
          "an unplugged controller drops its chord state");

    const std::string menu = Source("gpu/shim/dcp_options_menu.cpp");
    Check(Contains(menu, "ImGuiKey_GamepadFaceRight") && Contains(menu, "ImGuiKey_GamepadL1") &&
              Contains(menu, "ImGuiKey_GamepadR1") && Contains(menu, "ImGuiKey_GamepadFaceDown") &&
              Contains(menu, "ImGuiKey_GamepadDpadUp") && Contains(menu, "ImGuiKey_GamepadLStickUp"),
          "the controller moves, changes, pages and closes");
    Check(Contains(menu, "ImGuiKey_Q") && Contains(menu, "ImGuiKey_E") && Contains(menu, "ImGuiKey_Tab") &&
              Contains(menu, "ImGuiKey_Enter"),
          "the keyboard pages and changes");
    Check(Contains(menu, "ImGuiWindowFlags_NoNav"), "the menu moves through its rows itself");
    Check(!Contains(menu, "kPages"), "the menu pages only through the visible page list");

    const std::string pad = Source("src/runtime_pad.c");
    Check(Contains(pad, "SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_LEFT_STICK) && "
                        "SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_RIGHT_STICK)"),
          "the game never sees L3 and R3 together");
    Check(Contains(pad, "if (dcpgpu_overlay_captures_input()) return; /* settings menu open: neutral input */"),
          "an open menu still holds the game's pad neutral");
    Check(Contains(pad, "if (dcpgpu_overlay_captures_input()) { s->hold_after_capture=1; return; }"),
          "a button held as the menu closes stays hidden from the game until released");
}

void TestPageModel() {
    using DcpOptions::Page;
    const struct {
        bool online_play;
        std::vector<Page> order;
    } cases[] = {{false, {Page::Display, Page::Graphics, Page::Gameplay, Page::Controls, Page::Cheats}},
                 {true, {Page::Display, Page::Graphics, Page::Gameplay, Page::Online, Page::Controls}}};
    for (const auto& c : cases) {
        const std::string name = c.online_play ? "Online Play" : "offline";
        const DcpOptions::PageList list = DcpOptions::VisiblePages(c.online_play);
        const int count = int(list.count);
        bool same = list.count == c.order.size();
        for (size_t i = 0; same && i < list.count; ++i) {
            same = list.pages[i] == c.order[i];
        }
        Check(same && count == 5, (name + ": the pages and their order").c_str());
        Check((DcpOptions::PageIndex(list, Page::Cheats) >= 0) == !c.online_play &&
                  (DcpOptions::PageIndex(list, Page::Online) >= 0) == c.online_play,
              (name + ": Cheats and Online are never both on offer").c_str());
        Check(dcp::menu::WrapIndex(0, count, -1) == count - 1 && dcp::menu::WrapIndex(count - 1, count, 1) == 0,
              (name + ": paging wraps at both ends").c_str());
        int index = 0;
        bool visited[6] = {};
        for (int step = 0; step < count; ++step) {
            visited[int(list.pages[size_t(index)])] = true;
            index = dcp::menu::WrapIndex(index, count, 1);
        }
        bool every_page = index == 0;
        for (size_t i = 0; i < list.count; ++i) {
            every_page = every_page && visited[int(list.pages[i])];
        }
        Check(every_page, (name + ": a full lap visits each page once").c_str());
    }

    const DcpOptions::PageList offline = DcpOptions::VisiblePages(false);
    const DcpOptions::PageList online = DcpOptions::VisiblePages(true);
    const auto landed = [](const DcpOptions::PageList& list, Page selected) {
        return list.pages[size_t(DcpOptions::RecoverPage(list, selected))];
    };
    Check(landed(online, Page::Cheats) == Page::Controls, "Cheats selected when Online Play starts moves to Controls");
    Check(landed(offline, Page::Online) == Page::Controls, "Online selected when Online Play ends moves to Controls");
    bool kept = true;
    for (const Page page : {Page::Display, Page::Graphics, Page::Gameplay, Page::Controls}) {
        kept = kept && landed(online, page) == page && landed(offline, page) == page;
    }
    Check(kept, "a page that is still on offer stays selected");
    Check(landed(online, Page::Online) == Page::Online && landed(offline, Page::Cheats) == Page::Cheats,
          "the page that belongs to the state is kept");
}

// The text of the function that starts at `signature`, up to the next top-level closing brace.
std::string FunctionBody(const std::string& source, const std::string& signature) {
    const size_t start = source.find(signature);
    const size_t end = source.find("\n}\n", start);
    return start == std::string::npos || end == std::string::npos ? std::string() : source.substr(start, end - start);
}

void TestOnlinePageIsObservational() {
    const std::string matching = Source("src/runtime_matching2.cpp");
    const std::string snapshot = FunctionBody(matching, "PageLink page_link() {");
    Check(!snapshot.empty() && Contains(snapshot, "std::lock_guard<std::mutex> lock(g_mutex);"),
          "the page reads the port's state under its lock");
    for (const char* effect : {"request(", "Issue(", "pending[", "make_unique", "->start", "->stop", "poll(", "reset(",
                               "hello", "presence", "friends_fetch", "g.room =", "PushContextEvent", "g.calls"}) {
        Check(!Contains(snapshot, effect), (std::string("the page snapshot does not ") + effect).c_str());
    }

    const std::string runtime = Source("src/runtime_online.cpp");
    const std::string update = FunctionBody(runtime, "extern \"C\" void runtime_online_page_update(");
    Check(!update.empty() && Contains(update, "online::page_link()"), "the page update takes the snapshot");
    for (const char* effect : {"friends_fetch", "pump(", "LobbyLink", "presence_", "peer_socket", "runtime_online_configure"}) {
        Check(!Contains(update, effect), (std::string("the page update does not ") + effect).c_str());
    }
    for (const char* file : {"gpu/shim/dcp_options_online.h", "gpu/shim/dcp_options_menu.cpp", "src/runtime_online_page.h"}) {
        const std::string text = Source(file);
        for (const char* effect : {"LobbyLink", "friends_fetch", "presence_publish", "online_enabled", "online_name",
                                   "SaveEntries", "hello_body"}) {
            Check(!Contains(text, effect), (std::string(file) + " does not touch " + effect).c_str());
        }
    }

    // The only thread the page starts is the Connection test's, running the launcher's own test.
    Check(runtime.find("std::thread(") != std::string::npos && runtime.find("std::thread(", runtime.find("std::thread(") + 1) == std::string::npos &&
              Contains(runtime, "net::run_connection_test(config, identity, name, game)") && Contains(runtime, ".detach();") &&
              Contains(runtime, "test.running.exchange(true)"),
          "the Connection test is the one network action, on one worker thread, started once at a time");

    for (const DcpOptions::Setting& setting : DcpOptions::kSettings) {
        Check(!std::string(setting.key).starts_with("online"), "the Options menu has no Online setting");
    }
}

RuntimeOnlinePage SamplePage() {
    RuntimeOnlinePage page{};
    page.active = 1;
    page.link = RUNTIME_ONLINE_LINK_UP;
    std::strcpy(page.name, "Alice");
    std::strcpy(page.friend_code, "ABCD-EFGH");
    page.friends_known = 1;
    return page;
}

void TestOnlinePage() {
    using namespace DcpOptions;
    RuntimeOnlinePage page = SamplePage();

    std::vector<OnlineRow> rows = OnlineRowsOf(page);
    Check(rows.size() == 3 && rows[0].label == "Online name" && rows[0].value == "Alice" &&
              rows[1].label == "Your friend code" && rows[1].value == "ABCD-EFGH" && rows[2].label == "Connection test" &&
              rows[2].value == "Run test",
          "the page lists the name, the friend code and the Connection test");
    Check(rows[0].action == OnlineAction::None, "the Online name cannot be changed");
    Check(rows[1].action == OnlineAction::CopyCode && rows[2].action == OnlineAction::RunTest,
          "only the friend code copy and the Connection test act");

    page.test = RUNTIME_ONLINE_TEST_RUNNING;
    rows = OnlineRowsOf(page);
    Check(rows[2].value == "Testing..." && rows[2].action == OnlineAction::None,
          "a running test says so and cannot start twice");

    dcp::net::ConnectionReport report;
    report.ready = true;
    report.verdict = "Ready for online play";
    report.lines.push_back({dcp::net::ConnectionReport::Mark::kOk, "Online service", "Connected \xC2\xB7 41 ms"});
    report.lines.push_back({dcp::net::ConnectionReport::Mark::kProblem, "Direct connections", "May be blocked."});
    page.test = RUNTIME_ONLINE_TEST_DONE;
    dcp::online::FillReport(page, report);
    rows = OnlineRowsOf(page);
    Check(rows[2].value == "Ready for online play" && rows[2].lit && !rows[2].warn && rows[2].action == OnlineAction::RunTest,
          "the verdict of the launcher's report is the row's value");
    Check(Contains(rows[2].help, "OK Online service: Connected \xC2\xB7 41 ms") &&
              Contains(rows[2].help, "!! Direct connections: May be blocked."),
          "the report's lines keep the launcher's marks");
    report.ready = false;
    report.verdict = "Limited: some players may not be able to connect to you";
    dcp::online::FillReport(page, report);
    rows = OnlineRowsOf(page);
    Check(rows[2].warn && !rows[2].lit, "a limited verdict is flagged");

    const dcp::OnlinePlayer players[] = {
        {"1", "Bob", false, false, "", "", 0},
        {"2", "Cy", true, false, "Windows", "0.9.0", dcp::net::kOnlineProtocolVersion},
        {"3", "Di", true, true, "Linux", "0.9.0", dcp::net::kOnlineProtocolVersion},
        {"4", "Ed", true, true, "Windows", "0.1.0", dcp::net::kOnlineProtocolVersion + 7},
    };
    dcp::online::FillFriends(page, std::vector<dcp::OnlinePlayer>(std::begin(players), std::end(players)));
    rows = OnlineRowsOf(page);
    Check(rows.size() == 7, "each friend is a row after the test");
    for (size_t i = 0; i < 4 && rows.size() == 7; ++i) {
        const OnlineRow& row = rows[3 + i];
        const dcp::FriendStatus status = dcp::FriendStatusOf(players[i]);
        Check(row.label == players[i].name, "a friend row shows the friend's name");
        Check(row.value.rfind(dcp::FriendPresence(players[i]), 0) == 0 && status.text.rfind(dcp::FriendPresence(players[i]), 0) == 0,
              "a friend row starts with the launcher's presence label");
        Check(Contains(row.value, "Incompatible") == status.incompatible, "Incompatible is shown as the launcher shows it");
        Check(row.lit == players[i].online && row.action == OnlineAction::None, "a friend row is read only");
    }
    Check(rows[3].value == "Offline" && rows[4].value == "Online" && rows[5].value == "Online, in a room" &&
              rows[6].value == "Online, in a room  \xC2\xB7  Incompatible",
          "the labels are the launcher's Offline, Online, Online in a room and Incompatible");
    Check(rows[4].help == "Windows \xC2\xB7 v0.9.0", "platform and version follow in the help");

    page.friend_count = 0;
    page.friend_total = 0;
    Check(Contains(OnlineNote(page), "No friends yet.") && Contains(OnlineNote(page), "managed in the launcher"),
          "an empty friend list is explained and the launcher stays the place for friends");
    page.link = RUNTIME_ONLINE_LINK_UNREACHABLE;
    std::memset(page.friend_code, 0, sizeof(page.friend_code));
    rows = OnlineRowsOf(page);
    Check(rows[1].value == "Not available right now" && rows[1].action == OnlineAction::None,
          "no friend code to copy while the service is unreachable");
    page.link = RUNTIME_ONLINE_LINK_CONNECTING;
    Check(OnlineRowsOf(page)[1].value == "Connecting...", "a friend code still on its way says so");

    RuntimeOnlinePage inactive{};
    std::strcpy(inactive.name, "Zed");
    rows = OnlineRowsOf(inactive);
    Check(rows.size() == 1 && rows[0].action == OnlineAction::None, "a session without online play offers the name alone");
    Check(Contains(OnlineNote(inactive), "not active"), "and says online play is not active");

    page.link = RUNTIME_ONLINE_LINK_OFF;
    rows = OnlineRowsOf(page);
    Check(rows[1].value == "Not available right now" && rows[1].action == OnlineAction::None &&
              Contains(OnlineNote(page), "has not connected"),
          "a game that has not connected shows no friend code and says so");
    page.link = RUNTIME_ONLINE_LINK_UP;
    page.friends_known = 0;
    Check(Contains(OnlineNote(page), "once the game has loaded it"), "an unloaded friend list is explained");

    const std::string model = Source("gpu/shim/dcp_options_online.h");
    for (const char* write : {"SaveEntries", "friends.add", "friends.remove", "friends.accept", "friends.decline", "online_name"}) {
        Check(!Contains(model, write), (std::string("the Online page model has no write: ") + write).c_str());
    }
}

void CheckCheatsStayOff(const std::string& build) {
    using namespace dcp::cheats;
    for (const MenuRow& row : kMenuRows) {
        if (row.kind == RowKind::Toggle) {
            runtime_cheat_toggle(int(row.cheat));
            Check(runtime_cheat_enabled(int(row.cheat)) == 0, (build + ": a cheat stays Off").c_str());
        } else if (row.kind == RowKind::Action) {
            runtime_cheat_action(int(row.action));
            Check(runtime_cheat_action_pending(int(row.action)) == 0, (build + ": no action is filed").c_str());
        }
    }
    Check(runtime_cheat_movement_cycle() == kMovementSpeedOff && runtime_cheat_movement_speed() == kMovementSpeedOff,
          (build + ": Movement Speed stays Off").c_str());
    Check(runtime_cheat_experience_cycle() == kExperienceMultiplierOff &&
              runtime_cheat_experience_multiplier() == kExperienceMultiplierOff,
          (build + ": Experience Multiplier stays Off").c_str());
}

// The Options menu is host-side: the only per-build fact it meets is whether the build has the native cheat hooks, and
// that is asked of the same runtime functions the F10 menu uses.
void TestEveryBuild() {
    using namespace dcp::cheats;
    const std::span<const dcp::GameBuild> builds = dcp::SupportedBuilds();
    Check(builds.size() == 8, "the Options menu is checked against all 8 supported builds");
    const unsigned every_cheat = (1u << kCheatCount) - 1;
    for (const dcp::GameBuild& build : builds) {
        for (int online = 0; online <= 1; ++online) {
            const std::string name = std::string(build.label) + (online ? ", Online Play" : "");
            runtime_cheats_set_available(0);
            runtime_cheats_set_online(online);
            runtime_cheats_start(every_cheat, 10, 4);

            const int hooks = runtime_cheats_hooks_wanted(build.cheats);
            Check(hooks == (build.cheats && !online), (name + ": the hooks follow the build and Online Play").c_str());
            runtime_cheats_set_available(hooks);
            Check(runtime_cheats_online_locked() == online, (name + ": the lock follows Online Play alone").c_str());
            Check(RowsUsable(runtime_cheats_available() != 0, runtime_cheats_online_locked() != 0) ==
                      (build.cheats && !online),
                  (name + ": the Cheats page acts exactly when F10 would").c_str());
            const DcpOptions::PageList pages = DcpOptions::VisiblePages(runtime_cheats_online_locked() != 0);
            Check(pages.count == 5 && (DcpOptions::PageIndex(pages, DcpOptions::Page::Online) >= 0) == (online != 0) &&
                      (DcpOptions::PageIndex(pages, DcpOptions::Page::Cheats) >= 0) == (online == 0),
                  (name + ": Online replaces Cheats exactly when Online Play is on").c_str());
            const bool locked = runtime_cheats_online_locked() != 0;
            const char* status = UnusableRowState(locked);
            if (online) {
                Check(status == nullptr, (name + ": Online Play shows no per-row status").c_str());
            } else if (!build.cheats) {
                Check(status && std::string(status) == "Unavailable", (name + ": no hooks still reads Unavailable").c_str());
            }
            const int expected = online ? RUNTIME_CHEATS_ONLINE_LOCKED
                                 : build.cheats ? RUNTIME_CHEATS_LISTED
                                                : RUNTIME_CHEATS_UNAVAILABLE;
            Check(runtime_cheats_log_state(build.cheats) == expected, (name + ": the log states the same support").c_str());
            if (online) {
                for (const CheatInfo& info : kCheats) {
                    Check(runtime_cheat_enabled(int(info.cheat)) == 0, (name + ": no saved cheat survives").c_str());
                }
                CheckCheatsStayOff(name);
            }
        }
    }
    runtime_cheats_set_online(0);
    runtime_cheats_set_available(0);
    runtime_cheats_start(0, kExperienceMultiplierOff, kMovementSpeedOff);
}

// No file of the Options menu's path names a build, so none can leave a supported build out.
void TestEveryRowIsTranslated() {
    std::vector<std::string> english;
    for (const dcp::localization::UiTextRow& row : dcp::localization::PortugueseBrazilUiText().rows) {
        english.push_back(row.english);
    }
    const auto translated = [&](const char* text) { return std::find(english.begin(), english.end(), text) != english.end(); };
    for (const DcpOptions::Setting& setting : DcpOptions::kSettings) {
        Check(translated(setting.label) && translated(setting.help), "every Options row has Portuguese (Brazil) text");
    }
    for (const DcpOptions::Page page : {DcpOptions::Page::Display, DcpOptions::Page::Graphics, DcpOptions::Page::Gameplay,
                                        DcpOptions::Page::Online, DcpOptions::Page::Controls}) {
        Check(translated(DcpOptions::PageName(page)) && translated(DcpOptions::PageNote(page)),
              "every Options page has Portuguese (Brazil) text");
    }
}

void TestConnectionLinesAreTranslated() {
    const std::string lines = "\nOK Online service: Connected \xC2\xB7 32 ms"
                              "\n!! Direct connections: May be blocked by your network or firewall on UDP port 3658.";
    Check(DcpOptions::TranslateTestLines(lines) == lines, "English keeps the Connection test lines");
    dcp::localization::SetActiveLanguage(dcp::kPortugueseBrazilKey);
    Check(DcpOptions::TranslateTestLines(lines) ==
              "\nOK Serviço online: Conectado \xC2\xB7 32 ms"
              "\n!! Conexões diretas: Pode estar bloqueado pela sua rede ou firewall na porta UDP 3658.",
          "the Connection test lines keep their marks and values in Portuguese (Brazil)");
    dcp::localization::SetActiveLanguage("english");
}

void TestNoBuildIdentity() {
    const char* const files[] = {"gpu/shim/dcp_options_menu.h", "gpu/shim/dcp_options_menu.cpp",
                                 "gpu/shim/dcp_options_ui.h",   "gpu/shim/dcp_options_online.h",
                                 "src/runtime_online_page.h",   "src/runtime_online_overlay.h",
                                 "gpu/shim/dcp_overlay.cpp",
                                 "gpu/shim/dcp_settings.h",     "gpu/shim/dcp_settings.cpp",
                                 "src/dcp_menu_input.h",        "src/dcp_cheat_menu.h",
                                 "src/runtime_pad.c",           "src/runtime_cheats.h",
                                 "src/runtime_cheats.cpp"};
    const char* const tokens[] = {"CUSA", "GameBuild", "game_build", "SupportedBuilds", "DCP_GAME_BUILD", "eboot"};
    for (const char* file : files) {
        const std::string text = Source(file);
        Check(!text.empty(), (std::string(file) + " is read").c_str());
        for (const char* token : tokens) {
            Check(!Contains(text, token), (std::string(file) + " does not name a build: " + token).c_str());
        }
    }
}

} // namespace

int main() {
    TestChord();
    TestToggleAndMutualExclusion();
    TestCloseGate();
    TestStepping();
    TestSettingsTable();
    TestEveryRowIsTranslated();
    TestConnectionLinesAreTranslated();
    const fs::path folder = MakeTemporaryFolder();
    TestPersistence(folder);
    TestSkipOffers(folder);
    TestSkipIntro(folder);
    TestUnlockGallery(folder);
    TestJapaneseMenuButtons(folder);
    TestControllerRumble(folder);
    TestLauncherParity();
    TestRendererRow(folder);
    TestRenderShapeRows(folder);
    TestBuildFeatureRows();
    TestEveryChoiceRoundTrips(folder);
    TestStepAndCycle(folder);
    TestOnlinePlayLocksCheats();
    TestSharedCheatState();
    TestInputWiring();
    TestPageModel();
    TestOnlinePage();
    TestOnlinePageIsObservational();
    TestEveryBuild();
    TestNoBuildIdentity();
    fs::remove_all(folder);
    if (failures) {
        std::printf("OPTIONS MENU: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("OPTIONS MENU OK: 0 failure(s)\n");
    return 0;
}
