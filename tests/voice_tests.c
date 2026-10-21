/* Focused tests of the Japanese NPC voice routing: the cue ranges, the handler and the CUSA10454 1.00 site. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dcp_voice.h"
#include "guest_hook.h"
#include "voice_hooks.h"

static int checks;
static int failures;

#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

static void TestCueRanges(void) {
    const uint32_t npc[] = {10040, 10043, 10044, 10049, 10240, 10249, 10440, 10449,
                            10540, 10549, 10640, 10649, 10840, 10849};
    for (size_t i = 0; i < sizeof(npc) / sizeof(npc[0]); ++i) {
        CHECK(dcp_is_japanese_npc_cue(npc[i]));
    }
    const uint32_t other[] = {
        10039, 10050, 10239, 10250, 10439, 10450, 10539, 10550, 10639, 10650, 10839, 10850,
        10010, 10015, 2900,  2953,  1000,  1528,  101000, 101528, 20000, 120000,
        110040, 110049, 110043, 110044, 102900, 102953,
    };
    for (size_t i = 0; i < sizeof(other) / sizeof(other[0]); ++i) {
        CHECK(!dcp_is_japanese_npc_cue(other[i]));
    }
    CHECK(!dcp_is_japanese_npc_cue(0));
    CHECK(!dcp_is_japanese_npc_cue(UINT32_MAX));
}

static void TestHandler(void) {
    GuestRegs regs = {0};
    regs.rdi = 10040;
    regs.rax = 0xdead;
    voice_hooks_set_enabled(0);
    voice_npc_hook(&regs);
    CHECK(regs.skip == 0 && regs.rax == 0xdead);

    voice_hooks_set_enabled(1);
    regs = (GuestRegs){0};
    regs.rdi = 10040;
    regs.rax = 0xdead;
    voice_npc_hook(&regs);
    CHECK(regs.skip == 1 && regs.rax == 10040);

    regs = (GuestRegs){0};
    regs.rdi = 2900;
    regs.rax = 0xdead;
    voice_npc_hook(&regs);
    CHECK(regs.skip == 0 && regs.rax == 0xdead);

    CHECK(voice_hooks_enabled() == 1);
    voice_hooks_set_enabled(0);
    CHECK(voice_hooks_enabled() == 0);
}

static void TestInstall(void) {
    const uint64_t size = 0x11f8060;
    const size_t stub = voice_hooks_stub_bytes();
    voice_hooks_set_enabled(1);
    uint8_t *image = malloc(size + stub);
    CHECK(image != NULL);
    if (!image) {
        return;
    }
    memset(image, 0x90, size);

    CHECK(VOICE_NPC_SITE_OFFSET == 0x68bc9);
    CHECK(VOICE_NPC_SITE_LENGTH == 5);
    CHECK(voice_npc_site[0] == 0xb8 && voice_npc_site[1] == 0x1b && voice_npc_site[2] == 0x97 &&
          voice_npc_site[3] == 0x01 && voice_npc_site[4] == 0x00);
    memcpy(image + VOICE_NPC_SITE_OFFSET, voice_npc_site, VOICE_NPC_SITE_LENGTH);

    CHECK(voice_hooks_install(image, size, 0x54c690, image + size, stub) == 1);
    CHECK(image[VOICE_NPC_SITE_OFFSET] == 0xe9);
    for (int i = 5; i < VOICE_NPC_SITE_LENGTH; ++i) {
        CHECK(image[VOICE_NPC_SITE_OFFSET + i] == 0x90);
    }
    int32_t displacement = 0;
    memcpy(&displacement, image + VOICE_NPC_SITE_OFFSET + 1, sizeof(displacement));
    CHECK(image + VOICE_NPC_SITE_OFFSET + 5 + displacement == image + size);

    const uint8_t *found = NULL;
    for (size_t at = 0; at + VOICE_NPC_SITE_LENGTH <= stub && !found; ++at) {
        if (memcmp(image + size + at, voice_npc_site, VOICE_NPC_SITE_LENGTH) == 0) {
            found = image + size + at;
        }
    }
    CHECK(found != NULL);
    if (found) {
        uint64_t resume = 0;
        memcpy(&resume, found + VOICE_NPC_SITE_LENGTH + 6, sizeof(resume));
        CHECK(resume == (uint64_t)(uintptr_t)(image + VOICE_NPC_SITE_OFFSET + VOICE_NPC_SITE_LENGTH));
    }

    const struct {
        uint64_t size;
        uint64_t entry;
    } rejected[] = {
        {0x11e8060, 0x54a800}, // CUSA07001 1.00
        {0x11e8060, 0x54a660}, // CUSA07001 1.02
        {0x11e8060, 0x54a720}, // CUSA07001 1.03
        {0x1238060, 0x54a790}, // CUSA10209 1.00
        {0x11f8060, 0x54c691}, // another CUSA10454 executable
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        uint8_t *copy = malloc(size + stub);
        CHECK(copy != NULL);
        if (!copy) {
            continue;
        }
        memcpy(copy, image, size + stub);
        const uint8_t before = copy[VOICE_NPC_SITE_OFFSET];
        CHECK(voice_hooks_install(copy, rejected[i].size, rejected[i].entry, copy + size, stub) == 0);
        CHECK(copy[VOICE_NPC_SITE_OFFSET] == before);
        free(copy);
    }

    {
        uint8_t *copy = malloc(size + stub);
        CHECK(copy != NULL);
        if (copy) {
            memset(copy, 0x90, size);
            memcpy(copy + VOICE_NPC_SITE_OFFSET, voice_npc_site, VOICE_NPC_SITE_LENGTH);
            copy[VOICE_NPC_SITE_OFFSET + 2] ^= 0xff;
            const uint8_t before = copy[VOICE_NPC_SITE_OFFSET + 2];
            CHECK(voice_hooks_install(copy, size, 0x54c690, copy + size, stub) == 0);
            CHECK(copy[VOICE_NPC_SITE_OFFSET + 2] == before);
            free(copy);
        }
    }
    free(image);
    voice_hooks_set_enabled(0);
}

int main(void) {
    TestCueRanges();
    TestHandler();
    TestInstall();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
