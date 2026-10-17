#include "trophies/trp_archive.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "crypto.h"

namespace fs = std::filesystem;

namespace dcp::trophies {
namespace {

constexpr uint32_t kMagic = 0xDCA24D00;
constexpr size_t kHeaderSize = 0x60;
constexpr size_t kEntryFixedSize = 52;
constexpr uint64_t kMaxFileSize = 256u << 20;
constexpr uint32_t kMaxEntries = 1024;
constexpr uint64_t kMaxEntrySize = 8u << 20;
constexpr size_t kNameSize = 32;
constexpr uint32_t kFlagPng = 0;
constexpr uint32_t kFlagEncryptedXml = 3;

uint32_t Be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

uint64_t Be64(const uint8_t* p) {
    return uint64_t(Be32(p)) << 32 | Be32(p + 4);
}

bool ReadFile(const fs::path& path, std::vector<uint8_t>& data) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec || size < kHeaderSize || size > kMaxFileSize) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    data.resize(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return in.gcount() == static_cast<std::streamsize>(data.size());
}

// Archive entry names are untrusted: reject anything that could escape the destination folder.
bool SafeName(const std::string& name) {
    if (name.empty() || name[0] == '.' || name.find("..") != std::string::npos) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '_' || c == '-';
    });
}

std::string Upper(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

bool EndsWith(const std::string& text, const std::string& suffix) {
    return text.size() > suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool LooksLikeXml(const std::string& text) {
    size_t start = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    for (;;) {
        while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
        if (text.compare(start, 4, "<!--") != 0) {
            break;
        }
        const size_t end = text.find("-->", start + 4);
        if (end == std::string::npos) {
            return false;
        }
        start = end + 3;
    }
    return text.compare(start, 5, "<?xml") == 0 || text.compare(start, 11, "<trophyconf") == 0;
}

bool WriteFile(const fs::path& path, const void* data, size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    out.flush();
    return static_cast<bool>(out);
}

} // namespace

bool DeriveListKey(const TrophyKey& trophy_key, const std::string& np_comm_id, TrophyKey& list_key) {
    if (np_comm_id.size() < 12) {
        return false;
    }
    list_key = {};
    std::memcpy(list_key.data(), np_comm_id.data(), std::min<size_t>(np_comm_id.size(), list_key.size()));
    uint8_t zero_iv[16] = {};
    crypto::AesKey aes;
    return aes.Init(crypto::AesMode::kCbc, trophy_key.data()) && aes.Encrypt(list_key.data(), list_key.size(), zero_iv);
}

bool DecryptTrophyList(const TrophyKey& trophy_key, const std::string& np_comm_id, const uint8_t* entry, size_t size,
                       std::string& xml) {
    constexpr size_t kIvSize = 16;
    if (size <= kIvSize || (size - kIvSize) % 16 != 0) {
        return false;
    }
    TrophyKey list_key;
    crypto::AesKey aes;
    if (!DeriveListKey(trophy_key, np_comm_id, list_key) || !aes.Init(crypto::AesMode::kCbc, list_key.data())) {
        return false;
    }
    uint8_t iv[kIvSize];
    std::memcpy(iv, entry, kIvSize);
    std::vector<uint8_t> text(entry + kIvSize, entry + size);
    if (!aes.Decrypt(text.data(), text.size(), iv)) {
        return false;
    }
    const auto last = std::find(text.rbegin(), text.rend(), static_cast<uint8_t>('>'));
    if (last != text.rend()) {
        text.resize(static_cast<size_t>(text.rend() - last));
    }
    xml.assign(text.begin(), text.end());
    return LooksLikeXml(xml);
}

TrpResult ExtractTrp(const fs::path& trp, const std::string& np_comm_id, const std::optional<TrophyKey>& key,
                     const fs::path& dest) {
    TrpResult result;
    std::vector<uint8_t> file;
    if (!ReadFile(trp, file)) {
        return result;
    }
    result.status = TrpStatus::kBadHeader;
    const uint8_t* header = file.data();
    const uint32_t entries = Be32(header + 0x10);
    const uint32_t entry_size = Be32(header + 0x14);
    if (Be32(header) != kMagic || entries == 0 || entries > kMaxEntries || entry_size < kEntryFixedSize ||
        entry_size > 4096) {
        return result;
    }
    const uint64_t table_end = kHeaderSize + uint64_t(entries) * entry_size;
    if (table_end > file.size()) {
        return result;
    }

    struct Entry {
        std::string name;
        uint64_t pos, size;
        uint32_t flag;
    };
    std::vector<Entry> list;
    result.status = TrpStatus::kBadEntry;
    for (uint32_t i = 0; i < entries; ++i) {
        const uint8_t* raw = header + kHeaderSize + size_t(i) * entry_size;
        const size_t name_length = strnlen(reinterpret_cast<const char*>(raw), kNameSize);
        Entry entry;
        entry.name = Upper(std::string(reinterpret_cast<const char*>(raw), name_length));
        entry.pos = Be64(raw + kNameSize);
        entry.size = Be64(raw + kNameSize + 8);
        entry.flag = Be32(raw + kNameSize + 16);
        if (name_length == kNameSize || !SafeName(entry.name) || entry.size > kMaxEntrySize || entry.pos < table_end ||
            entry.pos > file.size() || entry.size > file.size() - entry.pos) {
            return result;
        }
        if (std::any_of(list.begin(), list.end(), [&](const Entry& other) { return other.name == entry.name; })) {
            return result;
        }
        list.push_back(std::move(entry));
    }

    std::error_code ec;
    fs::create_directories(dest / "Icons", ec);
    fs::create_directories(dest / "Xml", ec);
    if (ec) {
        result.status = TrpStatus::kWriteFailed;
        return result;
    }
    bool lists_skipped = false;
    for (const Entry& entry : list) {
        const uint8_t* data = file.data() + entry.pos;
        if (entry.flag == kFlagPng && EndsWith(entry.name, ".PNG")) {
            if (!WriteFile(dest / "Icons" / entry.name, data, static_cast<size_t>(entry.size))) {
                result.status = TrpStatus::kWriteFailed;
                return result;
            }
            ++result.icons;
        } else if (entry.flag == kFlagEncryptedXml) {
            if (entry.size <= 16 || (entry.size - 16) % 16 != 0) {
                result.status = TrpStatus::kBadEntry;
                return result;
            }
            if (!key) {
                lists_skipped = true;
                continue;
            }
            std::string xml;
            if (!DecryptTrophyList(*key, np_comm_id, data, static_cast<size_t>(entry.size), xml)) {
                std::printf("Trophies: %s did not decrypt to a trophy list\n", entry.name.c_str());
                result.status = TrpStatus::kKeyRejected;
                return result;
            }
            const size_t dot = entry.name.rfind('.');
            if (!WriteFile(dest / "Xml" / (entry.name.substr(0, dot) + ".XML"), xml.data(), xml.size())) {
                result.status = TrpStatus::kWriteFailed;
                return result;
            }
            ++result.lists;
        }
    }
    result.status = lists_skipped ? TrpStatus::kKeyMissing : TrpStatus::kOk;
    return result;
}

} // namespace dcp::trophies
