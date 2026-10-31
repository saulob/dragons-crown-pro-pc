#include "build_scan.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "dcp_resolution.h"

namespace dcp::scan {
namespace {

constexpr uint8_t kSelfMagic[4] = {'O', 0x15, '=', 0x1d};
constexpr uint8_t kElfIdent[7] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
constexpr uint32_t kLoad = 1;
constexpr uint32_t kSceRelro = 0x61000010;
constexpr uint32_t kFlagExecute = 1;
constexpr uint64_t kBlockedSegment = 0x800;
constexpr uint64_t kEncryptedOrCompressed = 10;
constexpr size_t kProgramHeaderSize = 56;
constexpr size_t kMaxProgramHeaders = 255;
constexpr size_t kMaxSelfSegments = 4096;

bool AddOk(uint64_t a, uint64_t b, uint64_t& out) {
    out = a + b;
    return out >= a;
}

bool InFile(uint64_t file_size, uint64_t offset, uint64_t length) {
    uint64_t end = 0;
    return AddOk(offset, length, end) && end <= file_size;
}

template <typename T>
bool ReadAt(std::span<const uint8_t> file, uint64_t offset, T& value) {
    if (!InFile(file.size(), offset, sizeof(T))) {
        return false;
    }
    std::memcpy(&value, file.data() + offset, sizeof(T));
    return true;
}

bool IsLoadable(const ProgramHeader& header) {
    return header.type == kLoad || header.type == kSceRelro;
}

// The SELF block segment that carries each program header's data, or -1.
std::vector<int> SegmentOwners(const ExecutableInfo& info) {
    std::vector<int> owner(info.headers.size(), -1);
    for (size_t i = 0; i < info.segments.size(); ++i) {
        const SelfSegment& segment = info.segments[i];
        if (!(segment.flags & kBlockedSegment)) {
            continue;
        }
        const uint64_t index = (segment.flags >> 20) & 4095;
        if (index < owner.size() && owner[index] < 0) {
            owner[index] = static_cast<int>(i);
        }
    }
    return owner;
}

bool MappedExtent(const ExecutableInfo& info, uint64_t& extent) {
    extent = 0;
    for (const ProgramHeader& header : info.headers) {
        uint64_t end = 0;
        if (!IsLoadable(header)) {
            continue;
        }
        if (!AddOk(header.vaddr, header.memsz, end) || end > kMaxImageSize) {
            return false;
        }
        extent = std::max(extent, end);
    }
    return extent > 0;
}

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> wildcard;
    size_t anchor_index = 0; // first solid byte
};

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ParsePattern(std::string_view text, Pattern& pattern) {
    pattern = {};
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == ' ') {
            ++i;
            continue;
        }
        if (i + 2 > text.size()) {
            return false;
        }
        if (text[i] == '?' && text[i + 1] == '?') {
            pattern.bytes.push_back(0);
            pattern.wildcard.push_back(1);
        } else {
            const int high = HexValue(text[i]), low = HexValue(text[i + 1]);
            if (high < 0 || low < 0) {
                return false;
            }
            pattern.bytes.push_back(static_cast<uint8_t>(high << 4 | low));
            pattern.wildcard.push_back(0);
        }
        i += 2;
    }
    const auto solid = std::find(pattern.wildcard.begin(), pattern.wildcard.end(), uint8_t{0});
    if (solid == pattern.wildcard.end()) {
        return false;
    }
    pattern.anchor_index = static_cast<size_t>(solid - pattern.wildcard.begin());
    return true;
}

std::vector<uint64_t> Search(const MappedImage& image, const Pattern& pattern, size_t limit, size_t& total) {
    std::vector<uint64_t> found;
    total = 0;
    const size_t length = pattern.bytes.size();
    const uint8_t key = pattern.bytes[pattern.anchor_index];
    for (const Range& range : image.executable) {
        if (range.end > image.bytes.size() || range.end < range.begin || range.end - range.begin < length) {
            continue;
        }
        const uint8_t* base = image.bytes.data();
        const uint64_t last = range.end - length; // the last start
        uint64_t at = range.begin + pattern.anchor_index;
        const uint64_t scan_end = last + pattern.anchor_index + 1;
        while (at < scan_end) {
            const void* hit = std::memchr(base + at, key, scan_end - at);
            if (!hit) {
                break;
            }
            at = static_cast<uint64_t>(static_cast<const uint8_t*>(hit) - base);
            const uint64_t start = at - pattern.anchor_index;
            bool same = true;
            for (size_t k = 0; k < length && same; ++k) {
                same = pattern.wildcard[k] || base[start + k] == pattern.bytes[k];
            }
            if (same) {
                ++total;
                if (found.size() < limit) {
                    found.push_back(start);
                }
            }
            ++at;
        }
    }
    return found;
}

