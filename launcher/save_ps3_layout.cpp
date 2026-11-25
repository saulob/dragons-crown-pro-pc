#include "save_ps3_layout.h"

#include <array>
#include <cstring>
#include <iterator>

#include "save_data.h"
#include "save_ps3.h"

namespace dcp {
namespace {

constexpr size_t kSlotBase = 0x50;
constexpr size_t kTrailerOffset = kDataSlotStride * kDataSlotCount + kSlotBase;
constexpr size_t kHeaderTableOffset = 8;

// Version, counts and the fixed values the Pro game writes in its own header; the PS3 file differs only in the first three.
constexpr std::array<uint32_t, 18> kProHeaderTable = {38, 660, 979, 0x56, 0x3B, 1, 3, 7, 8, 3, 0x45, 1, 1, 0x42, 0x90, 1, 1, 1};
constexpr uint32_t kPs3FirstCount = 490;
constexpr uint32_t kPs3SecondCount = 532;

struct TrailerField {
    size_t offset;
    Ps3Kind kind;
    size_t count;
};

constexpr TrailerField kTrailer[] = {
    {0x00, Ps3Kind::kByte, 59},
    {0x3B, Ps3Kind::kByte, 6},
    {0x44, Ps3Kind::kU32, 1},
    {0x48, Ps3Kind::kByte, 2},
};

constexpr Ps3LayoutNode Leaf(Ps3Kind kind, uint16_t padding = 0) { return {Ps3Op::kLeaf, kind, padding, 0}; }
constexpr Ps3LayoutNode Repeat(uint16_t count, uint16_t body) { return {Ps3Op::kRepeat, Ps3Kind::kByte, count, body}; }

constexpr Ps3LayoutNode kSlotProgram[] = {
    Repeat(4, 1),
    Leaf(Ps3Kind::kU32),
    Repeat(7, 1),
    Leaf(Ps3Kind::kU16),
    Repeat(11, 1),
    Leaf(Ps3Kind::kByte),
    Repeat(69, 1),
    Leaf(Ps3Kind::kBool),
    Leaf(Ps3Kind::kU16, 2),
    Repeat(7, 2),
    Leaf(Ps3Kind::kU32, 2),
    Leaf(Ps3Kind::kU16),
    Leaf(Ps3Kind::kU32, 2),
    Repeat(499, 8),
    Leaf(Ps3Kind::kItemBits),
    Repeat(2, 1),
    Leaf(Ps3Kind::kU32),
    Repeat(8, 1),
    Leaf(Ps3Kind::kByte),
    Repeat(8, 2),
    Leaf(Ps3Kind::kU16),
    Leaf(Ps3Kind::kU32, 2),
    Leaf(Ps3Kind::kItemBits),
    Repeat(2, 1),
    Leaf(Ps3Kind::kU32),
    Repeat(8, 1),
    Leaf(Ps3Kind::kByte),
    Repeat(78, 60),
    Repeat(5, 1),
    Leaf(Ps3Kind::kU32),
    Repeat(9, 1),
    Leaf(Ps3Kind::kU16),
    Repeat(4, 1),
    Leaf(Ps3Kind::kByte),
    Leaf(Ps3Kind::kU32, 2),
    Leaf(Ps3Kind::kU16),
    Repeat(66, 1),
    Leaf(Ps3Kind::kByte),
    Repeat(144, 1),
    Leaf(Ps3Kind::kBool),
    Repeat(8, 1),
    Leaf(Ps3Kind::kByte),
    Repeat(2, 1),
    Leaf(Ps3Kind::kU32),
    Repeat(34, 1),
    Leaf(Ps3Kind::kByte),
    Leaf(Ps3Kind::kByte, 2),
    Repeat(3, 5),
    Leaf(Ps3Kind::kByte),
    Leaf(Ps3Kind::kByte),
    Leaf(Ps3Kind::kByte),
    Leaf(Ps3Kind::kByte),
    Leaf(Ps3Kind::kByte),
    Repeat(8, 5),
    Leaf(Ps3Kind::kByte),
    Leaf(Ps3Kind::kByte),
    Leaf(Ps3Kind::kByte),
    Leaf(Ps3Kind::kByte),
    Leaf(Ps3Kind::kU16),
    Repeat(9, 17),
    Repeat(9, 8),
    Repeat(8, 2),
    Leaf(Ps3Kind::kU16),
    Leaf(Ps3Kind::kU32, 2),
    Leaf(Ps3Kind::kItemBits),
    Repeat(2, 1),
    Leaf(Ps3Kind::kU32),
    Repeat(8, 1),
    Leaf(Ps3Kind::kByte),
    Repeat(8, 2),
    Leaf(Ps3Kind::kU16),
    Leaf(Ps3Kind::kU32, 2),
    Leaf(Ps3Kind::kItemBits),
    Repeat(2, 1),
    Leaf(Ps3Kind::kU32),
    Repeat(20, 1),
    Leaf(Ps3Kind::kByte),
    Leaf(Ps3Kind::kU32),
    Repeat(240, 1),
    Leaf(Ps3Kind::kU16),
    Repeat(90, 1),
    Leaf(Ps3Kind::kByte),
    Leaf(Ps3Kind::kU32, 2),
    Repeat(5, 1),
    Leaf(Ps3Kind::kU32),
    Repeat(5, 1),
    Leaf(Ps3Kind::kByte),
    Leaf(Ps3Kind::kU32, 3),
    Repeat(9, 1),
    Leaf(Ps3Kind::kU32),
    Repeat(4, 1),
    Leaf(Ps3Kind::kBool),
    Repeat(3, 2),
    Leaf(Ps3Kind::kByte),
    Leaf(Ps3Kind::kByte),
    Repeat(4, 2),
    Leaf(Ps3Kind::kByte),
    Leaf(Ps3Kind::kU16, 1),
    Repeat(14, 1),
    Leaf(Ps3Kind::kByte),
    Leaf(Ps3Kind::kU16),
    Repeat(5, 1),
    Leaf(Ps3Kind::kByte),
    Leaf(Ps3Kind::kU32, 1),
    Repeat(2, 1),
    Leaf(Ps3Kind::kU32),
    Repeat(2, 1),
    Leaf(Ps3Kind::kByte),
    Leaf(Ps3Kind::kU16, 2),
    Repeat(7, 2),
    Leaf(Ps3Kind::kU32, 2),
    Leaf(Ps3Kind::kU16),
    Leaf(Ps3Kind::kU32, 2),
    Repeat(19, 8),
    Leaf(Ps3Kind::kItemBits),
    Repeat(2, 1),
    Leaf(Ps3Kind::kU32),
    Repeat(8, 1),
    Leaf(Ps3Kind::kByte),
    Repeat(8, 2),
    Leaf(Ps3Kind::kU16),
    Leaf(Ps3Kind::kU32, 2),
    Leaf(Ps3Kind::kItemBits),
    Repeat(2, 1),
    Leaf(Ps3Kind::kU32),
    Repeat(8, 1),
    Leaf(Ps3Kind::kByte),
};

uint32_t ReadBe32(std::span<const uint8_t> bytes, size_t at) {
    return uint32_t(bytes[at]) << 24 | uint32_t(bytes[at + 1]) << 16 | uint32_t(bytes[at + 2]) << 8 | bytes[at + 3];
}

void WriteLe32(std::span<uint8_t> bytes, size_t at, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) bytes[at + i] = uint8_t(value >> (8 * i));
}

uint32_t ItemBitsToPro(uint32_t ps3) {
    const uint32_t occupant = ps3 >> 7;
    const uint32_t kind = (ps3 >> 3) & 0xF;
    const uint32_t flag_a = (ps3 >> 2) & 1;
    const uint32_t flag_b = (ps3 >> 1) & 1;
    const uint32_t flag_c = ps3 & 1;
    return occupant | kind << 25 | flag_a << 29 | flag_b << 30 | flag_c << 31;
}

void ConvertField(std::span<const uint8_t> from, std::span<uint8_t> to, size_t at, Ps3Kind kind) {
    switch (kind) {
    case Ps3Kind::kByte: to[at] = from[at]; break;
    case Ps3Kind::kBool: to[at] = from[at] != 0 ? 1 : 0; break;
    case Ps3Kind::kU16:
        to[at] = from[at + 1];
        to[at + 1] = from[at];
        break;
    case Ps3Kind::kU32: WriteLe32(to, at, ReadBe32(from, at)); break;
    case Ps3Kind::kItemBits: WriteLe32(to, at, ItemBitsToPro(ReadBe32(from, at))); break;
    }
}

template <class Visit>
size_t Run(size_t begin, size_t end, size_t offset, const Visit& visit) {
    while (begin < end) {
        const Ps3LayoutNode& node = kSlotProgram[begin];
        if (node.op == Ps3Op::kLeaf) {
            offset += node.value;
            visit(offset, node.kind);
            offset += Ps3KindWidth(node.kind);
            ++begin;
            continue;
        }
        const size_t body_begin = begin + 1;
        const size_t body_end = body_begin + node.body;
        for (uint16_t i = 0; i < node.value; ++i) offset = Run(body_begin, body_end, offset, visit);
        begin = body_end;
    }
    return offset;
}

} // namespace

