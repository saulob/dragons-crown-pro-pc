#include "cpk_patch.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <exception>
#include <fstream>
#include <map>
#include <set>
#include <system_error>
#include <utility>

#include <zlib.h>

#include "utf8.h"

namespace dcp {
namespace {

constexpr char kPatchMagic[8] = {'D', 'C', 'P', 'X', '1', 0, 0, 0};
constexpr char kUndoMagic[8] = {'D', 'C', 'P', 'U', 'N', 'D', 'O', '1'};
constexpr uint8_t kStorageMask = 0xF0, kStorageConstant = 0x30, kStoragePerRow = 0x50;
constexpr uint8_t kTypeMask = 0x0F, kTypeString = 0x0A, kTypeData = 0x0B;
constexpr int kTypeSizes[] = {1, 1, 2, 2, 4, 4, 8, 8, 4};

uint16_t ReadLe16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t ReadLe32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t ReadLe64(const uint8_t* p) { return uint64_t(ReadLe32(p)) | uint64_t(ReadLe32(p + 4)) << 32; }
uint16_t ReadBe16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t ReadBe32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}
uint64_t ReadBe64(const uint8_t* p) { return uint64_t(ReadBe32(p)) << 32 | uint64_t(ReadBe32(p + 4)); }
void WriteLe32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
}
void WriteLe64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(uint8_t(v >> (8 * i)));
}

// The XOR CRI applies to some @UTF packets; it is its own inverse.
void CryptUtf(std::vector<uint8_t>& packet) {
    uint32_t m = 0x5F;
    for (uint8_t& b : packet) {
        b ^= uint8_t(m);
        m = (m * 0x15) & 0xFF;
    }
}

// A numeric @UTF field as an unsigned value (types 0-7).
uint64_t ReadNumber(const uint8_t* p, int type) {
    switch (type) {
    case 0: case 1: return p[0];
    case 2: case 3: return ReadBe16(p);
    case 4: case 5: return ReadBe32(p);
    case 6: case 7: return ReadBe64(p);
    }
    return 0;
}

void WriteNumber(uint8_t* p, int type, uint64_t v) {
    const int size = kTypeSizes[type];
    for (int i = 0; i < size; ++i) {
        p[i] = uint8_t(v >> (8 * (size - 1 - i)));
    }
}

bool ReadFileRange(std::istream& in, uint64_t offset, size_t size, std::vector<uint8_t>& out) {
    out.clear();
    in.clear();
    in.seekg(0, std::ios::end);
    const std::streamoff length = in.tellg();
    if (length < 0 || offset > uint64_t(length) || size > uint64_t(length) - offset) return false;
    out.resize(size);
    in.seekg(std::streamoff(offset));
    in.read(reinterpret_cast<char*>(out.data()), std::streamsize(size));
    return in.gcount() == std::streamsize(size);
}

bool IEquals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
        return std::tolower(x) == std::tolower(y);
    });
}

// Parses one @UTF table. Numeric per-row fields are recorded for rewriting; strings and data are read for
// the few columns the reader needs.
struct UtfTable {
    std::vector<uint8_t> packet;
    bool encrypted = false;
    std::string name;
    size_t strings_off = 0, data_off = 0;
    std::vector<std::pair<std::string, uint8_t>> columns; // name, flags
    std::vector<std::vector<std::pair<std::string, size_t>>> positions; // per row: column -> field position
    std::vector<std::vector<std::pair<std::string, std::string>>> strings;  // per row: string columns
    std::vector<std::vector<std::pair<std::string, uint64_t>>> numbers;    // per row: numeric columns (constants too)

    std::string StringAt(size_t off) const {
        const size_t start = strings_off + off;
        if (start >= packet.size()) return {};
        const auto end = std::find(packet.begin() + std::ptrdiff_t(start), packet.end(), uint8_t(0));
        return std::string(packet.begin() + std::ptrdiff_t(start), end);
    }

    bool Parse(std::vector<uint8_t> bytes, std::string* error) {
        packet = std::move(bytes);
        if (packet.size() < 32) {
            if (error) *error = "@UTF packet too short";
            return false;
        }
        if (std::memcmp(packet.data(), "@UTF", 4) != 0) {
            CryptUtf(packet);
            encrypted = true;
            if (std::memcmp(packet.data(), "@UTF", 4) != 0) {
                if (error) *error = "not a @UTF table";
                return false;
            }
        }
        const uint8_t* p = packet.data();
        const size_t rows_off = ReadBe32(p + 8) + 8;
        strings_off = ReadBe32(p + 12) + 8;
        data_off = ReadBe32(p + 16) + 8;
        const uint32_t name_off = ReadBe32(p + 20);
        const uint16_t ncols = ReadBe16(p + 24);
        const uint16_t row_len = ReadBe16(p + 26);
        const uint32_t nrows = ReadBe32(p + 28);
        if (strings_off > packet.size() || rows_off > packet.size() || nrows > (1u << 22) ||
            uint64_t(rows_off) + uint64_t(row_len) * nrows > packet.size()) {
            if (error) *error = "@UTF offsets out of range";
            return false;
        }
        name = StringAt(name_off);
        size_t pos = 32;
        std::vector<std::pair<std::string, uint64_t>> constants;
        for (uint16_t c = 0; c < ncols; ++c) {
            if (pos + 5 > packet.size()) {
                if (error) *error = "@UTF column table truncated";
                return false;
            }
            const uint8_t flags = p[pos];
            const std::string cname = StringAt(ReadBe32(p + pos + 1));
            pos += 5;
            columns.emplace_back(cname, flags);
            const int type = flags & kTypeMask;
            if ((flags & kStorageMask) == kStorageConstant) {
                if (type == kTypeString) {
                    pos += 4;
                } else if (type == kTypeData) {
                    pos += 8;
                } else if (type <= 8) {
                    if (pos + size_t(kTypeSizes[type]) > packet.size()) return false;
                    constants.emplace_back(cname, ReadNumber(p + pos, type));
                    pos += size_t(kTypeSizes[type]);
                } else {
                    if (error) *error = "@UTF unknown column type";
                    return false;
                }
            }
        }
        positions.resize(nrows);
        strings.resize(nrows);
        numbers.resize(nrows);
        for (uint32_t r = 0; r < nrows; ++r) {
            pos = rows_off + size_t(r) * row_len;
            numbers[r] = constants;
            for (const auto& [cname, flags] : columns) {
                if ((flags & kStorageMask) != kStoragePerRow) continue;
                const int type = flags & kTypeMask;
                if (type == kTypeString) {
                    if (pos + 4 > packet.size()) return false;
                    strings[r].emplace_back(cname, StringAt(ReadBe32(p + pos)));
                    pos += 4;
                } else if (type == kTypeData) {
                    pos += 8;
                } else if (type <= 8) {
                    if (pos + size_t(kTypeSizes[type]) > packet.size()) return false;
                    positions[r].emplace_back(cname, pos);
                    numbers[r].emplace_back(cname, ReadNumber(p + pos, type));
                    pos += size_t(kTypeSizes[type]);
                } else {
                    return false;
                }
            }
        }
        return true;
    }

