#include "cheat_hooks.h"

#include <cstdint>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

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

bool OwnerIsAlly(const HostMemory& memory, uint64_t owner) {
    if (owner == 0) {
        return false;
    }
    const uint64_t unit = memory.U64(owner + kOwnerUnitOffset);
    return unit != 0 && memory.U64(unit + kUnitOwnerOffset) == owner && IsAlly(memory, unit);
}

// The owner is classified first: the item is read only for an owner one of the skill cheats covers.
bool KeepsSkillUse(const HostMemory& memory, uint64_t owner, uint64_t item) {
    const bool player = Enabled(Cheat::InfiniteSkillUses);
    const bool ally = Enabled(Cheat::InfiniteAllySkillUses);
    if ((!player && !ally) || item == 0) {
        return false;
    }
    const UnitKind kind = player ? ClassifyOwner(memory, owner) : UnitKind::Other;
    const bool is_ally = ally && OwnerIsAlly(memory, owner);
    if (kind != UnitKind::PlayerOne && !is_ally) {
        return false;
    }
    const int32_t uses = memory.U8(item + kItemUsesOffset);
    return PreservedSkillUse(player, kind, uses) || PreservedAllySkillUse(ally, is_ally, uses);
}

constexpr uint64_t kDamageUnitFrameOffset = 0x200; // the victim unit, [rbp-0x200] in the damage function
constexpr uint64_t kUnitContextVirtual = 0xd0;     // unit->vtable[+0xd0]() is the game object
constexpr uint64_t kGameSaveBaseOffset = 0x80;     // the save records, 0x23b0 bytes each, Player 1 at +0xa8
constexpr uint64_t kSaveRecordOffset = 0xa8;
constexpr uint64_t kSaveRecordSize = 0x23b0;
constexpr uint64_t kGameGoldContextOffset = 0x88;  // the object AddGold and GetGold take

GuestFunctions g_guest{};
FrozenHealth g_frozen;

struct PartyUnit {
    uint64_t unit = 0;
    int64_t seen_ms = 0;
};
PartyUnit g_allies[FrozenHealth::kSlots];
constexpr int64_t kAllyStaleMs = 250;
bool g_applying_health_action = false;

