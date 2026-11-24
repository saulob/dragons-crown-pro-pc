#ifndef DCP_HP_OVERRIDE_HOOKS_H
#define DCP_HP_OVERRIDE_HOOKS_H

#include <stddef.h>
#include <stdint.h>

#include "appearance_hooks.h"
#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HP_OVERRIDE_HOOK_COUNT = 3,
    HP_OVERRIDE_UNIT_CURRENT = 0xb0,
    HP_OVERRIDE_UNIT_MAX = 0xb4,
    HP_OVERRIDE_STATUS_RECORD = 0x5150, /* in the status viewer: the character record it draws */
    HP_OVERRIDE_STATUS_MAX = 0x6c30     /* in the status viewer: the HP with equipment the screen prints */
};

typedef struct {
    uint8_t identity[APPEARANCE_IDENTITY_BYTES];
    uint8_t klass;
    uint16_t max_hp;
} HpOverrideEntry;

typedef struct {
    uint64_t recalc_entry;
    uint64_t recalc_post;
    uint64_t spawn_load;
    uint64_t status_max;
} HpOverrideSites;

/* 1 when the ApplyLevelStats entry and post tail, the character spawn caller, and the status viewer's HP store are
 * uniquely located. */
int hp_override_hooks_locate(const uint8_t *image, uint64_t size, HpOverrideSites *sites);

/* The override for a local runtime character record, or NULL (native). */
const HpOverrideEntry *hp_override_of_record(uint64_t record);

/* The override for a saved character record shown by the status/preview screens, or NULL (native). Those records sit
 * in the save table, which has no party seat owner. */
const HpOverrideEntry *hp_override_of_status_record(uint64_t record);

/* Borrowed for the life of the process. */
void hp_override_hooks_set_entries(const HpOverrideEntry *entries, size_t count);

/* The record a unit plays: its party seat's. Replaced in tests. */
typedef uint64_t (*HpOverrideRecordOfUnit)(uint64_t unit);
void hp_override_hooks_set_record_of_unit(HpOverrideRecordOfUnit record_of_unit);

size_t hp_override_hooks_stub_bytes(void);

/* Installs all three hooks or none; returns 1 when installed. */
int hp_override_hooks_install(uint8_t *image, uint64_t size, uint64_t entry, const HpOverrideSites *sites,
                              uint8_t *stubs, size_t stubs_size);

void GUEST_HOOK_ABI hp_override_pre_recalc(GuestRegs *regs);
void GUEST_HOOK_ABI hp_override_post_recalc(GuestRegs *regs);
void GUEST_HOOK_ABI hp_override_status_max(GuestRegs *regs);

#ifdef __cplusplus
}
#endif

#endif