size_t Ps3KindWidth(Ps3Kind kind) {
    switch (kind) {
    case Ps3Kind::kByte:
    case Ps3Kind::kBool: return 1;
    case Ps3Kind::kU16: return 2;
    case Ps3Kind::kU32:
    case Ps3Kind::kItemBits: return 4;
    }
    return 1;
}

std::span<const Ps3LayoutNode> Ps3SlotProgram() { return kSlotProgram; }

void ForEachPs3SlotField(const std::function<void(size_t, Ps3Kind)>& visit) {
    Run(0, std::size(kSlotProgram), kSlotBase, visit);
}

bool ConvertPs3ToPro(std::span<const uint8_t> ps3, std::vector<uint8_t>& out, Ps3ConvertReport& report) {
    report = {};
    out.clear();
    if (!InspectPs3Plaintext(ps3).valid) return false;
    report.source_version = ReadBe32(ps3, kHeaderTableOffset);
    if (report.source_version != kPs3SaveVersion || ReadBe32(ps3, kHeaderTableOffset + 4) != kPs3FirstCount ||
        ReadBe32(ps3, kHeaderTableOffset + 8) != kPs3SecondCount) {
        return false;
    }

    std::vector<uint8_t> pro(kSaveFileSize, 0);
    std::memcpy(pro.data() + kSaveMagicOffset, "DCSD", 4);
    for (size_t i = 0; i < kProHeaderTable.size(); ++i) WriteLe32(pro, kHeaderTableOffset + 4 * i, kProHeaderTable[i]);

    size_t typed_bytes = 0;
    const auto convert_slot_field = [&](size_t offset, Ps3Kind kind) {
        ConvertField(ps3, pro, offset, kind);
        typed_bytes += Ps3KindWidth(kind);
        ++report.converted_fields;
    };
    for (size_t slot = 0; slot < kDataSlotCount; ++slot) {
        const size_t base = slot * kDataSlotStride;
        const auto shifted = [&](size_t offset, Ps3Kind kind) { convert_slot_field(base + offset, kind); };
        Run(0, std::size(kSlotProgram), kSlotBase, shifted);
    }
    for (const TrailerField& field : kTrailer) {
        for (size_t i = 0; i < field.count; ++i) convert_slot_field(kTrailerOffset + field.offset + i * Ps3KindWidth(field.kind), field.kind);
    }
    report.padding_bytes = kSaveFileSize - kSlotBase - typed_bytes;

    WriteLe32(pro, kSaveCrcOffset, SaveCrc32(pro));
    out = std::move(pro);
    report.converted = true;
    return true;
}

} // namespace dcp
