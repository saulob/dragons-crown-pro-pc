#include "latin_font.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "latin_glyphs.h"

namespace dcp {
namespace {

constexpr int kAtlasSize = 4096;
constexpr int kBlockSize = 4, kBlockBytes = 16;
constexpr uint32_t kDfmtBc7 = 41, kTilingThin1d = 13;
constexpr int kCellPixels = kLatinFontCellPixels; // the font descriptor's cell: 60 of them leave 16 pixels of the atlas unused
constexpr int kInkThreshold = 40;
constexpr int kMarkHeight = 15, kCapMarkHeight = 11;
constexpr int kMarkGap = 1, kTildeGap = 3;
constexpr int kCapMarkTop = 1, kCapTop = kCapMarkTop + kCapMarkHeight + kMarkGap;
constexpr int kAcuteLean = 2; // an acute's foot sits over the letter's centre, its head further right
constexpr int kCedillaOverlap = 2, kCedillaWidth = 10;
constexpr int kBlankLimit = 8;

constexpr std::array<int, 16> kWeights4 = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
constexpr std::array<int, 8> kWeights3 = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr std::array<int, 4> kWeights2 = {0, 21, 43, 64};
// BC7 two-subset partitions, bit n for texel n, and the anchor texel of subset 1.
constexpr std::array<uint16_t, 64> kPartitions = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
    0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C,
    0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660,
    0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22};
constexpr std::array<uint8_t, 64> kSecondAnchor = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8,  2,  2,  8,
                                                   8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,
                                                   2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15};

class BitReader {
public:
    explicit BitReader(const uint8_t* block) : block_(block) {}
    uint32_t Take(int count) {
        uint32_t value = 0;
        for (int i = 0; i < count; ++i, ++at_) value |= uint32_t(block_[at_ >> 3] >> (at_ & 7) & 1) << i;
        return value;
    }

private:
    const uint8_t* block_;
    int at_ = 0;
};

class BitWriter {
public:
    explicit BitWriter(uint8_t* block) : block_(block) { std::memset(block, 0, kBlockBytes); }
    void Put(uint32_t value, int count) {
        for (int i = 0; i < count; ++i, ++at_) block_[at_ >> 3] |= uint8_t((value >> i & 1) << (at_ & 7));
    }

private:
    uint8_t* block_;
    int at_ = 0;
};

int Expand(uint32_t value, int bits) {
    value <<= 8 - bits;
    return int(value | value >> bits);
}

int Lerp(int a, int b, int weight) {
    return ((64 - weight) * a + weight * b + 32) >> 6;
}

using Texels = std::array<uint8_t, 16>;

// Interpolation weights of a set of per-texel indices, read in texel order; the first index has one bit less.
template <size_t N>
std::array<int, 16> ReadWeights(BitReader& bits, const std::array<int, N>& weights, int index_bits) {
    std::array<int, 16> out{};
    for (int i = 0; i < 16; ++i) out[size_t(i)] = weights[bits.Take(i == 0 ? index_bits - 1 : index_bits)];
    return out;
}

Texels Interpolate(int a, int b, const std::array<int, 16>& weights) {
    Texels out{};
    for (size_t i = 0; i < out.size(); ++i) out[i] = uint8_t(Lerp(a, b, weights[i]));
    return out;
}

// Mode 4 and 5: one subset, colour and alpha indexed apart. A rotation swaps alpha with a colour channel.
Texels DecodeSeparateAlpha(BitReader& bits, int mode) {
    const uint32_t rotation = bits.Take(2);
    const bool swapped = mode == 4 && bits.Take(1);
    const int color_bits = mode == 4 ? 5 : 7, alpha_bits = mode == 4 ? 6 : 8;
    std::array<std::array<int, 2>, 3> color{};
    for (auto& channel : color) {
        for (int& value : channel) value = Expand(bits.Take(color_bits), color_bits);
    }
    const int a0 = Expand(bits.Take(alpha_bits), alpha_bits), a1 = Expand(bits.Take(alpha_bits), alpha_bits);
    const std::array<int, 16> first = ReadWeights(bits, kWeights2, 2);
    const std::array<int, 16> second = mode == 4 ? ReadWeights(bits, kWeights3, 3) : ReadWeights(bits, kWeights2, 2);
    const std::array<int, 16>& color_weights = swapped ? second : first;
    const std::array<int, 16>& alpha_weights = mode == 5 ? second : swapped ? first : second;
    if (rotation == 0) return Interpolate(a0, a1, alpha_weights);
    return Interpolate(color[rotation - 1][0], color[rotation - 1][1], color_weights);
}

