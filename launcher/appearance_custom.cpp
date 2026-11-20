#include "appearance_custom.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>

#include "fighter_horns.h"
#include "game_texture.h"
#include <cctype>

namespace dcp {
namespace {

constexpr uint32_t kMagic = 0x41434344; // "DCCA"
constexpr uint32_t kVersion = 2;
constexpr uint32_t kRegionMagic = 0x4D524344; // "DCRM"

void Put32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(value >> (8 * i)));
}

void PutText(std::vector<uint8_t>& out, std::string_view text) {
    out.push_back(uint8_t(std::min<size_t>(text.size(), 255)));
    out.insert(out.end(), text.begin(), text.begin() + std::ptrdiff_t(std::min<size_t>(text.size(), 255)));
}

class Cursor {
public:
    explicit Cursor(std::span<const uint8_t> bytes) : bytes_(bytes) {}
    bool U8(uint8_t& v) {
        if (at_ + 1 > bytes_.size()) return false;
        v = bytes_[at_++];
        return true;
    }
    bool U32(uint32_t& v) {
        if (at_ + 4 > bytes_.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(bytes_[at_++]) << (8 * i);
        return true;
    }
    bool Text(std::string& v) {
        uint8_t n = 0;
        if (!U8(n) || at_ + n > bytes_.size()) return false;
        v.assign(reinterpret_cast<const char*>(bytes_.data() + at_), n);
        at_ += n;
        return true;
    }
    bool done() const { return at_ == bytes_.size(); }

private:
    std::span<const uint8_t> bytes_;
    size_t at_ = 0;
};

std::optional<CustomKey> ParseKey(const std::string& text) {
    std::vector<std::string> fields;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '|') {
            fields.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    if (fields.size() != 5) return std::nullopt;
    CustomKey key;
    key.title_id = fields[0];
    key.save_dir = fields[1];
    try {
        key.data_slot = std::stoi(fields[2]);
        key.character_slot = std::stoi(fields[3]);
        key.klass = ClassFromId(uint8_t(std::stoi(fields[4])));
    } catch (...) {
        return std::nullopt;
    }
    return key;
}

template <typename Visit>
void ForEachTexel(const MbsQuad& quad, int width, int height, Visit visit) {
    const auto at = [&](const MbsPoint& p) { return MbsPoint{p.x * float(width), p.y * float(height)}; };
    const MbsPoint c = at(quad.center);
    for (size_t i = 0; i < 4; ++i) {
        const MbsPoint a = at(quad.corners[i]), b = at(quad.corners[(i + 1) % 4]);
        const float area = (a.x - c.x) * (b.y - c.y) - (a.y - c.y) * (b.x - c.x);
        if (std::fabs(area) < 1e-6f) continue;
        const int x0 = std::max(0, int(std::floor(std::min({a.x, b.x, c.x}))));
        const int x1 = std::min(width - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
        const int y0 = std::max(0, int(std::floor(std::min({a.y, b.y, c.y}))));
        const int y1 = std::min(height - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float px = float(x) + 0.5f, py = float(y) + 0.5f;
                const float w0 = ((a.x - px) * (b.y - py) - (a.y - py) * (b.x - px)) / area;
                const float w1 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
                const float w2 = 1.0f - w0 - w1;
                if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) visit(x, y);
            }
        }
    }
}

bool WriteAtomic(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!out.flush()) {
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) std::filesystem::remove(temporary, ec);
    return !ec;
}

} // namespace

