// The Hangul syllables of the port's Korean text, drawn with Noto Sans CJK KR (localization/ko/font/hangul.glyphs.z,
// written by scripts/localization/korean.py), copied block for block into the USA main font atlas at cells that only a
// kanji uses; src/hangul_glyphs.h lists them in the executable's glyph table.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "hangul_glyphs.h"

namespace dcp {

struct HangulCell {
    HangulGlyph glyph{};
    std::vector<uint8_t> blocks; // kFontCellBlocks x kFontCellBlocks BC7 blocks, row by row
};

// The glyph file's syllables; nothing when it is not one.
std::optional<std::vector<HangulCell>> ParseHangulGlyphs(std::span<const uint8_t> file);

// Draws every syllable into `font`, the main font atlas, over the kanji its cell held. On failure leaves `font` unchanged
// and says why.
bool ComposeHangulFont(std::vector<uint8_t>& font, const std::vector<HangulCell>& cells, std::string* error);

} // namespace dcp
