#include "voice_hooks.h"

#include <stdio.h>
#include <string.h>

const uint8_t voice_res_acquire_site[VOICE_RES_ACQUIRE_SITE_LENGTH] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
const uint8_t voice_narrator_bank_site[VOICE_NARRATOR_BANK_SITE_LENGTH] = {0x48, 0x8b, 0x34, 0x03, 0x4c,
                                                                           0x8b, 0x74, 0x03, 0x08};
const uint8_t voice_narrator_base_site[VOICE_NARRATOR_BASE_SITE_LENGTH] = {0x44, 0x8d, 0xb4, 0x08,
                                                                           0xe0, 0xb1, 0xff, 0xff};
const uint8_t voice_narrator_helper_site[VOICE_NARRATOR_HELPER_SITE_LENGTH] = {0x8d, 0x84, 0x08, 0xe0,
                                                                               0xb1, 0xff, 0xff};

static int language = DCP_VOICE_DEFAULT;
static unsigned available;
static int installed;
static const uint8_t *image_base;
static uint32_t resource_acquire_offset;

static uint32_t narrator_numbers[DCP_NARRATOR_BANKS];
static int narrator_prepared;
static int narrator_reason;

static const char *mode_language(int mode) {
    return mode == DCP_VOICE_MODE_ENGLISH ? "english" : "japanese";
}

void voice_hooks_configure(int requested, unsigned categories) {
    language = requested == DCP_VOICE_ENGLISH || requested == DCP_VOICE_JAPANESE ? requested : DCP_VOICE_DEFAULT;
    available = language == DCP_VOICE_DEFAULT ? 0 : categories & DCP_VOICE_ALL_CATEGORIES;
    installed = 0;
    narrator_reason = DCP_NARRATOR_ROUTE_OK;
}

int voice_hooks_language(void) {
    return language;
}

unsigned voice_hooks_routed(void) {
    unsigned routed = available;
    if (!narrator_prepared) routed &= ~DCP_VOICE_CATEGORY_BIT(DCP_VOICE_NARRATOR);
    return routed;
}

static int routes(int category) {
    return category != DCP_VOICE_NONE && (voice_hooks_routed() & DCP_VOICE_CATEGORY_BIT(category)) != 0;
}

/* Telemetry: the first routed voice of each category and every tenfold count after it, and the first switch of each
 * bank family. */
enum { kFamilySlots = DCP_PLAYER_VOICE_FAMILY_COUNT + DCP_CAMP_VOICE_FAMILY_COUNT + DCP_GIMMICK_VOICE_FAMILY_COUNT +
                      DCP_NPC_ENEMY_BANK_FAMILY_COUNT + DCP_ENEMY_VOICE_FAMILY_COUNT + DCP_NARRATOR_COUNT };
static uint32_t route_counts[DCP_VOICE_CATEGORY_COUNT];
static uint8_t family_switched[kFamilySlots];

static int slot_in(const uint32_t *families, int count, uint32_t family, int first) {
    for (int i = 0; i < count; ++i) {
        if (families[i] == family) return first + i;
    }
    return -1;
}

