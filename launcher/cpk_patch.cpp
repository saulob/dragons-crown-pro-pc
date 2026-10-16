#include "cpk_patch.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <exception>
#include <fstream>
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

bool ReadFileRange(std::ifstream& in, uint64_t offset, size_t size, std::vector<uint8_t>& out) {
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
bool ReadSection(std::ifstream& in, uint64_t offset, const char* magic, UtfTable& table, uint64_t* packet_pos,
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
    uint32_t bit_buf = 0;
    int bits_left = 0;
    bool underflow = false;
    const auto get_bits = [&](int n) -> uint32_t {
        uint32_t value = 0;
        while (n > 0) {
            if (bits_left == 0) {
                if (in_pos < 0) {
                    underflow = true;
                    return 0;
                }
                bit_buf = src[in_pos--];
                bits_left = 8;
            }
            const int take = bits_left < n ? bits_left : n;
            value = (value << take) | ((bit_buf >> (bits_left - take)) & ((1u << take) - 1));
            bits_left -= take;
            n -= take;
        }
        return value;
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
        a.file_ = file;
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
};

bool CpkArchive::Open(const std::filesystem::path& file, std::string* error) {
    UtfTable header, toc;
    return PatchWriter::Load(*this, file, header, toc, error);
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

// Whether every entry of the patch that the archive holds is the patched file. False when it holds none of them.
bool PatchHolds(const std::filesystem::path& cpk, const BlockPatch& patch) {
    CpkArchive archive;
    if (!archive.Open(cpk)) return false;
    bool found = false;
    for (const BlockPatchTarget& target : patch.targets) {
        const CpkEntry* entry = archive.Find(target.name);
        if (!entry) continue;
        const std::vector<uint8_t> data = archive.Read(*entry);
        if (data.size() != target.size || Crc32(data) != target.crc_after) return false;
        found = true;
    }
    return found;
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
            PatchWriter::Set(archive.toc_, entry->row, "FileOffset", pos - archive.toc_base());
            PatchWriter::Set(archive.toc_, entry->row, "FileSize", data.size());
            PatchWriter::Set(archive.toc_, entry->row, "ExtractSize", data.size());
            result.appended += data.size();
            pos += data.size();
        }
        // Repoint the table of contents, and clear the CRC flags so a checked TOC is still accepted.
        if (archive.enable_toc_crc()) {
            PatchWriter::Set(archive.header_, 0, "EnableTocCrc", 0);
            PatchWriter::Set(archive.header_, 0, "EnableFileCrc", 0);
        }
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

} // namespace dcp
