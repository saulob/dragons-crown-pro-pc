// Block patches for files inside the game's CRI CPK archives (DRACRO.CPK, DRACRO_US.CPK), applied in place and
// reversibly: the patched copy of an entry is appended, uncompressed, to the end of the archive and the entry's row
// in the table of contents is pointed at it. Nothing of the original data is overwritten, and an undo record next to
// the archive (<name>.CPK.undo) holds what revert needs: the original table of contents and header bytes and the
// original length. The Controller prompts setting is built on this.
//
// A DCPX patch (scripts/prompts/make_dcpx.py) names the entries it changes with the CRC32 of the entry before and
// after, so an archive that does not match is left alone, and the 16-byte runs that differ.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dcp {

struct BlockRun {
    uint32_t offset = 0;
    std::array<uint8_t, 16> data{};
};

struct BlockPatchTarget {
    std::string name; // path inside the archive, as the table of contents spells it: stage/Interface_US.ftx
    uint32_t size = 0; // of the uncompressed entry
    uint32_t crc_before = 0;
    uint32_t crc_after = 0;
    std::vector<BlockRun> runs;
};

struct BlockPatch {
    std::vector<BlockPatchTarget> targets;
};

// Nothing when the bytes are not a DCPX patch; `error` says why.
std::optional<BlockPatch> ParseBlockPatch(std::span<const uint8_t> bytes, std::string* error = nullptr);

// One entry of an archive's table of contents.
struct CpkEntry {
    std::string path;      // DirName/FileName
    uint32_t size = 0;     // bytes stored
    uint32_t extract_size = 0;
    uint64_t offset = 0;   // in the archive
    uint32_t id = 0;
    size_t row = 0;        // row in the TOC table
    bool compressed() const { return size < extract_size; }
};

// Reads the header and table of contents of a CPK archive; entries are read on demand.
class CpkArchive {
public:
    // False when the file is not a CPK the reader understands; `error` says why.
    bool Open(const std::filesystem::path& file, std::string* error = nullptr);
    const std::vector<CpkEntry>& entries() const { return entries_; }
    const CpkEntry* Find(std::string_view path) const; // case-insensitive, nothing when absent
    // The entry's data, decompressed when it is stored CRILAYLA-compressed; empty with `error` set on failure.
    std::vector<uint8_t> Read(const CpkEntry& entry, std::string* error = nullptr) const;

    const std::filesystem::path& file() const { return file_; }
    uint64_t toc_offset() const { return toc_offset_; }
    uint64_t toc_base() const { return toc_base_; }
    uint32_t align() const { return align_; }
    // The header's EnableTocCrc value (0 when the column is absent).
    uint32_t enable_toc_crc() const { return enable_toc_crc_; }

private:
    friend struct PatchWriter;
    std::filesystem::path file_;
    std::vector<CpkEntry> entries_;
    uint64_t toc_offset_ = 0, toc_base_ = 0, content_offset_ = 0;
    uint32_t align_ = 0x800;
    uint32_t enable_toc_crc_ = 0;
    // The raw packets (decrypted) and where their per-row numeric fields sit, for rewriting.
    struct Field {
        size_t pos;
        int type;
    };
    struct Table {
        bool encrypted = false;
        uint64_t file_pos = 0; // of the packet in the archive (after the 16-byte section header)
        std::vector<uint8_t> packet;
        std::vector<std::vector<std::pair<std::string, Field>>> fields; // per row
        const Field* field(size_t row, std::string_view name) const {
            if (row >= fields.size()) return nullptr;
            for (const auto& [n, f] : fields[row]) {
                if (n == name) return &f;
            }
            return nullptr;
        }
    };
    Table header_, toc_;
};

// Decompresses one CRILAYLA block as stored in a CPK entry; empty with `error` set when it is malformed.
std::vector<uint8_t> CrilaylaDecompress(std::span<const uint8_t> data, std::string* error = nullptr);

enum class ArchivePatchState { kOriginal, kPatched };

std::filesystem::path UndoPath(const std::filesystem::path& cpk);
// Patched when the undo record next to the archive exists.
ArchivePatchState ArchiveState(const std::filesystem::path& cpk);

struct PatchHooks {
    std::function<void(const char* text)> phase;                     // "Reading stage/Interface_US.ftx..."
    std::function<bool(uint64_t done, uint64_t total)> progress;     // false cancels
};

struct PatchResult {
    bool ok = false;
    bool cancelled = false;
    std::string detail;   // for the log
    size_t patched = 0;   // entries rewritten (0 with ok when the archive holds none of the patch's entries)
    uint64_t appended = 0;
};

// Applies every target of the patch the archive holds. An archive that is already patched is left alone (ok, 0
// patched); an entry that is not the file the patch was made for is skipped and named in `detail`.
PatchResult ApplyBlockPatch(const std::filesystem::path& cpk, const BlockPatch& patch, const PatchHooks& hooks = {});
// Puts the archive back as it was, from the undo record, and removes the record.
PatchResult RevertBlockPatch(const std::filesystem::path& cpk);

uint32_t Crc32(std::span<const uint8_t> bytes);

} // namespace dcp
