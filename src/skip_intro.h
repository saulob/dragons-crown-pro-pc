/* Skip Intro: the opening scene polls "did any player press a button" every frame and ends itself, with a fade, when one
 * did. With the setting on, the first poll is answered "pressed" and the game takes that skip by itself. Only the
 * poll's call is redirected, where the code around it is verified. */
#ifndef DCP_SKIP_INTRO_H
#define DCP_SKIP_INTRO_H

#include <stddef.h>
#include <stdint.h>

#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The saved choice (0/1) with DCP_SKIP_INTRO (0/1, off/on) taking precedence. */
int skip_intro_wanted(int saved);

/* The hooked poll for a build ("CUSA10454-1.00"), 0 when its code does not match. Returns 1 when the build is listed. */
int skip_intro_locate(const unsigned char *image, uint64_t size, const char *game, uint64_t *site);

size_t skip_intro_stub_bytes(void);

/* Hooks the poll for a listed build (NULL: unknown) and arms the one-shot press; returns 1 when installed. Writes
 * nothing and says why when the code or the stub space does not fit. */
int skip_intro_install(unsigned char *image, uint64_t size, uint64_t entry, const char *game, int wanted,
                       unsigned char *stubs, size_t stubs_size);

/* The stub for the poll's seven bytes: asks `claim` (System V, no arguments, 1 once) and, when it says 1, calls
 * `delivered` and returns 1 to `resume` without making the call. Returns its length. */
size_t skip_intro_build_stub(unsigned char *out, uint64_t claim, uint64_t delivered, uint64_t resume);

/* The one-shot press: 1 the first time after skip_intro_arm, then 0. */
void skip_intro_arm(void);
int GUEST_HOOK_ABI skip_intro_claim(void);
void GUEST_HOOK_ABI skip_intro_delivered(void);

#ifdef __cplusplus
}
#endif

#endif
