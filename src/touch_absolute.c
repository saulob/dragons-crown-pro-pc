#include "touch_absolute.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "dcp_pad_buttons.h"

#define ANY (-1)

/* The touch pointer update: the controller's option record, its cursor control mode, and the branch that leaves when
 * the mode is Disable. The read is the second call. */
static const short kUpdateRead[] = {
    0xe8, ANY, ANY, ANY, ANY,       /* call option record */
    0x48, 0x89, 0xc7,               /* mov rdi,rax */
    0xe8, ANY, ANY, ANY, ANY,       /* call cursor control mode */
    0x41, 0x89, 0xc7,               /* mov r15d,eax */
    0x41, 0x83, 0xff, 0x04,         /* cmp r15d,4 */
    0x0f, 0x84,                     /* je out */
};
/* The pointer step: the same read, kept in a local, and the Disable test. */
static const short kStepRead[] = {
    0xe8, ANY, ANY, ANY, ANY,       /* call option record */
    0x48, 0x89, 0xc7,               /* mov rdi,rax */
    0xe8, ANY, ANY, ANY, ANY,       /* call cursor control mode */
    0x89, 0x85, ANY, ANY, ANY, ANY, /* mov [rbp-..],eax */
    0x83, 0xf8, 0x04,               /* cmp eax,4 */
    0x75,                           /* jne on */
};
enum { kReadInWindow = 8 };

/* movzx eax,byte [rdi+3]; and eax,0xf; ret */
static const short kModeBody[] = {0x0f, 0xb6, 0x47, 0x03, 0x83, 0xe0, 0x0f, 0xc3};

/* Absolute position in the touch pointer update: the centred point times the gain, and the vertical offset as screen
 * height times two constants. */
static const short kAbsolute[] = {
    0xc5, 0xfa, 0x10, 0x0d, ANY, ANY, ANY, ANY, /* vmovss xmm1,[gain] */
    0xc5, 0xfa, 0x59, 0xd1,                     /* vmulss xmm2,xmm0,xmm1 */
    0xc5, 0xfa, 0x16, 0xc0,                     /* vmovshdup xmm0,xmm0 */
    0xc5, 0xfa, 0x59, 0xc1,                     /* vmulss xmm0,xmm0,xmm1 */
    0xc5, 0xf8, 0x29, 0x55, ANY,                /* vmovaps [rbp-..],xmm2 */
    0xc5, 0xfa, 0x11, 0x45, ANY,                /* vmovss [rbp-..],xmm0 */
    0xe8, ANY, ANY, ANY, ANY,                   /* call input manager */
    0x48, 0x8b, 0x40, 0x40,                     /* mov rax,[rax+0x40] */
    0xc5, 0xf8, 0x28, 0x4d, ANY,                /* vmovaps xmm1,[rbp-..] */
    0xc5, 0xfa, 0x10, 0x40, 0x40,               /* vmovss xmm0,[rax+0x40]: screen height */
    0xc5, 0xfa, 0x59, 0x05, ANY, ANY, ANY, ANY, /* vmulss xmm0,xmm0,[offset a] */
    0xc5, 0xfa, 0x59, 0x05, ANY, ANY, ANY, ANY, /* vmulss xmm0,xmm0,[offset b] */
};
enum {
    kGainDisp = 4,
    kOffsetADisp = 53,
    kOffsetBDisp = 61,
    kAbsoluteSearch = 0x400 /* bytes after the update's read */
};

#define COUNT(a) (sizeof(a) / sizeof(*(a)))

static int shape_at(const unsigned char *image, uint64_t size, uint64_t at, const short *shape, size_t count) {
    if (at > size || size - at < count) return 0;
    for (size_t i = 0; i < count; ++i)
        if (shape[i] != ANY && image[at + i] != (unsigned char)shape[i]) return 0;
    return 1;
}

static int64_t rel_target(const unsigned char *image, uint64_t disp_at, uint64_t end) {
    int32_t rel;
    memcpy(&rel, image + disp_at, sizeof(rel));
    return (int64_t)end + rel;
}

static int read_matches(const unsigned char *image, uint64_t size, uint64_t at, const short *shape, size_t count) {
    if (!shape_at(image, size, at, shape, count)) return 0;
    const int64_t mode = rel_target(image, at + kReadInWindow + 1, at + kReadInWindow + 5);
    return mode >= 0 && shape_at(image, size, (uint64_t)mode, kModeBody, COUNT(kModeBody));
}

/* The one place `shape` appears in [from, to); 0 when nowhere or more than once. */
static uint64_t find_once(const unsigned char *image, uint64_t size, uint64_t from, uint64_t to, const short *shape,
                          size_t count, int read) {
    uint64_t found = 0;
    int matches = 0;
    if (to > size) to = size;
    for (uint64_t at = from; at + count <= to; ++at) {
        if (image[at] != (unsigned char)shape[0]) continue;
        if (read ? !read_matches(image, size, at, shape, count) : !shape_at(image, size, at, shape, count)) continue;
        if (matches++) return 0;
        found = at;
    }
    return matches == 1 ? found : 0;
}

static int read_float(const unsigned char *image, uint64_t size, uint64_t disp_at, uint64_t end, float *value) {
    const int64_t at = rel_target(image, disp_at, end);
    if (at < 0 || (uint64_t)at > size || size - (uint64_t)at < sizeof(float)) return 0;
    memcpy(value, image + at, sizeof(*value));
    return isfinite(*value);
}

