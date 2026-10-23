#include "cheat_hooks.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "dcp_cheats.h"
#include "runtime_cheats.h"

namespace dcp::cheats {
namespace {

struct HostMemory {
    template <typename T>
    static T Load(uint64_t address) {
        T value;
        std::memcpy(&value, reinterpret_cast<const void*>(static_cast<uintptr_t>(address)), sizeof(value));
        return value;
    }
    template <typename T>
    static void Store(uint64_t address, T value) {
        std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(address)), &value, sizeof(value));
    }
    uint64_t U64(uint64_t address) const { return Load<uint64_t>(address); }
    uint32_t U32(uint64_t address) const { return Load<uint32_t>(address); }
    int32_t S32(uint64_t address) const { return Load<int32_t>(address); }
    uint8_t U8(uint64_t address) const { return Load<uint8_t>(address); }
};

bool Enabled(Cheat cheat) {
    return runtime_cheat_enabled(static_cast<int>(cheat)) != 0;
}

UnitKind Classify(uint64_t unit) {
    return ClassifyUnit(HostMemory{}, unit);
}

bool IsAlly(const HostMemory& memory, uint64_t unit) {
    if (unit == 0) {
        return false;
    }
    const uint64_t owner = memory.U64(unit + kUnitOwnerOffset);
    if (owner == 0) {
        return false;
    }
    const int32_t slot = memory.S32(unit + kUnitPlayerSlotOffset);
    if (runtime_cheat_slot_human(slot) != 0) {
        return false;
    }
    return IsAiAlly(memory.U32(owner + kOwnerCategoryOffset), slot, memory.U8(unit + kUnitClassOffset));
}

constexpr uint64_t kDamageUnitFrameOffset = 0x200; // the victim unit, [rbp-0x200] in the damage function
constexpr uint64_t kUnitContextVirtual = 0xd0;     // unit->vtable[+0xd0]() is the game object
constexpr uint64_t kGameSaveBaseOffset = 0x80;     // the save records, 0x23b0 bytes each, Player 1 at +0xa8
constexpr uint64_t kSaveRecordOffset = 0xa8;
constexpr uint64_t kSaveRecordSize = 0x23b0;
constexpr uint64_t kGameGoldContextOffset = 0x88;  // the object AddGold and GetGold take

GuestFunctions g_guest{};

// Each spending path computes the new gold in a register and stores it at [base + kGoldOffset]; which registers
// differ per site, so one handler is made for each pair.
template <uint64_t GuestRegs::*Base, uint64_t GuestRegs::*Value>
void GUEST_HOOK_ABI GoldStoreHook(GuestRegs* regs) {
    if (!Enabled(Cheat::InfiniteGold)) {
        return;
    }
    const int32_t before = HostMemory{}.S32(regs->*Base + kGoldOffset);
    const int32_t after = static_cast<int32_t>(regs->*Value);
    regs->*Value = static_cast<uint32_t>(ProtectedGold(true, before, after));
}

} // namespace

// In the damage function (rcx = signed amount, positive is a loss; r15 = the victim's owner).
void GUEST_HOOK_ABI HealthDamageHook(GuestRegs* regs) {
    const int32_t amount = static_cast<int32_t>(regs->rcx);
    if (amount <= 0) {
        return;
    }
    const HostMemory memory;
    const uint64_t unit = memory.U64(regs->rbp - kDamageUnitFrameOffset);
    const bool player = Enabled(Cheat::InfiniteHealth);
    const bool ally = Enabled(Cheat::InfiniteAllyHealth);
    if (!player && !ally) {
        return;
    }
    if (unit == 0 || memory.U64(unit + kUnitOwnerOffset) != regs->r15) {
        return;
    }
    int32_t kept = amount;
    if (player) {
        kept = ProtectedDamage(true, Classify(unit), kept);
    }
    if (ally) {
        kept = ProtectedAllyDamage(true, IsAlly(memory, unit), kept);
    }
    regs->rcx = static_cast<uint32_t>(kept);
}

// At the entry of UseMP (rdi = the caster's unit, ecx = the consume flag).
void GUEST_HOOK_ABI ManaCostHook(GuestRegs* regs) {
    const bool player = Enabled(Cheat::InfiniteMana);
    const bool ally = Enabled(Cheat::InfiniteAllyMana);
    if (!player && !ally) {
        return;
    }
    uint32_t consume = static_cast<uint32_t>(regs->rcx) & 0xff;
    if (player) {
        consume = ProtectedConsumeFlag(true, Classify(regs->rdi), consume);
    }
    if (ally) {
        consume = ProtectedAllyConsumeFlag(true, IsAlly(HostMemory{}, regs->rdi), consume);
    }
    regs->rcx = consume;
}

