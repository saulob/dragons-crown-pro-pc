// Forked from TRG Launcher (TekRantGaming/trg-launcher, pinned in gpu/deps.txt) and restyled for Dragon's Crown Pro PC.
// The upstream repository states no licence: see THIRD-PARTY-LICENSES.txt.
#include "trg/launcher.h"

#include <algorithm>
#include <cctype>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "trg/platform.h"

namespace trg {
namespace {

ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
  return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

void ApplyTheme(const Theme& t, float s) {
  ImGuiStyle& st = ImGui::GetStyle();
  st.WindowPadding = ImVec2(14 * s, 12 * s);  // popups and combos; the launcher's own windows use none
  st.WindowBorderSize = 0;
  st.ChildBorderSize = 0;
  st.WindowRounding = 0;
  st.ChildRounding = 5 * s;
  st.FrameRounding = 4 * s;
  st.PopupRounding = 4 * s;
  st.GrabRounding = 7 * s;
  st.TabRounding = 4 * s;
  st.ScrollbarRounding = 4 * s;
  st.ScrollbarSize = 8 * s;
  st.FramePadding = ImVec2(12 * s, 8 * s);
  st.ItemSpacing = ImVec2(10 * s, 8 * s);
  st.ItemInnerSpacing = ImVec2(8 * s, 6 * s);
  st.CellPadding = ImVec2(0, 8 * s);
  st.GrabMinSize = 14 * s;
  st.FrameBorderSize = 1;
  st.PopupBorderSize = 1;
  st.SelectableTextAlign = ImVec2(0, 0.5f);
  ImVec4* c = st.Colors;
  c[ImGuiCol_Text] = t.text;
  c[ImGuiCol_TextDisabled] = t.dim;
  c[ImGuiCol_WindowBg] = t.bg;
  c[ImGuiCol_ChildBg] = t.panel;
  c[ImGuiCol_PopupBg] = t.popup;
  c[ImGuiCol_FrameBg] = t.frame;
  c[ImGuiCol_FrameBgHovered] = t.frame_hot;
  c[ImGuiCol_FrameBgActive] = t.frame_active;
  c[ImGuiCol_Button] = t.frame;
  c[ImGuiCol_ButtonHovered] = t.frame_hot;
  c[ImGuiCol_ButtonActive] = t.frame_active;
  c[ImGuiCol_Header] = t.frame;
  c[ImGuiCol_HeaderHovered] = t.frame_hot;
  c[ImGuiCol_HeaderActive] = t.frame_active;
  c[ImGuiCol_SliderGrab] = t.accent;
  c[ImGuiCol_SliderGrabActive] = t.accent_hot;
  c[ImGuiCol_CheckMark] = t.accent;
  c[ImGuiCol_PlotHistogram] = t.accent;
  c[ImGuiCol_PlotHistogramHovered] = t.accent_hot;
  c[ImGuiCol_TextSelectedBg] = ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.35f);
  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = t.frame_hot;
  c[ImGuiCol_ScrollbarGrabHovered] = t.frame_active;
  c[ImGuiCol_ScrollbarGrabActive] = t.frame_active;
  c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.07f);
  c[ImGuiCol_TableBorderLight] = ImVec4(1, 1, 1, 0.06f);
  c[ImGuiCol_TableBorderStrong] = ImVec4(1, 1, 1, 0.08f);
  c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_TableRowBgAlt] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_NavCursor] = t.accent;
  c[ImGuiCol_Border] = t.border;
  c[ImGuiCol_FrameBg] = t.input;
  c[ImGuiCol_FrameBgHovered] = t.input;
  c[ImGuiCol_FrameBgActive] = t.input;
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
}

ImFont* LoadFirstFont(std::initializer_list<std::string> paths, float size) {
  for (const std::string& p : paths) {
    FILE* f = OpenFile(p, "rb");
    if (!f) continue;
    fclose(f);
    if (ImFont* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(p.c_str(), size)) return font;
  }
  return nullptr;
}

// A chosen option: gold text on a faint gold fill with a dark-gold outline (the mockup's .nav-btn.active).
void PushSelected(const Theme& t) {
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.14f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.20f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.26f));
  ImGui::PushStyleColor(ImGuiCol_Text, t.accent_hot);
  ImGui::PushStyleColor(ImGuiCol_Border, t.accent_dark);
}

void PushAccent(const Theme& t) {
  ImGui::PushStyleColor(ImGuiCol_Button, t.accent);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, t.accent_hot);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, t.accent_hot);
  ImGui::PushStyleColor(ImGuiCol_Text, t.on_accent);
  ImGui::PushStyleColor(ImGuiCol_Border, t.accent);
}

}  // namespace

// ------------------------------------------------------------------- theme --
ImU32 Col(const ImVec4& c, float alpha_mul) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alpha_mul));
}

Theme Theme::Midnight() {
  Theme t;
  t.bg = ImVec4(0.027f, 0.039f, 0.086f, 1.0f);
  t.panel = ImVec4(0.055f, 0.082f, 0.165f, 1.0f);
  t.frame = ImVec4(0.090f, 0.129f, 0.247f, 1.0f);
  t.frame_hot = ImVec4(0.125f, 0.176f, 0.325f, 1.0f);
  t.frame_active = ImVec4(0.153f, 0.212f, 0.384f, 1.0f);
  t.popup = ImVec4(0.075f, 0.110f, 0.212f, 0.99f);
  t.accent = ImVec4(0.545f, 0.835f, 0.314f, 1.0f);
  t.accent_hot = ImVec4(0.651f, 0.910f, 0.416f, 1.0f);
  t.on_accent = ImVec4(0.035f, 0.090f, 0.031f, 1.0f);
  t.text = ImVec4(0.95f, 0.97f, 1.0f, 1.0f);
  t.dim = ImVec4(0.580f, 0.659f, 0.800f, 1.0f);
  t.good = ImVec4(0.118f, 0.420f, 0.239f, 1.0f);
  t.warn = ImVec4(0.490f, 0.318f, 0.090f, 1.0f);
  t.good_dot = ImVec4(0.55f, 0.95f, 0.70f, 1.0f);
  t.warn_dot = ImVec4(1.0f, 0.70f, 0.30f, 1.0f);
  return t;
}

Theme Theme::Ocean() {
  Theme t;
  t.bg = ImVec4(0.035f, 0.100f, 0.200f, 1.0f);
  t.panel = ImVec4(0.070f, 0.170f, 0.320f, 1.0f);
  t.frame = ImVec4(0.120f, 0.270f, 0.490f, 1.0f);
  t.frame_hot = ImVec4(0.180f, 0.370f, 0.640f, 1.0f);
  t.frame_active = ImVec4(0.220f, 0.430f, 0.720f, 1.0f);
  t.popup = ImVec4(0.080f, 0.190f, 0.360f, 0.98f);
  t.accent = ImVec4(1.000f, 0.790f, 0.240f, 1.0f);
  t.accent_hot = ImVec4(1.000f, 0.620f, 0.200f, 1.0f);
  t.on_accent = ImVec4(0.080f, 0.120f, 0.220f, 1.0f);
  t.text = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
  t.dim = ImVec4(0.660f, 0.760f, 0.910f, 1.0f);
  t.good = ImVec4(0.110f, 0.431f, 0.282f, 1.0f);
  t.warn = ImVec4(0.588f, 0.322f, 0.078f, 1.0f);
  t.good_dot = ImVec4(0.47f, 0.90f, 0.63f, 1.0f);
  t.warn_dot = ImVec4(1.0f, 0.78f, 0.35f, 1.0f);
  return t;
}

Theme Theme::Ember() {
  Theme t;
  t.bg = ImVec4(0.055f, 0.047f, 0.051f, 1.0f);
  t.panel = ImVec4(0.098f, 0.082f, 0.086f, 1.0f);
  t.frame = ImVec4(0.157f, 0.129f, 0.133f, 1.0f);
  t.frame_hot = ImVec4(0.212f, 0.173f, 0.176f, 1.0f);
  t.frame_active = ImVec4(0.255f, 0.204f, 0.208f, 1.0f);
  t.popup = ImVec4(0.120f, 0.100f, 0.104f, 0.99f);
  t.accent = ImVec4(0.910f, 0.300f, 0.240f, 1.0f);
  t.accent_hot = ImVec4(0.970f, 0.420f, 0.340f, 1.0f);
  t.on_accent = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
  t.text = ImVec4(0.97f, 0.95f, 0.94f, 1.0f);
  t.dim = ImVec4(0.700f, 0.630f, 0.620f, 1.0f);
  t.good = ImVec4(0.118f, 0.400f, 0.239f, 1.0f);
  t.warn = ImVec4(0.520f, 0.300f, 0.080f, 1.0f);
  t.good_dot = ImVec4(0.55f, 0.95f, 0.70f, 1.0f);
  t.warn_dot = ImVec4(1.0f, 0.70f, 0.30f, 1.0f);
  return t;
}