namespace {

constexpr BodySurface kOneSurface[] = {{0, 0}};
constexpr BodySurface kDwarfSurfaces[] = {{0, 0}, {1, 1}};

constexpr uint16_t R(int region) {
    return uint16_t(1u << (region - 1));
}
constexpr uint16_t kAnyRegion = 0x1FF; // the eight regions and the texels outside them

// Region ids come from BuildRegionMaps (numbered by base brightness); each class names what its regions cover.
// The Fighter's visor glow in each preset, read from the atlases.
constexpr Rgb kFighterGlow[] = {{94, 120, 162}, {91, 87, 11}, {95, 120, 160}, {64, 119, 138}, {120, 137, 169}};
// The crest's gem is its regions 5 and 8; the frame around it is the armor's metal.
// The skin of the bare head and of the face in the visor, lit and shaded, in each preset.
constexpr Rgb kFighterSkin[] = {{241, 212, 123}, {236, 200, 104}, {212, 141, 63}, {243, 207, 110}, {248, 218, 156},
                                {212, 183, 108}, {203, 170, 90}, {146, 100, 58}, {210, 177, 97}, {213, 189, 137}};
// The helmet is drawn in 1100-1135 whole with its horns and visor glow; 1200-1299 draw the face under a lifted helmet.
constexpr CustomPart kFighterParts[] = {
    {.key = "armor", .label = "Armor", .regions = R(1) | R(4) | R(5) | R(6) | R(7) | R(8)},
    {.key = "trim", .label = "Engraved Trim", .piece_range = {800, 899}},
    {.key = "leather", .label = "Leather", .regions = R(2) | R(3)},
    {.key = "eye", .label = "Eye", .mark = PaintedMark::kSkinTone, .piece_range = {1100, 1299}, .tone = kFighterGlow},
    {.key = "crest", .label = "Crest", .piece_range = {900, 914}},
    {.key = "crest_accent", .label = "Crest Accent", .piece_regions = R(5) | R(8), .mark = PaintedMark::kPainted, .piece_range = {900, 914}},
    {.key = "horns", .label = "Horns", .mark = PaintedMark::kHorn, .piece_range = {1100, 1135}},
    {.key = "helmet", .label = "Helmet", .piece_range = {1100, 1135}},
    {.key = "face", .label = "Face", .piece_range = {1200, 1299}, .tone = kFighterSkin, .tone_range = {1100, 1135}},
    {.key = "sword", .label = "Sword", .weapon = 0},
    {.key = "shield", .label = "Shield", .weapon = 1},
    {.key = "shield_accent", .label = "Shield Accent", .weapon = 1, .accent = true},
};
// Piece ids as the in-game model (PLAYER/<CLASS>00.MBS) numbers them.
constexpr uint32_t kAmazonHair[] = {10, 11, 12, 13, 30, 31, 51, 52, 70, 71, 90, 110, 111, 130, 131, 132, 133, 134,
                                    150, 151, 170, 172, 190, 200, 201, 202, 203, 204, 205, 206, 207, 210, 211, 212,
                                    213, 214, 215, 216, 217, 220, 221, 222, 223, 224, 225, 226, 227, 240, 241, 242,
                                    243, 244, 245, 250, 251, 252, 253, 254, 255, 260, 261, 262, 263, 264, 265, 282,
                                    283, 284, 286, 287, 300, 301, 302, 320};
constexpr uint32_t kAmazonHeads[] = {1, 3, 21, 41, 61, 81, 101, 121, 141, 161, 181};
constexpr uint32_t kAmazonFace[] = {1, 3, 21, 41, 61, 81, 101, 121, 141, 161, 181, 22, 42, 43, 44, 45, 48, 62, 82, 102,
                                    103, 122, 123, 124, 125, 126, 127, 142, 162, 183};
constexpr uint32_t kAmazonSkin[] = {1, 3, 21, 41, 61, 81, 101, 121, 141, 161, 181, 500, 501, 510, 511, 512, 513, 524,
                                    527, 530, 531};
constexpr uint32_t kAmazonThighs[] = {524, 527, 530, 531};
constexpr uint32_t kAmazonArmor[] = {502, 503, 504, 505, 506, 521, 522, 525, 526};
constexpr uint32_t kAmazonFeather[] = {362};
// Upper arms and armor pieces: the arm bands and bracelets painted on them share colour regions with skin.
constexpr uint32_t kAmazonBanded[] = {500, 501, 502, 503, 504, 505, 506, 521, 522, 525, 526};
constexpr CustomPart kAmazonParts[] = {
    {.key = "hair", .label = "Hair", .reveals_unpainted = true, .pieces = kAmazonHair, .tone_pieces = kAmazonHeads},
    {"skin", "Skin", R(5) | R(6) | R(8), true, kAmazonSkin, uint16_t(kAnyRegion & ~(R(1) | R(3)))},
    {.key = "tattoo", .label = "Tattoo", .pieces = kAmazonThighs, .mark = PaintedMark::kTattooInk},
    {.key = "eye", .label = "Eye", .regions = R(2), .pieces = kAmazonFace, .piece_regions = R(2), .mark = PaintedMark::kPainted},
    {"feather", "Feather", 0, false, kAmazonFeather},
    {.key = "armor", .label = "Boots & Gauntlets", .regions = R(4) | R(7), .reveals_unpainted = true, .pieces = kAmazonArmor,
     .piece_regions = uint16_t(kAnyRegion & ~(R(5) | R(6) | R(8))), .dyed_pieces = kAmazonBanded},
    {"bikini", "Bikini", R(1) | R(3), true},
    {.key = "axe", .label = "Axe", .weapon = 0},
};
// Elf pieces of every pose of the in-game model, sorted from the atlas: heads 0-123, collars, shorts, gloves and legs among
// 500-959, tunic 1000-1115, hood and cape 1500-1799, quiver 3000s, arrows 4000s.
constexpr uint32_t kElfCollar[] = {500, 520, 529, 543, 557, 572, 590, 611, 612, 625, 643, 651, 665, 666, 667, 679, 680, 681, 707, 719,
    732, 745, 746, 760, 761, 785, 792, 865, 866, 867, 889};
// Shorts seen whole, then the thigh pieces that draw their hem next to bare skin.
constexpr uint32_t kElfShorts[] = {510, 512, 516, 523, 526, 530, 533, 540, 541, 542, 550, 551, 554, 558, 565, 566, 569, 579, 582, 600,
    603, 605, 610, 618, 621, 624, 628, 631, 633, 637, 640, 642, 647, 658, 661, 664, 672, 675, 688, 706,
    713, 716, 725, 726, 729, 738, 739, 742, 753, 754, 757, 767, 768, 771, 774, 782, 790, 791, 799, 800,
    816, 826, 827, 838, 839, 860, 872, 875, 880, 881, 883, 885, 888};
constexpr uint32_t kElfGloves[] = {502, 505, 506, 507, 509, 522, 546, 549, 561, 564, 574, 577, 585, 586, 587, 588, 589, 592, 598, 614,
    617, 626, 627, 636, 646, 654, 668, 671, 684, 687, 694, 695, 696, 712, 722, 735, 749, 752, 764, 775,
    778, 788, 795, 798, 809, 810, 815, 837, 847, 850, 878, 890, 891, 892, 900, 901, 902, 903, 950, 952,
    953, 954, 956, 958, 959};
// Arms and thighs: their pale texels are bare skin clustered with cloth.
constexpr uint32_t kElfSkinPieces[] = {501, 503, 504, 512, 516, 521, 523, 526, 530, 533, 541, 542, 544, 545, 547, 548, 551, 554, 559, 560,
    566, 569, 573, 575, 576, 579, 582, 591, 593, 596, 597, 600, 603, 605, 608, 610, 613, 615, 618, 621,
    628, 631, 635, 637, 640, 644, 645, 647, 655, 656, 658, 661, 669, 670, 672, 675, 682, 683, 685, 686,
    688, 700, 703, 708, 709, 711, 713, 716, 720, 721, 723, 726, 729, 733, 734, 736, 737, 739, 742, 747,
    748, 750, 751, 754, 757, 762, 763, 765, 766, 768, 771, 774, 776, 777, 779, 782, 786, 787, 789, 790,
    791, 793, 794, 796, 797, 800, 811, 812, 817, 827, 830, 833, 834, 835, 836, 839, 845, 846, 848, 849,
    851, 854, 857, 858, 861, 864, 869, 872, 877, 881, 883, 888};
constexpr uint32_t kElfLowerLegs[] = {508, 511, 513, 514, 515, 517, 518, 519, 524, 525, 527, 528, 531, 532, 536, 539, 552, 553, 555, 556,
    567, 570, 571, 580, 581, 583, 584, 601, 602, 604, 606, 607, 609, 619, 620, 622, 623, 629, 630, 632,
    638, 639, 641, 648, 649, 659, 660, 662, 663, 673, 674, 676, 677, 689, 691, 692, 701, 702, 704, 705,
    714, 715, 717, 718, 727, 728, 730, 731, 740, 741, 743, 744, 755, 756, 758, 759, 769, 770, 772, 773,
    780, 781, 783, 784, 801, 802, 803, 804, 805, 818, 819, 820, 821, 822, 828, 829, 831, 832, 840, 841,
    842, 843, 844, 852, 853, 855, 856, 862, 863, 870, 871, 873, 874, 882, 884, 886, 887};
// The laces in each preset, read from the atlases. The light V of each cuff is the cuff's own leather in every preset.
constexpr Rgb kElfLaceTones[] = {{246, 137, 30}, {120, 109, 98}, {146, 122, 151}, {245, 155, 1}, {157, 179, 104}};
constexpr uint16_t kElfHairRegions = R(3) | R(4) | R(6);
constexpr uint16_t kElfEyeRegions = R(1) | R(5) | R(7);
constexpr CustomPart kElfParts[] = {
    {.key = "hood", .label = "Hood & Cape", .regions = R(2), .reveals_unpainted = true, .piece_range = {1500, 1799}},
    {.key = "hair", .label = "Hair", .piece_regions = kElfHairRegions, .piece_range = {0, 123}},
    {.key = "skin", .label = "Skin", .regions = R(8), .reveals_unpainted = true, .pieces = kElfSkinPieces, .piece_regions = R(6) | R(8),
     .mark = PaintedMark::kSkinTone, .piece_range = {0, 123}},
    {.key = "eye", .label = "Eyes", .piece_regions = kElfEyeRegions, .mark = PaintedMark::kPainted,
     .piece_range = {0, 123}},
    {.key = "tunic", .label = "Tunic", .regions = R(4), .piece_range = {1000, 1115}},
    {.key = "belt", .label = "Belt", .mark = PaintedMark::kUndyed, .piece_range = {1000, 1115}, .encloses = true,
     .outlined = true},
    {.key = "collar", .label = "Collar", .pieces = kElfCollar},
    {.key = "shorts", .label = "Shorts", .pieces = kElfShorts, .piece_regions = R(6)},
    {.key = "gloves", .label = "Gloves", .pieces = kElfGloves},
    {.key = "laces", .label = "Laces & Trim", .pieces = kElfLowerLegs, .mark = PaintedMark::kSkinTone, .tone = kElfLaceTones,
     .outlined = true, .ridges_of = "boots"},
    {.key = "boots", .label = "Boots", .regions = R(1), .pieces = kElfLowerLegs, .piece_regions = uint16_t(kAnyRegion & ~R(8))},
    {.key = "quiver", .label = "Quiver", .piece_range = {3000, 3099}},
    {.key = "arrows", .label = "Arrows", .piece_range = {4000, 4499}},
    {.key = "bow", .label = "Bow", .weapon = 0},
};
// Pieces that paint several parts side by side, each part with materials of its own: a material's colour in each
// preset, read from the atlases.
struct Material {
    std::string_view part;
    std::array<Rgb, kNativeColorCount> colors;
};
struct SharedPieces {
    std::span<const std::span<const uint32_t>> pieces;
    std::span<const Material> materials;
    size_t island; // fewer texels of one material inside another are a stroke of shading that happens to match it
    std::string_view dark = {}; // takes the texels too dark to tell; without it they follow the texels around them
};
// The shorts' leather and cloth, bare skin, the laces, and the boot's cuff and leg, each in its lit and shaded tones (the
// presets do not darken every channel alike).
constexpr Material kElfLegMaterials[] = {
    {"shorts", {{{137, 92, 60}, {120, 96, 84}, {61, 51, 49}, {157, 120, 76}, {63, 53, 37}}}},
    {"shorts", {{{46, 23, 2}, {38, 24, 18}, {19, 14, 8}, {51, 31, 1}, {17, 15, 5}}}},
    {"shorts", {{{175, 121, 65}, {160, 141, 125}, {132, 130, 139}, {195, 134, 53}, {133, 137, 84}}}},
    {"shorts", {{{101, 53, 33}, {72, 56, 56}, {65, 63, 76}, {98, 54, 6}, {70, 69, 28}}}},
    {"shorts", {{{60, 37, 28}, {30, 27, 33}, {42, 42, 56}, {52, 29, 0}, {47, 46, 12}}}},
    {"skin", {{{252, 234, 175}, {197, 140, 83}, {232, 227, 219}, {255, 236, 181}, {247, 237, 197}}}},
    {"skin", {{{197, 125, 61}, {136, 60, 17}, {136, 118, 122}, {208, 142, 69}, {194, 144, 97}}}},
    {"skin", {{{72, 38, 22}, {46, 13, 1}, {43, 33, 38}, {88, 58, 38}, {86, 62, 58}}}},
    {"laces", {{{246, 137, 30}, {120, 109, 98}, {146, 122, 151}, {245, 155, 1}, {157, 179, 104}}}},
    {"laces", {{{81, 47, 29}, {40, 38, 40}, {50, 35, 82}, {102, 66, 2}, {56, 76, 44}}}},
    {"boots", {{{182, 139, 82}, {171, 141, 102}, {153, 144, 136}, {198, 129, 90}, {160, 144, 111}}}},
    {"boots", {{{95, 53, 32}, {86, 56, 43}, {70, 61, 64}, {112, 44, 30}, {76, 61, 36}}}},
    {"boots", {{{26, 14, 11}, {23, 17, 13}, {16, 18, 28}, {35, 10, 7}, {24, 16, 6}}}},
    {"boots", {{{55, 32, 28}, {46, 35, 30}, {45, 36, 34}, {54, 30, 40}, {50, 35, 24}}}},
};
// Thighs and arms keep larger stray patches apart than the laced lower legs do.
constexpr std::span<const uint32_t> kElfLimbs[] = {kElfShorts, kElfSkinPieces};
constexpr std::span<const uint32_t> kElfLegs[] = {kElfLowerLegs};
constexpr SharedPieces kElfShared[] = {{kElfLimbs, kElfLegMaterials, 64}, {kElfLegs, kElfLegMaterials, 32, "boots"}};

std::span<const SharedPieces> SharedPiecesOf(CharacterClass klass) {
    if (klass == CharacterClass::kElf) return kElfShared;
    return {};
}
// Dwarf piece ids of every pose of the in-game model, sorted from the atlas by what each piece draws. Helmet, wrist and
// boot pieces paint skin with the gear, so those parts take only the gear regions of their pieces.
constexpr uint32_t kDwarfBeard[] = {0, 7, 20, 22, 30, 32, 34, 36, 40, 50, 54, 61, 68, 69, 70};
constexpr uint32_t kDwarfWings[] = {1, 3, 10, 13, 14, 17, 24, 37, 41, 44, 51, 53, 71, 73};
constexpr uint32_t kDwarfHelmets[] = {2, 11, 15, 23, 31, 35, 38, 42, 52, 56, 58, 60, 67, 72};
constexpr uint32_t kDwarfWristGuards[] = {106, 107, 108, 160, 161, 176, 190, 218, 222, 239, 241, 330, 340,
                                          350, 384, 436, 447, 448, 469, 470, 509, 536, 539, 561, 564};
constexpr uint32_t kDwarfBelt[] = {120, 121, 122, 123, 124, 127, 154, 178, 244, 245, 257, 275, 300, 301, 302, 303,
                                   304, 320, 333, 334, 335, 336, 355, 356, 357, 358, 382, 383, 390, 422, 423, 424,
                                   425, 450, 451, 452, 453, 479, 480, 481, 482, 502, 517, 530, 554, 555, 556, 557,
                                   558, 559, 560};
constexpr uint32_t kDwarfBoots[] = {134, 135, 140, 141, 170, 179, 182, 202, 203, 206, 207, 209, 210, 246, 309, 315,
                                    316, 325, 338, 346, 348, 349, 368, 369, 374, 375, 380, 381, 393, 394, 400, 401,
                                    419, 420, 430, 431, 433, 460, 461, 484, 485, 500, 510, 533, 534};
constexpr uint16_t kDwarfGearRegions = R(1) | R(3) | R(6) | R(8);
constexpr CustomPart kDwarfParts[] = {
    {.key = "beard", .label = "Beard", .pieces = kDwarfBeard},
    {.key = "cape", .label = "Cape", .piece_range = {800, 899}},
    // Every gear texel belongs to a piece part; what is left, dark shading included, is skin.
    {.key = "skin", .label = "Skin", .regions = uint16_t((1u << kRegionCount) - 1), .reveals_unpainted = true},
    {.key = "helmet", .label = "Helmet", .pieces = kDwarfHelmets, .piece_regions = kDwarfGearRegions,
     .bare_underlay = "skin"},
    {.key = "eye", .label = "Eye", .pieces = kDwarfHelmets, .piece_regions = kDwarfGearRegions, .mark = PaintedMark::kSteady},
    {.key = "feather", .label = "Feather", .pieces = kDwarfWings},
    {.key = "wrist_guards", .label = "Wrist Guards", .pieces = kDwarfWristGuards, .piece_regions = kDwarfGearRegions},
    {.key = "belt", .label = "Belt", .pieces = kDwarfBelt},
    {.key = "boots", .label = "Boots", .pieces = kDwarfBoots, .piece_regions = kDwarfGearRegions},
    {.key = "hammers", .label = "Hammers", .weapon = 0},
};
constexpr uint32_t kSorceressHair[] = {8, 9, 18, 19, 29, 38, 39, 57, 58, 59, 100, 101, 102, 103, 104, 105, 106, 107,
                                       110, 111, 112, 113, 114, 115, 116, 117, 120, 121, 122, 123, 124, 125, 126,
                                       127, 130, 131, 132, 133, 134, 135, 136, 137, 140, 141, 142, 143, 144, 145,
                                       146, 147, 150, 151, 152, 153, 154, 155, 156, 157, 160, 161, 162, 163, 164,
                                       165, 170, 171, 172, 173, 180, 181, 182, 183, 184, 185, 190, 191, 192, 193,
                                       194, 195, 196, 197, 200, 201, 202, 203, 204, 205, 210, 211, 212};
constexpr uint32_t kSorceressHat[] = {66, 70, 71, 72, 73, 74, 75, 76, 77, 80, 81, 82, 83, 84, 85, 86, 87, 88,
                                           89, 91, 92, 93, 94, 95, 96, 97, 98, 99};
constexpr uint32_t kSorceressBodice[] = {1010, 1011, 1012, 1013, 1014, 1200, 1201, 1202, 1250, 1251, 1252};
constexpr uint32_t kSorceressBoots[] = {1420, 1421, 1422, 1423, 1424, 1425, 1426, 1427, 1428,
                                        1470, 1472, 1473, 1474, 1475, 1476, 1477, 1478, 1482};
// Heads, and the eye and mouth pieces drawn over them.
constexpr uint32_t kSorceressFace[] = {0,  1,  2,  3,  4,  5,  6,  7,  11, 12, 13, 14, 15, 16, 21, 22, 23, 24,
                                       25, 26, 30, 31, 32, 33, 34, 35, 40, 41, 42, 51, 52, 53, 60, 61, 62};
constexpr uint16_t kSorceressSkinRegions = R(7) | R(8);
constexpr CustomPart kSorceressParts[] = {
    {"hair", "Hair", R(4), true, kSorceressHair},
    {.key = "skin", .label = "Skin", .regions = kSorceressSkinRegions, .reveals_unpainted = true, .pieces = kSorceressFace,
     .piece_regions = kSorceressSkinRegions},
    {.key = "eye", .label = "Eyes", .pieces = kSorceressFace, .piece_regions = R(1) | R(2) | R(3) | R(5) | R(6),
     .mark = PaintedMark::kPainted},
    {.key = "dress", .label = "Dress", .reveals_unpainted = true, .piece_range = {500, 699}},
    {"bodice", "Bodice", 0, true, kSorceressBodice, R(1)},
    {.key = "lace", .label = "Bust Lace", .reveals_unpainted = true, .piece_regions = kSorceressSkinRegions,
     .mark = PaintedMark::kDyed, .piece_range = {1000, 1099}},
    {.key = "frills", .label = "Sleeve Frills", .reveals_unpainted = true, .piece_regions = kSorceressSkinRegions,
     .mark = PaintedMark::kDyed, .piece_range = {1200, 1299}},
    {.key = "hat", .label = "Hat", .regions = R(1), .reveals_unpainted = true, .pieces = kSorceressHat,
     .piece_regions = uint16_t(kAnyRegion & ~(R(2) | R(6)))},
    {"hat_band", "Hat Band", R(2) | R(6), false},
    {"boots", "Boots", 0, true, kSorceressBoots},
    {"accessories", "Accessories", R(3) | R(5), false},
    {.key = "staff", .label = "Staff", .weapon = 0},
};
// Wizard pieces of every pose of the in-game model, sorted from the atlas. Spell effects and the spellbook belong to no
// part, so no look changes them.
constexpr uint32_t kWizardHeads[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                     17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
constexpr uint32_t kWizardHands[] = {1504, 1507, 1517, 1520, 1523, 1526, 1528, 1529, 1532, 1547, 1550, 1554, 1555,
                                     1556, 1557, 1562, 1563, 1564, 1568, 1569, 1572, 1573, 1581, 1584, 1585, 1586,
                                     1588, 1800, 1801, 1802, 1803, 1900, 1901, 1902, 1903, 1904};
constexpr uint32_t kWizardSash[] = {1001, 1003, 1005, 1007, 1009, 1011, 1013, 1016, 1018, 1020, 1023, 1024, 1028, 1030, 1037};
// The robe's pieces: hood and collar, torso, sleeves and skirt. Their pale piping is tinted with the robe in every preset.
constexpr uint32_t kWizardRobe[] = {
    503,  504,  505,  506,  507,  508,  509,  510,  511,  512,  513,  514,  515,  516,  517,  518,  519,
    520,  521,  522,  523,  524,  525,  526,  527,  528,  529,  530,  531,  532,  533,  534,  535,  536,  537,  538,  539,
    540,  541,  542,  543,  544,  545,  546,  549,  550,  551,  552,  554,  555,  556,  557,  558,  559,  560,  561,  562,
    563,  564,  565,  566,  567,  568,  571,  572,  573,  574,  575,  576,  577,  578,  579,  580,  581,  1000, 1002, 1004,
    1006, 1008, 1010, 1012, 1014, 1015, 1017, 1019, 1021, 1022, 1025, 1029, 1031, 1032, 1033, 1035, 1036, 1500, 1501,
    1502, 1505, 1506, 1508, 1509, 1510, 1511, 1512, 1513, 1514, 1515, 1516, 1518, 1519, 1521, 1522, 1524, 1525,
    1527, 1530, 1531, 1533, 1534, 1535, 1536, 1537, 1539, 1541, 1542, 1545, 1546, 1549, 1552, 1553, 1558, 1559, 1565,
    1566, 1567, 1570, 1571, 1574, 1575, 1576, 1577, 1578, 1579, 1580, 1582, 1583, 1587};
constexpr uint32_t kWizardHood[] = {500, 501, 502};
// The silver pendant hanging from the robe: its own piece, the same colour in every preset.
constexpr uint32_t kWizardPendant[] = {1503};
// Leg pieces that draw trousers, whole or above a boot; the other pieces of 2000-2099 are feet.
constexpr uint32_t kWizardLegs[] = {2000, 2001, 2003, 2004, 2010, 2011, 2013, 2014, 2017, 2019, 2020, 2022, 2026, 2027, 2028, 2029,
                                    2031, 2032, 2034, 2036, 2038, 2040, 2041, 2044, 2046, 2047, 2051, 2052, 2055, 2057, 2058, 2060,
                                    2061, 2063, 2065, 2066, 2067, 2068, 2071, 2072, 2074, 2075, 2077, 2078, 2080, 2081, 2082, 2083};
constexpr uint16_t kWizardWhite = R(7);
constexpr uint16_t kWizardNotWhite = uint16_t(kAnyRegion & ~R(7));
constexpr CustomPart kWizardParts[] = {
    {.key = "robe", .label = "Robe", .reveals_unpainted = true, .pieces = kWizardRobe, .piece_range = {2500, 2699}},
    {.key = "hood", .label = "Hood", .pieces = kWizardHood},
    {.key = "pendant", .label = "Pendant", .pieces = kWizardPendant},
    {.key = "hair", .label = "Hair", .pieces = kWizardHeads, .piece_regions = kWizardWhite, .piece_range = {200, 299}},
    // The pale texels of a head that do not change with the hair (eyes, highlights) are the face's.
    {.key = "skin", .label = "Skin", .pieces = kWizardHeads, .piece_regions = kWizardNotWhite, .dyed_pieces = kWizardHeads},
    {.key = "gloves", .label = "Gloves", .pieces = kWizardHands},
    {.key = "belt", .label = "Belt", .pieces = kWizardSash},
    // On a leg piece the trousers' white wins over the boot below it; feet are boots whole.
    {.key = "trousers", .label = "Trousers", .pieces = kWizardLegs, .piece_regions = kWizardWhite},
    {.key = "boots", .label = "Boots", .piece_range = {2000, 2099}},
    {.key = "staff", .label = "Staff", .weapon = 0},
};

// Piece ids read from each class's in-game model (PLAYER/<CLASS>00.MBS), checked against the select model's pieces.

} // namespace

std::span<const WeaponTexture> WeaponTexturesOf(CharacterClass klass) {
    static constexpr WeaponTexture kArms[] = {{"ARMS01", 1}};
    static constexpr WeaponTexture kFighterArms[] = {{"ARMS01", 1}, {"SHIELD01", 2}};
    static constexpr WeaponTexture kDwarfArms[] = {{"ARMS01", 2}}; // its atlas continues as texture 1
    if (klass == CharacterClass::kFighter) return kFighterArms;
    if (klass == CharacterClass::kDwarf) return kDwarfArms;
    return kArms;
}

std::string WeaponAtlasName(CharacterClass klass, const WeaponTexture& weapon) {
    std::string name(ClassName(klass));
    for (char& c : name) c = char(std::toupper(static_cast<unsigned char>(c)));
    return "PLAYER/" + name + "00_" + std::string(weapon.name) + ".FTX";
}

const CustomPart* WeaponPartOf(CharacterClass klass, size_t weapon) {
    for (const CustomPart& part : CustomPartsOf(klass)) {
        if (part.weapon >= 0 && size_t(part.weapon) == weapon && !part.accent) return &part;
    }
    return nullptr;
}

const CustomPart* WeaponAccentOf(CharacterClass klass, size_t weapon) {
    for (const CustomPart& part : CustomPartsOf(klass)) {
        if (part.weapon >= 0 && size_t(part.weapon) == weapon && part.accent) return &part;
    }
    return nullptr;
}

const CustomPart* WeaponPartOfTexture(CharacterClass klass, uint8_t texture) {
    const std::span<const WeaponTexture> weapons = WeaponTexturesOf(klass);
    for (size_t i = 0; i < weapons.size(); ++i) {
        if (weapons[i].texture == texture) return WeaponPartOf(klass, i);
    }
    return nullptr;
}

Rgb ColorFromHsv(float hue, float saturation, float value) {
    const float h = (hue - std::floor(hue)) * 6.0f;
    const int sector = int(h) % 6;
    const float f = h - std::floor(h);
    const float p = value * (1.0f - saturation), q = value * (1.0f - saturation * f), t = value * (1.0f - saturation * (1.0f - f));
    float r = value, g = t, b = p;
    switch (sector) {
    case 1: r = q, g = value, b = p; break;
    case 2: r = p, g = value, b = t; break;
    case 3: r = p, g = q, b = value; break;
    case 4: r = t, g = p, b = value; break;
    case 5: r = value, g = p, b = q; break;
    default: break;
    }
    const auto byte = [](float c) { return uint8_t(std::clamp(std::lround(c * 255.0f), 0L, 255L)); };
    return Rgb{byte(r), byte(g), byte(b)};
}

std::string PartLabel(CharacterClass klass, std::string_view key) {
    const CustomPart* known = FindCustomPart(klass, key);
    return std::string(known ? known->label : key);
}

uint16_t PieceBitOf(CharacterClass klass, const CustomPart& part) {
    if (!part.HasPieces()) return 0;
    size_t bit = 0;
    for (const CustomPart& candidate : CustomPartsOf(klass)) {
        if (!candidate.HasPieces()) continue;
        if (&candidate == &part || candidate.key == part.key) return bit < kPieceBits ? uint16_t(1u << bit) : 0;
        ++bit;
    }
    return 0;
}

std::span<const CustomPart> CustomPartsOf(CharacterClass klass) {
    switch (klass) {
    case CharacterClass::kFighter: return kFighterParts;
    case CharacterClass::kAmazon: return kAmazonParts;
    case CharacterClass::kElf: return kElfParts;
    case CharacterClass::kDwarf: return kDwarfParts;
    case CharacterClass::kSorceress: return kSorceressParts;
    case CharacterClass::kWizard: return kWizardParts;
    }
    return {};
}

std::span<const BodySurface> BodySurfacesOf(CharacterClass klass) {
    if (klass == CharacterClass::kDwarf) return kDwarfSurfaces;
    return kOneSurface;
}

const CustomPart* FindCustomPart(CharacterClass klass, std::string_view key) {
    for (const CustomPart& part : CustomPartsOf(klass)) {
        if (part.key == key) return &part;
    }
    return nullptr;
}

const CustomPart* PartOfRegion(CharacterClass klass, uint8_t region) {
    if (region == 0 || region > kRegionCount) return nullptr;
    for (const CustomPart& part : CustomPartsOf(klass)) {
        if (part.regions >> (region - 1) & 1) return &part;
    }
    return nullptr;
}

const PartLook* CustomLook::Find(std::string_view key) const {
    for (const PartLook& part : parts) {
        if (part.key == key) return &part;
    }
    return nullptr;
}

PartLook& CustomLook::Edit(std::string_view key) {
    for (PartLook& part : parts) {
        if (part.key == key) return part;
    }
    parts.push_back(PartLook{std::string(key), std::nullopt, false});
    return parts.back();
}

void CustomLook::Tidy() {
    parts.erase(std::remove_if(parts.begin(), parts.end(), [](const PartLook& p) { return !p.color && !p.hidden; }),
                parts.end());
    std::sort(parts.begin(), parts.end(), [](const PartLook& a, const PartLook& b) { return a.key < b.key; });
}

size_t CustomLook::OverrideCount() const {
    return size_t(std::count_if(parts.begin(), parts.end(), [](const PartLook& p) { return p.color || p.hidden; }));
}

void CustomLook::SetPartColor(std::string_view key, Rgb color) {
    Edit(key).color = color;
}

void CustomLook::SetPartHidden(std::string_view key, bool hidden) {
    PartLook& part = Edit(key);
    part.hidden = hidden;
    if (!part.color && !part.hidden) ResetPart(key);
}

void CustomLook::ResetPart(std::string_view key) {
    parts.erase(std::remove_if(parts.begin(), parts.end(), [&](const PartLook& p) { return p.key == key; }), parts.end());
}

void CustomLook::ResetOverrides() {
    parts.clear();
}

std::string CustomKey::Text() const {
    return title_id + "|" + save_dir + "|" + std::to_string(data_slot) + "|" + std::to_string(character_slot) + "|" +
           std::to_string(int(klass));
}

std::filesystem::path CustomStorePath(const std::filesystem::path& user_dir) {
    return user_dir / "character_appearance.dat";
}

std::vector<uint8_t> CustomStore::Serialize() const {
    std::vector<uint8_t> out;
    Put32(out, kMagic);
    Put32(out, kVersion);
    Put32(out, uint32_t(entries_.size()));
    for (const auto& [key, look] : entries_) {
        PutText(out, key.Text());
        out.push_back(look.enabled ? 1 : 0);
        out.push_back(look.base);
        out.push_back(uint8_t(look.parts.size()));
        for (const PartLook& part : look.parts) {
            PutText(out, part.key);
            out.push_back(uint8_t((part.color ? 1 : 0) | (part.hidden ? 2 : 0)));
            const Rgb color = part.color.value_or(Rgb{});
            out.insert(out.end(), {color.r, color.g, color.b});
        }
    }
    return out;
}

bool CustomStore::Parse(std::span<const uint8_t> bytes) {
    Cursor in(bytes);
    uint32_t magic = 0, version = 0, count = 0;
    if (!in.U32(magic) || magic != kMagic || !in.U32(version) || !in.U32(count)) {
        error_ = "unrecognized appearance data";
        return false;
    }
    if (version != kVersion) {
        error_ = "customization file version " + std::to_string(version) + " is not supported";
        return false;
    }
    std::vector<std::pair<CustomKey, CustomLook>> entries;
    for (uint32_t i = 0; i < count; ++i) {
        std::string key_text;
        uint8_t enabled = 0, base = 0, parts = 0;
        if (!in.Text(key_text) || !in.U8(enabled) || !in.U8(base) || !in.U8(parts)) {
            error_ = "customization file is truncated";
            return false;
        }
        const std::optional<CustomKey> key = ParseKey(key_text);
        if (!key || base >= kNativeColorCount) {
            error_ = "customization file has a malformed entry";
            return false;
        }
        CustomLook look{enabled != 0, base, {}};
        for (uint8_t p = 0; p < parts; ++p) {
            PartLook part;
            uint8_t flags = 0;
            Rgb color;
            if (!in.Text(part.key) || !in.U8(flags) || !in.U8(color.r) || !in.U8(color.g) || !in.U8(color.b)) {
                error_ = "customization file is truncated";
                return false;
            }
            if (flags & 1) part.color = color;
            part.hidden = (flags & 2) != 0;
            if (part.key == "headgear") continue; // the separate Headgear switch no longer exists
            look.parts.push_back(std::move(part));
        }
        entries.emplace_back(*key, std::move(look));
    }
    if (!in.done()) {
        error_ = "customization file has trailing data";
        return false;
    }
    entries_ = std::move(entries);
    error_.clear();
    return true;
}

bool CustomStore::Load() {
    entries_.clear();
    error_.clear();
    std::error_code ec;
    if (!std::filesystem::exists(path_, ec)) return true;
    std::ifstream in(path_, std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return Parse(bytes);
}

bool CustomStore::Save() const {
    if (!error_.empty()) return false; // never replace a file this version could not read
    std::error_code ec;
    if (entries_.empty()) {
        std::filesystem::remove(path_, ec);
        return !std::filesystem::exists(path_, ec);
    }
    return WriteAtomic(path_, Serialize());
}

std::optional<CustomLook> CustomStore::Get(const CustomKey& key) const {
    for (const auto& [stored, look] : entries_) {
        if (stored == key) return look;
    }
    return std::nullopt;
}

void CustomStore::Set(const CustomKey& key, const CustomLook& look) {
    CustomLook tidy = look;
    tidy.Tidy();
    const bool empty = !tidy.enabled && tidy.parts.empty();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->first == key) {
            if (empty) entries_.erase(it);
            else it->second = tidy;
            return;
        }
    }
    if (!empty) entries_.emplace_back(key, tidy);
}

