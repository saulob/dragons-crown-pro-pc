#include "default_message_hook.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum {
    kRowCount = DEFAULT_MESSAGE_CATEGORIES,
    kRowHookInPattern = 7,
    kRowHookLength = 9,
    kRowLabelBeforePattern = 0x13,
    kRowBlockSearch = 0x400,
    kGetterLoadBeforeCall = 0x2c,
    kPickHookLength = 7,
    kPickerToPick = 0xb0,
    kDefaultsToPickerCall = 0xc0,
    kDefaultsArgumentsAt = 0x1f,
    kCharacterClass = 0x27,
    kCharacterMessages = 0x1f70,
    kCharacterAltMessages = 0x3c,
    kCharacterMessagePair = 0x78,
    kCharacterFlags = 0x21a8,
    kGetterSpan = 0xa0,
    kLoadPairLength = 16,
    kLoadPairGap = 0x1c,
    kStoredHooks = 2 * kRowCount,
    kNoVariant = -1
};

/* The character being edited, a local of the row builder. */
static const uint64_t kRowCharacterSlot = 0x368;

/* Slots in the picker's frame, in 8 byte words from rbp. */
enum { kFrameReturn = 1, kFrameCallerR14 = -2, kFrameCallerR13 = -3 };

static const unsigned char kRowPattern[] = {0x48, 0x8d, 0xb5, 0x38, 0xfd, 0xff, 0xff, 0x8b, 0x00,
                                            0x41, 0x89, 0x87, 0xc0, 0x06, 0x00, 0x00, 0xe8};
static const unsigned char kGetterLoad[] = {0x48, 0x8b, 0xbd, 0x98, 0xfc, 0xff, 0xff};
static const unsigned char kPickPattern[] = {0xe8, 0x00, 0x00, 0x00, 0x00, 0x31, 0xd2, 0xf7, 0xf3, 0x4d, 0x85, 0xff, 0x41, 0x89, 0xd5};
static const unsigned char kPickerStart[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53};
static const unsigned char kDefaultsArguments[] = {0x49, 0x89, 0xfd, 0x41, 0x89, 0xf6};
static const unsigned char kAfterPickerCall[] = {0x45, 0x84, 0xf6};

/* Records per class: the picker's tables. */
static const int32_t kFirstRecord[DEFAULT_MESSAGE_CATEGORIES][8] = {
    [DEFAULT_MESSAGE_GREETING] = {1761, 1693, 1897, 1829, 1965, 2033, 0, 0},
    [DEFAULT_MESSAGE_SELECTION] = {1744, 1676, 1880, 1812, 1948, 2016, 0, 0},
    [DEFAULT_MESSAGE_RESURRECTION] = {1727, 1659, 1863, 1795, 1931, 1999, 0, 0},
    [DEFAULT_MESSAGE_DEATH] = {1709, 1641, 1845, 1777, 1913, 1981, 0, 0}, /* the block's marker: variants start after it */
};
/* The order the character keeps its messages in: death, resurrection, selection, greeting. */
static const int kStoredIndex[DEFAULT_MESSAGE_CATEGORIES] = {
    [DEFAULT_MESSAGE_GREETING] = 3, [DEFAULT_MESSAGE_SELECTION] = 2, [DEFAULT_MESSAGE_RESURRECTION] = 1, [DEFAULT_MESSAGE_DEATH] = 0};
static const int32_t kDeathEnd[8] = {1725, 1657, 1861, 1793, 1929, 1997, 0, 0};

/* The guest's UTF-16 string, as the row's assignment reads its source. */
typedef struct {
    uint64_t allocator;
    const uint16_t *data;
    uint64_t unused;
    uint64_t size;
    uint64_t capacity;
} GuestText;

typedef struct {
    GuestText text;
    uint16_t units[DEFAULT_MESSAGE_MAX_UNITS + 1];
} Message;

typedef struct {
    uint16_t units[DEFAULT_MESSAGE_MAX_UNITS + 1];
    size_t size;
    int translated; /* the served text differs from this one */
} NativeMessage;

static Message messages[DEFAULT_MESSAGE_CLASSES][DEFAULT_MESSAGE_CATEGORIES][DEFAULT_MESSAGE_VARIANTS];
static NativeMessage natives[DEFAULT_MESSAGE_CLASSES][DEFAULT_MESSAGE_CATEGORIES][DEFAULT_MESSAGE_VARIANTS];
static atomic_int natives_loaded;
static atomic_int held_variant[DEFAULT_MESSAGE_CLASSES];
static atomic_uint_fast64_t variant_state;
static const unsigned char *hooked_image;
static uint64_t picker_return_offset;

