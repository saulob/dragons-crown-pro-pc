#include "inventory_data.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "modifier_values.h"

namespace dcp {
namespace {

uint32_t ReadU32(std::span<const uint8_t> bytes, size_t offset) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) value |= uint32_t(bytes[offset + i]) << (8 * i);
    return value;
}

void WriteU32(std::span<uint8_t> bytes, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) bytes[offset + i] = uint8_t(value >> (8 * i));
}

float ReadFloat(std::span<const uint8_t> bytes, size_t offset) {
    const uint32_t bits = ReadU32(bytes, offset);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

ItemRecord Malformed(ItemRecord record, std::string problem) {
    record.state = ItemState::kMalformed;
    record.problem = std::move(problem);
    return record;
}

template <class Visit>
void ForEachItem(const Visit& visit) {
    for (int slot = 0; slot < int(kDataSlotCount); ++slot) {
        for (int index = 0; index < int(kStorageCapacity); ++index) visit(ItemRef{slot, -1, 0, index});
        for (int character = 0; character < int(kCharacterCount); ++character) {
            for (int set = 0; set < int(kEquipmentSetCount); ++set) {
                for (int index = 0; index < int(kEquipmentSetItems); ++index) visit(ItemRef{slot, character, set, index});
            }
        }
    }
}

bool ModifierValuesValid(const ItemRecord& before, const ItemRecord& after) {
    for (size_t i = 0; i < after.effects.size() && i < before.effects.size(); ++i) {
        const float old_value = before.effects[i].value;
        const float new_value = after.effects[i].value;
        if (std::memcmp(&old_value, &new_value, sizeof(float)) != 0 && !IsValidModifierValue(new_value)) return false;
    }
    return true;
}

bool EmptyModifierSlotsUnchanged(std::span<const uint8_t> before, std::span<const uint8_t> after, size_t base, const ItemRecord& item) {
    for (size_t i = item.effects.size(); i < kItemEffectCapacity; ++i) {
        if (std::memcmp(before.data() + base + i * kItemEffectStride, after.data() + base + i * kItemEffectStride, kItemEffectStride) != 0) {
            return false;
        }
    }
    return true;
}

bool InRange(uint32_t value, uint32_t first, uint32_t end) { return value >= first && value < end; }

} // namespace

std::optional<size_t> ItemOffset(const ItemRef& ref) {
    if (ref.slot < 0 || ref.slot >= int(kDataSlotCount) || ref.index < 0) return std::nullopt;
    size_t offset = size_t(ref.slot) * kDataSlotStride;
    if (ref.storage()) {
        if (ref.index >= int(kStorageCapacity)) return std::nullopt;
        offset += kStorageOffset + size_t(ref.index) * kItemRecordSize;
    } else {
        if (ref.character >= int(kCharacterCount) || ref.set < 0 || ref.set >= int(kEquipmentSetCount) ||
            ref.index >= int(kEquipmentSetItems)) {
            return std::nullopt;
        }
        offset += kCharacterOffset + size_t(ref.character) * kCharacterStride + kEquipmentSetOffset +
                  size_t(ref.set) * kEquipmentSetStride + size_t(ref.index) * kItemRecordSize;
    }
    return offset + kItemRecordSize <= kSaveFileSize ? std::optional<size_t>(offset) : std::nullopt;
}

ItemRecord ReadItem(std::span<const uint8_t> bytes, const ItemRef& ref) {
    ItemRecord record;
    record.ref = ref;
    const std::optional<size_t> at = ItemOffset(ref);
    if (!at) return Malformed(std::move(record), "outside the save");
    if (*at + kItemRecordSize > bytes.size()) return Malformed(std::move(record), "truncated record");
    const size_t base = *at;

    const uint32_t bits = ReadU32(bytes, base + kItemFlags);
    record.definition = bits & kDefinitionMask;
    record.flags = uint8_t(bits >> 29);
    record.durability = ReadU32(bytes, base + kItemDurability);
    record.durability_max = ReadU32(bytes, base + kItemDurabilityMax);
    record.level_stored = bytes[base + kItemLevel];
    record.grade = bytes[base + kItemGrade];
    record.name_word = bytes[base + kItemNameWord];
    record.attack_min = bytes[base + kItemAttackMin];
    record.attack_max = bytes[base + kItemAttackMax];
    if (record.definition == 0) return record;

    bool gap = false;
    for (size_t i = 0; i < kItemEffectCapacity; ++i) {
        const size_t at_effect = base + i * kItemEffectStride;
        const uint16_t id = uint16_t(bytes[at_effect] | bytes[at_effect + 1] << 8);
        if (id == kNoEffect) {
            gap = true;
            continue;
        }
        if (gap) return Malformed(std::move(record), "modifier after an empty modifier slot");
        record.effects.push_back({id, uint16_t(bytes[at_effect + 2] | bytes[at_effect + 3] << 8), ReadFloat(bytes, at_effect + 4)});
    }
    if (record.effects.size() != ((bits >> 25) & 0xF)) {
        return Malformed(std::move(record), "modifier count does not match the modifiers");
    }
    if (record.durability > record.durability_max) return Malformed(std::move(record), "durability above its maximum");
    record.state = ItemState::kValid;
    return record;
}