static int family_slot(uint32_t family) {
    int first = 0, slot;
    if ((slot = slot_in(kDcpPlayerVoiceFamilies, DCP_PLAYER_VOICE_FAMILY_COUNT, family, first)) >= 0) return slot;
    first += DCP_PLAYER_VOICE_FAMILY_COUNT;
    if ((slot = slot_in(kDcpCampVoiceFamilies, DCP_CAMP_VOICE_FAMILY_COUNT, family, first)) >= 0) return slot;
    first += DCP_CAMP_VOICE_FAMILY_COUNT;
    if ((slot = slot_in(kDcpGimmickVoiceFamilies, DCP_GIMMICK_VOICE_FAMILY_COUNT, family, first)) >= 0) return slot;
    first += DCP_GIMMICK_VOICE_FAMILY_COUNT;
    if ((slot = slot_in(kDcpNpcEnemyBankFamilies, DCP_NPC_ENEMY_BANK_FAMILY_COUNT, family, first)) >= 0) return slot;
    first += DCP_NPC_ENEMY_BANK_FAMILY_COUNT;
    if ((slot = slot_in(kDcpEnemyVoiceFamilies, DCP_ENEMY_VOICE_FAMILY_COUNT, family, first)) >= 0) return slot;
    first += DCP_ENEMY_VOICE_FAMILY_COUNT;
    if (dcp_narrator_base_valid(family)) {
        const uint32_t japanese = dcp_narrator_base_for(family, DCP_VOICE_JAPANESE);
        return first + (int)((japanese - DCP_NARRATOR_FIRST_BASE) / DCP_NARRATOR_BASE_STEP);
    }
    return -1;
}

static void note_route(int category) {
    const uint32_t count = __atomic_add_fetch(&route_counts[category], 1, __ATOMIC_RELAXED);
    uint32_t mark = 1;
    while (mark < count && mark <= UINT32_MAX / 10) mark *= 10;
    if (mark == count) {
        printf("Voice telemetry: event=route_used category=%s language=%s count=%u\n", dcp_voice_category_name(category),
               dcp_voice_language_key(language), count);
    }
}

static void note_switch(int category, uint32_t family, const char *from) {
    const int slot = family_slot(family);
    if (slot < 0 || __atomic_exchange_n(&family_switched[slot], 1, __ATOMIC_RELAXED)) return;
    printf("Voice telemetry: event=bank_route category=%s family=%u from=%s to=%s count=1\n",
           dcp_voice_category_name(category), family, from, dcp_voice_language_key(language));
}

uint32_t voice_telemetry_count(int category) {
    return category >= 0 && category < DCP_VOICE_CATEGORY_COUNT ? __atomic_load_n(&route_counts[category], __ATOMIC_RELAXED)
                                                                : 0;
}

void voice_telemetry_reset(void) {
    memset(route_counts, 0, sizeof(route_counts));
    memset(family_switched, 0, sizeof(family_switched));
}

void voice_hooks_log_ready(const char *build_key, int requested) {
    const unsigned routed = installed ? voice_hooks_routed() : 0;
    const char *yes[2] = {"no", "yes"};
    printf("Voice telemetry: route_ready build=%s requested=%s effective=%s player=%s npc=%s enemy=%s narrator=%s\n",
           build_key && *build_key ? build_key : "unknown", dcp_voice_language_key(requested),
           dcp_voice_language_key(routed ? language : DCP_VOICE_DEFAULT),
           yes[(routed & DCP_VOICE_CATEGORY_BIT(DCP_VOICE_PLAYER)) != 0],
           yes[(routed & DCP_VOICE_CATEGORY_BIT(DCP_VOICE_NPC)) != 0],
           yes[(routed & DCP_VOICE_CATEGORY_BIT(DCP_VOICE_ENEMY)) != 0],
           yes[(routed & DCP_VOICE_CATEGORY_BIT(DCP_VOICE_NARRATOR)) != 0]);
    if (!installed) return;
    for (int category = DCP_VOICE_PLAYER; category < DCP_VOICE_CATEGORY_COUNT; ++category) {
        if (routed & DCP_VOICE_CATEGORY_BIT(category)) continue;
        const char *reason = !(available & DCP_VOICE_CATEGORY_BIT(category)) ? "banks"
                             : category == DCP_VOICE_NARRATOR            ? dcp_narrator_route_reason(narrator_reason)
                                                                         : "hooks";
        printf("Voice telemetry: capability_unavailable category=%s reason=%s count=1\n", dcp_voice_category_name(category),
               reason);
    }
}