int default_message_record(int character_class, DefaultMessageCategory category, int variant) {
    if (character_class < 0 || character_class >= DEFAULT_MESSAGE_CLASSES) return -1;
    if ((int)category < 0 || category >= DEFAULT_MESSAGE_CATEGORIES) return -1;
    if (variant < 0 || variant >= DEFAULT_MESSAGE_VARIANTS) return -1;
    return kFirstRecord[category][character_class] + variant + (category == DEFAULT_MESSAGE_DEATH);
}

static int bytes_at(const unsigned char *image, uint64_t size, uint64_t at, const void *bytes, size_t length) {
    return at <= size && length <= size - at && !memcmp(image + at, bytes, length);
}

static int found_in_image(const unsigned char *image, uint64_t size, const void *bytes, size_t length) {
    const unsigned char first = *(const unsigned char *)bytes;
    for (uint64_t at = 0; at + length <= size; ++at) {
        if (image[at] == first && !memcmp(image + at, bytes, length)) return 1;
    }
    return 0;
}

static uint64_t call_target(const unsigned char *image, uint64_t at) {
    int32_t displacement;
    memcpy(&displacement, image + at + 1, sizeof(displacement));
    return at + 5 + (uint64_t)(int64_t)displacement;
}

/* The getter of the row's block: its empty result jumps to the label that loads the empty-field text. */
static uint64_t row_getter(const unsigned char *image, uint64_t size, uint64_t label) {
    static const unsigned kCallBeforeJump[] = {20, 23};
    if (label < kRowBlockSearch || label > size) return 0;
    for (uint64_t at = label - kRowBlockSearch; at + 6 <= label; ++at) {
        if (image[at] != 0x0f || image[at + 1] != 0x84) continue;
        int32_t displacement;
        memcpy(&displacement, image + at + 2, sizeof(displacement));
        if (at + 6 + (uint64_t)(int64_t)displacement != label) continue;
        for (size_t i = 0; i < sizeof(kCallBeforeJump) / sizeof(*kCallBeforeJump); ++i) {
            const uint64_t call = at - kCallBeforeJump[i];
            if (at >= kCallBeforeJump[i] + kGetterLoadBeforeCall && image[call] == 0xe8 &&
                bytes_at(image, size, call - kGetterLoadBeforeCall, kGetterLoad, sizeof(kGetterLoad)))
                return call_target(image, call);
        }
    }
    return 0;
}

/* The pair of loads a getter makes of a block: the stored text's first 32 bytes, then the 32 that end it. */
static uint64_t stored_loads(const unsigned char *image, uint64_t size, uint64_t getter, size_t field) {
    unsigned char pair[kLoadPairLength] = {0xc5, 0xfc, 0x10, 0x87, 0, 0, 0, 0, 0xc5, 0xfc, 0x10, 0x8f, 0, 0, 0, 0};
    const int32_t first = (int32_t)field, second = (int32_t)(field + kLoadPairGap);
    memcpy(pair + 4, &first, sizeof(first));
    memcpy(pair + 12, &second, sizeof(second));
    uint64_t found = 0;
    for (uint64_t at = getter; at + kLoadPairLength <= getter + kGetterSpan && at + kLoadPairLength <= size; ++at) {
        if (!bytes_at(image, size, at, pair, sizeof(pair))) continue;
        if (found) return 0;
        found = at;
    }
    return found;
}

static void locate_stored(const unsigned char *image, uint64_t size, const uint64_t *getters, DefaultMessageSites *sites) {
    for (int category = 0; category < kRowCount; ++category) {
        const size_t field = kCharacterMessages + (size_t)kStoredIndex[category] * kCharacterMessagePair;
        sites->stored[category][0] = stored_loads(image, size, getters[category], field);
        sites->stored[category][1] = stored_loads(image, size, getters[category], field + kCharacterAltMessages);
        if (!sites->stored[category][0] || !sites->stored[category][1]) {
            memset(sites->stored, 0, sizeof(sites->stored));
            return;
        }
    }
}

static int locate_rows(const unsigned char *image, uint64_t size, DefaultMessageSites *sites) {
    uint64_t getters[kRowCount];
    int found = 0;
    for (uint64_t at = 0; at + sizeof(kRowPattern) <= size; ++at) {
        if (image[at] != kRowPattern[0] || memcmp(image + at, kRowPattern, sizeof(kRowPattern))) continue;
        if (found == kRowCount || at < kRowLabelBeforePattern) return 0;
        getters[found] = row_getter(image, size, at - kRowLabelBeforePattern);
        if (!getters[found]) return 0;
        sites->rows[found++] = at + kRowHookInPattern;
    }
    if (found != kRowCount) return 0;
    for (int i = 1; i < kRowCount; ++i) {
        if (getters[i] >= getters[i - 1]) return 0;
    }
    locate_stored(image, size, getters, sites);
    return 1;
}

