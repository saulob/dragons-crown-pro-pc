#include "appearance_hooks.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define ANY (-1)

enum {
    UNIT_CLASS = 0xcb,
    UNIT_COLOR = 0xcc,
    UNIT_PLAYER_SLOT = 0xa70,
    UNIT_CONTEXT_VIRTUAL = 0xd0,
    GAME_PARTY = 0x80,
    PARTY_SEATS = 0xa8,
    SEAT_SIZE = 0x23b0,
    SEAT_COUNT = 4,
    OWNER_UNIT = 0x9b8,
    OWNER_CLASS = 0x39e0,
    EFFECT_CLASS = 0x38e8,
    EFFECT_COLOR = 0x38ea
};

/* The class and preset from the unit, then a tail jump to the atlas name function. */
static const short kUnitGetter[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb, 0xe8, ANY,  ANY,  ANY,  ANY,  0x44, 0x0f,
    0xbe, 0xb0, 0xcb, 0x00, 0x00, 0x00, 0x48, 0x89, 0xdf, 0xe8, ANY,  ANY,  ANY,  ANY,  0x0f, 0xb6, 0xb0,
    0xcc, 0x00, 0x00, 0x00, 0x44, 0x89, 0xf7, 0x5b, 0x41, 0x5e, 0x5d, 0xe9, ANY,  ANY,  ANY,  ANY,
};
enum { kUnitGetterFirstCall = 10, kUnitGetterSecondCall = 26, kUnitGetterJump = 45 };

/* The Edit character preview: class and preset from the record in rax, then the atlas name call. */
static const short kSelectPreview[] = {
    0x49, 0x8b, 0x86, 0xa8, 0x00, 0x00, 0x00, 0x45, 0x31, 0xe4, 0x48, 0x85, 0xc0, 0x74, ANY, 0x0f,
    0xbe, 0x78, 0x27, 0x0f, 0xb6, 0x70, 0x28, 0xe8, ANY,  ANY,  ANY,  ANY,  0x49, 0x89, 0xc4,
};
enum { kSelectCall = 23 };

/* An effect takes its owner's look: class from the owner, preset from its unit, into three descriptors. */
static const short kEffectCopy[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x49, 0x89, 0xf6, 0x48, 0x89, 0xfb, 0x4d, 0x85, 0xf6, 0x74, ANY,
    0x4c, 0x89, 0xf7, 0xe8, ANY,  ANY,  ANY,  ANY,  0x48, 0x85, 0xc0, 0x74, ANY,  0x41, 0x0f, 0xb7, 0x8e, 0xe0,
    0x39, 0x00, 0x00, 0x0f, 0xb6, 0x80, 0xcc, 0x00, 0x00, 0x00, 0xc6, 0x83, 0xe0, 0x38, 0x00, 0x00, 0x02,
};
enum { kEffectCopyCall = 21, kEffectCopyLength = 7 };

/* The atlas descriptor set from an explicit class and preset. */
static const short kEffectSet[] = {
    0xc6, 0x87, 0xe6, 0x38, 0x00, 0x00, 0x02, 0x66, 0x89, 0xb7, 0xe8, 0x38,
    0x00, 0x00, 0x66, 0x89, 0x97, 0xea, 0x38, 0x00, 0x00, 0xc3,
};
enum { kEffectSetLength = 7 };

/* The atlas descriptor's name: class and preset words, then a tail jump to the atlas name function. */
static const short kEffectAtlas[] = {0x8b, 0xb7, 0xe8, 0x38, 0x00, 0x00, 0x0f, 0xbf, 0xfe, 0xc1, 0xfe, 0x10, 0xe9, ANY, ANY, ANY, ANY};
enum { kEffectAtlasJump = 12, kEffectAtlasLength = 9 };

/* A weapon or shield texture name: the unit's equipped item, its class, then a tail jump to the name function. */
static const short kWeaponGetter[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb, 0xe8, ANY,  ANY,  ANY,  ANY,  0x48, 0x85, 0xc0, 0x74, ANY,
    0x48, 0x05, 0xe8, 0x01, 0x00, 0x00, 0x48, 0x89, 0xc7, 0xe8, ANY,  ANY,  ANY,  ANY,  0x48, 0x85, 0xc0, 0x74, ANY,  0x44,
    0x8b, 0x70, 0x40, 0x48, 0x89, 0xdf, 0x41, 0x81, 0xe6, 0xff, 0xff, 0xff, 0x01, 0xe8, ANY,  ANY,  ANY,  ANY,  0x0f, 0xbe,
    0xb8, 0xcb, 0x00, 0x00, 0x00, 0x44, 0x89, 0xf6, 0x5b, 0x41, 0x5e, 0x5d, 0xe9, ANY,  ANY,  ANY,  ANY,
};
enum { kWeaponGetterFirstCall = 10, kWeaponGetterSecondCall = 53, kWeaponGetterJump = 72 };

