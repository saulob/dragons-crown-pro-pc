#include "hp_override_hooks.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum {
    UNIT_PLAYER_SLOT = 0xa70,
    UNIT_CONTEXT_VIRTUAL = 0xd0,
    GAME_PARTY = 0x80,
    PARTY_SEATS = 0xa8,
    SEAT_SIZE = 0x23b0,
    SEAT_COUNT = 4,
    kEntryHookLength = 6,
    kPostHookOffsetInTail = 13,
    kPostHookLength = 7,
    kSpawnCallOffset = 35,
    kStatusStoreLength = 7
};

/* ApplyLevelStats prologue: push rbp; mov rbp,rsp; push r15..rbx,rax; mov r12,rdi; mov r14d,esi; cmp byte [r12+0xcb],-1 */
static const uint8_t kRecalcEntry[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50, 0x49, 0x89,
    0xfc, 0x41, 0x89, 0xf6, 0x41, 0x80, 0xbc, 0x24, 0xcb, 0x00, 0x00, 0x00, 0xff,
};

/* ApplyLevelStats tail after class/level recalc and vtable[0x9f0] equipment/skill pass. */
static const uint8_t kRecalcTail[] = {
    0x49, 0x8b, 0x04, 0x24, 0x4c, 0x89, 0xe7, 0xff, 0x90, 0xf0, 0x09, 0x00, 0x00,
    0x41, 0x0f, 0xb6, 0xf6, 0x4c, 0x89, 0xff, 0x48, 0x83, 0xc4, 0x08,
    0x5b, 0x41, 0x5c, 0x41, 0x5d, 0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xe9,
};

/* Character spawn: copies 42-byte status block from [r12] to [rbx+0xa4], loads level [r12+0x26], calls ApplyLevelStats. */
static const uint8_t kSpawnLoad[] = {
    0xc4, 0xc1, 0x7c, 0x10, 0x04, 0x24, 0xc4, 0xc1, 0x7c, 0x10, 0x4c, 0x24, 0x0a,
    0xc5, 0xfc, 0x11, 0x8b, 0xae, 0x00, 0x00, 0x00, 0xc5, 0xfc, 0x11, 0x83, 0xa4, 0x00, 0x00, 0x00,
    0x41, 0x0f, 0xb6, 0x74, 0x24, 0x26, 0xe8,
};

/* Status viewer refresh: mov rbx,rdi; mov r15,rsi; call max HP with equipment; vcvttss2si; mov rbx,rdi; mov r15,rsi;
 * mov eax,[r14+0x6c30] (the HP the screen prints). */
static const uint8_t kStatusMaxHead[] = {0x48, 0x89, 0xdf, 0x4c, 0x89, 0xfe, 0xe8};
static const uint8_t kStatusMaxTail[] = {0xc5, 0xfa, 0x2c, 0xc0, 0x48, 0x89, 0xdf, 0x4c, 0x89, 0xfe, 0x41, 0x89, 0x86};
enum { kStatusMaxCall = 6, kStatusMaxStore = 21, kStatusMaxLength = 7 };

/* Max HP with equipment reads the block's max HP as an int; proves the call target, not just the shape. */
static const uint8_t kStatusMaxCompute[] = {0xc5, 0xfa, 0x2a, 0x47, 0x10};
/* The same refresh loads the viewer's character record from +0x5150. */
static const uint8_t kStatusRecordLoad[] = {0x49, 0x8b, 0x9e, 0x50, 0x51, 0x00, 0x00};
static const uint8_t kFunctionPrologue[] = {0x55, 0x48, 0x89, 0xe5};

/* The status viewer's six stat stores end with LUC: vcvttss2si eax,xmm0; mov rdi,r15; mov [r14+0x6c48],eax. */
static const uint8_t kStatusStatsTail[] = {0xc5, 0xfa, 0x2c, 0xc0, 0x4c, 0x89, 0xff,
                                           0x41, 0x89, 0x86, 0x48, 0x6c, 0x00, 0x00};
