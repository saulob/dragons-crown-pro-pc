#include "item_catalog.h"

#include <array>
#include <cstdio>

#include "character_art.h"
#include "inventory_data.h"
#include "modifier_values.h"

namespace dcp {

ItemCategory CategoryOf(uint32_t definition) {
    if (definition >= kFirstWeapon && definition < kFirstShield) return ItemCategory::kWeapon;
    if (definition >= kFirstShield && definition < kFirstAccessory) return ItemCategory::kShield;
    if (definition >= kFirstAccessory && definition < kEndGear) return ItemCategory::kAccessory;
    if (definition == kModifierNamedDefinition) return ItemCategory::kItem;
    return ItemCategory::kOther;
}

ItemIconFamily IconFamilyOf(uint32_t definition) {
    // Each class owns one run of the item list, ending at its Chaos item: sword 187-213, axe 214-240, bow 241-267,
    // hammer 268-294, staff 295-321, rod 322-348.
    if (definition >= 187 && definition <= 213) return ItemIconFamily::kSword;
    if (definition >= 214 && definition <= 240) return ItemIconFamily::kAxe;
    if (definition >= 241 && definition <= 267) return ItemIconFamily::kBow;
    if (definition >= 268 && definition <= 294) return ItemIconFamily::kHammer;
    if (definition >= 295 && definition <= 321) return ItemIconFamily::kStaff;
    if (definition >= 322 && definition <= 348) return ItemIconFamily::kRod;
    if (definition >= kFirstShield && definition < kFirstAccessory) return ItemIconFamily::kShield;
    if (definition >= kFirstAccessory && definition < kEndGear) return ItemIconFamily::kAmulet;
    return ItemIconFamily::kGeneric;
}

const char* IconAnimation(ItemIconFamily family) {
    switch (family) {
    case ItemIconFamily::kSword: return "GENRE_SWORD_0";
    case ItemIconFamily::kAxe: return "GENRE_POLEWEAPON_0";
    case ItemIconFamily::kBow: return "GENRE_BOW_0";
    case ItemIconFamily::kHammer: return "GENRE_HAMMER_0";
    case ItemIconFamily::kStaff: return "GENRE_STAFF_0";
    case ItemIconFamily::kRod: return "GENRE_ROD_0";
    case ItemIconFamily::kShield: return "GENRE_SHIELD_0";
    case ItemIconFamily::kAmulet: return "GENRE_AMULET_0";
    case ItemIconFamily::kGeneric: break;
    }
    return "GENRE_ITEM_0";
}

const char* RankAnimation(char letter) {
    switch (letter) {
    case 'S': return "ICON_RANK_S";
    case 'A': return "ICON_RANK_A";
    case 'B': return "ICON_RANK_B";
    case 'C': return "ICON_RANK_C";
    case 'D': return "ICON_RANK_D";
    case 'E': return "ICON_RANK_E";
    }
    return nullptr;
}

namespace {

// Each run of 24 items shares one look per three items and ends with Chaos items reusing looks of the run.
struct LookRun {
    uint32_t first;
    int base;
    std::array<int, 3> chaos;
    uint32_t chaos_count;
};
constexpr LookRun kLookRuns[] = {
    {187, 4, {2, 6, 7}, 3},  // Fighter swords
    {214, 20, {1, 6, 7}, 3}, // Amazon
    {268, 36, {1, 5, 7}, 3}, // Dwarf
    {349, 12, {6, 7, 0}, 2}, // Fighter shields
};
// The Heaven items: eight items, one per look.
constexpr std::pair<uint32_t, int> kHeavenRuns[] = {{491, 4}, {501, 20}, {521, 36}, {553, 12}};

struct LookFamily {
    int base;
    const char* model;
};
constexpr LookFamily kLookFamilies[] = {
    {4, "player/fighter00_arms"}, {12, "player/fighter00_shield"}, {20, "player/amazon00_arms"}, {36, "player/dwarf00_arms"}};

std::string TwoDigits(int value) { return std::string(1, char('0' + value / 10)) + char('0' + value % 10); }

} // namespace

std::optional<int> ItemLook(uint32_t definition) {
    for (const LookRun& run : kLookRuns) {
        if (definition < run.first) continue;
        const uint32_t index = definition - run.first;
        if (index < 24) return run.base + int(index / 3);
        if (index < 24 + run.chaos_count) return run.base + run.chaos[index - 24];
    }
    for (const auto& [first, base] : kHeavenRuns) {
        if (definition >= first && definition < first + 8) return base + int(definition - first);
    }
    return std::nullopt;
}

namespace {

// Accessory kinds in item-list order; each owns a run of icons and ends with Chaos items that reuse icons of the run.
struct AccessoryRun {
    uint32_t first;
    uint32_t regular;
    int first_icon;
    std::array<int, 12> chaos;
    uint32_t chaos_count;
};
constexpr AccessoryRun kAccessoryRuns[] = {
    {375, 16, 0, {3, 8, 11, 1, 5, 9, 4, 7, 13, 6, 10, 15}, 12}, // pendants
    {403, 8, 72, {4, 6, 7}, 3},                                 // arms
    {414, 8, 64, {2, 5, 7}, 3},                                 // belts
    {425, 8, 24, {5, 7}, 2},                                    // studs
    {435, 8, 16, {3, 7}, 2},                                    // bracelets
    {445, 8, 48, {2, 7, 5}, 3},                                 // boots
    {456, 8, 40, {2, 6, 7}, 3},                                 // mantles
    {467, 8, 56, {3, 7}, 2},                                    // gloves
    {477, 8, 32, {4, 5, 6, 7}, 4},                              // glasses
};

struct AccessoryType {
    int first_icon;
    int count;
    const char* name;
};
constexpr AccessoryType kAccessoryTypes[] = {{0, 16, "Amulet"}, {16, 8, "Bracelet"}, {24, 8, "Earring"}, {32, 8, "EyeGlasses"},
                                             {40, 8, "Mantle"}, {48, 8, "Boots"},    {56, 8, "Glove"},    {64, 8, "Belt"},
                                             {72, 8, "Gauntlet"}};

} // namespace

std::optional<int> AccessoryIcon(uint32_t definition) {
    for (const AccessoryRun& run : kAccessoryRuns) {
        if (definition < run.first) continue;
        const uint32_t index = definition - run.first;
        if (index < run.regular) return run.first_icon + int(index);
        if (index < run.regular + run.chaos_count) {
            return run.first_icon + run.chaos[index - run.regular];
        }
    }
    return std::nullopt;
}

std::string AccessoryIconAnimation(int icon) {
    for (const AccessoryType& type : kAccessoryTypes) {
        if (icon >= type.first_icon && icon < type.first_icon + type.count) return std::string(type.name) + TwoDigits(icon - type.first_icon + 1);
    }
    return {};
}

std::optional<ItemArtwork> ArtworkOfLook(int look) {
    for (const LookFamily& family : kLookFamilies) {
        if (look < family.base || look >= family.base + 8) continue;
        const std::string model = family.model;
        return ItemArtwork{model + "01_lost.mbs", model + TwoDigits(look - family.base + 1) + "_lost.ftx"};
    }
    return std::nullopt;
}

std::vector<int> ArtworkLooks() {
    std::vector<int> looks;
    for (const LookFamily& family : kLookFamilies) {
        for (int i = 0; i < 8; ++i) looks.push_back(family.base + i);
    }
    return looks;
}

const SpriteImage* ItemIconSet::Find(uint32_t definition) const {
    if (const std::optional<int> look = ItemLook(definition)) {
        if (const auto found = artwork.find(*look); found != artwork.end()) return &found->second;
    }
    if (const std::optional<int> icon = AccessoryIcon(definition)) {
        if (const auto found = accessories.find(*icon); found != accessories.end()) return &found->second;
    }
    for (const ItemIconFamily family : {IconFamilyOf(definition), ItemIconFamily::kGeneric}) {
        const auto found = images.find(family);
        if (found != images.end()) return &found->second;
    }
    return nullptr;
}

int ItemIconSet::PictureKey(uint32_t definition) const {
    if (const std::optional<int> look = ItemLook(definition); look && artwork.contains(*look)) return *look;
    if (const std::optional<int> icon = AccessoryIcon(definition); icon && accessories.contains(*icon)) return 100 + *icon;
    for (const ItemIconFamily family : {IconFamilyOf(definition), ItemIconFamily::kGeneric}) {
        if (images.contains(family)) return 1000 + int(family);
    }
    return -1;
}

ItemIconSet LoadItemIcons(CharacterArt& art) {
    std::vector<std::string> names;
    for (size_t i = 0; i < kItemIconFamilyCount; ++i) names.emplace_back(IconAnimation(ItemIconFamily(i)));
    std::map<std::string, SpriteImage> decoded = art.MenuIcons(names);
    ItemIconSet set;
    for (size_t i = 0; i < kItemIconFamilyCount; ++i) {
        const auto found = decoded.find(IconAnimation(ItemIconFamily(i)));
        if (found != decoded.end()) set.images.emplace(ItemIconFamily(i), std::move(found->second));
    }
    std::vector<std::string> interface_names;
    for (const char letter : {'S', 'A', 'B', 'C', 'D', 'E'}) interface_names.emplace_back(RankAnimation(letter));
    std::map<std::string, SpriteImage> badges = art.StageIcons("Interface", interface_names);
    for (const char letter : {'S', 'A', 'B', 'C', 'D', 'E'}) {
        if (const auto found = badges.find(RankAnimation(letter)); found != badges.end()) set.ranks.emplace(letter, std::move(found->second));
    }
    std::vector<std::string> icon_names;
    for (int icon = 0; icon < kAccessoryIconCount; ++icon) icon_names.push_back(AccessoryIconAnimation(icon));
    std::map<std::string, SpriteImage> icons = art.UseItemIcons(icon_names);
    for (int icon = 0; icon < kAccessoryIconCount; ++icon) {
        if (const auto found = icons.find(AccessoryIconAnimation(icon)); found != icons.end()) set.accessories.emplace(icon, std::move(found->second));
    }
    for (const int look : ArtworkLooks()) {
        const std::optional<ItemArtwork> artwork = ArtworkOfLook(look);
        if (std::optional<SpriteImage> picture = art.ItemPicture(artwork->mbs, artwork->ftx)) set.artwork.emplace(look, std::move(*picture));
    }
    return set;
}

std::string TrimText(std::string text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '\r')) text.pop_back();
    size_t start = 0;
    while (start < text.size() && text[start] == ' ') ++start;
    return text.substr(start);
}

