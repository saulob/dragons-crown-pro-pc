#include "kraken_fix.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The tentacle update from its mast read to the code after the broken-flag branch: the read, the compare, the branch
 * that skips the state change when the flag is clear, the state change itself and the position the branch lands on. */
static const unsigned char kWindow[] = {
    0x49, 0x8b, 0x87, 0xd8, 0x38, 0x00, 0x00,                   /* mov rax,[r15+0x38d8] */
    0x80, 0xb8, 0xc1, 0x38, 0x00, 0x00, 0x00,                   /* cmp byte [rax+0x38c1],0 */
    0x74, 0x1b,                                                 /* je position */
    0x49, 0x8b, 0xbf, 0xc0, 0x38, 0x00, 0x00,                   /* mov rdi,[r15+0x38c0] */
    0x80, 0x7f, 0x5b, 0x03,                                     /* cmp byte [rdi+0x5b],3 */
    0x74, 0x0e,                                                 /* je position */
    0x48, 0x8b, 0x07,                                           /* mov rax,[rdi] */
    0xbe, 0x03, 0x00, 0x00, 0x00,                               /* mov esi,3 */
    0xff, 0x90, 0xa8, 0x00, 0x00, 0x00,                         /* call [rax+0xa8] */
    0x49, 0x8b, 0x87, 0xc0, 0x38, 0x00, 0x00,                   /* position: mov rax,[r15+0x38c0] */
    0xc7, 0x80, 0x70, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x48, 0x43, /* mov dword [rax+0xc70],200.0 */
    0xc7, 0x80, 0x74, 0x0c, 0x00, 0x00, 0x00, 0x00, 0xaf, 0xc3, /* mov dword [rax+0xc74],-350.0 */
};
enum { kHookInWindow = 7 };

typedef struct {
    const char *game;
    uint64_t window;
} BuildSite;

#define SEARCHED 0 /* no local executable: the site is the one the signature finds */

static const BuildSite kBuilds[] = {
    {"CUSA10454-1.00", 0x242ec7},
    {"CUSA10454-1.01", 0x242ec7},
    {"CUSA07001-1.00", 0x242457},
    {"CUSA07001-1.02", 0x242797},
    {"CUSA07001-1.03", 0x2427e7},
    {"CUSA10209-1.00", 0x2425e7},
    {"CUSA10209-1.02", SEARCHED},
    {"CUSA10487-1.03", SEARCHED},
};

static const unsigned char kNoMast[KRAKEN_FIX_FLAG_OFFSET + 1];
static void (*g_observer)(uint64_t tentacle);

static int window_at(const unsigned char *image, uint64_t size, uint64_t at) {
    return at <= size && size - at >= sizeof(kWindow) && !memcmp(image + at, kWindow, sizeof(kWindow));
}

/* The one place in the image the update appears; 0 when it appears nowhere or more than once. */
static uint64_t find_window(const unsigned char *image, uint64_t size) {
    uint64_t found = 0;
    int matches = 0;
    for (uint64_t at = 0; at + sizeof(kWindow) <= size; ++at) {
        if (image[at] != kWindow[0] || !window_at(image, size, at)) continue;
        if (matches++) return 0;
        found = at;
    }
    return found;
}

int kraken_fix_locate(const unsigned char *image, uint64_t size, const char *game, uint64_t *site) {
    *site = 0;
    for (size_t i = 0; game && i < sizeof(kBuilds) / sizeof(*kBuilds); ++i) {
        if (strcmp(game, kBuilds[i].game)) continue;
        const uint64_t window = find_window(image, size);
        if (window && (kBuilds[i].window == SEARCHED || window == kBuilds[i].window)) *site = window + kHookInWindow;
        return 1;
    }
    return 0;
}

size_t kraken_fix_stub_bytes(void) {
    return guest_hook_stub_size();
}

void kraken_fix_set_observer(void (*observer)(uint64_t tentacle)) {
    g_observer = observer;
}

void GUEST_HOOK_ABI kraken_fix_null_mast(GuestRegs *regs) {
    if (regs->rax) return;
    regs->rax = (uint64_t)(uintptr_t)kNoMast;
    if (g_observer) g_observer(regs->r15);
}

static int unavailable(const char *game, const char *reason) {
    printf("Kraken fix: unavailable for %s: %s\n", game ? game : "an unknown game build", reason);
    return 0;
}

int kraken_fix_install(unsigned char *image, uint64_t size, uint64_t entry, const char *game, unsigned char *stubs,
                       size_t stubs_size) {
    uint64_t site = 0;
    const int listed = kraken_fix_locate(image, size, game, &site);
    if (!listed) return unavailable(game, "not a supported build");
    if (!site) return unavailable(game, "tentacle update not found exactly once where the build has it");
    GuestHook hook = {"kraken tentacle mast", (uint32_t)site, KRAKEN_FIX_SITE_BYTES, {0}, kraken_fix_null_mast};
    memcpy(hook.original, image + site, KRAKEN_FIX_SITE_BYTES);
    size_t bad = 0;
    const GuestHookStatus status = guest_hooks_install(image, size, entry, NULL, &hook, 1, stubs, stubs_size, &bad);
    if (status != GUEST_HOOK_OK) return unavailable(game, guest_hook_status_text(status));
    printf("Kraken fix: on (%s)\n", game);
    return 1;
}
