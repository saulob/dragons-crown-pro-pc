#include "ultrawide_capture.h"

#include <stdio.h>
#include <string.h>

#include "dcpgpu.h"

enum {
    PASS_SIZE = 0xb30,
    BUCKET_SETS = 0x2b0,
    BUCKET_SET_SIZE = 0x2d0,
    BUCKET_STRIDE = 0x20,
    SET_PROJECTION = 0x100,
    SET_VIEW = 0x140,
    SET_VIEWPORT = 0x284,
    SET_BUCKETS = 0x2b0,
    APP_RENDERER = 0x40,
    RENDERER_PASSES = 0x20,
    COMMAND_BUFFER_WRITE = 0x10,
    QUAD_KIND = 0x5c,
    QUAD_VERTICES = 0x70,
    QUAD_COUNT = 0x78,
    QUAD_TEXTURE = 0x80,
    QUAD_VERTEX_STRIDE = 0x20,
    QUAD_MAX_VERTICES = 8,
    SUBMIT_FIELDS = 0x8c,
    SUBMIT_TEXTURE_FIELD = 0xc3,
    SUBMIT_QUEUE_CALL = 0xf6,
    EXECUTOR_GETTER_CALL = 0x2a,
    EXECUTOR_LOOP = 0x130,
    EXECUTOR_COMMAND = 0x1b0,
    PASS_LOOKUP_RENDERER = 0x104e2,
    PASS_LOOKUP_CALL = 0x104e8,
    MAX_FRAME_DISTANCE = 0x40000
};

const UltrawideCaptureRecord ultrawide_capture_records[ULTRAWIDE_RECORD_COUNT] = {
    {0x597830, 0x5a3a00}, {0x5958d0, 0x5a1a90}, {0x595800, 0x5a19e0}, {0x5958c0, 0x5a1a90}, {0x595930, 0x5a1af0},
};

static const uint8_t kQueueEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
static const uint8_t kExecutorLoop[] = {0x49, 0x8b, 0x84, 0x24, 0xb8, 0x02, 0x00, 0x00};
static const uint8_t kExecutorCommand[] = {0x49, 0x8b, 0x46, 0x08, 0x4c, 0x89, 0xe6};
static const uint8_t kGetter[] = {0x48, 0x8b, 0x05};
static const uint8_t kRendererLoad[] = {0x48, 0x8b, 0x78, 0x40};
static const uint8_t kPassRange[] = {0x48, 0x8b, 0x47, 0x20, 0x48, 0x8b, 0x57, 0x28};
static const uint8_t kPassStride[] = {0x48, 0x69, 0xf9, 0x30, 0x0b, 0x00, 0x00};
static const uint8_t kQuadFields[] = {0x44, 0x89, 0x73, 0x5c, 0x4c, 0x89, 0x6b, 0x70, 0x4d,
                                      0x85, 0xff, 0x8b, 0x4d, 0xd4, 0x89, 0x4b, 0x78};
static const uint8_t kQuadTexture[] = {0x48, 0x89, 0xb3, 0x80, 0x00, 0x00, 0x00};

static uint64_t image_base, image_size;
static uint64_t app_pointer;
static uint32_t quad_submit_return;
static __thread uint64_t command_buffer;

static uint64_t load64(uint64_t at) {
    uint64_t value;
    memcpy(&value, (const void *)(uintptr_t)at, sizeof(value));
    return value;
}

static uint32_t load32(uint64_t at) {
    uint32_t value;
    memcpy(&value, (const void *)(uintptr_t)at, sizeof(value));
    return value;
}

static float load_float(uint64_t at) {
    float value;
    memcpy(&value, (const void *)(uintptr_t)at, sizeof(value));
    return value;
}

static uint32_t image_offset(uint64_t address) {
    return address >= image_base && address - image_base < image_size ? (uint32_t)(address - image_base) : 0;
}

/* At a function entry the return address is on top of the stack and rbp is still the caller's frame. */
static void read_callers(const GuestRegs *regs, uint32_t callers[4]) {
    const uint64_t rsp = (uint64_t)(uintptr_t)regs + sizeof(GuestRegs);
    callers[0] = image_offset(load64(rsp));
    uint64_t frame = regs->rbp;
    for (int i = 1; i < 4; ++i) {
        if (frame < rsp || frame - rsp > MAX_FRAME_DISTANCE || (frame & 7)) return;
        callers[i] = image_offset(load64(frame + 8));
        const uint64_t next = load64(frame);
        if (next <= frame) return;
        frame = next;
    }
}

