// The sidebar's page icons: the SVGs of assets/launcher/icons compiled into the executable, drawn at the exact pixel
// size the sidebar asks for.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "options.h"

namespace dcp {

struct PageIconAsset {
    PageId page;
    const char* file;
    float optical_scale;  // share of the icon box taken by the artwork's longer side
};

std::span<const PageIconAsset> PageIconAssets();
const PageIconAsset* FindPageIcon(PageId page);

// Square RGBA: white, with the artwork as alpha, so one image takes every sidebar colour. Empty on failure.
std::vector<uint8_t> RasterizeIcon(const uint8_t* svg, size_t size, float optical_scale, int pixels);
std::vector<uint8_t> RenderPageIcon(PageId page, int pixels);

} // namespace dcp
