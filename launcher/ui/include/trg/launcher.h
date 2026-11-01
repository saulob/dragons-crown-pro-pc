// Forked from TRG Launcher (TekRantGaming/trg-launcher, pinned in gpu/deps.txt) and restyled for Dragon's Crown Pro PC.
// The upstream repository states no licence: see THIRD-PARTY-LICENSES.txt.
// TRG Launcher: the pre-game launcher shared by TekRant Gaming PC ports.
//
// A game describes its launcher (title, theme, header art, pages of settings)
// in a LauncherConfig and calls Launcher::Frame() once per frame inside an
// existing Dear ImGui frame. The launcher fills the main viewport with the
// standard layout:
//
//   +----------------------------------------------------------------+
//   |  [icon]  GAME TITLE                         (animated header)  |
//   |          PC PORT  ·  LAUNCHER                                  |
//   +-----------+----------------------------------------------------+
//   |  Page 1   |  Page title                                        |
//   |  Page 2   |  One-line description                              |
//   |  ...      |  Setting label / help text      [ Off ][ On ]      |
//   +-----------+----------------------------------------------------+
//   |  hints / status                     [Quit] [Save] [ PLAY > ]   |
//   +----------------------------------------------------------------+
//
// Games that already run Dear ImGui (ReXGlue titles, ...) call Frame() from
// their own overlay; others use RunStandalone() (standalone.h), which opens
// its own SDL2 + OpenGL 3 window before the game starts.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>

#include "trg/settings.h"

#if !defined(IMGUI_VERSION_NUM) || IMGUI_VERSION_NUM < 19200
#error "TRG Launcher needs Dear ImGui 1.92 or newer"
#endif

namespace trg {

class Launcher;
class Task;
class Ui;

// ------------------------------------------------------------------ theme ---
struct Theme {
  ImVec4 bg;            // window background, behind everything
  ImVec4 panel;         // sidebar and page panels
  ImVec4 frame;         // buttons, combos, sliders
  ImVec4 frame_hot;     // hovered
  ImVec4 frame_active;  // pressed
  ImVec4 popup;
  ImVec4 accent;        // selected page, chosen option, PLAY
  ImVec4 accent_hot;
  ImVec4 on_accent;     // text drawn on the accent
  ImVec4 text;
  ImVec4 dim;           // help text, hints
  ImVec4 good;          // "Ready to play" card
  ImVec4 warn;          // "Game files needed" card
  ImVec4 good_dot;
  ImVec4 warn_dot;      // also the sidebar's attention dot
  // Used by the Dragon's Crown Pro layout (sidebar, cards, inputs).
  ImVec4 sidebar = ImVec4(0.090f, 0.090f, 0.110f, 1.0f);       // #17171c
  ImVec4 card = ImVec4(0.102f, 0.102f, 0.125f, 0.92f);          // rgba(26, 26, 32, .92)
  ImVec4 border = ImVec4(0.169f, 0.165f, 0.200f, 1.0f);         // #2b2a33
  ImVec4 input = ImVec4(0.071f, 0.071f, 0.090f, 1.0f);          // #121217
  ImVec4 accent_dark = ImVec4(0.549f, 0.431f, 0.149f, 1.0f);    // #8c6e26, outlines on gold