enum { kStatusStatsTailStore = 7 };
static const uint8_t kStatusStrengthStore[] = {0x41, 0x89, 0x86, 0x34, 0x6c, 0x00, 0x00};
static const uint8_t kStatusConstitutionStore[] = {0x41, 0x89, 0x86, 0x3c, 0x6c, 0x00, 0x00};
static const uint8_t kStatusIntelligenceStore[] = {0x41, 0x89, 0x86, 0x38, 0x6c, 0x00, 0x00};
static const uint8_t kStatusMagicResistanceStore[] = {0x41, 0x89, 0x86, 0x40, 0x6c, 0x00, 0x00};
static const uint8_t kStatusDexterityStore[] = {0x41, 0x89, 0x86, 0x44, 0x6c, 0x00, 0x00};

/* The unit's effective-stat bundle builder ends by storing LUC and returning. */
static const uint8_t kBundleStatsTail[] = {0xc5, 0xfa, 0x11, 0x83, 0x20, 0x01, 0x00, 0x00, 0x48, 0x83,
                                           0xc4, 0x08, 0x5b, 0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xc3};
enum { kBundleStatsHookOffset = 8 };
/* The effect dispatcher with the same bundle, after its last LUC store. */
static const uint8_t kEffectStatsStore[] = {0xc4, 0xc1, 0x7a, 0x11, 0x86, 0x20, 0x01, 0x00, 0x00};
static const uint8_t kEffectStatsPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                               0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x10};
static const uint8_t kEffectStatsTail[] = {0x48, 0x83, 0xc4, 0x10, 0x5b, 0x41, 0x5c,
                                           0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xc3};

static int contains(const uint8_t *image, uint64_t size, uint64_t at, uint64_t window, const uint8_t *pat,
                    size_t len) {
    if (at > size || window > size - at || window < len) return 0;
    for (uint64_t i = 0; i + len <= window; ++i) {
        if (!memcmp(image + at + i, pat, len)) return 1;
    }
    return 0;
}

static int find_unique(const uint8_t *image, uint64_t size, const uint8_t *pat, size_t len, uint64_t *out) {
    int count = 0;
    uint64_t found = 0;
    if (size < len) return 0;
    for (uint64_t at = 0; at + len <= size; ++at) {
        if (image[at] == pat[0] && !memcmp(image + at, pat, len)) {
            found = at;
            if (++count > 1) return 0;
        }
    }
    if (count != 1) return 0;
    *out = found;
    return 1;
}

static int find_next(const uint8_t *image, uint64_t size, uint64_t from, uint64_t to, const uint8_t *pat,
                     size_t len, uint64_t *out) {
    if (to > size) to = size;
    if (from > to || to - from < len) return 0;
    for (uint64_t at = from; at + len <= to; ++at) {
        if (image[at] == pat[0] && !memcmp(image + at, pat, len)) {
            *out = at;
            return 1;
        }
    }
    return 0;
}

static int64_t call_target(const uint8_t *image, uint64_t at) {
    int32_t rel = 0;
    memcpy(&rel, image + at + 1, sizeof(rel));
    return (int64_t)at + 5 + rel;
}

static int hp_locator_find_status_max(const uint8_t *image, uint64_t size, uint64_t *out) {
    int count = 0;
    uint64_t found = 0;
    for (uint64_t at = 0; at + kStatusMaxStore + kStatusMaxLength <= size; ++at) {
        if (memcmp(image + at, kStatusMaxHead, sizeof(kStatusMaxHead)) ||
            memcmp(image + at + 11, kStatusMaxTail, sizeof(kStatusMaxTail)))
            continue;
        const int64_t target = call_target(image, at + kStatusMaxCall);
        if (target < 0 || (uint64_t)target + sizeof(kFunctionPrologue) > size) continue;
        if (memcmp(image + target, kFunctionPrologue, sizeof(kFunctionPrologue))) continue;
        if (!contains(image, size, (uint64_t)target, 0x40, kStatusMaxCompute, sizeof(kStatusMaxCompute))) continue;
        if (at < 0x400 || !contains(image, size, at - 0x400, 0x400, kStatusRecordLoad, sizeof(kStatusRecordLoad)))
            continue;
        found = at + kStatusMaxStore;
        if (++count > 1) return 0;
    }
    if (count != 1) return 0;
    *out = found;
    return 1;
}

