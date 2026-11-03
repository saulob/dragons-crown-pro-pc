#include "menu_buttons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ANY (-1)
#define FLAG_OFFSET 0x134

/* The key setup: loads the flag through the settings getter, then gives the first key to Cross when it is set and to
 * Circle when it is not. */
static const short kReader[] = {
    0x8b, 0x93, 0xa0, 0x00, 0x00, 0x00, 0x83, 0xb8, 0x34, 0x01, 0x00, 0x00, ANY, 0x48, 0x8d, 0x8b, 0xa0, 0x00, 0x00, 0x00,
    0x74, 0x17, 0x83, 0xfa, 0x18, 0x74, 0x6b, 0x83, 0xbb, 0xa4, 0x00, 0x00, 0x00, 0x18, 0x75, 0x27, 0x48, 0x8d, 0x8b, 0xa4,
    0x00, 0x00, 0x00, 0xeb, 0x59, 0x83, 0xfa, 0x18, 0x0f, 0x84, ANY, ANY, ANY, ANY,
};
enum {
    kReaderCount = sizeof(kReader) / sizeof(*kReader),
    kCompareInReader = 12,
    kCrossBranchInReader = 27 + 0x6b,
    kJumpInReader = kReaderCount - 4,
    kGetterBeforeReader = 5
};

static const uint8_t kCrossBranch[] = {0xc7, 0x01, 0x06, 0x00, 0x00, 0x00, 0xfe, 0x83, 0xb5, 0x00, 0x00, 0x00};
static const uint8_t kCircleBranch[] = {0xc7, 0x01, 0x05, 0x00, 0x00, 0x00, 0xfe, 0x83, 0xb5, 0x00, 0x00, 0x00};
static const uint8_t kGetter[] = {0x48, 0x8b, 0x05};

/* The two stores of 1 (a fallback value, then the flag) that end the readers of the system settings. */
static const uint8_t kWriteLoadedByRegister[] = {
    0x41, 0xc7, 0x84, 0x24, 0x30, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x41, 0xc7, 0x84, 0x24, 0x34, 0x01,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xbe, 0x01, 0x00, 0x00, 0x00, 0x4c, 0x89, 0xe7, 0x41, 0x8a, 0x44, 0x24,
    0x5c, 0x49, 0x8b, 0x0c, 0x24, 0xfe, 0xc0, 0x0f, 0xb6, 0xd0, 0xff, 0x91, 0xa0, 0x00, 0x00, 0x00,
};
static const uint8_t kWriteInScene[] = {
    0xc7, 0x83, 0x30, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc7, 0x83, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x49, 0x8b, 0x06, 0x48, 0x3b, 0x45, 0xe8, 0x75, 0x09, 0x48, 0x83, 0xc4, 0x10, 0x5b, 0x41, 0x5e,
    0x5d, 0xc3,
};
enum { kImmediateInRegisterWrite = 20, kImmediateInSceneWrite = 16 };

typedef struct {
    const uint8_t *bytes;
    size_t length, immediate;
} WriteShape;

static const WriteShape kShapes[MENU_BUTTONS_MAX_WRITES] = {
    {kWriteLoadedByRegister, sizeof(kWriteLoadedByRegister), kImmediateInRegisterWrite},
    {kWriteInScene, sizeof(kWriteInScene), kImmediateInSceneWrite},
};

typedef struct {
    const char *game;
    uint64_t reader;
    uint64_t writes[MENU_BUTTONS_MAX_WRITES];
    int write_count;
} BuildSites;

static const BuildSites kBuilds[] = {
    {"CUSA10454-1.00", 0x7ee45, {0x176c8, 0x177e0}, 2},
    {"CUSA07001-1.00", 0x7e775, {0, 0}, 0},
    {"CUSA07001-1.02", 0x7e7c5, {0, 0}, 0},
    {"CUSA07001-1.03", 0x7e7c5, {0, 0}, 0},
    {"CUSA10209-1.00", 0x7e865, {0, 0}, 0},
};

static int enabled_by_name(const char *value, int fallback) {
    if (!value || !*value) return fallback;
    if (!strcmp(value, "1") || !strcmp(value, "on") || !strcmp(value, "true")) return 1;
    if (!strcmp(value, "0") || !strcmp(value, "off") || !strcmp(value, "false")) return 0;
    return fallback;
}

int menu_buttons_wanted(int saved) {
    return enabled_by_name(getenv("DCP_JAPANESE_MENU_BUTTONS"), saved != 0);
}