/* Called from the quad submit: r15 still holds its model matrix argument. */
static void read_quad(const GuestRegs *regs, DcpCaptureQueued *queued) {
    const uint64_t command = queued->command;
    queued->quad = 1;
    queued->quad_kind = (int32_t)load32(command + QUAD_KIND);
    queued->quad_vertices = (int32_t)load32(command + QUAD_COUNT);
    queued->quad_texture = load64(command + QUAD_TEXTURE);
    queued->quad_matrix = regs->r15 != 0;
    const uint64_t vertices = load64(command + QUAD_VERTICES);
    if (!vertices || queued->quad_vertices < 1 || queued->quad_vertices > QUAD_MAX_VERTICES) return;
    for (int32_t i = 0; i < queued->quad_vertices; ++i) {
        const float x = load_float(vertices + (uint64_t)i * QUAD_VERTEX_STRIDE);
        const float y = load_float(vertices + (uint64_t)i * QUAD_VERTEX_STRIDE + 4);
        if (i == 0 || x < queued->quad_left) queued->quad_left = x;
        if (i == 0 || x > queued->quad_right) queued->quad_right = x;
        if (i == 0 || y < queued->quad_top) queued->quad_top = y;
        if (i == 0 || y > queued->quad_bottom) queued->quad_bottom = y;
    }
}

/* queue(rdi = bucket set, rsi = command, edx = priority), at its entry. */
static void GUEST_HOOK_ABI queue_hook(GuestRegs *regs) {
    if (!dcpgpu_debug_saulo_capture_wanted()) return;
    DcpCaptureQueued queued;
    memset(&queued, 0, sizeof(queued));
    queued.command = regs->rsi;
    queued.bucket_set = regs->rdi;
    queued.priority = (uint32_t)regs->rdx;
    read_callers(regs, queued.callers);
    if (quad_submit_return && queued.callers[0] == quad_submit_return && queued.command) read_quad(regs, &queued);
    dcpgpu_debug_saulo_capture_queued(&queued);
}

/* Before the executor's bucket loop: r15 is the draw command buffer it set the pass viewport in. */
static void GUEST_HOOK_ABI executor_hook(GuestRegs *regs) {
    command_buffer = regs->r15;
}

static void locate_pass(uint64_t set, int32_t *layer, int32_t *index) {
    *layer = -1;
    *index = -1;
    if (!app_pointer) return;
    const uint64_t app = load64(app_pointer);
    const uint64_t renderer = app ? load64(app + APP_RENDERER) : 0;
    if (!renderer) return;
    const uint64_t first = load64(renderer + RENDERER_PASSES), last = load64(renderer + RENDERER_PASSES + 8);
    if (set < first || set >= last) return;
    const uint64_t within = (set - first) % PASS_SIZE;
    if (within < BUCKET_SETS) return;
    *layer = (int32_t)((set - first) / PASS_SIZE);
    *index = (int32_t)((within - BUCKET_SETS) / BUCKET_SET_SIZE);
}

/* Each command of the loop: r12 = bucket set, r14 = its bucket, rbx = the command's index in it. */
static void GUEST_HOOK_ABI command_hook(GuestRegs *regs) {
    if (!command_buffer || !dcpgpu_debug_saulo_capture_wanted()) return;
    const uint64_t set = regs->r12;
    DcpCaptureExecuted executed;
    memset(&executed, 0, sizeof(executed));
    executed.command_buffer = load64(command_buffer + COMMAND_BUFFER_WRITE);
    executed.command = load64(load64(regs->r14 + 8) + regs->rbx * 8);
    executed.bucket_set = set;
    executed.vtable = executed.command ? image_offset(load64(executed.command)) : 0;
    const uint64_t buckets = load64(set + SET_BUCKETS);
    executed.priority = regs->r14 >= buckets ? (uint32_t)((regs->r14 - buckets) / BUCKET_STRIDE) : 0;
    locate_pass(set, &executed.layer, &executed.set_index);
    executed.projection[0] = load_float(set + SET_PROJECTION);
    executed.projection[1] = load_float(set + SET_PROJECTION + 0x14);
    executed.projection[2] = load_float(set + SET_PROJECTION + 0x30);
    executed.projection[3] = load_float(set + SET_PROJECTION + 0x34);
    executed.view_scale = load_float(set + SET_VIEW);
    for (int i = 0; i < 4; ++i) executed.viewport[i] = load_float(set + SET_VIEWPORT + 4 * i);
    dcpgpu_debug_saulo_capture_executed(&executed);
}

static void hook_at(GuestHook *hook, const char *name, uint32_t offset, const uint8_t *original, uint8_t length,
                    GuestHookHandler handler) {
    hook->name = name;
    hook->offset = offset;
    hook->length = length;
    memset(hook->original, 0, sizeof(hook->original));
    memcpy(hook->original, original, length);
    hook->handler = handler;
}

