#include "skip_offers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Story: the menu's new-character test, the six "no class has finished the game" tests ANDed into cl, then the jne
 * past state 9 (the offer). Only the jne goes; the new-character test stays. */
#define STORY_WINDOW 47
#define STORY_JNE 39
/* Tutorial: the "first character of the file" result, the je past the question, and the question's message id. */
#define TUTORIAL_WINDOW 12
#define TUTORIAL_JE 2
#define TUTORIAL_MESSAGE 0x58

typedef struct {
    const char *game;
    uint64_t story;    /* start of the story window */
    uint8_t flag[2];   /* the new-character byte's offset in the menu object */
    uint64_t tutorial; /* start of the tutorial window */
} SkipSites;

static const SkipSites kSites[] = {
    {"CUSA10454-1.00", 0x3aed82, {0xda, 0xf8}, 0x3a8ad9},
    {"CUSA07001-1.02", 0x3ae522, {0x4a, 0xf4}, 0x3a8309},
    {"CUSA07001-1.03", 0x3ae572, {0x4a, 0xf4}, 0x3a8359},
};

static void story_bytes(const uint8_t flag[2], uint8_t out[STORY_WINDOW]) {
    static const uint8_t pattern[STORY_WINDOW] = {
        0x41, 0x80, 0xbd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x82, 0xb0, 0x00, 0x00, 0x00, 0x74,
        0x1f, 0x44, 0x22, 0xa5, 0x3b, 0xfd, 0xff, 0xff, 0x22, 0x8d, 0x28, 0xfd, 0xff, 0xff, 0x45, 0x20,
        0xfe, 0x45, 0x20, 0xe6, 0x44, 0x20, 0xf1, 0x75, 0x07, 0xba, 0x09, 0x00, 0x00, 0x00, 0xeb};
    memcpy(out, pattern, STORY_WINDOW);
    out[3] = flag[0];
    out[4] = flag[1];
}

static const uint8_t kTutorial[TUTORIAL_WINDOW] = {0x84, 0xc0, 0x0f, 0x84, 0x44, 0x05, 0x00, 0x00, 0x4c, 0x8b, 0x6d, 0xb0};
static const uint8_t kTutorialMessage[5] = {0xba, 0xd8, 0x05, 0x00, 0x00};

static int env_choice(const char *name, int fallback) {
    const char *v = getenv(name);
    if (!v || !*v) return fallback;
    if (!strcmp(v, "1") || !strcmp(v, "on") || !strcmp(v, "true")) return 1;
    if (!strcmp(v, "0") || !strcmp(v, "off") || !strcmp(v, "false")) return 0;
    return fallback;
}

unsigned skip_offers_wanted(unsigned saved) {
    unsigned wanted = 0;
    if (env_choice("DCP_STORY_SKIP", (saved & SKIP_OFFER_STORY) != 0)) wanted |= SKIP_OFFER_STORY;
    if (env_choice("DCP_TUTORIAL_SKIP", (saved & SKIP_OFFER_TUTORIAL) != 0)) wanted |= SKIP_OFFER_TUTORIAL;
    return wanted;
}

static int matches(const unsigned char *image, uint64_t size, uint64_t at, const uint8_t *bytes, size_t length) {
    return at + length <= size && !memcmp(image + at, bytes, length);
}

unsigned skip_offers_install(unsigned char *image, uint64_t size, const char *game, unsigned wanted) {
    const SkipSites *sites = NULL;
    for (size_t i = 0; i < sizeof(kSites) / sizeof(*kSites); ++i)
        if (game && !strcmp(game, kSites[i].game)) sites = &kSites[i];
    if (!wanted) {
        printf("Skip offers: story off, tutorial off\n");
        return 0;
    }
    if (!sites) {
        printf("Skip offers: not available for this game build\n");
        return 0;
    }
    unsigned applied = 0;
    if (wanted & SKIP_OFFER_STORY) {
        uint8_t expected[STORY_WINDOW];
        story_bytes(sites->flag, expected);
        if (matches(image, size, sites->story, expected, STORY_WINDOW)) {
            memset(image + sites->story + STORY_JNE, 0x90, 2);
            applied |= SKIP_OFFER_STORY;
        } else {
            printf("Skip offers: story site 0x%llx does not match; left unchanged\n", (unsigned long long)sites->story);
        }
    }
    if (wanted & SKIP_OFFER_TUTORIAL) {
        if (matches(image, size, sites->tutorial, kTutorial, TUTORIAL_WINDOW) &&
            matches(image, size, sites->tutorial + TUTORIAL_MESSAGE, kTutorialMessage, sizeof(kTutorialMessage))) {
            static const uint8_t nop6[6] = {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00};
            memcpy(image + sites->tutorial + TUTORIAL_JE, nop6, sizeof(nop6));
            applied |= SKIP_OFFER_TUTORIAL;
        } else {
            printf("Skip offers: tutorial site 0x%llx does not match; left unchanged\n", (unsigned long long)sites->tutorial);
        }
    }
    printf("Skip offers: story %s, tutorial %s\n", applied & SKIP_OFFER_STORY ? "on" : "off",
           applied & SKIP_OFFER_TUTORIAL ? "on" : "off");
    return applied;
}
