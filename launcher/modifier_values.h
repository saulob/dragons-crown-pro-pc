// What the game shows for a gear modifier: its effect record gives a range, and the stored value (0..200) picks the number
// inside it. Reading and writing both go through the game's own arithmetic.
#pragma once

#include <cstdint>
#include <optional>

namespace dcp {

struct ModifierSpec {
    int minimum = 0;       // shown at stored 0
    int maximum = 0;       // shown at stored 100
    int bonus_maximum = 0; // shown at stored 200
    int slot = 0;          // which value of the effect's text takes the number
};

// The effect record of a gear modifier id; nothing for an id the item effect tables do not hold.
std::optional<ModifierSpec> ModifierSpecOf(uint16_t id);
// The CustomItem sheet entry that holds the modifier's text.
std::optional<size_t> ModifierTextIndex(uint16_t id);

inline constexpr int kModifierStoredMin = 0;
inline constexpr int kModifierStoredMax = 200;

// The number the game shows for a stored value.
int ModifierShownValue(const ModifierSpec& spec, float stored);

struct ModifierShown {
    int value = 0;
    int minimum = 0; // lowest and highest number any stored value shows
    int maximum = 0;
    bool editable = false; // the shown number moves with the stored value, which is a whole number in the edit range
};
ModifierShown ModifierShownOf(const ModifierSpec& spec, float stored);

// The stored value that makes the game show `shown`. When several do, the nearest to `current` wins and the lower of two
// equals. A number no stored value shows goes to the nearest shown one, the lower of two equals; it never stays on the
// current number, which `shown` is not, so a step always moves.
int ModifierStoredForShown(const ModifierSpec& spec, int shown, int current);

} // namespace dcp
