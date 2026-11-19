// The custom look of an online player as it travels between clients: a validated recipe of semantic part ids over the
// class and colour preset, never texture data. Each client draws it with its own game files.
//
// Schema 1, bytes:
//   [0] schema  [1] class 0..5  [2] base preset 0..4  [3] part count n
//   n x { [id] [flags: bit 0 hidden, bit 1 coloured] [r g b when coloured] }
// Parts are sorted by id, unique, and each changes something; anything else is refused.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dcp::appearance_net {

inline constexpr uint8_t kSchema = 1;
inline constexpr int kClassCount = 6;
inline constexpr int kBaseCount = 5;
inline constexpr size_t kMaxParts = 16;
inline constexpr size_t kHeaderBytes = 4;
inline constexpr size_t kMaxBytes = kHeaderBytes + kMaxParts * 5;

enum : uint8_t { kHidden = 1, kColored = 2 };

// A part's network id is fixed forever for its class; labels and table order may change, ids never do.
struct PartName {
    uint8_t id;
    std::string_view key;
};
std::span<const PartName> PartTable(int klass);
std::string_view PartKey(int klass, uint8_t id); // empty when the class has no such id
int PartId(int klass, std::string_view key);     // -1 when the key has none

struct NetPart {
    uint8_t id = 0;
    bool hidden = false;
    std::optional<std::array<uint8_t, 3>> color;
    bool operator==(const NetPart&) const = default;
};

struct Recipe {
    uint8_t klass = 0;
    uint8_t base = 0;
    std::vector<NetPart> parts;
    bool operator==(const Recipe&) const = default;
    bool native() const { return parts.empty(); }
};

enum class Refusal { kNone, kEmpty, kTooLarge, kSchema, kClass, kBase, kCount, kTruncated, kPart, kDuplicate, kFlags, kTrailing };
const char* RefusalText(Refusal refusal);

// Sorted by id, parts that change nothing and ids the class lacks dropped; nothing for a class or base out of range or
// more parts than kMaxParts.
std::optional<Recipe> Normalized(Recipe recipe);
std::vector<uint8_t> Encode(const Recipe& recipe); // of a normalized recipe; empty when it is not one
// Any order on the wire is read; duplicates and every other impossible structure are refused.
std::optional<Recipe> Decode(std::span<const uint8_t> bytes, Refusal* why = nullptr);

// FNV-1a over the normalized encoding: the same look gives the same hash, whatever order its parts came in. For caches
// and logs, not for trust.
uint64_t Hash(const Recipe& recipe);
std::string ShortHash(uint64_t hash); // 8 hex digits
std::string_view ClassName(int klass);

} // namespace dcp::appearance_net
