#include "editor_page.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "host.h"
#include "loading_view.h"
#include "page_icons.h"
#include "localization/ui_text.h"
#include "trg/launcher.h"
#include "trg/layout.h"

namespace dcp {
namespace {

constexpr const char* kMiddleDot = " \xC2\xB7 ";

} // namespace

ImTextureID EditorPage::ButtonIconTexture(ButtonIcon icon, int pixels) {
    const auto [entry, is_new] = button_icons_.try_emplace({icon, pixels});
    if (is_new) {
        const FileIconAsset& asset = icon == ButtonIcon::kAppearance ? kAppearanceIcon : icon == ButtonIcon::kInventory ? kInventoryIcon : kBackIcon;
        const std::vector<uint8_t> rgba = RenderFileIcon(asset, pixels);
        if (!rgba.empty()) {
            entry->second = host_.CreateTexture(rgba.data(), pixels, pixels);
        }
    }
    return entry->second;
}

EditorPage::EditorPage(LauncherHost& host, std::filesystem::path user_dir, std::function<EditorGame()> game,
                       std::function<std::string()> message_suffix, const SaveSelection* selection)
    : host_(host), user_dir_(std::move(user_dir)), game_(std::move(game)), message_suffix_(std::move(message_suffix)),
      selection_(selection), custom_saved_(CustomStorePath(user_dir_)), custom_working_(CustomStorePath(user_dir_)),
      hp_saved_(HpOverrideStorePath(user_dir_)), hp_working_(HpOverrideStorePath(user_dir_)) {}

const EditorBuild* EditorPage::SelectedBuild() const {
    return selected_build_ < builds_.size() ? &builds_[selected_build_] : nullptr;
}

std::vector<size_t> EditorPage::VisibleSaves() const {
    if (const EditorBuild* build = SelectedBuild()) {
        return SavesOfBuild(saves_, *build);
    }
    return {};
}

void EditorPage::UseArtOf(const EditorBuild* build) {
    const std::filesystem::path root = build ? build->paths.app0 : std::filesystem::path();
    if (art_ && art_root_ == root) {
        return;
    }
    for (const auto& [key, texture] : textures_) {
        host_.DestroyTexture(texture);
    }
    textures_.clear();
    for (ImTextureID* texture : {&custom_texture_, &overlay_texture_}) {
        if (*texture != ImTextureID{}) host_.DestroyTexture(*texture);
        *texture = ImTextureID{};
    }
    custom_texture_key_.clear();
    overlay_key_.clear();
    preview_crops_.clear();
    CharacterArtSources sources = ArtSourcesOf(root, build ? build->build->regional_archive : nullptr);
    sources.cache_dir = EditorCacheDir(user_dir_);
    art_.emplace(std::move(sources));
    art_root_ = root;
    catalog_key_.clear();
    for (const auto& [key, texture] : icon_textures_) host_.DestroyTexture(texture);
    icon_textures_.clear();
    for (const auto& [letter, texture] : rank_textures_) host_.DestroyTexture(texture);
    rank_textures_.clear();
    icons_ = {};
}

void EditorPage::Refresh() {
    const std::filesystem::path selected = selection_ ? selection_->save0 : std::filesystem::path();
    const std::filesystem::path kept_save = SelectedSave() ? SelectedSave()->save0_path : std::filesystem::path();
    if (selection_) seen_generation_ = selection_->generation;
    current_game_ = game_ ? game_() : EditorGame{};
    builds_ = ListEditorBuilds(current_game_.roots, current_game_.configured);
    selected_build_ = DefaultBuildIndex(builds_);
    UseArtOf(SelectedBuild());

    saves_ = DiscoverSaves(user_dir_, current_game_.configured ? current_game_.configured->title_id : "");
    const std::vector<size_t> visible = VisibleSaves();
    const std::filesystem::path preferred[] = {selected, kept_save};
    int chosen = ChooseSave(saves_, visible, preferred);
    const bool kept = chosen >= 0 && saves_[chosen].save0_path == kept_save;
    if (!kept) {
        selected_slot_ = 0;
        selected_character_ = -1;
        nav_.Back();
        ResetInventoryView();
    }
    selected_save_ = chosen;
    edit_.reset();
    if (chosen >= 0) {
        edit_.emplace(saves_[chosen], BackupFolder(user_dir_));
        if (!kept) selected_slot_ = FirstPopulatedSlot(edit_->working());
    }
    custom_error_ = custom_saved_.Load() ? std::string() : custom_saved_.error();
    custom_working_ = custom_saved_;
    hp_saved_.Load();
    hp_working_ = hp_saved_;
    confirm_reset_custom_ = false;
    SyncNameBuffer();
}

const DiscoveredSave* EditorPage::SelectedSave() const {
    return selected_save_ >= 0 && selected_save_ < static_cast<int>(saves_.size()) ? &saves_[selected_save_] : nullptr;
}

const DataSlotView* EditorPage::SelectedSlot() const {
    return edit_ && edit_->working().valid() ? edit_->working().Slot(selected_slot_) : nullptr;
}

const CharacterView* EditorPage::SelectedCharacter() const {
    return edit_ ? edit_->working().Character(selected_slot_, selected_character_) : nullptr;
}

const CharacterView* EditorPage::PersistedCharacter() const {
    return edit_ ? edit_->persisted().Character(selected_slot_, selected_character_) : nullptr;
}

std::string EditorPage::ColorName(CharacterClass klass, int color) {
    return art_ ? art_->ColorName(klass, color, message_suffix_ ? message_suffix_() : std::string()) : std::string();
}

void EditorPage::SyncNameBuffer() {
    if (const CharacterView* character = SelectedCharacter()) {
        std::snprintf(name_buffer_, sizeof(name_buffer_), "%s", character->name.c_str());
    } else {
        name_buffer_[0] = '\0';
    }
}

std::string EditorPage::DirtyReason() const {
    return dirty() ? Tr("The Editor has unapplied changes. Use Apply Changes or Discard Changes on the Editor page first.")
                  : std::string();
}

void EditorPage::SelectSlot(int slot, trg::Ui& ui) {
    (void)ui;
    if (dirty()) {
        pending_switch_ = PendingSwitch::kSlot;
        pending_target_ = slot;
        return;
    }
    selected_slot_ = slot;
    selected_character_ = -1;
    nav_.Back();
    ResetInventoryView();
    SyncNameBuffer();
}

void EditorPage::SelectCharacter(int character) {
    selected_character_ = character;
    nav_.Open(character);
    ResetInventoryView();
    appearance_failed_ = false;
    SyncNameBuffer();
}

void EditorPage::ApplyChanges(trg::Ui& ui) {
    if (!PrepareInventorySave(ui)) {
        return;
    }
    if (!dirty()) {
        return;
    }
    const ApplyOutcome outcome =
        [&] {
            HpOverrideStore hp_next = hp_working_;
            if (edit_ && hp_saved_.error().empty()) {
                if (const DiscoveredSave* save = SelectedSave())
                    SyncHpOverrides(hp_next, edit_->working(), edit_->persisted(), save->title_id, save->dir_name);
            }
            return ApplyAll(edit_ ? &*edit_ : nullptr, custom_working_, custom_saved_, &hp_next, &hp_saved_);
        }();
    if (outcome.ok()) {
        typed_.Clear();
        typing_.End();
        KeepSearchMatchingSavedItem();
        if (edit_ && selected_save_ >= 0) saves_[selected_save_].model = edit_->persisted();
        custom_working_ = custom_saved_;
        hp_working_ = hp_saved_;
        confirm_reset_custom_ = false;
        SyncNameBuffer();
        ui.SetStatus(Tr("Changes applied."), 4.0);
        return;
    }
    if (outcome.custom_failed) {
        ui.SetStatus(Tr("Could not write the custom appearance; nothing was applied."), 8.0);
        return;
    }
    if (outcome.hp_failed) {
        ui.SetStatus(Tr("Could not write the character HP; nothing was applied."), 8.0);
        return;
    }
    switch (outcome.save) {
    case SaveEdit::Result::kInvalidAppearance:
        ui.SetStatus(Tr("A character has an invalid appearance value. Pick a color for it, then Apply Changes."), 8.0);
        break;
    case SaveEdit::Result::kInvalidHp:
        ui.SetStatus(Tr("The current HP cannot be higher than the maximum HP."), 8.0);
        break;
    case SaveEdit::Result::kInvalidInventory:
        ui.SetStatus(InventoryProblemText(edit_ ? edit_->detail() : std::string()), 8.0);
        break;
    case SaveEdit::Result::kInvalidSave:
        ui.SetStatus(Tr("The changes are not valid and were not saved."), 6.0);
        break;
    case SaveEdit::Result::kBackupFailed:
        ui.SetStatus(Tr("Could not create a backup; the save was not changed."), 6.0);
        break;
    case SaveEdit::Result::kStale:
        Refresh();
        ui.SetStatus(Tr("The save changed on disk since the Editor loaded it, so the changes were not written. The Editor "
                        "reloaded the save."),
                     10.0);
        break;
    default:
        ui.SetStatus(Tr("Could not write the save; the original was left untouched."), 6.0);
        break;
    }
}

const char* EditorPage::InventoryProblemText(const std::string& problem) const {
    if (problem == kProblemMaxDurability) return Tr("Max durability exceeds the limit the game keeps.");
    if (problem == kProblemLevel) return Tr("Required level is invalid.");
    if (problem == kProblemModifier) return Tr("Modifier value is outside the supported range.");
    if (problem == kProblemItem) return Tr("This item cannot be changed.");
    return Tr("An inventory change is not valid and was not saved.");
}

void EditorPage::DiscardChanges(trg::Ui& ui) {
    if (edit_) {
        edit_->Discard();
    }
    typed_.Clear();
    typing_.End();
    custom_working_ = custom_saved_;
    hp_working_ = hp_saved_;
    confirm_reset_custom_ = false;
    SyncNameBuffer();
    ui.SetStatus(Tr("Changes discarded."), 4.0);
}

ImTextureID EditorPage::Texture(const SpriteImage* image, int kind, CharacterClass klass, int color) {
    if (!image || image->rgba.empty()) {
        return ImTextureID{};
    }
    const auto key = std::make_tuple(kind, klass, color);
    const auto found = textures_.find(key);
    if (found != textures_.end()) {
        return found->second;
    }
    const ImTextureID texture = host_.CreateTexture(image->rgba.data(), image->width, image->height);
    textures_.emplace(key, texture);
    return texture;
}

// Fits the art inside [min, max], centred, keeping its aspect; a missing asset is named rather than replaced.
std::optional<EditorPage::ArtRect> EditorPage::DrawArt(ImDrawList* draw, ImTextureID texture, const SpriteImage* image,
                                                       ImVec2 min, ImVec2 max, const char* unavailable) {
    if (texture == ImTextureID{} || !image) {
        const char* text = unavailable ? unavailable : Tr("Asset unavailable");
        const ImVec2 size = ImGui::CalcTextSize(text);
        draw->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border));
        draw->PushClipRect(min, max, true);
        draw->AddText(ImVec2(min.x + std::max(0.0f, (max.x - min.x - size.x) * 0.5f), min.y + (max.y - min.y - size.y) * 0.5f),
                      ImGui::GetColorU32(ImGuiCol_TextDisabled), text);
        draw->PopClipRect();
        return std::nullopt;
    }
    const float scale = std::min((max.x - min.x) / float(image->width), (max.y - min.y) / float(image->height));
    const ImVec2 size(float(image->width) * scale, float(image->height) * scale);
    const ImVec2 at(min.x + (max.x - min.x - size.x) * 0.5f, min.y + (max.y - min.y - size.y) * 0.5f);
    draw->AddImage(ImTextureRef(texture), at, ImVec2(at.x + size.x, at.y + size.y));
    return ArtRect{at, scale};
}

