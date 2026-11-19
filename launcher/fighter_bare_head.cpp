#include "fighter_bare_head.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "game_texture.h"

namespace dcp {
namespace {

constexpr BareHeadLayer kHead1100[] = {
    {{3972, 2880, 4072, 3016}, 0.955045383, 0.303273177, 974.243917, -0.3014331, 0.953018226, 2871.41565},
    {{2832, 3028, 2916, 3140}, 0.987504931, 0.183609411, -106.601111, -0.188704649, 0.939521054, 2685.22653},
    {{2400, 3056, 2512, 3180}, 0.953716951, 0.3007058, -551.581283, -0.3007058, 0.953716951, 3056.63403},
    {{3496, 2448, 3656, 2608}, 0.954930857, 0.30368221, 566.284679, -0.301840571, 0.952491331, 2468.97451},
};
constexpr BareHeadLayer kHead1101[] = {
    {{3972, 2880, 4072, 3016}, 0.986428777, 0.123166312, 417.131789, -0.121737895, 0.984118717, 2105.2768},
    {{2832, 3028, 2916, 3140}, 0.996457567, 0.000571799692, -565.907977, -0.0142169234, 0.950572796, 1897.6049},
    {{2400, 3056, 2512, 3180}, 0.984668801, 0.120902126, -1105.99137, -0.120902126, 0.984668801, 2289.65077},
    {{3496, 2448, 3656, 2608}, 0.986391011, 0.123586009, 8.83929959, -0.12223062, 0.983678423, 1703.43039},
};
constexpr BareHeadLayer kHead1102[] = {
    {{3972, 2880, 4072, 3016}, 0.970048491, 0.251181668, 279.373437, -0.24945391, 0.96792479, 3743.40888},
    {{2832, 3028, 2916, 3140}, 0.995989265, 0.129937635, -905.975444, -0.137620328, 0.948351149, 3444.52314},
    {{2400, 3056, 2512, 3180}, 0.968583161, 0.248689887, -1246.82049, -0.248689887, 0.968583161, 3928.38922},
    {{3496, 2448, 3656, 2608}, 0.969956252, 0.251596296, -128.22528, -0.249889278, 0.967420702, 3341.01683},
};
constexpr BareHeadLayer kHead1103[] = {
    {{3972, 2880, 4072, 3016}, 0.995898019, 0.0909346524, 1302.0026, -0.0895755212, 0.993527762, 1648.91115},
    {{2832, 3028, 2916, 3140}, 1.00189233, -0.0326606517, 370.983194, 0.01741094, 0.956213296, 1554.00804},
    {{2400, 3056, 2512, 3180}, 0.994052959, 0.0887168631, -221.989103, -0.0887168631, 0.994052959, 1833.88},
    {{3496, 2448, 3656, 2608}, 0.995874046, 0.0913578903, 893.528793, -0.0900856093, 0.993101509, 1246.75434},
};
constexpr BareHeadLayer kHead1105[] = {
    {{3972, 2880, 4072, 3016}, 1.01467616, 0.193874887, 3081.27427, -0.192238692, 1.01237639, 1043.18471},
    {{2832, 3028, 2916, 3140}, 1.03329734, 0.0674157827, 2216.19926, -0.0784888517, 0.984799361, 1160.37249},
    {{2400, 3056, 2512, 3180}, 1.01300288, 0.191408882, 1554.89799, -0.191408882, 1.01300288, 1229.48885},
    {{3496, 2448, 3656, 2608}, 1.01460885, 0.194307585, 2672.56835, -0.192720126, 1.01188676, 640.285505},
};
constexpr BareHeadLayer kHead1107[] = {
    {{3972, 2880, 4072, 3016}, 0.995898019, 0.0909346524, 1445.23792, -0.0895755212, 0.993527762, 1380.69286},
    {{2832, 3028, 2916, 3140}, 1.00189233, -0.0326606517, 546.724603, 0.01741094, 0.956213296, 1313.17864},
    {{2400, 3056, 2512, 3180}, 0.994052959, 0.0887168631, -78.4967823, -0.0887168631, 0.994052959, 1565.67132},
    {{3496, 2448, 3656, 2608}, 0.995874046, 0.0913578903, 1036.65222, -0.0900856093, 0.993101509, 978.559458},
};
constexpr BareHeadLayer kHead1110[] = {
    {{3972, 2880, 4072, 3016}, 0.54300682, 0.775988788, 736.534479, -0.77344339, 0.542541049, 4772.03623},
    {{2832, 3028, 2916, 3140}, 0.636076026, 0.704771869, -578.637614, -0.681051154, 0.597121567, 4471.98059},
    {{2400, 3056, 2512, 3180}, 0.543483225, 0.77329841, -792.694925, -0.77329841, 0.543483225, 4957.88786},
    {{3496, 2448, 3656, 2608}, 0.542686008, 0.776230169, 329.37908, -0.773446866, 0.541911502, 4369.00456},
};
constexpr BareHeadLayer kHead1113[] = {
    {{3972, 2880, 4072, 3016}, 0.935579748, -0.29647492, 1441.92041, 0.296803118, 0.932918473, 528.235139},
    {{2832, 3028, 2916, 3140}, 0.893999035, -0.410572244, 649.070139, 0.379403777, 0.858472374, 500.446702},
    {{2400, 3056, 2512, 3180}, 0.933067073, -0.297779687, -80.0635533, 0.297779687, 0.933067073, 712.830379},
    {{3496, 2448, 3656, 2608}, 0.935719018, -0.296082896, 1032.97262, 0.296179531, 0.932726828, 126.426658},
};
constexpr BareHeadLayer kHead1115[] = {
    {{552, 3104, 672, 3220}, 0.436780399, 0.885692021, -1107.35032, -0.913388134, 0.435254911, 5874.5872},
    {{2392, 3412, 2496, 3496}, 0.348289352, 0.956917129, 958.972131, -0.956917129, 0.348289352, 6273.32147},
    {{2520, 3204, 2640, 3316}, 0.437084399, 0.886108908, 895.520811, -0.91390325, 0.435405374, 5940.16202},
    {{996, 2900, 1140, 3048}, 0.415537183, 0.880024558, -553.557945, -0.95722746, 0.434700097, 5793.39022},
};
constexpr BareHeadLayer kHead1120[] = {
    {{3972, 2880, 4072, 3016}, 0.861168845, -0.340662468, 1191.94448, 0.340796234, 0.85864212, 121.881527},
    {{2832, 3028, 2916, 3140}, 0.81451759, -0.445328052, 450.735924, 0.413649916, 0.783127108, 87.3955841},
    {{2400, 3056, 2512, 3180}, 0.858717719, -0.341725284, -328.69903, 0.341725284, 0.858717719, 306.069963},
    {{3496, 2448, 3656, 2608}, 0.861325749, -0.340302614, 782.82059, 0.340196622, 0.858502766, -279.636377},
};
constexpr BareHeadLayer kHead1122[] = {
    {{3972, 2880, 4072, 3016}, -1.06132802, -0.0182074992, 6293.68129, -0.0193699841, 1.05867104, 2551.54119},
    {{2832, 3028, 2916, 3140}, -1.05348507, -0.149316411, 5225.20104, -0.12798801, 1.00703123, 2929.35022},
    {{2400, 3056, 2512, 3180}, -1.05912681, -0.0203362737, 4757.67656, -0.0203362737, 1.05912681, 2741.4167},
    {{3496, 2448, 3656, 2608}, -1.06135124, -0.0177581367, 5885.66135, -0.0187828591, 1.05827972, 2146.3154},
};
constexpr BareHeadLayer kHead1127[] = {
    {{3972, 2880, 4072, 3016}, 1.01917281, 0.215092303, 1059.21266, -0.213398289, 1.01688602, 2218.34839},
    {{2832, 3028, 2916, 3140}, 1.04039327, 0.0879655137, 58.867267, -0.0981898291, 0.991286649, 2071.62418},
    {{2400, 3056, 2512, 3180}, 1.01753367, 0.212573858, -465.449534, -0.212573858, 1.01753367, 2403.35216},
    {{3496, 2448, 3656, 2608}, 1.01909658, 0.215527215, 650.977261, -0.213874159, 1.01638309, 1816.10837},
};

constexpr HelmetSpriteRule kRules[] = {
    {1100, HelmetSprite::kBareHead, {2764, 844, 2996, 1052}, kHead1100},
    {1101, HelmetSprite::kBareHead, {3352, 1140, 3548, 1344}, kHead1101},
    {1102, HelmetSprite::kBareHead, {3664, 0, 3840, 204}, kHead1102},
    {1103, HelmetSprite::kBareHead, {2436, 1412, 2688, 1612}, kHead1103},
    {1104, HelmetSprite::kClear, {636, 1856, 684, 1908}},
    {1105, HelmetSprite::kBareHead, {372, 1856, 636, 2048}, kHead1105},
    {1106, HelmetSprite::kClear, {3920, 2180, 3996, 2232}},
    {1107, HelmetSprite::kBareHead, {2268, 1668, 2520, 1868}, kHead1107},
    {1110, HelmetSprite::kBareHead, {3496, 1608, 3716, 1768}, kHead1110},
    {1111, HelmetSprite::kClear, {3180, 1096, 3220, 1140}},
    {1113, HelmetSprite::kBareHead, {3108, 1412, 3312, 1624}, kHead1113},
    {1115, HelmetSprite::kBareHead, {2952, 256, 3168, 460}, kHead1115},
    {1116, HelmetSprite::kClear, {3244, 2744, 3316, 2816}},
    {1117, HelmetSprite::kClear, {1140, 2368, 1196, 2452}},
    {1118, HelmetSprite::kClear, {2720, 0, 2952, 208}},
    {1120, HelmetSprite::kBareHead, {3844, 1608, 4016, 1800}, kHead1120},
    {1122, HelmetSprite::kBareHead, {2040, 288, 2304, 480}, kHead1122},
    {1127, HelmetSprite::kBareHead, {2464, 1140, 2728, 1332}, kHead1127},
    {1130, HelmetSprite::kClear, {756, 1436, 988, 1588}},
    {1132, HelmetSprite::kClear, {2924, 1868, 3132, 2048}},
    {1134, HelmetSprite::kClear, {3560, 844, 3744, 1056}},
    {1135, HelmetSprite::kClear, {3566, 256, 3798, 408}},
};

constexpr HelmetCopy kCopies[] = {{1101, -708, 2208}, {1102, 164, 3356}, {1115, -928, 3112}};

constexpr int kAtlasSize = 4096;
constexpr int kBlock = 4;

const HelmetSpriteRule* RuleOf(uint32_t id) {
    for (const HelmetSpriteRule& rule : kRules) {
        if (rule.id == id) return &rule;
    }
    return nullptr;
}

AtlasRect TexelRect(const MbsQuad& quad) {
    AtlasRect rect{kAtlasSize, kAtlasSize, 0, 0};
    for (const MbsPoint& c : quad.corners) {
        rect.x0 = std::min(rect.x0, int(std::lround(c.x * kAtlasSize)));
        rect.x1 = std::max(rect.x1, int(std::lround(c.x * kAtlasSize)));
        rect.y0 = std::min(rect.y0, int(std::lround(c.y * kAtlasSize)));
        rect.y1 = std::max(rect.y1, int(std::lround(c.y * kAtlasSize)));
    }
    return rect;
}

bool IsBareHead(uint32_t id) {
    return id >= 1200 && id < 1300;
}

bool IsHelmet(uint32_t id) {
    return id >= 1100 && id < 1200;
}

// Every piece but the head and the weapon and shield.
bool IsBody(uint32_t id) {
    return id < 1100 || (id >= 1300 && id < 3000);
}

} // namespace

std::span<const HelmetSpriteRule> FighterHelmetSprites() {
    return kRules;
}

std::span<const HelmetCopy> FighterHelmetCopies() {
    return kCopies;
}

AtlasRect HelmetCopySource(const HelmetSpriteRule& rule) {
    return {std::max(0, rule.rect.x0 / kBlock * kBlock - kBlock), std::max(0, rule.rect.y0 / kBlock * kBlock - kBlock),
            std::min(kAtlasSize, (rule.rect.x1 + kBlock - 1) / kBlock * kBlock + kBlock),
            std::min(kAtlasSize, (rule.rect.y1 + kBlock - 1) / kBlock * kBlock + kBlock)};
}

// Tracks run on their own clocks, so the moment a frame is drawn is its tick, not its index.
std::vector<uint32_t> OffHeadHelmetLayers(const Mbs& model) {
    std::vector<uint8_t> off(model.layers.size(), 0);
    std::vector<uint32_t> keys;
    for (const MbsAnimation& animation : model.animations) {
        long length = 0;
        for (size_t t = 0; t < animation.track_count; ++t) {
            const MbsTrack& track = model.tracks[animation.first_track + t];
            long sum = 0;
            for (size_t f = 0; f < track.frame_count; ++f) sum += model.frames[track.first_frame + f].duration;
            length = std::max(length, sum);
        }
        for (long tick = 0; tick < length; ++tick) {
            keys.clear();
            for (size_t t = 0; t < animation.track_count; ++t) {
                const MbsTrack& track = model.tracks[animation.first_track + t];
                if (!track.frame_count) continue;
                long end = 0;
                size_t f = 0;
                while (f + 1 < track.frame_count && tick >= (end += model.frames[track.first_frame + f].duration)) ++f;
                keys.push_back(model.frames[track.first_frame + f].key);
            }
            bool bare_head = false;
            for (const uint32_t k : keys) {
                for (uint32_t i = 0; i < model.keys[k].layer_count; ++i) bare_head |= IsBareHead(model.layers[model.keys[k].first_layer + i].id);
            }
            for (const uint32_t k : keys) {
                const MbsKey& key = model.keys[k];
                bool body = false;
                for (uint32_t i = 0; i < key.layer_count; ++i) body |= IsBody(model.layers[key.first_layer + i].id);
                if (body && !bare_head) continue;
                for (uint32_t i = 0; i < key.layer_count; ++i) {
                    if (IsHelmet(model.layers[key.first_layer + i].id)) off[key.first_layer + i] = 1;
                }
            }
        }
    }
    std::vector<uint32_t> layers;
    for (uint32_t i = 0; i < off.size(); ++i) {
        if (off[i]) layers.push_back(i);
    }
    return layers;
}

std::optional<std::vector<uint8_t>> FighterModelWithHelmetCopies(std::span<const uint8_t> bytes) {
    const std::optional<Mbs> model = ParseMbs(bytes);
    if (!model) return std::nullopt;
    std::vector<MbsSourceCopy> copies;
    for (const HelmetCopy& copy : kCopies) {
        const HelmetSpriteRule* rule = RuleOf(copy.id);
        std::optional<uint16_t> source;
        for (const MbsLayer& layer : model->layers) {
            if (layer.id != copy.id) continue;
            if (layer.texture != 0 || (source && *source != layer.source)) return std::nullopt;
            source = layer.source;
        }
        if (!rule || !source) return std::nullopt;
        const AtlasRect rect = TexelRect(model->sources[*source]);
        if (rect.x0 != rule->rect.x0 || rect.y0 != rule->rect.y0 || rect.x1 != rule->rect.x1 || rect.y1 != rule->rect.y1) return std::nullopt;
        copies.push_back({*source, float(copy.dx) / kAtlasSize, float(copy.dy) / kAtlasSize});
    }
    std::vector<std::pair<uint32_t, size_t>> moved;
    for (const uint32_t layer : OffHeadHelmetLayers(*model)) {
        for (size_t c = 0; c < std::size(kCopies); ++c) {
            if (model->layers[layer].id == kCopies[c].id) moved.emplace_back(layer, c);
        }
    }
    return WithSourceCopies(bytes, copies, moved);
}

bool AddHelmetCopies(std::vector<uint8_t>& pack) {
    const std::optional<FtxSurfaceLayout> layout = FtxLayoutOf(pack, 0);
    if (!layout || layout->width != kAtlasSize || layout->height != kAtlasSize) return false;
    constexpr size_t kBlockBytes = 16;
    for (const HelmetCopy& copy : kCopies) {
        const HelmetSpriteRule* rule = RuleOf(copy.id);
        if (!rule) return false;
        const AtlasRect from = HelmetCopySource(*rule);
        for (int by = from.y0 / kBlock; by < from.y1 / kBlock; ++by) {
            for (int bx = from.x0 / kBlock; bx < from.x1 / kBlock; ++bx) {
                const size_t source = FtxBlockOffset(*layout, bx, by);
                const size_t target = FtxBlockOffset(*layout, bx + copy.dx / kBlock, by + copy.dy / kBlock);
                if (source + kBlockBytes > pack.size() || target + kBlockBytes > pack.size()) return false;
                std::memcpy(pack.data() + target, pack.data() + source, kBlockBytes);
            }
        }
    }
    return true;
}

} // namespace dcp
