// What the Inventory Editor shows, as plain data: the rows of the list and the details of one item. No drawing and no
// writing; edits go through inventory_data.h.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "editor_model.h"
#include "inventory_data.h"
#include "item_catalog.h"
#include "save_data.h"

namespace dcp {

// Where the list looks: every set and the storage, one equipment set (0-8) or the shared storage.
inline constexpr int kLocationAll = -1;
inline constexpr int kLocationStorage = 9;

// The game's item groups: weapons and shields, accessories, Items (potions, scrolls, rings), and what none of them holds.
enum class InventoryGroup { kWeaponsShields, kAccessories, kItems, kOther };
InventoryGroup GroupOf(uint32_t definition);

enum class InventorySort { kName, kCategory, kLevel, kRank };

struct InventoryQuery {
    InventoryGroup group = InventoryGroup::kWeaponsShields;
    std::string search; // matched without case or accents against the displayed name, and the English one when it differs
    InventorySort sort = InventorySort::kName;
    bool operator==(const InventoryQuery&) const = default;
};

// Where an item sits in the sort modes; the same numbers the sorting uses.
int FamilyOrder(uint32_t definition); // sword, axe, bow, hammer, staff, rod, shield, accessory, other
int RankOrder(const ItemRecord& record); // S A B C E, then an unproven grade, then no rank

struct InventoryRow {
    ItemRef ref;
    std::string name;
    ItemCategory category = ItemCategory::kOther;
    uint32_t definition = 0;
    bool malformed = false;
    bool twin = false; // another item of the same place has the same name
    bool editable = false;
    bool changed = false; // differs from the persisted save
    int level = 0;        // required level, 0 without one
    int family = 0;
    int rank = 0;
};

struct InventoryList {
    std::vector<InventoryRow> rows; // the character's equipment sets, then the shared storage
};

// The name shown for an item: the item list's, or the first modifier's for the modifier-named item, or a stand-in.
std::string ItemDisplayName(const ItemRecord& record, const ItemCatalog& catalog);

// The selected item, and whether the details pane must return to the top because the selection changed.
struct InventorySelection {
    std::optional<ItemRef> item;
    bool scroll_to_top = false;
    void Select(const std::optional<ItemRef>& next);
    bool TakeScrollReset();
    void Clear();
};

// The character's items (equipment sets 1..9 in order) followed by the DATA slot's shared storage.
InventoryList BuildInventoryList(const SaveModel& working, const SaveModel& persisted, int slot, int character,
                                 const ItemCatalog& catalog, int location = kLocationAll);
// The items of one group matching the search, in the order of the sort mode; ties fall back to the name, then to where the
// item sits in the save. Only the order shown changes. An item with pending edits also matches by its saved name.
InventoryList BuildInventoryList(const SaveModel& working, const SaveModel& persisted, int slot, int character,
                                 const ItemCatalog& catalog, const InventoryQuery& query);

// Whether a name contains the search, ignoring case and accents; an empty search matches everything.
bool NameMatchesSearch(const std::string& name, const std::string& search);
// The search to keep once `saved` holds the item: unchanged while the item's saved name still matches, else empty.
std::string SearchKeptAfterSave(const SaveModel& saved, const ItemRef& item, const ItemCatalog& catalog, const std::string& search);

// The selection a list keeps: the current item when it is still listed, else the first item the editor understands.
std::optional<ItemRef> ResolveSelection(const InventoryList& list, const std::optional<ItemRef>& current);

struct ModifierLine {
    std::string name;
    int slot = 0;
    int shown = 0;      // the number the game shows
    NumberRange range;  // what an edit may show
    int stored = 0;     // the stored value, rounded
    bool percent = false;
    bool has_value = true; // the game's text shows a number
    bool listed = true;    // the number can change with the stored value
    bool editable = false;
    bool known = false;
};


struct ItemDetail {
    bool present = false;
    ItemRecord record;
    std::string name;
    std::string saved_name; // the name in the save; differs from `name` while a modifier edit changes it
    bool twin = false;
    ItemCategory category = ItemCategory::kOther;
    bool changed = false;
    bool durability_editable = false;
    bool level_editable = false;
    int durability = 0; // as the game shows it
    int durability_max = 0;
    NumberRange durability_range;     // 0 up to the maximum
    NumberRange durability_max_range; // 1 up to what the u32 storage holds
    NumberRange level_range{kMinItemLevel, kMaxItemLevel};
    int level = 0;
    char rank = 0; // read-only; 0 when the item has none or it is not proven
    bool has_attack = false;
    int attack_min = 0;
    int attack_max = 0;
    std::vector<ModifierLine> modifiers;
};

ItemDetail DescribeItem(const SaveModel& working, const SaveModel& persisted, const ItemRef& ref, const ItemCatalog& catalog);

} // namespace dcp
