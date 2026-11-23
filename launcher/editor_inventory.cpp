#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <vector>

#include "editor_page.h"
#include "host.h"
#include "loading_view.h"
#include "page_margin.h"
#include "localization/ui_text.h"
#include "trg/launcher.h"
#include "trg/layout.h"

namespace dcp {
namespace {

const char* CategoryLabel(ItemCategory category) {
    switch (category) {
    case ItemCategory::kWeapon: return "Weapon";
    case ItemCategory::kShield: return "Shield";
    case ItemCategory::kAccessory: return "Accessory";
    case ItemCategory::kItem: return "Item";
    case ItemCategory::kOther: break;
    }
    return "Other";
}

std::string WhereLabel(const ItemRef& ref, bool twin = false) {
    std::string where = ref.storage() ? std::string(Tr("Shared storage")) : std::string(Tr("Bag")) + " " + std::to_string(ref.set + 1);
    if (twin) where += " \xC2\xB7 " + std::string(Tr("Slot")) + " " + std::to_string(ref.index + 1);
    return where;
}

const char* GroupLabel(InventoryGroup group) {
    switch (group) {
    case InventoryGroup::kWeaponsShields: return "Weapons / Shields";
    case InventoryGroup::kAccessories: return "Accessories";
    case InventoryGroup::kItems: return "Items";
    case InventoryGroup::kOther: break;
    }
    return "Other";
}

const char* SortLabel(InventorySort sort) {
    switch (sort) {
    case InventorySort::kName: return "Name";
    case InventorySort::kCategory: return "Category";
    case InventorySort::kLevel: return "Equipment level";
    case InventorySort::kRank: break;
    }
    return "Rank";
}

void PushItemId(const ItemRef& ref) {
    ImGui::PushID(ref.slot);
    ImGui::PushID(ref.character);
    ImGui::PushID(ref.set);
    ImGui::PushID(ref.index);
}

void PopItemId() {
    for (int i = 0; i < 4; ++i) ImGui::PopID();
}

ImU32 WithAlpha(ImU32 color, float alpha) {
    ImVec4 value = ImGui::ColorConvertU32ToFloat4(color);
    value.w *= alpha;
    return ImGui::GetColorU32(value);
}

} // namespace

void DrawItemPlaceholder(ImDrawList* draw, ImVec2 min, float size, ItemCategory category, ImU32 color) {
    const float rounding = size * 0.18f;
    const float thickness = std::max(1.0f, size * 0.06f);
    draw->AddRectFilled(min, ImVec2(min.x + size, min.y + size), WithAlpha(color, 0.12f), rounding);
    draw->AddRect(min, ImVec2(min.x + size, min.y + size), WithAlpha(color, 0.45f), rounding, 0, thickness);
    const ImVec2 mid(min.x + size * 0.5f, min.y + size * 0.5f);
    const float arm = size * 0.24f;
    switch (category) {
    case ItemCategory::kWeapon:
        draw->AddLine(ImVec2(mid.x - arm, mid.y + arm), ImVec2(mid.x + arm, mid.y - arm), color, thickness * 1.5f);
        break;
    case ItemCategory::kShield:
        draw->AddQuad(ImVec2(mid.x - arm, mid.y - arm), ImVec2(mid.x + arm, mid.y - arm), ImVec2(mid.x + arm * 0.6f, mid.y + arm),
                      ImVec2(mid.x - arm * 0.6f, mid.y + arm), color, thickness * 1.5f);
        break;
    case ItemCategory::kAccessory: draw->AddCircle(mid, arm, color, 0, thickness * 1.5f); break;
    case ItemCategory::kItem:
    case ItemCategory::kOther: draw->AddCircleFilled(mid, arm * 0.45f, color); break;
    }
}

bool IconButton(const char* label, const std::function<ImTextureID(int pixels)>& icon, ImVec2 size) {
    ImGui::PushID(label);
    const bool pressed = ImGui::Button("##icon_button", size);
    ImGui::PopID();
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
    const float height = max.y - min.y;
    const int pixels = std::max(1, int(std::lround(height * 0.5f)));
    const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
    const ImVec2 text = ImGui::CalcTextSize(label);
    const bool room = max.x - min.x >= text.x + pixels + gap + ImGui::GetStyle().FramePadding.x * 2.0f;
    const ImTextureID texture = room && icon ? icon(pixels) : ImTextureID{};
    const bool drawn = texture != ImTextureID{};
    float x = min.x + ((max.x - min.x) - text.x - (drawn ? pixels + gap : 0.0f)) * 0.5f;
    if (drawn) {
        const ImVec2 at(std::round(x), std::round(min.y + (height - pixels) * 0.5f));
        draw->AddImage(texture, at, ImVec2(at.x + pixels, at.y + pixels), ImVec2(0, 0), ImVec2(1, 1), color);
        x += pixels + gap;
    }
    draw->AddText(ImVec2(x, min.y + (height - text.y) * 0.5f), color, label);
    return pressed;
}

bool EditorPage::DrawGameImage(ImDrawList* draw, const SpriteImage* image, ImTextureID& texture, ImVec2 min, float size, ImU32 tint) {
    if (!image) return false;
    if (texture == ImTextureID{}) texture = host_.CreateTexture(image->rgba.data(), image->width, image->height);
    if (texture == ImTextureID{}) return false;
    const float scale = size / float(std::max(image->width, image->height));
    const float w = float(image->width) * scale, h = float(image->height) * scale;
    const ImVec2 at(min.x + (size - w) * 0.5f, min.y + (size - h) * 0.5f);
    draw->AddImage(ImTextureRef(texture), at, ImVec2(at.x + w, at.y + h), ImVec2(0, 0), ImVec2(1, 1), tint);
    return true;
}

void EditorPage::DrawItemIcon(ImDrawList* draw, ImVec2 min, float size, uint32_t definition, ItemCategory category, ImU32 fallback) {
    const int key = icons_.PictureKey(definition);
    if (key < 0 || !DrawGameImage(draw, icons_.Find(definition), icon_textures_[key], min, size, IM_COL32_WHITE)) {
        DrawItemPlaceholder(draw, min, size, category, fallback);
    }
}

void EditorPage::KeepSearchMatchingSavedItem() {
    if (!edit_ || !inventory_selection_.item) return;
    const std::string kept = SearchKeptAfterSave(edit_->persisted(), *inventory_selection_.item, Catalog(), inventory_query_.search);
    if (kept == inventory_query_.search) return;
    inventory_query_.search = kept;
    std::snprintf(inventory_search_, sizeof(inventory_search_), "%s", kept.c_str());
}

void EditorPage::RequestInventory() {
    nav_.RequestInventory();
}

void EditorPage::ResetInventoryView() {
    inventory_selection_.Clear();
    inventory_query_ = InventoryQuery{};
    inventory_search_[0] = 0;
}

const ItemCatalog& EditorPage::Catalog() const { return catalog_; }

// The catalog is immutable once loaded and shared by every character and DATA slot. The worker owns its own
// CharacterArt, and the future joins in the destructor, so the job never outlives the page.
bool EditorPage::CatalogReady() {
    if (!art_) return true;
    const std::string suffix = message_suffix_ ? message_suffix_() : std::string();
    const std::string key = art_root_.string() + "|" + suffix;
    if (key == catalog_key_) return true;
    if (catalog_job_.valid()) {
        if (catalog_job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        ItemAssets loaded = catalog_job_.get();
        if (catalog_job_key_ == key) {
            catalog_ = std::move(loaded.catalog);
            for (const auto& [picture, texture] : icon_textures_) host_.DestroyTexture(texture);
            icon_textures_.clear();
            icons_ = std::move(loaded.icons);
            catalog_key_ = key;
            return true;
        }
    }
    catalog_job_key_ = key;
    catalog_job_ = std::async(std::launch::async, [sources = art_->sources(), suffix] {
        CharacterArt art(sources);
        return ItemAssets{ItemCatalog(art.Sheet("ItemList", suffix), art.Sheet("CustomItem", suffix), NameWords(art.Sheet("SystemMsg", suffix))),
                          LoadItemIcons(art)};
    });
    return false;
}

void EditorPage::DrawInventorySummary(trg::Ui& ui, const CharacterView& character, const CharacterView* saved) {
    const float scale = ui.scale();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const CharacterIdentity identity =
        DescribeIdentity(character, saved, Tr(ClassName(character.klass).data()), Tr("Lv"), Tr("Slot"));
    const ImVec2 text_size = IdentitySize(ui, identity);
    const float gap = 12.0f * scale;
    const SummaryLayout layout = LayoutCharacterSummary(ImGui::GetContentRegionAvail().x,
                                                        kSummaryPortrait * scale + gap + text_size.x, 0.0f,
                                                        ImGui::GetFrameHeight(), ImGui::GetStyle().ItemSpacing.x, scale);
    const SpriteImage* face = art_ ? art_->Portrait(character.klass) : nullptr;
    const ImVec2 portrait_min(origin.x, origin.y + (layout.height - layout.portrait) * 0.5f);
    DrawArt(ImGui::GetWindowDrawList(), Texture(face, 0, character.klass, 0), face, portrait_min,
            ImVec2(portrait_min.x + layout.portrait, portrait_min.y + layout.portrait));
    DrawIdentity(ui, ImVec2(origin.x + layout.portrait + gap, origin.y), identity, layout.height);
    ImGui::Dummy(ImVec2(layout.portrait + gap + text_size.x, layout.height));
}

void EditorPage::DrawInventory(trg::Ui& ui) {
    const CharacterView* character = SelectedCharacter();
    if (!character || !edit_) {
        nav_.BackToCharacter();
        return;
    }
    if (!CatalogReady()) {
        DrawLoadingView(ui, "Loading inventory...");
        return;
    }
    if (TitleWithButton(ui, Tr("Inventory Editor"), Tr("Back to Character Editor"), ButtonIcon::kBack)) {
        nav_.BackToCharacter();
        return;
    }
    DrawInventorySummary(ui, *character, PersistedCharacter());
    ui.Help(Tr("Attack, durability, required level and the modifier values can be changed. "
               "Nothing is written until Apply Changes."));
    if (!Catalog().loaded()) ui.Help(Tr("Item names are unavailable because the game files could not be read."));

    InventoryQuery other_query;
    other_query.group = InventoryGroup::kOther;
    const bool has_other = !BuildInventoryList(edit_->working(), edit_->persisted(), selected_slot_, selected_character_, Catalog(), other_query).rows.empty();
    if (!has_other && inventory_query_.group == InventoryGroup::kOther) inventory_query_.group = InventoryGroup::kWeaponsShields;
    const InventoryQuery before = inventory_query_;
    const float scale = ui.scale();
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(190.0f * scale);
    if (ImGui::BeginCombo("##inventory_group", Tr(GroupLabel(inventory_query_.group)))) {
        for (const InventoryGroup group : {InventoryGroup::kWeaponsShields, InventoryGroup::kAccessories, InventoryGroup::kItems,
                                           InventoryGroup::kOther}) {
            if (group == InventoryGroup::kOther && !has_other) continue;
            if (ImGui::Selectable(Tr(GroupLabel(group)), group == inventory_query_.group)) inventory_query_.group = group;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0.0f, gap);
    ImGui::SetNextItemWidth(std::max(120.0f * scale, ImGui::GetContentRegionAvail().x - 190.0f * scale - gap));
    ImGui::InputTextWithHint("##inventory_search", Tr("Search items..."), inventory_search_, sizeof(inventory_search_));
    inventory_query_.search = inventory_search_;
    ImGui::SameLine(0.0f, gap);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##inventory_sort", (std::string(Tr("Sort")) + ": " + Tr(SortLabel(inventory_query_.sort))).c_str())) {
        for (const InventorySort sort : {InventorySort::kName, InventorySort::kCategory, InventorySort::kLevel, InventorySort::kRank}) {
            if (ImGui::Selectable(Tr(SortLabel(sort)), sort == inventory_query_.sort)) inventory_query_.sort = sort;
        }
        ImGui::EndCombo();
    }
    const InventoryList list = BuildInventoryList(edit_->working(), edit_->persisted(), selected_slot_, selected_character_,
                                                  Catalog(), inventory_query_);
    const std::optional<ItemRef> resolved = ResolveSelection(list, inventory_selection_.item);
    if (!(before == inventory_query_) || (inventory_selection_.item && resolved != inventory_selection_.item)) {
        inventory_selection_.Select(resolved);
    }
    ui.Spacer(4);
    const float outer_gutter = std::floor(trg::layout::kContentPadding * scale);
    const InventoryLayout layout = LayoutInventory(ImGui::GetContentRegionAvail().x - outer_gutter, gap, scale);
    const float height = std::max(kInventoryMinHeight * scale, ImGui::GetContentRegionAvail().y);
    DrawInventoryList(ui, list, layout.list_width, height);
    if (!layout.stacked) ImGui::SameLine(0.0f, gap);
    DrawInventoryDetail(ui, layout.detail_width, height);
}

void EditorPage::DrawInventoryList(trg::Ui& ui, const InventoryList& list, float width, float height) {
    ImGui::BeginChild("##inventory_list", ImVec2(width, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    KeepClearOfScrollbar(std::floor(trg::layout::kContentPadding * ui.scale()));
    if (list.rows.empty()) {
        ui.Help(Tr(inventory_query_.search.empty() ? "This category has no items." : "No items match the search."));
    }
    const float scale = ui.scale();
    const float icon = 28.0f * scale;
    const float pad = 4.0f * scale;
    for (const InventoryRow& row : list.rows) {
        PushItemId(row.ref);
        const bool is_selected = inventory_selection_.item && *inventory_selection_.item == row.ref;
        if (ImGui::Selectable("##row", is_selected, 0, ImVec2(0.0f, icon + pad * 2.0f))) inventory_selection_.Select(row.ref);
        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        DrawItemIcon(draw, ImVec2(min.x + pad, min.y + pad), icon, row.definition, row.category, ImGui::GetColorU32(ui.theme().dim));
        const std::string name = TrValue(row.name);
        const float text_y = min.y + (max.y - min.y - ImGui::GetTextLineHeight()) * 0.5f;
        draw->AddText(ImVec2(min.x + pad * 2.0f + icon, text_y), ImGui::GetColorU32(row.changed ? ui.theme().accent : ui.theme().text),
                      name.c_str());
        const std::string origin = row.malformed ? std::string(Tr("Not understood")) : WhereLabel(row.ref, row.twin);
        const float origin_width = ImGui::CalcTextSize(origin.c_str()).x;
        const float name_end = min.x + pad * 2.0f + icon + ImGui::CalcTextSize(name.c_str()).x;
        if (name_end + origin_width + pad * 3.0f < max.x) {
            draw->AddText(ImVec2(max.x - origin_width - pad, text_y), ImGui::GetColorU32(ui.theme().dim), origin.c_str());
        }
        PopItemId();
    }
    ImGui::EndChild();
}

void EditorPage::DrawInventoryDetail(trg::Ui& ui, float width, float height) {
    ImGui::BeginChild("##inventory_detail", ImVec2(width, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    KeepClearOfScrollbar(std::floor(trg::layout::kContentPadding * ui.scale()));
    if (inventory_selection_.TakeScrollReset()) ImGui::SetScrollY(0.0f);
    visible_.clear();
    visible_frame_ = ImGui::GetFrameCount();
    if (typed_item_ != inventory_selection_.item) {
        if (edit_) typing_.Cancel(edit_->working());
        typed_.Clear();
        typed_item_ = inventory_selection_.item;
    }
    if (inventory_selection_.item && edit_) ClampToGameLimits(edit_->working(), *inventory_selection_.item);
    const ItemDetail detail = inventory_selection_.item && edit_
                                  ? DescribeItem(edit_->working(), edit_->persisted(), *inventory_selection_.item, Catalog())
                                  : ItemDetail{};
    if (!detail.present) {
        ui.Help(Tr("Select an item to see its details."));
        ImGui::EndChild();
        return;
    }
    const float icon = 48.0f * ui.scale();
    DrawItemIcon(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), icon, detail.record.definition, detail.category,
                 ImGui::GetColorU32(ui.theme().dim));
    ImGui::Dummy(ImVec2(icon, icon));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::PushFont(ui.fonts().semibold, ImGui::GetFontSize());
    ImGui::TextWrapped("%s", TrValue(detail.name).c_str());
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().dim);
    ImGui::TextUnformatted((std::string(Tr(CategoryLabel(detail.category))) + " \xC2\xB7 " + WhereLabel(detail.record.ref, detail.twin)).c_str());
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    if (detail.name != detail.saved_name) ui.Help((std::string(Tr("Name will change to")) + " " + TrValue(detail.name)).c_str());
    if (detail.record.ref.storage()) ui.Help(Tr("Shared by every character in this DATA slot."));
    ui.Spacer(4);
    PushItemId(detail.record.ref);
    DrawItemFields(ui, detail);
    DrawItemModifiers(ui, detail);
    PopItemId();
    ImGui::EndChild();
}

void EditorPage::DrawRank(trg::Ui& ui, char letter) {
    const auto found = icons_.ranks.find(letter);
    const float size = ImGui::GetFrameHeight();
    if (letter == 0 || found == icons_.ranks.end()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().dim);
        ImGui::TextUnformatted(letter ? std::string(1, letter).c_str() : "-");
        ImGui::PopStyleColor();
        return;
    }
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    DrawGameImage(ImGui::GetWindowDrawList(), &found->second, rank_textures_[letter], at, size, IM_COL32_WHITE);
}

void EditorPage::DrawItemFields(trg::Ui& ui, const ItemDetail& detail) {
    const ItemRecord& record = detail.record;
    if (record.state != ItemState::kValid) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().warn_dot);
        ImGui::TextWrapped("%s", Tr("This item is not understood by the editor and is kept unchanged."));
        ImGui::PopStyleColor();
        return;
    }
    const ItemRecord saved = ReadItem(edit_->persisted().bytes(), record.ref);

    if (detail.rank || (IsGear(record.definition) && record.grade != 0xFF)) {
        ui.Row(Tr("Rank"));
        DrawRank(ui, detail.rank);
    }
    if (detail.has_attack) {
        ui.Row(Tr("Attack min"));
        ItemField(ui, record.ref, "attack_min", {0, 255}, 0.25f, saved.attack_min != record.attack_min);
        ui.Row(Tr("Attack max"));
        ItemField(ui, record.ref, "attack_max", {0, 255}, 0.25f, saved.attack_max != record.attack_max);
    }
    if (detail.durability_editable) {
        ui.Row(Tr("Durability"));
        ItemField(ui, record.ref, "durability", detail.durability_range, 1.0f, saved.durability != record.durability);
        ui.Row(Tr("Max durability"));
        ItemField(ui, record.ref, "max_durability", detail.durability_max_range, 1.0f, saved.durability_max != record.durability_max);
        if (detail.durability_max_range.max < kNormalMaxDurability) {
            ui.Help((std::string(Tr("Game limit:")) + " " + FormatThousands(detail.durability_max_range.max)).c_str());
        }
    }
    if (detail.level_editable) {
        ui.Row(Tr("Required level"));
        ItemField(ui, record.ref, "required_level", detail.level_range, 0.1f, saved.level_stored != record.level_stored);
    }
    ui.EndRows();
}

void EditorPage::DrawItemModifiers(trg::Ui& ui, const ItemDetail& detail) {
    if (detail.category == ItemCategory::kItem) {
        ui.Help(Tr("Items are shown as the game names them; the editor does not change them."));
        return;
    }
    if (!detail.modifiers.empty() && std::none_of(detail.modifiers.begin(), detail.modifiers.end(), [](const ModifierLine& line) { return line.listed; })) return;
    ui.Spacer(6);
    ui.Heading(Tr("Modifiers"));
    if (detail.modifiers.empty()) {
        ui.Help(Tr("This item has no modifiers."));
        return;
    }
    ui.Help(Tr("Changing modifier values can also change the item's generated name."));
    const ItemRecord saved = ReadItem(edit_->persisted().bytes(), detail.record.ref);
    const bool editable = detail.record.state == ItemState::kValid && IsGear(detail.record.definition);
    for (const ModifierLine& line : detail.modifiers) {
        if (!line.listed) continue;
        ui.Row((TrValue(line.name) + (line.percent ? " (%)" : "")).c_str());
        if (!editable || !line.editable) {
            ImGui::TextUnformatted(std::to_string(line.shown).c_str());
            continue;
        }
        const size_t slot = size_t(line.slot);
        const bool changed = slot < saved.effects.size() && saved.effects[slot].value != detail.record.effects[slot].value;
        ItemField(ui, detail.record.ref, "modifier_" + std::to_string(line.slot), line.range, 0.25f, changed);
    }
    ui.EndRows();
}

} // namespace dcp
