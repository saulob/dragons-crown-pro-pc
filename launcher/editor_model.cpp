#include "editor_model.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <chrono>
#include <ctime>

namespace dcp {
namespace {

std::string RelativeToGameRoot(const std::filesystem::path& path, const GameRoots& roots) {
    const std::filesystem::path game_root = roots.prepared.parent_path().parent_path();
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(path, game_root, ec);
    const std::string text = relative.generic_string();
    if (ec || text.empty() || text.starts_with("..")) {
        return path.generic_string();
    }
    return text;
}

} // namespace

std::vector<EditorBuild> ListEditorBuilds(const GameRoots& roots, const GameBuild* configured,
                                          std::span<const GameBuild> builds) {
    std::vector<EditorBuild> list;
    for (const GameBuild& build : builds) {
        EditorBuild entry;
        entry.build = &build;
        entry.paths = BuildPaths(roots, build);
        std::error_code ec;
        if (!std::filesystem::is_directory(entry.paths.app0, ec)) {
            continue;
        }
        entry.configured = configured && BuildKey(build) == BuildKey(*configured);
        list.push_back(std::move(entry));
    }
    return list;
}

size_t DefaultBuildIndex(std::span<const EditorBuild> builds) {
    for (size_t i = 0; i < builds.size(); ++i) {
        if (builds[i].configured) return i;
    }
    return 0;
}

std::vector<size_t> SavesOfBuild(std::span<const DiscoveredSave> saves, const EditorBuild& build) {
    return build.build ? SavesOfTitle(saves, build.build->title_id) : std::vector<size_t>();
}

std::string ShortFingerprint(std::string_view sha256) {
    if (sha256.size() <= 12) return std::string(sha256);
    return std::string(sha256.substr(0, 12)) + "...";
}

BuildInfo DescribeBuild(const EditorBuild& entry, const GameRoots& roots, const GameCheck* check) {
    const GameBuild& build = *entry.build;
    BuildInfo info;
    info.label = build.label;
    info.title_id = build.title_id;
    info.version = build.version;
    info.region = build.region;
    info.prepared = RelativeToGameRoot(entry.paths.app0, roots);
    info.archive = build.regional_archive ? build.regional_archive : build.translation->archive;
    const bool read = check && !check->eboot_sha256.empty();
    info.executable = ShortFingerprint(read ? check->eboot_sha256 : build.eboot_sha256);
    info.state = check ? check->state : GameFileState::kMissing;
    return info;
}

std::string ModifiedTimeText(std::filesystem::file_time_type modified) {
    const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        modified - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    const std::time_t time = std::chrono::system_clock::to_time_t(system);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    char buffer[24];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &tm);
    return buffer;
}

SaveInfo DescribeSaveFile(const DiscoveredSave& save) {
    SaveInfo info;
    info.path = save.save0_path.string();
    info.title_id = save.title_id;
    info.region = save.region;
    info.modified = ModifiedTimeText(save.modified);
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(save.save0_path, ec);
    info.size = ec ? 0 : size;
    for (const DataSlotView& slot : save.model.slots()) {
        for (const CharacterView& character : slot.characters) {
            info.characters += character.present ? 1 : 0;
        }
    }
    if (save.model.valid()) {
        info.crc = "Valid";
    } else {
        info.crc = save.error.empty() ? "Invalid" : save.error;
    }
    return info;
}

std::string DataSlotName(int index) {
    return "DATA " + std::to_string(index + 1);
}

std::string CharacterListTitle(std::string_view characters, int slot) {
    return std::string(characters) + " (" + DataSlotName(slot) + ")";
}

DataSlotInfo DescribeDataSlot(const SaveModel& model, int index) {
    DataSlotInfo info;
    info.name = DataSlotName(index);
    const DataSlotView* slot = model.Slot(index);
    if (!slot) return info;
    info.gold = slot->gold;
    for (const CharacterView& character : slot->characters) {
        if (!character.present) continue;
        if (info.characters++ == 0) {
            info.primary = character.name + " (" + std::string(ClassName(character.klass)) + ")";
        }
    }
    return info;
}

ActionRow ActionRowFor(bool dirty) {
    return {dirty, dirty, dirty, dirty ? "Unsaved changes" : ""};
}

std::string InstalledGameLine(std::string_view prefix, std::string_view build_label, size_t saves, std::string_view one,
                              std::string_view many) {
    return std::string(prefix) + ": " + std::string(build_label) + " \xC2\xB7 " + std::to_string(saves) + " " +
           std::string(saves == 1 ? one : many);
}

std::string DataSlotChoiceLabel(const SaveModel& model, int slot, std::string_view one, std::string_view many) {
    const int count = DescribeDataSlot(model, slot).characters;
    return DataSlotName(slot) + " - " + std::to_string(count) + " " + std::string(count == 1 ? one : many);
}