    std::optional<uint64_t> Number(size_t row, std::string_view column) const {
        if (row >= numbers.size()) return std::nullopt;
        for (const auto& [name, value] : numbers[row]) {
            if (name == column) return value;
        }
        return std::nullopt;
    }
    std::string String(size_t row, std::string_view column) const {
        if (row >= strings.size()) return {};
        for (const auto& [name, value] : strings[row]) {
            if (name == column) return value;
        }
        return {};
    }
    // The flags of a column, nothing when absent.
    std::optional<uint8_t> Flags(std::string_view column) const {
        for (const auto& [name, flags] : columns) {
            if (name == column) return flags;
        }
        return std::nullopt;
    }
    // Overwrites a per-row numeric field in place. False when the row has no such per-row field.
    bool SetNumber(size_t row, std::string_view column, uint64_t value) {
        if (row >= positions.size()) return false;
        for (const auto& [name, pos] : positions[row]) {
            if (name != column) continue;
            const std::optional<uint8_t> flags = Flags(column);
            if (!flags) return false;
            WriteNumber(packet.data() + pos, *flags & kTypeMask, value);
            for (auto& [n, v] : numbers[row]) {
                if (n == column) v = value;
            }
            return true;
        }
        return false;
    }
    // The packet as the file stores it.
    std::vector<uint8_t> Stored() const {
        std::vector<uint8_t> out = packet;
        if (encrypted) CryptUtf(out);
        return out;
    }
};

// Reads the @UTF packet behind a 16-byte CPK section header (magic, u32, u64 size).
bool ReadSection(std::istream& in, uint64_t offset, const char* magic, UtfTable& table, uint64_t* packet_pos,
                 std::string* error) {
    std::vector<uint8_t> head;
    if (!ReadFileRange(in, offset, 16, head) || std::memcmp(head.data(), magic, 4) != 0) {
        if (error) *error = std::string("no ") + std::string(magic, 4).c_str() + " section";
        return false;
    }
    const uint64_t size = ReadLe64(head.data() + 8);
    if (size > (64u << 20)) {
        if (error) *error = "section too large";
        return false;
    }
    std::vector<uint8_t> packet;
    if (!ReadFileRange(in, offset + 16, size_t(size), packet)) {
        if (error) *error = "section truncated";
        return false;
    }
    if (packet_pos) *packet_pos = offset + 16;
    return table.Parse(std::move(packet), error);
}

} // namespace

uint32_t Crc32(std::span<const uint8_t> bytes) {
    uint32_t crc = uint32_t(crc32(0L, Z_NULL, 0));
    size_t pos = 0;
    while (pos < bytes.size()) {
        const size_t chunk = std::min<size_t>(bytes.size() - pos, 1u << 30);
        crc = uint32_t(crc32(crc, bytes.data() + pos, uInt(chunk)));
        pos += chunk;
    }
    return crc;
}

std::optional<BlockPatch> ParseBlockPatch(std::span<const uint8_t> b, std::string* error) {
    const auto fail = [&](const char* why) {
        if (error) *error = why;
        return std::optional<BlockPatch>();
    };
    if (b.size() < 12 || std::memcmp(b.data(), kPatchMagic, 8) != 0) return fail("not a DCPX patch");
    const uint32_t count = ReadLe32(b.data() + 8);
    size_t pos = 12;
    BlockPatch patch;
    for (uint32_t t = 0; t < count; ++t) {
        if (pos + 2 > b.size()) return fail("DCPX truncated");
        const uint16_t len = ReadLe16(b.data() + pos);
        pos += 2;
        if (pos + len + 16 > b.size()) return fail("DCPX truncated");
        BlockPatchTarget target;
        target.name.assign(reinterpret_cast<const char*>(b.data() + pos), len);
        pos += len;
        target.size = ReadLe32(b.data() + pos);
        target.crc_before = ReadLe32(b.data() + pos + 4);
        target.crc_after = ReadLe32(b.data() + pos + 8);
        const uint32_t runs = ReadLe32(b.data() + pos + 12);
        pos += 16;
        if (pos + uint64_t(runs) * 20 > b.size()) return fail("DCPX truncated");
        target.runs.reserve(runs);
        for (uint32_t r = 0; r < runs; ++r) {
            BlockRun run;
            run.offset = ReadLe32(b.data() + pos);
            if (uint64_t(run.offset) + 16 > target.size) return fail("DCPX run outside the file");
            std::memcpy(run.data.data(), b.data() + pos + 4, 16);
            pos += 20;
            target.runs.push_back(run);
        }
        patch.targets.push_back(std::move(target));
    }
    return patch;
}

