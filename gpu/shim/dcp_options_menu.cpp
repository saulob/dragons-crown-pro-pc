// SPDX-License-Identifier: GPL-2.0-or-later
#include "dcp_options_ui.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include "dcp_accent.h"
#include "dcp_options_menu.h"
#include "dcp_options_online.h"
#include "dcp_ping.h"
#include "imgui.h"

namespace DcpOptionsUi {

namespace {

using DcpOptions::Page;

constexpr uint32_t kChordButtons = (1u << SDL_GAMEPAD_BUTTON_LEFT_STICK) | (1u << SDL_GAMEPAD_BUTTON_RIGHT_STICK);

// The menu's text height as a share of the window height, so it reads the same from the couch at any size.
constexpr float kFontShareOfHeight = 1.0f / 46.0f;
constexpr float kWidthEm = 40.0f;
constexpr int kMinVisibleRows = 4;
// The footer reserves a line each for the restart banner and the status message, plus the hints line.
constexpr float kFooterOptionalLines = 2.0f;
constexpr char kArrowLeft[] = "\xE2\x80\xB9";
constexpr char kArrowRight[] = "\xE2\x80\xBA";

const ImVec4 kPanel(0.03f, 0.04f, 0.09f, 0.94f);
const ImVec4 kDim(0.64f, 0.66f, 0.76f, 1.0f);
const ImVec4 kShade(0.0f, 0.0f, 0.0f, 0.55f);
const ImVec4 kWarn(0.95f, 0.72f, 0.30f, 1.0f);

struct PadChord {
    uint32_t buttons = 0;
    dcp::menu::Chord chord{kChordButtons};
};
std::map<uint32_t, PadChord> pad_chords;

enum class Kind { Setting, Cheat, Info, Online };

struct RowRef {
    Kind kind;
    size_t index;
};

struct Info {
    std::string label;
    std::string value;
};

struct State {
    Page page = Page::Display;
    int row = 0;
    int first = 0;
    bool focused = false;
    std::string status;
};
State state;
RuntimeOnlinePage online_page;
std::vector<DcpOptions::OnlineRow> online_rows;

DcpOptions::Options& Model() {
    static DcpOptions::Options model{DcpSettings::Get()};
    return model;
}

std::string KeyName(int scancode) {
    const char* name = SDL_GetScancodeName(SDL_Scancode(scancode));
    return name && *name ? name : "-";
}

std::vector<Info> Hotkeys() {
    const auto& settings = DcpSettings::Get();
    return {{"Options menu", KeyName(DcpOptions::kKey) + "  /  L3 + R3"},
            {"Cheats menu", KeyName(settings.menu_key)},
            {"Display mode", KeyName(settings.window_mode_key)},
            {"Frame counter", KeyName(DcpPing::kFpsKey)},
            {"Ping (Online Play)", KeyName(DcpPing::kPingKey)}};
}

std::vector<RowRef> RowsOf(Page page, const CheatRows& cheats) {
    std::vector<RowRef> rows;
    if (page == Page::Controls) {
        const size_t hotkeys = Hotkeys().size();
        for (size_t i = 0; i < hotkeys; ++i) {
            rows.push_back({Kind::Info, i});
        }
    } else if (page == Page::Cheats) {
        for (size_t i = 0; i < cheats.count; ++i) {
            rows.push_back({Kind::Cheat, i});
        }
    } else if (page == Page::Online) {
        for (size_t i = 0; i < online_rows.size(); ++i) {
            rows.push_back({Kind::Online, i});
        }
    } else {
        for (const DcpOptions::Setting& setting : DcpOptions::kSettings) {
            if (setting.page == page) {
                rows.push_back({Kind::Setting, size_t(setting.id)});
            }
        }
    }
    return rows;
}

std::string Label(const RowRef& row, const CheatRows& cheats) {
    switch (row.kind) {
    case Kind::Setting: return DcpOptions::kSettings[row.index].label;
    case Kind::Cheat: return cheats.view(row.index).label;
    case Kind::Info: return Hotkeys()[row.index].label;
    case Kind::Online: return online_rows[row.index].label;
    }
    return {};
}

std::string Help(const RowRef& row, const CheatRows& cheats) {
    switch (row.kind) {
    case Kind::Setting: {
        const DcpOptions::Setting& setting = DcpOptions::kSettings[row.index];
        return setting.apply == DcpOptions::Apply::Restart ? std::string(setting.help) + " Applies after a restart."
                                                           : std::string(setting.help);
    }
    case Kind::Cheat: return cheats.view(row.index).help;
    case Kind::Info: return {};
    case Kind::Online: return online_rows[row.index].help;
    }
    return {};
}

std::string NoteOf(Page page, const CheatRows& cheats) {
    if (page == Page::Cheats) {
        if (const char* unusable = cheats.unusable_note()) {
            return unusable;
        }
    }
    return page == Page::Online ? DcpOptions::OnlineNote(online_page) : std::string(DcpOptions::PageNote(page));
}

float RowHeight(float em) {
    return ImGui::GetFrameHeight() + em * 0.3f;
}

float WrappedHeight(const std::string& text, float wrap_width) {
    return text.empty() ? 0.0f : ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap_width).y;
}

void Change(const RowRef& row, int direction, const CheatRows& cheats) {
    switch (row.kind) {
    case Kind::Setting: {
        const DcpOptions::Id id = DcpOptions::Id(row.index);
        const DcpOptions::Outcome outcome = direction == 0 ? Model().Cycle(id) : Model().Step(id, direction);
        state.status = outcome.changed && !outcome.saved ? "Could not save the settings." : std::string();
        break;
    }
    case Kind::Cheat: cheats.activate(row.index, direction); break;
    case Kind::Info: break;
    case Kind::Online:
        if (direction != 0) {
            break;
        }
        switch (online_rows[row.index].action) {
        case DcpOptions::OnlineAction::CopyCode:
            SDL_SetClipboardText(online_page.friend_code);
            state.status = "Friend code copied.";
            break;
        case DcpOptions::OnlineAction::RunTest: runtime_online_page_run_test(); break;
        case DcpOptions::OnlineAction::None: break;
        }
        break;
    }
}

void SelectPage(Page page) {
    state.page = page;
    state.row = 0;
    state.first = 0;
}

// Returns true when the menu should close.
bool HandleInput(const CheatRows& cheats, const DcpOptions::PageList& pages, int page_index, int row_count) {
    const auto pressed = [](ImGuiKey key, bool repeat = false) { return ImGui::IsKeyPressed(key, repeat); };
    if (pressed(ImGuiKey_GamepadFaceRight)) {
        return true;
    }
    if (pressed(ImGuiKey_GamepadL1) || pressed(ImGuiKey_Q)) {
        SelectPage(pages.pages[size_t(dcp::menu::WrapIndex(page_index, int(pages.count), -1))]);
        return false;
    }
    if (pressed(ImGuiKey_GamepadR1) || pressed(ImGuiKey_E) || pressed(ImGuiKey_Tab)) {
        SelectPage(pages.pages[size_t(dcp::menu::WrapIndex(page_index, int(pages.count), 1))]);
        return false;
    }
    if (pressed(ImGuiKey_GamepadDpadUp, true) || pressed(ImGuiKey_GamepadLStickUp, true) || pressed(ImGuiKey_UpArrow, true)) {
        state.row = dcp::menu::WrapIndex(state.row, row_count, -1);
    }
    if (pressed(ImGuiKey_GamepadDpadDown, true) || pressed(ImGuiKey_GamepadLStickDown, true) ||
        pressed(ImGuiKey_DownArrow, true)) {
        state.row = dcp::menu::WrapIndex(state.row, row_count, 1);
    }
    const std::vector<RowRef> rows = RowsOf(state.page, cheats);
    if (rows.empty()) {
        return false;
    }
    const RowRef& row = rows[size_t(std::clamp(state.row, 0, int(rows.size()) - 1))];
    if (pressed(ImGuiKey_GamepadDpadLeft, true) || pressed(ImGuiKey_GamepadLStickLeft, true) ||
        pressed(ImGuiKey_LeftArrow, true)) {
        Change(row, -1, cheats);
    }
    if (pressed(ImGuiKey_GamepadDpadRight, true) || pressed(ImGuiKey_GamepadLStickRight, true) ||
        pressed(ImGuiKey_RightArrow, true)) {
        Change(row, 1, cheats);
    }
    if (pressed(ImGuiKey_GamepadFaceDown) || pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter) ||
        pressed(ImGuiKey_Space)) {
        Change(row, 0, cheats);
    }
    return false;
}

