/* Focused tests of Voice Language (issue #69): the setting and its legacy rule, the cue families, the cue mapper caller
 * hook for player, resurrected and AI party members, NPCs and enemies, the narrator remap, the bank pin, telemetry and
 * an install on a synthetic image. The real images are checked by dcp-feature-sites-tests and dcp-build-parity-tests. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dcp_voice.h"
#include "guest_hook.h"
#include "voice_hooks.h"

static int checks;
static int failures;

typedef struct {
    GuestRegs regs;
    uint64_t return_address;
} Frame;

#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

static const unsigned kAll = DCP_VOICE_ALL_CATEGORIES;

/* The game's cue mapper as the disassembly shows it: mode 1 gives a family cue its English twin, any other mode keeps
 * the Japanese cue. */
static uint32_t GameMapper(uint32_t cue, uint32_t mode) {
    return mode == DCP_VOICE_MODE_ENGLISH && dcp_voice_cue_family(cue) ? cue + DCP_VOICE_ENGLISH_OFFSET : cue;
}

/* The cue the speaking actor gets: its mode (the actor's +0x2c0 on the actor path, 1 on every USA path) through the
 * hook at the mapper's caller, then the mapper. */
static uint32_t Spoken(uint32_t cue, uint32_t native_mode) {
    Frame frame = {0};
    frame.regs.rdi = cue;
    frame.regs.rsi = native_mode;
    voice_cue_call_hook(&frame.regs);
    return GameMapper((uint32_t)frame.regs.rdi, (uint32_t)frame.regs.rsi);
}

/* The routing of the replaced Japanese NPC Voices, kept verbatim: the NPC families and the enemy families only. */
static int OldRoutes(uint32_t cue) {
    static const uint32_t npc[][2] = {{10040, 10049}, {10240, 10249}, {10440, 10449}, {10540, 10549}, {10640, 10649},
                                      {10840, 10849}, {7100, 7125},   {7200, 7225},   {8800, 8825}};
    for (size_t i = 0; i < sizeof(npc) / sizeof(npc[0]); ++i) {
        if (dcp_cue_in_range(cue, npc[i][0], npc[i][1])) return 1;
    }
    for (int i = 0; i < DCP_ENEMY_VOICE_FAMILY_COUNT; ++i) {
        if (dcp_cue_in_range(cue, kDcpEnemyVoiceFamilies[i], kDcpEnemyVoiceFamilies[i] + 25)) return 1;
    }
    return 0;
}

static void TestSettingAndMigration(void) {
    CHECK(dcp_voice_language_parse("default") == DCP_VOICE_DEFAULT);
    CHECK(dcp_voice_language_parse("english") == DCP_VOICE_ENGLISH);
    CHECK(dcp_voice_language_parse("japanese") == DCP_VOICE_JAPANESE);
    CHECK(dcp_voice_language_parse("Japanese") == -1 && dcp_voice_language_parse("") == -1 &&
          dcp_voice_language_parse(NULL) == -1 && dcp_voice_language_parse("1") == -1);
    CHECK(strcmp(dcp_voice_language_name(DCP_VOICE_DEFAULT), "Game Default") == 0);
    CHECK(strcmp(dcp_voice_language_key(DCP_VOICE_JAPANESE), "japanese") == 0 &&
          strcmp(dcp_voice_language_key(99), "default") == 0);
    CHECK(dcp_voice_language_resolve(-1, 1) == DCP_VOICE_JAPANESE);
    CHECK(dcp_voice_language_resolve(-1, 0) == DCP_VOICE_DEFAULT);
    CHECK(dcp_voice_language_resolve(-1, -1) == DCP_VOICE_DEFAULT);
    CHECK(dcp_voice_language_resolve(DCP_VOICE_ENGLISH, 1) == DCP_VOICE_ENGLISH);
    CHECK(dcp_voice_language_resolve(DCP_VOICE_DEFAULT, 1) == DCP_VOICE_DEFAULT);
    CHECK(dcp_voice_language_resolve(7, 1) == DCP_VOICE_JAPANESE);
    CHECK(dcp_voice_mode_of(DCP_VOICE_ENGLISH) == DCP_VOICE_MODE_ENGLISH &&
          dcp_voice_mode_of(DCP_VOICE_JAPANESE) == DCP_VOICE_MODE_JAPANESE);
    CHECK(strcmp(DCP_VOICE_LANGUAGE_KEY, "voice_language") == 0 && strcmp(DCP_VOICE_LEGACY_KEY, "dcp_japanese_npc_voices") == 0);
}