  // Deep space blue with a green accent (Outpost Kaloki X).
  static Theme Midnight();
  // Ocean blue with a sunshine-yellow accent (Super Mario Sunshine).
  static Theme Ocean();
  // Charcoal with a red accent.
  static Theme Ember();
  // Dragon's Crown Pro: near-black with the launcher's gold (#e2c068).
  static Theme DragonsCrown();
  // The same theme with another accent; hover and text-on-accent colours are
  // derived from it.
  Theme WithAccent(ImVec4 accent) const;
};

ImU32 Col(const ImVec4& c, float alpha_mul = 1.0f);

// ------------------------------------------------------------------ fonts ---
struct Fonts {
  ImFont* regular = nullptr;   // body text (nullptr: ImGui's current font)
  ImFont* semibold = nullptr;  // labels and page names (nullptr: bold)
  ImFont* bold = nullptr;      // titles and PLAY (nullptr: regular)
  ImFont* heading = nullptr;   // page titles and the brand (nullptr: bold)
  ImFont* mono = nullptr;      // values, keys and build ids (nullptr: regular)
  float size = 18.0f;          // base size the layout is designed around
};

// Font files held in memory (the launcher embeds them in the executable).
struct FontBlob {
  const void* data = nullptr;
  size_t size = 0;
};
struct FontSet {
  FontBlob regular, semibold, bold, heading, mono;
};
// Adds the fonts to the current atlas; any that is missing or fails to load falls back to
// LoadSystemFonts(). Call with a context and before the first frame.
Fonts LoadFonts(const FontSet& set, float size = 13.0f);

// Adds Segoe UI (Windows), DejaVu Sans (Linux) or Arial (macOS) to the
// current ImGui font atlas. Call with a context and before the first frame.
Fonts LoadSystemFonts(float size = 18.0f);

// ----------------------------------------------------------------- header ---
struct HeaderContext {
  ImDrawList* draw;
  ImVec2 min, max;  // the header's rectangle
  float time;       // seconds, for animation
  float scale;      // UI scale (1 at 96 DPI)
  const Theme& theme;
};
using HeaderPainter = std::function<void(const HeaderContext&)>;

struct Branding {
  std::string title;                                     // "OUTPOST KALOKI X"
  std::string subtitle = "PC PORT   \xC2\xB7   LAUNCHER";
  ImTextureID icon{};                                    // optional square icon left of the title
  HeaderPainter background;                              // see headers.h; default: a theme gradient
  bool shade_title_side = true;                          // darken behind the title for contrast
  bool fade_into_page = true;                            // fade the header's bottom into the page
  std::optional<ImVec4> subtitle_color;                  // default: the accent
  ImU32 title_shadow = IM_COL32(0, 0, 0, 150);
};

// ------------------------------------------------------------------ pages ---
struct Page {
  std::string name;   // sidebar entry and page title
  std::string blurb;  // one line under the title
  std::function<void(Ui&)> draw;
  std::function<bool()> needs_attention;  // shows a dot in the sidebar (e.g. game not installed)
};

struct Option {
  std::string value;  // what is stored
  std::string label;  // what is shown
};

struct PlayCheck {
  bool ok = true;
  std::string reason;  // shown when PLAY is pressed but the game cannot start
  int page = -1;       // page to switch to then (e.g. the install page)
};

// A pop-up shown when PLAY is pressed, before the game starts: for a setting
// the player should know about (King Kong warns about frame rates above 30).
struct PromptButton {
  std::string label;
  bool accent = false;           // drawn in the accent colour (the recommended choice)
  bool play = true;              // start the game after `action`; false just closes the pop-up
  std::function<void()> action;  // runs first, e.g. to change a setting
};
struct PlayPrompt {
  std::string title;
  std::vector<std::string> paragraphs;
  std::string footnote;               // dim, after the paragraphs
  std::vector<PromptButton> buttons;  // a "Back" button is added unless one already has play = false
};

// One card on an achievements page.
struct AchievementCard {
  ImTextureID icon{};       // optional; drawn greyed out while locked
  std::string title;
  std::string description;  // shown when unlocked, or always when `locked_description` is empty
  std::string locked_description;
  int points = 0;           // gamerscore; 0 hides it
  bool unlocked = false;
};

enum class Result { kNone, kPlay, kQuit };

enum class Status { kReady, kAttention, kBusy };

struct LauncherConfig {
  Branding branding;
  Theme theme = Theme::Midnight();
  Fonts fonts;
  SettingsStore* settings = nullptr;  // required; not owned
  std::vector<Page> pages;
  int start_page = 0;

  std::function<PlayCheck()> can_play;                       // default: always
  std::function<bool()> on_save;                             // save extra files (key bindings...)
  std::function<void()> on_reset;                            // after "Reset all settings"
  std::function<void(const std::string& path)> on_file_drop;  // a file dropped on the window
  // Asked after can_play when PLAY is pressed. Return a prompt to show it first;
  // its buttons decide whether the game starts.
  std::function<std::optional<PlayPrompt>()> before_play;
  // Settings that only take effect after a restart (window size, renderer...).
  // After kPlay, Launcher::restart_needed() says whether any of them changed.
  std::vector<std::string> restart_keys;

