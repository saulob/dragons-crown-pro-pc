/* Story Skip Offer and Tutorial Skip Offer: the game's own questions, normally hidden, asked for every new character.
 * Story: on confirming a new character the game offers to start from the second half of the story, otherwise only once
 * a class on the file has finished the game. Tutorial: "Begin the tutorial?" is asked for the file's first character
 * too, instead of starting the tutorial by itself. Existing characters are never offered either. Each is one branch
 * NOPed in memory, only where the verified bytes match: at the build's known site, or, for a supported build without
 * one, at the single place in the image the same window appears. */
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

/* The story and tutorial window starts for a build ("CUSA10454-1.00"), 0 for one whose bytes do not match. Returns 1
 * when the build has known sites, 0 when they were searched for. */
int skip_offers_locate(const unsigned char *image, uint64_t size, const char *game, uint64_t *story, uint64_t *tutorial);

/* Patches the wanted offers for a supported build (NULL: an unknown build, never patched); returns the ones applied. */
unsigned skip_offers_install(unsigned char *image, uint64_t size, const char *game, unsigned wanted);

#ifdef __cplusplus
}
#endif

#endif
