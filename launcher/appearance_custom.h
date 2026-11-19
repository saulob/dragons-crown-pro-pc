// Custom appearance: per-part recolouring and hiding laid over a native colour preset. Parts are texture-space regions
// learned from the class's five native atlases (texels the in-game model samples, grouped by how their colour changes
// between presets), so one region map drives the Editor preview, the click selection and the in-game atlas.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "fighter_bare_head.h"
#include "game_mbs.h"
#include "save_data.h"

namespace dcp {

inline constexpr int kRegionCount = 8;

// A part painted into the texels of other pieces. kTattooInk tells its texels apart by
// how their colour changes between the presets; kPainted takes every texel of its pieces that is in `piece_regions`;
// kDyed takes pale texels the presets tint (lace and frills sewn onto skin pieces); kSkinTone takes texels whose colour
// follows the part's own regions through every preset (bare skin clustered with cloth of a similar shade); kSteady takes
// texels every preset paints alike, away from the piece's outline (eyes inside a helmet the presets recolour); kUndyed
// takes the patches every preset paints alike for their brightness (a leather belt on a tunic the presets recolour);
// kHorn takes the Fighter's horns, which the game draws into its helmet sprites (fighter_horns.h).
enum class PaintedMark { kNone, kTattooInk, kPainted, kDyed, kSkinTone, kSteady, kUndyed, kHorn };

struct Rgb {
    uint8_t r = 0, g = 0, b = 0;
    bool operator==(const Rgb&) const = default;
};

struct CustomPart {
    std::string_view key;   // stored in the sidecar
    std::string_view label; // shown
    uint16_t regions = 0;   // bit r set: region r + 1 belongs to the part
    bool reveals_unpainted = false; // hiding it can show artwork the game never draws uncovered
    std::span<const uint32_t> pieces = {}; // MBS piece ids drawn only by this part, for parts the regions cannot isolate
    uint16_t piece_regions = 0;            // when set, only the pieces' texels in these regions
    PaintedMark mark = PaintedMark::kNone; // when set, only the pieces' texels the mark is painted on
    std::pair<uint32_t, uint32_t> piece_range = {0, 0}; // first and last id of a run of pieces, with `pieces`
    int8_t weapon = -1; // index into WeaponTexturesOf(klass): the part is that whole default weapon texture
    // Pieces painted with other parts: the part takes the texels whose colour follows its own through every preset.
    std::span<const uint32_t> tone_pieces = {};
    // Pieces painted with other parts: the part takes the texels whose colour strays from their owner's between presets.
    std::span<const uint32_t> dyed_pieces = {};
    // Hidden, the part is painted with this part's colour, shaded like it: the bare head the game never draws.
    std::string_view bare_underlay = {};
    // kSkinTone without regions: the material's colour in each preset, one run of kNativeColorCount per material.
    std::span<const Rgb> tone = {};
    // Pieces another part draws whole, where this part's material is painted too: it takes their texels whose colour
    // follows `tone` through every preset (the face seen through a helmet's visor).
    std::pair<uint32_t, uint32_t> tone_range = {0, 0};
    bool encloses = false; // also takes, on its own pieces, the texels it mostly surrounds (highlights inside a buckle)
    bool outlined = false; // also takes the dark outline it is drawn with
    bool accent = false; // with `weapon`: the texture's blue band, which the weapon's other part leaves
    // Also takes the light lines painted on this part, lighter than the part around them in every preset (a cuff's trim).
    std::string_view ridges_of = {};
    bool OwnsPiece(uint32_t id) const {
        return (id >= piece_range.first && id <= piece_range.second && piece_range.second) ||
               std::find(pieces.begin(), pieces.end(), id) != pieces.end();
    }
    bool HasPieces() const { return !pieces.empty() || piece_range.second != 0; }
};

std::string PartLabel(CharacterClass klass, std::string_view key);

std::span<const CustomPart> CustomPartsOf(CharacterClass klass);
const CustomPart* FindCustomPart(CharacterClass klass, std::string_view key);
const CustomPart* PartOfRegion(CharacterClass klass, uint8_t region);

// A class's default weapon textures (the equipped item is not read) and the model texture id each is bound to.
struct WeaponTexture {
    std::string_view name; // ARMS01, SHIELD01
    uint8_t texture;
};
std::span<const WeaponTexture> WeaponTexturesOf(CharacterClass klass);
std::string WeaponAtlasName(CharacterClass klass, const WeaponTexture& weapon); // PLAYER/FIGHTER00_ARMS01.FTX
const CustomPart* WeaponPartOf(CharacterClass klass, size_t weapon);
const CustomPart* WeaponPartOfTexture(CharacterClass klass, uint8_t texture);

// An opaque colour bright and saturated enough to read on any part; the same generator gives the same colours.
template <typename Generator>
Rgb RandomVisibleColor(Generator& generator);
// The colour after `current` from the generator, never equal to it.
template <typename Generator>
Rgb NextRandomColor(Generator& generator, std::optional<Rgb> current);
Rgb ColorFromHsv(float hue, float saturation, float value);

struct PartLook {
    std::string key;
    std::optional<Rgb> color;
    bool hidden = false;
    bool operator==(const PartLook&) const = default;
};

struct CustomLook {
    bool enabled = false; // Custom chosen; disabled keeps the edits but shows the native preset
    uint8_t base = 0;     // the native preset the edits are drawn over
    std::vector<PartLook> parts;
    bool operator==(const CustomLook&) const = default;
    const PartLook* Find(std::string_view key) const;
    PartLook& Edit(std::string_view key);
    void Tidy(); // drops parts left with no change
    // Only a part the user changed is an override; the colours learned from the atlas are never stored here.
    size_t OverrideCount() const;
    void SetPartColor(std::string_view key, Rgb color);
    void SetPartHidden(std::string_view key, bool hidden);
    void ResetPart(std::string_view key);
    void ResetOverrides();
    // A new colour for every part of the class; visibility, the base and other keys stay.
    template <typename Generator>
    void RandomizeColors(CharacterClass klass, Generator& generator);
    bool active() const { return enabled && OverrideCount() > 0; }
};

struct CustomKey {
    std::string title_id;
    std::string save_dir; // SAVEDATA00
    int data_slot = 0;
    int character_slot = 0;
    CharacterClass klass = CharacterClass::kFighter;
    bool operator==(const CustomKey&) const = default;
    std::string Text() const;
};

// user/character_appearance.dat. Written atomically; an unreadable or newer file is never overwritten.
class CustomStore {
public:
    explicit CustomStore(std::filesystem::path path) : path_(std::move(path)) {}
    bool Load();
    bool Save() const;
    const std::filesystem::path& path() const { return path_; }
    const std::string& error() const { return error_; }