std::vector<std::string> DescribeCustomChanges(const CustomStore& before, const CustomStore& after) {
    std::vector<std::string> lines;
    for (const auto& [key, look] : after.entries()) {
        const std::optional<CustomLook> old = before.Get(key);
        if (old && *old == look) continue;
        lines.push_back(std::string(look.enabled ? "Custom " : "Native ") + std::string(ClassName(key.klass)) + " DATA " +
                        std::to_string(key.data_slot + 1) + " slot " + std::to_string(key.character_slot + 1) +
                        " base colour " + std::to_string(look.base));
        for (const PartLook& part : look.parts) {
            const std::string label = PartLabel(key.klass, part.key);
            if (part.color) {
                char hex[8];
                std::snprintf(hex, sizeof(hex), "%02X%02X%02X", part.color->r, part.color->g, part.color->b);
                lines.push_back("Custom part " + label + " colour #" + hex);
            }
            if (part.hidden) lines.push_back("Custom part " + label + " hidden");
        }
    }
    for (const auto& [key, look] : before.entries()) {
        if (!after.Get(key)) lines.push_back("Custom removed " + std::string(ClassName(key.klass)) + " DATA " +
                                             std::to_string(key.data_slot + 1) + " slot " + std::to_string(key.character_slot + 1));
    }
    return lines;
}

std::vector<RegionMap> BuildRegionMaps(std::span<const SurfaceTexels> surfaces, const Mbs& model) {
    std::vector<RegionMap> maps;
    std::vector<std::vector<uint8_t>> covered;
    for (const SurfaceTexels& surface : surfaces) {
        maps.push_back(RegionMap{surface.width, surface.height,
                                 std::vector<uint8_t>(size_t(surface.width) * size_t(surface.height), 0), {}});
        std::vector<uint8_t>& mask = covered.emplace_back(maps.back().region.size(), 0);
        for (const MbsLayer& layer : model.layers) {
            if (layer.texture != surface.texture) continue;
            ForEachTexel(model.sources[layer.source], surface.width, surface.height,
                         [&](int x, int y) { mask[size_t(y) * size_t(surface.width) + size_t(x)] = 1; });
        }
    }
    constexpr int kDims = 15;
    using Feature = std::array<float, kDims>;
    // How a texel's hue changes from preset to preset: two opponent chroma axes divided by intensity, so painted
    // shading falls out, and a light brightness term.
    const auto feature = [&](const SurfaceTexels& surface, int x, int y, bool& opaque) {
        Feature f{};
        int alpha = 0;
        for (size_t p = 0; p < surface.presets.size() && p < 5; ++p) {
            const std::array<uint8_t, 4> t = surface.presets[p](x, y);
            alpha = std::max(alpha, int(t[3]));
            const float sum = float(t[0]) + float(t[1]) + float(t[2]) + 60.0f;
            f[p * 3] = 0.15f * (t[0] + t[1] + t[2]) / 765.0f;
            f[p * 3 + 1] = 2.0f * (float(t[0]) - float(t[1])) / sum;
            f[p * 3 + 2] = 2.0f * ((float(t[0]) + float(t[1])) * 0.5f - float(t[2])) / sum;
        }
        opaque = alpha >= 160;
        return f;
    };
    const auto distance = [](const Feature& a, const Feature& b) {
        float d = 0.0f;
        for (int i = 0; i < kDims; ++i) d += (a[i] - b[i]) * (a[i] - b[i]);
        return d;
    };
    std::vector<Feature> samples;
    for (size_t i = 0; i < surfaces.size(); ++i) {
        for (int y = 0; y < surfaces[i].height; y += 4) {
            for (int x = 0; x < surfaces[i].width; x += 4) {
                if (!covered[i][size_t(y) * size_t(surfaces[i].width) + size_t(x)]) continue;
                bool opaque = false;
                const Feature f = feature(surfaces[i], x, y, opaque);
                if (opaque) samples.push_back(f);
            }
        }
    }
    if (samples.empty()) return maps;

    std::vector<Feature> centers;
    Feature mean{};
    for (const Feature& s : samples) {
        for (int i = 0; i < kDims; ++i) mean[i] += s[i] / float(samples.size());
    }
    centers.push_back(mean);
    while (int(centers.size()) < kRegionCount) { // farthest-point seeding: deterministic
        size_t best = 0;
        float best_distance = -1.0f;
        for (size_t s = 0; s < samples.size(); ++s) {
            float nearest = 1e30f;
            for (const Feature& c : centers) nearest = std::min(nearest, distance(samples[s], c));
            if (nearest > best_distance) {
                best_distance = nearest;
                best = s;
            }
        }
        centers.push_back(samples[best]);
    }
    const auto nearest_center = [&](const Feature& f) {
        int best = 0;
        float best_distance = 1e30f;
        for (int c = 0; c < kRegionCount; ++c) {
            const float d = distance(f, centers[size_t(c)]);
            if (d < best_distance) {
                best_distance = d;
                best = c;
            }
        }
        return best;
    };
    for (int iteration = 0; iteration < 12; ++iteration) {
        std::vector<Feature> sums(size_t(kRegionCount), Feature{});
        std::vector<size_t> counts(size_t(kRegionCount), 0);
        for (const Feature& s : samples) {
            const int c = nearest_center(s);
            for (int i = 0; i < kDims; ++i) sums[size_t(c)][i] += s[i];
            ++counts[size_t(c)];
        }
        for (int c = 0; c < kRegionCount; ++c) {
            if (!counts[size_t(c)]) continue;
            for (int i = 0; i < kDims; ++i) centers[size_t(c)][i] = sums[size_t(c)][i] / float(counts[size_t(c)]);
        }
    }
    // Regions are numbered by the base preset's brightness, so a class's part table names stable ids.
    std::array<int, kRegionCount> order{};
    for (int c = 0; c < kRegionCount; ++c) order[size_t(c)] = c;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return centers[size_t(a)][0] < centers[size_t(b)][0]; });
    std::array<uint8_t, kRegionCount> rank{};
    for (int r = 0; r < kRegionCount; ++r) rank[size_t(order[size_t(r)])] = uint8_t(r + 1);
    for (size_t i = 0; i < surfaces.size(); ++i) {
        const SurfaceTexels& surface = surfaces[i];
        for (int y = 0; y < surface.height; ++y) {
            for (int x = 0; x < surface.width; ++x) {
                const size_t at = size_t(y) * size_t(surface.width) + size_t(x);
                if (!covered[i][at]) continue;
                int alpha = 0;
                for (size_t p = 0; p < surface.presets.size() && p < 5; ++p) alpha = std::max(alpha, int(surface.presets[p](x, y)[3]));
                if (!alpha) continue;
                bool opaque = false;
                maps[i].region[at] = rank[size_t(nearest_center(feature(surface, x, y, opaque)))];
            }
        }
    }
    return maps;
}

namespace {

// A quad whose texture coordinates leave the texture samples no art of its own (one Sorceress layer spans a million rows).
bool SamplesInsideTexture(const MbsQuad& quad) {
    return std::all_of(quad.corners.begin(), quad.corners.end(),
                       [](const MbsPoint& p) { return p.x >= -0.01f && p.x <= 1.01f && p.y >= -0.01f && p.y <= 1.01f; });
}

// Keeps a part's bit only on the texels its region filter and painted mark allow. A mark keeps its strong texels and the
// weaker ones that touch them, so the soft edge of a line or a frill goes with it.
// The mean chroma, per preset, of the texels in `regions`: the tone kSkinTone texels must follow.
std::vector<std::pair<float, float>> RegionTone(const RegionMap& map, uint16_t regions, const SurfaceTexels& presets) {
    std::vector<std::pair<double, double>> sums(presets.presets.size());
    double weight = 0.0;
    for (int y = 0; y < map.height; y += 2) {
        for (int x = 0; x < map.width; x += 2) {
            const uint8_t region = map.At(x, y);
            if (!region || !(regions >> (region - 1) & 1)) continue;
            for (size_t p = 0; p < sums.size(); ++p) {
                const auto [red, blue] = TexelChroma(presets.presets[p](x, y));
                sums[p].first += red;
                sums[p].second += blue;
            }
            weight += 1.0;
        }
    }
    std::vector<std::pair<float, float>> tone;
    for (const auto& [red, blue] : sums) tone.emplace_back(float(red / std::max(weight, 1.0)), float(blue / std::max(weight, 1.0)));
    return tone;
}

// Keeps, of the patches of kept texels, those `keep` accepts.
template <typename Keep>
void KeepPatches(const RegionMap& map, std::vector<uint8_t>& level, Keep keep) {
    std::vector<uint8_t> seen(level.size(), 0);
    for (size_t start = 0; start < level.size(); ++start) {
        if (level[start] != 2 || seen[start]) continue;
        std::vector<size_t> patch{start}, stack{start};
        seen[start] = 1;
        while (!stack.empty()) {
            const size_t at = stack.back();
            stack.pop_back();
            const int x = int(at % size_t(map.width)), y = int(at / size_t(map.width));
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (x + dx < 0 || y + dy < 0 || x + dx >= map.width || y + dy >= map.height) continue;
                    const size_t near = size_t(y + dy) * size_t(map.width) + size_t(x + dx);
                    if (level[near] != 2 || seen[near]) continue;
                    seen[near] = 1;
                    patch.push_back(near);
                    stack.push_back(near);
                }
            }
        }
        if (!keep(patch)) {
            for (const size_t at : patch) level[at] = 0;
        }
    }
}

size_t LitTexels(const TexelReader& preset, const RegionMap& map, std::span<const size_t> patch) {
    return size_t(std::count_if(patch.begin(), patch.end(), [&](size_t at) {
        const std::array<uint8_t, 4> t = preset(int(at % size_t(map.width)), int(at / size_t(map.width)));
        return Luma(t[0], t[1], t[2]) >= 120.0f;
    }));
}

// Next to a texel the figure leaves transparent: the piece's painted outline.
bool NearOutline(const TexelReader& preset, int x, int y, const RegionMap& map) {
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (x + dx < 0 || y + dy < 0 || x + dx >= map.width || y + dy >= map.height) return true;
            if (preset(x + dx, y + dy)[3] < 64) return true;
        }
    }
    return false;
}

// How far the presets paint a texel from their mean: per channel, and in hundredths of its brightness.
struct Drift {
    float absolute = 0.0f;
    float relative = 0.0f;
};

Drift PresetDrift(std::span<const std::array<uint8_t, 4>> presets) {
    float mean[3] = {};
    for (const std::array<uint8_t, 4>& t : presets) {
        for (size_t c = 0; c < 3; ++c) mean[c] += float(t[c]) / float(presets.size());
    }
    Drift drift;
    for (const std::array<uint8_t, 4>& t : presets) {
        for (size_t c = 0; c < 3; ++c) drift.absolute = std::max(drift.absolute, std::fabs(float(t[c]) - mean[c]));
    }
    drift.relative = drift.absolute * 100.0f / ((mean[0] + mean[1] + mean[2]) / 3.0f + 40.0f);
    return drift;
}

float ShiftDistance(std::span<const std::array<uint8_t, 4>> presets, std::span<const std::pair<float, float>> reference);

// How far a material's colour moves from preset 0 through the others (hundredths).
float MaterialRange(std::span<const std::pair<float, float>> material) {
    float range = 0.0f;
    for (const auto& [red, blue] : material) range = std::max(range, std::fabs(red - material[0].first) + std::fabs(blue - material[0].second));
    return range * 100.0f;
}

// The texels of the segment a-b, stepping one axis at a time so that a 4-connected fill cannot slip through a corner.
template <typename Visit>
void ForEachLineTexel(HornPoint a, HornPoint b, Visit visit) {
    int x = a.x, y = a.y;
    const int dx = std::abs(b.x - a.x), dy = std::abs(b.y - a.y), sx = a.x < b.x ? 1 : -1, sy = a.y < b.y ? 1 : -1;
    int err = dx - dy;
    visit(x, y);
    while (x != b.x || y != b.y) {
        const int twice = 2 * err;
        if (twice > -dy) {
            err -= dy;
            x += sx;
        } else {
            err += dx;
            y += sy;
        }
        visit(x, y);
    }
}