static void TestCueFamilies(void) {
    const struct {
        uint32_t cue, family;
        int category;
    } cases[] = {
        {1000, 1000, DCP_VOICE_PLAYER},  {1018, 1000, DCP_VOICE_PLAYER},  {1019, 0, DCP_VOICE_NONE},
        {1051, 0, DCP_VOICE_NONE},       {1105, 1100, DCP_VOICE_PLAYER},  {1518, 1500, DCP_VOICE_PLAYER},
        {2900, 2900, DCP_VOICE_PLAYER},  {2953, 2950, DCP_VOICE_PLAYER},  {2904, 0, DCP_VOICE_NONE},
        {10010, 10000, DCP_VOICE_NPC},   {10043, 10000, DCP_VOICE_NPC},   {10055, 0, DCP_VOICE_NONE},
        {10241, 10200, DCP_VOICE_NPC},   {10544, 10500, DCP_VOICE_NPC},   {10840, 10800, DCP_VOICE_NPC},
        {7100, 7100, DCP_VOICE_NPC},     {7225, 7200, DCP_VOICE_NPC},     {8849, 8800, DCP_VOICE_NPC},
        {7155, 0, DCP_VOICE_NONE},       {3000, 3000, DCP_VOICE_ENEMY},   {4203, 4200, DCP_VOICE_ENEMY},
        {90316, 90300, DCP_VOICE_ENEMY}, {91625, 91600, DCP_VOICE_ENEMY}, {3026, 0, DCP_VOICE_NONE},
        {108, 0, DCP_VOICE_NONE},        {101000, 0, DCP_VOICE_NONE},     {20000, 0, DCP_VOICE_NONE},
        {0, 0, DCP_VOICE_NONE},          {UINT32_MAX, 0, DCP_VOICE_NONE},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        CHECK(dcp_voice_cue_family(cases[i].cue) == cases[i].family);
        CHECK(dcp_voice_cue_category(cases[i].cue) == cases[i].category);
    }
}

/* Issue #69: the old routing left every player-class voice, own, resurrected or AI, on the English bank. */
static void TestOldGapIssue69(void) {
    for (int i = 0; i < DCP_PLAYER_VOICE_FAMILY_COUNT; ++i) {
        CHECK(!OldRoutes(kDcpPlayerVoiceFamilies[i]) && !OldRoutes(kDcpPlayerVoiceFamilies[i] + 18));
    }
    for (int i = 0; i < DCP_CAMP_VOICE_FAMILY_COUNT; ++i) {
        CHECK(!OldRoutes(kDcpCampVoiceFamilies[i]));
    }
    CHECK(OldRoutes(10043) && OldRoutes(90900));
    CHECK(voice_pin_target_index("snd/se_player1000_v.acb", NULL) == -1);
    CHECK(voice_pin_target_index("snd/se_player101000_v.acb", NULL) == -1);

    voice_hooks_configure(DCP_VOICE_JAPANESE, kAll);
    CHECK(GameMapper(1000, DCP_VOICE_MODE_ENGLISH) == 101000); /* what a USA actor spoke before */
    CHECK(Spoken(1000, DCP_VOICE_MODE_ENGLISH) == 1000);        /* se_player1000_v now */
    voice_hooks_configure(DCP_VOICE_DEFAULT, kAll);
}

static void TestPlayerBothDirections(void) {
    voice_hooks_configure(DCP_VOICE_JAPANESE, kAll);
    for (int i = 0; i < DCP_PLAYER_VOICE_FAMILY_COUNT; ++i) {
        for (uint32_t line = 0; line <= 18; ++line) {
            const uint32_t cue = kDcpPlayerVoiceFamilies[i] + line;
            CHECK(Spoken(cue, DCP_VOICE_MODE_ENGLISH) == cue);
        }
    }
    voice_hooks_configure(DCP_VOICE_ENGLISH, kAll);
    for (int i = 0; i < DCP_PLAYER_VOICE_FAMILY_COUNT; ++i) {
        for (uint32_t line = 0; line <= 18; ++line) {
            const uint32_t cue = kDcpPlayerVoiceFamilies[i] + line;
            CHECK(Spoken(cue, DCP_VOICE_MODE_JAPANESE) == cue + DCP_VOICE_ENGLISH_OFFSET);
        }
    }
    for (int i = 0; i < DCP_CAMP_VOICE_FAMILY_COUNT; ++i) {
        CHECK(Spoken(kDcpCampVoiceFamilies[i] + 3, DCP_VOICE_MODE_JAPANESE) == kDcpCampVoiceFamilies[i] + 3 + DCP_VOICE_ENGLISH_OFFSET);
    }
    voice_hooks_configure(DCP_VOICE_DEFAULT, kAll);
}