std::vector<ItemRecord> ReadStorage(std::span<const uint8_t> bytes, int slot) {
    std::vector<ItemRecord> items;
    for (int index = 0; index < int(kStorageCapacity); ++index) {
        ItemRecord record = ReadItem(bytes, ItemRef{slot, -1, 0, index});
        if (record.state != ItemState::kEmpty) items.push_back(std::move(record));
    }
    return items;
}

std::vector<ItemRecord> ReadEquipment(std::span<const uint8_t> bytes, int slot, int character) {
    std::vector<ItemRecord> items;
    for (int set = 0; set < int(kEquipmentSetCount); ++set) {
        for (int index = 0; index < int(kEquipmentSetItems); ++index) {
            ItemRecord record = ReadItem(bytes, ItemRef{slot, character, set, index});
            if (record.state != ItemState::kEmpty) items.push_back(std::move(record));
        }
    }
    return items;
}

char RankLetter(uint8_t grade) {
    constexpr char kRanks[] = {'S', 'A', 'B', 'C', 'D', 'E'};
    return grade < sizeof(kRanks) ? kRanks[grade] : 0;
}

bool IsWeapon(uint32_t definition) { return definition >= kFirstWeapon && definition < kFirstShield; }

namespace {

// Modifier ids the name composer takes before any other, in record order.
bool NamedModifier(uint16_t id) {
    constexpr std::pair<uint16_t, uint16_t> kRanges[] = {{110, 49}, {295, 52}, {532, 50}, {751, 50}, {644, 51}, {876, 51}};
    for (const auto& [first, count] : kRanges) {
        if (id >= first && id < first + count) return true;
    }
    return false;
}

// The game's prefix lookup: one block per strength for each range of modifier ids; rune ids (373-531) have none.
std::optional<size_t> PrefixEntry(uint16_t id, int strength) {
    const size_t k = size_t(std::clamp(strength, 0, 4));
    if (id < 185) return id + 185 * k + 185;
    if (id < 373) return id + 188 * k + 1113;
    if (id >= 532 && id <= 750) return id + 219 * k + 2243;
    if (id >= 751 && id <= 978) return id + 228 * k + 3347;
    return std::nullopt;
}

constexpr uint32_t kMaxGearDurability = 450000;
constexpr std::pair<uint16_t, uint32_t> kDurabilityDownCaps[] = {{48, 50000}, {49, 25000}, {50, 10000}};

} // namespace

std::optional<size_t> NamePrefixIndex(const ItemRecord& record) {
    if (record.state != ItemState::kValid || !IsGear(record.definition)) return std::nullopt;
    const ItemEffect* chosen = nullptr;
    int strongest = -1;
    for (const ItemEffect& effect : record.effects) {
        if (NamedModifier(effect.id)) {
            chosen = &effect;
            break;
        }
        if (std::isfinite(effect.value) && effect.value > float(strongest)) {
            strongest = int(effect.value);
            chosen = &effect;
        }
    }
    if (!chosen || !std::isfinite(chosen->value)) return std::nullopt;
    return PrefixEntry(chosen->id, int(chosen->value / 20.0f));
}

uint32_t MaxStoredDurability(const ItemRecord& record) {
    uint32_t cap = kMaxGearDurability;
    for (const ItemEffect& effect : record.effects) {
        for (const auto& [id, limit] : kDurabilityDownCaps) {
            if (effect.id == id) cap = std::min(cap, limit);
        }
    }
    return cap;
}

