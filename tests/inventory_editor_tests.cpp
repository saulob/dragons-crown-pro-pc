// Focused tests of the Inventory Editor: item records of SAVE0.DAT, the item catalog, the edits and their safety
// checks, the Apply / Discard transaction and the editor page's structure. Saves are synthetic and built in memory;
// real saves under the source tree are copied to a temporary folder before they are read. No window, no game.
#include <algorithm>
#include <cctype>
#include <climits>
#include <tuple>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "character_art.h"
#include "cpk_fixture.h"
#include "game_mbs.h"
#include "editor_model.h"
#include "game_profile.h"
#include "game_translation.h"
#include "inventory_data.h"
#include "inventory_model.h"
#include "item_catalog.h"
#include "localization/ui_text.h"
#include "modifier_values.h"
#include "project_sheets.h"
#include "save_backup.h"
#include "save_data.h"
#include "save_discovery.h"
#include "save_edit.h"
#include "utf8.h"
#include "trg/layout.h"

namespace fs = std::filesystem;
using namespace dcp;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* what, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL line %d: %s\n", line, what);
    }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using Bytes = std::vector<uint8_t>;

void PutU16(Bytes& bytes, size_t at, uint16_t value) {
    bytes[at] = uint8_t(value);
    bytes[at + 1] = uint8_t(value >> 8);
}

void PutU32(Bytes& bytes, size_t at, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) bytes[at + i] = uint8_t(value >> (8 * i));
}

uint32_t GetU32(const Bytes& bytes, size_t at) {
    return uint32_t(bytes[at]) | uint32_t(bytes[at + 1]) << 8 | uint32_t(bytes[at + 2]) << 16 | uint32_t(bytes[at + 3]) << 24;
}

void PutFloat(Bytes& bytes, size_t at, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    PutU32(bytes, at, bits);
}

size_t At(const ItemRef& ref) { return *ItemOffset(ref); }

struct Modifier {
    uint16_t id;
    float value;
};

struct Item {
    uint32_t definition = 0;
    std::vector<Modifier> modifiers;
    uint32_t durability = 0;
    uint32_t durability_max = 0;
    uint8_t level_stored = 0;
    uint8_t flags = 1;
    uint8_t attack_min = 0;
    uint8_t attack_max = 0;
    uint8_t grade = 0xFF;
};

void PutEmptyItem(Bytes& bytes, size_t base) {
    for (size_t i = 0; i < kItemEffectCapacity; ++i) {
        PutU16(bytes, base + i * kItemEffectStride, kNoEffect);
        PutU16(bytes, base + i * kItemEffectStride + 2, 0xCCCC);
        PutU32(bytes, base + i * kItemEffectStride + 4, 0);
    }
    PutU32(bytes, base + kItemFlags, 0x20000000);
    PutU32(bytes, base + kItemDurability, 0);
    PutU32(bytes, base + kItemDurabilityMax, 0);
    const uint8_t tail[8] = {0, 0, 0, 0xFF, 0, 0, 0xFF, 0xFF};
    std::memcpy(bytes.data() + base + 0x4C, tail, sizeof(tail));
}

void PutItem(Bytes& bytes, const ItemRef& ref, const Item& item) {
    const size_t base = At(ref);
    PutEmptyItem(bytes, base);
    for (size_t i = 0; i < item.modifiers.size(); ++i) {
        PutU16(bytes, base + i * kItemEffectStride, item.modifiers[i].id);
        PutU16(bytes, base + i * kItemEffectStride + 2, 0);
        PutFloat(bytes, base + i * kItemEffectStride + 4, item.modifiers[i].value);
    }
    PutU32(bytes, base + kItemFlags, (item.definition & kDefinitionMask) | uint32_t(item.modifiers.size()) << 25 | uint32_t(item.flags) << 29);
    PutU32(bytes, base + kItemDurability, item.durability);
    PutU32(bytes, base + kItemDurabilityMax, item.durability_max);
    bytes[base + 0x4D] = 1;
    bytes[base + kItemLevel] = item.level_stored;
    bytes[base + kItemAttackMin] = item.attack_min;
    bytes[base + kItemAttackMax] = item.attack_max;
    bytes[base + kItemGrade] = item.grade;
}

void PutCharacter(Bytes& bytes, int slot, int character, const char* name, CharacterClass klass) {
    const size_t base = size_t(slot) * kDataSlotStride + kCharacterOffset + size_t(character) * kCharacterStride;
    PutU32(bytes, base, 0);
    PutU16(bytes, base + kCharHpCurrent, 300);
    PutU16(bytes, base + kCharHpMax, 300);
    PutU16(bytes, base + kCharSkillPoints, 5);
    bytes[base + kCharLevel] = 20;
    bytes[base + kCharClass] = uint8_t(klass);
    bytes[base + kCharColor] = 0;
    std::memset(bytes.data() + base + kCharName, 0, kCharNameBytes);
    std::memcpy(bytes.data() + base + kCharName, name, std::strlen(name));
    std::memcpy(bytes.data() + base + kCharNameSecond, name, std::strlen(name));
}

void Seal(Bytes& bytes) { PutU32(bytes, kSaveCrcOffset, SaveCrc32(bytes)); }

// DATA 1: George (Dwarf) and Alex (Amazon) with shared storage and equipment sets; DATA 2: George and one stored item.
Bytes MakeInventorySave() {
    Bytes bytes(kSaveFileSize, 0xCC);
    std::memcpy(bytes.data() + kSaveMagicOffset, "DCSD", 4);
    for (int slot = 0; slot < int(kDataSlotCount); ++slot) {
        for (int index = 0; index < int(kStorageCapacity); ++index) PutEmptyItem(bytes, At({slot, -1, 0, index}));
        for (int character = 0; character < int(kCharacterCount); ++character) {
            for (int set = 0; set < int(kEquipmentSetCount); ++set) {
                for (int index = 0; index < int(kEquipmentSetItems); ++index) PutEmptyItem(bytes, At({slot, character, set, index}));
            }
        }
    }
    for (int slot = 0; slot < int(kDataSlotCount); ++slot) {
        for (int character = 0; character < int(kCharacterCount); ++character) {
            bytes[size_t(slot) * kDataSlotStride + kCharacterOffset + size_t(character) * kCharacterStride + kCharName] = 0;
        }
    }
    PutCharacter(bytes, 0, 0, "George", CharacterClass::kDwarf);
    PutCharacter(bytes, 0, 1, "Alex", CharacterClass::kAmazon);
    PutCharacter(bytes, 1, 0, "George", CharacterClass::kDwarf);

    PutItem(bytes, {0, -1, 0, 0}, {283, {{0x12, 94}, {0x00, 89}, {0x2A, 58}}, 189587, 189587, 93, 1, 227, 250, 0});
    PutItem(bytes, {0, -1, 0, 1}, {445, {{0x110, 77}}, 230381, 230381, 98});
    PutItem(bytes, {0, -1, 0, 2}, {349, {}, 100000, 100000, 4});
    PutItem(bytes, {0, -1, 0, 3}, {112, {{0x194, 45}}, 4, 5, 0});
    PutItem(bytes, {0, -1, 0, 7}, {600, {{0x12, 10}}, 100, 200, 5});
    PutItem(bytes, {0, -1, 0, 9}, {3000, {{0x7FF, 12}}, 100, 200, 5});
    PutItem(bytes, {0, 0, 0, 0}, {279, {{0x29, 88}, {0x20, 68}}, 219363, 286263, 98, 3});
    PutItem(bytes, {0, 0, 0, 1}, {462, {{0x119, 94}}, 0, 235021, 98, 3});
    PutItem(bytes, {0, -1, 0, 10}, {284, {{0x00, 81}, {0x29, 70}}, 230738, 230738, 86, 1, 228, 243, 1});
    PutItem(bytes, {0, 0, 0, 5}, {112, {{0x1A5, 20}}, 2, 5, 0});
    PutItem(bytes, {0, 0, 3, 2}, {390, {{0x11C, 97}, {0x121, 90}}, 10110, 290660, 96, 3});
    PutItem(bytes, {0, 1, 0, 0}, {214, {{0x21, 50}}, 190000, 200000, 40, 3});
    PutItem(bytes, {1, -1, 0, 0}, {231, {{0x6A, 20}}, 200985, 200985, 97});
    PutItem(bytes, {1, 0, 0, 0}, {283, {}, 150000, 150000, 10, 3});
    Seal(bytes);
    return bytes;
}

std::vector<std::string> FixtureItems() {
    std::vector<std::string> items(500);
    items[112] = "%s";
    items[214] = "Poleaxe";
    items[231] = "Strike Axe";
    items[279] = "Berserk Hammer";
    items[283] = "Rock Hammer";
    items[284] = "Martial Hammer";
    items[349] = "Buckler";
    items[390] = "Jewel Amulet";
    items[445] = "Leather Boots ";
    items[462] = "Hunter Mantle";
    return items;
}

std::vector<std::string> FixtureEffects() {
    std::vector<std::string> effects(5238);
    effects[925 + 0x12] = "Demon's";
    effects[925 + 0x00] = "Envenomed";
    effects[0x119 + 188 * 4 + 1113] = "Blessed";
    effects[0x110 + 188 * 3 + 1113] = "Resistant ";
    effects[0x194 + 1865] = "Rebirth Scroll";
    effects[0x1A5 + 1865] = "Elixir";
    effects[0x209 + 1865] = "##(SKILL_RESETITEM)";
    effects[0x110 + 1113] = "Frail ";
    effects[0x110 + 188 * 1 + 1113] = "Mild ";
    effects[0x00] = "Poison enemy @c(15)%d\xEF\xBC\x85@/c chance";
    effects[0x12] = "Magic damage @c(15)+%d\xEF\xBC\x85@/c";
    effects[0x2A] = "Knockdown chance @c(15)+%d\xEF\xBC\x85@/c";
    effects[0x110 + 0x39d] = "Resistant ";
    effects[8] = "@c(15)%d\xEF\xBC\x85@/c of DMG taken converted to MP";
    effects[14] = "Max MP @c(15)+%d@/c";
    effects[155] = "@c(20)Power of Frenzy +%d\xEF\xBC\x85@/c";
    effects[115] = "DEX @c(15)+%d@/c";
    effects[30] = "Throw damage @c(15)+%d\xEF\xBC\x85@/c";
    effects[3] = "Petrify enemy @c(15)%d\xEF\xBC\x85@/c chance";
    effects[108] = "DEX @c(15)+%d@/c";
    effects[54] = "DMG to human foes @c(15)+%d\xEF\xBC\x85@/c";
    effects[35] = "Critical rate @c(15)+%d\xEF\xBC\x85@/c";
    effects[52] = "Bonus @c(15)%d\xEF\xBC\x85@/c as ice DMG";
    effects[0x194] = "Stout";
    return effects;
}

ItemCatalog FixtureCatalog() { return ItemCatalog(FixtureItems(), FixtureEffects()); }

std::vector<size_t> Diff(const Bytes& a, const Bytes& b) {
    std::vector<size_t> at;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        if (a[i] != b[i]) at.push_back(i);
    }
    return at;
}

std::string ReadText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteBytes(const fs::path& path, const Bytes& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

Bytes ReadBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

fs::path WriteSave(const fs::path& user, const Bytes& bytes) {
    const fs::path file = user / "savedata" / "1" / "CUSA10454" / "SAVEDATA00" / "SAVE0.DAT";
    fs::remove_all(user);
    WriteBytes(file, bytes);
    return file;
}

fs::path WriteSavePath(const fs::path& user) { return user / "savedata" / "1" / "CUSA10454" / "SAVEDATA00" / "SAVE0.DAT"; }

SaveEdit OpenEdit(const fs::path& user) {
    std::vector<DiscoveredSave> saves = DiscoverSaves(user, "CUSA10454");
    CHECK(saves.size() == 1);
    return SaveEdit(saves.front(), BackupFolder(user));
}

// ---- Structure ----

void TestLocation() {
    CHECK(kItemRecordSize == 84 && kStorageCapacity == 500 && kEquipmentSetCount == 9 && kEquipmentSetItems == 10);
    CHECK(At({0, -1, 0, 0}) == 0xC0);
    CHECK(At({0, -1, 0, 499}) == 0xC0 + 499 * 84);
    CHECK(At({1, -1, 0, 3}) == kDataSlotStride + 0xC0 + 3 * 84);
    CHECK(At({0, 0, 0, 0}) == kCharacterOffset + 0x248);
    CHECK(At({0, 0, 1, 0}) == kCharacterOffset + 0x248 + 0x354);
    CHECK(At({0, 0, 0, 9}) == kCharacterOffset + 0x248 + 9 * 84);
    CHECK(At({2, 15, 8, 9}) == 2 * kDataSlotStride + kCharacterOffset + 15 * kCharacterStride + 0x248 + 8 * 0x354 + 9 * 84);
    CHECK(kEquipmentSetStride == kEquipmentSetItems * kItemRecordSize + 12);

    for (const ItemRef& bad : {ItemRef{-1, -1, 0, 0}, ItemRef{3, -1, 0, 0}, ItemRef{0, -1, 0, 500}, ItemRef{0, -1, 0, -1},
                               ItemRef{0, 16, 0, 0}, ItemRef{0, 0, 9, 0}, ItemRef{0, 0, -1, 0}, ItemRef{0, 0, 0, 10}}) {
        CHECK(!ItemOffset(bad));
    }
    // Everything fits in the file, and the storage ends before the fields the character record keeps.
    CHECK(At({2, 15, 8, 9}) + kItemRecordSize <= kSaveFileSize);
    CHECK(kStorageOffset + kStorageCapacity * kItemRecordSize <= kCharacterOffset + kCharHpCurrent);
    CHECK(kEquipmentSetOffset + kEquipmentSetCount * kEquipmentSetStride <= kCharacterStride);
    // Equipment sets of consecutive characters and storage of consecutive slots never overlap.
    CHECK(At({0, 0, 8, 9}) + kItemRecordSize <= At({0, 1, 0, 0}));
    CHECK(At({0, -1, 0, 499}) + kItemRecordSize <= At({1, -1, 0, 0}));
}

void TestParse() {
    const Bytes save = MakeInventorySave();
    const SaveModel model = SaveModel::FromBytes(save);
    CHECK(model.valid());

    const std::vector<ItemRecord> storage = ReadStorage(model.bytes(), 0);
    CHECK(storage.size() == 7);
    CHECK(storage[0].ref == ItemRef({0, -1, 0, 0}) && storage[0].state == ItemState::kValid && storage[0].definition == 283);
    CHECK(storage[0].effects.size() == 3 && storage[0].effects[0].id == 0x12 && storage[0].effects[0].value == 94.0f);
    CHECK(storage[0].durability == 189587 && storage[0].durability_max == 189587 && storage[0].required_level() == 94);
    CHECK(storage[3].definition == 112 && storage[3].durability == 4 && storage[3].durability_max == 5);

    // Empty inventory: a slot with no item.
    CHECK(ReadStorage(model.bytes(), 2).empty());
    CHECK(ReadEquipment(model.bytes(), 0, 5).empty());
    CHECK(ReadItem(model.bytes(), {0, -1, 0, 30}).state == ItemState::kEmpty);

    // Equipped and unequipped are different records: George's set items are not in the storage.
    const std::vector<ItemRecord> george = ReadEquipment(model.bytes(), 0, 0);
    CHECK(george.size() == 4);
    CHECK(george[0].ref == ItemRef({0, 0, 0, 0}) && george[0].definition == 279 && george[0].flags == 3);
    CHECK(george[3].ref == ItemRef({0, 0, 3, 2}) && george[3].definition == 390);
    const std::vector<ItemRecord> alex = ReadEquipment(model.bytes(), 0, 1);
    CHECK(alex.size() == 1 && alex[0].definition == 214);
    CHECK(ReadEquipment(model.bytes(), 1, 0).size() == 1 && ReadStorage(model.bytes(), 1).size() == 1);
    for (const ItemRecord& item : george) CHECK(item.definition != 283 && item.definition != 445);

    // Several categories at once.
    const InventoryList all = BuildInventoryList(model, model, 0, 0, FixtureCatalog());
    CHECK(all.rows.size() == 11);
    CHECK(all.rows.front().ref == ItemRef({0, 0, 0, 0}) && !all.rows.front().ref.storage());
    CHECK(all.rows[4].ref.storage() && all.rows[4].name == "Demon's Rock Hammer");
}

void TestListGroups() {
    const SaveModel model = SaveModel::FromBytes(MakeInventorySave());
    const InventoryList george = BuildInventoryList(model, model, 0, 0, FixtureCatalog());
    const auto count = [&](ItemCategory category) {
        return std::count_if(george.rows.begin(), george.rows.end(), [&](const InventoryRow& row) { return row.category == category; });
    };
    CHECK(count(ItemCategory::kWeapon) == 3 && count(ItemCategory::kShield) == 1 && count(ItemCategory::kAccessory) == 3 &&
          count(ItemCategory::kItem) == 2 && count(ItemCategory::kOther) == 2);
    // Equipment sets come first, in order, then the shared storage; no row is dropped by a category.
    bool storage_seen = false;
    int last_set = -1;
    for (const InventoryRow& row : george.rows) {
        if (row.ref.storage()) storage_seen = true;
        CHECK(!(storage_seen && !row.ref.storage()));
        if (!row.ref.storage()) {
            CHECK(row.ref.set >= last_set);
            last_set = row.ref.set;
        }
    }
    CHECK(BuildInventoryList(model, model, 0, 1, FixtureCatalog()).rows.size() == 1 + 7);
    CHECK(BuildInventoryList(model, model, 0, 5, FixtureCatalog()).rows.size() == 7);
    CHECK(BuildInventoryList(model, model, 2, 0, FixtureCatalog()).rows.empty());
}

void TestDurabilityBound() {
    const Bytes original = MakeInventorySave();
    SaveModel model = SaveModel::FromBytes(original);
    const ItemRef hammer{0, 0, 0, 0}; // 2862 shown at most (stored 286263)
    CHECK(SetItemDurability(model, hammer, 2862) && ReadItem(model.bytes(), hammer).durability == 286263);
    CHECK(!SetItemDurability(model, hammer, 2863) && !SetItemDurability(model, hammer, UINT32_MAX));
    CHECK(ReadItem(model.bytes(), hammer).durability_max == 286263);
    CHECK(SetItemDurability(model, hammer, 0) && ReadItem(model.bytes(), hammer).durability == 0);
    const SaveModel pristine = SaveModel::FromBytes(original);
    const ItemDetail detail = DescribeItem(pristine, pristine, hammer, FixtureCatalog());
    CHECK(detail.durability_range.min == 0 && detail.durability_range.max == 2862 && detail.durability == 2193);
    // A changed maximum is accepted when the current value still fits under it.
    std::string error;
    Bytes raised = original;
    PutU32(raised, At(hammer) + kItemDurabilityMax, 300000);
    CHECK(ValidateItemChanges(original, raised, error));
    Bytes lowered = original;
    PutU32(lowered, At(hammer) + kItemDurabilityMax, 100000);
    PutU32(lowered, At(hammer) + kItemDurability, 100000);
    CHECK(ValidateItemChanges(original, lowered, error));
    Bytes stranded = original;
    PutU32(stranded, At(hammer) + kItemDurabilityMax, 100000);
    CHECK(!ValidateItemChanges(original, stranded, error));
    CHECK(!SetItemDurability(model, {0, -1, 0, 3}, 5));
}

void TestAttack() {
    const Bytes original = MakeInventorySave();
    const SaveModel model = SaveModel::FromBytes(original);
    const ItemDetail hammer = DescribeItem(model, model, {0, -1, 0, 0}, FixtureCatalog());
    CHECK(hammer.has_attack && hammer.attack_min == 227 && hammer.attack_max == 250);
    CHECK(!DescribeItem(model, model, {0, -1, 0, 1}, FixtureCatalog()).has_attack);
    CHECK(!DescribeItem(model, model, {0, -1, 0, 2}, FixtureCatalog()).has_attack);
    CHECK(!DescribeItem(model, model, {0, -1, 0, 3}, FixtureCatalog()).has_attack);
    Bytes unordered = original;
    unordered[At({0, -1, 0, 0}) + kItemAttackMin] = 251;
    const SaveModel odd = SaveModel::FromBytes(unordered);
    CHECK(!DescribeItem(odd, odd, {0, -1, 0, 0}, FixtureCatalog()).has_attack);

    const ItemRef ref{0, -1, 0, 0};
    const size_t at = At(ref);
    SaveModel edited = SaveModel::FromBytes(original);
    CHECK(SetItemAttack(edited, ref, 237, 250));
    CHECK(edited.bytes()[at + kItemAttackMin] == 237 && edited.bytes()[at + kItemAttackMax] == 250);
    CHECK(SetItemAttack(edited, ref, 237, 251) && edited.bytes()[at + kItemAttackMax] == 251 && SetItemAttack(edited, ref, 237, 250)); // no cap by level
    std::vector<size_t> changed = Diff(original, Bytes(edited.bytes().begin(), edited.bytes().end()));
    for (size_t field : changed) CHECK(field == at + kItemAttackMin || field == at + kItemAttackMax);
    // Rank, durability, level, modifiers and every other byte stay exactly as they were.
    SaveModel untouched = SaveModel::FromBytes(original);
    CHECK(SetItemAttack(untouched, ref, 227, 250) && Bytes(untouched.bytes().begin(), untouched.bytes().end()) == original);

    // Boundaries and ordering.
    SaveModel bounds = SaveModel::FromBytes(original);
    CHECK(SetItemAttack(bounds, ref, 0, 0) && SetItemAttack(bounds, ref, 250, 250) && SetItemAttack(bounds, ref, 0, 250));
    CHECK(SetItemAttack(bounds, ref, 100, 100));
    CHECK(!SetItemAttack(bounds, ref, 101, 100) && !SetItemAttack(bounds, ref, -1, 100) && !SetItemAttack(bounds, ref, 0, 256));
    CHECK(bounds.bytes()[at + kItemAttackMin] == 100 && bounds.bytes()[at + kItemAttackMax] == 100);

    // Only weapons: accessories, shields, the modifier-named item, empty slots, unordered or malformed records.
    CHECK(!SetItemAttack(bounds, {0, -1, 0, 1}, 1, 2) && !SetItemAttack(bounds, {0, -1, 0, 2}, 1, 2));
    CHECK(!SetItemAttack(bounds, {0, -1, 0, 3}, 1, 2) && !SetItemAttack(bounds, {0, -1, 0, 30}, 1, 2));
    SaveModel odd_model = SaveModel::FromBytes(unordered);
    CHECK(!SetItemAttack(odd_model, ref, 1, 2));

    // Modifier edits do not touch attack and attack edits do not touch modifiers.
    SaveModel both = SaveModel::FromBytes(original);
    CHECK(SetModifierValue(both, ref, 1, 42.0f) && both.bytes()[at + kItemAttackMin] == 227 && both.bytes()[at + kItemAttackMax] == 250);
    CHECK(SetItemAttack(both, ref, 200, 210) && ReadItem(both.bytes(), ref).effects[1].value == 42.0f && ReadItem(both.bytes(), ref).grade == 0);

    // Apply-side validation: attack bytes of a weapon, only.
    std::string error;
    CHECK(ValidateItemChanges(original, Bytes(edited.bytes().begin(), edited.bytes().end()), error));
    Bytes bad = original;
    bad[at + kItemAttackMin] = 255;
    CHECK(!ValidateItemChanges(original, bad, error));
    bad = original;
    bad[At({0, -1, 0, 1}) + kItemAttackMin] ^= 1; // an accessory's bytes at that position
    CHECK(!ValidateItemChanges(original, bad, error));
    bad = original;
    bad[at + kItemGrade] = 2;
    CHECK(!ValidateItemChanges(original, bad, error));

    // Apply and Discard.
    const fs::path user = fs::temp_directory_path() / "dcp_inventory_attack";
    WriteSave(user, original);
    SaveEdit edit = OpenEdit(user);
    CHECK(SetItemAttack(edit.working(), ref, 230, 240) && edit.dirty());
    edit.Discard();
    CHECK(!edit.dirty() && ReadItem(edit.working().bytes(), ref).attack_min == 227);
    CHECK(SetItemAttack(edit.working(), ref, 230, 240) && edit.Apply() == SaveEdit::Result::kApplied);
    const Bytes written = ReadBytes(WriteSavePath(user));
    CHECK(ReadItem(written, ref).attack_min == 230 && ReadItem(written, ref).attack_max == 240);
    std::error_code ec;
    fs::remove_all(user, ec);
}

void TestHp(const fs::path& base) {
    const Bytes original = MakeInventorySave();
    const fs::path user = base / "hp";
    const fs::path file = WriteSave(user, original);
    SaveEdit edit = OpenEdit(user);
    const CharacterView& george = *edit.working().Character(0, 0);
    CHECK(george.hp_current == 300 && george.hp_max == 300);
    CHECK(edit.working().SetHp(0, 0, 300, 300) && edit.working().SetHp(0, 0, 0, 300) && edit.working().SetHp(0, 0, 123, 300));
    CHECK(!edit.working().SetHp(0, 0, 301, 300));
    CHECK(edit.Apply() == SaveEdit::Result::kApplied);
    const SaveModel after = SaveModel::FromFile(file);
    CHECK(after.Character(0, 0)->hp_current == 123 && after.Character(0, 0)->hp_max == 300);
    CHECK(ReadStorage(after.bytes(), 0).size() == ReadStorage(SaveModel::FromBytes(original).bytes(), 0).size());
    const Bytes hp_only = ReadBytes(file);
    for (const ItemRef& ref : {ItemRef{0, -1, 0, 0}, ItemRef{0, 0, 0, 0}, ItemRef{0, 0, 3, 2}}) {
        const size_t at = At(ref);
        CHECK(std::equal(original.begin() + long(at), original.begin() + long(at + kItemRecordSize), hp_only.begin() + long(at)));
    }

    // The maximum is editable too: lowering it clamps the current value, raising it leaves the current value alone.
    SaveEdit maximum = OpenEdit(user);
    CHECK(maximum.working().SetHp(0, 0, 123, 400) && maximum.Apply() == SaveEdit::Result::kApplied);
    CHECK(SaveModel::FromFile(file).Character(0, 0)->hp_current == 123 && SaveModel::FromFile(file).Character(0, 0)->hp_max == 400);
    HpValues hp{123, 400};
    CHECK(WithMaxHp(hp, 100).current == 100 && WithMaxHp(hp, 100).max == 100);
    CHECK(WithMaxHp(hp, kMaxHp).current == 123 && WithMaxHp(hp, kMaxHp).max == kMaxHp && WithMaxHp(hp, kMaxHp + 1).max == kMaxHp);
    CHECK(WithCurrentHp(hp, 400).current == 400 && WithCurrentHp(hp, 401).current == 400 && WithCurrentHp(hp, -1).current == 0);
    SaveEdit field_limit = OpenEdit(user);
    CHECK(field_limit.working().SetHp(0, 0, 65535, 65535) && field_limit.Apply() == SaveEdit::Result::kApplied);
    CHECK(SaveModel::FromFile(file).Character(0, 0)->hp_max == 65535 && SaveModel::FromFile(file).Character(0, 0)->hp_current == 65535);
    SaveEdit over = OpenEdit(user);
    CHECK(!over.working().SetHp(0, 0, 66, 65) && !over.dirty());

    // Inventory-only edits leave HP alone.
    SaveEdit inventory = OpenEdit(user);
    CHECK(SetItemDurability(inventory.working(), {0, 0, 0, 0}, 5) && inventory.Apply() == SaveEdit::Result::kApplied);
    const SaveModel inventory_after = SaveModel::FromFile(file);
    CHECK(inventory_after.Character(0, 0)->hp_current == 65535 && inventory_after.Character(0, 0)->hp_max == 65535);
}

void TestScrollReset() {
    InventorySelection selection;
    CHECK(!selection.TakeScrollReset());
    const ItemRef a{0, 0, 0, 0}, b{0, 0, 0, 1}, c{0, -1, 0, 0};
    selection.Select(a);
    CHECK(selection.item == a && selection.TakeScrollReset() && !selection.TakeScrollReset());
    selection.Select(a); // the same item again
    CHECK(!selection.TakeScrollReset());
    selection.Select(b);
    CHECK(selection.TakeScrollReset());
    selection.Select(c); // another group
    CHECK(selection.TakeScrollReset() && selection.item == c);

    // A filter change that keeps the item keeps the scroll; one that falls back resets it.
    const SaveModel model = SaveModel::FromBytes(MakeInventorySave());
    const ItemCatalog catalog = FixtureCatalog();
    selection.Select(a);
    selection.TakeScrollReset();
    selection.Select(ResolveSelection(BuildInventoryList(model, model, 0, 0, catalog, kLocationAll), selection.item));
    CHECK(!selection.TakeScrollReset() && selection.item == a);
    selection.Select(ResolveSelection(BuildInventoryList(model, model, 0, 0, catalog, 0), selection.item));
    CHECK(!selection.TakeScrollReset() && selection.item == a);
    selection.Select(ResolveSelection(BuildInventoryList(model, model, 0, 0, catalog, kLocationStorage), selection.item));
    CHECK(selection.TakeScrollReset() && selection.item == ItemRef({0, -1, 0, 0}));
    selection.Select(ResolveSelection(BuildInventoryList(model, model, 0, 0, catalog, 1), selection.item));
    CHECK(selection.TakeScrollReset() && !selection.item);
    selection.Clear();
    CHECK(!selection.item && !selection.TakeScrollReset());
}

ItemRecord GearWith(uint32_t definition, std::vector<ItemEffect> effects, uint8_t word = 0xFF) {
    ItemRecord record;
    record.state = ItemState::kValid;
    record.definition = definition;
    record.effects = std::move(effects);
    record.name_word = word;
    return record;
}

// The game's name composer (0x209d0) and prefix lookup (0x1a1d0).
void TestNameRule() {
    // The strongest modifier names the item; its strength (value / 20, at most 4) picks the block.
    CHECK(NamePrefixIndex(GearWith(376, {{10, 0, 30.0f}, {20, 0, 61.0f}})) == std::optional<size_t>(20 + 185 * 3 + 185));
    CHECK(NamePrefixIndex(GearWith(376, {{10, 0, 99.0f}, {20, 0, 61.0f}})) == std::optional<size_t>(10 + 185 * 4 + 185));
    CHECK(NamePrefixIndex(GearWith(376, {{10, 0, 100.0f}})) == std::optional<size_t>(10 + 185 * 4 + 185));
    CHECK(NamePrefixIndex(GearWith(376, {{10, 0, 1.0f}})) == std::optional<size_t>(10 + 185));
    // Ties keep the first; a modifier of a named kind wins whatever its strength.
    CHECK(NamePrefixIndex(GearWith(376, {{10, 0, 50.0f}, {20, 0, 50.0f}})) == std::optional<size_t>(10 + 185 * 2 + 185));
    CHECK(NamePrefixIndex(GearWith(376, {{10, 0, 99.0f}, {120, 0, 21.0f}})) == std::optional<size_t>(120 + 185 * 1 + 185));
    for (const uint16_t named : {110, 158, 295, 346, 532, 581, 644, 694, 751, 800, 876, 926}) {
        CHECK(NamePrefixIndex(GearWith(376, {{5, 0, 99.0f}, {named, 0, 1.0f}})).has_value());
        CHECK(NamePrefixIndex(GearWith(376, {{5, 0, 99.0f}, {named, 0, 1.0f}})) != NamePrefixIndex(GearWith(376, {{5, 0, 99.0f}})));
    }
    // One block size per range of modifier ids; rune ids have no prefix.
    CHECK(NamePrefixIndex(GearWith(376, {{200, 0, 40.0f}})) == std::optional<size_t>(200 + 188 * 2 + 1113));
    CHECK(NamePrefixIndex(GearWith(376, {{600, 0, 40.0f}})) == std::optional<size_t>(600 + 219 * 2 + 2243));
    CHECK(NamePrefixIndex(GearWith(376, {{960, 0, 40.0f}})) == std::optional<size_t>(960 + 228 * 2 + 3347));
    CHECK(!NamePrefixIndex(GearWith(376, {{400, 0, 40.0f}})) && !NamePrefixIndex(GearWith(376, {{979, 0, 40.0f}})));
    CHECK(!NamePrefixIndex(GearWith(376, {})) && !NamePrefixIndex(GearWith(112, {{404, 0, 40.0f}})));
    // The largest index the lookup produces is the sheet's last entry.
    CHECK(NamePrefixIndex(GearWith(376, {{978, 0, 100.0f}})) == std::optional<size_t>(5237));

    // A word byte replaces the modifier prefix with SystemMsg 1485 + n.
    std::vector<std::string> words(kNameWordCount);
    words[1] = "Heirloom ";
    const ItemCatalog catalog(FixtureItems(), FixtureEffects(), words);
    CHECK(ItemDisplayName(GearWith(283, {{0x12, 0, 94.0f}}, 1), catalog) == "Heirloom Rock Hammer");
    CHECK(ItemDisplayName(GearWith(283, {{0x12, 0, 94.0f}}, 2), catalog) == "Rock Hammer");
    CHECK(ItemDisplayName(GearWith(283, {{0x12, 0, 94.0f}}), catalog) == "Demon's Rock Hammer");
    std::vector<std::string> system(1500);
    for (int i = 0; i < kNameWordCount; ++i) system[size_t(1485 + i)] = "word" + std::to_string(i);
    const std::vector<std::string> taken = NameWords(system);
    CHECK(taken.size() == size_t(kNameWordCount) && taken.front() == "word0" && taken.back() == "word9" && NameWords({}).empty());
}

void TestNames() {
    const ItemCatalog catalog = FixtureCatalog();
    const SaveModel model = SaveModel::FromBytes(MakeInventorySave());
    const auto name = [&](const ItemRef& ref) { return ItemDisplayName(ReadItem(model.bytes(), ref), catalog); };
    CHECK(name({0, -1, 0, 0}) == "Demon's Rock Hammer");        // strongest modifier 0x12 at 94: block 4
    CHECK(name({0, -1, 0, 10}) == "Envenomed Martial Hammer");   // strongest modifier 0x00 at 81: block 4
    CHECK(name({0, -1, 0, 1}) == "Resistant Leather Boots");     // modifier 0x110 at 77: block 3 of its range
    CHECK(name({0, 0, 0, 1}) == "Blessed Hunter Mantle");
    CHECK(name({0, -1, 0, 2}) == "Buckler");                      // no modifiers, no prefix
    CHECK(name({0, 0, 0, 0}) == "Berserk Hammer");
    CHECK(name({0, -1, 0, 3}) == "Rebirth Scroll");               // an Items entry: its first modifier id + 1865
    CHECK(name({0, 0, 0, 5}) == "Elixir");
    CHECK(ItemDisplayName(GearWith(112, {{0x209, 0, 100.0f}}), catalog) == "Special item (SKILL_RESETITEM)");
    CHECK(ItemDisplayName(GearWith(112, {{0x100, 0, 100.0f}}), catalog) == "Unknown item 0x70" && ItemDisplayName(GearWith(112, {}), catalog) == "Unknown item 0x70");
    // List rows and the detail header use the one composed name.
    const InventoryList list = BuildInventoryList(model, model, 0, 0, catalog);
    for (const InventoryRow& row : list.rows) CHECK(row.name == DescribeItem(model, model, row.ref, catalog).name);
    // Missing components fall back to the base name and never touch the save.
    CHECK(ItemDisplayName(ReadItem(model.bytes(), {0, -1, 0, 0}), ItemCatalog(FixtureItems(), {})) == "Rock Hammer");
    CHECK(ItemDisplayName(ReadItem(model.bytes(), {0, -1, 0, 0}), ItemCatalog()) == "Unknown item 0x11b");
    std::vector<std::string> broken = FixtureEffects();
    broken[925 + 0x12] = "Hits for @c(15)%d@/c";
    CHECK(ItemDisplayName(ReadItem(model.bytes(), {0, -1, 0, 0}), ItemCatalog(FixtureItems(), broken)) == "Rock Hammer");
    CHECK(NamePrefixIndex(ReadItem(model.bytes(), {0, -1, 0, 0})) == std::optional<size_t>(925 + 0x12));
    CHECK(NamePrefixIndex(ReadItem(model.bytes(), {0, -1, 0, 10})) == std::optional<size_t>(925));
    CHECK(!NamePrefixIndex(ReadItem(model.bytes(), {0, -1, 0, 2})) && !NamePrefixIndex(ReadItem(model.bytes(), {0, -1, 0, 30})));
    CHECK(!NamePrefixIndex(ReadItem(model.bytes(), {0, -1, 0, 3}))); // Items have no prefix
    TestNameRule();
}

void TestModifierEdit() {
    const Bytes original = MakeInventorySave();
    SaveModel model = SaveModel::FromBytes(original);
    const ItemRef hammer{0, -1, 0, 0};
    const size_t at = At(hammer);

    CHECK(SetModifierValue(model, hammer, 1, 42.0f));
    CHECK(ReadItem(model.bytes(), hammer).effects[1].value == 42.0f);
    const std::vector<size_t> changed = Diff(original, Bytes(model.bytes().begin(), model.bytes().end()));
    for (size_t field : changed) CHECK(field >= at + 8 + 4 && field < at + 8 + 8);
    const ItemRecord after = ReadItem(model.bytes(), hammer);
    CHECK(after.effects[0].id == 0x12 && after.effects[0].value == 94.0f && after.effects[2].id == 0x2A && after.effects[2].value == 58.0f);
    CHECK(after.effects[1].id == 0x00 && after.effects[1].extra == 0 && after.effects.size() == 3);

    // An unchanged value writes nothing; a fractional stored value is kept until edited.
    Bytes fractional = original;
    PutFloat(fractional, at + 4, 66.5f);
    SaveModel fraction_model = SaveModel::FromBytes(fractional);
    CHECK(SetModifierValue(fraction_model, hammer, 0, 66.5f));
    CHECK(Bytes(fraction_model.bytes().begin(), fraction_model.bytes().end()) == fractional);
    CHECK(SetModifierValue(fraction_model, hammer, 0, 67.0f) && ReadItem(fraction_model.bytes(), hammer).effects[0].value == 67.0f);
    CHECK(DescribeItem(SaveModel::FromBytes(fractional), SaveModel::FromBytes(fractional), hammer, FixtureCatalog()).modifiers[0].stored == 67);

    // Range and finiteness.
    SaveModel strict = SaveModel::FromBytes(original);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (const float bad : {nan, inf, -inf, -0.5f, 200.01f, -5.0f, 1e30f}) CHECK(!SetModifierValue(strict, hammer, 0, bad));
    CHECK(SetModifierValue(strict, hammer, 0, 0.0f) && SetModifierValue(strict, hammer, 0, 100.0f) && SetModifierValue(strict, hammer, 0, 200.0f));
    CHECK(!SetModifierValue(strict, hammer, 3, 50.0f) && !SetModifierValue(strict, hammer, -1, 50.0f) && !SetModifierValue(strict, hammer, 8, 50.0f));
    CHECK(!IsValidModifierValue(nan) && IsValidModifierValue(50.5f));
    CHECK(!SetModifierValue(strict, {0, -1, 0, 30}, 0, 50.0f)); // an empty item has no modifiers

    // Only equipment is edited: an unknown item and an Items entry keep their modifiers.
    const ItemRef unknown{0, -1, 0, 9};
    CHECK(!SetModifierValue(model, unknown, 0, 77.0f));
    const ItemRecord unknown_after = ReadItem(model.bytes(), unknown);
    CHECK(unknown_after.definition == 3000 && unknown_after.effects[0].id == 0x7FF && unknown_after.effects[0].value == 12.0f);

    // The modifier-named item, with a non-zero extra field that is preserved.
    Bytes with_extra = original;
    PutU16(with_extra, At({0, -1, 0, 3}) + 2, 0x3F80);
    SaveModel extra_model = SaveModel::FromBytes(with_extra);
    CHECK(!SetModifierValue(extra_model, {0, -1, 0, 3}, 0, 60.0f) && ReadItem(extra_model.bytes(), {0, -1, 0, 3}).effects[0].extra == 0x3F80);

    // Malformed items are never edited.
    Bytes broken = original;
    PutU32(broken, at + kItemFlags, 283u | 1u << 25 | 1u << 29);
    SaveModel broken_model = SaveModel::FromBytes(broken);
    CHECK(!SetModifierValue(broken_model, hammer, 0, 50.0f));

    // Apply-side validation: only occupied value bytes may change, to a valid number.
    std::string error;
    CHECK(ValidateItemChanges(original, Bytes(model.bytes().begin(), model.bytes().end()), error));
    Bytes bad = original;
    PutFloat(bad, at + 4, nan);
    CHECK(!ValidateItemChanges(original, bad, error));
    PutFloat(bad, at + 4, 500.0f);
    CHECK(!ValidateItemChanges(original, bad, error));
    PutFloat(bad, at + 4, 50.0f);
    CHECK(ValidateItemChanges(original, bad, error));
    bad = original;
    PutFloat(bad, at + 3 * kItemEffectStride + 4, 50.0f); // an empty modifier slot
    CHECK(!ValidateItemChanges(original, bad, error));
    bad = original;
    PutU16(bad, at, 0x13); // the id
    CHECK(!ValidateItemChanges(original, bad, error));
    bad = original;
    PutU16(bad, at + 2, 1); // the extra field
    CHECK(!ValidateItemChanges(original, bad, error));

    // Discard puts the original value back.
    const fs::path user = fs::temp_directory_path() / "dcp_inventory_modifier";
    WriteSave(user, original);
    SaveEdit edit = OpenEdit(user);
    CHECK(SetModifierValue(edit.working(), hammer, 2, 30.0f) && edit.dirty());
    edit.Discard();
    CHECK(!edit.dirty() && ReadItem(edit.working().bytes(), hammer).effects[2].value == 58.0f);
    CHECK(SetModifierValue(edit.working(), hammer, 2, 30.0f) && edit.Apply() == SaveEdit::Result::kApplied);
    CHECK(ReadItem(ReadBytes(WriteSavePath(user)), hammer).effects[2].value == 30.0f);
    std::error_code ec;
    fs::remove_all(user, ec);
}

void TestModifierNames() {
    CHECK(ModifierName("Magic damage +?%") == "Magic damage");
    CHECK(ModifierName("Poison enemy ?% chance") == "Poison enemy chance");
    CHECK(ModifierName("Bonus ?% as fire DMG") == "Bonus as fire DMG");
    CHECK(ModifierName("INT +?") == "INT");
    CHECK(ModifierName("Gold obtained +?%, DMG dealt -?%") == "Gold obtained, DMG dealt");
    CHECK(ModifierName("Resistant") == "Resistant");
    const ItemCatalog catalog = FixtureCatalog();
    CHECK(catalog.Effect(0x12).name == "Magic damage" && catalog.Effect(0x00).name == "Poison enemy chance");
    CHECK(catalog.Effect(0x110).name == "Resistant");
}

void TestLocationFilter() {
    const SaveModel model = SaveModel::FromBytes(MakeInventorySave());
    const ItemCatalog catalog = FixtureCatalog();
    const auto rows = [&](int location) { return BuildInventoryList(model, model, 0, 0, catalog, location); };
    const InventoryList all = rows(kLocationAll);
    CHECK(all.rows.size() == 11);
    size_t sum = 0;
    for (int set = 0; set < int(kEquipmentSetCount); ++set) {
        const InventoryList one = rows(set);
        for (const InventoryRow& row : one.rows) CHECK(!row.ref.storage() && row.ref.set == set);
        sum += one.rows.size();
    }
    CHECK(rows(0).rows.size() == 3 && rows(3).rows.size() == 1 && rows(1).rows.empty() && rows(8).rows.empty());
    const InventoryList storage = rows(kLocationStorage);
    for (const InventoryRow& row : storage.rows) CHECK(row.ref.storage());
    CHECK(storage.rows.size() == 7 && sum + storage.rows.size() == all.rows.size());
    CHECK(rows(42).rows.empty());

    // The selection stays when still listed; otherwise the first understood item; nothing when the view is empty.
    const std::optional<ItemRef> kept = ResolveSelection(rows(0), ItemRef{0, 0, 0, 1});
    CHECK(kept && *kept == ItemRef({0, 0, 0, 1}));
    const std::optional<ItemRef> moved = ResolveSelection(rows(kLocationStorage), ItemRef{0, 0, 0, 1});
    CHECK(moved && *moved == ItemRef({0, -1, 0, 0}));
    CHECK(!ResolveSelection(rows(1), ItemRef{0, 0, 0, 1}) && !ResolveSelection(rows(1), std::nullopt));
    CHECK(ResolveSelection(rows(kLocationAll), std::nullopt) == std::optional<ItemRef>(ItemRef{0, 0, 0, 0}));
    Bytes broken = MakeInventorySave();
    PutU32(broken, At({0, 0, 0, 0}) + kItemFlags, 279u | 9u << 25 | 3u << 29);
    const SaveModel broken_model = SaveModel::FromBytes(broken);
    CHECK(ResolveSelection(BuildInventoryList(broken_model, broken_model, 0, 0, catalog, 0), std::nullopt) == std::optional<ItemRef>(ItemRef{0, 0, 0, 1}));

    // Pending edits stay in the working copy and show in every view.
    SaveModel edited = SaveModel::FromBytes(MakeInventorySave());
    CHECK(SetItemDurability(edited, {0, 0, 3, 2}, 1));
    CHECK(BuildInventoryList(edited, model, 0, 0, catalog, 3).rows[0].changed);
    CHECK(BuildInventoryList(edited, model, 0, 0, catalog, 0).rows[0].changed == false);
    CHECK(BuildInventoryList(edited, model, 0, 0, catalog, kLocationAll).rows.size() == 11);
    CHECK(ReadItem(edited.bytes(), {0, 0, 3, 2}).durability == 100);
}

// ---- Catalog ----

std::string SheetText(const std::string& json, int index) {
    const size_t at = json.find("\"index\": " + std::to_string(index) + ",");
    if (at == std::string::npos) return "<missing>";
    const size_t start = json.find("\"en\": \"", at) + 7;
    std::string text;
    for (size_t i = start; i < json.size() && json[i] != '"'; ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            ++i;
            text += json[i] == 'n' ? '\n' : json[i];
        } else {
            text += json[i];
        }
    }
    return text;
}

