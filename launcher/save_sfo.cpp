#include "save_sfo.h"

#include <cstring>

namespace dcp {
namespace {

constexpr size_t kHeaderSize = 20;
constexpr size_t kEntrySize = 16;
constexpr uint32_t kMaxEntries = 64;
constexpr uint16_t kFormatText = 0x0204;
constexpr size_t kMaxKeyLength = 32;
constexpr size_t kMaxValueLength = 1024;

uint32_t U32(std::span<const uint8_t> bytes, size_t at) {
    uint32_t value = 0;
    std::memcpy(&value, bytes.data() + at, sizeof(value));
    return value;
}

uint16_t U16(std::span<const uint8_t> bytes, size_t at) {
    uint16_t value = 0;
    std::memcpy(&value, bytes.data() + at, sizeof(value));
    return value;
}

std::string CString(std::span<const uint8_t> bytes, size_t from, size_t limit) {
    std::string text;
    for (size_t i = from; i < bytes.size() && i < from + limit && bytes[i] != 0; ++i) {
        text.push_back(char(bytes[i]));
    }
    return text;
}

bool IsTitleId(const std::string& text) {
    if (text.size() != 9) return false;
    for (size_t i = 0; i < 9; ++i) {
        const bool letter = text[i] >= 'A' && text[i] <= 'Z';
        const bool digit = text[i] >= '0' && text[i] <= '9';
        if (i < 4 ? !letter : !digit) return false;
    }
    return true;
}

} // namespace

std::optional<SfoInfo> ParseParamSfo(std::span<const uint8_t> bytes) {
    static const uint8_t kMagic[4] = {0, 'P', 'S', 'F'};
    if (bytes.size() < kHeaderSize || std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) {
        return std::nullopt;
    }
    const uint32_t key_table = U32(bytes, 8), data_table = U32(bytes, 12), count = U32(bytes, 16);
    if (count > kMaxEntries || kHeaderSize + size_t(count) * kEntrySize > bytes.size() || key_table > bytes.size() ||
        data_table > bytes.size()) {
        return std::nullopt;
    }
    SfoInfo info;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t entry = kHeaderSize + size_t(i) * kEntrySize;
        const size_t key_at = size_t(key_table) + U16(bytes, entry);
        const uint16_t format = U16(bytes, entry + 2);
        const uint32_t length = U32(bytes, entry + 4);
        const size_t data_at = size_t(data_table) + U32(bytes, entry + 12);
        if (key_at >= bytes.size() || format != kFormatText) continue;
        if (length > kMaxValueLength || data_at > bytes.size() || length > bytes.size() - data_at) continue;
        const std::string key = CString(bytes, key_at, kMaxKeyLength + 1);
        if (key.empty() || key.size() > kMaxKeyLength) continue;
        info.text[key] = CString(bytes, data_at, length);
    }
    return info;
}

std::string TitleIdOfSfo(const SfoInfo& sfo) {
    if (const auto found = sfo.text.find("SAVEDATA_DIRECTORY"); found != sfo.text.end()) {
        const std::string head = found->second.substr(0, 9);
        if (IsTitleId(head)) return head;
    }
    if (const auto found = sfo.text.find("PARENT_DIRECTORY"); found != sfo.text.end()) {
        std::string name = found->second;
        if (!name.empty() && name.front() == '/') name.erase(0, 1);
        if (IsTitleId(name)) return name;
    }
    return {};
}

} // namespace dcp
