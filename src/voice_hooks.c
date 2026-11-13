#include "voice_hooks.h"

#include <stdio.h>
#include <string.h>

#include "dcp_voice.h"

const uint8_t voice_npc_site[VOICE_NPC_SITE_LENGTH] = {0xb8, 0x1b, 0x97, 0x01, 0x00};
const uint8_t voice_res_acquire_site[VOICE_RES_ACQUIRE_SITE_LENGTH] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};

static const GuestImageId kImage = {0x11f8060, 0x54c690};
static int enabled, enemy_banks;
static const uint8_t *image_base;

static uint32_t resource_acquire_offset = VOICE_RES_ACQUIRE_SITE_OFFSET;

static const char *const kPinNames[VOICE_PIN_BANKS] = {
    "snd/se_gimmick10000_v.acb", "snd/se_gimmick10200_v.acb", "snd/se_gimmick10400_v.acb",
    "snd/se_gimmick10500_v.acb", "snd/se_gimmick10600_v.acb", "snd/se_gimmick10800_v.acb",
    "snd/se_enemy7100_v.acb",    "snd/se_enemy7200_v.acb",    "snd/se_enemy8800_v.acb",
    "snd/se_enemy3000_v.acb", "snd/se_enemy3100_v.acb", "snd/se_enemy3300_v.acb",
    "snd/se_enemy3400_v.acb", "snd/se_enemy3700_v.acb", "snd/se_enemy3800_v.acb",
    "snd/se_enemy4200_v.acb", "snd/se_enemy4300_v.acb", "snd/se_enemy4500_v.acb",
    "snd/se_enemy4700_v.acb", "snd/se_enemy5600_v.acb", "snd/se_enemy5700_v.acb",
    "snd/se_enemy5800_v.acb", "snd/se_enemy6200_v.acb", "snd/se_enemy7300_v.acb",
    "snd/se_enemy7400_v.acb", "snd/se_enemy7900_v.acb", "snd/se_enemy8200_v.acb",
    "snd/se_enemy8300_v.acb", "snd/se_enemy8400_v.acb", "snd/se_enemy8500_v.acb",
    "snd/se_enemy8600_v.acb", "snd/se_enemy8700_v.acb", "snd/se_enemy90000_v.acb",
    "snd/se_enemy90100_v.acb", "snd/se_enemy90200_v.acb", "snd/se_enemy90300_v.acb",
    "snd/se_enemy90400_v.acb", "snd/se_enemy90500_v.acb", "snd/se_enemy90600_v.acb",
    "snd/se_enemy90700_v.acb", "snd/se_enemy90800_v.acb", "snd/se_enemy90900_v.acb",
    "snd/se_enemy91000_v.acb", "snd/se_enemy91100_v.acb", "snd/se_enemy91200_v.acb",
    "snd/se_enemy91300_v.acb", "snd/se_enemy91400_v.acb", "snd/se_enemy91500_v.acb",
    "snd/se_enemy91600_v.acb",
};
enum { kPinGimmickBanks = 6 };
static const unsigned kPinNumbers[VOICE_PIN_BANKS] = {
    10000, 10200, 10400, 10500, 10600, 10800, 7100, 7200, 8800,
    3000, 3100, 3300, 3400, 3700, 3800, 4200, 4300, 4500, 4700, 5600, 5700,
    5800, 6200, 7300, 7400, 7900, 8200, 8300, 8400, 8500, 8600, 8700, 90000,
    90100, 90200, 90300, 90400, 90500, 90600, 90700, 90800, 90900, 91000, 91100, 91200,
    91300, 91400, 91500, 91600,
};
static int pinned_bank[VOICE_PIN_BANKS];
static int pinning;