// At the entry of AddMP (rdi = the unit, esi = the delta).
void GUEST_HOOK_ABI ManaDrainHook(GuestRegs* regs) {
    const bool player = Enabled(Cheat::InfiniteMana);
    const bool ally = Enabled(Cheat::InfiniteAllyMana);
    if (!player && !ally) {
        return;
    }
    const int32_t delta = static_cast<int32_t>(regs->rsi);
    const bool suppress = (player && SuppressesManaDelta(true, Classify(regs->rdi), delta)) ||
                          (ally && SuppressesAllyManaDelta(true, IsAlly(HostMemory{}, regs->rdi), delta));
    if (suppress) {
        regs->skip = 1;
    }
}

// At the entry of ConsumeArrow (rdi = the Elf's owner object, esi = the amount).
void GUEST_HOOK_ABI ArrowHook(GuestRegs* regs) {
    if (!Enabled(Cheat::InfiniteArrows)) {
        return;
    }
    const HostMemory memory;
    if (ClassifyOwner(memory, regs->rdi) != UnitKind::PlayerOne || !IsElf(memory, regs->rdi)) {
        return;
    }
    regs->rsi = static_cast<uint32_t>(ProtectedArrowAmount(true, UnitKind::PlayerOne, true, static_cast<int32_t>(regs->rsi)));
}

// Before the return of the experience-gain function (rax = the experience this call gains, int64). The stage result
// and the result screen's display both come through here, so the display shows the multiplied gain too.
void GUEST_HOOK_ABI ExperienceHook(GuestRegs* regs) {
    const int multiplier = runtime_cheat_experience_multiplier();
    if (multiplier == kExperienceMultiplierOff) {
        return;
    }
    regs->rax = static_cast<uint64_t>(MultipliedExperience(multiplier, static_cast<int64_t>(regs->rax)));
}

// In the pad-driven walk, once its speed multiplier is final (the unit is at [rbp-0x80], the float at [rbp-0x48]).
void GUEST_HOOK_ABI MovementHook(GuestRegs* regs) {
    const int movement_speed = runtime_cheat_movement_speed();
    if (movement_speed == kMovementSpeedOff) {
        return;
    }
    const HostMemory memory;
    const uint64_t multiplier_address = regs->rbp - 0x48;
    const UnitKind kind = Classify(memory.U64(regs->rbp - 0x80));
    const float multiplier = HostMemory::Load<float>(multiplier_address);
    const float scaled = ScaledWalkMultiplier(movement_speed, kind, multiplier);
    if (scaled != multiplier) {
        std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(multiplier_address)), &scaled, sizeof(scaled));
    }
}

// In the respawn's life count (r13 = the unit, eax = the current count): the original `dec eax` follows.
void GUEST_HOOK_ABI LivesHook(GuestRegs* regs) {
    if (!Enabled(Cheat::InfiniteLives)) {
        return;
    }
    const int32_t lives = static_cast<int32_t>(regs->rax);
    regs->rax = static_cast<uint32_t>(ProtectedLivesInput(true, Classify(regs->r13), lives));
}

// In the endurance event handler, before its delta is tested (the unit it applies to is [rbp-0x308], the signed delta
// the int at [rbp-0x2fc]).
void GUEST_HOOK_ABI DurabilityHook(GuestRegs* regs) {
    const bool player = Enabled(Cheat::InfiniteDurability);
    const bool ally = Enabled(Cheat::InfiniteAllyDurability);
    if (!player && !ally) {
        return;
    }
    const HostMemory memory;
    const uint64_t unit = memory.U64(regs->rbp - 0x308);
    const uint64_t delta_address = regs->rbp - 0x2fc;
    const int32_t delta = memory.S32(delta_address);
    int32_t kept = delta;
    if (player) {
        kept = ProtectedDurabilityDelta(true, Classify(unit), kept);
    }
    if (ally) {
        kept = ProtectedAllyDurabilityDelta(true, IsAlly(memory, unit), kept);
    }
    if (kept != delta) {
        std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(delta_address)), &kept, sizeof(kept));
    }
}

// In the skill purchase, before the new points are stored (r12 = player index, r13w = points left, r14 = the player's
// save record, whose +0x24 still holds the points before).
void GUEST_HOOK_ABI SkillPointsHook(GuestRegs* regs) {
    if (!Enabled(Cheat::InfiniteSkillPoints) || regs->r14 == 0) {
        return;
    }
    const int32_t before = static_cast<int16_t>(HostMemory::Load<uint16_t>(regs->r14 + kSkillPointsOffset));
    const int32_t after = static_cast<int16_t>(regs->r13);
    const int32_t kept = ProtectedSkillPoints(true, static_cast<int32_t>(regs->r12), before, after);
    regs->r13 = (regs->r13 & ~uint64_t{0xffff}) | static_cast<uint16_t>(kept);
}

