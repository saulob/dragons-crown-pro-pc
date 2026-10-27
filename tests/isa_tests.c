#include "isa_emulate.h"
#include <cpuid.h>
#include <stdio.h>
#include <string.h>

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

    const IsaHostFeatures ivy_bridge = {bit_SSE4_1 | bit_SSE4_2 | bit_POPCNT | bit_AVX, 0, 0};
    CHECK(isa_missing_sets(ivy_bridge, 0, text, sizeof text) == 1 && strcmp(text, "LZCNT") == 0);
    CHECK(isa_missing_sets(ivy_bridge, 1, text, sizeof text) == 2 && strcmp(text, "MOVBE, BMI1") == 0);

    const IsaHostFeatures bulldozer = {bit_SSE4_1 | bit_SSE4_2 | bit_POPCNT | bit_AVX, 0, bit_LZCNT};
    CHECK(isa_missing_sets(bulldozer, 0, text, sizeof text) == 0);

    const IsaHostFeatures none = {0, 0, 0};
    CHECK(isa_missing_sets(none, 0, text, sizeof text) == 5 && strcmp(text, "SSE4.1, SSE4.2, POPCNT, AVX, LZCNT") == 0);
    CHECK(isa_missing_sets(none, 0, text, 8) == 5 && strlen(text) < 8);

    isa_host_name(text, sizeof text);
    CHECK(text[0] != 0 && text[0] != ' ');
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
    if (failures) return 1;
    puts("isa tests passed");
    return 0;
}
