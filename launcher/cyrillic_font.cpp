#include "cyrillic_font.h"

#include <cstring>

#include <zlib.h>

#include "latin_font.h"

namespace dcp {
namespace {

constexpr char kMagic[8] = {'D', 'C', 'P', 'C', 'Y', 'R', '1', '\0'};
constexpr size_t kHeaderBytes = 16, kGlyphHeaderBytes = 8, kBlockBytes = 16;
constexpr size_t kCellBytes = size_t(kFontCellBlocks) * kFontCellBlocks * kBlockBytes;
constexpr int kBlankLimit = 8;
constexpr uint32_t kMaxFileBytes = 1u << 20;

uint16_t ReadU16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t ReadU32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

} // namespace

std::optional<std::vector<CyrillicGlyph>> ParseCyrillicGlyphs(std::span<const uint8_t> file) {
    if (file.size() < 4) return std::nullopt;
    const uint32_t size = ReadU32(file.data());
    if (size != kHeaderBytes + CYRILLIC_GLYPH_COUNT * (kGlyphHeaderBytes + kCellBytes) || size > kMaxFileBytes) {
        return std::nullopt;
    }
    std::vector<uint8_t> raw(size);
    uLongf length = size;
    if (uncompress(raw.data(), &length, file.data() + 4, uLong(file.size() - 4)) != Z_OK || length != size) return std::nullopt;
    if (std::memcmp(raw.data(), kMagic, sizeof(kMagic)) || ReadU16(&raw[8]) != CYRILLIC_GLYPH_COUNT ||
        ReadU16(&raw[10]) != kLatinFontCellPixels || ReadU16(&raw[12]) != kFontCellBlocks) {
        return std::nullopt;
    }
    std::vector<CyrillicGlyph> glyphs(CYRILLIC_GLYPH_COUNT);
    for (int i = 0; i < CYRILLIC_GLYPH_COUNT; ++i) {
        const uint8_t* at = raw.data() + kHeaderBytes + size_t(i) * (kGlyphHeaderBytes + kCellBytes);
        CyrillicGlyph& glyph = glyphs[size_t(i)];
        glyph.code = ReadU16(at);
        glyph.kana = ReadU16(at + 2);
        glyph.metrics = {at[4], at[5]};
        if (glyph.code != cyrillic_letter(i) || glyph.kana != cyrillic_kana(i) || !glyph.metrics.width ||
            glyph.metrics.width > kLatinFontCellPixels) {
            return std::nullopt;
        }
        glyph.blocks.assign(at + kGlyphHeaderBytes, at + kGlyphHeaderBytes + kCellBytes);
    }
    return glyphs;
}

bool ComposeCyrillicFont(std::vector<uint8_t>& font, const std::vector<CyrillicGlyph>& glyphs, CyrillicCells cells,
                         std::string* error) {
    const auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (glyphs.size() != CYRILLIC_GLYPH_COUNT || cells == CYRILLIC_CELLS_NONE) return fail("no Cyrillic letters to draw");
    for (int i = 0; i < CYRILLIC_GLYPH_COUNT; ++i) {
        const std::optional<int> ink = FontCellInk(font, cyrillic_cell(cells, i));
        if (!ink) return fail("not a font atlas the port can read");
        if (cells == CYRILLIC_CELLS_FREE && *ink > kBlankLimit) return fail("the atlas cells for the letters are not free");
    }
    for (int i = 0; i < CYRILLIC_GLYPH_COUNT; ++i) {
        if (!WriteFontCell(font, cyrillic_cell(cells, i), glyphs[size_t(i)].blocks)) return fail("a letter could not be written");
    }
    return true;
}

std::string CyrillicKana(std::string_view text) {
    const std::string source(text);
    std::string out(source.size() * 3 / 2 + 1, '\0');
    out.resize(cyrillic_kana_encode(source.c_str(), out.data(), out.size()));
    return out;
}

} // namespace dcp
