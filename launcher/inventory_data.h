// The item records of SAVE0.DAT: each DATA slot's shared storage and every character's equipment sets. Items are
// instances stored inline; nothing references another record, so editing one item never touches another.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "modifier_values.h"
#include "save_data.h"

namespace dcp {

inline constexpr size_t kItemRecordSize = 0x54;
inline constexpr size_t kStorageOffset = 0xC0; // from the start of a DATA slot
inline constexpr size_t kStorageCapacity = 500;
inline constexpr size_t kEquipmentSetCount = 9;
inline constexpr size_t kEquipmentSetItems = 10;
inline constexpr size_t kEquipmentSetOffset = 0x248; // from the start of a character record
inline constexpr size_t kEquipmentSetStride = 0x354;
inline constexpr size_t kItemEffectCapacity = 8;

inline constexpr size_t kItemEffectStride = 8;
inline constexpr size_t kItemFlags = 0x40;
inline constexpr size_t kItemDurability = 0x44;    // u32 little-endian
inline constexpr size_t kItemDurabilityMax = 0x48; // u32 little-endian
inline constexpr size_t kItemAttackMin = 0x50;
inline constexpr size_t kItemAttackMax = 0x51;
inline constexpr size_t kItemGrade = 0x53;
inline constexpr size_t kItemLevel = 0x4E;         // u8; stored = displayed required level - 1
inline constexpr size_t kItemNameWord = 0x52;      // u8; 0xFF = none, else SystemMsg 1485 + n before the name

inline constexpr uint16_t kNoEffect = 0xFFFF;
inline constexpr uint32_t kDefinitionMask = 0x1FFFFFF;
inline constexpr int kMinItemLevel = 1;
inline constexpr int kMaxItemLevel = 100;
inline constexpr uint32_t kEquipmentDurabilityScale = 100; // the game shows stored durability / 100
inline constexpr uint32_t kMaxStoredDurability = 0xFFFFFFFFu; // both durability fields are u32

// The one definition the game names from its first modifier instead of from the item list.
inline constexpr uint32_t kModifierNamedDefinition = 112;

struct ItemRef {
    int slot = 0;       // DATA slot
    int character = -1; // -1 = the DATA slot's shared storage
    int set = 0;        // equipment set of the character
    int index = 0;      // position in the storage or in the set

    bool storage() const { return character < 0; }
    bool operator==(const ItemRef&) const = default;
};

struct ItemEffect {
    uint16_t id = kNoEffect;
    uint16_t extra = 0;
    float value = 0.0f;
};

enum class ItemState { kEmpty, kValid, kMalformed };

struct ItemRecord {
    ItemRef ref;
    ItemState state = ItemState::kEmpty;
    std::string problem; // why a record is malformed
    uint32_t definition = 0;
    uint8_t flags = 0;
    std::vector<ItemEffect> effects;
    uint32_t durability = 0;
    uint32_t durability_max = 0;
    uint8_t level_stored = 0;
    uint8_t grade = 0xFF;
    uint8_t attack_min = 0;
    uint8_t attack_max = 0;
    uint8_t name_word = 0xFF;

