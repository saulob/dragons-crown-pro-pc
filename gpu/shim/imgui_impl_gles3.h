// imgui's OpenGL 3 backend compiled a second time for OpenGL ES 3: which of the two it is is fixed at compile time.
#pragma once

struct ImDrawData;

bool InitImGuiGles3();
void ImGuiGles3NewFrame();
void ImGuiGles3RenderDrawData(ImDrawData* draw_data);
