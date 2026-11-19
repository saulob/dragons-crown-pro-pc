#include "game_texture.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

namespace dcp {
namespace {

constexpr int kBlockBytes = 16;
constexpr uint32_t kDfmtBc7 = 41;
constexpr uint32_t kTilingLinear = 0;
constexpr uint32_t kTilingThin1d = 13;

class BitReader {
public:
    explicit BitReader(const uint8_t* block) : block_(block) {}
    uint32_t Take(int count) {
        uint32_t value = 0;
        for (int i = 0; i < count; ++i, ++at_) value |= uint32_t(block_[at_ >> 3] >> (at_ & 7) & 1) << i;
        return value;
    }

private:
    const uint8_t* block_;
    int at_ = 0;
};

int Expand(uint32_t value, int bits) {
    value <<= 8 - bits;
    return int(value | value >> bits);
}

int Lerp(int a, int b, int weight) { return ((64 - weight) * a + weight * b + 32) >> 6; }

constexpr std::array<int, 4> kWeights2 = {0, 21, 43, 64};
constexpr std::array<int, 8> kWeights3 = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr std::array<int, 16> kWeights4 = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

constexpr std::array<uint16_t, 64> kPartition2 = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00,
    0xFFF0, 0xF000, 0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C,
    0x17E8, 0x0FF0, 0x718E, 0x399C, 0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8,
    0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660, 0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C,
    0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22};

constexpr std::array<std::array<uint8_t, 16>, 64> kPartition3 = {{
    {0,0,1,1,0,0,1,1,0,2,2,1,2,2,2,2},{0,0,0,1,0,0,1,1,2,2,1,1,2,2,2,1},{0,0,0,0,2,0,0,1,2,2,1,1,2,2,1,1},
    {0,2,2,2,0,0,2,2,0,0,1,1,0,1,1,1},{0,0,0,0,0,0,0,0,1,1,2,2,1,1,2,2},{0,0,1,1,0,0,1,1,0,0,2,2,0,0,2,2},
    {0,0,2,2,0,0,2,2,1,1,1,1,1,1,1,1},{0,0,1,1,0,0,1,1,2,2,1,1,2,2,1,1},{0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2},
    {0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2},{0,0,0,0,1,1,1,1,2,2,2,2,2,2,2,2},{0,0,1,2,0,0,1,2,0,0,1,2,0,0,1,2},
    {0,1,1,2,0,1,1,2,0,1,1,2,0,1,1,2},{0,1,2,2,0,1,2,2,0,1,2,2,0,1,2,2},{0,0,1,1,0,1,1,2,1,1,2,2,1,2,2,2},
    {0,0,1,1,2,0,0,1,2,2,0,0,2,2,2,0},{0,0,0,1,0,0,1,1,0,1,1,2,1,1,2,2},{0,1,1,1,0,0,1,1,2,0,0,1,2,2,0,0},
    {0,0,0,0,1,1,2,2,1,1,2,2,1,1,2,2},{0,0,2,2,0,0,2,2,0,0,2,2,1,1,1,1},{0,1,1,1,0,1,1,1,0,2,2,2,0,2,2,2},
    {0,0,0,1,0,0,0,1,2,2,2,1,2,2,2,1},{0,0,0,0,0,0,1,1,0,1,2,2,0,1,2,2},{0,0,0,0,1,1,0,0,2,2,1,0,2,2,1,0},
    {0,1,2,2,0,1,2,2,0,0,1,1,0,0,0,0},{0,0,1,2,0,0,1,2,1,1,2,2,2,2,2,2},{0,1,1,0,1,2,2,1,1,2,2,1,0,1,1,0},
    {0,0,0,0,0,1,1,0,1,2,2,1,1,2,2,1},{0,0,2,2,1,1,0,2,1,1,0,2,0,0,2,2},{0,1,1,0,0,1,1,0,2,0,0,2,2,2,2,2},
    {0,0,1,1,0,1,2,2,0,1,2,2,0,0,1,1},{0,0,0,0,2,0,0,0,2,2,1,1,2,2,2,1},{0,0,0,0,0,0,0,2,1,1,2,2,1,2,2,2},
    {0,2,2,2,0,0,2,2,0,0,1,2,0,0,1,1},{0,0,1,1,0,0,1,2,0,0,2,2,0,2,2,2},{0,1,2,0,0,1,2,0,0,1,2,0,0,1,2,0},
    {0,0,0,0,1,1,1,1,2,2,2,2,0,0,0,0},{0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0},{0,1,2,0,2,0,1,2,1,2,0,1,0,1,2,0},
    {0,0,1,1,2,2,0,0,1,1,2,2,0,0,1,1},{0,0,1,1,1,1,2,2,2,2,0,0,0,0,1,1},{0,1,0,1,0,1,0,1,2,2,2,2,2,2,2,2},
    {0,0,0,0,0,0,0,0,2,1,2,1,2,1,2,1},{0,0,2,2,1,1,2,2,0,0,2,2,1,1,2,2},{0,0,2,2,0,0,1,1,0,0,2,2,0,0,1,1},
    {0,2,2,0,1,2,2,1,0,2,2,0,1,2,2,1},{0,1,0,1,2,2,2,2,2,2,2,2,0,1,0,1},{0,0,0,0,2,1,2,1,2,1,2,1,2,1,2,1},
    {0,1,0,1,0,1,0,1,0,1,0,1,2,2,2,2},{0,2,2,2,0,1,1,1,0,2,2,2,0,1,1,1},{0,0,0,2,1,1,1,2,0,0,0,2,1,1,1,2},
    {0,0,0,0,2,1,1,2,2,1,1,2,2,1,1,2},{0,2,2,2,0,1,1,1,0,1,1,1,0,2,2,2},{0,0,0,2,1,1,1,2,1,1,1,2,0,0,0,2},
    {0,1,1,0,0,1,1,0,0,1,1,0,2,2,2,2},{0,0,0,0,0,0,0,0,2,1,1,2,2,1,1,2},{0,1,1,0,0,1,1,0,2,2,2,2,2,2,2,2},
    {0,0,2,2,0,0,1,1,0,0,1,1,0,0,2,2},{0,0,2,2,1,1,2,2,1,1,2,2,0,0,2,2},{0,0,0,0,0,0,0,0,0,0,0,0,2,1,1,2},
    {0,0,0,2,0,0,0,1,0,0,0,2,0,0,0,1},{0,2,2,2,1,2,2,2,0,2,2,2,1,2,2,2},{0,1,0,1,2,2,2,2,2,2,2,2,2,2,2,2},
    {0,1,1,1,2,0,1,1,2,2,0,1,2,2,2,0}}};

constexpr std::array<uint8_t, 64> kAnchor2 = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8,  2,  2,  8,  8,  15, 2,  8,  2,  2,  8,  8,  2,  2,
    15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,  2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15};