void EditorPage::DrawChanges(trg::Ui& ui) {
    ImGuiWindow* body = ImGui::GetCurrentWindow();
    ImGuiWindow* header = body ? body->ParentWindow : nullptr;
    const float scale = ui.scale();
    const float height = ImGui::GetFrameHeight();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float available = body ? body->Size.x : ImGui::GetContentRegionAvail().x;
    const float left_width = header && body ? std::max(0.0f, header->DC.CursorPosPrevLine.x - body->Pos.x) : 0.0f;
    const float status_width = ImGui::CalcTextSize(Tr("Unsaved changes")).x;
    const EditorHeaderGeometry geom =
        LayoutEditorHeader(dirty(), available, left_width, status_width, scale, height, style.ItemSpacing.x, style.ItemSpacing.y);
    const ActionRow& row = geom.row;
    const ActionRowLayout& layout = geom.controls;
    const ImVec2 size(170.0f * scale, 0.0f);

    ImGuiContext& g = *GImGui;
    ImVec2 saved_cursor{}, saved_prev{}, saved_max{}, saved_curr_size{}, saved_prev_size{};
    float saved_curr_base = 0.0f, saved_prev_base = 0.0f;
    bool saved_same_line = false, saved_set_pos = false;
    if (header) {
        saved_cursor = header->DC.CursorPos;
        saved_prev = header->DC.CursorPosPrevLine;
        saved_max = header->DC.CursorMaxPos;
        saved_curr_size = header->DC.CurrLineSize;
        saved_prev_size = header->DC.PrevLineSize;
        saved_curr_base = header->DC.CurrLineTextBaseOffset;
        saved_prev_base = header->DC.PrevLineTextBaseOffset;
        saved_same_line = header->DC.IsSameLine;
        saved_set_pos = header->DC.IsSetPos;
        g.CurrentWindow = header;
        header->DC.CursorPos = ImVec2(body->Pos.x, header->Pos.y + geom.header_y);
        header->DC.CursorPosPrevLine = header->DC.CursorPos;
        header->DC.CurrLineSize = ImVec2(0.0f, 0.0f);
        header->DC.PrevLineSize = ImVec2(0.0f, 0.0f);
        header->DC.CurrLineTextBaseOffset = 0.0f;
        header->DC.PrevLineTextBaseOffset = 0.0f;
        header->DC.IsSameLine = false;
        header->DC.IsSetPos = false;
    }

    header_pair_x_ = (body ? body->Pos.x : 0.0f) + layout.pair_offset;
    header_pair_width_ = layout.pair_width;
    ImGui::Dummy(ImVec2(layout.pair_offset, height));
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::BeginDisabled(!row.apply_enabled);
    const bool apply = row.apply_accent ? ui.AccentButton(Tr("Apply Changes"), size) : ImGui::Button(Tr("Apply Changes"), size);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!row.discard_enabled);
    const bool discard = ImGui::Button(Tr("Discard Changes"), size);
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::Dummy(ImVec2(layout.status_gap, height));
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::Dummy(ImVec2(layout.status_width, height));
    if (*row.status) {
        const char* status = Tr(row.status);
        const ImVec2 text_size = ImGui::CalcTextSize(status);
        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddText(ImVec2(max.x - text_size.x, min.y + (height - text_size.y) * 0.5f),
                                            ImGui::GetColorU32(ui.theme().warn_dot), status);
    }

    if (header) {
        header->DC.CursorPos = saved_cursor;
        header->DC.CursorPosPrevLine = saved_prev;
        header->DC.CursorMaxPos = saved_max;
        header->DC.CurrLineSize = saved_curr_size;
        header->DC.PrevLineSize = saved_prev_size;
        header->DC.CurrLineTextBaseOffset = saved_curr_base;
        header->DC.PrevLineTextBaseOffset = saved_prev_base;
        header->DC.IsSameLine = saved_same_line;
        header->DC.IsSetPos = saved_set_pos;
        g.CurrentWindow = body;
    }
    if (apply) {
        ApplyChanges(ui);
    }
    if (discard) {
        DiscardChanges(ui);
    }
}

