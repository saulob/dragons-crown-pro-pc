#include "hangul_font.h"

#include <cstring>

#include <zlib.h>

#include "latin_font.h"

namespace dcp {
namespace {

constexpr char kMagic[8] = {'D', 'C', 'P', 'K', 'O', 'R', '1', '\0'};
constexpr size_t kHeaderBytes = 16, kGlyphHeaderBytes = 8, kBlockBytes = 16;
constexpr size_t kCellBytes = size_t(kFontCellBlocks) * kFontCellBlocks * kBlockBytes;
constexpr uint32_t kMaxFileBytes = kHeaderBytes + HANGUL_GLYPH_LIMIT * (kGlyphHeaderBytes + kCellBytes);

uint16_t ReadU16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t ReadU32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

} // namespace

std::optional<std::vector<HangulCell>> ParseHangulGlyphs(std::span<const uint8_t> file) {
    if (file.size() < 4) return std::nullopt;
    const uint32_t size = ReadU32(file.data());
    if (size < kHeaderBytes || size > kMaxFileBytes) return std::nullopt;
    std::vector<uint8_t> raw(size);
    uLongf length = size;
    if (uncompress(raw.data(), &length, file.data() + 4, uLong(file.size() - 4)) != Z_OK || length != size) return std::nullopt;
    const size_t count = ReadU16(&raw[8]);
    if (std::memcmp(raw.data(), kMagic, sizeof(kMagic)) || !count || count > HANGUL_GLYPH_LIMIT ||
        ReadU16(&raw[10]) != kLatinFontCellPixels || ReadU16(&raw[12]) != kFontCellBlocks ||
        size != kHeaderBytes + count * (kGlyphHeaderBytes + kCellBytes)) {
        return std::nullopt;
    }
    std::vector<HangulCell> cells(count);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* at = raw.data() + kHeaderBytes + i * (kGlyphHeaderBytes + kCellBytes);
        HangulCell& cell = cells[i];
        cell.glyph = {ReadU16(at), ReadU16(at + 2), at[4], at[5]};
        if (cell.glyph.code < 0xAC00 || cell.glyph.code > 0xD7A3 || !cell.glyph.width ||
            cell.glyph.width > kLatinFontCellPixels || (i && cell.glyph.code <= cells[i - 1].glyph.code)) {
            return std::nullopt;
        }
        cell.blocks.assign(at + kGlyphHeaderBytes, at + kGlyphHeaderBytes + kCellBytes);
    }
    return cells;
}

bool ComposeHangulFont(std::vector<uint8_t>& font, const std::vector<HangulCell>& cells, std::string* error) {
    const auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (cells.empty()) return fail("no Hangul syllables to draw");
    for (const HangulCell& cell : cells) {
        if (!FontCellInk(font, cell.glyph.cell)) return fail("not a font atlas the port can read");
    }
    const std::vector<uint8_t> original = font;
    for (const HangulCell& cell : cells) {
        if (!WriteFontCell(font, cell.glyph.cell, cell.blocks)) {
            font = original;
            return fail("a syllable could not be written");
        }
    }
    return true;
}

} // namespace dcp