static int hp_locator_find_status_stats(const uint8_t *image, uint64_t size, uint64_t status_max, uint64_t *out) {
    uint64_t tail = 0;
    if (!find_unique(image, size, kStatusStatsTail, sizeof(kStatusStatsTail), &tail)) return 0;
    const uint64_t store = tail + kStatusStatsTailStore;
    if (store <= status_max || store - status_max >= 0x200) return 0;
    const uint8_t *const stores[] = {kStatusStrengthStore, kStatusIntelligenceStore, kStatusConstitutionStore,
                                     kStatusMagicResistanceStore, kStatusDexterityStore};
    uint64_t previous = status_max;
    for (size_t i = 0; i < sizeof(stores) / sizeof(stores[0]); ++i) {
        uint64_t at = 0;
        if (!find_unique(image, size, stores[i], kStatusStoreLength, &at)) return 0;
        if (at <= previous || at >= store) return 0;
        previous = at;
    }
    *out = store;
    return 1;
}

static int locate_effective_stats(const uint8_t *image, uint64_t size, uint64_t *bundle, uint64_t *effect) {
    uint64_t bundle_tail = 0;
    if (!find_unique(image, size, kBundleStatsTail, sizeof(kBundleStatsTail), &bundle_tail)) return 0;
    uint64_t effect_store = 0, effect_tail = 0;
    if (!find_unique(image, size, kEffectStatsStore, sizeof(kEffectStatsStore), &effect_store)) return 0;
    if (effect_store < 0x400 ||
        !contains(image, size, effect_store - 0x400, 0x400, kEffectStatsPrologue, sizeof(kEffectStatsPrologue)))
        return 0;
    if (!find_next(image, size, effect_store + sizeof(kEffectStatsStore), effect_store + 0x100, kEffectStatsTail,
                   sizeof(kEffectStatsTail), &effect_tail))
        return 0;
    *bundle = bundle_tail + kBundleStatsHookOffset;
    *effect = effect_tail;
    return 1;
}

int character_override_hooks_locate(const uint8_t *image, uint64_t size, CharacterOverrideSites *sites) {
    memset(sites, 0, sizeof(*sites));
    uint64_t entry = 0, tail = 0, spawn = 0, status = 0, stats = 0, bundle = 0, effect = 0;
    if (!image || !find_unique(image, size, kRecalcEntry, sizeof(kRecalcEntry), &entry) ||
        !find_unique(image, size, kRecalcTail, sizeof(kRecalcTail), &tail) ||
        !find_unique(image, size, kSpawnLoad, sizeof(kSpawnLoad), &spawn) ||
        !hp_locator_find_status_max(image, size, &status) || !hp_locator_find_status_stats(image, size, status, &stats) ||
        !locate_effective_stats(image, size, &bundle, &effect)) {
        return 0;
    }
    if (tail <= entry || tail - entry >= 0x300 || spawn + kSpawnCallOffset + 5 > size) return 0;
    if (call_target(image, spawn + kSpawnCallOffset) != (int64_t)entry) return 0;
    sites->recalc_entry = entry;
    sites->recalc_post = tail + kPostHookOffsetInTail;
    sites->spawn_load = spawn;
    sites->status_max = status;
    sites->status_stats = stats;
    sites->bundle_stats = bundle;
    sites->effect_stats = effect;
    return 1;
}

static const CharacterOverrideEntry *entries;
static size_t entry_count;
static CharacterOverrideRecordOfUnit record_of_unit;
static _Atomic uint64_t party;
static _Atomic int logged_max[SEAT_COUNT];
static _Thread_local uint64_t recalc_unit;
static _Thread_local int32_t recalc_pre_hp;

static uint64_t guest_u64(uint64_t address) {
    uint64_t value;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof(value));
    return value;
}

static int32_t guest_i32(uint64_t address) {
    int32_t value;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof(value));
    return value;
}