Texels DecodeMode6(BitReader& bits) {
    bits.Take(7 * 6);
    const uint32_t e0 = bits.Take(7), e1 = bits.Take(7), p0 = bits.Take(1), p1 = bits.Take(1);
    return Interpolate(int(e0 << 1 | p0), int(e1 << 1 | p1), ReadWeights(bits, kWeights4, 4));
}

Texels DecodeMode7(BitReader& bits) {
    const uint32_t partition = bits.Take(6);
    bits.Take(5 * 12);
    std::array<uint32_t, 4> alpha{}, p{};
    for (uint32_t& value : alpha) value = bits.Take(5);
    for (uint32_t& value : p) value = bits.Take(1);
    std::array<int, 4> ends{};
    for (size_t i = 0; i < ends.size(); ++i) ends[i] = Expand(alpha[i] << 1 | p[i], 6);
    Texels out{};
    for (int i = 0; i < 16; ++i) {
        const int subset = kPartitions[partition] >> i & 1;
        const bool anchor = i == 0 || i == kSecondAnchor[partition];
        const int weight = kWeights2[bits.Take(anchor ? 1 : 2)];
        out[size_t(i)] = uint8_t(Lerp(ends[size_t(subset) * 2], ends[size_t(subset) * 2 + 1], weight));
    }
    return out;
}

// The alpha of a block in one of the modes the fonts use.
std::optional<Texels> DecodeAlpha(const uint8_t* block) {
    if (std::all_of(block, block + kBlockBytes, [](uint8_t b) { return b == 0; })) return Texels{};
    BitReader bits(block);
    int mode = 0;
    while (mode < 8 && !bits.Take(1)) ++mode;
    if (mode == 4 || mode == 5) return DecodeSeparateAlpha(bits, mode);
    if (mode == 6) return DecodeMode6(bits);
    if (mode == 7) return DecodeMode7(bits);
    return std::nullopt;
}

// Mode 6: white with the alpha ramp between the block's extremes. Even endpoints give 254 white, odd 255, as the fonts do.
void EncodeAlpha(const Texels& alpha, uint8_t* block) {
    auto [low, high] = std::minmax_element(alpha.begin(), alpha.end());
    int a0 = *low, a1 = *high;
    std::array<int, 16> index{};
    for (size_t i = 0; i < index.size(); ++i) {
        int best = 0, best_error = 1 << 30;
        for (int k = 0; k < 16; ++k) {
            const int error = std::abs(Lerp(a0, a1, kWeights4[size_t(k)]) - alpha[i]);
            if (error < best_error) {
                best = k;
                best_error = error;
            }
        }
        index[i] = best;
    }
    if (index[0] >= 8) {
        std::swap(a0, a1);
        for (int& value : index) value = 15 - value;
    }
    const uint32_t p0 = uint32_t(a0) & 1, p1 = uint32_t(a1) & 1;
    BitWriter bits(block);
    bits.Put(1u << 6, 7);
    for (int channel = 0; channel < 3; ++channel) {
        bits.Put(127, 7);
        bits.Put(127, 7);
    }
    bits.Put(uint32_t(a0) >> 1, 7);
    bits.Put(uint32_t(a1) >> 1, 7);
    bits.Put(p0, 1);
    bits.Put(p1, 1);
    for (int i = 0; i < 16; ++i) bits.Put(uint32_t(index[size_t(i)]), i == 0 ? 3 : 4);
}

uint32_t ReadU32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// A Thin_1dThin BC7 surface of the 4096 x 4096 fonts.
class Atlas {
public:
    static std::optional<Atlas> Open(uint8_t* pack, size_t size) {
        if (size < 0x20 || ReadU32(pack + 12) != 1) return std::nullopt;
        const uint64_t section = ReadU32(pack + 8);
        if (section + 12 > size || std::memcmp(pack + section, "FTX0", 4)) return std::nullopt;
        const uint64_t gnf = section + ReadU32(pack + section + 8);
        if (gnf + 0x100 > size || std::memcmp(pack + gnf, "GNF ", 4)) return std::nullopt;
        const uint8_t* descriptor = pack + gnf + 16;
        const uint32_t dfmt = ReadU32(descriptor + 4) >> 20 & 0x3f;
        const uint32_t dimensions = ReadU32(descriptor + 8);
        const uint32_t tiling = ReadU32(descriptor + 12) >> 20 & 0x1f;
        const uint32_t pitch = (ReadU32(descriptor + 16) >> 13 & 0x3fff) + 1;
        const uint64_t bytes = uint64_t(kAtlasSize / kBlockSize) * (kAtlasSize / kBlockSize) * kBlockBytes;
        if (dfmt != kDfmtBc7 || tiling != kTilingThin1d || pitch != kAtlasSize || (dimensions & 0x3fff) + 1 != kAtlasSize ||
            (dimensions >> 14 & 0x3fff) + 1 != kAtlasSize || gnf + 0x100 + bytes > size) {
            return std::nullopt;
        }
        return Atlas(pack + gnf + 0x100);
    }