    int required_level() const { return int(level_stored) + 1; }
};

// The byte offset of an item inside a save image; nothing when the reference falls outside the structure.
std::optional<size_t> ItemOffset(const ItemRef& ref);

ItemRecord ReadItem(std::span<const uint8_t> bytes, const ItemRef& ref);
std::vector<ItemRecord> ReadStorage(std::span<const uint8_t> bytes, int slot);
std::vector<ItemRecord> ReadEquipment(std::span<const uint8_t> bytes, int slot, int character);

// The item list's runs of weapons (swords to rods), shields and accessories; the Heaven rewards after them are not gear
// the editor knows.
inline constexpr uint32_t kFirstWeapon = 187;
inline constexpr uint32_t kFirstShield = 349;
inline constexpr uint32_t kFirstAccessory = 375;
inline constexpr uint32_t kEndGear = 489;

// The rank letter a gear items grade byte stands for (0 = S ... 4 = D, 5 = E, the starter items), or 0 beyond that.
char RankLetter(uint8_t grade);
// Weapons store their attack range as two bytes; nothing for other items or a range that is not ordered.
bool HasAttack(const ItemRecord& record);
bool IsWeapon(uint32_t definition);

// The modifier-sheet entry the game puts before a gear item's name, as its name composer picks it: the first modifier of a
// named kind, else the strongest one, with its strength (value / 20) selecting one of five blocks.
std::optional<size_t> NamePrefixIndex(const ItemRecord& record);
inline constexpr int kNameWordCount = 10;
bool IsGear(uint32_t definition);
// Gear durability is capped by 4500, or less with a durability-down modifier; the game enforces it when a save loads.
uint32_t MaxStoredDurability(const ItemRecord& record);
// Lowers what the game would lower when the save loads: the maximum durability to its cap and the durability to the
// maximum. Never raises anything; true when a byte changed.
bool ClampToGameLimits(SaveModel& model, const ItemRef& ref);

// Why ValidateItemChanges refused a change; each is also the English text shown for it.
inline constexpr int kNormalMaxDurability = 4500; // shown; modifiers may lower it for one item
inline constexpr const char* kProblemMaxDurability = "Max durability exceeds the limit the game keeps.";
inline constexpr const char* kProblemLevel = "Required level is invalid.";
inline constexpr const char* kProblemModifier = "Modifier value is outside the supported range.";
inline constexpr const char* kProblemItem = "This item cannot be changed.";
// The classes that can equip an item, in the order the game lists them (Fighter, Amazon, Dwarf, Elf, Sorceress, Wizard);
// it follows from the definition alone, no field of an item record holds it.
std::vector<CharacterClass> UsableBy(uint32_t definition);
// Gear has a required level and a durability the editor may change.
bool HasRequiredLevel(uint32_t definition);
bool HasEditableDurability(uint32_t definition);
uint32_t DurabilityScale(uint32_t definition);
// The largest durability the game keeps for an item, as shown.
int MaxShownDurability(const ItemRecord& record);


// Durability shown by the game for a stored value, and the stored value a shown one means: an unchanged shown value keeps
// the stored one, and the shown maximum is the stored maximum.
uint32_t ShownDurability(uint32_t stored, uint32_t scale);
std::optional<uint32_t> StoredDurability(uint32_t shown, uint32_t current_stored, uint32_t max_stored, uint32_t scale);

// Edits on the working copy. False when the item is empty, malformed, not editable gear or the value is out of range.
bool SetItemDurability(SaveModel& model, const ItemRef& ref, uint32_t shown);
// A lower maximum takes the current durability down with it; a higher one leaves it alone.
bool SetItemMaxDurability(SaveModel& model, const ItemRef& ref, uint32_t shown);
// Changes the stored value of an existing modifier; its id, extra field and position never change. The range is the game's
// 0..200; values are whole numbers when written.
inline constexpr float kModifierValueMin = float(kModifierStoredMin);
inline constexpr float kModifierValueMax = float(kModifierStoredMax);
bool IsValidModifierValue(float value);
bool SetModifierValue(SaveModel& model, const ItemRef& ref, int slot, float value);
// Sets the number the game shows for a modifier; the stored value follows from the game's formula.
bool SetModifierShown(SaveModel& model, const ItemRef& ref, int slot, int shown);
// Weapon attack range: whole bytes, the minimum never above the maximum.
bool SetItemAttack(SaveModel& model, const ItemRef& ref, int minimum, int maximum);
// The Attack editors: the range is the whole byte and the pair stays ordered (raising the minimum raises the maximum,
// lowering the maximum lowers the minimum). Attack and the Required level never change each other.
bool EditItemAttackMin(SaveModel& model, const ItemRef& ref, int minimum);
bool EditItemAttackMax(SaveModel& model, const ItemRef& ref, int maximum);
// Only the level changes.
bool SetItemRequiredLevel(SaveModel& model, const ItemRef& ref, int displayed);

// The editor's one way to edit an item number, by field key: attack_min, attack_max, durability, max_durability,
// required_level, modifier_0 .. modifier_7. Every control (buttons, hold, slider, typed text) proposes a value here; the setter
// applies the rules and the control then reads the result back.
std::optional<int> ItemFieldValue(const SaveModel& model, const ItemRef& ref, std::string_view key);
bool SetItemField(SaveModel& model, const ItemRef& ref, std::string_view key, int value);
std::vector<uint8_t> ItemBytes(const SaveModel& model, const ItemRef& ref);
void RestoreItemBytes(SaveModel& model, const ItemRef& ref, std::span<const uint8_t> bytes);

// A number the editor showed, and the first one that no longer equals what the edited record holds.
struct VisibleField {
    ItemRef ref;
    std::string key;
    int shown = 0;
};
struct UiMismatch {
    VisibleField field;
    int edited = 0;
};
std::optional<UiMismatch> FindUiMismatch(const SaveModel& edited, const std::vector<VisibleField>& visible);

// Whether `after` differs from `before` inside item records only in fields the editor writes, and every changed item is
// still valid. The first problem is put in `error`.
bool ValidateItemChanges(std::span<const uint8_t> before, std::span<const uint8_t> after, std::string& error);

} // namespace dcp
