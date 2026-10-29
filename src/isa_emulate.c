#include "isa_emulate.h"
#include <cpuid.h>
#include <stdio.h>
#include <string.h>

enum { MAX_INSTRUCTION = 15 };
enum { FLAG_CF = 1u << 0, FLAG_ZF = 1u << 6, FLAG_SF = 1u << 7, FLAG_OF = 1u << 11 };

typedef struct {
    const uint8_t *code;
    size_t length;
} Cursor;

typedef struct {
    int is_register;
    unsigned reg;
    uint64_t address;
} Operand;

static int take(Cursor *c, uint8_t *out) {
    if (c->length >= MAX_INSTRUCTION) return 0;
    *out = c->code[c->length++];
    return 1;
}

static int take_signed(Cursor *c, unsigned bytes, int64_t *out) {
    if (c->length + bytes > MAX_INSTRUCTION) return 0;
    if (bytes == 1) *out = (int8_t)c->code[c->length];
    else {
        int32_t v;
        memcpy(&v, c->code + c->length, 4);
        *out = v;
    }
    c->length += bytes;
    return 1;
}

/* The r/m operand of modrm with 64-bit addressing. RIP-relative addresses assume no immediate follows. */
static int decode_rm(Cursor *c, uint8_t modrm, int rex_x, int rex_b, uint64_t rip, const uint64_t gpr[16], Operand *out) {
    const unsigned mod = modrm >> 6, rm = modrm & 7;
    if (mod == 3) {
        *out = (Operand){1, rm | (rex_b ? 8 : 0), 0};
        return 1;
    }
    int has_base = 1, rip_relative = 0, has_index = 0;
    unsigned base = rm | (rex_b ? 8 : 0), index = 0, scale = 1;
    if (rm == 4) {
        uint8_t sib;
        if (!take(c, &sib)) return 0;
        scale = 1u << (sib >> 6);
        index = (sib >> 3 & 7) | (rex_x ? 8 : 0);
        has_index = index != 4;
        base = (sib & 7) | (rex_b ? 8 : 0);
        if ((sib & 7) == 5 && mod == 0) has_base = 0;
    } else if (rm == 5 && mod == 0) {
        has_base = 0;
        rip_relative = 1;
    }
    int64_t displacement = 0;
    if (mod == 1) { if (!take_signed(c, 1, &displacement)) return 0; }
    else if (mod == 2 || !has_base) { if (!take_signed(c, 4, &displacement)) return 0; }

    uint64_t address = (uint64_t)displacement;
    if (rip_relative) address += rip + c->length;
    if (has_base) address += gpr[base];
    if (has_index) address += gpr[index] * scale;
    *out = (Operand){0, 0, address};
    return 1;
}

static uint64_t swap(uint64_t value, unsigned bits) {
    if (bits == 16) return __builtin_bswap16((uint16_t)value);
    if (bits == 32) return __builtin_bswap32((uint32_t)value);
    return __builtin_bswap64(value);
}

size_t isa_emulate_movbe(const uint8_t *code, uint64_t rip, uint64_t gpr[16]) {
    Cursor c = {code, 0};
    uint8_t byte;
    int operand16 = 0;
    if (!take(&c, &byte)) return 0;
    if (byte == 0x66) {
        operand16 = 1;
        if (!take(&c, &byte)) return 0;
    }
    unsigned rex = 0;
    if ((byte & 0xf0) == 0x40) {
        rex = byte;
        if (!take(&c, &byte)) return 0;
    }
    if (byte != 0x0f || !take(&c, &byte) || byte != 0x38 || !take(&c, &byte)) return 0;
    const int store = byte == 0xf1;
    if (!store && byte != 0xf0) return 0;
    uint8_t modrm;
    Operand rm;
    if (!take(&c, &modrm) || modrm >> 6 == 3 || !decode_rm(&c, modrm, rex & 2, rex & 1, rip, gpr, &rm)) return 0;

    const unsigned reg = (modrm >> 3 & 7) | (rex & 4 ? 8 : 0);
    const unsigned bits = rex & 8 ? 64 : operand16 ? 16 : 32;
    uint8_t *memory = (uint8_t *)(uintptr_t)rm.address;

    if (store) {
        const uint64_t value = swap(gpr[reg], bits);
        memcpy(memory, &value, bits / 8);
    } else {
        uint64_t value = 0;
        memcpy(&value, memory, bits / 8);
        value = swap(value, bits);
        gpr[reg] = bits == 16 ? (gpr[reg] & ~0xffffull) | value : value;
    }
    return c.length;
}