int FirstPopulatedSlot(const SaveModel& model) {
    for (int slot = 0; slot < int(kDataSlotCount); ++slot) {
        if (DescribeDataSlot(model, slot).characters > 0) return slot;
    }
    return 0;
}

int ClampNumber(int value, NumberRange range) {
    return std::clamp(value, range.min, range.max);
}

int StepNumber(int value, int delta, NumberRange range) {
    return ClampNumber(int(std::clamp<int64_t>(int64_t(value) + delta, INT32_MIN, INT32_MAX)), range);
}

std::optional<int> ParseNumber(std::string_view text, NumberRange range) {
    int64_t value = 0;
    bool any = false;
    for (const char c : text) {
        if (c == ',' || c == ' ') continue;
        if (c < '0' || c > '9') return std::nullopt;
        value = std::min<int64_t>(value * 10 + (c - '0'), INT32_MAX);
        any = true;
    }
    if (!any) return std::nullopt;
    return ClampNumber(int(value), range);
}

void TypedNumberEdit::Begin(uint32_t id, std::string_view previous_text) {
    if (owner_ != 0 && owner_ != id) handed_.emplace_back(owner_, std::string(previous_text));
    owner_ = id;
}

std::optional<std::string> TypedNumberEdit::TakeHanded(uint32_t id) {
    for (auto it = handed_.begin(); it != handed_.end(); ++it) {
        if (it->first != id) continue;
        std::string text = std::move(it->second);
        handed_.erase(it);
        return text;
    }
    return std::nullopt;
}

void TypedNumberEdit::Clear() {
    owner_ = 0;
    handed_.clear();
}

void TypedFieldEdit::Begin(const ItemRef& ref, std::string key, NumberRange range, std::vector<uint8_t> snapshot) {
    active_ = true;
    ref_ = ref;
    key_ = std::move(key);
    range_ = range;
    snapshot_ = std::move(snapshot);
}

void TypedFieldEdit::Update(SaveModel& edited, std::string_view text) {
    if (!active_) return;
    RestoreItemBytes(edited, ref_, snapshot_);
    if (const std::optional<int> exact = ExactTypedNumber(text, range_)) SetItemField(edited, ref_, key_, *exact);
}

void TypedFieldEdit::Finish(SaveModel& edited, std::string_view text) {
    if (!active_) return;
    RestoreItemBytes(edited, ref_, snapshot_);
    if (const std::optional<int> typed = ParseNumber(text, range_)) SetItemField(edited, ref_, key_, *typed);
    active_ = false;
}

void TypedFieldEdit::Cancel(SaveModel& edited) {
    if (active_) RestoreItemBytes(edited, ref_, snapshot_);
    active_ = false;
}

std::optional<int> ExactTypedNumber(std::string_view text, NumberRange range) {
    int64_t value = 0;
    bool any = false;
    for (const char c : text) {
        if (c == ',' || c == ' ') continue;
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + (c - '0');
        any = true;
        if (value > INT32_MAX) return std::nullopt;
    }
    return any && value >= range.min && value <= range.max ? std::optional<int>(int(value)) : std::nullopt;
}

std::optional<int> CommitTypedNumber(std::string_view text, NumberRange range, int current) {
    const std::optional<int> typed = ParseNumber(text, range);
    return typed && *typed != current ? typed : std::nullopt;
}

std::string FormatThousands(int64_t value) {
    std::string digits = std::to_string(value < 0 ? -value : value);
    for (int at = int(digits.size()) - 3; at > 0; at -= 3) digits.insert(size_t(at), ",");
    return value < 0 ? "-" + digits : digits;
}

int FineDragValue(int anchor, float pixels, NumberRange range) {
    return ClampNumber(int(std::clamp<int64_t>(int64_t(anchor) + int64_t(std::lround(pixels)), INT32_MIN, INT32_MAX)), range);
}

Span SliderGrabSpan(float frame_min, float frame_max, NumberRange range, int value, float grab_min_width) {
    constexpr float kPadding = 2.0f;
    const float slider = (frame_max - frame_min) - kPadding * 2.0f;
    const float units = float(std::max<int64_t>(int64_t(range.max) - int64_t(range.min), 0));
    const float grab = std::min(std::max(slider / (units + 1.0f), grab_min_width), slider);
    const float t = units > 0.0f ? float(std::clamp(value, range.min, range.max) - range.min) / units : 0.0f;
    const float center = frame_min + kPadding + grab * 0.5f + t * (slider - grab);
    return {center - grab * 0.5f, center + grab * 0.5f};
}

bool TextOverSpan(Span text, Span covered) {
    const float width = text.max - text.min;
    if (width <= 0.0f) return false;
    const float overlap = std::min(text.max, covered.max) - std::max(text.min, covered.min);
    return overlap * 2.0f >= width;
}