std::vector<uint8_t> CrilaylaDecompress(std::span<const uint8_t> data, std::string* error) {
    const auto fail = [&](const char* why) {
        if (error) *error = why;
        return std::vector<uint8_t>();
    };
    if (data.size() < 16 || std::memcmp(data.data(), "CRILAYLA", 8) != 0) return fail("not CRILAYLA");
    const uint32_t out_size = ReadLe32(data.data() + 8);
    const uint32_t header_off = ReadLe32(data.data() + 12);
    if (uint64_t(16) + header_off + 0x100 > data.size()) return fail("CRILAYLA truncated");
    std::vector<uint8_t> out(size_t(out_size) + 0x100);
    std::memcpy(out.data(), data.data() + 16 + header_off, 0x100); // the file's first 0x100 bytes, stored raw
    const uint8_t* src = data.data() + 16;
    int64_t in_pos = int64_t(header_off) - 1;
    uint64_t bit_buf = 0;
    int bits_left = 0;
    bool underflow = false;
    const auto get_bits = [&](int n) -> uint32_t {
        while (bits_left < n && bits_left <= 56 && in_pos >= 0) {
            bit_buf = (bit_buf << 8) | src[in_pos--];
            bits_left += 8;
        }
        if (bits_left < n) {
            underflow = true;
            return 0;
        }
        bits_left -= n;
        return uint32_t((bit_buf >> bits_left) & ((uint64_t(1) << n) - 1));
    };
    int64_t out_pos = int64_t(out.size()) - 1;
    uint64_t written = 0;
    static constexpr int kLevels[] = {2, 3, 5, 8};
    while (written < out_size) {
        if (get_bits(1)) {
            const int64_t offset = int64_t(get_bits(13)) + 3;
            int64_t length = 3;
            int level = 0;
            for (;;) {
                const int nbits = level < 4 ? kLevels[level] : 8;
                const uint32_t more = get_bits(nbits);
                length += more;
                if (more != (1u << nbits) - 1) break;
                if (level < 4) ++level;
            }
            if (underflow || out_pos - length + 1 < 0 || out_pos + offset >= int64_t(out.size())) {
                return fail("CRILAYLA corrupt");
            }
            for (int64_t i = 0; i < length; ++i) {
                out[size_t(out_pos)] = out[size_t(out_pos + offset)];
                --out_pos;
            }
            written += uint64_t(length);
        } else {
            const uint32_t byte = get_bits(8);
            if (underflow || out_pos < 0x100) return fail("CRILAYLA corrupt");
            out[size_t(out_pos--)] = uint8_t(byte);
            ++written;
        }
    }
    if (written != out_size) return fail("CRILAYLA length mismatch");
    return out;
}

// The parsed tables are kept in the archive object as raw packets plus field positions so ApplyBlockPatch can
// rewrite them. PatchWriter is the only writer.
struct PatchWriter {
    static PatchResult Apply(const std::filesystem::path& cpk, const BlockPatch& patch, const PatchHooks& hooks);

    static bool Load(CpkArchive& a, const std::filesystem::path& file, UtfTable& header, UtfTable& toc, std::string* error) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            if (error) *error = "cannot open";
            return false;
        }
        if (!Load(a, in, header, toc, error)) return false;
        a.file_ = file;
        return true;
    }

    static bool Load(CpkArchive& a, std::istream& in, UtfTable& header, UtfTable& toc, std::string* error) {
        uint64_t header_pos = 0, toc_pos = 0;
        if (!ReadSection(in, 0, "CPK ", header, &header_pos, error)) return false;
        a.toc_offset_ = header.Number(0, "TocOffset").value_or(0);
        a.content_offset_ = header.Number(0, "ContentOffset").value_or(0);
        a.align_ = uint32_t(header.Number(0, "Align").value_or(0x800));
        if (a.align_ == 0) a.align_ = 0x800;
        if (a.align_ > (1u << 20)) {
            if (error) *error = "unsupported alignment";
            return false;
        }
        a.enable_toc_crc_ = uint32_t(header.Number(0, "EnableTocCrc").value_or(0));
        if (!a.toc_offset_) {
            if (error) *error = "no table of contents";
            return false;
        }
        if (!ReadSection(in, a.toc_offset_, "TOC ", toc, &toc_pos, error)) return false;
        // FileOffset counts from the lower of ContentOffset and TocOffset (at most 0x800), as CriPakTools does.
        const uint64_t toc_base = std::min<uint64_t>(a.toc_offset_, 0x800);
        a.toc_base_ = a.content_offset_ ? std::min(a.content_offset_, toc_base) : toc_base;
        a.entries_.clear();
        for (size_t r = 0; r < toc.positions.size(); ++r) {
            CpkEntry e;
            const std::string dir = toc.String(r, "DirName");
            const std::string name = toc.String(r, "FileName");
            e.path = dir.empty() ? name : dir + "/" + name;
            e.size = uint32_t(toc.Number(r, "FileSize").value_or(0));
            e.extract_size = uint32_t(toc.Number(r, "ExtractSize").value_or(e.size));
            e.offset = a.toc_base_ + toc.Number(r, "FileOffset").value_or(0);
            e.id = uint32_t(toc.Number(r, "ID").value_or(0));
            e.row = r;
            a.entries_.push_back(std::move(e));
        }
        Keep(a.header_, header, header_pos);
        Keep(a.toc_, toc, toc_pos);
        return true;
    }

    static void Keep(CpkArchive::Table& t, const UtfTable& u, uint64_t pos) {
        t.encrypted = u.encrypted;
        t.file_pos = pos;
        t.packet = u.packet;
        t.fields.assign(u.positions.size(), {});
        for (size_t r = 0; r < u.positions.size(); ++r) {
            for (const auto& [name, p] : u.positions[r]) {
                const std::optional<uint8_t> flags = u.Flags(name);
                t.fields[r].emplace_back(name, CpkArchive::Field{p, flags ? (*flags & kTypeMask) : 4});
            }
        }
    }

    static bool Set(CpkArchive::Table& t, size_t row, std::string_view name, uint64_t value) {
        const CpkArchive::Field* f = t.field(row, name);
        if (!f) return false;
        WriteNumber(t.packet.data() + f->pos, f->type, value);
        return true;
    }

    static std::vector<uint8_t> Stored(const CpkArchive::Table& t) {
        std::vector<uint8_t> out = t.packet;
        if (t.encrypted) CryptUtf(out);
        return out;
    }

    static void Repoint(CpkArchive& a, const CpkEntry& entry, uint64_t pos, size_t size) {
        Set(a.toc_, entry.row, "FileOffset", pos - a.toc_base());
        Set(a.toc_, entry.row, "FileSize", size);
        Set(a.toc_, entry.row, "ExtractSize", size);
    }

    // Clears the CRC flags so a checked TOC is still accepted.
    static void AcceptRewrittenToc(CpkArchive& a) {
        if (a.enable_toc_crc()) {
            Set(a.header_, 0, "EnableTocCrc", 0);
            Set(a.header_, 0, "EnableFileCrc", 0);
        }
    }

    static std::optional<ArchiveOverlay> Overlay(const CpkArchive& archive, uint64_t length,
                                                 std::span<const std::pair<std::string, std::vector<uint8_t>>> entries,
                                                 std::string* error) {
        CpkArchive a = archive;
        ArchiveOverlay overlay;
        uint64_t pos = length;
        for (const auto& [path, data] : entries) {
            const CpkEntry* entry = a.Find(path);
            if (!entry) {
                if (error) *error = "no " + path + " in the archive";
                return std::nullopt;
            }
            pos = (pos + a.align() - 1) / a.align() * a.align();
            Repoint(a, *entry, pos, data.size());
            overlay.patches.emplace_back(pos, data);
            pos += data.size();
        }
        AcceptRewrittenToc(a);
        overlay.patches.emplace_back(a.toc_.file_pos, Stored(a.toc_));
        overlay.patches.emplace_back(a.header_.file_pos, Stored(a.header_));
        overlay.length = pos;
        return overlay;
    }
};