constexpr std::array<uint8_t, 64> kAnchor3a = {
    3,  3,  15, 15, 8,  3,  15, 15, 8,  8,  6,  6,  6,  5,  3,  3,  3,  3,  8,  15, 3,  3,  6,  10, 5,  8,  8,  6,  8,  5,  15, 15,
    8,  15, 3,  5,  6,  10, 8,  15, 15, 3,  15, 5,  15, 15, 15, 15, 3,  15, 5,  5,  5,  8,  5,  10, 5,  10, 8,  13, 15, 12, 3,  3};

constexpr std::array<uint8_t, 64> kAnchor3b = {
    15, 8,  8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,  15, 8,  15, 3,  15, 8,  15, 8,  3,  15, 6,  10, 15, 15, 10, 8,
    15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15, 3,  6,  6,  8,  15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8};

struct Rgba {
    uint8_t r, g, b, a;
};

int WeightOf(int index, int index_bits) {
    return index_bits == 2 ? kWeights2[index] : index_bits == 3 ? kWeights3[index] : kWeights4[index];
}

int AnchorOf(int subset, int subsets, int partition) {
    if (subset == 0) return 0;
    if (subsets == 2) return kAnchor2[partition];
    return subset == 1 ? kAnchor3a[partition] : kAnchor3b[partition];
}

int SubsetOf(int texel, int subsets, int partition) {
    if (subsets == 1) return 0;
    if (subsets == 2) return kPartition2[partition] >> texel & 1;
    return kPartition3[partition][texel];
}

struct ModeInfo {
    int subsets, partition_bits, rotation_bits, selector_bits, color_bits, alpha_bits, endpoint_pbits, shared_pbits,
        index_bits, index2_bits;
};

constexpr ModeInfo kModes[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
};

} // namespace