// At the two item-count decrements of the skill-item use function (rdi = item, r12 = the using owner).
void GUEST_HOOK_ABI SkillUseHook(GuestRegs* regs) {
    const bool enabled = Enabled(Cheat::InfiniteSkillUses);
    if (!enabled || regs->rdi == 0) {
        return;
    }
    const HostMemory memory;
    const UnitKind kind = ClassifyOwner(memory, regs->r12);
    const int32_t uses = memory.U8(regs->rdi + kItemUsesOffset);
    if (PreservedSkillUse(enabled, kind, uses)) {
        regs->skip = 1;
    }
}

// At the two item-count decrements of the equipment wear function (rdi = item, rbx = the unit's equipment list). Every
// skill item the skill-item use function does not handle (the Dwarf's, for one) is counted down here.
void GUEST_HOOK_ABI SkillItemWearHook(GuestRegs* regs) {
    if (!Enabled(Cheat::InfiniteSkillUses) || regs->rdi == 0) {
        return;
    }
    const HostMemory memory;
    const bool skill_item =
        IsSkillItem(memory.U32(regs->rdi + kItemFlagsOffset), HostMemory::Load<uint16_t>(regs->rdi));
    const UnitKind kind = Classify(regs->rbx - kUnitEquipmentOffset);
    if (skill_item && PreservedSkillUse(true, kind, memory.U8(regs->rdi + kItemUsesOffset))) {
        regs->skip = 1;
    }
}

// In the record's skill budget check, before it compares what the record owns with what it may own (r12 = level
// allowance, r14 = quest bonus, r13 = points in bought levels, the points held at [rbp-0x5c]).
void GUEST_HOOK_ABI SkillBudgetHook(GuestRegs* regs) {
    if (!Enabled(Cheat::InfiniteSkillPoints)) {
        return;
    }
    const int32_t available = HostMemory{}.S32(regs->rbp - 0x5c);
    regs->r12 = static_cast<uint32_t>(SkillAllowance(true, static_cast<int32_t>(regs->r12),
                                                     static_cast<int32_t>(regs->r14),
                                                     static_cast<int32_t>(regs->r13), available));
}

void SetGuestFunctions(const GuestFunctions& functions) {
    g_guest = functions;
}

namespace {

void ApplyRefills(uint64_t unit, unsigned pending) {
    if (g_guest.refill == nullptr || g_guest.is_dead == nullptr) {
        return;
    }
    const HostMemory memory;
    const uint64_t owner = memory.U64(unit + kUnitOwnerOffset);
    const RefillPlan plan = PlanRefill((pending & Bit(Cheat::InfiniteHealth)) != 0,
                                       (pending & Bit(Cheat::InfiniteMana)) != 0, memory.U32(owner + kOwnerClassOffset),
                                       g_guest.is_dead(owner) != 0);
    if (plan.take_health) {
        runtime_cheat_take_refill(static_cast<int>(Cheat::InfiniteHealth));
    }
    if (plan.take_mana) {
        runtime_cheat_take_refill(static_cast<int>(Cheat::InfiniteMana));
    }
    if (plan.fill_health || plan.fill_mana) {
        g_guest.refill(unit, plan.fill_health, plan.fill_mana, 1);
    }
}

// Restores Player 1's worn gear through the game's own durability setter, which the game's repairs also end in.
// The list is read from the unit on this tick only; nothing is kept for the next.
void ApplyDurabilityRefill(uint64_t unit) {
    if (g_guest.equipment_count == nullptr || g_guest.equipment_item == nullptr || g_guest.is_gear == nullptr ||
        g_guest.set_durability == nullptr) {
        return;
    }
    const HostMemory memory;
    if (!EquipmentAvailable(memory, unit) ||
        runtime_cheat_take_refill(static_cast<int>(Cheat::InfiniteDurability)) == 0) {
        return;
    }
    const uint64_t list = unit + kUnitEquipmentOffset;
    const uint32_t count = g_guest.equipment_count(list);
    for (uint32_t slot = 0; slot < count && slot < kEquipmentSlotLimit; ++slot) {
        const uint64_t item = g_guest.equipment_item(list, static_cast<int32_t>(slot));
        if (item == 0 || g_guest.is_gear(item) == 0) {
            continue;
        }
        const int32_t maximum = memory.S32(item + kItemMaxDurabilityOffset);
        if (NeedsDurabilityRefill(memory.U32(item + kItemFlagsOffset), memory.S32(item + kItemDurabilityOffset), maximum)) {
            g_guest.set_durability(list, static_cast<int32_t>(slot), maximum);
        }
    }
}

// Keeps a living AI ally's HP and, for a mana class, MP topped up through the game's own fill function.
void ApplyAllyRefills(uint64_t unit, bool health, bool mana) {
    if (g_guest.refill == nullptr || g_guest.is_dead == nullptr) {
        return;
    }
    const HostMemory memory;
    const uint64_t owner = memory.U64(unit + kUnitOwnerOffset);
    if (owner == 0 || g_guest.is_dead(owner) != 0) {
        return;
    }
    const int32_t hp = memory.S32(unit + kUnitHpOffset);
    if (hp <= 0) {
        return;
    }
    const uint8_t class_index = memory.U8(unit + kUnitClassOffset);
    const bool has_mana = class_index == kSorceressClass || class_index == kWizardClass;
    if (!health && (!mana || !has_mana)) {
        return;
    }
    g_guest.refill(unit, health ? 1u : 0u, mana && has_mana ? 1u : 0u, 1);
}

// One request, one AddGold: the game applies its own clamp, and at the maximum nothing is added.
void ApplyAddGold(uint64_t unit) {
    if (g_guest.gold_context == nullptr || g_guest.get_gold == nullptr || g_guest.add_gold == nullptr) {
        return;
    }
    const uint64_t context = g_guest.gold_context(unit);
    if (context == 0 || runtime_cheat_take_action(static_cast<int>(Action::AddGold)) == 0) {
        return;
    }
    const int32_t before = g_guest.get_gold(context);
    const int32_t increase = GoldIncrease(before);
    if (increase > 0) {
        g_guest.add_gold(context, increase);
    }
    std::printf("Cheats: Add 1,000 Gold, %d -> %d\n", before, g_guest.get_gold(context));
}

// One request, one +100 to Player 1's own save record, clamped to the int16 maximum. The unit's mirror is kept in step.
void ApplyAddSkillPoints(uint64_t unit) {
    if (g_guest.save_record == nullptr) {
        return;
    }
    const uint64_t record = g_guest.save_record(unit);
    if (record == 0 || runtime_cheat_take_action(static_cast<int>(Action::AddSkillPoints)) == 0) {
        return;
    }
    const int32_t before = static_cast<int16_t>(HostMemory::Load<uint16_t>(record + kSkillPointsOffset));
    const int32_t after = SkillPointsAfterAdd(before);
    if (after != before) {
        const uint16_t value = static_cast<uint16_t>(after);
        std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(record + kSkillPointsOffset)), &value, sizeof(value));
        std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(unit + kSkillPointsMirrorOffset)), &value,
                    sizeof(value));
    }
    std::printf("Cheats: Add 100 Skill Points, %d -> %d\n", before, after);
}