/* The Edit character preview's weapon and shield: item and class from the record, then the name call. */
static const short kWeaponSelect[] = {0x49, 0x8b, 0x8e, 0xa8, 0x00, 0x00, 0x00, 0xbe, 0xff, 0xff, 0xff, 0x01,
                                      0x23, 0x70, 0x40, 0x0f, 0xbe, 0x79, 0x27, 0xe8, ANY,  ANY,  ANY,  ANY};
enum { kWeaponSelectCall = 19 };

/* The default texture each name function falls back to, by the class 0 name it loads. */
static const char *const kWeaponDefault[APPEARANCE_WEAPON_COUNT] = {"PLAYER/FIGHTER00_ARMS01.FTX", "PLAYER/FIGHTER00_SHIELD01.FTX"};

static const short kAtlasName[] = {0x31, 0xc0, 0x89, 0xf9, 0x83, 0xff, 0x05, 0x77, ANY, 0x48, 0x8d, 0x05};
static const short kUnitOf[] = {0x48, 0x8b, 0x87, 0xb8, 0x09, 0x00, 0x00, 0xc3};

#define COUNT(a) (sizeof(a) / sizeof(*(a)))

static int shape_at(const uint8_t *image, uint64_t size, uint64_t at, const short *shape, size_t count) {
    if (at > size || size - at < count) return 0;
    for (size_t i = 0; i < count; ++i)
        if (shape[i] != ANY && image[at + i] != (uint8_t)shape[i]) return 0;
    return 1;
}

static int64_t branch_target(const uint8_t *image, uint64_t at) {
    int32_t rel;
    memcpy(&rel, image + at + 1, sizeof(rel));
    return (int64_t)at + 5 + rel;
}

static int reaches(const uint8_t *image, uint64_t size, uint64_t at, const short *shape, size_t count, int64_t *target) {
    *target = branch_target(image, at);
    return *target >= 0 && shape_at(image, size, (uint64_t)*target, shape, count);
}

/* Which weapon name function starts at `function`: the one that loads that default texture name, or -1. */
static int weapon_of_function(const uint8_t *image, uint64_t size, int64_t function) {
    if (function < 0 || (uint64_t)function + 0x140 > size) return -1;
    for (uint64_t at = (uint64_t)function; at < (uint64_t)function + 0x140 - 7; ++at) {
        if ((image[at] != 0x48 && image[at] != 0x4c) || image[at + 1] != 0x8d || (image[at + 2] & 0xc7) != 0x05) continue;
        int32_t rel;
        memcpy(&rel, image + at + 3, sizeof(rel));
        const int64_t text = (int64_t)at + 7 + rel;
        for (int w = 0; w < APPEARANCE_WEAPON_COUNT; ++w) {
            const size_t length = strlen(kWeaponDefault[w]) + 1;
            if (text >= 0 && (uint64_t)text + length <= size && !memcmp(image + text, kWeaponDefault[w], length)) return w;
        }
    }
    return -1;
}

/* A weapon site: which name function it reaches, consistent with the ones found before. */
static int weapon_site(const uint8_t *image, uint64_t size, uint64_t call, AppearanceSites *sites) {
    const int64_t target = branch_target(image, call);
    const int weapon = weapon_of_function(image, size, target);
    if (weapon < 0) return -1;
    if (sites->weapon_name[weapon] && sites->weapon_name[weapon] != (uint64_t)target) return -1;
    sites->weapon_name[weapon] = (uint64_t)target;
    return weapon;
}