void EditorPage::Draw(trg::Ui& ui) {
    const int frame = ImGui::GetFrameCount();
    const bool replaced = selection_ && selection_->generation != seen_generation_;
    if ((frame > last_frame_ + 1 || replaced) && !dirty()) refresh_pending_ = true;
    last_frame_ = frame;
    if (refresh_pending_) {
        if (!loaded_ && !std::exchange(loading_shown_, true)) {
            DrawLoadingView(ui, "Loading Editor...");
            return;
        }
        Refresh();
        refresh_pending_ = false;
        loaded_ = true;
    }
    CollectAppearanceLoad();

    DrawSwitchPrompt(ui);
    if (pending_switch_ != PendingSwitch::kNone) {
        return;
    }
    DrawChanges(ui);

    const float body_width = ImGui::GetContentRegionAvail().x;
    const float body_height =
        std::max(0.0f, ImGui::GetContentRegionAvail().y - (ImGui::GetStyle().ItemSpacing.y + trg::layout::kItemGap * ui.scale()));
    ImGui::SetNextWindowContentSize(
        ImVec2(trg::layout::StableContentWidth(body_width, ImGui::GetStyle().ScrollbarSize), 0.0f));
    ImGui::BeginChild("##editor_body", ImVec2(0.0f, body_height), ImGuiChildFlags_None);
    DrawCharacter(ui);
    ImGui::EndChild();
}

void EditorPage::DrawSwitchPrompt(trg::Ui& ui) {
    if (pending_switch_ == PendingSwitch::kNone) {
        return;
    }
    const PendingSwitch kind = pending_switch_;
    const int target = pending_target_;
    const auto go = [&] {
        switch (kind) {
        case PendingSwitch::kSlot: SelectSlot(target, ui); break;
        default: break;
        }
    };
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().warn_dot);
    ImGui::TextUnformatted(Tr("You have unsaved changes. Apply them, discard them, or cancel."));
    ImGui::PopStyleColor();
    if (ui.AccentButton(Tr("Apply Changes"), ImVec2(170.0f * ui.scale(), 0))) {
        pending_switch_ = PendingSwitch::kNone;
        ApplyChanges(ui);
        if (!dirty()) go();
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr("Discard Changes"), ImVec2(170.0f * ui.scale(), 0))) {
        pending_switch_ = PendingSwitch::kNone;
        DiscardChanges(ui);
        go();
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr("Cancel"))) {
        pending_switch_ = PendingSwitch::kNone;
    }
}

void EditorPage::DrawSaveData(trg::Ui& ui) {
    const SaveModel& model = edit_->working();
    const char* one = Tr("character");
    const char* many = Tr("characters");
    const float row_start_x = ImGui::GetCursorScreenPos().x;
    const char* label = Tr("Save Data");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    const float label_end = row_start_x + ImGui::CalcTextSize(label).x + ImGui::GetStyle().ItemSpacing.x;
    const bool beside = header_pair_x_ >= label_end;
    float width = header_pair_width_;
    if (beside) {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::Dummy(ImVec2(header_pair_x_ - ImGui::GetCursorScreenPos().x, 1.0f));
        ImGui::SameLine(0.0f, 0.0f);
    } else {
        width = ImGui::GetContentRegionAvail().x;
    }
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("##select_slot", DataSlotChoiceLabel(model, selected_slot_, one, many).c_str())) {
        for (int slot = 0; slot < static_cast<int>(kDataSlotCount); ++slot) {
            const bool selected = slot == selected_slot_;
            if (ImGui::Selectable(DataSlotChoiceLabel(model, slot, one, many).c_str(), selected) && !selected) {
                SelectSlot(slot, ui);
            }
        }
        ImGui::EndCombo();
    }
}