Theme Theme::DragonsCrown() {
  Theme t;
  t.bg = ImVec4(0.071f, 0.071f, 0.082f, 1.0f);            // #121215
  t.panel = ImVec4(0.102f, 0.102f, 0.125f, 0.92f);        // cards
  t.frame = ImVec4(0.145f, 0.145f, 0.170f, 1.0f);         // buttons (.btn-dark)
  t.frame_hot = ImVec4(0.190f, 0.190f, 0.220f, 1.0f);
  t.frame_active = ImVec4(0.230f, 0.230f, 0.270f, 1.0f);
  t.popup = ImVec4(0.102f, 0.102f, 0.125f, 0.99f);
  t.accent = ImVec4(0.886f, 0.753f, 0.408f, 1.0f);        // #e2c068
  t.accent_hot = ImVec4(1.000f, 0.847f, 0.459f, 1.0f);    // #ffd875
  t.on_accent = ImVec4(0.067f, 0.067f, 0.067f, 1.0f);     // #111
  t.text = ImVec4(0.878f, 0.878f, 0.902f, 1.0f);          // #e0e0e6
  t.dim = ImVec4(0.533f, 0.533f, 0.580f, 1.0f);           // #888894
  t.good = ImVec4(0.118f, 0.420f, 0.239f, 1.0f);
  t.warn = ImVec4(0.470f, 0.180f, 0.100f, 1.0f);
  t.good_dot = ImVec4(0.400f, 0.800f, 0.550f, 1.0f);
  t.warn_dot = ImVec4(0.953f, 0.612f, 0.071f, 1.0f);      // #f39c12
  return t;
}

Theme Theme::WithAccent(ImVec4 colour) const {
  Theme t = *this;
  t.accent = colour;
  t.accent_hot = Mix(colour, ImVec4(1, 1, 1, 1), 0.18f);
  const float luminance = 0.2126f * colour.x + 0.7152f * colour.y + 0.0722f * colour.z;
  t.on_accent = luminance > 0.45f ? Mix(colour, ImVec4(0, 0, 0, 1), 0.88f) : ImVec4(1, 1, 1, 1);
  return t;
}

// ------------------------------------------------------------------- fonts --
Fonts LoadSystemFonts(float size) {
  Fonts f;
  f.size = size;
#if defined(_WIN32)
  const std::string dir = "C:\\Windows\\Fonts\\";
  f.regular = LoadFirstFont({dir + "segoeui.ttf"}, size);
  f.semibold = LoadFirstFont({dir + "seguisb.ttf"}, size);
  f.bold = LoadFirstFont({dir + "segoeuib.ttf"}, size);
#elif defined(__APPLE__)
  f.regular = LoadFirstFont({"/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf"}, size);
  f.bold = LoadFirstFont({"/System/Library/Fonts/Supplemental/Arial Bold.ttf", "/Library/Fonts/Arial Bold.ttf"}, size);
#else
  f.regular = LoadFirstFont({"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                             "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"},
                            size);
  f.bold = LoadFirstFont({"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                          "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                          "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf"},
                         size);
#endif
  if (!f.regular) f.regular = ImGui::GetIO().Fonts->AddFontDefault();
  if (!f.bold) f.bold = f.regular;
  if (!f.semibold) f.semibold = f.bold;
  ImGui::GetIO().FontDefault = f.regular;
  return f;
}

namespace {

// Dear ImGui sizes a font by its line height (ascent - descent); CSS sizes it by the em. This is the factor between
// them, read from the font's head and hhea tables (1 when they cannot be read).
float LineHeightPerEm(const FontBlob& b) {
  const auto* d = static_cast<const unsigned char*>(b.data);
  const auto u16 = [&](size_t o) { return o + 2 <= b.size ? unsigned(d[o] << 8 | d[o + 1]) : 0u; };
  const auto i16 = [&](size_t o) { return int(static_cast<short>(u16(o))); };
  if (b.size < 12) return 1.0f;
  const unsigned tables = u16(4);
  size_t head = 0, hhea = 0;
  for (unsigned i = 0; i < tables; ++i) {
    const size_t rec = 12 + 16 * size_t(i);
    if (rec + 16 > b.size) break;
    const size_t offset = size_t(u16(rec + 8)) << 16 | u16(rec + 10);
    if (std::equal(d + rec, d + rec + 4, "head")) head = offset;
    if (std::equal(d + rec, d + rec + 4, "hhea")) hhea = offset;
  }
  if (!head || !hhea) return 1.0f;
  const float upm = float(u16(head + 18));
  const float height = float(i16(hhea + 4) - i16(hhea + 6));
  return upm > 0.0f && height > 0.0f ? height / upm : 1.0f;
}

}  // namespace

Fonts LoadFonts(const FontSet& set, float size) {
  const auto add = [size](const FontBlob& b) -> ImFont* {
    if (!b.data || !b.size) return nullptr;
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;  // the bytes live in the executable
    cfg.ExtraSizeScale = LineHeightPerEm(b);  // so that `size` is the CSS font-size of the mockup
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<void*>(b.data), int(b.size), size, &cfg);
  };
  Fonts f;
  f.size = size;
  f.regular = add(set.regular);
  f.semibold = add(set.semibold);
  f.bold = add(set.bold);
  f.heading = add(set.heading);
  f.mono = add(set.mono);
  if (!f.regular) return LoadSystemFonts(size);
  if (!f.semibold) f.semibold = f.bold ? f.bold : f.regular;
  if (!f.bold) f.bold = f.semibold;
  if (!f.heading) f.heading = f.bold;
  if (!f.mono) f.mono = f.regular;
  ImGui::GetIO().FontDefault = f.regular;
  return f;
}

// ---------------------------------------------------------------------- ui --
const Theme& Ui::theme() const { return launcher_->config_.theme; }
const Fonts& Ui::fonts() const { return launcher_->config_.fonts; }
SettingsStore& Ui::settings() { return *launcher_->config_.settings; }
void Ui::SetStatus(std::string text, double seconds) { launcher_->SetStatus(std::move(text), seconds); }

void Ui::BeginPage() {
  in_rows_ = false;
  in_card_ = false;
  rows_id_ = 0;
  cards_id_ = 0;
}

void Ui::EndPage() {
  LeaveRows();
  EndCard();
}

// ------------------------------------------------------------------ cards --
void Ui::BeginCardImpl(const char* title, const char* subtitle, const char* badge) {
  LeaveRows();
  EndCard();
  const Theme& t = theme();
  char id[32];
  std::snprintf(id, sizeof id, "##card%d", cards_id_++);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, t.card);
  ImGui::PushStyleColor(ImGuiCol_Border, t.border);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * scale_, 10 * scale_));
  ImGui::BeginChild(id, ImVec2(0, 0),
                    ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
  in_card_ = true;
  ImGui::Dummy(ImVec2(0, 2 * scale_));
  const bool has_title = title && *title;
  const bool has_sub = subtitle && *subtitle;
  const ImVec2 top = ImGui::GetCursorScreenPos();
  if (has_title) {
    ImGui::PushFont(fonts().semibold, fonts().size * 15.0f / 13.0f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
  }
  if (badge && *badge) {  // .badge-tag: a small outlined label at the right
    ImFont* font = fonts().semibold;
    const float fs = fonts().size * 9.0f / 13.0f * scale_ / scale_;
    const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, badge);
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 18 * scale_;
    const ImVec2 a(right - ts.x - 12 * scale_, top.y), b(right, top.y + ts.y + 4 * scale_);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 15), 3 * scale_);
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 30), 3 * scale_);
    dl->AddText(font, fs, ImVec2(a.x + 6 * scale_, a.y + 2 * scale_), Col(t.dim), badge);
  }
  if (has_sub) {
    ImGui::PushStyleColor(ImGuiCol_Text, t.dim);
    ImGui::PushFont(nullptr, fonts().size * 11.0f / 13.0f);
    ImGui::TextUnformatted(subtitle);
    ImGui::PopFont();
    ImGui::PopStyleColor();
  }
  if (has_title || has_sub) ImGui::Dummy(ImVec2(0, 2 * scale_));
}