void DecodeBc7Block(const uint8_t block[16], uint8_t rgba[16 * 4]) {
    std::memset(rgba, 0, 64);
    BitReader bits(block);
    int mode = 0;
    while (mode < 8 && bits.Take(1) == 0) ++mode;
    if (mode >= 8) return; // reserved: transparent black
    const ModeInfo& m = kModes[mode];
    const int partition = int(bits.Take(m.partition_bits));
    const int rotation = int(bits.Take(m.rotation_bits));
    const int selector = int(bits.Take(m.selector_bits));
    const int endpoints = m.subsets * 2;

    // Endpoint bits are stored channel by channel across every endpoint, then the p-bits.
    std::array<std::array<int, 4>, 6> value{};
    for (int c = 0; c < 3; ++c) {
        for (int e = 0; e < endpoints; ++e) value[e][c] = int(bits.Take(m.color_bits));
    }
    for (int e = 0; e < endpoints; ++e) value[e][3] = m.alpha_bits ? int(bits.Take(m.alpha_bits)) : 255;
    std::array<int, 6> pbit{};
    if (m.endpoint_pbits) {
        for (int e = 0; e < endpoints; ++e) pbit[e] = int(bits.Take(1));
    } else if (m.shared_pbits) {
        for (int s = 0; s < m.subsets; ++s) pbit[s * 2] = pbit[s * 2 + 1] = int(bits.Take(1));
    }
    const bool has_pbit = m.endpoint_pbits || m.shared_pbits;
    std::array<Rgba, 6> endpoint{};
    for (int e = 0; e < endpoints; ++e) {
        int channel[4];
        for (int c = 0; c < 4; ++c) {
            if (c == 3 && !m.alpha_bits) {
                channel[c] = 255;
                continue;
            }
            const int width = c == 3 ? m.alpha_bits : m.color_bits;
            channel[c] = has_pbit ? Expand(uint32_t(value[e][c] << 1 | pbit[e]), width + 1) : Expand(uint32_t(value[e][c]), width);
        }
        endpoint[e] = {uint8_t(channel[0]), uint8_t(channel[1]), uint8_t(channel[2]), uint8_t(channel[3])};
    }

    std::array<int, 16> index{};
    for (int t = 0; t < 16; ++t) {
        const int anchor = AnchorOf(SubsetOf(t, m.subsets, partition), m.subsets, partition);
        index[t] = int(bits.Take(t == anchor ? m.index_bits - 1 : m.index_bits));
    }
    std::array<int, 16> index2{};
    if (m.index2_bits) {
        for (int t = 0; t < 16; ++t) index2[t] = int(bits.Take(t == 0 ? m.index2_bits - 1 : m.index2_bits));
    }

    for (int t = 0; t < 16; ++t) {
        const int s = SubsetOf(t, m.subsets, partition);
        const Rgba& e0 = endpoint[s * 2];
        const Rgba& e1 = endpoint[s * 2 + 1];
        int color_weight = WeightOf(index[t], m.index_bits);
        int alpha_weight = color_weight;
        if (m.index2_bits) {
            alpha_weight = WeightOf(index2[t], m.index2_bits);
            if (selector) std::swap(color_weight, alpha_weight);
        }
        int r = Lerp(e0.r, e1.r, color_weight), g = Lerp(e0.g, e1.g, color_weight), b = Lerp(e0.b, e1.b, color_weight);
        int a = m.alpha_bits ? Lerp(e0.a, e1.a, alpha_weight) : 255;
        if (rotation == 1) std::swap(a, r);
        else if (rotation == 2) std::swap(a, g);
        else if (rotation == 3) std::swap(a, b);
        rgba[t * 4 + 0] = uint8_t(r);
        rgba[t * 4 + 1] = uint8_t(g);
        rgba[t * 4 + 2] = uint8_t(b);
        rgba[t * 4 + 3] = uint8_t(a);
    }
}