bool AddLevelAvailable() {
    return g_guest.save_record != nullptr && g_guest.record_mode != nullptr && g_guest.record_stage != nullptr &&
           g_guest.max_level != nullptr && g_guest.level_entry != nullptr && g_guest.level_pattern != nullptr &&
           g_guest.set_level != nullptr && g_guest.write_stats != nullptr;
}

// One request, one legitimate level-up on Player 1's record and live unit, through the game's own tables and cap.
void ApplyAddLevel(uint64_t unit) {
    if (!AddLevelAvailable()) {
        return;
    }
    const uint64_t record = g_guest.save_record(unit);
    if (record == 0 || runtime_cheat_take_action(static_cast<int>(Action::AddLevel)) == 0) {
        return;
    }
    const HostMemory memory;
    const int32_t level = memory.U8(record + kRecordLevelOffset);
    const int32_t mode = g_guest.record_mode(record);
    const int32_t max_level = g_guest.max_level(mode, g_guest.record_stage(record), mode == 0x63 ? 1 : 0);
    if (level >= max_level || level >= kAbsoluteMaxLevel) {
        std::printf("Cheats: Level Up (+1 Level), already at maximum level %d\n", level + 1);
        return;
    }
    const int32_t next_level = level + 1;
    const uint64_t next_entry = g_guest.level_entry(memory.U8(record + kRecordClassOffset), next_level);
    if (next_entry == 0) {
        return;
    }

    const int32_t threshold = memory.S32(next_entry + kLevelXpOffset);
    const int32_t old_progress = memory.S32(record + kRecordXpProgressOffset);
    const int32_t old_pool = memory.S32(record + kRecordXpPoolOffset);
    const int32_t held_pool = old_pool < 0 ? 0 : old_pool;
    const int32_t excess = old_progress > threshold ? old_progress - threshold : 0;
    const int32_t pool = held_pool > INT32_MAX - excess ? INT32_MAX : held_pool + excess;

    HostMemory::Store<int32_t>(record + kRecordXpPoolOffset, pool);
    HostMemory::Store<int32_t>(record + kRecordXpProgressOffset, 0);
    g_guest.set_level(record, static_cast<uint32_t>(next_level));

    int32_t skill_points = static_cast<int16_t>(memory.Load<uint16_t>(record + kSkillPointsOffset));
    if (SkillPointsForLevel(next_level) != 0 && skill_points < kSkillPointsMaximum) {
        ++skill_points;
        HostMemory::Store<uint16_t>(record + kSkillPointsOffset, static_cast<uint16_t>(skill_points));
    }
    HostMemory::Store<uint16_t>(unit + kSkillPointsMirrorOffset, static_cast<uint16_t>(skill_points));
    HostMemory::Store<uint8_t>(unit + kUnitLevelOffset, static_cast<uint8_t>(next_level));
    HostMemory::Store<int32_t>(unit + kUnitXpOffset, pool);
    HostMemory::Store<int32_t>(unit + kUnitXpProgressOffset, 0);

    const int32_t pattern = g_guest.level_pattern(record);
    g_guest.write_stats(record, pattern, 0, next_level, 0);
    g_guest.write_stats(unit + kUnitStatOffset, pattern, 0, next_level, 0);

    std::printf("Cheats: Level Up (+1 Level), %d -> %d\n", level + 1, next_level + 1);
}

} // namespace