/* At a call or tail jump to the cue mapper (edi = cue, esi = mode): the voice's own cue in the chosen language. */
void GUEST_HOOK_ABI voice_cue_call_hook(GuestRegs *regs) {
    const uint32_t cue = (uint32_t)regs->rdi;
    const int category = dcp_voice_cue_category(cue);
    if (!routes(category)) return;
    const int native = (uint32_t)regs->rsi == DCP_VOICE_MODE_ENGLISH ? DCP_VOICE_MODE_ENGLISH : DCP_VOICE_MODE_JAPANESE;
    const int wanted = dcp_voice_mode_of(language);
    regs->rsi = (uint32_t)wanted;
    note_route(category);
    if (native != wanted) note_switch(category, dcp_voice_cue_family(cue), mode_language(native));
}

int voice_narrator_prepare(const uint32_t numbers[DCP_NARRATOR_BANKS]) {
    narrator_prepared = 0;
    if (!numbers) return 0;
    for (int i = 0; i < DCP_NARRATOR_BANKS; ++i) {
        if (!dcp_narrator_base_valid(numbers[i])) return 0;
        for (int other = DCP_VOICE_ENGLISH; other <= DCP_VOICE_JAPANESE; ++other) {
            const uint32_t twin = dcp_narrator_base_for(numbers[i], other);
            int found = 0;
            for (int j = 0; j < DCP_NARRATOR_BANKS; ++j) found += numbers[j] == twin;
            if (found != 1) return 0;
        }
    }
    memcpy(narrator_numbers, numbers, sizeof(narrator_numbers));
    narrator_prepared = 1;
    return 1;
}

int voice_narrator_target(int index) {
    if (!narrator_prepared || index < 0 || index >= DCP_NARRATOR_BANKS) return -1;
    if (language == DCP_VOICE_DEFAULT) return index;
    const uint32_t wanted = dcp_narrator_base_for(narrator_numbers[index], language);
    for (int j = 0; j < DCP_NARRATOR_BANKS; ++j) {
        if (narrator_numbers[j] == wanted) return j;
    }
    return -1;
}

/* Before the narrator manager reads its bank's ACB and AWB names (rbx = narrator index * 16). */
void GUEST_HOOK_ABI voice_narrator_bank_hook(GuestRegs *regs) {
    if (!routes(DCP_VOICE_NARRATOR) || regs->rbx % 16) return;
    const uint64_t index = regs->rbx / 16;
    if (index >= DCP_NARRATOR_BANKS) return;
    const int target = voice_narrator_target((int)index);
    if (target < 0 || target == (int)index) return;
    regs->rbx = (uint64_t)target * 16;
    note_switch(DCP_VOICE_NARRATOR, narrator_numbers[index],
                mode_language(narrator_numbers[index] >= DCP_VOICE_ENGLISH_OFFSET ? DCP_VOICE_MODE_ENGLISH
                                                                                  : DCP_VOICE_MODE_JAPANESE));
}

/* Before a narration cue is formed from the narrator's cue base (ecx): the same narrator's base in the chosen language,
 * which the bank above holds. */
void GUEST_HOOK_ABI voice_narrator_base_hook(GuestRegs *regs) {
    if (!routes(DCP_VOICE_NARRATOR)) return;
    const uint32_t base = (uint32_t)regs->rcx;
    if (!dcp_narrator_base_valid(base)) return;
    regs->rcx = dcp_narrator_base_for(base, language);
    note_route(DCP_VOICE_NARRATOR);
}

/* The NPC and enemy families whose chosen-language bank is pinned, the gimmick banks first. */
enum { kPinGimmickBanks = DCP_GIMMICK_VOICE_FAMILY_COUNT };
static char pin_names[2][VOICE_PIN_BANKS][32];
static int pinned_bank[VOICE_PIN_BANKS];
static int pinning;

static uint32_t pin_number(int index) {
    if (index < kPinGimmickBanks) return kDcpGimmickVoiceFamilies[index];
    if (index < VOICE_PIN_NPC_BANKS) return kDcpNpcEnemyBankFamilies[index - kPinGimmickBanks];
    return kDcpEnemyVoiceFamilies[index - VOICE_PIN_NPC_BANKS];
}