int appearance_hooks_locate(const uint8_t *image, uint64_t size, AppearanceSites *sites) {
    memset(sites, 0, sizeof(*sites));
    int units = 0, selects = 0, copies = 0, sets = 0, atlases = 0;
    int weapon_units[APPEARANCE_WEAPON_COUNT] = {0}, weapon_selects[APPEARANCE_WEAPON_COUNT] = {0};
    int64_t atlas = -1, target;
    for (uint64_t at = 0; at + 64 <= size; ++at) {
        switch (image[at]) {
        case 0x55:
            if (shape_at(image, size, at, kUnitGetter, COUNT(kUnitGetter))) {
                if (!reaches(image, size, at + kUnitGetterFirstCall, kUnitOf, COUNT(kUnitOf), &target) ||
                    !reaches(image, size, at + kUnitGetterSecondCall, kUnitOf, COUNT(kUnitOf), &target) ||
                    !reaches(image, size, at + kUnitGetterJump, kAtlasName, COUNT(kAtlasName), &target) ||
                    (atlas >= 0 && target != atlas))
                    return 0;
                atlas = target;
                if (units < 2) sites->unit_atlas[units] = at + kUnitGetterJump;
                ++units;
            } else if (shape_at(image, size, at, kEffectCopy, COUNT(kEffectCopy))) {
                if (!reaches(image, size, at + kEffectCopyCall, kUnitOf, COUNT(kUnitOf), &target)) return 0;
                sites->effect_copy = at;
                ++copies;
            } else if (shape_at(image, size, at, kWeaponGetter, COUNT(kWeaponGetter))) {
                if (!reaches(image, size, at + kWeaponGetterFirstCall, kUnitOf, COUNT(kUnitOf), &target) ||
                    !reaches(image, size, at + kWeaponGetterSecondCall, kUnitOf, COUNT(kUnitOf), &target))
                    return 0;
                const int weapon = weapon_site(image, size, at + kWeaponGetterJump, sites);
                if (weapon < 0) return 0;
                if (weapon_units[weapon] < 2) sites->unit_weapon[weapon][weapon_units[weapon]] = at + kWeaponGetterJump;
                ++weapon_units[weapon];
            }
            break;
        case 0x49:
            if (shape_at(image, size, at, kSelectPreview, COUNT(kSelectPreview))) {
                if (!reaches(image, size, at + kSelectCall, kAtlasName, COUNT(kAtlasName), &target) ||
                    (atlas >= 0 && target != atlas))
                    return 0;
                atlas = target;
                sites->select_atlas = at + kSelectCall;
                ++selects;
            } else if (shape_at(image, size, at, kWeaponSelect, COUNT(kWeaponSelect))) {
                const int weapon = weapon_site(image, size, at + kWeaponSelectCall, sites);
                if (weapon < 0) return 0;
                sites->select_weapon[weapon] = at + kWeaponSelectCall;
                ++weapon_selects[weapon];
            }
            break;
        case 0xc6:
            if (shape_at(image, size, at, kEffectSet, COUNT(kEffectSet))) {
                sites->effect_set = at;
                ++sets;
            }
            break;
        case 0x8b:
            if (shape_at(image, size, at, kEffectAtlas, COUNT(kEffectAtlas))) {
                if (!reaches(image, size, at + kEffectAtlasJump, kAtlasName, COUNT(kAtlasName), &target) ||
                    (atlas >= 0 && target != atlas))
                    return 0;
                atlas = target;
                sites->effect_atlas = at;
                ++atlases;
            }
            break;
        }
    }
    int weapons_found = 1;
    for (int w = 0; w < APPEARANCE_WEAPON_COUNT; ++w)
        weapons_found &= weapon_units[w] == 2 && weapon_selects[w] == 1 && sites->weapon_name[w] != 0;
    if (units == 2 && selects == 1 && copies == 1 && sets == 1 && atlases == 1 && weapons_found) return 1;
    memset(sites, 0, sizeof(*sites));
    return 0;
}

static const AppearanceInstance *instances;
static size_t instance_count;
static AppearanceRecordOfUnit record_of_unit;
static AppearanceWeaponName weapon_names[APPEARANCE_WEAPON_COUNT];
static _Atomic uint64_t party;
static AppearanceRemoteOf remote_of;
static AppearanceRemoteSerial remote_serial;
static AppearanceLocalSeen local_seen;
enum { kRemoteNumber = 0x8000 };

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

static int16_t guest_i16(uint64_t address) {
    int16_t value;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof(value));
    return value;
}

static uint8_t guest_u8(uint64_t address) {
    return *(const uint8_t *)(uintptr_t)address;
}

/* The party is made once at game start; its seats hold the records the players play. */
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

