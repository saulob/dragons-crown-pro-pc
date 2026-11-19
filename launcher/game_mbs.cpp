#include "game_mbs.h"

#include <cmath>
#include <cstring>

#include "cpk_patch.h"

namespace dcp {
namespace {

constexpr size_t kHeaderSize = 0x120;
constexpr size_t kNameOffset = 0x80;
constexpr size_t kNameSize = 32;
constexpr size_t kVersionOffset = 0x14;
constexpr uint32_t kExpectedVersion = 0x72;

struct Section {
    size_t pointer;     // u32 file offset of the section
    size_t stride;      // bytes per record
    size_t count;       // offset of the record count
    size_t count_bytes; // 2 or 4
};

enum SectionId { kColors, kSources, kDestinations, kLayers = 4, kKeys = 6, kTransforms, kFrames, kAnimations, kTracks };

constexpr Section kSections[] = {
    {0xB0, 0x18, 0x54, 2}, {0xB8, 0x30, 0x56, 2}, {0xC0, 0x30, 0x58, 2}, {0xC8, 0x50, 0x5A, 2},
    {0xD0, 0x10, 0x50, 4}, {0xD8, 0x08, 0x5C, 2}, {0xE0, 0x1C, 0x62, 2}, {0xE8, 0x24, 0x5E, 2},
    {0xF0, 0x20, 0x60, 2}, {0xF8, 0x30, 0x64, 2}, {0x100, 0x18, 0x66, 2},
};

class Reader {
public:
    explicit Reader(std::span<const uint8_t> bytes) : bytes_(bytes) {}
    uint8_t U8(size_t at) const { return at < bytes_.size() ? bytes_[at] : 0; }
    uint16_t U16(size_t at) const { return uint16_t(U8(at) | U8(at + 1) << 8); }
    uint32_t U32(size_t at) const { return uint32_t(U16(at)) | uint32_t(U16(at + 2)) << 16; }
    float F32(size_t at) const {
        const uint32_t bits = U32(at);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    MbsColor Color(size_t at) const { return {U8(at), U8(at + 1), U8(at + 2), U8(at + 3)}; }

private:
    std::span<const uint8_t> bytes_;
};

std::optional<Mbs> Fail(std::string message, std::string* error) {
    if (error) *error = std::move(message);
    return std::nullopt;
}

bool Finite(const MbsQuad& quad) {
    if (!std::isfinite(quad.center.x) || !std::isfinite(quad.center.y)) return false;
    for (const MbsPoint& p : quad.corners) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) return false;
    }
    return true;
}

MbsQuad ReadQuad(const Reader& r, size_t at) {
    MbsQuad quad;
    quad.center = {r.F32(at), r.F32(at + 4)};
    for (size_t i = 0; i < 4; ++i) quad.corners[i] = {r.F32(at + 8 + i * 8), r.F32(at + 12 + i * 8)};
    return quad;
}

} // namespace