namespace {

uint32_t ReadU32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

struct Surface {
    int width = 0;
    int height = 0;
    uint32_t tiling = 0;
    size_t data_offset = 0;
    size_t end = 0;
};

constexpr size_t kGnfHeader = 0x100;

std::optional<Surface> FindSurface(std::span<const uint8_t> pack, size_t index, std::string* error) {
    const auto fail = [&](const char* why) {
        if (error) *error = why;
        return std::optional<Surface>();
    };
    if (pack.size() < 0x20 || std::memcmp(pack.data(), "FTEX", 4) != 0) return fail("not an FTX pack");
    const uint32_t count = ReadU32(pack.data() + 12);
    if (index >= count) return fail("no such surface in the pack");
    size_t section = ReadU32(pack.data() + 8);
    for (size_t i = 0;; ++i) {
        if (section + 12 > pack.size() || std::memcmp(pack.data() + section, "FTX0", 4) != 0) return fail("no FTX0 section");
        const size_t gnf = section + ReadU32(pack.data() + section + 8);
        if (gnf + kGnfHeader > pack.size() || std::memcmp(pack.data() + gnf, "GNF ", 4) != 0) return fail("no GNF surface");
        const uint8_t* descriptor = pack.data() + gnf + 16;
        const uint32_t dimensions = ReadU32(descriptor + 8);
        Surface surface;
        surface.width = int(dimensions & 0x3fff) + 1;
        surface.height = int(dimensions >> 14 & 0x3fff) + 1;
        surface.tiling = ReadU32(descriptor + 12) >> 20 & 0x1f;
        surface.data_offset = gnf + kGnfHeader;
        if (surface.width % 4 || surface.height % 4) return fail("odd surface size");
        surface.end = surface.data_offset + size_t(surface.width / 4) * size_t(surface.height / 4) * kBlockBytes;
        if (surface.end > pack.size()) return fail("surface truncated");
        if (i == index) {
            if ((ReadU32(descriptor + 4) >> 20 & 0x3f) != kDfmtBc7) return fail("not a BC7 surface");
            if (surface.tiling != kTilingLinear && surface.tiling != kTilingThin1d) return fail("unsupported tiling");
            return surface;
        }
        section = surface.end;
    }
}

size_t BlockOffset(int width, uint32_t tiling, int bx, int by) {
    const int pitch_blocks = width / 4;
    if (tiling == kTilingLinear) return (size_t(by) * size_t(pitch_blocks) + size_t(bx)) * kBlockBytes;
    // 1D thin tiling: 8x8-block micro tiles in Morton order.
    const size_t tile = size_t(by / 8) * size_t(pitch_blocks / 8) + size_t(bx / 8);
    const int x = bx & 7, y = by & 7;
    const int micro = (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
    return tile * 64 * kBlockBytes + size_t(micro) * kBlockBytes;
}

} // namespace

std::optional<FtxSurfaceLayout> FtxLayoutOf(std::span<const uint8_t> pack, size_t index, std::string* error) {
    const std::optional<Surface> surface = FindSurface(pack, index, error);
    if (!surface) return std::nullopt;
    return FtxSurfaceLayout{surface->width, surface->height, surface->tiling, surface->data_offset};
}

size_t FtxBlockOffset(const FtxSurfaceLayout& layout, int bx, int by) {
    return layout.data_offset + BlockOffset(layout.width, layout.tiling, bx, by);
}

namespace {

int QuantizeWithPbit(int value, int pbit) {
    return std::clamp((value - pbit + 1) / 2, 0, 127);
}

} // namespace

namespace {

// Mode 6: one subset, RGBA endpoints of 7 bits plus a p-bit each, 4-bit indices along the texels' principal axis.
void EncodeBc7Mode6(const uint8_t rgba[16 * 4], uint8_t block[16]) {
    std::array<float, 4> mean{};
    for (int t = 0; t < 16; ++t) {
        for (int c = 0; c < 4; ++c) mean[c] += rgba[t * 4 + c] / 16.0f;
    }
    std::array<std::array<float, 4>, 4> covariance{};
    for (int t = 0; t < 16; ++t) {
        for (int a = 0; a < 4; ++a) {
            for (int b = 0; b < 4; ++b) covariance[a][b] += (rgba[t * 4 + a] - mean[a]) * (rgba[t * 4 + b] - mean[b]);
        }
    }
    std::array<float, 4> axis{1.0f, 1.0f, 1.0f, 1.0f};
    for (int iteration = 0; iteration < 8; ++iteration) {
        std::array<float, 4> next{};
        for (int a = 0; a < 4; ++a) {
            for (int b = 0; b < 4; ++b) next[a] += covariance[a][b] * axis[b];
        }
        const float length = std::sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2] + next[3] * next[3]);
        if (length < 1e-6f) break;
        for (int c = 0; c < 4; ++c) axis[c] = next[c] / length;
    }
    float low = 0.0f, high = 0.0f;
    for (int t = 0; t < 16; ++t) {
        float projection = 0.0f;
        for (int c = 0; c < 4; ++c) projection += (rgba[t * 4 + c] - mean[c]) * axis[c];
        low = std::min(low, projection);
        high = std::max(high, projection);
    }
    std::array<int, 4> lo{}, hi{};
    for (int c = 0; c < 4; ++c) {
        lo[c] = std::clamp(int(std::lround(mean[c] + low * axis[c])), 0, 255);
        hi[c] = std::clamp(int(std::lround(mean[c] + high * axis[c])), 0, 255);
    }
    std::array<std::array<int, 4>, 2> endpoint{};
    std::array<int, 2> pbit{};
    for (int e = 0; e < 2; ++e) {
        const std::array<int, 4>& target = e == 0 ? lo : hi;
        int best_error = INT32_MAX;
        for (int p = 0; p < 2; ++p) {
            int error = 0;
            std::array<int, 4> q{};
            for (int c = 0; c < 4; ++c) {
                q[c] = QuantizeWithPbit(target[c], p);
                const int d = (q[c] << 1 | p) - target[c];
                error += d * d;
            }
            if (error < best_error) {
                best_error = error;
                endpoint[e] = q;
                pbit[e] = p;
            }
        }
    }
    std::array<std::array<int, 4>, 16> palette{};
    for (int i = 0; i < 16; ++i) {
        for (int c = 0; c < 4; ++c) {
            palette[i][c] = Lerp(endpoint[0][c] << 1 | pbit[0], endpoint[1][c] << 1 | pbit[1], kWeights4[i]);
        }
    }
    std::array<int, 16> index{};
    for (int t = 0; t < 16; ++t) {
        int best = 0, best_error = INT32_MAX;
        for (int i = 0; i < 16; ++i) {
            int error = 0;
            for (int c = 0; c < 4; ++c) {
                const int d = palette[i][c] - rgba[t * 4 + c];
                error += d * d;
            }
            if (error < best_error) {
                best_error = error;
                best = i;
            }
        }
        index[t] = best;
    }
    if (index[0] >= 8) { // the anchor index has no top bit: swap the endpoints and mirror every index
        std::swap(endpoint[0], endpoint[1]);
        std::swap(pbit[0], pbit[1]);
        for (int& i : index) i = 15 - i;
    }
    std::memset(block, 0, 16);
    int at = 0;
    const auto put = [&](uint32_t value, int count) {
        for (int i = 0; i < count; ++i, ++at) block[at >> 3] |= uint8_t((value >> i & 1) << (at & 7));
    };
    put(1u << 6, 7);
    for (int c = 0; c < 4; ++c) {
        put(uint32_t(endpoint[0][c]), 7);
        put(uint32_t(endpoint[1][c]), 7);
    }
    put(uint32_t(pbit[0]), 1);
    put(uint32_t(pbit[1]), 1);
    for (int t = 0; t < 16; ++t) put(uint32_t(index[t]), t == 0 ? 3 : 4);
}

