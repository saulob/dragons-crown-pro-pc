// Conversion of a decrypted Dragon's Crown PS3 SAVE0.DAT (version 36, big-endian struct image) to the Pro layout.
// The slot layout is a compact program of fields and nested repeats; every byte it does not name is alignment padding.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace dcp {

enum class Ps3Op : uint8_t { kLeaf, kRepeat };

// kItemBits is the 32-bit word of an item record that packs five bitfields (25, 4, 1, 1 and 1 bits); the PS3 compiler
// allocates them from the most significant bit, Pro from the least.
enum class Ps3Kind : uint8_t { kByte, kBool, kU16, kU32, kItemBits };

// A leaf skips `value` padding bytes and then holds one field. A repeat runs the next `body` nodes `value` times.
struct Ps3LayoutNode {
    Ps3Op op;
    Ps3Kind kind;
    uint16_t value;
    uint16_t body;
};

size_t Ps3KindWidth(Ps3Kind kind);
std::span<const Ps3LayoutNode> Ps3SlotProgram();

// Calls `visit(offset, kind)` for every field of one DATA slot, offsets counted from the start of the file's slot.
void ForEachPs3SlotField(const std::function<void(size_t offset, Ps3Kind kind)>& visit);

constexpr uint32_t kPs3SaveVersion = 36;

struct Ps3ConvertReport {
    bool converted = false;
    uint32_t source_version = 0;
    size_t converted_fields = 0;
    size_t padding_bytes = 0;
};

// Rebuilds the Pro file from the typed fields of a verified PS3 plaintext: fields are byte-reversed, item bitfields are
// re-packed, padding is zero, the header is the Pro header and the Pro CRC-32 is written. Only version 36 is accepted.
bool ConvertPs3ToPro(std::span<const uint8_t> ps3, std::vector<uint8_t>& out, Ps3ConvertReport& report);

} // namespace dcp