bool EditorPage::NumberControl(trg::Ui& ui, const char* id, int* value, NumberRange range, float drag_speed, FieldStyle style) {
    ImGui::PushID(id);
    const int initial = *value;
    const float scale = ui.scale();
    const float height = ImGui::GetFrameHeight();
    const float button = height;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float field = LayoutNumberControl(ImGui::GetContentRegionAvail().x, height, spacing, scale).field_width;
    const ImU32 glyph_color = ImGui::GetColorU32(ImGuiCol_Text);
    const auto glyph = [&](bool plus) {
        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        const ImVec2 mid((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
        const float arm = height * kNumberGlyphRatio * 0.5f;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddLine(ImVec2(mid.x - arm, mid.y), ImVec2(mid.x + arm, mid.y), glyph_color, kNumberGlyphThickness * scale);
        if (plus) draw->AddLine(ImVec2(mid.x, mid.y - arm), ImVec2(mid.x, mid.y + arm), glyph_color, kNumberGlyphThickness * scale);
    };
    ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
    if (ImGui::Button("##minus", ImVec2(button, height))) {
        *value = StepNumber(*value, -1, range);
    }
    ImGui::PopItemFlag();
    glyph(false);
    ImGui::SameLine();
    const ImGuiID field_id = ImGui::GetID("##value");
    const ImVec4 accent = ui.theme().accent;
    if (const std::optional<std::string> handed = typed_.TakeHanded(field_id)) {
        if (const std::optional<int> typed = CommitTypedNumber(*handed, range, *value)) *value = *typed;
    }
    if (typed_.Editing(field_id)) {
        ImGui::SetNextItemWidth(field);
        if (focus_number_) {
            ImGui::SetKeyboardFocusHere();
            focus_number_ = false;
        }
        const bool entered = ImGui::InputText("##value", number_buffer_, sizeof(number_buffer_),
                                              ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue |
                                                  ImGuiInputTextFlags_AutoSelectAll);
        typing_now_ = true;
        const bool finished = entered || ImGui::IsItemDeactivated();
        typed_finished_ = finished;
        if (const std::optional<int> exact = ExactTypedNumber(number_buffer_, range); exact && *exact != *value) *value = *exact;
        if (finished) {
            if (const std::optional<int> typed = CommitTypedNumber(number_buffer_, range, *value)) *value = *typed;
            typed_.End();
        }
    } else {
        const int before = *value;
        int shown = before;
        const std::string text = FormatThousands(before);
        ImGui::SetNextItemWidth(field);
        const float grab_min = kNumberThumbMinWidth * scale;
        ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, grab_min);
        constexpr ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput;
        const bool thumb = NumberHasThumb(range);
        if (thumb) {
            ImGui::SliderInt("##value", &shown, range.min, range.max, "", flags);
        } else {
            ImGui::DragInt("##value", &shown, drag_speed, range.min, range.max, "", flags);
        }
        ImGui::PopStyleVar();
        if (ImGui::IsItemActivated()) {
            drag_id_ = field_id;
            drag_press_x_ = ImGui::GetIO().MousePos.x;
            drag_started_ = false;
        }
        if (ImGui::IsItemActive() && drag_id_ == field_id && !drag_started_) {
            drag_started_ = SliderDragStarted(drag_press_x_, ImGui::GetIO().MousePos.x, scale);
            if (!drag_started_) shown = before;
        }
        // One unit per pixel while Shift is held: a track over thousands of values cannot reach each value otherwise.
        if (ImGui::IsItemActive() && ImGui::GetIO().KeyShift) {
            if (!fine_drag_) {
                fine_drag_ = true;
                fine_anchor_value_ = before;
                fine_anchor_x_ = ImGui::GetIO().MousePos.x;
            }
            shown = FineDragValue(fine_anchor_value_, ImGui::GetIO().MousePos.x - fine_anchor_x_, range);
        } else {
            fine_drag_ = false;
        }
        const ImVec2 frame_min = ImGui::GetItemRectMin(), frame_max = ImGui::GetItemRectMax();
        const float text_width = ImGui::CalcTextSize(text.c_str()).x;
        const float text_x = (frame_min.x + frame_max.x - text_width) * 0.5f;
        ImU32 color = style == FieldStyle::kChanged ? ImGui::GetColorU32(accent) : ImGui::GetColorU32(ImGuiCol_Text);
        if (thumb) {
            const Span grab = SliderGrabSpan(frame_min.x, frame_max.x, range, ClampNumber(shown, range), grab_min);
            if (TextOverSpan({text_x, text_x + text_width}, grab)) color = IM_COL32_WHITE;
        }
        ImGui::GetWindowDrawList()->AddText(ImVec2(text_x, frame_min.y + (frame_max.y - frame_min.y - ImGui::GetTextLineHeight()) * 0.5f), color,
                                            text.c_str());
        const bool activated = ImGui::IsItemActivated();
        const bool double_click = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        if (activated && !double_click) {
            press_id_ = field_id;
            press_value_ = before;
        }
        if (double_click || (activated && ImGui::GetIO().KeyCtrl)) {
            *value = (activated && !double_click) || press_id_ != field_id ? before : press_value_;
            typed_.Begin(field_id, number_buffer_);
            focus_number_ = true;
            std::snprintf(number_buffer_, sizeof(number_buffer_), "%d", *value);
        } else {
            *value = ClampNumber(shown, range);
        }
    }
    ImGui::SameLine();
    ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
    if (ImGui::Button("##plus", ImVec2(button, height))) {
        *value = StepNumber(*value, 1, range);
    }
    ImGui::PopItemFlag();
    glyph(true);
    ImGui::PopID();
    return *value != initial;
}

void EditorPage::ItemField(trg::Ui& ui, const ItemRef& ref, const std::string& key, NumberRange range, float drag_speed, bool changed) {
    SaveModel& edited = edit_->working();
    const std::optional<int> current = ItemFieldValue(edited, ref, key);
    if (!current) return;
    int value = *current;
    const std::vector<uint8_t> start = ItemBytes(edited, ref);
    typing_now_ = false;
    typed_finished_ = false;
    const bool proposed = NumberControl(ui, key.c_str(), &value, range, drag_speed, changed ? FieldStyle::kChanged : FieldStyle::kClean);
    if (typing_now_) {
        if (!typing_.Is(ref, key)) typing_.Begin(ref, key, range, start);
        if (typed_finished_) {
            typing_.Finish(edited, number_buffer_);
        } else {
            typing_.Update(edited, number_buffer_);
        }
    } else if (proposed) {
        SetItemField(edited, ref, key, value);
    }
    if (const std::optional<int> after = ItemFieldValue(edited, ref, key)) visible_.push_back({ref, key, *after});
}

bool EditorPage::PrepareInventorySave(trg::Ui& ui) {
    if (!edit_) return true;
    if (typing_.Active()) {
        if (!ExactTypedNumber(number_buffer_, typing_.range())) {
            ui.SetStatus(Tr("Finish or correct the number you are typing before applying."), 6.0);
            return false;
        }
        typing_.Finish(edit_->working(), number_buffer_);
        typed_.Clear();
    }
    if (ImGui::GetFrameCount() - visible_frame_ > 2) return true;
    const std::optional<UiMismatch> mismatch = FindUiMismatch(edit_->working(), visible_);
    if (mismatch) {
        ui.SetStatus(Tr("The editor values and the item are out of sync; nothing was saved."), 8.0);
        return false;
    }
    return true;
}

void EditorPage::DrawCharacterList(trg::Ui& ui) {
    ui.Heading(CharacterListTitle(Tr("Characters"), selected_slot_).c_str());
    ui.Help(Tr("Select a character to edit it. Nothing is written until Apply Changes."));
    ui.Spacer(2);

    const float scale = ui.scale();
    const float pad = 6.0f * scale;
    const float portrait = 56.0f * scale;
    const float row_height = portrait + pad * 2.0f;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float available = ImGui::GetContentRegionAvail().x;
    const int columns = GridColumns(available, kCardMinWidth * scale, gap, 2);
    const float card_width = GridCellWidth(available, gap, columns, kCardMaxWidth * scale);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, 3.0f * scale));
    int shown = 0;
    for (const CharacterView& character : SelectedSlot()->characters) {
        if (!character.present) {
            continue;
        }
        if (shown++ % columns != 0) {
            ImGui::SameLine();
        }
        ImGui::PushID(character.slot);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 size(card_width, row_height);
        if (ImGui::InvisibleButton("##row", size)) {
            SelectCharacter(character.slot);
        }
        if (ImGui::IsItemHovered()) {
            ImVec4 highlight = ui.theme().frame_hot;
            highlight.w = 0.55f;
            draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), ImGui::GetColorU32(highlight),
                                6.0f * scale);
        }
        const ImVec2 portrait_min(origin.x + pad, origin.y + pad);
        const ImVec2 portrait_max(portrait_min.x + portrait, portrait_min.y + portrait);
        const SpriteImage* face = art_ ? art_->Portrait(character.klass) : nullptr;
        DrawArt(draw, Texture(face, 0, character.klass, 0), face, portrait_min, portrait_max);
        const CharacterRowText text =
            DescribeCharacterRow(character, Tr(ClassName(character.klass).data()), Tr("Lv"), Tr("Slot"));
        const float text_x = portrait_max.x + pad + 6.0f * scale;
        ImGui::PushFont(ui.fonts().semibold, ImGui::GetFontSize());
        draw->AddText(ImVec2(text_x, origin.y + pad + 2.0f * scale), ImGui::GetColorU32(ui.theme().text), text.name.c_str());
        ImGui::PopFont();
        draw->AddText(ImVec2(text_x, origin.y + pad + 24.0f * scale), ImGui::GetColorU32(ui.theme().dim),
                      text.detail.c_str());
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    if (art_ && !art_->Portrait(CharacterClass::kFighter) && !art_->error().empty()) {
        ui.Help((std::string(Tr("Character art unavailable")) + ": " + art_->error()).c_str());
    }
}