// How much a texel's colour counts in a fit: a transparent texel's colour is only seen through filtering beside it.
float Visibility(int alpha) { return (float(alpha) + 8.0f) / 263.0f; }

// Mode 5: one subset, 7-bit colour endpoints with 2-bit indices, and alpha apart: 8-bit endpoints with 2-bit indices,
// so the lowest and highest alpha of the block come back exactly.
void EncodeBc7Mode5(const uint8_t rgba[16 * 4], uint8_t block[16]) {
    std::array<float, 3> mean{};
    float weight = 0.0f;
    int alpha_low = 255, alpha_high = 0;
    for (int t = 0; t < 16; ++t) {
        alpha_low = std::min<int>(alpha_low, rgba[t * 4 + 3]);
        alpha_high = std::max<int>(alpha_high, rgba[t * 4 + 3]);
        const float w = Visibility(rgba[t * 4 + 3]);
        for (int c = 0; c < 3; ++c) mean[c] += rgba[t * 4 + c] * w;
        weight += w;
    }
    for (float& m : mean) m = weight > 0.0f ? m / weight : 0.0f;
    std::array<std::array<float, 3>, 3> covariance{};
    for (int t = 0; t < 16; ++t) {
        const float w = Visibility(rgba[t * 4 + 3]);
        for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) covariance[a][b] += w * (rgba[t * 4 + a] - mean[a]) * (rgba[t * 4 + b] - mean[b]);
        }
    }
    std::array<float, 3> axis{1.0f, 1.0f, 1.0f};
    for (int iteration = 0; iteration < 8; ++iteration) {
        std::array<float, 3> next{};
        for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) next[a] += covariance[a][b] * axis[b];
        }
        const float length = std::sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2]);
        if (length < 1e-6f) break;
        for (int c = 0; c < 3; ++c) axis[c] = next[c] / length;
    }
    float low = 0.0f, high = 0.0f;
    for (int t = 0; t < 16; ++t) {
        float projection = 0.0f;
        for (int c = 0; c < 3; ++c) projection += (rgba[t * 4 + c] - mean[c]) * axis[c];
        low = std::min(low, projection);
        high = std::max(high, projection);
    }
    std::array<std::array<int, 3>, 2> endpoint{};
    for (int c = 0; c < 3; ++c) {
        endpoint[0][c] = std::clamp(int(std::lround((mean[c] + low * axis[c]) * 127.0f / 255.0f)), 0, 127);
        endpoint[1][c] = std::clamp(int(std::lround((mean[c] + high * axis[c]) * 127.0f / 255.0f)), 0, 127);
    }
    std::array<int, 2> alpha{alpha_low, alpha_high};
    std::array<int, 16> color_index{}, alpha_index{};
    for (int t = 0; t < 16; ++t) {
        int best = 0, best_error = INT32_MAX;
        for (int i = 0; i < 4; ++i) {
            int error = 0;
            for (int c = 0; c < 3; ++c) {
                const int d = Lerp(Expand(uint32_t(endpoint[0][c]), 7), Expand(uint32_t(endpoint[1][c]), 7), kWeights2[i]) - rgba[t * 4 + c];
                error += d * d;
            }
            if (error < best_error) best_error = error, best = i;
        }
        color_index[t] = best;
        best = 0, best_error = INT32_MAX;
        for (int i = 0; i < 4; ++i) {
            const int error = std::abs(Lerp(alpha[0], alpha[1], kWeights2[i]) - rgba[t * 4 + 3]);
            if (error < best_error) best_error = error, best = i;
        }
        alpha_index[t] = best;
    }
    if (color_index[0] >= 2) { // the anchor index has no top bit: swap the endpoints and mirror every index
        std::swap(endpoint[0], endpoint[1]);
        for (int& i : color_index) i = 3 - i;
    }
    if (alpha_index[0] >= 2) {
        std::swap(alpha[0], alpha[1]);
        for (int& i : alpha_index) i = 3 - i;
    }
    std::memset(block, 0, 16);
    int at = 0;
    const auto put = [&](uint32_t value, int count) {
        for (int i = 0; i < count; ++i, ++at) block[at >> 3] |= uint8_t((value >> i & 1) << (at & 7));
    };
    put(1u << 5, 6);
    put(0, 2); // no rotation
    for (int c = 0; c < 3; ++c) {
        put(uint32_t(endpoint[0][c]), 7);
        put(uint32_t(endpoint[1][c]), 7);
    }
    put(uint32_t(alpha[0]), 8);
    put(uint32_t(alpha[1]), 8);
    for (int t = 0; t < 16; ++t) put(uint32_t(color_index[t]), t == 0 ? 1 : 2);
    for (int t = 0; t < 16; ++t) put(uint32_t(alpha_index[t]), t == 0 ? 1 : 2);
}