std::optional<Mbs> ParseMbs(std::span<const uint8_t> bytes, std::string* error) {
    if (bytes.size() < kHeaderSize) return Fail("too small for an FMBS header", error);
    if (std::memcmp(bytes.data(), "FMBS", 4) != 0) return Fail("missing FMBS magic", error);
    const Reader r(bytes);
    if (r.U16(kVersionOffset) != kExpectedVersion) return Fail("unexpected FMBS version", error);

    std::array<size_t, std::size(kSections)> start{};
    std::array<size_t, std::size(kSections)> count{};
    for (size_t i = 0; i < std::size(kSections); ++i) {
        const Section& s = kSections[i];
        start[i] = r.U32(s.pointer);
        count[i] = s.count_bytes == 4 ? r.U32(s.count) : r.U16(s.count);
        if (count[i] && (start[i] < kHeaderSize || start[i] + count[i] * s.stride > bytes.size())) {
            return Fail("section outside the file", error);
        }
    }
    const auto at = [&](SectionId id, size_t index) { return start[id] + index * kSections[id].stride; };

    Mbs mbs;
    const char* name = reinterpret_cast<const char*>(bytes.data() + kNameOffset);
    mbs.name.assign(name, strnlen(name, kNameSize));

    for (size_t i = 0; i < count[kColors]; ++i) {
        const size_t o = at(kColors, i);
        MbsColorQuad quad;
        quad.center = r.Color(o);
        for (size_t c = 0; c < 4; ++c) quad.corners[c] = r.Color(o + 4 + c * 4);
        mbs.colors.push_back(quad);
    }
    for (size_t i = 0; i < count[kSources]; ++i) mbs.sources.push_back(ReadQuad(r, at(kSources, i)));
    for (size_t i = 0; i < count[kDestinations]; ++i) mbs.destinations.push_back(ReadQuad(r, at(kDestinations, i)));
    for (const MbsQuad& quad : mbs.sources) {
        if (!Finite(quad)) return Fail("source quad is not finite", error);
    }
    for (const MbsQuad& quad : mbs.destinations) {
        if (!Finite(quad)) return Fail("destination quad is not finite", error);
    }

    for (size_t i = 0; i < count[kLayers]; ++i) {
        const size_t o = at(kLayers, i);
        MbsLayer layer;
        layer.id = r.U32(o);
        layer.flags = r.U8(o + 5);
        layer.blend = r.U8(o + 6);
        layer.texture = r.U8(o + 7);
        layer.attributes = r.U16(o + 8);
        layer.source = r.U16(o + 10);
        layer.color = r.U16(o + 12);
        layer.destination = r.U16(o + 14);
        if (layer.source >= mbs.sources.size() || layer.color >= mbs.colors.size() ||
            layer.destination >= mbs.destinations.size()) {
            return Fail("layer refers outside its quads", error);
        }
        mbs.layers.push_back(layer);
    }
    for (size_t i = 0; i < count[kKeys]; ++i) {
        const size_t o = at(kKeys, i);
        MbsKey key;
        for (size_t b = 0; b < 4; ++b) key.bounds[b] = r.F32(o + b * 4);
        key.first_layer = r.U32(o + 16);
        key.layer_count = r.U16(o + 22);
        if (size_t(key.first_layer) + key.layer_count > mbs.layers.size()) return Fail("key refers outside its layers", error);
        mbs.keys.push_back(key);
    }
    for (size_t i = 0; i < count[kTransforms]; ++i) {
        const size_t o = at(kTransforms, i);
        MbsTransform t;
        for (size_t b = 0; b < 3; ++b) t.move[b] = r.F32(o + b * 4);
        for (size_t b = 0; b < 3; ++b) t.rotate[b] = r.F32(o + 12 + b * 4);
        t.scale = {r.F32(o + 24), r.F32(o + 28)};
        t.tint = r.Color(o + 32);
        mbs.transforms.push_back(t);
    }
    for (size_t i = 0; i < count[kFrames]; ++i) {
        const size_t o = at(kFrames, i);
        MbsFrame frame{r.U16(o), r.U16(o + 4), r.U16(o + 6)};
        if (frame.key >= mbs.keys.size() || (!mbs.transforms.empty() && frame.transform >= mbs.transforms.size())) {
            return Fail("frame refers outside its keys", error);
        }
        mbs.frames.push_back(frame);
    }
    for (size_t i = 0; i < count[kTracks]; ++i) {
        const size_t o = at(kTracks, i);
        MbsTrack track{r.U16(o), r.U16(o + 2)};
        if (size_t(track.first_frame) + track.frame_count > mbs.frames.size()) return Fail("track refers outside its frames", error);
        mbs.tracks.push_back(track);
    }
    for (size_t i = 0; i < count[kAnimations]; ++i) {
        const size_t o = at(kAnimations, i);
        MbsAnimation animation;
        for (size_t b = 0; b < 4; ++b) animation.bounds[b] = r.F32(o + b * 4);
        const char* label = reinterpret_cast<const char*>(bytes.data() + o + 0x10);
        animation.name.assign(label, strnlen(label, 0x18));
        animation.first_track = r.U16(o + 0x28);
        animation.track_count = r.U8(o + 0x2A);
        if (size_t(animation.first_track) + animation.track_count > mbs.tracks.size()) {
            return Fail("animation refers outside its tracks", error);
        }
        mbs.animations.push_back(std::move(animation));
    }
    return mbs;
}

