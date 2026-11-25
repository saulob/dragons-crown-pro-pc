#ifndef DCP_CHARACTER_OVERRIDE_HOOKS_H
#define DCP_CHARACTER_OVERRIDE_HOOKS_H

#include <stddef.h>
#include <stdint.h>

#include "appearance_hooks.h"
#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CHARACTER_OVERRIDE_HOOK_COUNT = 6,
    CHARACTER_OVERRIDE_VIRTUAL_UPDATE = 0x28,
    CHARACTER_OVERRIDE_UNIT_CURRENT = 0xb0,
    CHARACTER_OVERRIDE_UNIT_MAX = 0xb4,
    CHARACTER_OVERRIDE_UNIT_STRENGTH = 0xbc,
    CHARACTER_OVERRIDE_UNIT_INTELLIGENCE = 0xbe,
    CHARACTER_OVERRIDE_UNIT_DEXTERITY = 0xc0,
    CHARACTER_OVERRIDE_UNIT_LUCK = 0xc2,
    CHARACTER_OVERRIDE_UNIT_CONSTITUTION = 0xc4,
    CHARACTER_OVERRIDE_UNIT_MAGIC_RESISTANCE = 0xc6,
    CHARACTER_OVERRIDE_UNIT_EFFECTIVE_STRENGTH = 0x10c,
    CHARACTER_OVERRIDE_UNIT_EFFECTIVE_INTELLIGENCE = 0x110,
    CHARACTER_OVERRIDE_UNIT_EFFECTIVE_DEXTERITY = 0x114,
    CHARACTER_OVERRIDE_UNIT_EFFECTIVE_MAGIC_RESISTANCE = 0x118,
    CHARACTER_OVERRIDE_UNIT_EFFECTIVE_CONSTITUTION = 0x11c,
    CHARACTER_OVERRIDE_UNIT_EFFECTIVE_LUCK = 0x120,
    CHARACTER_OVERRIDE_STATUS_RECORD = 0x5150, /* in the status viewer: the character record it draws */
    CHARACTER_OVERRIDE_STATUS_MAX = 0x6c30,    /* in the status viewer: the HP with equipment the screen prints */
    CHARACTER_OVERRIDE_STATUS_STRENGTH = 0x6c34,
    CHARACTER_OVERRIDE_STATUS_INTELLIGENCE = 0x6c38,
    CHARACTER_OVERRIDE_STATUS_CONSTITUTION = 0x6c3c,
    CHARACTER_OVERRIDE_STATUS_MAGIC_RESISTANCE = 0x6c40,
    CHARACTER_OVERRIDE_STATUS_DEXTERITY = 0x6c44,
    CHARACTER_OVERRIDE_STATUS_LUCK = 0x6c48
};

enum {
    CHARACTER_OVERRIDE_MAX_HP = 1u << 0,
    CHARACTER_OVERRIDE_STRENGTH = 1u << 1,
    CHARACTER_OVERRIDE_CONSTITUTION = 1u << 2,
    CHARACTER_OVERRIDE_DEXTERITY = 1u << 3,
    CHARACTER_OVERRIDE_INTELLIGENCE = 1u << 4,
    CHARACTER_OVERRIDE_MAGIC_RESISTANCE = 1u << 5,
    CHARACTER_OVERRIDE_LUCK = 1u << 6,
    CHARACTER_OVERRIDE_ALL = (1u << 7) - 1
};

typedef struct {
    uint8_t identity[APPEARANCE_IDENTITY_BYTES];
    uint8_t klass;
    uint16_t present;
    uint16_t max_hp;
    uint16_t strength;
    uint16_t constitution;
    uint16_t dexterity;
    uint16_t intelligence;
    uint16_t magic_resistance;
    uint16_t luck;
} CharacterOverrideEntry;

typedef struct {
    uint64_t recalc_entry;
    uint64_t recalc_post;
    uint64_t spawn_load;
    uint64_t status_max;
    uint64_t status_stats;
    uint64_t bundle_stats;
    uint64_t effect_stats;
    uint64_t player_update; /* not located here: the player classes' per-tick update (dcp_resolve_party) */
} CharacterOverrideSites;

/* 1 when the ApplyLevelStats entry and post tail, the character spawn caller, and the status viewer's HP and stat stores
 * are uniquely located. */
int character_override_hooks_locate(const uint8_t *image, uint64_t size, CharacterOverrideSites *sites);

/* The override for a local runtime character record, or NULL (native). */
const CharacterOverrideEntry *character_override_of_record(uint64_t record);

/* The override for a saved character record shown by the status/preview screens, or NULL (native). Those records sit in
 * the save table, which has no party seat owner. */
const CharacterOverrideEntry *character_override_of_status_record(uint64_t record);

/* Borrowed for the life of the process. */
void character_override_hooks_set_entries(const CharacterOverrideEntry *entries, size_t count);

/* The record a unit plays: its party seat's. Replaced in tests. */
typedef uint64_t (*CharacterOverrideRecordOfUnit)(uint64_t unit);
/* Enemies run the hooked functions too and keep other data where a player keeps its slot, so only a unit whose class
 * updates through `address` is looked up. */
void character_override_hooks_set_player_update(uint64_t address);
void character_override_hooks_set_record_of_unit(CharacterOverrideRecordOfUnit record_of_unit);

size_t character_override_hooks_stub_bytes(void);

/* Installs all hooks or none; returns 1 when installed. */
int character_override_hooks_install(uint8_t *image, uint64_t size, uint64_t entry, const CharacterOverrideSites *sites,
                                     uint8_t *stubs, size_t stubs_size);

void GUEST_HOOK_ABI character_override_pre_recalc(GuestRegs *regs);
void GUEST_HOOK_ABI character_override_post_recalc(GuestRegs *regs);
void GUEST_HOOK_ABI character_override_status_max(GuestRegs *regs);
void GUEST_HOOK_ABI character_override_status_stats(GuestRegs *regs);
void GUEST_HOOK_ABI character_override_bundle_stats(GuestRegs *regs);
void GUEST_HOOK_ABI character_override_effect_stats(GuestRegs *regs);

#ifdef __cplusplus
}
#endif

#endif