// How far a decoded block is from the texels, counting a texel's colour only as far as it is seen.
int64_t BlockError(const uint8_t rgba[16 * 4], const uint8_t decoded[16 * 4]) {
    int64_t error = 0;
    for (int t = 0; t < 16; ++t) {
        const int alpha = rgba[t * 4 + 3];
        const int da = int(decoded[t * 4 + 3]) - alpha;
        error += int64_t(da) * da * 4 * 255;
        for (int c = 0; c < 3; ++c) {
            const int d = int(decoded[t * 4 + c]) - int(rgba[t * 4 + c]);
            error += int64_t(d) * d * (alpha + 8);
        }
    }
    return error;
}

// Whether every fully transparent texel stays fully transparent, and every opaque one all but opaque.
bool KeepsAlphaExtremes(const uint8_t rgba[16 * 4], const uint8_t decoded[16 * 4]) {
    for (int t = 0; t < 16; ++t) {
        const int alpha = rgba[t * 4 + 3];
        if ((alpha == 0 && decoded[t * 4 + 3] != 0) || (alpha == 255 && decoded[t * 4 + 3] < 240)) return false;
    }
    return true;
}

} // namespace

// Opaque blocks take mode 6. A block with transparency takes the mode that keeps its transparent texels transparent and
// its opaque ones opaque, then the one closer to what is seen: mode 6 shares its indices between colour and alpha, mode 5
// keeps them apart.
void EncodeBc7Block(const uint8_t rgba[16 * 4], uint8_t block[16]) {
    EncodeBc7Mode6(rgba, block);
    bool opaque = true;
    for (int t = 0; t < 16; ++t) opaque = opaque && rgba[t * 4 + 3] == 255;
    if (opaque) return;
    uint8_t separate[16], decoded6[64], decoded5[64];
    EncodeBc7Mode5(rgba, separate);
    DecodeBc7Block(block, decoded6);
    DecodeBc7Block(separate, decoded5);
    const bool keeps6 = KeepsAlphaExtremes(rgba, decoded6), keeps5 = KeepsAlphaExtremes(rgba, decoded5);
    const bool take5 = keeps5 != keeps6 ? keeps5 : BlockError(rgba, decoded5) < BlockError(rgba, decoded6);
    if (take5) std::memcpy(block, separate, 16);
}

