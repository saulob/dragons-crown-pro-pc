#include "isa_emulate.h"
#include <string.h>

enum { MAX_INSTRUCTION = 15 };

typedef struct {
    const uint8_t *code;
    size_t length;
} Cursor;

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
    if (!take(&c, &modrm)) return 0;
    const unsigned mod = modrm >> 6, rm = modrm & 7;
    if (mod == 3) return 0;

    const unsigned reg = (modrm >> 3 & 7) | (rex & 4 ? 8 : 0);
    const unsigned bits = rex & 8 ? 64 : operand16 ? 16 : 32;
    int has_base = 1, rip_relative = 0;
    unsigned base = rm | (rex & 1 ? 8 : 0), index = 0, scale = 1;
    int has_index = 0;
    if (rm == 4) {
        uint8_t sib;
        if (!take(&c, &sib)) return 0;
        scale = 1u << (sib >> 6);
        index = (sib >> 3 & 7) | (rex & 2 ? 8 : 0);
        has_index = index != 4;
        base = (sib & 7) | (rex & 1 ? 8 : 0);
        if ((sib & 7) == 5 && mod == 0) has_base = 0;
    } else if (rm == 5 && mod == 0) {
        has_base = 0;
        rip_relative = 1;
    }
    int64_t displacement = 0;
    if (mod == 1) { if (!take_signed(&c, 1, &displacement)) return 0; }
    else if (mod == 2 || !has_base) { if (!take_signed(&c, 4, &displacement)) return 0; }

    uint64_t address = (uint64_t)displacement;
    if (rip_relative) address += rip + c.length;
    if (has_base) address += gpr[base];
    if (has_index) address += gpr[index] * scale;
    uint8_t *memory = (uint8_t *)(uintptr_t)address;

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