    uint8_t* Block(int bx, int by) const {
        const int pitch = kAtlasSize / kBlockSize;
        const size_t tile = size_t(by / 8) * size_t(pitch / 8) + size_t(bx / 8);
        const int x = bx & 7, y = by & 7;
        const int micro = (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
        return data_ + tile * 64 * kBlockBytes + size_t(micro) * kBlockBytes;
    }

private:
    explicit Atlas(uint8_t* data) : data_(data) {}
    uint8_t* data_;
};

int CellOrigin(int index) {
    return index * kCellPixels;
}

// A square of alpha.
class Bitmap {
public:
    Bitmap(int width, int height) : width_(width), height_(height), pixels_(size_t(width) * size_t(height)) {}
    int width() const { return width_; }
    int height() const { return height_; }
    uint8_t Get(int x, int y) const {
        return x < 0 || y < 0 || x >= width_ || y >= height_ ? uint8_t(0) : pixels_[size_t(y) * size_t(width_) + size_t(x)];
    }
    void Set(int x, int y, uint8_t value) {
        if (x >= 0 && y >= 0 && x < width_ && y < height_) pixels_[size_t(y) * size_t(width_) + size_t(x)] = value;
    }
    const std::vector<uint8_t>& pixels() const { return pixels_; }

private:
    int width_, height_;
    std::vector<uint8_t> pixels_;
};

struct Box {
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1; // inclusive
    int width() const { return x1 - x0 + 1; }
    int height() const { return y1 - y0 + 1; }
    bool empty() const { return x1 < x0; }
};

Box InkOf(const Bitmap& bitmap) {
    Box box{bitmap.width(), bitmap.height(), -1, -1};
    for (int y = 0; y < bitmap.height(); ++y) {
        for (int x = 0; x < bitmap.width(); ++x) {
            if (bitmap.Get(x, y) > kInkThreshold) {
                box.x0 = std::min(box.x0, x);
                box.x1 = std::max(box.x1, x);
                box.y0 = std::min(box.y0, y);
                box.y1 = std::max(box.y1, y);
            }
        }
    }
    return box;
}

std::optional<Bitmap> ReadCell(const Atlas& atlas, int cell) {
    const int left = CellOrigin(cell % LATIN_ATLAS_COLUMNS), top = CellOrigin(cell / LATIN_ATLAS_COLUMNS);
    Bitmap out(kCellPixels, kCellPixels);
    for (int by = top / kBlockSize; by * kBlockSize < top + kCellPixels; ++by) {
        for (int bx = left / kBlockSize; bx * kBlockSize < left + kCellPixels; ++bx) {
            if (bx * kBlockSize >= kAtlasSize || by * kBlockSize >= kAtlasSize) continue;
            const std::optional<Texels> texels = DecodeAlpha(atlas.Block(bx, by));
            if (!texels) return std::nullopt;
            for (int i = 0; i < 16; ++i) {
                out.Set(bx * kBlockSize + (i & 3) - left, by * kBlockSize + (i >> 2) - top, (*texels)[size_t(i)]);
            }
        }
    }
    return out;
}

// `source` box scaled to width x height, each target pixel the average of its footprint.
Bitmap Resample(const Bitmap& source, const Box& box, int width, int height) {
    constexpr int kSamples = 4;
    Bitmap out(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int total = 0;
            for (int sy = 0; sy < kSamples; ++sy) {
                for (int sx = 0; sx < kSamples; ++sx) {
                    const int px = box.x0 + (x * kSamples + sx) * box.width() / (width * kSamples);
                    const int py = box.y0 + (y * kSamples + sy) * box.height() / (height * kSamples);
                    total += source.Get(px, py);
                }
            }
            out.Set(x, y, uint8_t(total / (kSamples * kSamples)));
        }
    }
    return out;
}

