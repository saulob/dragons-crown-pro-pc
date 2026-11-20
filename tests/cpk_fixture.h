#pragma once
// A synthetic CRI CPK archive: a header, a table of contents and the files' bytes, for tests that open and overlay one.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cpk_build {

void Be16(std::string& o, uint32_t v) { o += char(v >> 8); o += char(v); }
void Be32(std::string& o, uint32_t v) { for (int i = 3; i >= 0; --i) o += char(v >> (8 * i)); }
void Be64(std::string& o, uint64_t v) { for (int i = 7; i >= 0; --i) o += char(v >> (8 * i)); }
void Le32(std::string& o, uint32_t v) { for (int i = 0; i < 4; ++i) o += char(v >> (8 * i)); }
void Le64(std::string& o, uint64_t v) { for (int i = 0; i < 8; ++i) o += char(v >> (8 * i)); }

struct Column {
    const char* name;
    int type;
    int storage;
};
struct Cell {
    uint64_t number = 0;
    std::string text;
};
using Row = std::vector<std::pair<std::string, Cell>>;

std::string Utf(const std::string& name, const std::vector<Column>& columns, const std::vector<Row>& rows) {
    std::string strings("<NULL>", 7);
    const auto intern = [&](const std::string& text) {
        const uint32_t off = uint32_t(strings.size());
        strings += text;
        strings += char(0);
        return off;
    };
    const auto cell = [](const Row& row, const char* key) {
        for (const auto& [k, v] : row) {
            if (k == key) return v;
        }
        return Cell{};
    };
    const uint32_t name_off = intern(name);
    std::string cols;
    for (const Column& c : columns) {
        cols += char(c.storage | c.type);
        Be32(cols, intern(c.name));
        if (c.storage == 0x30) {
            const uint64_t v = cell(rows.at(0), c.name).number;
            if (c.type == 2) Be16(cols, uint32_t(v)); else if (c.type == 4) Be32(cols, uint32_t(v)); else Be64(cols, v);
        }
    }
    std::string rowbuf;
    uint32_t row_len = 0;
    for (const Row& row : rows) {
        const size_t start = rowbuf.size();
        for (const Column& c : columns) {
            if (c.storage != 0x50) continue;
            const Cell v = cell(row, c.name);
            if (c.type == 0xA) Be32(rowbuf, intern(v.text));
            else if (c.type == 2) Be16(rowbuf, uint32_t(v.number));
            else if (c.type == 4) Be32(rowbuf, uint32_t(v.number));
            else Be64(rowbuf, v.number);
        }
        row_len = uint32_t(rowbuf.size() - start);
    }
    const uint32_t rows_off = 32 + uint32_t(cols.size()) - 8;
    const uint32_t strings_off = rows_off + uint32_t(rowbuf.size());
    std::string packet("@UTF");
    Be32(packet, 0);
    Be32(packet, rows_off);
    Be32(packet, strings_off);
    Be32(packet, strings_off + uint32_t(strings.size()));
    Be32(packet, name_off);
    Be16(packet, uint32_t(columns.size()));
    Be16(packet, row_len);
    Be32(packet, uint32_t(rows.size()));
    packet += cols + rowbuf + strings;
    const uint32_t size = uint32_t(packet.size() - 8);
    for (int i = 0; i < 4; ++i) packet[4 + i] = char(size >> (8 * (3 - i)));
    return packet;
}

std::string Crypt(std::string packet) {
    uint32_t m = 0x5F;
    for (char& b : packet) {
        b = char(uint8_t(b) ^ uint8_t(m));
        m = (m * 0x15) & 0xFF;
    }
    return packet;
}

std::string Section(const char* magic, const std::string& packet) {
    std::string out(magic, 4);
    Le32(out, 0xFF);
    Le64(out, packet.size());
    return out + packet;
}

Cell N(uint64_t n) { return {n, {}}; }
Cell S(std::string s) { return {0, std::move(s)}; }

std::string BuildCpkWith(const std::vector<std::pair<std::string, std::string>>& files) {
    const uint64_t toc_offset = 0x800, content_offset = 0x6000, align = 0x800;
    std::string content;
    std::vector<Row> rows;
    for (size_t i = 0; i < files.size(); ++i) {
        const std::string& path = files[i].first;
        const std::string& data = files[i].second;
        while ((content_offset + content.size()) % align) content += char(0);
        const size_t slash = path.rfind('/');
        rows.push_back({{"DirName", S(path.substr(0, slash))},
                        {"FileName", S(path.substr(slash + 1))},
                        {"FileSize", N(data.size())},
                        {"ExtractSize", N(data.size())},
                        {"FileOffset", N(content_offset + content.size() - toc_offset)},
                        {"ID", N(i)},
                        {"UserString", S("<NULL>")}});
        content += data;
    }
    const std::string toc = Utf("CpkTocInfo",
                                {{"DirName", 0xA, 0x50}, {"FileName", 0xA, 0x50}, {"FileSize", 4, 0x50}, {"ExtractSize", 4, 0x50},
                                 {"FileOffset", 6, 0x50}, {"ID", 4, 0x50}, {"UserString", 0xA, 0x50}},
                                rows);
    const std::string header = Utf("CpkHeader",
                                   {{"TocOffset", 6, 0x50}, {"TocSize", 6, 0x50}, {"ContentOffset", 6, 0x50}, {"Files", 4, 0x50},
                                    {"Align", 2, 0x50}, {"EnableTocCrc", 2, 0x50}, {"EnableFileCrc", 2, 0x50}, {"Version", 2, 0x30}},
                                   {{{"TocOffset", N(toc_offset)}, {"TocSize", N(toc.size() + 16)}, {"ContentOffset", N(content_offset)},
                                     {"Files", N(files.size())}, {"Align", N(align)}, {"EnableTocCrc", N(1)}, {"EnableFileCrc", N(0)},
                                     {"Version", N(7)}}});
    std::string blob = Section("CPK ", Crypt(header));
    blob.resize(toc_offset, char(0));
    blob += Section("TOC ", toc);
    blob.resize(content_offset, char(0));
    return blob + content;
}

std::string BuildCpk(const std::vector<std::string>& paths) {
    std::vector<std::pair<std::string, std::string>> files;
    for (const std::string& path : paths) files.emplace_back(path, "data");
    return BuildCpkWith(files);
}

} // namespace cpk_build
