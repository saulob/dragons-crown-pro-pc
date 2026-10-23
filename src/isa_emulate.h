#ifndef DCP_ISA_EMULATE_H
#define DCP_ISA_EMULATE_H
#include <stddef.h>
#include <stdint.h>

/* gpr uses x86 register numbering (rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8-r15). Executes the MOVBE at
 * code (15 readable bytes), whose address is rip, and returns its length; 0 leaves gpr and memory untouched. */
size_t isa_emulate_movbe(const uint8_t *code, uint64_t rip, uint64_t gpr[16]);

#endif
