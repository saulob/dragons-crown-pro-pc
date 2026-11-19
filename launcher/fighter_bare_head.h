// The Fighter's own bare head under a hidden helmet. The game draws head and helmet as one sprite per pose and has bare-head
// sprites only where the helmet comes off (IDLE_A/B, BLOWDOWN_B, DIE_B, WAKE_B), so a hidden helmet's sprite is repainted
// with those sprites, placed where the head sits under it.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "game_mbs.h"

namespace dcp {

// Texels [x0, x1) x [y0, y1) of PLAYER/FIGHTER0N.FTX surface 0.
struct AtlasRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool Contains(int x, int y) const { return x >= x0 && y >= y0 && x < x1 && y < y1; }
};

// A bare-head sprite drawn into a helmet sprite: the helmet texel centre (x, y) samples it at
// (a x + b y + c, d x + e y + f).
struct BareHeadLayer {
    AtlasRect source;
    double a = 1, b = 0, c = 0, d = 0, e = 1, f = 0;
};

enum class HelmetSprite {
    kBareHead, // worn: repainted with the bare head
    kClear,    // never the head: a visor piece, or a helmet held, falling or lying beside the head
};

struct HelmetSpriteRule {
    uint32_t id = 0; // MBS piece id
    HelmetSprite kind = HelmetSprite::kClear;
    AtlasRect rect;
    std::span<const BareHeadLayer> layers = {}; // bottom first
};

std::span<const HelmetSpriteRule> FighterHelmetSprites();

// A sprite the model draws both on the head and off it: the off-head draws sample a copy of its blocks moved by (dx, dy)
// texels into free atlas space, so a hidden helmet can leave the copy empty while the head shows in the original.
struct HelmetCopy {
    uint32_t id = 0;
    int dx = 0, dy = 0; // multiples of the 4-texel block
};
std::span<const HelmetCopy> FighterHelmetCopies();
// The copied blocks of a sprite: its rectangle and one block around it, which filtering reaches.
AtlasRect HelmetCopySource(const HelmetSpriteRule& rule);

// Layer records of helmet sprites drawn off the head: in a key with no body, or while any track of the animation draws
// the game's own bare head.
std::vector<uint32_t> OffHeadHelmetLayers(const Mbs& model);

// PLAYER/FIGHTER00.MBS with the off-head draws of every copied sprite sampling its copy. Nothing when the file is not the
// model the table was made for.
std::optional<std::vector<uint8_t>> FighterModelWithHelmetCopies(std::span<const uint8_t> model);
// The atlas pack with every copy's blocks written, byte for byte, from its sprite's blocks.
bool AddHelmetCopies(std::vector<uint8_t>& pack);

} // namespace dcp
