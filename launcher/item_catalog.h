// Item definitions as the game's own message sheets describe them: names from ItemList, modifier texts from CustomItem.
// Save instances (inventory_data.h) only carry the definition number; nothing here reads or writes a save.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "save_data.h"
#include "sprite_render.h"

namespace dcp {

enum class ItemCategory { kWeapon, kShield, kAccessory, kItem, kOther };

ItemCategory CategoryOf(uint32_t definition);

// The game's inventory tabs: one icon per item family, from stage/Menu.mbs (GENRE_<name>_0).
enum class ItemIconFamily { kSword, kAxe, kBow, kHammer, kStaff, kRod, kShield, kAmulet, kGeneric };
inline constexpr size_t kItemIconFamilyCount = 9;
ItemIconFamily IconFamilyOf(uint32_t definition);
const char* IconAnimation(ItemIconFamily family);
const char* RankAnimation(char letter);          // nullptr for a rank the game has no badge for

// The look id of the executable's item table: the index of the item's lost-weapon object in the game's resource table.
// Only the Fighter's swords and shields and the Amazon's and Dwarf's weapons have one.
std::optional<int> ItemLook(uint32_t definition);

// A look's resources: every look of a class shares the class's first model and has its own atlas.
struct ItemArtwork {
    std::string mbs;
    std::string ftx;
};
std::optional<ItemArtwork> ArtworkOfLook(int look);
std::vector<int> ArtworkLooks();

// The accessory's icon index (0..79 within the accessories): field [5] of the executable's item table less its first accessory
// entry. The game draws it from the animation of that index in player/UseItem00.mbs.
std::optional<int> AccessoryIcon(uint32_t definition);
// The UseItem00 animation of an accessory icon, e.g. "Amulet01"; empty for an index outside the accessories.
std::string AccessoryIconAnimation(int icon);
inline constexpr int kAccessoryIconCount = 80;

// The pictures decoded once from the game files: an item's own weapon picture when its look has one, otherwise its
// family icon, otherwise the generic one.
struct ItemIconSet {
    std::map<ItemIconFamily, SpriteImage> images;
    std::map<int, SpriteImage> artwork;               // by look id
    std::map<int, SpriteImage> accessories;           // by accessory icon index
    std::map<char, SpriteImage> ranks;                // the game's rank badges, by letter
    const SpriteImage* Find(uint32_t definition) const;
    // Identifies the picture Find returns, for caching its texture; -1 when there is none.
    int PictureKey(uint32_t definition) const;
};

class CharacterArt;
ItemIconSet LoadItemIcons(CharacterArt& art);

// A modifier as one line: the sheet's text with its colour tags removed and every value placeholder shown as "?".
struct EffectText {
    std::string text;
    std::string name; // the same without its value and unit
    bool percent = false;   // the number the game shows is followed by a percent sign
    bool has_value = false; // the text shows a number at all
    bool known = false;
};

class ItemCatalog {
public:
    ItemCatalog() = default;
    // `words` are the ten SystemMsg entries from 1485 the game may put before an item name. `mark_value_gaps`: a modifier whose
    // value stands inside its sentence keeps a "..." there, as a language that words it that way needs.
    ItemCatalog(std::vector<std::string> items, std::vector<std::string> effects, std::vector<std::string> words = {},
                bool mark_value_gaps = false);

    bool loaded() const { return !items_.empty(); }
    // The item list's name for a definition, or "" when the list has none.
    std::string ItemName(uint32_t definition) const;
    EffectText Effect(uint16_t id) const;
    // A name prefix of the modifier sheet, or "" when the entry is missing or is not a plain name.
    std::string Prefix(size_t index) const;
    std::string Word(uint8_t index) const;
    // The name of an Items entry (definition 112), which its first modifier id selects; "" outside 373..531.
    std::string ItemsEntryName(uint16_t id) const;
    // The same sheets in English, so a search by English name keeps finding the item while another language is shown;
    // nullptr when the names shown are the English ones.
    void SetEnglish(ItemCatalog english);
    const ItemCatalog* English() const { return english_.get(); }

private:
    std::vector<std::string> items_;
    std::vector<std::string> effects_;
    std::vector<std::string> words_;
    bool mark_value_gaps_ = false;
    std::shared_ptr<const ItemCatalog> english_;
};

// The name words of the SystemMsg sheet (entries 1485..1494), in the order an item's word byte selects them.
std::vector<std::string> NameWords(const std::vector<std::string>& system_messages);

// The catalog of the sheets in message language `suffix` with the port's `translation` laid over them. A translated catalog
// also holds the English names (ItemCatalog::English).
ItemCatalog LoadItemCatalog(CharacterArt& art, const std::string& suffix, const std::string& translation);

// Everything the Inventory Editor reads from the game files, prepared once per game folder and language.
struct ItemAssets {
    ItemCatalog catalog;
    ItemIconSet icons;
};

// "Unknown item 0x1a4": what stands in for a definition the list does not name.
std::string UnknownItemName(uint32_t definition);
// "Unknown modifier 0x1a4".
std::string UnknownEffectName(uint16_t id);

std::string TrimText(std::string text);
// The text with @c(n) and @/c removed, "%d" shown as "?" and the full-width percent sign as "%".
std::string CleanEffectText(std::string text);
// The modifier's name: its text without the value, its sign and its percent sign. `mark_value_gaps` keeps a "..." where a
// value stood inside a sentence ("Chance of ... to poison").
std::string ModifierName(const std::string& text, bool mark_value_gaps = false);

} // namespace dcp
