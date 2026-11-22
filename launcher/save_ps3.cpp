#include "save_ps3.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "crypto.h"
#include "save_data.h"

namespace dcp {
namespace {

constexpr uint64_t kPfdMagic = 0x50464442;
constexpr uint64_t kSupportedVersion = 4;
constexpr size_t kVersionOffset = 8;
constexpr size_t kSlotCountOffset = 0x60;
constexpr size_t kCapacityOffset = 0x68;
constexpr size_t kUsedOffset = 0x70;
constexpr size_t kSlotTableOffset = 0x78;
constexpr size_t kEntrySize = 0x110;
constexpr size_t kEntryNameOffset = 8;
constexpr size_t kEntryNameBytes = 65;
constexpr size_t kEntryKeyOffset = 0x50;
constexpr size_t kEntryHashOffset = 0x90;
constexpr size_t kEntrySizeOffset = 0x108;
constexpr uint64_t kMaxCount = 4096;
constexpr uint64_t kMaxProtectedSize = 64u << 20;
constexpr size_t kBlock = 16;

constexpr uint8_t kContainerKey[kBlock] = {0xD4, 0x13, 0xB8, 0x96, 0x63, 0xE1, 0xFE, 0x9F,
                                           0x75, 0x14, 0x3D, 0x3B, 0xB4, 0x56, 0x52, 0x74};

constexpr uint8_t kDragonsCrownId[kBlock] = {0x10, 0xA3, 0x43, 0x54, 0x5F, 0x12, 0x60, 0x25,
                                             0x38, 0x46, 0x9E, 0xA0, 0xBA, 0x6E, 0x87, 0x9F};

constexpr std::string_view kDragonsCrownTitles[] = {"BLUS30767", "BLES01950", "NPEB01836", "NPUB31235",
                                                    "BLJM61041", "BCAS20298", "BCAS20290", "BCAS20288"};

constexpr size_t kHashKeyBytes = 20;
constexpr size_t kFillerSlots[] = {1, 2, 5, 8};
constexpr uint8_t kFillers[] = {0x0B, 0x0F, 0x0E, 0x0A};

uint64_t ReadU64Be(std::span<const uint8_t> bytes, size_t at) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) value = value << 8 | bytes[at + i];
    return value;
}

std::string Lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    return result;
}

std::array<uint8_t, kHashKeyBytes> HashKey() {
    std::array<uint8_t, kHashKeyBytes> key{};
    size_t id = 0, filler = 0;
    for (size_t i = 0; i < key.size(); ++i) {
        const bool is_filler = filler < std::size(kFillerSlots) && kFillerSlots[filler] == i;
        key[i] = is_filler ? kFillers[filler++] : kDragonsCrownId[id++];
    }
    return key;
}

size_t AlignUp(uint64_t size) { return size_t((size + kBlock - 1) / kBlock * kBlock); }

} // namespace

const char* Ps3StatusText(Ps3Status status) {
    switch (status) {
    case Ps3Status::kOk: return "";
    case Ps3Status::kNotPfd: return "PARAM.PFD is not a PS3 protected-file database";
    case Ps3Status::kUnsupportedVersion: return "PARAM.PFD has a version this launcher does not read";
    case Ps3Status::kTruncated: return "PARAM.PFD is truncated";
    case Ps3Status::kNoEntry: return "PARAM.PFD does not list SAVE0.DAT";
    case Ps3Status::kBadEntry: return "PARAM.PFD has an invalid entry for SAVE0.DAT";
    case Ps3Status::kBadSize: return "SAVE0.DAT does not have the size PARAM.PFD records";
    case Ps3Status::kUnknownTitle: return "This is not a Dragon's Crown PS3 save directory";
    case Ps3Status::kHashMismatch: return "SAVE0.DAT does not match the hash PARAM.PFD records";
    case Ps3Status::kCryptoFailed: return "The decryption failed";
    case Ps3Status::kBadPlaintext: return "SAVE0.DAT decrypted, but it is not a Dragon's Crown save";
    }
    return "";
}

Ps3Status ParsePfd(std::span<const uint8_t> pfd, Ps3Pfd& out) {
    out = {};
    if (pfd.size() < kSlotTableOffset) return Ps3Status::kTruncated;
    if (ReadU64Be(pfd, 0) != kPfdMagic) return Ps3Status::kNotPfd;
    const uint64_t version = ReadU64Be(pfd, kVersionOffset);
    if (version != kSupportedVersion) return Ps3Status::kUnsupportedVersion;
    const uint64_t slots = ReadU64Be(pfd, kSlotCountOffset);
    const uint64_t capacity = ReadU64Be(pfd, kCapacityOffset);
    const uint64_t used = ReadU64Be(pfd, kUsedOffset);
    if (slots > kMaxCount || capacity > kMaxCount || used > capacity) return Ps3Status::kBadEntry;
    const uint64_t entries_end = kSlotTableOffset + slots * 8 + capacity * kEntrySize;
    if (entries_end > pfd.size()) return Ps3Status::kTruncated;
    out.version = uint32_t(version);
    out.capacity = uint32_t(capacity);
    out.used = uint32_t(used);
    return Ps3Status::kOk;
}

