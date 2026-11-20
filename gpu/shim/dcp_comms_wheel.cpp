// SPDX-License-Identifier: GPL-2.0-or-later
#include "dcp_comms_wheel.h"

#include <atomic>
#include <chrono>
#include <cstring>

#include <SDL3/SDL.h>
#include "comms_text.h"
#include "dcp_png.h"
#include "dcp_settings.h"
#include "dcpgpu.h"
#include "imgui.h"
#include "imgui_internal.h" // RegisterUserTexture

#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global dcp_comms_icons_png\n"
    "dcp_comms_icons_png:\n"
    ".incbin \"" DCP_COMMS_ICONS_PATH "\"\n"
    ".global dcp_comms_icons_png_end\n"
    "dcp_comms_icons_png_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden dcp_comms_icons_png\n"
    ".global dcp_comms_icons_png\n"
    "dcp_comms_icons_png:\n"
    ".incbin \"" DCP_COMMS_ICONS_PATH "\"\n"
    ".hidden dcp_comms_icons_png_end\n"
    ".global dcp_comms_icons_png_end\n"
    "dcp_comms_icons_png_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char dcp_comms_icons_png[];
extern "C" const unsigned char dcp_comms_icons_png_end[];

namespace DcpCommsWheel {

namespace {

constexpr int kAtlasColumns = 4, kAtlasRows = 2;
constexpr float kAtlasCell = 128.0f, kAtlasArt = 108.0f;

Driver g_driver;
MouseAccumulator g_mouse;
std::atomic<bool> g_left_click{false};
std::atomic<bool> g_cancel_click{false};
std::atomic<uint32_t> g_captured_buttons{0};

uint32_t ButtonBit(uint8_t button) { return button < 32 ? 1u << button : 0u; }
ImTextureData* g_atlas = nullptr;
bool g_atlas_failed = false;

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

const char* Label(int id) {
    const char* text = comms_text_message(id);
    if (DrawableByOverlayFont(text)) {
        return text;
    }
    return comms_text_lookup("english", id);
}

float Radians(float degrees) {
    return degrees * 3.14159265358979f / 180.0f;
}

ImVec2 Polar(const ImVec2& center, float radius, float degrees) {
    const float a = Radians(degrees);
    return ImVec2(center.x + std::sin(a) * radius, center.y - std::cos(a) * radius);
}

} // namespace

bool HandleEvent(const SDL_Event& event) {
    const bool engaged = g_driver.KeyboardEngaged();
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        if (!engaged) {
            return false;
        }
        g_mouse.Add(event.motion.xrel, event.motion.yrel);
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (!engaged) {
            return false;
        }
        if (event.button.button == SDL_BUTTON_LEFT) {
            g_left_click = true;
        } else if (event.button.button == SDL_BUTTON_RIGHT) {
            g_cancel_click = true;
        }
        g_captured_buttons |= ButtonBit(event.button.button);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        const uint32_t bit = ButtonBit(event.button.button);
        return (g_captured_buttons.fetch_and(~bit) & bit) != 0;
    }
    case SDL_EVENT_KEY_DOWN:
        if (!engaged || event.key.scancode != SDL_SCANCODE_ESCAPE) {
            return false;
        }
        g_cancel_click = true;
        return true;
    default:
        return false;
    }
}

bool Visible() {
    const View view = g_driver.Latest();
    return view.open && NowMs() - view.stamp_ms < kVisibleMs;
}

void PrepareTextures() {
    if (g_atlas || g_atlas_failed || !Visible()) {
        return;
    }
    DcpPng::Image image;
    if (!DcpPng::DecodeRgba(dcp_comms_icons_png, static_cast<size_t>(dcp_comms_icons_png_end - dcp_comms_icons_png), image)) {
        g_atlas_failed = true;
        return;
    }
    g_atlas = IM_NEW(ImTextureData)();
    g_atlas->Create(ImTextureFormat_RGBA32, image.width, image.height);
    std::memcpy(g_atlas->GetPixels(), image.rgba.data(), image.rgba.size());
    ImGui::RegisterUserTexture(g_atlas);
}