// The horns of every helmet sprite the cut table knows. A horn is what its seed reaches through drawn texels of the horn
// pieces without crossing its cut; the cut's own texels next to it are the horn's edge. A sprite whose seed is not drawn
// or whose fill would take most of it is not the art the table was made for, and keeps its horns with the helmet.
void HornTexels(uint16_t bit, const SurfaceTexels& presets, const RegionMap& map, std::vector<uint8_t>& level) {
    const auto drawn = [&](int x, int y) {
        if (!(map.pieces[size_t(y) * size_t(map.width) + size_t(x)] & bit)) return false;
        return std::any_of(presets.presets.begin(), presets.presets.end(), [&](const TexelReader& preset) { return preset(x, y)[3] != 0; });
    };
    for (const HornSprite& sprite : FighterHornSprites()) {
        if (sprite.x0 < 0 || sprite.y0 < 0 || sprite.x1 > map.width || sprite.y1 > map.height) continue;
        const int w = sprite.x1 - sprite.x0, h = sprite.y1 - sprite.y0;
        const auto local = [&](int x, int y) { return size_t(y) * size_t(w) + size_t(x); };
        std::vector<uint8_t> inside(size_t(w) * size_t(h), 0), horn(inside.size(), 0);
        size_t drawn_texels = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                inside[local(x, y)] = drawn(sprite.x0 + x, sprite.y0 + y);
                drawn_texels += inside[local(x, y)];
            }
        }
        bool fits = true;
        for (const HornCut& cut : sprite.horns) {
            std::vector<uint8_t> wall(inside.size(), 0), reached(inside.size(), 0);
            for (size_t i = 0; i + 1 < cut.cut.size(); ++i) {
                ForEachLineTexel(cut.cut[i], cut.cut[i + 1], [&](int x, int y) {
                    if (x >= 0 && y >= 0 && x < w && y < h) wall[local(x, y)] = 1;
                });
            }
            if (cut.seed.x < 0 || cut.seed.y < 0 || cut.seed.x >= w || cut.seed.y >= h || !inside[local(cut.seed.x, cut.seed.y)]) {
                fits = false;
                break;
            }
            std::vector<HornPoint> stack{cut.seed};
            reached[local(cut.seed.x, cut.seed.y)] = 1;
            size_t count = 0;
            constexpr int kStepX[4] = {1, -1, 0, 0}, kStepY[4] = {0, 0, 1, -1};
            while (!stack.empty()) {
                const HornPoint p = stack.back();
                stack.pop_back();
                ++count;
                for (int d = 0; d < 4; ++d) {
                    const int x = p.x + kStepX[d], y = p.y + kStepY[d];
                    if (x < 0 || y < 0 || x >= w || y >= h) continue;
                    const size_t at = local(x, y);
                    if (reached[at] || wall[at] || !inside[at]) continue;
                    reached[at] = 1;
                    stack.push_back({int16_t(x), int16_t(y)});
                }
            }
            if (count * 2 > drawn_texels) {
                fits = false;
                break;
            }
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const size_t at = local(x, y);
                    if (reached[at]) {
                        horn[at] = 1;
                        continue;
                    }
                    if (!wall[at] || !inside[at]) continue;
                    for (int d = 0; d < 4 && !horn[at]; ++d) {
                        const int nx = x + kStepX[d], ny = y + kStepY[d];
                        if (nx >= 0 && ny >= 0 && nx < w && ny < h && reached[local(nx, ny)]) horn[at] = 1;
                    }
                }
            }
        }
        if (!fits) continue;
        // A pocket a cut closes against the sprite's edge is the horn's rim: too small to be any of the helmet.
        constexpr size_t kPocket = 64;
        std::vector<uint8_t> seen(inside.size(), 0);
        for (size_t start = 0; start < inside.size(); ++start) {
            if (!inside[start] || horn[start] || seen[start]) continue;
            std::vector<size_t> patch{start}, stack{start};
            seen[start] = 1;
            bool touches_horn = false;
            while (!stack.empty()) {
                const size_t at = stack.back();
                stack.pop_back();
                const int x = int(at % size_t(w)), y = int(at / size_t(w));
                for (int d = 0; d < 4; ++d) {
                    const int nx = x + (d == 0) - (d == 1), ny = y + (d == 2) - (d == 3);
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    const size_t next = local(nx, ny);
                    touches_horn |= horn[next] != 0;
                    if (!inside[next] || horn[next] || seen[next]) continue;
                    seen[next] = 1;
                    patch.push_back(next);
                    stack.push_back(next);
                }
            }
            if (patch.size() < kPocket && touches_horn) {
                for (const size_t at : patch) horn[at] = 1;
            }
        }
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (horn[local(x, y)]) level[size_t(sprite.y0 + y) * size_t(map.width) + size_t(sprite.x0 + x)] = 2;
            }
        }
    }
}

void FilterPieceTexels(const CustomPart& part, uint16_t bit, const SurfaceTexels* presets, RegionMap& map) {
    const size_t count = size_t(map.width) * size_t(map.height);
    std::vector<uint8_t> level(count, 0); // 0 dropped, 1 edge, 2 kept
    if (part.mark == PaintedMark::kHorn) {
        if (presets) HornTexels(bit, *presets, map, level);
        for (size_t at = 0; at < count; ++at) {
            if (level[at] != 2) map.pieces[at] &= uint16_t(~bit);
        }
        return;
    }
    std::vector<float> drifts(part.mark == PaintedMark::kUndyed ? count : 0, 0.0f);
    std::vector<std::pair<float, float>> tone;
    if (part.mark == PaintedMark::kSkinTone && presets && presets->presets.size() >= 2) {
        if (part.tone.empty()) tone = RegionTone(map, part.regions, *presets);
        for (const Rgb& c : part.tone) tone.push_back(TexelChroma({c.r, c.g, c.b, 255}));
    }
    // Dye is told from the skin it is sewn onto, which some presets tint faintly too.
    if (part.mark == PaintedMark::kDyed && presets && presets->presets.size() >= 2) tone = RegionTone(map, part.piece_regions, *presets);
    for (size_t at = 0; at < count; ++at) {
        if (!(map.pieces[at] & bit)) continue;
        const uint8_t region = map.region[at];
        if (part.piece_regions && !(part.piece_regions >> (region ? region - 1 : kRegionCount) & 1)) continue;
        if (part.mark == PaintedMark::kNone || part.mark == PaintedMark::kPainted) {
            level[at] = 2;
            continue;
        }
        if (!presets || presets->presets.size() < 2) continue;
        const int x = int(at % size_t(map.width)), y = int(at / size_t(map.width));
        if (part.mark == PaintedMark::kTattooInk) {
            const std::array<uint8_t, 4> p0 = presets->presets[0](x, y), p1 = presets->presets[1](x, y);
            level[at] = TattooInk(p0, p1, kTattooInkCore) ? 2 : TattooInk(p0, p1, kTattooInkEdge) ? 1 : 0;
            continue;
        }
        std::array<std::array<uint8_t, 4>, kNativeColorCount> texels{};
        const size_t known = std::min<size_t>(presets->presets.size(), texels.size());
        for (size_t p = 0; p < known; ++p) texels[p] = presets->presets[p](x, y);
        const std::span<const std::array<uint8_t, 4>> seen(texels.data(), known);
        if (part.mark == PaintedMark::kSteady) {
            if (known < texels.size() || NearOutline(presets->presets[0], x, y, map)) continue;
            int spread = 0;
            for (const std::array<uint8_t, 4>& t : seen) {
                for (size_t c = 0; c < 3; ++c) spread = std::max(spread, std::abs(int(t[c]) - int(seen[0][c])));
            }
            level[at] = spread <= kSteadyCore ? 2 : spread <= kSteadyEdge ? 1 : 0;
            continue;
        }
        if (part.mark == PaintedMark::kUndyed) {
            if (known < texels.size()) continue;
            const Drift drift = PresetDrift(seen);
            const std::array<uint8_t, 4>& first = seen[0];
            const float luma = Luma(first[0], first[1], first[2]);
            const bool lit = luma >= kUndyedDark || (drift.absolute <= kUndyedExact && luma >= kUndyedBlack);
            const bool core = drift.relative <= kUndyedCore && lit && !NearOutline(presets->presets[0], x, y, map);
            level[at] = core ? 2 : drift.relative <= kUndyedEdge ? 1 : 0;
            drifts[at] = drift.absolute;
            continue;
        }
        if (part.mark == PaintedMark::kSkinTone) {
            float distance = 1e9f, shift = 1e9f;
            for (size_t k = 0; k + size_t(kNativeColorCount) <= tone.size(); k += size_t(kNativeColorCount)) {
                const std::span<const std::pair<float, float>> material = std::span(tone).subspan(k, size_t(kNativeColorCount));
                distance = std::min(distance, ToneDistance(seen, material));
                if (!part.tone.empty() && MaterialRange(material) >= kDistinctRange) shift = std::min(shift, ShiftDistance(seen, material));
            }
            // A material named by its colours, one the presets repaint strongly, keeps its shadowed stretches: darker, but
            // shifting through the presets alike. A mildly tinted one would take every similar highlight.
            const bool follows = shift <= kToneClaimLimit;
            const bool core = distance <= kSkinToneCore || (follows && distance <= kSkinToneEdge);
            level[at] = core ? 2 : distance <= kSkinToneEdge || follows ? 1 : 0;
            continue;
        }
        if (!tone.empty() && ToneDistance(seen, tone) <= kSkinToneCore) continue;
        level[at] = Dyed(seen, kDyedCoreShift, kDyedCoreSaturation) ? 2 : Dyed(seen, kDyedEdgeShift, kDyedEdgeSaturation) ? 1 : 0;
    }
    const int joins = part.tone.empty() ? 2 : 8;
    for (int pass = 0; pass < joins && part.mark != PaintedMark::kNone; ++pass) {
        std::vector<size_t> joined;
        for (size_t at = 0; at < count; ++at) {
            if (level[at] != 1) continue;
            const int x = int(at % size_t(map.width)), y = int(at / size_t(map.width));
            for (int dy = -1; dy <= 1 && level[at] == 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (x + dx < 0 || y + dy < 0 || x + dx >= map.width || y + dy >= map.height) continue;
                    if (level[size_t(y + dy) * size_t(map.width) + size_t(x + dx)] == 2) {
                        joined.push_back(at);
                        break;
                    }
                }
            }
        }
        for (const size_t at : joined) level[at] = 2;
    }
    // A steady part keeps the patches that hold a lit texel, as an eye's white does: the dark outlines of the gear around
    // it are painted alike in every preset too. An undyed one also keeps the dark patches every preset paints the same.
    constexpr size_t kLitTexels = 4;
    if (part.mark == PaintedMark::kSteady && presets && !presets->presets.empty()) {
        KeepPatches(map, level, [&](std::span<const size_t> patch) { return LitTexels(presets->presets[0], map, patch) >= kLitTexels; });
    }
    if (part.mark == PaintedMark::kUndyed && presets && !presets->presets.empty()) {
        KeepPatches(map, level, [&](std::span<const size_t> patch) {
            double drift = 0.0;
            for (const size_t at : patch) drift += drifts[at];
            const bool same = drift <= double(kUndyedExact) * double(patch.size());
            return patch.size() >= kUndyedPatch && (same || LitTexels(presets->presets[0], map, patch) >= kLitTexels);
        });
    }
    for (size_t at = 0; at < count; ++at) {
        if (level[at] != 2) map.pieces[at] &= uint16_t(~bit);
    }
}

// A texel two parts claim goes to the narrower one: the horns, a painted mark, then a region-limited part, then a whole-piece part.
void ResolvePieceOverlaps(CharacterClass klass, RegionMap& map) {
    struct Claim {
        uint16_t bit;
        int rank;
    };
    std::vector<Claim> claims;
    for (const CustomPart& part : CustomPartsOf(klass)) {
        if (const uint16_t bit = PieceBitOf(klass, part)) {
            claims.push_back({bit, part.mark == PaintedMark::kHorn ? 3 : part.mark != PaintedMark::kNone ? 2 : part.piece_regions ? 1 : 0});
        }
    }
    for (uint16_t& v : map.pieces) {
        const uint16_t owned = v;
        if (!(owned & (owned - 1))) continue;
        const Claim* best = nullptr;
        for (const Claim& claim : claims) {
            if ((owned & claim.bit) && (!best || claim.rank > best->rank)) best = &claim;
        }
        v = best->bit;
    }
}

} // namespace

bool TattooInk(const std::array<uint8_t, 4>& preset0, const std::array<uint8_t, 4>& preset1, int threshold) {
    if (!preset0[3] || !preset1[3]) return false;
    const int shift = (int(preset1[2]) - int(preset1[1])) - (int(preset0[2]) - int(preset0[1]));
    return shift >= threshold;
}

std::pair<float, float> TexelChroma(const std::array<uint8_t, 4>& t) {
    const float sum = float(t[0]) + float(t[1]) + float(t[2]) + 60.0f;
    return {2.0f * (float(t[0]) - float(t[1])) / sum, 2.0f * ((float(t[0]) + float(t[1])) * 0.5f - float(t[2])) / sum};
}

bool Dyed(std::span<const std::array<uint8_t, 4>> presets, int min_shift, int max_saturation) {
    if (presets.size() < 2 || !presets[0][3]) return false;
    const std::array<uint8_t, 4>& base = presets[0];
    const int high = std::max({base[0], base[1], base[2]}), low = std::min({base[0], base[1], base[2]});
    if ((high - low) * 100 > max_saturation * std::max(high, 1)) return false;
    const auto [red0, blue0] = TexelChroma(base);
    float shift = 0.0f;
    for (size_t p = 1; p < presets.size(); ++p) {
        const auto [red, blue] = TexelChroma(presets[p]);
        shift = std::max(shift, std::fabs(red - red0) + std::fabs(blue - blue0));
    }
    return shift * 100.0f >= float(min_shift);
}

float ToneDistance(std::span<const std::array<uint8_t, 4>> presets, std::span<const std::pair<float, float>> reference) {
    float worst = 0.0f;
    for (size_t p = 0; p < presets.size() && p < reference.size(); ++p) {
        if (!presets[p][3]) return 1e9f;
        const auto [red, blue] = TexelChroma(presets[p]);
        worst = std::max(worst, std::fabs(red - reference[p].first) + std::fabs(blue - reference[p].second));
    }
    return worst * 100.0f;
}

