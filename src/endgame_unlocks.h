/* Unlock Labyrinth of Chaos and Unlock Ultimate + Tower of Mirages: once the character's own story has opened the
 * Stables, the game's own availability checks answer as if the requirement were met. Labyrinth: the one story check
 * every Labyrinth entry point asks. Ultimate: the Temple's three prayer tier checks and its list's Ultimate flag, for
 * the Ultimate prayer (and the Normal prayer while the character is on Ultimate). Tower of Mirages is the game's
 * Labyrinth while the character's difficulty is Ultimate, so that setting also opens the Labyrinth entry then. Nothing
 * is written to the save; floors, rewards and difficulty choices stay the game's own. */
#ifndef DCP_ENDGAME_UNLOCKS_H
#define DCP_ENDGAME_UNLOCKS_H

#include <stddef.h>
#include <stdint.h>

#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ENDGAME_UNLOCK_LABYRINTH 1u
#define ENDGAME_UNLOCK_ULTIMATE_TOWER 2u

enum {
    ENDGAME_STORY_STEP = 0x6f,     /* character record byte */
    ENDGAME_STABLES_AFTER = 0x28,  /* the game's Stables check: step above this */
    ENDGAME_LABYRINTH_AFTER = 0x2d,
    ENDGAME_DIFFICULTY = 0x21a9,   /* character record byte the Temple's difficulty prayers write */
    ENDGAME_ULTIMATE = 0x63,
    ENDGAME_PRAYER_ULTIMATE = 19,
    ENDGAME_PRAYER_NORMAL = 14,
    ENDGAME_ULTIMATE_LISTED = 0x1a9 /* save byte the prayer list reads for the Ultimate prayer */
};

typedef struct {
    uint64_t labyrinth;    /* the story check call inside the Labyrinth wrapper */
    uint64_t list_tier;    /* the Temple prayer list's tier call */
    uint64_t row_tier;     /* a prayer row's tier call */
    uint64_t row_cleared;  /* a difficulty prayer row's cleared-difficulty call */
    uint64_t confirm_tier; /* the prayer confirmation's tier call */
} EndgameSites;

/* The saved choices (ENDGAME_UNLOCK_* bits) with DCP_UNLOCK_LABYRINTH / DCP_UNLOCK_ULTIMATE_TOWER (0/1, off/on) taking
 * precedence. */
unsigned endgame_unlocks_wanted(unsigned saved);

/* The game's own answers, from a character record (and a Temple prayer record: id, required tier, kind, value). */
int endgame_stables_open(const unsigned char *record);
int endgame_native_labyrinth(const unsigned char *record);
int endgame_native_prayer_tier(const unsigned char *record);
int endgame_native_cleared_tier(const unsigned char *record);

/* The answers with `unlocks` applied. */
int endgame_labyrinth_open(const unsigned char *record, unsigned unlocks);
int endgame_prayer_exposed(const unsigned char *record, const unsigned char *prayer, unsigned unlocks);
int endgame_prayer_tier(const unsigned char *record, const unsigned char *prayer, unsigned unlocks);
int endgame_cleared_tier(const unsigned char *record, const unsigned char *prayer, unsigned unlocks);

/* The sites for a build ("CUSA10454-1.00"); all zero unless every one matches. Returns 1 when the build is listed; any
 * other build is searched and every window must match exactly once. */
int endgame_unlocks_locate(const unsigned char *image, uint64_t size, const char *game, EndgameSites *sites);

size_t endgame_unlocks_stub_bytes(void);

/* The unlocks the hooks answer with; endgame_unlocks_install sets the ones it installed. */
void endgame_unlocks_set(unsigned unlocks);

/* Installs the hooks the wanted unlocks need; returns the unlocks installed. Writes nothing and says why when the code
 * or the stub space does not fit. */
unsigned endgame_unlocks_install(unsigned char *image, uint64_t size, uint64_t entry, const char *game, unsigned wanted,
                                 unsigned char *stubs, size_t stubs_size);

/* From guest code at the replaced calls: rdi is the character record; the prayer is in rbx (list, confirm) or r14 (row). */
void GUEST_HOOK_ABI endgame_labyrinth_check(GuestRegs *regs);
void GUEST_HOOK_ABI endgame_list_tier(GuestRegs *regs);
void GUEST_HOOK_ABI endgame_row_tier(GuestRegs *regs);
void GUEST_HOOK_ABI endgame_row_cleared(GuestRegs *regs);
void GUEST_HOOK_ABI endgame_confirm_tier(GuestRegs *regs);

#ifdef __cplusplus
}
#endif

#endif
