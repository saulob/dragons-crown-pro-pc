#include "isa_emulate.h"
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

int main(void) {
    crash_site();
    load_widths();
    store_widths();
    addressing_forms();
    rip_relative();
    rejected();
    if (failures) return 1;
    puts("isa tests passed");
    return 0;
}