std::string CleanEffectText(std::string text) {
    std::string out;
    for (size_t i = 0; i < text.size();) {
        if (text.compare(i, 3, "@c(") == 0) {
            const size_t close = text.find(')', i);
            if (close != std::string::npos) {
                i = close + 1;
                continue;
            }
        }
        if (text.compare(i, 3, "@/c") == 0) {
            i += 3;
            continue;
        }
        if (text.compare(i, 2, "%d") == 0) {
            out += '?';
            i += 2;
            continue;
        }
        if (text.compare(i, 3, "\xEF\xBC\x85") == 0) {
            out += '%';
            i += 3;
            continue;
        }
        out += text[i++];
    }
    return TrimText(std::move(out));
}

std::string ModifierName(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '?') {
            while (!out.empty() && (out.back() == '+' || out.back() == '-')) out.pop_back();
            ++i;
            if (i < text.size() && text[i] == '%') ++i;
            continue;
        }
        out += text[i++];
    }
    std::string collapsed;
    for (char ch : out) {
        if (ch == ' ' && (collapsed.empty() || collapsed.back() == ' ')) continue;
        if (ch == ',' && !collapsed.empty() && collapsed.back() == ' ') collapsed.pop_back();
        collapsed += ch;
    }
    return TrimText(std::move(collapsed));
}

