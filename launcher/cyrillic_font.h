// The Russian letters as the fan translation's font draws them (localization/ru/font/cyrillic.glyphs.z), copied block for
// block into a build's main font atlas at the cells src/cyrillic_glyphs.h gives them.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cyrillic_glyphs.h"

namespace dcp {

struct CyrillicGlyph {
    uint16_t code = 0, kana = 0;
    CyrillicMetrics metrics{};
    std::vector<uint8_t> blocks; // kFontCellBlocks x kFontCellBlocks BC7 blocks, row by row
};

// The glyph file's letters in cyrillic_letter order; nothing when it is not one.
std::optional<std::vector<CyrillicGlyph>> ParseCyrillicGlyphs(std::span<const uint8_t> file);

// Draws every letter into `font`, a main font atlas; the free cells must be blank. On failure leaves `font` unchanged and
// says why.
bool ComposeCyrillicFont(std::vector<uint8_t>& font, const std::vector<CyrillicGlyph>& glyphs, CyrillicCells cells,
                         std::string* error);

// UTF-8 text with each Russian letter written as the kana the USA font draws it at.
std::string CyrillicKana(std::string_view text);

} // namespace dcp
