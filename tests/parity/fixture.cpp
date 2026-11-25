#include "fixture.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Branch {
    uint64_t field, end, target;
};

} // namespace

struct Fx {
    std::vector<unsigned char> image, owned;
    std::vector<Branch> branches;
    std::vector<uint64_t> pointers;
    std::string context;
    uint64_t cursor = 0;
    int conflicts = 0;

    void Own(uint64_t at, const void* bytes, size_t length) {
        const auto* in = static_cast<const unsigned char*>(bytes);
        for (size_t i = 0; i < length; ++i) {
            if (at + i >= image.size()) {
                Conflict(at + i, "outside the image");
                return;
            }
            if (owned[at + i] && image[at + i] != in[i]) Conflict(at + i, "planted differently");
            image[at + i] = in[i];
            owned[at + i] = 1;
        }
    }

    void Conflict(uint64_t at, const char* why) {
        if (conflicts++ < 8) std::printf("  fixture conflict at 0x%llx (%s): %s\n", static_cast<unsigned long long>(at), context.c_str(), why);
    }

    bool Owned(uint64_t at, size_t length) const {
        for (size_t i = 0; i < length; ++i) {
            if (at + i >= owned.size() || !owned[at + i]) return false;
        }
        return true;
    }
};

extern "C" {

Fx* fx_create(uint64_t size, uint64_t code_region) {
    Fx* fx = new Fx;
    fx->image.assign(size, 0);
    fx->owned.assign(size, 0);
    fx->cursor = code_region;
    return fx;
}

Fx* fx_clone(const Fx* fx) { return new Fx(*fx); }
void fx_destroy(Fx* fx) { delete fx; }
unsigned char* fx_image(Fx* fx) { return fx->image.data(); }
uint64_t fx_size(const Fx* fx) { return fx->image.size(); }
int fx_conflicts(const Fx* fx) { return fx->conflicts; }
void fx_context(Fx* fx, const char* what) { fx->context = what; }
int fx_planted(const Fx* fx, uint64_t at) { return at < fx->owned.size() && fx->owned[at]; }

void fx_put(Fx* fx, uint64_t at, const void* bytes, size_t length) { fx->Own(at, bytes, length); }

void fx_shape(Fx* fx, uint64_t at, const short* shape, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (shape[i] < 0) continue;
        const unsigned char byte = static_cast<unsigned char>(shape[i]);
        fx->Own(at + i, &byte, 1);
    }
}

uint64_t fx_alloc(Fx* fx, size_t length) {
    const uint64_t at = fx->cursor;
    fx->cursor += (length + 0x2f) / 0x10 * 0x10;
    if (fx->cursor > fx->image.size()) fx->Conflict(at, "no room");
    return at;
}

uint64_t fx_text(Fx* fx, const char* text) {
    const size_t length = std::strlen(text) + 1;
    const uint64_t at = fx_alloc(fx, length);
    fx->Own(at, text, length);
    return at;
}

void fx_rel_to(Fx* fx, uint64_t field, uint64_t end, uint64_t target) {
    const int32_t rel = static_cast<int32_t>(static_cast<int64_t>(target) - static_cast<int64_t>(end));
    fx->Own(field, &rel, sizeof(rel));
    fx->branches.push_back({field, end, target});
}

uint64_t fx_rel(Fx* fx, uint64_t field, uint64_t end, size_t room) {
    if (fx->Owned(field, 4)) {
        int32_t rel;
        std::memcpy(&rel, &fx->image[field], sizeof(rel));
        return static_cast<uint64_t>(static_cast<int64_t>(end) + rel);
    }
    const uint64_t target = fx_alloc(fx, room);
    fx_rel_to(fx, field, end, target);
    return target;
}

void fx_call_to(Fx* fx, uint64_t at, uint64_t target) {
    const unsigned char opcode = 0xe8;
    fx->Own(at, &opcode, 1);
    fx_rel_to(fx, at + 1, at + 5, target);
}