void Ui::Card(const char* title, const char* subtitle, const char* badge) { BeginCardImpl(title, subtitle, badge); }

void Ui::Chips(const std::vector<std::string>& items, int highlight) {
  if (!in_card_) BeginCardImpl(nullptr, nullptr, nullptr);
  LeaveRows();
  const Theme& t = theme();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float gap = 8 * scale_, h = 30 * scale_;
  const float w = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
  const float fs = fonts().size * 11.0f / 13.0f;
  for (size_t i = 0; i < items.size(); i += 2) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    for (size_t k = 0; k < 2 && i + k < items.size(); ++k) {
      const ImVec2 a(p.x + float(k) * (w + gap), p.y), b(a.x + w, a.y + h);
      const bool on = int(i + k) == highlight;
      dl->AddRectFilled(a, b, Col(t.input), 4 * scale_);
      dl->AddRect(a, b, Col(on ? t.accent_dark : t.border), 4 * scale_);
      dl->PushClipRect(a, b, true);
      dl->AddText(fonts().mono, fs, ImVec2(a.x + 10 * scale_, a.y + (h - fs) * 0.5f), Col(t.accent), items[i + k].c_str());
      dl->PopClipRect();
    }
    ImGui::Dummy(ImVec2(0, h));
  }
}

void Ui::EndCard() {
  if (!in_card_) return;
  LeaveRows();
  ImGui::EndChild();
  in_card_ = false;
  ImGui::Dummy(ImVec2(0, 8 * scale_));
}

void Ui::Row(const char* label, const char* help) {
  if (!in_card_) BeginCardImpl(nullptr, nullptr, nullptr);
  if (!in_rows_) {
    char id[32];
    std::snprintf(id, sizeof id, "##rows%d", rows_id_++);
    in_rows_ = ImGui::BeginTable(id, 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp);
    if (in_rows_) {
      ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.46f);
      ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthStretch, 0.54f);
    }
  }
  if (in_rows_) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
  }
  if (label && *label) {
    ImGui::PushFont(fonts().semibold, 0.0f);
    ImGui::TextUnformatted(label);
    ImGui::PopFont();
  }
  if (help && *help) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme().dim);
    ImGui::PushFont(nullptr, fonts().size * 0.84f);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 24 * scale_);
    ImGui::TextUnformatted(help);
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::PopStyleColor();
  }
  if (in_rows_) ImGui::TableSetColumnIndex(1);
  ImGui::SetNextItemWidth(-FLT_MIN);
}

void Ui::RowLarge(const char* label, const char* help) {
  if (!in_card_) BeginCardImpl(nullptr, nullptr, nullptr);
  if (!in_rows_) {
    char id[32];
    std::snprintf(id, sizeof id, "##rows%d", rows_id_++);
    in_rows_ = ImGui::BeginTable(id, 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp);
    if (in_rows_) {
      ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.46f);
      ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthStretch, 0.54f);
    }
  }
  if (in_rows_) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
  }
  if (label && *label) {
    ImGui::PushFont(fonts().semibold, 0.0f);
    ImGui::TextUnformatted(label);
    ImGui::PopFont();
  }
  if (help && *help) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme().dim);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 24 * scale_);
    ImGui::TextUnformatted(help);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
  }
  if (in_rows_) ImGui::TableSetColumnIndex(1);
  ImGui::SetNextItemWidth(-FLT_MIN);
}

void Ui::EndRows() {
  if (!in_rows_) return;
  ImGui::EndTable();
  in_rows_ = false;
}

int Ui::Segmented(const char* id, const std::vector<std::string>& labels, int selected) {
  if (labels.empty()) return -1;
  ImGui::PushID(id);
  const float avail = ImGui::GetContentRegionAvail().x;
  const float gap = 6 * scale_;
  const float bw = (avail - gap * float(labels.size() - 1)) / float(labels.size());
  int clicked = -1;
  for (size_t i = 0; i < labels.size(); ++i) {
    if (i) ImGui::SameLine(0, gap);
    const bool sel = int(i) == selected;
    if (sel) PushSelected(theme());
    ImGui::PushID(int(i));
    if (ImGui::Button(labels[i].c_str(), ImVec2(bw, 0)) && !sel) clicked = int(i);
    ImGui::PopID();
    if (sel) ImGui::PopStyleColor(5);
  }
  ImGui::PopID();
  return clicked;
}

bool Ui::AccentButton(const char* label, ImVec2 size) {
  PushAccent(theme());
  const bool r = ImGui::Button(label, size);
  ImGui::PopStyleColor(5);
  return r;
}

bool Ui::Choice(const char* label, const char* help, const char* key, const char* def,
                const std::vector<Option>& options, int max_buttons) {
  if (int(options.size()) > max_buttons) return Combo(label, help, key, def, options);
  Row(label, help);
  const std::string cur = settings().Get(key, def);
  std::vector<std::string> labels;
  int sel = -1;
  for (size_t i = 0; i < options.size(); ++i) {
    labels.push_back(options[i].label);
    if (options[i].value == cur) sel = int(i);
  }
  if (const int i = Segmented(key, labels, sel); i >= 0) {
    settings().Set(key, options[size_t(i)].value);
    return true;
  }
  return false;
}

