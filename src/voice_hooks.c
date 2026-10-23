#include "voice_hooks.h"

#include <stdio.h>
#include <string.h>

#include "dcp_voice.h"

const uint8_t voice_npc_site[VOICE_NPC_SITE_LENGTH] = {0xb8, 0x1b, 0x97, 0x01, 0x00};
const uint8_t voice_play_site[VOICE_PLAY_SITE_LENGTH] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
const uint8_t voice_release_site[VOICE_RELEASE_SITE_LENGTH] = {0x49, 0x8b, 0x95, 0x40, 0x0e, 0x00, 0x00};
const uint8_t voice_bank_push_site[VOICE_BANK_PUSH_SITE_LENGTH] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
const uint8_t voice_bank_erase_site[VOICE_BANK_ERASE_SITE_LENGTH] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53};
const uint8_t voice_bank_dtor_site[VOICE_BANK_DTOR_SITE_LENGTH] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50};
const uint8_t voice_bank_delete_site[VOICE_BANK_DELETE_SITE_LENGTH] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50};
const uint8_t voice_res_acquire_site[VOICE_RES_ACQUIRE_SITE_LENGTH] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
const uint8_t voice_res_release_site[VOICE_RES_RELEASE_SITE_LENGTH] = {0x8b, 0x88, 0x98, 0x00, 0x00, 0x00, 0x85, 0xc9};
const uint8_t voice_res_release_ptr_site[VOICE_RES_RELEASE_PTR_SITE_LENGTH] = {0x8b, 0x86, 0x98, 0x00, 0x00, 0x00, 0x85, 0xc0};

static const GuestImageId kImage = {0x11f8060, 0x54c690};
static int enabled;
static const uint8_t *image_base; /* DEBUG_SAULO */

enum { kLogMapper, kLogPlay, kLogRelease, kLogSheet, kLogAdd, kLogRemove, kLogDestroy, kLogResAcquire, kLogResRelease,
       kLogPin, kLogCueSeen, kLogBankSeen, kLogKinds };
static unsigned log_left[kLogKinds] = {200, 200, 200, 100, 100, 100, 100, 100, 100, 60, 250, 80}; /* DEBUG_SAULO: line budget per log */

enum { kTracedSlots = 128 };
static uint32_t play_generation[kTracedSlots]; /* DEBUG_SAULO */

/* Channel fields: active players are listed at +0xe38..+0xe40, each slot is 0x38 bytes from +0x18 with its cue at
 * +0x38, and the generation counter is at +0xe54. */
enum { kActiveBegin = 0xe38, kActiveEnd = 0xe40, kGeneration = 0xe54, kSlotStride = 0x38, kSlotCue = 0x38 };

static int log_take(int kind) {
    if (!log_left[kind]) return 0;
    --log_left[kind];
    return 1;
}

/* DEBUG_SAULO: the Japanese NPC cues and their English counterparts. */
static int traced_index(uint32_t cue) {
    const int traced = dcp_is_japanese_npc_cue(cue) || (cue >= 100000 && dcp_is_japanese_npc_cue(cue - 100000));
    return traced ? (int)(cue % kTracedSlots) : -1;
}

static const char *const kPinNames[VOICE_PIN_BANKS] = {
    "snd/se_gimmick10000_v.acb", "snd/se_gimmick10200_v.acb", "snd/se_gimmick10400_v.acb",
    "snd/se_gimmick10500_v.acb", "snd/se_gimmick10600_v.acb", "snd/se_gimmick10800_v.acb",
    "snd/se_enemy7100_v.acb",    "snd/se_enemy7200_v.acb",    "snd/se_enemy8800_v.acb",
};
static const char *const kPinPrefixes[VOICE_PIN_BANKS] = {
    "se_gimmick", "se_gimmick", "se_gimmick", "se_gimmick", "se_gimmick", "se_gimmick",
    "se_enemy",   "se_enemy",   "se_enemy",
};
static const unsigned kPinNumbers[VOICE_PIN_BANKS] = {10000, 10200, 10400, 10500, 10600, 10800, 7100, 7200, 8800};
static const uint8_t *pinned_resource[VOICE_PIN_BANKS];
static int pinned_bank[VOICE_PIN_BANKS];
static int pinning;