/* The same voice: each class keeps its own bank family and line in the other language. */
static void TestSameVoicePreserved(void) {
    for (int language = DCP_VOICE_ENGLISH; language <= DCP_VOICE_JAPANESE; ++language) {
        voice_hooks_configure(language, kAll);
        for (int i = 0; i < DCP_PLAYER_VOICE_FAMILY_COUNT; ++i) {
            const uint32_t cue = kDcpPlayerVoiceFamilies[i] + 5;
            for (uint32_t native = 0; native < 2; ++native) {
                const uint32_t spoken = Spoken(cue, native);
                CHECK(spoken % DCP_VOICE_ENGLISH_OFFSET == cue);
                CHECK(dcp_voice_cue_family(spoken % DCP_VOICE_ENGLISH_OFFSET) == kDcpPlayerVoiceFamilies[i]);
            }
        }
    }
    voice_hooks_configure(DCP_VOICE_DEFAULT, kAll);
}

/* Every player-class actor speaks through the actor path (mode = the actor's own +0x2c0, which the USA executable sets to
 * 1 for any character record, resurrected bones included) or the party member pick (mode -1 when the member has no
 * own mode and the global lookup fails); both reach the same caller hook. */
static void TestResurrectedAndPartyPaths(void) {
    voice_hooks_configure(DCP_VOICE_JAPANESE, kAll);
    const uint32_t actor_modes[] = {1, 0, 0xffffffffu};
    for (size_t i = 0; i < sizeof(actor_modes) / sizeof(actor_modes[0]); ++i) {
        CHECK(Spoken(1300 + 2, actor_modes[i]) == 1302);
    }
    voice_hooks_configure(DCP_VOICE_ENGLISH, kAll);
    for (size_t i = 0; i < sizeof(actor_modes) / sizeof(actor_modes[0]); ++i) {
        CHECK(Spoken(1300 + 2, actor_modes[i]) == 101302);
    }
    voice_hooks_configure(DCP_VOICE_DEFAULT, kAll);
}

static void TestNpcEnemyAllyRoutes(void) {
    voice_hooks_configure(DCP_VOICE_JAPANESE, kAll);
    CHECK(Spoken(10043, 1) == 10043 && Spoken(10010, 1) == 10010 && Spoken(7200, 1) == 7200);
    CHECK(Spoken(90316, 1) == 90316 && Spoken(3000, 1) == 3000 && Spoken(91400, 1) == 91400);
    voice_hooks_configure(DCP_VOICE_ENGLISH, kAll);
    CHECK(Spoken(10043, 0) == 110043 && Spoken(8800, 0) == 108800 && Spoken(90316, 0) == 190316);
    voice_hooks_configure(DCP_VOICE_DEFAULT, kAll);
}

static void TestOtherCuesUntouched(void) {
    const uint32_t cues[] = {108, 1051, 10055, 2000, 20000, 120000, 101000, 0};
    for (int language = DCP_VOICE_DEFAULT; language < DCP_VOICE_LANGUAGE_COUNT; ++language) {
        voice_hooks_configure(language, kAll);
        for (size_t i = 0; i < sizeof(cues) / sizeof(cues[0]); ++i) {
            for (uint32_t mode = 0; mode < 2; ++mode) {
                Frame frame = {0};
                frame.regs.rdi = cues[i];
                frame.regs.rsi = mode;
                const Frame before = frame;
                voice_cue_call_hook(&frame.regs);
                CHECK(memcmp(&frame, &before, sizeof(frame)) == 0);
            }
        }
    }
    voice_hooks_configure(DCP_VOICE_DEFAULT, kAll);
}