static void guest_store_i32(uint64_t address, int32_t value) {
    memcpy((void *)(uintptr_t)address, &value, sizeof(value));
}

static void guest_store_u16(uint64_t address, uint16_t value) {
    memcpy((void *)(uintptr_t)address, &value, sizeof(value));
}

static void guest_store_f32(uint64_t address, float value) {
    memcpy((void *)(uintptr_t)address, &value, sizeof(value));
}

static uint8_t guest_u8(uint64_t address) {
    return *(const uint8_t *)(uintptr_t)address;
}

static uint64_t seat_record(uint64_t unit) {
    typedef uint64_t (GUEST_HOOK_ABI *GameOf)(uint64_t unit);
    const int32_t slot = guest_i32(unit + UNIT_PLAYER_SLOT);
    if (slot < 0 || slot >= SEAT_COUNT) return 0;
    uint64_t seats = atomic_load(&party);
    if (!seats) {
        const GameOf game_of = (GameOf)(uintptr_t)guest_u64(guest_u64(unit) + UNIT_CONTEXT_VIRTUAL);
        const uint64_t game = game_of ? game_of(unit) : 0;
        seats = game ? guest_u64(game + GAME_PARTY) : 0;
        if (!seats) return 0;
        atomic_store(&party, seats);
    }
    return seats + PARTY_SEATS + (uint64_t)slot * SEAT_SIZE;
}

void character_override_hooks_set_entries(const CharacterOverrideEntry *list, size_t count) {
    entries = list;
    entry_count = list ? count : 0;
    for (int i = 0; i < SEAT_COUNT; ++i) atomic_store(&logged_max[i], 0);
}

void character_override_hooks_set_record_of_unit(CharacterOverrideRecordOfUnit function) {
    record_of_unit = function;
    atomic_store(&party, 0);
}

const CharacterOverrideEntry *character_override_of_status_record(uint64_t record) {
    if (!record || !entry_count) return NULL;
    const uint8_t klass = guest_u8(record + APPEARANCE_RECORD_CLASS);
    const uint8_t *identity = (const uint8_t *)(uintptr_t)(record + APPEARANCE_RECORD_IDENTITY);
    for (size_t i = 0; i < entry_count; ++i) {
        const CharacterOverrideEntry *entry = &entries[i];
        if (entry->present && entry->klass == klass && appearance_same_identity(entry->identity, identity))
            return entry;
    }
    return NULL;
}

const CharacterOverrideEntry *character_override_of_record(uint64_t record) {
    if (!record || !entry_count) return NULL;
    if (guest_i32(record + APPEARANCE_SEAT_OWNER) != APPEARANCE_SEAT_LOCAL) return NULL;
    return character_override_of_status_record(record);
}

void GUEST_HOOK_ABI character_override_pre_recalc(GuestRegs *regs) {
    const uint64_t unit = regs->rdi;
    if (!unit || !entry_count) {
        recalc_unit = 0;
        return;
    }
    recalc_unit = unit;
    recalc_pre_hp = guest_i32(unit + CHARACTER_OVERRIDE_UNIT_CURRENT);
}

static void apply_stats(uint64_t block, const CharacterOverrideEntry *override) {
    if (override->present & CHARACTER_OVERRIDE_STRENGTH)
        guest_store_u16(block + CHARACTER_OVERRIDE_UNIT_STRENGTH, override->strength);
    if (override->present & CHARACTER_OVERRIDE_INTELLIGENCE)
        guest_store_u16(block + CHARACTER_OVERRIDE_UNIT_INTELLIGENCE, override->intelligence);
    if (override->present & CHARACTER_OVERRIDE_DEXTERITY)
        guest_store_u16(block + CHARACTER_OVERRIDE_UNIT_DEXTERITY, override->dexterity);
    if (override->present & CHARACTER_OVERRIDE_LUCK)
        guest_store_u16(block + CHARACTER_OVERRIDE_UNIT_LUCK, override->luck);
    if (override->present & CHARACTER_OVERRIDE_CONSTITUTION)
        guest_store_u16(block + CHARACTER_OVERRIDE_UNIT_CONSTITUTION, override->constitution);
    if (override->present & CHARACTER_OVERRIDE_MAGIC_RESISTANCE)
        guest_store_u16(block + CHARACTER_OVERRIDE_UNIT_MAGIC_RESISTANCE, override->magic_resistance);
}