std::optional<ItemCatalog> g_real_catalog;

void TestCatalog(const fs::path& source) {
    CHECK(CategoryOf(186) == ItemCategory::kOther && CategoryOf(187) == ItemCategory::kWeapon);
    CHECK(CategoryOf(348) == ItemCategory::kWeapon && CategoryOf(349) == ItemCategory::kShield);
    CHECK(CategoryOf(374) == ItemCategory::kShield && CategoryOf(375) == ItemCategory::kAccessory);
    CHECK(CategoryOf(488) == ItemCategory::kAccessory && CategoryOf(489) == ItemCategory::kOther);
    CHECK(CategoryOf(112) == ItemCategory::kItem && CategoryOf(0) == ItemCategory::kOther && CategoryOf(0xFFFFFF) == ItemCategory::kOther);

    const ItemCatalog catalog = FixtureCatalog();
    CHECK(catalog.loaded() && !ItemCatalog().loaded());
    CHECK(catalog.ItemName(445) == "Leather Boots");
    CHECK(catalog.ItemName(5) == "" && catalog.ItemName(100000) == "");
    CHECK(UnknownItemName(0x1234) == "Unknown item 0x1234" && UnknownEffectName(0x7FF) == "Unknown modifier 0x7ff");
    CHECK(catalog.Effect(0x12).known && catalog.Effect(0x12).text == "Magic damage +?%" && catalog.Effect(0x12).name == "Magic damage");
    CHECK(catalog.Effect(0x00).text == "Poison enemy ?% chance");
    CHECK(!catalog.Effect(0x7FF).known && !catalog.Effect(0xFFFF).known && !catalog.Effect(0x05).known);
    CHECK(CleanEffectText("@c(20)Gold obtained +%d\xEF\xBC\x85, DMG dealt -%d\xEF\xBC\x85@/c") == "Gold obtained +?%, DMG dealt -?%");
    CHECK(CleanEffectText("Broken @c(15 tag") == "Broken @c(15 tag");

    const SaveModel model = SaveModel::FromBytes(MakeInventorySave());
    CHECK(ItemDisplayName(ReadItem(model.bytes(), {0, -1, 0, 3}), catalog) == "Rebirth Scroll");
    CHECK(ItemDisplayName(ReadItem(model.bytes(), {0, -1, 0, 9}), catalog) == "Unknown item 0xbb8");
    CHECK(ItemDisplayName(ReadItem(model.bytes(), {0, -1, 0, 0}), ItemCatalog()) == "Unknown item 0x11b");

    // The catalog reads the same sheets the game's translation system uses; here the English sheets of the project.
    const fs::path sheets = source / "localization" / "en" / "sheets";
    if (fs::is_regular_file(sheets / "ItemList.json") && fs::is_regular_file(sheets / "CustomItem.json")) {
        const std::string items = ReadText(sheets / "ItemList.json");
        const std::string effects = ReadText(sheets / "CustomItem.json");
        CHECK(SheetText(items, 283) == "Rock Hammer" && SheetText(items, 112) == "%s" && SheetText(items, 445) == "Leather Boots");
        CHECK(SheetText(effects, 0x12).starts_with("Magic damage") && SheetText(effects, 0).starts_with("Poison enemy"));
        std::vector<std::string> item_records, effect_records;
        for (int i = 0; i < 740; ++i) item_records.push_back(SheetText(items, i));
        for (int i = 0; i < 5238; ++i) effect_records.push_back(SheetText(effects, i));
        std::vector<std::string> system_records;
        const std::string system = ReadText(sheets / "SystemMsg.json");
        for (int i = 0; i < 1500; ++i) system_records.push_back(SheetText(system, i));
        const ItemCatalog real(item_records, effect_records, NameWords(system_records));
        g_real_catalog = real;
        CHECK(real.Word(0) == "Gifted" && real.Word(9) == "Warped" && real.Word(10).empty());
        CHECK(real.ItemName(283) == "Rock Hammer" && real.ItemName(214) == "Poleaxe" && real.ItemName(349) == "Buckler");
        CHECK(real.Effect(0x2A).text == "Knockdown chance +?%");
        // The gear runs follow the list's own order: the first and last name of each run.
        CHECK(real.ItemName(kFirstWeapon) == "Short Sword" && real.ItemName(kFirstShield) == "Buckler");
        CHECK(real.ItemName(kFirstAccessory) == "Oracle Pendant" && real.ItemName(kEndGear - 1) == "Chaos Monocle");
        CHECK(real.ItemName(kEndGear) == "Money in Treasure Chest");
    } else {
        std::printf("localization sheets not present, skipping their check\n");
    }
}

// ---- Portuguese (Brazil) ----

using ProjectSheet = project_sheets::Sheet;

ProjectSheet LoadProjectSheet(const fs::path& source, const char* sheet) { return project_sheets::Load(source, sheet); }

const GameBuild* BuildWithEnglishSlot() {
    for (const GameBuild& build : SupportedBuilds()) {
        if (build.translation && build.translation->source.empty()) return &build;
    }
    return nullptr;
}

const GameBuild* BuildWithSource(std::string_view source) {
    for (const GameBuild& build : SupportedBuilds()) {
        if (build.translation && build.translation->source == source) return &build;
    }
    return nullptr;
}

std::string ProjectText(const ProjectSheet& sheet, size_t index) { return project_sheets::Text(sheet, index); }

// The name the game composes from the project's own Portuguese records: the word or prefix, then the item list's name.
std::string ComposedFromProject(const ItemRecord& record, const ProjectSheet& items, const ProjectSheet& effects, const ProjectSheet& system) {
    if (record.definition == kModifierNamedDefinition) {
        return CleanEffectText(ProjectText(effects, size_t(record.effects.front().id) + 1865));
    }
    const std::string name = TrimText(ProjectText(items, record.definition));
    if (!IsGear(record.definition)) return name;
    if (record.name_word != 0xFF) return TrimText(ProjectText(system, 1485 + size_t(record.name_word))) + " " + name;
    if (const std::optional<size_t> index = NamePrefixIndex(record)) {
        const std::string prefix = TrimText(ProjectText(effects, *index));
        if (!prefix.empty() && prefix.find_first_of("%@") == std::string::npos) return prefix + " " + name;
    }
    return name;
}

void TestPortugueseNames(const fs::path& source) {
    if (!project_sheets::Present(source, {"ItemList", "CustomItem", "SystemMsg"})) {
        std::printf("localization sheets not present, skipping the Portuguese name check\n");
        return;
    }
    const GameBuild* usa = BuildWithEnglishSlot();
    CHECK(usa != nullptr);
    if (!usa) return;
    const TranslationProfile& profile = *usa->translation;
    const ProjectSheet items = LoadProjectSheet(source, "ItemList"), effects = LoadProjectSheet(source, "CustomItem"),
                       system = LoadProjectSheet(source, "SystemMsg");
    const std::vector<std::string> pt_items = TranslatedSheetRecords(kPortPortugueseBrazil, profile, "ItemList", items.english);
    const std::vector<std::string> pt_effects = TranslatedSheetRecords(kPortPortugueseBrazil, profile, "CustomItem", effects.english);
    const std::vector<std::string> pt_system = TranslatedSheetRecords(kPortPortugueseBrazil, profile, "SystemMsg", system.english);

    // Every record of the project's Portuguese sheets is what the shared lookup serves, for the three sheets of a name.
    size_t compared = 0;
    for (const auto& [index, text] : items.portuguese) {
        CHECK(index < pt_items.size() && pt_items[index] == text);
        ++compared;
    }
    for (const auto& [index, text] : effects.portuguese) {
        CHECK(index < pt_effects.size() && pt_effects[index] == text);
        ++compared;
    }
    for (const auto& [index, text] : system.portuguese) {
        if (index >= 109 && index <= 225) continue; // the tag keys are never translated
        CHECK(index < pt_system.size() && pt_system[index] == text);
        ++compared;
    }
    CHECK(compared > 5000);
    CHECK(pt_items.size() == items.english.size() && pt_effects.size() == effects.english.size() && pt_system.size() == system.english.size());
    CHECK(pt_items[283] == "Martelo de Rocha" && pt_items[349] == "Broquel" && pt_items[445] == "Botas de Couro");
    CHECK(pt_items[112] == "%s"); // the Items entry's placeholder has no translation

    ItemCatalog portuguese(pt_items, pt_effects, NameWords(pt_system), true);
    const ItemCatalog english(items.english, effects.english, NameWords(system.english));
    portuguese.SetEnglish(english);
    CHECK(english.English() == nullptr && portuguese.English() != nullptr);
    CHECK(portuguese.Word(0) == "Abençoado" && portuguese.Word(9) == "Deformado" && english.Word(0) == "Gifted");
    CHECK(portuguese.ItemName(283) == "Martelo de Rocha" && english.ItemName(283) == "Rock Hammer");

    // The composed names are the game's own records, in the order the game puts them.
    const SaveModel model = SaveModel::FromBytes(MakeInventorySave());
    size_t named = 0;
    for (const ItemRecord& record : ReadStorage(model.bytes(), 0)) {
        if (record.state != ItemState::kValid || record.definition >= items.english.size() || record.definition == 600) continue;
        CHECK(ItemDisplayName(record, portuguese) == ComposedFromProject(record, items, effects, system));
        ++named;
    }
    for (const ItemRecord& record : ReadEquipment(model.bytes(), 0, 0)) {
        if (record.state != ItemState::kValid || record.definition >= items.english.size()) continue;
        CHECK(ItemDisplayName(record, portuguese) == ComposedFromProject(record, items, effects, system));
        ++named;
    }
    CHECK(named >= 8);
    const auto name = [&](const ItemRef& ref) { return ItemDisplayName(ReadItem(model.bytes(), ref), portuguese); };
    CHECK(name({0, -1, 0, 2}) == "Broquel");
    CHECK(name({0, -1, 0, 0}) == "do Demônio Martelo de Rocha");
    CHECK(name({0, -1, 0, 3}) == "Pergaminho do Renascimento");
    CHECK(ItemDisplayName(ReadItem(model.bytes(), {0, -1, 0, 0}), english) == "Demon's Rock Hammer");

    // Modifier names are the translated sheet's text without its value.
    CHECK(portuguese.Effect(0x12).known && portuguese.Effect(0x12).name == "Dano mágico" && portuguese.Effect(0x12).percent);
    CHECK(portuguese.Effect(0x2A).name == "Chance de derrubar");
    CHECK(portuguese.Effect(0x00).name == "Chance de ... de envenenar inimigos" && portuguese.Effect(0x00).percent);
    CHECK(english.Effect(0x00).name == "Poison enemy chance");
    CHECK(ModifierName("Chance de ?% de envenenar inimigos") == "Chance de de envenenar inimigos");
    CHECK(ModifierName("Chance de ?% de envenenar inimigos", true) == "Chance de ... de envenenar inimigos");
    CHECK(ModifierName("Ouro obtido +?%, Dano causado -?%", true) == "Ouro obtido, Dano causado");
    CHECK(ModifierName("?% do Dano recebido convertido em MP", true) == "do Dano recebido convertido em MP");
    CHECK(ModifierName("Dano mágico +?%", true) == "Dano mágico" && ModifierName("Poison enemy ?% chance", true) == "Poison enemy ... chance");
    CHECK(ModifierName("Poison enemy ?% chance") == "Poison enemy chance");

    // Presentation only: the same records, edits and bytes whichever language the names are in.
    const Bytes before(model.bytes().begin(), model.bytes().end());
    for (const ItemRecord& record : ReadStorage(model.bytes(), 0)) {
        const ItemDetail shown = DescribeItem(model, model, record.ref, portuguese);
        const ItemDetail plain = DescribeItem(model, model, record.ref, english);
        CHECK(shown.present == plain.present);
        if (!shown.present) continue;
        CHECK(shown.record.definition == plain.record.definition && shown.record.durability == plain.record.durability &&
              shown.record.durability_max == plain.record.durability_max && shown.record.grade == plain.record.grade);
        CHECK(shown.record.effects.size() == plain.record.effects.size() && shown.modifiers.size() == plain.modifiers.size());
        for (size_t i = 0; i < shown.modifiers.size() && i < plain.modifiers.size(); ++i) {
            CHECK(shown.modifiers[i].stored == plain.modifiers[i].stored && shown.modifiers[i].shown == plain.modifiers[i].shown &&
                  shown.modifiers[i].slot == plain.modifiers[i].slot && shown.modifiers[i].percent == plain.modifiers[i].percent);
        }
        CHECK(shown.category == plain.category && shown.level == plain.level && shown.durability == plain.durability);
    }
    const InventoryList shown_list = BuildInventoryList(model, model, 0, 0, portuguese);
    const InventoryList plain_list = BuildInventoryList(model, model, 0, 0, english);
    CHECK(shown_list.rows.size() == plain_list.rows.size() && !shown_list.rows.empty());
    std::set<std::tuple<int, int, int, int>> shown_refs, plain_refs;
    for (const InventoryRow& row : shown_list.rows) shown_refs.insert({row.ref.slot, row.ref.character, row.ref.set, row.ref.index});
    for (const InventoryRow& row : plain_list.rows) plain_refs.insert({row.ref.slot, row.ref.character, row.ref.set, row.ref.index});
    CHECK(shown_refs == plain_refs);
    CHECK(Bytes(model.bytes().begin(), model.bytes().end()) == before);

    // The appearance colours: the game's Portuguese names, for every class and preset.
    const char* const dwarf[] = {"Branco Marfim", "Vermelho Carmesim", "Ébano Escuro", "Marrom Cedro", "Amarelo Âmbar"};
    for (const CharacterClass klass : kCharacterClasses) {
        for (int color = 0; color < kNativeColorCount; ++color) {
            const int id = NativeColorMessage(klass, color);
            CHECK(id >= 871 && id <= 900);
            if (id < 871 || id > 900) continue;
            const std::string shown_name = TrimText(pt_system[size_t(id)]);
            CHECK(shown_name == TrimText(ProjectText(system, size_t(id))));
            CHECK(!shown_name.empty() && shown_name != TrimText(system.english[size_t(id)]));
            if (klass == CharacterClass::kDwarf) CHECK(shown_name == dwarf[color]);
        }
    }
}

void TestPortugueseFallbacks(const fs::path& source) {
    if (!project_sheets::Present(source, {"ItemList", "CustomItem", "SystemMsg"})) {
        std::printf("localization sheets not present, skipping the Portuguese fallback check\n");
        return;
    }
    const GameBuild* usa = BuildWithEnglishSlot();
    const GameBuild* japan = BuildWithSource("JP");
    CHECK(usa != nullptr && japan != nullptr);
    if (!usa || !japan) return;
    const ProjectSheet items = LoadProjectSheet(source, "ItemList");
    const std::string pt(kPortPortugueseBrazil);

    // A sheet, a language or a record the translation cannot serve stays as installed.
    CHECK(TranslatedSheetRecords(pt, *usa->translation, "NoSuchSheet", items.english) == items.english);
    CHECK(TranslatedSheetRecords("ko", *usa->translation, "ItemList", items.english) == items.english);
    CHECK(TranslatedSheetRecords("ru", *usa->translation, "ItemList", items.english) == items.english);
    CHECK(TranslatedSheetRecords("", *usa->translation, "ItemList", items.english) == items.english);
    CHECK(TranslatedSheetRecords(pt, *usa->translation, "ItemList", {}).empty());
    std::vector<std::string> edited = items.english;
    edited[283] += " ";
    const std::vector<std::string> kept = TranslatedSheetRecords(pt, *usa->translation, "ItemList", edited);
    CHECK(kept[283] == edited[283] && kept[284] == "Martelo Marcial");
    const std::vector<std::string> other = TranslatedSheetRecords(pt, *japan->translation, "ItemList", items.english); // another slot's hashes
    CHECK(other[283] == items.english[283] && other[349] == items.english[349]);
    CHECK(TranslatedSheetRecords(std::string(kPortEnglish), *usa->translation, "ItemList", items.english) == items.english);

    // Names never come out blank: a catalog without the translated sheets falls back to the base name or a stand-in.
    const SaveModel model = SaveModel::FromBytes(MakeInventorySave());
    const ItemRecord hammer = ReadItem(model.bytes(), {0, -1, 0, 0});
    CHECK(ItemDisplayName(hammer, ItemCatalog(kept, {}, {}, true)) == "Rock Hammer");
    CHECK(ItemDisplayName(hammer, ItemCatalog({}, {}, {}, true)) == "Unknown item 0x11b");
    CHECK(ItemCatalog().English() == nullptr && ItemCatalog().Effect(0x12).name.empty());
}

bool CleanText(const std::string& text) {
    return text.find("\xEF\xBF\xBD") == std::string::npos && IsValidUtf8(text);
}

// The catalog the Editor shows is read through the launcher language (English or Portuguese), never the game language:
// the Asia/Hong Kong build's Korean sheets are not what an Editor in either language reads.
void TestLauncherLanguageCatalogs(const fs::path& source) {
    const fs::path app0 = source / "game" / "prepared" / "CUSA10209-1.00";
    const GameBuild* asia = nullptr;
    for (const GameBuild& build : SupportedBuilds()) {
        if (std::string_view(build.title_id) == "CUSA10209" && std::string_view(build.version) == "1.00") asia = &build;
    }
    std::error_code ec;
    if (!asia || !project_sheets::Present(source, {"ItemList", "CustomItem", "SystemMsg"}) ||
        !fs::is_regular_file(app0 / "DRACRO_CK.CPK", ec)) {
        std::printf("Asia/Hong Kong folder or localization sheets not present, skipping the launcher language catalogs\n");
        return;
    }
    const ProjectSheet items = LoadProjectSheet(source, "ItemList"), effects = LoadProjectSheet(source, "CustomItem"),
                       system = LoadProjectSheet(source, "SystemMsg");
    CharacterArtSources sources = ArtSourcesOf(app0, "DRACRO_CK.CPK");
    sources.translation = asia->translation;
    CharacterArt art(sources);

    const TextLanguage& shown_english = LauncherDisplayLanguage(asia, kEnglishKey);
    const TextLanguage& shown_portuguese = LauncherDisplayLanguage(asia, kPortugueseBrazilKey);
    const std::string english_suffix(shown_english.suffix), english_translation(shown_english.translation);
    const ItemCatalog english =
        LoadItemCatalog(art, english_suffix, english_translation);
    const ItemCatalog portuguese = LoadItemCatalog(art, std::string(shown_portuguese.suffix), std::string(shown_portuguese.translation));

    // The game language's own sheets are Korean: that is what the Editor read before.
    const std::vector<std::string>& korean = art.Sheet("ItemList", "KR");
    CHECK(korean.size() > 445 && korean[445] != "Leather Boots" && korean[445].find_first_of("\xE1\xEA\xEB\xEC\xED") != std::string::npos);

    // English: the project's own English text for every gear name and every modifier the game words.
    CHECK(english.loaded() && english.English() == nullptr);
    CHECK(english.ItemName(283) == "Rock Hammer" && english.ItemName(445) == "Leather Boots" && english.Word(0) == "Gifted");
    size_t names = 0, wordings = 0;
    for (uint32_t id = kFirstWeapon; id < kEndGear; ++id) {
        const std::string name = english.ItemName(id);
        CHECK(CleanText(name) && name == TrimText(items.english[id]));
        for (const char c : name) CHECK(static_cast<unsigned char>(c) < 0x80);
        ++names;
    }
    for (uint16_t id = 0; id < 0x100; ++id) {
        const EffectText effect = english.Effect(id);
        if (!effect.known) continue;
        CHECK(CleanText(effect.name) && CleanText(effect.text));
        for (const char c : effect.text) CHECK(static_cast<unsigned char>(c) < 0x80);
        ++wordings;
    }
    CHECK(names == kEndGear - kFirstWeapon && wordings > 50);
    CHECK(english.Effect(0x12).name == "Magic damage" && english.Effect(0x2A).text == "Knockdown chance +?%");

    // Portuguese: the project's Portuguese text, with the English names behind it for a search.
    CHECK(portuguese.English() != nullptr && portuguese.English()->ItemName(283) == "Rock Hammer");
    CHECK(portuguese.ItemName(283) == "Martelo de Rocha" && portuguese.Effect(0x12).name == "Dano mágico");
    for (const auto& [index, text] : items.portuguese) {
        if (index >= kFirstWeapon && index < kEndGear) CHECK(portuguese.ItemName(uint32_t(index)) == TrimText(text));
    }
    for (uint32_t id = kFirstWeapon; id < kEndGear; ++id) CHECK(CleanText(portuguese.ItemName(id)) && !portuguese.ItemName(id).empty());

    // Every record either catalog is made from is valid UTF-8 and carries no replacement character.
    for (const char* sheet : {"ItemList", "CustomItem", "SystemMsg"}) {
        for (const std::string& translation : {std::string(), english_translation, std::string(shown_portuguese.translation)}) {
            for (const std::string& record : art.TranslatedSheet(sheet, english_suffix, translation)) CHECK(CleanText(record));
        }
    }
}

// A record that is not UTF-8 never reaches the Editor: the US sheet's text stands in, or nothing.
void TestMalformedSheetRecords(const fs::path& base) {
    const fs::path folder = base / "malformed_sheets";
    fs::create_directories(folder);
    const auto sheet = [](std::vector<std::string> records) {
        std::string out("FMSB\0\0\0\0", 8);
        cpk_build::Le32(out, 0x20);
        out += std::string(8, '\0');
        cpk_build::Le32(out, uint32_t(records.size()));
        out += '\x03';
        out += std::string(7 + records.size() * 8, '\0');
        for (const std::string& record : records) out += record + '\0';
        out.resize((out.size() + 15) / 16 * 16, '\0');
        const uint32_t size = uint32_t(out.size() - 0x20);
        for (int i = 0; i < 4; ++i) out[size_t(4 + i)] = char(size >> (8 * i));
        return out + "FEOC" + std::string(12, '\0');
    };
    const std::vector<std::string> english = {"Sword", "Axe", "Bow", "Shield"};
    const std::vector<std::string> broken = {"Espada", "Hache\xFF\xFE", "\xC3(", "Bouclier"};
    for (const bool with_english : {true, false}) {
        std::vector<std::pair<std::string, std::string>> files = {{"msgsheet/ItemList_FR.fms", sheet(broken)}};
        if (with_english) files.push_back({"msgsheet/ItemList_US.fms", sheet(english)});
        const std::string archive = cpk_build::BuildCpkWith(files);
        const fs::path path = folder / (with_english ? "with_english.cpk" : "without_english.cpk");
        std::ofstream(path, std::ios::binary).write(archive.data(), std::streamsize(archive.size()));
        CharacterArtSources sources;
        sources.archives = {path};
        CharacterArt art(sources);
        const std::vector<std::string>& records = art.Sheet("ItemList", "FR");
        CHECK(records.size() == 4);
        if (records.size() != 4) continue;
        CHECK(records[0] == "Espada" && records[3] == "Bouclier");
        CHECK(records[1] == (with_english ? "Axe" : "") && records[2] == (with_english ? "Bow" : ""));
        for (const std::string& record : records) CHECK(CleanText(record));
        const ItemCatalog catalog = LoadItemCatalog(art, "FR", "");
        CHECK(CleanText(catalog.ItemName(1)) && CleanText(catalog.ItemName(2)));
    }
}

void TestPortugueseSearchAndSort() {
    CHECK(NameMatchesSearch("Poção de Cura", "pocao") && NameMatchesSearch("Poção de Cura", "POÇÃO") && NameMatchesSearch("Poção", "Poção"));
    CHECK(NameMatchesSearch("Dano mágico", "magico") && NameMatchesSearch("Açúcar", "acucar") && !NameMatchesSearch("Poção", "pocoes"));
    CHECK(NameMatchesSearch("Rock Hammer", "ROCK") && !NameMatchesSearch("Rock Hammer", "rocha"));

    // Names sort by what the user reads: an accented letter sorts as its base letter.
    std::vector<std::string> names = FixtureItems();
    names[279] = "Zebra";
    names[283] = "Ábaco";
    names[284] = "Çedilha";
    names[349] = "Escudo";
    const SaveModel model = SaveModel::FromBytes(MakeInventorySave());
    InventoryQuery query;
    query.group = InventoryGroup::kWeaponsShields;
    query.sort = InventorySort::kName;
    const InventoryList sorted = BuildInventoryList(model, model, 0, 0, ItemCatalog(names, {}), query);
    std::vector<std::string> order;
    for (const InventoryRow& row : sorted.rows) order.push_back(row.name);
    CHECK(order.size() >= 4);
    const auto at = [&](const char* name) { return std::find(order.begin(), order.end(), name) - order.begin(); };
    CHECK(at("Ábaco") < at("Çedilha") && at("Çedilha") < at("Escudo") && at("Escudo") < at("Zebra"));

    // A search matches the displayed name, and the English name too while another language is shown.
    ItemCatalog shown(names, {});
    const ItemCatalog english(FixtureItems(), {});
    query.search = "abaco";
    CHECK(BuildInventoryList(model, model, 0, 0, shown, query).rows.size() == 1);
    query.search = "rock hammer";
    CHECK(BuildInventoryList(model, model, 0, 0, shown, query).rows.empty());
    shown.SetEnglish(english);
    const InventoryList by_english = BuildInventoryList(model, model, 0, 0, shown, query);
    CHECK(by_english.rows.size() == 1 && by_english.rows.front().name == "Ábaco");
    query.search = "ABACO";
    CHECK(BuildInventoryList(model, model, 0, 0, shown, query).rows.size() == 1);
    query.search = "rock hammer";
    CHECK(SearchKeptAfterSave(model, {0, -1, 0, 0}, shown, query.search) == query.search);
    CHECK(SearchKeptAfterSave(model, {0, -1, 0, 10}, shown, query.search).empty());
    CHECK(BuildInventoryList(model, model, 0, 0, english, query).rows.size() == 1); // English alone is unchanged
}

// ---- Edits ----

void TestDurabilityMath() {
    CHECK(ShownDurability(189587, 100) == 1895 && ShownDurability(99, 100) == 0 && ShownDurability(7, 1) == 7);
    CHECK(StoredDurability(1895, 189587, 189587, 100) == 189587u);          // unchanged keeps the stored value
    CHECK(StoredDurability(1000, 189587, 189587, 100) == 100000u);
    CHECK(StoredDurability(1895, 100000, 189587, 100) == 189587u);          // the shown maximum is the stored maximum
    CHECK(StoredDurability(0, 500, 189587, 100) == 0u);
    CHECK(!StoredDurability(1896, 100000, 189587, 100) && !StoredDurability(UINT32_MAX, 1, 189587, 100));
    CHECK(!StoredDurability(1, 5, 4, 100) && !StoredDurability(1, 5, 5, 0)); // a current above its maximum is malformed
    CHECK(StoredDurability(3, 4, 5, 1) == 3u && StoredDurability(5, 4, 5, 1) == 5u && !StoredDurability(6, 4, 5, 1));
}

void TestEditDurability() {
    const Bytes original = MakeInventorySave();
    SaveModel model = SaveModel::FromBytes(original);
    const ItemRef hammer{0, -1, 0, 0};

    CHECK(SetItemDurability(model, hammer, 1000));
    const std::vector<size_t> changed = Diff(original, Bytes(model.bytes().begin(), model.bytes().end()));
    CHECK(changed.size() == 3 || changed.size() == 4); // only the stored durability bytes
    for (size_t at : changed) CHECK(at >= At(hammer) + kItemDurability && at < At(hammer) + kItemDurability + 4);
    CHECK(ReadItem(model.bytes(), hammer).durability == 100000 && ReadItem(model.bytes(), hammer).durability_max == 189587);

    // A no-op edit changes nothing, a rejected edit changes nothing.
    const Bytes before(model.bytes().begin(), model.bytes().end());
    CHECK(SetItemDurability(model, hammer, 1000));
    CHECK(!SetItemDurability(model, hammer, 1896) && !SetItemDurability(model, hammer, UINT32_MAX));
    CHECK(std::equal(before.begin(), before.end(), model.bytes().begin()));

    // Refilling to the shown maximum restores the exact stored maximum.
    CHECK(SetItemDurability(model, hammer, 1895) && ReadItem(model.bytes(), hammer).durability == 189587);

    // The equipped item is edited in place: no copy, and the storage hammer is another record.
    const ItemRef equipped{0, 0, 0, 0};
    CHECK(SetItemDurability(model, equipped, 2000));
    CHECK(ReadItem(model.bytes(), equipped).durability == 200000 && ReadItem(model.bytes(), equipped).durability_max == 286263);
    CHECK(ReadItem(model.bytes(), hammer).durability == 189587);

    // Rune stones are not durability items: their fields are not shown or changed. Neither are empty, unknown and malformed ones.
    const ItemRef charm{0, -1, 0, 3};
    CHECK(!SetItemDurability(model, charm, 2) && ReadItem(model.bytes(), charm).durability == 4);
    CHECK(!SetItemDurability(model, charm, 6));
    CHECK(!SetItemDurability(model, {0, -1, 0, 30}, 1));
    CHECK(!SetItemDurability(model, {0, -1, 0, 7}, 1)); // definition 600: a Heaven reward the editor does not know
    CHECK(!SetItemDurability(model, {0, -1, 0, 9}, 1));
    CHECK(!SetItemDurability(model, {0, -1, 0, 500}, 1) && !SetItemDurability(model, {3, -1, 0, 0}, 1));
}