bool Ui::Combo(const char* label, const char* help, const char* key, const char* def,
               const std::vector<Option>& options) {
  Row(label, help);
  const std::string cur = settings().Get(key, def);
  std::string preview = cur;
  for (const Option& o : options)
    if (o.value == cur) preview = o.label;
  bool changed = false;
  ImGui::PushID(key);
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::BeginCombo("##combo", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
    for (const Option& o : options) {
      const bool on = o.value == cur;
      if (ImGui::Selectable(o.label.c_str(), on) && !on) {
        settings().Set(key, o.value);
        changed = true;
      }
      if (on) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  ImGui::PopID();
  return changed;
}

bool Ui::Toggle(const char* label, const char* help, const char* key, bool def, const char* off, const char* on) {
  Row(label, help);
  const Theme& t = theme();
  const bool v = settings().GetBool(key, def);
  ImGui::PushID(key);
  const float sw = 44 * scale_, sh = 22 * scale_;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float avail = ImGui::GetContentRegionAvail().x;
  const bool clicked = ImGui::InvisibleButton("##switch", ImVec2(avail, sh));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 a(p.x + avail - sw, p.y), b(p.x + avail, p.y + sh);
  // The state's name sits at the left of the switch (.val-badge).
  const char* state = v ? on : off;
  const ImVec2 ts = fonts().mono->CalcTextSizeA(fonts().size * 12.0f / 13.0f, FLT_MAX, 0, state);
  dl->AddText(fonts().mono, fonts().size * 12.0f / 13.0f, ImVec2(a.x - 12 * scale_ - ts.x, p.y + (sh - ts.y) * 0.5f),
              Col(v ? t.accent : t.dim), state);
  dl->AddRectFilled(a, b, v ? Col(t.accent, 0.25f) : IM_COL32(42, 42, 53, 255), sh * 0.5f);
  dl->AddRect(a, b, Col(v ? t.accent_dark : t.border), sh * 0.5f);
  const float r = 7 * scale_;
  dl->AddCircleFilled(ImVec2(v ? b.x - 3 * scale_ - r : a.x + 3 * scale_ + r, p.y + sh * 0.5f), r,
                      v ? Col(t.accent) : IM_COL32(136, 136, 136, 255), 20);
  ImGui::PopID();
  if (clicked) {
    settings().SetBool(key, !v);
    return true;
  }
  return false;
}

bool Ui::SliderInt(const char* label, const char* help, const char* key, int def, int lo, int hi, const char* format,
                   int step) {
  Row(label, help);
  int v = settings().GetInt(key, def);
  ImGui::PushID(key);
  ImGui::SetNextItemWidth(-FLT_MIN);
  bool changed = false;
  if (ImGui::SliderInt("##slider", &v, lo, hi, format, ImGuiSliderFlags_AlwaysClamp)) {
    if (step > 1) v = std::clamp(lo + (v - lo + step / 2) / step * step, lo, hi);
    settings().SetInt(key, v);
    changed = true;
  }
  ImGui::PopID();
  return changed;
}

bool Ui::SliderFloat(const char* label, const char* help, const char* key, float def, float lo, float hi,
                     const char* format) {
  Row(label, help);
  float v = settings().GetFloat(key, def);
  ImGui::PushID(key);
  ImGui::SetNextItemWidth(-FLT_MIN);
  bool changed = false;
  if (ImGui::SliderFloat("##slider", &v, lo, hi, format, ImGuiSliderFlags_AlwaysClamp)) {
    settings().SetFloat(key, v);
    changed = true;
  }
  ImGui::PopID();
  return changed;
}

bool Ui::InputText(const char* label, const char* help, const char* key, const char* hint) {
  Row(label, help);
  char buf[2048];
  std::snprintf(buf, sizeof buf, "%s", settings().Get(key).c_str());
  ImGui::PushID(key);
  ImGui::SetNextItemWidth(-FLT_MIN);
  const bool changed = ImGui::InputTextWithHint("##text", hint, buf, sizeof buf);
  if (changed) settings().Set(key, buf);
  ImGui::PopID();
  return changed;
}

void Ui::Info(const char* label, const char* text) {
  Row(label, nullptr);
  ImGui::PushStyleColor(ImGuiCol_Text, theme().dim);
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted(text);
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
}

bool Ui::ButtonRow(const char* label, const char* help, const char* button, bool accent) {
  Row(label, help);
  ImGui::PushID(label);
  // The mockup's buttons are as wide as their text, at the left of the control column.
  const float width = std::max(110 * scale_, ImGui::CalcTextSize(button).x + 36 * scale_);
  const ImVec2 size(std::min(width, ImGui::GetContentRegionAvail().x), 0);
  const bool r = accent ? AccentButton(button, size) : ImGui::Button(button, size);
  ImGui::PopID();
  return r;
}

bool Ui::ConfirmRow(const char* label, const char* help, const char* button, const char* question,
                    const char* confirm, const char* cancel) {
  Row(label, help);
  ImGui::PushID(label);
  if (ImGui::Button(button, ImVec2(-FLT_MIN, 0))) ImGui::OpenPopup("##confirm");
  bool confirmed = false;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * scale_, 20 * scale_));
  const bool open = ImGui::BeginPopupModal("##confirm", nullptr,
                                           ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                               ImGuiWindowFlags_NoSavedSettings);
  ImGui::PopStyleVar();
  if (open) {
    ImGui::PushFont(fonts().semibold, 0.0f);
    ImGui::TextUnformatted(question);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(320 * scale_, 6 * scale_));
    if (AccentButton(confirm, ImVec2(150 * scale_, 0))) {
      confirmed = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(cancel, ImVec2(150 * scale_, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return confirmed;
}

void Ui::FolderRow(const char* label, const char* help, const char* button, const std::string& path) {
  if (ButtonRow(label, help, button)) OpenInFileManager(path);
}

bool Ui::LauncherVisibilityRow(const char* key) {
  return Toggle("Show this launcher", "Off starts the game directly. Hold Shift while starting to bring it back.",
                key, true, "Off", "At startup");
}

void Ui::ResetAllRow() {
  if (ConfirmRow("Reset settings", "Put every setting back to its default.", "Reset all settings",
                 "Put every setting back to its default?", "Reset")) {
    settings().ResetToDefaults();
    if (launcher_->config_.on_reset) launcher_->config_.on_reset();
    SetStatus("Settings reset to defaults.");
  }
}

std::optional<ImGuiKey> Ui::KeyCapture(const char* id, const std::string& shown, float width) {
  ImGui::PushID(id);
  std::optional<ImGuiKey> result;
  if (capturing_ == id) {
    if (AccentButton("Press a key...  (Esc cancels)", ImVec2(width, 0))) capturing_.clear();
    // Wait a frame so the click or Enter that started the capture is not taken.
    if (!capturing_.empty() && ImGui::GetFrameCount() > capture_frame_) {
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        capturing_.clear();
      } else {
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_GamepadStart; ++k) {
          if (!ImGui::IsKeyPressed(ImGuiKey(k), false)) continue;
          result = ImGuiKey(k);
          capturing_.clear();
          break;
        }
      }
    }
  } else if (ImGui::Button(shown.empty() ? "(none)" : shown.c_str(), ImVec2(width, 0))) {
    capturing_ = id;
    capture_frame_ = ImGui::GetFrameCount();
  }
  ImGui::PopID();
  return result;
}

std::optional<ImGuiKey> Ui::KeyBindRow(const char* label, const char* help, const std::string& shown) {
  Row(label, help);
  return KeyCapture(label, shown);
}

void Ui::StatusCard(Status status, const char* title, const char* detail, float progress) {
  BeginCardImpl(nullptr, nullptr, nullptr);
  const Theme& t = theme();
  const ImVec4 tone = status == Status::kReady ? t.good_dot : status == Status::kBusy ? t.accent : t.warn_dot;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float r = 9 * scale_;
  const ImVec2 c(p.x + r, p.y + r + 1 * scale_);
  if (status == Status::kBusy) {
    const float a = float(ImGui::GetTime()) * 5.0f;
    dl->PathArcTo(c, r - 1 * scale_, a, a + 4.2f, 24);
    dl->PathStroke(Col(t.accent), 0, 2.5f * scale_);
  } else {
    dl->AddCircleFilled(c, r, Col(tone), 24);
    const ImU32 ink = IM_COL32(17, 17, 17, 255);
    if (status == Status::kReady) {  // a check mark
      dl->AddPolyline(std::array<ImVec2, 3>{ImVec2(c.x - 4 * scale_, c.y), ImVec2(c.x - 1 * scale_, c.y + 3 * scale_),
                                            ImVec2(c.x + 4 * scale_, c.y - 3 * scale_)}.data(),
                      3, ink, 0, 2 * scale_);
    } else {  // an exclamation mark
      dl->AddLine(ImVec2(c.x, c.y - 5 * scale_), ImVec2(c.x, c.y + 1 * scale_), ink, 2 * scale_);
      dl->AddCircleFilled(ImVec2(c.x, c.y + 4.5f * scale_), 1.3f * scale_, ink);
    }
  }
  ImGui::SetCursorScreenPos(ImVec2(p.x + 26 * scale_, p.y));
  ImGui::BeginGroup();
  ImGui::PushStyleColor(ImGuiCol_Text, tone);
  ImGui::PushFont(fonts().bold, fonts().size * 15.0f / 13.0f);
  ImGui::TextUnformatted(title);
  ImGui::PopFont();
  ImGui::PopStyleColor();
  if (progress >= 0.0f) {
    ImGui::Dummy(ImVec2(0, 2 * scale_));
    ImGui::ProgressBar(progress, ImVec2(ImGui::GetContentRegionAvail().x, 6 * scale_), "");
  } else if (detail && *detail) {
    ImGui::PushStyleColor(ImGuiCol_Text, t.dim);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(detail);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
  }
  ImGui::EndGroup();
  EndCard();
}

bool Ui::TaskProgress(Task& task) {
  if (!task.running()) return false;
  const float f = task.fraction();
  char label[160];
  if (f >= 0.0f)
    std::snprintf(label, sizeof label, "%s %.0f%%", task.label().c_str(), double(f) * 100.0);
  else
    std::snprintf(label, sizeof label, "%s", task.label().c_str());
  const float cancel_w = 120 * scale_;
  ImGui::ProgressBar(f >= 0.0f ? f : -1.0f * float(ImGui::GetTime()),
                     ImVec2(ImGui::GetContentRegionAvail().x - cancel_w - ImGui::GetStyle().ItemSpacing.x, 0), label);
  ImGui::SameLine();
  ImGui::BeginDisabled(task.cancelled());
  if (ImGui::Button("Cancel##task", ImVec2(-FLT_MIN, 0))) task.Cancel();
  ImGui::EndDisabled();
  return true;
}

void Ui::Heading(const char* text) { BeginCardImpl(text, nullptr, nullptr); }

void Ui::Paragraph(const char* text) {
  LeaveRows();
  ImGui::PushStyleColor(ImGuiCol_Text, theme().dim);
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted(text);
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
}

void Ui::Help(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, theme().dim);
  ImGui::PushFont(nullptr, fonts().size * 0.84f);
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted(text);
  ImGui::PopTextWrapPos();
  ImGui::PopFont();
  ImGui::PopStyleColor();
}