  // Shown in the sidebar's footer ("NO GAME INSTALLED", "READY TO PLAY") and in the top bar's breadcrumb.
  std::function<std::pair<Status, std::string>()> sidebar_status;
  std::string breadcrumb_root;  // default: branding.title

  std::string hints = "Enter  Play        Esc  Quit        Ctrl+S  Save";
  std::string note = "Settings are saved when you press Play.";
  bool save_on_play = true;
};

// --------------------------------------------------------------------- ui ---
// The widgets pages are built from. Most are rows: a label and help text on
// the left, the control on the right, bound to a key in the settings store.
// Rows are laid out in a table that opens on the first row and closes on any
// non-row content (Heading, Paragraph, Section...) or at the end of the page.
class Ui {
 public:
  float scale() const { return scale_; }
  const Theme& theme() const;
  const Fonts& fonts() const;
  SettingsStore& settings();
  Launcher& launcher() { return *launcher_; }

  // -- rows bound to settings. Each returns true when the value changed.
  // Up to `max_buttons` options are segmented buttons, more become a combo.
  bool Choice(const char* label, const char* help, const char* key, const char* def,
              const std::vector<Option>& options, int max_buttons = 4);
  bool Combo(const char* label, const char* help, const char* key, const char* def,
             const std::vector<Option>& options);
  bool Toggle(const char* label, const char* help, const char* key, bool def, const char* off = "Off",
              const char* on = "On");
  bool SliderInt(const char* label, const char* help, const char* key, int def, int lo, int hi,
                 const char* format = "%d", int step = 1);
  bool SliderFloat(const char* label, const char* help, const char* key, float def, float lo, float hi,
                   const char* format = "%.2f");
  bool InputText(const char* label, const char* help, const char* key, const char* hint = "");

  // -- other rows
  void Info(const char* label, const char* text);
  bool ButtonRow(const char* label, const char* help, const char* button, bool accent = false);
  // A button that asks first; true when confirmed.
  bool ConfirmRow(const char* label, const char* help, const char* button, const char* question,
                  const char* confirm = "Confirm", const char* cancel = "Cancel");
  void FolderRow(const char* label, const char* help, const char* button, const std::string& path);
  // "Show this launcher: Off / At startup", read by ShouldShowLauncher().
  bool LauncherVisibilityRow(const char* key = "launcher");
  // "Reset all settings", with a confirmation.
  void ResetAllRow();
  // A key binding: shows `shown`; click, then press a key. Returns the key
  // pressed (Esc cancels). Map it to the game's own key names.
  std::optional<ImGuiKey> KeyBindRow(const char* label, const char* help, const std::string& shown);

  // -- building blocks
  // Starts a row; draw the control after it (it gets the right-hand column).
  void Row(const char* label, const char* help = nullptr);
  // Same layout as Row, but with normal-size wrapped explanatory text.
  void RowLarge(const char* label, const char* help = nullptr);
  void EndRows();
  int Segmented(const char* id, const std::vector<std::string>& labels, int selected);
  bool AccentButton(const char* label, ImVec2 size = ImVec2(-FLT_MIN, 0));
  std::optional<ImGuiKey> KeyCapture(const char* id, const std::string& shown, float width = -FLT_MIN);
  // The coloured card at the top of an install page. `progress` in 0..1 draws
  // a bar under the title instead of `detail`.
  void StatusCard(Status status, const char* title, const char* detail, float progress = -1.0f);
  // A Task's progress bar and Cancel button; false when no task is running.
  bool TaskProgress(Task& task);
  // Starts a new card titled `text`, closing the previous one. Rows that come before any Heading
  // are put in an untitled card; the page's cards close when the page ends.
  void Heading(const char* text);
  // A card with a subtitle and an optional badge at the right; closed by the next Heading/Card or the page's end.
  void Card(const char* title, const char* subtitle = nullptr, const char* badge = nullptr);
  void EndCard();
  // Boxed items in two columns, in the mono font (the mockup's supported-builds grid); `highlight` gets a gold outline.
  void Chips(const std::vector<std::string>& items, int highlight = -1);
  void Paragraph(const char* text);  // dim, wrapped
  void Help(const char* text);       // small, dim, wrapped
  void Spacer(float height = 8.0f);
  bool Section(const char* title, bool open_by_default = false);  // collapsing header