void TestEditLevel() {
    const Bytes original = MakeInventorySave();
    SaveModel model = SaveModel::FromBytes(original);
    const ItemRef hammer{0, -1, 0, 0};
    CHECK(ReadItem(model.bytes(), hammer).required_level() == 94);
    CHECK(SetItemRequiredLevel(model, hammer, 1) && ReadItem(model.bytes(), hammer).level_stored == 0);
    CHECK(SetItemRequiredLevel(model, hammer, 100) && ReadItem(model.bytes(), hammer).level_stored == 99);
    const Bytes before(model.bytes().begin(), model.bytes().end());
    CHECK(!SetItemRequiredLevel(model, hammer, 0) && !SetItemRequiredLevel(model, hammer, 101) && !SetItemRequiredLevel(model, hammer, -5));
    CHECK(!SetItemRequiredLevel(model, {0, -1, 0, 3}, 5)); // no required level on the modifier-named item
    CHECK(!SetItemRequiredLevel(model, {0, -1, 0, 30}, 5) && !SetItemRequiredLevel(model, {0, -1, 0, 7}, 5));
    CHECK(std::equal(before.begin(), before.end(), model.bytes().begin()));
    SaveModel fresh = SaveModel::FromBytes(original);
    CHECK(SetItemRequiredLevel(fresh, hammer, 99));
    std::vector<size_t> changed = Diff(original, Bytes(fresh.bytes().begin(), fresh.bytes().end()));
    CHECK(changed.size() == 1 && changed[0] == At(hammer) + kItemLevel); // a higher cap leaves the attack alone
    // A lower level changes the level alone, and Apply accepts it with the attack as it was.
    SaveModel lowered = SaveModel::FromBytes(original);
    CHECK(SetItemRequiredLevel(lowered, hammer, 50));
    changed = Diff(original, Bytes(lowered.bytes().begin(), lowered.bytes().end()));
    CHECK(changed == (std::vector<size_t>{At(hammer) + kItemLevel}));
    std::string refusal;
    CHECK(ValidateItemChanges(original, Bytes(lowered.bytes().begin(), lowered.bytes().end()), refusal));
}

void TestIsolation() {
    const Bytes original = MakeInventorySave();
    SaveModel model = SaveModel::FromBytes(original);
    const Bytes alex = Bytes(model.bytes().begin() + long(At({0, 1, 0, 0})), model.bytes().begin() + long(At({0, 1, 8, 9}) + kItemRecordSize));
    CHECK(SetItemDurability(model, {0, 0, 0, 0}, 1) && SetItemDurability(model, {0, 0, 3, 2}, 1));
    const Bytes alex_after(model.bytes().begin() + long(At({0, 1, 0, 0})), model.bytes().begin() + long(At({0, 1, 8, 9}) + kItemRecordSize));
    CHECK(alex == alex_after);                                          // George's edits never reach Alex
    CHECK(ReadEquipment(model.bytes(), 1, 0).size() == 1 && ReadEquipment(model.bytes(), 1, 0)[0].durability == 150000); // nor DATA 2

    // The storage is shared by the DATA slot: the edit is seen from either character, and not from another slot.
    CHECK(SetItemDurability(model, {0, -1, 0, 0}, 500));
    CHECK(ReadStorage(model.bytes(), 0)[0].durability == 50000);
    CHECK(ReadStorage(model.bytes(), 1)[0].durability == 200985);
    const InventoryList george = BuildInventoryList(model, SaveModel::FromBytes(original), 0, 0, FixtureCatalog());
    const InventoryList alex_list = BuildInventoryList(model, SaveModel::FromBytes(original), 0, 1, FixtureCatalog());
    const auto shared = [](const InventoryList& list) {
        return std::count_if(list.rows.begin(), list.rows.end(), [](const InventoryRow& row) { return row.ref.storage() && row.changed; });
    };
    CHECK(shared(george) == 1 && shared(alex_list) == 1);
    CHECK(std::count_if(george.rows.begin(), george.rows.end(), [](const InventoryRow& row) { return !row.ref.storage() && row.changed; }) == 2);
    CHECK(std::count_if(alex_list.rows.begin(), alex_list.rows.end(), [](const InventoryRow& row) { return !row.ref.storage() && row.changed; }) == 0);

    // Inventory edits leave the character fields and every other byte alone.
    const std::vector<size_t> changed = Diff(original, Bytes(model.bytes().begin(), model.bytes().end()));
    for (size_t at : changed) CHECK(at >= At({0, -1, 0, 0}) && !(at >= At({0, 1, 0, 0}) && at < At({0, 1, 8, 9}) + kItemRecordSize));
    const SaveModel pristine = SaveModel::FromBytes(original);
    for (int index = 0; index < 2; ++index) {
        const CharacterView& a = *model.Character(0, index);
        const CharacterView& b = *pristine.Character(0, index);
        CHECK(a.name == b.name && a.level_stored == b.level_stored && a.hp_current == b.hp_current && a.hp_max == b.hp_max &&
              a.skill_points == b.skill_points && a.color == b.color && a.klass == b.klass);
    }
}

void TestDetail() {
    const SaveModel model = SaveModel::FromBytes(MakeInventorySave());
    const ItemCatalog catalog = FixtureCatalog();
    const ItemDetail hammer = DescribeItem(model, model, {0, -1, 0, 0}, catalog);
    CHECK(hammer.present && hammer.name == "Demon's Rock Hammer" && hammer.category == ItemCategory::kWeapon);
    CHECK(hammer.durability == 1895 && hammer.durability_max == 1895 && hammer.durability_range.min == 0 && hammer.durability_range.max == 1895);
    CHECK(hammer.level == 94 && hammer.level_editable && hammer.durability_editable && !hammer.changed);
    CHECK(hammer.modifiers.size() == 3 && hammer.modifiers[0].name == "Magic damage" && hammer.modifiers[0].stored == 94 && hammer.modifiers[0].slot == 0);
    CHECK(hammer.modifiers[1].name == "Poison enemy chance" && hammer.modifiers[1].stored == 89);

    const ItemDetail charm = DescribeItem(model, model, {0, -1, 0, 3}, catalog);
    CHECK(charm.name == "Rebirth Scroll" && charm.category == ItemCategory::kItem && !charm.level_editable && !charm.durability_editable);
    const ItemDetail unknown = DescribeItem(model, model, {0, -1, 0, 9}, catalog);
    CHECK(unknown.present && unknown.name == "Unknown item 0xbb8" && !unknown.durability_editable && !unknown.level_editable);
    CHECK(unknown.modifiers.size() == 1 && unknown.modifiers[0].name == "Unknown modifier 0x7ff" && !unknown.modifiers[0].known);
    CHECK(!DescribeItem(model, model, {0, -1, 0, 30}, catalog).present && !DescribeItem(model, model, {0, -1, 0, 900}, catalog).present);

    Bytes bad = MakeInventorySave();
    PutFloat(bad, At({0, -1, 0, 0}) + 4, std::numeric_limits<float>::quiet_NaN());
    PutFloat(bad, At({0, -1, 0, 0}) + 12, 1e30f);
    const SaveModel nan_model = SaveModel::FromBytes(bad);
    const ItemDetail odd = DescribeItem(nan_model, nan_model, {0, -1, 0, 0}, catalog);
    CHECK(odd.modifiers.size() == 3 && odd.modifiers[0].stored == 0 && odd.modifiers[1].stored == 1000000);
}

// ---- Safety ----

void TestMalformed() {
    const Bytes good = MakeInventorySave();
    const ItemRef ref{0, -1, 0, 0};
    const size_t base = At(ref);
    const auto state = [&](const Bytes& bytes) { return ReadItem(bytes, ref).state; };
    CHECK(state(good) == ItemState::kValid);

    // Every part of a record is checked against the others.
    Bytes bytes = good;
    PutU32(bytes, base + kItemFlags, (283u) | 2u << 25 | 1u << 29); // modifier count 2 for 3 modifiers
    CHECK(state(bytes) == ItemState::kMalformed && ReadItem(bytes, ref).problem.find("count") != std::string::npos);
    bytes = good;
    PutU32(bytes, base + kItemFlags, (283u) | 9u << 25 | 1u << 29);
    CHECK(state(bytes) == ItemState::kMalformed);
    bytes = good;
    PutU16(bytes, base + kItemEffectStride, kNoEffect); // an empty slot before filled ones
    CHECK(state(bytes) == ItemState::kMalformed);
    bytes = good;
    PutU32(bytes, base + kItemDurability, 189588);
    CHECK(state(bytes) == ItemState::kMalformed && ReadItem(bytes, ref).problem.find("maximum") != std::string::npos);
    bytes = good;
    PutU32(bytes, base + kItemDurabilityMax, 0);
    CHECK(state(bytes) == ItemState::kMalformed);
    // Definition 0 is an empty slot whatever follows.
    bytes = good;
    PutU32(bytes, base + kItemFlags, 3u << 25 | 1u << 29);
    CHECK(state(bytes) == ItemState::kEmpty);

    // Truncated buffers and references outside the structure never read out of bounds.
    CHECK(ReadItem(std::span<const uint8_t>(good.data(), base + kItemRecordSize - 1), ref).state == ItemState::kMalformed);
    CHECK(ReadItem(std::span<const uint8_t>(good.data(), 10), {0, 0, 8, 9}).state == ItemState::kMalformed);
    CHECK(ReadItem(good, {0, -1, 0, 500}).state == ItemState::kMalformed && ReadItem(good, {5, 0, 0, 0}).state == ItemState::kMalformed);
    for (const ItemRecord& cut : ReadStorage(std::span<const uint8_t>(good.data(), 100), 0)) CHECK(cut.state == ItemState::kMalformed);

    // A malformed item is listed, flagged and never editable.
    Bytes broken = good;
    PutU32(broken, base + kItemFlags, 283u | 1u << 25 | 1u << 29);
    Seal(broken);
    SaveModel broken_model = SaveModel::FromBytes(broken);
    const InventoryList list = BuildInventoryList(broken_model, broken_model, 0, 0, FixtureCatalog());
    const auto row = std::find_if(list.rows.begin(), list.rows.end(), [&](const InventoryRow& r) { return r.ref == ref; });
    CHECK(row != list.rows.end() && row->malformed && !row->editable);
    CHECK(!SetItemDurability(broken_model, ref, 5) && !SetItemRequiredLevel(broken_model, ref, 5));
    const ItemDetail detail = DescribeItem(broken_model, broken_model, ref, FixtureCatalog());
    CHECK(detail.present && !detail.durability_editable && !detail.level_editable);
}

void TestUnknownPreserved() {
    const Bytes original = MakeInventorySave();
    SaveModel model = SaveModel::FromBytes(original);
    // Unknown definition, unknown modifier id and the modifier extra field survive any permitted edit elsewhere.
    CHECK(SetItemDurability(model, {0, -1, 0, 0}, 3) && SetItemRequiredLevel(model, {0, -1, 0, 1}, 2));
    const ItemRecord unknown = ReadItem(model.bytes(), {0, -1, 0, 9});
    CHECK(unknown.definition == 3000 && unknown.effects.size() == 1 && unknown.effects[0].id == 0x7FF && unknown.effects[0].value == 12.0f);
    CHECK(std::equal(original.begin() + long(At({0, -1, 0, 9})), original.begin() + long(At({0, -1, 0, 9}) + kItemRecordSize),
                     model.bytes().begin() + long(At({0, -1, 0, 9}))));
    // Bytes this editor has no meaning for (flags, effect extras, tail bytes) are never rewritten by the setters.
    Bytes odd = original;
    odd[At({0, -1, 0, 0}) + 0x4C] = 0xAB;
    odd[At({0, -1, 0, 0}) + 0x50] = 0xCD;
    odd[At({0, -1, 0, 0}) + 0x53] = 0x03;
    PutU16(odd, At({0, -1, 0, 0}) + 2, 0x3F80);
    SaveModel odd_model = SaveModel::FromBytes(odd);
    CHECK(SetItemDurability(odd_model, {0, -1, 0, 0}, 100) && SetItemRequiredLevel(odd_model, {0, -1, 0, 0}, 7));
    const std::vector<size_t> diff = Diff(odd, Bytes(odd_model.bytes().begin(), odd_model.bytes().end()));
    for (size_t at : diff) {
        const size_t field = at - At({0, -1, 0, 0});
        CHECK(at >= At({0, -1, 0, 0}) && (field == kItemLevel || field == kItemAttackMin || field == kItemAttackMax ||
                                          (field >= kItemDurability && field < kItemDurability + 4)));
    }
    CHECK(odd_model.bytes()[At({0, -1, 0, 0}) + kItemAttackMin] == 0xCD); // the level change leaves it as it was
}

void TestValidateChanges() {
    const Bytes original = MakeInventorySave();
    std::string error;
    CHECK(ValidateItemChanges(original, original, error) && error.empty());

    // Every byte of a record: only the edited fields may differ.
    const ItemRef ref{0, 0, 0, 0};
    for (size_t field = 0; field < kItemRecordSize; ++field) {
        Bytes changed = original;
        changed[At(ref) + field] ^= 0x01;
        error.clear();
        const bool allowed = field == kItemLevel || field == kItemAttackMin || field == kItemAttackMax || (field >= kItemDurability && field < kItemDurabilityMax + 4) ||
                             (field < kItemEffectCapacity * kItemEffectStride && field % kItemEffectStride >= 4);
        const bool ok = ValidateItemChanges(original, changed, error);
        if (!allowed) CHECK(!ok && !error.empty());
        if (field == kItemDurability || field == kItemLevel) CHECK(ok);
        if (field == 4 + 8 * 2 || field == 4 + 8 * 5) CHECK(!ok); // the value bytes of empty modifier slots // an upper durability byte may leave its maximum
    }
    Bytes changed = original;
    PutU32(changed, At(ref) + kItemDurability, 100);
    CHECK(ValidateItemChanges(original, changed, error));
    PutU32(changed, At(ref) + kItemDurability, 286264);
    CHECK(!ValidateItemChanges(original, changed, error));
    changed = original;
    changed[At(ref) + kItemLevel] = 100; // displayed 101
    CHECK(!ValidateItemChanges(original, changed, error));
    changed[At(ref) + kItemLevel] = 99;
    CHECK(ValidateItemChanges(original, changed, error));

    // A change inside an empty slot, an item the editor does not edit, a malformed one, or outside every item record.
    changed = original;
    PutU32(changed, At({0, -1, 0, 30}) + kItemDurability, 5);
    CHECK(!ValidateItemChanges(original, changed, error));
    changed = original;
    PutU32(changed, At({0, -1, 0, 7}) + kItemDurability, 5);
    CHECK(!ValidateItemChanges(original, changed, error));
    Bytes malformed = original;
    PutU32(malformed, At(ref) + kItemFlags, 279u | 7u << 25 | 3u << 29);
    Bytes malformed_edit = malformed;
    PutU32(malformed_edit, At(ref) + kItemDurability, 1);
    CHECK(!ValidateItemChanges(malformed, malformed_edit, error));
    changed = original;
    changed[kDataSlotStride - 1] ^= 1; // a byte no item record covers
    CHECK(ValidateItemChanges(original, changed, error));
    CHECK(!ValidateItemChanges(Bytes(10), Bytes(10), error) && !ValidateItemChanges(original, Bytes(original.begin(), original.end() - 1), error));
}

// Single-field corruptions of the structural assumptions fail validation instead of being read as other data.
void TestMutations() {
    const Bytes good = MakeInventorySave();
    const ItemRef ref{0, 0, 3, 2}; // two modifiers
    const size_t base = At(ref);
    CHECK(ReadItem(good, ref).state == ItemState::kValid);
    for (uint32_t count = 0; count < 16; ++count) {
        Bytes bytes = good;
        PutU32(bytes, base + kItemFlags, (390u) | count << 25 | 3u << 29);
        CHECK((ReadItem(bytes, ref).state == ItemState::kValid) == (count == 2));
    }
    // The modifier id field: the empty marker in the first slot turns the rest into a gap.
    Bytes bytes = good;
    PutU16(bytes, base, kNoEffect);
    CHECK(ReadItem(bytes, ref).state == ItemState::kMalformed);
    // The definition field: a changed definition is not an editable change.
    bytes = good;
    PutU32(bytes, base + kItemFlags, (391u) | 2u << 25 | 3u << 29);
    std::string error;
    CHECK(!ValidateItemChanges(good, bytes, error));
    // The base of the storage and of the equipment sets: reading from a wrong base never validates real data.
    CHECK(ReadStorage(good, 0)[0].definition == 283);
    CHECK(At({0, -1, 0, 0}) == 0xC0 && At({0, 0, 0, 0}) == 0xA648);
}

// ---- Transaction ----

void TestTransaction(const fs::path& base) {
    const fs::path user = base / "transaction";
    const Bytes original = MakeInventorySave();
    const fs::path file = WriteSave(user, original);
    SaveEdit edit = OpenEdit(user);
    CHECK(!edit.dirty() && edit.Apply() == SaveEdit::Result::kNothingToApply);
    CHECK(edit.working().Serialize() == original);                               // round trip without an edit
    CHECK(ReadBytes(file) == original && ListBackups(BackupFolder(user)).empty());

    // Discard Changes restores the loaded values, in the working copy and on disk.
    CHECK(SetItemDurability(edit.working(), {0, 0, 0, 0}, 5) && edit.dirty());
    CHECK(ReadItem(edit.persisted().bytes(), {0, 0, 0, 0}).durability == 219363);
    edit.Discard();
    CHECK(!edit.dirty() && ReadItem(edit.working().bytes(), {0, 0, 0, 0}).durability == 219363);
    CHECK(ReadBytes(file) == original);

    // Pending inventory and character edits apply together, through the existing backup, validated and read back.
    CHECK(SetItemDurability(edit.working(), {0, 0, 0, 0}, 5) && SetItemRequiredLevel(edit.working(), {0, -1, 0, 0}, 99));
    CHECK(edit.working().SetName(0, 1, "Alexa") && edit.working().SetLevel(0, 0, 50));
    CHECK(edit.Apply() == SaveEdit::Result::kApplied);
    const Bytes written = ReadBytes(file);
    CHECK(SaveModel::FromBytes(written).valid() && !edit.dirty());
    CHECK(ReadItem(written, {0, 0, 0, 0}).durability == 500 && ReadItem(written, {0, -1, 0, 0}).level_stored == 98);
    CHECK(SaveModel::FromBytes(written).Character(0, 1)->name == "Alexa" && SaveModel::FromBytes(written).Character(0, 0)->level_stored == 49);
    const std::vector<BackupInfo> backups = ListBackups(BackupFolder(user));
    CHECK(backups.size() == 1 && ReadBytes(backups[0].file) == original);
    CHECK(edit.persisted().bytes().size() == written.size() && std::equal(written.begin(), written.end(), edit.persisted().bytes().begin()));

    // Only the inventory changes: the character fields stay as they were.
    const fs::path user_two = base / "inventory_only";
    const fs::path file_two = WriteSave(user_two, original);
    SaveEdit inventory_only = OpenEdit(user_two);
    CHECK(SetItemDurability(inventory_only.working(), {0, 0, 3, 2}, 100));
    CHECK(inventory_only.Apply() == SaveEdit::Result::kApplied);
    const SaveModel after = SaveModel::FromFile(file_two);
    const SaveModel before = SaveModel::FromBytes(original);
    for (int slot = 0; slot < int(kDataSlotCount); ++slot) {
        for (int index = 0; index < int(kCharacterCount); ++index) {
            const CharacterView& a = *after.Character(slot, index);
            const CharacterView& b = *before.Character(slot, index);
            CHECK(a.present == b.present && a.name == b.name && a.level_stored == b.level_stored && a.hp_max == b.hp_max &&
                  a.hp_current == b.hp_current && a.color == b.color && a.skill_points == b.skill_points);
        }
    }
    const std::vector<size_t> diff = Diff(original, ReadBytes(file_two));
    for (size_t at : diff) CHECK(at < 4 || (at >= At({0, 0, 3, 2}) + kItemDurability && at < At({0, 0, 3, 2}) + kItemDurability + 4));
}

void TestFailures(const fs::path& base) {
    const Bytes original = MakeInventorySave();

    // A write that cannot complete leaves the original untouched and no partial file behind.
    const fs::path user = base / "write_failure";
    const fs::path file = WriteSave(user, original);
    SaveEdit edit = OpenEdit(user);
    fs::create_directories(fs::path(file.string() + ".tmp"));
    WriteBytes(fs::path(file.string() + ".tmp") / "keep", Bytes(1, 0)); // a non-empty folder where the temporary file goes
    CHECK(SetItemDurability(edit.working(), {0, 0, 0, 0}, 5));
    CHECK(edit.Apply() == SaveEdit::Result::kWriteFailed && !edit.detail().empty());
    CHECK(ReadBytes(file) == original && edit.dirty());
    fs::remove_all(fs::path(file.string() + ".tmp"));
    CHECK(edit.Apply() == SaveEdit::Result::kApplied);                              // the same edit applies once the path is free

    // No backup, no write.
    const fs::path user_backup = base / "backup_failure";
    const fs::path file_backup = WriteSave(user_backup, original);
    SaveEdit blocked = OpenEdit(user_backup);
    fs::create_directories(BackupFolder(user_backup).parent_path());
    {
        std::ofstream marker(BackupFolder(user_backup), std::ios::binary); // a file where the backup folder must go
        marker << "x";
    }
    CHECK(SetItemDurability(blocked.working(), {0, 0, 0, 0}, 5));
    CHECK(blocked.Apply() == SaveEdit::Result::kBackupFailed && ReadBytes(file_backup) == original);

    // A pending edit that is not valid never reaches the disk.
    const fs::path user_invalid = base / "invalid";
    const fs::path file_invalid = WriteSave(user_invalid, original);
    SaveEdit invalid = OpenEdit(user_invalid);
    invalid.working().bytes()[At({0, 0, 0, 0}) + 0x10] ^= 0x55;                      // a modifier byte: not an edit the editor offers
    CHECK(invalid.dirty() && invalid.Apply() == SaveEdit::Result::kInvalidInventory && !invalid.detail().empty());
    CHECK(ReadBytes(file_invalid) == original && ListBackups(BackupFolder(user_invalid)).empty());
    invalid.Discard();
    invalid.working().bytes()[At({0, 0, 0, 1}) + kItemDurability] = 0xFF;            // above the maximum
    invalid.working().bytes()[At({0, 0, 0, 1}) + kItemDurability + 3] = 0x7F;
    CHECK(invalid.Apply() == SaveEdit::Result::kInvalidInventory && ReadBytes(file_invalid) == original);

    // A save that changed on disk since it was loaded is not overwritten.
    const fs::path user_stale = base / "stale";
    const fs::path file_stale = WriteSave(user_stale, original);
    SaveEdit stale = OpenEdit(user_stale);
    Bytes replaced = original;
    PutU32(replaced, kDataSlotGoldOffset, 7);
    Seal(replaced);
    WriteBytes(file_stale, replaced);
    CHECK(SetItemDurability(stale.working(), {0, 0, 0, 0}, 5) && stale.Apply() == SaveEdit::Result::kStale);
    CHECK(ReadBytes(file_stale) == replaced);
}

void TestNavigationAndSwitching(const fs::path& base) {
    CharacterNav nav;
    CHECK(!nav.RequestInventory() && !nav.inventory());                       // nothing open
    nav.Open(1);
    CHECK(nav.editing() && !nav.inventory());
    CHECK(nav.RequestInventory() && nav.inventory() && nav.editing() && !nav.appearance());
    CHECK(!nav.RequestInventory() && !nav.RequestAppearance());               // one view at a time
    nav.BackToCharacter();
    CHECK(nav.editing() && !nav.inventory() && nav.open() == 1);
    CHECK(nav.RequestAppearance() && !nav.RequestInventory());                // while the appearance loads
    nav.FinishAppearance(true);
    nav.BackToCharacter();
    CHECK(nav.RequestInventory());
    nav.Back();
    CHECK(!nav.editing() && !nav.inventory());

    // Back to Character keeps what was edited; switching character shows that character's own items.
    const fs::path user = base / "navigation";
    const Bytes original = MakeInventorySave();
    WriteSave(user, original);
    SaveEdit edit = OpenEdit(user);
    CHECK(SetItemDurability(edit.working(), {0, 0, 0, 0}, 7));
    nav.Open(0);
    nav.RequestInventory();
    nav.BackToCharacter();
    CHECK(edit.dirty() && ReadItem(edit.working().bytes(), {0, 0, 0, 0}).durability == 700);
    const InventoryList george = BuildInventoryList(edit.working(), edit.persisted(), 0, 0, FixtureCatalog());
    const InventoryList alex = BuildInventoryList(edit.working(), edit.persisted(), 0, 1, FixtureCatalog());
    CHECK(george.rows.size() == 11 && alex.rows.size() == 8);
    CHECK(george.rows[0].changed && !alex.rows[0].changed && alex.rows[0].name == "Poleaxe");
    // Another DATA slot has its own storage and its own character.
    const InventoryList second = BuildInventoryList(edit.working(), edit.persisted(), 1, 0, FixtureCatalog());
    CHECK(second.rows.size() == 2 && second.rows.back().name == "Strike Axe" && !second.rows[0].changed);
    // The Discard that follows a slot or character switch restores every loaded value.
    edit.Discard();
    CHECK(!edit.dirty() && BuildInventoryList(edit.working(), edit.persisted(), 0, 0, FixtureCatalog()).rows[0].changed == false);
}

// ---- Game icons ----

void TestIconMapping() {
    CHECK(IconFamilyOf(187) == ItemIconFamily::kSword && IconFamilyOf(213) == ItemIconFamily::kSword);
    CHECK(IconFamilyOf(214) == ItemIconFamily::kAxe && IconFamilyOf(240) == ItemIconFamily::kAxe);
    CHECK(IconFamilyOf(241) == ItemIconFamily::kBow && IconFamilyOf(267) == ItemIconFamily::kBow);
    CHECK(IconFamilyOf(268) == ItemIconFamily::kHammer && IconFamilyOf(283) == ItemIconFamily::kHammer && IconFamilyOf(294) == ItemIconFamily::kHammer);
    CHECK(IconFamilyOf(295) == ItemIconFamily::kStaff && IconFamilyOf(321) == ItemIconFamily::kStaff);
    CHECK(IconFamilyOf(322) == ItemIconFamily::kRod && IconFamilyOf(348) == ItemIconFamily::kRod);
    CHECK(IconFamilyOf(349) == ItemIconFamily::kShield && IconFamilyOf(374) == ItemIconFamily::kShield);
    CHECK(IconFamilyOf(375) == ItemIconFamily::kAmulet && IconFamilyOf(488) == ItemIconFamily::kAmulet && IconFamilyOf(445) == ItemIconFamily::kAmulet);
    CHECK(IconFamilyOf(112) == ItemIconFamily::kGeneric);
    CHECK(IconFamilyOf(0) == ItemIconFamily::kGeneric && IconFamilyOf(600) == ItemIconFamily::kGeneric && IconFamilyOf(3000) == ItemIconFamily::kGeneric);
    CHECK(std::string(IconAnimation(ItemIconFamily::kAxe)) == "GENRE_POLEWEAPON_0" && std::string(IconAnimation(ItemIconFamily::kGeneric)) == "GENRE_ITEM_0");

    // A set with only some icons: known family, generic fallback, and nothing when neither is there (no crash).
    ItemIconSet set;
    CHECK(set.Find(283) == nullptr);
    SpriteImage image;
    image.width = 1;
    image.height = 1;
    image.rgba = {1, 2, 3, 255};
    set.images[ItemIconFamily::kHammer] = image;
    CHECK(set.Find(283) == &set.images[ItemIconFamily::kHammer] && set.Find(5) == nullptr);
    set.images[ItemIconFamily::kGeneric] = image;
    CHECK(set.Find(5) == &set.images[ItemIconFamily::kGeneric] && set.Find(283) == &set.images[ItemIconFamily::kHammer]);
}

void TestItemLook() {
    // Byte 0x10 of every entry of the executable's item table that names a lost-weapon resource (table 0xc19d20).
    const std::map<uint32_t, int> game = {
        {187, 4}, {188, 4}, {189, 4}, {190, 5}, {191, 5}, {192, 5}, {193, 6}, {194, 6}, {195, 6}, {196, 7}, {197, 7},
        {198, 7}, {199, 8}, {200, 8}, {201, 8}, {202, 9}, {203, 9}, {204, 9}, {205, 10}, {206, 10}, {207, 10}, {208, 11},
        {209, 11}, {210, 11}, {211, 6}, {212, 10}, {213, 11}, {214, 20}, {215, 20}, {216, 20}, {217, 21}, {218, 21},
        {219, 21}, {220, 22}, {221, 22}, {222, 22}, {223, 23}, {224, 23}, {225, 23}, {226, 24}, {227, 24}, {228, 24},
        {229, 25}, {230, 25}, {231, 25}, {232, 26}, {233, 26}, {234, 26}, {235, 27}, {236, 27}, {237, 27}, {238, 21},
        {239, 26}, {240, 27}, {268, 36}, {269, 36}, {270, 36}, {271, 37}, {272, 37}, {273, 37}, {274, 38}, {275, 38},
        {276, 38}, {277, 39}, {278, 39}, {279, 39}, {280, 40}, {281, 40}, {282, 40}, {283, 41}, {284, 41}, {285, 41},
        {286, 42}, {287, 42}, {288, 42}, {289, 43}, {290, 43}, {291, 43}, {292, 37}, {293, 41}, {294, 43}, {349, 12},
        {350, 12}, {351, 12}, {352, 13}, {353, 13}, {354, 13}, {355, 14}, {356, 14}, {357, 14}, {358, 15}, {359, 15},
        {360, 15}, {361, 16}, {362, 16}, {363, 16}, {364, 17}, {365, 17}, {366, 17}, {367, 18}, {368, 18}, {369, 18},
        {370, 19}, {371, 19}, {372, 19}, {373, 18}, {374, 19}, {491, 4}, {492, 5}, {493, 6}, {494, 7}, {495, 8},
        {496, 9}, {497, 10}, {498, 11}, {501, 20}, {502, 21}, {503, 22}, {504, 23}, {505, 24}, {506, 25}, {507, 26},
        {508, 27}, {521, 36}, {522, 37}, {523, 38}, {524, 39}, {525, 40}, {526, 41}, {527, 42}, {528, 43}, {553, 12},
        {554, 13}, {555, 14}, {556, 15}, {557, 16}, {558, 17}, {559, 18}, {560, 19}};
    CHECK(game.size() == 139);
    for (uint32_t definition = 0; definition < 800; ++definition) {
        const auto found = game.find(definition);
        CHECK(ItemLook(definition) == (found == game.end() ? std::nullopt : std::optional<int>(found->second)));
    }
    // Bows, staffs, rods and accessories have no lost-weapon object.
    for (const uint32_t definition : {241u, 267u, 295u, 321u, 322u, 348u, 375u, 445u, 112u}) CHECK(!ItemLook(definition));

    const std::optional<ItemArtwork> rock = ArtworkOfLook(*ItemLook(283));
    CHECK(rock && rock->mbs == "player/dwarf00_arms01_lost.mbs" && rock->ftx == "player/dwarf00_arms06_lost.ftx");
    CHECK(ArtworkOfLook(4)->ftx == "player/fighter00_arms01_lost.ftx" && ArtworkOfLook(11)->ftx == "player/fighter00_arms08_lost.ftx");
    CHECK(ArtworkOfLook(12)->mbs == "player/fighter00_shield01_lost.mbs" && ArtworkOfLook(19)->ftx == "player/fighter00_shield08_lost.ftx");
    CHECK(ArtworkOfLook(21)->mbs == "player/amazon00_arms01_lost.mbs" && ArtworkOfLook(21)->ftx == "player/amazon00_arms02_lost.ftx");
    for (const int look : {0, 1, 2, 3, 28, 35, 44, 60, 61, 72, -1}) CHECK(!ArtworkOfLook(look));
    const std::vector<int> looks = ArtworkLooks();
    CHECK(looks.size() == 32 && std::set<int>(looks.begin(), looks.end()).size() == 32);
    for (const auto& [definition, look] : game) CHECK(std::find(looks.begin(), looks.end(), look) != looks.end());

    // Same look, same picture; a different look of the class, a different one; no look, the family icon.
    ItemIconSet set;
    SpriteImage hammer, axe, family;
    hammer.rgba = {1}, axe.rgba = {2}, family.rgba = {3};
    set.artwork[41] = hammer;
    set.artwork[40] = axe;
    set.images[ItemIconFamily::kHammer] = family;
    CHECK(set.Find(283) == &set.artwork[41] && set.Find(284) == &set.artwork[41] && set.Find(282) == &set.artwork[40]);
    CHECK(set.Find(268) == &set.images[ItemIconFamily::kHammer] && set.Find(300) == nullptr);
    CHECK(set.PictureKey(283) == 41 && set.PictureKey(282) == 40 && set.PictureKey(268) == 1000 + int(ItemIconFamily::kHammer));
    CHECK(set.PictureKey(300) == -1);
}

void TestAccessoryIcon() {
    // Field [5] of every accessory entry of the executable's item table, less 60 (the first accessory icon).
    const std::map<uint32_t, int> game = {
        {375, 0}, {376, 1}, {377, 2}, {378, 3}, {379, 4}, {380, 5}, {381, 6}, {382, 7}, {383, 8}, {384, 9}, {385, 10},
        {386, 11}, {387, 12}, {388, 13}, {389, 14}, {390, 15}, {391, 3}, {392, 8}, {393, 11}, {394, 1}, {395, 5},
        {396, 9}, {397, 4}, {398, 7}, {399, 13}, {400, 6}, {401, 10}, {402, 15}, {403, 72}, {404, 73}, {405, 74},
        {406, 75}, {407, 76}, {408, 77}, {409, 78}, {410, 79}, {411, 76}, {412, 78}, {413, 79}, {414, 64}, {415, 65},
        {416, 66}, {417, 67}, {418, 68}, {419, 69}, {420, 70}, {421, 71}, {422, 66}, {423, 69}, {424, 71}, {425, 24},
        {426, 25}, {427, 26}, {428, 27}, {429, 28}, {430, 29}, {431, 30}, {432, 31}, {433, 29}, {434, 31}, {435, 16},
        {436, 17}, {437, 18}, {438, 19}, {439, 20}, {440, 21}, {441, 22}, {442, 23}, {443, 19}, {444, 23}, {445, 48},
        {446, 49}, {447, 50}, {448, 51}, {449, 52}, {450, 53}, {451, 54}, {452, 55}, {453, 50}, {454, 55}, {455, 53},
        {456, 40}, {457, 41}, {458, 42}, {459, 43}, {460, 44}, {461, 45}, {462, 46}, {463, 47}, {464, 42}, {465, 46},
        {466, 47}, {467, 56}, {468, 57}, {469, 58}, {470, 59}, {471, 60}, {472, 61}, {473, 62}, {474, 63}, {475, 59},
        {476, 63}, {477, 32}, {478, 33}, {479, 34}, {480, 35}, {481, 36}, {482, 37}, {483, 38}, {484, 39}, {485, 36},
        {486, 37}, {487, 38}, {488, 39}};
    CHECK(game.size() == 114);
    for (uint32_t definition = 0; definition < 800; ++definition) {
        const auto found = game.find(definition);
        CHECK(AccessoryIcon(definition) == (found == game.end() ? std::nullopt : std::optional<int>(found->second)));
    }
    std::set<int> icons;
    for (const auto& [definition, icon] : game) icons.insert(icon);
    CHECK(icons.size() == size_t(kAccessoryIconCount) && *icons.begin() == 0 && *icons.rbegin() == kAccessoryIconCount - 1);
    CHECK(AccessoryIconAnimation(0) == "Amulet01" && AccessoryIconAnimation(15) == "Amulet16" && AccessoryIconAnimation(16) == "Bracelet01");
    CHECK(AccessoryIconAnimation(24) == "Earring01" && AccessoryIconAnimation(32) == "EyeGlasses01" && AccessoryIconAnimation(40) == "Mantle01");
    CHECK(AccessoryIconAnimation(48) == "Boots01" && AccessoryIconAnimation(56) == "Glove01" && AccessoryIconAnimation(64) == "Belt01");
    CHECK(AccessoryIconAnimation(72) == "Gauntlet01" && AccessoryIconAnimation(79) == "Gauntlet08");
    CHECK(AccessoryIconAnimation(-1).empty() && AccessoryIconAnimation(kAccessoryIconCount).empty());
    std::set<std::string> names;
    for (int icon = 0; icon < kAccessoryIconCount; ++icon) CHECK(names.insert(AccessoryIconAnimation(icon)).second);
    // Chaos items reuse the icon of the regular item they are built on; every other accessory has its own.
    CHECK(AccessoryIcon(391) == AccessoryIcon(378) && AccessoryIcon(412) == AccessoryIcon(409) && AccessoryIcon(381) != AccessoryIcon(382));

    ItemIconSet set;
    SpriteImage amulet, belt, family;
    amulet.rgba = {1}, belt.rgba = {2}, family.rgba = {3};
    set.accessories[6] = amulet;
    set.accessories[67] = belt;
    set.images[ItemIconFamily::kAmulet] = family;
    CHECK(set.Find(381) == &set.accessories[6] && set.Find(417) == &set.accessories[67] && set.Find(380) == &set.images[ItemIconFamily::kAmulet]);
    CHECK(set.PictureKey(381) == 106 && set.PictureKey(417) == 167 && set.PictureKey(380) == 1000 + int(ItemIconFamily::kAmulet));
    // The cache key is the icon, so two items with one icon share one texture.
    CHECK(set.PictureKey(391) == set.PictureKey(378) || !AccessoryIcon(378).has_value());
    set.images.clear();
    CHECK(set.Find(380) == nullptr && set.PictureKey(380) == -1);
}

