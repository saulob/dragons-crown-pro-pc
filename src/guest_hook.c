#include "guest_hook.h"

#include <string.h>

enum { STUB_ALIGN = 16 };

static const uint8_t stub_save[] = {
    0x6a, 0x00,                                     /* push 0: GuestRegs.skip */
    0x9c,                                           /* pushfq */
    0x50, 0x51, 0x52, 0x56, 0x57,                   /* push rax, rcx, rdx, rsi, rdi */
    0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, /* push r8, r9, r10, r11 */
    0x53, 0x55,                                     /* push rbx, rbp */
    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, /* push r12, r13, r14, r15 */
    0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00,       /* sub rsp, 128 */
    0x0f, 0x11, 0x04, 0x24,                         /* movups [rsp], xmm0 */
    0x0f, 0x11, 0x4c, 0x24, 0x10,                   /* xmm1..xmm7 at +16 .. +112 */
    0x0f, 0x11, 0x54, 0x24, 0x20,
    0x0f, 0x11, 0x5c, 0x24, 0x30,
    0x0f, 0x11, 0x64, 0x24, 0x40,
    0x0f, 0x11, 0x6c, 0x24, 0x50,
    0x0f, 0x11, 0x74, 0x24, 0x60,
    0x0f, 0x11, 0x7c, 0x24, 0x70,
    0x48, 0x89, 0xe7,                               /* mov rdi, rsp: the GuestRegs */
    0x48, 0x89, 0xe0,                               /* mov rax, rsp */
    0x48, 0x83, 0xe4, 0xf0,                         /* and rsp, -16 */
    0x50,                                           /* push rax: the old rsp */
    0x48, 0x83, 0xec, 0x08,                         /* sub rsp, 8: aligned for the call */
    0x48, 0xb8,                                     /* mov rax, imm64 (the handler) */
};
static const uint8_t stub_call[] = {
    0xff, 0xd0,                                     /* call rax */
    0x48, 0x83, 0xc4, 0x08,                         /* add rsp, 8 */
    0x5c,                                           /* pop rsp */
    0x0f, 0x10, 0x04, 0x24,                         /* movups xmm0, [rsp] */
    0x0f, 0x10, 0x4c, 0x24, 0x10,
    0x0f, 0x10, 0x54, 0x24, 0x20,
    0x0f, 0x10, 0x5c, 0x24, 0x30,
    0x0f, 0x10, 0x64, 0x24, 0x40,
    0x0f, 0x10, 0x6c, 0x24, 0x50,
    0x0f, 0x10, 0x74, 0x24, 0x60,
    0x0f, 0x10, 0x7c, 0x24, 0x70,
    0x48, 0x81, 0xc4, 0x80, 0x00, 0x00, 0x00,       /* add rsp, 128 */
    0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, /* pop r15, r14, r13, r12 */
    0x5d, 0x5b,                                     /* pop rbp, rbx */
    0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, /* pop r11, r10, r9, r8 */
    0x5f, 0x5e, 0x5a, 0x59, 0x58,                   /* pop rdi, rsi, rdx, rcx, rax */
    0x48, 0x83, 0x7c, 0x24, 0x08, 0x00,             /* cmp qword ptr [rsp+8], 0: skip? */
    0x74, 0x07,                                     /* je run */
    0x9d,                                           /* popfq */
    0x48, 0x8d, 0x64, 0x24, 0x08,                   /* lea rsp, [rsp+8] */
    0xc3,                                           /* ret: back to the caller */
    0x9d,                                           /* run: popfq */
    0x48, 0x8d, 0x64, 0x24, 0x08,                   /* lea rsp, [rsp+8]: no flags touched */
};
static const uint8_t stub_return[] = {0xff, 0x25, 0, 0, 0, 0}; /* jmp [rip+0]; the address follows */

size_t guest_hook_stub_size(void) {
    const size_t used = sizeof(stub_save) + 8 + sizeof(stub_call) + GUEST_HOOK_MAX_LENGTH + sizeof(stub_return) + 8;
    return (used + STUB_ALIGN - 1) / STUB_ALIGN * STUB_ALIGN;
}

