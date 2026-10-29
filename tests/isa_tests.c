#include "isa_emulate.h"
#include "lzcnt_compat.h"
#include <cpuid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

static int failures;
#define CHECK(condition) do { if (!(condition)) { printf("FAIL line %d: %s\n", __LINE__, #condition); ++failures; } } while (0)

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

static size_t run(const uint8_t *code, size_t size, uint64_t rip, uint64_t gpr[16]) {
    uint8_t padded[32] = {0};
    memcpy(padded, code, size);
    return isa_emulate_movbe(padded, rip, gpr);
}

static void crash_site(void) {
    const uint8_t code[] = {0x48, 0x0f, 0x38, 0xf1, 0x11};
    uint8_t memory[8] = {0};
    uint64_t gpr[16] = {0};
    gpr[RCX] = (uintptr_t)memory;
    gpr[RDX] = 0x65006e006f004e00ull;
    CHECK(run(code, sizeof code, 0, gpr) == 5);
    const uint8_t expected[8] = {0x65, 0x00, 0x6e, 0x00, 0x6f, 0x00, 0x4e, 0x00};
    CHECK(memcmp(memory, expected, 8) == 0);
    CHECK(gpr[RCX] == (uintptr_t)memory && gpr[RDX] == 0x65006e006f004e00ull);
}