ItemCatalog::ItemCatalog(std::vector<std::string> items, std::vector<std::string> effects, std::vector<std::string> words)
    : items_(std::move(items)), effects_(std::move(effects)), words_(std::move(words)) {}

std::vector<std::string> NameWords(const std::vector<std::string>& system_messages) {
    constexpr size_t kFirstWord = 1485;
    std::vector<std::string> words;
    for (size_t i = 0; i < size_t(kNameWordCount) && kFirstWord + i < system_messages.size(); ++i) words.push_back(system_messages[kFirstWord + i]);
    return words;
}

std::string ItemCatalog::Word(uint8_t index) const { return index < words_.size() ? TrimText(words_[index]) : std::string(); }

std::string ItemCatalog::ItemsEntryName(uint16_t id) const {
    constexpr uint16_t kFirstEntry = 373, kLastEntry = 531, kNameOffset = 1865;
    if (id < kFirstEntry || id > kLastEntry || size_t(id) + kNameOffset >= effects_.size()) return {};
    return CleanEffectText(effects_[size_t(id) + kNameOffset]);
}

std::string ItemCatalog::ItemName(uint32_t definition) const {
    if (definition >= items_.size()) return {};
    return TrimText(items_[definition]);
}

std::string ItemCatalog::Prefix(size_t index) const {
    if (index >= effects_.size()) return {};
    const std::string text = TrimText(effects_[index]);
    return text.find_first_of("%@") == std::string::npos ? text : std::string();
}

namespace {

// Whether the percent sign follows the `slot`-th value of the sheet text.
bool PercentAfterValue(const std::string& text, int slot) {
    size_t at = 0;
    for (int seen = 0; (at = text.find("%d", at)) != std::string::npos; ++seen, at += 2) {
        if (seen != slot) continue;
        size_t next = at + 2;
        while (text.compare(next, 3, "@/c") == 0) next += 3;
        return text.compare(next, 3, "\xEF\xBC\x85") == 0 || text.compare(next, 1, "%") == 0;
    }
    return false;
}

} // namespace

EffectText ItemCatalog::Effect(uint16_t id) const {
    const std::optional<size_t> index = ModifierTextIndex(id);
    if (!index || *index >= effects_.size()) return {};
    std::string text = CleanEffectText(effects_[*index]);
    if (text.empty()) return {};
    std::string name = ModifierName(text);
    const bool has_value = text.find('?') != std::string::npos;
    return {std::move(text), std::move(name), PercentAfterValue(effects_[*index], ModifierSpecOf(id)->slot), has_value, true};
}

std::string UnknownItemName(uint32_t definition) {
    char text[40];
    std::snprintf(text, sizeof(text), "Unknown item 0x%x", definition);
    return text;
}

std::string UnknownEffectName(uint16_t id) {
    char text[40];
    std::snprintf(text, sizeof(text), "Unknown modifier 0x%x", id);
    return text;
}

} // namespace dcp