const char *guest_hook_status_text(GuestHookStatus status) {
    switch (status) {
    case GUEST_HOOK_OK: return "ok";
    case GUEST_HOOK_WRONG_IMAGE: return "not the supported game image";
    case GUEST_HOOK_BAD_HOOK: return "invalid hook definition";
    case GUEST_HOOK_OUT_OF_RANGE: return "site outside the image";
    case GUEST_HOOK_MISMATCH: return "unexpected bytes at the site";
    case GUEST_HOOK_OVERLAP: return "sites overlap";
    case GUEST_HOOK_NO_ROOM: return "stub area too small or too far";
    }
    return "";
}

GuestHookStatus guest_hooks_verify(const uint8_t *image, uint64_t size, uint64_t entry, const GuestImageId *id,
                                   const GuestHook *hooks, size_t count, size_t *bad) {
    size_t scratch;
    if (!bad) bad = &scratch;
    *bad = 0;
    if (id && (size != id->size || entry != id->entry)) return GUEST_HOOK_WRONG_IMAGE;
    for (size_t i = 0; i < count; ++i) {
        const GuestHook *hook = &hooks[i];
        *bad = i;
        if (!hook->handler || hook->length < GUEST_HOOK_MIN_LENGTH || hook->length > GUEST_HOOK_MAX_LENGTH)
            return GUEST_HOOK_BAD_HOOK;
        if ((uint64_t)hook->offset + hook->length > size) return GUEST_HOOK_OUT_OF_RANGE;
        if (memcmp(image + hook->offset, hook->original, hook->length)) return GUEST_HOOK_MISMATCH;
        for (size_t j = 0; j < i; ++j) {
            const GuestHook *other = &hooks[j];
            if (hook->offset < other->offset + other->length && other->offset < hook->offset + hook->length)
                return GUEST_HOOK_OVERLAP;
        }
    }
    *bad = 0;
    return GUEST_HOOK_OK;
}

static uint8_t *emit(uint8_t *at, const void *data, size_t length) {
    memcpy(at, data, length);
    return at + length;
}

GuestHookStatus guest_hooks_install(uint8_t *image, uint64_t size, uint64_t entry, const GuestImageId *id,
                                    const GuestHook *hooks, size_t count, uint8_t *stubs, size_t stubs_size,
                                    size_t *bad) {
    const GuestHookStatus status = guest_hooks_verify(image, size, entry, id, hooks, count, bad);
    if (status != GUEST_HOOK_OK) return status;
    const size_t stride = guest_hook_stub_size();
    if (count * stride > stubs_size) return GUEST_HOOK_NO_ROOM;
    for (size_t i = 0; i < count; ++i) {
        const int64_t distance =
            (int64_t)(intptr_t)(stubs + i * stride) - (int64_t)(intptr_t)(image + hooks[i].offset + 5);
        if (distance > INT32_MAX || distance < INT32_MIN) return GUEST_HOOK_NO_ROOM;
    }
    for (size_t i = 0; i < count; ++i) {
        const GuestHook *hook = &hooks[i];
        uint8_t *stub = stubs + i * stride;
        uint8_t *at = stub;
        const uint64_t handler = (uint64_t)(uintptr_t)hook->handler;
        const uint64_t resume = (uint64_t)(uintptr_t)(image + hook->offset + hook->length);
        at = emit(at, stub_save, sizeof(stub_save));
        at = emit(at, &handler, 8);
        at = emit(at, stub_call, sizeof(stub_call));
        at = emit(at, hook->original, hook->length);
        at = emit(at, stub_return, sizeof(stub_return));
        at = emit(at, &resume, 8);
        memset(at, 0xcc, (size_t)(stub + stride - at));

        const int32_t rel = (int32_t)((int64_t)(intptr_t)stub - (int64_t)(intptr_t)(image + hook->offset + 5));
        uint8_t *site = image + hook->offset;
        site[0] = 0xe9;
        memcpy(site + 1, &rel, 4);
        memset(site + 5, 0x90, hook->length - 5u);
    }
    return GUEST_HOOK_OK;
}
