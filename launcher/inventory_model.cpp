#include "inventory_model.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <utility>

#include "modifier_values.h"

namespace dcp {
namespace {

bool SameBytes(const SaveModel& a, const SaveModel& b, const ItemRef& ref) {
    const std::optional<size_t> at = ItemOffset(ref);
    if (!at || *at + kItemRecordSize > a.bytes().size() || *at + kItemRecordSize > b.bytes().size()) return true;
    return std::memcmp(a.bytes().data() + *at, b.bytes().data() + *at, kItemRecordSize) == 0;
}

bool Editable(const ItemRecord& record) {
    return record.state == ItemState::kValid && HasEditableDurability(record.definition);
}

int CategoryOrder(const InventoryRow& row) {
    if (row.malformed) return 4;
    switch (row.category) {
    case ItemCategory::kWeapon: return 0;
    case ItemCategory::kShield: return 1;
    case ItemCategory::kAccessory: return 2;
    case ItemCategory::kItem:
    case ItemCategory::kOther: break;
    }
    return 3;
}

int LocationOrder(const ItemRef& ref) { return ref.storage() ? 99 : ref.set; }

// The letter each Latin-1 letter U+00C0..U+00FF sorts and searches as; 0 for a sign.
constexpr char kLatin1Base[] =
    "AAAAAAACEEEEIIIIDNOOOOO\0OUUUUY\0saaaaaaaceeeeiiiidnooooo\0ouuuuy\0y";
static_assert(sizeof(kLatin1Base) == 65, "one base letter for each of the 64 letters");

// The name in the form a search and the name sort compare: lower case, accents removed.
std::string SortName(const std::string& name) {
    std::string key;
    key.reserve(name.size());
    for (size_t i = 0; i < name.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(name[i]);
        if (ch == 0xC3 && i + 1 < name.size() && (static_cast<unsigned char>(name[i + 1]) & 0xC0) == 0x80) {
            const char base = kLatin1Base[static_cast<unsigned char>(name[i + 1]) - 0x80];
            if (base) {
                key += (base >= 'A' && base <= 'Z') ? char(base - 'A' + 'a') : base;
                ++i;
                continue;
            }
        }
        key += (ch >= 'A' && ch <= 'Z') ? char(ch - 'A' + 'a') : char(ch);
    }
    return key;
}

} // namespace

void InventorySelection::Select(const std::optional<ItemRef>& next) {
    if (next == item) return;
    item = next;
    scroll_to_top = true;
}

bool InventorySelection::TakeScrollReset() {
    const bool reset = scroll_to_top;
    scroll_to_top = false;
    return reset;
}

void InventorySelection::Clear() {
    item.reset();
    scroll_to_top = false;
}

std::optional<ItemRef> ResolveSelection(const InventoryList& list, const std::optional<ItemRef>& current) {
    for (const InventoryRow& row : list.rows) {
        if (current && row.ref == *current) return current;
    }
    for (const InventoryRow& row : list.rows) {
        if (!row.malformed) return row.ref;
    }
    return std::nullopt;
}

std::string ItemDisplayName(const ItemRecord& record, const ItemCatalog& catalog) {
    if (record.definition == kModifierNamedDefinition) {
        const std::string name = record.effects.empty() ? std::string() : catalog.ItemsEntryName(record.effects.front().id);
        if (name.empty()) return UnknownItemName(record.definition);
        if (name.starts_with("##(") && name.ends_with(")")) return "Special item (" + name.substr(3, name.size() - 4) + ")";
        return name;
    }
    const std::string name = catalog.ItemName(record.definition);
    if (name.empty()) return UnknownItemName(record.definition);
    if (!IsGear(record.definition)) return name;
    if (record.name_word != 0xFF) {
        const std::string word = catalog.Word(record.name_word);
        return word.empty() ? name : word + " " + name;
    }
    if (const std::optional<size_t> index = NamePrefixIndex(record)) {
        const std::string prefix = catalog.Prefix(*index);
        if (!prefix.empty()) return prefix + " " + name;
    }
    return name;
}

InventoryGroup GroupOf(uint32_t definition) {
    if (definition >= kFirstWeapon && definition < kFirstAccessory) return InventoryGroup::kWeaponsShields;
    if (definition >= kFirstAccessory && definition < kEndGear) return InventoryGroup::kAccessories;
    if (definition == kModifierNamedDefinition) return InventoryGroup::kItems;
    return InventoryGroup::kOther;
}

int FamilyOrder(uint32_t definition) {
    constexpr uint32_t kFamilySize = 27;
    if (definition >= kFirstWeapon && definition < kFirstShield) return int((definition - kFirstWeapon) / kFamilySize);
    if (definition >= kFirstShield && definition < kFirstAccessory) return 6;
    if (definition >= kFirstAccessory && definition < kEndGear) return 7;
    return 8;
}

int RankOrder(const ItemRecord& record) {
    switch (RankLetter(record.grade)) {
    case 'S': return 0;
    case 'A': return 1;
    case 'B': return 2;
    case 'C': return 3;
    case 'D': return 4;
    case 'E': return 5;
    default: break;
    }
    return IsGear(record.definition) && record.grade != 0xFF ? 6 : 7;
}

namespace {

std::vector<InventoryRow> ReadRows(const SaveModel& working, const SaveModel& persisted, int slot, int character,
                                   const ItemCatalog& catalog) {
    std::vector<ItemRecord> items = ReadEquipment(working.bytes(), slot, character);
    std::vector<ItemRecord> storage = ReadStorage(working.bytes(), slot);
    items.insert(items.end(), std::make_move_iterator(storage.begin()), std::make_move_iterator(storage.end()));
    std::vector<InventoryRow> rows;
    std::map<std::pair<std::string, int>, int> same_name;
    const auto place = [](const ItemRef& ref) { return ref.storage() ? -1 : ref.set; };
    for (const ItemRecord& record : items) ++same_name[{ItemDisplayName(record, catalog), place(record.ref)}];
    for (const ItemRecord& record : items) {
        InventoryRow row;
        row.ref = record.ref;
        row.name = ItemDisplayName(record, catalog);
        row.category = CategoryOf(record.definition);
        row.definition = record.definition;
        row.malformed = record.state == ItemState::kMalformed;
        row.twin = record.state != ItemState::kEmpty && same_name[{row.name, place(record.ref)}] > 1;
        row.editable = Editable(record);
        row.changed = !SameBytes(working, persisted, record.ref);
        row.level = record.state == ItemState::kValid && HasRequiredLevel(record.definition) ? record.required_level() : 0;
        row.family = FamilyOrder(record.definition);
        row.rank = record.state == ItemState::kValid ? RankOrder(record) : 8;
        rows.push_back(std::move(row));
    }
    return rows;
}

template <typename Less>
void SortRows(std::vector<InventoryRow>& rows, Less less) {
    std::vector<std::string> names;
    names.reserve(rows.size());
    for (const InventoryRow& row : rows) names.push_back(SortName(row.name));
    std::vector<size_t> order(rows.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return less(rows[a], names[a], rows[b], names[b]); });
    std::vector<InventoryRow> sorted;
    sorted.reserve(rows.size());
    for (const size_t i : order) sorted.push_back(std::move(rows[i]));
    rows = std::move(sorted);
}

} // namespace