void appearance_hooks_set_instances(const AppearanceInstance *list, size_t count) {
    instances = list;
    instance_count = list ? count : 0;
}

void appearance_hooks_set_record_of_unit(AppearanceRecordOfUnit function) {
    record_of_unit = function;
    atomic_store(&party, 0);
}

void appearance_hooks_set_online(AppearanceRemoteOf of, AppearanceRemoteSerial serial, AppearanceLocalSeen seen) {
    remote_of = of;
    remote_serial = serial;
    local_seen = seen;
}

static int hooks_active(void) {
    return instance_count || remote_of;
}

/* Local instances are 1..count, remote looks kRemoteNumber | serial. */
static uint64_t instance_number(const AppearanceInstance *instance) {
    if (!instance) return 0;
    if (instance->remote) return kRemoteNumber | instance->remote;
    return (uint64_t)(instance - instances) + 1;
}

static const AppearanceInstance *instance_of_number(uint64_t number) {
    if (number & kRemoteNumber) return remote_serial ? remote_serial((uint16_t)(number & (kRemoteNumber - 1))) : NULL;
    return number >= 1 && number <= instance_count ? &instances[number - 1] : NULL;
}

/* Online, another player's seat wears that player's look from the room, never one of ours (a copied save has our
 * identity); every other seat keeps the offline rule. */
static const AppearanceInstance *instance_of_unit(uint64_t unit, int klass, int color) {
    const uint64_t record = record_of_unit ? record_of_unit(unit) : seat_record(unit);
    if (!remote_of || !record) return appearance_instance_of_record(record, klass, color);
    const int32_t owner = guest_i32(record + APPEARANCE_SEAT_OWNER);
    const int32_t slot = guest_i32(unit + UNIT_PLAYER_SLOT);
    if (owner == APPEARANCE_SEAT_REMOTE) return remote_of(slot, klass, color);
    const AppearanceInstance *instance = appearance_instance_of_record(record, klass, color);
    if (owner == APPEARANCE_SEAT_LOCAL && local_seen) local_seen(slot, instance);
    return instance;
}

void appearance_hooks_set_weapon_names(AppearanceWeaponName weapon, AppearanceWeaponName shield) {
    weapon_names[0] = weapon;
    weapon_names[1] = shield;
}

int appearance_same_identity(const uint8_t *a, const uint8_t *b) {
    return !memcmp(a, b, 6) && !memcmp(a + 8, b + 8, APPEARANCE_IDENTITY_BYTES - 8);
}

const AppearanceInstance *appearance_instance_of_record(uint64_t record, int klass, int color) {
    if (!record || !instance_count) return NULL;
    if (guest_u8(record + APPEARANCE_RECORD_CLASS) != klass || guest_u8(record + APPEARANCE_RECORD_COLOR) != color) return NULL;
    const uint8_t *identity = (const uint8_t *)(uintptr_t)(record + APPEARANCE_RECORD_IDENTITY);
    for (size_t i = 0; i < instance_count; ++i) {
        const AppearanceInstance *instance = &instances[i];
        if (instance->klass == klass && instance->color == color && appearance_same_identity(instance->identity, identity))
            return instance;
    }
    return NULL;
}

/* DEBUG_SAULO: one line per drawn unit and look. */
static const char *class_name(int klass) {
    static const char *const names[] = {"Fighter", "Amazon", "Elf", "Dwarf", "Sorceress", "Wizard"};
    return klass >= 0 && klass < 6 ? names[klass] : "class?";
}

static void note_binding(const char *what, uint64_t holder, const AppearanceInstance *instance, int klass, int color) {
    enum { kSeen = 64 };
    static _Atomic uint64_t seen[kSeen];
    const uint64_t key = holder ^ instance_number(instance) << 44 ^ ((uint64_t)color << 60);
    for (int i = 0; i < kSeen; ++i) {
        uint64_t expected = 0;
        if (atomic_load(&seen[i]) == key) return;
        if (atomic_compare_exchange_strong(&seen[i], &expected, key)) break;
    }
    if (instance)
        printf("Appearance runtime: %s 0x%llx -> %s, bound custom texture %s\n", what, (unsigned long long)holder,
               instance->label, instance->entry);
    else
        printf("Appearance runtime: %s 0x%llx native texture (%s colour %d)\n", what, (unsigned long long)holder,
               class_name(klass), color);
}

