/* Character Highlight: a local visual effect, never sent to or read from another client. Each state belongs to a party slot
 * this client owns; the selected atlas of each local player and the match of a GPU image against the atlas signatures
 * (the boot image table plus the local custom looks) decide which draws get the outline. */
#ifndef DCP_CHARACTER_HIGHLIGHT_H
#define DCP_CHARACTER_HIGHLIGHT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHARACTER_HIGHLIGHT_PLAYERS 4
#define CHARACTER_HIGHLIGHT_NATIVE_LOOK 0u
#define CHARACTER_HIGHLIGHT_REMOTE_CUSTOM 0xffffffu

/* The atlas of a party seat's look: NATIVE_LOOK for the native atlas, character_highlight_custom_key() of the entry for
 * this client's custom look, REMOTE_CUSTOM for another player's custom look. `owner` is the seat's owner field. */
typedef uint32_t (*CharacterHighlightLook)(int slot, uint64_t record, int klass, int color, int owner);
void character_highlight_set_look_resolver(CharacterHighlightLook look);
uint32_t character_highlight_custom_key(const char *entry);

/* Signatures of this client's custom atlas (FTX bytes); the number of textures added. */
int character_highlight_add_custom_atlas(const char *entry, int klass, int color, const uint8_t *ftx, size_t size);

/* The party slot (0..3) of the local player with this index in slot order, -1 when there is none. Local player 0 is the
 * primary one: the keyboard's; a controller's index is the player it drives. */
int character_highlight_local_slot(int local_index);
/* Flips that local player's state; the party slot, or -1 when there is no such local player. */
int character_highlight_toggle_local(int local_index);
/* Flips the state of the seat this client owns whose pad binding is `pad` (the controller's game pad index); the party
 * slot, or -1 when no owned seat has that pad. Never falls back to another seat. */
int character_highlight_toggle_pad(int pad);
int character_highlight_player_enabled(int slot);

/* The atlas-signatures.txt of the boot image folder; the entry count, or -1 when it is missing or unreadable. */
int character_highlight_load_table(const char *path);

/* Every player unit on its per-tick update (game thread). The first also calls `forward` with the unit. */
void character_highlight_set_forward(void (*forward)(uint64_t unit));
void character_highlight_observe_unit(uint64_t unit);
void character_highlight_observe_local_unit(uint64_t unit);

/* The GPU image lifecycle: registered once with its guest range, unregistered when it goes away. */
void character_highlight_image_registered(uint32_t image, uint64_t guest_address, uint64_t guest_size, uint32_t width,
                                          uint32_t height);
void character_highlight_image_unregistered(uint32_t image);

/* Signatures computed so far (images that passed the size and dimension checks), for tests. */
unsigned character_highlight_hash_count(void);

/* The draw path: the party slot (0..3) whose highlight applies to a draw sampling this image, or -1. -1 also when the image
 * is not a local player's atlas, that player's highlight is off, or the atlas is shared with another local or remote
 * player. */
int character_highlight_slot_for_image(uint32_t image);
/* The same for a draw whose texture descriptor points at this registered atlas address. */
int character_highlight_slot_for_address(uint64_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
