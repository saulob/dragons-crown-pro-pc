// Dragon's Crown Pro FTX textures: the Vanillaware FTEX pack (FTX0 -> GNF -> BC7) read from the user's own game archive.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dcp {

struct FtxTexture {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba; // width * height * 4, top-left origin
};

// One BC7 surface of a pack, decoded block by block as texels are sampled; a sprite touches a small part of a
// 4096x4096 atlas, so the full 64 MiB image is never built.
class FtxSurface {
public:
    int width() const { return width_; }
    int height() const { return height_; }
    // Texel (x, y) clamped to the surface, RGBA.
    std::array<uint8_t, 4> Texel(int x, int y) const;

private:
    friend std::optional<FtxSurface> ReadFtxSurface(std::vector<uint8_t> pack, size_t index, std::string* error);
    const uint8_t* Block(int bx, int by) const;

    int width_ = 0;
    int height_ = 0;
    uint32_t tiling_ = 0;
    size_t data_offset_ = 0;
    std::vector<uint8_t> pack_;
    mutable std::vector<int32_t> slot_;       // per block: index into decoded_, or -1
    mutable std::vector<uint8_t> decoded_;    // 64 bytes per decoded block
};

size_t FtxSurfaceCount(std::span<const uint8_t> pack);
std::optional<FtxSurface> ReadFtxSurface(std::vector<uint8_t> pack, size_t index, std::string* error = nullptr);

// Decodes a pack's first BC7 surface completely.
std::optional<FtxTexture> DecodeFtx(std::span<const uint8_t> bytes, std::string* error = nullptr);

// Decodes a BC7 (BPTC) 4x4 block to 16 RGBA texels, in texel order (row by row).
void DecodeBc7Block(const uint8_t block[16], uint8_t rgba[16 * 4]);
// Encodes 16 RGBA texels as one BC7 block: mode 6, or mode 5 where it keeps transparent texels transparent.
void EncodeBc7Block(const uint8_t rgba[16 * 4], uint8_t block[16]);

// Where a pack's BC7 surface keeps its blocks, for rewriting some of them in place.
struct FtxSurfaceLayout {
    int width = 0;
    int height = 0;
    uint32_t tiling = 0;
    size_t data_offset = 0;
};
std::optional<FtxSurfaceLayout> FtxLayoutOf(std::span<const uint8_t> pack, size_t index, std::string* error = nullptr);
size_t FtxBlockOffset(const FtxSurfaceLayout& layout, int bx, int by);

} // namespace dcp
