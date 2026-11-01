#include "call_site.h"

#include <stdio.h>
#include <string.h>

static const char *const register_names[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                               "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};

static int32_t read32(const uint8_t *p) {
    int32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static int decode_group5(const uint8_t *p, unsigned length, CallSite *site) {
    unsigned at = 0;
    const int rex = (p[0] & 0xf0) == 0x40;
    if (rex) ++at;
    if (p[at] != 0xff || at + 2 > length) return 0;
    const unsigned modrm = p[at + 1], operation = (modrm >> 3) & 7, mod = modrm >> 6, rm = modrm & 7;
    if (operation != 2 && operation != 4) return 0;
    const int base_extension = rex && (p[0] & 1);
    unsigned expected = at + 2;
    CallSite found = {CALL_SITE_MEMORY, length, operation == 4, (int)rm | (base_extension << 3), 0};
    if (mod == 3) {
        found.kind = CALL_SITE_REGISTER;
    } else if (rm == 4) {
        if (at + 3 > length) return 0;
        found.reg = -1;
        ++expected;
        const unsigned sib_base = p[at + 2] & 7;
        if (mod == 1) expected += 1;
        else if (mod == 2 || (mod == 0 && sib_base == 5)) expected += 4;
    } else if (mod == 0 && rm == 5) {
        found.kind = CALL_SITE_RIP_MEMORY;
        found.reg = -1;
        expected += 4;
        if (expected == length) found.displacement = read32(p + at + 2);
    } else {
        if (mod == 1) expected += 1;
        else if (mod == 2) expected += 4;
    }
    if (expected != length) return 0;
    if (found.kind == CALL_SITE_MEMORY && found.reg >= 0) {
        if (mod == 1) found.displacement = (int8_t)p[at + 2];
        else if (mod == 2) found.displacement = read32(p + at + 2);
    }
    *site = found;
    return 1;
}

int call_site_decode(const uint8_t *before, size_t size, CallSite *site) {
    if (size >= 5 && before[size - 5] == 0xe8) {
        site->kind = CALL_SITE_DIRECT;
        site->length = 5;
        site->is_jump = 0;
        site->reg = -1;
        site->displacement = read32(before + size - 4);
        return 1;
    }
    for (unsigned length = 2; length <= 8 && length <= size; ++length) {
        const uint8_t *start = before + size - length;
        if (!decode_group5(start, length, site)) continue;
        const int rex_before = length < size && start[0] == 0xff && (start[-1] & 0xf0) == 0x40;
        if (!rex_before || !decode_group5(start - 1, length + 1, site)) decode_group5(start, length, site);
        return 1;
    }
    return 0;
}

void call_site_format(const CallSite *site, char *out, size_t size) {
    const char *verb = site->is_jump ? "jmp" : "call";
    const char *base = site->reg >= 0 ? register_names[site->reg] : "sib";
    switch (site->kind) {
    case CALL_SITE_DIRECT: snprintf(out, size, "%s rel 0x%x", verb, (unsigned)site->displacement); break;
    case CALL_SITE_REGISTER: snprintf(out, size, "%s %s", verb, base); break;
    case CALL_SITE_RIP_MEMORY: snprintf(out, size, "%s [rip%+d]", verb, (int)site->displacement); break;
    case CALL_SITE_MEMORY:
        if (site->reg >= 0 && site->displacement) snprintf(out, size, "%s [%s%+d]", verb, base, (int)site->displacement);
        else snprintf(out, size, "%s [%s]", verb, base);
        break;
    }
}