namespace {

// Holds Player 1's Elf at the held arrow count while Infinite Arrows is On. The shot hook already keeps a shot from
// taking arrows, so this only puts back what other code changed.
void HoldArrows(uint64_t unit) {
    const HostMemory memory;
    const uint64_t owner = memory.U64(unit + kUnitOwnerOffset);
    const int32_t count = memory.S32(owner + kArrowCountOffset);
    const int32_t held = HeldArrowCount(true, UnitKind::PlayerOne, IsElf(memory, owner), count);
    if (held != count) {
        std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(owner + kArrowCountOffset)), &held, sizeof(held));
    }
}

} // namespace

// At the entry of the player unit's per-tick update (rdi = the unit). Only players' units have this method.
void GUEST_HOOK_ABI PlayerTickHook(GuestRegs* regs) {
    const unsigned refills = runtime_cheat_refills_pending();
    const bool add_skill_points = runtime_cheat_action_pending(static_cast<int>(Action::AddSkillPoints)) != 0;
    const bool add_gold = runtime_cheat_action_pending(static_cast<int>(Action::AddGold)) != 0;
    const bool add_level = runtime_cheat_action_pending(static_cast<int>(Action::AddLevel)) != 0;
    const bool arrows = Enabled(Cheat::InfiniteArrows);
    const bool ally_health = Enabled(Cheat::InfiniteAllyHealth);
    const bool ally_mana = Enabled(Cheat::InfiniteAllyMana);
    if (refills == 0 && !add_skill_points && !add_gold && !add_level && !arrows && !ally_health && !ally_mana) {
        return;
    }
    const UnitKind kind = Classify(regs->rdi);
    if (kind == UnitKind::PlayerOne) {
        if (arrows) {
            HoldArrows(regs->rdi);
        }
        if ((refills & (Bit(Cheat::InfiniteHealth) | Bit(Cheat::InfiniteMana))) != 0) {
            ApplyRefills(regs->rdi, refills);
        }
        if ((refills & Bit(Cheat::InfiniteDurability)) != 0) {
            ApplyDurabilityRefill(regs->rdi);
        }
        if (add_skill_points) {
            ApplyAddSkillPoints(regs->rdi);
        }
        if (add_gold) {
            ApplyAddGold(regs->rdi);
        }
        if (add_level) {
            ApplyAddLevel(regs->rdi);
        }
        return;
    }
    if (kind == UnitKind::OtherPlayer && (ally_health || ally_mana) && IsAlly(HostMemory{}, regs->rdi)) {
        ApplyAllyRefills(regs->rdi, ally_health, ally_mana);
    }
}