std::string Hex(uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

const SiteResult* FindSite(const ScanResult& result, std::string_view feature, std::string_view name) {
    for (const SiteResult& site : result.sites) {
        if (feature == site.spec->feature && name == site.spec->name) {
            return &site;
        }
    }
    return nullptr;
}

bool UniqueSite(const SiteResult* site) {
    return site && site->status == SiteStatus::kUnique && !site->matches.empty();
}

bool BranchTarget(const MappedImage& image, uint64_t at, uint64_t& target) {
    if (at > image.bytes.size() || image.bytes.size() - at < 5) {
        return false;
    }
    const uint8_t opcode = image.bytes[at];
    if (opcode != 0xe8 && opcode != 0xe9) {
        return false;
    }
    int32_t displacement = 0;
    std::memcpy(&displacement, image.bytes.data() + at + 1, sizeof(displacement));
    const int64_t result = static_cast<int64_t>(at) + 5 + displacement;
    if (result < 0) {
        return false;
    }
    target = static_cast<uint64_t>(result);
    return true;
}

RelationResult CheckRelation(const MappedImage& image, const ScanResult& result, const RelationSpec& spec) {
    RelationResult relation;
    relation.spec = &spec;
    const SiteResult* from = FindSite(result, spec.feature, spec.from);
    const SiteResult* to = FindSite(result, spec.feature, spec.to);
    const bool from_found = from && (from->status == SiteStatus::kUnique || from->status == SiteStatus::kGroup);
    if (!from_found) {
        relation.detail = std::string(spec.from) + " not located";
        return relation;
    }
    if (spec.kind == RelationKind::kCallsSite) {
        if (!UniqueSite(to)) {
            relation.detail = std::string(spec.to) + " not uniquely located";
            return relation;
        }
        for (const uint64_t at : from->matches) {
            uint64_t target = 0;
            if (!BranchTarget(image, at, target) || target != to->matches.front()) {
                relation.status = RelationStatus::kBroken;
                relation.detail = "branch at " + Hex(at) + " does not reach " + spec.to;
                return relation;
            }
        }
        relation.status = RelationStatus::kHolds;
        return relation;
    }
    if (!UniqueSite(from) || !UniqueSite(to)) {
        relation.detail = "both sites must be uniquely located";
        return relation;
    }
    uint64_t first = 0, second = 0;
    if (!BranchTarget(image, from->matches.front(), first) || !BranchTarget(image, to->matches.front(), second)) {
        relation.status = RelationStatus::kBroken;
        relation.detail = "not a call or jump";
        return relation;
    }
    relation.status = first == second ? RelationStatus::kHolds : RelationStatus::kBroken;
    if (first != second) {
        relation.detail = "the two sites call different functions";
    }
    return relation;
}

// An LZCNT (F3 [REX] 0F BD /r, optionally under a 66 prefix) and the length of its encoding.
bool LzcntAt(const std::vector<uint8_t>& bytes, uint64_t at, uint64_t end, size_t& opcode_length, size_t& total_length) {
    size_t i = 0;
    const auto byte = [&](size_t k) -> int { return at + k < end ? bytes[at + k] : -1; };
    if (byte(0) == 0x66) {
        i = 1;
    }
    if (byte(i) != 0xf3) {
        return false;
    }
    ++i;
    if (byte(i) >= 0x40 && byte(i) <= 0x4f) {
        ++i;
    }
    if (byte(i) != 0x0f || byte(i + 1) != 0xbd) {
        return false;
    }
    i += 2;
    opcode_length = i;
    const int modrm = byte(i);
    if (modrm < 0) {
        return false;
    }
    ++i;
    const int mod = modrm >> 6, rm = modrm & 7;
    if (mod != 3 && rm == 4) {
        const int sib = byte(i);
        if (sib < 0) {
            return false;
        }
        ++i;
        if (mod == 0 && (sib & 7) == 5) {
            i += 4;
        }
    }
    if (mod == 1) {
        i += 1;
    } else if (mod == 2 || (mod == 0 && rm == 5)) {
        i += 4;
    }
    if (at + i > end) {
        return false;
    }
    total_length = i;
    return true;
}

LzcntResult ScanLzcnt(const MappedImage& image) {
    LzcntResult result;
    uint64_t digest = 0xcbf29ce484222325ull;
    for (const Range& range : image.executable) {
        if (range.end > image.bytes.size() || range.end < range.begin) {
            continue;
        }
        uint64_t at = range.begin;
        while (at < range.end) {
            size_t opcode_length = 0, total = 0;
            if (!LzcntAt(image.bytes, at, range.end, opcode_length, total)) {
                ++at;
                continue;
            }
            if (result.count == 0) {
                result.first = at;
            }
            result.last = at;
            ++result.count;
            for (size_t k = 0; k < total; ++k) {
                digest = (digest ^ image.bytes[at + k]) * 0x100000001b3ull;
            }
            at += opcode_length;
        }
    }
    result.digest = digest;
    result.complete = result.count == kLzcntInstructions && digest == kLzcntDigest;
    return result;
}

ResolutionSiteResult ScanResolution(const MappedImage& image) {
    ResolutionSiteResult result;
    result.expected = DCP_CUSA10209_102_SIGNATURE_COUNT;
    if (image.bytes.empty()) {
        result.missing = result.expected;
        return result;
    }
    DcpResolutionSite sites[DCP_CUSA10209_102_SIGNATURE_COUNT];
    DcpResolutionSignatureResult found[DCP_CUSA10209_102_SIGNATURE_COUNT] = {};
    const int all = dcp_find_resolution_sites(image.bytes.data(), image.bytes.size(), dcp_cusa10209_102_signatures,
                                              DCP_CUSA10209_102_SIGNATURE_COUNT, sites, found);
    for (const DcpResolutionSignatureResult& item : found) {
        if (item.matches >= 2) {
            ++result.ambiguous;
        } else if (item.matches == 1 && item.valid) {
            ++result.unique;
        } else {
            ++result.missing;
        }
    }
    result.complete = all == 1 && result.unique == result.expected;
    return result;
}

} // namespace