static int locate_pick(const unsigned char *image, uint64_t size, DefaultMessageSites *sites) {
    uint64_t pick = 0;
    for (uint64_t at = kPickerToPick; at + sizeof(kPickPattern) <= size; ++at) {
        if (image[at] != 0xe8 || memcmp(image + at + 5, kPickPattern + 5, sizeof(kPickPattern) - 5)) continue;
        if (!bytes_at(image, size, at + 5 - kPickerToPick, kPickerStart, sizeof(kPickerStart))) continue;
        if (pick) return 0;
        pick = at + 5;
    }
    if (!pick) return 0;
    const uint64_t picker = pick - kPickerToPick;
    uint64_t back = 0;
    for (uint64_t at = kDefaultsToPickerCall; at + 8 <= size; ++at) {
        if (image[at] != 0xe8 || call_target(image, at) != picker) continue;
        if (!bytes_at(image, size, at + 5, kAfterPickerCall, sizeof(kAfterPickerCall))) continue;
        const uint64_t defaults = at - kDefaultsToPickerCall;
        if (!bytes_at(image, size, defaults + kDefaultsArgumentsAt, kDefaultsArguments, sizeof(kDefaultsArguments))) continue;
        if (back) return 0;
        back = at + 5;
    }
    sites->pick = pick;
    sites->picker_return = back;
    return back != 0;
}

int default_message_locate(const unsigned char *image, uint64_t size, DefaultMessageSites *sites) {
    memset(sites, 0, sizeof(*sites));
    for (int i = 0; i < DEFAULT_MESSAGE_CATEGORIES; ++i) {
        if (!found_in_image(image, size, kFirstRecord[i], sizeof(kFirstRecord[i]))) return 0;
    }
    if (!found_in_image(image, size, kDeathEnd, sizeof(kDeathEnd))) return 0;
    if (locate_rows(image, size, sites) && locate_pick(image, size, sites)) return 1;
    memset(sites, 0, sizeof(*sites));
    return 0;
}

size_t default_message_stub_bytes(void) {
    return (kRowCount + 1 + kStoredHooks) * guest_hook_stub_size();
}

/* The units of the text, up to what the field keeps; 0 for empty or malformed text. */
static size_t utf16_from_utf8(const char *text, uint16_t *units) {
    const unsigned char *in = (const unsigned char *)text;
    size_t count = 0;
    while (*in && count < DEFAULT_MESSAGE_MAX_UNITS) {
        unsigned unit = *in++;
        int continuation = unit < 0x80 ? 0 : (unit & 0xe0) == 0xc0 ? 1 : (unit & 0xf0) == 0xe0 ? 2 : -1;
        if (continuation < 0) return 0;
        if (continuation) unit &= 0x3f >> continuation;
        for (; continuation; --continuation) {
            if ((*in & 0xc0) != 0x80) return 0;
            unit = unit << 6 | (*in++ & 0x3f);
        }
        units[count++] = (uint16_t)unit;
    }
    units[count] = 0;
    return count;
}

static int load_messages(const char *(*record_text)(unsigned)) {
    for (int c = 0; c < DEFAULT_MESSAGE_CLASSES; ++c) {
        atomic_store(&held_variant[c], kNoVariant);
        for (int k = 0; k < DEFAULT_MESSAGE_CATEGORIES; ++k) {
            for (int v = 0; v < DEFAULT_MESSAGE_VARIANTS; ++v) {
                const char *text = record_text((unsigned)default_message_record(c, (DefaultMessageCategory)k, v));
                Message *message = &messages[c][k][v];
                memset(message->units, 0, sizeof(message->units));
                const size_t units = text ? utf16_from_utf8(text, message->units) : 0;
                if (!units) return 0;
                message->text = (GuestText){0, message->units, 0, units, DEFAULT_MESSAGE_MAX_UNITS};
            }
        }
    }
    return 1;
}