Bitmap Mirrored(const Bitmap& source) {
    Bitmap out(source.width(), source.height());
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) out.Set(source.width() - 1 - x, y, source.Get(x, y));
    }
    return out;
}

void Stamp(Bitmap& target, const Bitmap& source, int left, int top) {
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            target.Set(left + x, top + y, std::max(target.Get(left + x, top + y), source.Get(x, y)));
        }
    }
}

// Marks are built from the font's own glyphs, so they carry its weight and style.
class Marks {
public:
    static std::optional<Marks> Read(const Atlas& atlas) {
        const auto donor = [&](char c) { return ReadCell(atlas, Cell(c)); };
        std::optional<Bitmap> grave = donor('`'), circumflex = donor('^'), tilde = donor('~'), dot = donor('.'), comma = donor(',');
        if (!grave || !circumflex || !tilde || !dot || !comma) return std::nullopt;
        Marks marks(std::move(*grave), std::move(*circumflex), std::move(*tilde), std::move(*dot), std::move(*comma));
        return marks;
    }

    // The ASCII glyphs' cell order in every font.
    static int Cell(char c) { return uint8_t(c) - 0x1f; }

    Bitmap Make(LatinMark mark, int height) const {
        switch (mark) {
        case LATIN_MARK_GRAVE:
            return Scaled(grave_, height * 6 / 5, height);
        case LATIN_MARK_ACUTE:
            return Mirrored(Scaled(grave_, height * 6 / 5, height));
        case LATIN_MARK_CIRCUMFLEX:
            return Scaled(circumflex_, height * 8 / 5, height);
        case LATIN_MARK_TILDE:
            return Scaled(tilde_, height * 8 / 5, height * 3 / 5);
        case LATIN_MARK_DIAERESIS: {
            const int size = height / 2;
            Bitmap dots(size * 3, size);
            const Bitmap dot = Scaled(dot_, size, size);
            Stamp(dots, dot, 0, 0);
            Stamp(dots, dot, dots.width() - size, 0);
            return dots;
        }
        case LATIN_MARK_CEDILLA:
            break;
        }
        return Scaled(comma_, kCedillaWidth, height);
    }

private:
    Marks(Bitmap grave, Bitmap circumflex, Bitmap tilde, Bitmap dot, Bitmap comma)
        : grave_(std::move(grave)), circumflex_(std::move(circumflex)), tilde_(std::move(tilde)), dot_(std::move(dot)), comma_(std::move(comma)) {}

    static Bitmap Scaled(const Bitmap& source, int width, int height) { return Resample(source, InkOf(source), width, height); }

    Bitmap grave_, circumflex_, tilde_, dot_, comma_;
};

bool IsCapital(char c) {
    return c >= 'A' && c <= 'Z';
}

// The base squeezed under the mark when a capital leaves it no room, the dot of i removed.
Bitmap PreparedBase(const Bitmap& base, char letter, const Box& ink, int x_height_top) {
    Bitmap out(kCellPixels, kCellPixels);
    if (IsCapital(letter) && ink.y0 < kCapTop) {
        const Bitmap squeezed = Resample(base, ink, ink.width(), ink.y1 - kCapTop + 1);
        Stamp(out, squeezed, ink.x0, kCapTop);
        return out;
    }
    for (int y = letter == 'i' ? x_height_top : 0; y < kCellPixels; ++y) {
        for (int x = 0; x < kCellPixels; ++x) out.Set(x, y, base.Get(x, y));
    }
    return out;
}

int MarkHeight(char letter) {
    return IsCapital(letter) ? kCapMarkHeight : kMarkHeight;
}

// Under the letter, touching it: the mark's own bottom is free, an accent's is not.
void StampCedilla(Bitmap& letter, const Marks& marks, const Box& ink) {
    const int top = ink.y1 + 1 - kCedillaOverlap;
    const Bitmap mark = marks.Make(LATIN_MARK_CEDILLA, kCellPixels - top);
    Stamp(letter, mark, (ink.x0 + ink.x1 + 1 - mark.width()) / 2, top);
}

// Just above the letter's own ink, over its centre.
void StampAccent(Bitmap& letter, const Marks& marks, const Box& ink, LatinMark kind, char base) {
    const Bitmap mark = marks.Make(kind, MarkHeight(base));
    const int gap = kind == LATIN_MARK_TILDE ? kTildeGap : kMarkGap;
    const int lean = kind == LATIN_MARK_ACUTE ? kAcuteLean : kind == LATIN_MARK_GRAVE ? -kAcuteLean : 0;
    const int top = std::max(0, ink.y0 - gap - mark.height());
    Stamp(letter, mark, (ink.x0 + ink.x1 + 1 - mark.width()) / 2 + lean, top);
}