int64_t NowMs() {
    if (g_guest.now_ms != nullptr) {
        return g_guest.now_ms();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool IsFrozen(const HostMemory& memory, uint64_t unit) {
    if (Classify(unit) == UnitKind::PlayerOne) {
        return Enabled(Cheat::InfiniteHealth);
    }
    return Enabled(Cheat::InfiniteAllyHealth) && IsAlly(memory, unit);
}

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

// [rbp-0x15c] in the damage function: a hit with this field nonzero sends its HP change as a message instead of
// writing HP there.
constexpr uint64_t kDamageMessageFrameOffset = 0x15c;

bool HitWritesHp(const GuestRegs* regs) {
    const float message = HostMemory::Load<float>(regs->rbp - kDamageMessageFrameOffset);
    return message > -1.0f && message < 1.0f;
}

// Lowers HP so the game's own, unchanged hit takes it to hp - amount * multiplier.
void MultiplyHpLoss(const HostMemory& memory, uint64_t owner, uint64_t unit, UnitKind kind, int multiplier,
                    int32_t amount) {
    if (memory.U64(owner + kOwnerUnitOffset) != unit) {
        return;
    }
    const int32_t hp = memory.S32(unit + kUnitHpOffset);
    const int32_t before_hit = MultipliedHpBeforeHit(multiplier, kind, hp, amount);
    if (before_hit != hp) {
        HostMemory::Store<int32_t>(unit + kUnitHpOffset, before_hit);
    }
}

} // namespace

// In the hit check, before the game compares the target's HP with the hit to choose a lethal reaction or a break
// (eax = the hit, r15 = the target's owner).
void GUEST_HOOK_ABI LethalCheckHook(GuestRegs* regs) {
    const int multiplier = runtime_cheat_damage_multiplier();
    const int32_t amount = static_cast<int32_t>(regs->rax);
    if (multiplier == kDamageMultiplierOff || amount <= 0) {
        return;
    }
    regs->rax = static_cast<uint32_t>(MultipliedDamage(multiplier, ClassifyOwner(HostMemory{}, regs->r15), amount));
}

// In the damage function (rcx = signed amount, positive is a loss; r15 = the victim's owner), just before HP is read.
// Damage Multiplier leaves the amount alone, so every reaction, effect and total sees the game's own hit.
void GUEST_HOOK_ABI HealthDamageHook(GuestRegs* regs) {
    const int32_t amount = static_cast<int32_t>(regs->rcx);
    const int taken = runtime_cheat_damage_taken();
    const int multiplier = runtime_cheat_damage_multiplier();
    if (amount == 0 || g_applying_health_action ||
        (!Enabled(Cheat::InfiniteHealth) && !Enabled(Cheat::InfiniteAllyHealth) && taken == kDamageTakenOff &&
         multiplier == kDamageMultiplierOff)) {
        return;
    }
    const HostMemory memory;
    const uint64_t unit = memory.U64(regs->rbp - kDamageUnitFrameOffset);
    if (unit == 0 || memory.U64(unit + kUnitOwnerOffset) != regs->r15) {
        return;
    }
    const UnitKind kind = Classify(unit);
    if (kind == UnitKind::Other) {
        if (HitWritesHp(regs)) {
            MultiplyHpLoss(memory, regs->r15, unit, kind, multiplier, amount);
        }
        return;
    }
    const int32_t kept = FrozenHealthDelta(true, IsFrozen(memory, unit), amount);
    regs->rcx = static_cast<uint32_t>(ScaledDamageTaken(taken, kind, kept));
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

// At the item-count decrement of the skill-item use function (rdi = item, r12 = the using owner).
void GUEST_HOOK_ABI SkillUseHook(GuestRegs* regs) {
    if (KeepsSkillUse(HostMemory{}, regs->r12, regs->rdi)) {
        regs->skip = 1;
    }
}

// At the same function's bag lookup (rdi = the bag, esi = the slot, r12 = the using owner). Its bag decrement is a tail
// jump made after the epilogue has given r12 back to the caller, so the owner is only known here. A skipped lookup finds
// no record, and the function returns without that decrement.
void GUEST_HOOK_ABI SkillUseMirrorHook(GuestRegs* regs) {
    if (KeepsSkillUse(HostMemory{}, regs->r12, BagItem(regs->rdi, static_cast<uint32_t>(regs->rsi)))) {
        regs->rax = 0;
        regs->skip = 1;
    }
}

// At the two item-count decrements of the equipment wear function (rdi = item, rbx = the unit's equipment list). Every
// skill item the skill-item use function does not handle (the Dwarf's, for one) is counted down here, as are consumables.
void GUEST_HOOK_ABI SkillItemWearHook(GuestRegs* regs) {
    const bool player_skills = Enabled(Cheat::InfiniteSkillUses);
    const bool player_items = Enabled(Cheat::InfiniteItems);
    const bool ally_skills = Enabled(Cheat::InfiniteAllySkillUses);
    const bool ally_items = Enabled(Cheat::InfiniteAllyItems);
    if ((!player_skills && !player_items && !ally_skills && !ally_items) || regs->rdi == 0) {
        return;
    }
    const HostMemory memory;
    const bool skill_item =
        IsSkillItem(memory.U32(regs->rdi + kItemFlagsOffset), HostMemory::Load<uint16_t>(regs->rdi));
    const uint64_t unit = regs->rbx - kUnitEquipmentOffset;
    const int32_t uses = memory.U8(regs->rdi + kItemUsesOffset);
    const UnitKind kind = Classify(unit);
    const bool is_ally = IsAlly(memory, unit);
    if (PreservedSkillUse(skill_item ? player_skills : player_items, kind, uses) ||
        PreservedAllySkillUse(skill_item ? ally_skills : ally_items, is_ally, uses)) {
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
    const RefillPlan plan = PlanRefill(false,
                                       (pending & Bit(Cheat::InfiniteMana)) != 0, memory.U32(owner + kOwnerClassOffset),
                                       g_guest.is_dead(owner) != 0);
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

// Keeps a living AI ally's MP topped up through the game's own fill function.
void ApplyAllyManaRefill(uint64_t unit) {
    if (g_guest.refill == nullptr || g_guest.is_dead == nullptr) {
        return;
    }
    const HostMemory memory;
    const uint64_t owner = memory.U64(unit + kUnitOwnerOffset);
    if (owner == 0 || g_guest.is_dead(owner) != 0 || memory.S32(unit + kUnitHpOffset) <= 0) {
        return;
    }
    const uint8_t class_index = memory.U8(unit + kUnitClassOffset);
    if (class_index != kSorceressClass && class_index != kWizardClass) {
        return;
    }
    g_guest.refill(unit, 0, 1, 1);
}

bool UnitAlive(const HostMemory& memory, uint64_t unit) {
    const uint64_t owner = memory.U64(unit + kUnitOwnerOffset);
    return owner != 0 && (g_guest.is_dead == nullptr || g_guest.is_dead(owner) == 0) &&
           memory.S32(unit + kUnitHpOffset) > 0;
}

void HoldFrozenHealth(uint64_t unit) {
    const HostMemory memory;
    const int32_t slot = memory.S32(unit + kUnitPlayerSlotOffset);
    const int32_t hp = memory.S32(unit + kUnitHpOffset);
    const int32_t held = g_frozen.Hold(slot, unit, hp, NowMs());
    if (held != hp && hp > 0 && UnitAlive(memory, unit)) {
        HostMemory::Store<int32_t>(unit + kUnitHpOffset, held);
    }
}

// Fills a living party member through the game's fill function, then lowers it to 20% for Set Low Health.
void ApplyHealthAction(uint64_t unit, bool low) {
    const HostMemory memory;
    if (g_guest.refill == nullptr || g_guest.is_dead == nullptr || !UnitAlive(memory, unit)) {
        return;
    }
    g_applying_health_action = true;
    g_guest.refill(unit, 1, 0, 1);
    g_applying_health_action = false;
    int32_t hp = memory.S32(unit + kUnitHpOffset);
    if (low) {
        hp = LowHealth(hp);
        HostMemory::Store<int32_t>(unit + kUnitHpOffset, hp);
    }
    if (IsFrozen(memory, unit)) {
        g_frozen.Set(memory.S32(unit + kUnitPlayerSlotOffset), unit, hp, NowMs());
    }
}

bool TakeAction(Action action) {
    return runtime_cheat_take_action(static_cast<int>(action)) != 0;
}

bool HealthActionPending() {
    for (const Action action : {Action::FillHealth, Action::SetLowHealth, Action::FillAllyHealth, Action::SetAllyLowHealth}) {
        if (runtime_cheat_action_pending(static_cast<int>(action)) != 0) {
            return true;
        }
    }
    return false;
}

void RememberAlly(uint64_t unit) {
    const int32_t slot = HostMemory{}.S32(unit + kUnitPlayerSlotOffset);
    if (slot > kPlayerOneSlot && slot <= kLastPartySlot) {
        g_allies[slot] = {unit, NowMs()};
    }
}

void ApplyToAllies(bool low) {
    const HostMemory memory;
    const int64_t now = NowMs();
    for (int32_t slot = kPlayerOneSlot + 1; slot <= kLastPartySlot; ++slot) {
        const PartyUnit& ally = g_allies[slot];
        if (ally.unit == 0 || now - ally.seen_ms > kAllyStaleMs || Classify(ally.unit) != UnitKind::OtherPlayer ||
            !IsAlly(memory, ally.unit) || memory.S32(ally.unit + kUnitPlayerSlotOffset) != slot) {
            continue;
        }
        ApplyHealthAction(ally.unit, low);
    }
}

void ApplyHealthActions(uint64_t unit) {
    const bool fill = TakeAction(Action::FillHealth);
    const bool low = TakeAction(Action::SetLowHealth);
    if (fill || low) {
        ApplyHealthAction(unit, low);
    }
    const bool fill_allies = TakeAction(Action::FillAllyHealth);
    const bool low_allies = TakeAction(Action::SetAllyLowHealth);
    if (fill_allies || low_allies) {
        ApplyToAllies(low_allies);
    }
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
    runtime_cheat_observe_player(regs->rdi);
    const unsigned refills = runtime_cheat_refills_pending();
    const bool add_skill_points = runtime_cheat_action_pending(static_cast<int>(Action::AddSkillPoints)) != 0;
    const bool add_gold = runtime_cheat_action_pending(static_cast<int>(Action::AddGold)) != 0;
    const bool add_level = runtime_cheat_action_pending(static_cast<int>(Action::AddLevel)) != 0;
    const bool health_action = HealthActionPending();
    const bool arrows = Enabled(Cheat::InfiniteArrows);
    const bool player_frozen = Enabled(Cheat::InfiniteHealth);
    const bool ally_frozen = Enabled(Cheat::InfiniteAllyHealth);
    const bool ally_mana = Enabled(Cheat::InfiniteAllyMana);
    if (!player_frozen) {
        g_frozen.ClearSlot(kPlayerOneSlot);
    }
    if (!ally_frozen) {
        for (int32_t slot = kPlayerOneSlot + 1; slot <= kLastPartySlot; ++slot) {
            g_frozen.ClearSlot(slot);
        }
    }
    const UnitKind kind = Classify(regs->rdi);
    const bool ally = kind == UnitKind::OtherPlayer && IsAlly(HostMemory{}, regs->rdi);
    if (ally) {
        RememberAlly(regs->rdi);
    }
    if (refills == 0 && !add_skill_points && !add_gold && !add_level && !arrows && !player_frozen && !ally_frozen &&
        !ally_mana &&
        !health_action) {
        return;
    }
    if (kind == UnitKind::PlayerOne) {
        if (health_action) {
            ApplyHealthActions(regs->rdi);
        }
        if (player_frozen) {
            HoldFrozenHealth(regs->rdi);
        }
        if (arrows) {
            HoldArrows(regs->rdi);
        }
        if ((refills & Bit(Cheat::InfiniteMana)) != 0) {
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
    if (ally) {
        if (ally_frozen) {
            HoldFrozenHealth(regs->rdi);
        }
        if (ally_mana) {
            ApplyAllyManaRefill(regs->rdi);
        }
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
        {"skill use mirror", 0x1af110, 5, {0xe8, 0x4b, 0x38, 0xe7, 0xff}, SkillUseMirrorHook},
        {"skill item wear", 0x225f90, 5, {0xe8, 0xbb, 0xa4, 0xdf, 0xff}, SkillItemWearHook},
        {"skill item wear mirror", 0x225fb1, 5, {0xe8, 0x9a, 0xa4, 0xdf, 0xff}, SkillItemWearHook},
        {"skill budget", 0x8ab24, 12, {0x8b, 0x5d, 0xa4, 0x45, 0x01, 0xf4, 0x41, 0x01, 0xdd, 0x45, 0x29, 0xec},
         SkillBudgetHook},
        {"lethal check", 0x29def5, 7, {0x4d, 0x8b, 0xaf, 0xb8, 0x09, 0x00, 0x00}, LethalCheckHook},
    };
    return hooks;
}

std::vector<GuestHook> HooksAt(const uint32_t (&offsets)[DCP_CHEAT_HOOKS], const uint8_t* image, uint64_t size) {
    const std::span<const GuestHook> base = Hooks();
    std::vector<GuestHook> hooks(base.begin(), base.end());
    for (size_t i = 0; i < hooks.size(); ++i) {
        hooks[i].offset = offsets[i];
        const bool branch = hooks[i].length == 5 && (hooks[i].original[0] == 0xe8 || hooks[i].original[0] == 0xe9);
        if (branch && uint64_t(offsets[i]) + 5 <= size) {
            std::memcpy(hooks[i].original, image + offsets[i], 5);
        }
    }
    return hooks;
}

bool LayoutMatches(const uint32_t (&layout)[DCP_LAYOUT_COUNT], std::string* mismatch) {
    static_assert(kRecordClassOffset == kRecordLevelOffset + 1, "the record's class is the byte after its level");
    struct Expected {
        DcpLayoutField field;
        uint32_t value;
    };
    const Expected expected[] = {
        {DCP_LAYOUT_DAMAGE_UNIT_FRAME, static_cast<uint32_t>(0u - static_cast<uint32_t>(kDamageUnitFrameOffset))},
        {DCP_LAYOUT_CONTEXT_VIRTUAL, static_cast<uint32_t>(kUnitContextVirtual)},
        {DCP_LAYOUT_UNIT_OWNER, kUnitOwnerOffset},
        {DCP_LAYOUT_UNIT_PLAYER_SLOT, kUnitPlayerSlotOffset},
        {DCP_LAYOUT_UNIT_CLASS, kUnitClassOffset},
        {DCP_LAYOUT_UNIT_LEVEL, kUnitLevelOffset},
        {DCP_LAYOUT_UNIT_HP, kUnitHpOffset},
        {DCP_LAYOUT_UNIT_STAT, kUnitStatOffset},
        {DCP_LAYOUT_UNIT_XP, kUnitXpOffset},
        {DCP_LAYOUT_UNIT_XP_PROGRESS, kUnitXpProgressOffset},
        {DCP_LAYOUT_UNIT_EQUIPMENT, kUnitEquipmentOffset},
        {DCP_LAYOUT_OWNER_CATEGORY, kOwnerCategoryOffset},
        {DCP_LAYOUT_OWNER_UNIT, kOwnerUnitOffset},
        {DCP_LAYOUT_OWNER_CLASS, kOwnerClassOffset},
        {DCP_LAYOUT_ARROW_COUNT, kArrowCountOffset},
        {DCP_LAYOUT_EQUIPMENT_OWNER, kEquipmentOwnerOffset},
        {DCP_LAYOUT_EQUIPMENT_BAG, kEquipmentBagOffset},
        {DCP_LAYOUT_ITEM_FLAGS, kItemFlagsOffset},
        {DCP_LAYOUT_ITEM_DURABILITY, kItemDurabilityOffset},
        {DCP_LAYOUT_ITEM_MAX_DURABILITY, kItemMaxDurabilityOffset},
        {DCP_LAYOUT_ITEM_USES, kItemUsesOffset},
        {DCP_LAYOUT_GOLD, kGoldOffset},
        {DCP_LAYOUT_GAME_SAVE_BASE, static_cast<uint32_t>(kGameSaveBaseOffset)},
        {DCP_LAYOUT_SAVE_RECORD_OFFSET, static_cast<uint32_t>(kSaveRecordOffset)},
        {DCP_LAYOUT_SAVE_RECORD_SIZE, static_cast<uint32_t>(kSaveRecordSize)},
        {DCP_LAYOUT_GAME_GOLD_CONTEXT, static_cast<uint32_t>(kGameGoldContextOffset)},
        {DCP_LAYOUT_SKILL_POINTS, kSkillPointsOffset},
        {DCP_LAYOUT_SKILL_POINTS_MIRROR, kSkillPointsMirrorOffset},
        {DCP_LAYOUT_RECORD_LEVEL, kRecordLevelOffset},
        {DCP_LAYOUT_RECORD_XP_POOL, kRecordXpPoolOffset},
        {DCP_LAYOUT_RECORD_XP_PROGRESS, kRecordXpProgressOffset},
        {DCP_LAYOUT_LEVEL_XP, kLevelXpOffset},
        {DCP_LAYOUT_ABSOLUTE_MAX_LEVEL, static_cast<uint32_t>(kAbsoluteMaxLevel)},
    };
    static_assert(sizeof(expected) / sizeof(expected[0]) == DCP_LAYOUT_COUNT);
    for (const Expected& item : expected) {
        if (layout[item.field] != item.value) {
            if (mismatch) {
                *mismatch = dcp_layout_field_name(item.field);
            }
            return false;
        }
    }
    return true;
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

namespace {

using dcp::cheats::FunctionOffsets;

FunctionOffsets StaticOffsets() {
    FunctionOffsets f{};
    f.at[DCP_CHEAT_FN_IS_DEAD] = kIsDeadOffset;
    f.at[DCP_CHEAT_FN_REFILL] = kRefillOffset;
    f.at[DCP_CHEAT_FN_GET_GOLD] = kGetGoldOffset;
    f.at[DCP_CHEAT_FN_ADD_GOLD] = kAddGoldOffset;
    f.at[DCP_CHEAT_FN_RECORD_MODE] = kRecordModeOffset;
    f.at[DCP_CHEAT_FN_RECORD_STAGE] = kRecordStageOffset;
    f.at[DCP_CHEAT_FN_MAX_LEVEL] = kMaxLevelOffset;
    f.at[DCP_CHEAT_FN_LEVEL_ENTRY] = kLevelEntryOffset;
    f.at[DCP_CHEAT_FN_LEVEL_PATTERN] = kLevelPatternOffset;
    f.at[DCP_CHEAT_FN_SET_LEVEL] = kSetLevelOffset;
    f.at[DCP_CHEAT_FN_WRITE_STATS] = kWriteStatsOffset;
    f.at[DCP_CHEAT_FN_EQUIPMENT_COUNT] = kEquipmentCountOffset;
    f.at[DCP_CHEAT_FN_EQUIPMENT_ITEM] = kEquipmentItemOffset;
    f.at[DCP_CHEAT_FN_IS_GEAR] = kIsGearOffset;
    f.at[DCP_CHEAT_FN_SET_DURABILITY] = kSetDurabilityOffset;
    return f;
}

// The game functions the actions call. The static image's are checked by prologue; a located set was matched by the
// locator's signatures already.
void BindFunctions(uint8_t* image, const FunctionOffsets& f, bool check_prologues) {
    dcp::cheats::GuestFunctions functions;
    functions.save_record = GameSaveRecord;
    const auto at = [&](DcpCheatFunction function) { return image + f.at[function]; };
    const auto fits = [&](bool matches) { return !check_prologues || matches; };
    if (fits(PrologueMatches(image, kRecordModeOffset, kRecordModePrologue) &&
             PrologueMatches(image, kRecordStageOffset, kRecordStagePrologue) &&
             PrologueMatches(image, kMaxLevelOffset, kMaxLevelPrologue) &&
             PrologueMatches(image, kLevelEntryOffset, kLevelEntryPrologue) &&
             PrologueMatches(image, kLevelPatternOffset, kLevelPatternPrologue) &&
             PrologueMatches(image, kSetLevelOffset, kSetLevelPrologue) &&
             PrologueMatches(image, kWriteStatsOffset, kWriteStatsPrologue))) {
        functions.record_mode = reinterpret_cast<int32_t(GUEST_HOOK_ABI*)(uint64_t)>(at(DCP_CHEAT_FN_RECORD_MODE));
        functions.record_stage = reinterpret_cast<uint8_t(GUEST_HOOK_ABI*)(uint64_t)>(at(DCP_CHEAT_FN_RECORD_STAGE));
        functions.max_level =
            reinterpret_cast<int32_t(GUEST_HOOK_ABI*)(int32_t, int32_t, int32_t)>(at(DCP_CHEAT_FN_MAX_LEVEL));
        functions.level_entry = reinterpret_cast<uint64_t(GUEST_HOOK_ABI*)(int32_t, int32_t)>(at(DCP_CHEAT_FN_LEVEL_ENTRY));
        functions.level_pattern = reinterpret_cast<int32_t(GUEST_HOOK_ABI*)(uint64_t)>(at(DCP_CHEAT_FN_LEVEL_PATTERN));
        functions.set_level = reinterpret_cast<void(GUEST_HOOK_ABI*)(uint64_t, uint32_t)>(at(DCP_CHEAT_FN_SET_LEVEL));
        functions.write_stats = reinterpret_cast<void(GUEST_HOOK_ABI*)(uint64_t, int32_t, int32_t, int32_t, int32_t)>(
            at(DCP_CHEAT_FN_WRITE_STATS));
    } else {
        std::printf("Cheats: Level Up (+1 Level) not available, the game's level functions differ\n");
    }
    if (fits(PrologueMatches(image, kIsDeadOffset, kIsDeadPrologue) && PrologueMatches(image, kRefillOffset, kRefillPrologue))) {
        functions.is_dead = reinterpret_cast<uint8_t(GUEST_HOOK_ABI*)(uint64_t)>(at(DCP_CHEAT_FN_IS_DEAD));
        functions.refill = reinterpret_cast<void(GUEST_HOOK_ABI*)(uint64_t, uint32_t, uint32_t, uint32_t)>(at(DCP_CHEAT_FN_REFILL));
    } else {
        std::printf("Cheats: the Health and Mana refill is not available, the game's fill functions differ\n");
    }
    if (fits(PrologueMatches(image, kGetGoldOffset, kGetGoldPrologue) && PrologueMatches(image, kAddGoldOffset, kAddGoldPrologue))) {
        functions.gold_context = GameGoldContext;
        functions.get_gold = reinterpret_cast<int32_t(GUEST_HOOK_ABI*)(uint64_t)>(at(DCP_CHEAT_FN_GET_GOLD));
        functions.add_gold = reinterpret_cast<void(GUEST_HOOK_ABI*)(uint64_t, int32_t)>(at(DCP_CHEAT_FN_ADD_GOLD));
    } else {
        std::printf("Cheats: Add 1,000 Gold not available, the game's gold functions differ\n");
    }
    if (fits(PrologueMatches(image, kEquipmentCountOffset, kEquipmentCountPrologue) &&
             PrologueMatches(image, kEquipmentItemOffset, kEquipmentItemPrologue) &&
             PrologueMatches(image, kIsGearOffset, kIsGearPrologue) &&
             PrologueMatches(image, kSetDurabilityOffset, kSetDurabilityPrologue))) {
        functions.equipment_count = reinterpret_cast<uint8_t(GUEST_HOOK_ABI*)(uint64_t)>(at(DCP_CHEAT_FN_EQUIPMENT_COUNT));
        functions.equipment_item = reinterpret_cast<uint64_t(GUEST_HOOK_ABI*)(uint64_t, int32_t)>(at(DCP_CHEAT_FN_EQUIPMENT_ITEM));
        functions.is_gear = reinterpret_cast<uint8_t(GUEST_HOOK_ABI*)(uint64_t)>(at(DCP_CHEAT_FN_IS_GEAR));
        functions.set_durability =
            reinterpret_cast<void(GUEST_HOOK_ABI*)(uint64_t, int32_t, int32_t)>(at(DCP_CHEAT_FN_SET_DURABILITY));
    } else {
        std::printf("Cheats: Infinite Durability refill not available, the game's item functions differ\n");
    }
    dcp::cheats::SetGuestFunctions(functions);
    runtime_cheats_set_available(1);
}

} // namespace

dcp::cheats::FunctionOffsets dcp::cheats::StaticFunctionOffsets() {
    return StaticOffsets();
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
    BindFunctions(image, StaticOffsets(), true);
    std::printf("Cheats: %zu of %zu native hooks installed\n", installed, hooks.size());
    return 1;
}

extern "C" int runtime_cheat_hooks_install_located(uint8_t* image, uint64_t size, uint64_t entry, const DcpCheatSites* sites,
                                                   uint8_t* stubs, size_t stubs_size) {
    std::string mismatch;
    if (!sites || !dcp::cheats::LayoutMatches(sites->layout, &mismatch)) {
        std::printf("Cheats: unavailable, the game's %s displacement differs from the handlers'\n",
                    mismatch.empty() ? "layout" : mismatch.c_str());
        return 0;
    }
    const std::vector<GuestHook> hooks = dcp::cheats::HooksAt(sites->hooks, image, size);
    const GuestImageId id{size, entry};
    size_t bad = 0;
    const GuestHookStatus status =
        guest_hooks_install(image, size, entry, &id, hooks.data(), hooks.size(), stubs, stubs_size, &bad);
    if (status != GUEST_HOOK_OK) {
        std::printf("Cheats: unavailable, hook '%s': %s\n", bad < hooks.size() ? hooks[bad].name : "?",
                    guest_hook_status_text(status));
        return 0;
    }
    FunctionOffsets functions{};
    for (size_t i = 0; i < DCP_CHEAT_FUNCTIONS; ++i) functions.at[i] = sites->functions[i];
    BindFunctions(image, functions, false);
    std::printf("Cheats: %zu/%zu hooks, %d/%d functions, layout verified by signature\n", hooks.size(), hooks.size(),
                DCP_CHEAT_FUNCTIONS, DCP_CHEAT_FUNCTIONS);
    return 1;
}