static int load_natives(const char *(*native_text)(unsigned)) {
    atomic_store(&natives_loaded, 0);
    for (int c = 0; c < DEFAULT_MESSAGE_CLASSES; ++c) {
        for (int k = 0; k < DEFAULT_MESSAGE_CATEGORIES; ++k) {
            for (int v = 0; v < DEFAULT_MESSAGE_VARIANTS; ++v) {
                const char *text = native_text((unsigned)default_message_record(c, (DefaultMessageCategory)k, v));
                NativeMessage *native = &natives[c][k][v];
                memset(native->units, 0, sizeof(native->units));
                native->size = text ? utf16_from_utf8(text, native->units) : 0;
                if (!native->size) return 0;
                const Message *served = &messages[c][k][v];
                native->translated = served->text.size != native->size ||
                                     memcmp(served->units, native->units, native->size * sizeof(uint16_t));
            }
        }
    }
    atomic_store(&natives_loaded, 1);
    return 1;
}

/* Never the guest's rand, so the game's random sequence stays its own. */
static int new_variant(void) {
    uint_fast64_t x = atomic_load(&variant_state);
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    atomic_store(&variant_state, x);
    return (int)(x % DEFAULT_MESSAGE_VARIANTS);
}

static int valid_class(const unsigned char *character) {
    const int character_class = (signed char)character[kCharacterClass];
    return character_class >= 0 && character_class < DEFAULT_MESSAGE_CLASSES ? character_class : -1;
}

void default_message_preview(GuestRegs *regs, DefaultMessageCategory category) {
    const unsigned char *character;
    memcpy(&character, (const void *)(uintptr_t)(regs->rbp - kRowCharacterSlot), sizeof(character));
    const int character_class = character ? valid_class(character) : -1;
    if (character_class < 0) return;
    int variant = atomic_load(&held_variant[character_class]);
    if (variant == kNoVariant) {
        variant = new_variant();
        atomic_store(&held_variant[character_class], variant);
    }
    regs->rsi = (uint64_t)(uintptr_t)&messages[character_class][category][variant].text;
}

static int has_empty_message(const unsigned char *character) {
    const size_t block = (signed char)character[kCharacterFlags] < 0 ? kCharacterAltMessages : 0;
    for (int i = 0; i < DEFAULT_MESSAGE_CATEGORIES; ++i) {
        const unsigned char *field = character + kCharacterMessages + block + (size_t)i * kCharacterMessagePair;
        if (!field[0] && !field[1]) return 1;
    }
    return 0;
}

void default_message_display(GuestRegs *regs, DefaultMessageCategory category, int alternate) {
    if (!atomic_load(&natives_loaded) || (int)category < 0 || category >= DEFAULT_MESSAGE_CATEGORIES) return;
    const unsigned char *character = (const unsigned char *)(uintptr_t)regs->rdi;
    const int character_class = character ? valid_class(character) : -1;
    if (character_class < 0) return;
    const size_t field = kCharacterMessages + (size_t)kStoredIndex[category] * kCharacterMessagePair +
                         (alternate ? kCharacterAltMessages : 0);
    uint16_t stored[DEFAULT_MESSAGE_MAX_UNITS];
    memcpy(stored, character + field, sizeof(stored));
    size_t length = 0;
    while (length < DEFAULT_MESSAGE_MAX_UNITS && stored[length]) ++length;
    if (!length) return;
    for (int variant = 0; variant < DEFAULT_MESSAGE_VARIANTS; ++variant) {
        const NativeMessage *native = &natives[character_class][category][variant];
        if (!native->translated || native->size != length || memcmp(native->units, stored, length * sizeof(uint16_t))) continue;
        regs->rdi = (uint64_t)(uintptr_t)messages[character_class][category][variant].units - field;
        return;
    }
}

void GUEST_HOOK_ABI default_message_pin(GuestRegs *regs) {
    const uint64_t *frame = (const uint64_t *)(uintptr_t)regs->rbp;
    if (frame[kFrameReturn] != (uint64_t)(uintptr_t)(hooked_image + picker_return_offset)) return;
    if (!(uint8_t)frame[kFrameCallerR14]) return;
    const unsigned char *character = (const unsigned char *)(uintptr_t)frame[kFrameCallerR13];
    const int character_class = valid_class(character);
    if (character_class < 0 || !has_empty_message(character)) return;
    const int variant = atomic_exchange(&held_variant[character_class], kNoVariant);
    if (variant != kNoVariant && (uint32_t)variant < (uint32_t)regs->rbx) regs->rax = (uint64_t)variant;
}

static void GUEST_HOOK_ABI preview_greeting(GuestRegs *regs) { default_message_preview(regs, DEFAULT_MESSAGE_GREETING); }
static void GUEST_HOOK_ABI preview_selection(GuestRegs *regs) { default_message_preview(regs, DEFAULT_MESSAGE_SELECTION); }
static void GUEST_HOOK_ABI preview_resurrection(GuestRegs *regs) { default_message_preview(regs, DEFAULT_MESSAGE_RESURRECTION); }
static void GUEST_HOOK_ABI preview_death(GuestRegs *regs) { default_message_preview(regs, DEFAULT_MESSAGE_DEATH); }