std::optional<Bitmap> Letter(const Atlas& atlas, const Marks& marks, const LatinGlyph& glyph, int x_height_top) {
    const std::optional<Bitmap> base = ReadCell(atlas, Marks::Cell(glyph.base));
    if (!base) return std::nullopt;
    const Box ink = InkOf(*base);
    if (ink.empty()) return std::nullopt;
    Bitmap out = PreparedBase(*base, glyph.base, ink, x_height_top);
    const Box placed = InkOf(out);
    const LatinMark kind = LatinMark(glyph.mark);
    if (kind == LATIN_MARK_CEDILLA) StampCedilla(out, marks, placed);
    else StampAccent(out, marks, placed, kind, glyph.base);
    return out;
}

} // namespace

bool ComposeLatinFont(std::vector<uint8_t>& font, const std::vector<uint8_t>& mark_font, std::string* error,
                      std::vector<LatinCell>* cells) {
    const auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    std::optional<Atlas> atlas = Atlas::Open(font.data(), font.size());
    if (!atlas) return fail("not a font atlas the port can read");
    std::vector<uint8_t> mark_copy = mark_font;
    const std::optional<Atlas> mark_atlas = Atlas::Open(mark_copy.data(), mark_copy.size());
    if (!mark_atlas) return fail("the mark font is not an atlas the port can read");
    const std::optional<Marks> marks = Marks::Read(*mark_atlas);
    if (!marks) return fail("a mark glyph has an unknown block format");
    const std::optional<Bitmap> x_glyph = ReadCell(*atlas, Marks::Cell('x'));
    if (!x_glyph) return fail("a base glyph has an unknown block format");
    const int x_height_top = InkOf(*x_glyph).y0;

    const int top = CellOrigin(LATIN_FIRST_CELL / LATIN_ATLAS_COLUMNS);
    const int cell_blocks = kCellPixels / kBlockSize, first_block_row = top / kBlockSize;
    for (int by = 0; by < cell_blocks; ++by) {
        for (int bx = 0; bx < cell_blocks * LATIN_GLYPH_COUNT; ++bx) {
            const std::optional<Texels> texels = DecodeAlpha(atlas->Block(bx, first_block_row + by));
            if (!texels) return fail("the free atlas rows have an unknown block format");
            if (*std::max_element(texels->begin(), texels->end()) > kBlankLimit) return fail("the atlas rows are not free");
        }
    }
    std::vector<Bitmap> letters;
    for (const LatinGlyph& glyph : latin_glyphs) {
        std::optional<Bitmap> letter = Letter(*atlas, *marks, glyph, x_height_top);
        if (!letter) return fail("a base glyph is missing");
        letters.push_back(std::move(*letter));
    }
    for (int i = 0; i < LATIN_GLYPH_COUNT; ++i) {
        for (int by = 0; by < cell_blocks; ++by) {
            for (int bx = 0; bx < cell_blocks; ++bx) {
                Texels alpha{};
                for (int t = 0; t < 16; ++t) alpha[size_t(t)] = letters[size_t(i)].Get(bx * kBlockSize + (t & 3), by * kBlockSize + (t >> 2));
                EncodeAlpha(alpha, atlas->Block(CellOrigin(i) / kBlockSize + bx, first_block_row + by));
            }
        }
    }
    if (cells) {
        cells->clear();
        for (int i = 0; i < LATIN_GLYPH_COUNT; ++i) {
            const LatinGlyph& glyph = latin_glyphs[i];
            const std::optional<Bitmap> letter = ReadCell(*atlas, LATIN_FIRST_CELL + i);
            const std::optional<Bitmap> base = ReadCell(*atlas, Marks::Cell(glyph.base));
            if (!letter || !base) return fail("a composed glyph cannot be read back");
            cells->push_back({glyph.code, glyph.base, letter->pixels(), base->pixels()});
        }
    }
    return true;
}

bool LatinFontCellAlpha(const std::vector<uint8_t>& font, int cell, std::vector<uint8_t>& alpha) {
    std::vector<uint8_t> copy = font;
    const std::optional<Atlas> atlas = Atlas::Open(copy.data(), copy.size());
    if (!atlas) return false;
    const std::optional<Bitmap> bitmap = ReadCell(*atlas, cell);
    if (!bitmap) return false;
    alpha = bitmap->pixels();
    return true;
}

} // namespace dcp