static void load_widths(void) {
    uint8_t memory[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    uint64_t gpr[16];
    const uint8_t load64[] = {0x48, 0x0f, 0x38, 0xf0, 0x00};
    memset(gpr, 0xaa, sizeof gpr);
    gpr[RAX] = (uintptr_t)memory;
    CHECK(run(load64, sizeof load64, 0, gpr) == 5);
    CHECK(gpr[RAX] == 0x1122334455667788ull);

    const uint8_t load32[] = {0x0f, 0x38, 0xf0, 0x08};
    gpr[RAX] = (uintptr_t)memory;
    gpr[RCX] = ~0ull;
    CHECK(run(load32, sizeof load32, 0, gpr) == 4);
    CHECK(gpr[RCX] == 0x11223344u);

    const uint8_t load16[] = {0x66, 0x0f, 0x38, 0xf0, 0x08};
    gpr[RAX] = (uintptr_t)memory;
    gpr[RCX] = 0xffffffffffffffffull;
    CHECK(run(load16, sizeof load16, 0, gpr) == 5);
    CHECK(gpr[RCX] == 0xffffffffffff1122ull);
}

static void store_widths(void) {
    uint8_t memory[8];
    uint64_t gpr[16] = {0};
    const uint8_t store16[] = {0x66, 0x0f, 0x38, 0xf1, 0x08};
    memset(memory, 0xcc, sizeof memory);
    gpr[RAX] = (uintptr_t)memory;
    gpr[RCX] = 0x1234;
    CHECK(run(store16, sizeof store16, 0, gpr) == 5);
    CHECK(memory[0] == 0x12 && memory[1] == 0x34 && memory[2] == 0xcc);

    const uint8_t store32[] = {0x0f, 0x38, 0xf1, 0x08};
    memset(memory, 0xcc, sizeof memory);
    gpr[RCX] = 0xdeadbeef12345678ull;
    CHECK(run(store32, sizeof store32, 0, gpr) == 4);
    CHECK(memory[0] == 0x12 && memory[1] == 0x34 && memory[2] == 0x56 && memory[3] == 0x78 && memory[4] == 0xcc);
}

static void addressing_forms(void) {
    uint8_t memory[256] = {0};
    uint64_t gpr[16] = {0};
    const uint8_t sib_disp8[] = {0x48, 0x0f, 0x38, 0xf1, 0x74, 0x3b, 0x48};
    gpr[RBX] = (uintptr_t)memory;
    gpr[RDI] = 8;
    gpr[RSI] = 0x0102030405060708ull;
    CHECK(run(sib_disp8, sizeof sib_disp8, 0, gpr) == 7);
    CHECK(memory[0x48 + 8] == 0x01 && memory[0x48 + 15] == 0x08);

    const uint8_t extended[] = {0x66, 0x44, 0x0f, 0x38, 0xf1, 0x59, 0x32};
    gpr[RCX] = (uintptr_t)memory;
    gpr[R11] = 0xabcd;
    CHECK(run(extended, sizeof extended, 0, gpr) == 7);
    CHECK(memory[0x32] == 0xab && memory[0x33] == 0xcd);

    const uint8_t r12_base[] = {0x49, 0x0f, 0x38, 0xf0, 0x04, 0x24};
    gpr[R12] = (uintptr_t)memory + 0x50;
    CHECK(run(r12_base, sizeof r12_base, 0, gpr) == 6);
    CHECK(gpr[RAX] == 0x0102030405060708ull);

    const uint8_t scaled_index_r9[] = {0x4a, 0x0f, 0x38, 0xf1, 0x14, 0xc8};
    memset(memory, 0, sizeof memory);
    gpr[RAX] = (uintptr_t)memory;
    gpr[R9] = 2;
    gpr[RDX] = 0x1122334455667788ull;
    CHECK(run(scaled_index_r9, sizeof scaled_index_r9, 0, gpr) == 6);
    CHECK(memory[16] == 0x11 && memory[23] == 0x88);

    const uint8_t no_index[] = {0x48, 0x0f, 0x38, 0xf1, 0x14, 0x24};
    gpr[RSP] = (uintptr_t)memory;
    CHECK(run(no_index, sizeof no_index, 0, gpr) == 6);
    CHECK(memory[0] == 0x11);
}

static void rip_relative(void) {
    uint8_t memory[16] = {0};
    uint64_t gpr[16] = {0};
    const uint8_t code[] = {0x48, 0x0f, 0x38, 0xf1, 0x05, 0x10, 0x00, 0x00, 0x00};
    const uint64_t rip = (uintptr_t)memory;
    gpr[RAX] = 0x0102030405060708ull;
    CHECK(run(code, sizeof code, rip - 0x10 - sizeof code, gpr) == sizeof code);
    CHECK(memory[0] == 0x01 && memory[7] == 0x08);
}

static void rejected(void) {
    uint64_t gpr[16] = {0};
    uint64_t before[16];
    uint8_t memory[8] = {0};
    gpr[RAX] = (uintptr_t)memory;
    memcpy(before, gpr, sizeof gpr);

    const uint8_t register_form[] = {0x48, 0x0f, 0x38, 0xf1, 0xc1};
    const uint8_t crc32[] = {0xf2, 0x48, 0x0f, 0x38, 0xf1, 0x00};
    const uint8_t other_opcode[] = {0x48, 0x0f, 0x38, 0xf2, 0x00};
    const uint8_t not_0f38[] = {0x48, 0x0f, 0x3a, 0xf1, 0x00};
    const uint8_t ud2[] = {0x0f, 0x0b};
    const uint8_t segment[] = {0x65, 0x48, 0x0f, 0x38, 0xf1, 0x00};
    CHECK(run(register_form, sizeof register_form, 0, gpr) == 0);
    CHECK(run(crc32, sizeof crc32, 0, gpr) == 0);
    CHECK(run(other_opcode, sizeof other_opcode, 0, gpr) == 0);
    CHECK(run(not_0f38, sizeof not_0f38, 0, gpr) == 0);
    CHECK(run(ud2, sizeof ud2, 0, gpr) == 0);
    CHECK(run(segment, sizeof segment, 0, gpr) == 0);
    CHECK(memcmp(gpr, before, sizeof gpr) == 0);
    CHECK(memory[0] == 0 && memory[7] == 0);
}

enum { CF = 1 << 0, ZF = 1 << 6, SF = 1 << 7, OF = 1 << 11, OTHER_FLAGS = 0x202 };

static size_t run_flags(const uint8_t *code, size_t size, uint64_t gpr[16], uint64_t *flags) {
    uint8_t padded[32] = {0};
    memcpy(padded, code, size);
    return isa_emulate(padded, 0, gpr, flags);
}

static void strlen_site(void) {
    const uint8_t andn_rax_rax_rdx[] = {0xc4, 0xe2, 0xf8, 0xf2, 0xc2};
    uint64_t gpr[16] = {0}, flags = OTHER_FLAGS | CF | OF | ZF | SF;
    gpr[RAX] = 0x486362694c656353ull;
    gpr[RDX] = 0x476261684b646252ull;
    CHECK(run_flags(andn_rax_rax_rdx, sizeof andn_rax_rax_rdx, gpr, &flags) == 5);
    CHECK(gpr[RAX] == 0x0700010003000000ull);
    CHECK(gpr[RDX] == 0x476261684b646252ull);
    CHECK(flags == OTHER_FLAGS);
}

static void andn_forms(void) {
    uint64_t gpr[16] = {0}, flags = OTHER_FLAGS;
    const uint8_t andn_esi_ecx_eax[] = {0xc4, 0xe2, 0x70, 0xf2, 0xf0};
    gpr[RSI] = ~0ull;
    gpr[RCX] = 0xffffffff0000ffffull;
    gpr[RAX] = 0x123456787fff0000ull;
    CHECK(run_flags(andn_esi_ecx_eax, sizeof andn_esi_ecx_eax, gpr, &flags) == 5);
    CHECK(gpr[RSI] == 0x7fff0000ull);
    CHECK(flags == OTHER_FLAGS);

    uint8_t memory[0x80] = {0};
    const uint32_t value = 0x80000001u;
    memcpy(memory + 0x70, &value, 4);
    const uint8_t andn_eax_eax_r15[] = {0xc4, 0xc2, 0x78, 0xf2, 0x47, 0x70};
    gpr[R15] = (uintptr_t)memory;
    gpr[RAX] = 1;
    CHECK(run_flags(andn_eax_eax_r15, sizeof andn_eax_eax_r15, gpr, &flags) == 6);
    CHECK(gpr[RAX] == 0x80000000ull);
    CHECK(flags == (OTHER_FLAGS | SF));

    const uint8_t andn_r11d_r11d_rbx_disp32[] = {0xc4, 0x62, 0x20, 0xf2, 0x9b, 0x08, 0x69, 0x01, 0x00};
    gpr[RBX] = (uintptr_t)memory + 0x70 - 0x16908;
    gpr[R11] = 0x80000001ull;
    CHECK(run_flags(andn_r11d_r11d_rbx_disp32, sizeof andn_r11d_r11d_rbx_disp32, gpr, &flags) == 9);
    CHECK(gpr[R11] == 0);
    CHECK(flags == (OTHER_FLAGS | ZF));
}

static void bextr_forms(void) {
    uint64_t gpr[16] = {0}, flags = OTHER_FLAGS | CF | OF;
    uint32_t table[4] = {0, 0, 0xabcd1234u, 0};
    const uint8_t bextr_eax_sib_edx[] = {0xc4, 0xe2, 0x68, 0xf7, 0x04, 0x81};
    gpr[RCX] = (uintptr_t)table;
    gpr[RAX] = 2;
    gpr[RDX] = 0x0804;
    CHECK(run_flags(bextr_eax_sib_edx, sizeof bextr_eax_sib_edx, gpr, &flags) == 6);
    CHECK(gpr[RAX] == 0x23);
    CHECK(flags == OTHER_FLAGS);

    const uint8_t bextr_r15d_rbx_r15d[] = {0xc4, 0x62, 0x00, 0xf7, 0x3b};
    gpr[RBX] = (uintptr_t)&table[2];
    gpr[R15] = 0x101c;
    CHECK(run_flags(bextr_r15d_rbx_r15d, sizeof bextr_r15d_rbx_r15d, gpr, &flags) == 5);
    CHECK(gpr[R15] == 0xa);

    const uint8_t bextr_ecx_eax_r14d[] = {0xc4, 0xe2, 0x08, 0xf7, 0xc8};
    gpr[RAX] = 0xffffffff80000000ull;
    gpr[R14] = 0x401f;
    CHECK(run_flags(bextr_ecx_eax_r14d, sizeof bextr_ecx_eax_r14d, gpr, &flags) == 5);
    CHECK(gpr[RCX] == 1);
    gpr[R14] = 0x0820;
    CHECK(run_flags(bextr_ecx_eax_r14d, sizeof bextr_ecx_eax_r14d, gpr, &flags) == 5);
    CHECK(gpr[RCX] == 0 && flags == (OTHER_FLAGS | ZF));
    gpr[R14] = 0x0004;
    CHECK(run_flags(bextr_ecx_eax_r14d, sizeof bextr_ecx_eax_r14d, gpr, &flags) == 5);
    CHECK(gpr[RCX] == 0);

    const uint8_t bextr_rax_rcx_rdx[] = {0xc4, 0xe2, 0xe8, 0xf7, 0xc1};
    gpr[RCX] = 0xf000000000000000ull;
    gpr[RDX] = 0xff3c;
    CHECK(run_flags(bextr_rax_rcx_rdx, sizeof bextr_rax_rcx_rdx, gpr, &flags) == 5);
    CHECK(gpr[RAX] == 0xf);
}

static void bls_forms(void) {
    uint64_t gpr[16] = {0}, flags = OTHER_FLAGS;
    const uint8_t blsi_eax_ecx[] = {0xc4, 0xe2, 0x78, 0xf3, 0xd9};
    gpr[RAX] = ~0ull;
    gpr[RCX] = 0x1234567800000c00ull;
    CHECK(run_flags(blsi_eax_ecx, sizeof blsi_eax_ecx, gpr, &flags) == 5);
    CHECK(gpr[RAX] == 0x400 && flags == (OTHER_FLAGS | CF));
    gpr[RCX] = 0;
    CHECK(run_flags(blsi_eax_ecx, sizeof blsi_eax_ecx, gpr, &flags) == 5);
    CHECK(gpr[RAX] == 0 && flags == (OTHER_FLAGS | ZF));

    const uint8_t blsr_rax_rcx[] = {0xc4, 0xe2, 0xf8, 0xf3, 0xc9};
    gpr[RCX] = 0x8000000000000000ull;
    CHECK(run_flags(blsr_rax_rcx, sizeof blsr_rax_rcx, gpr, &flags) == 5);
    CHECK(gpr[RAX] == 0 && flags == (OTHER_FLAGS | ZF));
    gpr[RCX] = 0;
    CHECK(run_flags(blsr_rax_rcx, sizeof blsr_rax_rcx, gpr, &flags) == 5);
    CHECK(gpr[RAX] == 0 && flags == (OTHER_FLAGS | CF | ZF));

    const uint8_t blsmsk_eax_ecx[] = {0xc4, 0xe2, 0x78, 0xf3, 0xd1};
    gpr[RCX] = 0x18;
    CHECK(run_flags(blsmsk_eax_ecx, sizeof blsmsk_eax_ecx, gpr, &flags) == 5);
    CHECK(gpr[RAX] == 0xf && flags == OTHER_FLAGS);
    gpr[RCX] = 0;
    CHECK(run_flags(blsmsk_eax_ecx, sizeof blsmsk_eax_ecx, gpr, &flags) == 5);
    CHECK(gpr[RAX] == 0xffffffffull && flags == (OTHER_FLAGS | CF | SF));
}

static void bmi1_rejected(void) {
    uint64_t gpr[16], before[16], flags = OTHER_FLAGS;
    for (int i = 0; i < 16; ++i) gpr[i] = 0x1111111111111111ull * (uint64_t)i;
    memcpy(before, gpr, sizeof gpr);
    const uint8_t shlx[] = {0xc4, 0xe2, 0xf9, 0xf7, 0xc1};
    const uint8_t andn_l1[] = {0xc4, 0xe2, 0xfc, 0xf2, 0xc2};
    const uint8_t bls_group0[] = {0xc4, 0xe2, 0x78, 0xf3, 0xc1};
    const uint8_t bls_group4[] = {0xc4, 0xe2, 0x78, 0xf3, 0xe1};
    const uint8_t map_0f[] = {0xc4, 0xe1, 0xf8, 0xf2, 0xc2};
    const uint8_t bzhi[] = {0xc4, 0xe2, 0xf8, 0xf5, 0xc1};
    const uint8_t vex2[] = {0xc5, 0xf8, 0xf2, 0xc2};
    CHECK(run_flags(shlx, sizeof shlx, gpr, &flags) == 0);
    CHECK(run_flags(andn_l1, sizeof andn_l1, gpr, &flags) == 0);
    CHECK(run_flags(bls_group0, sizeof bls_group0, gpr, &flags) == 0);
    CHECK(run_flags(bls_group4, sizeof bls_group4, gpr, &flags) == 0);
    CHECK(run_flags(map_0f, sizeof map_0f, gpr, &flags) == 0);
    CHECK(run_flags(bzhi, sizeof bzhi, gpr, &flags) == 0);
    CHECK(run_flags(vex2, sizeof vex2, gpr, &flags) == 0);
    CHECK(memcmp(gpr, before, sizeof gpr) == 0 && flags == OTHER_FLAGS);
}

static void host_sets(void) {
    char text[64];
    const IsaHostFeatures haswell = {bit_SSE4_1 | bit_SSE4_2 | bit_POPCNT | bit_AVX | bit_MOVBE, bit_BMI, bit_LZCNT};
    CHECK(isa_missing_sets(haswell, 0, text, sizeof text) == 0 && text[0] == 0);
    CHECK(isa_missing_sets(haswell, 1, text, sizeof text) == 0);
    CHECK(!lzcnt_compat_needed(haswell));

    const IsaHostFeatures ivy_bridge = {bit_SSE4_1 | bit_SSE4_2 | bit_POPCNT | bit_AVX, 0, 0};
    CHECK(isa_missing_sets(ivy_bridge, 0, text, sizeof text) == 0 && text[0] == 0);
    CHECK(isa_missing_sets(ivy_bridge, 1, text, sizeof text) == 3 && strcmp(text, "LZCNT, MOVBE, BMI1") == 0);
    CHECK(lzcnt_compat_needed(ivy_bridge));

    const IsaHostFeatures bulldozer = {bit_SSE4_1 | bit_SSE4_2 | bit_POPCNT | bit_AVX, 0, bit_LZCNT};
    CHECK(isa_missing_sets(bulldozer, 0, text, sizeof text) == 0);
    CHECK(isa_missing_sets(bulldozer, 1, text, sizeof text) == 2 && strcmp(text, "MOVBE, BMI1") == 0);
    CHECK(!lzcnt_compat_needed(bulldozer));

    const IsaHostFeatures nehalem = {bit_SSE4_1 | bit_SSE4_2 | bit_POPCNT, 0, bit_LZCNT};
    CHECK(isa_missing_sets(nehalem, 0, text, sizeof text) == 1 && strcmp(text, "AVX") == 0);
    const IsaHostFeatures no_popcnt = {bit_SSE4_1 | bit_SSE4_2 | bit_AVX, 0, 0};
    CHECK(isa_missing_sets(no_popcnt, 0, text, sizeof text) == 1 && strcmp(text, "POPCNT") == 0);
    const IsaHostFeatures penryn = {bit_SSE4_1, 0, 0};
    CHECK(isa_missing_sets(penryn, 0, text, sizeof text) == 3 && strcmp(text, "SSE4.2, POPCNT, AVX") == 0);

    const IsaHostFeatures none = {0, 0, 0};
    CHECK(isa_missing_sets(none, 0, text, sizeof text) == 4 && strcmp(text, "SSE4.1, SSE4.2, POPCNT, AVX") == 0);
    CHECK(isa_missing_sets(none, 0, text, 8) == 4 && strlen(text) < 8);

    isa_host_name(text, sizeof text);
    CHECK(text[0] != 0 && text[0] != ' ');
}

enum { PROBE_PAGE = 4096, PROBE_RSP = 4, PROBE_FLAGS = 11 };

uint64_t dcp_probe_in[16], dcp_probe_out[16];
const void *dcp_probe_code;
void dcp_probe_run(void);
__asm__(".text\n.globl dcp_probe_run\ndcp_probe_run:\n"
        " push %rbx\n push %rbp\n push %rdi\n push %rsi\n push %r12\n push %r13\n push %r14\n push %r15\n"
        " lea dcp_probe_in(%rip), %rax\n mov dcp_probe_code(%rip), %r11\n"
        " mov 8(%rax), %rcx\n mov 16(%rax), %rdx\n mov 24(%rax), %rbx\n mov 40(%rax), %rbp\n"
        " mov 48(%rax), %rsi\n mov 56(%rax), %rdi\n mov 64(%rax), %r8\n mov 72(%rax), %r9\n mov 80(%rax), %r10\n"
        " mov 96(%rax), %r12\n mov 104(%rax), %r13\n mov 112(%rax), %r14\n mov 120(%rax), %r15\n"
        " mov (%rax), %rax\n call *%r11\n"
        " mov %rax, dcp_probe_out(%rip)\n mov %rcx, dcp_probe_out+8(%rip)\n mov %rdx, dcp_probe_out+16(%rip)\n"
        " mov %rbx, dcp_probe_out+24(%rip)\n mov %rbp, dcp_probe_out+40(%rip)\n mov %rsi, dcp_probe_out+48(%rip)\n"
        " mov %rdi, dcp_probe_out+56(%rip)\n mov %r8, dcp_probe_out+64(%rip)\n mov %r9, dcp_probe_out+72(%rip)\n"
        " mov %r10, dcp_probe_out+80(%rip)\n mov %r11, dcp_probe_out+88(%rip)\n mov %r12, dcp_probe_out+96(%rip)\n"
        " mov %r13, dcp_probe_out+104(%rip)\n mov %r14, dcp_probe_out+112(%rip)\n mov %r15, dcp_probe_out+120(%rip)\n"
        " pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rsi\n pop %rdi\n pop %rbp\n pop %rbx\n ret\n");

static uint8_t *executable_block(size_t size) {
#ifdef _WIN32
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    void *block = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return block == MAP_FAILED ? NULL : block;
#endif
}

static uint64_t leading_zeros(uint64_t value, unsigned bits) {
    if (bits < 64) value &= (1ull << bits) - 1;
    return value ? (uint64_t)__builtin_clzll(value) - (64 - bits) : bits;
}

static int host_has_lzcnt(void) {
    return (isa_host_features().ext1_ecx & bit_LZCNT) != 0;
}

static const uint8_t probe_epilogue[] = {0x9c, 0x41, 0x5b, 0xc3}; /* pushfq; pop r11; ret */

/* Page 0 holds the instruction under test and the epilogue, rewritten; page 1 the stub; page 2 the same code
 * untouched. */
static uint8_t *probe_block;
static uint8_t probe_arena[PROBE_PAGE] __attribute__((aligned(64)));

static int prepare_probe(const uint8_t *insn, size_t length) {
    if (!probe_block) probe_block = executable_block(3 * PROBE_PAGE);
    if (!probe_block) return 0;
    LzcntShape shape = {(uint8_t)(length < 5 ? length + 1 : length), 0, {0}};
    memcpy(shape.bytes, insn, length);
    if (length < 5) shape.bytes[length] = probe_epilogue[0];
    memset(probe_block, 0xcc, 3 * PROBE_PAGE);
    memcpy(probe_block, insn, length);
    memcpy(probe_block + length, probe_epilogue, sizeof probe_epilogue);
    memcpy(probe_block + 2 * PROBE_PAGE, probe_block, length + sizeof probe_epilogue);
    const uint32_t offset = 0;
    return lzcnt_compat_apply(probe_block, PROBE_PAGE, &offset, &shape, 1, probe_block + PROBE_PAGE, PROBE_PAGE) ==
           LZCNT_COMPAT_OK;
}

static void probe(const uint8_t *code, const uint64_t in[16], uint64_t out[16]) {
    memcpy(dcp_probe_in, in, sizeof dcp_probe_in);
    dcp_probe_code = code;
    dcp_probe_run();
    memcpy(out, dcp_probe_out, sizeof dcp_probe_out);
}

static const uint64_t probe_values[] = {
    0, 1, 2, 3, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000, 0x12345, 0x7fffffffull, 0x80000000ull,
    0xffffffffull, 0x100000000ull, 0x0123456789abcdefull, 0x00ff00ff00ff00ffull, 0x7fffffffffffffffull,
    0x8000000000000000ull, ~0ull,
};

/* Runs one LZCNT form on each value, rewritten and, on a host that has LZCNT, as it is. `source` is the register
 * holding the value, or -1 for the memory operand at `value_at`. */
static void check_form(const uint8_t *insn, size_t length, unsigned bits, unsigned dst, int source,
                       const uint64_t regs[16], uint8_t *value_at, int all_bits) {
    if (!prepare_probe(insn, length)) {
        CHECK(0 && "probe block");
        return;
    }
    const size_t count = sizeof probe_values / sizeof *probe_values;
    const uint64_t mask = bits == 64 ? ~0ull : (1ull << bits) - 1;
    for (size_t n = 0; n < count + (all_bits ? 64u : 0u); ++n) {
        const uint64_t value = n < count ? probe_values[n] : 1ull << (n - count);
        uint64_t in[16], out[16];
        memcpy(in, regs, sizeof in);
        if (source >= 0) in[source] = value;
        else memcpy(value_at, &value, bits / 8);
        const uint64_t result = leading_zeros(value, bits);
        const uint64_t expected = bits == 16 ? (in[dst] & ~0xffffull) | result : result;
        for (int pass = 0; pass < 2; ++pass) {
            if (pass && !host_has_lzcnt()) break;
            probe(probe_block + (pass ? 2 * PROBE_PAGE : 0), in, out);
            int same = out[dst] == expected;
            for (unsigned r = 0; r < 16; ++r)
                if (r != dst && r != PROBE_RSP && r != PROBE_FLAGS && out[r] != in[r]) same = 0;
            const uint64_t flags = out[PROBE_FLAGS];
            same = same && (flags & 1) == ((value & mask) == 0) && (flags >> 6 & 1) == (result == 0);
            if (!same)
                printf("FAIL %s bits=%u dst=%u source=%d value=%llx: got %llx flags %llx, expected %llx\n",
                       pass ? "hardware" : "rewritten", bits, dst, source, (unsigned long long)value,
                       (unsigned long long)out[dst], (unsigned long long)flags, (unsigned long long)expected);
            CHECK(same);
        }
    }
}

static void fill_registers(uint64_t regs[16]) {
    for (unsigned r = 0; r < 16; ++r) regs[r] = 0xc0de0000face0000ull + r * 0x100001ull;
}

static size_t encode_register_form(uint8_t *out, unsigned bits, unsigned dst, unsigned source) {
    size_t n = 0;
    if (bits == 16) out[n++] = 0x66;
    out[n++] = 0xf3;
    const unsigned rex = (bits == 64 ? 8u : 0u) | (dst >= 8 ? 4u : 0u) | (source >= 8 ? 1u : 0u);
    if (rex) out[n++] = (uint8_t)(0x40 | rex);
    out[n++] = 0x0f;
    out[n++] = 0xbd;
    out[n++] = (uint8_t)(0xc0 | (dst & 7) << 3 | (source & 7));
    return n;
}

/* mode 0: [base], 1: [base+0x10], 2: [base+0x3a8], 3: [base+rcx*4+8]; *displacement is what the address adds. */
static size_t encode_memory_form(uint8_t *out, unsigned bits, unsigned dst, unsigned base, int mode,
                                 uint64_t *displacement) {
    size_t n = 0;
    if (bits == 16) out[n++] = 0x66;
    out[n++] = 0xf3;
    const unsigned rex = (bits == 64 ? 8u : 0u) | (dst >= 8 ? 4u : 0u) | (base >= 8 ? 1u : 0u);
    if (rex) out[n++] = (uint8_t)(0x40 | rex);
    out[n++] = 0x0f;
    out[n++] = 0xbd;
    const int indexed = mode == 3, sib = indexed || (base & 7) == 4;
    const unsigned mod = mode == 0 ? 0 : mode == 2 ? 2 : 1;
    out[n++] = (uint8_t)(mod << 6 | (dst & 7) << 3 | (sib ? 4 : base & 7));
    if (sib) out[n++] = (uint8_t)(indexed ? 0x88 | (base & 7) : 0x24);
    *displacement = mode == 0 ? 0 : mode == 1 ? 0x10 : mode == 2 ? 0x3a8 : 8;
    if (mod == 1) out[n++] = (uint8_t)*displacement;
    if (mod == 2) {
        const uint32_t d = 0x3a8;
        memcpy(out + n, &d, 4);
        n += 4;
    }
    return n;
}

static const unsigned probe_registers[] = {0, 1, 2, 3, 5, 6, 7, 8, 9, 10, 12, 13, 14, 15};
enum { PROBE_REGISTER_COUNT = sizeof probe_registers / sizeof *probe_registers };
static const unsigned probe_widths[] = {16, 32, 64};

static void rewritten_register_forms(void) {
    for (unsigned w = 0; w < 3; ++w)
        for (unsigned d = 0; d < PROBE_REGISTER_COUNT; ++d)
            for (unsigned s = 0; s < PROBE_REGISTER_COUNT; ++s) {
                uint8_t insn[16];
                uint64_t regs[16];
                fill_registers(regs);
                const size_t length =
                    encode_register_form(insn, probe_widths[w], probe_registers[d], probe_registers[s]);
                check_form(insn, length, probe_widths[w], probe_registers[d], (int)probe_registers[s], regs, NULL,
                           d == s && s < 3);
            }
}

static void rewritten_memory_forms(void) {
    static const unsigned bases[] = {0, 3, 5, 6, 8, 12, 13, 15};
    for (unsigned w = 0; w < 3; ++w)
        for (unsigned d = 0; d < PROBE_REGISTER_COUNT; ++d)
            for (unsigned b = 0; b < sizeof bases / sizeof *bases; ++b)
                for (int mode = 0; mode < 4; ++mode) {
                    const unsigned base = bases[b];
                    if ((mode == 0 && (base & 7) == 5) || (mode == 2 && ((base & 7) == 4 || probe_widths[w] == 16)))
                        continue;
                    uint8_t insn[16];
                    uint64_t regs[16], displacement;
                    fill_registers(regs);
                    const size_t length =
                        encode_memory_form(insn, probe_widths[w], probe_registers[d], base, mode, &displacement);
                    regs[1] = mode == 3 ? 4 : regs[1];
                    regs[base] = (uint64_t)(uintptr_t)probe_arena + 64;
                    uint8_t *value_at = probe_arena + 64 + displacement + (mode == 3 ? 16 : 0);
                    check_form(insn, length, probe_widths[w], probe_registers[d], -1, regs, value_at, 0);
                }
}

static void rewritten_game_forms(void) {
    uint64_t regs[16], out[16];
    fill_registers(regs);
    regs[15] = regs[13] = (uint64_t)(uintptr_t)probe_arena;
    regs[3] = regs[2] = 0;
    const uint8_t r15_rbx[] = {0xf3, 0x49, 0x0f, 0xbd, 0x94, 0x1f, 0xa8, 0x03, 0x00, 0x00};
    check_form(r15_rbx, sizeof r15_rbx, 64, 2, -1, regs, probe_arena + 0x3a8, 0);
    const uint8_t r13_disp32[] = {0xf3, 0x49, 0x0f, 0xbd, 0x85, 0x98, 0x0c, 0x00, 0x00};
    check_form(r13_disp32, sizeof r13_disp32, 64, 0, -1, regs, probe_arena + 0xc98, 0);

    /* Two LZCNTs in one range: the first's result, the second's result and flags. */
    const LzcntShape *pair = NULL;
    for (size_t i = 0; i < LZCNT_SITE_COUNT; ++i)
        if (lzcnt_shapes[i].second) pair = &lzcnt_shapes[i];
    CHECK(pair != NULL);
    if (!pair || !prepare_probe(pair->bytes, 4)) return;
    memset(probe_block, 0xcc, PROBE_PAGE);
    memcpy(probe_block, pair->bytes, pair->length);
    memcpy(probe_block + pair->length, probe_epilogue, sizeof probe_epilogue);
    const uint32_t offset = 0;
    CHECK(lzcnt_compat_apply(probe_block, PROBE_PAGE, &offset, pair, 1, probe_block + PROBE_PAGE, PROBE_PAGE) ==
          LZCNT_COMPAT_OK);
    fill_registers(regs);
    regs[1] = 0x00ff0000;
    regs[9] = 0;
    probe(probe_block, regs, out);
    CHECK(out[1] == 8 && (out[0] & 0xffffffff) == 32);
    CHECK((out[PROBE_FLAGS] & 1) == 1 && (out[PROBE_FLAGS] >> 6 & 1) == 0);
    regs[1] = 0x80000000;
    regs[9] = 1;
    probe(probe_block, regs, out);
    CHECK(out[1] == 0 && (out[0] & 0xffffffff) == 31);
    CHECK((out[PROBE_FLAGS] & 1) == 0 && (out[PROBE_FLAGS] >> 6 & 1) == 0);
}

static int count_lzcnt_bytes(const uint8_t *image, uint64_t size) {
    int count = 0;
    for (uint64_t i = 0; i + 3 < size; ++i) {
        if (image[i] != 0xf3) continue;
        const uint64_t at = (image[i + 1] & 0xf0) == 0x40 ? i + 2 : i + 1;
        count += at + 1 < size && image[at] == 0x0f && image[at + 1] == 0xbd;
    }
    return count;
}

/* A readable image holding every site of the build, followed by the stub area. */
typedef struct {
    uint8_t *bytes;
    uint8_t *stubs;
} SyntheticImage;

static SyntheticImage synthetic_image(const LzcntBuild *build) {
    SyntheticImage image = {NULL, NULL};
    image.bytes = calloc(1, build->image.size + lzcnt_compat_stub_bytes());
    if (!image.bytes) return image;
    image.stubs = image.bytes + build->image.size;
    for (size_t i = 0; i < LZCNT_SITE_COUNT; ++i)
        memcpy(image.bytes + build->offsets[i], lzcnt_shapes[i].bytes, lzcnt_shapes[i].length);
    return image;
}

static void site_tables(void) {
    const size_t stubs = lzcnt_compat_stub_bytes(), stride = stubs / LZCNT_SITE_COUNT;
    int lzcnts = 0;
    for (size_t i = 0; i < LZCNT_SITE_COUNT; ++i) {
        CHECK(lzcnt_shapes[i].length >= 5 && lzcnt_shapes[i].length <= LZCNT_SHAPE_MAX);
        lzcnts += count_lzcnt_bytes(lzcnt_shapes[i].bytes, lzcnt_shapes[i].length);
    }
    CHECK(lzcnts == 62);
    for (size_t b = 0; b < LZCNT_BUILD_COUNT; ++b) {
        const LzcntBuild *build = &lzcnt_builds[b];
        for (size_t i = 1; i < LZCNT_SITE_COUNT; ++i)
            CHECK(build->offsets[i] >= build->offsets[i - 1] + lzcnt_shapes[i - 1].length);
        CHECK(build->offsets[LZCNT_SITE_COUNT - 1] + lzcnt_shapes[LZCNT_SITE_COUNT - 1].length <= build->image.size);
        for (size_t o = 0; o < b; ++o)
            CHECK(lzcnt_builds[o].image.size != build->image.size || lzcnt_builds[o].image.entry != build->image.entry);

        SyntheticImage image = synthetic_image(build);
        if (!image.bytes) {
            CHECK(0 && "synthetic image");
            continue;
        }
        uint8_t *before = malloc(build->image.size);
        memcpy(before, image.bytes, build->image.size);
        const char *name = NULL;
        CHECK(lzcnt_compat_install(image.bytes, build->image.size, build->image.entry, image.stubs, stubs, &name) ==
              LZCNT_COMPAT_OK);
        CHECK(name == build->build);
        CHECK(count_lzcnt_bytes(image.bytes, build->image.size) == 0);
        size_t changed = 0, allowed = 0;
        for (size_t i = 0; i < LZCNT_SITE_COUNT; ++i) {
            const uint8_t *site = image.bytes + build->offsets[i];
            int32_t rel;
            memcpy(&rel, site + 1, 4);
            CHECK(site[0] == 0xe9 && site + 5 + rel == image.stubs + i * stride);
            for (unsigned k = 5; k < lzcnt_shapes[i].length; ++k) CHECK(site[k] == 0x90);
            allowed += lzcnt_shapes[i].length;
        }
        for (uint64_t k = 0; k < build->image.size; ++k) changed += image.bytes[k] != before[k];
        CHECK(changed <= allowed);
        free(before);
        free(image.bytes);
    }
}

static void patch_rejections(void) {
    const LzcntBuild *build = &lzcnt_builds[0];
    SyntheticImage image = synthetic_image(build);
    if (!image.bytes) {
        CHECK(0 && "synthetic image");
        return;
    }
    const size_t stubs = lzcnt_compat_stub_bytes(), total = build->image.size + stubs;
    uint8_t *before = malloc(total);
    memcpy(before, image.bytes, total);
#define UNTOUCHED() (memcmp(before, image.bytes, total) == 0)

    CHECK(lzcnt_compat_install(image.bytes, build->image.size + 16, build->image.entry, image.stubs, stubs, NULL) ==
          LZCNT_COMPAT_UNKNOWN_IMAGE);
    CHECK(lzcnt_compat_install(image.bytes, build->image.size, build->image.entry + 16, image.stubs, stubs, NULL) ==
          LZCNT_COMPAT_UNKNOWN_IMAGE);
    CHECK(lzcnt_compat_install(image.bytes, build->image.size, build->image.entry, image.stubs, stubs - 1, NULL) ==
          LZCNT_COMPAT_NO_ROOM);
    uint8_t *far_stubs = (uint8_t *)((uintptr_t)image.bytes + 0x90000000u);
    CHECK(lzcnt_compat_install(image.bytes, build->image.size, build->image.entry, far_stubs, stubs, NULL) ==
          LZCNT_COMPAT_NO_ROOM);
    CHECK(UNTOUCHED());

    for (size_t i = 0; i < LZCNT_SITE_COUNT; i += 10)
        for (unsigned k = 0; k < lzcnt_shapes[i].length; ++k) {
            image.bytes[build->offsets[i] + k] ^= 0x40;
            CHECK(lzcnt_compat_install(image.bytes, build->image.size, build->image.entry, image.stubs, stubs, NULL) ==
                  LZCNT_COMPAT_BAD_SITE);
            image.bytes[build->offsets[i] + k] ^= 0x40;
            CHECK(UNTOUCHED());
        }
    CHECK(lzcnt_compat_apply(image.bytes, build->offsets[LZCNT_SITE_COUNT - 1] + 4, build->offsets, lzcnt_shapes,
                             LZCNT_SITE_COUNT, image.stubs, stubs) == LZCNT_COMPAT_BAD_SITE);
    CHECK(UNTOUCHED());

    const LzcntBuild *other = &lzcnt_builds[1];
    CHECK(lzcnt_compat_apply(image.bytes, build->image.size, other->offsets, lzcnt_shapes, LZCNT_SITE_COUNT,
                             image.stubs, stubs) == LZCNT_COMPAT_BAD_SITE);
    CHECK(UNTOUCHED());
    free(before);
    free(image.bytes);
#undef UNTOUCHED

    /* Shapes the rewrite cannot handle, each matching the bytes it sits on. */
    static const LzcntShape bad[] = {
        {4, 0, {0xf3, 0x0f, 0xbd, 0xc8}},                                      /* too short for a jmp */
        {8, 0, {0xf3, 0x0f, 0xbd, 0x05, 0x10, 0x00, 0x00, 0x00}},              /* RIP-relative operand */
        {5, 0, {0x0f, 0xbd, 0xc8, 0x90, 0x90}},                                /* BSR, not LZCNT */
        {5, 0, {0xf2, 0xf3, 0x0f, 0xbd, 0xc8}},                                /* another prefix */
        {6, 0, {0xf3, 0x0f, 0xbc, 0xc8, 0x90, 0x90}},                          /* TZCNT */
        {9, 4, {0xf3, 0x0f, 0xbd, 0xc9, 0x90, 0x90, 0x90, 0x90, 0x90}},        /* the second is not an LZCNT */
        {9, 9, {0xf3, 0x0f, 0xbd, 0xc9, 0x90, 0x90, 0x90, 0x90, 0x90}},        /* the second is beyond the range */
        {5, 0, {0xf3, 0x0f, 0xbd, 0x44, 0x24}},                                /* operand cut by the range */
        {11, 0, {0xf3, 0x0f, 0xbd, 0xc9, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90}}, /* longer than a range may be */
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        uint8_t buffer[64 + 128], copy[sizeof buffer];
        memset(buffer, 0, sizeof buffer);
        const uint32_t offset = 16;
        memcpy(buffer + offset, bad[i].bytes, LZCNT_SHAPE_MAX);
        memcpy(copy, buffer, sizeof buffer);
        CHECK(lzcnt_compat_apply(buffer, 64, &offset, &bad[i], 1, buffer + 64, 128) == LZCNT_COMPAT_BAD_SITE);
        CHECK(memcmp(buffer, copy, sizeof buffer) == 0);
    }
}

/* The memory image inside build/image/<folder>/boot.bin (layout written by launcher/boot_image.cpp). */
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) return 0;
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = v << 8 | b[i];
    return v;
}