static void answer(GuestRegs *regs, const AppearanceInstance *instance) {
    regs->rax = (uint64_t)(uintptr_t)instance->entry;
    regs->skip = 1;
}

void GUEST_HOOK_ABI appearance_unit_atlas(GuestRegs *regs) {
    const uint64_t unit = regs->rax;
    const int klass = (int)(int32_t)regs->rdi, color = (int)(uint32_t)regs->rsi;
    if (!unit || !hooks_active()) return;
    const AppearanceInstance *instance = instance_of_unit(unit, klass, color);
    note_binding("unit", unit, instance, klass, color);
    if (instance) answer(regs, instance);
}

void GUEST_HOOK_ABI appearance_select_atlas(GuestRegs *regs) {
    const uint64_t record = regs->rax;
    const int klass = (int)(int32_t)regs->rdi, color = (int)(uint32_t)regs->rsi;
    const AppearanceInstance *instance = appearance_instance_of_record(record, klass, color);
    if (instance_count) note_binding("record", record, instance, klass, color);
    if (instance) answer(regs, instance);
}

/* The custom weapon only where the game would draw the default one: the native name is asked first. */
static void answer_weapon(GuestRegs *regs, const AppearanceInstance *instance, int weapon) {
    if (!instance || !instance->weapons[weapon].entry[0] || !weapon_names[weapon]) return;
    const char *native = weapon_names[weapon]((int)(int32_t)regs->rdi, (uint32_t)regs->rsi);
    if (!native || strcmp(native, instance->weapons[weapon].native) != 0) return;
    note_binding(weapon ? "shield" : "weapon", (uint64_t)(uintptr_t)instance, instance, instance->klass, instance->color);
    regs->rax = (uint64_t)(uintptr_t)instance->weapons[weapon].entry;
    regs->skip = 1;
}

static void unit_weapon(GuestRegs *regs, int weapon) {
    const uint64_t unit = regs->rax;
    if (!unit || !hooks_active()) return;
    answer_weapon(regs, instance_of_unit(unit, (int)(int32_t)regs->rdi, guest_u8(unit + UNIT_COLOR)), weapon);
}

static void select_weapon(GuestRegs *regs, int weapon) {
    const uint64_t record = regs->rcx;
    if (!record || !instance_count) return;
    answer_weapon(regs, appearance_instance_of_record(record, (int)(int32_t)regs->rdi, guest_u8(record + APPEARANCE_RECORD_COLOR)),
                  weapon);
}

void GUEST_HOOK_ABI appearance_unit_weapon(GuestRegs *regs) {
    unit_weapon(regs, 0);
}

void GUEST_HOOK_ABI appearance_unit_shield(GuestRegs *regs) {
    unit_weapon(regs, 1);
}

void GUEST_HOOK_ABI appearance_select_weapon(GuestRegs *regs) {
    select_weapon(regs, 0);
}

void GUEST_HOOK_ABI appearance_select_shield(GuestRegs *regs) {
    select_weapon(regs, 1);
}

/* Effect objects live briefly and are reused: each look copy binds the object to its owner's instance, and an
 * explicit look unbinds it. Entries pack the object's address with the instance number above bit 48; a remote look
 * whose player changed or left is no longer found by its number. */
enum { kEffectSlots = 256, kEffectProbes = 8 };
static _Atomic uint64_t effect_bindings[kEffectSlots];
static const uint64_t kAddressMask = (1ull << 48) - 1;

static size_t effect_slot(uint64_t effect, int probe) {
    return (size_t)(((effect >> 4) ^ (effect >> 12)) + (uint64_t)probe) % kEffectSlots;
}

static void unbind_effect(uint64_t effect) {
    for (int p = 0; p < kEffectProbes; ++p) {
        _Atomic uint64_t *slot = &effect_bindings[effect_slot(effect, p)];
        if ((atomic_load(slot) & kAddressMask) == effect) atomic_store(slot, 0);
    }
}

static void bind_effect(uint64_t effect, const AppearanceInstance *instance) {
    unbind_effect(effect);
    const uint64_t packed = effect | instance_number(instance) << 48;
    for (int p = 0; p < kEffectProbes; ++p) {
        uint64_t expected = 0;
        if (atomic_compare_exchange_strong(&effect_bindings[effect_slot(effect, p)], &expected, packed)) return;
    }
    atomic_store(&effect_bindings[effect_slot(effect, 0)], packed);
}