Ps3Status FindPfdEntry(std::span<const uint8_t> pfd, std::string_view name, Ps3Entry& out) {
    Ps3Pfd header;
    if (const Ps3Status status = ParsePfd(pfd, header); status != Ps3Status::kOk) return status;
    const size_t first = kSlotTableOffset + size_t(ReadU64Be(pfd, kSlotCountOffset)) * 8;
    const std::string wanted = Lower(name);
    for (uint32_t i = 0; i < header.capacity; ++i) {
        const size_t base = first + size_t(i) * kEntrySize;
        const uint8_t* text = pfd.data() + base + kEntryNameOffset;
        const size_t length = std::find(text, text + kEntryNameBytes, uint8_t(0)) - text;
        if (length == 0 || length == kEntryNameBytes || Lower(std::string_view(reinterpret_cast<const char*>(text), length)) != wanted) {
            continue;
        }
        const uint64_t size = ReadU64Be(pfd, base + kEntrySizeOffset);
        if (size == 0 || size > kMaxProtectedSize) return Ps3Status::kBadEntry;
        out.size = size;
        std::memcpy(out.key.data(), pfd.data() + base + kEntryKeyOffset, out.key.size());
        std::memcpy(out.hash.data(), pfd.data() + base + kEntryHashOffset, out.hash.size());
        return Ps3Status::kOk;
    }
    return Ps3Status::kNoEntry;
}

bool IsDragonsCrownPs3Directory(std::string_view save_directory) {
    const std::string name = Lower(save_directory);
    return std::any_of(std::begin(kDragonsCrownTitles), std::end(kDragonsCrownTitles), [&](std::string_view title) {
        return name.size() >= title.size() && Lower(title) == name.substr(0, title.size());
    });
}

Ps3Status DecryptPs3Save0(std::span<const uint8_t> pfd, std::span<const uint8_t> ciphertext, std::string_view save_directory,
                          std::vector<uint8_t>& plain) {
    plain.clear();
    if (!IsDragonsCrownPs3Directory(save_directory)) return Ps3Status::kUnknownTitle;
    Ps3Entry entry;
    if (const Ps3Status status = FindPfdEntry(pfd, "SAVE0.DAT", entry); status != Ps3Status::kOk) return status;
    if (ciphertext.size() != AlignUp(entry.size)) return Ps3Status::kBadSize;

    const std::array<uint8_t, kHashKeyBytes> hash_key = HashKey();
    uint8_t digest[20];
    if (!crypto::HmacSha1(hash_key.data(), hash_key.size(), ciphertext.data(), ciphertext.size(), digest)) {
        return Ps3Status::kCryptoFailed;
    }
    if (!std::equal(std::begin(digest), std::end(digest), entry.hash.begin())) return Ps3Status::kHashMismatch;

    crypto::AesKey container;
    uint8_t file_key[kBlock];
    {
        std::array<uint8_t, 64> unwrapped = entry.key;
        uint8_t iv[kBlock];
        std::memcpy(iv, hash_key.data(), kBlock);
        if (!container.Init(crypto::AesMode::kCbc, kContainerKey) || !container.Decrypt(unwrapped.data(), unwrapped.size(), iv)) {
            return Ps3Status::kCryptoFailed;
        }
        std::memcpy(file_key, unwrapped.data(), kBlock);
    }

    crypto::AesKey cipher;
    if (!cipher.Init(crypto::AesMode::kEcb, file_key)) return Ps3Status::kCryptoFailed;
    std::vector<uint8_t> mask(ciphertext.size(), 0);
    for (size_t block = 0; block < ciphertext.size() / kBlock; ++block) {
        for (int shift = 0; shift < 8; ++shift) mask[block * kBlock + size_t(7 - shift)] = uint8_t(uint64_t(block) >> (8 * shift));
    }
    plain.assign(ciphertext.begin(), ciphertext.end());
    if (!cipher.Encrypt(mask.data(), mask.size()) || !cipher.Decrypt(plain.data(), plain.size())) {
        plain.clear();
        return Ps3Status::kCryptoFailed;
    }
    for (size_t i = 0; i < plain.size(); ++i) plain[i] ^= mask[i];
    plain.resize(size_t(entry.size));
    return Ps3Status::kOk;
}

Ps3Plaintext InspectPs3Plaintext(std::span<const uint8_t> plain) {
    Ps3Plaintext info;
    if (plain.size() != kSaveFileSize) return info;
    if (std::memcmp(plain.data() + kSaveMagicOffset, "DCSD", 4) != 0) return info;
    if (ReadU64Be(plain, 0) >> 32 != SaveCrc32(plain)) return info;
    info.valid = true;
    info.slots = int(kDataSlotCount);
    for (size_t slot = 0; slot < kDataSlotCount; ++slot) {
        const size_t gold = slot * kDataSlotStride + kDataSlotGoldOffset;
        info.gold[slot] = uint32_t(plain[gold]) << 24 | uint32_t(plain[gold + 1]) << 16 | uint32_t(plain[gold + 2]) << 8 | plain[gold + 3];
        for (size_t character = 0; character < kCharacterCount; ++character) {
            const size_t record = slot * kDataSlotStride + kCharacterOffset + character * kCharacterStride;
            const bool empty_fill = plain[record] == 0xCC && plain[record + 1] == 0xCC && plain[record + 2] == 0xCC && plain[record + 3] == 0xCC;
            if (!empty_fill && plain[record + kCharName] != 0) ++info.characters;
        }
    }
    return info;
}

} // namespace dcp
