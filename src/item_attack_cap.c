#include "item_attack_cap.h"

#include <stdio.h>
#include <string.h>

#define ANY (-1)
#define SEARCHED 0 /* no local executable: the site is the one the signature finds */

/* The weapon branch of the validator: the attack pair, the level's row of the cap table, then each byte stored back when it
 * is above the cap. The two stores are what the port removes. */
static const short kWindow[ITEM_ATTACK_CAP_WINDOW_BYTES] = {
    0x41, 0x0f, 0xb7, 0x54, 0x24, 0x50, 0xb9, 0x62, 0x00, 0x00, 0x00, 0x89, 0xd0, 0xc1, 0xe8, 0x08, 0x83, 0xfb, 0x62, 0x0f,
    0x42, 0xcb, 0x31, 0xf6, 0x84, 0xdb, 0x89, 0xc9, 0x48, 0x0f, 0x45, 0xf1, 0x48, 0x6b, 0xce, 0x58, 0x48, 0x8d, 0x35, ANY,
    ANY,  ANY,  ANY,  0x8a, 0x0c, 0x31, 0x38, 0xca, 0x76, 0x05, 0x41, 0x88, 0x4c, 0x24, 0x50, 0x38, 0xc8, 0x0f, 0x86, ANY,
    ANY,  ANY,  ANY,  0x41, 0x88, 0x4c, 0x24, 0x51, 0xe9, ANY,  ANY,  ANY,  ANY,
};
enum { kTableLeaEnd = 43, kTableRowBytes = 0x58, kTableRows = 99 };
static const uint8_t kNop5[ITEM_ATTACK_CAP_STORE_BYTES] = {0x0f, 0x1f, 0x44, 0x00, 0x00};

typedef struct {
    const char *game;
    uint64_t window;
} BuildSite;

static const BuildSite kBuilds[] = {
    {"CUSA10454-1.00", 0x239a8},
    {"CUSA10454-1.01", 0x239a8},
    {"CUSA07001-1.00", 0x236e8},
    {"CUSA07001-1.02", 0x23708},
    {"CUSA07001-1.03", 0x23708},
    {"CUSA10209-1.00", 0x235c8},
    {"CUSA10209-1.02", SEARCHED},
    {"CUSA10487-1.03", SEARCHED},
};

/* The level cap table the window reads: the first byte of each 0x58 byte row, 5 at level 1 up to 255 at level 99. */
static int cap_table_at(const unsigned char *image, uint64_t size, uint64_t window) {
    int32_t disp;
    memcpy(&disp, image + window + kTableLeaEnd - 4, sizeof(disp));
    const int64_t table = (int64_t)window + kTableLeaEnd + disp;
    if (table < 0 || (uint64_t)table + (uint64_t)kTableRowBytes * kTableRows > size) return 0;
    const unsigned char *rows = image + table;
    return rows[0] == 5 && rows[kTableRowBytes] == 7 && rows[kTableRowBytes * (kTableRows - 1)] == 255;
}

static int window_at(const unsigned char *image, uint64_t size, uint64_t at) {
    if (at > size || size - at < ITEM_ATTACK_CAP_WINDOW_BYTES) return 0;
    for (size_t i = 0; i < ITEM_ATTACK_CAP_WINDOW_BYTES; ++i)
        if (kWindow[i] != ANY && image[at + i] != (unsigned char)kWindow[i]) return 0;
    return cap_table_at(image, size, at);
}

/* The one place in the image the window appears; 0 when it appears nowhere or more than once. */
static uint64_t find_window(const unsigned char *image, uint64_t size) {
    uint64_t found = 0;
    int matches = 0;
    for (uint64_t at = 0; at + ITEM_ATTACK_CAP_WINDOW_BYTES <= size; ++at) {
        if (image[at] != kWindow[0] || !window_at(image, size, at)) continue;
        if (matches++) return 0;
        found = at;
    }
    return found;
}

int item_attack_cap_locate(const unsigned char *image, uint64_t size, const char *game, uint64_t *window) {
    *window = 0;
    for (size_t i = 0; game && i < sizeof(kBuilds) / sizeof(*kBuilds); ++i) {
        if (strcmp(game, kBuilds[i].game)) continue;
        if (kBuilds[i].window == SEARCHED) *window = find_window(image, size);
        else *window = window_at(image, size, kBuilds[i].window) ? kBuilds[i].window : 0;
        return 1;
    }
    return 0;
}

static int unavailable(const char *game, const char *reason) {
    printf("Item attack cap: not applied for %s: %s\n", game ? game : "an unknown game build", reason);
    return 0;
}

int item_attack_cap_install(unsigned char *image, uint64_t size, const char *game) {
    uint64_t window = 0;
    if (!item_attack_cap_locate(image, size, game, &window)) return unavailable(game, "not a supported build");
    if (!window) return unavailable(game, "the validator's weapon clamp was not found where the build has it");
    memcpy(image + window + ITEM_ATTACK_CAP_STORE_MIN, kNop5, sizeof(kNop5));
    memcpy(image + window + ITEM_ATTACK_CAP_STORE_MAX, kNop5, sizeof(kNop5));
    printf("Item attack cap: weapon Attack kept on load (%s, site 0x%llx)\n", game, (unsigned long long)window);
    return 1;
}
