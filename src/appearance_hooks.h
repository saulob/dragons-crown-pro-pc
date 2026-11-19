/* Custom character looks per character: the game names a player atlas by class and colour preset only, so every
 * character of one class and preset would share one texture. The hooked atlas-name sites answer with a per-character
 * archive entry instead, found from the game's own character identity (the record block it matches saved characters
 * by); any other character keeps the native name. */
#ifndef DCP_APPEARANCE_HOOKS_H
#define DCP_APPEARANCE_HOOKS_H

#include <stddef.h>
#include <stdint.h>

#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    APPEARANCE_IDENTITY_BYTES = 18,
    APPEARANCE_RECORD_IDENTITY = 0x104, /* in a runtime character record; the save file's record holds it at +0x1d4 */
    APPEARANCE_RECORD_CLASS = 0x27,
    APPEARANCE_RECORD_COLOR = 0x28,
    APPEARANCE_ENTRY_BYTES = 64,
    APPEARANCE_LABEL_BYTES = 80,
    APPEARANCE_WEAPON_NAME_BYTES = 48,
    APPEARANCE_WEAPON_COUNT = 2, /* the weapon, then the Fighter's shield */
    APPEARANCE_HOOK_COUNT = 12,
    APPEARANCE_RECIPE_BYTES = 84, /* appearance_net.h kMaxBytes */
    APPEARANCE_SEAT_OWNER = 0x21e0, /* in a party seat: -1 empty, 0 this client, 1 another online player */
    APPEARANCE_SEAT_LOCAL = 0,
    APPEARANCE_SEAT_REMOTE = 1
};

/* A default weapon texture replaced for one character; the equipped item's own texture is never replaced. */
typedef struct {
    char native[APPEARANCE_WEAPON_NAME_BYTES]; /* PLAYER/FIGHTER00_ARMS01.FTX */
    char entry[APPEARANCE_ENTRY_BYTES];        /* empty: native */
} AppearanceWeapon;

typedef struct {
    uint8_t identity[APPEARANCE_IDENTITY_BYTES];
    uint8_t klass, color;              /* drawn custom only while the character still has this class and preset */
    char entry[APPEARANCE_ENTRY_BYTES]; /* the per-character atlas in the archive */
    char label[APPEARANCE_LABEL_BYTES]; /* "DATA 1 slot 2 Amazon colour 0" */
    AppearanceWeapon weapons[APPEARANCE_WEAPON_COUNT];
    uint8_t recipe[APPEARANCE_RECIPE_BYTES]; /* the look as the other players of an online room get it */
    uint8_t recipe_size;
    uint16_t remote; /* 0: a saved character of this client; else the serial of another online player's look */
} AppearanceInstance;

/* The two in-game atlas getters' tail jumps, the Edit character preview's call, the effect look copy and explicit set,
 * the effect atlas name; then the same getters and preview call for the weapon and shield texture names, and the two
 * name functions they reach. */
typedef struct {
    uint64_t unit_atlas[2], select_atlas, effect_copy, effect_set, effect_atlas;
    uint64_t unit_weapon[APPEARANCE_WEAPON_COUNT][2], select_weapon[APPEARANCE_WEAPON_COUNT], weapon_name[APPEARANCE_WEAPON_COUNT];
} AppearanceSites;

/* 1 when every site is found exactly where its shape is unique and all name calls reach the same atlas function. */
int appearance_hooks_locate(const uint8_t *image, uint64_t size, AppearanceSites *sites);

/* Whether two identities name the same character, as the game compares them (bytes 6 and 7 are not part of it). */
int appearance_same_identity(const uint8_t *a, const uint8_t *b);

/* The instance for a runtime record drawn with `klass` and `color`, or NULL (native). */
const AppearanceInstance *appearance_instance_of_record(uint64_t record, int klass, int color);

/* Borrowed for the life of the process. */
void appearance_hooks_set_instances(const AppearanceInstance *instances, size_t count);

/* The record a unit plays: its party seat's. Replaced in the tests. */
typedef uint64_t (*AppearanceRecordOfUnit)(uint64_t unit);
void appearance_hooks_set_record_of_unit(AppearanceRecordOfUnit record_of_unit);

/* Online: a seat of another player is drawn with that player's look, by party slot, never with this client's saved
 * looks; other seats keep the offline rule. `seen` hears each look the game draws in this client's seats (NULL
 * instance: native). `remote_serial` gives a remote look back while it is still the current one. All NULL: offline. */
typedef const AppearanceInstance *(*AppearanceRemoteOf)(int slot, int klass, int color);
typedef const AppearanceInstance *(*AppearanceRemoteSerial)(uint16_t serial);
typedef void (*AppearanceLocalSeen)(int slot, const AppearanceInstance *instance);
void appearance_hooks_set_online(AppearanceRemoteOf remote_of, AppearanceRemoteSerial remote_serial, AppearanceLocalSeen seen);

/* The game's weapon and shield texture name functions (class, item); installing sets them, the tests replace them. */
typedef const char *(GUEST_HOOK_ABI *AppearanceWeaponName)(int klass, uint32_t item);
void appearance_hooks_set_weapon_names(AppearanceWeaponName weapon, AppearanceWeaponName shield);

size_t appearance_hooks_stub_bytes(void);

/* Installs all six hooks or none; returns 1 when installed. */
int appearance_hooks_install(uint8_t *image, uint64_t size, uint64_t entry, const AppearanceSites *sites, uint8_t *stubs,
                             size_t stubs_size);

void GUEST_HOOK_ABI appearance_unit_atlas(GuestRegs *regs);
void GUEST_HOOK_ABI appearance_select_atlas(GuestRegs *regs);
void GUEST_HOOK_ABI appearance_effect_copy(GuestRegs *regs);
void GUEST_HOOK_ABI appearance_effect_set(GuestRegs *regs);
void GUEST_HOOK_ABI appearance_effect_atlas(GuestRegs *regs);
void GUEST_HOOK_ABI appearance_unit_weapon(GuestRegs *regs);
void GUEST_HOOK_ABI appearance_unit_shield(GuestRegs *regs);
void GUEST_HOOK_ABI appearance_select_weapon(GuestRegs *regs);
void GUEST_HOOK_ABI appearance_select_shield(GuestRegs *regs);

#ifdef __cplusplus
}
#endif

#endif