void DrawTabs(const DcpOptions::PageList& pages, int page_index, const ImVec4& accent, float em) {
    ImGui::TextColored(kDim, "%s[LB]", kArrowLeft);
    for (size_t i = 0; i < pages.count; ++i) {
        ImGui::SameLine(0.0f, em * 0.9f);
        const bool selected = int(i) == page_index;
        const char* name = DcpOptions::PageName(pages.pages[i]);
        ImGui::PushStyleColor(ImGuiCol_Text, selected ? accent : kDim);
        ImGui::PushID(int(i));
        if (ImGui::Selectable(name, false, ImGuiSelectableFlags_None, ImGui::CalcTextSize(name)) && !selected) {
            SelectPage(pages.pages[i]);
        }
        if (selected) {
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x, max.y + 2.0f), ImVec2(max.x, max.y + 2.0f),
                                                ImGui::GetColorU32(accent), 2.0f);
        }
        ImGui::PopID();
        ImGui::PopStyleColor();
    }
    ImGui::SameLine(0.0f, em * 0.9f);
    ImGui::TextColored(kDim, "[RB]%s", kArrowRight);
}

void DrawRows(const std::vector<RowRef>& rows, int visible, const CheatRows& cheats, const ImVec4& accent,
              const ImVec4& focus, float em) {
    const float height = RowHeight(em);
    const float pad = em * 0.5f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const int last = std::min(state.first + visible, int(rows.size()));
    for (int i = state.first; i < last; ++i) {
        const RowRef& row = rows[size_t(i)];
        ImGui::PushID(i);
        const bool clicked = ImGui::Selectable("##row", false, ImGuiSelectableFlags_None, ImVec2(0.0f, height));
        if (ImGui::IsItemHovered() && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
            state.row = i;
        }
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const bool selected = i == state.row;
        if (selected) {
            draw->AddRectFilled(min, max, ImGui::GetColorU32(focus), 3.0f);
        }
        const float y = min.y + (height - ImGui::GetFontSize()) * 0.5f;
        std::string value;
        bool lit = selected;
        bool warn = false;
        bool steppable = false;
        const std::string label = Label(row, cheats);
        draw->AddText(ImVec2(min.x + pad, y), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
        switch (row.kind) {
        case Kind::Setting: {
            const DcpOptions::Setting& setting = DcpOptions::kSettings[row.index];
            const std::string shown = Model().Shown(DcpOptions::Id(row.index));
            const int index = DcpOptions::ChoiceIndex(setting, shown);
            value = index >= 0 ? std::string(kArrowLeft) + "  " + std::string(setting.choices[size_t(index)].label) +
                                     "  " + kArrowRight
                               : shown;
            steppable = true;
            if (setting.apply == DcpOptions::Apply::Restart) {
                const float label_w = ImGui::CalcTextSize(label.c_str()).x;
                draw->AddText(ImVec2(min.x + pad + label_w + em * 0.5f, y), ImGui::GetColorU32(kDim), "(restart)");
            }
            break;
        }
        case Kind::Cheat: {
            const CheatRowView view = cheats.view(row.index);
            value = view.state ? view.state : "";
            lit = view.lit;
            break;
        }
        case Kind::Info: value = Hotkeys()[row.index].value; break;
        case Kind::Online: {
            const DcpOptions::OnlineRow& online = online_rows[row.index];
            value = online.value;
            lit = online.lit;
            warn = online.warn;
            break;
        }
        }
        const float value_w = ImGui::CalcTextSize(value.c_str()).x;
        draw->AddText(ImVec2(max.x - pad - value_w, y), ImGui::GetColorU32(warn ? kWarn : lit ? accent : kDim),
                      value.c_str());
        ImGui::PopID();
        if (clicked) {
            state.row = i;
            const float x = io.MousePos.x;
            const int direction = !steppable ? 0
                                  : x >= max.x - pad - value_w && x < max.x - pad - value_w * 0.5f ? -1
                                  : x >= max.x - pad - value_w * 0.5f                                ? 1
                                                                                                     : 0;
            Change(row, direction, cheats);
        }
    }
    if (int(rows.size()) > visible) {
        const float spacing = ImGui::GetStyle().ItemSpacing.y;
        const float total = float(visible) * height + float(visible - 1) * spacing;
        const float x = origin.x + width + em * 0.3f;
        const float thumb = total * float(visible) / float(rows.size());
        const float offset = (total - thumb) * float(state.first) / float(int(rows.size()) - visible);
        draw->AddRectFilled(ImVec2(x, origin.y), ImVec2(x + 3.0f, origin.y + total), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.12f)));
        draw->AddRectFilled(ImVec2(x, origin.y + offset), ImVec2(x + 3.0f, origin.y + offset + thumb),
                            ImGui::GetColorU32(accent));
    }
}

