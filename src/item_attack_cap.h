/* Item Attack Cap: the game's item validator, run on every item when a save loads, lowers a weapon's two attack bytes to
 * the cap its table gives for the item's required level. The PC port keeps the saved attack: the two stores of that clamp
 * are NOPed in the loaded image, and the rest of the validator (durability, modifiers, names, empty slots) is untouched. A
 * supported build is patched only where the verified bytes match: at the build's known site, or at the single place in
 * the image the same window appears. */
#ifndef DCP_ITEM_ATTACK_CAP_H
#define DCP_ITEM_ATTACK_CAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ITEM_ATTACK_CAP_WINDOW_BYTES 73
#define ITEM_ATTACK_CAP_STORE_MIN 50
#define ITEM_ATTACK_CAP_STORE_MAX 63
#define ITEM_ATTACK_CAP_STORE_BYTES 5

/* The window start for a supported build ("CUSA10454-1.00"; NULL: no build), 0 when its code does not match. Returns 1
 * when the build is supported; a build without a known site is searched and must match exactly once. */
int item_attack_cap_locate(const unsigned char *image, uint64_t size, const char *game, uint64_t *window);

/* Removes the clamp for a supported build; returns 1 when both stores were replaced. Writes nothing and says why when the
 * code does not match. */
int item_attack_cap_install(unsigned char *image, uint64_t size, const char *game);

#ifdef __cplusplus
}
#endif

#endif