void Ui::Spacer(float height) {
  LeaveRows();
  ImGui::Dummy(ImVec2(0, height * scale_));
}

bool Ui::Section(const char* title, bool open_by_default) {
  LeaveRows();
  EndCard();
  ImGui::PushFont(fonts().semibold, 0.0f);
  const bool open = ImGui::CollapsingHeader(title, open_by_default ? ImGuiTreeNodeFlags_DefaultOpen : 0);
  ImGui::PopFont();
  return open;
}

void Ui::AchievementSummary(int unlocked, int total, int points, int total_points) {
  LeaveRows();
  ImGui::PushFont(fonts().semibold, 0.0f);
  if (total_points > 0)
    ImGui::Text("%d / %d unlocked     %d / %d G", unlocked, total, points, total_points);
  else
    ImGui::Text("%d / %d unlocked", unlocked, total);
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(0, 4 * scale_));
}

void Ui::AchievementCardView(const AchievementCard& a, float width) {
  LeaveRows();
  const Theme& t = theme();
  const Fonts& f = fonts();
  const float s = scale_, base = ImGui::GetFontSize();
  const float card_w = width > 0 ? width : ImGui::GetContentRegionAvail().x;
  const float card_h = 86 * s, icon = 60 * s;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(card_w, card_h));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + card_w, p.y + card_h), Col(a.unlocked ? t.frame_hot : t.frame), 10 * s);
  if (a.unlocked)
    dl->AddRectFilled(p, ImVec2(p.x + 4 * s, p.y + card_h), Col(t.accent), 10 * s, ImDrawFlags_RoundCornersLeft);
  float tx = p.x + 18 * s;
  if (a.icon != ImTextureID{}) {
    const ImVec2 i0(p.x + 14 * s, p.y + (card_h - icon) * 0.5f);
    dl->AddImageRounded(ImTextureRef(a.icon), i0, ImVec2(i0.x + icon, i0.y + icon), ImVec2(0, 0), ImVec2(1, 1),
                        a.unlocked ? IM_COL32_WHITE : IM_COL32(105, 110, 125, 200), 8 * s);
    tx = i0.x + icon + 14 * s;
  }
  const float right = p.x + card_w - 14 * s;
  dl->AddText(f.semibold, base, ImVec2(tx, p.y + 12 * s), Col(a.unlocked ? t.text : t.dim), a.title.c_str());
  if (a.points > 0) {
    const std::string g = std::to_string(a.points) + " G";
    const ImVec2 gs = f.semibold->CalcTextSizeA(base * 0.9f, FLT_MAX, 0, g.c_str());
    dl->AddText(f.semibold, base * 0.9f, ImVec2(right - gs.x, p.y + 13 * s), Col(a.unlocked ? t.accent : t.dim),
                g.c_str());
  }
  const std::string& desc = a.unlocked || a.locked_description.empty() ? a.description : a.locked_description;
  dl->AddText(f.regular, base * 0.84f, ImVec2(tx, p.y + 38 * s), Col(t.dim), desc.c_str(), nullptr, right - tx);
  if (a.unlocked)
    dl->AddText(f.semibold, base * 0.72f, ImVec2(tx, p.y + card_h - 22 * s), Col(t.accent), "UNLOCKED");
}

void Ui::AchievementGrid(const std::vector<AchievementCard>& cards, int max_columns) {
  LeaveRows();
  const float avail = ImGui::GetContentRegionAvail().x;
  const int cols = std::max(1, std::min(max_columns, avail > 700 * scale_ ? 2 : 1));
  const float gap = 12 * scale_;
  const float card_w = (avail - gap * float(cols - 1)) / float(cols);
  for (size_t i = 0; i < cards.size(); ++i) {
    if (i % cols) ImGui::SameLine(0, gap);
    ImGui::PushID(int(i));
    AchievementCardView(cards[i], card_w);
    ImGui::PopID();
    if (i % cols == size_t(cols - 1) || i + 1 == cards.size()) ImGui::Dummy(ImVec2(0, gap * 0.5f));
  }
}

// ---------------------------------------------------------------- launcher --
Launcher::Launcher(LauncherConfig config) : config_(std::move(config)) {
  ui_.launcher_ = this;
  if (config_.fonts.size <= 0.0f) config_.fonts.size = 18.0f;
  page_ = std::clamp(config_.start_page, 0, std::max(0, int(config_.pages.size()) - 1));
  if (config_.settings)
    for (const std::string& key : config_.restart_keys) restart_baseline_.push_back(config_.settings->Get(key));
}

void Launcher::ShowPrompt(PlayPrompt prompt) {
  bool has_back = false;
  for (const PromptButton& b : prompt.buttons) has_back |= !b.play;
  if (!has_back) prompt.buttons.push_back({"Back", false, false, {}});
  prompt_ = std::move(prompt);
  open_prompt_ = true;
}

bool Launcher::restart_needed() const {
  for (size_t i = 0; i < restart_baseline_.size(); ++i)
    if (config_.settings->Get(config_.restart_keys[i]) != restart_baseline_[i]) return true;
  return false;
}

void Launcher::DrawPrompt() {
  if (!prompt_) return;
  const Theme& t = config_.theme;
  const char* id = "##trg_prompt";
  if (open_prompt_) {
    ImGui::OpenPopup(id);
    open_prompt_ = false;
  }
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(600 * s_, vp->Size.x - 40 * s_), 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * s_, 20 * s_));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12 * s_);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, t.panel);
  ImGui::PushStyleColor(ImGuiCol_Border, t.warn);
  const bool open = ImGui::BeginPopupModal(id, nullptr,
                                           ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(3);
  if (!open) {
    prompt_.reset();  // closed some other way
    return;
  }
  const PlayPrompt& p = *prompt_;
  ImGui::PushFont(config_.fonts.bold, ImGui::GetFontSize() * 1.2f);
  ImGui::TextUnformatted(p.title.c_str());
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(0, 6 * s_));
  ImGui::PushTextWrapPos(0.0f);
  for (const std::string& para : p.paragraphs) {
    ImGui::TextUnformatted(para.c_str());
    ImGui::Dummy(ImVec2(0, 6 * s_));
  }
  if (!p.footnote.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, t.dim);
    ImGui::TextUnformatted(p.footnote.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::PopTextWrapPos();
  ImGui::Dummy(ImVec2(0, 10 * s_));
  // Buttons that start the game share the first line; the others go below, full width.
  std::vector<const PromptButton*> play_buttons, other;
  for (const PromptButton& b : p.buttons) (b.play ? play_buttons : other).push_back(&b);
  const PromptButton* chosen = nullptr;
  const float gap = 8 * s_;
  const float avail = ImGui::GetContentRegionAvail().x;
  for (size_t i = 0; i < play_buttons.size(); ++i) {
    if (i) ImGui::SameLine(0, gap);
    const float bw = (avail - gap * float(play_buttons.size() - 1)) / float(play_buttons.size());
    const PromptButton& b = *play_buttons[i];
    ImGui::PushID(int(i));
    if (b.accent ? ui_.AccentButton(b.label.c_str(), ImVec2(bw, 0)) : ImGui::Button(b.label.c_str(), ImVec2(bw, 0)))
      chosen = &b;
    ImGui::PopID();
  }
  for (size_t i = 0; i < other.size(); ++i) {
    ImGui::PushID(int(100 + i));
    if (other[i]->accent ? ui_.AccentButton(other[i]->label.c_str(), ImVec2(-FLT_MIN, 0))
                         : ImGui::Button(other[i]->label.c_str(), ImVec2(-FLT_MIN, 0)))
      chosen = other[i];
    ImGui::PopID();
  }
  if (!chosen && !other.empty() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) chosen = other.front();
  if (chosen) {
    const PromptButton b = *chosen;  // copied: the prompt is reset below
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    prompt_.reset();
    if (b.action) b.action();
    if (b.play) {
      prompt_confirmed_ = true;
      want_play_ = true;
    }
    return;
  }
  ImGui::EndPopup();
}