size_t FtxSurfaceCount(std::span<const uint8_t> pack) {
    return pack.size() >= 0x20 && std::memcmp(pack.data(), "FTEX", 4) == 0 ? ReadU32(pack.data() + 12) : 0;
}

std::optional<FtxSurface> ReadFtxSurface(std::vector<uint8_t> pack, size_t index, std::string* error) {
    const std::optional<Surface> found = FindSurface(pack, index, error);
    if (!found) return std::nullopt;
    FtxSurface surface;
    surface.width_ = found->width;
    surface.height_ = found->height;
    surface.tiling_ = found->tiling;
    surface.data_offset_ = found->data_offset;
    surface.pack_ = std::move(pack);
    surface.slot_.assign(size_t(found->width / 4) * size_t(found->height / 4), -1);
    return surface;
}

const uint8_t* FtxSurface::Block(int bx, int by) const {
    const size_t index = size_t(by) * size_t(width_ / 4) + size_t(bx);
    if (slot_[index] < 0) {
        slot_[index] = int32_t(decoded_.size() / 64);
        decoded_.resize(decoded_.size() + 64);
        DecodeBc7Block(pack_.data() + data_offset_ + BlockOffset(width_, tiling_, bx, by), decoded_.data() + decoded_.size() - 64);
    }
    return decoded_.data() + size_t(slot_[index]) * 64;
}

std::array<uint8_t, 4> FtxSurface::Texel(int x, int y) const {
    x = std::clamp(x, 0, width_ - 1);
    y = std::clamp(y, 0, height_ - 1);
    const uint8_t* texel = Block(x / 4, y / 4) + ((y & 3) * 4 + (x & 3)) * 4;
    return {texel[0], texel[1], texel[2], texel[3]};
}

std::optional<FtxTexture> DecodeFtx(std::span<const uint8_t> bytes, std::string* error) {
    const std::optional<Surface> surface = FindSurface(bytes, 0, error);
    if (!surface) return std::nullopt;
    FtxTexture texture;
    texture.width = surface->width;
    texture.height = surface->height;
    texture.rgba.assign(size_t(surface->width) * size_t(surface->height) * 4, 0);
    uint8_t texels[64];
    for (int by = 0; by < surface->height / 4; ++by) {
        for (int bx = 0; bx < surface->width / 4; ++bx) {
            DecodeBc7Block(bytes.data() + surface->data_offset + BlockOffset(surface->width, surface->tiling, bx, by), texels);
            for (int t = 0; t < 16; ++t) {
                const size_t dst = (size_t(by * 4 + (t >> 2)) * size_t(surface->width) + size_t(bx * 4 + (t & 3))) * 4;
                std::memcpy(texture.rgba.data() + dst, texels + size_t(t) * 4, 4);
            }
        }
    }
    return texture;
}

} // namespace dcp
