#pragma once

#include "imgui.h"
#include "imgui_internal.h"

namespace dcp {

// Keeps a page's content a margin short of the page's scrollbar. ImGui rebuilds these rectangles every frame, so nothing has
// to be restored.
inline void KeepClearOfScrollbar(float margin) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    window->WorkRect.Max.x -= margin;
    window->ContentRegionRect.Max.x -= margin;
}

} // namespace dcp
