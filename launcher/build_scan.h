// Read-only mapping of a game executable: the SELF/ELF structure, the image the loader builds from it, and the masked
// signatures of every site the port's build-specific features depend on. Nothing here writes or patches anything, and a
// result never makes a build supported.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dcp::scan {

inline constexpr uint64_t kMaxImageSize = 512ull << 20;
inline constexpr size_t kReferenceBuildCount = 5;
inline constexpr uint64_t kModuleAlignment = 0x10000;

struct ProgramHeader {
    uint32_t type = 0, flags = 0;
    uint64_t offset = 0, vaddr = 0, filesz = 0, memsz = 0, align = 0;
};

struct SelfSegment {
    uint64_t flags = 0, offset = 0, size = 0, memory = 0;
};

struct Range {
    uint64_t begin = 0, end = 0;
};

// The structure of one SELF file. `valid` means the headers parsed; `mappable` means every loadable segment has its
// data in the file.
struct ExecutableInfo {
    bool valid = false;
    bool mappable = false;
    std::string error;
    uint64_t file_size = 0;
    uint8_t ident[4] = {};
    uint16_t header_size = 0, meta_size = 0;
    uint16_t elf_type = 0, machine = 0;
    uint64_t entry = 0;
    std::vector<ProgramHeader> headers;
    std::vector<SelfSegment> segments;
    uint64_t mapped_size = 0;
};

struct ModuleMap {
    std::string name;
    uint64_t base = 0, size = 0;
    bool mapped = false;
    std::string error;
};

// The loader's image: the executable at 0, each bundled module after it at the next 64 KiB boundary.
struct MappedImage {
    std::vector<uint8_t> bytes;
    std::vector<Range> executable;
    std::vector<ModuleMap> modules;
    uint64_t entry = 0;
};

ExecutableInfo ParseExecutable(std::span<const uint8_t> file);

// Copies the loadable segments of `file` into `image` at `base` (growing it) and appends the executable ranges. False
// with `error` set, and `image` left as it was, when the file does not map.
bool MapExecutable(std::span<const uint8_t> file, const ExecutableInfo& info, uint64_t base, MappedImage& image,
                   std::string& error);

// The mapped size of the executable alone, 0 when it does not map.
uint64_t MappedSize(const ExecutableInfo& info);

// Where an image offset sits in the file: the SELF data offset of the load segment holding it, if any.
bool FileOffsetOf(const ExecutableInfo& info, uint64_t rva, uint64_t& file_offset);

enum class SiteKind : uint8_t { kFunction, kHook, kCall, kJump, kLoad, kWindow };

struct ReferenceBuild {
    const char* label;
    uint64_t image_size;
    uint64_t entry;
};
extern const ReferenceBuild kReferenceBuilds[kReferenceBuildCount];

// A site found by its bytes: `pattern` is hex with "??" for bytes that move with the build; `anchor` is where in the
// pattern the site's address is. `expected` is how many identical copies the supported builds have.
struct SiteSpec {
    const char* feature;
    const char* name;
    SiteKind kind;
    const char* pattern;
    uint16_t anchor;
    uint8_t expected;
    uint32_t reference[kReferenceBuildCount]; // first match in each reference build, 0 when absent
};

enum class RelationKind : uint8_t { kCallsSite, kSameTarget };

// kCallsSite: every match of `from` is a call or jump to the `to` site. kSameTarget: `from` and `to` call one function.
struct RelationSpec {
    RelationKind kind;
    const char* feature;
    const char* from;
    const char* to;
};

std::span<const SiteSpec> Sites();
std::span<const RelationSpec> Relations();

enum class SiteStatus : uint8_t { kUnique, kGroup, kMissing, kAmbiguous };

struct SiteResult {
    const SiteSpec* spec = nullptr;
    SiteStatus status = SiteStatus::kMissing;
    size_t count = 0;                // all matches
    std::vector<uint64_t> matches;   // the first few, as image offsets of the site
};

enum class RelationStatus : uint8_t { kHolds, kBroken, kUnverifiable };

struct RelationResult {
    const RelationSpec* spec = nullptr;
    RelationStatus status = RelationStatus::kUnverifiable;
    std::string detail;
};

struct LzcntResult {
    size_t count = 0;
    uint64_t digest = 0;
    uint64_t first = 0, last = 0;
    bool complete = false;
};

struct ResolutionSiteResult {
    size_t expected = 0;
    size_t unique = 0, ambiguous = 0, missing = 0;
    bool complete = false;
};

struct ScanResult {
    std::vector<SiteResult> sites;
    std::vector<RelationResult> relations;
    LzcntResult lzcnt;
    ResolutionSiteResult resolution;
};

inline constexpr size_t kMaxListedMatches = 8;
inline constexpr size_t kLzcntInstructions = 62;
inline constexpr uint64_t kLzcntDigest = 0xe53eadb9792a3e8eull;

// Reads the image only.
ScanResult ScanImage(const MappedImage& image);

// Matches of one hex pattern ("??" wildcards) inside the executable ranges, as the offsets of the pattern's start.
// Empty for a pattern that does not parse. `total` gets the number of matches, `limit` caps the offsets kept.
std::vector<uint64_t> FindPattern(const MappedImage& image, std::string_view pattern, size_t limit, size_t& total);
// The same over bytes owned by someone else, with no copy.
std::vector<uint64_t> FindPattern(const uint8_t* bytes, uint64_t size, std::span<const Range> executable,
                                  std::string_view pattern, size_t limit, size_t& total);

// FNV-1a over the pattern's solid bytes as they are at `start`; the fingerprint a later reader can recompute.
uint64_t SolidFingerprint(const MappedImage& image, std::string_view pattern, uint64_t start);

const char* SiteStatusName(SiteStatus status);

} // namespace dcp::scan
