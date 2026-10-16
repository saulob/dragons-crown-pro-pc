// Tiling tests: the macro tile parameters and the macro-tiled address of the base PS4 and the PS4 Pro (alt tile) layout,
// checked against values taken from the game's own tiler. Pure address math: no Vulkan, no window, no game files.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "shader_recompiler/resource.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/amdgpu/tiling.h"
#include "video_core/texture_cache/image_info.h"

namespace {

using AmdGpu::PipeConfig;
using AmdGpu::TileMode;

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                                       \
    do {                                                                                                       \
        ++g_checks;                                                                                            \
        if (!(condition)) {                                                                                    \
            ++g_failures;                                                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                  \
        }                                                                                                      \
    } while (0)

struct LayoutRow {
    unsigned tile_mode, bpp, samples, alt_tile, pipes, bank_width, bank_height, banks, aspect, tile_split;
};
const LayoutRow layout_rows[] = {
#include "tiling_layouts.inc"
};

struct AddressRow {
    unsigned tile_mode, bpp, alt_tile, x, y, offset;
};
const AddressRow address_rows[] = {
#include "tiling_addresses.inc"
};

unsigned Bit(unsigned value, unsigned index) {
    return (value >> index) & 1;
}

unsigned PixelIndexInMicroTile(AmdGpu::MicroTileMode mode, unsigned bpp, unsigned x, unsigned y) {
    if (mode == AmdGpu::MicroTileMode::Display) {
        switch (bpp) {
        case 8:
            return Bit(x, 0) | Bit(x, 1) << 1 | Bit(x, 2) << 2 | Bit(y, 1) << 3 | Bit(y, 0) << 4 | Bit(y, 2) << 5;
        case 16:
            return Bit(x, 0) | Bit(x, 1) << 1 | Bit(x, 2) << 2 | Bit(y, 0) << 3 | Bit(y, 1) << 4 | Bit(y, 2) << 5;
        case 32:
            return Bit(x, 0) | Bit(x, 1) << 1 | Bit(y, 0) << 2 | Bit(x, 2) << 3 | Bit(y, 1) << 4 | Bit(y, 2) << 5;
        default:
            return Bit(x, 0) | Bit(y, 0) << 1 | Bit(x, 1) << 2 | Bit(x, 2) << 3 | Bit(y, 1) << 4 | Bit(y, 2) << 5;
        }
    }
    return Bit(x, 0) | Bit(y, 0) << 1 | Bit(x, 1) << 2 | Bit(y, 1) << 3 | Bit(x, 2) << 4 | Bit(y, 2) << 5;
}

unsigned PipeFromCoord(PipeConfig config, unsigned x, unsigned y) {
    const unsigned tx = x / 8, ty = y / 8;
    const unsigned x3 = Bit(tx, 0), x4 = Bit(tx, 1), x5 = Bit(tx, 2), x6 = Bit(tx, 3);
    const unsigned y3 = Bit(ty, 0), y4 = Bit(ty, 1), y5 = Bit(ty, 2);
    switch (config) {
    case PipeConfig::P8_32x32_8x16:
        return (x4 ^ y3 ^ x5) | (x3 ^ y4) << 1 | (x5 ^ y5) << 2;
    case PipeConfig::P8_32x32_16x16:
        return (x3 ^ y3 ^ x4) | (x4 ^ y4) << 1 | (x5 ^ y5) << 2;
    case PipeConfig::P16:
        return (x3 ^ y3 ^ x4) | (x4 ^ y4) << 1 | (x5 ^ y5) << 2 | (x6 ^ y5) << 3;
    default:
        return 0;
    }
}

unsigned BankFromCoord(const AmdGpu::MacroTileLayout& layout, unsigned x, unsigned y, unsigned pipes,
                       unsigned tile_split_slice, AmdGpu::ArrayMode array_mode) {
    const unsigned tx = x / 8 / (layout.bank_width * pipes), ty = y / 8 / layout.bank_height;
    const unsigned x3 = Bit(tx, 0), x4 = Bit(tx, 1), x5 = Bit(tx, 2), x6 = Bit(tx, 3);
    const unsigned y3 = Bit(ty, 0), y4 = Bit(ty, 1), y5 = Bit(ty, 2), y6 = Bit(ty, 3);
    unsigned bank = 0;
    switch (layout.num_banks) {
    case 16:
        bank = (x3 ^ y6) | (x4 ^ y5 ^ y6) << 1 | (x5 ^ y4) << 2 | (x6 ^ y3) << 3;
        break;
    case 8:
        bank = (x3 ^ y5) | (x4 ^ y4 ^ y5) << 1 | (x5 ^ y3) << 2;
        break;
    case 4:
        bank = (x3 ^ y4) | (x4 ^ y3) << 1;
        break;
    default:
        bank = x3 ^ y3;
        break;
    }
    if (array_mode == AmdGpu::ArrayMode::Array2DTiledThin1) {
        bank ^= (layout.num_banks / 2 + 1) * tile_split_slice;
    }
    return bank & (layout.num_banks - 1);
}

struct Surface {
    TileMode tile_mode;
    unsigned bpp;
    bool alt_tile;
    unsigned pitch;
    unsigned height;
};

uint64_t MacroTiledOffset(const Surface& surface, unsigned x, unsigned y) {
    const AmdGpu::ArrayMode array_mode = AmdGpu::GetArrayMode(surface.tile_mode);
    const AmdGpu::MicroTileMode micro_mode = AmdGpu::GetMicroTileMode(surface.tile_mode);
    const AmdGpu::MacroTileLayout layout =
        AmdGpu::GetMacroTileLayout(surface.tile_mode, surface.bpp, 1, surface.alt_tile);
    const unsigned pipes = AmdGpu::GetPipeCount(layout.pipe_config);
    const unsigned tile_split =
        AmdGpu::CalculateTileSplit(surface.tile_mode, array_mode, micro_mode, surface.bpp);

    unsigned element = PixelIndexInMicroTile(micro_mode, surface.bpp, x, y) * surface.bpp / 8;
    unsigned micro_tile_bytes = 64 * surface.bpp / 8;
    unsigned tile_split_slice = 0;
    if (micro_tile_bytes > tile_split) {
        tile_split_slice = element / tile_split;
        element %= tile_split;
        micro_tile_bytes = tile_split;
    }

    const unsigned macro_pitch = 8 * layout.bank_width * pipes * layout.macro_tile_aspect;
    const unsigned macro_height = 8 * layout.bank_height * layout.num_banks / layout.macro_tile_aspect;
    const uint64_t macro_bytes = uint64_t(micro_tile_bytes) * (macro_pitch / 8) * (macro_height / 8) /
                                 (pipes * layout.num_banks);
    const unsigned macros_per_row = surface.pitch / macro_pitch;
    const uint64_t macro_offset = (uint64_t(y / macro_height) * macros_per_row + x / macro_pitch) * macro_bytes;
    const uint64_t slice_bytes = uint64_t(macros_per_row) * (surface.height / macro_height) * macro_bytes;
    const uint64_t tile_offset =
        (uint64_t((y / 8) % layout.bank_height) * layout.bank_width + (x / 8 / pipes) % layout.bank_width) *
        micro_tile_bytes;
    const uint64_t total = slice_bytes * tile_split_slice + macro_offset + element + tile_offset;

    const unsigned pipe = PipeFromCoord(layout.pipe_config, x, y);
    const unsigned bank = BankFromCoord(layout, x, y, pipes, tile_split_slice, array_mode);
    unsigned pipe_bits = 0;
    while ((1u << pipe_bits) < pipes) {
        ++pipe_bits;
    }
    unsigned bank_bits = 0;
    while ((1u << bank_bits) < layout.num_banks) {
        ++bank_bits;
    }
    return (total & 0xff) | uint64_t(pipe) << 8 | uint64_t(bank) << (8 + pipe_bits) |
           (total >> 8) << (8 + pipe_bits + bank_bits);
}

void TestMacroTileLayouts() {
    for (const LayoutRow& row : layout_rows) {
        const auto tile_mode = TileMode(row.tile_mode);
        const AmdGpu::MacroTileLayout layout =
            AmdGpu::GetMacroTileLayout(tile_mode, row.bpp, row.samples, row.alt_tile != 0);
        CHECK(AmdGpu::GetPipeCount(layout.pipe_config) == row.pipes);
        CHECK(layout.bank_width == row.bank_width);
        CHECK(layout.bank_height == row.bank_height);
        CHECK(layout.num_banks == row.banks);
        CHECK(layout.macro_tile_aspect == row.aspect);
        if (AmdGpu::GetMicroTileThickness(AmdGpu::GetArrayMode(tile_mode)) == 1) {
            CHECK(AmdGpu::CalculateTileSplit(tile_mode, AmdGpu::GetArrayMode(tile_mode),
                                             AmdGpu::GetMicroTileMode(tile_mode), row.bpp) == row.tile_split);
        }
    }
}

void TestAltPipeConfig() {
    CHECK(AmdGpu::GetMacroTileLayout(TileMode::Thin2DThin, 32, 1, false).pipe_config == PipeConfig::P8_32x32_16x16);
    CHECK(AmdGpu::GetMacroTileLayout(TileMode::Thin2DThin, 32, 1, true).pipe_config == PipeConfig::P16);
    CHECK(AmdGpu::GetPipeCount(PipeConfig::P16) == 16);
}

void TestGameAddresses() {
    int altered = 0, alt_rows = 0;
    for (const AddressRow& row : address_rows) {
        const Surface surface{TileMode(row.tile_mode), row.bpp, row.alt_tile != 0, 1024, 1024};
        CHECK(MacroTiledOffset(surface, row.x, row.y) == row.offset);
        if (row.alt_tile) {
            ++alt_rows;
            Surface base = surface;
            base.alt_tile = false;
            altered += MacroTiledOffset(base, row.x, row.y) != row.offset;
        }
    }
    CHECK(alt_rows > 0 && altered * 2 > alt_rows);
}

void TestDetileStaysInBounds() {
    constexpr unsigned size = 512;
    const Surface surfaces[] = {
        {TileMode::Thin2DThin, 32, false, size, size}, {TileMode::Thin2DThin, 32, true, size, size},
        {TileMode::Display2DThin, 32, true, size, size}, {TileMode::Thin2DThin, 8, true, size, size},
    };
    constexpr unsigned guard = 64;
    for (const Surface& surface : surfaces) {
        const unsigned bytes_per_pixel = surface.bpp / 8;
        const size_t tiled_size = size_t(size) * size * bytes_per_pixel;
        std::vector<uint8_t> tiled(tiled_size);
        for (size_t i = 0; i < tiled_size; ++i) {
            tiled[i] = uint8_t(i * 131 + (i >> 8) * 17);
        }
        std::vector<uint8_t> linear(tiled_size + 2 * guard, 0xA5);
        std::vector<uint8_t> seen(tiled_size / bytes_per_pixel, 0);
        bool in_bounds = true, injective = true, matches = true;
        for (unsigned y = 0; y < size; ++y) {
            for (unsigned x = 0; x < size; ++x) {
                const uint64_t offset = MacroTiledOffset(surface, x, y);
                if (offset + bytes_per_pixel > tiled_size) {
                    in_bounds = false;
                    continue;
                }
                injective &= !seen[offset / bytes_per_pixel]++;
                const size_t destination = guard + (size_t(y) * size + x) * bytes_per_pixel;
                for (unsigned byte = 0; byte < bytes_per_pixel; ++byte) {
                    linear[destination + byte] = tiled[offset + byte];
                }
                matches &= linear[destination] == tiled[offset];
            }
        }
        CHECK(in_bounds);
        CHECK(injective);
        CHECK(matches);
        for (unsigned i = 0; i < guard; ++i) {
            CHECK(linear[i] == 0xA5 && linear[guard + tiled_size + i] == 0xA5);
        }
    }
}

void TestAltTileFollowsDescriptor() {
    AmdGpu::Image image{};
    image.width = 255;
    image.height = 255;
    image.pitch = 255;
    image.data_format = u64(AmdGpu::DataFormat::Format8_8_8_8);
    image.tiling_index = u64(TileMode::Thin2DThin);
    image.type = u64(AmdGpu::ImageType::Color2D);
    const Shader::ImageResource resource{};
    CHECK(!VideoCore::ImageInfo(image, resource).alt_tile);
    image.alt_tile_mode = 1;
    CHECK(VideoCore::ImageInfo(image, resource).alt_tile);
}

} // namespace

int main() {
    TestMacroTileLayouts();
    TestAltPipeConfig();
    TestGameAddresses();
    TestDetileStaysInBounds();
    TestAltTileFollowsDescriptor();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
