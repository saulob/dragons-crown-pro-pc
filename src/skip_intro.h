/* Skip Intro: the title flow plays the opening scene (the ATLUS, Basiscape and CRIWARE logos, then the opening) between
 * the save data check and the title. That step asks the game whether the opening is needed and, when it is not, goes on
 * to the title without creating the scene. With the setting on, the first such question at startup is answered "not
 * needed"; the save data check before it is untouched. Builds that open with a legal notice skip it too. */
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

#define SKIP_INTRO_REDIRECT_BYTES 6
#define SKIP_INTRO_TITLE_CALL_BYTES 5

typedef struct {
    uint64_t redirect; /* the opening step's `call [rax+0x260]`, the "opening not needed" question */
    uint64_t title;    /* the title step's call that creates the title scene */
} SkipIntroSites;

/* Both sites for a build ("CUSA10454-1.00"), zero when its title flow does not match. Returns 1 when the build is
 * listed; an unlisted build is searched for exactly one title flow. */
int skip_intro_locate(const unsigned char *image, uint64_t size, const char *game, SkipIntroSites *sites);

typedef enum {
    SKIP_INTRO_NOTICE_ABSENT,   /* the build never constructs the regional startup notice */
    SKIP_INTRO_NOTICE_FOUND,    /* *site is where the bypass is written */
    SKIP_INTRO_NOTICE_UNMATCHED /* the notice is constructed, or expected, but not by the verified step */
} SkipIntroNotice;

#define SKIP_INTRO_BYPASS_BYTES 33

/* The Japanese builds show a legal notice in their boot step 5, which no other build has. */
SkipIntroNotice skip_intro_locate_notice(const unsigned char *image, uint64_t size, const char *game, uint64_t *site);

size_t skip_intro_stub_bytes(void);

/* Redirects the opening step for a listed build (NULL: unknown) and, where the build shows the regional notice, replaces
 * its construction with the step the other builds take. Returns 1 when installed. Writes nothing and says why when the
 * code or the stub space does not fit, or the notice is not the verified one. */
int skip_intro_install(unsigned char *image, uint64_t size, uint64_t entry, const char *game, int wanted,
                       unsigned char *stubs, size_t stubs_size);

/* The stub called in place of the question: when `claim` (System V, no arguments) says 1 it calls `redirected` and
 * answers 1, otherwise it asks the game's own question with the original rdi. Returns its length. */
size_t skip_intro_build_redirect_stub(unsigned char *out, uint64_t claim, uint64_t redirected);

/* The stub called in place of the title scene's creation: calls `reached`, then the original `create` with rdi. */
size_t skip_intro_build_title_stub(unsigned char *out, uint64_t reached, uint64_t create);

/* The one-shot redirect: 1 the first time after skip_intro_arm, then 0. */
void skip_intro_arm(void);
int GUEST_HOOK_ABI skip_intro_claim(void);
void GUEST_HOOK_ABI skip_intro_redirected(void);
void GUEST_HOOK_ABI skip_intro_title_reached(void);

#ifdef __cplusplus
}
#endif

#endif
