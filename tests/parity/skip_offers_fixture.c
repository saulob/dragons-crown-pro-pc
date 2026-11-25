#include "../../src/skip_offers.c"

#include "fixture.h"

static void plant(Fx *fx, const SkipSites *sites) {
    uint8_t story[STORY_WINDOW], tutorial[TUTORIAL_WINDOW];
    fx_context(fx, "Skip Offers");
    memcpy(story, kStory, sizeof(story));
    memcpy(story + STORY_FLAG, sites->flag, sizeof(sites->flag));
    memcpy(tutorial, kTutorial, sizeof(tutorial));
    tutorial[TUTORIAL_SLOT] = sites->slot;
    FX_BYTES(fx, sites->story, story);
    FX_BYTES(fx, sites->tutorial, tutorial);
    FX_BYTES(fx, sites->tutorial + TUTORIAL_MESSAGE, kTutorialMessage);
}

void fixture_skip_offers_again(Fx *fx) {
    SkipSites copy = kBuilds[0].sites;
    copy.story = fx_alloc(fx, 0x80);
    copy.tutorial = fx_alloc(fx, 0x80);
    plant(fx, &copy);
}

void fixture_skip_offers(Fx *fx, const char *layout) {
    const SkipSites *sites = build_sites(layout);
    if (sites) plant(fx, sites);
}