static size_t emulate_bmi1(const uint8_t *code, uint64_t rip, uint64_t gpr[16], uint64_t *rflags) {
    Cursor c = {code, 0};
    uint8_t vex, p1, p2, opcode, modrm;
    if (!take(&c, &vex) || vex != 0xc4 || !take(&c, &p1) || !take(&c, &p2) || !take(&c, &opcode) || !take(&c, &modrm))
        return 0;
    /* VEX.LZ.0F38 without an implied prefix; the other pp values of these opcodes are BMI2. */
    if ((p1 & 0x1f) != 2 || (p2 & 7) != 0) return 0;
    const unsigned group = modrm >> 3 & 7;
    if (opcode != 0xf2 && opcode != 0xf7 && !(opcode == 0xf3 && group >= 1 && group <= 3)) return 0;
    Operand rm;
    if (!decode_rm(&c, modrm, !(p1 & 0x40), !(p1 & 0x20), rip, gpr, &rm)) return 0;

    const unsigned bits = p2 & 0x80 ? 64 : 32;
    const uint64_t mask = bits == 64 ? ~0ull : 0xffffffffull;
    const unsigned reg = group | (p1 & 0x80 ? 0 : 8), vvvv = ~p2 >> 3 & 15;
    uint64_t source = 0;
    if (rm.is_register) source = gpr[rm.reg] & mask;
    else memcpy(&source, (const void *)(uintptr_t)rm.address, bits / 8);
    const uint64_t other = gpr[vvvv] & mask;

    uint64_t result;
    unsigned destination = reg;
    int carry = 0;
    if (opcode == 0xf2) {
        result = ~other & source;
    } else if (opcode == 0xf7) {
        const unsigned start = other & 0xff, length = other >> 8 & 0xff;
        result = start < bits ? source >> start : 0;
        if (length < bits) result &= (1ull << length) - 1;
    } else {
        destination = vvvv;
        result = group == 1 ? source & (source - 1) : group == 2 ? source ^ (source - 1) : source & -source;
        carry = group == 3 ? source != 0 : source == 0;
    }
    result &= mask;

    uint64_t flags = *rflags & ~(uint64_t)(FLAG_CF | FLAG_ZF | FLAG_SF | FLAG_OF);
    if (carry) flags |= FLAG_CF;
    if (!result) flags |= FLAG_ZF;
    if (result >> (bits - 1)) flags |= FLAG_SF;
    gpr[destination] = result;
    *rflags = flags;
    return c.length;
}

size_t isa_emulate(const uint8_t *code, uint64_t rip, uint64_t gpr[16], uint64_t *rflags) {
    const size_t length = isa_emulate_movbe(code, rip, gpr);
    return length ? length : emulate_bmi1(code, rip, gpr, rflags);
}

IsaHostFeatures isa_host_features(void) {
    IsaHostFeatures host = {0, 0, 0};
    unsigned a, b, c, d;
    if (__get_cpuid(1, &a, &b, &c, &d)) {
        host.leaf1_ecx = c;
        /* AVX counts only when the OS saves the YMM state (OSXSAVE and XCR0 bits 1-2). */
        unsigned xcr0 = 0, high = 0;
        if (c & bit_OSXSAVE) __asm__ volatile("xgetbv" : "=a"(xcr0), "=d"(high) : "c"(0));
        if ((xcr0 & 6) != 6) host.leaf1_ecx &= ~(uint32_t)bit_AVX;
    }
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) host.leaf7_ebx = b;
    if (__get_cpuid(0x80000001, &a, &b, &c, &d)) host.ext1_ecx = c;
    return host;
}

int isa_missing_sets(IsaHostFeatures host, int emulated, char *out, size_t size) {
    /* LZCNT runs as BSR on hosts without it instead of trapping: lzcnt_compat rewrites it in the loaded image. TZCNT
     * likewise runs as BSF without BMI1, which the game's uses (nonzero inputs or a zero-safe result) do not notice. */
    static const struct { const char *name; int emulated, word; uint32_t bit; } sets[] = {
        {"SSE4.1", 0, 0, bit_SSE4_1}, {"SSE4.2", 0, 0, bit_SSE4_2}, {"POPCNT", 0, 0, bit_POPCNT},
        {"AVX", 0, 0, bit_AVX},       {"LZCNT", 1, 2, bit_LZCNT},   {"MOVBE", 1, 0, bit_MOVBE},
        {"BMI1", 1, 1, bit_BMI},
    };
    const uint32_t words[] = {host.leaf1_ecx, host.leaf7_ebx, host.ext1_ecx};
    int missing = 0;
    size_t used = 0;
    if (size) out[0] = 0;
    for (size_t i = 0; i < sizeof sets / sizeof *sets; ++i) {
        if (sets[i].emulated != !!emulated || (words[sets[i].word] & sets[i].bit)) continue;
        if (used < size) {
            const int n = snprintf(out + used, size - used, "%s%s", missing ? ", " : "", sets[i].name);
            used = n < 0 || (size_t)n >= size - used ? size : used + (size_t)n;
        }
        ++missing;
    }
    return missing;
}

void isa_host_name(char *out, size_t size) {
    unsigned regs[12] = {0}, high = __get_cpuid_max(0x80000000, NULL);
    for (unsigned i = 0; high >= 0x80000004 && i < 3; ++i)
        __get_cpuid(0x80000002 + i, &regs[4 * i], &regs[4 * i + 1], &regs[4 * i + 2], &regs[4 * i + 3]);
    char brand[49] = {0};
    memcpy(brand, regs, 48);
    const char *name = brand;
    while (*name == ' ') ++name;
    snprintf(out, size, "%s", *name ? name : "unknown processor");
}
