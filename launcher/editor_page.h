// The launcher's Editor page: the saves of the installed game, the characters of the chosen save and their appearance. Selecting never writes; every change stays pending until Apply Changes is used.
#pragma once

#include <imgui.h>

#include <filesystem>
#include <future>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include "appearance_custom.h"
#include "appearance_loader.h"
#include "character_art.h"
#include "editor_model.h"
#include "hp_override.h"
#include "inventory_data.h"
#include "inventory_model.h"
#include "item_catalog.h"
#include "save_data.h"
#include "save_discovery.h"
#include "save_edit.h"
#include "save_page_model.h"

namespace trg {
class Ui;
}

namespace dcp {

class LauncherHost;

enum class ButtonIcon { kAppearance, kInventory, kBack };
bool IconButton(const char* label, const std::function<ImTextureID(int pixels)>& icon, ImVec2 size);
void DrawItemPlaceholder(ImDrawList* draw, ImVec2 min, float size, ItemCategory category, ImU32 color);

struct EditorGame {
    GameRoots roots;
    const GameBuild* configured = nullptr;
};

class EditorPage {
public:
    // `game` says where the builds are and which one the launcher runs; `message_suffix` names the SystemMsg language
    // the game uses (msgsheet/SystemMsg_<suffix>.fms). `selection` is the save the Save page chose; when that page
    // replaces a save, the Editor reloads before it can write again.
    EditorPage(LauncherHost& host, std::filesystem::path user_dir, std::function<EditorGame()> game,
               std::function<std::string()> message_suffix, const SaveSelection* selection = nullptr);

    void Refresh();

    bool dirty() const {
        return (edit_ && edit_->dirty()) || !(custom_working_ == custom_saved_) ||
               !(override_working_ == override_saved_);
    }
    // A reason Play/Quit must wait (empty when clean), for the launcher gate.
    std::string DirtyReason() const;

    void Draw(trg::Ui& ui);

private:
    enum class PendingSwitch { kNone, kSlot };

    void SelectSlot(int slot, trg::Ui& ui);
    void SelectCharacter(int character);
    void ApplyChanges(trg::Ui& ui);
    void DiscardChanges(trg::Ui& ui);

    const EditorBuild* SelectedBuild() const;
    std::vector<size_t> VisibleSaves() const;
    void UseArtOf(const EditorBuild* build);

    const DiscoveredSave* SelectedSave() const;
    const DataSlotView* SelectedSlot() const;
    const CharacterView* SelectedCharacter() const;
    const CharacterView* PersistedCharacter() const;
    std::string ColorName(CharacterClass klass, int color);
    void SyncNameBuffer();

    // The art as a texture for ImGui; 0 when the game's files could not be read.
    ImTextureID Texture(const SpriteImage* image, int kind, CharacterClass klass, int color);
    struct ArtRect {
        ImVec2 min;
        float scale = 1.0f;
    };
    std::optional<ArtRect> DrawArt(ImDrawList* draw, ImTextureID texture, const SpriteImage* image, ImVec2 min, ImVec2 max,
                                   const char* unavailable = nullptr);
    ImTextureID Replace(ImTextureID& slot, std::string& slot_key, const std::string& key, const uint8_t* rgba, int width,
                        int height);

    std::optional<CustomKey> CurrentKey() const;
    CustomLook CurrentLook(const CharacterView& character) const;
    void SetLook(const CustomLook& look);
    void RestoreLook(const std::optional<CustomLook>& stored);
    IdentityLine IdentityLineOf(trg::Ui& ui, float row_height) const;
    ImVec2 IdentitySize(trg::Ui& ui, const CharacterIdentity& identity) const;
    void DrawIdentity(trg::Ui& ui, ImVec2 at, const CharacterIdentity& identity, float row_height);
    std::optional<PixelBox> PreviewCrop(CharacterClass klass, int color);
    void DrawBaseColor(trg::Ui& ui, const CharacterView& character, CustomLook look);
    void DrawDifficulty(trg::Ui& ui, const CharacterView& character, bool changed);
    void DrawCustom(trg::Ui& ui, const CharacterView& character, CustomLook look);
    void DrawAllColorsRandom(trg::Ui& ui, CharacterClass klass, CustomLook look);
    void DrawResetCustom(trg::Ui& ui, CustomLook look);

    bool TitleWithButton(trg::Ui& ui, const char* title, const char* button, ButtonIcon icon);
    void RequestAppearance();
    void DrawAppearanceLoading(trg::Ui& ui);
    void AdvanceAppearanceLoad();
    void CollectAppearanceLoad();
    bool AppearanceJobCurrent() const;
    void DrawColorPicker(trg::Ui& ui, CustomLook& look, const CustomPart& part);