static void TestGameDefaultIsVanilla(void) {
    voice_hooks_configure(DCP_VOICE_DEFAULT, kAll);
    CHECK(voice_hooks_routed() == 0 && voice_hooks_language() == DCP_VOICE_DEFAULT);
    const uint32_t cues[] = {1000, 1518, 2900, 10043, 7100, 90900, 3000};
    for (size_t i = 0; i < sizeof(cues) / sizeof(cues[0]); ++i) {
        for (uint32_t mode = 0; mode < 2; ++mode) {
            Frame frame = {0};
            frame.regs.rdi = cues[i];
            frame.regs.rsi = mode;
            frame.regs.rax = 0xdead;
            const Frame before = frame;
            voice_cue_call_hook(&frame.regs);
            CHECK(memcmp(&frame, &before, sizeof(frame)) == 0);
        }
    }
    Frame narrator = {0};
    narrator.regs.rcx = 120000;
    narrator.regs.rbx = 16;
    const Frame before = narrator;
    voice_narrator_base_hook(&narrator.regs);
    voice_narrator_bank_hook(&narrator.regs);
    CHECK(memcmp(&narrator, &before, sizeof(narrator)) == 0);
    voice_hooks_configure(99, kAll);
    CHECK(voice_hooks_language() == DCP_VOICE_DEFAULT && voice_hooks_routed() == 0);
}

static void TestMissingAlternateBank(void) {
    voice_hooks_configure(DCP_VOICE_JAPANESE, DCP_VOICE_CATEGORY_BIT(DCP_VOICE_NPC));
    CHECK(Spoken(1000, 1) == 101000); /* no Japanese player banks: the game's own choice */
    CHECK(Spoken(90900, 1) == 190900);
    CHECK(Spoken(10043, 1) == 10043);
    voice_hooks_configure(DCP_VOICE_JAPANESE, 0);
    CHECK(voice_hooks_routed() == 0 && Spoken(10043, 1) == 110043);
    voice_hooks_configure(DCP_VOICE_DEFAULT, kAll);
}

static const uint32_t kUsNarrators[DCP_NARRATOR_BANKS] = {120000, 20000, 130000, 140000, 150000, 160000, 170000,
                                                         180000, 30000,  40000,  50000,  60000,  70000,  80000};
static const uint32_t kJapanNarrators[DCP_NARRATOR_BANKS] = {20000, 120000, 30000, 40000, 50000, 60000, 70000,
                                                            80000, 130000, 140000, 150000, 160000, 170000, 180000};

static int NarratorIndex(int index) {
    Frame frame = {0};
    frame.regs.rbx = (uint64_t)index * 16;
    voice_narrator_bank_hook(&frame.regs);
    return frame.regs.rbx % 16 ? -1 : (int)(frame.regs.rbx / 16);
}

static uint32_t NarratorBase(uint32_t base) {
    Frame frame = {0};
    frame.regs.rcx = base;
    voice_narrator_base_hook(&frame.regs);
    return (uint32_t)frame.regs.rcx;
}

static void TestNarrator(void) {
    CHECK(voice_narrator_prepare(kUsNarrators) == 1);
    voice_hooks_configure(DCP_VOICE_JAPANESE, kAll);
    CHECK(NarratorIndex(0) == 1 && NarratorIndex(1) == 1);
    for (int c = 0; c < 6; ++c) CHECK(NarratorIndex(2 + c) == 8 + c);
    CHECK(NarratorBase(120000) == 20000 && NarratorBase(20000) == 20000 && NarratorBase(180000) == 80000);
    CHECK(NarratorBase(19999) == 19999 && NarratorBase(90000) == 90000 && NarratorBase(125000) == 125000);
    voice_hooks_configure(DCP_VOICE_ENGLISH, kAll);
    CHECK(NarratorIndex(0) == 0 && NarratorIndex(1) == 0 && NarratorIndex(3) == 3);
    CHECK(NarratorBase(20000) == 120000 && NarratorBase(30000) == 130000);

    CHECK(voice_narrator_prepare(kJapanNarrators) == 1);
    voice_hooks_configure(DCP_VOICE_JAPANESE, kAll);
    for (int i = 0; i < 8; ++i) CHECK(NarratorIndex(i) == (i == 1 ? 0 : i)); /* the Japanese build's own narrators */
    voice_hooks_configure(DCP_VOICE_ENGLISH, kAll);
    CHECK(NarratorIndex(0) == 1);
    for (int c = 0; c < 6; ++c) CHECK(NarratorIndex(2 + c) == 8 + c);

    Frame odd = {0};
    odd.regs.rbx = 17;
    voice_narrator_bank_hook(&odd.regs);
    CHECK(odd.regs.rbx == 17);
    odd.regs.rbx = (uint64_t)DCP_NARRATOR_BANKS * 16;
    voice_narrator_bank_hook(&odd.regs);
    CHECK(odd.regs.rbx == (uint64_t)DCP_NARRATOR_BANKS * 16);

    voice_hooks_configure(DCP_VOICE_JAPANESE, kAll & ~DCP_VOICE_CATEGORY_BIT(DCP_VOICE_NARRATOR));
    CHECK(NarratorIndex(0) == 0 && NarratorBase(120000) == 120000);

    uint32_t broken[DCP_NARRATOR_BANKS];
    memcpy(broken, kUsNarrators, sizeof(broken));
    broken[13] = 90000;
    CHECK(voice_narrator_prepare(broken) == 0 && voice_narrator_target(0) == -1);
    voice_hooks_configure(DCP_VOICE_JAPANESE, kAll);
    CHECK((voice_hooks_routed() & DCP_VOICE_CATEGORY_BIT(DCP_VOICE_NARRATOR)) == 0 && NarratorBase(120000) == 120000);
    broken[13] = 80000;
    broken[12] = 80000;
    CHECK(voice_narrator_prepare(broken) == 0);
    CHECK(voice_narrator_prepare(NULL) == 0);
    voice_hooks_configure(DCP_VOICE_DEFAULT, kAll);
}