    std::optional<CustomLook> Get(const CustomKey& key) const;
    void Set(const CustomKey& key, const CustomLook& look); // an empty, disabled look removes the entry
    const std::vector<std::pair<CustomKey, CustomLook>>& entries() const { return entries_; }
    bool operator==(const CustomStore& other) const { return entries_ == other.entries_; }

    std::vector<uint8_t> Serialize() const;
    bool Parse(std::span<const uint8_t> bytes);

private:
    std::filesystem::path path_;
    std::vector<std::pair<CustomKey, CustomLook>> entries_;
    mutable std::string error_;
};

std::filesystem::path CustomStorePath(const std::filesystem::path& user_dir);

// One log line per saved look that changes between two stores.
std::vector<std::string> DescribeCustomChanges(const CustomStore& before, const CustomStore& after);

using TexelReader = std::function<std::array<uint8_t, 4>(int x, int y)>;

// Region of every texel of one atlas surface: 0 outside the character, 1..kRegionCount inside.
struct RegionMap {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> region;
    // Per texel, a bit n for the n-th piece part when the texel belongs to it alone.
    std::vector<uint16_t> pieces;
    uint8_t At(int x, int y) const {
        return x >= 0 && y >= 0 && x < width && y < height ? region[size_t(y) * size_t(width) + size_t(x)] : 0;
    }
    uint16_t Pieces(int x, int y) const {
        return !pieces.empty() && x >= 0 && y >= 0 && x < width && y < height ? pieces[size_t(y) * size_t(width) + size_t(x)] : 0;
    }
};

inline constexpr size_t kPieceBits = 16;
// The bit of the part's piece set in RegionMap::pieces, or 0 for a part with no pieces of its own.
uint16_t PieceBitOf(CharacterClass klass, const CustomPart& part);

// Tattoo ink: preset 1 shifts it towards blue against preset 0, where the skin around it keeps its hue.
bool TattooInk(const std::array<uint8_t, 4>& preset0, const std::array<uint8_t, 4>& preset1, int threshold);
inline constexpr int kTattooInkCore = 18;
inline constexpr int kTattooInkEdge = 11;

// Dyed fabric: pale in preset 0 (saturation at most `max_saturation` percent) and tinted by the other presets (summed
// chroma shift of at least `min_shift` hundredths). Skin keeps its hue, so its highlights are not dyed.
bool Dyed(std::span<const std::array<uint8_t, 4>> presets, int min_shift, int max_saturation);
inline constexpr int kDyedCoreShift = 10;
inline constexpr int kDyedCoreSaturation = 35;
inline constexpr int kDyedEdgeShift = 6;
inline constexpr int kDyedEdgeSaturation = 45;

// Opponent chroma of a texel, the measure kDyed and kSkinTone compare presets with.
std::pair<float, float> TexelChroma(const std::array<uint8_t, 4>& texel);
// How far a texel's colour strays, in its worst preset, from a reference chroma per preset (hundredths).
float ToneDistance(std::span<const std::array<uint8_t, 4>> presets, std::span<const std::pair<float, float>> reference);
inline constexpr float kSkinToneCore = 12.0f;
inline constexpr float kSkinToneEdge = 18.0f;
inline constexpr int kSteadyCore = 12; // per channel, across the presets
inline constexpr int kSteadyEdge = 20;
inline constexpr int kSteadyLine = 40; // the edge line of a steady part, mixed with the cloth beside it
inline constexpr float kUndyedCore = 18.0f; // hundredths of the texel's brightness, across the presets
inline constexpr float kUndyedEdge = 24.0f;
inline constexpr float kUndyedDark = 20.0f; // darker: the shadow of the cloth, unless every preset paints it the same
inline constexpr float kUndyedExact = 3.0f; // per channel
inline constexpr float kUndyedBlack = 6.0f; // the outline every piece is drawn with
inline constexpr size_t kUndyedPatch = 24; // fewer texels: a shade of the dyed cloth
inline constexpr float kSharedLit = 16.0f; // darker in every preset: too dark to tell its material
inline constexpr int kRidgeReach = 6;      // texels around a ridge texel it is lighter than
inline constexpr float kRidgeCore = 12.0f; // luma above that mean, in every preset
inline constexpr float kRidgeEdge = 4.0f;
inline constexpr size_t kRidgeLength = 20; // fewer core texels: a speck of shading
inline constexpr float kToneClaimLimit = 25.0f;
inline constexpr float kDistinctRange = 60.0f; // a material the presets repaint this far is told by how it shifts
inline constexpr float kPaleLuma = 150.0f;
inline constexpr float kPaleChroma = 22.0f;
inline constexpr float kSkinShadeLuma = 60.0f; // darker: too little colour to tell skin from shadowed metal

// The atlas surfaces the character's body is painted on, and the model texture id each is bound to.
struct BodySurface {
    size_t surface;
    uint8_t texture;
};
std::span<const BodySurface> BodySurfacesOf(CharacterClass klass);

struct SurfaceTexels {
    std::vector<TexelReader> presets; // the surface in each of the five native presets
    int width = 0;
    int height = 0;
    uint8_t texture = 0; // the model texture id that samples it
};

// Marks, in each map, the texels only the given piece sets draw `model` is the in-game model;
// `presets` (per map: presets 0 and 1 at least) is needed only by parts with a painted mark.
void BuildPiecePlanes(CharacterClass klass, const Mbs& model, std::vector<RegionMap>& maps,
                      std::span<const SurfaceTexels> presets = {});
// Learns the regions of a class's body surfaces together, from the five presets' texels the model samples. One map per
// surface, with region ids shared across them. Deterministic.
std::vector<RegionMap> BuildRegionMaps(std::span<const SurfaceTexels> surfaces, const Mbs& model);

// A class's region maps, from `cache_dir` when an earlier build for the same game files (`identity`) is there, else
// learned from the five atlases read through `read` and cached. Nothing when the game files are missing.
using EntryReader = std::function<std::vector<uint8_t>(const std::string& entry)>;
std::optional<std::vector<RegionMap>> ClassRegions(CharacterClass klass, const EntryReader& read,
                                                   const std::filesystem::path& cache_dir, const std::string& identity);
std::string AtlasName(CharacterClass klass, int color);
std::string GameModelName(CharacterClass klass);

std::vector<uint8_t> SerializeRegionMap(const RegionMap& map, std::string_view identity);
std::optional<RegionMap> ParseRegionMap(std::span<const uint8_t> bytes, std::string_view identity, uint8_t max_value = kRegionCount);
std::vector<uint8_t> SerializePiecePlane(int width, int height, const std::vector<uint16_t>& plane, std::string_view identity);
std::optional<std::vector<uint16_t>> ParsePiecePlane(std::span<const uint8_t> bytes, std::string_view identity, int width,
                                                     int height);
// Cache keys: a change to the region learning or to any part table must change these.
inline constexpr std::string_view kRegionCacheVersion = "regions-2";
inline constexpr std::string_view kPieceCacheVersion = "pieces-22";

// How much of the hidden helmet's blurred shading the bare head keeps: metal is shinier than skin.
inline constexpr float kBareContrast = 0.6f;

// Shifts a texel to the target colour by the part's mean luma, so its shading, highlights and alpha stay.
float Luma(float r, float g, float b);
std::array<uint8_t, 4> Recolor(std::array<uint8_t, 4> texel, Rgb target, float part_luma);

// The look turned into a per-texel operation for one surface of the base atlas; the preview and the in-game atlas use
// the same shader, so they match texel for texel.
class LookShader {
public:
    LookShader(CharacterClass klass, const CustomLook& look, const RegionMap& map, const TexelReader& base);
    bool Changes(uint8_t region) const { return region <= kRegionCount && by_region_[region].has_value(); }
    void Apply(uint8_t region, std::array<uint8_t, 4>& texel) const;
    // The same for the texel (x, y) of the map the shader was made for: regions, then piece parts.
    bool ChangesAt(int x, int y) const;
    void ApplyAt(int x, int y, std::array<uint8_t, 4>& texel) const;
    // The mean colour of a part on this surface, or nothing when the part has no texel here.
    std::optional<Rgb> NativeColor(CharacterClass klass, const CustomPart& part) const;

private:
    struct Change {
        std::optional<Rgb> color;
        bool hidden = false;
        float luma = 128.0f;
    };
    // A part painted with its bare underlay: the underlay's lit colour, the blurred shade of each texel, and the
    // underlay's own change.
    struct Bare {
        std::array<float, 3> color{};
        float luma = 128.0f;
        std::optional<Change> underlay;
    };
    // A helmet sprite with the helmet hidden: repainted with the bare head, or cleared.
    struct HelmetPatch {
        AtlasRect rect;
        HelmetSprite kind = HelmetSprite::kClear;
        std::vector<std::array<uint8_t, 4>> texels;
    };
    void PaintBareHead(CharacterClass klass, const CustomPart& part, uint16_t piece_bit, const CustomLook& look, const TexelReader& base);
    void PaintBare(size_t bit, size_t at, std::array<uint8_t, 4>& texel) const;
    void PaintHelmetSprites(CharacterClass klass, const CustomLook& look, const TexelReader& base);
    std::vector<std::array<uint8_t, 4>> ComposeBareHead(const HelmetSpriteRule& rule, uint16_t horn_bit, const TexelReader& base) const;
    const HelmetPatch* PatchAt(int x, int y) const;
    void ApplyOwned(int x, int y, std::array<uint8_t, 4>& texel) const;
    std::array<std::optional<Change>, kRegionCount + 1> by_region_{};
    std::array<std::optional<Change>, kPieceBits> by_piece_{}; // by piece bit
    std::array<std::optional<Bare>, kPieceBits> bare_{};        // by piece bit
    std::vector<uint8_t> shade_;                                // per texel, where a part is painted bare
    std::vector<HelmetPatch> helmet_patches_;
    const RegionMap* map_ = nullptr;
    std::array<std::array<double, 4>, kPieceBits> piece_sums_{};
    std::array<double, kPieceBits> piece_weights_{};
    std::array<std::array<double, 4>, kRegionCount + 1> sums_{}; // alpha-weighted r, g, b, luma; then the weight
    std::array<double, kRegionCount + 1> weights_{};
};

// The look on one default weapon texture: its part recolours or hides every texel of it, but for the blue band its
// accent part takes.
class WeaponShader {
public:
    WeaponShader(CharacterClass klass, const CustomLook& look, size_t weapon, const TexelReader& base, int width, int height);
    void Apply(std::array<uint8_t, 4>& texel) const;
    std::optional<Rgb> NativeColor(bool accent = false) const;

private:
    struct Paint {
        std::optional<Rgb> color;
        bool hidden = false;
        float luma = 128.0f;
        std::array<double, 5> sums{}; // weighted r, g, b, luma, then the weight
    };
    void ApplyPaint(const Paint& paint, std::array<uint8_t, 4>& texel) const;
    Paint main_, accent_;
    bool split_ = false;
};
// How much of a weapon texel is its blue band: 1 inside, 0 outside, between on the band's soft edge.
float AccentWeight(const std::array<uint8_t, 4>& texel);
const CustomPart* WeaponAccentOf(CharacterClass klass, size_t weapon);
bool WeaponChanged(CharacterClass klass, size_t weapon, const CustomLook& look);

// The atlas pack with the look applied: only blocks a changed region touches are re-encoded, the rest of the file is
// byte-identical. `maps[i]` is the map of BodySurfacesOf(klass)[i].
std::optional<std::vector<uint8_t>> CustomizeAtlas(std::span<const uint8_t> pack, std::span<const RegionMap> maps,
                                                   CharacterClass klass, const CustomLook& look, std::string* error = nullptr);
// The same for a default weapon texture pack.
std::optional<std::vector<uint8_t>> CustomizeWeapon(std::span<const uint8_t> pack, CharacterClass klass, size_t weapon,
                                                    const CustomLook& look, std::string* error = nullptr);

template <typename Generator>
Rgb RandomVisibleColor(Generator& generator) {
    const auto unit = [&] { return float(generator() % 10000u) / 10000.0f; };
    const float hue = unit();
    const float saturation = 0.45f + 0.55f * unit();
    const float value = 0.55f + 0.45f * unit();
    return ColorFromHsv(hue, saturation, value);
}

template <typename Generator>
Rgb NextRandomColor(Generator& generator, std::optional<Rgb> current) {
    Rgb next = RandomVisibleColor(generator);
    while (current && next == *current) next = RandomVisibleColor(generator);
    return next;
}

template <typename Generator>
void CustomLook::RandomizeColors(CharacterClass klass, Generator& generator) {
    for (const CustomPart& part : CustomPartsOf(klass)) {
        const PartLook* now = Find(part.key);
        SetPartColor(part.key, NextRandomColor(generator, now ? now->color : std::nullopt));
    }
}

} // namespace dcp