void Draw(float display_width, float display_height, float ui_scale) {
    if (!Visible()) {
        return;
    }
    const View view = g_driver.Latest();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();

    std::array<const char*, dcp::comms::kSectors> labels{};
    for (int i = 0; i < dcp::comms::kSectors; ++i) {
        labels[static_cast<size_t>(i)] = Label(dcp::comms::MessageAtSector(i));
    }
    const Layout layout = ComputeLayout(display_width * 0.5f, display_height * 0.5f, ui_scale, labels,
                                        [font](const char* text, float px) { return font->CalcTextSizeA(px, FLT_MAX, 0.0f, text).x; });
    const ImVec2 center(layout.center_x, layout.center_y);
    const float outer = layout.outer_radius, inner = layout.inner_radius;

    const ImU32 gold = IM_COL32(214, 168, 78, 255);
    const ImU32 gold_soft = IM_COL32(190, 146, 66, 150);
    const ImU32 body = IM_COL32(30, 18, 10, 218);
    const ImU32 hub = IM_COL32(48, 30, 16, 240);
    const ImU32 hot = IM_COL32(222, 172, 74, 238);
    const ImU32 ink = IM_COL32(244, 230, 196, 255);
    const ImU32 ink_hot = IM_COL32(42, 24, 8, 255);

    dl->AddRectFilled(ImVec2(0, 0), ImVec2(display_width, display_height), IM_COL32(6, 4, 2, 120));
    dl->AddCircleFilled(center, outer, body, 96);

    if (view.selected >= 0) {
        const float centre_angle = static_cast<float>(view.selected) * 45.0f;
        ImVec2 points[2 * 13];
        int count = 0;
        for (int step = 0; step <= 12; ++step) {
            points[count++] = Polar(center, outer - 3.0f * ui_scale, centre_angle - 22.5f + 45.0f * static_cast<float>(step) / 12.0f);
        }
        for (int step = 12; step >= 0; --step) {
            points[count++] = Polar(center, inner + 3.0f * ui_scale, centre_angle - 22.5f + 45.0f * static_cast<float>(step) / 12.0f);
        }
        dl->AddConcavePolyFilled(points, count, hot);
    }

    for (int i = 0; i < dcp::comms::kSectors; ++i) {
        const float angle = (static_cast<float>(i) + 0.5f) * 45.0f;
        dl->AddLine(Polar(center, inner, angle), Polar(center, outer, angle), gold_soft, 2.0f * ui_scale);
        const ImVec2 stud = Polar(center, outer, angle);
        const float s = 6.0f * ui_scale;
        const ImVec2 diamond[4] = {ImVec2(stud.x, stud.y - s), ImVec2(stud.x + s, stud.y), ImVec2(stud.x, stud.y + s),
                                   ImVec2(stud.x - s, stud.y)};
        dl->AddConvexPolyFilled(diamond, 4, gold);
    }
    dl->AddCircle(center, outer, gold, 128, 3.0f * ui_scale);
    dl->AddCircle(center, outer - 9.0f * ui_scale, gold_soft, 128, 1.0f * ui_scale);
    dl->AddCircleFilled(center, inner, hub, 64);
    dl->AddCircle(center, inner, gold, 64, 2.5f * ui_scale);
    dl->AddCircle(center, inner - 7.0f * ui_scale, gold_soft, 64, 1.0f * ui_scale);

    for (int i = 0; i < dcp::comms::kSectors; ++i) {
        const SectorLayout& s = layout.sectors[static_cast<size_t>(i)];
        const bool selected = i == view.selected;
        const ImU32 color = selected ? ink_hot : ink;
        if (g_atlas) {
            const int id = dcp::comms::MessageAtSector(i);
            const float u0 = static_cast<float>(id % kAtlasColumns) / kAtlasColumns;
            const float v0 = static_cast<float>(id / kAtlasColumns) / kAtlasRows;
            const float pad = s.icon.w * (kAtlasCell / kAtlasArt - 1.0f) * 0.5f;
            dl->AddImage(g_atlas->GetTexRef(), ImVec2(s.icon.x - pad, s.icon.y - pad),
                         ImVec2(s.icon.x + s.icon.w + pad, s.icon.y + s.icon.h + pad), ImVec2(u0, v0),
                         ImVec2(u0 + 1.0f / kAtlasColumns, v0 + 1.0f / kAtlasRows), color);
        }
        dl->AddText(font, s.font_px, ImVec2(s.label.x, s.label.y), color, labels[static_cast<size_t>(i)]);
    }
}

} // namespace DcpCommsWheel

extern "C" void dcpgpu_comms_wheel_step(const DcpCommsPadIn* in, DcpCommsPadOut* out) {
    using namespace DcpCommsWheel;
    Inputs inputs;
    inputs.now_ms = NowMs();
    inputs.available = in->available != 0;
    inputs.pad_up = in->dpad_up != 0;
    inputs.stick_x = (static_cast<float>(in->right_x) - 128.0f) / 127.5f;
    inputs.stick_y = (static_cast<float>(in->right_y) - 128.0f) / 127.5f;
    inputs.circle = in->circle != 0;
    inputs.confirm = in->cross != 0;
    inputs.nav = static_cast<uint8_t>(in->nav);
    const bool* keys = SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(nullptr) : nullptr;
    const int scancode = DcpSettings::Get().comms_key;
    inputs.key_down = keys && scancode > SDL_SCANCODE_UNKNOWN && scancode < SDL_SCANCODE_COUNT && keys[scancode];
    if (!g_driver.KeyboardEngaged()) {
        g_mouse.Clear();
        g_left_click = false;
        g_cancel_click = false;
    }
    inputs.click = g_left_click.exchange(false);
    inputs.cancel_click = g_cancel_click.exchange(false);
    g_mouse.Read(inputs.mouse_x, inputs.mouse_y);
    const Outputs result = g_driver.Step(inputs);
    out->hide_dpad_up = result.hide_pad_up;
    out->hide_cross = result.hide_confirm;
    out->hide_circle = result.hide_circle;
    out->neutral_right_stick = result.neutral_right_stick;
    out->neutral_keyboard_axes = result.neutral_keyboard_axes;
    out->send = result.send;
}
