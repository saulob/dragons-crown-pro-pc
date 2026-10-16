/* Hooks on the loaded game image: a few bytes at a verified site become a jump to a stub that saves the registers,
 * calls a host handler, restores them, runs the replaced instructions and returns to the next one. Nothing is written
 * to eboot.bin; the handler decides what the hook does, so an idle hook leaves the game running as it was. */
#ifndef DCP_GUEST_HOOK_H
#define DCP_GUEST_HOOK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GUEST_HOOK_ABI __attribute__((sysv_abi))

/* The guest's registers at the hook, in memory order. A handler may change them: the stub loads them back. rbp and
 * the other callee-saved ones are here too, because the hooked code keeps its locals behind them. */
typedef struct {
    uint8_t xmm[8][16];
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8, rdi, rsi, rdx, rcx, rax, rflags;
    uint64_t skip; /* set by a handler at a function's first instruction: return to the caller now, rax as left */
} GuestRegs;

typedef void (GUEST_HOOK_ABI *GuestHookHandler)(GuestRegs *regs);

#define GUEST_HOOK_MAX_LENGTH 16
#define GUEST_HOOK_MIN_LENGTH 5 /* a jmp rel32 */

typedef struct {
    const char *name;
    uint32_t offset;     /* image offset of the first replaced byte */
    uint8_t length;      /* whole instructions, GUEST_HOOK_MIN_LENGTH..GUEST_HOOK_MAX_LENGTH, no relative operands */
    uint8_t original[GUEST_HOOK_MAX_LENGTH];
    GuestHookHandler handler;
} GuestHook;

/* The image of the one supported game version. */
typedef struct {
    uint64_t size;
    uint64_t entry;
} GuestImageId;

typedef enum {
    GUEST_HOOK_OK = 0,
    GUEST_HOOK_WRONG_IMAGE,   /* size or entry is not the supported version's */
    GUEST_HOOK_BAD_HOOK,      /* length out of range, no handler */
    GUEST_HOOK_OUT_OF_RANGE,  /* a site outside the image */
    GUEST_HOOK_MISMATCH,      /* the bytes are not the original ones */
    GUEST_HOOK_OVERLAP,       /* two sites share bytes */
    GUEST_HOOK_NO_ROOM        /* the stub area is too small or too far from the image */
} GuestHookStatus;

const char *guest_hook_status_text(GuestHookStatus status);

/* Bytes of stub area one hook needs. */
size_t guest_hook_stub_size(void);

/* Checks the image against the supported version and every site, without writing. `bad` gets the index of the hook
 * that failed. */
GuestHookStatus guest_hooks_verify(const uint8_t *image, uint64_t size, uint64_t entry, const GuestImageId *id,
                                   const GuestHook *hooks, size_t count, size_t *bad);

/* Verifies, then writes the stubs into `stubs` (within 2 GiB of the image; executable once the caller protects it)
 * and the jumps into the image. Nothing is written when a check fails. */
GuestHookStatus guest_hooks_install(uint8_t *image, uint64_t size, uint64_t entry, const GuestImageId *id,
                                    const GuestHook *hooks, size_t count, uint8_t *stubs, size_t stubs_size,
                                    size_t *bad);

#ifdef __cplusplus
}
#endif

#endif