std::span<const GuestHook> Hooks() {
    static const GuestHook hooks[] = {
        {"damage", 0xd8703, 6, {0x89, 0x8d, 0xdc, 0xfd, 0xff, 0xff}, HealthDamageHook},
        {"use mana", 0xd6d70, 6, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, ManaCostHook},
        {"mana drain", 0x345380, 6, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56}, ManaDrainHook},
        {"arrows", 0x1b06f0, 6, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, ArrowHook},
        {"skill points", 0x537e8a, 8, {0x66, 0x44, 0x89, 0xab, 0xc8, 0x00, 0x00, 0x00}, SkillPointsHook},
        {"player tick", 0x2ea420, 6, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, PlayerTickHook},
        {"durability", 0xfe183, 7, {0x83, 0xbd, 0x04, 0xfd, 0xff, 0xff, 0x00}, DurabilityHook},
        {"lives", 0x2ec7cb, 5, {0xff, 0xc8, 0x83, 0xf8, 0xff}, LivesHook},
        {"walk speed", 0x2ecd0a, 7, {0x48, 0x8b, 0x5d, 0x80, 0x48, 0x8b, 0x03}, MovementHook},
        {"gold shop", 0x52e0b3, 8, {0x41, 0x89, 0x8c, 0x24, 0x38, 0xf2, 0x0a, 0x00}, GoldStoreHook<&GuestRegs::r12, &GuestRegs::rcx>},
        {"gold continue", 0x43d562, 6, {0x89, 0x88, 0x38, 0xf2, 0x0a, 0x00}, GoldStoreHook<&GuestRegs::rax, &GuestRegs::rcx>},
        {"gold stable", 0x3bc693, 8, {0x41, 0x89, 0x84, 0x24, 0x38, 0xf2, 0x0a, 0x00}, GoldStoreHook<&GuestRegs::r12, &GuestRegs::rax>},
        {"gold beggar", 0x3bff22, 6, {0x89, 0x8a, 0x38, 0xf2, 0x0a, 0x00}, GoldStoreHook<&GuestRegs::rdx, &GuestRegs::rcx>},
        {"gold base", 0x3c2420, 7, {0x41, 0x89, 0x8d, 0x38, 0xf2, 0x0a, 0x00}, GoldStoreHook<&GuestRegs::r13, &GuestRegs::rcx>},
        {"gold inn", 0x3c3b82, 7, {0x41, 0x89, 0x85, 0x38, 0xf2, 0x0a, 0x00}, GoldStoreHook<&GuestRegs::r13, &GuestRegs::rax>},
        {"gold inn 2", 0x3c40da, 7, {0x41, 0x89, 0x8e, 0x38, 0xf2, 0x0a, 0x00}, GoldStoreHook<&GuestRegs::r14, &GuestRegs::rcx>},
        {"gold list", 0x4953cd, 7, {0x41, 0x89, 0x96, 0x38, 0xf2, 0x0a, 0x00}, GoldStoreHook<&GuestRegs::r14, &GuestRegs::rdx>},
        {"gold list 2", 0x49562d, 7, {0x41, 0x89, 0xb6, 0x38, 0xf2, 0x0a, 0x00}, GoldStoreHook<&GuestRegs::r14, &GuestRegs::rsi>},
        {"gold equip", 0x4cc47a, 6, {0x89, 0x82, 0x38, 0xf2, 0x0a, 0x00}, GoldStoreHook<&GuestRegs::rdx, &GuestRegs::rax>},
        {"experience", 0x27511, 7, {0x48, 0x83, 0xc4, 0x08, 0x5b, 0x41, 0x5c}, ExperienceHook},
        {"skill use", 0x1af0dc, 5, {0xe8, 0x6f, 0x13, 0xe7, 0xff}, SkillUseHook},
        {"skill use mirror", 0x1af12a, 5, {0xe9, 0x21, 0x13, 0xe7, 0xff}, SkillUseHook},
        {"skill item wear", 0x225f90, 5, {0xe8, 0xbb, 0xa4, 0xdf, 0xff}, SkillItemWearHook},
        {"skill item wear mirror", 0x225fb1, 5, {0xe8, 0x9a, 0xa4, 0xdf, 0xff}, SkillItemWearHook},
        {"skill budget", 0x8ab24, 12, {0x8b, 0x5d, 0xa4, 0x45, 0x01, 0xf4, 0x41, 0x01, 0xdd, 0x45, 0x29, 0xec},
         SkillBudgetHook},
    };
    return hooks;
}

} // namespace dcp::cheats

namespace {

constexpr uint64_t kIsDeadOffset = 0x281ba0;
constexpr uint64_t kRefillOffset = 0xe3be0;
constexpr uint8_t kIsDeadPrologue[] = {0x48, 0x8b, 0x8f, 0xb8, 0x09, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x74, 0x0b};
constexpr uint8_t kRefillPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x41, 0x89, 0xd6};

constexpr uint64_t kGetGoldOffset = 0x43d460;
constexpr uint64_t kAddGoldOffset = 0x43d490;
constexpr uint8_t kGetGoldPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x8b, 0x07, 0xff, 0x90, 0xd8, 0x00, 0x00, 0x00};
constexpr uint8_t kAddGoldPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb};

constexpr uint64_t kRecordModeOffset = 0x83ae0;
constexpr uint64_t kRecordStageOffset = 0x86ee0;
constexpr uint64_t kMaxLevelOffset = 0x4f950;
constexpr uint64_t kLevelEntryOffset = 0x2f9da0;
constexpr uint64_t kLevelPatternOffset = 0x81fe0;
constexpr uint64_t kSetLevelOffset = 0x81fb0;
constexpr uint64_t kWriteStatsOffset = 0x82000;
constexpr uint8_t kRecordModePrologue[] = {0x80, 0xbf, 0x01, 0x01, 0x00, 0x00, 0x00,
                                           0xb8, 0x63, 0x00, 0x00, 0x00, 0x75};
