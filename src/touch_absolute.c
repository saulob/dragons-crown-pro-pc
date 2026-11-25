#include "touch_absolute.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "dcp_pad_buttons.h"

#define ANY (-1)

/* Pointer update and step: the player's controller port in esi (0xff: none), the port's option record, its cursor control
 * mode, and the Disable test. The last call is the hooked read. */
static const short kUpdateRead[] = {
    0xbe, 0xff, 0x00, 0x00, 0x00,                   /* mov esi,0xff */
    0x83, 0xfb, 0x03, 0x77, 0x18,                   /* cmp ebx,3; ja */
    0x48, 0x8b, 0x80, 0x80, 0x00, 0x00, 0x00,       /* mov rax,[rax+0x80] */
    0x48, 0x63, 0xcb,                               /* movsxd rcx,ebx */
    0x48, 0x69, 0xc9, 0xb0, 0x23, 0x00, 0x00,       /* imul rcx,rcx,0x23b0 */
    0x8b, 0xb4, 0x08, 0x84, 0x22, 0x00, 0x00,       /* mov esi,[rax+rcx+0x2284] */
    0x4c, 0x89, 0xff,                               /* mov rdi,r15 */
    0xe8, ANY, ANY, ANY, ANY,                       /* call option record */
    0x48, 0x89, 0xc7,                               /* mov rdi,rax */
    0xe8, ANY, ANY, ANY, ANY,                       /* call cursor control mode */
    0x41, 0x89, 0xc7,                               /* mov r15d,eax */
    0x41, 0x83, 0xff, 0x04, 0x0f, 0x84,             /* cmp r15d,4; je */
};
static const short kStepRead[] = {
    0xbe, 0xff, 0x00, 0x00, 0x00,                   /* mov esi,0xff */
    0x41, 0x83, 0xff, 0x03, 0x77, 0x18,             /* cmp r15d,3; ja */
    0x48, 0x8b, 0x80, 0x80, 0x00, 0x00, 0x00,       /* mov rax,[rax+0x80] */
    0x49, 0x63, 0xcf,                               /* movsxd rcx,r15d */
    0x48, 0x69, 0xc9, 0xb0, 0x23, 0x00, 0x00,       /* imul rcx,rcx,0x23b0 */
    0x8b, 0xb4, 0x08, 0x84, 0x22, 0x00, 0x00,       /* mov esi,[rax+rcx+0x2284] */
    0x4c, 0x89, 0xef,                               /* mov rdi,r13 */
    0xe8, ANY, ANY, ANY, ANY,                       /* call option record */
    0x48, 0x89, 0xc7,                               /* mov rdi,rax */
    0xe8, ANY, ANY, ANY, ANY,                       /* call cursor control mode */
    0x89, 0x85, ANY, ANY, ANY, ANY,                 /* mov [rbp-..],eax */
    0x83, 0xf8, 0x04, 0x75,                         /* cmp eax,4; jne */
};
enum { kUpdateOptionCall = 37, kStepOptionCall = 38, kModeCallAfterOption = 8 };

/* The option record of port esi: never writes rsi, so the hook still finds the port there. */
static const short kOptionRecord[] = {
    0x89, 0xf0, 0x83, 0xfe, 0x03, 0x77, 0x10,       /* mov eax,esi; cmp esi,3; ja */
    0x48, 0x8d, 0x0d, ANY, ANY, ANY, ANY,           /* lea rcx,[table] */
    0x48, 0x63, 0x04, 0x81, 0x48, 0x01, 0xc8,       /* movsxd rax,[rcx+rax*4]; add rax,rcx */
    0xff, 0xe0,                                     /* jmp rax */
    0x48, 0x83, 0xc7, 0x02, 0x48, 0x89, 0xf8, 0xc3, /* lea rax,[rdi+2]; ret */
    0x48, 0x83, 0xc7, 0x06, 0x48, 0x89, 0xf8, 0xc3,
    0x48, 0x83, 0xc7, 0x0a, 0x48, 0x89, 0xf8, 0xc3,
    0x48, 0x83, 0xc7, 0x0e, 0x48, 0x89, 0xf8, 0xc3,
};

