#pragma once

#include <imgui.h>

#include <algorithm>

#include "localization/ui_text.h"
#include "trg/launcher.h"

namespace dcp {

// The centered text and spinner a page shows while its first content is being prepared.
inline void DrawLoadingView(trg::Ui& ui, const char* english_text) {
    const float scale = ui.scale();
    const char* text = Tr(english_text);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 text_size = ImGui::CalcTextSize(text);
    const float radius = 10.0f * scale;
    const float gap = 14.0f * scale;
    const float block = text_size.y + gap + radius * 2.0f;
    const ImVec2 center(min.x + avail.x * 0.5f, min.y + std::max(0.0f, (avail.y - block) * 0.5f));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddText(ImVec2(center.x - text_size.x * 0.5f, center.y), ImGui::GetColorU32(ui.theme().text), text);
    const ImVec2 spinner(center.x, center.y + text_size.y + gap + radius);
    const float angle = float(ImGui::GetTime()) * 5.0f;
    draw->PathArcTo(spinner, radius, angle, angle + 4.2f, 24);
    draw->PathStroke(ImGui::GetColorU32(ui.theme().accent), 0, 3.0f * scale);
}

} // namespace dcp
