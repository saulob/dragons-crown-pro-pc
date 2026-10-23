/* Focused tests of the Japanese NPC voice routing: the cue ranges, the handler, the bank pin and the CUSA10454 1.00
 * sites. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dcp_voice.h"
#include "guest_hook.h"
#include "voice_hooks.h"

static int checks;
static int failures;

/* The stub stores the guest return address right after GuestRegs. */
typedef struct {
    GuestRegs regs;
    uint64_t return_address;
} Frame;

#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

static void TestCueRanges(void) {
    const uint32_t npc[] = {10040, 10043, 10044, 10049, 10240, 10249, 10440, 10449,
                            10540, 10549, 10640, 10649, 10840, 10849, 7100, 7125, 7200, 7225, 8800, 8825};
    for (size_t i = 0; i < sizeof(npc) / sizeof(npc[0]); ++i) {
        CHECK(dcp_is_japanese_npc_cue(npc[i]));
    }
    const uint32_t other[] = {
        10039, 10050, 10239, 10250, 10439, 10450, 10539, 10550, 10639, 10650, 10839, 10850,
        10010, 10015, 2900,  2953,  1000,  1528,  101000, 101528, 20000, 120000,
        110040, 110049, 110043, 110044, 102900, 102953, 7099, 7126, 7199, 7226, 8799, 8826,
        107100, 107200, 108800, 90900, 91000, 8700, 7300,
    };
    for (size_t i = 0; i < sizeof(other) / sizeof(other[0]); ++i) {
        CHECK(!dcp_is_japanese_npc_cue(other[i]));
    }
    CHECK(!dcp_is_japanese_npc_cue(0));
    CHECK(!dcp_is_japanese_npc_cue(UINT32_MAX));
}

static void TestHandler(void) {
    Frame frame = {0};
    GuestRegs *regs = &frame.regs;
    regs->rdi = 10040;
    regs->rax = 0xdead;
    voice_hooks_set_enabled(0);
    voice_npc_hook(regs);
    CHECK(regs->skip == 0 && regs->rax == 0xdead);

    voice_hooks_set_enabled(1);
    frame = (Frame){0};
    regs->rdi = 10040;
    regs->rax = 0xdead;
    voice_npc_hook(regs);
    CHECK(regs->skip == 1 && regs->rax == 10040);

    frame = (Frame){0};
    regs->rdi = 2900;
    regs->rax = 0xdead;
    voice_npc_hook(regs);
    CHECK(regs->skip == 0 && regs->rax == 0xdead);

    CHECK(voice_hooks_enabled() == 1);
    voice_hooks_set_enabled(0);
    CHECK(voice_hooks_enabled() == 0);
}

static void TestOffIsVanilla(void) {
    voice_hooks_set_enabled(0);
    const uint32_t cues[] = {10040, 10043, 10045, 110043, 110045, 108, 10544, 2900, 7100, 107100};
    for (size_t i = 0; i < sizeof(cues) / sizeof(cues[0]); ++i) {
        Frame frame = {0};
        frame.regs.rdi = cues[i];
        frame.regs.rsi = 1;
        frame.regs.rax = 0xdead;
        frame.regs.rdx = 7;
        const Frame before = frame;
        voice_npc_hook(&frame.regs);
        CHECK(memcmp(&frame, &before, sizeof(frame)) == 0);
    }
}

static void TestMapperTable(void) {
    voice_hooks_set_enabled(1);
    const uint32_t npc[] = {10040, 10045, 10049, 10240, 10241, 10440, 10540, 10544, 10640, 10840,
                            7100, 7102, 7123, 7200, 7202, 7223, 8800, 8806};
    for (size_t i = 0; i < sizeof(npc) / sizeof(npc[0]); ++i) {
        Frame frame = {0};
        frame.regs.rdi = npc[i];
        frame.regs.rsi = 1;
        voice_npc_hook(&frame.regs);
        CHECK(frame.regs.skip == 1 && frame.regs.rax == npc[i] && frame.regs.rsi == 1);
    }
    const uint32_t english[] = {1000, 1500, 2900, 2953, 3000, 3026, 4300, 7099, 7126, 7199, 7226, 7300, 7900, 8515,
                                8799, 8826, 8700, 90900, 91000, 10010, 10015, 90000, 91600,
                                110045, 110043, 107102, 107202, 108800, 10050};
    for (size_t i = 0; i < sizeof(english) / sizeof(english[0]); ++i) {
        Frame frame = {0};
        frame.regs.rdi = english[i];
        frame.regs.rax = 0xdead;
        const Frame before = frame;
        voice_npc_hook(&frame.regs);
        CHECK(memcmp(&frame, &before, sizeof(frame)) == 0);
    }
    voice_hooks_set_enabled(0);
}