Launcher::~Launcher() {
  if (nav_suspended_) ImGui::GetIO().ConfigFlags |= saved_nav_flags_;
}

int Launcher::FindPage(std::string_view name) const {
  for (size_t i = 0; i < config_.pages.size(); ++i)
    if (config_.pages[i].name == name) return int(i);
  return -1;
}

void Launcher::GoToPage(int index) {
  if (index < 0 || index >= int(config_.pages.size())) return;
  page_ = index;
  ui_.capturing_.clear();
}

void Launcher::SetStatus(std::string text, double seconds) {
  status_ = std::move(text);
  status_until_ = ImGui::GetTime() + seconds;
}

void Launcher::DropFile(const std::string& path) {
  if (config_.on_file_drop) config_.on_file_drop(path);
}

bool Launcher::Save() {
  bool ok = config_.settings && config_.settings->Save();
  if (config_.on_save) ok = config_.on_save() && ok;
  SetStatus(ok ? "Settings saved." : "Could not save the settings.", ok ? 3.0 : 6.0);
  return ok;
}

Result Launcher::Frame() {
  IM_ASSERT(config_.settings && "LauncherConfig::settings is required");
  Fonts& fonts = config_.fonts;
  if (!fonts.regular) fonts.regular = ImGui::GetFont();
  if (!fonts.bold) fonts.bold = fonts.semibold ? fonts.semibold : fonts.regular;
  if (!fonts.semibold) fonts.semibold = fonts.bold;

  const ImGuiStyle saved_style = ImGui::GetStyle();
  ImGui::PushFont(fonts.regular, fonts.size);
  s_ = ImGui::GetFontSize() / fonts.size;
  ui_.scale_ = s_;
  ApplyTheme(config_.theme, s_);

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize(vp->Size);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##trg_launcher", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::PopStyleVar();
  const float w = vp->Size.x, h = vp->Size.y;
  const float sidebar_w = std::min(250 * s_, w * 0.32f);
  const float top_h = 40 * s_, footer_h = 56 * s_;
  const ImVec2 o = vp->Pos;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), Col(config_.theme.bg));
  // The main area: the mockup's rgba(14, 14, 18, .7) panel over the dark backdrop.
  dl->AddRectFilledMultiColor(ImVec2(o.x + sidebar_w, o.y), ImVec2(o.x + w, o.y + h), IM_COL32(17, 15, 14, 255),
                              IM_COL32(13, 13, 17, 255), IM_COL32(12, 12, 16, 255), IM_COL32(14, 13, 15, 255));

  DrawSidebar(o, sidebar_w, h);
  DrawTopBar(ImVec2(o.x + sidebar_w, o.y), w - sidebar_w, top_h);
  DrawContent(ImVec2(o.x + sidebar_w, o.y + top_h), w - sidebar_w, h - top_h - footer_h);
  DrawFooter(ImVec2(o.x + sidebar_w, o.y + h - footer_h), w - sidebar_w, footer_h);
  HandleHotkeys();
  DrawPrompt();
  ImGui::End();
  ImGui::PopFont();
  ImGui::GetStyle() = saved_style;

  // Arrow keys must not move the keyboard focus while a key is being bound.
  ImGuiIO& io = ImGui::GetIO();
  if (capturing_key() && !nav_suspended_) {
    saved_nav_flags_ = io.ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    nav_suspended_ = true;
  } else if (!capturing_key() && nav_suspended_) {
    io.ConfigFlags |= saved_nav_flags_;
    nav_suspended_ = false;
  }

  if (want_quit_) {
    want_quit_ = want_play_ = false;
    return Result::kQuit;
  }
  if (want_play_) {
    want_play_ = false;
    return TryPlay();
  }
  return Result::kNone;
}

Result Launcher::TryPlay() {
  const PlayCheck check = config_.can_play ? config_.can_play() : PlayCheck{};
  if (!check.ok) {
    if (!check.reason.empty()) SetStatus(check.reason, 5.0);
    GoToPage(check.page);
    return Result::kNone;
  }
  if (!prompt_confirmed_ && config_.before_play) {
    if (auto prompt = config_.before_play()) {
      ShowPrompt(std::move(*prompt));
      return Result::kNone;
    }
  }
  prompt_confirmed_ = false;
  if (config_.save_on_play) {
    bool ok = config_.settings->Save();
    if (config_.on_save) ok = config_.on_save() && ok;
    if (!ok) std::fprintf(stderr, "[trg-launcher] could not save the settings\n");
  }
  return Result::kPlay;
}

namespace {

// Small line icons for the sidebar, drawn with the draw list (the mockup uses emoji, which ImGui cannot colour).
void DrawNavIcon(ImDrawList* dl, ImVec2 c, float u, const std::string& name, ImU32 col) {
  const float r = 6 * u, lw = 1.5f * u;
  if (name == "Play") {
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.55f, c.y - r), ImVec2(c.x - r * 0.55f, c.y + r), ImVec2(c.x + r, c.y), col);
  } else if (name == "Display") {
    dl->AddRect(ImVec2(c.x - r, c.y - r * 0.8f), ImVec2(c.x + r, c.y + r * 0.5f), col, 1.5f * u, 0, lw);
    dl->AddLine(ImVec2(c.x - r * 0.5f, c.y + r), ImVec2(c.x + r * 0.5f, c.y + r), col, lw);
  } else if (name == "Graphics") {
    dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x - r, c.y + r), col, lw);
    dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y + r), col, lw);
    const ImVec2 pts[] = {ImVec2(c.x - r * 0.6f, c.y + r * 0.3f), ImVec2(c.x - r * 0.1f, c.y - r * 0.2f),
                          ImVec2(c.x + r * 0.2f, c.y + r * 0.1f), ImVec2(c.x + r, c.y - r * 0.7f)};
    dl->AddPolyline(pts, 4, col, 0, lw);
  } else if (name == "Gameplay") {
    dl->AddCircle(c, r * 0.55f, col, 12, lw);
    for (int i = 0; i < 8; ++i) {
      const float a = float(i) * 0.7853982f;
      dl->AddLine(ImVec2(c.x + std::cos(a) * r * 0.8f, c.y + std::sin(a) * r * 0.8f),
                  ImVec2(c.x + std::cos(a) * r * 1.1f, c.y + std::sin(a) * r * 1.1f), col, lw * 1.2f);
    }
  } else if (name == "Cheats") {
    dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), col, lw * 1.3f);
    dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), col, lw * 1.3f);
  } else if (name == "Controls") {
    dl->AddRect(ImVec2(c.x - r, c.y - r * 0.6f), ImVec2(c.x + r, c.y + r * 0.7f), col, 1.5f * u, 0, lw);
    for (int i = 0; i < 3; ++i) dl->AddCircleFilled(ImVec2(c.x - r * 0.55f + float(i) * r * 0.55f, c.y - r * 0.15f), 0.8f * u, col);
    dl->AddLine(ImVec2(c.x - r * 0.5f, c.y + r * 0.35f), ImVec2(c.x + r * 0.5f, c.y + r * 0.35f), col, lw);
  } else if (name == "Trophies") {
    const ImVec2 cup[] = {ImVec2(c.x - r * 0.7f, c.y - r), ImVec2(c.x + r * 0.7f, c.y - r), ImVec2(c.x + r * 0.45f, c.y + r * 0.2f),
                          ImVec2(c.x - r * 0.45f, c.y + r * 0.2f)};
    dl->AddPolyline(cup, 4, col, ImDrawFlags_Closed, lw);
    dl->AddLine(ImVec2(c.x, c.y + r * 0.2f), ImVec2(c.x, c.y + r * 0.8f), col, lw);
    dl->AddLine(ImVec2(c.x - r * 0.5f, c.y + r), ImVec2(c.x + r * 0.5f, c.y + r), col, lw * 1.2f);
  } else if (name == "About") {
    dl->AddCircle(c, r, col, 20, lw);
    dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.45f), 0.9f * u, col);
    dl->AddLine(ImVec2(c.x, c.y - r * 0.1f), ImVec2(c.x, c.y + r * 0.55f), col, lw);
  } else {
    dl->AddCircleFilled(c, 2 * u, col);
  }
}