HpValues WithMaxHp(HpValues hp, int max) {
    hp.max = ClampNumber(max, kHpRange);
    hp.current = std::min(hp.current, hp.max);
    return hp;
}

HpValues WithCurrentHp(HpValues hp, int current) {
    hp.current = ClampNumber(current, {0, hp.max});
    return hp;
}

CharacterRowText DescribeCharacterRow(const CharacterView& character, std::string_view class_text, std::string_view level_word,
                                      std::string_view slot_word) {
    static const char* const kDot = " · ";
    return {character.name, std::string(class_text) + kDot + std::string(level_word) + " " +
                                std::to_string(SaveModel::DisplayedLevel(character.level_stored)) + kDot +
                                std::string(slot_word) + " " + std::to_string(character.slot + 1)};
}

int GridColumns(float width, float min_cell, float gap, int max_columns) {
    int columns = 1;
    while (columns < max_columns && width >= float(columns + 1) * min_cell + float(columns) * gap) ++columns;
    return columns;
}

float GridCellWidth(float width, float gap, int columns, float max_cell) {
    const float cell = (width - float(columns - 1) * gap) / float(std::max(columns, 1));
    return std::min(cell, max_cell);
}

CharacterIdentity DescribeIdentity(const CharacterView& working, const CharacterView* saved, std::string_view class_text,
                                   std::string_view level_word, std::string_view slot_word) {
    CharacterIdentity identity;
    identity.name = working.name;
    const bool level_changed = saved && saved->level_stored != working.level_stored;
    identity.parts.push_back({std::string(class_text), false});
    identity.parts.push_back({std::string(level_word) + " " + std::to_string(SaveModel::DisplayedLevel(working.level_stored)), level_changed});
    identity.parts.push_back({std::string(slot_word) + " " + std::to_string(working.slot + 1), false});
    return identity;
}

float PickerSwatchesHeight(float frame_height, float text_height, float spacing) {
    return 4.0f * frame_height + text_height + 2.0f * spacing; // two swatches two frames tall, the caption between
}

IdentityLine LayoutIdentityLine(float name_ascent, float name_height, float detail_ascent, float detail_height, float row_height) {
    const float baseline = std::max(name_ascent, detail_ascent);
    IdentityLine line;
    line.name_y = baseline - name_ascent;
    line.detail_y = baseline - detail_ascent;
    line.height = std::max(line.name_y + name_height, line.detail_y + detail_height);
    const float offset = std::max(0.0f, (row_height - line.height) * 0.5f);
    line.name_y += offset;
    line.detail_y += offset;
    return line;
}

LabelRowLayout LayoutLabelRow(float width, float scale) {
    LabelRowLayout layout;
    layout.stacked = width < kRowStackWidth * scale;
    layout.control_width = layout.stacked ? width : width * kRowControlFraction;
    layout.control_offset = layout.stacked ? 0.0f : width - layout.control_width;
    return layout;
}

PickerOutcome ResolvePicker(bool popup_open, bool ok, bool cancel, bool enter, bool escape) {
    if (cancel || escape) return PickerOutcome::kCancel;
    if (ok || enter || !popup_open) return PickerOutcome::kCommit;
    return PickerOutcome::kOpen;
}

PickerPoint PlacePicker(PickerPoint swatch_min, PickerPoint swatch_max, PickerPoint size, PickerPoint view_min,
                        PickerPoint view_max, float clear_left, float gap) {
    float x = swatch_max.x + gap;
    if (x + size.x > view_max.x) x = view_max.x - size.x;
    if (x < clear_left && clear_left + size.x <= view_max.x) x = clear_left;
    x = std::max(x, view_min.x);
    float y = swatch_min.y;
    if (y + size.y > view_max.y) y = view_max.y - size.y;
    y = std::max(y, view_min.y);
    return {x, y};
}

float RightAlignGap(float available, float left_width, float right_width) {
    return std::max(0.0f, available - left_width - right_width);
}

int AppearanceColumns(float width, float scale) {
    return width >= kAppearanceWideWidth * scale ? 2 : 1;
}

float PreviewHeight(float column_width, float scale) {
    return std::clamp(column_width * 1.3f, 220.0f * scale, 440.0f * scale);
}

ImagePoint MapToImage(float mouse_x, float mouse_y, float image_min_x, float image_min_y, float scale) {
    return {int((mouse_x - image_min_x) / scale), int((mouse_y - image_min_y) / scale)};
}

PixelBox OpaqueBounds(const SpriteImage& image, int pad) {
    PixelBox box{image.width, image.height, 0, 0};
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            if (!image.rgba[(size_t(y) * size_t(image.width) + size_t(x)) * 4 + 3]) continue;
            box = {std::min(box.x0, x), std::min(box.y0, y), std::max(box.x1, x + 1), std::max(box.y1, y + 1)};
        }
    }
    if (box.x1 <= box.x0 || box.y1 <= box.y0) return {0, 0, image.width, image.height};
    return {std::max(0, box.x0 - pad), std::max(0, box.y0 - pad), std::min(image.width, box.x1 + pad),
            std::min(image.height, box.y1 + pad)};
}

