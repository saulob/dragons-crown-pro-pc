#include "page_icons.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "game_translation.h"

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "third_party/nanosvg/nanosvg.h"
#include "third_party/nanosvg/nanosvgrast.h"

namespace dcp {

extern const TranslationFile kPageIconFiles[];

namespace {

constexpr PageIconAsset kAssets[] = {
    {PageId::kPlay, "play.svg", 0.84f},         {PageId::kDisplay, "display.svg", 0.90f},
    {PageId::kGraphics, "graphics.svg", 0.84f}, {PageId::kGameplay, "gameplay.svg", 0.92f},
    {PageId::kOnline, "online.svg", 0.90f},     {PageId::kCheats, "cheats.svg", 0.92f},
    {PageId::kControls, "controls.svg", 0.94f}, {PageId::kTrophies, "trophies.svg", 0.86f},
    {PageId::kBugReport, "bug-report.svg", 0.90f}, {PageId::kAbout, "about.svg", 0.88f},
};

struct Bounds {
    float min_x = FLT_MAX, min_y = FLT_MAX, max_x = -FLT_MAX, max_y = -FLT_MAX;
    float Width() const { return max_x - min_x; }
    float Height() const { return max_y - min_y; }
};

Bounds ArtworkBounds(const NSVGimage& image) {
    Bounds bounds;
    for (const NSVGshape* shape = image.shapes; shape; shape = shape->next) {
        const bool stroked = shape->stroke.type != NSVG_PAINT_NONE && shape->strokeWidth > 0.0f;
        if (!(shape->flags & NSVG_FLAGS_VISIBLE) || (shape->fill.type == NSVG_PAINT_NONE && !stroked)) {
            continue;
        }
        const float grow = stroked ? shape->strokeWidth * 0.5f : 0.0f;
        bounds.min_x = std::min(bounds.min_x, shape->bounds[0] - grow);
        bounds.min_y = std::min(bounds.min_y, shape->bounds[1] - grow);
        bounds.max_x = std::max(bounds.max_x, shape->bounds[2] + grow);
        bounds.max_y = std::max(bounds.max_y, shape->bounds[3] + grow);
    }
    return bounds;
}

} // namespace

std::span<const PageIconAsset> PageIconAssets() {
    return kAssets;
}

const PageIconAsset* FindPageIcon(PageId page) {
    const auto found = std::find_if(std::begin(kAssets), std::end(kAssets),
                                    [page](const PageIconAsset& asset) { return asset.page == page; });
    return found == std::end(kAssets) ? nullptr : found;
}

std::vector<uint8_t> RasterizeIcon(const uint8_t* svg, size_t size, float optical_scale, int pixels) {
    if (!svg || size == 0 || pixels <= 0 || pixels > 1024) {
        return {};
    }
    std::string text(reinterpret_cast<const char*>(svg), size);
    const std::unique_ptr<NSVGimage, decltype(&nsvgDelete)> image(nsvgParse(text.data(), "px", 96.0f), nsvgDelete);
    if (!image) {
        return {};
    }
    const Bounds bounds = ArtworkBounds(*image);
    const float longest = std::max(bounds.Width(), bounds.Height());
    if (!(longest > 0.0f)) {
        return {};
    }
    const std::unique_ptr<NSVGrasterizer, decltype(&nsvgDeleteRasterizer)> rasterizer(nsvgCreateRasterizer(),
                                                                                      nsvgDeleteRasterizer);
    if (!rasterizer) {
        return {};
    }
    const float scale = pixels * optical_scale / longest;
    const float x = (pixels - bounds.Width() * scale) * 0.5f - bounds.min_x * scale;
    const float y = (pixels - bounds.Height() * scale) * 0.5f - bounds.min_y * scale;
    std::vector<uint8_t> rgba(size_t(pixels) * size_t(pixels) * 4);
    nsvgRasterize(rasterizer.get(), image.get(), x, y, scale, rgba.data(), pixels, pixels, pixels * 4);
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        rgba[i] = rgba[i + 1] = rgba[i + 2] = 255;
    }
    return rgba;
}

std::vector<uint8_t> RenderPageIcon(PageId page, int pixels) {
    const PageIconAsset* asset = FindPageIcon(page);
    if (!asset) {
        return {};
    }
    for (const TranslationFile* file = kPageIconFiles; file->name; ++file) {
        if (std::strcmp(file->name, asset->file) == 0) {
            return RasterizeIcon(file->data, file->size, asset->optical_scale, pixels);
        }
    }
    return {};
}

} // namespace dcp