bool CpkArchive::Open(const std::filesystem::path& file, std::string* error) {
    UtfTable header, toc;
    return PatchWriter::Load(*this, file, header, toc, error);
}

bool CpkArchive::OpenStream(std::istream& in, std::string* error) {
    UtfTable header, toc;
    return PatchWriter::Load(*this, in, header, toc, error);
}

const CpkEntry* CpkArchive::Find(std::string_view path) const {
    for (const CpkEntry& e : entries_) {
        if (IEquals(e.path, path)) return &e;
    }
    return nullptr;
}

std::vector<uint8_t> CpkArchive::Read(const CpkEntry& entry, std::string* error) const {
    std::ifstream in(file_, std::ios::binary);
    std::vector<uint8_t> data;
    if (!in || !ReadFileRange(in, entry.offset, entry.size, data)) {
        if (error) *error = "cannot read " + entry.path;
        return {};
    }
    if (entry.compressed() || (data.size() >= 8 && std::memcmp(data.data(), "CRILAYLA", 8) == 0)) {
        return CrilaylaDecompress(data, error);
    }
    return data;
}

std::filesystem::path UndoPath(const std::filesystem::path& cpk) {
    std::filesystem::path undo = cpk;
    undo += ".undo";
    return undo;
}

ArchivePatchState ArchiveState(const std::filesystem::path& cpk) {
    std::error_code ec;
    return std::filesystem::is_regular_file(UndoPath(cpk), ec) ? ArchivePatchState::kPatched : ArchivePatchState::kOriginal;
}

namespace {

struct UndoRecord {
    uint64_t original_len = 0;
    uint64_t toc_pos = 0, header_pos = 0;
    std::vector<uint8_t> toc, header;
    std::vector<std::string> entries;

    std::vector<uint8_t> Serialize() const {
        std::vector<uint8_t> out(kUndoMagic, kUndoMagic + 8);
        WriteLe64(out, original_len);
        WriteLe64(out, toc_pos);
        WriteLe32(out, uint32_t(toc.size()));
        out.insert(out.end(), toc.begin(), toc.end());
        WriteLe64(out, header_pos);
        WriteLe32(out, uint32_t(header.size()));
        out.insert(out.end(), header.begin(), header.end());
        std::string names;
        for (size_t i = 0; i < entries.size(); ++i) {
            names += (i ? "\n" : "") + entries[i];
        }
        WriteLe32(out, uint32_t(names.size()));
        out.insert(out.end(), names.begin(), names.end());
        return out;
    }

    bool Parse(const std::vector<uint8_t>& b) {
        if (b.size() < 28 || std::memcmp(b.data(), kUndoMagic, 8) != 0) return false;
        original_len = ReadLe64(b.data() + 8);
        toc_pos = ReadLe64(b.data() + 16);
        const uint32_t toc_len = ReadLe32(b.data() + 24);
        size_t pos = 28;
        if (pos + toc_len + 12 > b.size()) return false;
        toc.assign(b.begin() + std::ptrdiff_t(pos), b.begin() + std::ptrdiff_t(pos + toc_len));
        pos += toc_len;
        header_pos = ReadLe64(b.data() + pos);
        const uint32_t hdr_len = ReadLe32(b.data() + pos + 8);
        pos += 12;
        if (pos + hdr_len + 4 > b.size()) return false;
        header.assign(b.begin() + std::ptrdiff_t(pos), b.begin() + std::ptrdiff_t(pos + hdr_len));
        pos += hdr_len;
        const uint32_t names_len = ReadLe32(b.data() + pos);
        pos += 4;
        if (pos + names_len > b.size()) return false;
        std::string names(b.begin() + std::ptrdiff_t(pos), b.begin() + std::ptrdiff_t(pos + names_len));
        entries.clear();
        size_t start = 0;
        while (start <= names.size() && !names.empty()) {
            const size_t end = names.find('\n', start);
            entries.push_back(names.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return true;
    }
};

bool WriteWhole(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    out.flush();
    return out.good();
}

bool ReadWhole(const std::filesystem::path& path, std::vector<uint8_t>& bytes) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamoff size = in.tellg();
    if (size < 0) return false;
    in.seekg(0);
    bytes.resize(size_t(size));
    in.read(reinterpret_cast<char*>(bytes.data()), size);
    return in.gcount() == size;
}

// Whether the undo record belongs to this file: the sections it restores are where it says, of the sizes it holds, and
// the file has not shrunk below the length it would be cut back to.
bool UndoMatches(const std::filesystem::path& cpk, const UndoRecord& undo) {
    std::error_code ec;
    if (std::filesystem::file_size(cpk, ec) < undo.original_len || ec) return false;
    std::ifstream in(cpk, std::ios::binary);
    const auto section_is = [&](uint64_t packet_pos, const char* magic, size_t size) {
        std::vector<uint8_t> head;
        return packet_pos >= 16 && ReadFileRange(in, packet_pos - 16, 16, head) && std::memcmp(head.data(), magic, 4) == 0 &&
               ReadLe64(head.data() + 8) == size;
    };
    return in && section_is(undo.toc_pos, "TOC ", undo.toc.size()) && section_is(undo.header_pos, "CPK ", undo.header.size());
}

// Whether the archive holds this patch and nothing else: every entry of the patch it contains is the patched file, and
// the undo record names exactly those entries. False when it holds none of them.
bool PatchHolds(const std::filesystem::path& cpk, const BlockPatch& patch) {
    CpkArchive archive;
    if (!archive.Open(cpk)) return false;
    std::vector<std::string> present;
    for (const BlockPatchTarget& target : patch.targets) {
        const CpkEntry* entry = archive.Find(target.name);
        if (!entry) continue;
        const std::vector<uint8_t> data = archive.Read(*entry);
        if (data.size() != target.size || Crc32(data) != target.crc_after) return false;
        present.push_back(entry->path);
    }
    std::vector<uint8_t> bytes;
    UndoRecord undo;
    if (present.empty() || !ReadWhole(UndoPath(cpk), bytes) || !undo.Parse(bytes)) return false;
    std::vector<std::string> recorded = undo.entries;
    std::sort(present.begin(), present.end());
    std::sort(recorded.begin(), recorded.end());
    return present == recorded;
}

// The install hard-links the game's files from the folder it was made from, and writing through such a link would
// change that folder's archive too: the patch gets a file of its own first.
bool Unshare(const std::filesystem::path& file, std::string* error) {
    std::error_code ec;
    const uintmax_t links = std::filesystem::hard_link_count(file, ec);
    if (ec || links <= 1) return true;
    std::filesystem::path copy = file;
    copy += ".copy";
    std::filesystem::remove(copy, ec);
    std::filesystem::copy_file(file, copy, std::filesystem::copy_options::none, ec);
    if (!ec) {
        std::filesystem::rename(copy, file, ec);
        if (ec) {
            std::filesystem::remove(file, ec);
            std::filesystem::rename(copy, file, ec);
        }
    }
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(copy, ignored);
        *error = "cannot make a file of its own (" + ec.message() + ")";
        return false;
    }
    return true;
}

} // namespace