void TestRank() {
    CHECK(RankLetter(0) == 'S' && RankLetter(1) == 'A' && RankLetter(2) == 'B' && RankLetter(3) == 'C' && RankLetter(4) == 'D' && RankLetter(5) == 'E');
    CHECK(RankLetter(6) == 0 && RankLetter(0xFF) == 0);
    Bytes bytes = MakeInventorySave();
    bytes[At({0, -1, 0, 0}) + kItemGrade] = 0;
    const SaveModel model = SaveModel::FromBytes(bytes);
    CHECK(DescribeItem(model, model, {0, -1, 0, 0}, FixtureCatalog()).rank == 'S');
    CHECK(DescribeItem(model, model, {0, -1, 0, 3}, FixtureCatalog()).rank == 0); // the modifier-named item has none
}


void TestMaxDurability() {
    const Bytes original = MakeInventorySave();
    SaveModel model = SaveModel::FromBytes(original);
    const ItemRef hammer{0, 0, 0, 0}; // 2193 / 2862 shown (stored 219363 / 286263)
    const ItemDetail before = DescribeItem(model, model, hammer, FixtureCatalog());
    CHECK(before.durability == 2193 && before.durability_max == 2862 && before.durability_max_range.min == 1);
    CHECK(before.durability_max_range.max == 4500 && MaxShownDurability(ReadItem(original, hammer)) == 4500);

    // Raising the maximum leaves the current value alone; lowering it takes the current value down with it.
    CHECK(SetItemMaxDurability(model, hammer, 4500));
    CHECK(ReadItem(model.bytes(), hammer).durability_max == 450000 && ReadItem(model.bytes(), hammer).durability == 219363);
    CHECK(SetItemMaxDurability(model, hammer, 1000));
    CHECK(ReadItem(model.bytes(), hammer).durability_max == 100000 && ReadItem(model.bytes(), hammer).durability == 100000);
    CHECK(DescribeItem(model, model, hammer, FixtureCatalog()).durability == 1000);
    CHECK(SetItemDurability(model, hammer, 1000) && !SetItemDurability(model, hammer, 1001));

    // The same value keeps the stored one, so the remainder under the shown value survives.
    SaveModel same = SaveModel::FromBytes(original);
    CHECK(SetItemMaxDurability(same, hammer, 2862) && std::equal(same.bytes().begin(), same.bytes().end(), original.begin(), original.end()));

    // The game's own limit (it resets a larger maximum to 4500 on load); zero and values beyond it are refused.
    SaveModel limit = SaveModel::FromBytes(original);
    CHECK(SetItemMaxDurability(limit, hammer, 4500) && ReadItem(limit.bytes(), hammer).durability_max == 450000);
    CHECK(!SetItemMaxDurability(limit, hammer, 4501) && !SetItemMaxDurability(limit, hammer, 0) && !SetItemMaxDurability(limit, hammer, UINT32_MAX));
    const ItemRef counter{0, -1, 0, 3};
    CHECK(!SetItemMaxDurability(limit, counter, 1000000) && ReadItem(limit.bytes(), counter).durability_max == 5);
    CHECK(!SetItemMaxDurability(limit, {0, -1, 0, 7}, 10) && !SetItemMaxDurability(limit, {0, -1, 0, 5}, 10));

    // Only the two durability fields differ, and the change passes Apply-time validation.
    SaveModel lowered = SaveModel::FromBytes(original);
    CHECK(SetItemMaxDurability(lowered, hammer, 1000));
    for (const size_t at : Diff(original, Bytes(lowered.bytes().begin(), lowered.bytes().end()))) {
        CHECK(at >= At(hammer) + kItemDurability && at < At(hammer) + kItemDurabilityMax + 4);
    }
    std::string error;
    CHECK(ValidateItemChanges(original, Bytes(lowered.bytes().begin(), lowered.bytes().end()), error));
    Bytes broken = original; // maximum below the current value, or zero
    PutU32(broken, At(hammer) + kItemDurabilityMax, 100000);
    CHECK(!ValidateItemChanges(original, broken, error));
    Bytes zero = original;
    PutU32(zero, At(hammer) + kItemDurabilityMax, 0);
    PutU32(zero, At(hammer) + kItemDurability, 0);
    CHECK(!ValidateItemChanges(original, zero, error));
    Bytes accessory = original; // an item with no editable durability keeps its maximum
    PutU32(accessory, At({0, -1, 0, 7}) + kItemDurabilityMax, 300);
    CHECK(!ValidateItemChanges(original, accessory, error));
}

void TestSorting() {
    const Bytes original = MakeInventorySave();
    SaveModel model = SaveModel::FromBytes(original);
    const InventoryList list = BuildInventoryList(model, model, 0, 0, FixtureCatalog());
    const auto order = [](const InventoryRow& row) {
        switch (row.category) {
        case ItemCategory::kWeapon: return 0;
        case ItemCategory::kShield: return 1;
        case ItemCategory::kAccessory: return 2;
        case ItemCategory::kItem:
        case ItemCategory::kOther: break;
        }
        return 3;
    };
    const auto lower = [](std::string text) {
        for (char& ch : text) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        return text;
    };
    CHECK(list.rows.size() == 11);
    for (size_t i = 1; i < list.rows.size(); ++i) {
        const InventoryRow& a = list.rows[i - 1];
        const InventoryRow& b = list.rows[i];
        const int location_a = a.ref.storage() ? 99 : a.ref.set, location_b = b.ref.storage() ? 99 : b.ref.set;
        CHECK(location_a <= location_b);
        if (location_a != location_b || a.malformed || b.malformed) continue;
        CHECK(order(a) <= order(b));
        if (order(a) == order(b)) CHECK(lower(a.name) <= lower(b.name));
    }
    // Each location is sorted on its own: storage has weapons first by name, then the shield, accessories and the rest.
    const InventoryList storage = BuildInventoryList(model, model, 0, 0, FixtureCatalog(), kLocationStorage);
    CHECK(storage.rows.size() >= 5 && storage.rows[0].name == "Demon's Rock Hammer" && storage.rows[1].name == "Envenomed Martial Hammer");
    CHECK(storage.rows[2].category == ItemCategory::kShield && storage.rows[3].category == ItemCategory::kAccessory);
    // The prefix is part of the name that is sorted: it is the composed name the user reads.
    for (const InventoryRow& row : list.rows) CHECK(row.name == ItemDisplayName(ReadItem(model.bytes(), row.ref), FixtureCatalog()));
    // Sorting reads records, never moves them: every row still points at its own record, whatever its place in the list.
    std::set<std::tuple<int, int, int, int>> seen;
    for (const InventoryRow& row : list.rows) {
        CHECK(ReadItem(model.bytes(), row.ref).definition == row.definition);
        seen.insert({row.ref.slot, row.ref.character, row.ref.set, row.ref.index});
    }
    CHECK(seen.size() == list.rows.size());
    // An edit through a sorted row changes that record only, and the selection survives the next sort.
    const InventoryRow& second = list.rows[1];
    InventorySelection selection;
    selection.Select(second.ref);
    CHECK(SetItemAttack(model, second.ref, 1, 2) || SetItemDurability(model, second.ref, 1));
    const std::vector<size_t> changed = Diff(original, Bytes(model.bytes().begin(), model.bytes().end()));
    CHECK(!changed.empty() && changed.front() >= At(second.ref) && changed.back() < At(second.ref) + kItemRecordSize);
    const InventoryList again = BuildInventoryList(model, SaveModel::FromBytes(original), 0, 0, FixtureCatalog());
    CHECK(ResolveSelection(again, selection.item) == second.ref);
    const auto row_of = [&](const ItemRef& ref) { return std::find_if(again.rows.begin(), again.rows.end(), [&](const InventoryRow& r) { return r.ref == ref; }); };
    CHECK(row_of(second.ref) != again.rows.end() && row_of(second.ref)->changed);
    CHECK(std::count_if(again.rows.begin(), again.rows.end(), [](const InventoryRow& r) { return r.changed; }) == 1);
    selection.TakeScrollReset();
    selection.Select(again.rows[row_of(second.ref) - again.rows.begin()].ref);
    CHECK(!selection.TakeScrollReset());
}

void TestClassCompatibility() {
    const std::vector<CharacterClass> game_order = {CharacterClass::kFighter, CharacterClass::kAmazon, CharacterClass::kDwarf,
                                                    CharacterClass::kElf, CharacterClass::kSorceress, CharacterClass::kWizard};
    // Weapons: one class per family of 27 items, in the item list's order.
    const CharacterClass family[] = {CharacterClass::kFighter, CharacterClass::kAmazon, CharacterClass::kElf,
                                     CharacterClass::kDwarf, CharacterClass::kSorceress, CharacterClass::kWizard};
    for (uint32_t block = 0; block < 6; ++block) {
        CHECK(UsableBy(kFirstWeapon + block * 27) == std::vector<CharacterClass>{family[block]});
        CHECK(UsableBy(kFirstWeapon + block * 27 + 26) == std::vector<CharacterClass>{family[block]});
    }
    CHECK(UsableBy(kFirstShield) == std::vector<CharacterClass>{CharacterClass::kFighter});
    CHECK(UsableBy(kFirstAccessory) == game_order && UsableBy(kEndGear - 1) == game_order);
    CHECK(UsableBy(kModifierNamedDefinition).empty() && UsableBy(kEndGear).empty() && UsableBy(0).empty());
    // Compatibility is not stored in a record: no editor entry point takes a class.
    const std::string header = ReadText(fs::path(DCP_SOURCE_DIR) / "launcher" / "inventory_data.h");
    CHECK(header.find("SetItemUsable") == std::string::npos && header.find("SetItemClass") == std::string::npos);
    CHECK(std::string(RankAnimation('S')) == "ICON_RANK_S" && std::string(RankAnimation('E')) == "ICON_RANK_E");
    CHECK(std::string(RankAnimation('D')) == "ICON_RANK_D" && RankAnimation(0) == nullptr && RankAnimation('?') == nullptr);
    for (uint8_t grade = 0; grade < 6; ++grade) CHECK(RankAnimation(RankLetter(grade)) != nullptr);
    CHECK(RankAnimation(RankLetter(6)) == nullptr && RankAnimation(RankLetter(0xFF)) == nullptr);
}


InventoryQuery Query(InventoryGroup group, const char* search = "", InventorySort sort = InventorySort::kName) {
    InventoryQuery query;
    query.group = group;
    query.search = search;
    query.sort = sort;
    return query;
}

std::vector<std::string> Names(const InventoryList& list) {
    std::vector<std::string> names;
    for (const InventoryRow& row : list.rows) names.push_back(row.name);
    return names;
}

void TestQuery() {
    const Bytes original = MakeInventorySave();
    SaveModel model = SaveModel::FromBytes(original);
    const SaveModel pristine = SaveModel::FromBytes(original);
    const ItemCatalog catalog = FixtureCatalog();
    const auto build = [&](const InventoryQuery& query) { return BuildInventoryList(model, pristine, 0, 0, catalog, query); };

    // Categories: every item of the character and the shared storage is in exactly one of them.
    const InventoryList weapons = build(Query(InventoryGroup::kWeaponsShields));
    const InventoryList accessories = build(Query(InventoryGroup::kAccessories));
    const InventoryList other = build(Query(InventoryGroup::kOther));
    const InventoryList runes = build(Query(InventoryGroup::kItems));
    CHECK(weapons.rows.size() == 4 && accessories.rows.size() == 3 && other.rows.size() == 2 && runes.rows.size() == 2);
    CHECK(weapons.rows.size() + accessories.rows.size() + other.rows.size() + runes.rows.size() == BuildInventoryList(model, pristine, 0, 0, catalog).rows.size());
    std::set<std::tuple<int, int, int, int>> seen;
    for (const InventoryList* list : {&weapons, &accessories, &other, &runes}) {
        for (const InventoryRow& row : list->rows) {
            CHECK(seen.insert({row.ref.slot, row.ref.character, row.ref.set, row.ref.index}).second);
            CHECK(ReadItem(model.bytes(), row.ref).definition == row.definition);
        }
    }
    for (const InventoryRow& row : weapons.rows) CHECK(row.category == ItemCategory::kWeapon || row.category == ItemCategory::kShield);
    for (const InventoryRow& row : accessories.rows) CHECK(row.category == ItemCategory::kAccessory);
    CHECK(GroupOf(112) == InventoryGroup::kItems && GroupOf(3000) == InventoryGroup::kOther && GroupOf(kFirstWeapon) == InventoryGroup::kWeaponsShields &&
          GroupOf(kFirstShield) == InventoryGroup::kWeaponsShields && GroupOf(kFirstAccessory) == InventoryGroup::kAccessories &&
          GroupOf(kEndGear) == InventoryGroup::kOther);

    // Search: case-insensitive, partial, prefix, base name, combined with the category, empty and without a result.
    CHECK(Names(build(Query(InventoryGroup::kWeaponsShields, "hammer"))).size() == 3);
    CHECK(Names(build(Query(InventoryGroup::kWeaponsShields, "HAMMER"))) == Names(build(Query(InventoryGroup::kWeaponsShields, "hammer"))));
    CHECK(Names(build(Query(InventoryGroup::kWeaponsShields, "demon"))) == std::vector<std::string>{"Demon's Rock Hammer"});
    CHECK(Names(build(Query(InventoryGroup::kWeaponsShields, "envenomed martial"))) == std::vector<std::string>{"Envenomed Martial Hammer"});
    CHECK(Names(build(Query(InventoryGroup::kWeaponsShields, "Rock Hammer"))) == std::vector<std::string>{"Demon's Rock Hammer"});
    CHECK(build(Query(InventoryGroup::kAccessories, "hammer")).rows.empty());
    CHECK(build(Query(InventoryGroup::kWeaponsShields, "zzz")).rows.empty());
    CHECK(build(Query(InventoryGroup::kWeaponsShields, "")).rows.size() == weapons.rows.size());
    CHECK(Names(build(Query(InventoryGroup::kAccessories, "boots"))).size() == 1);

    // Sorting: name, category, equipment level and rank, each with the name as the tie-breaker.
    const auto by = [&](InventorySort sort) { return build(Query(InventoryGroup::kWeaponsShields, "", sort)); };
    const InventoryList name = by(InventorySort::kName);
    for (size_t i = 1; i < name.rows.size(); ++i) {
        std::string a = name.rows[i - 1].name, b = name.rows[i].name;
        for (char& ch : a) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        for (char& ch : b) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        CHECK(a <= b);
    }
    const InventoryList category = by(InventorySort::kCategory);
    for (size_t i = 1; i < category.rows.size(); ++i) CHECK(category.rows[i - 1].family <= category.rows[i].family);
    CHECK(category.rows.back().category == ItemCategory::kShield);
    const InventoryList level = by(InventorySort::kLevel);
    for (size_t i = 1; i < level.rows.size(); ++i) CHECK(level.rows[i - 1].level <= level.rows[i].level);
    const InventoryList rank = by(InventorySort::kRank);
    for (size_t i = 1; i < rank.rows.size(); ++i) CHECK(rank.rows[i - 1].rank <= rank.rows[i].rank);
    CHECK(rank.rows.front().rank == 0 && rank.rows.back().rank >= 3);
    CHECK(FamilyOrder(kFirstWeapon) == 0 && FamilyOrder(kFirstWeapon + 27) == 1 && FamilyOrder(kFirstWeapon + 27 * 5) == 5 &&
          FamilyOrder(kFirstShield) == 6 && FamilyOrder(kFirstAccessory) == 7 && FamilyOrder(112) == 8);
    // Rank order S A B C E, an unproven grade after E, no rank last; grade 4 never becomes D.
    ItemRecord record;
    record.definition = kFirstWeapon;
    const int order[] = {RankOrder((record.grade = 0, record)), RankOrder((record.grade = 1, record)), RankOrder((record.grade = 2, record)),
                         RankOrder((record.grade = 3, record)), RankOrder((record.grade = 4, record)), RankOrder((record.grade = 5, record)),
                         RankOrder((record.grade = 6, record)), RankOrder((record.grade = 0xFF, record))};
    for (size_t i = 1; i < std::size(order); ++i) CHECK(order[i - 1] < order[i]);

    // Display only: the same records, the same bytes, and an edit through a row lands on its own item.
    CHECK(std::equal(model.bytes().begin(), model.bytes().end(), original.begin(), original.end()));
    const InventoryRow& first = rank.rows.front();
    CHECK(SetItemDurability(model, first.ref, 1) || SetItemAttack(model, first.ref, 1, 2));
    const std::vector<size_t> changed = Diff(original, Bytes(model.bytes().begin(), model.bytes().end()));
    CHECK(!changed.empty() && changed.front() >= At(first.ref) && changed.back() < At(first.ref) + kItemRecordSize);
    // The pending edit follows the item through search, sort and category changes.
    for (const InventoryQuery& query : {Query(InventoryGroup::kWeaponsShields, "", InventorySort::kLevel), Query(InventoryGroup::kWeaponsShields, "hammer"),
                                        Query(InventoryGroup::kWeaponsShields, "", InventorySort::kCategory)}) {
        const InventoryList list = build(query);
        const auto row = std::find_if(list.rows.begin(), list.rows.end(), [&](const InventoryRow& r) { return r.ref == first.ref; });
        if (row != list.rows.end()) CHECK(row->changed);
        CHECK(std::count_if(list.rows.begin(), list.rows.end(), [](const InventoryRow& r) { return r.changed; }) <= 1);
    }
    // Selection: kept while the item is listed, replaced (and the details reset once) when a search drops it.
    InventorySelection selection;
    selection.Select(first.ref);
    selection.TakeScrollReset();
    const InventoryList keep = build(Query(InventoryGroup::kWeaponsShields, "", InventorySort::kLevel));
    CHECK(ResolveSelection(keep, selection.item) == first.ref);
    selection.Select(ResolveSelection(keep, selection.item));
    CHECK(!selection.TakeScrollReset());
    const InventoryList dropped = build(Query(InventoryGroup::kAccessories));
    selection.Select(ResolveSelection(dropped, selection.item));
    CHECK(selection.item != first.ref && selection.TakeScrollReset());
    CHECK(ResolveSelection(build(Query(InventoryGroup::kWeaponsShields, "zzz")), first.ref) == std::nullopt);
}


size_t CharacterBase(int slot, int character) {
    return size_t(slot) * kDataSlotStride + kCharacterOffset + size_t(character) * kCharacterStride;
}

Bytes MakeDuplicateSave() {
    Bytes bytes = MakeInventorySave();
    const Item twin{283, {{0x12, 94}, {0x00, 89}, {0x2A, 58}}, 189587, 189587, 93, 1, 227, 250, 0};
    PutItem(bytes, {0, -1, 0, 20}, twin);
    PutItem(bytes, {0, -1, 0, 21}, twin);
    Seal(bytes);
    return bytes;
}

std::vector<size_t> ChangedOffsets(const Bytes& before, const Bytes& after) {
    std::vector<size_t> at = Diff(before, after);
    at.erase(std::remove_if(at.begin(), at.end(), [](size_t i) { return i >= kSaveCrcOffset && i < kSaveCrcOffset + 4; }), at.end());
    return at;
}

void TestDuplicateIdentity(const fs::path& base) {
    const Bytes original = MakeDuplicateSave();
    const ItemRef first{0, -1, 0, 20}, second{0, -1, 0, 21};
    const ItemCatalog catalog = FixtureCatalog();
    CHECK(ReadItem(original, first).definition == ReadItem(original, second).definition);
    CHECK(std::equal(original.begin() + long(At(first)), original.begin() + long(At(first) + kItemRecordSize), original.begin() + long(At(second))));

    const std::vector<InventoryQuery> queries = {
        Query(InventoryGroup::kWeaponsShields), Query(InventoryGroup::kWeaponsShields, "demon"),
        Query(InventoryGroup::kWeaponsShields, "", InventorySort::kRank), Query(InventoryGroup::kWeaponsShields, "ROCK HAMMER", InventorySort::kLevel),
        Query(InventoryGroup::kWeaponsShields, "", InventorySort::kCategory)};
    int round = 0;
    for (const InventoryQuery& query : queries) {
        const fs::path user = base / ("duplicates_" + std::to_string(round++));
        const fs::path file = WriteSave(user, original);
        SaveEdit edit = OpenEdit(user);
        const InventoryList before = BuildInventoryList(edit.working(), edit.persisted(), 0, 0, catalog, query);
        std::vector<const InventoryRow*> twins;
        for (const InventoryRow& row : before.rows) {
            if (row.ref == first || row.ref == second) twins.push_back(&row);
        }
        CHECK(twins.size() == 2 && twins[0]->name == twins[1]->name && twins[0]->definition == twins[1]->definition && twins[0]->rank == twins[1]->rank);
        if (twins.size() != 2) continue;
        // The row the UI would select is the one found by its position in this list; only its reference leaves the list.
        const ItemRef selected = (twins[0]->ref == second ? twins[0] : twins[1])->ref;
        CHECK(selected == second);

        CHECK(SetItemAttack(edit.working(), selected, 12, 99) && SetItemDurability(edit.working(), selected, 777) &&
              SetItemRequiredLevel(edit.working(), selected, 55));
        const InventoryList after = BuildInventoryList(edit.working(), edit.persisted(), 0, 0, catalog, query);
        CHECK(ResolveSelection(after, selected) == std::optional<ItemRef>(selected));
        for (const InventoryRow& row : after.rows) CHECK(row.changed == (row.ref == second));
        CHECK(edit.Apply() == SaveEdit::Result::kApplied);

        const Bytes written = ReadBytes(file);
        for (const size_t i : ChangedOffsets(original, written)) CHECK(i >= At(second) && i < At(second) + kItemRecordSize);
        CHECK(!ChangedOffsets(original, written).empty());
        CHECK(std::equal(written.begin() + long(At(first)), written.begin() + long(At(first) + kItemRecordSize), original.begin() + long(At(first))));

        const SaveModel reloaded = SaveModel::FromFile(file);
        const ItemDetail one = DescribeItem(reloaded, reloaded, first, catalog), two = DescribeItem(reloaded, reloaded, second, catalog);
        CHECK(one.attack_min == 227 && one.attack_max == 250 && one.durability == 1895 && one.level == 94);
        CHECK(two.attack_min == 12 && two.attack_max == 99 && two.durability == 777 && two.level == 55 && two.name == one.name);
    }
}

void TestItemsEntries() {
    const Bytes original = MakeInventorySave();
    SaveModel model = SaveModel::FromBytes(original);
    const ItemCatalog catalog = FixtureCatalog();
    for (const ItemRef& ref : {ItemRef{0, -1, 0, 3}, ItemRef{0, 0, 0, 5}}) {
        const ItemDetail detail = DescribeItem(model, model, ref, catalog);
        CHECK(detail.present && detail.category == ItemCategory::kItem && !detail.durability_editable && !detail.level_editable && !detail.has_attack && detail.rank == 0);
        CHECK(!SetItemDurability(model, ref, 1) && !SetItemMaxDurability(model, ref, 1) && !SetItemAttack(model, ref, 1, 2) && !SetItemRequiredLevel(model, ref, 5));
        CHECK(std::equal(model.bytes().begin(), model.bytes().end(), original.begin(), original.end()));
    }
    const InventoryList runes = BuildInventoryList(model, model, 0, 0, catalog, Query(InventoryGroup::kItems));
    CHECK(runes.rows.size() == 2);
    for (const InventoryRow& row : runes.rows) CHECK(row.definition == 112 && row.category == ItemCategory::kItem && !row.editable);
    CHECK(Names(runes) == (std::vector<std::string>{"Elixir", "Rebirth Scroll"}));
    // Items are read-only: no setter changes them and the transaction refuses any change to one.
    const ItemRef rune{0, -1, 0, 3};
    CHECK(!SetModifierValue(model, rune, 0, 60.0f));
    CHECK(std::equal(model.bytes().begin(), model.bytes().end(), original.begin(), original.end()));
    std::string error;
    Bytes counter = original;
    PutFloat(counter, At(rune) + 4, 60.0f);
    CHECK(!ValidateItemChanges(original, counter, error));
    counter = original;
    PutU32(counter, At(rune) + kItemDurability, 3);
    CHECK(!ValidateItemChanges(original, counter, error));
    PutU32(counter, At(rune) + kItemDurability, 4);
    PutU32(counter, At(rune) + kItemDurabilityMax, 9);
    CHECK(!ValidateItemChanges(original, counter, error));
}

// The limits the game's item validator keeps on durability; Attack and the Required level are the user's own.
void TestGameLimits() {
    CHECK(MaxStoredDurability(GearWith(283, {})) == 450000 && MaxStoredDurability(GearWith(283, {{48, 0, 10.0f}})) == 50000);
    CHECK(MaxStoredDurability(GearWith(283, {{49, 0, 10.0f}})) == 25000 && MaxStoredDurability(GearWith(283, {{50, 0, 10.0f}, {48, 0, 1.0f}})) == 10000);

    const Bytes original = MakeInventorySave();
    const ItemRef hammer{0, -1, 0, 0}; // level 94, attack 227-250
    SaveModel model = SaveModel::FromBytes(original);
    std::string error;
    // Level 1 and Attack 255-255 are accepted together, in either order, and neither edit moves the other.
    CHECK(SetItemRequiredLevel(model, hammer, 1));
    CHECK(ReadItem(model.bytes(), hammer).attack_min == 227 && ReadItem(model.bytes(), hammer).attack_max == 250);
    CHECK(SetItemAttack(model, hammer, 255, 255) && ReadItem(model.bytes(), hammer).required_level() == 1);
    CHECK(ValidateItemChanges(original, Bytes(model.bytes().begin(), model.bytes().end()), error));
    CHECK(!SetItemAttack(model, hammer, 5, 256) && !SetItemAttack(model, hammer, 6, 5) && SetItemAttack(model, hammer, 1, 5));
    CHECK(SetItemRequiredLevel(model, hammer, 99) && ReadItem(model.bytes(), hammer).attack_max == 5);
    // A transaction holding an attack above what the level used to allow is accepted; one holding a durability the game would undo is not.
    Bytes beyond = original;
    beyond[At(hammer) + kItemAttackMax] = 251;
    CHECK(ValidateItemChanges(original, beyond, error));
    beyond = original;
    PutU32(beyond, At(hammer) + kItemDurabilityMax, 450001);
    CHECK(!ValidateItemChanges(original, beyond, error));
    // Shields and accessories keep their hidden attack bytes whatever the level.
    SaveModel shield = SaveModel::FromBytes(original);
    const ItemRef buckler{0, -1, 0, 2};
    shield.bytes()[At(buckler) + kItemAttackMin] = 40;
    const Bytes shield_before(shield.bytes().begin(), shield.bytes().end());
    CHECK(SetItemRequiredLevel(shield, buckler, 1) && shield.bytes()[At(buckler) + kItemAttackMin] == 40);
    CHECK(ValidateItemChanges(shield_before, Bytes(shield.bytes().begin(), shield.bytes().end()), error));
    Bytes raised = shield_before;
    raised[At(buckler) + kItemAttackMin] = 41;
    CHECK(!ValidateItemChanges(shield_before, raised, error));
    // A lower level with the old attack left in place is the user's choice.
    Bytes lower_level = original;
    lower_level[At(hammer) + kItemLevel] = 0;
    CHECK(ValidateItemChanges(original, lower_level, error));
    // A durability-down modifier lowers the maximum the editor offers.
    Bytes with_down = original;
    PutItem(with_down, {0, -1, 0, 40}, {283, {{49, 30}}, 20000, 20000, 93, 1, 200, 220, 0});
    SaveModel down = SaveModel::FromBytes(with_down);
    CHECK(DescribeItem(down, down, {0, -1, 0, 40}, FixtureCatalog()).durability_max_range.max == 250);
    CHECK(SetItemMaxDurability(down, {0, -1, 0, 40}, 250) && !SetItemMaxDurability(down, {0, -1, 0, 40}, 251));
}

// The effective limits: what the controls offer is what Apply accepts.
void TestEffectiveLimits(const fs::path& base) {
    const ItemRef hammer{0, -1, 0, 0};
    // A record already above the game's limits (5000 on disk) is brought to them when it is opened, never raised.
    Bytes legacy = MakeInventorySave();
    PutU32(legacy, At(hammer) + kItemDurability, 480000);
    PutU32(legacy, At(hammer) + kItemDurabilityMax, 500000);
    legacy[At(hammer) + kItemLevel] = 98;
    legacy[At(hammer) + kItemAttackMin] = 255;
    legacy[At(hammer) + kItemAttackMax] = 255;
    SaveModel model = SaveModel::FromBytes(legacy);
    std::string error;
    // The cause of the generic failure: touching only Durability of a record above the limit leaves the maximum above it.
    Bytes raw = legacy;
    PutU32(raw, At(hammer) + kItemDurability, 100000);
    CHECK(!ValidateItemChanges(legacy, raw, error) && error == kProblemMaxDurability);
    CHECK(SetItemDurability(model, hammer, 1000));
    const ItemDetail detail = DescribeItem(model, model, hammer, FixtureCatalog());
    CHECK(detail.durability_max == 4500 && detail.durability_max_range.max == 4500 && detail.durability == 1000);
    CHECK(detail.attack_min == 255 && detail.attack_max == 255);
    CHECK(ValidateItemChanges(legacy, Bytes(model.bytes().begin(), model.bytes().end()), error));
    // Opening it does the same without an edit, and nothing is raised when the limit allows more.
    SaveModel opened = SaveModel::FromBytes(legacy);
    CHECK(ClampToGameLimits(opened, hammer) && ReadItem(opened.bytes(), hammer).durability == 450000);
    CHECK(!ClampToGameLimits(opened, hammer));
    CHECK(SetItemRequiredLevel(opened, hammer, 99) && ReadItem(opened.bytes(), hammer).durability_max == 450000);
    CHECK(!SetItemMaxDurability(opened, hammer, 4501) && SetItemMaxDurability(opened, hammer, 4500));
    // The limit follows the record: a durability-down modifier (ids 48 to 50) lowers it at once, and the current value follows.
    Bytes down = MakeInventorySave();
    PutItem(down, {0, -1, 0, 40}, {283, {{0x12, 60}, {49, 30}}, 400000, 400000, 98, 1, 200, 220, 0});
    SaveModel limited = SaveModel::FromBytes(down);
    CHECK(DescribeItem(limited, limited, {0, -1, 0, 40}, FixtureCatalog()).durability_max_range.max == 250);
    CHECK(ClampToGameLimits(limited, {0, -1, 0, 40}) && ReadItem(limited.bytes(), {0, -1, 0, 40}).durability_max == 25000);
    CHECK(DescribeItem(limited, limited, {0, -1, 0, 40}, FixtureCatalog()).durability == 250);

    // Messages: each refusal names its cause, and a refused Apply keeps the draft, the file and the selection.
    const Bytes original = MakeInventorySave();
    const auto refused = [&](auto&& change) {
        Bytes bad = original;
        change(bad);
        std::string why;
        return ValidateItemChanges(original, bad, why) ? std::string() : why;
    };
    CHECK(refused([&](Bytes& b) { b[At(hammer) + kItemAttackMax] = 251; }).empty());
    CHECK(refused([&](Bytes& b) { PutU32(b, At(hammer) + kItemDurabilityMax, 450100); }) == kProblemMaxDurability);
    CHECK(refused([&](Bytes& b) { PutU32(b, At(hammer) + kItemDurability, 190000); }) == kProblemItem); // a current above its maximum is malformed
    CHECK(refused([&](Bytes& b) { PutFloat(b, At(hammer) + 4, 500.0f); }) == kProblemModifier);
    CHECK(refused([&](Bytes& b) { b[At(hammer) + kItemFlags] ^= 1; }) == kProblemItem);
    const fs::path user = base / "refused_apply";
    const fs::path file = WriteSave(user, original);
    SaveEdit edit = OpenEdit(user);
    for (size_t k = 0; k < 4; ++k) edit.working().bytes()[At(hammer) + kItemDurabilityMax + k] = uint8_t(450100u >> (8 * k));
    CHECK(edit.Apply() == SaveEdit::Result::kInvalidInventory && edit.detail() == kProblemMaxDurability);
    CHECK(ReadItem(edit.working().bytes(), hammer).durability_max == 450100 && ReadBytes(file) == original);
    const InventoryList still = BuildInventoryList(edit.working(), edit.persisted(), 0, 0, FixtureCatalog(), Query(InventoryGroup::kWeaponsShields));
    CHECK(ResolveSelection(still, hammer) == std::optional<ItemRef>(hammer));
    for (const char* text : {kProblemMaxDurability, kProblemLevel, kProblemModifier, kProblemItem}) {
        bool found = false;
        for (const localization::UiTextRow& row : localization::PortugueseBrazilUiText().rows) found |= row.english == text;
        CHECK(found);
    }
}

// Editing the modifier that picks the prefix renames the item; the search that found it keeps it until Apply or Discard.
void TestRenameKeepsRow(const fs::path& base) {
    Bytes original = MakeInventorySave();
    const ItemRef mantle{0, -1, 0, 30};
    const ItemRef twin{0, -1, 0, 31};
    const Item item{462, {{0x110, 77}}, 100000, 100000, 50, 1, 0, 0, 3};
    PutItem(original, mantle, item);
    PutItem(original, twin, item);
    Seal(original);
    const ItemCatalog catalog = FixtureCatalog();
    const fs::path user = base / "rename";
    const fs::path file = WriteSave(user, original);
    SaveEdit edit = OpenEdit(user);
    const auto list = [&](const char* search, InventorySort sort = InventorySort::kName) {
        return BuildInventoryList(edit.working(), edit.persisted(), 0, 0, catalog, Query(InventoryGroup::kAccessories, search, sort));
    };
    const auto has = [](const InventoryList& rows, const ItemRef& ref) {
        return std::any_of(rows.rows.begin(), rows.rows.end(), [&](const InventoryRow& r) { return r.ref == ref; });
    };
    const auto name_of = [&](const ItemRef& ref) { return DescribeItem(edit.working(), edit.persisted(), ref, catalog).name; };
    CHECK(name_of(mantle) == "Resistant Hunter Mantle" && has(list("resist"), mantle) && has(list("resist"), twin));

    // The cause: the value picks the block of the prefix, so a lower value is a different name the search does not match.
    SaveModel probe = SaveModel::FromBytes(original);
    CHECK(SetModifierValue(probe, mantle, 0, 1.0f));
    CHECK(ItemDisplayName(ReadItem(probe.bytes(), mantle), catalog) == "Frail Hunter Mantle");

    CHECK(SetModifierValue(edit.working(), mantle, 0, 1.0f));
    CHECK(name_of(mantle) == "Frail Hunter Mantle");
    InventoryList rows = list("resist");
    CHECK(has(rows, mantle) && has(rows, twin)); // pinned by its baseline name; the twin is untouched
    for (const InventoryRow& row : rows.rows) CHECK(row.changed == (row.ref == mantle));
    CHECK(has(list("frail"), mantle) && !has(list("frail"), twin)); // and by the new one
    CHECK(!has(list("mild"), mantle));
    for (const InventorySort sort : {InventorySort::kName, InventorySort::kRank, InventorySort::kLevel, InventorySort::kCategory}) {
        CHECK(ResolveSelection(list("resist", sort), mantle) == std::optional<ItemRef>(mantle));
    }
    // Sorted by name the renamed item moves, and the selection follows its record, never the twin.
    const InventoryList all = list("");
    const auto at = [&](const ItemRef& ref) { return std::find_if(all.rows.begin(), all.rows.end(), [&](const InventoryRow& r) { return r.ref == ref; }) - all.rows.begin(); };
    CHECK(at(mantle) < at(twin) && ResolveSelection(all, mantle) == std::optional<ItemRef>(mantle));
    CHECK(SetModifierValue(edit.working(), mantle, 0, 77.0f) && name_of(mantle) == "Resistant Hunter Mantle");

    // Discard brings the saved name back and the item is still found by "resist".
    CHECK(SetModifierValue(edit.working(), mantle, 0, 1.0f));
    edit.Discard();
    CHECK(name_of(mantle) == "Resistant Hunter Mantle" && has(list("resist"), mantle) && !has(list("frail"), mantle));

    // Apply writes the new name, and the list is rebuilt from the saved record.
    CHECK(SetModifierValue(edit.working(), mantle, 0, 1.0f) && edit.Apply() == SaveEdit::Result::kApplied);
    CHECK(name_of(mantle) == "Frail Hunter Mantle" && has(list("frail"), mantle) && !has(list("resist"), mantle) && has(list("resist"), twin));
    const Bytes written = ReadBytes(file);
    for (const size_t i : ChangedOffsets(original, written)) CHECK(i >= At(mantle) && i < At(mantle) + kItemRecordSize);
    CHECK(std::equal(written.begin() + long(At(twin)), written.begin() + long(At(twin) + kItemRecordSize), original.begin() + long(At(twin))));
}