/* The damage and critical paths read the unit's effective-stat floats, after equipment and effects were added. */
static void write_effective_stats(uint64_t unit, const CharacterOverrideEntry *override) {
    if (override->present & CHARACTER_OVERRIDE_STRENGTH)
        guest_store_f32(unit + CHARACTER_OVERRIDE_UNIT_EFFECTIVE_STRENGTH, (float)override->strength);
    if (override->present & CHARACTER_OVERRIDE_INTELLIGENCE)
        guest_store_f32(unit + CHARACTER_OVERRIDE_UNIT_EFFECTIVE_INTELLIGENCE, (float)override->intelligence);
    if (override->present & CHARACTER_OVERRIDE_DEXTERITY)
        guest_store_f32(unit + CHARACTER_OVERRIDE_UNIT_EFFECTIVE_DEXTERITY, (float)override->dexterity);
    if (override->present & CHARACTER_OVERRIDE_CONSTITUTION)
        guest_store_f32(unit + CHARACTER_OVERRIDE_UNIT_EFFECTIVE_CONSTITUTION, (float)override->constitution);
    if (override->present & CHARACTER_OVERRIDE_MAGIC_RESISTANCE)
        guest_store_f32(unit + CHARACTER_OVERRIDE_UNIT_EFFECTIVE_MAGIC_RESISTANCE, (float)override->magic_resistance);
    if (override->present & CHARACTER_OVERRIDE_LUCK)
        guest_store_f32(unit + CHARACTER_OVERRIDE_UNIT_EFFECTIVE_LUCK, (float)override->luck);
}

static void apply_effective_override(uint64_t unit) {
    if (!unit || !entry_count) return;
    const uint64_t record = record_of_unit ? record_of_unit(unit) : seat_record(unit);
    const CharacterOverrideEntry *override = character_override_of_record(record);
    if (override) write_effective_stats(unit, override);
}

void GUEST_HOOK_ABI character_override_post_recalc(GuestRegs *regs) {
    const uint64_t unit = regs->r12;
    const int have_pre = (recalc_unit == unit);
    const int32_t pre_hp = recalc_pre_hp;
    recalc_unit = 0;
    if (!unit || !entry_count) return;
    const uint64_t record = record_of_unit ? record_of_unit(unit) : seat_record(unit);
    const CharacterOverrideEntry *override = character_override_of_record(record);
    if (!override) return;
    if (override->present & CHARACTER_OVERRIDE_MAX_HP) {
        const int32_t target_max = (int32_t)override->max_hp;
        int32_t target_cur = have_pre ? pre_hp : guest_i32(unit + CHARACTER_OVERRIDE_UNIT_CURRENT);
        if (target_cur < 0) target_cur = 0;
        if (target_cur > target_max) target_cur = target_max;
        guest_store_i32(unit + CHARACTER_OVERRIDE_UNIT_MAX, target_max);
        guest_store_i32(unit + CHARACTER_OVERRIDE_UNIT_CURRENT, target_cur);
        const int32_t slot = guest_i32(unit + UNIT_PLAYER_SLOT);
        if (slot >= 0 && slot < SEAT_COUNT && atomic_exchange(&logged_max[slot], target_max) != target_max) {
            printf("Character override: player=%d max_hp=%d\n", slot + 1, target_max);
        }
    }
    apply_stats(unit, override);
    write_effective_stats(unit, override);
}

size_t character_override_hooks_stub_bytes(void) {
    return CHARACTER_OVERRIDE_HOOK_COUNT * guest_hook_stub_size();
}

void GUEST_HOOK_ABI character_override_status_max(GuestRegs *regs) {
    if (!entry_count) return;
    const uint64_t viewer = regs->r14;
    const uint64_t record = viewer ? guest_u64(viewer + CHARACTER_OVERRIDE_STATUS_RECORD) : 0;
    const CharacterOverrideEntry *override = character_override_of_status_record(record);
    if (override && (override->present & CHARACTER_OVERRIDE_MAX_HP)) regs->rax = override->max_hp;
}