std::optional<std::vector<uint8_t>> WithSourceCopies(std::span<const uint8_t> bytes, std::span<const MbsSourceCopy> copies,
                                                     std::span<const std::pair<uint32_t, size_t>> layers, std::string* error) {
    constexpr size_t kPayloadSizeOffset = 0x04;
    constexpr size_t kFooterSize = 0x10; // the FEOC chunk that ends the file
    const auto fail = [&](std::string message) -> std::optional<std::vector<uint8_t>> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    const std::optional<Mbs> model = ParseMbs(bytes, error);
    if (!model) return std::nullopt;
    const Reader r(bytes);
    if (bytes.size() < kHeaderSize + kFooterSize || std::memcmp(bytes.data() + bytes.size() - kFooterSize, "FEOC", 4) != 0 ||
        r.U32(kPayloadSizeOffset) != bytes.size() - kHeaderSize - kFooterSize) {
        return fail("unexpected FMBS chunk layout");
    }
    const Section& sources = kSections[kSources];
    const Section& layer_section = kSections[kLayers];
    const size_t old_count = model->sources.size();
    if (old_count + copies.size() > 0xFFFF) return fail("too many source quads");
    for (const MbsSourceCopy& copy : copies) {
        if (copy.of >= old_count) return fail("copy of a missing source quad");
    }
    for (const auto& [layer, copy] : layers) {
        if (layer >= model->layers.size() || copy >= copies.size()) return fail("layer or copy out of range");
    }

    const size_t old_start = r.U32(sources.pointer);
    std::vector<uint8_t> out(bytes.begin(), bytes.end() - std::ptrdiff_t(kFooterSize));
    out.resize((out.size() + 15) / 16 * 16, 0);
    const size_t new_start = out.size();
    out.insert(out.end(), bytes.begin() + std::ptrdiff_t(old_start), bytes.begin() + std::ptrdiff_t(old_start + old_count * sources.stride));
    for (const MbsSourceCopy& copy : copies) {
        const size_t from = old_start + copy.of * sources.stride;
        std::vector<uint8_t> record(bytes.begin() + std::ptrdiff_t(from), bytes.begin() + std::ptrdiff_t(from + sources.stride));
        for (size_t point = 0; point < 5; ++point) { // the centre and four corners, x then y
            for (size_t axis = 0; axis < 2; ++axis) {
                float value;
                std::memcpy(&value, record.data() + point * 8 + axis * 4, sizeof(value));
                value += axis ? copy.dv : copy.du;
                std::memcpy(record.data() + point * 8 + axis * 4, &value, sizeof(value));
            }
        }
        out.insert(out.end(), record.begin(), record.end());
    }
    out.insert(out.end(), bytes.end() - std::ptrdiff_t(kFooterSize), bytes.end());

    const auto put = [&](size_t at, uint32_t value, size_t width) {
        for (size_t i = 0; i < width; ++i) out[at + i] = uint8_t(value >> (8 * i));
    };
    put(sources.pointer, uint32_t(new_start), 4);
    put(sources.count, uint32_t(old_count + copies.size()), 2);
    put(kPayloadSizeOffset, uint32_t(out.size() - kHeaderSize - kFooterSize), 4);
    const size_t layer_start = r.U32(layer_section.pointer);
    for (const auto& [layer, copy] : layers) put(layer_start + layer * layer_section.stride + 10, uint32_t(old_count + copy), 2);
    if (!ParseMbs(out, error)) return std::nullopt;
    return out;
}

std::optional<Mbs> LoadGameMbs(const std::filesystem::path& cpk, std::string_view entry, std::string* error) {
    CpkArchive archive;
    if (!archive.Open(cpk, error)) return std::nullopt;
    const CpkEntry* found = archive.Find(entry);
    if (!found) return Fail("entry not found: " + std::string(entry), error);
    std::vector<uint8_t> data = archive.Read(*found, error);
    if (data.empty()) return std::nullopt;
    return ParseMbs(data, error);
}

} // namespace dcp