bool HasAttack(const ItemRecord& record) {
    return IsWeapon(record.definition) && record.attack_min <= record.attack_max;
}

bool IsGear(uint32_t definition) { return InRange(definition, kFirstWeapon, kEndGear); }
std::vector<CharacterClass> UsableBy(uint32_t definition) {
    constexpr uint32_t kFamilySize = 27;
    if (InRange(definition, kFirstWeapon, kFirstShield)) return {CharacterClass((definition - kFirstWeapon) / kFamilySize)};
    if (InRange(definition, kFirstShield, kFirstAccessory)) return {CharacterClass::kFighter};
    if (InRange(definition, kFirstAccessory, kEndGear)) {
        return {CharacterClass::kFighter, CharacterClass::kAmazon, CharacterClass::kDwarf, CharacterClass::kElf,
                CharacterClass::kSorceress, CharacterClass::kWizard};
    }
    return {};
}

bool HasRequiredLevel(uint32_t definition) { return IsGear(definition); }
bool HasEditableDurability(uint32_t definition) { return IsGear(definition); }
uint32_t DurabilityScale(uint32_t definition) { return IsGear(definition) ? kEquipmentDurabilityScale : 1; }

uint32_t ShownDurability(uint32_t stored, uint32_t scale) { return scale == 0 ? stored : stored / scale; }

std::optional<uint32_t> StoredDurability(uint32_t shown, uint32_t current_stored, uint32_t max_stored, uint32_t scale) {
    if (scale == 0 || current_stored > max_stored) return std::nullopt;
    if (shown == ShownDurability(current_stored, scale)) return current_stored;
    if (shown == ShownDurability(max_stored, scale)) return max_stored;
    const uint64_t stored = uint64_t(shown) * scale;
    if (stored > max_stored) return std::nullopt;
    return uint32_t(stored);
}

bool SetItemDurability(SaveModel& model, const ItemRef& ref, uint32_t shown) {
    const ItemRecord record = ReadItem(model.bytes(), ref);
    if (record.state != ItemState::kValid || !HasEditableDurability(record.definition)) return false;
    const std::optional<uint32_t> stored =
        StoredDurability(shown, record.durability, record.durability_max, DurabilityScale(record.definition));
    if (!stored) return false;
    WriteU32(model.bytes(), *ItemOffset(ref) + kItemDurability, *stored);
    ClampToGameLimits(model, ref);
    return true;
}

bool ClampToGameLimits(SaveModel& model, const ItemRef& ref) {
    const ItemRecord record = ReadItem(model.bytes(), ref);
    if (record.state != ItemState::kValid || !IsGear(record.definition)) return false;
    const size_t at = *ItemOffset(ref);
    const std::vector<uint8_t> before(model.bytes().begin() + long(at), model.bytes().begin() + long(at + kItemRecordSize));
    const uint32_t maximum = std::min(record.durability_max, MaxStoredDurability(record));
    WriteU32(model.bytes(), at + kItemDurabilityMax, maximum);
    WriteU32(model.bytes(), at + kItemDurability, std::min(record.durability, maximum));
    return !std::equal(before.begin(), before.end(), model.bytes().begin() + long(at));
}

int MaxShownDurability(const ItemRecord& record) {
    return int(ShownDurability(MaxStoredDurability(record), DurabilityScale(record.definition)));
}

bool SetItemMaxDurability(SaveModel& model, const ItemRef& ref, uint32_t shown) {
    const ItemRecord record = ReadItem(model.bytes(), ref);
    if (record.state != ItemState::kValid || !HasEditableDurability(record.definition) || shown < 1) return false;
    const uint32_t scale = DurabilityScale(record.definition);
    if (shown > uint32_t(MaxShownDurability(record))) return false;
    const size_t at = *ItemOffset(ref);
    const uint32_t stored = shown == ShownDurability(record.durability_max, scale) ? record.durability_max : shown * scale;
    WriteU32(model.bytes(), at + kItemDurabilityMax, stored);
    if (record.durability > stored) WriteU32(model.bytes(), at + kItemDurability, stored);
    ClampToGameLimits(model, ref);
    return true;
}

