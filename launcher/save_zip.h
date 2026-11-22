// A small in-memory ZIP reader for save import: it lists an archive, refuses unsafe names and oversized contents, and
// inflates only the entries it is asked for. Nothing is ever written to disk.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dcp {

struct ZipLimits {
    size_t max_archive = 96u << 20;
    size_t max_entries = 2048;
    size_t max_name = 260;
    size_t max_file = 16u << 20;
    size_t max_total = 48u << 20;
    size_t max_extracted = 8;
};

enum class ZipError { kNone, kNotZip, kCorrupt, kTooLarge, kTooManyEntries, kUnsafePath, kEncrypted, kUnsupported };

struct ZipEntry {
    std::string name; // forward slashes, no leading slash
    uint16_t flags = 0;
    uint16_t method = 0;
    uint32_t crc = 0;
    uint32_t compressed = 0;
    uint32_t size = 0;
    uint32_t offset = 0;
    bool directory = false;
};

// "a\\b/../c" and absolute names are refused; the result uses forward slashes.
std::optional<std::string> SafeArchivePath(std::string_view name, size_t max_name);

class ZipReader {
public:
    explicit ZipReader(std::span<const uint8_t> data, ZipLimits limits = {}) : data_(data), limits_(limits) {}

    ZipError Open();
    const std::vector<ZipEntry>& entries() const { return entries_; }

    // Inflates entry `index`; stored and deflated entries only, checked against the declared size and CRC-32.
    ZipError Extract(size_t index, std::vector<uint8_t>& out);

private:
    std::span<const uint8_t> data_;
    ZipLimits limits_;
    std::vector<ZipEntry> entries_;
    size_t extracted_ = 0;
    size_t extracted_bytes_ = 0;
};

} // namespace dcp
