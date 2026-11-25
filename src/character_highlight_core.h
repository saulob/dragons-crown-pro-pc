// Character Highlight: class and atlas naming, FTX layout and the atlas signature table, shared by the launcher's
// generator, the runtime and the tests.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dcp::highlight {

inline constexpr int kClassCount = 6;
inline constexpr int kMaxColor = 9;
inline constexpr std::array<const char*, kClassCount> kClassNames = {"Fighter", "Amazon", "Elf", "Dwarf", "Sorceress", "Wizard"};
inline constexpr char kTableFile[] = "atlas-signatures.txt";
inline constexpr char kTableHeader[] = "dcp-atlas-signatures 1";

inline constexpr size_t kSampleWindows = 16;
inline constexpr size_t kSampleBytes = 4096;
inline constexpr uint64_t kMinAtlasBytes = 1u << 20;

inline constexpr int kPlayerSlots = 4;

enum class PadButton { Back, LeftStick, RightStick, LeftShoulder, RightShoulder };

struct Chord {
    const char* name;
    const char* label;
    PadButton first, second;
};

inline constexpr std::array<Chord, 5> kChords = {{
    {"back+r3", "View / Back + R3", PadButton::Back, PadButton::RightStick},
    {"back+l3", "View / Back + L3", PadButton::Back, PadButton::LeftStick},
    {"l1+r3", "L1 + R3", PadButton::LeftShoulder, PadButton::RightStick},
    {"r1+l3", "R1 + L3", PadButton::RightShoulder, PadButton::LeftStick},
    {"off", "Off", PadButton::Back, PadButton::Back},
}};
inline constexpr char kChordKey[] = "controller_character_highlight";
inline constexpr int kDefaultChord = 0;
inline constexpr int kChordOff = 4;

inline int ChordFromName(std::string_view name) {
    for (size_t i = 0; i < kChords.size(); ++i) {
        if (name == kChords[i].name) return static_cast<int>(i);
    }
    return -1;
}

inline const char* ClassName(int klass) {
    return klass >= 0 && klass < kClassCount ? kClassNames[static_cast<size_t>(klass)] : nullptr;
}

inline std::string AtlasPath(int klass, int color) {
    const char* name = ClassName(klass);
    if (!name || color < 0 || color > kMaxColor) return {};
    return std::string("player/") + name + "0" + std::to_string(color) + ".ftx";
}

struct Texture {
    uint32_t width = 0, height = 0, data_size = 0, format_word = 0;
    uint64_t data_offset = 0;
    bool operator==(const Texture&) const = default;
};

namespace detail {
inline bool Field(const std::string& line, const char* name, int base, unsigned long long& value) {
    const size_t at = line.find(name);
    if (at == std::string::npos) return false;
    const char* begin = line.c_str() + at + std::strlen(name);
    char* end = nullptr;
    value = std::strtoull(begin, &end, base);
    return end != begin;
}
inline uint32_t Word(const uint8_t* bytes, size_t at) {
    uint32_t value;
    std::memcpy(&value, bytes + at, sizeof(value));
    return value;
}
inline constexpr uint32_t kGnfMagic = 0x20464e47;
inline constexpr size_t kGnfHeader = 0x100;
inline constexpr size_t kGnfFormatWord = 5 * 4;
inline constexpr size_t kGnfDimensionWord = 6 * 4;
inline constexpr size_t kGnfSizeWord = 11 * 4;
} // namespace detail

// Textures of an FTEX container: one GNF block each, 0x100 bytes of header, then the texels.
inline std::optional<std::vector<Texture>> ParseFtx(const uint8_t* data, size_t size) {
    using namespace detail;
    if (size < 64 || std::memcmp(data, "FTEX", 4) != 0) return std::nullopt;
    const uint32_t count = Word(data, 12);
    if (count == 0 || count > 16) return std::nullopt;
    std::vector<Texture> textures;
    size_t from = 32 + static_cast<size_t>(count) * 48;
    for (uint32_t i = 0; i < count; ++i) {
        size_t at = from;
        while (at + kGnfHeader <= size && Word(data, at) != kGnfMagic) at += 4;
        if (at + kGnfHeader > size) return std::nullopt;
        Texture texture;
        texture.format_word = Word(data, at + kGnfFormatWord);
        const uint32_t dimensions = Word(data, at + kGnfDimensionWord);
        texture.width = (dimensions & 0x3fff) + 1;
        texture.height = ((dimensions >> 14) & 0x3fff) + 1;
        texture.data_size = Word(data, at + kGnfSizeWord);
        texture.data_offset = at + kGnfHeader;
        if (texture.data_size < kMinAtlasBytes || texture.data_offset + texture.data_size > size) return std::nullopt;
        textures.push_back(texture);
        from = static_cast<size_t>(texture.data_offset) + texture.data_size;
    }
    return textures;
}