InventoryList BuildInventoryList(const SaveModel& working, const SaveModel& persisted, int slot, int character,
                                 const ItemCatalog& catalog, int location) {
    InventoryList list;
    for (InventoryRow& row : ReadRows(working, persisted, slot, character, catalog)) {
        const bool listed = location == kLocationAll ||
                            (location == kLocationStorage ? row.ref.storage() : (!row.ref.storage() && row.ref.set == location));
        if (listed) list.rows.push_back(std::move(row));
    }
    SortRows(list.rows, [](const InventoryRow& a, const std::string& an, const InventoryRow& b, const std::string& bn) {
        if (LocationOrder(a.ref) != LocationOrder(b.ref)) return LocationOrder(a.ref) < LocationOrder(b.ref);
        if (CategoryOrder(a) != CategoryOrder(b)) return CategoryOrder(a) < CategoryOrder(b);
        return an < bn;
    });
    return list;
}

bool NameMatchesSearch(const std::string& name, const std::string& search) {
    return search.empty() || SortName(name).find(SortName(search)) != std::string::npos;
}

std::string SearchKeptAfterSave(const SaveModel& saved, const ItemRef& item, const ItemCatalog& catalog, const std::string& search) {
    const ItemRecord record = ReadItem(saved.bytes(), item);
    const ItemCatalog* english = catalog.English();
    const bool matches = record.state == ItemState::kEmpty || NameMatchesSearch(ItemDisplayName(record, catalog), search) ||
                         (english && NameMatchesSearch(ItemDisplayName(record, *english), search));
    return matches ? search : std::string();
}