static int acquire_calls;
static char acquired_names[64][40];
static uint32_t fake_refs[2][VOICE_PIN_BANKS];

static void *GUEST_HOOK_ABI FakeAcquire(void *manager, const char *name) {
    (void)manager;
    if (acquire_calls < 64) snprintf(acquired_names[acquire_calls], sizeof(acquired_names[0]), "%s", name);
    ++acquire_calls;
    int english = 0;
    const int index = voice_pin_target_index(name, &english);
    if (index < 0) return NULL;
    ++fake_refs[english][index];
    return &fake_refs[english][index];
}

static void *GUEST_HOOK_ABI FailingAcquire(void *manager, const char *name) {
    (void)manager;
    (void)name;
    ++acquire_calls;
    return NULL;
}

static void TestPinNames(void) {
    CHECK(strcmp(voice_pin_bank_name(0, 0), "snd/se_gimmick10000_v.acb") == 0);
    CHECK(strcmp(voice_pin_bank_name(0, 1), "snd/se_gimmick110000_v.acb") == 0);
    CHECK(strcmp(voice_pin_bank_name(6, 0), "snd/se_enemy7100_v.acb") == 0);
    CHECK(strcmp(voice_pin_bank_name(8, 1), "snd/se_enemy108800_v.acb") == 0);
    CHECK(voice_pin_bank_name(-1, 0) == NULL && voice_pin_bank_name(VOICE_PIN_BANKS, 1) == NULL);
    for (int i = 0; i < VOICE_PIN_BANKS; ++i) {
        for (int english = 0; english < 2; ++english) {
            int is_english = -1;
            CHECK(voice_pin_target_index(voice_pin_bank_name(i, english), &is_english) == i && is_english == english);
        }
    }
    CHECK(voice_pin_target_index("SND/SE_GIMMICK110200_V.ACB", NULL) == 1);
    const char *others[] = {"snd/se_gimmick10000.acb", "snd/se_gimmick10100_v.acb", "snd/se_voice2900.acb",
                            "snd/narration_gm20000.acb", "snd/se_enemy7100.acb", "snd/se_gimmick10000_v.acb.bak",
                            "snd/se_enemy10000_v.acb", "", NULL};
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); ++i) {
        CHECK(voice_pin_target_index(others[i], NULL) == -1);
    }
}

