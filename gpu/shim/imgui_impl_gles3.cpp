#ifdef _WIN32

// The ES3 build of imgui's backend, with its public names changed so it links beside the desktop build.
#define IMGUI_IMPL_OPENGL_ES3
#define ImGui_ImplOpenGL3_Init ImGuiGles3Backend_Init
#define ImGui_ImplOpenGL3_InitLoader ImGuiGles3Backend_InitLoader
#define ImGui_ImplOpenGL3_Shutdown ImGuiGles3Backend_Shutdown
#define ImGui_ImplOpenGL3_NewFrame ImGuiGles3Backend_NewFrame
#define ImGui_ImplOpenGL3_RenderDrawData ImGuiGles3Backend_RenderDrawData
#define ImGui_ImplOpenGL3_CreateDeviceObjects ImGuiGles3Backend_CreateDeviceObjects
#define ImGui_ImplOpenGL3_DestroyDeviceObjects ImGuiGles3Backend_DestroyDeviceObjects
#define ImGui_ImplOpenGL3_UpdateTexture ImGuiGles3Backend_UpdateTexture

#include "imgui_impl_opengl3.cpp"

#include "imgui_impl_gles3.h"

bool InitImGuiGles3() {
    return LoadGles3ForImGui() && ImGui_ImplOpenGL3_Init("#version 300 es");
}

void ImGuiGles3NewFrame() {
    ImGui_ImplOpenGL3_NewFrame();
}

void ImGuiGles3RenderDrawData(ImDrawData* draw_data) {
    ImGui_ImplOpenGL3_RenderDrawData(draw_data);
}

#endif
