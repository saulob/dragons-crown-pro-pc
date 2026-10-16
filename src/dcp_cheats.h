// The cheats: one list for the launcher page, the in-game menu and the saved settings, the session layer the menu
// changes, and the rules the game hooks apply. Logic only: no game memory, no window.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>

namespace dcp::cheats {

enum class Cheat : uint8_t {
    InfiniteHealth,
    InfiniteMana,
    InfiniteLives,
    InfiniteDurability,
    InfiniteArrows,
    InfiniteSkillPoints,
    InfiniteGold,
    MovementSpeed
};
inline constexpr size_t kCheatCount = 8;
inline constexpr bool kCheatDefault = false;

inline constexpr char kInfiniteHealthKey[] = "cheat_infinite_health";
inline constexpr char kInfiniteManaKey[] = "cheat_infinite_mana";
inline constexpr char kInfiniteLivesKey[] = "cheat_infinite_lives";
inline constexpr char kInfiniteDurabilityKey[] = "cheat_infinite_durability";
inline constexpr char kInfiniteArrowsKey[] = "cheat_infinite_arrows";
inline constexpr char kInfiniteSkillPointsKey[] = "cheat_infinite_skill_points";
inline constexpr char kInfiniteGoldKey[] = "cheat_infinite_gold";
inline constexpr char kMovementSpeedKey[] = "cheat_movement_speed_2x";

struct CheatInfo {
    Cheat cheat;
    const char* key;
    const char* label;
    const char* description;
};

inline constexpr CheatInfo kCheats[] = {
    {Cheat::InfiniteHealth, kInfiniteHealthKey, "Infinite Health", "Prevent normal damage from reducing player health"},
    {Cheat::InfiniteMana, kInfiniteManaKey, "Infinite Mana", "Prevent mana from decreasing when casting spells"},
    {Cheat::InfiniteLives, kInfiniteLivesKey, "Infinite Lives", "Prevent remaining lives from decreasing"},
    {Cheat::InfiniteDurability, kInfiniteDurabilityKey, "Infinite Durability",
     "Prevent equipped gear from losing durability"},
    {Cheat::InfiniteArrows, kInfiniteArrowsKey, "Infinite Arrows", "Prevent the Elf's arrows from decreasing"},
    {Cheat::InfiniteSkillPoints, kInfiniteSkillPointsKey, "Infinite Skill Points",
     "Prevent skill points from decreasing when spent"},
    {Cheat::InfiniteGold, kInfiniteGoldKey, "Infinite Gold", "Prevent gold from decreasing when spent"},
    {Cheat::MovementSpeed, kMovementSpeedKey, "Movement Speed 2x", "Move at twice the normal speed"},
};
static_assert(std::size(kCheats) == kCheatCount);

inline constexpr char kLauncherPageSubtitle[] = "Optional gameplay modifiers";
inline constexpr char kLauncherHelp[] =
    "Press F10 while playing to open the Cheats menu. Changes made there apply only to the current session. Meant for "
    "offline and local play.";

constexpr size_t Index(Cheat cheat) {
    return static_cast<size_t>(cheat);
}

// One-shot commands of the in-game menu. They are not cheats: nothing is saved or kept On.
enum class Action : uint8_t { AddGold };
inline constexpr size_t kActionCount = 1;

struct ActionInfo {
    Action action;
    Cheat after; // the menu lists the action right below this cheat
    const char* label;
    const char* verb;
};

inline constexpr ActionInfo kActions[] = {
    {Action::AddGold, Cheat::InfiniteGold, "Add 1,000 Gold", "Add"},
};
static_assert(std::size(kActions) == kActionCount);

constexpr size_t Index(Action action) {
    return static_cast<size_t>(action);
}

// What the in-game menu changed. It only records differences from the saved value, so the saved value is never
// altered and a cheat toggled back follows the saved value again.
class SessionCheats {
public:
    bool Effective(Cheat cheat, bool saved) const {
        const uint8_t state = states_[Index(cheat)].load(std::memory_order_relaxed);
        return state == kFollowsSaved ? saved : state == kOn;
    }

    void Set(Cheat cheat, bool saved, bool enabled) {
        states_[Index(cheat)].store(enabled == saved ? kFollowsSaved : (enabled ? kOn : kOff),
                                    std::memory_order_relaxed);
    }

    // Returns the new state.
    bool Toggle(Cheat cheat, bool saved) {
        const bool enabled = !Effective(cheat, saved);
        Set(cheat, saved, enabled);
        return enabled;
    }