bool IsValidModifierValue(float value) {
    return std::isfinite(value) && value >= kModifierValueMin && value <= kModifierValueMax;
}

bool SetModifierValue(SaveModel& model, const ItemRef& ref, int slot, float value) {
    const ItemRecord record = ReadItem(model.bytes(), ref);
    if (record.state != ItemState::kValid || !IsGear(record.definition) || slot < 0 || size_t(slot) >= record.effects.size() ||
        !IsValidModifierValue(value)) {
        return false;
    }
    if (record.effects[size_t(slot)].value == value) return true;
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    WriteU32(model.bytes(), *ItemOffset(ref) + size_t(slot) * kItemEffectStride + 4, bits);
    ClampToGameLimits(model, ref);
    return true;
}

bool SetModifierShown(SaveModel& model, const ItemRef& ref, int slot, int shown) {
    const ItemRecord record = ReadItem(model.bytes(), ref);
    if (record.state != ItemState::kValid || !IsGear(record.definition) || slot < 0 || size_t(slot) >= record.effects.size()) return false;
    const ItemEffect& effect = record.effects[size_t(slot)];
    const std::optional<ModifierSpec> spec = ModifierSpecOf(effect.id);
    if (!spec || !ModifierShownOf(*spec, effect.value).editable) return false;
    return SetModifierValue(model, ref, slot, float(ModifierStoredForShown(*spec, shown, int(effect.value))));
}

bool SetItemAttack(SaveModel& model, const ItemRef& ref, int minimum, int maximum) {
    const ItemRecord record = ReadItem(model.bytes(), ref);
    if (record.state != ItemState::kValid || !HasAttack(record) || minimum < 0 || maximum > 255 || minimum > maximum) return false;
    const size_t at = *ItemOffset(ref);
    model.bytes()[at + kItemAttackMin] = uint8_t(minimum);
    model.bytes()[at + kItemAttackMax] = uint8_t(maximum);
    return true;
}

bool EditItemAttackMin(SaveModel& model, const ItemRef& ref, int minimum) {
    const ItemRecord record = ReadItem(model.bytes(), ref);
    return SetItemAttack(model, ref, minimum, std::max(minimum, int(record.attack_max)));
}

bool EditItemAttackMax(SaveModel& model, const ItemRef& ref, int maximum) {
    const ItemRecord record = ReadItem(model.bytes(), ref);
    return SetItemAttack(model, ref, std::min(maximum, int(record.attack_min)), maximum);
}

bool SetItemRequiredLevel(SaveModel& model, const ItemRef& ref, int displayed) {
    if (displayed < kMinItemLevel || displayed > kMaxItemLevel) return false;
    const ItemRecord record = ReadItem(model.bytes(), ref);
    if (record.state != ItemState::kValid || !HasRequiredLevel(record.definition)) return false;
    const size_t at = *ItemOffset(ref);
    model.bytes()[at + kItemLevel] = uint8_t(displayed - 1);
    ClampToGameLimits(model, ref);
    return true;
}

namespace {

const char* ItemProblem(std::span<const uint8_t> before, std::span<const uint8_t> after, size_t base, const ItemRecord& old_item,
                        const ItemRecord& new_item) {
    if (old_item.state != ItemState::kValid || new_item.state != ItemState::kValid || !IsGear(new_item.definition)) return kProblemItem;
    if (!ModifierValuesValid(old_item, new_item) || !EmptyModifierSlotsUnchanged(before, after, base, new_item)) return kProblemModifier;
    if (new_item.required_level() > kMaxItemLevel) return kProblemLevel;
    const bool attack_changed = old_item.attack_min != new_item.attack_min || old_item.attack_max != new_item.attack_max;
    if (attack_changed && IsWeapon(new_item.definition) && !HasAttack(new_item)) return kProblemItem;
    if (attack_changed && !IsWeapon(new_item.definition)) return kProblemItem;
    if (new_item.durability_max == 0) return kProblemMaxDurability;
    if (new_item.durability_max > MaxStoredDurability(new_item)) return kProblemMaxDurability;
    return nullptr;
}

} // namespace

namespace {

std::optional<int> ModifierSlot(std::string_view key) {
    constexpr std::string_view kPrefix = "modifier_";
    if (!key.starts_with(kPrefix) || key.size() != kPrefix.size() + 1 || key.back() < '0' || key.back() > '7') return std::nullopt;
    return key.back() - '0';
}

} // namespace