static int is_pinned_resource(const uint8_t *res) {
    for (int i = 0; i < VOICE_PIN_BANKS; ++i) {
        if (pinned_resource[i] == res) return 1;
    }
    return 0;
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

const char *voice_pin_bank_name(int index) {
    return index >= 0 && index < VOICE_PIN_BANKS ? kPinNames[index] : NULL;
}

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
        if (kPinNumbers[i] == number && !strcmp(kPinPrefixes[i], prefix)) {
            if (english) *english = is_english;
            return i;
        }
    }
    return -1;
}

unsigned voice_pinned_mask(void) {
    unsigned mask = 0;
    for (int i = 0; i < VOICE_PIN_BANKS; ++i) {
        if (pinned_bank[i]) mask |= 1u << i;
    }
    return mask;
}

void voice_pin_reset(void) {
    memset(pinned_resource, 0, sizeof(pinned_resource));
    memset(pinned_bank, 0, sizeof(pinned_bank));
    pinning = 0;
}

int voice_pin_bank(int index, void *manager, ResourceAcquire acquire) {
    if (!enabled || !acquire || !manager || index < 0 || index >= VOICE_PIN_BANKS || pinned_bank[index]) return 0;
    pinned_bank[index] = 1;
    pinning = 1;
    void *resource = acquire(manager, kPinNames[index]);
    pinning = 0;
    pinned_resource[index] = (const uint8_t *)resource;
    if (!resource) pinned_bank[index] = 0;
    return resource != NULL;
}