std::vector<std::string> PatchedEntries(const std::filesystem::path& cpk) {
    std::vector<uint8_t> bytes;
    UndoRecord undo;
    if (!ReadWhole(UndoPath(cpk), bytes) || !undo.Parse(bytes)) return {};
    return undo.entries;
}

PatchResult PatchWriter::Apply(const std::filesystem::path& cpk, const BlockPatch& patch, const PatchHooks& hooks) {
    PatchResult result;
    const auto fail = [&](std::string why) {
        result.ok = false;
        result.detail = std::move(why);
        return result;
    };
    const std::string name = Utf8(cpk.filename());
    if (ArchiveState(cpk) == ArchivePatchState::kPatched) {
        if (PatchHolds(cpk, patch)) {
            result.ok = true;
            result.detail = name + ": already patched";
            return result;
        }
        // An earlier run was interrupted: back to the original first.
        const PatchResult undone = RevertBlockPatch(cpk);
        if (!undone.ok) return fail(undone.detail);
    }
    CpkArchive archive;
    std::string error;
    if (!archive.Open(cpk, &error)) return fail(name + ": " + error);
    std::vector<std::pair<const CpkEntry*, std::vector<uint8_t>>> patched;
    std::string skipped;
    for (const BlockPatchTarget& target : patch.targets) {
        const CpkEntry* entry = archive.Find(target.name);
        if (!entry) continue;
        if (hooks.phase) hooks.phase(("Reading " + target.name + "...").c_str());
        if (hooks.progress && !hooks.progress(0, 0)) {
            result.cancelled = true;
            return fail("cancelled");
        }
        std::vector<uint8_t> data = archive.Read(*entry, &error);
        if (data.empty()) return fail(name + ": " + error);
        const uint32_t crc = Crc32(data);
        if (data.size() == target.size && crc == target.crc_after) {
            continue; // already carries the patch (then the undo record was lost; nothing to redo)
        }
        if (data.size() != target.size || crc != target.crc_before) {
            skipped += (skipped.empty() ? "" : ", ") + target.name + " is not the file the patch was made for";
            continue;
        }
        for (const BlockRun& run : target.runs) {
            std::memcpy(data.data() + run.offset, run.data.data(), 16);
        }
        if (Crc32(data) != target.crc_after) return fail(name + ": " + target.name + ": patched data does not verify");
        patched.emplace_back(entry, std::move(data));
    }
    if (patched.empty()) {
        result.ok = true;
        result.detail = name + ": " + (skipped.empty() ? "nothing to patch" : skipped);
        return result;
    }
    std::error_code ec;
    const uint64_t original_len = std::filesystem::file_size(cpk, ec);
    if (ec) return fail(name + ": cannot read the size");
    if (!Unshare(cpk, &error)) return fail(name + ": " + error);

    // The undo record goes first, and any failure from here on puts the archive back as it was.
    UndoRecord undo;
    undo.original_len = original_len;
    undo.toc_pos = archive.toc_.file_pos;
    undo.toc = PatchWriter::Stored(archive.toc_);
    undo.header_pos = archive.header_.file_pos;
    undo.header = PatchWriter::Stored(archive.header_);
    for (const auto& [entry, data] : patched) undo.entries.push_back(entry->path);
    if (!WriteWhole(UndoPath(cpk), undo.Serialize())) {
        std::filesystem::remove(UndoPath(cpk), ec);
        return fail(name + ": cannot write the undo record");
    }

    std::fstream out(cpk, std::ios::binary | std::ios::in | std::ios::out);
    const auto fail_undone = [&](std::string why) {
        out.close();
        const PatchResult undone = RevertBlockPatch(cpk);
        if (!undone.ok) why += " (" + undone.detail + ")";
        return fail(std::move(why));
    };
    if (!out) return fail_undone(name + ": cannot open for writing");
    try {
        uint64_t total = 0;
        for (const auto& [entry, data] : patched) total += data.size();
        uint64_t done = 0;
        uint64_t pos = original_len;
        const uint64_t align = archive.align();
        for (const auto& [entry, data] : patched) {
            if (hooks.phase) hooks.phase(("Writing " + entry->path + "...").c_str());
            const uint64_t aligned = (pos + align - 1) / align * align;
            out.seekp(std::streamoff(pos));
            const std::vector<char> zeros(size_t(aligned - pos), 0);
            out.write(zeros.data(), std::streamsize(zeros.size()));
            pos = aligned;
            out.seekp(std::streamoff(pos));
            constexpr size_t kChunk = 4u << 20;
            for (size_t off = 0; off < data.size(); off += kChunk) {
                const size_t n = std::min(kChunk, data.size() - off);
                out.write(reinterpret_cast<const char*>(data.data() + off), std::streamsize(n));
                done += n;
                if (hooks.progress && !hooks.progress(done, total)) {
                    result.cancelled = true;
                    return fail_undone("cancelled");
                }
            }
            if (!out.good()) return fail_undone(name + ": write failed (disk full?)");
            PatchWriter::Repoint(archive, *entry, pos, data.size());
            result.appended += data.size();
            pos += data.size();
        }
        PatchWriter::AcceptRewrittenToc(archive);
        const std::vector<uint8_t> toc = PatchWriter::Stored(archive.toc_);
        const std::vector<uint8_t> header = PatchWriter::Stored(archive.header_);
        out.seekp(std::streamoff(archive.toc_.file_pos));
        out.write(reinterpret_cast<const char*>(toc.data()), std::streamsize(toc.size()));
        out.seekp(std::streamoff(archive.header_.file_pos));
        out.write(reinterpret_cast<const char*>(header.data()), std::streamsize(header.size()));
        out.flush();
        if (!out.good()) return fail_undone(name + ": write failed");
        out.close();

        // Read back through the normal path.
        CpkArchive check;
        if (!check.Open(cpk, &error)) return fail_undone(name + ": verify: " + error);
        for (const auto& [entry, data] : patched) {
            const CpkEntry* e = check.Find(entry->path);
            std::vector<uint8_t> back = e ? check.Read(*e, &error) : std::vector<uint8_t>();
            if (back != data) return fail_undone(name + ": verify failed for " + entry->path);
        }
    } catch (const std::exception& e) {
        return fail_undone(name + ": " + e.what());
    }
    result.ok = true;
    result.patched = patched.size();
    result.detail = name + ": " + std::to_string(patched.size()) + " file(s) patched";
    if (!skipped.empty()) {
        result.detail += "; " + skipped;
    }
    return result;
}

