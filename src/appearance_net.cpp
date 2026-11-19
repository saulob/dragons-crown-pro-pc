#include "appearance_net.h"

#include <algorithm>
#include <cstdio>

namespace dcp::appearance_net {
namespace {

constexpr PartName kFighter[] = {{1, "armor"}, {2, "trim"},   {3, "leather"}, {4, "eye"},     {5, "crest"},  {6, "crest_accent"},
                                 {7, "horns"}, {8, "helmet"}, {9, "face"},    {10, "sword"}, {11, "shield"}, {12, "shield_accent"}};
constexpr PartName kAmazon[] = {{1, "hair"}, {2, "skin"}, {3, "tattoo"}, {4, "eye"}, {5, "feather"}, {6, "armor"}, {7, "bikini"}, {8, "axe"}};
constexpr PartName kElf[] = {{1, "hood"},    {2, "hair"},    {3, "skin"},  {4, "eye"},    {5, "tunic"},   {6, "belt"},    {7, "collar"},
                             {8, "shorts"}, {9, "gloves"}, {10, "laces"}, {11, "boots"}, {12, "quiver"}, {13, "arrows"}, {14, "bow"}};
constexpr PartName kDwarf[] = {{1, "beard"},   {2, "cape"},         {3, "skin"}, {4, "helmet"}, {5, "eye"},
                               {6, "feather"}, {7, "wrist_guards"}, {8, "belt"}, {9, "boots"},  {10, "hammers"}};
constexpr PartName kSorceress[] = {{1, "hair"}, {2, "skin"},  {3, "eye"},       {4, "dress"},  {5, "bodice"},       {6, "lace"},
                                   {7, "frills"}, {8, "hat"}, {9, "hat_band"}, {10, "boots"}, {11, "accessories"}, {12, "staff"}};
constexpr PartName kWizard[] = {{1, "robe"}, {2, "hood"}, {3, "pendant"},  {4, "hair"},  {5, "skin"},
                                {6, "gloves"}, {7, "belt"}, {8, "trousers"}, {9, "boots"}, {10, "staff"}};

bool Changes(const NetPart& part) { return part.hidden || part.color.has_value(); }

} // namespace

std::span<const PartName> PartTable(int klass) {
    switch (klass) {
    case 0: return kFighter;
    case 1: return kAmazon;
    case 2: return kElf;
    case 3: return kDwarf;
    case 4: return kSorceress;
    case 5: return kWizard;
    }
    return {};
}

std::string_view PartKey(int klass, uint8_t id) {
    for (const PartName& part : PartTable(klass))
        if (part.id == id) return part.key;
    return {};
}

int PartId(int klass, std::string_view key) {
    for (const PartName& part : PartTable(klass))
        if (part.key == key) return part.id;
    return -1;
}

const char* RefusalText(Refusal refusal) {
    switch (refusal) {
    case Refusal::kNone: return "valid";
    case Refusal::kEmpty: return "empty";
    case Refusal::kTooLarge: return "too large";
    case Refusal::kSchema: return "unknown schema";
    case Refusal::kClass: return "invalid class";
    case Refusal::kBase: return "invalid base colour";
    case Refusal::kCount: return "too many parts";
    case Refusal::kTruncated: return "truncated";
    case Refusal::kPart: return "part not of this class";
    case Refusal::kDuplicate: return "duplicate part";
    case Refusal::kFlags: return "invalid part flags";
    case Refusal::kTrailing: return "trailing bytes";
    }
    return "invalid";
}

std::optional<Recipe> Normalized(Recipe recipe) {
    if (recipe.klass >= kClassCount || recipe.base >= kBaseCount) return std::nullopt;
    std::erase_if(recipe.parts, [&](const NetPart& part) { return !Changes(part) || PartKey(recipe.klass, part.id).empty(); });
    std::stable_sort(recipe.parts.begin(), recipe.parts.end(), [](const NetPart& a, const NetPart& b) { return a.id < b.id; });
    const auto same = [](const NetPart& a, const NetPart& b) { return a.id == b.id; };
    recipe.parts.erase(std::unique(recipe.parts.begin(), recipe.parts.end(), same), recipe.parts.end());
    if (recipe.parts.size() > kMaxParts) return std::nullopt;
    return recipe;
}

std::vector<uint8_t> Encode(const Recipe& recipe) {
    const std::optional<Recipe> normal = Normalized(recipe);
    if (!normal || *normal != recipe) return {};
    std::vector<uint8_t> out = {kSchema, recipe.klass, recipe.base, uint8_t(recipe.parts.size())};
    for (const NetPart& part : recipe.parts) {
        out.push_back(part.id);
        out.push_back(uint8_t((part.hidden ? kHidden : 0) | (part.color ? kColored : 0)));
        if (part.color) out.insert(out.end(), part.color->begin(), part.color->end());
    }
    return out;
}

std::optional<Recipe> Decode(std::span<const uint8_t> bytes, Refusal* why) {
    const auto refuse = [&](Refusal refusal) -> std::optional<Recipe> {
        if (why) *why = refusal;
        return std::nullopt;
    };
    if (bytes.empty()) return refuse(Refusal::kEmpty);
    if (bytes.size() > kMaxBytes) return refuse(Refusal::kTooLarge);
    if (bytes[0] != kSchema) return refuse(Refusal::kSchema);
    if (bytes.size() < kHeaderBytes) return refuse(Refusal::kTruncated);
    Recipe recipe;
    recipe.klass = bytes[1];
    recipe.base = bytes[2];
    if (recipe.klass >= kClassCount) return refuse(Refusal::kClass);
    if (recipe.base >= kBaseCount) return refuse(Refusal::kBase);
    const size_t count = bytes[3];
    if (count > kMaxParts) return refuse(Refusal::kCount);
    size_t at = kHeaderBytes;
    for (size_t i = 0; i < count; ++i) {
        if (bytes.size() - at < 2) return refuse(Refusal::kTruncated);
        NetPart part;
        part.id = bytes[at];
        const uint8_t flags = bytes[at + 1];
        at += 2;
        if (PartKey(recipe.klass, part.id).empty()) return refuse(Refusal::kPart);
        if (!flags || (flags & ~(kHidden | kColored))) return refuse(Refusal::kFlags);
        part.hidden = flags & kHidden;
        if (flags & kColored) {
            if (bytes.size() - at < 3) return refuse(Refusal::kTruncated);
            part.color = std::array<uint8_t, 3>{bytes[at], bytes[at + 1], bytes[at + 2]};
            at += 3;
        }
        for (const NetPart& seen : recipe.parts)
            if (seen.id == part.id) return refuse(Refusal::kDuplicate);
        recipe.parts.push_back(part);
    }
    if (at != bytes.size()) return refuse(Refusal::kTrailing);
    std::sort(recipe.parts.begin(), recipe.parts.end(), [](const NetPart& a, const NetPart& b) { return a.id < b.id; });
    if (why) *why = Refusal::kNone;
    return recipe;
}

uint64_t Hash(const Recipe& recipe) {
    const std::optional<Recipe> normal = Normalized(recipe);
    const std::vector<uint8_t> bytes = normal ? Encode(*normal) : std::vector<uint8_t>{};
    uint64_t hash = 1469598103934665603ull;
    for (const uint8_t byte : bytes) hash = (hash ^ byte) * 1099511628211ull;
    return hash;
}

std::string ShortHash(uint64_t hash) {
    char text[9];
    std::snprintf(text, sizeof(text), "%08X", unsigned(hash ^ hash >> 32));
    return text;
}

std::string_view ClassName(int klass) {
    static constexpr std::string_view kNames[] = {"Fighter", "Amazon", "Elf", "Dwarf", "Sorceress", "Wizard"};
    return klass >= 0 && klass < kClassCount ? kNames[klass] : "class?";
}

} // namespace dcp::appearance_net