constexpr uint8_t kRecordStagePrologue[] = {0x80, 0x7f, 0x6f, 0x2d, 0x0f, 0x92, 0xc0};
constexpr uint8_t kMaxLevelPrologue[] = {0x84, 0xd2, 0xb9, 0xfe, 0x00, 0x00,
                                         0x00, 0xb8, 0x22, 0x00, 0x00, 0x00};
constexpr uint8_t kLevelEntryPrologue[] = {0x40, 0x80, 0xfe, 0xfe, 0xb9, 0xfe, 0x00,
                                           0x00, 0x00, 0x89, 0xfa, 0x0f, 0x42, 0xce};
constexpr uint8_t kLevelPatternPrologue[] = {0x48, 0x0f, 0xbe, 0x47, 0x27, 0x48, 0x83,
                                             0xf8, 0x05, 0x77, 0x0b, 0x48, 0x8d, 0x0d};
constexpr uint8_t kSetLevelPrologue[] = {0x40, 0x80, 0xfe, 0xff, 0x75, 0x06,
                                         0x80, 0x7f, 0x27, 0xff, 0x75, 0x0a};
constexpr uint8_t kWriteStatsPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                           0x56, 0x53, 0x50, 0x89, 0xcb, 0x49, 0x89, 0xff};

constexpr uint64_t kEquipmentCountOffset = 0x224b40;
constexpr uint64_t kEquipmentItemOffset = 0x2251b0;
constexpr uint64_t kIsGearOffset = 0x21d20;
constexpr uint64_t kSetDurabilityOffset = 0x225fe0;
constexpr uint8_t kEquipmentCountPrologue[] = {0x48, 0x8b, 0x87, 0x10, 0x05, 0x00, 0x00};
constexpr uint8_t kEquipmentItemPrologue[] = {0x85, 0xf6, 0x78, 0x3b};
constexpr uint8_t kIsGearPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb};
constexpr uint8_t kSetDurabilityPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53};

template <size_t N>
bool PrologueMatches(const uint8_t* image, uint64_t offset, const uint8_t (&bytes)[N]) {
    return std::memcmp(image + offset, bytes, N) == 0;
}

uint64_t GameContext(uint64_t unit) {
    using Virtual = uint64_t(GUEST_HOOK_ABI*)(uint64_t);
    const dcp::cheats::HostMemory memory;
    const uint64_t method = memory.U64(memory.U64(unit) + dcp::cheats::kUnitContextVirtual);
    return reinterpret_cast<Virtual>(method)(unit);
}

uint64_t GUEST_HOOK_ABI GameGoldContext(uint64_t unit) {
    const dcp::cheats::HostMemory memory;
    const uint64_t game = GameContext(unit);
    return game == 0 ? 0 : memory.U64(game + dcp::cheats::kGameGoldContextOffset);
}

uint64_t GUEST_HOOK_ABI GameSaveRecord(uint64_t unit) {
    const dcp::cheats::HostMemory memory;
    const uint64_t game = GameContext(unit);
    if (game == 0) {
        return 0;
    }
    const uint64_t base = memory.U64(game + dcp::cheats::kGameSaveBaseOffset);
    const int32_t slot = memory.S32(unit + dcp::cheats::kUnitPlayerSlotOffset);
    if (base == 0 || slot < 0 || slot > 3) {
        return 0;
    }
    return base + dcp::cheats::kSaveRecordOffset + uint64_t(slot) * dcp::cheats::kSaveRecordSize;
}

} // namespace

extern "C" size_t runtime_cheat_hooks_stub_bytes(void) {
    return dcp::cheats::Hooks().size() * guest_hook_stub_size();
}