// Draws into the space reserved for `capacity` and pads out the rest, so what follows starts at the same Y whatever
// the page or the row is.
void ReserveSpace(float capacity, float used) {
    if (capacity > used) {
        ImGui::Dummy(ImVec2(0.0f, capacity - used));
    }
}

void DrawWrapped(const std::string& text, float capacity) {
    const float start_y = ImGui::GetCursorPosY();
    if (!text.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX());
        ImGui::TextColored(kDim, "%s", text.c_str());
        ImGui::PopTextWrapPos();
    }
    ReserveSpace(capacity, ImGui::GetCursorPosY() - start_y);
}

void DrawFooter(const ImVec4& accent, float line) {
    const float start_y = ImGui::GetCursorPosY();
    if (Model().RestartPending()) {
        ImGui::TextColored(accent, "%s", DcpOptions::kRestartNote);
    }
    if (!state.status.empty()) {
        ImGui::TextColored(accent, "%s", state.status.c_str());
    }
    ReserveSpace(line * kFooterOptionalLines, ImGui::GetCursorPosY() - start_y);
    const std::string hints = std::string("D-pad / Arrows  Select     ") + kArrowLeft + " " + kArrowRight +
                              " / A  Change     [LB] [RB] / Q E  Page     B / Esc  Back";
    ImGui::TextColored(kDim, "%s", hints.c_str());
}

