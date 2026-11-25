// What the Editor's Save Data page shows, as plain data: the installed builds, the saves each one edits, and the
// build, save and DATA slot descriptions.
#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "appearance_custom.h"
#include "game_files.h"
#include "inventory_data.h"
#include "save_data.h"
#include "save_discovery.h"
#include "sprite_render.h"

namespace dcp {

struct EditorBuild {
    const GameBuild* build = nullptr;
    GamePaths paths;
    bool configured = false; // the build the launcher is set to run
};

// Builds whose prepared folder exists, in SupportedBuilds order.
std::vector<EditorBuild> ListEditorBuilds(const GameRoots& roots, const GameBuild* configured,
                                          std::span<const GameBuild> builds = SupportedBuilds());
size_t DefaultBuildIndex(std::span<const EditorBuild> builds);

// Savedata is per title, so every version of a title lists the same saves.
std::vector<size_t> SavesOfBuild(std::span<const DiscoveredSave> saves, const EditorBuild& build);

// "Installed game: <build label> - 2 saves found": the build label is what the Play page shows (title id, version,
// region). `one` and `many` are the already translated "save found" / "saves found".
std::string InstalledGameLine(std::string_view prefix, std::string_view build_label, size_t saves, std::string_view one,
                              std::string_view many);

struct BuildInfo {
    std::string label;
    std::string title_id;
    std::string version;
    std::string region;
    std::string prepared;   // the prepared image folder, relative to the game root when it is under it
    std::string archive;    // the language archive
    std::string executable; // abbreviated eboot SHA-256
    GameFileState state = GameFileState::kMissing;
};

std::string ShortFingerprint(std::string_view sha256);
// `check` is the build's file check; without one the executable shows the build's expected fingerprint.
BuildInfo DescribeBuild(const EditorBuild& build, const GameRoots& roots, const GameCheck* check);

std::string ModifiedTimeText(std::filesystem::file_time_type modified);
struct SaveInfo {
    std::string path;
    std::string title_id;
    std::string region;
    std::string modified;
    int characters = 0;
    uintmax_t size = 0;
    std::string crc; // "Valid", or why the file is not
};
SaveInfo DescribeSaveFile(const DiscoveredSave& save);

// Slots are the three records inside SAVE0.DAT; the savedata folder (SAVEDATA00) is a different thing.
std::string DataSlotName(int index);
// The DATA slot shown for a freshly loaded save: the first one holding characters, else DATA 1.
int FirstPopulatedSlot(const SaveModel& model);
// "DATA 1 - 6 characters": a DATA slot as the Save Data combo lists it; `one` and `many` are the translated words.
std::string DataSlotChoiceLabel(const SaveModel& model, int slot, std::string_view one, std::string_view many);
// "Characters (DATA 2)": the list title, with the word already translated.
std::string CharacterListTitle(std::string_view characters, int slot);

struct DataSlotInfo {
    std::string name;
    uint32_t gold = 0;
    int characters = 0;
    std::string primary; // the first character, or empty for an empty slot
};
DataSlotInfo DescribeDataSlot(const SaveModel& model, int index);

// The Apply / Discard row is always drawn in the same place; only its state follows the pending changes.
struct ActionRow {
    bool apply_enabled = false;
    bool apply_accent = false;
    bool discard_enabled = false;
    const char* status = ""; // shown at the right end of the row
};
ActionRow ActionRowFor(bool dirty);

struct NumberRange {
    int min = 0;
    int max = 0;
};
inline constexpr NumberRange kGoldRange{0, int(kMaxGold)};
inline constexpr NumberRange kLevelRange{kMinLevel, kMaxLevel};
inline constexpr NumberRange kSkillPointRange{0, kMaxSkillPoints};
inline constexpr NumberRange kHpRange{0, kMaxHp};
inline constexpr NumberRange kCharacterStatRange{0, kMaxCharacterStat};

int ClampNumber(int value, NumberRange range);
// The value a drag of `pixels` from `anchor` reaches at one unit per pixel, kept inside the range.
int FineDragValue(int anchor, float pixels, NumberRange range);
// The horizontal span of an integer slider's grab (ImGui's own layout: 2 px padding, the grab covers one unit when it can).
struct Span {
    float min = 0.0f, max = 0.0f;
};
Span SliderGrabSpan(float frame_min, float frame_max, NumberRange range, int value, float grab_min_width);
// Whether the colored span is behind most of the text span.
bool TextOverSpan(Span text, Span covered);
// The value moved by exactly `delta` and kept inside the range.
int StepNumber(int value, int delta, NumberRange range);
// Typed text with or without thousands separators, clamped to the range; nothing when it holds no number.
std::optional<int> ParseNumber(std::string_view text, NumberRange range);
// A press on a slider track becomes an edit only once the pointer has moved this far: ImGui would otherwise jump the value to
// wherever a plain click lands, changing fields that were only clicked.
inline constexpr float kSliderDragThreshold = 4.0f; // at scale 1
inline bool SliderDragStarted(float press_x, float x, float scale) {
    const float moved = x - press_x;
    return (moved < 0.0f ? -moved : moved) >= kSliderDragThreshold * scale;
}

// The number being typed into one control, owned by that control's id. Typing into another control hands the earlier text
// to its own control, which commits it through its own setter; nothing is ever committed through another field.
class TypedNumberEdit {
public:
    bool Editing(uint32_t id) const { return owner_ == id; }
    void Begin(uint32_t id, std::string_view previous_text);
    void End() { owner_ = 0; }
    std::optional<std::string> TakeHanded(uint32_t id);
    void Clear();

private:
    uint32_t owner_ = 0;
    std::vector<std::pair<uint32_t, std::string>> handed_;
};

// The number a text holds when it is a whole number inside the range as typed; nothing for empty, invalid or out-of-range text.
std::optional<int> ExactTypedNumber(std::string_view text, NumberRange range);
// One typed edit of an item field. It keeps the record as it was when typing began and evaluates every text from that
// snapshot: a valid number is applied through the field's setter to the snapshot, anything else leaves the snapshot, so a
// prefix such as "9" on the way to "99" never leaves a dependent change behind. It is a temporary snapshot, not a model.
class TypedFieldEdit {
public:
    bool Active() const { return active_; }
    bool Is(const ItemRef& ref, std::string_view key) const { return active_ && ref_ == ref && key_ == key; }
    const ItemRef& ref() const { return ref_; }
    NumberRange range() const { return range_; }
    void Begin(const ItemRef& ref, std::string key, NumberRange range, std::vector<uint8_t> snapshot);
    void Update(SaveModel& edited, std::string_view text);
    void Finish(SaveModel& edited, std::string_view text); // an out-of-range number is clamped
    void Cancel(SaveModel& edited);
    void End() { active_ = false; }

private:
    bool active_ = false;
    ItemRef ref_;
    std::string key_;
    NumberRange range_;
    std::vector<uint8_t> snapshot_;
};

// What leaving a typed field commits, by Enter or by clicking away: the typed number when it differs from the current one.
std::optional<int> CommitTypedNumber(std::string_view text, NumberRange range, int current);
// 2356557 -> "2,356,557": display only, never stored.
std::string FormatThousands(int64_t value);

struct HpValues {
    int current = 0;
    int max = 0;
};
// Current never exceeds Max: lowering Max lowers Current to it.
HpValues WithCurrentHp(HpValues hp, int current);
HpValues WithMaxHp(HpValues hp, int max);

// How many cells of at least `min_cell` fit in `width` with `gap` between them, at most `max_columns` and at least one.
int GridColumns(float width, float min_cell, float gap, int max_columns);
// The width of one cell in that grid, never above `max_cell`.
float GridCellWidth(float width, float gap, int columns, float max_cell);

// A range this wide is dragged by relative movement, because a slider over it would jump by thousands per pixel.
inline constexpr int kNumberThumbMaxSpan = 100000;
inline constexpr float kNumberThumbMinWidth = 16.0f; // at scale 1
inline bool NumberHasThumb(NumberRange range) {
    return int64_t(range.max) - int64_t(range.min) <= kNumberThumbMaxSpan;
}

inline constexpr float kNumberGlyphRatio = 0.6f;          // glyph length over button height

// A label on the left and its control on the right; below `kStackWidth` the control goes under the label instead.
inline constexpr float kRowControlFraction = 0.58f;
inline constexpr float kRowStackWidth = 480.0f; // at scale 1
struct LabelRowLayout {
    bool stacked = false;
    float control_offset = 0.0f; // from the row's left edge
    float control_width = 0.0f;
};
LabelRowLayout LayoutLabelRow(float width, float scale);

struct ActionRowLayout {
    float pair_offset = 0.0f;
    float pair_width = 0.0f;
    float apply_x = 0.0f;
    float discard_x = 0.0f;
    float status_gap = 0.0f;
    float status_x = 0.0f;
    float status_width = 0.0f;
};
ActionRowLayout LayoutActionRow(float available, float button_width, float spacing, float left_width = 0.0f,
                                float status_width = 0.0f);

struct EditorHeaderGeometry {
    ActionRow row;
    ActionRowLayout controls;
    float header_y = 0.0f;
    float header_height = 0.0f;
    float tabs_y = 0.0f;
    float body_y = 0.0f;
};
EditorHeaderGeometry LayoutEditorHeader(bool dirty, float available, float left_width, float status_width, float scale,
                                        float frame_height, float item_spacing_x, float item_spacing_y);

struct NumberControlLayout {
    float total_width = 0.0f;
    float button_width = 0.0f;
    float field_width = 0.0f;
    float height = 0.0f;
};
NumberControlLayout LayoutNumberControl(float available_width, float frame_height, float spacing, float scale);

float RightAlignGap(float available, float left_width, float right_width);

// Appearance: smaller preview on the left, controls on the right; stacked when narrow.
inline constexpr float kAppearanceWideWidth = 640.0f; // at scale 1
inline constexpr float kPreviewColumnWeight = 0.35f;
inline constexpr float kControlsColumnWeight = 0.65f;
int AppearanceColumns(float width, float scale);
float PreviewHeight(float column_width, float scale);
struct ImagePoint {
    int x = 0;
    int y = 0;
};
// The part of an image that holds the art, so the preview is not drawn with the animation's empty margins.
struct PixelBox {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
};
PixelBox OpaqueBounds(const SpriteImage& image, int pad);
// `crop` of an image fitted into a box, centred across and set against the top; x, y place the crop's corner.
struct ArtFit {
    float x = 0.0f, y = 0.0f, scale = 1.0f, width = 0.0f, height = 0.0f;
};
ArtFit FitArt(float box_x, float box_y, float box_width, float box_height, const PixelBox& crop);

// The Character Editor summary: portrait and two identity lines on the left, Edit Appearance over the control column of
// the rows under it, centred on the portrait.
inline constexpr float kSummaryPortrait = 60.0f;    // at scale 1
inline constexpr float kSummaryButtonScale = 1.4f; // over the frame height
struct SummaryLayout {
    float portrait = 0.0f;
    float height = 0.0f;
    float button_x = 0.0f, button_y = 0.0f, button_width = 0.0f, button_height = 0.0f;
};
SummaryLayout LayoutCharacterSummary(float row_width, float info_width, float control_width, float frame_height,
                                     float spacing, float scale);
// Edit Appearance and Edit Inventory share the summary's button column, side by side.
struct SummaryButtons {
    float width = 0.0f;
    float second_x = 0.0f; // from the first button's left edge
};
SummaryButtons SplitSummaryButtons(const SummaryLayout& layout, float spacing);

// The Inventory Editor: the item list beside the item's details; the details go under the list when the page is narrow.
inline constexpr float kInventoryWideWidth = 720.0f; // at scale 1
inline constexpr float kInventoryListWeight = 0.42f;
inline constexpr float kInventoryMinHeight = 320.0f; // at scale 1
struct InventoryLayout {
    bool stacked = false;
    float list_width = 0.0f;
    float detail_width = 0.0f;
};
InventoryLayout LayoutInventory(float width, float gap, float scale);

ImagePoint MapToImage(float mouse_x, float mouse_y, float image_min_x, float image_min_y, float scale);
inline constexpr float kNumberGlyphThickness = 2.5f;     // at scale 1

inline constexpr float kCardMinWidth = 300.0f;  // at scale 1
inline constexpr float kCardMaxWidth = 420.0f;

enum class FieldStyle { kClean, kChanged };
template <class T>
FieldStyle FieldStyleFor(const T& saved, const T& working) {
    return saved == working ? FieldStyle::kClean : FieldStyle::kChanged;
}

struct IdentityPart {
    std::string text;
    bool changed = false;
};
// "Vivian  Amazon · Lv 1 · Slot 1": the name, then the class, level and slot on the same line. `saved` is the character as
// persisted (nullptr when unknown); the level is marked when it differs from it.
struct CharacterIdentity {
    std::string name;
    std::vector<IdentityPart> parts;
};
CharacterIdentity DescribeIdentity(const CharacterView& working, const CharacterView* saved, std::string_view class_text,
                                   std::string_view level_word, std::string_view slot_word);
// The identity line in a row of `row_height`: the name and the details share one baseline, and the line is centred.
struct IdentityLine {
    float name_y = 0.0f, detail_y = 0.0f, height = 0.0f;
};
IdentityLine LayoutIdentityLine(float name_ascent, float name_height, float detail_ascent, float detail_height, float row_height);

// The Character page: the list, one character's editor, its appearance being prepared, its appearance or its inventory.
class CharacterNav {
public:
    bool editing() const { return open_ >= 0; }
    bool loading() const { return open_ >= 0 && view_ == View::kLoading; }
    bool appearance() const { return open_ >= 0 && view_ == View::kAppearance; }
    bool inventory() const { return open_ >= 0 && view_ == View::kInventory; }
    int open() const { return open_; }
    void Open(int character) {
        open_ = character;
        view_ = View::kEditor;
    }
    // From the open character's editor only, so a second request while one loads or shows is refused.
    bool RequestAppearance() {
        if (open_ < 0 || view_ != View::kEditor) return false;
        view_ = View::kLoading;
        return true;
    }
    // From the open character's editor only.
    bool RequestInventory() {
        if (open_ < 0 || view_ != View::kEditor) return false;
        view_ = View::kInventory;
        return true;
    }
    void FinishAppearance(bool ready) {
        if (view_ == View::kLoading) view_ = ready ? View::kAppearance : View::kEditor;
    }
    void BackToCharacter() { view_ = View::kEditor; }
    void Back() {
        open_ = -1;
        view_ = View::kEditor;
    }

private:
    enum class View { kEditor, kLoading, kAppearance, kInventory };
    int open_ = -1;
    View view_ = View::kEditor;
};

// The Appearance page always edits a custom look: the stored one while enabled, else no override over `base`.
CustomLook EditableLook(const std::optional<CustomLook>& stored, uint8_t base);
// What an edited look is stored as: with no override it is no entry, so the character stays its native preset.
CustomLook StorableLook(CustomLook look);
// "Part Visibility (Hair)", or the bare label when no part is selected.
std::string PartVisibilityLabel(std::string_view label, std::string_view part);

// The colour picker popup: compact, opened beside the swatch, never over the preview when there is room.
inline constexpr float kPickerWidth = 400.0f;  // at scale 1
inline constexpr float kPickerHeight = 400.0f; // at scale 1
// The height of the picker's Current and Original swatches with the "Original" caption: OK and Cancel start under it.
float PickerSwatchesHeight(float frame_height, float text_height, float spacing);
struct PickerPoint {
    float x = 0.0f, y = 0.0f;
};
// The top-left of a picker of `size`: right of the swatch when it fits, else against the right edge of the view, but
// never left of `clear_left` (the preview's right edge) while the view is wide enough for that; inside the view always.
// What the picker does this frame. OK, Enter and a click outside keep the colour; Cancel and Escape put the old state back.
enum class PickerOutcome { kOpen, kCommit, kCancel };
PickerOutcome ResolvePicker(bool popup_open, bool ok, bool cancel, bool enter, bool escape);

PickerPoint PlacePicker(PickerPoint swatch_min, PickerPoint swatch_max, PickerPoint size, PickerPoint view_min,
                        PickerPoint view_max, float clear_left, float gap);

struct CharacterRowText {
    std::string name;
    std::string detail; // "Amazon · Lv 1 · Slot 1"
};
CharacterRowText DescribeCharacterRow(const CharacterView& character, std::string_view class_text, std::string_view level_word,
                                      std::string_view slot_word);

} // namespace dcp