InventoryList BuildInventoryList(const SaveModel& working, const SaveModel& persisted, int slot, int character,
                                 const ItemCatalog& catalog, const InventoryQuery& query) {
    const std::string needle = SortName(query.search);
    const ItemCatalog* english = catalog.English();
    InventoryList list;
    for (InventoryRow& row : ReadRows(working, persisted, slot, character, catalog)) {
        const InventoryGroup group = row.malformed ? InventoryGroup::kOther : GroupOf(row.definition);
        // An item with pending edits also matches by the name it has in the save, so editing never makes it vanish.
        const auto matches = [&](const std::string& name) { return SortName(name).find(needle) != std::string::npos; };
        const auto matches_record = [&](const ItemRecord& record) {
            return matches(ItemDisplayName(record, catalog)) || (english && matches(ItemDisplayName(record, *english)));
        };
        const bool found = needle.empty() || matches(row.name) || (english && matches_record(ReadItem(working.bytes(), row.ref))) ||
                           (row.changed && matches_record(ReadItem(persisted.bytes(), row.ref)));
        if (group == query.group && found) {
            list.rows.push_back(std::move(row));
        }
    }
    const auto primary = [&](const InventoryRow& a, const InventoryRow& b) {
        switch (query.sort) {
        case InventorySort::kName: return 0;
        case InventorySort::kCategory: return a.family - b.family;
        case InventorySort::kLevel: return a.level == 0 || b.level == 0 ? (a.level == 0) - (b.level == 0) : a.level - b.level;
        case InventorySort::kRank: return a.rank - b.rank;
        }
        return 0;
    };
    SortRows(list.rows, [&](const InventoryRow& a, const std::string& an, const InventoryRow& b, const std::string& bn) {
        if (const int by_mode = primary(a, b)) return by_mode < 0;
        return an < bn;
    });
    return list;
}

ItemDetail DescribeItem(const SaveModel& working, const SaveModel& persisted, const ItemRef& ref, const ItemCatalog& catalog) {
    ItemDetail detail;
    detail.record = ReadItem(working.bytes(), ref);
    if (detail.record.state == ItemState::kEmpty || !ItemOffset(ref)) return detail;
    const ItemRecord& record = detail.record;
    detail.present = true;
    detail.name = ItemDisplayName(record, catalog);
    detail.saved_name = ItemDisplayName(ReadItem(persisted.bytes(), ref), catalog);
    detail.twin = false;
    for (const ItemRecord& other : ref.storage() ? ReadStorage(working.bytes(), ref.slot) : ReadEquipment(working.bytes(), ref.slot, ref.character)) {
        const bool same_place = ref.storage() || other.ref.set == ref.set;
        if (other.state != ItemState::kEmpty && !(other.ref == ref) && same_place && ItemDisplayName(other, catalog) == detail.name) detail.twin = true;
    }
    detail.category = CategoryOf(record.definition);
    detail.changed = !SameBytes(working, persisted, ref);
    detail.durability_editable = record.state == ItemState::kValid && HasEditableDurability(record.definition);
    detail.level_editable = record.state == ItemState::kValid && HasRequiredLevel(record.definition);
    const uint32_t scale = DurabilityScale(record.definition);
    detail.durability = int(ShownDurability(record.durability, scale));
    detail.durability_max = int(ShownDurability(record.durability_max, scale));
    detail.durability_range = {0, detail.durability_max};
    detail.durability_max_range = {1, MaxShownDurability(record)};
    detail.level = record.required_level();
    if (IsGear(record.definition)) detail.rank = RankLetter(record.grade);
    if (HasAttack(record)) {
        detail.has_attack = true;
        detail.attack_min = record.attack_min;
        detail.attack_max = record.attack_max;
    }
    for (size_t slot = 0; slot < record.effects.size(); ++slot) {
        const ItemEffect& effect = record.effects[slot];
        const EffectText text = catalog.Effect(effect.id);
        const float clamped = std::isfinite(effect.value) ? std::clamp(effect.value, -1000000.0f, 1000000.0f) : 0.0f;
        ModifierLine line{text.known ? text.name : UnknownEffectName(effect.id), int(slot), int(std::lround(clamped)), {}, int(std::lround(clamped)),
                          text.percent, !text.known || text.has_value, true, false, text.known};
        if (const std::optional<ModifierSpec> spec = ModifierSpecOf(effect.id); spec && std::isfinite(effect.value)) {
            const ModifierShown shown = ModifierShownOf(*spec, effect.value);
            line.shown = shown.value;
            line.range = {shown.minimum, shown.maximum};
            line.editable = shown.editable;
            line.listed = line.has_value && shown.minimum < shown.maximum;
        }
        detail.modifiers.push_back(std::move(line));
    }
    return detail;
}

} // namespace dcp