/* One reference per family in the chosen language, taken once, never for player or narrator banks. */
static void TestBankLifetime(void) {
    int manager = 0;
    for (int language = DCP_VOICE_ENGLISH; language <= DCP_VOICE_JAPANESE; ++language) {
        const int english = language == DCP_VOICE_ENGLISH;
        voice_pin_reset();
        memset(fake_refs, 0, sizeof(fake_refs));
        acquire_calls = 0;
        voice_hooks_configure(language, kAll);
        CHECK(voice_pin_bank(-1, &manager, FakeAcquire) == 0 && voice_pin_bank(0, NULL, FakeAcquire) == 0 &&
              voice_pin_bank(0, &manager, NULL) == 0 && acquire_calls == 0);
        for (int i = 0; i < VOICE_PIN_BANKS; ++i) {
            CHECK(voice_pin_bank(i, &manager, FakeAcquire) == 1);
            for (int again = 0; again < 5; ++again) CHECK(voice_pin_bank(i, &manager, FakeAcquire) == 0);
        }
        CHECK(acquire_calls == VOICE_PIN_BANKS && voice_pinned_mask() == (UINT64_C(1) << VOICE_PIN_BANKS) - 1);
        for (int i = 0; i < VOICE_PIN_BANKS; ++i) CHECK(fake_refs[english][i] == 1 && fake_refs[!english][i] == 0);
        CHECK(strcmp(acquired_names[0], voice_pin_bank_name(0, english)) == 0);
    }

    voice_pin_reset();
    acquire_calls = 0;
    voice_hooks_configure(DCP_VOICE_JAPANESE, DCP_VOICE_CATEGORY_BIT(DCP_VOICE_NPC));
    CHECK(voice_pin_bank(VOICE_PIN_NPC_BANKS, &manager, FakeAcquire) == 0); /* enemy banks absent */
    CHECK(voice_pin_bank(0, &manager, FailingAcquire) == 0 && voice_pinned_mask() == 0);
    CHECK(voice_pin_bank(0, &manager, FakeAcquire) == 1 && voice_pinned_mask() == 1);

    voice_pin_reset();
    acquire_calls = 0;
    voice_hooks_configure(DCP_VOICE_DEFAULT, kAll);
    CHECK(voice_pin_bank(0, &manager, FakeAcquire) == 0 && acquire_calls == 0);

    char name[] = "snd/se_gimmick110200_v.acb";
    voice_hooks_configure(DCP_VOICE_JAPANESE, kAll);
    Frame frame = {0};
    frame.regs.rsi = (uint64_t)(uintptr_t)name;
    frame.regs.rdi = (uint64_t)(uintptr_t)&manager;
    const Frame before = frame;
    voice_res_acquire_hook(&frame.regs); /* no installed image: no acquire to call */
    CHECK(memcmp(&frame, &before, sizeof(frame)) == 0 && voice_pinned_mask() == 0);
    voice_pin_reset();
    voice_hooks_configure(DCP_VOICE_DEFAULT, kAll);
}

static void TestTelemetryCounts(void) {
    voice_telemetry_reset();
    voice_hooks_configure(DCP_VOICE_JAPANESE, kAll);
    for (int i = 0; i < 12; ++i) Spoken(1000, 1);
    Spoken(10043, 1);
    Spoken(90900, 1);
    Spoken(108, 1);
    CHECK(voice_narrator_prepare(kUsNarrators) == 1);
    NarratorBase(120000);
    CHECK(voice_telemetry_count(DCP_VOICE_PLAYER) == 12 && voice_telemetry_count(DCP_VOICE_NPC) == 1 &&
          voice_telemetry_count(DCP_VOICE_ENEMY) == 1 && voice_telemetry_count(DCP_VOICE_NARRATOR) == 1 &&
          voice_telemetry_count(DCP_VOICE_NONE) == 0 && voice_telemetry_count(99) == 0);
    voice_telemetry_reset();
    CHECK(voice_telemetry_count(DCP_VOICE_PLAYER) == 0);
    voice_hooks_configure(DCP_VOICE_DEFAULT, kAll);
}

/* A synthetic image with the located sites: the mapper's seven callers, the acquire and the narrator sites and table. */
enum {
    kSize = 0x40000,
    kMapper = 0x1000,
    kAcquire = 0x3000,
    kBank = 0x4000,
    kBase = 0x5000,
    kHelper = 0x6000,
    kTable = 0x20000,
    kNames = 0x21000,
};
static const uint32_t kCalls[DCP_VOICE_CUE_CALLS] = {0x2000, 0x2100, 0x2200, 0x2300, 0x2400, 0x2500, 0x2600};