ExecutableInfo ParseExecutable(std::span<const uint8_t> file) {
    ExecutableInfo info;
    info.file_size = file.size();
    if (file.size() < 32 || std::memcmp(file.data(), kSelfMagic, 4) != 0) {
        info.error = "not a PS4 SELF";
        return info;
    }
    std::memcpy(info.ident, file.data(), 4);
    uint16_t count = 0;
    ReadAt(file, 0x10, info.header_size);
    ReadAt(file, 0x12, info.meta_size);
    if (!ReadAt(file, 24, count) || count > kMaxSelfSegments) {
        info.error = "invalid SELF segment count";
        return info;
    }
    const uint64_t base = 32 + uint64_t{count} * 32;
    if (!InFile(file.size(), base, 64)) {
        info.error = "truncated SELF header";
        return info;
    }
    for (uint16_t i = 0; i < count; ++i) {
        SelfSegment segment;
        const uint64_t at = 32 + uint64_t{i} * 32;
        ReadAt(file, at, segment.flags);
        ReadAt(file, at + 8, segment.offset);
        ReadAt(file, at + 16, segment.size);
        ReadAt(file, at + 24, segment.memory);
        info.segments.push_back(segment);
    }
    if (std::memcmp(file.data() + base, kElfIdent, sizeof(kElfIdent)) != 0) {
        info.error = "not a little-endian 64-bit ELF";
        return info;
    }
    uint64_t phoff = 0;
    uint16_t phentsize = 0, phnum = 0;
    ReadAt(file, base + 16, info.elf_type);
    ReadAt(file, base + 18, info.machine);
    ReadAt(file, base + 24, info.entry);
    ReadAt(file, base + 32, phoff);
    ReadAt(file, base + 54, phentsize);
    ReadAt(file, base + 56, phnum);
    if (info.machine != 62) {
        info.error = "not an x86-64 ELF";
        return info;
    }
    if (phentsize != kProgramHeaderSize || phnum == 0 || phnum > kMaxProgramHeaders) {
        info.error = "unsupported program header table";
        return info;
    }
    for (uint16_t i = 0; i < phnum; ++i) {
        uint64_t at = 0;
        if (!AddOk(base, phoff, at) || !AddOk(at, uint64_t{i} * kProgramHeaderSize, at) ||
            !InFile(file.size(), at, kProgramHeaderSize)) {
            info.error = "truncated program header table";
            info.headers.clear();
            return info;
        }
        ProgramHeader header;
        ReadAt(file, at, header.type);
        ReadAt(file, at + 4, header.flags);
        ReadAt(file, at + 8, header.offset);
        ReadAt(file, at + 16, header.vaddr);
        ReadAt(file, at + 32, header.filesz);
        ReadAt(file, at + 40, header.memsz);
        ReadAt(file, at + 48, header.align);
        info.headers.push_back(header);
    }
    info.valid = true;

    const std::vector<int> owner = SegmentOwners(info);
    info.mappable = MappedExtent(info, info.mapped_size);
    for (size_t i = 0; i < info.headers.size() && info.mappable; ++i) {
        const ProgramHeader& header = info.headers[i];
        if (!IsLoadable(header) || header.filesz == 0) {
            continue;
        }
        if (header.filesz > header.memsz || owner[i] < 0) {
            info.mappable = false;
            break;
        }
        const SelfSegment& segment = info.segments[static_cast<size_t>(owner[i])];
        if ((segment.flags & kEncryptedOrCompressed) || segment.size != header.filesz || segment.memory != segment.size ||
            !InFile(file.size(), segment.offset, segment.size)) {
            info.mappable = false;
        }
    }
    if (!info.mappable) {
        info.mapped_size = 0;
        info.error = "loadable segments are not all present in plain form";
    }
    return info;
}