std::optional<int> ItemFieldValue(const SaveModel& model, const ItemRef& ref, std::string_view key) {
    const ItemRecord record = ReadItem(model.bytes(), ref);
    if (record.state != ItemState::kValid) return std::nullopt;
    const uint32_t scale = DurabilityScale(record.definition);
    if (key == "attack_min") return HasAttack(record) ? std::optional<int>(record.attack_min) : std::nullopt;
    if (key == "attack_max") return HasAttack(record) ? std::optional<int>(record.attack_max) : std::nullopt;
    if (key == "durability") return int(ShownDurability(record.durability, scale));
    if (key == "max_durability") return int(ShownDurability(record.durability_max, scale));
    if (key == "required_level") return record.required_level();
    if (const std::optional<int> slot = ModifierSlot(key)) {
        if (size_t(*slot) >= record.effects.size()) return std::nullopt;
        const ItemEffect& effect = record.effects[size_t(*slot)];
        const std::optional<ModifierSpec> spec = ModifierSpecOf(effect.id);
        if (!spec || !std::isfinite(effect.value)) return std::nullopt;
        return ModifierShownValue(*spec, effect.value);
    }
    return std::nullopt;
}

bool SetItemField(SaveModel& model, const ItemRef& ref, std::string_view key, int value) {
    if (key == "attack_min") return EditItemAttackMin(model, ref, value);
    if (key == "attack_max") return EditItemAttackMax(model, ref, value);
    if (value < 0) return false;
    if (key == "durability") return SetItemDurability(model, ref, uint32_t(value));
    if (key == "max_durability") return SetItemMaxDurability(model, ref, uint32_t(value));
    if (key == "required_level") return SetItemRequiredLevel(model, ref, value);
    if (const std::optional<int> slot = ModifierSlot(key)) return SetModifierShown(model, ref, *slot, value);
    return false;
}

std::vector<uint8_t> ItemBytes(const SaveModel& model, const ItemRef& ref) {
    const std::optional<size_t> at = ItemOffset(ref);
    if (!at) return {};
    return std::vector<uint8_t>(model.bytes().begin() + long(*at), model.bytes().begin() + long(*at + kItemRecordSize));
}

void RestoreItemBytes(SaveModel& model, const ItemRef& ref, std::span<const uint8_t> bytes) {
    const std::optional<size_t> at = ItemOffset(ref);
    if (at && bytes.size() == kItemRecordSize) std::copy(bytes.begin(), bytes.end(), model.bytes().begin() + long(*at));
}

std::optional<UiMismatch> FindUiMismatch(const SaveModel& edited, const std::vector<VisibleField>& visible) {
    for (const VisibleField& field : visible) {
        const std::optional<int> now = ItemFieldValue(edited, field.ref, field.key);
        if (!now || *now != field.shown) return UiMismatch{field, now.value_or(-1)};
    }
    return std::nullopt;
}

bool ValidateItemChanges(std::span<const uint8_t> before, std::span<const uint8_t> after, std::string& error) {
    if (before.size() != kSaveFileSize || after.size() != kSaveFileSize) {
        error = "unexpected save size";
        return false;
    }
    bool ok = true;
    ForEachItem([&](const ItemRef& ref) {
        if (!ok) return;
        const size_t base = *ItemOffset(ref);
        if (std::memcmp(before.data() + base, after.data() + base, kItemRecordSize) == 0) return;
        const auto allowed = [](size_t field) {
            return (field >= kItemDurability && field < kItemDurabilityMax + 4) || field == kItemLevel ||
                   field == kItemAttackMin || field == kItemAttackMax ||
                   (field < kItemEffectCapacity * kItemEffectStride && field % kItemEffectStride >= 4);
        };
        for (size_t field = 0; field < kItemRecordSize; ++field) {
            if (before[base + field] != after[base + field] && !allowed(field)) {
                error = kProblemItem;
                ok = false;
                return;
            }
        }
        const ItemRecord old_item = ReadItem(before, ref);
        const ItemRecord new_item = ReadItem(after, ref);
        const char* problem = ItemProblem(before, after, base, old_item, new_item);
        if (problem) {
            error = problem;
            ok = false;
        }
    });
    return ok;
}

} // namespace dcp
