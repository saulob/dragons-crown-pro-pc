#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace dcp::trophies {

using TrophyKey = std::array<uint8_t, 16>;

enum class TrpStatus {
    kOk,
    kUnreadable,  // the file is missing, empty or beyond the size limit
    kBadHeader,
    kBadEntry,    // a name, offset or size outside what the archive can hold
    kKeyMissing,  // the icons were unpacked, the lists need the trophy key
    kKeyRejected, // the key does not decrypt the lists
    kWriteFailed,
};

struct TrpResult {
    TrpStatus status = TrpStatus::kUnreadable;
    int icons = 0;
    int lists = 0;
};

// AES-128-CBC of the NP Communication ID (zero-padded to 16 bytes, zero IV) under the trophy key.
bool DeriveListKey(const TrophyKey& trophy_key, const std::string& np_comm_id, TrophyKey& list_key);

// An encrypted list entry: a 16-byte IV, then CBC ciphertext. The text after the last '>' (padding) is dropped.
bool DecryptTrophyList(const TrophyKey& trophy_key, const std::string& np_comm_id, const uint8_t* entry, size_t size,
                       std::string& xml);

TrpResult ExtractTrp(const std::filesystem::path& trp, const std::string& np_comm_id, const std::optional<TrophyKey>& key,
                     const std::filesystem::path& dest);

} // namespace dcp::trophies