ArtFit FitArt(float box_x, float box_y, float box_width, float box_height, const PixelBox& crop) {
    ArtFit fit;
    if (crop.width() <= 0 || crop.height() <= 0) return fit;
    fit.scale = std::min(box_width / float(crop.width()), box_height / float(crop.height()));
    fit.width = float(crop.width()) * fit.scale;
    fit.height = float(crop.height()) * fit.scale;
    fit.x = box_x + (box_width - fit.width) * 0.5f;
    fit.y = box_y;
    return fit;
}

SummaryLayout LayoutCharacterSummary(float row_width, float info_width, float control_width, float frame_height,
                                     float spacing, float scale) {
    SummaryLayout layout;
    layout.portrait = kSummaryPortrait * scale;
    layout.button_height = std::round(frame_height * kSummaryButtonScale);
    layout.height = std::max(layout.portrait, layout.button_height);
    layout.button_x = std::max(info_width + spacing, row_width - control_width);
    layout.button_width = std::max(0.0f, row_width - layout.button_x);
    layout.button_y = std::floor((layout.height - layout.button_height) * 0.5f);
    return layout;
}

SummaryButtons SplitSummaryButtons(const SummaryLayout& layout, float spacing) {
    SummaryButtons buttons;
    buttons.width = std::max(0.0f, std::floor((layout.button_width - spacing) * 0.5f));
    buttons.second_x = buttons.width + spacing;
    return buttons;
}

InventoryLayout LayoutInventory(float width, float gap, float scale) {
    InventoryLayout layout;
    layout.stacked = width < kInventoryWideWidth * scale;
    if (layout.stacked) {
        layout.list_width = layout.detail_width = std::max(0.0f, width);
        return layout;
    }
    layout.list_width = std::floor((width - gap) * kInventoryListWeight);
    layout.detail_width = std::max(0.0f, width - gap - layout.list_width);
    return layout;
}

CustomLook EditableLook(const std::optional<CustomLook>& stored, uint8_t base) {
    if (stored && stored->enabled) return *stored;
    return CustomLook{true, base, {}};
}

CustomLook StorableLook(CustomLook look) {
    look.Tidy();
    if (look.OverrideCount() == 0) return CustomLook{false, look.base, {}};
    look.enabled = true;
    return look;
}

std::string PartVisibilityLabel(std::string_view label, std::string_view part) {
    if (part.empty()) return std::string(label);
    return std::string(label) + " (" + std::string(part) + ")";
}

ActionRowLayout LayoutActionRow(float available, float button_width, float spacing, float left_width, float status_width) {
    ActionRowLayout layout;
    layout.pair_width = 2.0f * button_width + spacing;
    const float min_pair_x = left_width > 0.0f ? left_width + spacing : 0.0f;
    layout.pair_offset = std::max(min_pair_x, (available - layout.pair_width) * 0.5f);
    layout.apply_x = layout.pair_offset;
    layout.discard_x = layout.pair_offset + button_width + spacing;
    layout.status_width = std::max(0.0f, status_width);
    layout.status_gap = RightAlignGap(available, layout.pair_offset + layout.pair_width, layout.status_width);
    layout.status_x = layout.pair_offset + layout.pair_width + layout.status_gap;
    return layout;
}

EditorHeaderGeometry LayoutEditorHeader(bool dirty, float available, float left_width, float status_width, float scale,
                                        float frame_height, float item_spacing_x, float item_spacing_y) {
    const float title_y = 12.0f * scale;
    const float title_height = 30.0f * scale;
    const float item_gap = 6.0f * scale;
    EditorHeaderGeometry geom;
    geom.row = ActionRowFor(dirty);
    geom.controls = LayoutActionRow(available, 170.0f * scale, item_spacing_x, left_width, status_width);
    geom.header_y = std::floor(title_y + (title_height - frame_height) * 0.5f);
    geom.header_height = std::max(title_height, frame_height);
    geom.tabs_y = title_y + title_height + item_gap;
    geom.body_y = geom.tabs_y + frame_height + item_spacing_y;
    return geom;
}

NumberControlLayout LayoutNumberControl(float available_width, float frame_height, float spacing, float scale) {
    NumberControlLayout layout;
    layout.height = frame_height;
    layout.button_width = frame_height;
    layout.field_width = std::max(48.0f * scale, available_width - 2.0f * layout.button_width - 2.0f * spacing);
    layout.total_width = 2.0f * layout.button_width + 2.0f * spacing + layout.field_width;
    return layout;
}

} // namespace dcp