PatchResult ApplyBlockPatch(const std::filesystem::path& cpk, const BlockPatch& patch, const PatchHooks& hooks) {
    return PatchWriter::Apply(cpk, patch, hooks);
}

std::optional<ArchiveOverlay> OverlayEntries(const CpkArchive& archive, uint64_t length,
                                             std::span<const std::pair<std::string, std::vector<uint8_t>>> entries,
                                             std::string* error) {
    return PatchWriter::Overlay(archive, length, entries, error);
}

PatchResult RevertBlockPatch(const std::filesystem::path& cpk) {
    PatchResult result;
    const std::string name = Utf8(cpk.filename());
    const std::filesystem::path undo_path = UndoPath(cpk);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(undo_path, ec)) {
        result.ok = true;
        result.detail = name + ": not patched";
        return result;
    }
    std::vector<uint8_t> bytes;
    UndoRecord undo;
    if (!ReadWhole(undo_path, bytes) || !undo.Parse(bytes)) {
        result.detail = name + ": the undo record is unreadable";
        return result;
    }
    if (!UndoMatches(cpk, undo)) {
        result.detail = name + ": the undo record is not for this archive";
        return result;
    }
    if (std::string error; !Unshare(cpk, &error)) {
        result.detail = name + ": " + error;
        return result;
    }
    {
        std::fstream out(cpk, std::ios::binary | std::ios::in | std::ios::out);
        if (!out) {
            result.detail = name + ": cannot open for writing";
            return result;
        }
        out.seekp(std::streamoff(undo.toc_pos));
        out.write(reinterpret_cast<const char*>(undo.toc.data()), std::streamsize(undo.toc.size()));
        out.seekp(std::streamoff(undo.header_pos));
        out.write(reinterpret_cast<const char*>(undo.header.data()), std::streamsize(undo.header.size()));
        out.flush();
        if (!out.good()) {
            result.detail = name + ": write failed";
            return result;
        }
    }
    std::filesystem::resize_file(cpk, undo.original_len, ec);
    if (ec) {
        result.detail = name + ": cannot truncate: " + ec.message();
        return result;
    }
    std::filesystem::remove(undo_path, ec);
    result.ok = true;
    result.patched = undo.entries.size();
    result.detail = name + ": reverted";
    return result;
}