// FNV-1a over evenly spaced windows of the texel data: cheap enough to run once on a 16 MiB image.
inline uint64_t SampleSignature(const uint8_t* texels, uint64_t size) {
    uint64_t hash = 0xcbf29ce484222325ull;
    const uint64_t stride = size / kSampleWindows;
    for (size_t window = 0; window < kSampleWindows; ++window) {
        const uint64_t at = (stride * window) & ~uint64_t{255};
        const uint64_t length = at + kSampleBytes <= size ? kSampleBytes : (at < size ? size - at : 0);
        for (uint64_t i = 0; i < length; ++i) {
            hash = (hash ^ texels[at + i]) * 0x100000001b3ull;
        }
    }
    hash = (hash ^ size) * 0x100000001b3ull;
    return hash;
}

// What identifies a look's atlas: the class and color of the native atlas, or the hash of a custom atlas's entry name.
inline constexpr uint32_t kCustomKeyFlag = 0x800000;
inline constexpr uint32_t kRemoteCustomKey = 0xffffff;
inline constexpr uint32_t kKeyMaskValue = 0xffffff;
inline constexpr int kNoKey = -1;

inline uint32_t BaseKey(int klass, int color) {
    return static_cast<uint32_t>(klass) | (static_cast<uint32_t>(color) << 8);
}

inline uint32_t CustomKey(std::string_view entry) {
    uint32_t hash = 2166136261u;
    for (const char c : entry) {
        hash = (hash ^ static_cast<uint8_t>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c)) * 16777619u;
    }
    return kCustomKeyFlag | (hash & 0x7fffff);
}

struct AtlasEntry {
    int klass = 0, color = 0, texture = 0;
    uint32_t custom = 0;
    Texture texels;
    uint64_t signature = 0;
    bool operator==(const AtlasEntry&) const = default;
    uint32_t key() const { return custom ? custom : BaseKey(klass, color); }
};

inline std::string FormatTable(const std::vector<AtlasEntry>& entries) {
    std::string text = std::string(kTableHeader) + "\n";
    char line[256];
    for (const AtlasEntry& e : entries) {
        std::snprintf(line, sizeof(line), "atlas class=%d color=%d texture=%d width=%u height=%u format=0x%08x size=%u signature=0x%016llx\n",
                      e.klass, e.color, e.texture, e.texels.width, e.texels.height, e.texels.format_word, e.texels.data_size,
                      static_cast<unsigned long long>(e.signature));
        text += line;
    }
    return text;
}

inline std::optional<std::vector<AtlasEntry>> ParseTable(std::string_view text) {
    std::vector<AtlasEntry> entries;
    bool header = false;
    while (!text.empty()) {
        const size_t end = text.find('\n');
        const std::string line(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.empty()) continue;
        if (!header) {
            if (line != kTableHeader) return std::nullopt;
            header = true;
            continue;
        }
        AtlasEntry e;
        unsigned long long klass = 0, color = 0, texture = 0, width = 0, height = 0, format = 0, size = 0, signature = 0;
        if (!detail::Field(line, "atlas class=", 10, klass) || !detail::Field(line, " color=", 10, color) ||
            !detail::Field(line, " texture=", 10, texture) || !detail::Field(line, " width=", 10, width) ||
            !detail::Field(line, " height=", 10, height) || !detail::Field(line, " format=0x", 16, format) ||
            !detail::Field(line, " size=", 10, size) || !detail::Field(line, " signature=0x", 16, signature) ||
            klass >= kClassCount || color > kMaxColor || texture > 15 || size < kMinAtlasBytes || size > 0xffffffffull ||
            width > 0xffffffffull || height > 0xffffffffull || format > 0xffffffffull) {
            return std::nullopt;
        }
        e.klass = static_cast<int>(klass);
        e.color = static_cast<int>(color);
        e.texture = static_cast<int>(texture);
        e.texels.width = static_cast<uint32_t>(width);
        e.texels.height = static_cast<uint32_t>(height);
        e.texels.format_word = static_cast<uint32_t>(format);
        e.texels.data_size = static_cast<uint32_t>(size);
        e.signature = signature;
        entries.push_back(e);
    }
    if (!header) return std::nullopt;
    return entries;
}

// The entries one atlas file contributes, from its decompressed bytes.
inline std::optional<std::vector<AtlasEntry>> EntriesFor(int klass, int color, const uint8_t* ftx, size_t size) {
    const auto textures = ParseFtx(ftx, size);
    if (!textures) return std::nullopt;
    std::vector<AtlasEntry> entries;
    for (size_t i = 0; i < textures->size(); ++i) {
        AtlasEntry e;
        e.klass = klass;
        e.color = color;
        e.texture = static_cast<int>(i);
        e.texels = (*textures)[i];
        e.signature = SampleSignature(ftx + e.texels.data_offset, e.texels.data_size);
        entries.push_back(e);
    }
    return entries;
}

} // namespace dcp::highlight
