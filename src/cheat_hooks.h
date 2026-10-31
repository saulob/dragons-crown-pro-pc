// The native hooks behind the cheats: tables for CUSA10454 1.00, located sites for the other builds. The handlers are
// exposed for the tests.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "feature_sites.h"
#include "guest_hook.h"

namespace dcp::cheats {

// The one supported image (the boot image built from the eboot with SHA-256 f9bff150...3508a).
inline constexpr GuestImageId kSupportedImage{0x11f8060, 0x54c690};

void GUEST_HOOK_ABI HealthDamageHook(GuestRegs* regs);
void GUEST_HOOK_ABI ManaCostHook(GuestRegs* regs);
void GUEST_HOOK_ABI ManaDrainHook(GuestRegs* regs);
void GUEST_HOOK_ABI ArrowHook(GuestRegs* regs);
void GUEST_HOOK_ABI SkillPointsHook(GuestRegs* regs);
void GUEST_HOOK_ABI SkillUseHook(GuestRegs* regs);
void GUEST_HOOK_ABI SkillItemWearHook(GuestRegs* regs);
void GUEST_HOOK_ABI SkillBudgetHook(GuestRegs* regs);
void GUEST_HOOK_ABI PlayerTickHook(GuestRegs* regs);
void GUEST_HOOK_ABI DurabilityHook(GuestRegs* regs);
void GUEST_HOOK_ABI LivesHook(GuestRegs* regs);
void GUEST_HOOK_ABI MovementHook(GuestRegs* regs);
void GUEST_HOOK_ABI ExperienceHook(GuestRegs* regs);

// The game functions the refills and the menu's actions call; set from the image when the hooks are installed,
// replaced in the tests. gold_context is the object AddGold and GetGold take, found from a player's unit; save_record
// is that player's save record (Player 1's for slot 0).
struct GuestFunctions {
    uint8_t(GUEST_HOOK_ABI* is_dead)(uint64_t owner) = nullptr;
    void(GUEST_HOOK_ABI* refill)(uint64_t unit, uint32_t health, uint32_t mana, uint32_t only_below_max) = nullptr;
    uint64_t(GUEST_HOOK_ABI* gold_context)(uint64_t unit) = nullptr;
    int32_t(GUEST_HOOK_ABI* get_gold)(uint64_t context) = nullptr;
    void(GUEST_HOOK_ABI* add_gold)(uint64_t context, int32_t amount) = nullptr;
    uint8_t(GUEST_HOOK_ABI* equipment_count)(uint64_t list) = nullptr;
    uint64_t(GUEST_HOOK_ABI* equipment_item)(uint64_t list, int32_t index) = nullptr;
    uint8_t(GUEST_HOOK_ABI* is_gear)(uint64_t item) = nullptr;
    void(GUEST_HOOK_ABI* set_durability)(uint64_t list, int32_t index, int32_t durability) = nullptr;
    uint64_t(GUEST_HOOK_ABI* save_record)(uint64_t unit) = nullptr;
    int32_t(GUEST_HOOK_ABI* record_mode)(uint64_t record) = nullptr;
    uint8_t(GUEST_HOOK_ABI* record_stage)(uint64_t record) = nullptr;
    int32_t(GUEST_HOOK_ABI* max_level)(int32_t mode, int32_t stage, int32_t hard) = nullptr;
    uint64_t(GUEST_HOOK_ABI* level_entry)(int32_t class_index, int32_t level) = nullptr;
    int32_t(GUEST_HOOK_ABI* level_pattern)(uint64_t record) = nullptr;
    void(GUEST_HOOK_ABI* set_level)(uint64_t record, uint32_t level) = nullptr;
    void(GUEST_HOOK_ABI* write_stats)(uint64_t record, int32_t pattern, int32_t arg2, int32_t level,
                                      int32_t arg1) = nullptr;
    int64_t (*now_ms)() = nullptr;
};
void SetGuestFunctions(const GuestFunctions& functions);

std::span<const GuestHook> Hooks();

// The hook table at the sites a locator found; the originals of the call and jump hooks are the image's own.
std::vector<GuestHook> HooksAt(const uint32_t (&offsets)[DCP_CHEAT_HOOKS], const uint8_t* image, uint64_t size);

// The image offsets of the game functions in the order of DcpCheatFunction, for the one supported static image.
struct FunctionOffsets {
    uint64_t at[DCP_CHEAT_FUNCTIONS];
};
FunctionOffsets StaticFunctionOffsets();

// Whether the displacements a build's own instructions encode are the ones the handlers use; `mismatch` names the first
// field that differs.
bool LayoutMatches(const uint32_t (&layout)[DCP_LAYOUT_COUNT], std::string* mismatch = nullptr);

} // namespace dcp::cheats