void ultrawide_capture_record_hooks(const UltrawideCaptureRecord *record, GuestHook hooks[ULTRAWIDE_CAPTURE_HOOK_COUNT]) {
    hook_at(&hooks[0], "capture queue", record->queue, kQueueEntry, sizeof(kQueueEntry), queue_hook);
    hook_at(&hooks[1], "capture executor", record->executor + EXECUTOR_LOOP, kExecutorLoop, sizeof(kExecutorLoop),
            executor_hook);
    hook_at(&hooks[2], "capture command", record->executor + EXECUTOR_COMMAND, kExecutorCommand,
            sizeof(kExecutorCommand), command_hook);
}

size_t ultrawide_capture_stub_bytes(void) {
    return ULTRAWIDE_CAPTURE_HOOK_COUNT * guest_hook_stub_size();
}

static int bytes_at(const uint8_t *image, uint64_t size, uint64_t offset, const uint8_t *bytes, size_t length) {
    return offset + length <= size && !memcmp(image + offset, bytes, length);
}

static int call_target(const uint8_t *image, uint64_t size, uint64_t site, uint64_t *target) {
    if (site + 5 > size || image[site] != 0xe8) return 0;
    int32_t rel;
    memcpy(&rel, image + site + 1, sizeof(rel));
    const int64_t to = (int64_t)site + 5 + rel;
    if (to < 0 || (uint64_t)to >= size) return 0;
    *target = (uint64_t)to;
    return 1;
}

/* The pass array: app (what the executor's getter loads) + 0x40 is the renderer, whose passes are a vector at +0x20
 * with 0xb30 bytes each, as the pass setup's lookup reads them. */
static uint64_t find_app_pointer(const uint8_t *image, uint64_t size, const UltrawideCaptureRecord *record) {
    uint64_t getter = 0, lookup = 0;
    if (!call_target(image, size, record->executor + EXECUTOR_GETTER_CALL, &getter) ||
        !bytes_at(image, size, getter, kGetter, sizeof(kGetter)) || getter + 8 > size || image[getter + 7] != 0xc3)
        return 0;
    if (!bytes_at(image, size, PASS_LOOKUP_RENDERER, kRendererLoad, sizeof(kRendererLoad)) ||
        !call_target(image, size, PASS_LOOKUP_CALL, &lookup) ||
        !bytes_at(image, size, lookup, kPassRange, sizeof(kPassRange)) ||
        !bytes_at(image, size, lookup + 0x14, kPassStride, sizeof(kPassStride)))
        return 0;
    int32_t disp;
    memcpy(&disp, image + getter + 3, sizeof(disp));
    const int64_t global = (int64_t)getter + 7 + disp;
    return global >= 0 && (uint64_t)global + 8 <= size ? (uint64_t)global : 0;
}

/* The quad submit stores kind, vertices, count and texture into the command it queues. */
static uint32_t find_quad_return(const uint8_t *image, uint64_t size, const UltrawideRecord *record,
                                 const UltrawideCaptureRecord *capture) {
    uint64_t queue = 0;
    if (!call_target(image, size, record->submit + SUBMIT_QUEUE_CALL, &queue) || queue != capture->queue ||
        !bytes_at(image, size, record->submit + SUBMIT_FIELDS, kQuadFields, sizeof(kQuadFields)) ||
        !bytes_at(image, size, record->submit + SUBMIT_TEXTURE_FIELD, kQuadTexture, sizeof(kQuadTexture)))
        return 0;
    return record->submit + SUBMIT_QUEUE_CALL + 5;
}

int ultrawide_capture_install(uint8_t *image, uint64_t size, uint64_t entry, uint8_t *stubs, size_t stubs_size,
                              char *detail, size_t detail_size) {
    for (size_t i = 0; i < ULTRAWIDE_RECORD_COUNT; ++i) {
        const UltrawideRecord *record = &ultrawide_records[i];
        if (size != record->image.size || entry != record->image.entry) continue;
        const UltrawideCaptureRecord *capture = &ultrawide_capture_records[i];
        GuestHook hooks[ULTRAWIDE_CAPTURE_HOOK_COUNT];
        ultrawide_capture_record_hooks(capture, hooks);
        const uint64_t app = find_app_pointer(image, size, capture);
        const uint32_t quad_return = find_quad_return(image, size, record, capture);
        size_t bad = 0;
        const GuestHookStatus status = guest_hooks_install(image, size, entry, &record->image, hooks,
                                                           ULTRAWIDE_CAPTURE_HOOK_COUNT, stubs, stubs_size, &bad);
        if (status != GUEST_HOOK_OK) {
            snprintf(detail, detail_size, "%s hook: %s", hooks[bad].name, guest_hook_status_text(status));
            return 0;
        }
        image_base = (uint64_t)(uintptr_t)image;
        image_size = size;
        app_pointer = app ? image_base + app : 0;
        quad_submit_return = quad_return;
        snprintf(detail, detail_size, "capture hooks installed (pass ids %s, quad bounds %s)", app ? "yes" : "no",
                 quad_return ? "yes" : "no");
        return 1;
    }
    snprintf(detail, detail_size, "no capture record for this image");
    return 0;
}