static int ci_prefix(const char *text, const char *prefix) {
    for (; *prefix; ++text, ++prefix) {
        char a = *text, b = *prefix;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}

const char *voice_pin_bank_name(int index) {
    return index >= 0 && index < VOICE_PIN_BANKS ? kPinNames[index] : NULL;
}

/* Matches a bank name against the Japanese or English (+100000, "11" prefix on the family number) name of one of
 * the pinned families, case-insensitively; -1 when it is neither. */
int voice_pin_target_index(const char *name, int *english) {
    static const char tail[] = "_v.acb";
    static const char *const prefixes[] = {"se_gimmick", "se_enemy"};
    if (english) *english = 0;
    if (!name) return -1;
    const char *at = NULL;
    const char *prefix = NULL;
    for (const char *c = name; *c; ++c) {
        for (size_t k = 0; k < sizeof(prefixes) / sizeof(prefixes[0]); ++k) {
            if (ci_prefix(c, prefixes[k])) {
                at = c;
                prefix = prefixes[k];
            }
        }
    }
    if (!at) return -1;
    const char *digits = at + strlen(prefix);
    unsigned number = 0;
    int count = 0;
    while (digits[count] >= '0' && digits[count] <= '9' && count < 7) {
        number = number * 10 + (unsigned)(digits[count] - '0');
        ++count;
    }
    if (!count || !ci_prefix(digits + count, tail) || digits[count + sizeof(tail) - 1]) return -1;
    const int is_english = number >= 100000;
    if (is_english) number -= 100000;
    for (int i = 0; i < VOICE_PIN_BANKS; ++i) {
        if (kPinNumbers[i] == number && !strcmp(i < kPinGimmickBanks ? "se_gimmick" : "se_enemy", prefix)) {
            if (english) *english = is_english;
            return i;
        }
    }
    return -1;
}

uint64_t voice_pinned_mask(void) {
    uint64_t mask = 0;
    for (int i = 0; i < VOICE_PIN_BANKS; ++i) {
        if (pinned_bank[i]) mask |= UINT64_C(1) << i;
    }
    return mask;
}

void voice_pin_reset(void) {
    memset(pinned_bank, 0, sizeof(pinned_bank));
    pinning = 0;
}

int voice_pin_bank(int index, void *manager, ResourceAcquire acquire) {
    if (!enabled || !acquire || !manager || index < 0 || index >= VOICE_PIN_BANKS || pinned_bank[index]) return 0;
    if (index >= VOICE_PIN_NPC_BANKS && !enemy_banks) return 0;
    pinned_bank[index] = 1;
    pinning = 1;
    void *resource = acquire(manager, kPinNames[index]);
    pinning = 0;
    if (!resource) pinned_bank[index] = 0;
    else printf("Japanese NPC Voices: keeping %s loaded\n", kPinNames[index]);
    return resource != NULL;
}

void voice_hooks_set_enabled(int value) {
    enabled = value != 0;
}

void voice_hooks_set_enemy_banks(int present) {
    enemy_banks = present != 0;
}

int voice_hooks_enabled(void) {
    return enabled;
}

/* On the US-only path of the cue mapper, before `mov eax, 0x1971b`: returning the cue unchanged and skipping the rest
 * of the mapper leaves the NPC cue on its Japanese ID. */
void GUEST_HOOK_ABI voice_npc_hook(GuestRegs *regs) {
    const uint32_t cue = (uint32_t)regs->rdi;
    if (!enabled || !(dcp_is_japanese_npc_cue(cue) || (enemy_banks && dcp_is_japanese_enemy_cue(cue)))) return;
    regs->rax = regs->rdi;
    regs->skip = 1;
}

/* At the entry of the resource acquire (rdi = manager, rsi = name): a request for either language's name of a
 * pinned family also acquires the Japanese one, through the same acquire, so it stays referenced once the mapper
 * starts asking for it. */
void GUEST_HOOK_ABI voice_res_acquire_hook(GuestRegs *regs) {
    if (pinning) return;
    const char *name = (const char *)(uintptr_t)regs->rsi;
    if (!name) return;
    const int index = voice_pin_target_index(name, NULL);
    if (index < 0) return;
    const ResourceAcquire acquire =
        image_base ? (ResourceAcquire)(uintptr_t)(image_base + resource_acquire_offset) : NULL;
    voice_pin_bank(index, (void *)(uintptr_t)regs->rdi, acquire);
}

size_t voice_hooks_stub_bytes(void) {
    return VOICE_HOOK_COUNT * guest_hook_stub_size();
}

static int install_at(uint8_t *image, uint64_t size, uint64_t entry, const GuestImageId *id, uint32_t cue_offset,
                      uint32_t acquire_offset, uint8_t *stubs, size_t stubs_size) {
    GuestHook hooks[VOICE_HOOK_COUNT] = {
        {"japanese npc voices", cue_offset, VOICE_NPC_SITE_LENGTH, {0}, voice_npc_hook},
        {"japanese npc voices bank pin", acquire_offset, VOICE_RES_ACQUIRE_SITE_LENGTH, {0}, voice_res_acquire_hook},
    };
    memcpy(hooks[0].original, voice_npc_site, sizeof(voice_npc_site));
    memcpy(hooks[1].original, voice_res_acquire_site, sizeof(voice_res_acquire_site));
    size_t bad = 0;
    const GuestHookStatus status =
        guest_hooks_install(image, size, entry, id, hooks, VOICE_HOOK_COUNT, stubs, stubs_size, &bad);
    if (status != GUEST_HOOK_OK) {
        printf("Japanese NPC Voices: hooks not installed: %s (hook %zu: %s)\n", guest_hook_status_text(status), bad,
               hooks[bad].name);
        return 0;
    }
    image_base = image;
    resource_acquire_offset = acquire_offset;
    return 1;
}

int voice_hooks_install(uint8_t *image, uint64_t size, uint64_t entry, uint8_t *stubs, size_t stubs_size) {
    if (size != kImage.size || entry != kImage.entry) {
        printf("Japanese NPC Voices: hook not installed: not the CUSA10454 1.00 image\n");
        return 0;
    }
    if (!install_at(image, size, entry, &kImage, VOICE_NPC_SITE_OFFSET, VOICE_RES_ACQUIRE_SITE_OFFSET, stubs, stubs_size)) {
        return 0;
    }
    printf("Japanese NPC Voices: hooks installed at 0x68bc9, 0x57cbb0\n");
    return 1;
}

int voice_hooks_install_located(uint8_t *image, uint64_t size, uint64_t entry, const DcpVoiceSites *sites, uint8_t *stubs,
                                size_t stubs_size) {
    const GuestImageId id = {size, entry};
    if (!sites || !install_at(image, size, entry, &id, sites->npc_cue, sites->resource_acquire, stubs, stubs_size)) {
        return 0;
    }
    printf("Japanese NPC Voices: hooks found by signature\n");
    return 1;
}