namespace {

using Tone = std::vector<std::pair<float, float>>;

// The mean chroma, per preset, of each colour region among the texels `owned` accepts; regions holding less than
// `percent` of them are left out.
template <typename Owned>
std::vector<Tone> TonesOf(const RegionMap& map, const SurfaceTexels& presets, int percent, Owned owned) {
    std::vector<std::vector<std::pair<double, double>>> sums(kRegionCount + 1,
                                                             std::vector<std::pair<double, double>>(presets.presets.size()));
    std::vector<double> weight(kRegionCount + 1, 0.0);
    double total = 0.0;
    for (int y = 0; y < map.height; y += 2) {
        for (int x = 0; x < map.width; x += 2) {
            const size_t at = size_t(y) * size_t(map.width) + size_t(x);
            if (!owned(at) || map.region[at] > kRegionCount) continue;
            for (size_t p = 0; p < presets.presets.size(); ++p) {
                const auto [red, blue] = TexelChroma(presets.presets[p](x, y));
                sums[map.region[at]][p].first += red;
                sums[map.region[at]][p].second += blue;
            }
            weight[map.region[at]] += 1.0;
            total += 1.0;
        }
    }
    std::vector<Tone> tones;
    for (size_t r = 0; r < sums.size(); ++r) {
        if (weight[r] < 16.0 || weight[r] * 100.0 < total * percent) continue;
        Tone& tone = tones.emplace_back();
        for (const auto& [red, blue] : sums[r]) tone.emplace_back(float(red / weight[r]), float(blue / weight[r]));
    }
    return tones;
}

// How far a texel's change of colour from preset 0 to each other preset strays from a reference's (hundredths): texels
// of one material shift alike whatever their shade.
float ShiftDistance(std::span<const std::array<uint8_t, 4>> presets, std::span<const std::pair<float, float>> reference) {
    if (presets.empty() || reference.size() < presets.size()) return 1e9f;
    const auto [red0, blue0] = TexelChroma(presets[0]);
    float worst = 0.0f;
    for (size_t p = 1; p < presets.size(); ++p) {
        const auto [red, blue] = TexelChroma(presets[p]);
        const float shift_red = red - red0, shift_blue = blue - blue0;
        const float ref_red = reference[p].first - reference[0].first, ref_blue = reference[p].second - reference[0].second;
        worst = std::max(worst, std::fabs(shift_red - ref_red) + std::fabs(shift_blue - ref_blue));
    }
    return worst * 100.0f;
}

// A part's tone pieces give it the texels whose colour shifts between presets like the part's own texels rather than like
// their owner's (hair painted onto a head); its dyed pieces, the texels whose colour shifts unlike their owner's (a band
// painted onto an arm).
// A part's material where a whole-piece part draws it too: texels following the part's `tone` in every preset, and the
// edge texels joined to them.
void ClaimToneRange(CharacterClass klass, const Mbs& model, uint8_t texture, const SurfaceTexels* presets, RegionMap& map) {
    if (!presets || presets->presets.size() < size_t(kNativeColorCount)) return;
    const size_t count = size_t(map.width) * size_t(map.height);
    uint16_t whole = 0;
    for (const CustomPart& part : CustomPartsOf(klass)) {
        if (part.mark == PaintedMark::kNone && !part.piece_regions) whole |= PieceBitOf(klass, part);
    }
    for (const CustomPart& part : CustomPartsOf(klass)) {
        const uint16_t bit = PieceBitOf(klass, part);
        if (!bit || !part.tone_range.second || part.tone.empty()) continue;
        std::vector<uint8_t> candidate(count, 0);
        for (const MbsLayer& layer : model.layers) {
            if (layer.texture != texture || layer.id < part.tone_range.first || layer.id > part.tone_range.second) continue;
            if (!SamplesInsideTexture(model.sources[layer.source])) continue;
            ForEachTexel(model.sources[layer.source], map.width, map.height, [&](int x, int y) {
                const size_t at = size_t(y) * size_t(map.width) + size_t(x);
                const uint16_t owner = map.pieces[at];
                candidate[at] = owner && (owner & whole) == owner && !(owner & bit);
            });
        }
        std::vector<std::pair<float, float>> tone;
        for (const Rgb& c : part.tone) tone.push_back(TexelChroma({c.r, c.g, c.b, 255}));
        std::vector<uint8_t> level(count, 0);
        for (size_t at = 0; at < count; ++at) {
            if (!candidate[at]) continue;
            const int x = int(at % size_t(map.width)), y = int(at / size_t(map.width));
            std::array<std::array<uint8_t, 4>, kNativeColorCount> texels{};
            for (size_t p = 0; p < texels.size(); ++p) texels[p] = presets->presets[p](x, y);
            if (!texels[0][3]) continue;
            float distance = 1e9f, shift = 1e9f;
            for (size_t k = 0; k + size_t(kNativeColorCount) <= tone.size(); k += size_t(kNativeColorCount)) {
                const std::span<const std::pair<float, float>> material = std::span(tone).subspan(k, size_t(kNativeColorCount));
                distance = std::min(distance, ToneDistance(texels, material));
                shift = std::min(shift, ShiftDistance(texels, material));
            }
            // Shaded skin is darker and redder than the lit skin, but shifts through the presets alike; it joins the lit skin
            // it touches, while dark metal, whose shift says little, stays out.
            const bool shaded = shift <= kToneClaimLimit && Luma(texels[0][0], texels[0][1], texels[0][2]) >= kSkinShadeLuma;
            level[at] = distance <= kSkinToneCore ? 2 : distance <= kSkinToneEdge || shaded ? 1 : 0;
        }
        for (int pass = 0; pass < 64; ++pass) {
            std::vector<size_t> joined;
            for (size_t at = 0; at < count; ++at) {
                if (level[at] != 1) continue;
                const int x = int(at % size_t(map.width)), y = int(at / size_t(map.width));
                for (int d = 0; d < 4; ++d) {
                    const int nx = x + (d == 0) - (d == 1), ny = y + (d == 2) - (d == 3);
                    if (nx >= 0 && ny >= 0 && nx < map.width && ny < map.height && level[size_t(ny) * size_t(map.width) + size_t(nx)] == 2) {
                        joined.push_back(at);
                        break;
                    }
                }
            }
            for (const size_t at : joined) level[at] = 2;
        }
        for (size_t at = 0; at < count; ++at) {
            if (level[at] == 2) map.pieces[at] = bit;
        }
    }
}

void ClaimToneTexels(CharacterClass klass, const Mbs& model, uint8_t texture, const SurfaceTexels* presets, RegionMap& map) {
    if (!presets || presets->presets.size() < size_t(kNativeColorCount)) return;
    const size_t count = size_t(map.width) * size_t(map.height);
    const auto claim = [&](uint16_t bit, std::span<const uint32_t> ids, bool like_part) {
        std::vector<uint8_t> candidate(count, 0);
        for (const MbsLayer& layer : model.layers) {
            if (layer.texture != texture || !SamplesInsideTexture(model.sources[layer.source])) continue;
            if (std::find(ids.begin(), ids.end(), layer.id) == ids.end()) continue;
            // With the texel beyond the quad its filtered edge samples.
            ForEachTexel(model.sources[layer.source], map.width, map.height, [&](int x, int y) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (x + dx < 0 || y + dy < 0 || x + dx >= map.width || y + dy >= map.height) continue;
                        candidate[size_t(y + dy) * size_t(map.width) + size_t(x + dx)] = 1;
                    }
                }
            });
        }
        const std::vector<Tone> own = TonesOf(map, *presets, 2, [&](size_t at) { return (map.pieces[at] & bit) != 0; });
        std::map<uint16_t, std::vector<Tone>> others;
        const auto tones_of_owner = [&](size_t at) -> const std::vector<Tone>& {
            const uint16_t owner = map.pieces[at];
            const uint8_t region = map.region[at];
            const uint16_t key = owner ? owner : uint16_t(0x8000u | region);
            auto [found, fresh] = others.try_emplace(key);
            if (fresh) {
                // The owner's own colours, away from the pieces in question.
                found->second =
                    owner ? TonesOf(map, *presets, 10, [&](size_t i) { return !candidate[i] && (map.pieces[i] & owner); })
                          : TonesOf(map, *presets, 10,
                                    [&](size_t i) { return !candidate[i] && !map.pieces[i] && map.region[i] == region; });
            }
            return found->second;
        };
        std::vector<uint8_t> claimed(count, 0);
        for (size_t at = 0; at < count; ++at) {
            if (!candidate[at] || (map.pieces[at] & bit)) continue;
            const int x = int(at % size_t(map.width)), y = int(at / size_t(map.width));
            std::array<std::array<uint8_t, 4>, kNativeColorCount> texels{};
            for (size_t p = 0; p < texels.size(); ++p) texels[p] = presets->presets[p](x, y);
            if (!texels[0][3]) continue;
            const auto nearest = [&](const std::vector<Tone>& tones) {
                float best = 1e9f;
                for (const Tone& tone : tones) best = std::min(best, ShiftDistance(texels, tone));
                return best;
            };
            // An owner whose every texel lies in these pieces, such as eyes painted on a head, keeps them.
            const std::vector<Tone>& owner_tones = tones_of_owner(at);
            if (owner_tones.empty()) continue;
            const float theirs = nearest(owner_tones);
            if (!like_part) {
                claimed[at] = theirs > kToneClaimLimit;
                continue;
            }
            const float mine = nearest(own);
            claimed[at] = (mine <= kToneClaimLimit && mine * 10.0f < theirs * 7.0f) || (theirs > kToneClaimLimit && mine < theirs);
        }
        for (size_t at = 0; at < count; ++at) {
            if (claimed[at]) map.pieces[at] = bit;
        }
    };
    for (const CustomPart& part : CustomPartsOf(klass)) {
        const uint16_t bit = PieceBitOf(klass, part);
        if (!bit) continue;
        if (!part.tone_pieces.empty()) claim(bit, part.tone_pieces, true);
        if (!part.dyed_pieces.empty()) claim(bit, part.dyed_pieces, false);
    }
}

// How far a texel's colours through the presets stray from a material's, each taken relative to its brightness in preset
// 0 (hundredths): a material keeps those proportions whatever its shade.
float MaterialDistance(std::span<const std::array<uint8_t, 4>> texels, std::span<const Rgb, kNativeColorCount> colors) {
    const float mine = Luma(texels[0][0], texels[0][1], texels[0][2]) + 8.0f;
    const float theirs = Luma(colors[0].r, colors[0].g, colors[0].b) + 8.0f;
    float worst = 0.0f;
    for (size_t p = 0; p < colors.size() && p < texels.size(); ++p) {
        const float wanted[3] = {float(colors[p].r), float(colors[p].g), float(colors[p].b)};
        for (size_t c = 0; c < 3; ++c) {
            worst = std::max(worst, std::fabs((float(texels[p][c]) + 4.0f) / mine - (wanted[c] + 4.0f) / theirs));
        }
    }
    return worst * 100.0f;
}

// The part most of the texels within two of `at` belong to, among the candidates; 0 for none.
uint16_t MajorityOwner(const RegionMap& map, const std::vector<uint8_t>& candidate, const std::vector<uint16_t>& owner, size_t at) {
    const int x = int(at % size_t(map.width)), y = int(at / size_t(map.width));
    std::array<int, kPieceBits> votes{};
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            if (x + dx < 0 || y + dy < 0 || x + dx >= map.width || y + dy >= map.height) continue;
            const size_t near = size_t(y + dy) * size_t(map.width) + size_t(x + dx);
            if (candidate[near] && owner[near]) votes[size_t(std::countr_zero(owner[near]))]++;
        }
    }
    const auto most = std::max_element(votes.begin(), votes.end());
    return *most ? uint16_t(1u << size_t(most - votes.begin())) : uint16_t(0);
}

// On pieces several parts paint, each texel goes to the part whose material it is painted with. A texel too dark or
// faint to tell, or nearly alone among texels of another part, goes to the part around it.
void ClaimSharedPieces(CharacterClass klass, const Mbs& model, uint8_t texture, const SurfaceTexels* presets, RegionMap& map) {
    if (!presets || presets->presets.size() < size_t(kNativeColorCount)) return;
    const size_t count = size_t(map.width) * size_t(map.height);
    for (const SharedPieces& shared : SharedPiecesOf(klass)) {
        std::vector<uint8_t> candidate(count, 0);
        for (const MbsLayer& layer : model.layers) {
            if (layer.texture != texture || !SamplesInsideTexture(model.sources[layer.source])) continue;
            const bool listed = std::any_of(shared.pieces.begin(), shared.pieces.end(), [&](std::span<const uint32_t> ids) {
                return std::find(ids.begin(), ids.end(), layer.id) != ids.end();
            });
            if (!listed) continue;
            ForEachTexel(model.sources[layer.source], map.width, map.height, [&](int x, int y) {
                const size_t at = size_t(y) * size_t(map.width) + size_t(x);
                candidate[at] = map.region[at] != 0;
            });
        }
        std::vector<uint16_t> owners;
        uint16_t bits = 0;
        for (const Material& material : shared.materials) {
            const CustomPart* part = FindCustomPart(klass, material.part);
            owners.push_back(part ? PieceBitOf(klass, *part) : 0);
            bits |= owners.back();
        }
        const CustomPart* dark = shared.dark.empty() ? nullptr : FindCustomPart(klass, shared.dark);
        const uint16_t dark_bit = dark ? PieceBitOf(klass, *dark) : 0;
        std::vector<uint16_t> owner(count, 0);
        for (size_t at = 0; at < count; ++at) {
            if (!candidate[at]) continue;
            const int x = int(at % size_t(map.width)), y = int(at / size_t(map.width));
            std::array<std::array<uint8_t, 4>, kNativeColorCount> texels{};
            float brightest = 0.0f;
            for (size_t p = 0; p < texels.size(); ++p) {
                texels[p] = presets->presets[p](x, y);
                brightest = std::max(brightest, Luma(texels[p][0], texels[p][1], texels[p][2]));
            }
            if (brightest < kSharedLit || texels[0][3] < 128) {
                owner[at] = dark_bit;
                continue;
            }
            float nearest = 1e9f;
            for (size_t m = 0; m < shared.materials.size(); ++m) {
                const float distance = MaterialDistance(texels, shared.materials[m].colors);
                if (owners[m] && distance < nearest) {
                    nearest = distance;
                    owner[at] = owners[m];
                }
            }
        }
        for (int pass = 0; pass < 32; ++pass) {
            std::vector<std::pair<size_t, uint16_t>> decided;
            for (size_t at = 0; at < count; ++at) {
                if (!candidate[at] || owner[at]) continue;
                if (const uint16_t bit = MajorityOwner(map, candidate, owner, at)) decided.emplace_back(at, bit);
            }
            if (decided.empty()) break;
            for (const auto& [at, bit] : decided) owner[at] = bit;
        }
        std::vector<uint8_t> seen(count, 0);
        for (size_t start = 0; start < count; ++start) {
            if (!candidate[start] || !owner[start] || seen[start]) continue;
            std::vector<size_t> island{start}, stack{start};
            std::array<int, kPieceBits> around{};
            seen[start] = 1;
            while (!stack.empty()) {
                const size_t at = stack.back();
                stack.pop_back();
                const int x = int(at % size_t(map.width)), y = int(at / size_t(map.width));
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (x + dx < 0 || y + dy < 0 || x + dx >= map.width || y + dy >= map.height) continue;
                        const size_t near = size_t(y + dy) * size_t(map.width) + size_t(x + dx);
                        if (!candidate[near] || !owner[near]) continue;
                        if (owner[near] != owner[start]) {
                            around[size_t(std::countr_zero(owner[near]))]++;
                            continue;
                        }
                        if (seen[near]) continue;
                        seen[near] = 1;
                        island.push_back(near);
                        stack.push_back(near);
                    }
                }
            }
            const auto most = std::max_element(around.begin(), around.end());
            if (island.size() >= shared.island || !*most) continue;
            for (const size_t at : island) owner[at] = uint16_t(1u << size_t(most - around.begin()));
        }
        for (size_t at = 0; at < count; ++at) {
            if (candidate[at] && owner[at]) map.pieces[at] = uint16_t((map.pieces[at] & ~bits) | owner[at]);
        }
    }
}

// How much lighter each texel the part owns is than the part's texels within kRidgeReach of it, in its darkest preset.
std::vector<float> RidgeContrast(uint16_t bit, const SurfaceTexels& presets, const RegionMap& map) {
    const size_t count = size_t(map.width) * size_t(map.height);
    std::vector<float> contrast(count, 1e9f);
    std::vector<uint8_t> luma(count), mine(count);
    std::vector<uint16_t> row_sum(count);
    std::vector<uint8_t> row_count(count);
    for (const TexelReader& preset : presets.presets) {
        for (size_t at = 0; at < count; ++at) {
            const int x = int(at % size_t(map.width)), y = int(at / size_t(map.width));
            const std::array<uint8_t, 4> t = (map.pieces[at] & bit) ? preset(x, y) : std::array<uint8_t, 4>{};
            mine[at] = (map.pieces[at] & bit) && t[3] >= 128;
            luma[at] = mine[at] ? uint8_t(std::lround(Luma(t[0], t[1], t[2]))) : 0;
        }
        for (int y = 0; y < map.height; ++y) {
            const size_t row = size_t(y) * size_t(map.width);
            uint32_t sum = 0, n = 0;
            for (int x = 0; x < map.width + kRidgeReach; ++x) {
                if (x < map.width) sum += luma[row + size_t(x)], n += mine[row + size_t(x)];
                if (x - 2 * kRidgeReach - 1 >= 0) sum -= luma[row + size_t(x - 2 * kRidgeReach - 1)], n -= mine[row + size_t(x - 2 * kRidgeReach - 1)];
                if (x - kRidgeReach >= 0) row_sum[row + size_t(x - kRidgeReach)] = uint16_t(sum), row_count[row + size_t(x - kRidgeReach)] = uint8_t(n);
            }
        }
        for (size_t at = 0; at < count; ++at) {
            if (!mine[at]) continue;
            const int x = int(at % size_t(map.width)), y = int(at / size_t(map.width));
            uint32_t sum = 0, n = 0;
            for (int dy = std::max(0, y - kRidgeReach); dy <= std::min(map.height - 1, y + kRidgeReach); ++dy) {
                const size_t near = size_t(dy) * size_t(map.width) + size_t(x);
                sum += row_sum[near];
                n += row_count[near];
            }
            contrast[at] = std::min(contrast[at], float(luma[at]) - float(sum) / float(std::max(n, 1u)));
        }
    }
    return contrast;
}

// A part's ridges: the long light lines painted on another part, with the soft edges that touch them.
void ClaimRidges(CharacterClass klass, const SurfaceTexels* presets, RegionMap& map) {
    if (!presets || presets->presets.size() < size_t(kNativeColorCount)) return;
    const size_t count = size_t(map.width) * size_t(map.height);
    for (const CustomPart& part : CustomPartsOf(klass)) {
        const CustomPart* under = part.ridges_of.empty() ? nullptr : FindCustomPart(klass, part.ridges_of);
        const uint16_t bit = PieceBitOf(klass, part), under_bit = under ? PieceBitOf(klass, *under) : 0;
        if (!bit || !under_bit) continue;
        const std::vector<float> contrast = RidgeContrast(under_bit, *presets, map);
        std::vector<uint8_t> level(count, 0);
        for (size_t at = 0; at < count; ++at) {
            if (contrast[at] < 1e8f) level[at] = contrast[at] >= kRidgeCore ? 2 : contrast[at] >= kRidgeEdge ? 1 : 0;
        }
        KeepPatches(map, level, [](std::span<const size_t> patch) { return patch.size() >= kRidgeLength; });
        for (int pass = 0; pass < 3; ++pass) {
            std::vector<size_t> joined;
            for (size_t at = 0; at < count; ++at) {
                if (level[at] != 1) continue;
                const int x = int(at % size_t(map.width)), y = int(at / size_t(map.width));
                bool touches = false;
                for (int dy = -1; dy <= 1 && !touches; ++dy) {
                    for (int dx = -1; dx <= 1 && !touches; ++dx) {
                        if (x + dx < 0 || y + dy < 0 || x + dx >= map.width || y + dy >= map.height) continue;
                        touches = level[size_t(y + dy) * size_t(map.width) + size_t(x + dx)] == 2;
                    }
                }
                if (touches) joined.push_back(at);
            }
            for (const size_t at : joined) level[at] = 2;
        }
        for (size_t at = 0; at < count; ++at) {
            if (level[at] == 2) map.pieces[at] = bit;
        }
    }
}

// A highlight left white by every preset: bright, with hardly any hue in any of them.
bool PaleHighlight(const SurfaceTexels* presets, int x, int y) {
    if (!presets || presets->presets.empty()) return false;
    for (const TexelReader& preset : presets->presets) {
        const std::array<uint8_t, 4> t = preset(x, y);
        const auto [red, blue] = TexelChroma(t);
        if (!t[3] || Luma(t[0], t[1], t[2]) < kPaleLuma || (std::fabs(red) + std::fabs(blue)) * 100.0f > kPaleChroma) return false;
    }
    return true;
}