static uint32_t read_u32(const uint8_t *at) {
    uint32_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

static uint64_t read_u64(const uint8_t *at) {
    uint64_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

/* The stub stores the guest's return address right after the saved registers. */
static uint64_t caller_offset(const GuestRegs *regs) {
    return read_u64((const uint8_t *)regs + sizeof(GuestRegs)) - (uint64_t)(uintptr_t)image_base;
}

/* DEBUG_SAULO: the play function looks the cue up in the manager's bank list (the channel is manager+0x138). */
enum { kManagerToChannel = 0x138, kBankBegin = 0x100, kBankEnd = 0x108, kBankHandle = 0x158, kBankName = 0x168,
       kBankNameSize = 0x178, kBankNameCap = 0x180, kMaxBanks = 256 };
enum { kBankState = 0x98, kManagerFlags = 0x1eb8 };
#define CUE_EXISTS_OFFSET 0x5c0cd0
#define RESOURCE_ACQUIRE_OFFSET 0x57cbb0

static const uint8_t *last_manager; /* DEBUG_SAULO */
static uint32_t manager_generation(void) {
    return last_manager ? read_u32(last_manager + kManagerToChannel + kGeneration) : 0;
}

static const char *sheet_state_text(int state) {
    static const char *const text[] = {"found", "none", "no image", "no manager", "bad bank list"};
    return text[state];
}

CueSheet voice_resolve_cue_sheet(const uint8_t *manager, uint32_t cue, CueExists exists) {
    CueSheet sheet = {kSheetNoImage, 0, 0, NULL, NULL, 0};
    if (!exists) return sheet;
    sheet.state = kSheetNoManager;
    if (!manager) return sheet;
    const uint64_t begin = read_u64(manager + kBankBegin);
    const uint64_t end = read_u64(manager + kBankEnd);
    sheet.state = kSheetBadList;
    if (!begin || end < begin || (end - begin) / 8 > kMaxBanks) return sheet;
    sheet.count = (end - begin) / 8;
    sheet.state = kSheetNone;
    for (uint64_t i = 0; i < sheet.count; ++i) {
        const uint8_t *bank = (const uint8_t *)(uintptr_t)read_u64((const uint8_t *)(uintptr_t)begin + i * 8);
        if (!bank) continue;
        const void *handle = (const void *)(uintptr_t)read_u64(bank + kBankHandle);
        if (!handle || !exists(handle, cue)) continue;
        sheet.state = kSheetFound;
        sheet.index = i;
        sheet.handle = handle;
        sheet.name_size = read_u64(bank + kBankNameSize);
        sheet.name = read_u64(bank + kBankNameCap) >= 16 ? (const char *)(uintptr_t)read_u64(bank + kBankName)
                                                         : (const char *)(bank + kBankName);
        break;
    }
    return sheet;
}

static void log_cue_sheet(const uint8_t *channel, uint32_t cue) {
    if (!log_take(kLogSheet)) return;
    const uint8_t *manager = channel ? channel - kManagerToChannel : NULL;
    const CueExists exists = image_base ? (CueExists)(uintptr_t)(image_base + CUE_EXISTS_OFFSET) : NULL;
    const CueSheet sheet = voice_resolve_cue_sheet(manager, cue, exists);
    if (sheet.state == kSheetFound) {
        printf("DEBUG_SAULO voice sheet: cue %u channel %llx sheets %llu match %llu acb %llx name %.*s pinned 0x%x\n",
               cue, (unsigned long long)(uintptr_t)channel, (unsigned long long)sheet.count,
               (unsigned long long)sheet.index, (unsigned long long)(uintptr_t)sheet.handle,
               sheet.name_size < 96 ? (int)sheet.name_size : 96, sheet.name, voice_pinned_mask());
    } else if (sheet.state == kSheetNone) {
        printf("DEBUG_SAULO voice sheet: cue %u channel %llx sheets %llu match none\n", cue,
               (unsigned long long)(uintptr_t)channel, (unsigned long long)sheet.count);
    } else {
        printf("DEBUG_SAULO voice sheet: cue %u unavailable reason %s\n", cue, sheet_state_text(sheet.state));
    }
}

void voice_hooks_set_enabled(int value) {
    enabled = value != 0;
}

int voice_hooks_enabled(void) {
    return enabled;
}

/* On the US-only path of the cue mapper, before `mov eax, 0x1971b`: returning the cue unchanged and skipping the rest
 * of the mapper leaves the NPC cue on its Japanese ID. */
static void log_new_cue(uint32_t cue, const GuestRegs *regs) {
    static uint32_t seen[512];
    static unsigned seen_count;
    if (cue < 3000 || cue >= 100000) return;
    for (unsigned i = 0; i < seen_count; ++i) {
        if (seen[i] == cue) return;
    }
    if (seen_count == sizeof(seen) / sizeof(seen[0])) return;
    seen[seen_count++] = cue;
    if (log_take(kLogCueSeen))
        printf("DEBUG_SAULO voice mapper new cue %u generation %u caller 0x%llx\n", cue, manager_generation(),
               (unsigned long long)caller_offset(regs));
}

void GUEST_HOOK_ABI voice_npc_hook(GuestRegs *regs) {
    const uint32_t cue = (uint32_t)regs->rdi;
    if (traced_index(cue) >= 0 && log_take(kLogMapper))
        printf("DEBUG_SAULO voice mapper: input %u family %d remap %d caller 0x%llx\n", cue,
               dcp_is_japanese_npc_cue(cue), enabled, (unsigned long long)caller_offset(regs));
    if (!dcp_is_japanese_npc_cue(cue)) log_new_cue(cue, regs);
    if (!enabled || !dcp_is_japanese_npc_cue(cue)) return;
    regs->rax = regs->rdi;
    regs->skip = 1;
}

/* DEBUG_SAULO: a traced cue handed to the player, with the channel's active players and generation. */
void GUEST_HOOK_ABI voice_play_hook(GuestRegs *regs) {
    if (!regs->rdi) return;
    const uint32_t cue = (uint32_t)regs->rsi;
    const int index = traced_index(cue);
    if (index < 0) return;
    const uint8_t *channel = (const uint8_t *)(uintptr_t)regs->rdi;
    const uint32_t generation = read_u32(channel + kGeneration);
    play_generation[index] = generation;
    log_cue_sheet(channel, cue);
    if (!log_take(kLogPlay)) return;
    const uint64_t active = read_u64(channel + kActiveEnd) - read_u64(channel + kActiveBegin);
    printf("DEBUG_SAULO voice play: cue %u flag %d volume %d channel %llx generation %u active %llu caller 0x%llx\n",
           cue, (int32_t)regs->rdx, (int32_t)regs->r8, (unsigned long long)regs->rdi, generation,
           (unsigned long long)active, (unsigned long long)caller_offset(regs));
}

/* DEBUG_SAULO: a player leaving its channel with the status the channel update read for it. */
void GUEST_HOOK_ABI voice_release_hook(GuestRegs *regs) {
    if (!regs->r13 || !regs->r14) return;
    const uint8_t *channel = (const uint8_t *)(uintptr_t)regs->r13;
    const uint8_t slot = *(const uint8_t *)(uintptr_t)regs->r14;
    const uint32_t cue = read_u32(channel + kSlotCue + slot * kSlotStride);
    const int index = traced_index(cue);
    if (index < 0 || !log_take(kLogRelease)) return;
    const uint32_t generation = read_u32(channel + kGeneration);
    const uint64_t active = read_u64(channel + kActiveEnd) - read_u64(channel + kActiveBegin);
    printf("DEBUG_SAULO voice release: cue %u status %u generation %u played at %u active %llu\n", cue,
           (uint32_t)regs->rax, generation, play_generation[index], (unsigned long long)active);
}

static const char *bank_name(const uint8_t *bank, uint64_t *size) {
    *size = read_u64(bank + kBankNameSize);
    return read_u64(bank + kBankNameCap) >= 16 ? (const char *)(uintptr_t)read_u64(bank + kBankName)
                                               : (const char *)(bank + kBankName);
}

static uint64_t bank_count(const uint8_t *manager) {
    const uint64_t begin = read_u64(manager + kBankBegin);
    const uint64_t end = read_u64(manager + kBankEnd);
    return begin && end >= begin ? (end - begin) / 8 : 0;
}

int voice_bank_index(const uint8_t *manager, const uint8_t *bank) {
    const uint64_t begin = read_u64(manager + kBankBegin);
    const uint64_t count = bank_count(manager);
    for (uint64_t i = 0; i < count; ++i) {
        if (read_u64((const uint8_t *)(uintptr_t)(begin + i * 8)) == (uint64_t)(uintptr_t)bank) {
            return (int)i;
        }
    }
    return -1;
}

/* DEBUG_SAULO: manager+0x100 is the bank list: the constructor pushes at 0x582420, the destructor erases at 0x582610. */
void GUEST_HOOK_ABI voice_bank_push_hook(GuestRegs *regs) {
    const uint8_t *manager = (const uint8_t *)(uintptr_t)regs->rdi;
    const uint8_t *bank = (const uint8_t *)(uintptr_t)regs->rsi;
    if (manager) last_manager = manager;
    if (!manager || !bank || !pinning || !log_take(kLogAdd)) return;
    const int duplicate = voice_bank_index(manager, bank) >= 0;
    const uint64_t count = bank_count(manager);
    printf("DEBUG_SAULO voice acb add: bank %llx count %llu -> %llu duplicate %d generation %u caller 0x%llx\n",
           (unsigned long long)(uintptr_t)bank, (unsigned long long)count,
           (unsigned long long)(count + (duplicate ? 0 : 1)), duplicate,
           read_u32(manager + kManagerToChannel + kGeneration), (unsigned long long)caller_offset(regs));
}

void GUEST_HOOK_ABI voice_bank_erase_hook(GuestRegs *regs) {
    const uint8_t *manager = (const uint8_t *)(uintptr_t)regs->rdi;
    const uint8_t *bank = (const uint8_t *)(uintptr_t)regs->rsi;
    if (!manager || !bank) return;
    const int index = voice_bank_index(manager, bank);
    if (index < 0 || !is_pinned_resource(bank) || !log_take(kLogRemove)) return;
    uint64_t name_size = 0;
    const char *name = bank_name(bank, &name_size);
    const uint64_t count = bank_count(manager);
    const int state = (int32_t)read_u32(bank + kBankState);
    const int flag = (read_u32(manager + kManagerFlags) & 1) != 0;
    const uint64_t after = state <= 0 && flag ? count - 1 : count;
    printf("DEBUG_SAULO voice acb remove: index %d bank %llx acb %llx name %.*s count %llu -> %llu state %d flag %d "
           "generation %u\n",
           index, (unsigned long long)(uintptr_t)bank, (unsigned long long)read_u64(bank + kBankHandle),
           name_size < 96 ? (int)name_size : 96, name, (unsigned long long)count, (unsigned long long)after, state,
           flag, read_u32(manager + kManagerToChannel + kGeneration));
}

static void log_bank_destroy(const GuestRegs *regs, const char *kind) {
    const uint8_t *bank = (const uint8_t *)(uintptr_t)regs->rdi;
    if (!bank || !is_pinned_resource(bank) || !log_take(kLogDestroy)) return;
    uint64_t name_size = 0;
    const char *name = bank_name(bank, &name_size);
    printf("DEBUG_SAULO voice acb destroy (%s): bank %llx acb %llx name %.*s state %d caller 0x%llx\n", kind,
           (unsigned long long)(uintptr_t)bank, (unsigned long long)read_u64(bank + kBankHandle),
           name_size < 96 ? (int)name_size : 96, name, (int32_t)read_u32(bank + kBankState),
           (unsigned long long)caller_offset(regs));
}

void GUEST_HOOK_ABI voice_bank_dtor_hook(GuestRegs *regs) {
    log_bank_destroy(regs, "complete");
}

void GUEST_HOOK_ABI voice_bank_delete_hook(GuestRegs *regs) {
    log_bank_destroy(regs, "deleting");
}

/* DEBUG_SAULO: resources are reference counted at +0x98 with a std::string name at +0xd8 (size +0xe8, capacity +0xf0). */
enum { kResRefs = 0x98, kResFlags = 0xa0, kResName = 0xd8, kResNameSize = 0xe8, kResNameCap = 0xf0, kMaxName = 160 };

static int is_traced_bank_name(const char *name, size_t size) {
    (void)size;
    return voice_pin_target_index(name, NULL) >= 0;
}

static uint32_t name_hash(const char *name, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ (uint8_t)name[i]) * 16777619u;
    return hash;
}

static void log_new_bank_name(const char *name, size_t size, const GuestRegs *regs) {
    static uint32_t seen[96];
    static unsigned seen_count;
    if (!strstr(name, "se_enemy") && !strstr(name, "se_player") && !strstr(name, "se_voice")) return;
    const uint32_t hash = name_hash(name, size);
    for (unsigned i = 0; i < seen_count; ++i) {
        if (seen[i] == hash) return;
    }
    if (seen_count == sizeof(seen) / sizeof(seen[0])) return;
    seen[seen_count++] = hash;
    if (log_take(kLogBankSeen))
        printf("DEBUG_SAULO voice bank requested: %.*s generation %u caller 0x%llx\n", (int)size, name,
               manager_generation(), (unsigned long long)caller_offset(regs));
}

void GUEST_HOOK_ABI voice_res_acquire_hook(GuestRegs *regs) {
    if (pinning) return;
    const char *name = (const char *)(uintptr_t)regs->rsi;
    if (!name) return;
    const size_t size = strnlen(name, kMaxName);
    int english = 0;
    const int index = voice_pin_target_index(name, &english);
    if (index < 0) {
        log_new_bank_name(name, size, regs);
        return;
    }
    const ResourceAcquire acquire =
        image_base ? (ResourceAcquire)(uintptr_t)(image_base + RESOURCE_ACQUIRE_OFFSET) : NULL;
    const int was_pinned = pinned_bank[index];
    const int applied = voice_pin_bank(index, (void *)(uintptr_t)regs->rdi, acquire);
    if (!log_take(kLogPin)) return;
    const uint8_t *resource = pinned_resource[index];
    printf("DEBUG_SAULO voice pin: request %.*s english %d bank %s result %s resource %llx refs %d pinned 0x%x "
           "generation %u caller 0x%llx\n",
           (int)size, name, english, kPinNames[index],
           applied ? "applied" : (was_pinned ? "already" : (enabled ? "failed" : "off")),
           (unsigned long long)(uintptr_t)resource, resource ? (int32_t)read_u32(resource + kResRefs) : -1,
           voice_pinned_mask(), manager_generation(), (unsigned long long)caller_offset(regs));
}

static void log_res_release(const uint8_t *res, const GuestRegs *regs) {
    if (!res) return;
    const char *name = read_u64(res + kResNameCap) >= 16 ? (const char *)(uintptr_t)read_u64(res + kResName)
                                                         : (const char *)(res + kResName);
    if (!name) return;
    const size_t size = strnlen(name, kMaxName);
    if (!is_traced_bank_name(name, size) || !log_take(kLogResRelease)) return;
    const int32_t refs = (int32_t)read_u32(res + kResRefs);
    printf("DEBUG_SAULO voice res release: resource %llx name %.*s refs %d -> %d generation %u caller 0x%llx\n",
           (unsigned long long)(uintptr_t)res, (int)size, name, refs, refs > 0 ? refs - 1 : refs, manager_generation(),
           (unsigned long long)caller_offset(regs));
}

void GUEST_HOOK_ABI voice_res_release_hook(GuestRegs *regs) {
    log_res_release((const uint8_t *)(uintptr_t)regs->rax, regs);
}

void GUEST_HOOK_ABI voice_res_release_ptr_hook(GuestRegs *regs) {
    log_res_release((const uint8_t *)(uintptr_t)regs->rsi, regs);
}

size_t voice_hooks_stub_bytes(void) {
    return VOICE_HOOK_COUNT * guest_hook_stub_size();
}

int voice_hooks_install(uint8_t *image, uint64_t size, uint64_t entry, uint8_t *stubs, size_t stubs_size) {
    if (size != kImage.size || entry != kImage.entry) {
        printf("Japanese NPC Voices: hook not installed: not the CUSA10454 1.00 image\n");
        return 0;
    }
    GuestHook hooks[VOICE_HOOK_COUNT] = {
        {"japanese npc voices", VOICE_NPC_SITE_OFFSET, VOICE_NPC_SITE_LENGTH, {0}, voice_npc_hook},
        {"DEBUG_SAULO voice play", VOICE_PLAY_SITE_OFFSET, VOICE_PLAY_SITE_LENGTH, {0}, voice_play_hook},
        {"DEBUG_SAULO voice release", VOICE_RELEASE_SITE_OFFSET, VOICE_RELEASE_SITE_LENGTH, {0}, voice_release_hook},
        {"DEBUG_SAULO voice bank push", VOICE_BANK_PUSH_SITE_OFFSET, VOICE_BANK_PUSH_SITE_LENGTH, {0},
         voice_bank_push_hook},
        {"DEBUG_SAULO voice bank erase", VOICE_BANK_ERASE_SITE_OFFSET, VOICE_BANK_ERASE_SITE_LENGTH, {0},
         voice_bank_erase_hook},
        {"DEBUG_SAULO voice bank dtor", VOICE_BANK_DTOR_SITE_OFFSET, VOICE_BANK_DTOR_SITE_LENGTH, {0},
         voice_bank_dtor_hook},
        {"DEBUG_SAULO voice bank delete", VOICE_BANK_DELETE_SITE_OFFSET, VOICE_BANK_DELETE_SITE_LENGTH, {0},
         voice_bank_delete_hook},
        {"DEBUG_SAULO voice res acquire", VOICE_RES_ACQUIRE_SITE_OFFSET, VOICE_RES_ACQUIRE_SITE_LENGTH, {0},
         voice_res_acquire_hook},
        {"DEBUG_SAULO voice res release", VOICE_RES_RELEASE_SITE_OFFSET, VOICE_RES_RELEASE_SITE_LENGTH, {0},
         voice_res_release_hook},
        {"DEBUG_SAULO voice res release ptr", VOICE_RES_RELEASE_PTR_SITE_OFFSET, VOICE_RES_RELEASE_PTR_SITE_LENGTH, {0},
         voice_res_release_ptr_hook},
    };
    memcpy(hooks[0].original, voice_npc_site, sizeof(voice_npc_site));
    memcpy(hooks[1].original, voice_play_site, sizeof(voice_play_site));
    memcpy(hooks[2].original, voice_release_site, sizeof(voice_release_site));
    memcpy(hooks[3].original, voice_bank_push_site, sizeof(voice_bank_push_site));
    memcpy(hooks[4].original, voice_bank_erase_site, sizeof(voice_bank_erase_site));
    memcpy(hooks[5].original, voice_bank_dtor_site, sizeof(voice_bank_dtor_site));
    memcpy(hooks[6].original, voice_bank_delete_site, sizeof(voice_bank_delete_site));
    memcpy(hooks[7].original, voice_res_acquire_site, sizeof(voice_res_acquire_site));
    memcpy(hooks[8].original, voice_res_release_site, sizeof(voice_res_release_site));
    memcpy(hooks[9].original, voice_res_release_ptr_site, sizeof(voice_res_release_ptr_site));
    size_t bad = 0;
    const GuestHookStatus status =
        guest_hooks_install(image, size, entry, &kImage, hooks, VOICE_HOOK_COUNT, stubs, stubs_size, &bad);
    if (status != GUEST_HOOK_OK) {
        printf("Japanese NPC Voices: hooks not installed: %s (hook %zu: %s)\n", guest_hook_status_text(status), bad,
               hooks[bad].name);
        return 0;
    }
    image_base = image;
    printf("Japanese NPC Voices: hooks installed (remap %s) at 0x68bc9, 0x5837f0, 0x5836d8, 0x582420, 0x582610, "
           "0x582570, 0x582760, 0x57cbb0, 0x57d159, 0x57d228\n", enabled ? "on" : "off");
    return 1;
}