static const AppearanceInstance *effect_instance(uint64_t effect) {
    for (int p = 0; p < kEffectProbes; ++p) {
        const uint64_t packed = atomic_load(&effect_bindings[effect_slot(effect, p)]);
        if (packed && (packed & kAddressMask) == effect) return instance_of_number(packed >> 48);
    }
    return NULL;
}

void GUEST_HOOK_ABI appearance_effect_copy(GuestRegs *regs) {
    const uint64_t effect = regs->rdi, owner = regs->rsi;
    if (!effect || effect > kAddressMask || !hooks_active()) return;
    const uint64_t unit = owner ? guest_u64(owner + OWNER_UNIT) : 0;
    const AppearanceInstance *instance = unit ? instance_of_unit(unit, guest_i16(owner + OWNER_CLASS), guest_u8(unit + UNIT_COLOR)) : NULL;
    if (instance) bind_effect(effect, instance);
    else unbind_effect(effect);
}

void GUEST_HOOK_ABI appearance_effect_set(GuestRegs *regs) {
    if (regs->rdi && regs->rdi <= kAddressMask) unbind_effect(regs->rdi);
}

void GUEST_HOOK_ABI appearance_effect_atlas(GuestRegs *regs) {
    const uint64_t effect = regs->rdi;
    if (!effect || effect > kAddressMask || !hooks_active()) return;
    const AppearanceInstance *instance = effect_instance(effect);
    if (!instance || instance->klass != guest_i16(effect + EFFECT_CLASS) || instance->color != guest_i16(effect + EFFECT_COLOR))
        return;
    note_binding("effect", effect, instance, instance->klass, instance->color);
    answer(regs, instance);
}

size_t appearance_hooks_stub_bytes(void) {
    return APPEARANCE_HOOK_COUNT * guest_hook_stub_size();
}

int appearance_hooks_install(uint8_t *image, uint64_t size, uint64_t entry, const AppearanceSites *sites, uint8_t *stubs,
                             size_t stubs_size) {
    GuestHook hooks[APPEARANCE_HOOK_COUNT] = {
        {"unit atlas", (uint32_t)sites->unit_atlas[0], 5, {0}, appearance_unit_atlas},
        {"unit atlas", (uint32_t)sites->unit_atlas[1], 5, {0}, appearance_unit_atlas},
        {"select atlas", (uint32_t)sites->select_atlas, 5, {0}, appearance_select_atlas},
        {"effect look copy", (uint32_t)sites->effect_copy, kEffectCopyLength, {0}, appearance_effect_copy},
        {"effect look set", (uint32_t)sites->effect_set, kEffectSetLength, {0}, appearance_effect_set},
        {"effect atlas", (uint32_t)sites->effect_atlas, kEffectAtlasLength, {0}, appearance_effect_atlas},
        {"unit weapon", (uint32_t)sites->unit_weapon[0][0], 5, {0}, appearance_unit_weapon},
        {"unit weapon", (uint32_t)sites->unit_weapon[0][1], 5, {0}, appearance_unit_weapon},
        {"unit shield", (uint32_t)sites->unit_weapon[1][0], 5, {0}, appearance_unit_shield},
        {"unit shield", (uint32_t)sites->unit_weapon[1][1], 5, {0}, appearance_unit_shield},
        {"select weapon", (uint32_t)sites->select_weapon[0], 5, {0}, appearance_select_weapon},
        {"select shield", (uint32_t)sites->select_weapon[1], 5, {0}, appearance_select_shield},
    };
    for (size_t i = 0; i < APPEARANCE_HOOK_COUNT; ++i) {
        if (!hooks[i].offset || hooks[i].offset + (uint64_t)hooks[i].length > size) return 0;
        memcpy(hooks[i].original, image + hooks[i].offset, hooks[i].length);
    }
    size_t bad = 0;
    const GuestHookStatus status =
        guest_hooks_install(image, size, entry, NULL, hooks, APPEARANCE_HOOK_COUNT, stubs, stubs_size, &bad);
    if (status != GUEST_HOOK_OK) {
        printf("Appearance runtime: hook %s not installed: %s\n", hooks[bad].name, guest_hook_status_text(status));
        return 0;
    }
    appearance_hooks_set_weapon_names((AppearanceWeaponName)(uintptr_t)(image + sites->weapon_name[0]),
                                      (AppearanceWeaponName)(uintptr_t)(image + sites->weapon_name[1]));
    return 1;
}