void EditorPage::DrawCharacterEditor(trg::Ui& ui) {
    const CharacterView& character = *SelectedCharacter();
    const CharacterView* saved = PersistedCharacter();
    const float scale = ui.scale();
    if (TitleWithButton(ui, Tr("Character Editor"), Tr("Back to Characters"), ButtonIcon::kBack)) {
        nav_.Back();
        return;
    }

    const float row_start = ImGui::GetCursorPosX();
    const float row_width = ImGui::GetContentRegionAvail().x;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const CharacterIdentity identity =
        DescribeIdentity(character, saved, Tr(ClassName(character.klass).data()), Tr("Lv"), Tr("Slot"));
    const ImVec2 text_size = IdentitySize(ui, identity);
    const float gap = 12.0f * scale;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const SummaryLayout layout = LayoutCharacterSummary(row_width, kSummaryPortrait * scale + gap + text_size.x,
                                                        trg::layout::ControlColumnWidth(row_width, scale),
                                                        ImGui::GetFrameHeight(), spacing, scale);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const SpriteImage* face = art_ ? art_->Portrait(character.klass) : nullptr;
    const ImVec2 portrait_min(origin.x, origin.y + (layout.height - layout.portrait) * 0.5f);
    DrawArt(draw, Texture(face, 0, character.klass, 0), face, portrait_min,
            ImVec2(portrait_min.x + layout.portrait, portrait_min.y + layout.portrait));
    DrawIdentity(ui, ImVec2(origin.x + layout.portrait + gap, origin.y), identity, layout.height);
    ImGui::Dummy(ImVec2(layout.portrait + gap + text_size.x, layout.height));
    const SummaryButtons buttons = SplitSummaryButtons(layout, spacing);
    ImGui::SameLine(row_start + layout.button_x);
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, layout.button_y - ImGui::GetStyle().ItemSpacing.y)));
    const bool edit_appearance = IconButton(Tr("Edit Appearance"), [this](int pixels) { return ButtonIconTexture(ButtonIcon::kAppearance, pixels); }, ImVec2(buttons.width, layout.button_height));
    ImGui::SameLine(0.0f, spacing);
    const bool edit_inventory = IconButton(Tr("Edit Inventory"), [this](int pixels) { return ButtonIconTexture(ButtonIcon::kInventory, pixels); }, ImVec2(buttons.width, layout.button_height));
    ImGui::EndGroup();
    if (edit_appearance) {
        RequestAppearance();
        return;
    }
    if (edit_inventory) {
        RequestInventory();
        return;
    }
    if (appearance_failed_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().warn_dot);
        ImGui::TextUnformatted(Tr("Unable to load appearance"));
        ImGui::PopStyleColor();
    }
    const auto changed_style = [&](bool changed) { return changed ? FieldStyle::kChanged : FieldStyle::kClean; };

    ui.Row(Tr("Name"), Tr("The character's name, up to 8 letters."));
    const bool name_changed = saved && FieldStyleFor(saved->name, character.name) == FieldStyle::kChanged;
    if (name_changed) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().accent);
        ImGui::PushStyleColor(ImGuiCol_Border, ui.theme().accent);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    }
    if (ImGui::InputText("##name", name_buffer_, sizeof(name_buffer_))) {
        edit_->working().SetName(selected_slot_, selected_character_, name_buffer_);
    }
    if (name_changed) {
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::PopStyleColor();
    }

    DrawDifficulty(ui, character, saved && saved->difficulty != character.difficulty);

    ui.Row(Tr("Level"));
    int level = SaveModel::DisplayedLevel(character.level_stored);
    const bool level_changed = saved && saved->level_stored != character.level_stored;
    if (NumberControl(ui, "level", &level, kLevelRange, 0.1f, changed_style(level_changed))) {
        edit_->working().SetLevel(selected_slot_, selected_character_, level);
    }

    ui.Row(Tr("Skill Points"));
    int skill_points = character.skill_points;
    const bool skill_changed = saved && saved->skill_points != character.skill_points;
    if (NumberControl(ui, "skill_points", &skill_points, kSkillPointRange, 0.1f, changed_style(skill_changed))) {
        edit_->working().SetSkillPoints(selected_slot_, selected_character_, skill_points);
    }

    const std::optional<CustomKey> key = CurrentKey();
    const std::optional<uint16_t> hp_override = key ? hp_working_.Get(*key) : std::nullopt;
    const std::optional<uint16_t> hp_override_saved = key ? hp_saved_.Get(*key) : std::nullopt;
    const HpValues hp{character.hp_current, character.hp_max};
    ui.Row(Tr("Current HP"));
    int hp_current = hp.current;
    const bool hp_current_changed = saved && saved->hp_current != character.hp_current;
    if (NumberControl(ui, "hp_current", &hp_current, {0, hp.max}, 0.25f, changed_style(hp_current_changed))) {
        const HpValues next = WithCurrentHp(hp, hp_current);
        edit_->working().SetHp(selected_slot_, selected_character_, uint16_t(next.current), uint16_t(next.max));
    }

    ui.Row(Tr("Max HP"));
    int hp_max = hp.max;
    const bool hp_max_changed = (saved && saved->hp_max != character.hp_max) || hp_override != hp_override_saved;
    if (NumberControl(ui, "hp_max", &hp_max, kHpRange, 0.25f, changed_style(hp_max_changed))) {
        const HpValues next = WithMaxHp(hp, hp_max);
        edit_->working().SetHp(selected_slot_, selected_character_, uint16_t(next.current), uint16_t(next.max));
        if (key) {
            if (!hp_override_saved && saved && next.max == saved->hp_max) hp_working_.Clear(*key);
            else hp_working_.Set(*key, next.max > 0 ? std::optional<uint16_t>(uint16_t(next.max)) : std::nullopt);
        }
    }
    ui.EndRows();
}

