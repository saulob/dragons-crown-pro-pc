#ifndef DCP_ISA_EMULATE_H
#define DCP_ISA_EMULATE_H
#include <stddef.h>
#include <stdint.h>

/* gpr uses x86 register numbering (rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8-r15). Executes the MOVBE at
 * code (15 readable bytes), whose address is rip, and returns its length; 0 leaves gpr and memory untouched. */
size_t isa_emulate_movbe(const uint8_t *code, uint64_t rip, uint64_t gpr[16]);
/* The same for MOVBE and the BMI1 instructions (ANDN, BEXTR, BLSI, BLSMSK, BLSR), which also update rflags. */
size_t isa_emulate(const uint8_t *code, uint64_t rip, uint64_t gpr[16], uint64_t *rflags);

/* CPUID leaf 1 ECX, leaf 7 EBX and leaf 0x80000001 ECX. */
typedef struct { uint32_t leaf1_ecx, leaf7_ebx, ext1_ecx; } IsaHostFeatures;
IsaHostFeatures isa_host_features(void);
/* The Jaguar instruction sets the host lacks, comma separated in out: the ones the loader works around (isa_emulate,
 * or lzcnt_compat for LZCNT) when emulated is nonzero, otherwise the ones the game cannot run without. Returns how
 * many. */
int isa_missing_sets(IsaHostFeatures host, int emulated, char *out, size_t size);
/* The CPUID brand string. */
void isa_host_name(char *out, size_t size);

#endif