  // -- achievements
  // "12 / 50 unlocked     240 / 1000 G" (points are left out when total_points is 0).
  void AchievementSummary(int unlocked, int total, int points = 0, int total_points = 0);
  void AchievementCardView(const AchievementCard& card, float width = -1.0f);
  // Cards in up to `max_columns` columns, depending on the page width.
  void AchievementGrid(const std::vector<AchievementCard>& cards, int max_columns = 2);

  void SetStatus(std::string text, double seconds = 4.0);

 private:
  friend class Launcher;
  void BeginPage();
  void EndPage();
  void LeaveRows() {
    if (in_rows_) EndRows();
  }
  void BeginCardImpl(const char* title, const char* subtitle, const char* badge);

  Launcher* launcher_ = nullptr;
  float scale_ = 1.0f;
  bool in_rows_ = false;
  bool in_card_ = false;
  int rows_id_ = 0;
  int cards_id_ = 0;
  std::string capturing_;  // id of the key binding waiting for a key
  int capture_frame_ = 0;
};

// --------------------------------------------------------------- launcher ---
class Launcher {
 public:
  explicit Launcher(LauncherConfig config);
  ~Launcher();
  Launcher(const Launcher&) = delete;
  Launcher& operator=(const Launcher&) = delete;

  // Draws the launcher over the whole main viewport. Call between
  // ImGui::NewFrame() and ImGui::Render(). Returns kPlay once settings are
  // saved and the game should start, kQuit when the player quits.
  Result Frame();

  bool Save();  // settings + on_save, with a status line
  void SetStatus(std::string text, double seconds = 4.0);
  void GoToPage(int index);
  int FindPage(std::string_view name) const;
  int page() const { return page_; }
  void RequestPlay() { want_play_ = true; }
  // Shows a pop-up over the launcher (the same one before_play uses).
  void ShowPrompt(PlayPrompt prompt);
  // After kPlay: whether a LauncherConfig::restart_keys setting changed while the launcher was open.
  bool restart_needed() const;
  void RequestQuit() { want_quit_ = true; }
  void DropFile(const std::string& path);  // for hosts: forwards to config.on_file_drop
  bool capturing_key() const { return !ui_.capturing_.empty(); }

  LauncherConfig& config() { return config_; }
  const LauncherConfig& config() const { return config_; }
  SettingsStore& settings() { return *config_.settings; }

 private:
  friend class Ui;
  void DrawSidebar(ImVec2 origin, float w, float h);
  void DrawTopBar(ImVec2 origin, float w, float h);
  void DrawContent(ImVec2 origin, float w, float h);
  void DrawFooter(ImVec2 origin, float w, float h);
  void DrawPlayButton(ImVec2 size, bool can_play);
  void HandleHotkeys();
  void DrawPrompt();
  Result TryPlay();

  LauncherConfig config_;
  Ui ui_;
  float s_ = 1.0f;
  int page_ = 0;
  bool want_play_ = false, want_quit_ = false;
  std::string status_;
  double status_until_ = 0.0;
  bool nav_suspended_ = false;
  ImGuiConfigFlags saved_nav_flags_ = 0;
  std::optional<PlayPrompt> prompt_;
  bool open_prompt_ = false;
  bool prompt_confirmed_ = false;  // a prompt button chose to play: skip before_play once
  std::vector<std::string> restart_baseline_;
};

// Whether to show the launcher at startup: always when the game cannot start
// yet or `force` is set, otherwise unless `key` is off - and holding Shift
// brings it back.
bool ShouldShowLauncher(const SettingsStore& settings, std::string_view key, bool game_ready, bool force = false);

// ImGuiKey -> Windows virtual-key code (0 when there is none), for games
// whose bindings use VK codes.
int ImGuiKeyToVirtualKey(ImGuiKey key);

}  // namespace trg