extern "C" int runtime_cheat_hooks_install(uint8_t* image, uint64_t size, uint64_t entry, uint8_t* stubs,
                                           size_t stubs_size) {
    const std::span<const GuestHook> hooks = dcp::cheats::Hooks();
    if (guest_hooks_verify(image, size, entry, &dcp::cheats::kSupportedImage, hooks.data(), 0, nullptr) !=
        GUEST_HOOK_OK) {
        std::printf("Cheats: unavailable, not the supported game image\n");
        return 0;
    }
    const size_t stride = guest_hook_stub_size();
    if (stubs_size < hooks.size() * stride) {
        std::printf("Cheats: unavailable, no room for the hook stubs\n");
        return 0;
    }
    size_t installed = 0;
    for (size_t i = 0; i < hooks.size(); ++i) {
        size_t bad = 0;
        const GuestHookStatus status = guest_hooks_install(image, size, entry, &dcp::cheats::kSupportedImage, &hooks[i],
                                                           1, stubs + i * stride, stubs_size - i * stride, &bad);
        if (status == GUEST_HOOK_OK) {
            ++installed;
        } else {
            std::printf("Cheats: hook '%s' not installed, %s\n", hooks[i].name, guest_hook_status_text(status));
        }
    }
    if (installed == 0) {
        std::printf("Cheats: unavailable\n");
        return 0;
    }
    dcp::cheats::GuestFunctions functions;
    functions.save_record = GameSaveRecord;
    if (PrologueMatches(image, kRecordModeOffset, kRecordModePrologue) &&
        PrologueMatches(image, kRecordStageOffset, kRecordStagePrologue) &&
        PrologueMatches(image, kMaxLevelOffset, kMaxLevelPrologue) &&
        PrologueMatches(image, kLevelEntryOffset, kLevelEntryPrologue) &&
        PrologueMatches(image, kLevelPatternOffset, kLevelPatternPrologue) &&
        PrologueMatches(image, kSetLevelOffset, kSetLevelPrologue) &&
        PrologueMatches(image, kWriteStatsOffset, kWriteStatsPrologue)) {
        functions.record_mode = reinterpret_cast<int32_t(GUEST_HOOK_ABI*)(uint64_t)>(image + kRecordModeOffset);
        functions.record_stage = reinterpret_cast<uint8_t(GUEST_HOOK_ABI*)(uint64_t)>(image + kRecordStageOffset);
        functions.max_level =
            reinterpret_cast<int32_t(GUEST_HOOK_ABI*)(int32_t, int32_t, int32_t)>(image + kMaxLevelOffset);
        functions.level_entry =
            reinterpret_cast<uint64_t(GUEST_HOOK_ABI*)(int32_t, int32_t)>(image + kLevelEntryOffset);
        functions.level_pattern = reinterpret_cast<int32_t(GUEST_HOOK_ABI*)(uint64_t)>(image + kLevelPatternOffset);
        functions.set_level = reinterpret_cast<void(GUEST_HOOK_ABI*)(uint64_t, uint32_t)>(image + kSetLevelOffset);
        functions.write_stats = reinterpret_cast<void(GUEST_HOOK_ABI*)(uint64_t, int32_t, int32_t, int32_t, int32_t)>(
            image + kWriteStatsOffset);
    } else {
        std::printf("Cheats: Level Up (+1 Level) not available, the game's level functions differ\n");
    }
    if (PrologueMatches(image, kIsDeadOffset, kIsDeadPrologue) && PrologueMatches(image, kRefillOffset, kRefillPrologue)) {
        functions.is_dead = reinterpret_cast<uint8_t(GUEST_HOOK_ABI*)(uint64_t)>(image + kIsDeadOffset);
        functions.refill =
            reinterpret_cast<void(GUEST_HOOK_ABI*)(uint64_t, uint32_t, uint32_t, uint32_t)>(image + kRefillOffset);
    } else {
        std::printf("Cheats: the Health and Mana refill is not available, the game's fill functions differ\n");
    }
    if (PrologueMatches(image, kGetGoldOffset, kGetGoldPrologue) && PrologueMatches(image, kAddGoldOffset, kAddGoldPrologue)) {
        functions.gold_context = GameGoldContext;
        functions.get_gold = reinterpret_cast<int32_t(GUEST_HOOK_ABI*)(uint64_t)>(image + kGetGoldOffset);
        functions.add_gold = reinterpret_cast<void(GUEST_HOOK_ABI*)(uint64_t, int32_t)>(image + kAddGoldOffset);
    } else {
        std::printf("Cheats: Add 1,000 Gold not available, the game's gold functions differ\n");
    }
    if (PrologueMatches(image, kEquipmentCountOffset, kEquipmentCountPrologue) &&
        PrologueMatches(image, kEquipmentItemOffset, kEquipmentItemPrologue) &&
        PrologueMatches(image, kIsGearOffset, kIsGearPrologue) &&
        PrologueMatches(image, kSetDurabilityOffset, kSetDurabilityPrologue)) {
        functions.equipment_count = reinterpret_cast<uint8_t(GUEST_HOOK_ABI*)(uint64_t)>(image + kEquipmentCountOffset);
        functions.equipment_item = reinterpret_cast<uint64_t(GUEST_HOOK_ABI*)(uint64_t, int32_t)>(image + kEquipmentItemOffset);
        functions.is_gear = reinterpret_cast<uint8_t(GUEST_HOOK_ABI*)(uint64_t)>(image + kIsGearOffset);
        functions.set_durability =
            reinterpret_cast<void(GUEST_HOOK_ABI*)(uint64_t, int32_t, int32_t)>(image + kSetDurabilityOffset);
    } else {
        std::printf("Cheats: Infinite Durability refill not available, the game's item functions differ\n");
    }
    dcp::cheats::SetGuestFunctions(functions);
    runtime_cheats_set_available(1);
    std::printf("Cheats: %zu of %zu native hooks installed\n", installed, hooks.size());
    return 1;
}
