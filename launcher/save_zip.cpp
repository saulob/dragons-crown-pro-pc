#include "save_zip.h"

#include <zlib.h>

#include <cstring>

namespace dcp {
namespace {

constexpr uint32_t kLocalSignature = 0x04034b50;
constexpr uint32_t kCentralSignature = 0x02014b50;
constexpr uint32_t kEndSignature = 0x06054b50;
constexpr size_t kLocalHeader = 30;
constexpr size_t kCentralHeader = 46;
constexpr size_t kEndRecord = 22;
constexpr size_t kMaxComment = 65535;
constexpr uint16_t kEncryptedFlag = 0x0001;
constexpr uint32_t kZip64Marker = 0xFFFFFFFFu;

uint16_t U16(std::span<const uint8_t> data, size_t at) {
    return uint16_t(data[at] | data[at + 1] << 8);
}

uint32_t U32(std::span<const uint8_t> data, size_t at) {
    return uint32_t(data[at]) | uint32_t(data[at + 1]) << 8 | uint32_t(data[at + 2]) << 16 | uint32_t(data[at + 3]) << 24;
}

bool Fits(std::span<const uint8_t> data, uint64_t at, uint64_t length) {
    return at <= data.size() && length <= data.size() - at;
}

} // namespace

std::optional<std::string> SafeArchivePath(std::string_view name, size_t max_name) {
    if (name.empty() || name.size() > max_name) return std::nullopt;
    std::string path;
    for (const char ch : name) {
        if (ch == '\0' || uint8_t(ch) < 0x20) return std::nullopt;
        path.push_back(ch == '\\' ? '/' : ch);
    }
    if (path.front() == '/' || (path.size() > 1 && path[1] == ':')) return std::nullopt;
    size_t at = 0;
    while (at <= path.size()) {
        const size_t end = path.find('/', at);
        const std::string_view segment = std::string_view(path).substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (segment == "..") return std::nullopt;
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return path;
}

ZipError ZipReader::Open() {
    entries_.clear();
    if (data_.size() > limits_.max_archive) return ZipError::kTooLarge;
    if (data_.size() < kEndRecord) return ZipError::kNotZip;
    const size_t earliest = data_.size() > kEndRecord + kMaxComment ? data_.size() - kEndRecord - kMaxComment : 0;
    size_t end = data_.size() - kEndRecord;
    while (true) {
        if (U32(data_, end) == kEndSignature) break;
        if (end == earliest) return ZipError::kNotZip;
        --end;
    }
    const uint16_t disk = U16(data_, end + 4), directory_disk = U16(data_, end + 6);
    const uint16_t entries_here = U16(data_, end + 8), entries_total = U16(data_, end + 10);
    const uint32_t directory_size = U32(data_, end + 12), directory_offset = U32(data_, end + 16);
    if (disk != 0 || directory_disk != 0 || entries_here != entries_total) return ZipError::kUnsupported;
    if (entries_total == 0xFFFF || directory_size == kZip64Marker || directory_offset == kZip64Marker) return ZipError::kUnsupported;
    if (entries_total > limits_.max_entries) return ZipError::kTooManyEntries;
    if (!Fits(data_, directory_offset, directory_size)) return ZipError::kCorrupt;

    size_t at = directory_offset;
    const size_t stop = size_t(directory_offset) + directory_size;
    for (uint16_t i = 0; i < entries_total; ++i) {
        if (!Fits(data_, at, kCentralHeader) || at + kCentralHeader > stop || U32(data_, at) != kCentralSignature) {
            return ZipError::kCorrupt;
        }
        ZipEntry entry;
        entry.flags = U16(data_, at + 8);
        entry.method = U16(data_, at + 10);
        entry.crc = U32(data_, at + 16);
        entry.compressed = U32(data_, at + 20);
        entry.size = U32(data_, at + 24);
        const uint16_t name_length = U16(data_, at + 28), extra_length = U16(data_, at + 30), comment_length = U16(data_, at + 32);
        entry.offset = U32(data_, at + 42);
        const size_t record = kCentralHeader + size_t(name_length) + extra_length + comment_length;
        if (!Fits(data_, at, record) || at + record > stop) return ZipError::kCorrupt;
        if (entry.compressed == kZip64Marker || entry.size == kZip64Marker || entry.offset == kZip64Marker) {
            return ZipError::kUnsupported;
        }
        const std::string_view raw(reinterpret_cast<const char*>(data_.data()) + at + kCentralHeader, name_length);
        const std::optional<std::string> safe = SafeArchivePath(raw, limits_.max_name);
        if (!safe) return ZipError::kUnsafePath;
        entry.name = *safe;
        entry.directory = entry.name.back() == '/';
        entries_.push_back(std::move(entry));
        at += record;
    }
    return ZipError::kNone;
}

ZipError ZipReader::Extract(size_t index, std::vector<uint8_t>& out) {
    out.clear();
    if (index >= entries_.size()) return ZipError::kCorrupt;
    const ZipEntry& entry = entries_[index];
    if (entry.directory) return ZipError::kCorrupt;
    if (entry.flags & kEncryptedFlag) return ZipError::kEncrypted;
    if (entry.method != 0 && entry.method != 8) return ZipError::kUnsupported;
    if (entry.size > limits_.max_file) return ZipError::kTooLarge;
    if (++extracted_ > limits_.max_extracted || extracted_bytes_ + entry.size > limits_.max_total) return ZipError::kTooLarge;
    extracted_bytes_ += entry.size;

    if (!Fits(data_, entry.offset, kLocalHeader) || U32(data_, entry.offset) != kLocalSignature) return ZipError::kCorrupt;
    const size_t start = size_t(entry.offset) + kLocalHeader + U16(data_, entry.offset + 26) + U16(data_, entry.offset + 28);
    if (!Fits(data_, start, entry.compressed)) return ZipError::kCorrupt;
    const uint8_t* source = data_.data() + start;

    out.resize(entry.size);
    if (entry.method == 0) {
        if (entry.compressed != entry.size) return ZipError::kCorrupt;
        std::memcpy(out.data(), source, entry.size);
    } else {
        z_stream stream{};
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return ZipError::kCorrupt;
        stream.next_in = const_cast<Bytef*>(source);
        stream.avail_in = entry.compressed;
        stream.next_out = out.data();
        stream.avail_out = entry.size;
        const int status = inflate(&stream, Z_FINISH);
        const size_t produced = stream.total_out;
        inflateEnd(&stream);
        if (status != Z_STREAM_END || produced != entry.size) {
            out.clear();
            return status == Z_BUF_ERROR || status == Z_OK ? ZipError::kTooLarge : ZipError::kCorrupt;
        }
    }
    if (uint32_t(crc32(0, out.data(), uInt(out.size()))) != entry.crc) {
        out.clear();
        return ZipError::kCorrupt;
    }
    return ZipError::kNone;
}

} // namespace dcp
