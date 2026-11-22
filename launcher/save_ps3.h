// Reading a Dragon's Crown PS3 save: the PARAM.PFD that describes its protected files, and the unwrapping of SAVE0.DAT into
// the plaintext the game wrote. Only PFD version 4 and Dragon's Crown's own save directories are handled; no console
// identifier is involved. The plaintext is big-endian, and nothing here converts it.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dcp {

enum class Ps3Status {
    kOk,
    kNotPfd,
    kUnsupportedVersion,
    kTruncated,
    kNoEntry,
    kBadEntry,
    kBadSize,
    kUnknownTitle,
    kHashMismatch,
    kCryptoFailed,
    kBadPlaintext,
};

const char* Ps3StatusText(Ps3Status status);

struct Ps3Entry {
    uint64_t size = 0;
    std::array<uint8_t, 64> key{};
    std::array<uint8_t, 20> hash{};
};

struct Ps3Pfd {
    uint32_t version = 0;
    uint32_t capacity = 0;
    uint32_t used = 0;
};

Ps3Status ParsePfd(std::span<const uint8_t> pfd, Ps3Pfd& out);
// The entry named `name` (case-insensitive), and nothing about any other.
Ps3Status FindPfdEntry(std::span<const uint8_t> pfd, std::string_view name, Ps3Entry& out);

// True for the save directories Dragon's Crown PS3 uses: a name that starts with one of its Title IDs.
bool IsDragonsCrownPs3Directory(std::string_view save_directory);

// Unwraps SAVE0.DAT as it sits in a PS3 save folder (`ciphertext`, `pfd`, and the save directory's name). On success `plain`
// holds the logical file, and the entry's stored hash has matched the ciphertext.
Ps3Status DecryptPs3Save0(std::span<const uint8_t> pfd, std::span<const uint8_t> ciphertext, std::string_view save_directory,
                          std::vector<uint8_t>& plain);

struct Ps3Plaintext {
    bool valid = false;
    int slots = 0;
    int characters = 0;
    std::array<uint32_t, 3> gold{};
};

// Header, size and checksum of a decrypted PS3 SAVE0.DAT, and what its three DATA slots hold.
Ps3Plaintext InspectPs3Plaintext(std::span<const uint8_t> plain);

} // namespace dcp
