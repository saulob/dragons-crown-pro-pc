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
    uint64_t U64(uint64_t address) const { return Load<uint64_t>(address); }
    uint32_t U32(uint64_t address) const { return Load<uint32_t>(address); }
    int32_t S32(uint64_t address) const { return Load<int32_t>(address); }
};

bool Enabled(Cheat cheat) {
    return runtime_cheat_enabled(static_cast<int>(cheat)) != 0;
}

UnitKind Classify(uint64_t unit) {
    return ClassifyUnit(HostMemory{}, unit);
}

constexpr uint64_t kDamageUnitFrameOffset = 0x200; // the victim unit, [rbp-0x200] in the damage function
constexpr uint64_t kSkillPointsOffset = 0x24;      // int16 in the player's save record
constexpr uint64_t kUnitContextVirtual = 0xd0;     // unit->vtable[+0xd0]() is the game object
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
    if (amount <= 0 || !Enabled(Cheat::InfiniteHealth)) {
        return;
    }
    const uint64_t unit = HostMemory{}.U64(regs->rbp - kDamageUnitFrameOffset);
    if (unit == 0 || HostMemory{}.U64(unit + kUnitOwnerOffset) != regs->r15) {
        return;
    }
    regs->rcx = static_cast<uint32_t>(ProtectedDamage(true, Classify(unit), amount));
}

// At the entry of UseMP (rdi = the caster's unit, ecx = the consume flag).
void GUEST_HOOK_ABI ManaCostHook(GuestRegs* regs) {
    if (!Enabled(Cheat::InfiniteMana)) {
        return;
    }
    regs->rcx = ProtectedConsumeFlag(true, Classify(regs->rdi), static_cast<uint32_t>(regs->rcx) & 0xff);
}

// At the entry of AddMP (rdi = the unit, esi = the delta).
void GUEST_HOOK_ABI ManaDrainHook(GuestRegs* regs) {
    if (!Enabled(Cheat::InfiniteMana)) {
        return;
    }
    if (SuppressesManaDelta(true, Classify(regs->rdi), static_cast<int32_t>(regs->rsi))) {
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
    if (!Enabled(Cheat::MovementSpeed)) {
        return;
    }
    const HostMemory memory;
    const uint64_t multiplier_address = regs->rbp - 0x48;
    const UnitKind kind = Classify(memory.U64(regs->rbp - 0x80));
    const float multiplier = HostMemory::Load<float>(multiplier_address);
    const float scaled = ScaledWalkMultiplier(true, kind, multiplier);
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
    if (!Enabled(Cheat::InfiniteDurability)) {
        return;
    }
    const HostMemory memory;
    const uint64_t unit = memory.U64(regs->rbp - 0x308);
    const uint64_t delta_address = regs->rbp - 0x2fc;
    const int32_t delta = memory.S32(delta_address);
    const int32_t kept = ProtectedDurabilityDelta(true, Classify(unit), delta);
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
    const bool add_gold = runtime_cheat_action_pending(static_cast<int>(Action::AddGold)) != 0;
    const bool arrows = Enabled(Cheat::InfiniteArrows);
    if (refills == 0 && !add_gold && !arrows) {
        return;
    }
    if (Classify(regs->rdi) != UnitKind::PlayerOne) {
        return;
    }
    if (arrows) {
        HoldArrows(regs->rdi);
    }
    if ((refills & (Bit(Cheat::InfiniteHealth) | Bit(Cheat::InfiniteMana))) != 0) {
        ApplyRefills(regs->rdi, refills);
    }
    if ((refills & Bit(Cheat::InfiniteDurability)) != 0) {
        ApplyDurabilityRefill(regs->rdi);
    }
    if (add_gold) {
        ApplyAddGold(regs->rdi);
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

uint64_t GUEST_HOOK_ABI GameGoldContext(uint64_t unit) {
    using Virtual = uint64_t(GUEST_HOOK_ABI*)(uint64_t);
    const dcp::cheats::HostMemory memory;
    const uint64_t method = memory.U64(memory.U64(unit) + dcp::cheats::kUnitContextVirtual);
    const uint64_t game = reinterpret_cast<Virtual>(method)(unit);
    return game == 0 ? 0 : memory.U64(game + dcp::cheats::kGameGoldContextOffset);
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