/* The same esi picks the touch record, and that record's port picks the pad the engine reads by its handle. */
static const short kTouchRecord[] = {
    0x89, 0xf0, 0x48, 0x6b, 0xc8, 0x34, 0x31, 0xc0, /* mov eax,esi; imul rcx,rax,0x34; xor eax,eax */
    0x83, 0xfe, 0x03,                               /* cmp esi,3 */
    0x48, 0x8d, 0x8c, 0x0f, 0x8c, 0x24, 0x00, 0x00, /* lea rcx,[rdi+rcx+0x248c] */
    0x48, 0x0f, 0x46, 0xc1, 0xc3,                   /* cmovbe rax,rcx; ret */
};
static const short kTouchFill[] = {
    0x44, 0x8b, 0x73, 0x30,                         /* mov r14d,[rbx+0x30]: port */
    0xe8, ANY, ANY, ANY, ANY,                       /* call engine */
    0x48, 0x8b, 0x40, 0x68,                         /* mov rax,[rax+0x68]: pads */
    0x4d, 0x69, 0xf6, 0xd0, 0x01, 0x00, 0x00,       /* imul r14,r14,0x1d0 */
};
static const short kPadRead[] = {
    0x43, 0x8b, 0x5c, 0x3d, 0x44,                   /* mov ebx,[r13+r15+0x44]: handle */
    0x85, 0xdb, 0x0f, 0x88, ANY, ANY, ANY, ANY,     /* test ebx,ebx; js */
    0x89, 0xdf, 0x4c, 0x89, 0xf6, 0xe8,             /* scePadReadState(handle, ..) */
};
static const short kEngine[] = {0x48, 0x8b, 0x05, ANY, ANY, ANY, ANY, 0xc3}; /* mov rax,[engine]; ret */
enum {
    kPorts = 4,
    kTouchRecordPort0 = 0x248c + 0x30,
    kTouchRecordBytes = 0x34,
    kEnginePads = 0x68,
    kPadBytes = 0x1d0,
    kPadHandle = 0x44,
    kEngineCall = 4
};

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
    0xe8, ANY, ANY, ANY, ANY,                   /* call engine */
    0x48, 0x8b, 0x40, 0x40,                     /* mov rax,[rax+0x40] */
    0xc5, 0xf8, 0x28, 0x4d, ANY,                /* vmovaps xmm1,[rbp-..] */
    0xc5, 0xfa, 0x10, 0x40, 0x40,               /* vmovss xmm0,[rax+0x40]: screen height */
    0xc5, 0xfa, 0x59, 0x05, ANY, ANY, ANY, ANY, /* vmulss xmm0,xmm0,[offset a] */
    0xc5, 0xfa, 0x59, 0x05, ANY, ANY, ANY, ANY, /* vmulss xmm0,xmm0,[offset b] */
};
enum {
    kGainDisp = 4,
    kAbsoluteEngineCall = 30,
    kOffsetADisp = 53,
    kOffsetBDisp = 61,
    kAbsoluteSearch = 0x400
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

static int64_t call_target(const unsigned char *image, uint64_t call) {
    return rel_target(image, call + 1, call + 5);
}

static int target_matches(const unsigned char *image, uint64_t size, int64_t target, const short *shape, size_t count) {
    return target >= 0 && shape_at(image, size, (uint64_t)target, shape, count);
}

static int read_matches(const unsigned char *image, uint64_t size, uint64_t option_call) {
    return target_matches(image, size, call_target(image, option_call), kOptionRecord, COUNT(kOptionRecord)) &&
           target_matches(image, size, call_target(image, option_call + kModeCallAfterOption), kModeBody, COUNT(kModeBody));
}

/* The one place `shape` appears in [from, to); 0 when nowhere or more than once. */
static uint64_t find_once(const unsigned char *image, uint64_t size, uint64_t from, uint64_t to, const short *shape,
                          size_t count) {
    uint64_t found = 0;
    int matches = 0;
    if (to > size) to = size;
    for (uint64_t at = from; at + count <= to; ++at) {
        if (image[at] != (unsigned char)shape[0] || !shape_at(image, size, at, shape, count)) continue;
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

#define FIND(shape) find_once(image, size, 0, size, shape, COUNT(shape))

/* Touch record k is set to port k once, when the records are made. */
static int touch_records_keep_their_port(const unsigned char *image, uint64_t size) {
    for (int port = 0; port < kPorts; ++port) {
        const int field = kTouchRecordPort0 + port * kTouchRecordBytes;
        const short write[] = {0xc7, 0x83, (short)(field & 0xff), (short)(field >> 8), 0, 0, (short)port, 0, 0, 0};
        if (!FIND(write)) return 0;
    }
    return 1;
}

/* The image offset of the engine pointer behind the controller ports; 0 unless esi, the touch record and the pad handle
 * are tied together exactly as expected. */
static uint64_t find_engine(const unsigned char *image, uint64_t size) {
    const uint64_t fill = FIND(kTouchFill);
    if (!fill || !FIND(kTouchRecord) || !FIND(kPadRead) || !touch_records_keep_their_port(image, size)) return 0;
    const int64_t accessor = call_target(image, fill + kEngineCall);
    if (!target_matches(image, size, accessor, kEngine, COUNT(kEngine))) return 0;
    const int64_t engine = rel_target(image, (uint64_t)accessor + 3, (uint64_t)accessor + 7);
    return engine > 0 && (uint64_t)engine <= size && size - (uint64_t)engine >= sizeof(void *) ? (uint64_t)engine : 0;
}

int touch_absolute_locate(const unsigned char *image, uint64_t size, TouchAbsoluteSites *sites) {
    memset(sites, 0, sizeof(*sites));
    if (!image || !size) return 0;
    const uint64_t update = FIND(kUpdateRead), step = FIND(kStepRead);
    if (!update || !step || !read_matches(image, size, update + kUpdateOptionCall) ||
        !read_matches(image, size, step + kStepOptionCall))
        return 0;
    const uint64_t absolute = find_once(image, size, update, update + kAbsoluteSearch, kAbsolute, COUNT(kAbsolute));
    const uint64_t engine = find_engine(image, size);
    if (!absolute || !engine) return 0;
    const int64_t accessor = call_target(image, absolute + kAbsoluteEngineCall);
    if (!target_matches(image, size, accessor, kEngine, COUNT(kEngine)) ||
        rel_target(image, (uint64_t)accessor + 3, (uint64_t)accessor + 7) != (int64_t)engine)
        return 0;
    float gain = 0, offset_a = 0, offset_b = 0;
    if (!read_float(image, size, absolute + kGainDisp, absolute + kGainDisp + 4, &gain) ||
        !read_float(image, size, absolute + kOffsetADisp, absolute + kOffsetADisp + 4, &offset_a) ||
        !read_float(image, size, absolute + kOffsetBDisp, absolute + kOffsetBDisp + 4, &offset_b))
        return 0;
    const float offset = offset_a * offset_b;
    if (!(gain >= 0.5f && gain <= 4.0f) || !(offset >= -0.5f && offset <= 0.5f)) return 0;
    sites->sites[0] = update + kUpdateOptionCall + kModeCallAfterOption;
    sites->sites[1] = step + kStepOptionCall + kModeCallAfterOption;
    sites->engine = engine;
    sites->map = (TouchAbsoluteMap){gain, gain, offset};
    return 1;
}

size_t touch_absolute_stub_bytes(void) {
    return TOUCH_ABSOLUTE_SITES * guest_hook_stub_size();
}

int touch_absolute_mode(int saved, int owns_contact) {
    const int overridable = saved >= TOUCH_ABSOLUTE_MODE_RELATIVE && saved < TOUCH_ABSOLUTE_MODE_DISABLED;
    return owns_contact && overridable ? TOUCH_ABSOLUTE_MODE_ABSOLUTE : saved;
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
static const unsigned char *g_engine;
static atomic_int g_ready;
static atomic_int g_contact_handle;

int touch_absolute_ready(void) {
    return atomic_load_explicit(&g_ready, memory_order_acquire);
}

static const unsigned char *read_pointer(const unsigned char *at) {
    const unsigned char *value = NULL;
    if (at) memcpy(&value, at, sizeof(value));
    return value;
}

/* The pad handle the game opened for a controller port; negative when there is none. */
static int32_t port_handle(uint32_t port) {
    if (port >= kPorts || !touch_absolute_ready()) return -1;
    const unsigned char *engine = read_pointer(g_engine);
    const unsigned char *pads = engine ? read_pointer(engine + kEnginePads) : NULL;
    int32_t handle = -1;
    if (pads) memcpy(&handle, pads + (size_t)port * kPadBytes + kPadHandle, sizeof(handle));
    return handle;
}

void touch_absolute_set_contact(int32_t pad_handle) {
    atomic_store_explicit(&g_contact_handle, pad_handle, memory_order_relaxed);
}

int touch_absolute_port_owns_contact(uint32_t port) {
    const int32_t contact = atomic_load_explicit(&g_contact_handle, memory_order_relaxed);
    return contact > 0 && port_handle(port) == contact;
}

void touch_absolute_remap(int32_t pad_handle, uint16_t *x, uint16_t *y) {
    if (pad_handle <= 0) return;
    for (uint32_t port = 0; port < kPorts; ++port) {
        if (port_handle(port) != pad_handle) continue;
        touch_absolute_map_point(&g_map, *x, *y, x, y);
        return;
    }
}

void GUEST_HOOK_ABI touch_absolute_read_mode(GuestRegs *regs) {
    const unsigned char *record = (const unsigned char *)(uintptr_t)regs->rdi;
    const int saved = record[3] & 0x0f;
    regs->rax = (uint64_t)(unsigned)touch_absolute_mode(saved, touch_absolute_port_owns_contact((uint32_t)regs->rsi));
    regs->skip = 1;
}

static int unavailable(const char *game, const char *reason) {
    printf("Touchscreen mode: PS Vita Direct Touch is not available for %s (%s); using PS4 Touchpad\n",
           game ? game : "an unknown game build", reason);
    return 0;
}

int touch_absolute_install(unsigned char *image, uint64_t size, uint64_t entry, const char *game, unsigned char *stubs,
                           size_t stubs_size) {
    if (!game) return unavailable(game, "unsupported build");
    TouchAbsoluteSites sites;
    if (!touch_absolute_locate(image, size, &sites)) return unavailable(game, "pointer update or controller ports not found exactly once");
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
    g_engine = image + sites.engine;
    atomic_store_explicit(&g_ready, 1, memory_order_release);
    printf("Touchscreen mode: PS Vita Direct Touch (%s)\n", game);
    return 1;
}