void EditorPage::DrawDifficulty(trg::Ui& ui, const CharacterView& character, bool changed) {
    ui.Row(Tr("Difficulty"), Tr("Difficulty belongs to this character and can be changed here"));
    const std::string_view name = DifficultyName(character.difficulty);
    const std::string current = name.empty() ? std::string(Tr("Unknown")) : std::string(Tr(std::string(name).c_str()));
    if (changed) {
        ImGui::PushStyleColor(ImGuiCol_Border, ui.theme().accent);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    }
    if (ImGui::BeginCombo("##difficulty", current.c_str())) {
        for (const uint8_t value : kEditableDifficulties) {
            const bool selected = value == character.difficulty;
            const std::string label = std::string(Tr(std::string(DifficultyName(value)).c_str())) + "##difficulty" + std::to_string(value);
            if (ImGui::Selectable(label.c_str(), selected) && !selected) {
                edit_->working().SetDifficulty(selected_slot_, selected_character_, value);
            }
        }
        ImGui::EndCombo();
    }
    if (changed) {
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
}

void EditorPage::DrawCharacter(trg::Ui& ui) {
    if (!SelectedBuild()) {
        ui.Help(Tr("Install the game first to edit characters"));
        return;
    }
    if (nav_.editing() && (!SelectedCharacter() || nav_.open() != selected_character_)) {
        nav_.Back();
    }
    if (nav_.loading()) {
        DrawAppearanceLoading(ui);
        return;
    }
    if (nav_.appearance()) {
        DrawAppearance(ui);
        return;
    }
    if (nav_.inventory()) {
        DrawInventory(ui);
        return;
    }
    if (nav_.editing()) {
        DrawCharacterEditor(ui);
        return;
    }
    const std::vector<size_t> visible = VisibleSaves();
    if (visible.empty()) {
        ui.Help(Tr("No saves found for the installed game"));
        return;
    }
    if (!SelectedSlot()) {
        ui.Help(Tr("This save could not be loaded."));
        return;
    }
    DrawSaveData(ui);
    ui.Help(InstalledGameLine(Tr("Installed game"), SelectedBuild()->build->label, visible.size(), Tr("save found"),
                              Tr("saves found"))
                .c_str());
    DrawCharacterList(ui);
}

// The title, then its button right after it on the same line.
bool EditorPage::TitleWithButton(trg::Ui& ui, const char* title, const char* button, ButtonIcon icon) {
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(ui.fonts().semibold, ImGui::GetFontSize());
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::SameLine();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight();
    const float glyph = float(std::max(1, int(std::lround(height * 0.5f))));
    const ImVec2 size(ImGui::CalcTextSize(button).x + glyph + style.ItemInnerSpacing.x + style.FramePadding.x * 2.0f, height);
    return IconButton(button, [this, icon](int pixels) { return ButtonIconTexture(icon, pixels); }, size);
}

void EditorPage::RequestAppearance() {
    const DiscoveredSave* save = SelectedSave();
    const CharacterView* character = SelectedCharacter();
    if (!save || !character || !nav_.RequestAppearance()) return;
    appearance_failed_ = false;
    appearance_job_ = AppearanceJob{art_root_, save->save0_path, selected_slot_, selected_character_, character->klass, 0};
}

bool EditorPage::AppearanceJobCurrent() const {
    const DiscoveredSave* save = SelectedSave();
    const CharacterView* character = SelectedCharacter();
    if (!appearance_job_ || !save || !character || !art_ || !nav_.loading()) return false;
    const AppearanceJob& job = *appearance_job_;
    return job.art_root == art_root_ && job.save == save->save0_path && job.slot == selected_slot_ &&
           job.character == selected_character_ && nav_.open() == job.character && job.klass == character->klass;
}

void EditorPage::DrawAppearanceLoading(trg::Ui& ui) {
    DrawLoadingView(ui, "Loading appearance...");
    // The work starts only after this frame has been drawn with the loading state.
    AdvanceAppearanceLoad();
}

void EditorPage::AdvanceAppearanceLoad() {
    if (!AppearanceJobCurrent()) {
        appearance_job_.reset();
        nav_.FinishAppearance(false);
        return;
    }
    if (appearance_job_->serial != 0) return;
    const CharacterView& character = *SelectedCharacter();
    const int color = character.color_valid() ? int(character.color) : -1;
    if (art_->Ready(character.klass, color)) {
        appearance_job_.reset();
        nav_.FinishAppearance(true);
        return;
    }
    AppearanceRequest request{art_->sources(), character.klass, color, CurrentLook(character)};
    // Zero while an abandoned job still runs; the next frame tries again.
    appearance_job_->serial = appearance_loader_.Start([request = std::move(request)] { return PrepareAppearance(request); });
}

void EditorPage::CollectAppearanceLoad() {
    std::optional<std::pair<uint64_t, AppearancePrepared>> done = appearance_loader_.Collect();
    if (!done) return;
    const bool current = AppearanceJobCurrent() && appearance_job_->serial == done->first;
    if (!current) return;
    const CharacterClass klass = appearance_job_->klass;
    appearance_job_.reset();
    AppearancePrepared& prepared = done->second;
    if (prepared.ok && prepared.art) {
        art_->Adopt(std::move(*prepared.art), klass);
        nav_.FinishAppearance(true);
        return;
    }
    std::printf("Editor: appearance could not be loaded: %s\n", prepared.error.c_str());
    nav_.FinishAppearance(false);
    appearance_failed_ = true;
}

std::optional<CustomKey> EditorPage::CurrentKey() const {
    const DiscoveredSave* save = SelectedSave();
    const CharacterView* character = SelectedCharacter();
    if (!save || !character) return std::nullopt;
    return CustomKey{save->title_id, save->dir_name, selected_slot_, selected_character_, character->klass};
}

CustomLook EditorPage::CurrentLook(const CharacterView& character) const {
    const std::optional<CustomKey> key = CurrentKey();
    const uint8_t base = character.color_valid() ? character.color : 0;
    return EditableLook(key ? custom_working_.Get(*key) : std::nullopt, base);
}

void EditorPage::SetLook(const CustomLook& look) {
    if (const std::optional<CustomKey> key = CurrentKey()) {
        custom_working_.Set(*key, StorableLook(look));
    }
}

void EditorPage::RestoreLook(const std::optional<CustomLook>& stored) {
    if (const std::optional<CustomKey> key = CurrentKey()) {
        custom_working_.Set(*key, stored.value_or(CustomLook{}));
    }
}

ImTextureID EditorPage::Replace(ImTextureID& slot, std::string& slot_key, const std::string& key, const uint8_t* rgba,
                                int width, int height) {
    if (slot != ImTextureID{} && slot_key == key) return slot;
    if (slot != ImTextureID{}) host_.DestroyTexture(slot);
    slot = rgba ? host_.CreateTexture(rgba, width, height) : ImTextureID{};
    slot_key = key;
    return slot;
}

namespace {

std::string LookText(const CustomLook& look) {
    std::string text = std::to_string(look.base) + (look.enabled ? "+" : "-");
    for (const PartLook& part : look.parts) {
        text += part.key + (part.hidden ? "h" : "") +
                (part.color ? std::to_string(part.color->r) + "." + std::to_string(part.color->g) + "." +
                                  std::to_string(part.color->b)
                            : std::string()) +
                ";";
    }
    return text;
}

} // namespace

void EditorPage::DrawAppearance(trg::Ui& ui) {
    if (!SelectedCharacter()) {
        ui.Help(Tr("Select a character first."));
        return;
    }
    if (TitleWithButton(ui, Tr("Appearance"), Tr("Back to Character Editor"), ButtonIcon::kBack)) {
        nav_.BackToCharacter();
        return;
    }
    const CharacterView& character = *SelectedCharacter();
    const bool valid = character.color_valid();
    const int color = character.color;
    const CustomLook look = CurrentLook(character);
    const std::span<const CustomPart> parts = CustomPartsOf(character.klass);
    if (!FindCustomPart(character.klass, selected_part_)) {
        selected_part_ = parts.empty() ? std::string() : std::string(parts.front().key);
    }

    const float scale = ui.scale();
    const int columns = AppearanceColumns(ImGui::GetContentRegionAvail().x, scale);
    if (!ImGui::BeginTable("##appearance", columns, ImGuiTableFlags_SizingStretchProp)) {
        return;
    }
    if (columns == 2) {
        ImGui::TableSetupColumn("##figure_column", ImGuiTableColumnFlags_WidthStretch, kPreviewColumnWeight);
        ImGui::TableSetupColumn("##controls_column", ImGuiTableColumnFlags_WidthStretch, kControlsColumnWeight);
    }
    ImGui::TableNextColumn();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail(ImGui::GetContentRegionAvail().x, 0.0f);
    preview_right_ = origin.x + avail.x;
    const float art_height = PreviewHeight(avail.x, scale);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const SpriteImage* figure = nullptr;
    ImTextureID texture{};
    if (valid && art_) {
        figure = art_->CustomPreview(character.klass, color, look);
        if (look.active() && figure) {
            texture = Replace(custom_texture_, custom_texture_key_,
                              std::to_string(int(character.klass)) + "|" + std::to_string(color) + "|" + LookText(look),
                              figure->rgba.data(), figure->width, figure->height);
        } else {
            texture = Texture(figure, 1, character.klass, color);
        }
    }
    std::optional<ArtRect> placed;
    float figure_height = art_height;
    const std::optional<PixelBox> crop = valid && figure ? PreviewCrop(character.klass, color) : std::nullopt;
    if (texture != ImTextureID{} && figure && crop) {
        const ArtFit fit = FitArt(origin.x, origin.y, avail.x, art_height, *crop);
        draw->AddImage(ImTextureRef(texture), ImVec2(fit.x, fit.y), ImVec2(fit.x + fit.width, fit.y + fit.height),
                       ImVec2(float(crop->x0) / float(figure->width), float(crop->y0) / float(figure->height)),
                       ImVec2(float(crop->x1) / float(figure->width), float(crop->y1) / float(figure->height)));
        placed = ArtRect{ImVec2(fit.x - float(crop->x0) * fit.scale, fit.y - float(crop->y0) * fit.scale), fit.scale};
        figure_height = fit.height;
    } else {
        placed = DrawArt(draw, texture, figure, origin, ImVec2(origin.x + avail.x, origin.y + art_height),
                         valid ? nullptr : Tr("Invalid appearance value"));
    }
    ImGui::InvisibleButton("##figure", ImVec2(avail.x, figure_height));
    const CustomPart* pointed = nullptr;
    if (placed && figure && art_ && ImGui::IsItemHovered()) {
        const ImVec2 mouse = ImGui::GetMousePos();
        const ImagePoint at = MapToImage(mouse.x, mouse.y, placed->min.x, placed->min.y, placed->scale);
        pointed = art_->PartAt(character.klass, *figure, at.x, at.y);
        if (pointed) {
            ImGui::SetTooltip("%s", Tr(std::string(pointed->label).c_str()));
        }
        if (pointed && ImGui::IsItemClicked()) {
            selected_part_ = std::string(pointed->key);
        }
    }
    const CustomPart* highlight = pointed;
    if (placed && figure && highlight) {
        const std::string key = std::string(highlight->key) + "|" + std::to_string(int(character.klass)) + "|" +
                                std::to_string(color) + "|" + LookText(look);
        if (overlay_key_ != key) {
            const std::vector<uint8_t> overlay = art_->PartOverlay(character.klass, *figure, *highlight, {255, 214, 64, 70});
            Replace(overlay_texture_, overlay_key_, key, overlay.data(), figure->width, figure->height);
        }
        draw->AddImage(ImTextureRef(overlay_texture_), placed->min,
                       ImVec2(placed->min.x + float(figure->width) * placed->scale,
                              placed->min.y + float(figure->height) * placed->scale));
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().dim);
    ImGui::TextWrapped("%s", Tr("The preview draws the default weapon, not the equipped one."));
    ImGui::PopStyleColor();

    ImGui::TableNextColumn();
    const CharacterIdentity identity =
        DescribeIdentity(character, PersistedCharacter(), Tr(ClassName(character.klass).data()), Tr("Lv"), Tr("Slot"));
    const ImVec2 identity_size = IdentitySize(ui, identity);
    DrawIdentity(ui, ImGui::GetCursorScreenPos(), identity, identity_size.y);
    ImGui::Dummy(identity_size);
    if (!custom_error_.empty()) {
        ui.Help(Tr("Saved custom looks could not be read."));
    }
    DrawCustom(ui, character, look);
    ImGui::EndTable();
}

std::optional<PixelBox> EditorPage::PreviewCrop(CharacterClass klass, int color) {
    const auto key = std::make_pair(klass, color);
    if (const auto found = preview_crops_.find(key); found != preview_crops_.end()) return found->second;
    const SpriteImage* native = art_ ? art_->Preview(klass, color) : nullptr;
    if (!native || native->rgba.empty()) return std::nullopt;
    return preview_crops_[key] = OpaqueBounds(*native, 4);
}

IdentityLine EditorPage::IdentityLineOf(trg::Ui& ui, float row_height) const {
    ImGui::PushFont(ui.fonts().semibold, ImGui::GetFontSize());
    const float name_ascent = ImGui::GetFontBaked()->Ascent, name_height = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    return LayoutIdentityLine(name_ascent, name_height, ImGui::GetFontBaked()->Ascent, ImGui::GetTextLineHeight(), row_height);
}

ImVec2 EditorPage::IdentitySize(trg::Ui& ui, const CharacterIdentity& identity) const {
    ImGui::PushFont(ui.fonts().semibold, ImGui::GetFontSize());
    float width = ImGui::CalcTextSize(identity.name.c_str()).x;
    ImGui::PopFont();
    width += ImGui::CalcTextSize("  ").x;
    for (size_t i = 0; i < identity.parts.size(); ++i) {
        width += ImGui::CalcTextSize(identity.parts[i].text.c_str()).x + (i > 0 ? ImGui::CalcTextSize(kMiddleDot).x : 0.0f);
    }
    return ImVec2(width, IdentityLineOf(ui, 0.0f).height);
}

// The one identity line of the Character Editor and Appearance pages, centred in a row of `row_height`.
void EditorPage::DrawIdentity(trg::Ui& ui, ImVec2 at, const CharacterIdentity& identity, float row_height) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 dim = ImGui::GetColorU32(ui.theme().dim);
    const IdentityLine line = IdentityLineOf(ui, row_height);
    ImGui::PushFont(ui.fonts().semibold, ImGui::GetFontSize());
    draw->AddText(ImVec2(at.x, at.y + line.name_y), ImGui::GetColorU32(ui.theme().text), identity.name.c_str());
    float x = at.x + ImGui::CalcTextSize(identity.name.c_str()).x;
    ImGui::PopFont();
    x += ImGui::CalcTextSize("  ").x;
    const float y = at.y + line.detail_y;
    for (size_t i = 0; i < identity.parts.size(); ++i) {
        if (i > 0) {
            draw->AddText(ImVec2(x, y), dim, kMiddleDot);
            x += ImGui::CalcTextSize(kMiddleDot).x;
        }
        const IdentityPart& part = identity.parts[i];
        draw->AddText(ImVec2(x, y), part.changed ? ImGui::GetColorU32(ui.theme().accent) : dim, part.text.c_str());
        x += ImGui::CalcTextSize(part.text.c_str()).x;
    }
}