// After Apply the search is cleared when the saved name no longer matches it; the item stays selected and listed.
void TestSearchKeptAfterSave(const fs::path& base) {
    Bytes original = MakeInventorySave();
    const ItemRef mantle{0, -1, 0, 30};
    const Item item{462, {{0x110, 77}}, 100000, 100000, 50, 1, 0, 0, 3};
    PutItem(original, mantle, item);
    PutItem(original, {0, -1, 0, 31}, item);
    Seal(original);
    const ItemCatalog catalog = FixtureCatalog();
    const fs::path user = base / "search_after_apply";
    WriteSave(user, original);
    SaveEdit edit = OpenEdit(user);
    {
        SaveEdit fresh = OpenEdit(user);
    // Two items of one place with one name say which is which; one alone does not.
        const InventoryList places = BuildInventoryList(fresh.working(), fresh.persisted(), 0, 0, catalog, Query(InventoryGroup::kAccessories, "hunter"));
        for (const InventoryRow& row : places.rows) CHECK(row.twin == (row.name == "Resistant Hunter Mantle"));
        const ItemDetail twin = DescribeItem(fresh.working(), fresh.persisted(), {0, -1, 0, 31}, catalog);
        CHECK(twin.twin && twin.name == "Resistant Hunter Mantle");
        CHECK(!DescribeItem(fresh.working(), fresh.persisted(), {0, -1, 0, 0}, catalog).twin);
    }
    CHECK(NameMatchesSearch("Propitious Rose Pendant", "prop") && NameMatchesSearch("Monk's Opal Necklace", "MONK") && NameMatchesSearch("Any", ""));
    CHECK(!NameMatchesSearch("Fortunate Rose Pendant", "prop"));
    // Before Apply the edited item is still found by the search; after it the search no longer holds, so it is cleared.
    CHECK(SetModifierValue(edit.working(), mantle, 0, 1.0f));
    CHECK(SearchKeptAfterSave(edit.persisted(), mantle, catalog, "resist") == "resist");
    CHECK(edit.Apply() == SaveEdit::Result::kApplied);
    CHECK(SearchKeptAfterSave(edit.persisted(), mantle, catalog, "resist").empty());
    CHECK(SearchKeptAfterSave(edit.persisted(), mantle, catalog, "frail") == "frail" && SearchKeptAfterSave(edit.persisted(), mantle, catalog, "").empty());
    // With the search cleared the same record is listed and selected under its new saved name; a fresh search is plain.
    const InventoryList all = BuildInventoryList(edit.working(), edit.persisted(), 0, 0, catalog, Query(InventoryGroup::kAccessories, ""));
    const auto found = std::find_if(all.rows.begin(), all.rows.end(), [&](const InventoryRow& r) { return r.ref == mantle; });
    CHECK(found != all.rows.end() && found->name == "Frail Hunter Mantle" && ResolveSelection(all, mantle) == std::optional<ItemRef>(mantle));
    const InventoryList old = BuildInventoryList(edit.working(), edit.persisted(), 0, 0, catalog, Query(InventoryGroup::kAccessories, "resist"));
    CHECK(std::none_of(old.rows.begin(), old.rows.end(), [&](const InventoryRow& r) { return r.ref == mantle; }));
    // The detail says what the name will become until Apply, and nothing after it or after Discard.
    SaveEdit again = OpenEdit(user);
    CHECK(DescribeItem(again.working(), again.persisted(), mantle, catalog).name == DescribeItem(again.working(), again.persisted(), mantle, catalog).saved_name);
    CHECK(SetModifierValue(again.working(), mantle, 0, 77.0f));
    const ItemDetail pending = DescribeItem(again.working(), again.persisted(), mantle, catalog);
    CHECK(pending.name == "Resistant Hunter Mantle" && pending.saved_name == "Frail Hunter Mantle");
    again.Discard();
    const ItemDetail discarded = DescribeItem(again.working(), again.persisted(), mantle, catalog);
    CHECK(discarded.name == discarded.saved_name && discarded.name == "Frail Hunter Mantle");
    const std::string view = ReadText(fs::path(DCP_SOURCE_DIR) / "launcher" / "editor_inventory.cpp");
    CHECK(view.find("Tr(\"Name will change to\")") != std::string::npos && view.find("Tr(\"Slot\")") != std::string::npos);
    CHECK(view.find("Tr(\"Changing modifier values can also change the item's generated name.\")") != std::string::npos);
    CHECK(view.find("KeepSearchMatchingSavedItem") != std::string::npos);
    CHECK(ReadText(fs::path(DCP_SOURCE_DIR) / "launcher" / "editor_page.cpp").find("KeepSearchMatchingSavedItem();") != std::string::npos);
}

// Saulo's case: two identical hammers in shared storage, the second set to the game's maximum, through Apply and back.
void TestDuplicateMaxed(const fs::path& base) {
    const Bytes original = MakeDuplicateSave();
    const ItemRef first{0, -1, 0, 20}, second{0, -1, 0, 21};
    const fs::path user = base / "duplicate_maxed";
    const fs::path file = WriteSave(user, original);
    SaveEdit edit = OpenEdit(user);
    CHECK(!SetItemMaxDurability(edit.working(), second, 5000)); // the game would reset 5000 to 4500
    CHECK(SetItemRequiredLevel(edit.working(), second, 99) && SetItemAttack(edit.working(), second, 255, 255));
    CHECK(SetItemMaxDurability(edit.working(), second, 4500) && SetItemDurability(edit.working(), second, 4500));
    CHECK(edit.Apply() == SaveEdit::Result::kApplied);
    const Bytes written = ReadBytes(file);
    const size_t at = At(second);
    CHECK(at == 0xC0 + 21 * kItemRecordSize);
    CHECK(written[at + kItemAttackMin] == 255 && written[at + kItemAttackMax] == 255 && written[at + kItemLevel] == 98);
    CHECK(GetU32(written, at + kItemDurability) == 450000 && GetU32(written, at + kItemDurabilityMax) == 450000);
    for (const size_t i : ChangedOffsets(original, written)) CHECK(i >= at && i < at + kItemRecordSize);
    CHECK(std::equal(written.begin() + long(At(first)), written.begin() + long(At(first) + kItemRecordSize), original.begin() + long(At(first))));
    std::string error;
    CHECK(ValidateItemChanges(original, written, error)); // nothing the game's validator would change
}

// The launcher edits the file the game mounts: both sides take <user>/savedata/<user id>/<title>/<dir> from one user folder.
void TestSavePaths(const fs::path& base, const fs::path& source) {
    const Bytes original = MakeDuplicateSave();
    const fs::path user = base / "save_paths";
    const fs::path file = WriteSave(user, original);
    const std::vector<DiscoveredSave> saves = DiscoverSaves(user, "CUSA10454");
    CHECK(saves.size() == 1 && saves.front().save0_path == user / "savedata" / "1" / "CUSA10454" / "SAVEDATA00" / "SAVE0.DAT");
    CHECK(saves.front().save0_path == file);
    // The runtime's mount point for SAVEDATA00 is that save's folder; the game reads SAVE0.DAT inside it.
    const fs::path mounted = user / kSavedataFolder / "1" / "CUSA10454" / "SAVEDATA00";
    CHECK(saves.front().folder == mounted);

    SaveEdit edit = OpenEdit(user);
    const ItemRef second{0, -1, 0, 21};
    CHECK(SetItemAttack(edit.working(), second, 12, 99) && edit.Apply() == SaveEdit::Result::kApplied);
    const Bytes written = ReadBytes(mounted / "SAVE0.DAT");
    CHECK(written != original && written == ReadBytes(file) && written.size() == kSaveFileSize);
    CHECK(SaveModel::FromBytes(written).Serialize() == written);
    // The backup of the replaced file names the same path and holds the bytes the game would have read before.
    size_t backups = 0;
    for (const auto& entry : fs::directory_iterator(BackupFolder(user))) {
        if (entry.path().extension() == ".DAT") {
            ++backups;
            CHECK(ReadBytes(entry.path()) == original);
        } else {
            CHECK(ReadText(entry.path()).find("original_path=" + file.string()) != std::string::npos);
        }
    }
    CHECK(backups == 1);

    const std::string runtime = ReadText(source / "src" / "runtime_savedata.c");
    CHECK(runtime.find("#define CAMPAIGN_SAVES \"savedata\"") != std::string::npos);
    CHECK(runtime.find("snprintf(out,size,\"%s/%s/%d/%s\",runtime_file_user_dir()") != std::string::npos);
    CHECK(runtime.find("snprintf(host,sizeof(host),\"%s/%s\",base,dir->data);") != std::string::npos);
    CHECK(runtime.find("runtime_file_mount(result->point.data,host);") != std::string::npos);
    const std::string loader = ReadText(source / "src" / "loader.c");
    CHECK(loader.find("runtime_file_configure(app0, user_dir ? user_dir : \"user\");") != std::string::npos);
    CHECK(loader.find("DcpLauncherConfig launcher={app0,paths.image_dir,user_dir,") != std::string::npos);
    CHECK(kSavedataFolder == "savedata");
}

void TestHpScope(const fs::path& base) {
    Bytes original = MakeInventorySave();
    PutU16(original, CharacterBase(0, 0) + kCharHpCurrent, 99);
    PutU16(original, CharacterBase(0, 0) + kCharHpMax, 500);
    PutU16(original, CharacterBase(0, 1) + kCharHpCurrent, 400);
    PutU16(original, CharacterBase(0, 1) + kCharHpMax, 300);
    Seal(original);
    const fs::path user = base / "hp_scope";
    const fs::path file = WriteSave(user, original);

    SaveEdit inventory = OpenEdit(user);
    CHECK(inventory.working().Character(0, 0)->hp_current == 99 && inventory.working().Character(0, 0)->hp_max == 500);
    CHECK(SetItemDurability(inventory.working(), {0, 0, 0, 0}, 5) && inventory.Apply() == SaveEdit::Result::kApplied);
    const Bytes after = ReadBytes(file);
    for (const int character : {0, 1}) {
        for (size_t i = kCharHpCurrent; i < kCharHpMax + 2; ++i) CHECK(after[CharacterBase(0, character) + i] == original[CharacterBase(0, character) + i]);
    }
    const size_t other = CharacterBase(0, 1);
    for (size_t i = 0; i < kCharacterStride; ++i) CHECK(after[other + i] == original[other + i]);

    SaveEdit invalid = OpenEdit(user);
    CHECK(!invalid.working().SetHp(0, 0, 501, 500) && !invalid.working().SetHp(0, 0, 100, 99) && !invalid.dirty());
    CHECK(invalid.working().SetHp(0, 0, 99, 500) && !invalid.dirty());
}

void TestSliderEditing() {
    // Every integer is reachable: a drag at one unit per pixel, the buttons and the model all move by exactly one.
    for (int value = 196; value <= 204; ++value) CHECK(FineDragValue(199, float(value - 199), {1, 4500}) == value);
    CHECK(FineDragValue(4498, 1.0f, {1, 4500}) == 4499 && FineDragValue(4498, 2.0f, {1, 4500}) == 4500 && FineDragValue(4498, 9.0f, {1, 4500}) == 4500);
    CHECK(FineDragValue(2, -9.0f, {1, 500}) == 1 && FineDragValue(100, 0.4f, {0, 500}) == 100 && FineDragValue(100, 0.6f, {0, 500}) == 101);
    CHECK(StepNumber(199, 1, {1, 4500}) == 200 && StepNumber(200, 1, {1, 4500}) == 201 && StepNumber(4500, 1, {1, 4500}) == 4500);

    const Bytes original = MakeInventorySave();
    const ItemRef hammer{0, 0, 0, 0}; // 2193 / 2862
    SaveModel model = SaveModel::FromBytes(original);
    CHECK(DescribeItem(model, model, hammer, FixtureCatalog()).durability_max_range.max == 4500);
    for (const uint32_t shown : {198u, 199u, 200u, 201u, 202u, 4498u, 4499u, 4500u}) {
        CHECK(SetItemMaxDurability(model, hammer, shown) && ReadItem(model.bytes(), hammer).durability_max == shown * 100);
        CHECK(SetItemDurability(model, hammer, shown) && ReadItem(model.bytes(), hammer).durability == shown * 100);
    }
    CHECK(!SetItemMaxDurability(model, hammer, 4501));
    // An item whose modifier lowers the game's limit to 500 offers every value up to it, one at a time.
    Bytes limited = original;
    PutItem(limited, {0, -1, 0, 40}, {283, {{0x12, 60}, {48, 30}}, 30000, 30000, 98, 1, 100, 200, 0});
    SaveModel capped = SaveModel::FromBytes(limited);
    const ItemRef item{0, -1, 0, 40};
    const ItemDetail detail = DescribeItem(capped, capped, item, FixtureCatalog());
    CHECK(detail.durability_max_range.max == 500 && detail.durability_max_range.min == 1 && detail.durability_max == 300);
    for (const uint32_t shown : {198u, 199u, 200u, 201u, 498u, 499u, 500u}) {
        CHECK(SetItemMaxDurability(capped, item, shown) && ReadItem(capped.bytes(), item).durability_max == shown * 100);
    }
    CHECK(!SetItemMaxDurability(capped, item, 501));
    const ItemDetail normal = DescribeItem(model, model, hammer, FixtureCatalog());
    CHECK(normal.durability_max_range.max == kNormalMaxDurability && detail.durability_max_range.max < kNormalMaxDurability);

    // The number text stays readable over the thumb, wherever it is.
    const NumberRange attack{0, 255};
    const Span at_start = SliderGrabSpan(0.0f, 200.0f, attack, 0, 16.0f);
    const Span at_end = SliderGrabSpan(0.0f, 200.0f, attack, 255, 16.0f);
    CHECK(at_start.min == 2.0f && at_start.max == 18.0f && at_end.min == 182.0f && at_end.max == 198.0f);
    const Span mid = SliderGrabSpan(0.0f, 200.0f, attack, 128, 16.0f);
    CHECK(mid.max - mid.min == 16.0f && mid.min > at_start.min && mid.max < at_end.max);
    const Span text{90.0f, 110.0f};
    CHECK(!TextOverSpan(text, at_start));                // fill before the text
    CHECK(!TextOverSpan(text, {70.0f, 94.0f}));          // thumb just reaching the text
    CHECK(TextOverSpan(text, {80.0f, 100.0f}));          // half the text behind it
    CHECK(TextOverSpan(text, {85.0f, 115.0f}));          // fully behind
    CHECK(!TextOverSpan(text, {108.0f, 126.0f}) && !TextOverSpan(text, at_end)); // thumb past the text
    CHECK(!TextOverSpan({50.0f, 50.0f}, {0.0f, 100.0f}));
    // A thumb centred behind the number, at every value of the track, covers it.
    CHECK(TextOverSpan(text, SliderGrabSpan(0.0f, 200.0f, attack, 128, 16.0f)) == (SliderGrabSpan(0.0f, 200.0f, attack, 128, 16.0f).min < 106.0f &&
                                                                                      SliderGrabSpan(0.0f, 200.0f, attack, 128, 16.0f).max > 94.0f));

    const std::string view = ReadText(fs::path(DCP_SOURCE_DIR) / "launcher" / "editor_inventory.cpp");
    const std::string page = ReadText(fs::path(DCP_SOURCE_DIR) / "launcher" / "editor_page.cpp");
    CHECK(view.find("\"attack_min\", {0, 255}") != std::string::npos && view.find("\"attack_max\", {0, 255}") != std::string::npos);
    CHECK(view.find("AdaptiveSliderMax") == std::string::npos && page.find("slider_max") == std::string::npos);
    CHECK(page.find("SliderGrabSpan(") != std::string::npos && page.find("TextOverSpan(") != std::string::npos && page.find("ReadableTextColor") == std::string::npos);
    CHECK(page.find("color = IM_COL32_WHITE;") != std::string::npos && page.find("MouseWheel") == std::string::npos);
    const auto occurrences = [&](const std::string& needle) {
        size_t n = 0;
        for (size_t at = page.find(needle); at != std::string::npos; at = page.find(needle, at + needle.size())) ++n;
        return n;
    };
    CHECK(occurrences("ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);") == 2 && occurrences("ImGui::PopItemFlag();") == 2);
    CHECK(view.find("if (detail.durability_max_range.max < kNormalMaxDurability)") != std::string::npos && view.find("Tr(\"Game limit:\")") != std::string::npos);
}

std::string FunctionBody(const std::string& text, const std::string& signature);

// What the editor does while a number is typed: each prefix that is a whole number inside the range goes through the field's
// setter at once, applied to the item as it was when typing began, so an intermediate "2" never leaves a trace.
void TypeInto(SaveModel& edited, const ItemRef& ref, const std::string& key, NumberRange range, const std::string& text) {
    TypedFieldEdit typing;
    typing.Begin(ref, key, range, ItemBytes(edited, ref));
    for (size_t length = 1; length <= text.size(); ++length) typing.Update(edited, text.substr(0, length));
    typing.Finish(edited, text);
}

// The game's own pairs: a Rampaging Chaos Hammer's eight modifiers, stored value to the number its panel shows.
struct ShownPair {
    uint16_t id;
    float stored;
    int shown;
};
constexpr ShownPair kHammerPairs[8] = {{155, 90, 46}, {115, 95, 20}, {30, 85, 27}, {3, 96, 9}, {108, 97, 29}, {54, 80, 26}, {35, 98, 14}, {52, 99, 29}};

void TestModifierDisplay() {
    for (const ShownPair& pair : kHammerPairs) {
        const std::optional<ModifierSpec> spec = ModifierSpecOf(pair.id);
        CHECK(spec && ModifierShownValue(*spec, pair.stored) == pair.shown);
    }
    // The arithmetic: range ends, single precision rounding down, the bonus above 100, clamps.
    const ModifierSpec frenzy = *ModifierSpecOf(155);
    CHECK(frenzy.minimum == 10 && frenzy.maximum == 50 && frenzy.bonus_maximum == 50);
    CHECK(ModifierShownValue(frenzy, 0.0f) == 10 && ModifierShownValue(frenzy, 100.0f) == 50 && ModifierShownValue(frenzy, 50.0f) == 30);
    CHECK(ModifierShownValue(frenzy, -5.0f) == 10 && ModifierShownValue(frenzy, 200.0f) == 50 && ModifierShownValue(frenzy, 500.0f) == 50);
    const ModifierSpec magic = *ModifierSpecOf(14); // Max MP: 10 to 100, 150 at stored 200
    CHECK(magic.minimum == 10 && magic.maximum == 100 && magic.bonus_maximum == 150);
    CHECK(ModifierShownValue(magic, 100.0f) == 100 && ModifierShownValue(magic, 150.0f) == 125 && ModifierShownValue(magic, 200.0f) == 150 && ModifierShownValue(magic, 101.0f) == 100);
    // Text index of the effect sheet: the same entry the game's text getter uses for each group of ids.
    CHECK(ModifierTextIndex(0) == std::optional<size_t>(0) && ModifierTextIndex(184) == std::optional<size_t>(184));
    CHECK(ModifierTextIndex(185) == std::optional<size_t>(185 + 0x39d) && ModifierTextIndex(372) == std::optional<size_t>(372 + 0x39d));
    CHECK(ModifierTextIndex(532) == std::optional<size_t>(532 + 0x7e8) && ModifierTextIndex(750) == std::optional<size_t>(750 + 0x7e8));
    CHECK(ModifierTextIndex(751) == std::optional<size_t>(751 + 0xc2f) && ModifierTextIndex(978) == std::optional<size_t>(978 + 0xc2f));
    CHECK(!ModifierSpecOf(373) && !ModifierSpecOf(531) && !ModifierSpecOf(979) && !ModifierTextIndex(400) && !ModifierSpecOf(0xFFFF));

    // The inverse over every record and stored value: an exact number goes to the nearest stored value that shows it, the
    // lower of two equals; a number nothing shows goes to the nearest shown one and never stays put.
    int sparse = 0, fixed = 0;
    for (int id = 0; id < 979; ++id) {
        const std::optional<ModifierSpec> spec = ModifierSpecOf(uint16_t(id));
        if (!spec) continue;
        const ModifierShown range = ModifierShownOf(*spec, 50.0f);
        std::set<int> reachable;
        for (int stored = kModifierStoredMin; stored <= kModifierStoredMax; ++stored) reachable.insert(ModifierShownValue(*spec, float(stored)));
        CHECK(*reachable.begin() == range.minimum && *reachable.rbegin() == range.maximum);
        fixed += range.minimum == range.maximum;
        sparse += int(reachable.size()) < range.maximum - range.minimum + 1;
        if (range.minimum == range.maximum) continue;
        for (const int current : {kModifierStoredMin, 17, 50, 93, kModifierStoredMax}) {
            const int current_shown = ModifierShownValue(*spec, float(current));
            for (int want = range.minimum; want <= range.maximum; ++want) {
                const int stored = ModifierStoredForShown(*spec, want, current);
                CHECK(stored >= kModifierStoredMin && stored <= kModifierStoredMax);
                const int got = ModifierShownValue(*spec, float(stored));
                if (reachable.count(want)) {
                    CHECK(got == want);
                    for (int other = kModifierStoredMin; other <= kModifierStoredMax; ++other) {
                        if (ModifierShownValue(*spec, float(other)) != want) continue;
                        CHECK(std::abs(stored - current) < std::abs(other - current) || (std::abs(stored - current) == std::abs(other - current) && stored <= other));
                    }
                } else if (want != current_shown) {
                    const auto above = reachable.upper_bound(want);
                    const auto below = std::prev(above);
                    CHECK(above != reachable.end() && above != reachable.begin());
                    CHECK(got != current_shown || *below == current_shown || *above == current_shown);
                    CHECK(got == *below || got == *above);
                }
            }
            CHECK(ModifierStoredForShown(*spec, current_shown, current) == current);
        }
    }
    CHECK(sparse == 226 && fixed == 81); // the six wide records and the fixed ones exist
    // A step on a record with gaps always moves to the next shown number.
    const ModifierSpec wide{10, 500, 500, 0};
    int stored = 50;
    for (int step = 0; step < 20; ++step) {
        const int before = ModifierShownValue(wide, float(stored));
        stored = ModifierStoredForShown(wide, before + 1, stored);
        CHECK(ModifierShownValue(wide, float(stored)) > before);
    }
    for (int step = 0; step < 20; ++step) {
        const int before = ModifierShownValue(wide, float(stored));
        stored = ModifierStoredForShown(wide, before - 1, stored);
        CHECK(ModifierShownValue(wide, float(stored)) < before);
    }
    // Equal distance goes to the lower neighbour.
    int tie = 0;
    for (int stored_value = kModifierStoredMin + 1; stored_value <= kModifierStoredMax; ++stored_value) {
        const int low = ModifierShownValue(wide, float(stored_value - 1)), high = ModifierShownValue(wide, float(stored_value));
        if (high - low != 4) continue;
        const int middle = low + 2;
        const int from = stored_value > 50 ? 1 : kModifierStoredMax;
        CHECK(ModifierShownValue(wide, float(ModifierStoredForShown(wide, middle, from))) == low);
        ++tie;
    }
    CHECK(tie > 0);
}

// The whole stored domain 0..200 decides what a modifier can show: ranges, fixed records, steps, the slider and typing.
void TestModifierFullRange() {
    int records = 0, fixed = 0, false_fixed = 0, above = 0, sparse = 0, base_fixed = 0;
    for (int id = 0; id < 979; ++id) {
        const std::optional<ModifierSpec> found = ModifierSpecOf(uint16_t(id));
        if (!found) continue;
        const ModifierSpec spec = *found;
        ++records;
        int number[kModifierStoredMax + 1];
        std::set<int> reachable;
        for (int stored = kModifierStoredMin; stored <= kModifierStoredMax; ++stored) {
            number[stored] = ModifierShownValue(spec, float(stored));
            reachable.insert(number[stored]);
        }
        CHECK(number[0] == spec.minimum && number[100] == spec.maximum && number[200] == spec.bonus_maximum);
        const ModifierShown range = ModifierShownOf(spec, 100.0f);
        CHECK(range.minimum == *reachable.begin() && range.maximum == *reachable.rbegin() && range.editable == (reachable.size() > 1));
        fixed += reachable.size() == 1;
        base_fixed += spec.minimum == spec.maximum;
        false_fixed += spec.minimum == spec.maximum && reachable.size() > 1;
        above += range.maximum > number[100];
        sparse += reachable.size() > 1 && int(reachable.size()) < range.maximum - range.minimum + 1;
        CHECK(ModifierShownValue(spec, 500.0f) == number[200] && ModifierShownValue(spec, -3.0f) == number[0]);
        if (reachable.size() == 1) continue;
        // Every stored value round-trips through the inverse, from wherever the edit starts.
        for (int stored = kModifierStoredMin; stored <= kModifierStoredMax; ++stored) {
            for (const int current : {0, 100, 200, stored}) {
                const int back = ModifierStoredForShown(spec, number[stored], current);
                CHECK(back >= kModifierStoredMin && back <= kModifierStoredMax && number[back] == number[stored]);
            }
        }
        // Plus and minus walk the reachable numbers one at a time, to both ends and back.
        const std::vector<int> ordered(reachable.begin(), reachable.end());
        int stored = ModifierStoredForShown(spec, ordered.front(), 100);
        for (size_t step = 1; step < ordered.size(); ++step) {
            stored = ModifierStoredForShown(spec, number[stored] + 1, stored);
            CHECK(number[stored] == ordered[step]);
        }
        stored = ModifierStoredForShown(spec, ordered.back() + 1, stored);
        CHECK(number[stored] == ordered.back());
        for (size_t step = ordered.size() - 1; step > 0; --step) {
            stored = ModifierStoredForShown(spec, number[stored] - 1, stored);
            CHECK(number[stored] == ordered[step - 1]);
        }
        stored = ModifierStoredForShown(spec, ordered.front() - 1, stored);
        CHECK(number[stored] == ordered.front());
        // A slider or typed number goes to the nearest reachable one, the lower of two, whatever the starting point.
        const int start = ModifierStoredForShown(spec, ordered.back(), 200);
        for (int want = ordered.front() - 2; want <= ordered.back() + 2; ++want) {
            const int got = number[ModifierStoredForShown(spec, want, start)];
            if (got != ordered.back() && reachable.count(want) == 0) {
                const auto above_it = reachable.upper_bound(want);
                if (above_it != reachable.end() && above_it != reachable.begin()) {
                    const int below_value = *std::prev(above_it);
                    const int nearest = want - below_value <= *above_it - want ? below_value : *above_it;
                    CHECK(nearest == number[start] || got == nearest);
                }
            }
            if (reachable.count(want) != 0) CHECK(got == want);
            CHECK(got >= ordered.front() && got <= ordered.back());
        }
    }
    CHECK(records == 820 && fixed == 81 && base_fixed == 266 && false_fixed == 185 && above == 336 && sparse == 226);

    // Percentages and flat stats above 100 when the game's table allows them.
    const ModifierSpec taken = *ModifierSpecOf(8), mana = *ModifierSpecOf(14), throwing = *ModifierSpecOf(30);
    CHECK(taken.maximum == 100 && taken.bonus_maximum == 200 && ModifierShownOf(taken, 50.0f).maximum == 200 && ModifierShownValue(taken, 200.0f) == 200);
    CHECK(mana.maximum == 100 && mana.bonus_maximum == 150 && ModifierShownOf(mana, 50.0f).maximum == 150);
    CHECK(throwing.maximum == 30 && throwing.bonus_maximum == 40 && ModifierShownOf(throwing, 50.0f).maximum == 40);
    // The values the old 1..100 domain could not reach exist: the same number is not shown by any stored value up to 100.
    for (const ModifierSpec& spec : {taken, mana, throwing}) {
        const int cap = ModifierShownValue(spec, 100.0f);
        CHECK(ModifierShownValue(spec, float(ModifierStoredForShown(spec, cap + 1, 100))) == cap + 1);
        CHECK(ModifierStoredForShown(spec, ModifierShownOf(spec, 100.0f).maximum, 100) == 200);
    }

    // Edited through the field path and saved: the stored value is 200 and the number comes back after Apply and a reopen.
    Bytes bytes = MakeInventorySave();
    const ItemRef ref{0, -1, 0, 41};
    PutItem(bytes, ref, {294, {{8, 50}, {14, 50}, {30, 50}}, 448000, 449500, 99, 1, 250, 255, 0});
    Seal(bytes);
    SaveModel model = SaveModel::FromBytes(bytes);
    const ItemDetail detail = DescribeItem(model, model, ref, FixtureCatalog());
    CHECK(detail.modifiers.size() == 3 && detail.modifiers[0].percent && !detail.modifiers[1].percent && detail.modifiers[2].percent);
    CHECK(detail.modifiers[0].range.max == 200 && detail.modifiers[1].range.max == 150 && detail.modifiers[2].range.max == 40);
    CHECK(SetItemField(model, ref, "modifier_0", 200) && SetItemField(model, ref, "modifier_1", 150) && SetItemField(model, ref, "modifier_2", 40));
    const ItemRecord set = ReadItem(model.bytes(), ref);
    CHECK(set.effects[0].value == 200.0f && set.effects[1].value == 200.0f && set.effects[2].value == 200.0f);
    CHECK(*ItemFieldValue(model, ref, "modifier_0") == 200 && *ItemFieldValue(model, ref, "modifier_1") == 150 && *ItemFieldValue(model, ref, "modifier_2") == 40);
    std::string error;
    CHECK(ValidateItemChanges(bytes, Bytes(model.bytes().begin(), model.bytes().end()), error));
    for (const size_t i : ChangedOffsets(bytes, Bytes(model.bytes().begin(), model.bytes().end()))) {
        CHECK(i >= At(ref) && (i - At(ref)) / kItemEffectStride < 3 && (i - At(ref)) % kItemEffectStride >= 4);
    }
    // Stored values outside 0..200 are still refused.
    Bytes beyond = bytes;
    PutFloat(beyond, At(ref) + 4, 201.0f);
    CHECK(!ValidateItemChanges(bytes, beyond, error) && error == kProblemModifier);
    // A modifier whose stored value is already above 100 is editable and shows what the game shows.
    PutFloat(beyond, At(ref) + 4, 150.0f);
    const SaveModel above_model = SaveModel::FromBytes(beyond);
    CHECK(*ItemFieldValue(above_model, ref, "modifier_0") == 150 && DescribeItem(above_model, above_model, ref, FixtureCatalog()).modifiers[0].editable);
}

// Rows whose number cannot change, or that show none, are left out of the Modifiers list; the record keeps them.
void TestListedModifiers(const fs::path& base) {
    Bytes bytes = MakeInventorySave();
    const ItemRef hammer{0, -1, 0, 41};
    std::vector<Modifier> eight;
    for (const ShownPair& pair : kHammerPairs) eight.push_back({pair.id, pair.stored});
    PutItem(bytes, hammer, {294, eight, 448000, 449500, 99, 1, 250, 255, 0});
    Seal(bytes);
    const SaveModel model = SaveModel::FromBytes(bytes);
    const ItemDetail detail = DescribeItem(model, model, hammer, FixtureCatalog());
    std::vector<int> listed;
    for (const ModifierLine& line : detail.modifiers) {
        if (line.listed) listed.push_back(line.slot);
    }
    CHECK(detail.modifiers.size() == 8 && listed == (std::vector<int>{0, 2, 3, 4, 5, 6, 7}));
    CHECK(!detail.modifiers[1].listed && detail.modifiers[4].listed && detail.modifiers[4].range.min == 3 && detail.modifiers[4].range.max == 30);

    // The rule holds for every record: listed exactly when the number varies over the whole stored range.
    int hidden = 0;
    for (int id = 0; id < 979; ++id) {
        const std::optional<ModifierSpec> spec = ModifierSpecOf(uint16_t(id));
        if (!spec) continue;
        Bytes one = MakeInventorySave();
        PutItem(one, hammer, {294, {{uint16_t(id), 50}}, 448000, 449500, 99, 1, 250, 255, 0});
        Seal(one);
        const SaveModel single = SaveModel::FromBytes(one);
        const ItemDetail line = DescribeItem(single, single, hammer, FixtureCatalog());
        std::set<int> reachable;
        for (int stored = kModifierStoredMin; stored <= kModifierStoredMax; ++stored) reachable.insert(ModifierShownValue(*spec, float(stored)));
        CHECK(line.modifiers.size() == 1 && line.modifiers[0].listed == (reachable.size() > 1 && line.modifiers[0].has_value));
        hidden += reachable.size() == 1;
    }
    CHECK(hidden == 81);

    // A modifier whose text shows no number is left out too.
    std::vector<std::string> effects = FixtureEffects();
    effects[30] = "Throw damage";
    const ItemCatalog text_only(FixtureItems(), effects);
    const ItemDetail plain = DescribeItem(model, model, hammer, text_only);
    CHECK(!plain.modifiers[2].has_value && !plain.modifiers[2].listed && plain.modifiers[0].listed);

    // Editing a listed row and applying leaves every hidden row's bytes, the order and the count as they were.
    const fs::path user = base / "listed_modifiers";
    const fs::path file = WriteSave(user, bytes);
    SaveEdit edit = OpenEdit(user);
    CHECK(SetItemField(edit.working(), hammer, "modifier_4", 12));
    CHECK(edit.Apply() == SaveEdit::Result::kApplied);
    const Bytes written = ReadBytes(file);
    const ItemRecord after = ReadItem(written, hammer);
    CHECK(after.effects.size() == 8 && after.effects[1].id == 115 && after.effects[1].value == kHammerPairs[1].stored);
    for (size_t i = 0; i < 8; ++i) CHECK(after.effects[i].id == kHammerPairs[i].id);
    for (const size_t i : ChangedOffsets(bytes, written)) CHECK((i - At(hammer)) / kItemEffectStride == 4);

    // The view skips unlisted rows without a gap, and hides the section when nothing is listed.
    const std::string view = ReadText(fs::path(DCP_SOURCE_DIR) / "launcher" / "editor_inventory.cpp");
    const std::string modifiers = FunctionBody(view, "void EditorPage::DrawItemModifiers(");
    CHECK(modifiers.find("if (!line.listed) continue;") < modifiers.find("ui.Row(") && modifiers.find("none_of(") < modifiers.find("Heading(Tr(\"Modifiers\"))"));
    CHECK(modifiers.find("\"-\"") == std::string::npos);
}

// The two DEX modifiers of the Chaos Hammer are two records of one game effect: id 115 always shows 20, id 108 scales 3 to 30.
void TestDexRecords() {
    const ModifierSpec fixed = *ModifierSpecOf(115), scaled = *ModifierSpecOf(108), twin_fixed = *ModifierSpecOf(300), twin_scaled = *ModifierSpecOf(293);
    CHECK(fixed.minimum == 20 && fixed.maximum == 20 && fixed.bonus_maximum == 20 && twin_fixed.minimum == 20 && twin_fixed.bonus_maximum == 20);
    CHECK(scaled.minimum == 3 && scaled.maximum == 30 && scaled.bonus_maximum == 30 && twin_scaled.minimum == 3 && twin_scaled.maximum == 30);
    for (int stored = kModifierStoredMin; stored <= kModifierStoredMax; ++stored) CHECK(ModifierShownValue(fixed, float(stored)) == 20);
    CHECK(!ModifierShownOf(fixed, 95.0f).editable && ModifierShownOf(scaled, 97.0f).editable);
    Bytes bytes = MakeInventorySave();
    const ItemRef ref{0, -1, 0, 41};
    PutItem(bytes, ref, {294, {{115, 95}, {108, 97}}, 448000, 449500, 99, 1, 250, 255, 0});
    Seal(bytes);
    SaveModel model = SaveModel::FromBytes(bytes);
    CHECK(*ItemFieldValue(model, ref, "modifier_0") == 20 && *ItemFieldValue(model, ref, "modifier_1") == 29);
    CHECK(!SetItemField(model, ref, "modifier_0", 30) && SetItemField(model, ref, "modifier_1", 30) && *ItemFieldValue(model, ref, "modifier_0") == 20);
    CHECK(ReadItem(model.bytes(), ref).effects[0].value == 95.0f);
}

// The Required level the editor shows is the byte plus one, as the game's detail panel prints it; the byte is never exposed.
void TestRequiredLevelRoundTrip(const fs::path& base) {
    Bytes bytes = MakeInventorySave();
    const ItemRef weapon{0, -1, 0, 41}, pendant{0, -1, 0, 42}, equipped{0, 0, 2, 2};
    PutItem(bytes, weapon, {294, {{155, 90}}, 448000, 449500, 0, 1, 250, 255, 0});
    PutItem(bytes, pendant, {376, {{294, 100}, {288, 100}, {281, 100}}, 450000, 450000, 0, 1, 5, 5, 1});
    PutItem(bytes, equipped, {376, {{294, 100}}, 450000, 450000, 48, 1, 12, 12, 1});
    Seal(bytes);
    const std::vector<std::pair<ItemRef, std::vector<int>>> matrix = {
        {weapon, {1, 10, 99, 100}}, {pendant, {1, 2, 4, 50, 100}}, {equipped, {1, 2, 4, 50, 100}}};
    int run = 0;
    for (const auto& [ref, levels] : matrix) {
        for (const int level : levels) {
            const fs::path user = base / ("level_round_trip_" + std::to_string(run++));
            const fs::path file = WriteSave(user, bytes);
            SaveEdit edit = OpenEdit(user);
            CHECK(SetItemField(edit.working(), ref, "required_level", level));
            CHECK(*ItemFieldValue(edit.working(), ref, "required_level") == level);
            CHECK(ReadItem(edit.working().bytes(), ref).level_stored == level - 1);
            CHECK(edit.Apply() == (level == ReadItem(bytes, ref).required_level() ? SaveEdit::Result::kNothingToApply : SaveEdit::Result::kApplied));
            const Bytes written = ReadBytes(file);
            CHECK(ReadItem(written, ref).level_stored == level - 1 && ReadItem(written, ref).required_level() == level);
            const SaveModel reopened = SaveModel::FromFile(file);
            CHECK(*ItemFieldValue(reopened, ref, "required_level") == level);
            CHECK(DescribeItem(reopened, reopened, ref, FixtureCatalog()).level == level);
            for (const size_t i : ChangedOffsets(bytes, written)) CHECK(i == At(ref) + kItemLevel);
        }
    }
    CHECK(!SetItemField(*new SaveModel(SaveModel::FromBytes(bytes)), pendant, "required_level", 0) && !SetItemField(*new SaveModel(SaveModel::FromBytes(bytes)), pendant, "required_level", 101));
}