// A patch every preset paints alike that only the part and the transparency around the figure border: the black
// leather inside a belt, too dark to tell from the shadow of the cloth by itself.
template <typename Takeable>
void FillUndyedHoles(uint16_t bit, const std::vector<uint8_t>& candidate, Takeable takeable, const SurfaceTexels& presets,
                     RegionMap& map) {
    if (presets.presets.size() < size_t(kNativeColorCount)) return;
    const size_t count = size_t(map.width) * size_t(map.height);
    const auto undyed = [&](size_t at) {
        std::array<std::array<uint8_t, 4>, kNativeColorCount> texels{};
        const int x = int(at % size_t(map.width)), y = int(at / size_t(map.width));
        for (size_t p = 0; p < texels.size(); ++p) texels[p] = presets.presets[p](x, y);
        const Drift drift = PresetDrift(texels);
        return drift.relative <= kUndyedEdge || drift.absolute <= kUndyedExact;
    };
    std::vector<uint8_t> seen(count, 0);
    for (size_t start = 0; start < count; ++start) {
        if (seen[start] || !candidate[start] || !takeable(start, bit) || !undyed(start)) continue;
        std::vector<size_t> patch{start}, stack{start};
        seen[start] = 1;
        bool bordered = true;
        while (!stack.empty()) {
            const size_t at = stack.back();
            stack.pop_back();
            const int x = int(at % size_t(map.width)), y = int(at / size_t(map.width));
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (x + dx < 0 || y + dy < 0 || x + dx >= map.width || y + dy >= map.height) continue;
                    const size_t near = size_t(y + dy) * size_t(map.width) + size_t(x + dx);
                    if (seen[near] || !map.region[near] || (map.pieces[near] & bit)) continue;
                    if (!candidate[near] || !takeable(near, bit) || !undyed(near)) {
                        bordered = false;
                        continue;
                    }
                    seen[near] = 1;
                    patch.push_back(near);
                    stack.push_back(near);
                }
            }
        }
        if (!bordered) continue;
        for (const size_t at : patch) map.pieces[at] = bit;
    }
}

// On a part's dyed pieces, the outlines and highlights painted with it that no preset tints are its own: a texel mostly
// surrounded by the part, or a pale, nearly colourless texel touching it (warm skin is never that grey in any preset).
void ClaimEnclosedTexels(CharacterClass klass, const Mbs& model, uint8_t texture, const SurfaceTexels* presets, RegionMap& map) {
    constexpr int kReach = 3;
    const size_t count = size_t(map.width) * size_t(map.height);
    uint16_t plain = 0; // owners whose texels can be taken: parts told apart by pieces or regions alone
    for (const CustomPart& part : CustomPartsOf(klass)) {
        if (part.mark == PaintedMark::kNone && part.tone_pieces.empty()) plain |= PieceBitOf(klass, part);
    }
    const auto takeable = [&](size_t at, uint16_t bit) {
        const uint16_t owned = map.pieces[at];
        return map.region[at] && !(owned & bit) && !(owned & ~plain);
    };
    for (const CustomPart& part : CustomPartsOf(klass)) {
        const uint16_t bit = PieceBitOf(klass, part);
        if (!bit || (part.dyed_pieces.empty() && !part.encloses && !part.outlined)) continue;
        std::vector<uint8_t> candidate(count, 0);
        for (const MbsLayer& layer : model.layers) {
            if (layer.texture != texture || !SamplesInsideTexture(model.sources[layer.source])) continue;
            const bool dyed = std::find(part.dyed_pieces.begin(), part.dyed_pieces.end(), layer.id) != part.dyed_pieces.end();
            if (!dyed && !part.OwnsPiece(layer.id)) continue;
            ForEachTexel(model.sources[layer.source], map.width, map.height,
                         [&](int x, int y) { candidate[size_t(y) * size_t(map.width) + size_t(x)] = 1; });
        }
        if (part.outlined && presets && !presets->presets.empty()) {
            // The line it is drawn with touches it: darker than half its mean, or, for a part every preset paints alike
            // (black leather), a texel the presets hardly change where the cloth around it changes a lot.
            const auto spread = [&](int x, int y) {
                int most = 0;
                const std::array<uint8_t, 4> first = presets->presets[0](x, y);
                for (const TexelReader& preset : presets->presets) {
                    const std::array<uint8_t, 4> t = preset(x, y);
                    for (size_t c = 0; c < 3; ++c) most = std::max(most, std::abs(int(t[c]) - int(first[c])));
                }
                return most;
            };
            double sum = 0.0, spreads = 0.0, weight = 0.0;
            for (size_t at = 0; at < count; at += 3) {
                if (!(map.pieces[at] & bit)) continue;
                const int x = int(at % size_t(map.width)), y = int(at / size_t(map.width));
                const std::array<uint8_t, 4> t = presets->presets[0](x, y);
                sum += Luma(t[0], t[1], t[2]);
                spreads += spread(x, y);
                weight += 1.0;
            }
            const float dark = weight > 0.0 ? float(sum / weight) * 0.5f : 0.0f;
            const bool steady_part = weight > 0.0 && spreads / weight <= kSteadyCore;
            for (int pass = 0; pass < 2; ++pass) {
                std::vector<size_t> claimed;
                for (size_t at = 0; at < count; ++at) {
                    if (!candidate[at] || !takeable(at, bit)) continue;
                    const int x = int(at % size_t(map.width)), y = int(at / size_t(map.width));
                    const std::array<uint8_t, 4> t = presets->presets[0](x, y);
                    // A part named by its colours owns just the one texel its painted edge blends into, whatever its shade.
                    const bool line = part.tone.empty() ? Luma(t[0], t[1], t[2]) < dark || (steady_part && spread(x, y) <= kSteadyLine)
                                                        : pass == 0;
                    if (!line) continue;
                    bool touches = false;
                    for (int dy = -1; dy <= 1 && !touches; ++dy) {
                        for (int dx = -1; dx <= 1 && !touches; ++dx) {
                            if (x + dx < 0 || y + dy < 0 || x + dx >= map.width || y + dy >= map.height) continue;
                            touches = (map.pieces[size_t(y + dy) * size_t(map.width) + size_t(x + dx)] & bit) != 0;
                        }
                    }
                    if (touches) claimed.push_back(at);
                }
                for (const size_t at : claimed) map.pieces[at] = bit;
            }
        }
        if (part.dyed_pieces.empty() && !part.encloses) continue;
        for (int pass = 0; pass < 4; ++pass) {
            std::vector<size_t> claimed;
            for (size_t at = 0; at < count; ++at) {
                if (!candidate[at] || !takeable(at, bit)) continue;
                const int x = int(at % size_t(map.width)), y = int(at / size_t(map.width));
                int mine = 0, opaque = 0;
                for (int dy = -kReach; dy <= kReach; ++dy) {
                    for (int dx = -kReach; dx <= kReach; ++dx) {
                        if (x + dx < 0 || y + dy < 0 || x + dx >= map.width || y + dy >= map.height) continue;
                        const size_t near = size_t(y + dy) * size_t(map.width) + size_t(x + dx);
                        if (!map.region[near]) continue;
                        ++opaque;
                        mine += (map.pieces[near] & bit) != 0;
                    }
                }
                const bool pale = !part.dyed_pieces.empty() && mine && PaleHighlight(presets, x, y);
                if (opaque && (mine * 2 >= opaque || pale)) claimed.push_back(at);
            }
            for (const size_t at : claimed) map.pieces[at] = bit;
        }
        if (part.mark == PaintedMark::kUndyed && presets) FillUndyedHoles(bit, candidate, takeable, *presets, map);
    }
}

// A texel some part's pieces draw but no part took (filtered out of each, in a region no part names) goes to the part that
// owns most of the texels around it, by piece or by region: nothing a part's pieces draw is left out of every part.
void AdoptOrphans(CharacterClass klass, const std::vector<uint16_t>& cover, RegionMap& map) {
    const size_t count = size_t(map.width) * size_t(map.height);
    const auto orphan = [&](size_t at) {
        return map.region[at] && !map.pieces[at] && !PartOfRegion(klass, map.region[at]) && cover[at];
    };
    const auto owner_bit = [&](size_t at) -> uint16_t {
        if (const uint16_t owned = map.pieces[at]) return owned;
        const CustomPart* part = PartOfRegion(klass, map.region[at]);
        return part ? PieceBitOf(klass, *part) : 0;
    };
    for (int pass = 0; pass < 32; ++pass) {
        std::vector<std::pair<size_t, uint16_t>> adopted;
        for (size_t at = 0; at < count; ++at) {
            if (!orphan(at)) continue;
            const int x = int(at % size_t(map.width)), y = int(at / size_t(map.width));
            std::array<int, kPieceBits> votes{};
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    if (x + dx < 0 || y + dy < 0 || x + dx >= map.width || y + dy >= map.height) continue;
                    const uint16_t owner = owner_bit(size_t(y + dy) * size_t(map.width) + size_t(x + dx));
                    for (size_t b = 1; b < kPieceBits; ++b) votes[b] += owner >> b & 1;
                }
            }
            size_t best = 0;
            for (size_t b = 1; b < kPieceBits; ++b) {
                if (votes[b] > 0 && (!best || votes[b] > votes[best])) best = b;
            }
            if (best) adopted.emplace_back(at, uint16_t(1u << best));
        }
        if (adopted.empty()) break;
        for (const auto& [at, bit] : adopted) map.pieces[at] = bit;
    }
}

} // namespace

void BuildPiecePlanes(CharacterClass klass, const Mbs& model, std::vector<RegionMap>& maps,
                      std::span<const SurfaceTexels> presets) {
    const std::span<const BodySurface> bodies = BodySurfacesOf(klass);
    struct Set {
        uint16_t bit;
        const CustomPart* part;
    };
    std::vector<Set> sets;
    for (const CustomPart& part : CustomPartsOf(klass)) {
        if (const uint16_t bit = PieceBitOf(klass, part)) sets.push_back({bit, &part});
    }
    const auto member_of = [&](uint32_t id) {
        uint16_t member = 0;
        for (const Set& set : sets) {
            if (set.part->OwnsPiece(id)) member |= set.bit;
        }
        return member;
    };
    for (size_t i = 0; i < maps.size() && i < bodies.size(); ++i) {
        RegionMap& map = maps[i];
        const size_t count = size_t(map.width) * size_t(map.height);
        std::vector<uint16_t> cover(count, 0), foreign(count, 0);
        std::set<std::pair<uint16_t, uint16_t>> done;
        for (const MbsLayer& layer : model.layers) {
            if (layer.texture != bodies[i].texture || !SamplesInsideTexture(model.sources[layer.source])) continue;
            const uint16_t member = member_of(layer.id);
            if (!done.insert({layer.source, member}).second) continue;
            ForEachTexel(model.sources[layer.source], map.width, map.height, [&](int x, int y) {
                const size_t at = size_t(y) * size_t(map.width) + size_t(x);
                cover[at] |= member;
                foreign[at] |= uint16_t(~member);
            });
        }
        map.pieces.assign(count, 0);
        for (size_t at = 0; at < count; ++at) map.pieces[at] = uint16_t(cover[at] & ~foreign[at]);
        for (const CustomPart& part : CustomPartsOf(klass)) {
            const uint16_t bit = PieceBitOf(klass, part);
            if (bit && (part.piece_regions || part.mark != PaintedMark::kNone)) {
                FilterPieceTexels(part, bit, i < presets.size() ? &presets[i] : nullptr, map);
            }
        }
        ResolvePieceOverlaps(klass, map);
        // The filtered edge of a quad samples one texel beyond it. A dyed garment stops at its own texels inside a piece,
        // or it would take a ring of the skin it is sewn onto.
        uint16_t dyed = 0;
        for (const CustomPart& part : CustomPartsOf(klass)) {
            if (part.mark == PaintedMark::kDyed || part.mark == PaintedMark::kSkinTone || part.mark == PaintedMark::kHorn) dyed |= PieceBitOf(klass, part);
        }
        std::vector<uint16_t> grown = map.pieces;
        for (int y = 0; y < map.height; ++y) {
            for (int x = 0; x < map.width; ++x) {
                const size_t at = size_t(y) * size_t(map.width) + size_t(x);
                const uint16_t blocked = uint16_t(foreign[at] | (cover[at] ? dyed : 0));
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (x + dx < 0 || y + dy < 0 || x + dx >= map.width || y + dy >= map.height) continue;
                        grown[at] |= uint16_t(map.pieces[size_t(y + dy) * size_t(map.width) + size_t(x + dx)] & ~blocked);
                    }
                }
            }
        }
        map.pieces = std::move(grown);
        ResolvePieceOverlaps(klass, map);
        ClaimToneTexels(klass, model, bodies[i].texture, i < presets.size() ? &presets[i] : nullptr, map);
        ClaimToneRange(klass, model, bodies[i].texture, i < presets.size() ? &presets[i] : nullptr, map);
        ClaimEnclosedTexels(klass, model, bodies[i].texture, i < presets.size() ? &presets[i] : nullptr, map);
        ClaimSharedPieces(klass, model, bodies[i].texture, i < presets.size() ? &presets[i] : nullptr, map);
        ClaimRidges(klass, i < presets.size() ? &presets[i] : nullptr, map);
        AdoptOrphans(klass, cover, map);
    }
}

std::vector<uint8_t> SerializeRegionMap(const RegionMap& map, std::string_view identity) {
    std::vector<uint8_t> out;
    Put32(out, kRegionMagic);
    Put32(out, uint32_t(map.width));
    Put32(out, uint32_t(map.height));
    PutText(out, identity);
    // Runs of one region: the maps are large and mostly empty.
    for (size_t i = 0; i < map.region.size();) {
        size_t run = 1;
        while (i + run < map.region.size() && run < 0xFFFF && map.region[i + run] == map.region[i]) ++run;
        out.push_back(map.region[i]);
        out.push_back(uint8_t(run));
        out.push_back(uint8_t(run >> 8));
        i += run;
    }
    return out;
}

std::optional<RegionMap> ParseRegionMap(std::span<const uint8_t> bytes, std::string_view identity, uint8_t max_value) {
    Cursor in(bytes);
    uint32_t magic = 0, width = 0, height = 0;
    std::string stored;
    if (!in.U32(magic) || magic != kRegionMagic || !in.U32(width) || !in.U32(height) || !in.Text(stored) ||
        stored != identity || width == 0 || height == 0 || width > 16384 || height > 16384) {
        return std::nullopt;
    }
    RegionMap map{int(width), int(height), {}, {}};
    map.region.reserve(size_t(width) * size_t(height));
    while (!in.done()) {
        uint8_t region = 0, lo = 0, hi = 0;
        if (!in.U8(region) || !in.U8(lo) || !in.U8(hi) || region > max_value) return std::nullopt;
        map.region.insert(map.region.end(), size_t(lo) | size_t(hi) << 8, region);
        if (map.region.size() > size_t(width) * size_t(height)) return std::nullopt;
    }
    if (map.region.size() != size_t(width) * size_t(height)) return std::nullopt;
    return map;
}

std::vector<uint8_t> SerializePiecePlane(int width, int height, const std::vector<uint16_t>& plane, std::string_view identity) {
    std::vector<uint8_t> out;
    Put32(out, kRegionMagic);
    Put32(out, uint32_t(width));
    Put32(out, uint32_t(height));
    PutText(out, identity);
    for (size_t i = 0; i < plane.size();) {
        size_t run = 1;
        while (i + run < plane.size() && run < 0xFFFF && plane[i + run] == plane[i]) ++run;
        out.insert(out.end(), {uint8_t(plane[i]), uint8_t(plane[i] >> 8), uint8_t(run), uint8_t(run >> 8)});
        i += run;
    }
    return out;
}

std::optional<std::vector<uint16_t>> ParsePiecePlane(std::span<const uint8_t> bytes, std::string_view identity, int width,
                                                     int height) {
    Cursor in(bytes);
    uint32_t magic = 0, stored_width = 0, stored_height = 0;
    std::string stored;
    if (!in.U32(magic) || magic != kRegionMagic || !in.U32(stored_width) || !in.U32(stored_height) || !in.Text(stored) ||
        stored != identity || int(stored_width) != width || int(stored_height) != height) {
        return std::nullopt;
    }
    const size_t count = size_t(width) * size_t(height);
    std::vector<uint16_t> plane;
    plane.reserve(count);
    while (!in.done()) {
        uint8_t lo = 0, hi = 0, run_lo = 0, run_hi = 0;
        if (!in.U8(lo) || !in.U8(hi) || !in.U8(run_lo) || !in.U8(run_hi)) return std::nullopt;
        plane.insert(plane.end(), size_t(run_lo) | size_t(run_hi) << 8, uint16_t(lo | hi << 8));
        if (plane.size() > count) return std::nullopt;
    }
    if (plane.size() != count) return std::nullopt;
    return plane;
}

float Luma(float r, float g, float b) {
    return 0.299f * r + 0.587f * g + 0.114f * b;
}

