// The accented letters of Brazilian Portuguese drawn into the player's own font on builds whose font has none: a base
// letter of the atlas with a mark made from the atlas's own punctuation. Nothing of the game is stored by the port.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dcp {

inline constexpr int kLatinFontCellPixels = 68;
inline constexpr int kFontCellBlocks = kLatinFontCellPixels / 4; // BC7 blocks along a cell's side

// A composed letter and its base letter as the atlas holds them: alpha, kLatinFontCellPixels square.
struct LatinCell {
    uint16_t code = 0;
    char base = 0;
    std::vector<uint8_t> alpha, base_alpha;
};

// Draws the letters into `font`, a fontUTF16.ftx pack, with the marks of `mark_font` (DRACRO.CPK's: the Chinese font's
// grave is a small hook). On failure leaves `font` unchanged and says why. `cells`, when given, receives every letter as
// encoded into the atlas.
bool ComposeLatinFont(std::vector<uint8_t>& font, const std::vector<uint8_t>& mark_font, std::string* error,
                      std::vector<LatinCell>* cells = nullptr);

// The alpha of `cell`, kLatinFontCellPixels square, for tests.
bool LatinFontCellAlpha(const std::vector<uint8_t>& font, int cell, std::vector<uint8_t>& alpha);

// The highest alpha of a main font atlas cell; nothing when `font` is not such an atlas or a block's format is unknown.
std::optional<int> FontCellInk(std::vector<uint8_t>& font, int cell);
// Replaces the cell with `blocks`, its kFontCellBlocks x kFontCellBlocks BC7 blocks row by row.
bool WriteFontCell(std::vector<uint8_t>& font, int cell, std::span<const uint8_t> blocks);

} // namespace dcp