static void BuildImage(uint8_t *image, const uint32_t numbers[DCP_NARRATOR_BANKS]) {
    memset(image, 0x90, kSize);
    for (int i = 0; i < DCP_VOICE_CUE_CALLS; ++i) {
        const int32_t rel = (int32_t)kMapper - (int32_t)(kCalls[i] + 5);
        image[kCalls[i]] = i == DCP_VOICE_CUE_TAIL_JUMP ? 0xe9 : 0xe8;
        memcpy(image + kCalls[i] + 1, &rel, 4);
    }
    memcpy(image + kAcquire, voice_res_acquire_site, sizeof(voice_res_acquire_site));
    memcpy(image + kBank, voice_narrator_bank_site, sizeof(voice_narrator_bank_site));
    memcpy(image + kBase, voice_narrator_base_site, sizeof(voice_narrator_base_site));
    memcpy(image + kHelper, voice_narrator_helper_site, sizeof(voice_narrator_helper_site));
    for (int i = 0; i < DCP_NARRATOR_BANKS; ++i) {
        const uint64_t acb = kNames + 64 * (uint64_t)i, awb = acb + 32;
        snprintf((char *)image + acb, 32, "SND/NARRATION_GM%u.ACB", (unsigned)numbers[i]);
        snprintf((char *)image + awb, 32, "SND/NARRATION_GM%u.AWB", (unsigned)numbers[i]);
        memcpy(image + kTable + 16 * i, &acb, 8);
        memcpy(image + kTable + 16 * i + 8, &awb, 8);
    }
}

static DcpVoiceSites Sites(void) {
    DcpVoiceSites sites = {0};
    sites.cue_mapper = kMapper;
    memcpy(sites.cue_calls, kCalls, sizeof(kCalls));
    sites.resource_acquire = kAcquire;
    sites.narrator_bank_select = kBank;
    sites.narrator_cue_base = kBase;
    sites.narrator_cue_helper = kHelper;
    sites.narrator_banks = kTable;
    return sites;
}

static int JumpsTo(const uint8_t *image, uint32_t at, const uint8_t *stub) {
    int32_t rel = 0;
    memcpy(&rel, image + at + 1, 4);
    return image[at] == 0xe9 && image + at + 5 + rel == stub;
}