void Draw(const CheatRows& cheats, const DcpOptions::PageList& pages, int page_index, float em, float scales) {
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec4 accent(dcp::kAccentRed / 255.0f, dcp::kAccentGreen / 255.0f, dcp::kAccentBlue / 255.0f, 1.0f);
    const ImVec4 focus(accent.x, accent.y, accent.z, 0.38f);

    const float width = std::min(em * kWidthEm, io.DisplaySize.x - em * 2.0f);
    const float wrap_width = width - em * 2.0f;
    const Page page = state.page;

    float note_capacity = 0.0f, help_capacity = 0.0f;
    size_t max_rows = 0;
    for (size_t i = 0; i < pages.count; ++i) {
        const Page candidate = pages.pages[i];
        const std::vector<RowRef> rows = RowsOf(candidate, cheats);
        max_rows = std::max(max_rows, rows.size());
        note_capacity = std::max(note_capacity, WrappedHeight(NoteOf(candidate, cheats), wrap_width));
        for (const RowRef& row : rows) {
            help_capacity = std::max(help_capacity, WrappedHeight(Help(row, cheats), wrap_width));
        }
    }
    note_capacity += em * 0.2f;
    help_capacity += em * 0.2f;

    ImGui::PushFont(nullptr, em * 1.2f / scales);
    const float title_height = ImGui::GetTextLineHeightWithSpacing();
    ImGui::PopFont();
    const float line = ImGui::GetTextLineHeightWithSpacing();
    const float separator = style.ItemSpacing.y * 2.0f + 1.0f;
    const float row_height = RowHeight(em);
    const float chrome = em * 0.8f * 2.0f + title_height + line + separator + note_capacity + separator +
                         help_capacity + line * (kFooterOptionalLines + 1.0f);
    const int fit = int((io.DisplaySize.y - em * 2.0f - chrome) / (row_height + style.ItemSpacing.y));
    const int visible = std::clamp(fit, kMinVisibleRows, int(max_rows));
    const float height = chrome + float(visible) * row_height + style.ItemSpacing.y * float(visible - 1);

    const std::vector<RowRef> rows = RowsOf(page, cheats);
    state.row = std::clamp(state.row, 0, std::max(int(rows.size()) - 1, 0));
    state.first = dcp::menu::ScrollToShow(state.row, state.first, visible, int(rows.size()));

    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, ImGui::GetColorU32(kShade));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    if (!state.focused) {
        state.focused = true;
        ImGui::SetNextWindowFocus();
    }
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kPanel);
    ImGui::PushStyleColor(ImGuiCol_Border, accent);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, focus);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, focus);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.0f, em * 0.8f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##dcp_options_menu", nullptr, flags)) {
        ImGui::PushFont(nullptr, em * 1.2f / scales);
        ImGui::TextColored(accent, "%s", DcpOptions::kTitle);
        ImGui::PopFont();
        DrawTabs(pages, page_index, accent, em);
        ImGui::Separator();
        DrawRows(rows, visible, cheats, accent, focus, em);
        if (visible > int(rows.size())) {
            ImGui::Dummy(ImVec2(0.0f, float(visible - int(rows.size())) * (row_height + style.ItemSpacing.y)));
        }
        DrawWrapped(NoteOf(page, cheats), note_capacity);
        ImGui::Separator();
        const std::string help = rows.empty() ? std::string() : Help(rows[size_t(state.row)], cheats);
        DrawWrapped(help, help_capacity);
        DrawFooter(accent, line);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
}

} // namespace