#define STORED_HANDLERS(name, category)                                                                       \
    static void GUEST_HOOK_ABI name##_main(GuestRegs *regs) { default_message_display(regs, category, 0); }   \
    static void GUEST_HOOK_ABI name##_alternate(GuestRegs *regs) { default_message_display(regs, category, 1); }

STORED_HANDLERS(stored_greeting, DEFAULT_MESSAGE_GREETING)
STORED_HANDLERS(stored_selection, DEFAULT_MESSAGE_SELECTION)
STORED_HANDLERS(stored_resurrection, DEFAULT_MESSAGE_RESURRECTION)
STORED_HANDLERS(stored_death, DEFAULT_MESSAGE_DEATH)

static int unavailable(const char *reason) {
    printf("Character messages: previews unavailable, %s\n", reason);
    return 0;
}

int default_message_install(unsigned char *image, uint64_t size, uint64_t entry, const char *(*record_text)(unsigned),
                            const char *(*native_text)(unsigned), unsigned char *stubs, size_t stubs_size) {
    DefaultMessageSites sites;
    atomic_store(&natives_loaded, 0);
    if (!default_message_locate(image, size, &sites)) return unavailable("the character creation code does not match");
    if (!load_messages(record_text)) return unavailable("a default message is missing from the served sheet");
    GuestHook hooks[kRowCount + 1 + kStoredHooks] = {
        {"default greeting row", (uint32_t)sites.rows[0], kRowHookLength, {0}, preview_greeting},
        {"default selection row", (uint32_t)sites.rows[1], kRowHookLength, {0}, preview_selection},
        {"default resurrection row", (uint32_t)sites.rows[2], kRowHookLength, {0}, preview_resurrection},
        {"default death row", (uint32_t)sites.rows[3], kRowHookLength, {0}, preview_death},
        {"default message pick", (uint32_t)sites.pick, kPickHookLength, {0}, default_message_pin},
        {"stored greeting", (uint32_t)sites.stored[0][0], kLoadPairLength, {0}, stored_greeting_main},
        {"stored greeting, alternate", (uint32_t)sites.stored[0][1], kLoadPairLength, {0}, stored_greeting_alternate},
        {"stored selection", (uint32_t)sites.stored[1][0], kLoadPairLength, {0}, stored_selection_main},
        {"stored selection, alternate", (uint32_t)sites.stored[1][1], kLoadPairLength, {0}, stored_selection_alternate},
        {"stored resurrection", (uint32_t)sites.stored[2][0], kLoadPairLength, {0}, stored_resurrection_main},
        {"stored resurrection, alternate", (uint32_t)sites.stored[2][1], kLoadPairLength, {0}, stored_resurrection_alternate},
        {"stored death", (uint32_t)sites.stored[3][0], kLoadPairLength, {0}, stored_death_main},
        {"stored death, alternate", (uint32_t)sites.stored[3][1], kLoadPairLength, {0}, stored_death_alternate},
    };
    const char *stored_note = NULL;
    size_t count = kRowCount + 1;
    if (!native_text) stored_note = "the slot's own defaults were not given";
    else if (!sites.stored[0][0]) stored_note = "the getters of the stored messages do not match";
    else if (!load_natives(native_text)) stored_note = "a default message is missing from the slot's own sheet";
    else count += kStoredHooks;
    for (size_t i = 0; i < count; ++i) memcpy(hooks[i].original, image + hooks[i].offset, hooks[i].length);
    size_t bad = 0;
    GuestHookStatus status = guest_hooks_install(image, size, entry, NULL, hooks, count, stubs, stubs_size, &bad);
    if (status != GUEST_HOOK_OK && count > kRowCount + 1) {
        atomic_store(&natives_loaded, 0);
        count = kRowCount + 1;
        stored_note = guest_hook_status_text(status);
        status = guest_hooks_install(image, size, entry, NULL, hooks, count, stubs, stubs_size, &bad);
    }
    if (status != GUEST_HOOK_OK) {
        atomic_store(&natives_loaded, 0);
        return unavailable(guest_hook_status_text(status));
    }
    hooked_image = image;
    picker_return_offset = sites.picker_return;
    atomic_store(&variant_state, (uint_fast64_t)time(NULL) * 0x9e3779b97f4a7c15u | 1);
    puts("Character messages: default previews on");
    if (stored_note) printf("Character messages: stored defaults not translated, %s\n", stored_note);
    else puts("Character messages: stored defaults translated on display");
    return 1;
}