int touch_absolute_locate(const unsigned char *image, uint64_t size, TouchAbsoluteSites *sites) {
    memset(sites, 0, sizeof(*sites));
    if (!image || !size) return 0;
    const uint64_t update = find_once(image, size, 0, size, kUpdateRead, COUNT(kUpdateRead), 1);
    const uint64_t step = find_once(image, size, 0, size, kStepRead, COUNT(kStepRead), 1);
    if (!update || !step) return 0;
    const uint64_t absolute =
        find_once(image, size, update, update + kAbsoluteSearch, kAbsolute, COUNT(kAbsolute), 0);
    if (!absolute) return 0;
    float gain = 0, offset_a = 0, offset_b = 0;
    if (!read_float(image, size, absolute + kGainDisp, absolute + kGainDisp + 4, &gain) ||
        !read_float(image, size, absolute + kOffsetADisp, absolute + kOffsetADisp + 4, &offset_a) ||
        !read_float(image, size, absolute + kOffsetBDisp, absolute + kOffsetBDisp + 4, &offset_b))
        return 0;
    const float offset = offset_a * offset_b;
    if (!(gain >= 0.5f && gain <= 4.0f) || !(offset >= -0.5f && offset <= 0.5f)) return 0;
    sites->sites[0] = update + kReadInWindow;
    sites->sites[1] = step + kReadInWindow;
    sites->map = (TouchAbsoluteMap){gain, gain, offset};
    return 1;
}

size_t touch_absolute_stub_bytes(void) {
    return TOUCH_ABSOLUTE_SITES * guest_hook_stub_size();
}

int touch_absolute_mode(int saved, int contact) {
    return contact && saved != TOUCH_ABSOLUTE_MODE_DISABLED ? TOUCH_ABSOLUTE_MODE_ABSOLUTE : saved;
}

static float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

static uint16_t surface_axis(float screen, float gain, float offset, int resolution, int max) {
    const float touch = clamp01(0.5f + (screen - 0.5f - offset) / gain);
    const long units = lroundf(touch * (float)resolution);
    return (uint16_t)(units > max ? max : units);
}

void touch_absolute_map_point(const TouchAbsoluteMap *map, uint16_t x, uint16_t y, uint16_t *out_x, uint16_t *out_y) {
    const float sx = clamp01((float)x / (float)DCP_TOUCH_MAX_X), sy = clamp01((float)y / (float)DCP_TOUCH_MAX_Y);
    *out_x = surface_axis(sx, map->gain_x, 0.0f, DCP_TOUCH_RES_X, DCP_TOUCH_MAX_X);
    *out_y = surface_axis(sy, map->gain_y, map->offset_y, DCP_TOUCH_RES_Y, DCP_TOUCH_MAX_Y);
}

static TouchAbsoluteMap g_map;
static atomic_int g_ready;
static atomic_int g_contact;

int touch_absolute_ready(void) {
    return atomic_load_explicit(&g_ready, memory_order_acquire);
}

void touch_absolute_set_contact(int contact) {
    atomic_store_explicit(&g_contact, contact != 0, memory_order_relaxed);
}

void touch_absolute_remap(uint16_t *x, uint16_t *y) {
    if (touch_absolute_ready()) touch_absolute_map_point(&g_map, *x, *y, x, y);
}

void GUEST_HOOK_ABI touch_absolute_read_mode(GuestRegs *regs) {
    const unsigned char *record = (const unsigned char *)(uintptr_t)regs->rdi;
    const int saved = record[3] & 0x0f;
    regs->rax = (uint64_t)(unsigned)touch_absolute_mode(saved, atomic_load_explicit(&g_contact, memory_order_relaxed));
    regs->skip = 1;
}

static int unavailable(const char *game, const char *reason) {
    printf("Touchscreen pointer: touchpad style for %s: %s\n", game ? game : "an unknown game build", reason);
    return 0;
}

int touch_absolute_install(unsigned char *image, uint64_t size, uint64_t entry, const char *game, unsigned char *stubs,
                           size_t stubs_size) {
    if (!game) return unavailable(game, "not a supported build");
    TouchAbsoluteSites sites;
    if (!touch_absolute_locate(image, size, &sites)) return unavailable(game, "pointer update not found exactly once");
    GuestHook hooks[TOUCH_ABSOLUTE_SITES] = {
        {"touch pointer mode (update)", (uint32_t)sites.sites[0], TOUCH_ABSOLUTE_SITE_BYTES, {0}, touch_absolute_read_mode},
        {"touch pointer mode (step)", (uint32_t)sites.sites[1], TOUCH_ABSOLUTE_SITE_BYTES, {0}, touch_absolute_read_mode},
    };
    for (int i = 0; i < TOUCH_ABSOLUTE_SITES; ++i) memcpy(hooks[i].original, image + sites.sites[i], TOUCH_ABSOLUTE_SITE_BYTES);
    size_t bad = 0;
    const GuestHookStatus status =
        guest_hooks_install(image, size, entry, NULL, hooks, TOUCH_ABSOLUTE_SITES, stubs, stubs_size, &bad);
    if (status != GUEST_HOOK_OK) return unavailable(game, guest_hook_status_text(status));
    g_map = sites.map;
    atomic_store_explicit(&g_ready, 1, memory_order_release);
    printf("Touchscreen pointer: direct (%s)\n", game);
    return 1;
}