static int32_t load_rel32(const unsigned char *at) {
    int32_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

static int bytes_at(const unsigned char *image, uint64_t size, int64_t at, const uint8_t *bytes, size_t count) {
    return at >= 0 && (uint64_t)at <= size && size - (uint64_t)at >= count && !memcmp(image + at, bytes, count);
}

static int is_compare_value(unsigned char value) {
    return value == 0x00 || value == 0xff;
}

static int getter_before(const unsigned char *image, uint64_t size, uint64_t reader) {
    if (reader < kGetterBeforeReader || image[reader - kGetterBeforeReader] != 0xe8) return 0;
    const int64_t target = (int64_t)reader + load_rel32(image + reader - 4);
    return bytes_at(image, size, target, kGetter, sizeof(kGetter)) && bytes_at(image, size, target + 7, (const uint8_t *)"\xc3", 1);
}

static int reader_matches(const unsigned char *image, uint64_t size, uint64_t at) {
    if (at > size || size - at < kReaderCount) return 0;
    for (size_t i = 0; i < kReaderCount; ++i)
        if (kReader[i] != ANY && image[at + i] != (unsigned char)kReader[i]) return 0;
    const int64_t circle = (int64_t)at + kReaderCount + load_rel32(image + at + kJumpInReader);
    return is_compare_value(image[at + kCompareInReader]) &&
           bytes_at(image, size, (int64_t)at + kCrossBranchInReader, kCrossBranch, sizeof(kCrossBranch)) &&
           bytes_at(image, size, circle, kCircleBranch, sizeof(kCircleBranch)) && getter_before(image, size, at);
}

static uint64_t find_reader(const unsigned char *image, uint64_t size) {
    uint64_t found = 0;
    int matches = 0;
    for (uint64_t at = 0; at + kReaderCount <= size; ++at) {
        if (image[at] != kReader[0] || !reader_matches(image, size, at)) continue;
        if (matches++) return 0;
        found = at;
    }
    return found;
}

/* Every `mov dword [reg+0x134], 1` of the image, as the offset of its immediate; the count may exceed `max`. */
static int stores_of_one(const unsigned char *image, uint64_t size, uint64_t *immediates, int max) {
    int count = 0;
    for (uint64_t at = 0; at + 2 <= size; ++at) {
        if (image[at] != 0xc7) continue;
        const unsigned char modrm = image[at + 1];
        if ((modrm & 0xc0) != 0x80 || (modrm & 0x38) != 0) continue;
        const uint64_t displacement = at + 2 + ((modrm & 7) == 4);
        if (size < 8 || displacement > size - 8) continue;
        if (load_rel32(image + displacement) != FLAG_OFFSET || load_rel32(image + displacement + 4) != 1) continue;
        if (count < max) immediates[count] = displacement + 4;
        ++count;
    }
    return count;
}

static int write_matches(const unsigned char *image, uint64_t size, const WriteShape *shape, uint64_t immediate) {
    return immediate >= shape->immediate &&
           bytes_at(image, size, (int64_t)(immediate - shape->immediate), shape->bytes, shape->length);
}

/* The two stores in the order of kShapes, each matching its own shape. */
static int sites_from_stores(const unsigned char *image, uint64_t size, const uint64_t *immediates, int count, uint64_t *out) {
    if (count != MENU_BUTTONS_MAX_WRITES) return 0;
    for (int order = 0; order < 2; ++order) {
        if (write_matches(image, size, &kShapes[0], immediates[order]) &&
            write_matches(image, size, &kShapes[1], immediates[1 - order])) {
            out[0] = immediates[order];
            out[1] = immediates[1 - order];
            return 1;
        }
    }
    return 0;
}

static const uint8_t kPromptCompare[] = {0x31, 0xc9, 0x83, 0xb8, 0x34, 0x01, 0x00, 0x00};
static const uint8_t kSetNotEqual[] = {0x0f, 0x95, 0xc1};
static const uint8_t kLoadHead[] = {0x41, 0xbc, 0xff, 0xff, 0xff, 0xff, 0x45, 0x85, 0xff, 0x0f, 0x88};
static const uint8_t kLoadOriginal[] = {0x8b, 0x80, 0x34, 0x01, 0x00, 0x00};
static const uint8_t kLoadWestern[] = {0xb8, 0x01, 0x00, 0x00, 0x00, 0x90};
enum { kPromptSites = 3, kCompareInPrompt = 5 + 8, kLoadAfterCall = 5 + sizeof(kLoadHead) + 4, kSetNotEqualReach = 40 };

static int calls_getter(const unsigned char *image, uint64_t size, uint64_t at, uint64_t getter) {
    return at + 5 <= size && image[at] == 0xe8 && (int64_t)at + 5 + load_rel32(image + at + 1) == (int64_t)getter;
}

static int has_set_not_equal(const unsigned char *image, uint64_t size, uint64_t from) {
    for (int i = 0; i < kSetNotEqualReach; ++i)
        if (bytes_at(image, size, (int64_t)from + i, kSetNotEqual, sizeof(kSetNotEqual))) return 1;
    return 0;
}

/* The other readers of the flag, found through the getter the key setup calls: three prompt pickers that test it
 * against 0 and one that copies it. All five must be there. */
static int find_western_sites(const unsigned char *image, uint64_t size, MenuButtonSites *sites) {
    const uint64_t getter = (uint64_t)((int64_t)sites->reader + load_rel32(image + sites->reader - 4));
    int prompts = 0, loads = 0;
    uint64_t load = 0, found[kPromptSites] = {0};
    for (uint64_t at = 0; at + 64 <= size; ++at) {
        if (image[at] != 0xe8 || !calls_getter(image, size, at, getter)) continue;
        if (bytes_at(image, size, (int64_t)at + 5, kPromptCompare, sizeof(kPromptCompare)) &&
            is_compare_value(image[at + kCompareInPrompt])) {
            if (!has_set_not_equal(image, size, at + kCompareInPrompt + 1)) return 0;
            if (prompts < kPromptSites) found[prompts] = at + kCompareInPrompt;
            ++prompts;
        } else if (bytes_at(image, size, (int64_t)at + 5, kLoadHead, sizeof(kLoadHead)) &&
                   (bytes_at(image, size, (int64_t)at + kLoadAfterCall, kLoadOriginal, sizeof(kLoadOriginal)) ||
                    bytes_at(image, size, (int64_t)at + kLoadAfterCall, kLoadWestern, sizeof(kLoadWestern)))) {
            load = at + kLoadAfterCall;
            ++loads;
        }
    }
    if (prompts != kPromptSites || loads != 1) return 0;
    sites->compares[0] = sites->reader + kCompareInReader;
    for (int i = 0; i < kPromptSites; ++i) sites->compares[i + 1] = found[i];
    sites->load = load;
    return 1;
}

int menu_buttons_locate(const unsigned char *image, uint64_t size, const char *game, MenuButtonSites *sites) {
    memset(sites, 0, sizeof(*sites));
    const BuildSites *listed = NULL;
    for (size_t i = 0; game && i < sizeof(kBuilds) / sizeof(*kBuilds); ++i)
        if (!strcmp(game, kBuilds[i].game)) listed = &kBuilds[i];
    if (!game) return 0;

    uint64_t immediates[MENU_BUTTONS_MAX_WRITES + 1] = {0};
    const int stores = stores_of_one(image, size, immediates, MENU_BUTTONS_MAX_WRITES + 1);
    uint64_t writes[MENU_BUTTONS_MAX_WRITES] = {0};
    const uint64_t reader = listed ? (reader_matches(image, size, listed->reader) ? listed->reader : 0)
                                   : find_reader(image, size);
    if (!reader) return listed != NULL;
    if (stores) {
        if (stores > MENU_BUTTONS_MAX_WRITES || !sites_from_stores(image, size, immediates, stores, writes)) return listed != NULL;
        if (listed && (listed->write_count != stores || writes[0] != listed->writes[0] || writes[1] != listed->writes[1]))
            return 1;
    } else if (listed && listed->write_count) {
        return 1;
    }
    sites->reader = reader;
    sites->write_count = stores;
    memcpy(sites->writes, writes, sizeof(writes));
    sites->western_sites = find_western_sites(image, size, sites);
    return listed != NULL;
}

static int unavailable(const char *game, int japanese, const char *reason) {
    printf("Japanese menu buttons: %s requested but unavailable for %s: %s\n", japanese ? "On" : "Off",
           game ? game : "an unknown game build", reason);
    return 0;
}

int menu_buttons_install(unsigned char *image, uint64_t size, const char *game, int japanese) {
    if (!game) return japanese ? unavailable(game, japanese, "not a supported build") : 0;
    MenuButtonSites sites;
    const int listed = menu_buttons_locate(image, size, game, &sites);
    if (!sites.reader)
        return unavailable(game, japanese, listed ? "the verified sites do not match" : "key setup not found exactly once with the expected stores");
    for (int i = 0; i < sites.write_count; ++i)
        if (image[sites.writes[i]] > 1) return unavailable(game, japanese, "the flag store is not the expected one");
    if (japanese) {
        for (int i = 0; i < sites.write_count; ++i) image[sites.writes[i]] = 0;
        puts(sites.write_count ? "Japanese menu buttons: on, Circle confirms and Cross cancels"
                               : "Japanese menu buttons: on, this build starts with Circle confirming");
        return 1;
    }
    if (sites.write_count) {
        puts("Japanese menu buttons: off, Cross confirms and Circle cancels");
        return 1;
    }
    if (!sites.western_sites) return unavailable(game, japanese, "the readers of the setting do not match");
    for (int i = 0; i < MENU_BUTTONS_COMPARES; ++i) image[sites.compares[i]] = 0xff;
    memcpy(image + sites.load, kLoadWestern, sizeof(kLoadWestern));
    puts("Japanese menu buttons: off, Cross confirms and Circle cancels (set for this build)");
    return 1;
}