void GUEST_HOOK_ABI character_override_status_stats(GuestRegs *regs) {
    if (!entry_count) return;
    const uint64_t viewer = regs->r14;
    const uint64_t record = viewer ? guest_u64(viewer + CHARACTER_OVERRIDE_STATUS_RECORD) : 0;
    const CharacterOverrideEntry *override = character_override_of_status_record(record);
    if (!override) return;
    if (override->present & CHARACTER_OVERRIDE_STRENGTH)
        guest_store_i32(viewer + CHARACTER_OVERRIDE_STATUS_STRENGTH, override->strength);
    if (override->present & CHARACTER_OVERRIDE_INTELLIGENCE)
        guest_store_i32(viewer + CHARACTER_OVERRIDE_STATUS_INTELLIGENCE, override->intelligence);
    if (override->present & CHARACTER_OVERRIDE_CONSTITUTION)
        guest_store_i32(viewer + CHARACTER_OVERRIDE_STATUS_CONSTITUTION, override->constitution);
    if (override->present & CHARACTER_OVERRIDE_MAGIC_RESISTANCE)
        guest_store_i32(viewer + CHARACTER_OVERRIDE_STATUS_MAGIC_RESISTANCE, override->magic_resistance);
    if (override->present & CHARACTER_OVERRIDE_DEXTERITY)
        guest_store_i32(viewer + CHARACTER_OVERRIDE_STATUS_DEXTERITY, override->dexterity);
    if (override->present & CHARACTER_OVERRIDE_LUCK) {
        guest_store_i32(viewer + CHARACTER_OVERRIDE_STATUS_LUCK, override->luck);
        regs->rax = override->luck;
    }
}

void GUEST_HOOK_ABI character_override_bundle_stats(GuestRegs *regs) {
    apply_effective_override(regs->rbx);
}

void GUEST_HOOK_ABI character_override_effect_stats(GuestRegs *regs) {
    apply_effective_override(regs->r14);
}

int character_override_hooks_install(uint8_t *image, uint64_t size, uint64_t entry, const CharacterOverrideSites *sites,
                                     uint8_t *stubs, size_t stubs_size) {
    if (!sites || !sites->recalc_entry || !sites->recalc_post || !sites->status_max || !sites->status_stats ||
        !sites->bundle_stats || !sites->effect_stats)
        return 0;
    GuestHook hooks[CHARACTER_OVERRIDE_HOOK_COUNT] = {
        {"character recalc pre", (uint32_t)sites->recalc_entry, kEntryHookLength, {0}, character_override_pre_recalc},
        {"character recalc post", (uint32_t)sites->recalc_post, kPostHookLength, {0}, character_override_post_recalc},
        {"character status max", (uint32_t)sites->status_max, kStatusMaxLength, {0}, character_override_status_max},
        {"character status stats", (uint32_t)sites->status_stats, kStatusStoreLength, {0},
         character_override_status_stats},
        {"character bundle stats", (uint32_t)sites->bundle_stats, 5, {0}, character_override_bundle_stats},
        {"character effect stats", (uint32_t)sites->effect_stats, 5, {0}, character_override_effect_stats},
    };
    for (size_t i = 0; i < CHARACTER_OVERRIDE_HOOK_COUNT; ++i) {
        if (!hooks[i].offset || hooks[i].offset + (uint64_t)hooks[i].length > size) return 0;
        memcpy(hooks[i].original, image + hooks[i].offset, hooks[i].length);
    }
    size_t bad = 0;
    const GuestHookStatus status =
        guest_hooks_install(image, size, entry, NULL, hooks, CHARACTER_OVERRIDE_HOOK_COUNT, stubs, stubs_size, &bad);
    if (status != GUEST_HOOK_OK) {
        printf("Character override: hook %s not installed: %s\n", hooks[bad].name, guest_hook_status_text(status));
        return 0;
    }
    return 1;
}