std::array<uint8_t, 4> Recolor(std::array<uint8_t, 4> texel, Rgb target, float part_luma) {
    const float shift = Luma(texel[0], texel[1], texel[2]) - part_luma;
    const auto channel = [&](uint8_t c) { return uint8_t(std::clamp(std::lround(float(c) + shift), 0L, 255L)); };
    return {channel(target.r), channel(target.g), channel(target.b), texel[3]};
}

LookShader::LookShader(CharacterClass klass, const CustomLook& look, const RegionMap& map, const TexelReader& base) : map_(&map) {
    for (int y = 0; y < map.height; y += 2) {
        for (int x = 0; x < map.width; x += 2) {
            const uint8_t region = map.At(x, y);
            const uint16_t pieces = map.Pieces(x, y);
            if (!region && !pieces) continue;
            const std::array<uint8_t, 4> t = base(x, y);
            const double a = t[3];
            const double luma = double(Luma(t[0], t[1], t[2])) * a;
            if (region) {
                sums_[region][0] += t[0] * a;
                sums_[region][1] += t[1] * a;
                sums_[region][2] += t[2] * a;
                sums_[region][3] += luma;
                weights_[region] += a;
            }
            for (size_t bit = 0; bit < piece_sums_.size(); ++bit) {
                if (!(pieces >> bit & 1)) continue;
                piece_sums_[bit][0] += t[0] * a;
                piece_sums_[bit][1] += t[1] * a;
                piece_sums_[bit][2] += t[2] * a;
                piece_sums_[bit][3] += luma;
                piece_weights_[bit] += a;
            }
        }
    }
    if (!look.enabled) return;
    for (const CustomPart& part : CustomPartsOf(klass)) {
        if (part.weapon >= 0) continue;
        const PartLook* chosen = look.Find(part.key);
        const std::optional<Rgb> color = chosen ? chosen->color : std::nullopt;
        const bool hidden = chosen && chosen->hidden;
        if (!color && !hidden) continue;
        double luma = 0.0, weight = 0.0;
        for (int r = 1; r <= kRegionCount; ++r) {
            if (!(part.regions >> (r - 1) & 1)) continue;
            luma += sums_[size_t(r)][3];
            weight += weights_[size_t(r)];
        }
        const Change change{color, hidden, weight > 0.0 ? float(luma / weight) : 128.0f};
        for (int r = 1; r <= kRegionCount; ++r) {
            if (part.regions >> (r - 1) & 1) by_region_[size_t(r)] = change;
        }
        if (const uint16_t piece_bit = PieceBitOf(klass, part)) {
            size_t bit = 0;
            while (!(piece_bit >> bit & 1)) ++bit;
            const double piece_weight = piece_weights_[bit];
            by_piece_[bit] = Change{color, hidden, piece_weight > 0.0 ? float(piece_sums_[bit][3] / piece_weight) : 128.0f};
            if (hidden && !part.bare_underlay.empty()) PaintBareHead(klass, part, piece_bit, look, base);
        }
    }
    if (klass == CharacterClass::kFighter) PaintHelmetSprites(klass, look, base);
}

void LookShader::PaintHelmetSprites(CharacterClass klass, const CustomLook& look, const TexelReader& base) {
    const CustomPart* helmet = FindCustomPart(klass, "helmet");
    const PartLook* chosen = look.Find("helmet");
    if (!helmet || !chosen || !chosen->hidden) return;
    const CustomPart* horns = FindCustomPart(klass, "horns");
    const uint16_t horn_bit = horns ? PieceBitOf(klass, *horns) : 0;
    const auto fits = [&](const AtlasRect& r) { return r.x0 >= 0 && r.y0 >= 0 && r.x1 <= map_->width && r.y1 <= map_->height; };
    std::vector<HelmetPatch> patches;
    for (const HelmetSpriteRule& rule : FighterHelmetSprites()) {
        if (!fits(rule.rect)) continue;
        HelmetPatch patch{rule.rect, rule.kind, {}};
        if (rule.kind == HelmetSprite::kBareHead) {
            if (!std::all_of(rule.layers.begin(), rule.layers.end(), [&](const BareHeadLayer& l) { return fits(l.source); })) continue;
            patch.texels = ComposeBareHead(rule, horn_bit, base);
        }
        patches.push_back(std::move(patch));
    }
    helmet_patches_ = std::move(patches);
}

// The bare-head sprites as the look paints them, in the helmet sprite's texels, with the horns drawn over them. The head
// fades out next to the sprite's edge, where filtering would reach the next sprite of the atlas.
std::vector<std::array<uint8_t, 4>> LookShader::ComposeBareHead(const HelmetSpriteRule& rule, uint16_t horn_bit,
                                                                const TexelReader& base) const {
    struct Premultiplied {
        AtlasRect rect;
        std::vector<std::array<float, 4>> texels;
    };
    std::vector<Premultiplied> sources;
    for (const BareHeadLayer& layer : rule.layers) {
        Premultiplied source{layer.source, {}};
        const AtlasRect& r = layer.source;
        source.texels.resize(size_t(r.x1 - r.x0) * size_t(r.y1 - r.y0));
        for (int y = r.y0; y < r.y1; ++y) {
            for (int x = r.x0; x < r.x1; ++x) {
                std::array<uint8_t, 4> t = base(x, y);
                ApplyOwned(x, y, t);
                const float a = t[3] / 255.0f;
                source.texels[size_t(y - r.y0) * size_t(r.x1 - r.x0) + size_t(x - r.x0)] = {t[0] / 255.0f * a, t[1] / 255.0f * a, t[2] / 255.0f * a, a};
            }
        }
        sources.push_back(std::move(source));
    }
    const AtlasRect& rect = rule.rect;
    const int w = rect.x1 - rect.x0, h = rect.y1 - rect.y0;
    std::vector<std::array<uint8_t, 4>> out(size_t(w) * size_t(h), {0, 0, 0, 0});
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double px = rect.x0 + x + 0.5, py = rect.y0 + y + 0.5;
            std::array<float, 4> acc{};
            for (size_t i = 0; i < rule.layers.size(); ++i) {
                const BareHeadLayer& l = rule.layers[i];
                const AtlasRect& r = l.source;
                const double u = l.a * px + l.b * py + l.c, v = l.d * px + l.e * py + l.f;
                if (u < r.x0 || v < r.y0 || u >= r.x1 || v >= r.y1) continue;
                const double fx = u - 0.5, fy = v - 0.5;
                const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
                const float tx = float(fx - ix), ty = float(fy - iy);
                std::array<float, 4> s{};
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const int sx = std::clamp(ix + dx, r.x0, r.x1 - 1), sy = std::clamp(iy + dy, r.y0, r.y1 - 1);
                        const std::array<float, 4>& t = sources[i].texels[size_t(sy - r.y0) * size_t(r.x1 - r.x0) + size_t(sx - r.x0)];
                        const float weight = (dx ? tx : 1.0f - tx) * (dy ? ty : 1.0f - ty);
                        for (int c = 0; c < 4; ++c) s[size_t(c)] += weight * t[size_t(c)];
                    }
                }
                for (int c = 0; c < 4; ++c) acc[size_t(c)] = s[size_t(c)] + acc[size_t(c)] * (1.0f - s[3]);
            }
            const int edge = std::min({x, y, w - 1 - x, h - 1 - y});
            const float fade = std::clamp((float(edge) - 0.5f) / 2.0f, 0.0f, 1.0f);
            for (float& c : acc) c *= fade;
            if (horn_bit && (map_->Pieces(rect.x0 + x, rect.y0 + y) & horn_bit)) {
                std::array<uint8_t, 4> horn = base(rect.x0 + x, rect.y0 + y);
                ApplyOwned(rect.x0 + x, rect.y0 + y, horn);
                const float a = horn[3] / 255.0f;
                for (int c = 0; c < 3; ++c) acc[size_t(c)] = horn[size_t(c)] / 255.0f * a + acc[size_t(c)] * (1.0f - a);
                acc[3] = a + acc[3] * (1.0f - a);
            }
            std::array<uint8_t, 4>& texel = out[size_t(y) * size_t(w) + size_t(x)];
            const float alpha = std::clamp(acc[3], 0.0f, 1.0f);
            texel[3] = uint8_t(std::lround(alpha * 255.0f));
            if (!texel[3]) continue;
            for (int c = 0; c < 3; ++c) texel[size_t(c)] = uint8_t(std::lround(std::clamp(acc[size_t(c)] / alpha, 0.0f, 1.0f) * 255.0f));
        }
    }
    // The game filters straight alpha: transparent texels beside the head take its colour, so its edge does not darken.
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<std::array<uint8_t, 4>> padded = out;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                std::array<uint8_t, 4>& texel = padded[size_t(y) * size_t(w) + size_t(x)];
                if (texel[3] || texel[0] || texel[1] || texel[2]) continue;
                int sum[3] = {}, count = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                        const std::array<uint8_t, 4>& n = out[size_t(ny) * size_t(w) + size_t(nx)];
                        if (!n[3] && !n[0] && !n[1] && !n[2]) continue;
                        for (int c = 0; c < 3; ++c) sum[c] += n[size_t(c)];
                        ++count;
                    }
                }
                if (count) {
                    for (int c = 0; c < 3; ++c) texel[size_t(c)] = uint8_t(std::max(1, (sum[c] + count / 2) / count));
                }
            }
        }
        out = std::move(padded);
    }
    return out;
}

const LookShader::HelmetPatch* LookShader::PatchAt(int x, int y) const {
    for (const HelmetPatch& patch : helmet_patches_) {
        if (patch.rect.Contains(x, y)) return &patch;
    }
    return nullptr;
}

// The underlay's lit colour over the surface: the texels at least as bright as its mean, without its outlines and shade.
std::optional<Rgb> LitColor(const RegionMap& map, const std::function<bool(size_t)>& is_underlay, const TexelReader& base) {
    std::vector<std::array<uint8_t, 4>> texels;
    double mean = 0.0;
    for (int y = 0; y < map.height; y += 4) {
        for (int x = 0; x < map.width; x += 4) {
            const size_t at = size_t(y) * size_t(map.width) + size_t(x);
            if (!map.region[at] || !is_underlay(at)) continue;
            const std::array<uint8_t, 4> t = base(x, y);
            if (t[3] < 128) continue;
            texels.push_back(t);
            mean += Luma(t[0], t[1], t[2]);
        }
    }
    if (texels.empty()) return std::nullopt;
    mean /= double(texels.size());
    double sum[3] = {}, weight = 0.0;
    for (const std::array<uint8_t, 4>& t : texels) {
        if (Luma(t[0], t[1], t[2]) < mean) continue;
        for (int c = 0; c < 3; ++c) sum[c] += double(t[size_t(c)]) * t[3];
        weight += t[3];
    }
    if (weight <= 0.0) return std::nullopt;
    return Rgb{uint8_t(sum[0] / weight + 0.5), uint8_t(sum[1] / weight + 0.5), uint8_t(sum[2] / weight + 0.5)};
}

// The part's texels painted with its bare underlay: the underlay's lit colour, shaded by the part's own luma with its
// rivets, bands and highlights blurred away, and its painted outline kept.
void LookShader::PaintBareHead(CharacterClass klass, const CustomPart& part, uint16_t piece_bit, const CustomLook& look,
                               const TexelReader& base) {
    const RegionMap& map = *map_;
    const CustomPart* under = FindCustomPart(klass, part.bare_underlay);
    if (!under) return;
    const uint16_t under_bit = PieceBitOf(klass, *under);
    const auto is_underlay = [&](size_t at) {
        const uint16_t owned = map.pieces.empty() ? 0 : map.pieces[at];
        if (under_bit) return (owned & under_bit) != 0;
        return !owned && map.region[at] <= kRegionCount && PartOfRegion(klass, map.region[at]) == under;
    };
    const std::optional<Rgb> color = LitColor(map, is_underlay, base);
    if (!color) return; // nothing of the underlay on this surface to paint with: the part is cut out
    size_t bit = 0;
    while (!(piece_bit >> bit & 1)) ++bit;
    int x0 = map.width, y0 = map.height, x1 = -1, y1 = -1;
    for (size_t at = 0; at < map.pieces.size(); ++at) {
        if (!(map.pieces[at] & piece_bit)) continue;
        const int x = int(at % size_t(map.width)), y = int(at / size_t(map.width));
        x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
    }
    if (x1 < 0) return;
    x0 = std::max(x0 - 1, 0), y0 = std::max(y0 - 1, 0), x1 = std::min(x1 + 1, map.width - 1), y1 = std::min(y1 + 1, map.height - 1);
    const int w = x1 - x0 + 1, h = y1 - y0 + 1;
    const auto global = [&](int x, int y) { return size_t(y0 + y) * size_t(map.width) + size_t(x0 + x); };
    std::vector<uint8_t> luma(size_t(w) * size_t(h), 0), painted(luma.size(), 0), alpha(luma.size(), 0), inside(luma.size(), 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::array<uint8_t, 4> t = base(x0 + x, y0 + y);
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            painted[i] = uint8_t(std::lround(Luma(t[0], t[1], t[2])));
            alpha[i] = t[3];
            inside[i] = (map.pieces[global(x, y)] & piece_bit) != 0;
            luma[i] = inside[i] ? painted[i] : 0;
        }
    }
    constexpr int kRadius = 4;
    std::vector<uint16_t> row_sum(luma.size());
    std::vector<uint8_t> row_count(luma.size());
    std::vector<int32_t> prefix_sum(size_t(w) + 1, 0), prefix_count(size_t(w) + 1, 0), column_sum(size_t(w), 0), column_count(size_t(w), 0);
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < h; ++y) {
            const size_t row = size_t(y) * size_t(w);
            for (int x = 0; x < w; ++x) {
                prefix_sum[size_t(x) + 1] = prefix_sum[size_t(x)] + (inside[row + size_t(x)] ? luma[row + size_t(x)] : 0);
                prefix_count[size_t(x) + 1] = prefix_count[size_t(x)] + inside[row + size_t(x)];
            }
            for (int x = 0; x < w; ++x) {
                const size_t lo = size_t(std::max(x - kRadius, 0)), hi = size_t(std::min(x + kRadius + 1, w));
                row_sum[row + size_t(x)] = uint16_t(prefix_sum[hi] - prefix_sum[lo]);
                row_count[row + size_t(x)] = uint8_t(prefix_count[hi] - prefix_count[lo]);
            }
        }
        std::fill(column_sum.begin(), column_sum.end(), 0);
        std::fill(column_count.begin(), column_count.end(), 0);
        const auto slide = [&](int y, int sign) {
            if (y < 0 || y >= h) return;
            const size_t row = size_t(y) * size_t(w);
            for (int x = 0; x < w; ++x) {
                column_sum[size_t(x)] += sign * int32_t(row_sum[row + size_t(x)]);
                column_count[size_t(x)] += sign * int32_t(row_count[row + size_t(x)]);
            }
        };
        for (int y = 0; y < kRadius; ++y) slide(y, 1);
        for (int y = 0; y < h; ++y) {
            slide(y + kRadius, 1);
            slide(y - kRadius - 1, -1);
            const size_t row = size_t(y) * size_t(w);
            for (int x = 0; x < w; ++x) {
                const int32_t n = column_count[size_t(x)];
                if (inside[row + size_t(x)] && n > 0) luma[row + size_t(x)] = uint8_t((column_sum[size_t(x)] + n / 2) / n);
            }
        }
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            if (!inside[i]) continue;
            bool edge = false;
            for (int dy = -1; dy <= 1 && !edge; ++dy) {
                for (int dx = -1; dx <= 1 && !edge; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (x0 + nx < 0 || y0 + ny < 0 || x0 + nx >= map.width || y0 + ny >= map.height) edge = true;
                    else edge = nx >= 0 && ny >= 0 && nx < w && ny < h && alpha[size_t(ny) * size_t(w) + size_t(nx)] < 64;
                }
            }
            if (edge) luma[i] = std::min(luma[i], painted[i]);
        }
    }
    double sum = 0.0, weight = 0.0;
    for (size_t i = 0; i < luma.size(); ++i) {
        if (!inside[i]) continue;
        sum += double(luma[i]) * alpha[i];
        weight += alpha[i];
    }
    if (shade_.empty()) shade_.assign(map.region.size(), 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            if (inside[i]) shade_[global(x, y)] = luma[i];
        }
    }
    Bare bare{{float(color->r), float(color->g), float(color->b)}, weight > 0.0 ? float(sum / weight) : 128.0f, std::nullopt};
    const PartLook* chosen = look.Find(under->key);
    if (chosen && (chosen->color || chosen->hidden)) {
        double luma_sum = 0.0, luma_weight = 0.0;
        for (int r = 1; r <= kRegionCount; ++r) {
            if (!(under->regions >> (r - 1) & 1)) continue;
            luma_sum += sums_[size_t(r)][3];
            luma_weight += weights_[size_t(r)];
        }
        bare.underlay = Change{chosen->color, chosen->hidden, luma_weight > 0.0 ? float(luma_sum / luma_weight) : 128.0f};
    }
    bare_[bit] = bare;
}

void LookShader::PaintBare(size_t bit, size_t at, std::array<uint8_t, 4>& texel) const {
    const Bare& bare = *bare_[bit];
    const float shift = kBareContrast * (float(shade_[at]) - bare.luma);
    for (size_t c = 0; c < 3; ++c) texel[c] = uint8_t(std::clamp(std::lround(bare.color[c] + shift), 0L, 255L));
    if (!bare.underlay) return;
    if (bare.underlay->hidden) texel = {0, 0, 0, 0};
    else if (bare.underlay->color) texel = Recolor(texel, *bare.underlay->color, bare.underlay->luma);
}