static uint8_t *boot_image(const char *folder, uint64_t *size, size_t extra) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/build/image/%s/boot.bin", DCP_SOURCE_DIR, folder);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "DCPBOOT1", 8)) return fclose(f), NULL;
    *size = read64(f);
    read64(f);
    const uint64_t segments = read64(f), relocs = read64(f), names = read64(f);
    read64(f), read64(f);
    for (int i = 0; i < 4; ++i) read64(f);
    const uint64_t modules = read64(f);
    fseek(f, (long)(modules * 56), SEEK_CUR);
    const uint64_t bindings = read64(f);
    fseek(f, (long)(bindings * 24 + segments * 24 + names * 128 + relocs * 32), SEEK_CUR);
    uint8_t *image = calloc(1, *size + extra);
    if (image && fread(image, 1, *size, f) != *size) free(image), image = NULL;
    fclose(f);
    return image;
}

static void real_images(void) {
    static const char *const folders[LZCNT_BUILD_COUNT] = {"v1.03", "CUSA07001-1.00", "CUSA07001-1.02",
                                                           "CUSA07001-1.03", "CUSA10209-1.00"};
    const size_t stubs = lzcnt_compat_stub_bytes();
    for (size_t b = 0; b < LZCNT_BUILD_COUNT; ++b) {
        const LzcntBuild *build = &lzcnt_builds[b];
        uint64_t size = 0;
        uint8_t *image = boot_image(folders[b], &size, stubs);
        if (!image) {
            printf("no boot image for %s: skipped\n", folders[b]);
            continue;
        }
        CHECK(size == build->image.size);
        CHECK(count_lzcnt_bytes(image, size) == 62);
        uint8_t *before = malloc(size);
        memcpy(before, image, size);

        const LzcntBuild *wrong = &lzcnt_builds[(b + 1) % LZCNT_BUILD_COUNT];
        CHECK(lzcnt_compat_apply(image, size, wrong->offsets, lzcnt_shapes, LZCNT_SITE_COUNT, image + size, stubs) ==
              LZCNT_COMPAT_BAD_SITE);
        CHECK(memcmp(image, before, size) == 0);

        CHECK(lzcnt_compat_install(image, size, build->image.entry, image + size, stubs, NULL) == LZCNT_COMPAT_OK);
        CHECK(count_lzcnt_bytes(image, size) == 0);
        size_t changed = 0, inside = 0;
        for (uint64_t k = 0; k < size; ++k) {
            if (image[k] == before[k]) continue;
            ++changed;
            for (size_t i = 0; i < LZCNT_SITE_COUNT; ++i)
                inside += k >= build->offsets[i] && k < build->offsets[i] + lzcnt_shapes[i].length;
        }
        CHECK(changed == inside);
        printf("%s: %zu bytes changed at %d sites\n", folders[b], changed, LZCNT_SITE_COUNT);
        free(before);
        free(image);
    }
}

int main(void) {
    crash_site();
    load_widths();
    store_widths();
    addressing_forms();
    rip_relative();
    rejected();
    strlen_site();
    andn_forms();
    bextr_forms();
    bls_forms();
    bmi1_rejected();
    host_sets();
    site_tables();
    patch_rejections();
    rewritten_register_forms();
    rewritten_memory_forms();
    rewritten_game_forms();
    real_images();
    if (failures) return 1;
    puts("isa tests passed");
    return 0;
}
