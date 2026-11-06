// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

#include "video_core/renderer_opengl/gl_detile.h"

namespace OpenGL {
namespace {

constexpr u32 MicroTileWidth = 8;
constexpr u32 MicroTileHeight = 8;
constexpr u32 NumPipeInterleaveBits = 8;

constexpr u32 Bit(u32 value, u32 index) {
    return (value >> index) & 1;
}

constexpr bool IsPrtMode(AmdGpu::ArrayMode mode) {
    using enum AmdGpu::ArrayMode;
    return mode == ArrayPrtTiledThin1 || mode == ArrayPrtTiledThick ||
           mode == ArrayPrt2DTiledThin1 || mode == ArrayPrt2DTiledThick ||
           mode == ArrayPrt3DTiledThin1 || mode == ArrayPrt3DTiledThick;
}

constexpr bool Is3DMode(AmdGpu::ArrayMode mode) {
    using enum AmdGpu::ArrayMode;
    return mode == Array3DTiledThin1 || mode == Array3DTiledThick || mode == Array3DTiledXThick;
}

constexpr bool Is2DMode(AmdGpu::ArrayMode mode) {
    using enum AmdGpu::ArrayMode;
    return mode == Array2DTiledThin1 || mode == Array2DTiledThick || mode == Array2DTiledXThick;
}

constexpr bool HasTileSplitRotation(AmdGpu::ArrayMode mode) {
    using enum AmdGpu::ArrayMode;
    return mode == Array2DTiledThin1 || mode == Array3DTiledThin1 ||
           mode == ArrayPrt2DTiledThin1 || mode == ArrayPrt3DTiledThin1;
}

} // namespace

SurfaceTiler::SurfaceTiler(const TilingParams& params, u32 pitch_, u32 height_)
    : array_mode{params.array_mode}, num_bits{params.num_bits}, num_samples{params.num_samples},
      bank_swizzle{params.bank_swizzle}, pitch{pitch_}, height{height_},
      micro_mode{AmdGpu::GetMicroTileMode(params.tile_mode)},
      thickness{AmdGpu::GetMicroTileThickness(params.array_mode)},
      micro_tile_bytes{MicroTileWidth * MicroTileHeight * thickness * num_bits * num_samples / 8},
      macro_tiled{AmdGpu::IsMacroTiled(params.array_mode)} {
    if (!macro_tiled) {
        return;
    }
    const auto layout = AmdGpu::GetMacroTileLayout(params.tile_mode, params.num_bits,
                                                   params.num_samples, params.alt_tile);
    pipe_config = layout.pipe_config;
    num_pipes = layout.pipe_config == AmdGpu::PipeConfig::P2    ? 2
                : layout.pipe_config == AmdGpu::PipeConfig::P16 ? 16
                                                                : 8;
    num_pipe_bits = std::bit_width(num_pipes) - 1;
    bank_width = layout.bank_width;
    bank_height = layout.bank_height;
    num_banks = layout.num_banks;
    num_bank_bits = std::bit_width(num_banks) - 1;
    tile_split_bytes =
        AmdGpu::CalculateTileSplit(params.tile_mode, params.array_mode, micro_mode, params.num_bits);
    macro_tile_aspect = layout.macro_tile_aspect;
}

u32 SurfaceTiler::PixelIndex(u32 x, u32 y, u32 z) const {
    std::array<u32, 9> p{};
    const u32 x0 = Bit(x, 0), x1 = Bit(x, 1), x2 = Bit(x, 2);
    const u32 y0 = Bit(y, 0), y1 = Bit(y, 1), y2 = Bit(y, 2);
    const u32 z0 = Bit(z, 0), z1 = Bit(z, 1), z2 = Bit(z, 2);
    if (micro_mode == AmdGpu::MicroTileMode::Display) {
        switch (num_bits) {
        case 8:
            p = {x0, x1, x2, y1, y0, y2};
            break;
        case 16:
            p = {x0, x1, x2, y0, y1, y2};
            break;
        case 32:
            p = {x0, x1, y0, x2, y1, y2};
            break;
        case 64:
            p = {x0, y0, x1, x2, y1, y2};
            break;
        case 128:
            p = {y0, x0, x1, x2, y1, y2};
            break;
        default:
            break;
        }
    } else if (micro_mode == AmdGpu::MicroTileMode::Thin ||
               micro_mode == AmdGpu::MicroTileMode::Depth) {
        p = {x0, y0, x1, y1, x2, y2};
    } else {
        switch (num_bits) {
        case 8:
        case 16:
            p = {x0, y0, x1, y1, z0, z1};
            break;
        case 32:
            p = {x0, y0, x1, z0, y1, z1};
            break;
        case 64:
        case 128:
            p = {x0, y0, z0, x1, y1, z1};
            break;
        default:
            break;
        }
        p[6] = x2;
        p[7] = y2;
        if (thickness == 8) {
            p[8] = z2;
        }
    }
    u32 pixel = 0;
    for (u32 i = 0; i < p.size(); ++i) {
        pixel |= p[i] << i;
    }
    return pixel;
}

u32 SurfaceTiler::MicroTiledAddress(u32 x, u32 y, u32 slice) const {
    const u32 slice_bytes = (pitch * height * thickness * num_bits * num_samples + 7) / 8;
    const u32 tiles_per_row = pitch / MicroTileWidth;
    const u32 slice_offset = (slice / thickness) * slice_bytes;
    const u32 tile_offset =
        ((y / MicroTileHeight) * tiles_per_row + x / MicroTileWidth) * micro_tile_bytes;
    const u32 pixel = PixelIndex(x, y, slice);
    const u32 pixel_offset = micro_mode == AmdGpu::MicroTileMode::Depth
                                 ? pixel * num_bits * num_samples
                                 : pixel * num_bits;
    return slice_offset + tile_offset + pixel_offset / 8;
}

u32 SurfaceTiler::Pipe(u32 x, u32 y, u32 slice) const {
    const u32 tx = x / MicroTileWidth, ty = y / MicroTileHeight;
    const u32 x3 = Bit(tx, 0), x4 = Bit(tx, 1), x5 = Bit(tx, 2), x6 = Bit(tx, 3);
    const u32 y3 = Bit(ty, 0), y4 = Bit(ty, 1), y5 = Bit(ty, 2);
    u32 p0 = 0, p1 = 0, p2 = 0, p3 = 0;
    switch (pipe_config) {
    case AmdGpu::PipeConfig::P2:
        p0 = x3 ^ y3;
        break;
    case AmdGpu::PipeConfig::P8_32x32_8x16:
        p0 = x4 ^ y3 ^ x5;
        p1 = x3 ^ y4;
        p2 = x5 ^ y5;
        break;
    case AmdGpu::PipeConfig::P8_32x32_16x16:
        p0 = x3 ^ y3 ^ x4;
        p1 = x4 ^ y4;
        p2 = x5 ^ y5;
        break;
    case AmdGpu::PipeConfig::P16:
        p0 = x3 ^ y3 ^ x4;
        p1 = x4 ^ y4;
        p2 = x5 ^ y5;
        p3 = x6 ^ y5;
        break;
    default:
        break;
    }
    u32 pipe = p0 | p1 << 1 | p2 << 2 | p3 << 3;
    if (Is3DMode(array_mode)) {
        const u32 swizzle = std::max(1u, num_pipes / 2 - 1) * (slice / thickness);
        pipe ^= swizzle & (num_pipes - 1);
    }
    return pipe;
}

u32 SurfaceTiler::Bank(u32 x, u32 y, u32 slice, u32 tile_split_slice) const {
    const u32 tx = x / MicroTileWidth / (bank_width * num_pipes);
    const u32 ty = y / MicroTileHeight / bank_height;
    const u32 x3 = Bit(tx, 0), x4 = Bit(tx, 1), x5 = Bit(tx, 2), x6 = Bit(tx, 3);
    const u32 y3 = Bit(ty, 0), y4 = Bit(ty, 1), y5 = Bit(ty, 2), y6 = Bit(ty, 3);
    u32 b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    switch (num_banks) {
    case 16:
        b0 = x3 ^ y6;
        b1 = x4 ^ y5 ^ y6;
        b2 = x5 ^ y4;
        b3 = x6 ^ y3;
        break;
    case 8:
        b0 = x3 ^ y5;
        b1 = x4 ^ y4 ^ y5;
        b2 = x5 ^ y3;
        break;
    case 4:
        b0 = x3 ^ y4;
        b1 = x4 ^ y3;
        break;
    case 2:
        b0 = x3 ^ y3;
        break;
    default:
        break;
    }
    u32 bank = b0 | b1 << 1 | b2 << 2 | b3 << 3;
    u32 slice_rotation = 0;
    if (Is2DMode(array_mode)) {
        slice_rotation = (num_banks / 2 - 1) * (slice / thickness);
    } else if (Is3DMode(array_mode)) {
        slice_rotation = std::max(1u, num_pipes / 2 - 1) * (slice / thickness) / num_pipes;
    }
    const u32 tile_split_rotation =
        HasTileSplitRotation(array_mode) ? (num_banks / 2 + 1) * tile_split_slice : 0;
    bank ^= bank_swizzle + slice_rotation;
    bank ^= tile_split_rotation;
    return bank & (num_banks - 1);
}

u32 SurfaceTiler::MacroTiledAddress(u32 x, u32 y, u32 slice) const {
    const u32 pixel = PixelIndex(x, y, slice);
    const u32 pixel_offset = micro_mode == AmdGpu::MicroTileMode::Depth
                                 ? pixel * num_bits * num_samples
                                 : pixel * num_bits;
    u32 element_offset = pixel_offset / 8;

    u32 tile_bytes = micro_tile_bytes;
    u32 slices_per_tile = 1;
    u32 tile_split_slice = 0;
    if (micro_tile_bytes > tile_split_bytes && thickness == 1) {
        slices_per_tile = micro_tile_bytes / tile_split_bytes;
        tile_split_slice = element_offset / tile_split_bytes;
        element_offset %= tile_split_bytes;
        tile_bytes = tile_split_bytes;
    }

    const u32 macro_pitch = MicroTileWidth * bank_width * num_pipes * macro_tile_aspect;
    const u32 macro_height = MicroTileHeight * bank_height * num_banks / macro_tile_aspect;
    const u32 macro_bytes = tile_bytes * (macro_pitch / MicroTileWidth) *
                            (macro_height / MicroTileHeight) / (num_pipes * num_banks);
    const u32 macros_per_row = pitch / macro_pitch;
    const u32 macro_offset =
        ((y / macro_height) * macros_per_row + x / macro_pitch) * macro_bytes;
    const u32 macros_per_slice = macros_per_row * (height / macro_height);
    const u32 slice_bytes = macros_per_slice * macro_bytes;
    const u32 slice_offset =
        slice_bytes * (tile_split_slice + slices_per_tile * (slice / thickness));

    const u32 tile_row = (y / MicroTileHeight) % bank_height;
    const u32 tile_column = ((x / MicroTileWidth) / num_pipes) % bank_width;
    const u32 tile_offset = (tile_row * bank_width + tile_column) * tile_bytes;
    const u32 total = slice_offset + macro_offset + element_offset + tile_offset;

    if (IsPrtMode(array_mode)) {
        x %= macro_pitch;
        y %= macro_height;
    }
    const u32 pipe = Pipe(x, y, slice);
    const u32 bank = Bank(x, y, slice, tile_split_slice);

    const u32 interleave_offset = total & ((1u << NumPipeInterleaveBits) - 1);
    const u32 offset = total >> NumPipeInterleaveBits;
    return interleave_offset | pipe << NumPipeInterleaveBits |
           bank << (NumPipeInterleaveBits + num_pipe_bits) |
           offset << (NumPipeInterleaveBits + num_pipe_bits + num_bank_bits);
}

u32 SurfaceTiler::Address(u32 x, u32 y, u32 slice) const {
    return macro_tiled ? MacroTiledAddress(x, y, slice) : MicroTiledAddress(x, y, slice);
}

// Inside a thin micro tile no larger than the pipe interleave, an element's address is the tile's
// plus its pixel offset, so only the tile's own address needs the full calculation.
bool SurfaceTiler::UsesMicroTileCopies() const {
    return thickness == 1 && num_samples == 1 && micro_tile_bytes <= 256 &&
           (!macro_tiled || micro_tile_bytes <= tile_split_bytes) && pitch % MicroTileWidth == 0 &&
           height % MicroTileHeight == 0;
}

void SurfaceTiler::Detile(const u8* tiled, size_t tiled_size, u8* linear, u32 slices) const {
    const u32 element_bytes = num_bits / 8;
    const size_t row_bytes = size_t(pitch) * element_bytes;
    const size_t slice_bytes = row_bytes * height;
    if (!UsesMicroTileCopies()) {
        for (u32 slice = 0; slice < slices; ++slice) {
            u8* out = linear + slice_bytes * slice;
            for (u32 y = 0; y < height; ++y) {
                for (u32 x = 0; x < pitch; ++x) {
                    const u32 address = Address(x, y, slice);
                    if (size_t(address) + element_bytes <= tiled_size) {
                        std::memcpy(out + y * row_bytes + size_t(x) * element_bytes,
                                    tiled + address, element_bytes);
                    }
                }
            }
        }
        return;
    }
    std::array<u32, 64> element_offsets;
    for (u32 y = 0; y < MicroTileHeight; ++y) {
        for (u32 x = 0; x < MicroTileWidth; ++x) {
            element_offsets[y * MicroTileWidth + x] = PixelIndex(x, y, 0) * element_bytes;
        }
    }
    for (u32 slice = 0; slice < slices; ++slice) {
        u8* out = linear + slice_bytes * slice;
        for (u32 tile_y = 0; tile_y < height; tile_y += MicroTileHeight) {
            for (u32 tile_x = 0; tile_x < pitch; tile_x += MicroTileWidth) {
                const u32 base = Address(tile_x, tile_y, slice);
                if (size_t(base) + micro_tile_bytes > tiled_size) {
                    continue;
                }
                u8* tile_out = out + tile_y * row_bytes + size_t(tile_x) * element_bytes;
                for (u32 i = 0; i < 64; ++i) {
                    const size_t row = i / MicroTileWidth, column = i % MicroTileWidth;
                    std::memcpy(tile_out + row * row_bytes + column * element_bytes,
                                tiled + base + element_offsets[i], element_bytes);
                }
            }
        }
    }
}

} // namespace OpenGL