void EditorPage::DrawBaseColor(trg::Ui& ui, const CharacterView& character, CustomLook look) {
    ui.Row(Tr("Base Color"), Tr("The native color the custom changes are drawn over."));
    const std::string current =
        character.color_valid() ? ColorName(character.klass, character.color) : std::string(Tr("Invalid appearance value"));
    if (ImGui::BeginCombo("##base_color", current.c_str())) {
        for (int preset = 0; preset < kNativeColorCount; ++preset) {
            const std::string name = ColorName(character.klass, preset);
            const bool selected = character.color_valid() && preset == character.color;
            if (ImGui::Selectable((name + "##base" + std::to_string(preset)).c_str(), selected) && !selected) {
                edit_->working().SetColor(selected_slot_, selected_character_, preset);
                look.base = uint8_t(preset);
                SetLook(look);
            }
        }
        ImGui::EndCombo();
    }
}

void EditorPage::DrawCustom(trg::Ui& ui, const CharacterView& character, CustomLook look) {
    DrawBaseColor(ui, character, look);
    const bool editable = character.color_valid() && custom_error_.empty() && art_ && art_->Regions(character.klass) &&
                          !CustomPartsOf(character.klass).empty();
    if (!editable) {
        ui.EndRows();
        return;
    }
    ui.Row(Tr("Selected Part"), Tr("Click the character, or choose a part here."));
    const CustomPart* part = FindCustomPart(character.klass, selected_part_);
    if (ImGui::BeginCombo("##selected_part", part ? Tr(std::string(part->label).c_str()) : "")) {
        for (const CustomPart& candidate : CustomPartsOf(character.klass)) {
            if (ImGui::Selectable(Tr(std::string(candidate.label).c_str()), part == &candidate)) {
                selected_part_ = std::string(candidate.key);
            }
        }
        ImGui::EndCombo();
    }
    if (part) {
        const PartLook* current = look.Find(part->key);
        const bool has_override = current && (current->color || current->hidden);
        const std::optional<Rgb> current_color = current ? current->color : std::nullopt;
        const bool current_hidden = current && current->hidden;
        ui.Row(Tr("Part Color"), Tr("Recolors the part and keeps its painted shading."));
        const std::optional<Rgb> native = art_->PartAverage(character.klass, character.color, *part);
        const Rgb shown = current_color.value_or(native.value_or(Rgb{128, 128, 128}));
        const ImVec4 swatch(shown.r / 255.0f, shown.g / 255.0f, shown.b / 255.0f, 1.0f);
        const float frame = ImGui::GetFrameHeight();
        if (ImGui::ColorButton("##part_color", swatch, ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoTooltip,
                               ImVec2(frame * 2.0f, frame))) {
            pick_ = ColorPick{};
            pick_.active = true;
            pick_.part = std::string(part->key);
            pick_.before = custom_working_.Get(*CurrentKey());
            pick_.working[0] = pick_.original[0] = swatch.x;
            pick_.working[1] = pick_.original[1] = swatch.y;
            pick_.working[2] = pick_.original[2] = swatch.z;
            pick_.swatch_min = ImGui::GetItemRectMin();
            pick_.swatch_max = ImGui::GetItemRectMax();
            ImGui::OpenPopup("##color_picker");
        }
        DrawColorPicker(ui, look, *part);
        ImGui::SameLine();
        if (ImGui::Button(Tr("Random"))) {
            look.SetPartColor(part->key, NextRandomColor(random_, current_color));
            SetLook(look);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!has_override);
        if (ImGui::Button(Tr("Reset Part"))) {
            look.ResetPart(part->key);
            SetLook(look);
        }
        ImGui::EndDisabled();
        const std::string visibility_label = PartVisibilityLabel(Tr("Part Visibility"), Tr(std::string(part->label).c_str()));
        ui.Row(visibility_label.c_str(), part->reveals_unpainted ? Tr("Hiding it can show areas the game never paints.") : nullptr);
        const int visibility = ui.Segmented("##part_visibility", {Tr("Shown"), Tr("Hidden")}, current_hidden ? 1 : 0);
        if (visibility >= 0 && (visibility == 1) != current_hidden) {
            look.SetPartHidden(part->key, visibility == 1);
            SetLook(look);
        }
    }
    ui.EndRows();
    DrawAllColorsRandom(ui, character.klass, look);
    DrawResetCustom(ui, look);
}