static void TestInstall(void) {
    const size_t stub = voice_hooks_stub_bytes();
    const size_t stride = guest_hook_stub_size();
    CHECK(VOICE_HOOK_COUNT == DCP_VOICE_CUE_CALLS + 4 && stub == VOICE_HOOK_COUNT * stride);
    uint8_t *image = malloc(kSize + stub);
    uint8_t *copy = malloc(kSize + stub);
    CHECK(image != NULL && copy != NULL);
    if (!image || !copy) {
        free(image);
        free(copy);
        return;
    }
    const DcpVoiceSites sites = Sites();

    BuildImage(image, kUsNarrators);
    voice_hooks_configure(DCP_VOICE_JAPANESE, kAll);
    CHECK(voice_hooks_install(image, kSize, 0x1000, &sites, image + kSize, stub) == 1);
    for (int i = 0; i < DCP_VOICE_CUE_CALLS; ++i) CHECK(JumpsTo(image, kCalls[i], image + kSize + i * stride));
    CHECK(JumpsTo(image, kAcquire, image + kSize + DCP_VOICE_CUE_CALLS * stride));
    CHECK(JumpsTo(image, kBank, image + kSize + (DCP_VOICE_CUE_CALLS + 1) * stride));
    CHECK(JumpsTo(image, kBase, image + kSize + (DCP_VOICE_CUE_CALLS + 2) * stride));
    CHECK(JumpsTo(image, kHelper, image + kSize + (DCP_VOICE_CUE_CALLS + 3) * stride));
    CHECK(voice_hooks_routed() == kAll && voice_narrator_target(0) == 1);
    voice_hooks_log_ready("CUSA10454-1.00", DCP_VOICE_JAPANESE);

    /* The table as the loader leaves it, every entry a loaded address. */
    BuildImage(copy, kUsNarrators);
    for (int i = 0; i < 2 * DCP_NARRATOR_BANKS; ++i) {
        uint64_t entry = 0;
        memcpy(&entry, copy + kTable + 8 * i, 8);
        entry += (uint64_t)(uintptr_t)copy;
        memcpy(copy + kTable + 8 * i, &entry, 8);
    }
    voice_hooks_configure(DCP_VOICE_JAPANESE, kAll);
    CHECK(voice_hooks_install(copy, kSize, 0x1000, &sites, copy + kSize, stub) == 1);
    CHECK(voice_hooks_routed() == kAll && voice_narrator_target(0) == 1 && JumpsTo(copy, kBank, copy + kSize + (DCP_VOICE_CUE_CALLS + 1) * stride));

    /* A narrator the locator turned off, or a table that is not the verified one: the other routes install, the narrator
     * sites stay as they were. */
    const unsigned no_narrator = kAll & ~DCP_VOICE_CATEGORY_BIT(DCP_VOICE_NARRATOR);
    uint32_t broken[DCP_NARRATOR_BANKS];
    memcpy(broken, kUsNarrators, sizeof(broken));
    broken[9] = 90000;
    DcpVoiceSites off = sites;
    off.narrator_route = DCP_NARRATOR_ROUTE_NAMES;
    off.narrator_bank_select = off.narrator_cue_base = off.narrator_cue_helper = off.narrator_banks = 0;
    snprintf(off.narrator_failure, sizeof(off.narrator_failure), "relation narrator banks: not a narrator ACB/AWB name pair");
    DcpVoiceSites outside = sites;
    outside.narrator_banks = kSize - 8;
    struct {
        const uint32_t *numbers;
        const DcpVoiceSites *sites;
    } partial[] = {{broken, &sites}, {kUsNarrators, &off}, {kUsNarrators, &outside}};
    for (size_t k = 0; k < sizeof(partial) / sizeof(partial[0]); ++k) {
        BuildImage(copy, partial[k].numbers);
        memcpy(image, copy, kSize);
        voice_hooks_configure(DCP_VOICE_JAPANESE, kAll);
        CHECK(voice_hooks_install(copy, kSize, 0x1000, partial[k].sites, copy + kSize, stub) == 1);
        for (int i = 0; i < DCP_VOICE_CUE_CALLS; ++i) CHECK(JumpsTo(copy, kCalls[i], copy + kSize + i * stride));
        CHECK(JumpsTo(copy, kAcquire, copy + kSize + DCP_VOICE_CUE_CALLS * stride));
        CHECK(!memcmp(copy + kBank, image + kBank, VOICE_NARRATOR_BANK_SITE_LENGTH) &&
              !memcmp(copy + kBase, image + kBase, VOICE_NARRATOR_BASE_SITE_LENGTH) &&
              !memcmp(copy + kHelper, image + kHelper, VOICE_NARRATOR_HELPER_SITE_LENGTH));
        CHECK(voice_hooks_routed() == no_narrator && voice_narrator_target(0) == -1);
        CHECK(Spoken(1000, DCP_VOICE_MODE_ENGLISH) == 1000 && Spoken(10043, DCP_VOICE_MODE_ENGLISH) == 10043 &&
              Spoken(kDcpEnemyVoiceFamilies[0], DCP_VOICE_MODE_ENGLISH) == kDcpEnemyVoiceFamilies[0]);
        voice_hooks_log_ready("CUSA10454-1.00", DCP_VOICE_JAPANESE);
    }

    /* Each rejected case leaves the image as it was. */
    voice_hooks_configure(DCP_VOICE_JAPANESE, kAll);
    BuildImage(copy, kUsNarrators);
    copy[kCalls[DCP_VOICE_CUE_TAIL_JUMP]] = 0xe8;
    memcpy(image, copy, kSize);
    CHECK(voice_hooks_install(copy, kSize, 0x1000, &sites, copy + kSize, stub) == 0 && !memcmp(copy, image, kSize));

    BuildImage(copy, kUsNarrators);
    copy[kBase + 3] ^= 0xff;
    memcpy(image, copy, kSize);
    CHECK(voice_hooks_install(copy, kSize, 0x1000, &sites, copy + kSize, stub) == 0 && !memcmp(copy, image, kSize));

    BuildImage(copy, kUsNarrators);
    memcpy(image, copy, kSize);
    CHECK(voice_hooks_install(copy, kSize, 0x1000, NULL, copy + kSize, stub) == 0 && !memcmp(copy, image, kSize));
    CHECK(voice_hooks_install(copy, kSize, 0x1000, &sites, copy + kSize, stub - 1) == 0 && !memcmp(copy, image, kSize));
    voice_hooks_log_ready(NULL, DCP_VOICE_JAPANESE);

    free(image);
    free(copy);
    voice_hooks_configure(DCP_VOICE_DEFAULT, kAll);
}

int main(void) {
    TestSettingAndMigration();
    TestCueFamilies();
    TestOldGapIssue69();
    TestPlayerBothDirections();
    TestSameVoicePreserved();
    TestResurrectedAndPartyPaths();
    TestNpcEnemyAllyRoutes();
    TestOtherCuesUntouched();
    TestGameDefaultIsVanilla();
    TestMissingAlternateBank();
    TestNarrator();
    TestPinNames();
    TestBankLifetime();
    TestTelemetryCounts();
    TestInstall();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