    void Reset() {
        for (std::atomic<uint8_t>& state : states_) {
            state.store(kFollowsSaved, std::memory_order_relaxed);
        }
    }

private:
    static constexpr uint8_t kFollowsSaved = 0;
    static constexpr uint8_t kOff = 1;
    static constexpr uint8_t kOn = 2;

    std::array<std::atomic<uint8_t>, kCheatCount> states_{};
};

// Experience Multiplier is one setting, not a cheat: Off (1), 2x, 5x or 10x. The saved text is the multiplier itself.
inline constexpr char kExperienceMultiplierKey[] = "cheat_experience_multiplier";
inline constexpr char kExperienceMultiplierName[] = "Experience Multiplier";
inline constexpr char kExperienceMultiplierHelp[] = "Multiplies the experience each award adds";

struct ExperienceChoice {
    int multiplier;
    const char* saved;
    const char* label;
};

inline constexpr ExperienceChoice kExperienceChoices[] = {
    {1, "1", "Off"}, {2, "2", "2x"}, {5, "5", "5x"}, {10, "10", "10x"},
};
inline constexpr int kExperienceMultiplierOff = 1;

constexpr int ListedExperienceMultiplier(int multiplier) {
    for (const ExperienceChoice& choice : kExperienceChoices) {
        if (choice.multiplier == multiplier) {
            return multiplier;
        }
    }
    return kExperienceMultiplierOff;
}

// The launcher's saved text as a multiplier; anything that is not listed is Off.
constexpr int ExperienceMultiplierFromSaved(std::string_view saved) {
    for (const ExperienceChoice& choice : kExperienceChoices) {
        if (saved == choice.saved) {
            return choice.multiplier;
        }
    }
    return kExperienceMultiplierOff;
}

constexpr const char* ExperienceMultiplierLabel(int multiplier) {
    for (const ExperienceChoice& choice : kExperienceChoices) {
        if (choice.multiplier == multiplier) {
            return choice.label;
        }
    }
    return kExperienceChoices[0].label;
}

// The menu's next value in list order; after 10x it is Off again.
constexpr int NextExperienceMultiplier(int multiplier) {
    size_t index = 0;
    for (size_t i = 0; i < std::size(kExperienceChoices); ++i) {
        if (kExperienceChoices[i].multiplier == multiplier) {
            index = i;
        }
    }
    return kExperienceChoices[(index + 1) % std::size(kExperienceChoices)].multiplier;
}

// The experience an award adds, multiplied once. Only a positive award is multiplied; the result stops at the int32
// maximum, which keeps the sum with the character's total (a 32-bit field, clamped to the cap after) from wrapping.
constexpr int64_t MultipliedExperience(int multiplier, int64_t awarded) {
    if (multiplier <= kExperienceMultiplierOff || awarded <= 0) {
        return awarded;
    }
    if (awarded > INT32_MAX / multiplier) {
        return INT32_MAX;
    }
    return awarded * multiplier;
}

// The menu's Experience Multiplier for this session. Like SessionCheats, a change is kept as a difference from the
// saved value, so cycling back to the saved value follows the launcher again.
class SessionExperience {
public:
    int Effective(int saved) const {
        const int changed = changed_.load(std::memory_order_relaxed);
        return changed == 0 ? saved : changed;
    }

    // Returns the new multiplier.
    int Cycle(int saved) {
        const int next = NextExperienceMultiplier(Effective(saved));
        changed_.store(next == saved ? 0 : next, std::memory_order_relaxed);
        return next;
    }

