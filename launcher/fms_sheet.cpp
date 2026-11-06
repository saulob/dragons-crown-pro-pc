#include "fms_sheet.h"

#include <algorithm>
#include <cstring>

namespace dcp {
namespace {

constexpr size_t kHeaderSize = 0x20;

uint32_t Le32(std::span<const uint8_t> b, size_t at) {
    return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}

void PutLe32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + size_t(i)] = uint8_t(v >> (8 * i));
}

} // namespace

std::optional<FmsSheet> ParseFms(std::span<const uint8_t> bytes) {
    if (bytes.size() < kHeaderSize || std::memcmp(bytes.data(), "FMSB", 4) != 0) return std::nullopt;
    const size_t header = Le32(bytes, 8), count = Le32(bytes, 0x14), trailer = size_t(Le32(bytes, 4)) + kHeaderSize;
    if (header < kHeaderSize || count > bytes.size() / 8 || trailer > bytes.size() || header + count * 8 > trailer ||
        bytes.size() - trailer < 4 || std::memcmp(bytes.data() + trailer, "FEOC", 4) != 0) {
        return std::nullopt;
    }
    const auto table = bytes.subspan(header, count * 8);
    if (std::any_of(table.begin(), table.end(), [](uint8_t b) { return b != 0; })) return std::nullopt;
    FmsSheet sheet;
    sheet.header.assign(bytes.begin(), bytes.begin() + std::ptrdiff_t(header));
    size_t at = header + count * 8;
    while (sheet.records.size() < count) {
        const auto* start = bytes.data() + at;
        const auto* end = static_cast<const uint8_t*>(std::memchr(start, 0, trailer - at));
        if (!end) return std::nullopt;
        sheet.records.emplace_back(reinterpret_cast<const char*>(start), size_t(end - start));
        at += size_t(end - start) + 1;
    }
    if (std::any_of(bytes.begin() + std::ptrdiff_t(at), bytes.begin() + std::ptrdiff_t(trailer), [](uint8_t b) { return b != 0; })) {
        return std::nullopt;
    }
    sheet.trailer.assign(bytes.begin() + std::ptrdiff_t(trailer), bytes.end());
    return sheet;
}

std::vector<uint8_t> BuildFms(const FmsSheet& sheet) {
    std::vector<uint8_t> out = sheet.header;
    out.resize(out.size() + sheet.records.size() * 8, 0);
    for (const std::string& record : sheet.records) {
        out.insert(out.end(), record.begin(), record.end());
        out.push_back(0);
    }
    out.resize((out.size() + 15) / 16 * 16, 0);
    PutLe32(out, 4, uint32_t(out.size() - kHeaderSize));
    PutLe32(out, 0x14, uint32_t(sheet.records.size()));
    out.insert(out.end(), sheet.trailer.begin(), sheet.trailer.end());
    return out;
}

} // namespace dcp