static void TestResourceAcquireHook(void) {
    char gimmick[] = "snd/se_gimmick110000_v.acb";
    char other[] = "snd/bgm.acb";
    voice_hooks_set_enabled(1);
    voice_pin_reset();
    Frame frame = {0};
    frame.regs.rsi = (uint64_t)(uintptr_t)gimmick;
    frame.regs.rax = 0xdead;
    const Frame before = frame;
    voice_res_acquire_hook(&frame.regs); // no installed image: the acquire pointer is unusable, so this is a no-op
    CHECK(memcmp(&frame, &before, sizeof(frame)) == 0 && voice_pinned_mask() == 0);
    frame.regs.rsi = (uint64_t)(uintptr_t)other;
    voice_res_acquire_hook(&frame.regs);
    frame.regs.rsi = 0;
    voice_res_acquire_hook(&frame.regs);
    voice_pin_reset();
    voice_hooks_set_enabled(0);
}

static int acquire_calls;
static char acquired_names[16][64];
static uint32_t fake_refs[VOICE_PIN_BANKS];

static void *GUEST_HOOK_ABI FakeAcquire(void *manager, const char *name) {
    (void)manager;
    if (acquire_calls < 16) snprintf(acquired_names[acquire_calls], sizeof(acquired_names[0]), "%s", name);
    ++acquire_calls;
    int english = 0;
    const int index = voice_pin_target_index(name, &english);
    if (index < 0) return NULL;
    ++fake_refs[index];
    return &fake_refs[index];
}

static void TestPinClassification(void) {
    const char *japanese[VOICE_PIN_BANKS] = {"snd/se_gimmick10000_v.acb", "snd/se_gimmick10200_v.acb",
                                             "snd/se_gimmick10400_v.acb", "snd/se_gimmick10500_v.acb",
                                             "snd/se_gimmick10600_v.acb", "snd/se_gimmick10800_v.acb",
                                             "snd/se_enemy7100_v.acb",    "snd/se_enemy7200_v.acb",
                                             "snd/se_enemy8800_v.acb"};
    const char *english[VOICE_PIN_BANKS] = {"snd/se_gimmick110000_v.acb", "SND/SE_GIMMICK110200_V.ACB",
                                            "snd/se_gimmick110400_v.acb", "se_gimmick110500_v.acb",
                                            "snd/se_gimmick110600_v.acb", "snd/se_gimmick110800_v.acb",
                                            "snd/se_enemy107100_v.acb",  "SND/SE_ENEMY107200_V.ACB",
                                            "snd/se_enemy108800_v.acb"};
    for (int i = 0; i < VOICE_PIN_BANKS; ++i) {
        int is_english = -1;
        CHECK(voice_pin_target_index(japanese[i], &is_english) == i && is_english == 0);
        CHECK(voice_pin_target_index(english[i], &is_english) == i && is_english == 1);
        CHECK(strcmp(voice_pin_bank_name(i), japanese[i]) == 0);
    }
    const char *others[] = {"snd/se_gimmick10000.acb", "snd/se_gimmick10100.acb", "snd/se_gimmick10100_v.acb",
                            "snd/se_gimmick110100_v.acb", "snd/se_gimmick10900.acb", "snd/se_player1000_v.acb",
                            "snd/se_player101000_v.acb", "snd/se_enemy3000_v.acb", "snd/se_common.acb",
                            "snd/bgm.acb", "snd/se_gimmick10000_v.acb.bak", "snd/se_enemy3000_v.acb",
                            "snd/se_enemy107300_v.acb", "snd/se_enemy7100.acb", "snd/se_enemy7100_v.acb2",
                            "snd/se_enemy90900_v.acb", "snd/se_enemy7300_v.acb", "snd/se_gimmick7100_v.acb",
                            "snd/se_enemy10000_v.acb", "snd/se_gimmick_v.acb", "", NULL};
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); ++i) {
        CHECK(voice_pin_target_index(others[i], NULL) == -1);
    }
    CHECK(voice_pin_bank_name(-1) == NULL && voice_pin_bank_name(VOICE_PIN_BANKS) == NULL);
}