// Each of the eight modifier rows is wired to its own slot and edits the number the game shows; an edit of one reaches EDITED,
// the changed fields, the mismatch check and the saved file, with nothing else moving.
void TestModifierSlots(const fs::path& base) {
    Bytes bytes = MakeInventorySave();
    const ItemRef hammer{0, -1, 0, 41}, twin{0, -1, 0, 42};
    std::vector<Modifier> eight;
    for (const ShownPair& pair : kHammerPairs) eight.push_back({pair.id, pair.stored});
    const Item item{294, eight, 448000, 449500, 99, 1, 250, 255, 0};
    PutItem(bytes, hammer, item);
    PutItem(bytes, twin, item);
    Seal(bytes);
    const SaveModel pristine = SaveModel::FromBytes(bytes);
    std::vector<std::string> keys;
    for (int i = 0; i < 8; ++i) keys.push_back("modifier_" + std::to_string(i));
    const auto shown = [&](const SaveModel& m, const ItemRef& ref) {
        std::vector<int> v;
        for (const std::string& key : keys) v.push_back(*ItemFieldValue(m, ref, key));
        return v;
    };
    const auto stored = [&](const SaveModel& m, const ItemRef& ref) {
        std::vector<float> v;
        for (const ItemEffect& effect : ReadItem(m.bytes(), ref).effects) v.push_back(effect.value);
        return v;
    };
    const auto top = [&](const SaveModel& m) {
        return std::vector<int>{*ItemFieldValue(m, hammer, "attack_min"), *ItemFieldValue(m, hammer, "attack_max"), *ItemFieldValue(m, hammer, "durability"),
                                *ItemFieldValue(m, hammer, "max_durability"), *ItemFieldValue(m, hammer, "required_level")};
    };
    const std::vector<int> start = {46, 20, 27, 9, 29, 26, 14, 29};
    CHECK(shown(pristine, hammer) == start && top(pristine) == (std::vector<int>{250, 255, 4480, 4495, 100}));
    CHECK(!ItemFieldValue(pristine, hammer, "modifier_8") && !ItemFieldValue(pristine, hammer, "modifier_") && !ItemFieldValue(pristine, hammer, "modifier"));

    // The rows: the game's label, a percent sign where the number is one, the range the formula gives, fixed rows read-only.
    const ItemDetail detail = DescribeItem(pristine, pristine, hammer, FixtureCatalog());
    const char* labels[8] = {"Power of Frenzy", "DEX", "Throw damage", "Petrify enemy chance", "DEX", "DMG to human foes", "Critical rate", "Bonus as ice DMG"};
    const bool percents[8] = {true, false, true, true, false, true, true, true};
    const std::pair<int, int> ranges[8] = {{10, 50}, {20, 20}, {10, 40}, {2, 10}, {3, 30}, {10, 40}, {3, 20}, {10, 40}};
    CHECK(detail.modifiers.size() == 8);
    for (size_t i = 0; i < detail.modifiers.size() && i < 8; ++i) {
        const ModifierLine& line = detail.modifiers[i];
        CHECK(line.name == labels[i] && line.percent == percents[i] && line.shown == start[i] && line.known);
        CHECK(line.range.min == ranges[i].first && line.range.max == ranges[i].second && line.editable == (ranges[i].first < ranges[i].second));
        CHECK(line.stored == int(kHammerPairs[i].stored));
    }

    // Every editable slot is addressable on its own: one edit changes that slot's value bytes only, and the number is the one asked for.
    for (size_t slot = 0; slot < 8; ++slot) {
        SaveModel m = pristine;
        if (ranges[slot].first == ranges[slot].second) {
            CHECK(!SetItemField(m, hammer, keys[slot], ranges[slot].first + 1) && stored(m, hammer) == stored(pristine, hammer));
            continue;
        }
        const int want = start[slot] == ranges[slot].first ? ranges[slot].second : ranges[slot].first;
        CHECK(SetItemField(m, hammer, keys[slot], want));
        std::vector<int> expected = start;
        expected[slot] = want;
        CHECK(shown(m, hammer) == expected && top(m) == top(pristine) && shown(m, twin) == start);
        for (const size_t i : ChangedOffsets(Bytes(pristine.bytes().begin(), pristine.bytes().end()), Bytes(m.bytes().begin(), m.bytes().end()))) {
            CHECK(i >= At(hammer) && (i - At(hammer)) / kItemEffectStride == slot && (i - At(hammer)) % kItemEffectStride >= 4);
        }
    }
    // The same number asked for again writes nothing, and a number the stored value already shows keeps it.
    for (size_t slot = 0; slot < 8; ++slot) {
        SaveModel m = pristine;
        CHECK(SetItemField(m, hammer, keys[slot], start[slot]) == (ranges[slot].first < ranges[slot].second));
        CHECK(Bytes(m.bytes().begin(), m.bytes().end()) == Bytes(pristine.bytes().begin(), pristine.bytes().end()));
    }
    // Typing, buttons, a slider drag and the hold all end in the same record: Throw damage 27 to 20.
    {
        SaveModel typed = pristine, stepped = pristine, dragged = pristine;
        TypeInto(typed, hammer, "modifier_2", {10, 40}, "20");
        for (int n = 0; n < 7; ++n) SetItemField(stepped, hammer, "modifier_2", *ItemFieldValue(stepped, hammer, "modifier_2") - 1);
        SetItemField(dragged, hammer, "modifier_2", FineDragValue(27, -7.0f, {10, 40}));
        CHECK(ItemBytes(typed, hammer) == ItemBytes(stepped, hammer) && ItemBytes(stepped, hammer) == ItemBytes(dragged, hammer));
        CHECK(shown(typed, hammer) == (std::vector<int>{46, 20, 20, 9, 29, 26, 14, 29}) && top(typed) == top(pristine));
        // The raw value closest to the current one that shows 20: 85 stays near, 54 is the nearest below it.
        CHECK(ReadItem(typed.bytes(), hammer).effects[2].value == 54.0f);
        // A typed value is in EDITED at once, with no Enter and no click away.
        SaveModel live = pristine;
        TypedFieldEdit typing;
        typing.Begin(hammer, "modifier_2", {10, 30}, ItemBytes(live, hammer));
        typing.Update(live, "2");
        CHECK(*ItemFieldValue(live, hammer, "modifier_2") == 27); // a prefix outside the range leaves the record as it was
        typing.Update(live, "29");
        CHECK(*ItemFieldValue(live, hammer, "modifier_2") == 29 && typing.Active());
    }
    // Three edits change exactly three slots: Power of Frenzy 46 to 30, Throw damage 27 to 15, DMG to human foes 26 to 12.
    {
        SaveModel m = pristine;
        SetItemField(m, hammer, "modifier_0", 30);
        SetItemField(m, hammer, "modifier_2", 15);
        SetItemField(m, hammer, "modifier_5", 12);
        CHECK(shown(m, hammer) == (std::vector<int>{30, 20, 15, 9, 29, 12, 14, 29}) && top(m) == top(pristine) && shown(m, twin) == start);
        std::set<size_t> slots;
        for (const size_t i : ChangedOffsets(Bytes(pristine.bytes().begin(), pristine.bytes().end()), Bytes(m.bytes().begin(), m.bytes().end()))) {
            slots.insert((i - At(hammer)) / kItemEffectStride);
        }
        CHECK(slots == (std::set<size_t>{0, 2, 5}));
    }
    // The visible numbers are compared with EDITED, modifiers included.
    {
        SaveModel m = pristine;
        std::vector<VisibleField> visible;
        for (size_t slot = 0; slot < 8; ++slot) visible.push_back({hammer, keys[slot], start[slot]});
        CHECK(!FindUiMismatch(m, visible));
        SetItemField(m, hammer, "modifier_5", 12);
        const std::optional<UiMismatch> stale = FindUiMismatch(m, visible);
        CHECK(stale && stale->field.key == "modifier_5" && stale->field.shown == 26 && stale->edited == 12);
        visible[5].shown = 12;
        CHECK(!FindUiMismatch(m, visible));
    }
    // The name follows the stored value: the prefix of the strongest modifier is recomputed, nothing else moves.
    {
        const ItemCatalog catalog = FixtureCatalog();
        SaveModel m = pristine;
        SetItemField(m, hammer, "modifier_0", 10);
        CHECK(ItemDisplayName(ReadItem(m.bytes(), hammer), catalog).size() > 0);
        CHECK(top(m) == top(pristine) && shown(m, hammer)[1] == 20);
    }

    // Apply from the active field with no Enter: the file holds the stored value for the number typed, the rest is untouched.
    const fs::path user = base / "modifier_slots";
    const fs::path file = WriteSave(user, bytes);
    SaveEdit edit = OpenEdit(user);
    TypedFieldEdit typing;
    typing.Begin(hammer, "modifier_2", {10, 30}, ItemBytes(edit.working(), hammer));
    typing.Update(edit.working(), "2");
    typing.Update(edit.working(), "20");
    CHECK(shown(edit.working(), hammer) == (std::vector<int>{46, 20, 20, 9, 29, 26, 14, 29}));
    CHECK(edit.Apply() == SaveEdit::Result::kApplied);
    const Bytes written = ReadBytes(file);
    const SaveModel reopened = SaveModel::FromFile(file);
    CHECK(shown(reopened, hammer) == (std::vector<int>{46, 20, 20, 9, 29, 26, 14, 29}) && top(reopened) == top(pristine));
    CHECK(ItemBytes(reopened, twin) == ItemBytes(pristine, twin));
    for (const size_t i : ChangedOffsets(Bytes(pristine.bytes().begin(), pristine.bytes().end()), written)) {
        CHECK((i >= At(hammer) && (i - At(hammer)) / kItemEffectStride == 2 && (i - At(hammer)) % kItemEffectStride >= 4) ||
              i >= written.size() - 64);
    }

    // The source wires each row to its own slot through the one field path, with the range the formula gives.
    const std::string view = ReadText(fs::path(DCP_SOURCE_DIR) / "launcher" / "editor_inventory.cpp");
    const std::string modifiers = FunctionBody(view, "void EditorPage::DrawItemModifiers(");
    CHECK(modifiers.find("\"modifier_\" + std::to_string(line.slot), line.range") != std::string::npos && modifiers.find("SetModifierValue") == std::string::npos);
    CHECK(modifiers.find("kModifierValueMin") == std::string::npos && modifiers.find("line.percent") != std::string::npos);
}

// Typed edits evaluate every text from the record as it was when typing began: the final number decides the result.
void TestTypedTransactions(const fs::path& base) {
    Bytes bytes = MakeInventorySave();
    const ItemRef hammer{0, -1, 0, 41}, twin{0, -1, 0, 42};
    const Item item{283, {{155, 95}, {115, 99}, {30, 98}, {3, 97}}, 205100, 450000, 94, 1, 200, 250, 1};
    PutItem(bytes, hammer, item);
    PutItem(bytes, twin, item);
    Seal(bytes);
    const SaveModel pristine = SaveModel::FromBytes(bytes);
    const auto fresh = [&] { return pristine; };
    const auto at = [&](SaveModel& m, const char* key) { return ItemFieldValue(m, hammer, key); };
    const auto changed_fields = [&](const SaveModel& m) {
        std::vector<size_t> fields;
        for (const size_t i : ChangedOffsets(bytes, Bytes(m.bytes().begin(), m.bytes().end()))) fields.push_back(i - At(hammer));
        return fields;
    };

    // Required level 99 typed over 95: the intermediate 9 changes only the level, and so does the final 99.
    {
        SaveModel m = fresh();
        TypedFieldEdit typing;
        typing.Begin(hammer, "required_level", {1, 100}, ItemBytes(m, hammer));
        typing.Update(m, "9");
        CHECK(at(m, "required_level") == 9 && at(m, "attack_max") == 250 && at(m, "attack_min") == 200);
        typing.Update(m, "99");
        CHECK(at(m, "required_level") == 99 && at(m, "attack_min") == 200 && at(m, "attack_max") == 250 && at(m, "durability") == 2051 && at(m, "max_durability") == 4500);
        CHECK(changed_fields(m) == std::vector<size_t>{kItemLevel});
        typing.Finish(m, "99");
        CHECK(!typing.Active() && changed_fields(m) == std::vector<size_t>{kItemLevel});
    }
    // 50: the level alone, the same as setting it directly; Attack is left for the user.
    {
        SaveModel m = fresh(), direct = fresh();
        TypeInto(m, hammer, "required_level", {1, 100}, "50");
        CHECK(SetItemRequiredLevel(direct, hammer, 50) && ItemBytes(m, hammer) == ItemBytes(direct, hammer));
        CHECK(at(m, "attack_max") == 250 && at(m, "required_level") == 50);
    }
    // Attack max 2 -> 25 -> 255: Attack min is never lowered by the prefixes, the level follows the final 255.
    {
        SaveModel m = fresh();
        TypedFieldEdit typing;
        typing.Begin(hammer, "attack_max", {0, 255}, ItemBytes(m, hammer));
        typing.Update(m, "2");
        CHECK(at(m, "attack_max") == 2 && at(m, "attack_min") == 2);
        typing.Update(m, "25");
        typing.Update(m, "255");
        CHECK(at(m, "attack_min") == 200 && at(m, "attack_max") == 255 && at(m, "required_level") == 95);
        typing.Finish(m, "255");
        CHECK(at(m, "attack_min") == 200 && at(m, "attack_max") == 255 && at(m, "durability") == 2051);
    }
    // Max durability 4 -> 45 -> 450 -> 4500: the low maximums do not leave the current durability clamped.
    {
        SaveModel m = fresh();
        SetItemDurability(m, hammer, 2074);
        const Bytes with_2074(m.bytes().begin(), m.bytes().end());
        TypedFieldEdit typing;
        typing.Begin(hammer, "max_durability", {1, 4500}, ItemBytes(m, hammer));
        typing.Update(m, "4");
        CHECK(at(m, "max_durability") == 4 && at(m, "durability") == 4);
        typing.Update(m, "45");
        typing.Update(m, "450");
        CHECK(at(m, "durability") == 450);
        typing.Update(m, "4500");
        typing.Finish(m, "4500");
        CHECK(at(m, "max_durability") == 4500 && at(m, "durability") == 2074 && Bytes(m.bytes().begin(), m.bytes().end()) == with_2074);
    }
    // A modifier typed as 2 then 20 is the same as 20; only its own slot differs from the start.
    {
        SaveModel m = fresh(), direct = fresh();
        TypeInto(m, hammer, "modifier_2", {10, 30}, "20");
        SetModifierShown(direct, hammer, 2, 20);
        CHECK(ItemBytes(m, hammer) == ItemBytes(direct, hammer));
        for (const size_t field : changed_fields(m)) CHECK(field / kItemEffectStride == 2 && field % kItemEffectStride >= 4);
        const ItemCatalog catalog = FixtureCatalog();
        TypedFieldEdit typing;
        SaveModel n = fresh();
        typing.Begin(hammer, "modifier_2", {10, 30}, ItemBytes(n, hammer));
        typing.Update(n, "2");
        typing.Update(n, "20");
        CHECK(ItemDisplayName(ReadItem(n.bytes(), hammer), catalog) == ItemDisplayName(ReadItem(direct.bytes(), hammer), catalog));
    }
    // Backspace to empty and typing again, replacement, and Escape.
    {
        SaveModel m = fresh();
        const Bytes start = ItemBytes(m, hammer);
        TypedFieldEdit typing;
        typing.Begin(hammer, "required_level", {1, 100}, start);
        typing.Update(m, "9");
        typing.Update(m, "");
        CHECK(ItemBytes(m, hammer) == start); // empty text is local: the record is the snapshot again
        typing.Update(m, "-");
        CHECK(ItemBytes(m, hammer) == start);
        typing.Update(m, "99");
        CHECK(at(m, "required_level") == 99 && at(m, "attack_max") == 250);
        typing.Update(m, "5");
        typing.Update(m, "50");
        CHECK(at(m, "required_level") == 50);
        typing.Cancel(m); // Escape
        CHECK(ItemBytes(m, hammer) == start && !typing.Active());
        TypedFieldEdit again;
        again.Begin(hammer, "required_level", {1, 100}, ItemBytes(m, hammer));
        again.Update(m, "99");
        again.Finish(m, "");
        CHECK(ItemBytes(m, hammer) == start); // finishing with nothing typed keeps the item as it was
        again.Begin(hammer, "durability", {0, 4500}, ItemBytes(m, hammer));
        again.Finish(m, "9999"); // an out-of-range number finishes clamped
        CHECK(at(m, "durability") == 4500);
    }
    // Moving on: the second field takes its own snapshot from the result of the first, and an item switch ends the edit.
    {
        SaveModel m = fresh();
        TypedFieldEdit first;
        first.Begin(hammer, "attack_max", {0, 255}, ItemBytes(m, hammer));
        first.Update(m, "255");
        first.Finish(m, "255");
        CHECK(at(m, "attack_max") == 255 && at(m, "required_level") == 95);
        TypedFieldEdit second;
        second.Begin(hammer, "durability", {0, 4500}, ItemBytes(m, hammer));
        second.Update(m, "2074");
        second.Update(m, "20");
        CHECK(at(m, "attack_max") == 255 && at(m, "required_level") == 95 && at(m, "durability") == 20);
        second.Update(m, "2074");
        second.Cancel(m);
        CHECK(at(m, "attack_max") == 255 && at(m, "required_level") == 95 && at(m, "durability") == 2051); // not the original attack
        TypedFieldEdit open;
        open.Begin(hammer, "attack_min", {0, 255}, ItemBytes(m, hammer));
        open.Update(m, "255");
        const Bytes twin_before = ItemBytes(m, twin);
        open.Cancel(m); // another item is selected
        CHECK(at(m, "attack_min") == 200 && ItemBytes(m, twin) == twin_before && !open.Active());
        TypedFieldEdit other;
        other.Begin(twin, "durability", {0, 4500}, ItemBytes(m, twin));
        other.Update(m, "100");
        CHECK(ItemFieldValue(m, twin, "durability") == 100 && at(m, "durability") == 2051 && !other.Is(hammer, "durability") && other.Is(twin, "durability"));
    }
    // Typing then Apply straight away: the edited record already holds the final state and Apply saves exactly it.
    {
        const fs::path user = base / "typed_apply";
        const fs::path file = WriteSave(user, bytes);
        SaveEdit edit = OpenEdit(user);
        TypedFieldEdit typing;
        typing.Begin(hammer, "required_level", {1, 100}, ItemBytes(edit.working(), hammer));
        typing.Update(edit.working(), "9");
        typing.Update(edit.working(), "99");
        const Bytes edited(edit.working().bytes().begin(), edit.working().bytes().end());
        CHECK(*ItemFieldValue(edit.working(), hammer, "required_level") == 99 && *ItemFieldValue(edit.working(), hammer, "attack_max") == 250);
        typing.Finish(edit.working(), "99");
        CHECK(Bytes(edit.working().bytes().begin(), edit.working().bytes().end()) == edited);
        CHECK(edit.Apply() == SaveEdit::Result::kApplied);
        const Bytes written = ReadBytes(file);
        CHECK(ChangedOffsets(edited, written).empty()); // the checksum aside, the file is the edited record
        const ItemRecord disk = ReadItem(written, hammer);
        CHECK(disk.required_level() == 99 && disk.attack_min == 200 && disk.attack_max == 250 && disk.durability == 205100 && disk.durability_max == 450000);
        CHECK(ItemBytes(SaveModel::FromBytes(written), twin) == ItemBytes(pristine, twin));
    }
}

// What the editor shows is what Apply saves: one edited record, changed at once by every input, never rebuilt.
void TestEditedState(const fs::path& base) {
    Bytes bytes = MakeInventorySave();
    const ItemRef hammer{0, -1, 0, 41}, twin{0, -1, 0, 42};
    const Item item{283, {{155, 95}, {115, 99}, {30, 98}, {3, 97}}, 205100, 450000, 94, 1, 200, 250, 1};
    PutItem(bytes, hammer, item);
    PutItem(bytes, twin, item);
    Seal(bytes);
    const fs::path user = base / "edited_state";
    const fs::path file = WriteSave(user, bytes);
    SaveEdit edit = OpenEdit(user);
    SaveModel& edited = edit.working();
    const auto value = [&](const char* key) { return ItemFieldValue(edited, hammer, key); };
    CHECK(value("attack_max") == 250 && value("durability") == 2051 && value("required_level") == 95 && !edit.dirty());

    // Typing, with no Enter and no click away: the edited record holds the number after every valid prefix.
    const std::vector<uint8_t> baseline = ItemBytes(edited, hammer);
    SetItemField(edited, hammer, "attack_max", 2);
    CHECK(value("attack_max") == 2 && value("attack_min") == 2); // what a bare setter does with the prefix ...
    RestoreItemBytes(edited, hammer, baseline);
    TypeInto(edited, hammer, "attack_max", {0, 255}, "255");
    CHECK(value("attack_max") == 255 && value("attack_min") == 200 && value("required_level") == 95); // ... and what typing keeps
    TypeInto(edited, hammer, "attack_min", {0, 255}, "255");
    CHECK(value("attack_min") == 255 && value("attack_max") == 255 && value("required_level") == 95);
    TypeInto(edited, hammer, "durability", {0, 4500}, "2074");
    CHECK(value("durability") == 2074 && value("max_durability") == 4500);
    CHECK(ExactTypedNumber("4500", {0, 4500}) == std::optional<int>(4500) && !ExactTypedNumber("4501", {0, 4500}) && !ExactTypedNumber("", {0, 255}) &&
          !ExactTypedNumber("-", {0, 255}) && !ExactTypedNumber("25x", {0, 255}));
    TypeInto(edited, hammer, "max_durability", {1, 4500}, "4500");
    CHECK(value("max_durability") == 4500 && value("durability") == 2074);
    // The modifier typed into changes its own slot; nothing else moves.
    const Bytes before_modifier(edited.bytes().begin(), edited.bytes().end());
    const std::vector<uint8_t> before_record = ItemBytes(edited, hammer);
    const std::vector<std::optional<int>> others = {value("modifier_0"), value("modifier_1"), value("modifier_3")};
    TypeInto(edited, hammer, "modifier_2", {10, 30}, "20");
    CHECK(value("modifier_2") == 20 && value("modifier_0") == others[0] && value("modifier_1") == others[1] && value("modifier_3") == others[2]);
    for (const size_t i : ChangedOffsets(before_modifier, Bytes(edited.bytes().begin(), edited.bytes().end()))) {
        CHECK(i >= At(hammer) && (i - At(hammer)) / kItemEffectStride == 2 && (i - At(hammer)) % kItemEffectStride >= 4);
    }
    RestoreItemBytes(edited, hammer, before_record);
    // Every input kind reaches the same setter and the same number.
    SaveModel by_buttons = edit.persisted(), by_typing = edit.persisted(), by_slider = edit.persisted();
    for (int step = 0; step < 4; ++step) SetItemField(by_buttons, hammer, "durability", *ItemFieldValue(by_buttons, hammer, "durability") + 1);
    TypeInto(by_typing, hammer, "durability", {0, 4500}, "2055");
    SetItemField(by_slider, hammer, "durability", FineDragValue(2051, 4.0f, {0, 4500}));
    CHECK(ItemBytes(by_buttons, hammer) == ItemBytes(by_typing, hammer) && ItemBytes(by_typing, hammer) == ItemBytes(by_slider, hammer));
    CHECK(ItemFieldValue(by_buttons, hammer, "max_durability") == 4500);

    // The visible numbers are the edited ones; a stale one is caught before anything is saved.
    std::vector<VisibleField> shown;
    for (const char* key : {"attack_min", "attack_max", "durability", "max_durability", "required_level", "modifier_0", "modifier_3"}) {
        shown.push_back({hammer, key, *ItemFieldValue(edited, hammer, key)});
    }
    CHECK(!FindUiMismatch(edited, shown));
    shown[2].shown = 2051; // the screen still says what the record held before
    const std::optional<UiMismatch> stale = FindUiMismatch(edited, shown);
    CHECK(stale && stale->field.key == "durability" && stale->field.shown == 2051 && stale->edited == 2074);
    CHECK(FindUiMismatch(edited, {{hammer, "modifier_9", 1}}).has_value() && FindUiMismatch(edited, {{twin, "attack_max", 5}}).has_value());

    // Apply serialises the edited record as it is, and what was typed is what lands.
    CHECK(edit.dirty() && ValidateItemChanges(edit.persisted().bytes(), edited.bytes(), *new std::string));
    const Bytes edited_bytes(edited.bytes().begin(), edited.bytes().end());
    CHECK(edit.Apply() == SaveEdit::Result::kApplied);
    const Bytes written = ReadBytes(file);
    CHECK(ChangedOffsets(edited_bytes, written).empty());
    const ItemRecord disk = ReadItem(written, hammer);
    CHECK(disk.attack_min == 255 && disk.attack_max == 255 && disk.durability == 207400 && disk.durability_max == 450000 && disk.required_level() == 95);
    CHECK(disk.effects[2].value == 98.0f && disk.effects[0].value == 95.0f);
    CHECK(ItemBytes(SaveModel::FromBytes(written), twin) == ItemBytes(SaveModel::FromBytes(bytes), twin)); // the identical twin is untouched
    // The saved state becomes the original and the edited record stays identical to it.
    CHECK(!edit.dirty() && ItemBytes(edit.persisted(), hammer) == ItemBytes(edit.working(), hammer));

    // Discard puts the original back; a failed Apply keeps the edited values as they are.
    TypeInto(edit.working(), hammer, "durability", {0, 4500}, "100");
    CHECK(*ItemFieldValue(edit.working(), hammer, "durability") == 100 && edit.dirty());
    edit.Discard();
    CHECK(*ItemFieldValue(edit.working(), hammer, "durability") == 2074 && !edit.dirty());
    edit.working().bytes()[At(hammer) + kItemAttackMax] = 0;
    edit.working().bytes()[At(hammer) + kItemAttackMin] = 9;
    const Bytes failed_edit(edit.working().bytes().begin(), edit.working().bytes().end());
    CHECK(edit.Apply() != SaveEdit::Result::kApplied);
    CHECK(Bytes(edit.working().bytes().begin(), edit.working().bytes().end()) == failed_edit && ReadBytes(file) == written);

    // The editor page has one path from a control to the model, and checks the screen against it before saving.
    const std::string view = ReadText(fs::path(DCP_SOURCE_DIR) / "launcher" / "editor_inventory.cpp");
    const std::string field_page = ReadText(fs::path(DCP_SOURCE_DIR) / "launcher" / "editor_page.cpp");
    const std::string fields = FunctionBody(view, "void EditorPage::DrawItemFields(");
    CHECK(fields.find("SetItem") == std::string::npos && fields.find("EditItem") == std::string::npos && fields.find("ItemField(ui, record.ref, \"attack_max\"") != std::string::npos);
    CHECK(FunctionBody(field_page, "void EditorPage::ItemField(").find("SetItemField(edited, ref, key, value)") != std::string::npos);
    CHECK(FunctionBody(view, "void EditorPage::DrawItemModifiers(").find("SetModifierValue") == std::string::npos);
    const std::string prepare = FunctionBody(field_page, "bool EditorPage::PrepareInventorySave(");
    CHECK(prepare.find("FindUiMismatch(edit_->working(), visible_)") != std::string::npos && prepare.find("return false;") != std::string::npos && prepare.find("ExactTypedNumber(number_buffer_") != std::string::npos);
    const std::string page = ReadText(fs::path(DCP_SOURCE_DIR) / "launcher" / "editor_page.cpp");
    CHECK(page.find("if (!PrepareInventorySave(ui))") < page.find("ApplyAll(edit_ ? &*edit_ : nullptr"));
    CHECK(page.find("const std::optional<int> exact = ExactTypedNumber(number_buffer_, range)") != std::string::npos);
}

// One interaction never changes another field: typed text belongs to its control, a hold steps only the held button, and
// the hidden attack bytes of shields and accessories survive a level change.
void TestFieldIsolation(const fs::path& base) {
    Bytes bytes = MakeInventorySave();
    const ItemRef hammer{0, -1, 0, 41}, other{0, -1, 0, 42};
    const Item item{283, {{155, 95}, {115, 99}, {30, 98}, {3, 97}}, 205100, 450000, 94, 1, 200, 250, 1};
    PutItem(bytes, hammer, item);
    PutItem(bytes, other, item);
    Seal(bytes);
    SaveModel model = SaveModel::FromBytes(bytes);
    const auto draft = [&](const ItemRef& ref) { return DescribeItem(model, model, ref, FixtureCatalog()); };
    const auto record_of = [&](const ItemRef& ref) { return Bytes(model.bytes().begin() + long(At(ref)), model.bytes().begin() + long(At(ref) + kItemRecordSize)); };
    const Bytes other_before = record_of(other);

    // Durability: stepping the current value leaves the maximum, the modifiers and the level alone.
    for (const int expected : {2052, 2053, 2054}) {
        CHECK(SetItemDurability(model, hammer, uint32_t(draft(hammer).durability + 1)) && draft(hammer).durability == expected);
        CHECK(draft(hammer).durability_max == 4500 && draft(hammer).level == 95 && draft(hammer).attack_min == 200);
    }
    // Attack min: a hold of four steps changes Attack only (and the maximum it must not pass).
    for (const int expected : {201, 202, 203, 204}) {
        CHECK(EditItemAttackMin(model, hammer, draft(hammer).attack_min + 1) && draft(hammer).attack_min == expected);
        CHECK(draft(hammer).attack_max == 250 && draft(hammer).level == 95 && draft(hammer).durability == 2054 && draft(hammer).durability_max == 4500);
    }
    // Max durability lowered below the current value takes the current value down with it, never the reverse.
    CHECK(SetItemMaxDurability(model, hammer, 2000) && draft(hammer).durability == 2000 && draft(hammer).durability_max == 2000);
    CHECK(SetItemDurability(model, hammer, 1000) && draft(hammer).durability_max == 2000);
    // A modifier edit changes its own slot only.
    for (int slot = 0; slot < 4; ++slot) {
        const Bytes before = record_of(hammer);
        CHECK(SetModifierValue(model, hammer, slot, 50.0f + float(slot)));
        const Bytes after = record_of(hammer);
        for (size_t i = 0; i < before.size(); ++i) {
            if (before[i] != after[i]) CHECK(i / kItemEffectStride == size_t(slot) && i % kItemEffectStride >= 4);
        }
    }
    CHECK(record_of(other) == other_before);

    // Typed text: each commit reaches the field it was typed into, in the order the controls hand it over.
    struct Setter {
        uint32_t id;
        const char* field;
    };
    std::vector<std::string> calls;
    const auto commit = [&](const char* field, const std::string& text, NumberRange range, int current) {
        if (const std::optional<int> value = CommitTypedNumber(text, range, current)) calls.push_back(std::string(field) + "=" + std::to_string(*value));
    };
    TypedNumberEdit typed;
    typed.Begin(1, "");                       // type 255 in attack_max
    typed.Begin(2, "255");                    // click attack_min: attack_max's text goes to attack_max
    if (auto text = typed.TakeHanded(1)) commit("attack_max", *text, {0, 255}, 250);
    CHECK(!typed.TakeHanded(2) && typed.Editing(2) && !typed.Editing(1));
    typed.Begin(3, "255");                    // type 255 in attack_min, click durability
    if (auto text = typed.TakeHanded(2)) commit("attack_min", *text, {0, 255}, 200);
    CHECK(typed.Editing(3) && !typed.Editing(2));
    commit("durability", "2074", {0, 4500}, 2051); // type 2074 in durability, click Apply: that field ends and commits itself
    typed.End();
    CHECK(!typed.Editing(3) && !typed.TakeHanded(3) && !typed.TakeHanded(4));
    CHECK(calls == (std::vector<std::string>{"attack_max=255", "attack_min=255", "durability=2074"}));
    typed.Begin(5, "");
    typed.Begin(6, "4500"); // another item is selected: what was typed for the first is dropped, never handed on
    typed.Clear();
    CHECK(!typed.TakeHanded(5) && !typed.Editing(6));
    // The same text is never handed to a control with another id.
    typed.Begin(7, "");
    typed.Begin(8, "99");
    CHECK(!typed.TakeHanded(8) && !typed.TakeHanded(9) && typed.TakeHanded(7) == std::optional<std::string>("99"));

    // A plain click on a slider track changes nothing; a drag past the threshold does.
    CHECK(!SliderDragStarted(100.0f, 100.0f, 1.0f) && !SliderDragStarted(100.0f, 103.0f, 1.0f) && !SliderDragStarted(100.0f, 97.5f, 1.0f));
    CHECK(SliderDragStarted(100.0f, 104.0f, 1.0f) && SliderDragStarted(100.0f, 60.0f, 1.0f) && !SliderDragStarted(100.0f, 107.0f, 2.0f) && SliderDragStarted(100.0f, 108.0f, 2.0f));
    const std::string page = ReadText(fs::path(DCP_SOURCE_DIR) / "launcher" / "editor_page.cpp");
    // The threshold is the mouse's: a keyboard or controller tweak has no pointer travel.
    CHECK(page.find("ImGui::IsMouseDown(ImGuiMouseButton_Left) && drag_id_ == field_id && !drag_started_") != std::string::npos);
    CHECK(page.find("SliderDragStarted(drag_press_x_") != std::string::npos && page.find("if (!drag_started_) shown = before;") != std::string::npos);
    CHECK(page.find("typed_.Begin(field_id, number_buffer_)") != std::string::npos && page.find("typed_.TakeHanded(field_id)") != std::string::npos);
    CHECK(page.find("editing_number_") == std::string::npos && page.find("press_id_ != field_id") != std::string::npos);
    const std::string view = ReadText(fs::path(DCP_SOURCE_DIR) / "launcher" / "editor_inventory.cpp");
    CHECK(view.find("typed_item_ != inventory_selection_.item") != std::string::npos);

    // Hidden attack bytes of an accessory and a shield survive a Required level change, through Apply.
    Bytes gear = MakeInventorySave();
    const ItemRef pendant{0, -1, 0, 20}, buckler{0, -1, 0, 21};
    PutItem(gear, pendant, {376, {{291, 100}, {260, 100}, {280, 100}, {251, 100}}, 440100, 450000, 94, 1, 59, 5, 3});
    PutItem(gear, buckler, {349, {}, 100000, 100000, 94, 1, 59, 5, 3});
    Seal(gear);
    const fs::path user = base / "hidden_attack";
    const fs::path file = WriteSave(user, gear);
    SaveEdit edit = OpenEdit(user);
    for (const ItemRef& ref : {pendant, buckler}) {
        CHECK(SetItemRequiredLevel(edit.working(), ref, 10));
        CHECK(edit.working().bytes()[At(ref) + kItemAttackMin] == 59 && edit.working().bytes()[At(ref) + kItemAttackMax] == 5);
    }
    CHECK(SetItemDurability(edit.working(), pendant, 4400));
    CHECK(SetModifierValue(edit.working(), pendant, 0, 99.0f));
    CHECK(edit.Apply() == SaveEdit::Result::kApplied);
    const Bytes written = ReadBytes(file);
    const ItemRecord kept = ReadItem(written, pendant);
    CHECK(kept.attack_min == 59 && kept.attack_max == 5 && kept.required_level() == 10 && kept.durability == 440000 && kept.durability_max == 450000);
    CHECK(kept.effects[0].value == 99.0f && kept.effects[1].value == 100.0f && kept.effects[2].value == 100.0f && kept.effects[3].value == 100.0f);
    CHECK(ReadItem(written, buckler).attack_min == 59 && ReadItem(written, buckler).required_level() == 10);
}

// Typed numbers: what the box shows is committed whether the field ends by Enter or by clicking away (Apply included).
void TestTypedCommit() {
    const NumberRange durability{0, 4500};
    CHECK(CommitTypedNumber("2074", durability, 2050) == std::optional<int>(2074));
    CHECK(CommitTypedNumber("2,074", durability, 2050) == std::optional<int>(2074));
    CHECK(CommitTypedNumber("9999", durability, 2050) == std::optional<int>(4500));
    CHECK(!CommitTypedNumber("2050", durability, 2050) && !CommitTypedNumber("", durability, 2050) && !CommitTypedNumber("abc", durability, 2050));
    const std::string page = ReadText(fs::path(DCP_SOURCE_DIR) / "launcher" / "editor_page.cpp");
    CHECK(page.find("const bool finished = entered || ImGui::IsItemDeactivated();") != std::string::npos);
    CHECK(page.find("CommitTypedNumber(number_buffer_, range, *value)") != std::string::npos);
}