static void pin_names_fill(void) {
    if (pin_names[0][0][0]) return;
    for (int english = 0; english < 2; ++english) {
        for (int i = 0; i < VOICE_PIN_BANKS; ++i) {
            snprintf(pin_names[english][i], sizeof(pin_names[english][i]), "snd/se_%s%u_v.acb",
                     i < kPinGimmickBanks ? "gimmick" : "enemy", pin_number(i) + (english ? DCP_VOICE_ENGLISH_OFFSET : 0));
        }
    }
}

const char *voice_pin_bank_name(int index, int english) {
    if (index < 0 || index >= VOICE_PIN_BANKS) return NULL;
    pin_names_fill();
    return pin_names[english != 0][index];
}

static int ci_prefix(const char *text, const char *prefix) {
    for (; *prefix; ++text, ++prefix) {
        char a = *text, b = *prefix;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}

/* Matches a bank name against either language's name of a pinned family, case-insensitively; -1 when it is neither. */
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
    const int is_english = number >= DCP_VOICE_ENGLISH_OFFSET;
    if (is_english) number -= DCP_VOICE_ENGLISH_OFFSET;
    for (int i = 0; i < VOICE_PIN_BANKS; ++i) {
        if (pin_number(i) == number && !strcmp(i < kPinGimmickBanks ? "se_gimmick" : "se_enemy", prefix)) {
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
    if (!acquire || !manager || index < 0 || index >= VOICE_PIN_BANKS || pinned_bank[index]) return 0;
    if (!routes(index < VOICE_PIN_NPC_BANKS ? DCP_VOICE_NPC : DCP_VOICE_ENEMY)) return 0;
    const char *name = voice_pin_bank_name(index, language == DCP_VOICE_ENGLISH);
    pinned_bank[index] = 1;
    pinning = 1;
    void *resource = acquire(manager, name);
    pinning = 0;
    if (!resource) pinned_bank[index] = 0;
    else printf("Voice Language: keeping %s loaded\n", name);
    return resource != NULL;
}

/* At the entry of the resource acquire (rdi = manager, rsi = name): a request for either language's bank of a pinned
 * family also takes a reference on the chosen language's bank, through the same acquire. */
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

/* The narrator numbers of the bank table at image offset `table`; its entries point to the names, relocated or not. */
static int read_narrator_numbers(const uint8_t *image, uint64_t size, uint32_t table, uint32_t numbers[DCP_NARRATOR_BANKS]) {
    static const char prefix[] = "SND/NARRATION_GM";
    if ((uint64_t)table + DCP_NARRATOR_BANKS * 16 > size) return 0;
    for (int i = 0; i < DCP_NARRATOR_BANKS; ++i) {
        uint64_t name = 0;
        memcpy(&name, image + table + 16 * i, sizeof(name));
        if (name >= (uint64_t)(uintptr_t)image && name < (uint64_t)(uintptr_t)image + size) name -= (uint64_t)(uintptr_t)image;
        if (name + sizeof(prefix) + 12 > size || memcmp(image + name, prefix, sizeof(prefix) - 1)) return 0;
        uint32_t number = 0;
        const char *digits = (const char *)image + name + sizeof(prefix) - 1;
        int count = 0;
        while (count < 7 && digits[count] >= '0' && digits[count] <= '9') number = number * 10 + (uint32_t)(digits[count++] - '0');
        if (!count || digits[count] != '.') return 0;
        numbers[i] = number;
    }
    return 1;
}

int voice_hooks_install(uint8_t *image, uint64_t size, uint64_t entry, const DcpVoiceSites *sites, uint8_t *stubs,
                        size_t stubs_size) {
    installed = 0;
    narrator_prepared = 0;
    if (!image || !sites) return 0;
    narrator_reason = sites->narrator_route;
    if (narrator_reason == DCP_NARRATOR_ROUTE_OK) {
        uint32_t numbers[DCP_NARRATOR_BANKS];
        if (!read_narrator_numbers(image, size, sites->narrator_banks, numbers) || !voice_narrator_prepare(numbers))
            narrator_reason = DCP_NARRATOR_ROUTE_NAMES;
    }
    const int narrator = narrator_reason == DCP_NARRATOR_ROUTE_OK;
    const size_t hook_count = narrator ? VOICE_HOOK_COUNT : DCP_VOICE_CUE_CALLS + 1;
    GuestHook hooks[VOICE_HOOK_COUNT];
    memset(hooks, 0, sizeof(hooks));
    for (int i = 0; i < DCP_VOICE_CUE_CALLS; ++i) {
        const uint32_t at = sites->cue_calls[i];
        if ((uint64_t)at + 5 > size || image[at] != (i == DCP_VOICE_CUE_TAIL_JUMP ? 0xe9 : 0xe8)) {
            printf("Voice Language: hooks not installed: cue mapper caller %d is not the located call\n", i);
            return 0;
        }
        hooks[i] = (GuestHook){"voice language cue call", at, 5, {0}, voice_cue_call_hook};
        memcpy(hooks[i].original, image + at, 5);
    }
    GuestHook *more = hooks + DCP_VOICE_CUE_CALLS;
    more[0] = (GuestHook){"voice language bank pin", sites->resource_acquire, VOICE_RES_ACQUIRE_SITE_LENGTH, {0},
                          voice_res_acquire_hook};
    memcpy(more[0].original, voice_res_acquire_site, sizeof(voice_res_acquire_site));
    more[1] = (GuestHook){"voice language narrator bank", sites->narrator_bank_select, VOICE_NARRATOR_BANK_SITE_LENGTH, {0},
                          voice_narrator_bank_hook};
    memcpy(more[1].original, voice_narrator_bank_site, sizeof(voice_narrator_bank_site));
    more[2] = (GuestHook){"voice language narrator cue", sites->narrator_cue_base, VOICE_NARRATOR_BASE_SITE_LENGTH, {0},
                          voice_narrator_base_hook};
    memcpy(more[2].original, voice_narrator_base_site, sizeof(voice_narrator_base_site));
    more[3] = (GuestHook){"voice language narrator preview cue", sites->narrator_cue_helper,
                          VOICE_NARRATOR_HELPER_SITE_LENGTH, {0}, voice_narrator_base_hook};
    memcpy(more[3].original, voice_narrator_helper_site, sizeof(voice_narrator_helper_site));

    const GuestImageId id = {size, entry};
    size_t bad = 0;
    const GuestHookStatus status = guest_hooks_install(image, size, entry, &id, hooks, hook_count, stubs, stubs_size, &bad);
    if (status != GUEST_HOOK_OK) {
        printf("Voice Language: hooks not installed: %s (hook %zu: %s)\n", guest_hook_status_text(status), bad,
               hooks[bad].name);
        narrator_prepared = 0;
        return 0;
    }
    image_base = image;
    resource_acquire_offset = sites->resource_acquire;
    installed = 1;
    if (narrator)
        printf("Voice Language: hooks installed at %d cue mapper callers (mapper 0x%x), bank pin 0x%x, narrator 0x%x, 0x%x, 0x%x\n",
               DCP_VOICE_CUE_CALLS, sites->cue_mapper, sites->resource_acquire, sites->narrator_bank_select,
               sites->narrator_cue_base, sites->narrator_cue_helper);
    else
        printf("Voice Language: hooks installed at %d cue mapper callers (mapper 0x%x), bank pin 0x%x; narrator off: %s\n",
               DCP_VOICE_CUE_CALLS, sites->cue_mapper, sites->resource_acquire,
               sites->narrator_failure[0] ? sites->narrator_failure : dcp_narrator_route_reason(narrator_reason));
    return 1;
}