uint64_t fx_call(Fx* fx, uint64_t at, size_t room) {
    const unsigned char opcode = 0xe8;
    fx->Own(at, &opcode, 1);
    return fx_rel(fx, at + 1, at + 5, room);
}

void fx_jump_to(Fx* fx, uint64_t at, uint64_t target) {
    const unsigned char opcode = 0xe9;
    fx->Own(at, &opcode, 1);
    fx_rel_to(fx, at + 1, at + 5, target);
}

uint64_t fx_jump(Fx* fx, uint64_t at, size_t room) {
    const unsigned char opcode = 0xe9;
    fx->Own(at, &opcode, 1);
    return fx_rel(fx, at + 1, at + 5, room);
}

void fx_pointer_to(Fx* fx, uint64_t slot, uint64_t target) {
    fx->Own(slot, &target, sizeof(target));
    fx->pointers.push_back(slot);
}

uint64_t fx_pointer(Fx* fx, uint64_t slot, size_t room) {
    if (fx->Owned(slot, 8)) {
        uint64_t target;
        std::memcpy(&target, &fx->image[slot], sizeof(target));
        return target;
    }
    const uint64_t target = fx_alloc(fx, room);
    fx_pointer_to(fx, slot, target);
    return target;
}

void fx_entry(Fx* fx, uint64_t table, int index, uint64_t target) {
    const int32_t offset = static_cast<int32_t>(static_cast<int64_t>(target) - static_cast<int64_t>(table));
    fx->Own(table + 4 * static_cast<uint64_t>(index), &offset, sizeof(offset));
}

void fx_shift(Fx* fx, uint64_t distance) {
    const size_t size = fx->image.size();
    for (size_t i = size - distance; i < size; ++i) {
        if (fx->owned[i]) fx->Conflict(i, "shifted out of the image");
    }
    std::memmove(&fx->image[distance], &fx->image[0], size - distance);
    std::memmove(&fx->owned[distance], &fx->owned[0], size - distance);
    std::memset(&fx->image[0], 0, distance);
    std::memset(&fx->owned[0], 0, distance);
    for (uint64_t& slot : fx->pointers) {
        slot += distance;
        uint64_t target;
        std::memcpy(&target, &fx->image[slot], sizeof(target));
        target += distance;
        std::memcpy(&fx->image[slot], &target, sizeof(target));
    }
    for (Branch& branch : fx->branches) {
        branch.field += distance;
        branch.end += distance;
        branch.target += distance;
    }
    fx->cursor += distance;
}

uint64_t fx_duplicate(Fx* fx, uint64_t at) {
    constexpr uint64_t kGap = 12;
    const auto planted_near = [&](uint64_t from, int direction) {
        for (uint64_t i = 1; i <= kGap; ++i) {
            const uint64_t near = direction > 0 ? from + i : from - i;
            if (near < fx->owned.size() && fx->owned[near]) return i;
        }
        return uint64_t(0);
    };
    if (!fx_planted(fx, at)) return 0;
    uint64_t begin = at, end = at;
    while (const uint64_t step = planted_near(begin, -1)) begin -= step;
    while (const uint64_t step = planted_near(end, 1)) end += step;
    ++end;
    const uint64_t copy = fx_alloc(fx, end - begin);
    std::memcpy(&fx->image[copy], &fx->image[begin], end - begin);
    for (const Branch& branch : fx->branches) {
        if (branch.field < begin || branch.field + 4 > end) continue;
        const uint64_t field = branch.field - begin + copy, field_end = branch.end - begin + copy;
        const bool inside = branch.target >= begin && branch.target < end;
        const uint64_t target = inside ? branch.target - begin + copy : branch.target;
        const int32_t rel = static_cast<int32_t>(static_cast<int64_t>(target) - static_cast<int64_t>(field_end));
        std::memcpy(&fx->image[field], &rel, sizeof(rel));
    }
    return copy + (at - begin);
}

} // extern "C"