// Every way of proposing a number ends in the model's setter, and the control re-reads the draft; a hold repeats one step.
void TestInputPaths(const fs::path& base) {
    Bytes bytes = MakeInventorySave();
    const ItemRef ref{0, -1, 0, 41};
    PutItem(bytes, ref, {283, {{0x12, 40}}, 100000, 100000, 0, 1, 5, 5, 0});
    Seal(bytes);
    SaveModel model = SaveModel::FromBytes(bytes);
    const auto draft = [&] { return DescribeItem(model, model, ref, FixtureCatalog()); };
    // The proposals the control can make: a button step, a hold repeating it, a drag, a Shift drag and typed text.
    const NumberRange attack{0, 255};
    int proposed = draft().attack_max;
    for (int tick = 0; tick < 5; ++tick) {
        const int before = proposed;
        proposed = StepNumber(proposed, 1, attack);
        CHECK(proposed == before + 1);
        CHECK(EditItemAttackMax(model, ref, proposed) && draft().attack_max == proposed);
    }
    CHECK(proposed == 10 && draft().attack_max == 10);
    for (const int value : {100, 101, 102, 103}) { // a hold on + : one setter call per repeat, never one per click
        CHECK(EditItemAttackMax(model, ref, value) && draft().attack_max == value && draft().attack_min <= value);
    }
    const int dragged = FineDragValue(draft().attack_max, 3.0f, attack);
    CHECK(dragged == 106 && EditItemAttackMax(model, ref, dragged) && draft().attack_max == 106);
    CHECK(ParseNumber("255", attack) == std::optional<int>(255) && EditItemAttackMax(model, ref, *ParseNumber("255", attack)));
    CHECK(draft().attack_max == 255 && draft().level == 1); // the level did not follow
    CHECK(ParseNumber("999", attack) == std::optional<int>(255) && ParseNumber("x", attack) == std::nullopt);
    // Durability and Max durability: one unit per step, 199 -> 200 -> 201, held repeats included.
    const NumberRange durability{0, draft().durability_max};
    int value = 199;
    for (const int expected : {199, 200, 201, 202}) {
        CHECK(value == expected && SetItemDurability(model, ref, uint32_t(value)) && draft().durability == expected);
        value = StepNumber(value, 1, durability);
    }
    CHECK(SetItemMaxDurability(model, ref, 4500) && SetItemDurability(model, ref, 2074) && ReadItem(model.bytes(), ref).durability == 207400 && draft().durability == 2074);
    // Required level through its setter changes only the level; a refused proposal leaves the draft as the control will show it.
    CHECK(SetItemRequiredLevel(model, ref, 1) && draft().attack_max == 255 && draft().level == 1);
    CHECK(SetItemRequiredLevel(model, ref, 99) && draft().attack_max == 255 && draft().level == 99);
    CHECK(!SetItemRequiredLevel(model, ref, 0) && draft().level == 99);
    CHECK(!SetItemDurability(model, ref, 4501) && draft().durability == 2074);
    std::string error;
    CHECK(ValidateItemChanges(bytes, Bytes(model.bytes().begin(), model.bytes().end()), error));
    // The same edits through Apply read back identically.
    const fs::path user = base / "input_paths";
    const fs::path file = WriteSave(user, bytes);
    SaveEdit edit = OpenEdit(user);
    CHECK(EditItemAttackMax(edit.working(), ref, 255) && SetItemMaxDurability(edit.working(), ref, 4500) && SetItemDurability(edit.working(), ref, 2074));
    CHECK(edit.Apply() == SaveEdit::Result::kApplied);
    const ItemRecord disk = ReadItem(ReadBytes(file), ref);
    CHECK(disk.attack_max == 255 && disk.required_level() == 1 && disk.durability == 207400);
}

// Attack and the Required level are edited independently: neither setter moves the other, and Apply accepts any pair.
void TestAttackAndLevelIndependent() {
    Bytes bytes = MakeInventorySave();
    const ItemRef ref{0, -1, 0, 41};
    PutItem(bytes, ref, {283, {{0x12, 40}}, 100000, 100000, 0, 1, 5, 5, 0});
    Seal(bytes);
    SaveModel model = SaveModel::FromBytes(bytes);
    const auto read = [&] { return ReadItem(model.bytes(), ref); };
    const auto now = [&] { return Bytes(model.bytes().begin(), model.bytes().end()); };
    std::string error;
    CHECK(read().required_level() == 1 && read().attack_min == 5 && read().attack_max == 5);
    CHECK(EditItemAttackMax(model, ref, 255));
    CHECK(read().attack_max == 255 && read().attack_min == 5 && read().required_level() == 1);
    CHECK(EditItemAttackMin(model, ref, 255));
    CHECK(read().attack_min == 255 && read().attack_max == 255 && read().required_level() == 1);
    CHECK(ValidateItemChanges(bytes, now(), error));
    // Only the attack bytes changed, never the level.
    for (const size_t i : ChangedOffsets(bytes, now())) CHECK(i == At(ref) + kItemAttackMin || i == At(ref) + kItemAttackMax);

    // Attack 255-255 with Required level 10, 1 and 100: each is accepted and written as chosen.
    for (const int level : {10, 1, 100, 40, 99}) {
        const Bytes before = now();
        CHECK(SetItemRequiredLevel(model, ref, level));
        CHECK(read().required_level() == level && read().attack_min == 255 && read().attack_max == 255);
        CHECK(ChangedOffsets(before, now()).size() <= 1);
        CHECK(ValidateItemChanges(bytes, now(), error));
    }
    // The pair stays ordered, inside the whole byte, at any level.
    CHECK(SetItemRequiredLevel(model, ref, 1) && EditItemAttackMin(model, ref, 100) && read().attack_min == 100 && read().attack_max == 255);
    CHECK(EditItemAttackMax(model, ref, 20) && read().attack_max == 20 && read().attack_min == 20 && read().required_level() == 1);
    CHECK(EditItemAttackMax(model, ref, 0) && read().attack_min == 0 && read().attack_max == 0);
    CHECK(!EditItemAttackMax(model, ref, 256) && !EditItemAttackMin(model, ref, -1) && !EditItemAttackMin(model, {0, -1, 0, 2}, 5));
    CHECK(SetItemRequiredLevel(model, ref, 60) && EditItemAttackMax(model, ref, 255) && read().required_level() == 60);
    CHECK(DescribeItem(model, model, ref, FixtureCatalog()).has_attack);
}

// A picture is one copy of the weapon: the last track of the throw animation, minus the trail's fading repeats.
void TestItemPictureStructure(const fs::path& app0, CharacterArt& art, const ItemIconSet& set) {
    size_t checked = 0;
    for (const int look : ArtworkLooks()) {
        const std::optional<ItemArtwork> artwork = ArtworkOfLook(look);
        const std::optional<Mbs> mbs = LoadGameMbs(app0 / "DRACRO.CPK", artwork->mbs);
        CHECK(mbs.has_value());
        if (!mbs) continue;
        const MbsAnimation& animation = mbs->animations.front();
        const std::vector<uint32_t> layers = ItemPictureLayers(*mbs);
        CHECK(!layers.empty() && animation.track_count >= 1);
        const MbsTrack& last = mbs->tracks[animation.first_track + animation.track_count - 1];
        const MbsKey& key = mbs->keys[mbs->frames[last.first_frame].key];
        std::set<uint32_t> ids;
        for (const uint32_t layer : layers) {
            CHECK(layer >= key.first_layer && layer < key.first_layer + key.layer_count);
            CHECK(ids.insert(mbs->layers[layer].id).second);
        }
        CHECK(set.artwork.contains(look) && set.artwork.at(look).width > 8);
        const std::optional<SpriteImage> again = art.ItemPicture(artwork->mbs, artwork->ftx);
        CHECK(again && again->rgba == set.artwork.at(look).rgba && again->width == set.artwork.at(look).width);
        ++checked;
    }
    CHECK(checked == 32);

    const std::optional<Mbs> dwarf = LoadGameMbs(app0 / "DRACRO.CPK", "player/dwarf00_arms01_lost.mbs");
    CHECK(dwarf && dwarf->animations.front().track_count == 2);
    if (dwarf) {
        const MbsAnimation& animation = dwarf->animations.front();
        const MbsKey& back = dwarf->keys[dwarf->frames[dwarf->tracks[animation.first_track].first_frame].key];
        const MbsKey& front = dwarf->keys[dwarf->frames[dwarf->tracks[animation.first_track + 1].first_frame].key];
        CHECK(back.first_layer != front.first_layer && back.layer_count == 3 && front.layer_count == 3);
        const std::vector<uint32_t> layers = ItemPictureLayers(*dwarf);
        CHECK(layers.size() == 3 && layers.front() == front.first_layer);
        for (const uint32_t layer : layers) CHECK(layer < back.first_layer || layer >= back.first_layer + back.layer_count);
    }
    // Dwarf arms 02, 06 and 08: a single hammer, roughly square, not the pair both tracks draw.
    for (const int look : {37, 41, 43}) {
        const SpriteImage& picture = set.artwork.at(look);
        CHECK(picture.width > 60 && picture.width <= 80 && picture.height > 60 && picture.height <= 90);
        CHECK(picture.width * 10 < picture.height * 14 && picture.height * 10 < picture.width * 14);
    }
    std::printf("weapon picture structure checked: %zu resources\n", checked);
}

// Accessory icons come from UseItem00: animation 441 + field [5], and the item itself is its one layer on the icon atlas.
void TestAccessoryPictures(const fs::path& app0, const ItemIconSet& set) {
    const std::optional<Mbs> mbs = LoadGameMbs(app0 / "DRACRO.CPK", "player/UseItem00.mbs");
    CHECK(mbs.has_value());
    if (!mbs) return;
    for (int icon = 0; icon < kAccessoryIconCount; ++icon) {
        const size_t animation = size_t(441 + 60 + icon);
        CHECK(animation < mbs->animations.size() && mbs->animations[animation].name == AccessoryIconAnimation(icon));
        CHECK(UseItemIconLayers(*mbs, animation).size() == 1);
        CHECK(set.accessories.contains(icon) && set.accessories.at(icon).width > 20 && set.accessories.at(icon).height > 20);
    }
    CHECK(set.accessories.size() == size_t(kAccessoryIconCount));
    std::set<std::vector<uint8_t>> distinct;
    for (const auto& [icon, image] : set.accessories) {
        CHECK(image.rgba.size() == size_t(image.width) * size_t(image.height) * 4);
        distinct.insert(image.rgba);
    }
    CHECK(distinct.size() == set.accessories.size());
    CHECK(set.Find(381) == &set.accessories.at(6) && set.Find(381) != set.Find(382) && set.Find(391) == set.Find(378));
    CHECK(set.Find(403) == &set.accessories.at(72) && set.Find(445) == &set.accessories.at(48) && set.Find(488) == &set.accessories.at(39));
    // Weapons keep their own pictures and what has none keeps its family icon.
    CHECK(set.Find(283) == &set.artwork.at(41) && set.Find(241) == &set.images.at(ItemIconFamily::kBow) && set.Find(112) == &set.images.at(ItemIconFamily::kGeneric));
    std::printf("accessory icons decoded: %zu of %d\n", set.accessories.size(), kAccessoryIconCount);
}

void TestRealIcons(const fs::path& source) {
    const fs::path app0 = source / "game" / "prepared" / "v1.03";
    std::error_code ec;
    if (!fs::is_regular_file(app0 / "DRACRO.CPK", ec)) {
        std::printf("installed game not present, skipping the icon check\n");
        return;
    }
    CharacterArt art(ArtSourcesOf(app0, "DRACRO_US.CPK"));
    const ItemIconSet set = LoadItemIcons(art);
    std::printf("item icons decoded: %zu of %zu\n", set.images.size(), kItemIconFamilyCount);
    for (const auto& [family, image] : set.images) std::printf("  family %d: %dx%d\n", int(family), image.width, image.height);
    CHECK(set.images.size() == kItemIconFamilyCount);
    for (const auto& [family, image] : set.images) {
        CHECK(image.width > 8 && image.height > 8 && image.rgba.size() == size_t(image.width) * size_t(image.height) * 4);
    }
    CHECK(set.Find(283) != nullptr && set.Find(283) != set.Find(445) && set.Find(445) != set.Find(349));
    // The rank badges of stage/Interface.mbs, all distinct pictures.
    CHECK(set.ranks.size() == 6);
    for (const char letter : {'S', 'A', 'B', 'C', 'D', 'E'}) CHECK(set.ranks.contains(letter) && set.ranks.at(letter).width > 4);
    CHECK(!set.ranks.contains('\0'));
    for (const auto& [a, image_a] : set.ranks) {
        for (const auto& [b, image_b] : set.ranks) CHECK(a == b || image_a.rgba != image_b.rgba);
    }
    std::printf("rank badges decoded: %zu\n", set.ranks.size());

    // Every look of the lost-weapon objects decodes to its own picture.
    CHECK(set.artwork.size() == ArtworkLooks().size());
    std::set<std::vector<uint8_t>> distinct;
    for (const auto& [look, image] : set.artwork) {
        CHECK(image.width > 8 && image.height > 8 && image.rgba.size() == size_t(image.width) * size_t(image.height) * 4);
        distinct.insert(image.rgba);
    }
    CHECK(distinct.size() == set.artwork.size());
    CHECK(set.Find(283) == &set.artwork.at(41) && set.Find(283) == set.Find(284) && set.Find(283) != set.Find(282));
    CHECK(set.Find(292) == set.Find(271) && set.Find(218) == set.Find(502) && set.Find(231) != set.Find(224));
    CHECK(set.Find(241) == &set.images.at(ItemIconFamily::kBow) && set.Find(375) == &set.accessories.at(0) && set.Find(0) == &set.images.at(ItemIconFamily::kGeneric));
    std::printf("weapon pictures decoded: %zu of %zu looks\n", set.artwork.size(), ArtworkLooks().size());
    TestItemPictureStructure(app0, art, set);
    TestAccessoryPictures(app0, set);
}

// ---- Real saves (read only, through a copy) ----

struct ModifierStats {
    size_t instances = 0, fractional = 0, negative = 0, non_finite = 0;
    float min = 1e9f, max = -1e9f;
    std::set<uint16_t> ids;
};
ModifierStats g_stats;
bool g_rock_hammer_checked = false;
std::set<std::string> g_real_names;
std::set<uint32_t> g_real_non_gear;
std::set<std::string> g_real_item_names;
size_t g_names_composed = 0, g_names_validated = 0, g_bag_weapons = 0, g_bag_other_first = 0;

struct PictureCount {
    size_t exact = 0, family = 0;
};
std::map<ItemIconFamily, PictureCount> g_pictures;
std::set<int> g_looks_seen;

void CountPicture(const ItemRecord& item) {
    if (item.state != ItemState::kValid || !IsGear(item.definition)) return;
    PictureCount& count = g_pictures[IconFamilyOf(item.definition)];
    const std::optional<int> look = ItemLook(item.definition);
    if ((look && ArtworkOfLook(*look)) || AccessoryIcon(item.definition)) {
        ++count.exact;
        if (look) g_looks_seen.insert(*look);
    } else {
        ++count.family;
    }
}

void CountModifiers(const ItemRecord& item) {
    CountPicture(item);
    if (item.state == ItemState::kValid && item.definition != 0 && !IsGear(item.definition)) {
        g_real_non_gear.insert(item.definition);
        if (g_real_catalog && item.definition == kModifierNamedDefinition && !item.effects.empty()) {
            const std::string name = g_real_catalog->ItemsEntryName(item.effects.front().id);
            CHECK(!name.empty());
            g_real_item_names.insert(name);
            CHECK(ItemDisplayName(item, *g_real_catalog) == (name.starts_with("##(") ? "Special item (" + name.substr(3, name.size() - 4) + ")" : name));
        }
    }
    for (const ItemEffect& effect : item.effects) {
        ++g_stats.instances;
        g_stats.ids.insert(effect.id);
        if (!std::isfinite(effect.value)) {
            ++g_stats.non_finite;
            continue;
        }
        g_stats.fractional += effect.value != std::floor(effect.value);
        g_stats.negative += effect.value < 0.0f;
        g_stats.min = std::min(g_stats.min, effect.value);
        g_stats.max = std::max(g_stats.max, effect.value);
        CHECK(IsValidModifierValue(effect.value));
    }
    if (g_real_catalog && IsGear(item.definition) && !item.effects.empty()) {
        const std::string composed = ItemDisplayName(item, *g_real_catalog);
        const std::string base = g_real_catalog->ItemName(item.definition);
        g_real_names.insert(composed);
        const std::optional<size_t> prefix = NamePrefixIndex(item);
        const bool prefixed = item.name_word == 0xFF && prefix && !g_real_catalog->Prefix(*prefix).empty();
        if (prefixed) {
            ++g_names_composed;
            CHECK(composed.size() > base.size() + 1 && composed.ends_with(" " + base));
        }
        // Items whose in-game names are known independently of this rule.
        if (item.definition == 283 && item.required_level() == 94 && item.effects.size() == 7 && item.grade == 0) {
            ++g_names_validated;
            CHECK(composed == "Demon's Rock Hammer");
        }
        if (item.definition == 284 && item.required_level() == 87 && item.effects.size() == 3 && item.effects[0].id == 0 && item.grade == 1) {
            ++g_names_validated;
            CHECK(composed == "Envenomed Martial Hammer");
        }
    }
    if (item.definition == 283 && item.required_level() == 94 && item.effects.size() == 7 && !g_rock_hammer_checked) {
        g_rock_hammer_checked = true;
        CHECK(HasAttack(item) && item.attack_min == 227 && item.attack_max == 250 && RankLetter(item.grade) == 'S');
    }
    if (item.definition >= kFirstWeapon && item.definition < kFirstShield) CHECK(HasAttack(item));
}

size_t g_real_duplicates = 0, g_real_chaos_pairs = 0;

// Search and sort run on the final game-facing name: "prop" finds the Propitious pendant, "monk" the Monk's necklace.
// A Propitious Rose Pendant of a real save, opened under "prop" and edited until its name changes.
void TestRealPendant(const fs::path& base, const Bytes& original) {
    const SaveModel model = SaveModel::FromBytes(original);
    std::optional<ItemRecord> pendant;
    for (int slot = 0; slot < int(kDataSlotCount) && !pendant; ++slot) {
        for (const ItemRecord& item : ReadStorage(model.bytes(), slot)) {
            if (item.state == ItemState::kValid && ItemDisplayName(item, *g_real_catalog) == "Propitious Rose Pendant") {
                pendant = item;
                break;
            }
        }
    }
    if (!pendant) return;
    const fs::path user = base / "real_pendant";
    const fs::path file = WriteSave(user, original);
    SaveEdit edit = OpenEdit(user);
    const ItemRef ref = pendant->ref;
    const auto accessories = [&](const char* search) {
        return BuildInventoryList(edit.working(), edit.persisted(), ref.slot, 0, *g_real_catalog, Query(InventoryGroup::kAccessories, search));
    };
    const auto find = [&](const char* search) {
        const InventoryList rows = accessories(search);
        return std::find_if(rows.rows.begin(), rows.rows.end(), [&](const InventoryRow& r) { return r.ref == ref; }) != rows.rows.end();
    };
    CHECK(find("prop"));
    size_t slot = 0;
    for (size_t i = 0; i < pendant->effects.size(); ++i) {
        if (pendant->effects[i].value > pendant->effects[slot].value) slot = i;
    }
    CHECK(SetModifierValue(edit.working(), ref, int(slot), pendant->effects[slot].value > 50.0f ? 1.0f : 99.0f));
    const std::string renamed = DescribeItem(edit.working(), edit.persisted(), ref, *g_real_catalog).name;
    CHECK(renamed != "Propitious Rose Pendant" && renamed.ends_with("Rose Pendant"));
    CHECK(find("prop")); // still listed under the search that found it
    CHECK(ResolveSelection(accessories("prop"), ref) == std::optional<ItemRef>(ref));
    edit.Discard();
    CHECK(DescribeItem(edit.working(), edit.persisted(), ref, *g_real_catalog).name == "Propitious Rose Pendant" && find("prop"));
    CHECK(SetModifierValue(edit.working(), ref, int(slot), pendant->effects[slot].value > 50.0f ? 1.0f : 99.0f));
    CHECK(edit.Apply() == SaveEdit::Result::kApplied);
    CHECK(DescribeItem(edit.working(), edit.persisted(), ref, *g_real_catalog).name == renamed);
    // After Apply the saved name no longer matches "prop": the search is cleared and the same record is listed and selected.
    CHECK(SearchKeptAfterSave(edit.persisted(), ref, *g_real_catalog, "prop").empty());
    const InventoryList kept = BuildInventoryList(edit.working(), edit.persisted(), ref.slot, 0, *g_real_catalog, Query(InventoryGroup::kAccessories, ""));
    const auto listed = std::find_if(kept.rows.begin(), kept.rows.end(), [&](const InventoryRow& r) { return r.ref == ref; });
    CHECK(listed != kept.rows.end() && listed->name == renamed && !listed->changed && ResolveSelection(kept, ref) == std::optional<ItemRef>(ref));
    CHECK(!find("prop"));
    const std::vector<size_t> changed = ChangedOffsets(original, ReadBytes(file));
    CHECK(!changed.empty());
    for (const size_t i : changed) CHECK(i >= At(ref) && i < At(ref) + kItemRecordSize && (i - At(ref)) / kItemEffectStride == slot && (i - At(ref)) % kItemEffectStride >= 4);
    std::printf("real pendant: '%s' renamed to '%s' under search prop, then discarded and applied\n", "Propitious Rose Pendant", renamed.c_str());
}

// Rampaging Chaos Hammer pair of a real save: the second one at Attack 255, Durability 2074 of 4500, through Apply and a reopen.
void TestRealMaxed(const fs::path& base, const Bytes& original, const ItemRef& first, const ItemRef& second) {
    const fs::path user = base / "real_maxed";
    const fs::path file = WriteSave(user, original);
    SaveEdit edit = OpenEdit(user);
    CHECK(EditItemAttackMin(edit.working(), second, 255));
    CHECK(EditItemAttackMax(edit.working(), second, 255));
    const ItemDetail draft = DescribeItem(edit.working(), edit.persisted(), second, *g_real_catalog);
    CHECK(draft.attack_min == 255 && draft.attack_max == 255 && draft.level == ReadItem(original, second).required_level()); // what the UI shows is what the draft holds
    CHECK(!ClampToGameLimits(edit.working(), second));
    CHECK(SetItemMaxDurability(edit.working(), second, 4500) && SetItemDurability(edit.working(), second, 2074));
    CHECK(edit.Apply() == SaveEdit::Result::kApplied);
    const Bytes written = ReadBytes(file);
    const ItemRecord after = ReadItem(written, second);
    CHECK(after.attack_min == 255 && after.attack_max == 255 && after.required_level() == ReadItem(original, second).required_level());
    CHECK(after.durability == 207400 && after.durability_max == 450000);
    for (const size_t i : ChangedOffsets(original, written)) CHECK(i >= At(second) && i < At(second) + kItemRecordSize);
    CHECK(std::equal(written.begin() + long(At(first)), written.begin() + long(At(first) + kItemRecordSize), original.begin() + long(At(first))));
    CHECK(!DescribeItem(edit.working(), edit.persisted(), second, *g_real_catalog).changed);
    // Reopened from the file, the model returns what is on disk, which is what the draft showed.
    const SaveModel reopened = SaveModel::FromFile(file);
    const ItemDetail disk = DescribeItem(reopened, reopened, second, *g_real_catalog);
    CHECK(disk.attack_min == 255 && disk.attack_max == 255 && disk.level == ReadItem(original, second).required_level() && disk.durability == 2074 && disk.durability_max == 4500);
    CHECK(disk.name == draft.name && disk.modifiers.size() == draft.modifiers.size());
    std::string error;
    CHECK(ValidateItemChanges(original, written, error));
}

void TestRealSearch(const Bytes& original) {
    const SaveModel model = SaveModel::FromBytes(original);
    int character = -1;
    for (const CharacterView& view : model.slots()[0].characters) {
        if (view.present) {
            character = view.slot;
            break;
        }
    }
    if (character < 0) return;
    const auto names = [&](InventoryGroup group, const char* search, InventorySort sort) {
        return Names(BuildInventoryList(model, model, 0, character, *g_real_catalog, Query(group, search, sort)));
    };
    const std::tuple<const char*, const char*, InventoryGroup> cases[] = {{"prop", "Propitious Rose Pendant", InventoryGroup::kAccessories},
                                                                        {"monk", "Monk's Opal Necklace", InventoryGroup::kAccessories},
                                                                        {"rampaging", "Rampaging Chaos Hammer", InventoryGroup::kWeaponsShields}};
    for (const auto& [needle, full, group] : cases) {
        const std::vector<std::string> all = names(group, "", InventorySort::kName);
        const std::vector<std::string> found = names(group, needle, InventorySort::kName);
        CHECK(std::all_of(found.begin(), found.end(), [&](const std::string& name) {
            std::string lower = name;
            for (char& ch : lower) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            return lower.find(needle) != std::string::npos;
        }));
        CHECK(std::count(found.begin(), found.end(), std::string(full)) == std::count(all.begin(), all.end(), std::string(full)));
        const std::vector<std::string> by_rank = names(group, needle, InventorySort::kRank);
        CHECK(std::is_permutation(by_rank.begin(), by_rank.end(), found.begin(), found.end()));
    }
}

// The first pair of gear items of a real save that read alike (same name, definition and rank) but sit in two records.
void TestRealDuplicate(const fs::path& base, const Bytes& original) {
    if (!g_real_catalog) return;
    SaveModel model = SaveModel::FromBytes(original);
    std::map<std::string, std::vector<ItemRecord>> groups;
    for (int slot = 0; slot < int(kDataSlotCount); ++slot) {
        std::vector<ItemRecord> items = ReadStorage(model.bytes(), slot);
        for (const CharacterView& character : model.slots()[size_t(slot)].characters) {
            if (!character.present) continue;
            for (ItemRecord& item : ReadEquipment(model.bytes(), slot, character.slot)) items.push_back(std::move(item));
        }
        for (ItemRecord& item : items) {
            if (item.state != ItemState::kValid || !HasAttack(item) || item.attack_min >= item.attack_max) continue;
            groups[std::to_string(slot) + "|" + ItemDisplayName(item, *g_real_catalog) + "|" + std::to_string(item.definition) + "|" + std::to_string(item.grade)].push_back(item);
        }
    }
    std::vector<std::pair<std::string, std::vector<ItemRecord>>> pairs(groups.begin(), groups.end());
    std::stable_partition(pairs.begin(), pairs.end(), [](const auto& group) { return group.first.find("Chaos Hammer") != std::string::npos; });
    for (const auto& [key, items] : pairs) {
        const bool chaos = key.find("Chaos Hammer") != std::string::npos;
        if (items.size() < 2 || (!chaos && g_real_duplicates >= 2) || g_real_chaos_pairs >= 1) continue;
        const ItemRecord& a = items[0];
        const ItemRecord& b = items[1];
        const fs::path user = base / "real_duplicate";
        const fs::path file = WriteSave(user, original);
        SaveEdit edit = OpenEdit(user);
        const int new_min = b.attack_min > 0 ? b.attack_min - 1 : b.attack_min + 1;
        CHECK(SetItemAttack(edit.working(), b.ref, new_min, b.attack_max));
        CHECK(edit.Apply() == SaveEdit::Result::kApplied);
        const Bytes written = ReadBytes(file);
        const std::vector<size_t> changed = ChangedOffsets(original, written);
        CHECK(changed.size() == 1 && changed.front() == At(b.ref) + kItemAttackMin);
        const SaveModel reloaded = SaveModel::FromFile(file);
        CHECK(ReadItem(reloaded.bytes(), b.ref).attack_min == new_min && ReadItem(reloaded.bytes(), a.ref).attack_min == a.attack_min);
        std::printf("real duplicate: %s slot %d/%d/%d/%d and %d/%d/%d/%d, only the second changed (offset %zu)\n", key.c_str(), a.ref.slot, a.ref.character,
                    a.ref.set, a.ref.index, b.ref.slot, b.ref.character, b.ref.set, b.ref.index, changed.empty() ? size_t(0) : changed.front());
        ++g_real_duplicates;
        g_real_chaos_pairs += chaos;
        if (chaos) {
            TestRealSearch(original);
            TestRealPendant(base, original);
            TestRealMaxed(base, original, a.ref, b.ref);
            return;
        }
    }
}

// The byte chain of one item, step by step as the launcher takes it: the UI's frames, the draft, the serialized file, the
// replaced file and the reopened file. Run on a copy of the newest real save, never on the save itself.
void WriteU32Test(std::span<uint8_t> bytes, size_t at, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) bytes[at + i] = uint8_t(value >> (8 * i));
}

void TestRealByteChain(const fs::path& base, const Bytes& original) {
    const SaveModel model = SaveModel::FromBytes(original);
    std::vector<ItemRecord> hammers;
    for (const ItemRecord& item : ReadStorage(model.bytes(), 0)) {
        if (item.state == ItemState::kValid && item.definition == 294) hammers.push_back(item);
    }
    if (hammers.size() < 2) return;
    const ItemRef first = hammers[0].ref, second = hammers[1].ref;
    const fs::path user = base / "byte_chain";
    const fs::path file = WriteSave(user, original);
    SaveEdit edit = OpenEdit(user);
    const size_t at = At(second);
    std::printf("real byte chain: hammers at storage %d (offset %zu) and %d (offset %zu); file %s\n", first.index, At(first), second.index, at, file.string().c_str());
    const auto fields = [&](const Bytes& bytes) { return ReadItem(bytes, second); };
    // The state of the report: Attack 200-250, Durability 2051 of 4500, level 95; then only the three edits it names.
    edit.working().bytes()[at + kItemAttackMin] = 200;
    edit.working().bytes()[at + kItemAttackMax] = 250;
    edit.working().bytes()[at + kItemLevel] = 94;
    WriteU32Test(edit.working().bytes(), at + kItemDurability, 205100);
    WriteU32Test(edit.working().bytes(), at + kItemDurabilityMax, 450000);
    const Bytes before(edit.working().bytes().begin(), edit.working().bytes().end());
    const Bytes record_before(before.begin() + long(at), before.begin() + long(at + kItemRecordSize));
    {
        SaveModel exact = SaveModel::FromBytes(before);
        CHECK(EditItemAttackMin(exact, second, 255) && EditItemAttackMax(exact, second, 255) && SetItemDurability(exact, second, 2074));
        const Bytes record_after(exact.bytes().begin() + long(at), exact.bytes().begin() + long(at + kItemRecordSize));
        const ItemRecord want = ReadItem(exact.bytes(), second);
        CHECK(want.attack_min == 255 && want.attack_max == 255 && want.durability == 207400 && want.durability_max == 450000 && want.required_level() == 95);
        for (size_t i = 0; i < record_after.size(); ++i) {
            if (record_before[i] == record_after[i]) continue;
            CHECK(i == kItemAttackMin || i == kItemAttackMax || i == kItemLevel || (i >= kItemDurability && i < kItemDurability + 4)); // not Max durability, not a modifier
        }
    }
    // UI frames: a hold on + proposes one step per frame, each frame the view clamps and re-reads the draft.
    const auto frame = [&] {
        ClampToGameLimits(edit.working(), second);
        return DescribeItem(edit.working(), edit.persisted(), second, *g_real_catalog);
    };
    int shown = frame().attack_max;
    for (int guard = 0; guard < 300 && shown < 255; ++guard) {
        shown = StepNumber(shown, 1, {0, 255});
        CHECK(EditItemAttackMax(edit.working(), second, shown));
        const ItemDetail now = frame();
        CHECK(now.attack_max == shown);
    }
    CHECK(frame().attack_max == 255 && frame().level == 95);
    int low = frame().attack_min;
    for (int guard = 0; guard < 300 && low < 255; ++guard) {
        low = StepNumber(low, 1, {0, 255});
        CHECK(EditItemAttackMin(edit.working(), second, low) && frame().attack_min == low);
    }
    CHECK(frame().attack_min == 255 && frame().attack_max == 255 && frame().level == 95);
    CHECK(SetItemMaxDurability(edit.working(), second, 4500) && SetItemDurability(edit.working(), second, 2074));
    const ItemDetail draft = frame();
    CHECK(draft.durability == 2074 && draft.durability_max == 4500);
    const Bytes draft_bytes(edit.working().bytes().begin(), edit.working().bytes().end());
    // Serialization writes the draft as it is: the same bytes but for the checksum, with nothing normalised after validation.
    const std::vector<uint8_t> serialized = edit.working().Serialize();
    CHECK(serialized.size() == draft_bytes.size());
    for (const size_t i : ChangedOffsets(draft_bytes, Bytes(serialized.begin(), serialized.end()))) CHECK(false && i);
    std::string error;
    CHECK(ValidateItemChanges(before, draft_bytes, error));
    CHECK(edit.Apply() == SaveEdit::Result::kApplied);
    const Bytes final_file = ReadBytes(file);
    CHECK(final_file == Bytes(serialized.begin(), serialized.end()));
    for (const size_t i : ChangedOffsets(draft_bytes, final_file)) CHECK(false && i);
    const ItemRecord disk = fields(final_file);
    CHECK(disk.attack_min == 255 && disk.attack_max == 255 && disk.durability == 207400 && disk.durability_max == 450000 && disk.required_level() == 95);
    // Reopened, the model shows the same item; the first hammer and every unrelated byte are untouched.
    const SaveModel reopened = SaveModel::FromFile(file);
    const ItemDetail again = DescribeItem(reopened, reopened, second, *g_real_catalog);
    CHECK(again.attack_min == 255 && again.attack_max == 255 && again.durability == 2074 && again.durability_max == 4500 && again.level == 95);
    CHECK(again.modifiers.size() == draft.modifiers.size());
    for (size_t i = 0; i < again.modifiers.size(); ++i) CHECK(again.modifiers[i].stored == draft.modifiers[i].stored);
    for (const size_t i : ChangedOffsets(before, final_file)) CHECK(i >= at && i < at + kItemRecordSize);
    CHECK(std::equal(final_file.begin() + long(At(first)), final_file.begin() + long(At(first) + kItemRecordSize), before.begin() + long(At(first))));
    // The game's own load rules leave these values alone: nothing to clamp.
    SaveModel as_loaded = reopened;
    CHECK(!ClampToGameLimits(as_loaded, second) && !ClampToGameLimits(as_loaded, first));
    // The same by typing, on a fresh copy: the edited record holds each number before Apply exists in the sequence.
    {
        const fs::path typed_user = base / "byte_chain_typed";
        Bytes seeded = original;
        seeded[at + kItemAttackMin] = 200;
        seeded[at + kItemAttackMax] = 250;
        seeded[at + kItemLevel] = 94;
        WriteU32Test(seeded, at + kItemDurability, 205100);
        WriteU32Test(seeded, at + kItemDurabilityMax, 450000);
        Seal(seeded);
        const fs::path typed_file = WriteSave(typed_user, seeded);
        SaveEdit typed_edit = OpenEdit(typed_user);
        SaveModel& edited = typed_edit.working();
        TypeInto(edited, second, "attack_max", {0, 255}, "255");
        TypeInto(edited, second, "attack_min", {0, 255}, "255");
        CHECK(ItemFieldValue(edited, second, "attack_max") == 255 && ItemFieldValue(edited, second, "required_level") == 95);
        TypeInto(edited, second, "durability", {0, 4500}, "2074");
        CHECK(ItemFieldValue(edited, second, "durability") == 2074);
        TypeInto(edited, second, "max_durability", {1, 4500}, "4500");
        TypeInto(edited, second, "required_level", {1, 100}, "99"); // last: the 9 on the way must not damage Attack 255
        CHECK(ItemFieldValue(edited, second, "attack_min") == 255 && ItemFieldValue(edited, second, "attack_max") == 255);
        for (size_t slot = 0; slot < ReadItem(seeded, second).effects.size(); ++slot) {
            CHECK(ReadItem(edited.bytes(), second).effects[slot].value == ReadItem(seeded, second).effects[slot].value);
        }
        const ItemRecord now = ReadItem(edited.bytes(), second);
        CHECK(now.attack_min == 255 && now.attack_max == 255 && now.required_level() == 99 && now.durability == 207400 && now.durability_max == 450000);
        CHECK(typed_edit.Apply() == SaveEdit::Result::kApplied);
        const Bytes typed_written = ReadBytes(typed_file);
        const ItemRecord saved_item = ReadItem(typed_written, second);
        CHECK(saved_item.attack_min == 255 && saved_item.attack_max == 255 && saved_item.required_level() == 99 && saved_item.durability == 207400 &&
              saved_item.durability_max == 450000);
        CHECK(ItemBytes(SaveModel::FromBytes(typed_written), first) == ItemBytes(SaveModel::FromBytes(seeded), first));
    }
    // One modifier typed on the hammer (Attack 255-255, Durability 4490/4495, level 100): only that modifier moves.
    {
        const fs::path one_user = base / "byte_chain_modifier";
        Bytes seeded = original;
        seeded[at + kItemAttackMin] = 255;
        seeded[at + kItemAttackMax] = 255;
        seeded[at + kItemLevel] = 99;
        WriteU32Test(seeded, at + kItemDurability, 449000);
        WriteU32Test(seeded, at + kItemDurabilityMax, 449500);
        Seal(seeded);
        const fs::path one_file = WriteSave(one_user, seeded);
        SaveEdit one = OpenEdit(one_user);
        const ItemRecord start = ReadItem(one.working().bytes(), second);
        CHECK(!start.effects.empty());
        const ItemDetail one_detail = DescribeItem(one.working(), one.persisted(), second, *g_real_catalog);
        size_t slot = one_detail.modifiers.size();
        for (size_t i = 0; i < one_detail.modifiers.size() && slot == one_detail.modifiers.size(); ++i) {
            if (one_detail.modifiers[i].editable) slot = i;
        }
        CHECK(slot < one_detail.modifiers.size());
        if (slot < one_detail.modifiers.size()) {
            const ModifierLine& line = one_detail.modifiers[slot];
            const int want = line.shown == line.range.min ? line.range.max : line.range.min;
            TypeInto(one.working(), second, "modifier_" + std::to_string(slot), line.range, std::to_string(want));
            CHECK(one.Apply() == SaveEdit::Result::kApplied);
            const ItemRecord after_one = ReadItem(ReadBytes(one_file), second);
            CHECK(after_one.attack_min == 255 && after_one.attack_max == 255 && after_one.required_level() == 100 && after_one.durability == 449000 && after_one.durability_max == 449500);
            const SaveModel after_model = SaveModel::FromFile(one_file);
            for (size_t i = 0; i < start.effects.size(); ++i) {
                CHECK((after_one.effects[i].value != start.effects[i].value) == (i == slot));
                CHECK(*ItemFieldValue(after_model, second, "modifier_" + std::to_string(i)) == (i == slot ? want : *ItemFieldValue(SaveModel::FromBytes(seeded), second, "modifier_" + std::to_string(i))));
            }
        }
    }
    // An accessory with one modifier edited: only that modifier changes; the name may follow.
    {
        std::optional<ItemRecord> accessory;
        for (const ItemRecord& item : ReadStorage(model.bytes(), 0)) {
            if (item.state == ItemState::kValid && item.definition >= kFirstAccessory && item.definition < kEndGear && item.effects.size() >= 2) {
                accessory = item;
                break;
            }
        }
        if (accessory) {
            const fs::path acc_user = base / "byte_chain_accessory";
            const fs::path acc_file = WriteSave(acc_user, original);
            SaveEdit acc = OpenEdit(acc_user);
            const ItemRef ref = accessory->ref;
            const ItemDetail acc_detail = DescribeItem(acc.working(), acc.persisted(), ref, *g_real_catalog);
            size_t slot = acc_detail.modifiers.size();
            for (size_t i = 0; i < acc_detail.modifiers.size(); ++i) {
                if (acc_detail.modifiers[i].editable) slot = i;
            }
            if (slot < acc_detail.modifiers.size()) {
                const ModifierLine& line = acc_detail.modifiers[slot];
                const int want = line.shown == line.range.min ? line.range.max : line.range.min;
                TypeInto(acc.working(), ref, "modifier_" + std::to_string(slot), line.range, std::to_string(want));
                CHECK(acc.Apply() == SaveEdit::Result::kApplied);
                const ItemRecord now = ReadItem(ReadBytes(acc_file), ref);
                CHECK(now.attack_min == accessory->attack_min && now.attack_max == accessory->attack_max && now.durability == accessory->durability &&
                      now.durability_max == accessory->durability_max && now.level_stored == accessory->level_stored);
                for (size_t i = 0; i < now.effects.size(); ++i) CHECK((i == slot) == (now.effects[i].value != accessory->effects[i].value));
                CHECK(*ItemFieldValue(SaveModel::FromFile(acc_file), ref, "modifier_" + std::to_string(slot)) == want);
            }
        }
    }
    // A write the read-back would not reproduce is refused and leaves the file as it was.
    SaveEdit refused = OpenEdit(user);
    refused.working().bytes()[At(second) + kItemAttackMax] = 0;
    refused.working().bytes()[At(second) + kItemAttackMin] = 7;
    refused.working().bytes()[At(second) + 0x30] ^= 0x01;
    const Bytes disk_before = ReadBytes(file);
    CHECK(refused.Apply() != SaveEdit::Result::kApplied && ReadBytes(file) == disk_before);
}