namespace {

constexpr uint8_t kTocMagic[8] = {'T', 'O', 'C', ' ', 0xFF, 0, 0, 0};

struct NewRow {
    std::string path;
    uint32_t size = 0, extract_size = 0;
    uint64_t offset = 0; // from the archive's FileOffset base
};

using RowKey = std::pair<std::string, std::string>;

std::string Lower(std::string text) {
    for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// The order the game's tables keep: directory, then name, each without regard to case.
RowKey KeyOf(std::string_view path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string_view::npos) return {{}, Lower(std::string(path))};
    return {Lower(std::string(path.substr(0, slash))), Lower(std::string(path.substr(slash + 1)))};
}

// Puts `added` into the table's rows, in sorted place when the table is sorted, with their names added to the string
// pool. Rows are numbered by position when the table's IDs are.
bool InsertRows(UtfTable& toc, const std::vector<NewRow>& added, std::string* error) {
    const auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    const std::vector<uint8_t>& packet = toc.packet;
    const size_t rows_off = ReadBe32(packet.data() + 8) + 8, row_len = ReadBe16(packet.data() + 26);
    const size_t row_count = ReadBe32(packet.data() + 28), rows_end = rows_off + row_len * row_count;
    if (rows_end > toc.strings_off || toc.strings_off > toc.data_off || toc.data_off > packet.size()) {
        return fail("unsupported table layout");
    }
    struct Field {
        size_t offset;
        int type;
    };
    std::map<std::string, Field> layout;
    size_t at = 0;
    for (const auto& [name, flags] : toc.columns) {
        if ((flags & kStorageMask) != kStoragePerRow) continue;
        const int type = flags & kTypeMask;
        const size_t size = type == kTypeString ? 4 : type == kTypeData ? 8 : type <= 8 ? size_t(kTypeSizes[type]) : 0;
        if (!size) return fail("unsupported column type");
        layout[name] = {at, type};
        at += size;
    }
    for (const char* needed : {"DirName", "FileName", "FileSize", "ExtractSize", "FileOffset"}) {
        if (!layout.count(needed)) return fail("the table has no file columns");
    }
    if (at != row_len) return fail("unsupported table layout");

    std::string pool(packet.begin() + std::ptrdiff_t(toc.strings_off), packet.begin() + std::ptrdiff_t(toc.data_off));
    const size_t pool_size = pool.size();
    const auto intern = [&](const std::string& text) {
        for (size_t start = 0, end; (end = pool.find('\0', start)) != std::string::npos; start = end + 1) {
            if (end - start == text.size() && pool.compare(start, end - start, text) == 0) return uint32_t(start);
        }
        const uint32_t offset = uint32_t(pool.size());
        pool += text;
        pool += '\0';
        return offset;
    };

    struct Row {
        RowKey key;
        std::vector<uint8_t> bytes;
    };
    std::vector<Row> rows;
    bool sequential = layout.count("ID") != 0;
    uint64_t next_id = 0;
    for (size_t r = 0; r < row_count; ++r) {
        const std::string dir = toc.String(r, "DirName"), name = toc.String(r, "FileName");
        const auto first = packet.begin() + std::ptrdiff_t(rows_off + r * row_len);
        rows.push_back({KeyOf(dir.empty() ? name : dir + "/" + name), std::vector<uint8_t>(first, first + std::ptrdiff_t(row_len))});
        const uint64_t id = toc.Number(r, "ID").value_or(r);
        sequential = sequential && id == r;
        next_id = std::max(next_id, id + 1);
    }
    const bool sorted = std::is_sorted(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.key < b.key; });

    for (const NewRow& row : added) {
        Row made{KeyOf(row.path), rows.empty() ? std::vector<uint8_t>(row_len) : rows.front().bytes};
        const size_t slash = row.path.rfind('/');
        const std::string dir = slash == std::string::npos ? std::string() : row.path.substr(0, slash);
        const auto put = [&](const char* column, uint64_t value) {
            const Field& field = layout.at(column);
            WriteNumber(made.bytes.data() + field.offset, field.type == kTypeString ? 4 : field.type, value);
        };
        put("DirName", intern(dir));
        put("FileName", intern(slash == std::string::npos ? row.path : row.path.substr(slash + 1)));
        put("FileSize", row.size);
        put("ExtractSize", row.extract_size);
        put("FileOffset", row.offset);
        if (layout.count("ID")) put("ID", next_id++);
        const auto place = sorted ? std::upper_bound(rows.begin(), rows.end(), made,
                                                     [](const Row& a, const Row& b) { return a.key < b.key; })
                                  : rows.end();
        rows.insert(place, std::move(made));
    }
    if (sequential) {
        for (size_t r = 0; r < rows.size(); ++r) {
            const Field& id = layout.at("ID");
            WriteNumber(rows[r].bytes.data() + id.offset, id.type, r);
        }
    }

    std::vector<uint8_t> out(packet.begin(), packet.begin() + std::ptrdiff_t(rows_off));
    for (const Row& row : rows) out.insert(out.end(), row.bytes.begin(), row.bytes.end());
    out.insert(out.end(), packet.begin() + std::ptrdiff_t(rows_end), packet.begin() + std::ptrdiff_t(toc.strings_off));
    const size_t strings_at = out.size();
    out.insert(out.end(), pool.begin(), pool.end());
    while (pool_size % 8 == 0 && (out.size() - strings_at) % 8 != 0) out.push_back(0);
    const size_t data_at = out.size();
    out.insert(out.end(), packet.begin() + std::ptrdiff_t(toc.data_off), packet.end());
    const uint32_t size_field = ReadBe32(packet.data() + 4) + uint32_t(out.size() - packet.size());
    WriteNumber(out.data() + 4, 4, size_field);
    WriteNumber(out.data() + 12, 4, strings_at - 8);
    WriteNumber(out.data() + 16, 4, data_at - 8);
    WriteNumber(out.data() + 28, 4, rows.size());
    toc.packet = std::move(out);
    return true;
}

bool SameBytes(const std::filesystem::path& a, uint64_t offset_a, const std::filesystem::path& b, uint64_t offset_b,
               uint64_t size) {
    std::ifstream first(a, std::ios::binary), second(b, std::ios::binary);
    std::vector<uint8_t> left, right;
    for (uint64_t done = 0; done < size; done += left.size()) {
        const size_t chunk = size_t(std::min<uint64_t>(size - done, 4u << 20));
        if (!ReadFileRange(first, offset_a + done, chunk, left) || !ReadFileRange(second, offset_b + done, chunk, right) ||
            left != right) {
            return false;
        }
    }
    return true;
}

} // namespace

