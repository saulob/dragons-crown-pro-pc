/* Story Skip Offer and Tutorial Skip Offer: the game's own questions, normally hidden, asked for every new character.
 * Story: on confirming a new character the game offers to start from the second half of the story, otherwise only once
 * a class on the file has finished the game. Tutorial: "Begin the tutorial?" is asked for the file's first character
 * too, instead of starting the tutorial by itself. Existing characters are never offered either. Each is one branch
 * NOPed in memory, only on builds whose bytes match. */
#ifndef DCP_SKIP_OFFERS_H
#define DCP_SKIP_OFFERS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SKIP_OFFER_STORY 1u
#define SKIP_OFFER_TUTORIAL 2u

/* The saved choices with DCP_STORY_SKIP / DCP_TUTORIAL_SKIP (0/1, off/on) taking precedence. */
unsigned skip_offers_wanted(unsigned saved);

/* Patches the wanted offers for a build ("CUSA10454-1.00"); returns the ones applied. A site whose bytes differ is
 * left alone. */
unsigned skip_offers_install(unsigned char *image, uint64_t size, const char *game, unsigned wanted);

#ifdef __cplusplus
}
#endif

#endif