// The hints string ("Enter  Play        Esc  Quit ...") as key/label pairs: runs of two or more spaces separate the tokens.
std::vector<std::pair<std::string, std::string>> ParseHints(const std::string& hints) {
  std::vector<std::string> tokens;
  size_t i = 0;
  while (i < hints.size()) {
    while (i < hints.size() && hints[i] == ' ') ++i;
    size_t j = i;
    while (j < hints.size() && !(hints[j] == ' ' && j + 1 < hints.size() && hints[j + 1] == ' ')) ++j;
    size_t end = j;
    while (end > i && hints[end - 1] == ' ') --end;
    if (end > i) tokens.push_back(hints.substr(i, end - i));
    i = j;
  }
  std::vector<std::pair<std::string, std::string>> out;
  for (size_t k = 0; k + 1 < tokens.size(); k += 2) out.emplace_back(tokens[k], tokens[k + 1]);
  return out;
}

}  // namespace

void Launcher::DrawSidebar(ImVec2 o, float w, float h) {
  const Theme& t = config_.theme;
  const Branding& b = config_.branding;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), Col(t.sidebar));
  dl->AddLine(ImVec2(o.x + w - 0.5f, o.y), ImVec2(o.x + w - 0.5f, o.y + h), Col(t.border));

  // Brand: logo box, title in the heading font, subtitle.
  const float brand_h = 80 * s_;
  const ImVec2 lo(o.x + 20 * s_, o.y + 16 * s_), lh(lo.x + 48 * s_, lo.y + 48 * s_);
  if (b.icon != ImTextureID{}) {
    dl->AddImageRounded(ImTextureRef(b.icon), lo, lh, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 6 * s_);
  } else {
    dl->AddRectFilled(lo, lh, Col(t.input), 6 * s_);
    const ImVec2 ts = config_.fonts.heading->CalcTextSizeA(20 * s_, FLT_MAX, 0, "DC");
    dl->AddText(config_.fonts.heading, 20 * s_, ImVec2(lo.x + (48 * s_ - ts.x) * 0.5f, lo.y + (48 * s_ - ts.y) * 0.5f),
                Col(t.accent), "DC");
  }
  dl->AddRect(lo, lh, Col(t.accent_dark), 6 * s_, 0, 1.0f);
  const float tx = lh.x + 12 * s_;
  dl->AddText(config_.fonts.heading, 13 * s_, ImVec2(tx, lo.y + 8 * s_), Col(t.accent), b.title.c_str());
  dl->AddText(config_.fonts.regular, 10 * s_, ImVec2(tx, lo.y + 26 * s_), Col(t.dim), b.subtitle.c_str());
  dl->AddLine(ImVec2(o.x, o.y + brand_h), ImVec2(o.x + w, o.y + brand_h), Col(t.border));

  // Navigation.
  float y = o.y + brand_h + 20 * s_;
  dl->AddText(config_.fonts.semibold, 11 * s_, ImVec2(o.x + 24 * s_, y), Col(t.dim), "LAUNCHER");
  y += 11 * s_ + 10 * s_ + 4 * s_;
  const float item_h = 34 * s_, gap = 2 * s_, x0 = o.x + 12 * s_, iw = w - 24 * s_;
  ImGui::PushFont(config_.fonts.semibold, 0.0f);
  for (int i = 0; i < int(config_.pages.size()); ++i) {
    const Page& page = config_.pages[size_t(i)];
    const bool selected = page_ == i;
    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("##page", ImVec2(iw, item_h))) GoToPage(i);
    const bool hot = ImGui::IsItemHovered();
    const ImVec2 a(x0, y), z(x0 + iw, y + item_h);
    if (selected) {
      dl->AddRectFilled(a, z, Col(t.accent, 0.14f), 4 * s_);
      dl->AddRect(a, z, Col(t.accent_dark), 4 * s_);
    } else if (hot) {
      dl->AddRectFilled(a, z, Col(t.accent, 0.08f), 4 * s_);
    }
    const ImU32 ink = Col(selected || hot ? t.accent_hot : t.dim);
    DrawNavIcon(dl, ImVec2(x0 + 14 * s_ + 8 * s_, y + item_h * 0.5f), s_, page.name, ink);
    dl->AddText(ImVec2(x0 + 14 * s_ + 16 * s_ + 10 * s_, y + (item_h - ImGui::GetFontSize()) * 0.5f), ink,
                page.name.c_str());
    if (page.needs_attention && page.needs_attention())
      dl->AddCircleFilled(ImVec2(z.x - 14 * s_, y + item_h * 0.5f), 3.5f * s_, Col(t.warn_dot));
    ImGui::PopID();
    y += item_h + gap;
  }
  ImGui::PopFont();

  // Footer: a status dot and a line of text.
  const float foot_h = 38 * s_;
  dl->AddLine(ImVec2(o.x, o.y + h - foot_h), ImVec2(o.x + w, o.y + h - foot_h), Col(t.border));
  std::pair<Status, std::string> st{Status::kReady, ""};
  if (config_.sidebar_status) {
    st = config_.sidebar_status();
  } else if (config_.can_play) {
    const PlayCheck check = config_.can_play();
    st = {check.ok ? Status::kReady : Status::kAttention, check.ok ? "READY TO PLAY" : "GAME FILES NEEDED"};
  }
  const ImVec4 dot = st.first == Status::kReady ? t.good_dot : st.first == Status::kBusy ? t.accent
                                                                                         : ImVec4(0.878f, 0.624f, 0.243f, 1.0f);
  const float cy = o.y + h - foot_h * 0.5f;
  dl->AddCircleFilled(ImVec2(o.x + 20 * s_ + 3.5f * s_, cy), 3.5f * s_, Col(dot), 16);
  dl->AddText(config_.fonts.regular, 11 * s_, ImVec2(o.x + 20 * s_ + 15 * s_, cy - 7 * s_), Col(t.dim), st.second.c_str());
}

void Launcher::DrawTopBar(ImVec2 o, float w, float h) {
  const Theme& t = config_.theme;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), IM_COL32(18, 18, 22, 217));
  dl->AddLine(ImVec2(o.x, o.y + h - 0.5f), ImVec2(o.x + w, o.y + h - 0.5f), Col(t.border));
  const std::string root = config_.breadcrumb_root.empty() ? config_.branding.title : config_.breadcrumb_root;
  const float fs = 12 * s_, y = o.y + (h - fs) * 0.5f - 1 * s_;
  float x = o.x + 28 * s_;
  dl->AddText(config_.fonts.regular, fs, ImVec2(x, y), Col(t.dim), root.c_str());
  x += config_.fonts.regular->CalcTextSizeA(fs, FLT_MAX, 0, root.c_str()).x + 8 * s_;
  dl->AddText(config_.fonts.regular, fs, ImVec2(x, y), Col(t.dim), "/");
  x += 12 * s_;
  if (!config_.pages.empty())
    dl->AddText(config_.fonts.semibold, fs, ImVec2(x, y), Col(t.accent), config_.pages[size_t(page_)].name.c_str());
}