bool LookShader::ChangesAt(int x, int y) const {
    if (PatchAt(x, y)) return true;
    const uint16_t pieces = map_->Pieces(x, y);
    const uint16_t owned = pieces;
    if (!owned && Changes(map_->At(x, y))) return true;
    for (size_t bit = 0; bit < by_piece_.size(); ++bit) {
        if ((pieces >> bit & 1) && by_piece_[bit]) return true;
    }
    return false;
}

// A texel a piece part owns follows that part alone; the region it was clustered into does not recolour it. Hiding a part
// clears its own texels and nothing else.
void LookShader::ApplyAt(int x, int y, std::array<uint8_t, 4>& texel) const {
    const HelmetPatch* patch = PatchAt(x, y);
    if (patch && patch->kind == HelmetSprite::kBareHead) {
        texel = patch->texels[size_t(y - patch->rect.y0) * size_t(patch->rect.x1 - patch->rect.x0) + size_t(x - patch->rect.x0)];
    } else if (patch) {
        texel = {0, 0, 0, 0};
    } else {
        ApplyOwned(x, y, texel);
    }
}

void LookShader::ApplyOwned(int x, int y, std::array<uint8_t, 4>& texel) const {
    const uint16_t pieces = map_->Pieces(x, y);
    const uint16_t owned = pieces;
    if (!owned) Apply(map_->At(x, y), texel);
    for (size_t bit = 0; bit < by_piece_.size(); ++bit) {
        if (!(owned >> bit & 1) || !by_piece_[bit]) continue;
        const Change& change = *by_piece_[bit];
        if (change.hidden && bare_[bit]) PaintBare(bit, size_t(y) * size_t(map_->width) + size_t(x), texel);
        else if (change.hidden) texel = {0, 0, 0, 0};
        else if (change.color) texel = Recolor(texel, *change.color, change.luma);
    }
}

void LookShader::Apply(uint8_t region, std::array<uint8_t, 4>& texel) const {
    if (!Changes(region)) return;
    const Change& change = *by_region_[region];
    if (change.hidden) texel = {0, 0, 0, 0};
    else if (change.color) texel = Recolor(texel, *change.color, change.luma);
}

std::optional<Rgb> LookShader::NativeColor(CharacterClass klass, const CustomPart& part) const {
    double sum[3] = {}, weight = 0.0;
    for (int r = 1; r <= kRegionCount; ++r) {
        if (!(part.regions >> (r - 1) & 1)) continue;
        for (int c = 0; c < 3; ++c) sum[c] += sums_[size_t(r)][size_t(c)];
        weight += weights_[size_t(r)];
    }
    if (const uint16_t piece_bit = PieceBitOf(klass, part)) {
        size_t bit = 0;
        while (!(piece_bit >> bit & 1)) ++bit;
        for (int c = 0; c < 3; ++c) sum[c] += piece_sums_[bit][size_t(c)];
        weight += piece_weights_[bit];
    }
    if (weight <= 0.0) return std::nullopt;
    return Rgb{uint8_t(sum[0] / weight + 0.5), uint8_t(sum[1] / weight + 0.5), uint8_t(sum[2] / weight + 0.5)};
}

namespace {

// A texel `apply` makes transparent takes the colour of the texels still seen around it: the game filters the atlas
// without premultiplying, so the colour of a transparent texel tints the edge beside it.
template <typename ApplyTexel>
std::array<uint8_t, 4> Shaded(const FtxSurface& source, int x, int y, ApplyTexel& apply) {
    std::array<uint8_t, 4> texel = source.Texel(x, y);
    apply(x, y, texel);
    if (texel[3] || !source.Texel(x, y)[3]) return texel;
    float sum[3] = {}, weight = 0.0f;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if ((!dx && !dy) || x + dx < 0 || y + dy < 0 || x + dx >= source.width() || y + dy >= source.height()) continue;
            std::array<uint8_t, 4> beside = source.Texel(x + dx, y + dy);
            apply(x + dx, y + dy, beside);
            for (int c = 0; c < 3; ++c) sum[c] += float(beside[size_t(c)]) * beside[3];
            weight += beside[3];
        }
    }
    if (weight > 0.0f) {
        for (int c = 0; c < 3; ++c) texel[size_t(c)] = uint8_t(std::lround(sum[c] / weight));
    }
    return texel;
}

// Re-encodes, in `out`, the blocks of one surface where `changes` holds for a texel, with `apply` run on each texel.
template <typename Changes, typename ApplyTexel>
void ReencodeBlocks(const FtxSurfaceLayout& layout, const FtxSurface& source, std::vector<uint8_t>& out, Changes changes,
                    ApplyTexel apply) {
    uint8_t texels[64];
    for (int by = 0; by < layout.height / 4; ++by) {
        for (int bx = 0; bx < layout.width / 4; ++bx) {
            bool touched = false;
            for (int t = 0; t < 16 && !touched; ++t) touched = changes(bx * 4 + (t & 3), by * 4 + (t >> 2));
            if (!touched) continue;
            for (int t = 0; t < 16; ++t) {
                const std::array<uint8_t, 4> texel = Shaded(source, bx * 4 + (t & 3), by * 4 + (t >> 2), apply);
                std::memcpy(texels + t * 4, texel.data(), 4);
            }
            EncodeBc7Block(texels, out.data() + FtxBlockOffset(layout, bx, by));
        }
    }
}

} // namespace

std::optional<std::vector<uint8_t>> CustomizeAtlas(std::span<const uint8_t> pack, std::span<const RegionMap> maps,
                                                   CharacterClass klass, const CustomLook& look, std::string* error) {
    std::vector<uint8_t> out(pack.begin(), pack.end());
    if (!look.active()) return out;
    const std::span<const BodySurface> surfaces = BodySurfacesOf(klass);
    for (size_t i = 0; i < surfaces.size() && i < maps.size(); ++i) {
        const std::optional<FtxSurfaceLayout> layout = FtxLayoutOf(pack, surfaces[i].surface, error);
        std::optional<FtxSurface> source = ReadFtxSurface(std::vector<uint8_t>(pack.begin(), pack.end()), surfaces[i].surface, error);
        if (!layout || !source) return std::nullopt;
        const RegionMap& map = maps[i];
        if (map.width != layout->width || map.height != layout->height) {
            if (error) *error = "region map does not match the atlas";
            return std::nullopt;
        }
        const LookShader shader(klass, look, map, [&](int x, int y) { return source->Texel(x, y); });
        ReencodeBlocks(*layout, *source, out, [&](int x, int y) { return shader.ChangesAt(x, y); },
                       [&](int x, int y, std::array<uint8_t, 4>& texel) { shader.ApplyAt(x, y, texel); });
    }
    return out;
}

// The shield's band is the only strongly blue paint on a weapon texture; its soft edge blends between the two parts.
float AccentWeight(const std::array<uint8_t, 4>& texel) {
    if (!texel[3]) return 0.0f;
    return std::clamp((-0.15f - TexelChroma(texel).second) / 0.25f, 0.0f, 1.0f);
}

WeaponShader::WeaponShader(CharacterClass klass, const CustomLook& look, size_t weapon, const TexelReader& base, int width,
                           int height) {
    const CustomPart* part = WeaponPartOf(klass, weapon);
    const CustomPart* accent = WeaponAccentOf(klass, weapon);
    split_ = accent != nullptr;
    for (int y = 0; y < height; y += 2) {
        for (int x = 0; x < width; x += 2) {
            const std::array<uint8_t, 4> t = base(x, y);
            if (!t[3]) continue;
            const double band = split_ ? AccentWeight(t) : 0.0;
            for (Paint* paint : {&main_, &accent_}) {
                const double a = t[3] * (paint == &main_ ? 1.0 - band : band);
                for (int c = 0; c < 3; ++c) paint->sums[size_t(c)] += t[size_t(c)] * a;
                paint->sums[3] += double(Luma(t[0], t[1], t[2])) * a;
                paint->sums[4] += a;
            }
        }
    }
    for (Paint* paint : {&main_, &accent_}) {
        if (paint->sums[4] > 0.0) paint->luma = float(paint->sums[3] / paint->sums[4]);
    }
    const auto chosen = [&](const CustomPart* owner, Paint& paint) {
        const PartLook* look_of = owner && look.enabled ? look.Find(owner->key) : nullptr;
        if (!look_of) return;
        paint.color = look_of->color;
        paint.hidden = look_of->hidden;
    };
    chosen(part, main_);
    chosen(accent, accent_);
}

void WeaponShader::ApplyPaint(const Paint& paint, std::array<uint8_t, 4>& texel) const {
    if (paint.hidden) texel = {0, 0, 0, 0};
    else if (paint.color) texel = Recolor(texel, *paint.color, paint.luma);
}

void WeaponShader::Apply(std::array<uint8_t, 4>& texel) const {
    const float band = split_ ? AccentWeight(texel) : 0.0f;
    std::array<uint8_t, 4> outside = texel;
    ApplyPaint(main_, outside);
    if (band <= 0.0f) {
        texel = outside;
        return;
    }
    std::array<uint8_t, 4> inside = texel;
    ApplyPaint(accent_, inside);
    for (size_t c = 0; c < 4; ++c) texel[c] = uint8_t(std::lround(float(outside[c]) + (float(inside[c]) - float(outside[c])) * band));
}

std::optional<Rgb> WeaponShader::NativeColor(bool accent) const {
    const Paint& paint = accent ? accent_ : main_;
    if (paint.sums[4] <= 0.0) return std::nullopt;
    const double w = paint.sums[4];
    return Rgb{uint8_t(paint.sums[0] / w + 0.5), uint8_t(paint.sums[1] / w + 0.5), uint8_t(paint.sums[2] / w + 0.5)};
}

bool WeaponChanged(CharacterClass klass, size_t weapon, const CustomLook& look) {
    for (const CustomPart* part : {WeaponPartOf(klass, weapon), WeaponAccentOf(klass, weapon)}) {
        const PartLook* chosen = part && look.enabled ? look.Find(part->key) : nullptr;
        if (chosen && (chosen->color || chosen->hidden)) return true;
    }
    return false;
}

std::optional<std::vector<uint8_t>> CustomizeWeapon(std::span<const uint8_t> pack, CharacterClass klass, size_t weapon,
                                                    const CustomLook& look, std::string* error) {
    std::vector<uint8_t> out(pack.begin(), pack.end());
    if (!WeaponChanged(klass, weapon, look)) return out;
    const std::optional<FtxSurfaceLayout> layout = FtxLayoutOf(pack, 0, error);
    std::optional<FtxSurface> source = ReadFtxSurface(std::vector<uint8_t>(pack.begin(), pack.end()), 0, error);
    if (!layout || !source) return std::nullopt;
    const WeaponShader shader(klass, look, weapon, [&](int x, int y) { return source->Texel(x, y); }, source->width(),
                              source->height());
    ReencodeBlocks(*layout, *source, out, [&](int x, int y) { return source->Texel(x, y)[3] != 0; },
                   [&](int, int, std::array<uint8_t, 4>& texel) { shader.Apply(texel); });
    return out;
}


std::string AtlasName(CharacterClass klass, int color) {
    std::string name(ClassName(klass));
    for (char& c : name) c = char(std::toupper(static_cast<unsigned char>(c)));
    return "PLAYER/" + name + "0" + std::to_string(color) + ".FTX";
}

std::string GameModelName(CharacterClass klass) {
    std::string name(ClassName(klass));
    for (char& c : name) c = char(std::toupper(static_cast<unsigned char>(c)));
    return "PLAYER/" + name + "00.MBS";
}

namespace {

// The piece planes of a class: from the cache for the same game files, else built from the in-game model and cached.
// `known` holds the presets' texels when the caller has read them already.
void LoadPiecePlanes(CharacterClass klass, const EntryReader& read, const std::filesystem::path& cache_dir,
                     const std::string& identity, std::vector<RegionMap>& maps, std::span<const SurfaceTexels> known = {}) {
    const std::span<const BodySurface> bodies = BodySurfacesOf(klass);
    const std::string key = identity + "|" + std::string(ClassName(klass)) + "|" + std::string(kPieceCacheVersion);
    const auto cache_file = [&](size_t i) {
        return cache_dir / ("pieces_" + std::string(ClassName(klass)) + "_" + std::to_string(bodies[i].surface) + ".bin");
    };
    std::vector<std::vector<uint16_t>> planes;
    for (size_t i = 0; i < maps.size() && !cache_dir.empty(); ++i) {
        std::ifstream in(cache_file(i), std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::optional<std::vector<uint16_t>> plane = ParsePiecePlane(bytes, key, maps[i].width, maps[i].height);
        if (!plane) break;
        planes.push_back(std::move(*plane));
    }
    if (planes.size() == maps.size() && !planes.empty()) {
        for (size_t i = 0; i < maps.size(); ++i) maps[i].pieces = std::move(planes[i]);
        return;
    }
    const std::optional<Mbs> model = ParseMbs(read(GameModelName(klass)));
    if (!model) return;
    const std::span<const CustomPart> parts = CustomPartsOf(klass);
    const auto uses = [&](PaintedMark mark) {
        return std::any_of(parts.begin(), parts.end(), [mark](const CustomPart& p) { return p.mark == mark; });
    };
    const bool toned = std::any_of(parts.begin(), parts.end(),
                                   [](const CustomPart& p) { return !p.tone_pieces.empty() || !p.dyed_pieces.empty(); });
    const int wanted = uses(PaintedMark::kDyed) || uses(PaintedMark::kSkinTone) || uses(PaintedMark::kSteady) || uses(PaintedMark::kUndyed) || toned ? kNativeColorCount
                       : uses(PaintedMark::kTattooInk)                            ? 2
                                                                                  : 0;
    std::vector<std::shared_ptr<const FtxSurface>> surfaces;
    std::vector<SurfaceTexels> presets;
    for (size_t i = 0; wanted && i < maps.size(); ++i) {
        if (i < known.size() && int(known[i].presets.size()) >= wanted) {
            presets.push_back(known[i]);
            continue;
        }
        SurfaceTexels texels;
        for (int color = 0; color < wanted; ++color) {
            std::optional<FtxSurface> surface = ReadFtxSurface(read(AtlasName(klass, color)), bodies[i].surface);
            if (!surface) break;
            surfaces.push_back(std::make_shared<const FtxSurface>(std::move(*surface)));
            texels.presets.push_back([s = surfaces.back()](int x, int y) { return s->Texel(x, y); });
        }
        presets.push_back(std::move(texels));
    }
    BuildPiecePlanes(klass, *model, maps, presets);
    for (size_t i = 0; i < maps.size() && !cache_dir.empty(); ++i) {
        WriteAtomic(cache_file(i), SerializePiecePlane(maps[i].width, maps[i].height, maps[i].pieces, key));
    }
}

} // namespace

std::optional<std::vector<RegionMap>> ClassRegions(CharacterClass klass, const EntryReader& read,
                                                   const std::filesystem::path& cache_dir, const std::string& identity) {
    const std::span<const BodySurface> bodies = BodySurfacesOf(klass);
    const std::string key = identity + "|" + std::string(ClassName(klass)) + "|" + std::string(kRegionCacheVersion);
    const auto cache_file = [&](size_t i) {
        return cache_dir / ("regions_" + std::string(ClassName(klass)) + "_" + std::to_string(bodies[i].surface) + ".bin");
    };
    std::vector<RegionMap> maps;
    for (size_t i = 0; i < bodies.size() && !cache_dir.empty(); ++i) {
        std::ifstream in(cache_file(i), std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::optional<RegionMap> map = ParseRegionMap(bytes, key);
        if (!map) break;
        maps.push_back(std::move(*map));
    }
    if (maps.size() == bodies.size()) {
        LoadPiecePlanes(klass, read, cache_dir, identity, maps);
        return maps;
    }

    const std::optional<Mbs> model = ParseMbs(read(GameModelName(klass)));
    if (!model) return std::nullopt;
    std::vector<std::vector<FtxSurface>> surfaces(bodies.size());
    for (int color = 0; color < kNativeColorCount; ++color) {
        std::vector<uint8_t> pack = read(AtlasName(klass, color));
        for (size_t i = 0; i < bodies.size(); ++i) {
            std::optional<FtxSurface> surface = ReadFtxSurface(pack, bodies[i].surface);
            if (!surface) return std::nullopt;
            surfaces[i].push_back(std::move(*surface));
        }
    }
    std::vector<SurfaceTexels> inputs;
    for (size_t i = 0; i < bodies.size(); ++i) {
        SurfaceTexels input;
        for (FtxSurface& surface : surfaces[i]) input.presets.push_back([&surface](int x, int y) { return surface.Texel(x, y); });
        input.width = surfaces[i][0].width();
        input.height = surfaces[i][0].height();
        input.texture = bodies[i].texture;
        inputs.push_back(std::move(input));
    }
    maps = BuildRegionMaps(inputs, *model);
    for (size_t i = 0; i < maps.size() && !cache_dir.empty(); ++i) WriteAtomic(cache_file(i), SerializeRegionMap(maps[i], key));
    LoadPiecePlanes(klass, read, cache_dir, identity, maps, inputs);
    return maps;
}

} // namespace dcp