static void TestPinLifecycle(void) {
    int manager = 0;
    voice_pin_reset();
    voice_hooks_set_enabled(0);
    acquire_calls = 0;
    memset(fake_refs, 0, sizeof(fake_refs));
    CHECK(voice_pin_bank(0, &manager, FakeAcquire) == 0 && acquire_calls == 0 && voice_pinned_mask() == 0);

    voice_hooks_set_enabled(1);
    CHECK(voice_pin_bank(-1, &manager, FakeAcquire) == 0);
    CHECK(voice_pin_bank(VOICE_PIN_BANKS, &manager, FakeAcquire) == 0);
    CHECK(voice_pin_bank(0, NULL, FakeAcquire) == 0);
    CHECK(voice_pin_bank(0, &manager, NULL) == 0);
    CHECK(acquire_calls == 0 && voice_pinned_mask() == 0);

    CHECK(voice_pin_bank(0, &manager, FakeAcquire) == 1);
    CHECK(strcmp(acquired_names[0], "snd/se_gimmick10000_v.acb") == 0);
    for (int i = 0; i < 50; ++i) {
        CHECK(voice_pin_bank(0, &manager, FakeAcquire) == 0);
    }
    CHECK(acquire_calls == 1 && fake_refs[0] == 1 && voice_pinned_mask() == 1);

    for (int i = 1; i < VOICE_PIN_BANKS; ++i) {
        CHECK(voice_pin_bank(i, &manager, FakeAcquire) == 1);
        CHECK(voice_pin_bank(i, &manager, FakeAcquire) == 0);
    }
    CHECK(acquire_calls == VOICE_PIN_BANKS && voice_pinned_mask() == (1u << VOICE_PIN_BANKS) - 1);
    for (int i = 0; i < VOICE_PIN_BANKS; ++i) {
        CHECK(fake_refs[i] == 1);
    }

    voice_pin_reset();
    voice_hooks_set_enabled(0);
    CHECK(voice_pinned_mask() == 0);
}

static void TestPinHookIsInertWithoutImage(void) {
    char english[] = "snd/se_gimmick110200_v.acb";
    int manager = 0;
    voice_pin_reset();
    voice_hooks_set_enabled(1);
    Frame frame = {0};
    frame.regs.rsi = (uint64_t)(uintptr_t)english;
    frame.regs.rdi = (uint64_t)(uintptr_t)&manager;
    frame.regs.rax = 0xdead;
    const Frame before = frame;
    voice_res_acquire_hook(&frame.regs);
    CHECK(memcmp(&frame, &before, sizeof(frame)) == 0 && voice_pinned_mask() == 0);
    voice_hooks_set_enabled(0);
}

static void TestSites(void) {
    CHECK(VOICE_NPC_SITE_OFFSET == 0x68bc9 && VOICE_NPC_SITE_LENGTH == 5);
    CHECK(VOICE_RES_ACQUIRE_SITE_OFFSET == 0x57cbb0 && VOICE_RES_ACQUIRE_SITE_LENGTH == 6);
    CHECK(VOICE_HOOK_COUNT == 2);
    CHECK(voice_hooks_stub_bytes() == VOICE_HOOK_COUNT * guest_hook_stub_size());
    CHECK(sizeof(GuestRegs) == 264);
    CHECK(VOICE_NPC_SITE_OFFSET + VOICE_NPC_SITE_LENGTH <= VOICE_RES_ACQUIRE_SITE_OFFSET ||
          VOICE_RES_ACQUIRE_SITE_OFFSET + VOICE_RES_ACQUIRE_SITE_LENGTH <= VOICE_NPC_SITE_OFFSET);
}