    void Reset() { changed_.store(0, std::memory_order_relaxed); }

private:
    std::atomic<int> changed_{0};
};

// The game's unit (the object that holds HP and MP) and its owner, as laid out in v1.03. A unit's slot is the player
// index the CREATE_PLAYER handler stores (0..3); the owner's category word has the human-player bit for players only.
inline constexpr uint32_t kUnitOwnerOffset = 0x1e0;
inline constexpr uint32_t kUnitPlayerSlotOffset = 0xa70;
inline constexpr uint32_t kOwnerCategoryOffset = 0x2a0c;
inline constexpr uint32_t kHumanPlayerCategory = 1;
inline constexpr int32_t kPlayerOneSlot = 0;

enum class UnitKind { Other, PlayerOne, OtherPlayer };

// `memory` reads the game's memory: U64(address), U32(address) and S32(address).
template <typename Memory>
UnitKind ClassifyUnit(const Memory& memory, uint64_t unit) {
    if (unit == 0) {
        return UnitKind::Other;
    }
    const uint64_t owner = memory.U64(unit + kUnitOwnerOffset);
    if (owner == 0 || (memory.U32(owner + kOwnerCategoryOffset) & kHumanPlayerCategory) == 0) {
        return UnitKind::Other;
    }
    return memory.S32(unit + kUnitPlayerSlotOffset) == kPlayerOneSlot ? UnitKind::PlayerOne : UnitKind::OtherPlayer;
}

// The chara (owner) side of a unit: [owner+0x9b8] is its unit, [owner+0x39e0] the class index.
inline constexpr uint32_t kOwnerUnitOffset = 0x9b8;
inline constexpr uint32_t kOwnerClassOffset = 0x39e0;
inline constexpr uint32_t kElfClass = 2;
inline constexpr uint32_t kSorceressClass = 4;
inline constexpr uint32_t kWizardClass = 5;

template <typename Memory>
UnitKind ClassifyOwner(const Memory& memory, uint64_t owner) {
    if (owner == 0) {
        return UnitKind::Other;
    }
    const uint64_t unit = memory.U64(owner + kOwnerUnitOffset);
    if (unit == 0 || memory.U64(unit + kUnitOwnerOffset) != owner) {
        return UnitKind::Other;
    }
    return ClassifyUnit(memory, unit);
}

template <typename Memory>
bool IsElf(const Memory& memory, uint64_t owner) {
    return memory.U32(owner + kOwnerClassOffset) == kElfClass;
}

// The damage a hit takes from a unit's HP: positive is a loss, anything else heals and stays as it is.
constexpr int32_t ProtectedDamage(bool enabled, UnitKind kind, int32_t amount) {
    return enabled && kind == UnitKind::PlayerOne && amount > 0 ? 0 : amount;
}

// UseMP deducts only when its consume flag is 1; with 0 the cast still succeeds and costs nothing.
constexpr uint32_t ProtectedConsumeFlag(bool enabled, UnitKind kind, uint32_t consume) {
    return enabled && kind == UnitKind::PlayerOne ? 0 : consume;
}

// AddMP's negative deltas are the sustained drain; positive ones (recovery) always go through.
constexpr bool SuppressesManaDelta(bool enabled, UnitKind kind, int32_t delta) {
    return enabled && kind == UnitKind::PlayerOne && delta < 0;
}

// The endurance event (item durability) carries a signed delta: wear is negative, repair and recovery positive.
constexpr int32_t ProtectedDurabilityDelta(bool enabled, UnitKind kind, int32_t delta) {
    return enabled && kind == UnitKind::PlayerOne && delta < 0 ? 0 : delta;
}

// The respawn consumes a life as `lp - 1`; handing it `lp + 1` for Player 1 leaves the count where it was, and the
// player still dies and respawns as always.
constexpr int32_t ProtectedLivesInput(bool enabled, UnitKind kind, int32_t lives) {
    return enabled && kind == UnitKind::PlayerOne ? lives + 1 : lives;
}

// Gold is one value for the whole save: a store that would lower it keeps the old value, a gain goes through.
inline constexpr uint32_t kGoldOffset = 0xaf238;

constexpr int32_t ProtectedGold(bool enabled, int32_t before, int32_t after) {
    return enabled && after < before ? before : after;
}

inline constexpr int32_t kGoldMaximum = 99999999;
inline constexpr int32_t kGoldPerAdd = 1000;

// What Add 1,000 Gold hands to the game's AddGold: the full amount, or what is left below the game's own maximum.
constexpr int32_t GoldIncrease(int32_t current) {
    const int32_t held = current < 0 ? 0 : current;
    const int32_t room = held >= kGoldMaximum ? 0 : kGoldMaximum - held;
    return room < kGoldPerAdd ? room : kGoldPerAdd;
}

// The walk multiplier the game builds from MOVE_UP buffs (1.0 without any) scales the pad-driven walk's speeds and its
// look-ahead alike; Movement Speed 2x doubles it for Player 1, as a +100% MOVE_UP buff would.
inline constexpr float kMovementScale = 2.0f;

constexpr float ScaledWalkMultiplier(bool enabled, UnitKind kind, float multiplier) {
    return enabled && kind == UnitKind::PlayerOne ? multiplier * kMovementScale : multiplier;
}

// The unit's equipment list: its items are worn gear when flagged equipped, with current and maximum durability
// (100 times the displayed value). The list has an owner and a bag once the player's equipment exists.
inline constexpr uint32_t kUnitEquipmentOffset = 0x1e8;
inline constexpr uint32_t kEquipmentOwnerOffset = 0x508;
inline constexpr uint32_t kEquipmentBagOffset = 0x510;
inline constexpr uint32_t kEquipmentSlotLimit = 10;
inline constexpr uint32_t kItemFlagsOffset = 0x40;
inline constexpr uint32_t kItemEquippedFlag = 0x40000000;
inline constexpr uint32_t kItemDurabilityOffset = 0x44;
inline constexpr uint32_t kItemMaxDurabilityOffset = 0x48;

template <typename Memory>
bool EquipmentAvailable(const Memory& memory, uint64_t unit) {
    const uint64_t list = unit + kUnitEquipmentOffset;
    return memory.U64(list + kEquipmentOwnerOffset) != 0 && memory.U64(list + kEquipmentBagOffset) != 0;
}

// Worn gear below its own maximum is restored to that maximum; anything else is left alone.
constexpr bool NeedsDurabilityRefill(uint32_t flags, int32_t current, int32_t maximum) {
    return (flags & kItemEquippedFlag) != 0 && maximum > 0 && current >= 0 && current < maximum;
}

// The Elf's arrow count drops by the amount of a shot; with 0 the shot still goes out and the count stays.
constexpr int32_t ProtectedArrowAmount(bool enabled, UnitKind kind, bool elf, int32_t amount) {
    return enabled && kind == UnitKind::PlayerOne && elf && amount > 0 ? 0 : amount;
}

// The Elf's arrow count is an int at owner+0x3ad4. Infinite Arrows holds it at 10 while the cheat is On.
inline constexpr uint32_t kArrowCountOffset = 0x3ad4;
inline constexpr int32_t kInfiniteArrowCount = 10;

constexpr int32_t HeldArrowCount(bool enabled, UnitKind kind, bool elf, int32_t count) {
    return enabled && kind == UnitKind::PlayerOne && elf ? kInfiniteArrowCount : count;
}

// The skill purchase stores the points left; Player 1 keeps what they had. Earning and refunds never come here.
constexpr int32_t ProtectedSkillPoints(bool enabled, int32_t player_index, int32_t before, int32_t after) {
    return enabled && player_index == kPlayerOneSlot && after < before ? before : after;
}

// What a pending refill does for Player 1's unit this tick. Nothing is taken from the requests while the unit is dead,
// so the refill waits for the revive; a mana request for a class without mana is dropped.
struct RefillPlan {
    bool fill_health = false;
    bool fill_mana = false;
    bool take_health = false;
    bool take_mana = false;
};

constexpr RefillPlan PlanRefill(bool health_pending, bool mana_pending, uint32_t class_index, bool dead) {
    if (dead) {
        return {};
    }
    const bool has_mana = class_index == kSorceressClass || class_index == kWizardClass;
    return {health_pending, mana_pending && has_mana, health_pending, mana_pending};
}

// The saved settings as a bit per cheat, the form the loader hands to the runtime.
constexpr uint32_t Bit(Cheat cheat) {
    return uint32_t{1} << Index(cheat);
}

constexpr bool RefillsOnEnable(Cheat cheat) {
    return cheat == Cheat::InfiniteHealth || cheat == Cheat::InfiniteMana || cheat == Cheat::InfiniteDurability;
}

// Cheats that fill a resource when they turn On: the request waits until the game thread can apply it to Player 1.
class RefillRequests {
public:
    void Request(Cheat cheat) { pending_.fetch_or(Bit(cheat), std::memory_order_relaxed); }
    void Cancel(Cheat cheat) { pending_.fetch_and(~Bit(cheat), std::memory_order_relaxed); }
    void Clear() { pending_.store(0, std::memory_order_relaxed); }
    bool Pending(Cheat cheat) const { return (pending_.load(std::memory_order_relaxed) & Bit(cheat)) != 0; }

    // True once per request.
    bool Take(Cheat cheat) { return (pending_.fetch_and(~Bit(cheat), std::memory_order_relaxed) & Bit(cheat)) != 0; }

private:
    std::atomic<uint32_t> pending_{0};
};

// Presses of a menu action that the game thread has not carried out yet; one press is one request.
class ActionRequests {
public:
    void Request(Action action) { counts_[Index(action)].fetch_add(1, std::memory_order_relaxed); }
    void Clear() {
        for (std::atomic<uint32_t>& count : counts_) {
            count.store(0, std::memory_order_relaxed);
        }
    }
    bool Pending(Action action) const { return counts_[Index(action)].load(std::memory_order_relaxed) != 0; }

    // True once per request.
    bool Take(Action action) {
        std::atomic<uint32_t>& count = counts_[Index(action)];
        uint32_t seen = count.load(std::memory_order_relaxed);
        while (seen != 0 && !count.compare_exchange_weak(seen, seen - 1, std::memory_order_relaxed)) {
        }
        return seen != 0;
    }

private:
    std::array<std::atomic<uint32_t>, kActionCount> counts_{};
};

} // namespace dcp::cheats