void Reset() {
    state = {};
    online_page = {};
    online_rows = DcpOptions::OnlineRowsOf(online_page);
}

Result Frame(const CheatRows& cheats) {
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float scales = std::max(style.FontScaleMain * style.FontScaleDpi, 0.01f);
    const float em = std::max(ImGui::GetFontSize(), io.DisplaySize.y * kFontShareOfHeight);
    const DcpOptions::PageList pages = DcpOptions::VisiblePages(cheats.online_play());
    const int recovered = DcpOptions::RecoverPage(pages, state.page);
    if (pages.pages[size_t(recovered)] != state.page) {
        SelectPage(pages.pages[size_t(recovered)]);
    }
    const int row_count = int(RowsOf(state.page, cheats).size());
    if (HandleInput(cheats, pages, recovered, row_count)) {
        return Result::Close;
    }
    if (state.page == Page::Online) {
        runtime_online_page_update(&online_page);
        online_rows = DcpOptions::OnlineRowsOf(online_page);
    }
    ImGui::PushFont(nullptr, em / scales);
    Draw(cheats, pages, DcpOptions::PageIndex(pages, state.page), em, scales);
    ImGui::PopFont();
    return Result::Stay;
}

bool TrackChord(uint32_t pad, int sdl_button, bool down) {
    if (sdl_button < 0 || sdl_button >= 32) {
        return false;
    }
    PadChord& chord = pad_chords[pad];
    const uint32_t bit = 1u << sdl_button;
    chord.buttons = down ? chord.buttons | bit : chord.buttons & ~bit;
    return chord.chord.Poll(chord.buttons);
}

void ForgetPad(uint32_t pad) {
    pad_chords.erase(pad);
}

} // namespace DcpOptionsUi