static void PlaceSites(uint8_t *image) {
    memcpy(image + VOICE_NPC_SITE_OFFSET, voice_npc_site, VOICE_NPC_SITE_LENGTH);
    memcpy(image + VOICE_RES_ACQUIRE_SITE_OFFSET, voice_res_acquire_site, VOICE_RES_ACQUIRE_SITE_LENGTH);
}

static void TestInstall(int enabled) {
    const uint64_t size = 0x11f8060;
    const size_t stub = voice_hooks_stub_bytes();
    const size_t stride = guest_hook_stub_size();
    voice_hooks_set_enabled(enabled);
    uint8_t *image = malloc(size + stub);
    CHECK(image != NULL);
    if (!image) {
        return;
    }
    memset(image, 0x90, size);

    CHECK(voice_npc_site[0] == 0xb8 && voice_npc_site[1] == 0x1b && voice_npc_site[2] == 0x97 &&
          voice_npc_site[3] == 0x01 && voice_npc_site[4] == 0x00);
    PlaceSites(image);

    CHECK(voice_hooks_install(image, size, 0x54c690, image + size, stub) == 1);
    CHECK(image[VOICE_NPC_SITE_OFFSET] == 0xe9);
    for (int i = 5; i < VOICE_NPC_SITE_LENGTH; ++i) {
        CHECK(image[VOICE_NPC_SITE_OFFSET + i] == 0x90);
    }
    int32_t displacement = 0;
    memcpy(&displacement, image + VOICE_NPC_SITE_OFFSET + 1, sizeof(displacement));
    CHECK(image + VOICE_NPC_SITE_OFFSET + 5 + displacement == image + size);

    CHECK(image[VOICE_RES_ACQUIRE_SITE_OFFSET] == 0xe9);
    int32_t jump = 0;
    memcpy(&jump, image + VOICE_RES_ACQUIRE_SITE_OFFSET + 1, sizeof(jump));
    CHECK(image + VOICE_RES_ACQUIRE_SITE_OFFSET + 5 + jump == image + size + stride);

    const uint8_t *found = NULL;
    for (size_t at = 0; at + VOICE_NPC_SITE_LENGTH <= stride && !found; ++at) {
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
            PlaceSites(copy);
            copy[VOICE_NPC_SITE_OFFSET + 2] ^= 0xff;
            const uint8_t before = copy[VOICE_NPC_SITE_OFFSET + 2];
            CHECK(voice_hooks_install(copy, size, 0x54c690, copy + size, stub) == 0);
            CHECK(copy[VOICE_NPC_SITE_OFFSET + 2] == before);
            free(copy);
        }
    }

    // A bad byte on the second hook's site must roll back the first hook too: install is all-or-nothing.
    {
        uint8_t *copy = malloc(size + stub);
        CHECK(copy != NULL);
        if (copy) {
            memset(copy, 0x90, size);
            PlaceSites(copy);
            copy[VOICE_RES_ACQUIRE_SITE_OFFSET + 1] ^= 0xff;
            const uint8_t mapper_before = copy[VOICE_NPC_SITE_OFFSET];
            CHECK(voice_hooks_install(copy, size, 0x54c690, copy + size, stub) == 0);
            CHECK(copy[VOICE_NPC_SITE_OFFSET] == mapper_before);
            free(copy);
        }
    }
    free(image);
    voice_hooks_set_enabled(0);
}

int main(void) {
    TestCueRanges();
    TestHandler();
    TestOffIsVanilla();
    TestMapperTable();
    TestResourceAcquireHook();
    TestPinClassification();
    TestPinLifecycle();
    TestPinHookIsInertWithoutImage();
    TestSites();
    TestInstall(0);
    TestInstall(1);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