// The Rampaging Chaos Hammer of a real save (shared storage, 8 modifiers): the numbers the game shows decode from its stored
// values, three edited numbers come back as asked after Apply and a reopen, and only those slots change.
void TestRealHammerDisplay(const fs::path& base, const Bytes& original) {
    const SaveModel model = SaveModel::FromBytes(original);
    std::optional<ItemRef> found;
    for (const ItemRecord& item : ReadStorage(model.bytes(), 0)) {
        if (item.state != ItemState::kValid || item.definition != 294 || item.effects.size() != 8) continue;
        bool same = true;
        for (size_t i = 0; i < 8; ++i) same = same && item.effects[i].id == kHammerPairs[i].id;
        if (same) {
            found = item.ref;
            break;
        }
    }
    if (!found) {
        std::printf("the real Chaos Hammer with the eight known pairs is not in this save, skipping its check\n");
        return;
    }
    const ItemRef hammer = *found;
    const ItemRecord start = ReadItem(original, hammer);
    for (size_t i = 0; i < 8; ++i) CHECK(*ItemFieldValue(model, hammer, "modifier_" + std::to_string(i)) == ModifierShownValue(*ModifierSpecOf(kHammerPairs[i].id), start.effects[i].value));
    if (g_real_catalog) {
        const ItemDetail detail = DescribeItem(model, model, hammer, *g_real_catalog);
        const char* labels[8] = {"Power of Frenzy", "DEX", "Throw damage", "Petrify enemy chance", "DEX", "DMG to human foes", "Critical rate", "Bonus as ice DMG"};
        CHECK(detail.modifiers.size() == 8);
        for (size_t i = 0; i < detail.modifiers.size() && i < 8; ++i) CHECK(detail.modifiers[i].name == labels[i] && detail.modifiers[i].shown == ModifierShownValue(*ModifierSpecOf(kHammerPairs[i].id), start.effects[i].value));
    }

    const fs::path user = base / "real_hammer_display";
    const fs::path file = WriteSave(user, original);
    SaveEdit edit = OpenEdit(user);
    const int wanted[3][2] = {{0, 30}, {2, 15}, {5, 12}}; // Power of Frenzy, Throw damage, DMG to human foes
    for (const auto& [slot, shown] : wanted) CHECK(SetItemField(edit.working(), hammer, "modifier_" + std::to_string(slot), shown));
    const Bytes draft(edit.working().bytes().begin(), edit.working().bytes().end());
    CHECK(edit.Apply() == SaveEdit::Result::kApplied);
    const Bytes written = ReadBytes(file);
    CHECK(ChangedOffsets(draft, written).empty());
    const SaveModel reopened = SaveModel::FromFile(file);
    for (size_t i = 0; i < 8; ++i) {
        int expected = ModifierShownValue(*ModifierSpecOf(kHammerPairs[i].id), start.effects[i].value);
        for (const auto& [slot, shown] : wanted) {
            if (size_t(slot) == i) expected = shown;
        }
        CHECK(*ItemFieldValue(reopened, hammer, "modifier_" + std::to_string(i)) == expected);
    }
    const ItemRecord after = ReadItem(written, hammer);
    for (size_t i = 0; i < 8; ++i) CHECK((after.effects[i].value != start.effects[i].value) == (i == 0 || i == 2 || i == 5));
    CHECK(after.attack_min == start.attack_min && after.attack_max == start.attack_max && after.durability == start.durability &&
          after.durability_max == start.durability_max && after.level_stored == start.level_stored && after.grade == start.grade);
    for (const size_t i : ChangedOffsets(original, written)) {
        CHECK(i >= At(hammer) && i < At(hammer) + kItemRecordSize);
        const size_t slot = (i - At(hammer)) / kItemEffectStride;
        CHECK(i - At(hammer) < 8 * kItemEffectStride && (slot == 0 || slot == 2 || slot == 5) && (i - At(hammer)) % kItemEffectStride >= 4);
    }
}

// The real Fortunate Rose Pendant: the numbers the game shows for an accessory, including the modifier that shows none, and the
// labels the game's text getter gives to modifier ids above 184.
void TestRealAccessoryDisplay(const Bytes& original) {
    if (!g_real_catalog) return;
    const SaveModel model = SaveModel::FromBytes(original);
    const uint16_t ids[5] = {294, 288, 281, 285, 273};
    for (const ItemRecord& item : ReadStorage(model.bytes(), 0)) {
        if (item.state != ItemState::kValid || item.definition != 376 || item.effects.size() != 5) continue;
        bool same = true;
        for (size_t i = 0; i < 5; ++i) same = same && item.effects[i].id == ids[i] && item.effects[i].value == 100.0f;
        if (!same) continue;
        const ItemDetail detail = DescribeItem(model, model, item.ref, *g_real_catalog);
        const int shown[5] = {30, 0, 30, 50, 20};
        const char* names[5] = {"LUC", "DEX/10 chance to ignore enemy defense", "DMG from demonic foes", "HP recovery effect", "Physical resistance"};
        const bool percent[5] = {false, false, true, true, true}, has_value[5] = {true, false, true, true, true};
        CHECK(detail.modifiers.size() == 5);
        for (size_t i = 0; i < detail.modifiers.size() && i < 5; ++i) {
            CHECK(detail.modifiers[i].shown == shown[i] && detail.modifiers[i].name == names[i]);
            CHECK(detail.modifiers[i].percent == percent[i] && detail.modifiers[i].has_value == has_value[i] && detail.modifiers[i].known);
        }
        CHECK(!detail.modifiers[1].editable); // it shows no number
        const std::pair<int, int> full[5] = {{3, 30}, {0, 0}, {10, 30}, {20, 50}, {5, 20}};
        for (size_t i = 0; i < 5; ++i) {
            CHECK(detail.modifiers[i].range.min == full[i].first && detail.modifiers[i].range.max == full[i].second);
            CHECK(ModifierShownOf(*ModifierSpecOf(ids[i]), 200.0f).maximum == detail.modifiers[i].range.max && detail.modifiers[i].shown == full[i].second);
        }
        return;
    }
}

// The real Chaos Hammer's modifiers reach numbers the stored range 1..100 could not show; three are set to their highest.
void TestRealHammerFullRange(const fs::path& base, const Bytes& original) {
    const SaveModel model = SaveModel::FromBytes(original);
    std::optional<ItemRef> found;
    for (const ItemRecord& item : ReadStorage(model.bytes(), 0)) {
        if (item.state != ItemState::kValid || item.definition != 294 || item.effects.size() != 8) continue;
        bool same = true;
        for (size_t i = 0; i < 8; ++i) same = same && item.effects[i].id == kHammerPairs[i].id;
        if (same) {
            found = item.ref;
            break;
        }
    }
    if (!found) return;
    const ItemRef hammer = *found;
    const ItemDetail detail = DescribeItem(model, model, hammer, FixtureCatalog());
    const int highest[8] = {50, 20, 40, 10, 30, 40, 20, 40};
    const int base_highest[8] = {50, 20, 30, 10, 30, 30, 15, 30};
    for (size_t i = 0; i < 8; ++i) {
        CHECK(detail.modifiers[i].range.max == highest[i] && detail.modifiers[i].editable == (highest[i] > detail.modifiers[i].range.min));
        CHECK(ModifierShownValue(*ModifierSpecOf(kHammerPairs[i].id), 100.0f) == base_highest[i]);
    }
    const fs::path user = base / "real_hammer_full_range";
    const fs::path file = WriteSave(user, original);
    SaveEdit edit = OpenEdit(user);
    const size_t above[4] = {2, 5, 6, 7};
    for (const size_t slot : above) CHECK(SetItemField(edit.working(), hammer, "modifier_" + std::to_string(slot), highest[slot]));
    const bool any_change = edit.dirty();
    CHECK(edit.Apply() == (any_change ? SaveEdit::Result::kApplied : SaveEdit::Result::kNothingToApply));
    const Bytes written = ReadBytes(file);
    const SaveModel reopened = SaveModel::FromFile(file);
    const ItemRecord after = ReadItem(written, hammer), start = ReadItem(original, hammer);
    for (size_t i = 0; i < 8; ++i) {
        const bool edited = (i == 2 || i == 5 || i == 6 || i == 7) && *ItemFieldValue(model, hammer, "modifier_" + std::to_string(i)) != highest[i];
        CHECK(*ItemFieldValue(reopened, hammer, "modifier_" + std::to_string(i)) == ((i == 2 || i == 5 || i == 6 || i == 7) ? highest[i] : *ItemFieldValue(model, hammer, "modifier_" + std::to_string(i))));
        CHECK((after.effects[i].value != start.effects[i].value) == edited);
        CHECK(!edited || after.effects[i].value == 200.0f);
    }
    for (const size_t i : ChangedOffsets(original, written)) {
        CHECK(i >= At(hammer) && (i - At(hammer)) / kItemEffectStride < 8 && (i - At(hammer)) % kItemEffectStride >= 4);
    }
}

// Attack 255-255 with a low Required level is accepted and written as chosen; Attack and the level never change each other.
void TestRequiredLevelFreedom(const fs::path& base, const Bytes& original) {
    const SaveModel model = SaveModel::FromBytes(original);
    std::optional<ItemRef> found;
    for (const ItemRecord& item : ReadStorage(model.bytes(), 0)) {
        if (item.state == ItemState::kValid && IsWeapon(item.definition) && HasAttack(item) && item.definition >= 280) {
            found = item.ref;
            break;
        }
    }
    if (!found) return;
    const ItemRef weapon = *found;
    for (const int level : {10, 1, 100}) {
        const fs::path user = base / ("real_level_" + std::to_string(level));
        const fs::path file = WriteSave(user, original);
        SaveEdit edit = OpenEdit(user);
        CHECK(SetItemField(edit.working(), weapon, "attack_min", 255) && SetItemField(edit.working(), weapon, "attack_max", 255));
        CHECK(SetItemField(edit.working(), weapon, "required_level", level));
        CHECK(*ItemFieldValue(edit.working(), weapon, "attack_min") == 255 && *ItemFieldValue(edit.working(), weapon, "attack_max") == 255);
        CHECK(*ItemFieldValue(edit.working(), weapon, "required_level") == level);
        CHECK(edit.Apply() == SaveEdit::Result::kApplied);
        const ItemRecord disk = ReadItem(ReadBytes(file), weapon);
        CHECK(disk.attack_min == 255 && disk.attack_max == 255 && disk.required_level() == level);
        const ItemRecord before = ReadItem(original, weapon);
        CHECK(disk.durability == before.durability && disk.durability_max == before.durability_max && disk.grade == before.grade);
        for (size_t i = 0; i < before.effects.size(); ++i) CHECK(disk.effects[i].value == before.effects[i].value);
        const SaveModel reopened = SaveModel::FromFile(file);
        CHECK(*ItemFieldValue(reopened, weapon, "attack_min") == 255 && *ItemFieldValue(reopened, weapon, "attack_max") == 255 &&
              *ItemFieldValue(reopened, weapon, "required_level") == level);
    }
}

void TestRealSaves(const fs::path& source, const fs::path& base) {
    std::vector<fs::path> found;
    std::error_code ec;
    const fs::path user = source / "user";
    for (const fs::path& folder : {user / "savedata", user / "save_backups"}) {
        if (!fs::is_directory(folder, ec)) continue;
        for (fs::recursive_directory_iterator it(folder, ec), end; it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && it->file_size(ec) == kSaveFileSize) found.push_back(it->path());
        }
    }
    std::sort(found.begin(), found.end());
    if (found.size() > 12) found.erase(found.begin(), found.end() - 12);
    if (found.empty()) {
        std::printf("real saves not present, skipping their check\n");
        return;
    }
    int index = 0;
    for (const fs::path& path : found) {
        const fs::path copy = base / ("real_" + std::to_string(index++)) / "SAVE0.DAT";
        fs::create_directories(copy.parent_path(), ec);
        fs::copy_file(path, copy, fs::copy_options::overwrite_existing, ec);
        if (ec) continue;
        SaveModel model = SaveModel::FromFile(copy);
        if (!model.valid()) continue;
        const Bytes original(model.bytes().begin(), model.bytes().end());
        for (int slot = 0; slot < int(kDataSlotCount); ++slot) {
            for (const ItemRecord& item : ReadStorage(model.bytes(), slot)) {
                CHECK(item.state == ItemState::kValid);
                CountModifiers(item);
            }
            for (const CharacterView& character : model.slots()[size_t(slot)].characters) {
                if (!character.present) continue;
                for (const ItemRecord& item : ReadEquipment(model.bytes(), slot, character.slot)) {
                    CHECK(item.state == ItemState::kValid);
                    CountModifiers(item);
                    if (item.ref.index == 0 && IsWeapon(item.definition)) {
                        ++g_bag_weapons;
                        CHECK(UsableBy(item.definition) == std::vector<CharacterClass>{character.klass});
                    }
                    if (item.ref.index == 0 && !IsWeapon(item.definition) && item.state == ItemState::kValid && item.definition != 0) ++g_bag_other_first;
                }
            }
        }
        // The first editable gear item takes an edit and the change is accepted as the only one.
        for (const ItemRecord& item : ReadStorage(model.bytes(), 0)) {
            if (!HasEditableDurability(item.definition) || item.durability_max < DurabilityScale(item.definition)) continue;
            CHECK(SetItemDurability(model, item.ref, ShownDurability(item.durability_max, DurabilityScale(item.definition)) / 2));
            std::string error;
            CHECK(ValidateItemChanges(original, Bytes(model.bytes().begin(), model.bytes().end()), error));
            break;
        }
        CHECK(SaveModel::FromBytes(original).Serialize() == original);
        TestRealDuplicate(base, original);
        if (&path == &found.back()) {
            TestRealByteChain(base, original);
            TestRealHammerDisplay(base, original);
            TestRealHammerFullRange(base, original);
            TestRealAccessoryDisplay(original);
            TestRequiredLevelFreedom(base, original);
        }
    }
    std::printf("real modifiers: %zu instances, %zu ids, value %g..%g, %zu fractional, %zu negative, %zu non-finite; rock hammer cross-check %s; %zu composed names, %zu validated against known in-game names\n",
                g_stats.instances, g_stats.ids.size(), double(g_stats.min), double(g_stats.max), g_stats.fractional, g_stats.negative,
                g_stats.non_finite, g_rock_hammer_checked ? "done" : "item not in these saves", g_names_composed, g_names_validated);
    std::printf("real bags: %zu slot-0 weapons, every one of the character's own class; %zu slot-0 items that are not weapons\n", g_bag_weapons, g_bag_other_first);
    if (g_bag_weapons > 0) CHECK(g_bag_other_first == 0);
    // Every non-equipment record of the real saves is an Items entry (definition 112), and each is named by the game's table.
    CHECK(g_real_non_gear.empty() || (g_real_non_gear.size() == 1 && *g_real_non_gear.begin() == kModifierNamedDefinition));
    // The names the game shows for the accessories and weapons of the reported saves, never the earlier wrong prefixes.
    CHECK(!g_real_names.contains("Astute Opal Necklace") && !g_real_names.contains("Auspicious Rose Pendant"));
    if (g_real_chaos_pairs > 0) {
        size_t found_known = 0;
        for (const char* known : {"Propitious Rose Pendant", "Monk's Opal Necklace", "Fireproof Temple Necklace", "Phasing Iron Arm",
                                  "Wyrmreaper Battle Arm", "Wyrmreaper Titan Arm", "Godly Hide Gauntlet", "Last Stand Iron Arm",
                                  "Valkyrie's Lord Belt", "Immunizing Leather Belt", "Rampaging Chaos Hammer", "Veteran's War Hammer",
                                  "Scorching Rock Hammer", "Demon's Rock Hammer", "Envenomed Martial Hammer", "Psychotic Chaos Axe",
                                  "Veteran's Battle Scythe", "Butcher's Colossal Axe", "Monk's Strike Axe"}) {
            found_known += g_real_names.contains(known);
        }
        std::printf("real names: %zu of 19 in-game names composed from the saves present; %zu distinct Items entries named\n", found_known, g_real_item_names.size());
    }
    size_t exact = 0, family = 0;
    for (const auto& [kind, count] : g_pictures) {
        exact += count.exact, family += count.family;
        std::printf("real pictures, family %d: %zu exact, %zu family icon\n", int(kind), count.exact, count.family);
        const bool lost_weapon = kind == ItemIconFamily::kSword || kind == ItemIconFamily::kAxe || kind == ItemIconFamily::kHammer ||
                                 kind == ItemIconFamily::kShield || kind == ItemIconFamily::kAmulet;
        CHECK(lost_weapon ? count.family == 0 : count.exact == 0);
    }
    std::printf("real pictures: %zu gear items, %zu exact (%zu distinct looks), %zu family icon, 0 placeholder\n", exact + family, exact,
                g_looks_seen.size(), family);
}

// ---- Page structure ----

size_t Count(const std::string& text, const std::string& needle) {
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++count;
    return count;
}

std::string FunctionBody(const std::string& text, const std::string& signature) {
    const size_t start = text.find(signature);
    if (start == std::string::npos) return {};
    return text.substr(start, text.find("\n}\n", start) - start);
}

std::vector<std::string> TrLiterals(const std::string& text) {
    std::vector<std::string> found;
    for (size_t at = text.find("Tr(\""); at != std::string::npos; at = text.find("Tr(\"", at + 1)) {
        if (at > 0 && (std::isalnum(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '_')) continue;
        std::string literal;
        size_t i = at + 3;
        while (i < text.size() && text[i] == '"') {
            ++i;
            while (i < text.size() && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < text.size()) ++i;
                literal += text[i++];
            }
            ++i;
            while (i < text.size() && (text[i] == ' ' || text[i] == '\n')) ++i;
        }
        found.push_back(literal);
    }
    return found;
}

void TestLayoutAndSource(const fs::path& source) {
    for (const float scale : {1.0f, 1.5f, 2.25f}) {
        const float scrollbar = 14.0f * scale;
        const float frame = trg::layout::kControlHeight * scale;
        for (const float pane : {260.0f * scale, 420.0f * scale, 700.0f * scale}) {
            const float content = trg::layout::StableContentWidth(pane, scrollbar);
            CHECK(content + scrollbar == pane && content < pane);
            const NumberControlLayout control = LayoutNumberControl(content - 16.0f * scale, frame, 8.0f * scale, scale);
            CHECK(control.total_width <= content - 16.0f * scale + 0.01f && control.total_width + 16.0f * scale + scrollbar <= pane + 0.01f);
            CHECK(control.button_width > 0.0f && control.field_width > 0.0f);
        }
        const float gap = 8.0f * scale;
        for (const float width : {500.0f * scale, 719.0f * scale, 720.0f * scale, 1000.0f * scale, 1600.0f * scale}) {
            const InventoryLayout layout = LayoutInventory(width, gap, scale);
            CHECK(layout.stacked == (width < kInventoryWideWidth * scale));
            if (layout.stacked) {
                CHECK(layout.list_width == width && layout.detail_width == width);
            } else {
                CHECK(layout.list_width + gap + layout.detail_width <= width + 0.01f && layout.list_width > 0.0f && layout.detail_width > layout.list_width * 0.9f);
            }
        }
        for (const float row : {700.0f * scale, 1000.0f * scale, 1600.0f * scale}) {
            const float control = trg::layout::ControlColumnWidth(row, scale);
            const SummaryLayout summary = LayoutCharacterSummary(row, 260.0f * scale, control, trg::layout::kControlHeight * scale, gap, scale);
            const SummaryButtons buttons = SplitSummaryButtons(summary, gap);
            CHECK(buttons.width > 0.0f && buttons.width * 2.0f + gap <= summary.button_width + 0.01f);
            CHECK(buttons.second_x == buttons.width + gap && buttons.second_x + buttons.width <= summary.button_width + 0.01f);
            CHECK(summary.button_x + buttons.second_x + buttons.width <= row + 0.01f);
        }
    }

#ifdef DCP_SOURCE_DIR
    const std::string page = ReadText(source / "launcher" / "editor_page.cpp");
    const std::string view = ReadText(source / "launcher" / "editor_inventory.cpp");
    const std::string editor = FunctionBody(page, "void EditorPage::DrawCharacterEditor(");
    // Edit Inventory sits right after Edit Appearance, in the same column, and leads to the inventory view.
    CHECK(Count(page, "Tr(\"Edit Inventory\")") == 1);
    CHECK(editor.find("IconButton(Tr(\"Edit Appearance\"), [this](int pixels) { return ButtonIconTexture(ButtonIcon::kAppearance, pixels); }") != std::string::npos);
    CHECK(editor.find("IconButton(Tr(\"Edit Inventory\"), [this](int pixels) { return ButtonIconTexture(ButtonIcon::kInventory, pixels); }") != std::string::npos);
    CHECK(ReadText(source / "launcher" / "launcher.cpp").find("Characters, appearance and inventory.") != std::string::npos);
    CHECK(editor.find("Tr(\"Edit Appearance\")") < editor.find("ImGui::SameLine(0.0f, spacing)") &&
          editor.find("ImGui::SameLine(0.0f, spacing)") < editor.find("Tr(\"Edit Inventory\")"));
    CHECK(editor.find("SplitSummaryButtons(layout, spacing)") != std::string::npos && editor.find("RequestInventory();") != std::string::npos);
    CHECK(page.find("DrawChanges(ui);") < page.find("DrawCharacter(ui);")); // Apply / Discard stay above every view
    const std::string route = FunctionBody(page, "void EditorPage::DrawCharacter(");
    CHECK(route.find("nav_.inventory()") != std::string::npos && route.find("DrawInventory(ui)") != std::string::npos);
    CHECK(route.find("nav_.appearance()") < route.find("nav_.inventory()") && route.find("DrawInventory(ui)") < route.find("DrawCharacterEditor(ui)"));
    CHECK(page.find("ResetInventoryView();") != std::string::npos);
    CHECK(FunctionBody(page, "void EditorPage::SelectCharacter(").find("ResetInventoryView();") != std::string::npos);
    CHECK(FunctionBody(page, "void EditorPage::SelectSlot(").find("ResetInventoryView();") != std::string::npos);
    CHECK(FunctionBody(page, "void EditorPage::ApplyChanges(").find("kInvalidInventory") != std::string::npos);

    const std::string draw = FunctionBody(view, "void EditorPage::DrawInventory(");
    CHECK(draw.find("Tr(\"Inventory Editor\"), Tr(\"Back to Character Editor\")") != std::string::npos);
    CHECK(draw.find("nav_.BackToCharacter()") != std::string::npos && draw.find("DrawInventorySummary(") != std::string::npos);
    CHECK(view.find("Segmented(") == std::string::npos && view.find("InventoryFilter") == std::string::npos);
    CHECK(view.find("Equipment set") == std::string::npos && view.find("Tr(\"Bag\")") != std::string::npos);
    CHECK(view.find("BeginCombo(\"##inventory_group\"") != std::string::npos && view.find("InputTextWithHint(\"##inventory_search\"") != std::string::npos &&
          view.find("BeginCombo(\"##inventory_sort\"") != std::string::npos);
    CHECK(view.find("\"Weapons / Shields\"") != std::string::npos && view.find("\"Equipment level\"") != std::string::npos);
    CHECK(draw.find("DrawLoadingView(ui, \"Loading inventory...\")") != std::string::npos && draw.find("CatalogReady()") < draw.find("TitleWithButton("));
    CHECK(Count(view, "DrawLoadingView(") == 1 && view.find("PathArcTo(") == std::string::npos && view.find("AddCircle(") != std::string::npos);
    CHECK(view.find("std::async(std::launch::async") != std::string::npos && view.find(".detach()") == std::string::npos);
    CHECK(view.find("IconButton(") != std::string::npos && view.find("DrawItemPlaceholder(") != std::string::npos);
    const std::string button = FunctionBody(view, "bool IconButton(");
    CHECK(!button.empty() && button.find("AddImage(") != std::string::npos);
    CHECK(button.find("AddPolyline") == std::string::npos && button.find("AddConvexPolyFilled") == std::string::npos && button.find("AddLine") == std::string::npos);
    CHECK(view.find("half_ellipse") == std::string::npos && page.find("RenderFileIcon(") != std::string::npos);
    CHECK(page.find("button_icons_.try_emplace") != std::string::npos && button.find("RenderFileIcon") == std::string::npos);
    const std::string icon_map = ReadText(source / "launcher" / "page_icons.cpp");
    CHECK(icon_map.find("\"save.svg\"") != std::string::npos && icon_map.find("\"editor.svg\"") != std::string::npos);
    const std::string icon_header = ReadText(source / "launcher" / "page_icons.h");
    CHECK(icon_header.find("kAppearanceIcon{\"appearance.svg\"") != std::string::npos && icon_header.find("kInventoryIcon{\"inventory.svg\"") != std::string::npos);
    CHECK(draw.find("BuildInventoryList(") != std::string::npos && draw.find("LayoutInventory(") != std::string::npos);
    CHECK(draw.find("DrawInventoryList(") < draw.find("DrawInventoryDetail("));
    CHECK(view.find("inventory_selection_.Select(row.ref)") != std::string::npos);
    const std::string detail = FunctionBody(view, "void EditorPage::DrawInventoryDetail(");
    CHECK(detail.find("DescribeItem(") != std::string::npos && detail.find("DrawItemFields(") < detail.find("DrawItemModifiers("));
    const std::string fields = FunctionBody(view, "void EditorPage::DrawItemFields(");
    CHECK(fields.find("Tr(\"Max durability\")") != std::string::npos && fields.find("\"max_durability\"") != std::string::npos);
    CHECK(fields.find("Tr(\"Durability\")") < fields.find("Tr(\"Max durability\")") && fields.find("Tr(\"Max durability\")") < fields.find("Tr(\"Required level\")") &&
          fields.find("DrawRank(") < fields.find("Tr(\"Attack min\")"));
    // Required level is the last field; the Modifiers section follows it with no class row.
    CHECK(fields.find("Tr(\"Required level\")") < fields.find("ui.EndRows()") && fields.find("Usable by") == std::string::npos &&
          view.find("class_icons") == std::string::npos && view.find("DrawUsableBy") == std::string::npos);
    CHECK(fields.find("SetItemUsable") == std::string::npos && view.find("Checkbox") == std::string::npos && view.find("icons_.ranks") != std::string::npos);
    CHECK(ReadText(source / "launcher" / "editor_page.h").find("class_textures_") == std::string::npos &&
          ReadText(source / "launcher" / "inventory_model.h").find("usable_by") == std::string::npos);
    CHECK(fields.find("Tr(\"Rank\")") < fields.find("Tr(\"Attack min\")") && fields.find("Tr(\"Attack min\")") < fields.find("Tr(\"Attack max\")") &&
          fields.find("Tr(\"Attack max\")") < fields.find("Tr(\"Durability\")") &&
          fields.find("\"attack_max\"") != std::string::npos && fields.find("\"attack_min\"") != std::string::npos &&
          fields.find("Tr(\"Durability\")") < fields.find("Tr(\"Required level\")"));
    const std::string modifiers = FunctionBody(view, "void EditorPage::DrawItemModifiers(");
    CHECK(modifiers.find("ItemField(ui, detail.record.ref, \"modifier_\"") != std::string::npos &&
          modifiers.find("Stored value") == std::string::npos && modifiers.find("+?") == std::string::npos && view.find("\"?\"") == std::string::npos);
    CHECK(draw.find("ResolveSelection(") != std::string::npos && draw.find("CatalogReady()") < draw.find("BeginCombo("));
    CHECK(draw.find("BuildInventoryList(") != std::string::npos && draw.find("inventory_query_") != std::string::npos);
    CHECK(view.find("LocationLabel(") == std::string::npos && view.find("Tr(\"Show\")") == std::string::npos);
    CHECK(Count(modifiers, "stored directly") == 0 && Count(modifiers, "formulas are decoded") == 0 && modifiers.find("line.shown") != std::string::npos);
    CHECK(Count(view, "Stored value") == 0);
    const std::string draw_list = FunctionBody(view, "void EditorPage::DrawInventoryList(");
    CHECK(draw_list.find("Equipment") == std::string::npos && draw_list.find("TextUnformatted(WhereLabel") == std::string::npos);
    // The Saves page's outer gutter is reserved around both panes, then each pane keeps the same margin inside its scrollbar.
    CHECK(draw.find("const float outer_gutter = std::floor(trg::layout::kContentPadding * scale);") != std::string::npos &&
          draw.find("ImGui::GetContentRegionAvail().x - outer_gutter") != std::string::npos);
    // The panes keep clear of their scrollbar the way the Saves page's list does: an always-present bar and the same margin.
    const std::string saves = ReadText(source / "launcher" / "save_page.cpp");
    CHECK(saves.find("KeepClearOfScrollbar(std::floor(trg::layout::kContentPadding * ui.scale()))") != std::string::npos);
    CHECK(Count(view, "ImGuiWindowFlags_AlwaysVerticalScrollbar);") == 2);
    CHECK(Count(view, "KeepClearOfScrollbar(std::floor(trg::layout::kContentPadding * ui.scale()));") == 2);
    CHECK(view.find("SetNextWindowContentSize") == std::string::npos && view.find("page_margin.h") != std::string::npos);
    // Every Back button carries the arrow, through the cached icon texture.
    CHECK(Count(page, "ButtonIcon::kBack)") == 2 && Count(view, "ButtonIcon::kBack)") == 1);
    CHECK(FunctionBody(page, "bool EditorPage::TitleWithButton(").find("IconButton(") != std::string::npos);
    CHECK(FunctionBody(page, "bool EditorPage::TitleWithButton(").find("ImGui::Button(") == std::string::npos);
    CHECK(!fs::exists(source / "back-light-svgrepo-com.svg") && fs::is_regular_file(source / "assets" / "launcher" / "icons" / "back.svg"));
    CHECK(!fs::exists(source / "chest-games-gaming-svgrepo-com.svg") && fs::is_regular_file(source / "assets" / "launcher" / "icons" / "inventory.svg"));
    CHECK(FunctionBody(view, "void EditorPage::DrawInventoryDetail(").find("TakeScrollReset()") < FunctionBody(view, "void EditorPage::DrawInventoryDetail(").find("DescribeItem("));
    CHECK(FunctionBody(view, "void EditorPage::DrawInventoryDetail(").find("SetScrollY(0.0f)") != std::string::npos && view.find("SetScrollY") == view.rfind("SetScrollY"));
    CHECK(view.find("inventory_item_") == std::string::npos);
    CHECK(page.find("Tr(\"Max HP\")") != std::string::npos && page.find("kInvalidHp") != std::string::npos);
    CHECK(fields.find("\"durability\"") != std::string::npos && fields.find("\"required_level\"") != std::string::npos);
    CHECK(fields.find("apply") == std::string::npos && fields.find("Apply(") == std::string::npos); // nothing is written here
    CHECK(view.find("Apply(") == std::string::npos && view.find("WriteSave") == std::string::npos);
    // Raw record numbers are never shown to the player.
    CHECK(view.find("record.ref.index") == std::string::npos && Count(view, "ref.slot") == 1 && FunctionBody(view, "void PushItemId(").find("ref.slot") != std::string::npos);
    // Items have no stored name to edit: no entry point renames one.
    const std::string data_header = ReadText(source / "launcher" / "inventory_data.h");
    CHECK(data_header.find("SetItemName") == std::string::npos && data_header.find("SetItemWord") == std::string::npos && fields.find("Tr(\"Name\")") == std::string::npos);
    CHECK(view.find("if (group == InventoryGroup::kOther && !has_other) continue;") != std::string::npos);
    CHECK(view.find("RowId(") == std::string::npos && view.find("PushItemId(row.ref)") != std::string::npos && view.find("PushItemId(detail.record.ref)") != std::string::npos);

    // The text the view draws exists in Portuguese (Brazil).
    std::set<std::string> translated;
    for (const localization::UiTextRow& row : localization::PortugueseBrazilUiText().rows) translated.insert(row.english);
    CHECK(localization::PortugueseBrazilUiText().errors.empty());
    std::vector<std::string> needed = TrLiterals(view);
    for (const std::string& text : TrLiterals(page)) {
        if (text.find("nventory") != std::string::npos) needed.push_back(text);
    }
    for (const char* label : {"Other", "Weapon", "Shield", "Accessory", "Unknown item %s", "Unknown modifier %s", "Characters, appearance and inventory.", "Loading inventory...", "The current HP cannot be higher than the maximum HP."}) {
        needed.push_back(label);
    }
    CHECK(needed.size() > 20);
    for (const std::string& text : needed) {
        if (!translated.contains(text)) std::printf("missing Portuguese text: %s\n", text.c_str());
        CHECK(translated.contains(text));
    }
#else
    (void)source;
#endif
}

struct Group {
    const char* name;
    std::function<void()> run;
};

} // namespace

int main(int argc, char** argv) {
    const fs::path base = fs::temp_directory_path() / "dcp_inventory_editor_tests";
    fs::remove_all(base);
    fs::create_directories(base);
#ifdef DCP_SOURCE_DIR
    const fs::path source = DCP_SOURCE_DIR;
#else
    const fs::path source;
#endif
    const bool quick = argc > 1 && std::strcmp(argv[1], "quick") == 0;
    const bool localization_only = argc > 1 && std::strcmp(argv[1], "localization") == 0;
    const std::vector<Group> groups = {
        {"inventory-location", TestLocation},
        {"inventory-parse", [] { TestParse(); TestListGroups(); TestLocationFilter(); }},
        {"inventory-catalog", [&] { TestCatalog(source); }},
        {"inventory-localization", [&] { TestPortugueseNames(source); TestPortugueseFallbacks(source); TestLauncherLanguageCatalogs(source); TestMalformedSheetRecords(base); TestPortugueseSearchAndSort(); }},
        {"inventory-edit", [] { TestDurabilityMath(); TestEditDurability(); TestDurabilityBound(); TestMaxDurability(); TestSliderEditing(); TestAttackAndLevelIndependent(); TestSorting(); TestQuery(); TestClassCompatibility(); TestAttack(); TestScrollReset(); TestNames(); TestModifierEdit(); TestModifierNames(); TestEditLevel(); TestIsolation(); TestDetail(); }},
        {"inventory-safety", [] { TestMalformed(); TestUnknownPreserved(); TestValidateChanges(); TestMutations(); }},
        {"inventory-transaction", [&] { TestTransaction(base); TestFailures(base); TestNavigationAndSwitching(base); TestHp(base); TestHpScope(base); TestDuplicateIdentity(base); TestSavePaths(base, source); TestItemsEntries(); TestGameLimits(); TestDuplicateMaxed(base); TestEffectiveLimits(base); TestRenameKeepsRow(base); TestSearchKeptAfterSave(base); TestInputPaths(base); TestTypedCommit(); TestFieldIsolation(base); TestEditedState(base); TestTypedTransactions(base); TestModifierDisplay(); TestModifierFullRange(); TestDexRecords(); TestListedModifiers(base); TestRequiredLevelRoundTrip(base); TestModifierSlots(base); }},
        {"inventory-ui", [&] { TestLayoutAndSource(source); }},
        {"inventory-icons", [&] { TestIconMapping(); TestItemLook(); TestAccessoryIcon(); TestRank(); TestRealIcons(source); }},
        {"inventory-real-saves", [&] { if (!quick) TestRealSaves(source, base); }},
    };
    for (const Group& group : groups) {
        if (localization_only && std::strcmp(group.name, "inventory-localization") != 0 && std::strcmp(group.name, "inventory-ui") != 0) continue;
        const int checks_before = g_checks;
        const int failures_before = g_failures;
        group.run();
        std::printf("GROUP|%s|%s|checks=%d\n", group.name, g_failures == failures_before ? "PASS" : "FAIL", g_checks - checks_before);
    }
    std::error_code cleanup;
    fs::remove_all(base, cleanup);
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
