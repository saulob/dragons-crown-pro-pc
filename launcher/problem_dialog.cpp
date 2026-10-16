#include "problem_dialog.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include <imgui.h>

namespace dcp {
namespace {

constexpr char kTitle[] = "Graphics could not start";

} // namespace

void ProblemDialog::Show(std::string cause, std::string advice) {
    cause_ = std::move(cause);
    advice_ = std::move(advice);
    open_ = true;
    opened_ = false;
}

void ProblemDialog::Draw(const trg::Fonts& fonts, const trg::Theme& theme) {
    if (!open_) {
        return;
    }
    if (!opened_) {
        ImGui::OpenPopup(kTitle);
        opened_ = true;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(viewport->Size.x * 0.92f, fonts.size * 34.0f), 0.0f), ImGuiCond_Always);
    ImGui::PushFont(fonts.regular, fonts.size);
    const std::pair<ImGuiCol, ImVec4> colors[] = {
        {ImGuiCol_PopupBg, theme.popup},     {ImGuiCol_Border, theme.warn_dot},
        {ImGuiCol_TitleBg, theme.panel},     {ImGuiCol_TitleBgActive, theme.panel},
        {ImGuiCol_Text, theme.text},         {ImGuiCol_Button, theme.accent},
        {ImGuiCol_ButtonHovered, theme.accent_hot}, {ImGuiCol_ButtonActive, theme.accent},
        {ImGuiCol_NavCursor, theme.text}};
    for (const auto& [slot, color] : colors) {
        ImGui::PushStyleColor(slot, color);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fonts.size, fonts.size));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::BeginPopupModal(kTitle, nullptr, kFlags)) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted("Dragon's Crown Pro PC could not start the graphics system.");
        ImGui::Spacing();
        ImGui::TextUnformatted(cause_.c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted(advice_.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0.0f, fonts.size * 0.5f));
        const bool key = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                         ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false) ||
                         ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
        ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetWindowWidth() - fonts.size * 6.0f) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, theme.on_accent);
        const bool clicked = ImGui::Button("OK", ImVec2(fonts.size * 6.0f, 0.0f));
        ImGui::PopStyleColor();
        if (clicked || key) {
            ImGui::CloseCurrentPopup();
            open_ = false;
        }
        ImGui::EndPopup();
    } else {
        open_ = false;
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(static_cast<int>(std::size(colors)));
    ImGui::PopFont();
}

} // namespace dcp
