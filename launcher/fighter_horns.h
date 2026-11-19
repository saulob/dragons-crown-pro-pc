// Where the Fighter's horns are in its helmet sprites. The game draws helmet and horns into one sprite per pose, so each
// horn is told apart by the cut across its base: the horn is the drawn region its seed reaches without crossing the cut.
#pragma once

#include <cstdint>
#include <span>

namespace dcp {

struct HornPoint {
    int16_t x = 0, y = 0; // texels from the sprite's top-left corner
};

struct HornCut {
    HornPoint seed;
    std::span<const HornPoint> cut; // a polyline whose ends lie where no preset draws anything
};

// A helmet sprite of PLAYER/FIGHTER00.FTX surface 0, by its atlas rectangle [x0, x1) x [y0, y1).
struct HornSprite {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::span<const HornCut> horns;
};

std::span<const HornSprite> FighterHornSprites();

} // namespace dcp