OverlayResult OverlayArchive(const std::filesystem::path& base, const std::filesystem::path& update,
                             const std::function<bool(std::string_view path)>& keep_base,
                             const std::filesystem::path& output) {
    namespace fs = std::filesystem;
    OverlayResult result;
    const auto fail = [&](std::string why, bool unreadable = false) {
        std::error_code ignored;
        fs::remove(output, ignored);
        result.ok = false;
        result.update_unreadable = unreadable;
        result.detail = std::move(why);
        return result;
    };
    const std::string base_name = Utf8(base.filename()), update_name = "the update's " + Utf8(update.filename());
    CpkArchive base_archive, update_archive;
    UtfTable header, toc;
    std::string error;
    if (!PatchWriter::Load(base_archive, base, header, toc, &error)) return fail(base_name + ": " + error);
    if (!update_archive.Open(update, &error)) return fail(update_name + ": " + error, true);

    struct Planned {
        const CpkEntry* update;
        const CpkEntry* base; // nullptr for a new path
        uint64_t offset = 0;
    };
    std::vector<Planned> plan;
    std::set<std::string> seen;
    std::error_code ec;
    const uint64_t update_length = fs::file_size(update, ec);
    if (ec) return fail(update_name + ": cannot read the size", true);
    for (const CpkEntry& entry : update_archive.entries()) {
        if (keep_base(entry.path)) {
            ++result.kept;
            continue;
        }
        if (entry.offset > update_length || entry.size > update_length - entry.offset || !seen.insert(Lower(entry.path)).second) {
            return fail(update_name + ": the entry " + entry.path + " is damaged", true);
        }
        plan.push_back({&entry, base_archive.Find(entry.path)});
    }
    if (plan.empty()) {
        result.ok = true;
        result.detail = base_name + ": the update changes nothing outside the kept entries";
        return result;
    }

    fs::create_directories(output.parent_path(), ec);
    fs::copy_file(base, output, fs::copy_options::overwrite_existing, ec);
    if (ec) return fail(base_name + ": cannot copy it (" + ec.message() + ")");
    const uint64_t base_length = fs::file_size(output, ec);
    if (ec) return fail(base_name + ": cannot read the copy");
    std::fstream out(output, std::ios::binary | std::ios::in | std::ios::out);
    std::ifstream source(update, std::ios::binary);
    if (!out || !source) return fail(base_name + ": cannot open the copy for writing");

    const uint64_t align = base_archive.align(), toc_base = base_archive.toc_base();
    const auto aligned = [&](uint64_t position) { return (position + align - 1) / align * align; };
    const auto pad_to = [&](uint64_t from, uint64_t to) {
        const std::vector<char> zeros(size_t(to - from), 0);
        out.seekp(std::streamoff(from));
        out.write(zeros.data(), std::streamsize(zeros.size()));
    };
    uint64_t pos = base_length;
    std::vector<char> buffer(4u << 20);
    for (Planned& item : plan) {
        const uint64_t at = aligned(pos);
        pad_to(pos, at);
        out.seekp(std::streamoff(at));
        source.clear();
        source.seekg(std::streamoff(item.update->offset));
        for (uint64_t left = item.update->size; left;) {
            const size_t chunk = size_t(std::min<uint64_t>(left, buffer.size()));
            source.read(buffer.data(), std::streamsize(chunk));
            if (source.gcount() != std::streamsize(chunk)) return fail(update_name + ": " + item.update->path + " is truncated", true);
            out.write(buffer.data(), std::streamsize(chunk));
            left -= chunk;
        }
        if (!out.good()) return fail(base_name + ": write failed (disk full?)");
        item.offset = at;
        pos = at + item.update->size;
        result.appended += item.update->size;
    }

    std::vector<NewRow> added;
    for (const Planned& item : plan) {
        const CpkEntry& entry = *item.update;
        if (item.base) {
            if (!toc.SetNumber(item.base->row, "FileSize", entry.size) ||
                !toc.SetNumber(item.base->row, "ExtractSize", entry.extract_size) ||
                !toc.SetNumber(item.base->row, "FileOffset", item.offset - toc_base)) {
                return fail(base_name + ": the table of contents cannot be rewritten");
            }
            ++result.replaced;
        } else {
            added.push_back({entry.path, entry.size, entry.extract_size, item.offset - toc_base});
            ++result.added;
        }
    }
    if (!added.empty() && !InsertRows(toc, added, &error)) return fail(base_name + ": " + error);

    const std::vector<uint8_t> packet = toc.Stored();
    const uint64_t section_length = 16 + packet.size(), toc_at = base_archive.toc_offset();
    const uint64_t content_at = header.Number(0, "ContentOffset").value_or(0);
    uint64_t write_at = toc_at;
    if (!added.empty() && !(content_at > toc_at && toc_at + section_length <= content_at)) {
        write_at = aligned(pos);
        pad_to(pos, write_at);
        if (!header.SetNumber(0, "TocOffset", write_at)) return fail(base_name + ": the table of contents cannot be moved");
    }
    header.SetNumber(0, "Files", base_archive.entries().size() + added.size());
    header.SetNumber(0, "TocSize", section_length);
    if (header.Number(0, "EnableTocCrc").value_or(0) || header.Number(0, "EnableFileCrc").value_or(0)) {
        header.SetNumber(0, "EnableTocCrc", 0);
        header.SetNumber(0, "EnableFileCrc", 0);
    }
    std::vector<uint8_t> toc_section(kTocMagic, kTocMagic + 8);
    WriteLe64(toc_section, packet.size());
    toc_section.insert(toc_section.end(), packet.begin(), packet.end());
    const std::vector<uint8_t> header_packet = header.Stored();
    out.seekp(std::streamoff(write_at));
    out.write(reinterpret_cast<const char*>(toc_section.data()), std::streamsize(toc_section.size()));
    out.seekp(16);
    out.write(reinterpret_cast<const char*>(header_packet.data()), std::streamsize(header_packet.size()));
    out.flush();
    if (!out.good()) return fail(base_name + ": write failed");
    out.close();

    CpkArchive merged;
    if (!merged.Open(output, &error)) return fail(base_name + ": the result cannot be read back: " + error);
    if (merged.entries().size() != base_archive.entries().size() + added.size()) {
        return fail(base_name + ": the result has the wrong number of entries");
    }
    std::set<std::string> replaced;
    for (const Planned& item : plan) {
        const CpkEntry* entry = merged.Find(item.update->path);
        if (!entry || entry->size != item.update->size || entry->extract_size != item.update->extract_size ||
            !SameBytes(output, entry->offset, update, item.update->offset, item.update->size)) {
            return fail(base_name + ": the result does not hold " + item.update->path);
        }
        if (item.base) replaced.insert(Lower(item.base->path));
    }
    for (const CpkEntry& entry : base_archive.entries()) {
        const CpkEntry* kept = merged.Find(entry.path);
        if (!kept || (!replaced.count(Lower(entry.path)) &&
                      (kept->offset != entry.offset || kept->size != entry.size || kept->extract_size != entry.extract_size))) {
            return fail(base_name + ": the result lost " + entry.path);
        }
    }
    result.ok = true;
    result.wrote = true;
    result.detail = base_name + ": " + std::to_string(result.replaced) + " replaced, " + std::to_string(result.added) +
                    " added, " + std::to_string(result.kept) + " left to the base";
    return result;
}

} // namespace dcp