uint64_t MappedSize(const ExecutableInfo& info) {
    return info.mappable ? info.mapped_size : 0;
}

bool MapExecutable(std::span<const uint8_t> file, const ExecutableInfo& info, uint64_t base, MappedImage& image,
                   std::string& error) {
    if (!info.valid || !info.mappable) {
        error = info.error.empty() ? "executable does not map" : info.error;
        return false;
    }
    uint64_t end = 0;
    if (!AddOk(base, info.mapped_size, end) || end > kMaxImageSize) {
        error = "linked image exceeds the 512 MiB limit";
        return false;
    }
    if (base < image.bytes.size()) {
        error = "overlapping module layout";
        return false;
    }
    const std::vector<int> owner = SegmentOwners(info);
    image.bytes.resize(static_cast<size_t>(end), 0);
    for (size_t i = 0; i < info.headers.size(); ++i) {
        const ProgramHeader& header = info.headers[i];
        if (!IsLoadable(header)) {
            continue;
        }
        if (header.filesz > 0) {
            const SelfSegment& segment = info.segments[static_cast<size_t>(owner[i])];
            std::memcpy(image.bytes.data() + base + header.vaddr, file.data() + segment.offset,
                        static_cast<size_t>(segment.size));
        }
        if (header.type == kLoad && (header.flags & kFlagExecute)) {
            image.executable.push_back({base + header.vaddr, base + header.vaddr + header.memsz});
        }
    }
    return true;
}

bool FileOffsetOf(const ExecutableInfo& info, uint64_t rva, uint64_t& file_offset) {
    const std::vector<int> owner = SegmentOwners(info);
    for (size_t i = 0; i < info.headers.size(); ++i) {
        const ProgramHeader& header = info.headers[i];
        if (!IsLoadable(header) || header.filesz == 0 || owner[i] < 0 || rva < header.vaddr ||
            rva - header.vaddr >= header.filesz) {
            continue;
        }
        file_offset = info.segments[static_cast<size_t>(owner[i])].offset + (rva - header.vaddr);
        return true;
    }
    return false;
}

std::vector<uint64_t> FindPattern(const MappedImage& image, std::string_view pattern, size_t limit, size_t& total) {
    Pattern parsed;
    total = 0;
    if (!ParsePattern(pattern, parsed)) {
        return {};
    }
    return Search(image, parsed, limit, total);
}

uint64_t SolidFingerprint(const MappedImage& image, std::string_view pattern, uint64_t start) {
    Pattern parsed;
    if (!ParsePattern(pattern, parsed) || start > image.bytes.size() || image.bytes.size() - start < parsed.bytes.size()) {
        return 0;
    }
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t k = 0; k < parsed.bytes.size(); ++k) {
        if (!parsed.wildcard[k]) {
            hash = (hash ^ image.bytes[start + k]) * 0x100000001b3ull;
        }
    }
    return hash;
}

const char* SiteStatusName(SiteStatus status) {
    switch (status) {
    case SiteStatus::kUnique: return "unique";
    case SiteStatus::kGroup: return "matched";
    case SiteStatus::kMissing: return "missing";
    case SiteStatus::kAmbiguous: return "ambiguous";
    }
    return "?";
}

ScanResult ScanImage(const MappedImage& image) {
    ScanResult result;
    for (const SiteSpec& spec : Sites()) {
        SiteResult site;
        site.spec = &spec;
        size_t total = 0;
        site.matches = FindPattern(image, spec.pattern, kMaxListedMatches, total);
        site.count = total;
        for (uint64_t& match : site.matches) {
            match += spec.anchor;
        }
        if (total == 0) {
            site.status = SiteStatus::kMissing;
        } else if (total != spec.expected) {
            site.status = SiteStatus::kAmbiguous;
        } else {
            site.status = spec.expected == 1 ? SiteStatus::kUnique : SiteStatus::kGroup;
        }
        result.sites.push_back(std::move(site));
    }
    for (const RelationSpec& spec : Relations()) {
        result.relations.push_back(CheckRelation(image, result, spec));
    }
    result.lzcnt = ScanLzcnt(image);
    result.resolution = ScanResolution(image);
    return result;
}

} // namespace dcp::scan
