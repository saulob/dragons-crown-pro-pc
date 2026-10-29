#include "skip_offers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Story: the menu's new-character test, the six "no class has finished the game" tests ANDed into cl, then the jne
 * past state 9 (the offer). Only the jne goes; the new-character test stays. */
#define STORY_WINDOW 47
#define STORY_FLAG 3
#define STORY_JNE 39
/* Tutorial: the "first character of the file" result, the je past the question, and the question's message id. */
#define TUTORIAL_WINDOW 12
#define TUTORIAL_JE 2
#define TUTORIAL_SLOT 11
#define TUTORIAL_MESSAGE 0x58

typedef struct {
    uint64_t story;    /* start of the story window */
    uint8_t flag[2];   /* the new-character byte's offset in the menu object */
    uint64_t tutorial; /* start of the tutorial window */
    uint8_t slot;      /* the frame slot the tutorial window loads r13 from */
} SkipSites;

typedef struct {
    const char *game;
    SkipSites sites;
} BuildSites;

static const BuildSites kBuilds[] = {
    {"CUSA10454-1.00", {0x3aed82, {0xda, 0xf8}, 0x3a8ad9, 0xb0}},
    {"CUSA07001-1.00", {0x3ae162, {0x4a, 0xf4}, 0x3a7f1c, 0xb8}},
    {"CUSA07001-1.02", {0x3ae522, {0x4a, 0xf4}, 0x3a8309, 0xb0}},
    {"CUSA07001-1.03", {0x3ae572, {0x4a, 0xf4}, 0x3a8359, 0xb0}},
    {"CUSA10209-1.00", {0x3ae2d2, {0x4a, 0xf4}, 0x3a808c, 0xb8}},
};

static const uint8_t kStory[STORY_WINDOW] = {
    0x41, 0x80, 0xbd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x82, 0xb0, 0x00, 0x00, 0x00, 0x74,
    0x1f, 0x44, 0x22, 0xa5, 0x3b, 0xfd, 0xff, 0xff, 0x22, 0x8d, 0x28, 0xfd, 0xff, 0xff, 0x45, 0x20,
    0xfe, 0x45, 0x20, 0xe6, 0x44, 0x20, 0xf1, 0x75, 0x07, 0xba, 0x09, 0x00, 0x00, 0x00, 0xeb};
static const uint8_t kTutorial[TUTORIAL_WINDOW] = {0x84, 0xc0, 0x0f, 0x84, 0x44, 0x05, 0x00, 0x00, 0x4c, 0x8b, 0x6d, 0x00};
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

static int story_at(const unsigned char *image, uint64_t size, uint64_t at, const uint8_t flag[2]) {
    if (at + STORY_WINDOW > size) return 0;
    uint8_t expected[STORY_WINDOW];
    memcpy(expected, kStory, STORY_WINDOW);
    expected[STORY_FLAG] = flag ? flag[0] : image[at + STORY_FLAG];
    expected[STORY_FLAG + 1] = flag ? flag[1] : image[at + STORY_FLAG + 1];
    return !memcmp(image + at, expected, STORY_WINDOW);
}

/* A negative slot accepts the slot found in the image. */
static int tutorial_at(const unsigned char *image, uint64_t size, uint64_t at, int slot) {
    if (at + TUTORIAL_MESSAGE + sizeof(kTutorialMessage) > size) return 0;
    uint8_t expected[TUTORIAL_WINDOW];
    memcpy(expected, kTutorial, TUTORIAL_WINDOW);
    expected[TUTORIAL_SLOT] = slot < 0 ? image[at + TUTORIAL_SLOT] : (uint8_t)slot;
    return !memcmp(image + at, expected, TUTORIAL_WINDOW) &&
           !memcmp(image + at + TUTORIAL_MESSAGE, kTutorialMessage, sizeof(kTutorialMessage));
}

/* The one place in the image a window matches; 0 when it matches nowhere or more than once. */
static int find_story(const unsigned char *image, uint64_t size, uint64_t *at) {
    int found = 0;
    for (uint64_t i = 0; i + STORY_WINDOW <= size; ++i) {
        if (image[i] != kStory[0] || !story_at(image, size, i, NULL)) continue;
        if (found++) return 0;
        *at = i;
    }
    return found;
}

static int find_tutorial(const unsigned char *image, uint64_t size, uint64_t *at) {
    int found = 0;
    for (uint64_t i = 0; i + TUTORIAL_MESSAGE + sizeof(kTutorialMessage) <= size; ++i) {
        if (image[i] != kTutorial[0] || !tutorial_at(image, size, i, -1)) continue;
        if (found++) return 0;
        *at = i;
    }
    return found;
}

static const SkipSites *build_sites(const char *game) {
    for (size_t i = 0; game && i < sizeof(kBuilds) / sizeof(*kBuilds); ++i)
        if (!strcmp(game, kBuilds[i].game)) return &kBuilds[i].sites;
    return NULL;
}

int skip_offers_locate(const unsigned char *image, uint64_t size, const char *game, uint64_t *story,
                       uint64_t *tutorial) {
    const SkipSites *known = build_sites(game);
    if (known) {
        *story = story_at(image, size, known->story, known->flag) ? known->story : 0;
        *tutorial = tutorial_at(image, size, known->tutorial, known->slot) ? known->tutorial : 0;
        return 1;
    }
    if (!find_story(image, size, story)) *story = 0;
    if (!find_tutorial(image, size, tutorial)) *tutorial = 0;
    return 0;
}

unsigned skip_offers_install(unsigned char *image, uint64_t size, const char *game, unsigned wanted) {
    if (!wanted) {
        printf("Skip offers: story off, tutorial off\n");
        return 0;
    }
    if (!game) {
        printf("Skip offers: not available for an unknown game build\n");
        return 0;
    }
    uint64_t story = 0, tutorial = 0;
    const int known = skip_offers_locate(image, size, game, &story, &tutorial);
    unsigned applied = 0;
    if (wanted & SKIP_OFFER_STORY) {
        if (story) {
            memset(image + story + STORY_JNE, 0x90, 2);
            applied |= SKIP_OFFER_STORY;
        } else {
            printf("Skip offers: story site %s; left unchanged\n", known ? "does not match" : "not found");
        }
    }
    if (wanted & SKIP_OFFER_TUTORIAL) {
        if (tutorial) {
            static const uint8_t nop6[6] = {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00};
            memcpy(image + tutorial + TUTORIAL_JE, nop6, sizeof(nop6));
            applied |= SKIP_OFFER_TUTORIAL;
        } else {
            printf("Skip offers: tutorial site %s; left unchanged\n", known ? "does not match" : "not found");
        }
    }
    printf("Skip offers: story %s, tutorial %s (%s sites 0x%llx, 0x%llx)\n", applied & SKIP_OFFER_STORY ? "on" : "off",
           applied & SKIP_OFFER_TUTORIAL ? "on" : "off", known ? game : "signature", (unsigned long long)story,
           (unsigned long long)tutorial);
    return applied;
}
