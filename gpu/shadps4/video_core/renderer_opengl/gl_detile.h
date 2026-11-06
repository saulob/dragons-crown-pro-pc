// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// CPU port of host_shaders/tiling.comp: OpenGL 3.3 has no compute shaders.

#pragma once

#include <cstddef>
#include "common/types.h"
#include "video_core/amdgpu/tiling.h"

namespace OpenGL {

struct TilingParams {
    AmdGpu::TileMode tile_mode;
    AmdGpu::ArrayMode array_mode;
    u32 num_bits;
    u32 num_samples;
    bool alt_tile;
    u32 bank_swizzle;
};

class SurfaceTiler {
public:
    /// `pitch` and `height` are in elements (blocks for compressed formats).
    SurfaceTiler(const TilingParams& params, u32 pitch, u32 height);

    [[nodiscard]] u32 Address(u32 x, u32 y, u32 slice) const;

    /// Fills `linear` with `slices` tightly packed pitch x height images.
    void Detile(const u8* tiled, size_t tiled_size, u8* linear, u32 slices) const;

    [[nodiscard]] bool UsesMicroTileCopies() const;

private:
    [[nodiscard]] u32 PixelIndex(u32 x, u32 y, u32 z) const;
    [[nodiscard]] u32 MicroTiledAddress(u32 x, u32 y, u32 slice) const;
    [[nodiscard]] u32 MacroTiledAddress(u32 x, u32 y, u32 slice) const;
    [[nodiscard]] u32 Pipe(u32 x, u32 y, u32 slice) const;
    [[nodiscard]] u32 Bank(u32 x, u32 y, u32 slice, u32 tile_split_slice) const;

    AmdGpu::ArrayMode array_mode;
    u32 num_bits;
    u32 num_samples;
    u32 bank_swizzle;
    u32 pitch;
    u32 height;
    AmdGpu::MicroTileMode micro_mode;
    u32 thickness;
    u32 micro_tile_bytes;
    bool macro_tiled;
    AmdGpu::PipeConfig pipe_config{};
    u32 num_pipes{1};
    u32 num_pipe_bits{};
    u32 bank_width{1};
    u32 bank_height{1};
    u32 num_banks{1};
    u32 num_bank_bits{};
    u32 tile_split_bytes{1};
    u32 macro_tile_aspect{1};
};

} // namespace OpenGL