void Launcher::DrawContent(ImVec2 o, float w, float h) {
  if (config_.pages.empty()) return;
  const Page& page = config_.pages[size_t(page_)];
  ImGui::SetCursorScreenPos(o);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
  ImGui::BeginChild("##content", ImVec2(w, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
  const float padx = 28 * s_, pady = 24 * s_;
  ImGui::SetCursorPos(ImVec2(padx, pady));
  ImGui::BeginGroup();
  std::string title = page.name;
  for (char& c : title) c = char(std::toupper(static_cast<unsigned char>(c)));
  ImGui::PushStyleColor(ImGuiCol_Text, config_.theme.accent);
  ImGui::PushFont(config_.fonts.heading, config_.fonts.size * 24.0f / 13.0f);
  ImGui::TextUnformatted(title.c_str());
  ImGui::PopFont();
  ImGui::PopStyleColor();
  if (!page.blurb.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, config_.theme.dim);
    ImGui::PushFont(nullptr, config_.fonts.size * 12.0f / 13.0f);
    ImGui::TextUnformatted(page.blurb.c_str());
    ImGui::PopFont();
    ImGui::PopStyleColor();
  }
  ImGui::EndGroup();

  ImGui::SetCursorPos(ImVec2(padx, ImGui::GetCursorPosY() + 8 * s_));
  // The scrollbar's track sits in the right margin and is always reserved, so the cards keep one width.
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
  ImGui::BeginChild("##page_body", ImVec2(w - padx * 2 + 8 * s_, h - ImGui::GetCursorPosY() - 8 * s_),
                    ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);
  ImGui::PopStyleColor();
  ImGui::PushID(page_);
  ui_.BeginPage();
  if (page.draw) page.draw(ui_);
  ui_.EndPage();
  ImGui::PopID();
  ImGui::Dummy(ImVec2(0, 8 * s_));
  ImGui::EndChild();
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

void Launcher::DrawFooter(ImVec2 o, float w, float h) {
  const Theme& t = config_.theme;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), IM_COL32(14, 14, 17, 255));
  dl->AddLine(ImVec2(o.x, o.y + 0.5f), ImVec2(o.x + w, o.y + 0.5f), Col(t.border));

  // Keyboard hints: gold key caps with a label.
  const float fs = 11 * s_, cy = o.y + h * 0.5f;
  float x = o.x + 28 * s_;
  for (const auto& [key, label] : ParseHints(config_.hints)) {
    const float kf = 10 * s_;
    const ImVec2 ks = config_.fonts.bold->CalcTextSizeA(kf, FLT_MAX, 0, key.c_str());
    dl->AddRectFilled(ImVec2(x, cy - ks.y * 0.5f - 2 * s_), ImVec2(x + ks.x + 12 * s_, cy + ks.y * 0.5f + 2 * s_),
                      Col(t.accent), 3 * s_);
    dl->AddText(config_.fonts.bold, kf, ImVec2(x + 6 * s_, cy - ks.y * 0.5f), Col(t.on_accent), key.c_str());
    x += ks.x + 12 * s_ + 6 * s_;
    const ImVec2 ls = config_.fonts.regular->CalcTextSizeA(fs, FLT_MAX, 0, label.c_str());
    dl->AddText(config_.fonts.regular, fs, ImVec2(x, cy - ls.y * 0.5f), Col(t.dim), label.c_str());
    x += ls.x + 20 * s_;
  }

  // Buttons at the right: Quit, Save, PLAY.
  const float bh = 32 * s_, bw = 84 * s_, play_w = 130 * s_;
  const float right = o.x + w - 28 * s_;
  const bool show_status = !status_.empty() && ImGui::GetTime() < status_until_;
  const std::string& note = show_status ? status_ : config_.note;
  const float note_x = x + 8 * s_, note_right = right - play_w - bw * 2 - 30 * s_;
  if (!note.empty() && note_right - note_x > 80 * s_) {
    ImGui::PushClipRect(ImVec2(note_x, o.y), ImVec2(note_right, o.y + h), true);
    dl->AddText(show_status ? config_.fonts.semibold : config_.fonts.regular, fs,
                ImVec2(note_x, cy - fs * 0.5f - 1 * s_), Col(show_status ? t.accent : t.dim), note.c_str());
    ImGui::PopClipRect();
  }
  ImGui::SetCursorScreenPos(ImVec2(right - play_w - bw * 2 - 20 * s_, cy - bh * 0.5f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14 * s_, 8 * s_));
  ImGui::PushStyleColor(ImGuiCol_Border, t.border);
  if (ImGui::Button("Quit", ImVec2(bw, bh))) want_quit_ = true;
  ImGui::SameLine(0, 10 * s_);
  if (ImGui::Button("Save", ImVec2(bw, bh))) Save();
  ImGui::SameLine(0, 10 * s_);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  const PlayCheck check = config_.can_play ? config_.can_play() : PlayCheck{};
  DrawPlayButton(ImVec2(play_w, bh), check.ok);
}

void Launcher::DrawPlayButton(ImVec2 size, bool can_play) {
  const Theme& t = config_.theme;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  // Still clickable when the game cannot start: that explains why.
  const bool clicked = ImGui::InvisibleButton("##play", size);
  const bool hot = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 q(p.x + size.x, p.y + size.y);
  if (can_play) {
    dl->AddRectFilled(p, q, Col(hot ? t.accent_hot : t.accent), 4 * s_);
  } else {  // .btn-play-disabled
    dl->AddRectFilled(p, q, IM_COL32(34, 34, 40, 255), 4 * s_);
    dl->AddRect(p, q, IM_COL32(51, 51, 62, 255), 4 * s_);
  }
  const float fs = 15 * s_;
  const ImVec2 ts = config_.fonts.heading->CalcTextSizeA(fs, FLT_MAX, 0, "PLAY");
  dl->AddText(config_.fonts.heading, fs, ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f),
              can_play ? Col(t.on_accent) : IM_COL32(85, 85, 96, 255), "PLAY");
  if (clicked) want_play_ = true;
}

void Launcher::HandleHotkeys() {
  const ImGuiIO& io = ImGui::GetIO();
  if (capturing_key() || io.WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
  if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)) {
    want_play_ = true;
  } else if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
    // Enter activates the focused control while navigating with the keyboard.
    if (!io.NavVisible && !ImGui::IsAnyItemActive()) want_play_ = true;
  } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    want_quit_ = true;
  } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
    Save();
  }
}

// ------------------------------------------------------------------ helpers --
bool ShouldShowLauncher(const SettingsStore& settings, std::string_view key, bool game_ready, bool force) {
  if (force || !game_ready || ShiftHeld()) return true;
  return settings.GetBool(key, true);
}

int ImGuiKeyToVirtualKey(ImGuiKey k) {
  if (k >= ImGuiKey_A && k <= ImGuiKey_Z) return 'A' + (k - ImGuiKey_A);
  if (k >= ImGuiKey_0 && k <= ImGuiKey_9) return '0' + (k - ImGuiKey_0);
  if (k >= ImGuiKey_F1 && k <= ImGuiKey_F24) return 0x70 + (k - ImGuiKey_F1);
  if (k >= ImGuiKey_Keypad0 && k <= ImGuiKey_Keypad9) return 0x60 + (k - ImGuiKey_Keypad0);
  switch (k) {
    case ImGuiKey_Tab: return 0x09;
    case ImGuiKey_LeftArrow: return 0x25;
    case ImGuiKey_RightArrow: return 0x27;
    case ImGuiKey_UpArrow: return 0x26;
    case ImGuiKey_DownArrow: return 0x28;
    case ImGuiKey_PageUp: return 0x21;
    case ImGuiKey_PageDown: return 0x22;
    case ImGuiKey_Home: return 0x24;
    case ImGuiKey_End: return 0x23;
    case ImGuiKey_Insert: return 0x2D;
    case ImGuiKey_Delete: return 0x2E;
    case ImGuiKey_Backspace: return 0x08;
    case ImGuiKey_Space: return 0x20;
    case ImGuiKey_Enter:
    case ImGuiKey_KeypadEnter: return 0x0D;
    case ImGuiKey_Escape: return 0x1B;
    case ImGuiKey_CapsLock: return 0x14;
    case ImGuiKey_LeftShift: return 0xA0;
    case ImGuiKey_RightShift: return 0xA1;
    case ImGuiKey_LeftCtrl: return 0xA2;
    case ImGuiKey_RightCtrl: return 0xA3;
    case ImGuiKey_LeftAlt: return 0xA4;
    case ImGuiKey_RightAlt: return 0xA5;
    case ImGuiKey_LeftSuper: return 0x5B;
    case ImGuiKey_RightSuper: return 0x5C;
    case ImGuiKey_Menu: return 0x5D;
    case ImGuiKey_Semicolon: return 0xBA;
    case ImGuiKey_Equal: return 0xBB;
    case ImGuiKey_Comma: return 0xBC;
    case ImGuiKey_Minus: return 0xBD;
    case ImGuiKey_Period: return 0xBE;
    case ImGuiKey_Slash: return 0xBF;
    case ImGuiKey_GraveAccent: return 0xC0;
    case ImGuiKey_LeftBracket: return 0xDB;
    case ImGuiKey_Backslash: return 0xDC;
    case ImGuiKey_RightBracket: return 0xDD;
    case ImGuiKey_Apostrophe: return 0xDE;
    case ImGuiKey_KeypadDecimal: return 0x6E;
    case ImGuiKey_KeypadDivide: return 0x6F;
    case ImGuiKey_KeypadMultiply: return 0x6A;
    case ImGuiKey_KeypadSubtract: return 0x6D;
    case ImGuiKey_KeypadAdd: return 0x6B;
    default: return 0;
  }
}

}  // namespace trg