void EditorPage::DrawAllColorsRandom(trg::Ui& ui, CharacterClass klass, CustomLook look) {
    const bool randomize = ui.ButtonRow(Tr("All Colors Random"), Tr("Gives every part a new color; visibility and the base color stay."),
                                        Tr("All Colors Random"));
    ui.EndRows();
    if (randomize) {
        look.RandomizeColors(klass, random_);
        SetLook(look);
    }
}

void EditorPage::DrawResetCustom(trg::Ui& ui, CustomLook look) {
    if (!confirm_reset_custom_) {
        ImGui::BeginDisabled(look.OverrideCount() == 0);
        const bool reset = ui.ButtonRow(Tr("Reset Custom"), Tr("Removes every custom change and shows the base color again."),
                                        Tr("Reset Custom"));
        ImGui::EndDisabled();
        ui.EndRows();
        if (!reset) {
            return;
        }
        if (look.OverrideCount() > 1) {
            confirm_reset_custom_ = true;
        } else {
            look.ResetOverrides();
            SetLook(look);
        }
        return;
    }
    ui.Row(Tr("Reset Custom"), Tr("Remove every custom change of this character?"));
    const float half = std::floor((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Button, ui.theme().warn);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui.theme().warn_dot);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ui.theme().warn_dot);
    const bool remove = ImGui::Button(Tr("Remove changes"), ImVec2(half, 0.0f));
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    const bool keep = ImGui::Button(Tr("Keep changes"), ImVec2(-FLT_MIN, 0.0f));
    ui.EndRows();
    if (remove) {
        look.ResetOverrides();
        SetLook(look);
    }
    if (remove || keep) {
        confirm_reset_custom_ = false;
    }
}

void EditorPage::DrawColorPicker(trg::Ui& ui, CustomLook& look, const CustomPart& part) {
    const float scale = ui.scale();
    const ImGuiViewport* view = ImGui::GetMainViewport();
    const PickerPoint at = PlacePicker({pick_.swatch_min.x, pick_.swatch_min.y}, {pick_.swatch_max.x, pick_.swatch_max.y},
                                       {kPickerWidth * scale, kPickerHeight * scale}, {view->WorkPos.x, view->WorkPos.y},
                                       {view->WorkPos.x + view->WorkSize.x, view->WorkPos.y + view->WorkSize.y}, preview_right_,
                                       12.0f * scale);
    ImGui::SetNextWindowPos(ImVec2(at.x, at.y), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(kPickerWidth * scale, 0.0f), ImVec2(kPickerWidth * scale, kPickerHeight * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, std::max(1.0f, scale));
    ImGui::PushStyleColor(ImGuiCol_Border, ui.theme().border_panel);
    const bool open = pick_.active && pick_.part == part.key && ImGui::BeginPopup("##color_picker");
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (!open && !(pick_.active && pick_.part == part.key)) {
        return;
    }
    bool ok = false, cancel = false, enter = false;
    if (open) {
        ImGui::SetNextItemWidth(220.0f * scale);
        const ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_PickerHueBar |
                                          ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_DisplayHSV |
                                          ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_Uint8;
        if (ImGui::ColorPicker4("##picker", pick_.working, flags, pick_.original)) {
            look.SetPartColor(part.key, Rgb{uint8_t(std::lround(pick_.working[0] * 255.0f)),
                                            uint8_t(std::lround(pick_.working[1] * 255.0f)),
                                            uint8_t(std::lround(pick_.working[2] * 255.0f))});
            SetLook(look);
        }
        const ImVec2 picker_max = ImGui::GetItemRectMax();
        const float column = 3.0f * ImGui::GetFrameHeight();
        ImGui::SameLine(picker_max.x - ImGui::GetCurrentWindow()->Pos.x - column);
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(0.0f, PickerSwatchesHeight(ImGui::GetFrameHeight(), ImGui::GetTextLineHeight(), ImGui::GetStyle().ItemSpacing.y)));
        ok = ui.AccentButton(Tr("OK"), ImVec2(column, 0.0f));
        cancel = ImGui::Button(Tr("Cancel"), ImVec2(column, 0.0f));
        ImGui::EndGroup();
        enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    }
    const PickerOutcome outcome = ResolvePicker(open, ok, cancel, enter, ImGui::IsKeyPressed(ImGuiKey_Escape, false));
    if (outcome == PickerOutcome::kCancel) {
        RestoreLook(pick_.before);
        look = EditableLook(pick_.before, look.base);
    }
    if (outcome != PickerOutcome::kOpen) {
        pick_.active = false;
        if (open) ImGui::CloseCurrentPopup();
    }
    if (open) {
        ImGui::EndPopup();
    }
}

} // namespace dcp