    void DrawChanges(trg::Ui& ui);
    void DrawSaveData(trg::Ui& ui);
    void DrawCharacter(trg::Ui& ui);
    void DrawCharacterList(trg::Ui& ui);
    void DrawCharacterEditor(trg::Ui& ui);
    bool NumberControl(trg::Ui& ui, const char* id, int* value, NumberRange range, float drag_speed, FieldStyle style,
                       bool positional_marker = false);
    void DrawAppearance(trg::Ui& ui);
    void DrawInventory(trg::Ui& ui);
    void DrawInventorySummary(trg::Ui& ui, const CharacterView& character, const CharacterView* saved);
    void DrawInventoryList(trg::Ui& ui, const InventoryList& list, float width, float height);
    void DrawInventoryDetail(trg::Ui& ui, float width, float height);
    void DrawItemFields(trg::Ui& ui, const ItemDetail& detail);
    void DrawItemModifiers(trg::Ui& ui, const ItemDetail& detail);
    ImTextureID ButtonIconTexture(ButtonIcon icon, int pixels);
    bool DrawGameImage(ImDrawList* draw, const SpriteImage* image, ImTextureID& texture, ImVec2 min, float size, ImU32 tint);
    void DrawRank(trg::Ui& ui, char letter);
    void DrawItemIcon(ImDrawList* draw, ImVec2 min, float size, uint32_t definition, ItemCategory category, ImU32 fallback);
    void RequestInventory();
    void ResetInventoryView();
    const ItemCatalog& Catalog() const;
    bool CatalogReady();
    void DrawSwitchPrompt(trg::Ui& ui);
    void KeepSearchMatchingSavedItem();
    void ItemField(trg::Ui& ui, const ItemRef& ref, const std::string& key, NumberRange range, float drag_speed, bool changed);
    bool PrepareInventorySave(trg::Ui& ui);
    const char* InventoryProblemText(const std::string& problem) const;

    LauncherHost& host_;
    std::filesystem::path user_dir_;
    std::function<EditorGame()> game_;
    std::function<std::string()> message_suffix_;
    const SaveSelection* selection_ = nullptr;
    uint64_t seen_generation_ = 0;

    EditorGame current_game_;
    std::vector<EditorBuild> builds_;
    size_t selected_build_ = 0;

    std::optional<CharacterArt> art_;
    std::filesystem::path art_root_;
    std::map<std::tuple<int, CharacterClass, int>, ImTextureID> textures_;

    std::vector<DiscoveredSave> saves_;
    int selected_save_ = -1;
    int selected_slot_ = 0;
    int selected_character_ = -1;
    CharacterNav nav_;
    TypedNumberEdit typed_;
    TypedFieldEdit typing_;
    bool typed_finished_ = false;
    std::vector<VisibleField> visible_;
    int visible_frame_ = -100;
    bool typing_now_ = false;
    std::optional<ItemRef> typed_item_;
    bool focus_number_ = false;
    ImGuiID drag_id_ = 0;
    float drag_press_x_ = 0.0f;
    bool drag_started_ = false;
    ImGuiID press_id_ = 0;
    int press_value_ = 0;
    char number_buffer_[16] = {};

    std::optional<SaveEdit> edit_;
    CustomStore custom_saved_;
    CustomStore custom_working_;
    std::string custom_error_;
    CharacterOverrideStore override_saved_;
    CharacterOverrideStore override_working_;
    std::string selected_part_;
    bool confirm_reset_custom_ = false;
    std::mt19937 random_{std::random_device{}()};
    ImTextureID custom_texture_{};
    std::string custom_texture_key_;
    ImTextureID overlay_texture_{};
    std::string overlay_key_;
    // The colour picker works on the live look; `before` is what Cancel puts back.
    struct ColorPick {
        bool active = false;
        std::string part;
        std::optional<CustomLook> before;
        float working[4] = {0, 0, 0, 1};
        float original[4] = {0, 0, 0, 1};
        ImVec2 swatch_min, swatch_max;
    };
    ColorPick pick_;
    float preview_right_ = 0.0f;
    std::map<std::pair<CharacterClass, int>, PixelBox> preview_crops_;

    // The character whose Appearance is being prepared; `serial` is its loader job once started.
    struct AppearanceJob {
        std::filesystem::path art_root;
        std::filesystem::path save;
        int slot = 0;
        int character = -1;
        CharacterClass klass = CharacterClass::kFighter;
        uint64_t serial = 0;
    };
    std::optional<AppearanceJob> appearance_job_;
    bool appearance_failed_ = false;

    InventorySelection inventory_selection_;
    InventoryQuery inventory_query_;
    int fine_anchor_value_ = 0;
    float fine_anchor_x_ = 0.0f;
    bool fine_drag_ = false;
    char inventory_search_[96] = {};
    ItemCatalog catalog_;
    std::string catalog_key_;
    std::future<ItemAssets> catalog_job_;
    ItemIconSet icons_;
    std::map<int, ImTextureID> icon_textures_; // by ItemIconSet::PictureKey
    std::map<std::pair<ButtonIcon, int>, ImTextureID> button_icons_;
    std::map<char, ImTextureID> rank_textures_;
    std::string catalog_job_key_;

    PendingSwitch pending_switch_ = PendingSwitch::kNone;
    int pending_target_ = 0;
    int last_frame_ = -2;
    bool loaded_ = false;
    bool refresh_pending_ = false;
    bool loading_shown_ = false;
    float header_pair_x_ = 0.0f;     // screen x and width of the Apply / Discard pair, the Save Data combo's span
    float header_pair_width_ = 0.0f;
    char name_buffer_[kCharNameBytes + 1] = {};
    AppearanceLoader appearance_loader_;
};

} // namespace dcp
