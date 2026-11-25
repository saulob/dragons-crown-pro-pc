// The structural locators for Cheats, Voice Language and True Ultrawide (src/feature_sites.h), without the game:
// on the local boot images they must find the sites of the static tables and install to the same bytes; on synthetic
// images built from the signatures at shifted places, with look-alike decoys, they must find the complete set and refuse
// every incomplete, ambiguous, altered or truncated one; and nothing resolves for a build that is not a supported
// signature-locator profile, whatever the image holds.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "build_scan.h"
#include "cheat_hooks.h"
#include "dcp_cheats.h"
#include "dcp_launcher.h"
#include "dcp_resolution.h"
#include "early_story_online.h"
#include "feature_sites.h"
#include "game_profile.h"
#include "party_hook.h"
#include "runtime_cheats.h"
#include "ultrawide_hooks.h"
#include "voice_hooks.h"

namespace fs = std::filesystem;
using namespace dcp;

namespace {

int g_checks = 0, g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        ++g_checks;                                                      \
        if (!(cond)) {                                                   \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);          \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

struct Reloc {
    uint64_t target, kind, value;
};

struct Image {
    std::vector<uint8_t> bytes;
    uint64_t entry = 0;
    std::vector<Reloc> relocs;
};

uint64_t Read64(std::ifstream& in) {
    uint8_t b[8] = {};
    in.read(reinterpret_cast<char*>(b), 8);
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i) value = value << 8 | b[i];
    return value;
}

bool LoadBootImage(const GameBuild& build, Image& out) {
    std::ifstream in(fs::path(DCP_SOURCE_DIR) / "build" / "image" / build.folder / "boot.bin", std::ios::binary);
    char magic[8] = {};
    if (!in.read(magic, 8) || std::memcmp(magic, "DCPBOOT1", 8)) return false;
    const uint64_t size = Read64(in);
    out.entry = Read64(in);
    const uint64_t segments = Read64(in), relocs = Read64(in), names = Read64(in);
    Read64(in), Read64(in);
    for (int i = 0; i < 4; ++i) Read64(in);
    const uint64_t modules = Read64(in);
    in.seekg(std::streamoff(modules * 56), std::ios::cur);
    const uint64_t bindings = Read64(in);
    in.seekg(std::streamoff(bindings * 24 + segments * 24 + names * 128), std::ios::cur);
    out.relocs.resize(relocs);
    for (Reloc& reloc : out.relocs) {
        reloc.target = Read64(in);
        reloc.kind = Read64(in);
        reloc.value = Read64(in);
        Read64(in);
    }
    out.bytes.resize(size);
    return size && in.read(reinterpret_cast<char*>(out.bytes.data()), std::streamsize(size)).good();
}

struct Located {
    DcpFeatureSites sites;
    DcpVoiceSites voice_sites{};
    DcpLocateReport cheat_report{}, voice_report{}, ultrawide_report{};
    bool cheats = false, voices = false, ultrawide = false;
};

Located LocateAll(const std::vector<uint8_t>& bytes) {
    const DcpExecRange range{0, bytes.size()};
    const DcpImageView view{bytes.data(), bytes.size(), &range, 1};
    Located result{};
    result.cheats = dcp_locate_cheat_sites(&view, &result.sites.cheat_sites, &result.cheat_report) != 0;
    result.voices = dcp_locate_voice_sites(&view, &result.voice_sites, &result.voice_report) != 0;
    result.ultrawide = dcp_locate_ultrawide_sites(&view, &result.sites.ultrawide_sites, &result.ultrawide_report) != 0;
    return result;
}

bool SameRecord(const UltrawideRecord& a, const UltrawideRecord& b) {
    return a.submit == b.submit && !std::memcmp(a.fill_calls, b.fill_calls, sizeof(a.fill_calls)) &&
           a.frame_commit == b.frame_commit && a.queue == b.queue && a.part_queue_call == b.part_queue_call &&
           a.model_draw == b.model_draw && a.widget_draw_call == b.widget_draw_call &&
           a.map_draw_call == b.map_draw_call && a.facility_draw_call == b.facility_draw_call &&
           a.worldmap_draw == b.worldmap_draw && !std::memcmp(a.fade_jumps, b.fade_jumps, sizeof(a.fade_jumps)) &&
           a.camera_view_call == b.camera_view_call && a.view_build == b.view_build;
}

// Installs with the static tables and with the locator's sites on two copies of a real image placed alike: the patched
// image bytes must be identical. The cheat and Voice Language hooks install at the located sites on every build's image.
void CompareInstalls(const GameBuild& build, const Image& image, const Located& located) {
    const uint64_t size = image.bytes.size();
    const size_t base = (size + 0xfff) / 0x1000 * 0x1000, room = 0x20000;
    std::vector<uint8_t> a(base + room, 0), b(base + room, 0);
    std::memcpy(a.data(), image.bytes.data(), size);
    std::memcpy(b.data(), image.bytes.data(), size);
    char detail_a[600] = {}, detail_b[600] = {};
    const int static_ok = ultrawide_hooks_install(a.data(), size, image.entry, a.data() + base, room, 1.3125, detail_a, sizeof(detail_a));
    const int located_ok = ultrawide_hooks_install_located(b.data(), size, image.entry, &located.sites.ultrawide_sites,
                                                           b.data() + base, room, 1.3125, detail_b, sizeof(detail_b));
    CHECK(static_ok == 1 && located_ok == 1 && std::string_view(detail_a).find("off (") == std::string_view::npos);
    CHECK(std::memcmp(a.data(), b.data(), size) == 0);

    std::vector<uint8_t> located_cheats(base + room, 0);
    std::memcpy(located_cheats.data(), image.bytes.data(), size);
    CHECK(runtime_cheat_hooks_install_located(located_cheats.data(), size, image.entry, &located.sites.cheat_sites,
                                              located_cheats.data() + base, room) == 1);
    std::vector<uint8_t> located_voices(base + room, 0);
    std::memcpy(located_voices.data(), image.bytes.data(), size);
    CHECK(voice_hooks_install(located_voices.data(), size, image.entry, &located.voice_sites, located_voices.data() + base,
                              room) == 1);
    if (std::string_view(build.label) == "CUSA10454 1.00 USA") {
        std::vector<uint8_t> c(base + room, 0);
        std::memcpy(c.data(), image.bytes.data(), size);
        CHECK(runtime_cheat_hooks_install(c.data(), size, image.entry, c.data() + base, room) == 1);
        CHECK(std::memcmp(c.data(), located_cheats.data(), size) == 0);
    }
}

DcpGameBuild ProfileOf(const GameBuild& build);

size_t PlayerTickIndex() {
    const std::span<const GuestHook> hooks = cheats::Hooks();
    for (size_t i = 0; i < hooks.size(); ++i) {
        if (std::string_view(hooks[i].name) == "player tick") return i;
    }
    return SIZE_MAX;
}

bool LocateParty(const std::vector<uint8_t>& bytes, DcpPartySites& out, DcpLocateReport& report) {
    const DcpExecRange range{0, bytes.size()};
    const DcpImageView view{bytes.data(), bytes.size(), &range, 1};
    out = {};
    return dcp_locate_party_sites(&view, &out, &report) != 0;
}

bool LayoutIsHandlers(const DcpPartySites& party) {
    return party.unit_owner == cheats::kUnitOwnerOffset && party.unit_slot == cheats::kUnitPlayerSlotOffset &&
           party.unit_class == cheats::kUnitClassOffset && party.owner_category == cheats::kOwnerCategoryOffset;
}

// The online party observer needs the player-tick site and four displacements of every supported build, found on its
// own: the same site the cheats hook, equal to the cheat set's wherever that resolves, and installing to one jump.
void CheckPartyOnRealImage(const GameBuild& build, const Image& image, const Located& located) {
    DcpPartySites party;
    DcpLocateReport report{};
    CHECK(LocateParty(image.bytes, party, report));
    const size_t tick = PlayerTickIndex();
    CHECK(tick != SIZE_MAX && party.tick == located.sites.cheat_sites.hooks[tick]);
    CHECK(LayoutIsHandlers(party));
    if (std::string_view(build.label) == "CUSA10454 1.00 USA") CHECK(party.tick == cheats::Hooks()[tick].offset);

    const uint64_t size = image.bytes.size();
    const size_t base = (size + 0xfff) / 0x1000 * 0x1000, room = party_hook_stub_bytes();
    std::vector<uint8_t> memory(base + room, 0);
    std::memcpy(memory.data(), image.bytes.data(), size);
    CHECK(party_hook_install_located(memory.data(), size, image.entry, &party, memory.data() + base, room) == 1);
    size_t changed_outside_site = 0;
    for (uint64_t i = 0; i < size; ++i) {
        changed_outside_site += (i < party.tick || i >= party.tick + 6) && memory[i] != image.bytes[i];
    }
    CHECK(changed_outside_site == 0 && memory[party.tick] == 0xe9);

    const DcpGameBuild profile = ProfileOf(build);
    const DcpExecRange range{0, size};
    const DcpImageView view{image.bytes.data(), size, &range, 1};
    DcpPartySites resolved{};
    char failure[192];
    CHECK(dcp_resolve_party(&profile, &view, &resolved, failure, sizeof(failure)) == 1 && resolved.tick == party.tick);
    std::printf("%s: party observer site 0x%x\n", build.label, party.tick);
}

size_t HexCount(const uint8_t* data, size_t size, std::string_view hex, std::vector<uint64_t>* first = nullptr) {
    const scan::Range range{0, size};
    size_t total = 0;
    const std::vector<uint64_t> hits = scan::FindPattern(data, size, std::span<const scan::Range>(&range, 1), hex, 4, total);
    if (first) *first = hits;
    return total;
}

// Issue #69 on the real executable: every actor, player-class ones included, speaks through a mapper caller whose mode
// is the actor's own virtual getter; the mapper gives the six player classes and the camp voices their English twins;
// and the one function that turns a character record into that mode ignores the record: the USA builds answer 1
// (English) for every character, resurrected ones included, the Japanese and Asian builds read the game's language
// byte through a table.
void CheckVoicePathOnRealImage(const GameBuild& build, const Image& image, const Located& located) {
    const std::vector<uint8_t>& b = image.bytes;
    const DcpVoiceSites& v = located.voice_sites;
    for (const uint32_t call : {v.cue_calls[0], v.cue_calls[1]}) {
        CHECK(call >= 8 && HexCount(b.data() + call - 8, 8, "ff 50 68 44 89 ef 89 c6") == 1);
    }
    const size_t window = std::min<size_t>(0x500, b.size() - v.cue_mapper);
    for (const uint32_t family : kDcpPlayerVoiceFamilies) {
        const uint32_t minus = uint32_t(-int32_t(family));
        char hex[64];
        std::snprintf(hex, sizeof(hex), "8d 87 %02x %02x %02x %02x 83 f8 12", minus & 0xff, (minus >> 8) & 0xff,
                      (minus >> 16) & 0xff, (minus >> 24) & 0xff);
        CHECK(HexCount(b.data() + v.cue_mapper, window, hex) == 1);
    }
    CHECK(HexCount(b.data() + v.cue_mapper, window, "3d 54 0b 00 00") == 1 &&
          HexCount(b.data() + v.cue_mapper, window, "81 c7 a0 86 01 00") == 1);

    std::vector<uint64_t> source;
    CHECK(HexCount(b.data(), b.size(), "48 8b 47 70 48 89 f3 48 8b b8 e8 00 00 00", &source) == 1);
    if (source.size() != 1 || source.front() + 0x50 > b.size()) return;
    const bool usa = std::string_view(build.title_id) == "CUSA10454";
    CHECK(HexCount(b.data() + source.front(), 0x50, usa ? "c7 03 01 00 00 00" : "ff c8 83 f8 0c") == 1);
    std::printf("%s: voice mapper 0x%x, %d callers, character voice mode %s\n", build.label, v.cue_mapper,
                DCP_VOICE_CUE_CALLS, usa ? "always English" : "from the language byte");
}

// The narrator name parser as it was when Voice Language first shipped: it read a table entry as an image offset.
uint32_t PreFixNarratorName(const uint8_t* bytes, uint64_t size, uint64_t at, std::string_view extension) {
    constexpr std::string_view kPrefix = "SND/NARRATION_GM";
    if (at >= size || size - at < kPrefix.size() + 12) return 0;
    const char* text = reinterpret_cast<const char*>(bytes + at);
    if (std::string_view(text, kPrefix.size()) != kPrefix) return 0;
    uint32_t number = 0;
    size_t i = kPrefix.size();
    for (; i < kPrefix.size() + 7 && text[i] >= '0' && text[i] <= '9'; ++i) number = number * 10 + uint32_t(text[i] - '0');
    if (i == kPrefix.size() || text[i] != '.' || std::string_view(text + i + 1, 3) != extension || text[i + 4] != 0) return 0;
    return number;
}

// The image as the loader hands it to the locators: placed at `memory` with every relocation applied. Imports point
// outside the image, as the loader's traps do.
void LoadLikeTheLoader(const Image& image, std::vector<uint8_t>& memory) {
    std::memcpy(memory.data(), image.bytes.data(), image.bytes.size());
    for (const Reloc& reloc : image.relocs) {
        const uint64_t value = reloc.kind == 0 ? uint64_t(uintptr_t(memory.data())) + reloc.value
                                               : UINT64_C(0x7ff000000000) + 32 * reloc.value;
        if (reloc.target + 8 <= image.bytes.size()) std::memcpy(&memory[reloc.target], &value, 8);
    }
}

// Issue #69 Q7V2N runtime failure: the loader resolves Voice Language after relocating the image, when the narrator
// table holds loaded addresses. The locator must find the same sites, the narrator route included, and every route
// must install for a Japanese request.
void CheckVoiceOnLoadedImage(const GameBuild& build, const Image& image, const Located& raw) {
    const uint64_t size = image.bytes.size();
    const size_t base = (size + 0xfff) / 0x1000 * 0x1000, room = voice_hooks_stub_bytes();
    std::vector<uint8_t> memory(base + room, 0);
    LoadLikeTheLoader(image, memory);
    CHECK(!image.relocs.empty());

    const uint64_t table = raw.voice_sites.narrator_banks;
    uint64_t acb = 0, awb = 0;
    std::memcpy(&acb, &memory[table], 8);
    std::memcpy(&awb, &memory[table + 8], 8);
    const uint64_t loaded = uint64_t(uintptr_t(memory.data()));
    CHECK(acb > loaded && acb - loaded < size && awb > loaded && awb - loaded < size);
    const uint32_t before = PreFixNarratorName(memory.data(), size, acb, "ACB");
    const uint32_t after = PreFixNarratorName(memory.data(), size, acb - loaded, "ACB");
    CHECK(before == 0 && after == PreFixNarratorName(memory.data(), size, awb - loaded, "AWB") &&
          dcp_narrator_base_valid(after));
    std::printf("%s: loaded narrator entry 0 = %s / %s; read as an image offset: %u, as a loaded address: %u\n",
                build.label, reinterpret_cast<const char*>(&memory[acb - loaded]),
                reinterpret_cast<const char*>(&memory[awb - loaded]), unsigned(before), unsigned(after));

    const DcpExecRange range{0, size};
    const DcpImageView view{memory.data(), size, &range, 1};
    DcpVoiceSites sites{};
    DcpLocateReport report{};
    CHECK(dcp_locate_voice_sites(&view, &sites, &report) == 1);
    if (sites.narrator_route != DCP_NARRATOR_ROUTE_OK) std::printf("  %s: narrator %s\n", build.label, sites.narrator_failure);
    const DcpVoiceSites& r = raw.voice_sites;
    CHECK(sites.narrator_route == DCP_NARRATOR_ROUTE_OK && r.narrator_route == DCP_NARRATOR_ROUTE_OK);
    CHECK(sites.cue_mapper == r.cue_mapper && std::equal(std::begin(sites.cue_calls), std::end(sites.cue_calls), r.cue_calls) &&
          sites.resource_acquire == r.resource_acquire && sites.narrator_bank_select == r.narrator_bank_select &&
          sites.narrator_cue_base == r.narrator_cue_base && sites.narrator_cue_helper == r.narrator_cue_helper &&
          sites.narrator_banks == r.narrator_banks && sites.narrator_bases == r.narrator_bases);
    DcpGameBuild profile = ProfileOf(build);
    char failure[192];
    DcpVoiceSites resolved{};
    CHECK(dcp_resolve_voice_language(&profile, &view, &resolved, failure, sizeof(failure)) == 1 &&
          resolved.narrator_route == DCP_NARRATOR_ROUTE_OK);

    voice_hooks_configure(DCP_VOICE_JAPANESE, DCP_VOICE_ALL_CATEGORIES);
    CHECK(voice_hooks_install(memory.data(), size, image.entry, &resolved, memory.data() + base, room) == 1);
    CHECK(voice_hooks_routed() == DCP_VOICE_ALL_CATEGORIES);
    voice_hooks_log_ready((std::string(build.title_id) + "-" + build.version).c_str(), DCP_VOICE_JAPANESE);
    voice_hooks_configure(DCP_VOICE_DEFAULT, 0);
}

void TestRealImages() {
    size_t found = 0;
    for (const GameBuild& build : SupportedBuilds()) {
        Image image;
        if (!LoadBootImage(build, image)) {
            std::printf("%s: no local boot image; real-image checks skipped\n", build.label);
            continue;
        }
        ++found;
        const Located located = LocateAll(image.bytes);
        if (!located.cheats || !located.voices || !located.ultrawide) {
            std::printf("  %s: %s | %s | %s\n", build.label, located.cheat_report.failure, located.voice_report.failure,
                        located.ultrawide_report.failure);
        }
        CHECK(located.cheats && located.voices && located.ultrawide);

        const UltrawideRecord* table = nullptr;
        for (const UltrawideRecord& record : ultrawide_records) {
            if (std::string_view(record.build) == build.label) table = &record;
        }
        CHECK(table != nullptr);
        if (table) {
            CHECK(SameRecord(ultrawide_record_from_sites(&located.sites.ultrawide_sites, image.bytes.size(), image.entry), *table));
            for (size_t i = 0; i < DCP_ORTHO_LOADS; ++i) {
                CHECK(located.sites.ultrawide_sites.ortho_loads[i] == dcp_pass_ortho_sites[i].offset);
            }
            CompareInstalls(build, image, located);
        }
        if (std::string_view(build.label) == "CUSA10454 1.00 USA") {
            const std::span<const GuestHook> hooks = cheats::Hooks();
            for (size_t i = 0; i < hooks.size(); ++i) CHECK(located.sites.cheat_sites.hooks[i] == hooks[i].offset);
            const cheats::FunctionOffsets functions = cheats::StaticFunctionOffsets();
            for (size_t i = 0; i < DCP_CHEAT_FUNCTIONS; ++i) CHECK(located.sites.cheat_sites.functions[i] == functions.at[i]);
            const uint32_t calls[DCP_VOICE_CUE_CALLS] = {0x52e98, 0x52f03, 0x68276, 0x68687, 0x68703, 0x6878c, 0x6908b};
            CHECK(located.voice_sites.cue_mapper == 0x68bc0 && located.voice_sites.resource_acquire == 0x57cbb0);
            CHECK(std::equal(std::begin(calls), std::end(calls), std::begin(located.voice_sites.cue_calls)));
            CHECK(located.voice_sites.narrator_bank_select == 0x5bc14 && located.voice_sites.narrator_cue_base == 0x5c09e &&
                  located.voice_sites.narrator_cue_helper == 0x5c5ea);
            CHECK(located.voice_sites.narrator_banks == 0x8c4020 && located.voice_sites.narrator_bases == 0x6ac450);
        }
        CheckPartyOnRealImage(build, image, located);
        CheckVoicePathOnRealImage(build, image, located);
        CheckVoiceOnLoadedImage(build, image, located);
        std::string mismatch;
        CHECK(cheats::LayoutMatches(located.sites.cheat_sites.layout, &mismatch));
        std::printf("%s: locators equal the tables (%zu/%zu cheat sites, %zu/%zu ultrawide sites)\n", build.label,
                    located.cheat_report.found, located.cheat_report.expected, located.ultrawide_report.found,
                    located.ultrawide_report.expected);
    }
    std::printf("%zu of %zu supported builds have a local boot image\n", found, SupportedBuilds().size());
}

// --- synthetic images ---------------------------------------------------------------------------------------------

struct Pattern {
    std::vector<int> bytes; // -1 is a wildcard
};

Pattern Parse(std::string_view text) {
    Pattern pattern;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == ' ') {
            ++i;
        } else if (text[i] == '?') {
            pattern.bytes.push_back(-1);
            i += 2;
        } else {
            pattern.bytes.push_back(int(std::strtoul(std::string(text.substr(i, 2)).c_str(), nullptr, 16)));
            i += 2;
        }
    }
    return pattern;
}

constexpr uint64_t kImageSize = 0x240000;
constexpr uint64_t kConstants = 0x200000;

std::vector<uint64_t> Find(const std::vector<uint8_t>& bytes, std::string_view pattern) {
    const scan::Range range{0, bytes.size()};
    size_t total = 0;
    return scan::FindPattern(bytes.data(), bytes.size(), std::span<const scan::Range>(&range, 1), pattern, 64, total);
}

size_t Count(const std::vector<uint8_t>& bytes, std::string_view pattern) {
    const scan::Range range{0, bytes.size()};
    size_t total = 0;
    scan::FindPattern(bytes.data(), bytes.size(), std::span<const scan::Range>(&range, 1), pattern, 1, total);
    return total;
}

void Plant(std::vector<uint8_t>& bytes, std::string_view pattern, uint64_t at) {
    const Pattern parsed = Parse(pattern);
    for (size_t i = 0; i < parsed.bytes.size(); ++i) {
        if (parsed.bytes[i] >= 0) bytes[at + i] = uint8_t(parsed.bytes[i]);
    }
}

struct Plan {
    std::string pattern;
    size_t expected;
    const char* feature; // nullptr for a layout probe
    const char* name;
};

std::vector<Plan> Plans() {
    std::vector<Plan> plans;
    for (const scan::SiteSpec& spec : scan::Sites()) {
        const std::string_view feature = spec.feature;
        if (feature == "Cheats" || feature == "True ultrawide") {
            plans.push_back({spec.pattern, spec.expected, spec.feature, spec.name});
        }
    }
    for (size_t i = 0; i < dcp_layout_probe_count(); ++i) {
        size_t expected = 0;
        const char* pattern = dcp_layout_probe_pattern(i, &expected);
        plans.push_back({pattern, expected, nullptr, "layout probe"});
    }
    return plans;
}

// The displacements the cheat handlers expect, as this test's own oracle.
uint32_t ExpectedLayout(DcpLayoutField field) {
    static const uint32_t values[DCP_LAYOUT_COUNT] = {
        0xfffffe00, 0xd0,  0x1e0,  0xa70,    0xcb,   0xca,  0xb0,   0xa4,   0xa8,    0xac,  0x1e8,
        0x2a0c,     0x9b8, 0x39e0, 0x3ad4,   0x508,  0x510, 0x40,   0x44,   0x48,    0x4c,  0xaf238,
        0x80,       0xa8,  0x23b0, 0x88,     0x24,   0xc8,  0x26,   0x4,    0x8,     0x24,  0xfe};
    return values[field];
}

uint64_t Place(uint32_t seed, size_t index, uint64_t cursor) {
    return cursor + ((seed * 2654435761u + uint32_t(index) * 40503u) % 0x40) * 16;
}

void WriteLayoutValues(std::vector<uint8_t>& bytes) {
    for (size_t probe = 0; probe < dcp_layout_probe_count(); ++probe) {
        size_t expected = 0;
        const char* pattern = dcp_layout_probe_pattern(probe, &expected);
        for (const uint64_t start : Find(bytes, pattern)) {
            DcpLayoutField field;
            size_t position, width;
            for (size_t i = 0; dcp_layout_probe_field(probe, i, &field, &position, &width); ++i) {
                const uint32_t value = ExpectedLayout(field);
                std::memcpy(&bytes[start + position], &value, width);
            }
        }
    }
}

// Gives the True Ultrawide sites their relations and the projection loads their constants.
void WireUltrawide(std::vector<uint8_t>& bytes, uint64_t constants) {
    const auto site_of = [&](const char* name) {
        std::vector<uint64_t> sites;
        for (const scan::SiteSpec& spec : scan::Sites()) {
            if (std::string_view(spec.feature) == "True ultrawide" && std::string_view(spec.name) == name) {
                sites = Find(bytes, spec.pattern);
                for (uint64_t& site : sites) site += spec.anchor;
            }
        }
        return sites;
    };
    for (const scan::RelationSpec& relation : scan::Relations()) {
        if (std::string_view(relation.feature) != "True ultrawide") continue;
        const uint64_t target = site_of(relation.to).front();
        for (const uint64_t from : site_of(relation.from)) {
            const int32_t displacement = int32_t(int64_t(target) - int64_t(from) - 5);
            std::memcpy(&bytes[from + 1], &displacement, 4);
        }
    }
    for (size_t i = 0; i < DCP_ORTHO_LOADS; ++i) {
        const std::string name = "pass ortho load " + std::to_string(i + 1);
        const uint64_t site = site_of(name.c_str()).front();
        const uint64_t constant = constants + i * 16;
        const int32_t displacement = int32_t(int64_t(constant) - int64_t(site) - 8);
        std::memcpy(&bytes[site + 4], &displacement, 4);
        std::memcpy(&bytes[constant], &dcp_pass_ortho_sites[i].value, 4);
    }
}

// Every site and probe pattern, longest first so a window of a longer one is found inside it as in a real image, each at
// a deterministic place that differs from the real executables'. The probes' displacement bytes get the values the
// handlers expect, the calls the sites' relations need get their targets, and the projection loads their constants.
std::vector<uint8_t> BuildSynthetic(uint32_t seed, bool decoys, bool layout_values = true) {
    std::vector<uint8_t> bytes(kImageSize, 0);
    std::vector<Plan> plans = Plans();
    std::stable_sort(plans.begin(), plans.end(), [](const Plan& a, const Plan& b) { return a.pattern.size() > b.pattern.size(); });
    uint64_t cursor = 0x1000;
    size_t index = 0;
    for (const Plan& plan : plans) {
        while (Count(bytes, plan.pattern) < plan.expected) {
            Plant(bytes, plan.pattern, Place(seed, index++, cursor));
            cursor += 0x700;
            if (cursor + 0x1000 > 0x1c0000) std::abort();
        }
    }
    if (layout_values) WriteLayoutValues(bytes);
    WireUltrawide(bytes, kConstants);
    if (decoys) {
        // Look-alikes: a copy with one solid byte off and the first half of one. A decoy that another, more permissive
        // pattern matches is dropped, as it would be a second occurrence of that one.
        const auto matches_anything = [&](uint64_t at, size_t length) {
            const std::vector<uint8_t> window(bytes.begin() + std::ptrdiff_t(at), bytes.begin() + std::ptrdiff_t(at + length));
            return std::any_of(plans.begin(), plans.end(), [&](const Plan& plan) { return Count(window, plan.pattern) != 0; });
        };
        uint64_t at = 0x1c0000;
        for (const Plan& plan : plans) {
            const Pattern parsed = Parse(plan.pattern);
            std::vector<size_t> solid;
            for (size_t i = 0; i < parsed.bytes.size(); ++i) {
                if (parsed.bytes[i] >= 0) solid.push_back(i);
            }
            const size_t length = parsed.bytes.size();
            for (const bool half : {false, true}) {
                Plant(bytes, plan.pattern, at);
                if (half) {
                    std::memset(&bytes[at + length / 2], 0, length - length / 2);
                } else {
                    bytes[at + solid[solid.size() / 2]] ^= 0x55;
                }
                if (matches_anything(at, length)) std::memset(&bytes[at], 0, length);
                at += 0x100;
            }
            if (at + 0x200 > kConstants - 0x100) break;
        }
    }
    return bytes;
}

std::set<uint32_t> AllOffsets(const Located& located) {
    std::set<uint32_t> offsets;
    for (uint32_t site : located.sites.cheat_sites.hooks) offsets.insert(site);
    for (uint32_t site : located.sites.cheat_sites.functions) offsets.insert(site);
    const DcpUltrawideSites& u = located.sites.ultrawide_sites;
    for (uint32_t site : {u.submit, u.frame_commit, u.queue, u.part_queue_call, u.model_draw, u.widget_draw_call,
                          u.map_draw_call, u.facility_draw_call, u.worldmap_draw, u.camera_view_call, u.view_build}) {
        offsets.insert(site);
    }
    return offsets;
}

void ExpectAll(const Located& located, const char* what) {
    if (!located.cheats || !located.ultrawide) {
        std::printf("  %s: %s | %s\n", what, located.cheat_report.failure, located.ultrawide_report.failure);
    }
    CHECK(located.cheats && located.ultrawide);
}

void TestSyntheticLocators() {
    const std::vector<uint8_t> first = BuildSynthetic(1, true), second = BuildSynthetic(7, true);
    const Located a = LocateAll(first), b = LocateAll(second);
    ExpectAll(a, "seed 1");
    ExpectAll(b, "seed 7");

    // Nothing depends on the executables' addresses: the sites sit elsewhere, differently for each seed.
    std::set<uint32_t> real;
    for (const GuestHook& hook : cheats::Hooks()) real.insert(hook.offset);
    for (const UltrawideRecord& record : ultrawide_records) real.insert(record.submit);
    const std::set<uint32_t> found_a = AllOffsets(a), found_b = AllOffsets(b);
    CHECK(found_a != found_b);
    for (uint32_t site : found_a) CHECK(!real.contains(site));
    CHECK(found_a.size() >= 40 && std::all_of(found_a.begin(), found_a.end(), [&](uint32_t s) { return s < first.size(); }));

    // The locator and an independent search of the planted bytes agree on every site.
    for (const scan::SiteSpec& spec : scan::Sites()) {
        if (std::string_view(spec.feature) != "Cheats" || spec.kind != scan::SiteKind::kHook) continue;
        const std::vector<uint64_t> hits = Find(first, spec.pattern);
        CHECK(hits.size() == 1 && std::find(std::begin(a.sites.cheat_sites.hooks), std::end(a.sites.cheat_sites.hooks),
                                            uint32_t(hits.front() + spec.anchor)) != std::end(a.sites.cheat_sites.hooks));
    }
    CHECK(cheats::LayoutMatches(a.sites.cheat_sites.layout));
    for (size_t i = 0; i < DCP_LAYOUT_COUNT; ++i) CHECK(a.sites.cheat_sites.layout[i] == ExpectedLayout(DcpLayoutField(i)));
    CHECK(a.cheat_report.found == a.cheat_report.expected && a.ultrawide_report.found == a.ultrawide_report.expected);

    // Each site missing, or one byte off, refuses its feature and names a site.
    const std::vector<Plan> plans = Plans();
    for (const Plan& plan : plans) {
        if (!plan.feature) continue;
        const Pattern parsed = Parse(plan.pattern);
        std::vector<size_t> solid;
        for (size_t i = 0; i < parsed.bytes.size(); ++i) {
            if (parsed.bytes[i] >= 0) solid.push_back(i);
        }
        const std::vector<uint64_t> hits = Find(first, plan.pattern);
        if (hits.size() != plan.expected) continue; // a window inside a longer one is covered by that one
        for (const uint8_t flip : {uint8_t(0xff), uint8_t(0x01)}) {
            std::vector<uint8_t> broken = first;
            for (const uint64_t hit : hits) broken[hit + solid[solid.size() / 2]] ^= flip;
            const Located result = LocateAll(broken);
            const std::string_view feature = plan.feature;
            const bool refused = feature == "Cheats" ? !result.cheats
                                 : feature == "True ultrawide" ? !result.ultrawide
                                                              : !result.voices;
            const char* failure = feature == "Cheats" ? result.cheat_report.failure
                                  : feature == "True ultrawide" ? result.ultrawide_report.failure
                                                               : result.voice_report.failure;
            if (!refused) std::printf("  %s %s was not refused\n", plan.feature, plan.name);
            CHECK(refused && failure[0] != 0);
        }
    }

    // A second copy of any unique site makes it ambiguous.
    for (const Plan& plan : plans) {
        if (!plan.feature || plan.expected != 1 || Count(first, plan.pattern) != 1) continue;
        std::vector<uint8_t> doubled = first;
        Plant(doubled, plan.pattern, 0x1f0000);
        const Located result = LocateAll(doubled);
        const std::string_view feature = plan.feature;
        const bool refused = feature == "Cheats" ? !result.cheats : feature == "True ultrawide" ? !result.ultrawide : !result.voices;
        if (!refused) std::printf("  %s %s was not refused when duplicated\n", plan.feature, plan.name);
        CHECK(refused);
    }
    {
        std::vector<uint8_t> grouped = first;
        for (const Plan& plan : plans) {
            if (plan.feature && plan.expected == 3) Plant(grouped, plan.pattern, 0x1f4000);
        }
        CHECK(!LocateAll(grouped).ultrawide);
    }

    // Truncation at any point leaves no feature that reports a site beyond the image.
    for (const size_t cut : {size_t(0), size_t(16), size_t(0x1000), first.size() / 4, first.size() / 2, size_t(kConstants + 0x20)}) {
        const std::vector<uint8_t> shorter(first.begin(), first.begin() + std::ptrdiff_t(cut));
        const Located result = LocateAll(shorter);
        CHECK(!(result.cheats && result.ultrawide));
        for (const uint32_t site : AllOffsets(result)) CHECK(site == 0 || site < shorter.size());
        if (cut == kConstants + 0x20) CHECK(result.cheats && !result.ultrawide); // the constants are cut
    }
    {
        const DcpExecRange range{0, 0};
        const DcpImageView empty{first.data(), 0, &range, 1};
        DcpCheatSites sites;
        DcpLocateReport report;
        CHECK(!dcp_locate_cheat_sites(&empty, &sites, &report) && report.failure[0] != 0);
        CHECK(!dcp_locate_cheat_sites(nullptr, &sites, &report) && !dcp_locate_voice_sites(nullptr, nullptr, nullptr));
    }

    // A call that goes to another function breaks the relation; a projection constant that differs, or points outside the
    // image, refuses True Ultrawide.
    for (const scan::RelationSpec& relation : scan::Relations()) {
        if (std::string_view(relation.feature) != "True ultrawide") continue;
        for (const scan::SiteSpec& spec : scan::Sites()) {
            if (std::string_view(spec.feature) != "True ultrawide" || std::string_view(spec.name) != relation.from) continue;
            std::vector<uint8_t> wrong = first;
            const uint64_t from = Find(first, spec.pattern).front() + spec.anchor;
            int32_t displacement;
            std::memcpy(&displacement, &wrong[from + 1], 4);
            displacement += 0x10;
            std::memcpy(&wrong[from + 1], &displacement, 4);
            const Located result = LocateAll(wrong);
            if (result.ultrawide) std::printf("  %s was not refused when it calls elsewhere\n", relation.from);
            CHECK(!result.ultrawide && std::string_view(result.ultrawide_report.failure).find("relation") != std::string_view::npos);
            CHECK(result.cheats);
        }
    }
    for (size_t i = 0; i < DCP_ORTHO_LOADS; ++i) {
        std::vector<uint8_t> altered = first;
        altered[kConstants + i * 16] ^= 0x01;
        CHECK(!LocateAll(altered).ultrawide);
        altered = first;
        const std::string name = "pass ortho load " + std::to_string(i + 1);
        for (const scan::SiteSpec& spec : scan::Sites()) {
            if (name != spec.name) continue;
            const uint64_t site = Find(first, spec.pattern).front() + spec.anchor;
            const int32_t outside = 0x7ffffff0;
            std::memcpy(&altered[site + 4], &outside, 4);
            CHECK(!LocateAll(altered).ultrawide);
            altered = first;
            altered[site] = 0xc4; // not the vmovss the load is
            CHECK(!LocateAll(altered).ultrawide);
        }
    }
    {
        const DcpExecRange touching[] = {{10, 20}, {20, 30}}, shared[] = {{10, 21}, {20, 30}}, inside[] = {{10, 40}, {20, 30}};
        CHECK(!dcp_ranges_overlap(touching, 2) && dcp_ranges_overlap(shared, 2) && dcp_ranges_overlap(inside, 2));
    }
}

void TestLayout() {
    const std::vector<uint8_t> base = BuildSynthetic(3, false);
    CHECK(LocateAll(base).cheats);

    // Two instructions of the game that disagree about a field refuse the whole layout.
    size_t stat = SIZE_MAX, level_stats = SIZE_MAX;
    for (size_t i = 0; i < dcp_layout_probe_count(); ++i) {
        size_t n;
        const std::string_view pattern = dcp_layout_probe_pattern(i, &n);
        if (pattern.starts_with("8b 08 8b 50 ?? 8b 3e")) stat = i;
        if (pattern.starts_with("41 b8 ff ff ff 01 44 23 83")) level_stats = i;
    }
    CHECK(stat != SIZE_MAX && level_stats != SIZE_MAX);
    const auto field_position = [&](size_t probe, DcpLayoutField wanted) {
        DcpLayoutField field;
        size_t position, width;
        for (size_t i = 0; dcp_layout_probe_field(probe, i, &field, &position, &width); ++i) {
            if (field == wanted) return position;
        }
        return SIZE_MAX;
    };
    const auto start_of = [&](size_t probe) {
        size_t n;
        return Find(base, dcp_layout_probe_pattern(probe, &n)).front();
    };
    std::vector<uint8_t> conflicting = base;
    conflicting[start_of(stat) + field_position(stat, DCP_LAYOUT_UNIT_HP)] = 0xb4; // IsDead's own instruction says 0xb0
    const Located conflict = LocateAll(conflicting);
    CHECK(!conflict.cheats && std::string_view(conflict.cheat_report.failure).find("conflicting") != std::string_view::npos);

    // A build whose every instruction agrees on a different displacement is found, but the handlers do not match it, and
    // installing refuses it before writing anything.
    std::vector<uint8_t> shifted = base;
    shifted[start_of(stat) + field_position(stat, DCP_LAYOUT_UNIT_STAT)] = 0xb8;
    shifted[start_of(level_stats) + field_position(level_stats, DCP_LAYOUT_UNIT_STAT)] = 0xb8;
    const Located moved = LocateAll(shifted);
    std::string mismatch;
    CHECK(moved.cheats && !cheats::LayoutMatches(moved.sites.cheat_sites.layout, &mismatch) && mismatch == "unit stat");
    std::vector<uint8_t> memory = shifted;
    memory.resize(shifted.size() + 0x10000);
    const std::vector<uint8_t> before = memory;
    CHECK(runtime_cheat_hooks_install_located(memory.data(), shifted.size(), 0x1000, &moved.sites.cheat_sites,
                                              memory.data() + shifted.size(), 0x10000) == 0);
    CHECK(memory == before);

    // Every handler constant is read back from some instruction, and a field that no instruction names refuses the layout.
    std::set<DcpLayoutField> covered;
    for (size_t probe = 0; probe < dcp_layout_probe_count(); ++probe) {
        DcpLayoutField field;
        size_t position, width;
        for (size_t i = 0; dcp_layout_probe_field(probe, i, &field, &position, &width); ++i) covered.insert(field);
    }
    CHECK(covered.size() == DCP_LAYOUT_COUNT);
    for (size_t i = 0; i < DCP_LAYOUT_COUNT; ++i) CHECK(std::string_view(dcp_layout_field_name(DcpLayoutField(i))).size() > 3);

    // The hooks are installed together or not at all: one site whose bytes are not the original instruction, found by the
    // locator or not, refuses all 25 and leaves the image as it was.
    const Located ok = LocateAll(base);
    std::vector<uint8_t> memory2 = base;
    memory2.resize(base.size() + 0x10000);
    memory2[ok.sites.cheat_sites.hooks[3] + 1] ^= 0x40;
    const std::vector<uint8_t> memory2_before = memory2;
    CHECK(runtime_cheat_hooks_install_located(memory2.data(), base.size(), 0x1000, &ok.sites.cheat_sites,
                                              memory2.data() + base.size(), 0x10000) == 0);
    CHECK(memory2 == memory2_before);
    std::vector<uint8_t> whole = base;
    whole.resize(base.size() + 0x10000);
    CHECK(runtime_cheat_hooks_install_located(whole.data(), base.size(), 0x1000, &ok.sites.cheat_sites,
                                              whole.data() + base.size(), 0x10000) == 1);
    CHECK(whole != memory2_before);
}

DcpGameBuild ProfileOf(const GameBuild& build) {
    const fs::path dir = fs::temp_directory_path() / "dcp_feature_sites_profile";
    fs::create_directories(dir);
    std::ofstream(dir / "image.json") << "{\n  \"title_id\": \"" << build.title_id << "\",\n  \"eboot_sha256\": \""
                                      << build.eboot_sha256 << "\",\n  \"app_ver\": \"" << FinishedLayer(build).app_ver
                                      << "\"\n}";
    DcpGameBuild game{};
    CHECK(dcp_game_build(dir.string().c_str(), &game) == 1);
    fs::remove_all(dir);
    return game;
}

void TestParty() {
    const std::vector<uint8_t> base = BuildSynthetic(3, false), other = BuildSynthetic(9, false);
    DcpPartySites party, second;
    DcpLocateReport report{};
    const Located all = LocateAll(base);
    const size_t tick = PlayerTickIndex();
    CHECK(all.cheats && tick != SIZE_MAX);
    CHECK(LocateParty(base, party, report) && party.tick == all.sites.cheat_sites.hooks[tick] && LayoutIsHandlers(party));
    CHECK(LocateParty(other, second, report) && second.tick != party.tick && LayoutIsHandlers(second));
    CHECK(party.tick != cheats::Hooks()[tick].offset);

    const scan::SiteSpec* spec = nullptr;
    for (const scan::SiteSpec& candidate : scan::Sites()) {
        if (std::string_view(candidate.name) == "hook: player tick") spec = &candidate;
    }
    CHECK(spec != nullptr);
    const auto zero_all = [&](std::vector<uint8_t>& bytes, std::string_view pattern) {
        const size_t length = Parse(pattern).bytes.size();
        for (const uint64_t hit : Find(bytes, pattern)) std::memset(&bytes[hit], 0, length);
    };

    // Without the player-tick site, or with two, nothing is reported.
    std::vector<uint8_t> broken = base;
    zero_all(broken, spec->pattern);
    CHECK(!LocateParty(broken, party, report) && report.failure[0] != 0);
    std::vector<uint8_t> doubled = base;
    Plant(doubled, spec->pattern, 0x1f0000);
    CHECK(!LocateParty(doubled, party, report) && std::string_view(report.failure).find("2 matches") != std::string_view::npos);

    // Without any one of the probes that read the four displacements, or with one off, nothing is reported ...
    const char* const needed[] = {"48 83 bb ?? ?? ?? ?? 00 0f 84", "4c 8b bd ?? ?? ?? ?? 8b 40 ??", "48 8b bd ?? ?? ?? ?? 48 8b 07 48 63 9f"};
    for (const char* prefix : needed) {
        bool found = false;
        for (size_t i = 0; i < dcp_layout_probe_count(); ++i) {
            size_t n;
            const std::string_view pattern = dcp_layout_probe_pattern(i, &n);
            if (!pattern.starts_with(prefix)) continue;
            found = true;
            std::vector<uint8_t> gone = base, off = base;
            zero_all(gone, pattern);
            CHECK(!LocateParty(gone, party, report));
            const std::vector<uint64_t> hits = Find(off, pattern);
            CHECK(!hits.empty());
            for (const uint64_t hit : hits) off[hit] ^= 0xff;
            CHECK(!LocateParty(off, party, report));
        }
        CHECK(found);
    }

    // ... while a cheat site the observer does not read (the arrows) can be gone: the cheats refuse, the observer stays.
    const scan::SiteSpec* arrows = nullptr;
    for (const scan::SiteSpec& candidate : scan::Sites()) {
        if (std::string_view(candidate.name) == "hook: arrows") arrows = &candidate;
    }
    CHECK(arrows != nullptr);
    std::vector<uint8_t> no_arrows = base;
    zero_all(no_arrows, arrows->pattern);
    CHECK(!LocateAll(no_arrows).cheats && LocateParty(no_arrows, party, report) && party.tick == all.sites.cheat_sites.hooks[tick]);

    // The observer is placed for a supported build only, tables or signatures alike.
    const DcpExecRange range{0, base.size()};
    const DcpImageView view{base.data(), base.size(), &range, 1};
    char failure[192];
    CHECK(dcp_resolve_party(nullptr, &view, &party, failure, sizeof(failure)) == 0 &&
          std::string_view(failure).find("not a supported game build") != std::string_view::npos);
    for (const GameBuild& build : SupportedBuilds()) {
        const DcpGameBuild profile = ProfileOf(build);
        CHECK(dcp_resolve_party(&profile, &view, &party, failure, sizeof(failure)) == 1);
        CHECK(dcp_resolve_party(&profile, nullptr, &party, failure, sizeof(failure)) == 0);
        CHECK(dcp_resolve_party(&profile, &view, nullptr, failure, sizeof(failure)) == 0);
    }
    CHECK(SupportedBuilds().size() == 8);
}

const scan::SiteSpec* ExitGameSpec(std::string_view name) {
    for (const scan::SiteSpec& spec : scan::Sites()) {
        if (std::string_view(spec.feature) == "Exit Game" && std::string_view(spec.name) == name) return &spec;
    }
    return nullptr;
}

bool LocateExitGame(const std::vector<uint8_t>& bytes, DcpExitGameSites& out, DcpLocateReport& report) {
    const DcpExecRange range{0, bytes.size()};
    const DcpImageView view{bytes.data(), bytes.size(), &range, 1};
    out = {};
    return dcp_locate_exit_game_sites(&view, &out, &report) != 0;
}

// The title's four-row menu and Exit Game sites on a synthetic image: each found where its pattern is planted, none missing, doubled or
// half there, and the same sites for a build that has no real image here (CUSA10454 1.01, CUSA10209 1.02, CUSA10487 1.03).
void TestExitGame() {
    const char* const names[] = {"sheet fill", "row widget", "help limit", "part table", "menu tick", "part setter"};
    const scan::SiteSpec* specs[6];
    for (size_t i = 0; i < 6; ++i) {
        specs[i] = ExitGameSpec(names[i]);
        CHECK(specs[i] != nullptr);
        if (!specs[i]) return;
    }
    CHECK(specs[0]->expected == 1 && specs[1]->expected == 1 && specs[2]->expected == 2 && specs[3]->expected == 2 &&
          specs[4]->expected == 1 && specs[5]->expected == 1);
    const uint64_t at[8] = {0x10000, 0x20000, 0x30000, 0x40000, 0x50000, 0x60000, 0x70000, 0x78000};
    std::vector<uint8_t> image(0x100000, 0x90);
    Plant(image, specs[0]->pattern, at[0]);
    Plant(image, specs[1]->pattern, at[1]);
    Plant(image, specs[2]->pattern, at[2]);
    Plant(image, specs[2]->pattern, at[3]);
    Plant(image, specs[3]->pattern, at[4]);
    Plant(image, specs[3]->pattern, at[5]);
    Plant(image, specs[4]->pattern, at[6]);
    Plant(image, specs[5]->pattern, at[7]);
    const char* const part_names[DCP_TRANSITION_PART_COUNT] = {
        "part TITLE_1", "part START_1", "part START_2", "part OPTION_1", "part OPTION_2", "part GALLERY_1", "part GALLERY_2"};
    const uint8_t part_indexes[DCP_TRANSITION_PART_COUNT] = {11, 18, 21, 102, 105, 139, 142};
    for (const char* const upload_download : {"part UPLOAD_1", "part UPLOAD_2", "part DOWNLOAD_1", "part DOWNLOAD_2"})
        CHECK(ExitGameSpec(upload_download) == nullptr);
    uint64_t part_at[DCP_TRANSITION_PART_COUNT];
    for (size_t i = 0; i < DCP_TRANSITION_PART_COUNT; ++i) {
        const scan::SiteSpec* part = ExitGameSpec(part_names[i]);
        CHECK(part != nullptr && part->expected == 1 && part->anchor == 8);
        if (!part) return;
        part_at[i] = 0x90000 + i * 0x100;
        Plant(image, part->pattern, part_at[i]);
    }
    DcpExitGameSites sites;
    DcpLocateReport report{};
    CHECK(LocateExitGame(image, sites, report));
    CHECK(sites.sheet_fill == at[0] + specs[0]->anchor && sites.row_widget == at[1] + specs[1]->anchor);
    CHECK(sites.help_limits[0] == at[2] + specs[2]->anchor && sites.help_limits[1] == at[3] + specs[2]->anchor);
    CHECK(sites.part_table_loads[0] == at[4] + specs[3]->anchor && sites.part_table_loads[1] == at[5] + specs[3]->anchor);
    CHECK(sites.menu_tick == at[6] + specs[4]->anchor && sites.part_setter == at[7] + specs[5]->anchor);
    for (size_t i = 0; i < DCP_TRANSITION_PART_COUNT; ++i)
        CHECK(sites.transition_parts[i] == part_at[i] + 8 && image[sites.transition_parts[i]] == part_indexes[i]);
    for (size_t i = 0; i < DCP_TRANSITION_PART_COUNT; ++i) {
        std::vector<uint8_t> gone = image, twice = image;
        std::memset(&gone[part_at[i]], 0x90, Parse(ExitGameSpec(part_names[i])->pattern).bytes.size());
        CHECK(!LocateExitGame(gone, sites, report) && std::string_view(report.failure).find(part_names[i]) != std::string_view::npos);
        Plant(twice, ExitGameSpec(part_names[i])->pattern, 0xa0000);
        CHECK(!LocateExitGame(twice, sites, report) && std::string_view(report.failure).find("matches") != std::string_view::npos);
    }
    CHECK(LocateExitGame(image, sites, report));

    for (size_t i = 0; i < 6; ++i) {
        std::vector<uint8_t> missing = image;
        const size_t length = Parse(specs[i]->pattern).bytes.size();
        const size_t first_copy[6] = {0, 1, 2, 4, 6, 7}, second_copy[6] = {0, 0, 3, 5, 0, 0};
        std::memset(&missing[at[first_copy[i]]], 0x90, length);
        if (second_copy[i]) std::memset(&missing[at[second_copy[i]]], 0x90, length);
        CHECK(!LocateExitGame(missing, sites, report) && std::string_view(report.failure).find(names[i]) != std::string_view::npos);
        std::vector<uint8_t> doubled = image;
        Plant(doubled, specs[i]->pattern, 0x80000);
        CHECK(!LocateExitGame(doubled, sites, report) && std::string_view(report.failure).find("matches") != std::string_view::npos);
    }
    std::vector<uint8_t> half = image;
    std::memset(&half[at[3]], 0x90, Parse(specs[2]->pattern).bytes.size());
    CHECK(!LocateExitGame(half, sites, report) && std::string_view(report.failure).find("help limit") != std::string_view::npos);

    DcpExecRange outside{0x1000, 0x2000};
    const DcpImageView elsewhere{image.data(), image.size(), &outside, 1};
    CHECK(dcp_locate_exit_game_sites(&elsewhere, &sites, &report) == 0);
    for (const GameBuild& build : SupportedBuilds()) {
        const DcpGameBuild game = ProfileOf(build);
        const DcpExecRange range{0, image.size()};
        const DcpImageView view{image.data(), image.size(), &range, 1};
        char failure[200];
        CHECK(game.exit_game && dcp_resolve_exit_game(&game, &view, &sites, failure, sizeof(failure)) == 1);
    }
}

struct MappedSite {
    const char* feature;
    const char* name;
    std::vector<uint64_t> at;
};

// The sites the launcher's read-only scan reported for the CUSA10487 1.03 executable.
const std::vector<MappedSite> kEuropeSites = {
    {"True ultrawide", "submit", {0x5811d0}},
    {"True ultrawide", "fill call 1", {0x581ad3}},
    {"True ultrawide", "fill call 2", {0x581bc6}},
    {"True ultrawide", "fill call 3", {0x581879}},
    {"True ultrawide", "frame commit", {0x5a20ad}},
    {"True ultrawide", "queue", {0x597840}},
    {"True ultrawide", "part queue call", {0x57531c}},
    {"True ultrawide", "model draw", {0x579ad0}},
    {"True ultrawide", "widget draw call", {0x4ec2fc, 0x4ed69f, 0x4edc27}},
    {"True ultrawide", "map draw call", {0x541b99}},
    {"True ultrawide", "facility draw call", {0x3a5065}},
    {"True ultrawide", "worldmap draw", {0x549a90}},
    {"True ultrawide", "fade jumps", {0x44294b, 0x44310b}},
    {"True ultrawide", "camera view call", {0x118e9}},
    {"True ultrawide", "view build", {0x110b0}},
    {"True ultrawide", "pass ortho load 1", {0x10686}},
    {"True ultrawide", "pass ortho load 2", {0x106bb}},
    {"True ultrawide", "pass ortho load 3", {0x10699}},
    {"True ultrawide", "pass ortho load 4", {0x106d5}},
    {"True ultrawide", "pass ortho load 5", {0x106f7}},
    {"True ultrawide", "pass ortho load 6", {0x10703}},
    {"System type", "card site 1", {0x4d31f3}},
    {"System type", "card site 2", {0x4d320c}},
    {"System type", "set text", {0x58f0f0}},
    {"Skip offers", "story window", {0x3aed92}},
    {"Skip offers", "tutorial window", {0x3a8ae9}},
    {"Startup save", "step tail", {0x4ddbb0}},
    {"Startup save", "block owner", {0x81a20}},
    {"Startup save", "accept", {0x8bb50}},
    {"Voice language", "cue mapper", {0x68bc0}},
    {"Voice language", "resource acquire", {0x57cbc0}},
    {"Cheats", "hook: damage", {0xd8713}},
    {"Cheats", "hook: use mana", {0xd6d80}},
    {"Cheats", "hook: mana drain", {0x345390}},
    {"Cheats", "hook: arrows", {0x1b0700}},
    {"Cheats", "hook: skill points", {0x537e9a}},
    {"Cheats", "hook: player tick", {0x2ea430}},
    {"Cheats", "hook: durability", {0xfe193}},
    {"Cheats", "hook: lives", {0x2ec7db}},
    {"Cheats", "hook: walk speed", {0x2ecd1a}},
    {"Cheats", "hook: gold shop", {0x52e0c3}},
    {"Cheats", "hook: gold continue", {0x43d572}},
    {"Cheats", "hook: gold stable", {0x3bc6a3}},
    {"Cheats", "hook: gold beggar", {0x3bff32}},
    {"Cheats", "hook: gold base", {0x3c2430}},
    {"Cheats", "hook: gold inn", {0x3c3b92}},
    {"Cheats", "hook: gold inn 2", {0x3c40ea}},
    {"Cheats", "hook: gold list", {0x4953dd}},
    {"Cheats", "hook: gold list 2", {0x49563d}},
    {"Cheats", "hook: gold equip", {0x4cc48a}},
    {"Cheats", "hook: experience", {0x27511}},
    {"Cheats", "hook: skill use", {0x1af0ec}},
    {"Cheats", "hook: skill item wear", {0x225fa0}},
    {"Cheats", "hook: skill item wear mirror", {0x225fc1}},
    {"Cheats", "hook: skill budget", {0x8ab24}},
    {"Cheats", "function: IsDead", {0x281bb0}},
    {"Cheats", "function: Refill", {0xe3bf0}},
    {"Cheats", "function: GetGold", {0x43d470}},
    {"Cheats", "function: AddGold", {0x43d4a0}},
    {"Cheats", "function: RecordMode", {0x83ae0}},
    {"Cheats", "function: RecordStage", {0x86ee0}},
    {"Cheats", "function: MaxLevel", {0x4f950}},
    {"Cheats", "function: LevelEntry", {0x2f9db0}},
    {"Cheats", "function: LevelPattern", {0x81fe0}},
    {"Cheats", "function: SetLevel", {0x81fb0}},
    {"Cheats", "function: WriteStats", {0x82000}},
    {"Cheats", "function: EquipmentCount", {0x224b50}},
    {"Cheats", "function: EquipmentItem", {0x2251c0}},
    {"Cheats", "function: IsGear", {0x21d20}},
    {"Cheats", "function: SetDurability", {0x225ff0}},
    {"Exit Game", "sheet fill", {0x58e340}},
    {"Exit Game", "row widget", {0x53c23a}},
    {"Exit Game", "part table", {0x53cfe7, 0x53d05b}},
    {"Exit Game", "menu tick", {0x53d074}},
    {"Exit Game", "help limit", {0x53d358, 0x53d39a}},
    {"Exit Game", "part setter", {0x578010}},
    {"Exit Game", "part TITLE_1", {0x53cf30}},
    {"Exit Game", "part START_1", {0x53d4cf}},
    {"Exit Game", "part START_2", {0x53d7d2}},
    {"Exit Game", "part OPTION_1", {0x53fb5d}},
    {"Exit Game", "part OPTION_2", {0x53fccf}},
    {"Exit Game", "part GALLERY_1", {0x53feac}},
    {"Exit Game", "part GALLERY_2", {0x53ff2a}},
};

// Sites added after that scan: not reported by it, planted at the US 1.00 offset moved like their neighbours (+0x10).
const std::vector<MappedSite> kEuropeSitesAfterScan = {
    {"Cheats", "hook: skill use mirror", {0x1af120}},
    {"Cheats", "hook: lethal check", {0x29df05}},
};

// The sites the launcher's read-only scan reported for the CUSA10209 1.02 executable (SHA-256 927a3d0f...6ac5, issue #14).
const std::vector<MappedSite> kAsia102Sites = {
    {"True ultrawide", "submit", {0x57f120}},
    {"True ultrawide", "fill call 1", {0x57fa23}},
    {"True ultrawide", "fill call 2", {0x57fb16}},
    {"True ultrawide", "fill call 3", {0x57f7c9}},
    {"True ultrawide", "frame commit", {0x5a000d}},
    {"True ultrawide", "queue", {0x595790}},
    {"True ultrawide", "part queue call", {0x57326c}},
    {"True ultrawide", "model draw", {0x577a20}},
    {"True ultrawide", "widget draw call", {0x4ea6dc, 0x4eba7f, 0x4ec007}},
    {"True ultrawide", "map draw call", {0x53faf9}},
    {"True ultrawide", "facility draw call", {0x3a49f5}},
    {"True ultrawide", "worldmap draw", {0x5479f0}},
    {"True ultrawide", "fade jumps", {0x4420bb, 0x44287b}},
    {"True ultrawide", "camera view call", {0x118e9}},
    {"True ultrawide", "view build", {0x110b0}},
    {"True ultrawide", "pass ortho load 1", {0x10686}},
    {"True ultrawide", "pass ortho load 2", {0x106bb}},
    {"True ultrawide", "pass ortho load 3", {0x10699}},
    {"True ultrawide", "pass ortho load 4", {0x106d5}},
    {"True ultrawide", "pass ortho load 5", {0x106f7}},
    {"True ultrawide", "pass ortho load 6", {0x10703}},
    {"System type", "card site 1", {0x4d1823}},
    {"System type", "card site 2", {0x4d183c}},
    {"System type", "set text", {0x58d040}},
    {"Skip offers", "story window", {0x3ae692}},
    {"Skip offers", "tutorial window", {0x3a8479}},
    {"Startup save", "step tail", {0x4dc1d0}},
    {"Startup save", "block owner", {0x81510}},
    {"Startup save", "accept", {0x8b640}},
    {"Voice language", "cue mapper", {0x686c0}},
    {"Voice language", "resource acquire", {0x57ab10}},
    {"Voice language", "narrator bank select", {0x5b6b4}},
    {"Voice language", "narrator cue base", {0x5bb3e}},
    {"Voice language", "narrator cue helper", {0x5c08a}},
    {"Cheats", "hook: damage", {0xd8163}},
    {"Cheats", "hook: use mana", {0xd67d0}},
    {"Cheats", "hook: mana drain", {0x344df0}},
    {"Cheats", "hook: arrows", {0x1b0150}},
    {"Cheats", "hook: skill points", {0x535e1a}},
    {"Cheats", "hook: player tick", {0x2e9e90}},
    {"Cheats", "hook: durability", {0xfdbe3}},
    {"Cheats", "hook: lives", {0x2ec23b}},
    {"Cheats", "hook: walk speed", {0x2ec77a}},
    {"Cheats", "hook: gold shop", {0x52c0bb}},
    {"Cheats", "hook: gold continue", {0x43cce2}},
    {"Cheats", "hook: gold stable", {0x3bbf33}},
    {"Cheats", "hook: gold beggar", {0x3bf7a2}},
    {"Cheats", "hook: gold base", {0x3c1ca0}},
    {"Cheats", "hook: gold inn", {0x3c35a9}},
    {"Cheats", "hook: gold inn 2", {0x3c3b2c}},
    {"Cheats", "hook: gold list", {0x494b4d}},
    {"Cheats", "hook: gold list 2", {0x494dad}},
    {"Cheats", "hook: gold equip", {0x4cab5a}},
    {"Cheats", "hook: experience", {0x26fa1}},
    {"Cheats", "hook: skill use", {0x1aeb3c}},
    {"Cheats", "hook: skill use mirror", {0x1aeb70}},
    {"Cheats", "hook: skill item wear", {0x2259f0}},
    {"Cheats", "hook: skill item wear mirror", {0x225a11}},
    {"Cheats", "hook: skill budget", {0x8a614}},
    {"Cheats", "hook: lethal check", {0x29d955}},
    {"Cheats", "function: IsDead", {0x281600}},
    {"Cheats", "function: Refill", {0xe3640}},
    {"Cheats", "function: GetGold", {0x43cbe0}},
    {"Cheats", "function: AddGold", {0x43cc10}},
    {"Cheats", "function: RecordMode", {0x835d0}},
    {"Cheats", "function: RecordStage", {0x869d0}},
    {"Cheats", "function: MaxLevel", {0x4f3e0}},
    {"Cheats", "function: LevelEntry", {0x2f9810}},
    {"Cheats", "function: LevelPattern", {0x81ad0}},
    {"Cheats", "function: SetLevel", {0x81aa0}},
    {"Cheats", "function: WriteStats", {0x81af0}},
    {"Cheats", "function: EquipmentCount", {0x2245a0}},
    {"Cheats", "function: EquipmentItem", {0x224c10}},
    {"Cheats", "function: IsGear", {0x21960}},
    {"Cheats", "function: SetDurability", {0x225a40}},
    {"Exit Game", "sheet fill", {0x58c290}},
    {"Exit Game", "row widget", {0x53a1ba}},
    {"Exit Game", "part table", {0x53af47, 0x53afbb}},
    {"Exit Game", "menu tick", {0x53afd4}},
    {"Exit Game", "help limit", {0x53b2b8, 0x53b2fa}},
    {"Exit Game", "part setter", {0x575f60}},
    {"Exit Game", "part TITLE_1", {0x53ae90}},
    {"Exit Game", "part START_1", {0x53b42f}},
    {"Exit Game", "part START_2", {0x53b732}},
    {"Exit Game", "part OPTION_1", {0x53dabd}},
    {"Exit Game", "part OPTION_2", {0x53dc2f}},
    {"Exit Game", "part GALLERY_1", {0x53de0c}},
    {"Exit Game", "part GALLERY_2", {0x53de8a}},
    {"Early Story Online", "lead selector", {0x475b10}},
    {"Early Story Online", "hint call", {0x39c77f}},
    {"Early Story Online", "notice call", {0x36976e}},
    {"Early Story Online", "prompt call", {0x369466}},
    {"Early Story Online", "prompt function", {0x369380}},
    {"Early Story Online", "prompt owner ctor", {0x333069, 0x369129, 0x369219, 0x39c449, 0x3bb829, 0x40c7a9}},
    {"Early Story Online", "manager getter", {0x281080}},
    {"Early Story Online", "town update", {0x3cd280}},
    {"Early Story Online", "town portal call", {0x3cd3c9}},
    {"Early Story Online", "menu press call", {0x432084}},
    {"Early Story Online", "menu step call", {0x4322a1}},
    {"Early Story Online", "menu kind store", {0x432472}},
    {"Early Story Online", "hint create", {0x4af2f0}},
    {"Early Story Online", "hint text", {0x4eaa20}},
    {"Early Story Online", "hint hide", {0x4ead00}},
    {"Early Story Online", "hint refresh", {0x58f000}},
    {"Early Story Online", "room wish", {0x43b3e0}},
    {"Early Story Online", "room wish location", {0x43b4c0}},
    {"Early Story Online", "room info location", {0x4387e9}},
    {"Early Story Online", "host record call", {0x42a05c}},
    {"Early Story Online", "advance story", {0x475bd0}},
    {"Early Story Online", "tier writer", {0x477ca0}},
    {"Early Story Online", "talisman award", {0x476120}},
    {"Early Story Online", "flag body", {0x86810}},
    {"Early Story Online", "flag facade", {0x476fb0, 0x477000, 0x477050}},
};

using SiteLists = std::vector<const std::vector<MappedSite>*>;

const scan::SiteSpec* SpecOf(std::string_view feature, std::string_view name) {
    for (const scan::SiteSpec& spec : scan::Sites()) {
        if (feature == spec.feature && name == spec.name) return &spec;
    }
    return nullptr;
}

// No game bytes: each signature is planted where the scan reported it, so the sites' own layout is what the locators see.
std::vector<uint8_t> BuildSiteMap(const SiteLists& lists) {
    constexpr uint64_t kSize = 0xed0000, kProbes = 0xd00000, kConstantsAt = 0xe00000;
    std::vector<uint8_t> bytes(kSize, 0);
    for (const std::vector<MappedSite>* sites : lists) {
        for (const MappedSite& site : *sites) {
            const scan::SiteSpec* spec = SpecOf(site.feature, site.name);
            CHECK(spec != nullptr);
            if (!spec) continue;
            for (const uint64_t at : site.at) Plant(bytes, spec->pattern, at - spec->anchor);
        }
    }
    uint64_t cursor = kProbes;
    for (size_t probe = 0; probe < dcp_layout_probe_count(); ++probe) {
        size_t expected = 0;
        const char* pattern = dcp_layout_probe_pattern(probe, &expected);
        while (Count(bytes, pattern) < expected) {
            Plant(bytes, pattern, cursor);
            cursor += 0x100;
        }
    }
    WriteLayoutValues(bytes);
    WireUltrawide(bytes, kConstantsAt);
    return bytes;
}

uint64_t SiteIn(const SiteLists& lists, std::string_view feature, std::string_view name, size_t index = 0) {
    for (const std::vector<MappedSite>* sites : lists) {
        for (const MappedSite& site : *sites) {
            if (feature == site.feature && name == site.name) return site.at[index];
        }
    }
    return 0;
}

// The loader's own entry points, given the build's profile, resolve exactly the sites its reporter's scan mapped.
void TestReportedSites(std::string_view label, const SiteLists& lists) {
    const auto build = std::find_if(SupportedBuilds().begin(), SupportedBuilds().end(),
                                     [&](const GameBuild& candidate) { return label == candidate.label; });
    CHECK(build != SupportedBuilds().end());
    if (build == SupportedBuilds().end()) return;
    const std::vector<uint8_t> bytes = BuildSiteMap(lists);
    const DcpExecRange range{0, bytes.size()};
    const DcpImageView view{bytes.data(), bytes.size(), &range, 1};
    const DcpGameBuild profile = ProfileOf(*build);

    DcpFeatureSites located{};
    CHECK(dcp_resolve_features(&profile, &view, 1, 1, &located) == 1);
    if (!located.cheats || !located.ultrawide) {
        std::printf("  %s: %s | %s\n", build->label, located.cheat_failure, located.ultrawide_failure);
    }
    CHECK(located.cheats && located.ultrawide);

    size_t hook = 0, function = 0;
    for (const scan::SiteSpec& spec : scan::Sites()) {
        if (std::string_view(spec.feature) != "Cheats") continue;
        const uint64_t expected = SiteIn(lists, "Cheats", spec.name);
        if (spec.kind == scan::SiteKind::kHook) {
            CHECK(located.cheat_sites.hooks[hook++] == expected);
        } else {
            CHECK(located.cheat_sites.functions[function++] == expected);
        }
    }
    CHECK(hook == DCP_CHEAT_HOOKS && function == DCP_CHEAT_FUNCTIONS);
    std::string mismatch;
    CHECK(cheats::LayoutMatches(located.cheat_sites.layout, &mismatch));

    const auto ultrawide = [&](const char* name, size_t index = 0) { return SiteIn(lists, "True ultrawide", name, index); };
    const DcpUltrawideSites& u = located.ultrawide_sites;
    CHECK(u.submit == ultrawide("submit") && u.frame_commit == ultrawide("frame commit") && u.queue == ultrawide("queue"));
    CHECK(u.fill_calls[0] == ultrawide("fill call 1") && u.fill_calls[1] == ultrawide("fill call 2") &&
          u.fill_calls[2] == ultrawide("fill call 3"));
    CHECK(u.part_queue_call == ultrawide("part queue call") && u.model_draw == ultrawide("model draw"));
    CHECK(u.widget_draw_call == ultrawide("widget draw call") && u.map_draw_call == ultrawide("map draw call"));
    CHECK(u.facility_draw_call == ultrawide("facility draw call") && u.worldmap_draw == ultrawide("worldmap draw"));
    CHECK(u.fade_jumps[0] == ultrawide("fade jumps") && u.fade_jumps[1] == ultrawide("fade jumps", 1));
    CHECK(u.camera_view_call == ultrawide("camera view call") && u.view_build == ultrawide("view build"));
    for (size_t i = 0; i < DCP_ORTHO_LOADS; ++i) {
        CHECK(u.ortho_loads[i] == ultrawide(("pass ortho load " + std::to_string(i + 1)).c_str()));
    }

    DcpExitGameSites exit_game{};
    char failure[192];
    CHECK(dcp_resolve_exit_game(&profile, &view, &exit_game, failure, sizeof(failure)) == 1);
    const auto exit_site = [&](const char* name, size_t index = 0) { return SiteIn(lists, "Exit Game", name, index); };
    CHECK(exit_game.sheet_fill == exit_site("sheet fill") && exit_game.row_widget == exit_site("row widget"));
    CHECK(exit_game.help_limits[0] == exit_site("help limit") && exit_game.help_limits[1] == exit_site("help limit", 1));
    CHECK(exit_game.part_table_loads[0] == exit_site("part table") && exit_game.part_table_loads[1] == exit_site("part table", 1));
    CHECK(exit_game.menu_tick == exit_site("menu tick") && exit_game.part_setter == exit_site("part setter"));
    const char* const parts[DCP_TRANSITION_PART_COUNT] = {"part TITLE_1", "part START_1", "part START_2", "part OPTION_1",
                                                          "part OPTION_2", "part GALLERY_1", "part GALLERY_2"};
    for (size_t i = 0; i < DCP_TRANSITION_PART_COUNT; ++i) CHECK(exit_game.transition_parts[i] == exit_site(parts[i]));

    DcpPartySites party{};
    CHECK(dcp_resolve_party(&profile, &view, &party, failure, sizeof(failure)) == 1 && party.tick == SiteIn(lists, "Cheats", "hook: player tick"));

    // A scan lists no caller of the cue mapper, so a map made from one is not enough for Voice Language.
    DcpVoiceSites voice{};
    CHECK(dcp_resolve_voice_language(&profile, &view, &voice, failure, sizeof(failure)) == 0 &&
          std::string_view(failure).find("cue mapper callers") != std::string_view::npos);

    scan::MappedImage image;
    image.bytes = bytes;
    image.executable = {{0, bytes.size()}};
    const scan::ScanResult scanned = scan::ScanImage(image);
    size_t compared = 0;
    for (const scan::SiteResult& result : scanned.sites) {
        for (const MappedSite& site : *lists.front()) {
            if (std::string_view(site.feature) != result.spec->feature || std::string_view(site.name) != result.spec->name) continue;
            ++compared;
            CHECK(result.status == scan::SiteStatus::kUnique || result.status == scan::SiteStatus::kGroup);
            CHECK(result.matches == site.at);
        }
    }
    CHECK(compared == lists.front()->size());
    CHECK(scan::kLzcntInstructions == 62 && scan::kLzcntDigest == 0xe53eadb9792a3e8eull);
}

// The skill-use bag hook reads the owner from r12, so on every build its call must come before the epilogue that gives r12
// back to the caller for the tail-jumped decrement.
void TestSkillUseMirrorSite() {
    const scan::SiteSpec* spec = SpecOf("Cheats", "hook: skill use mirror");
    CHECK(spec != nullptr);
    if (!spec) return;
    const std::vector<int> bytes = Parse(spec->pattern).bytes;
    const auto first = [&](std::initializer_list<int> sequence) {
        return size_t(std::search(bytes.begin(), bytes.end(), sequence.begin(), sequence.end()) - bytes.begin());
    };
    const size_t pop_r12 = first({0x41, 0x5c});
    CHECK(bytes[spec->anchor] == 0xe8 && spec->anchor + 5u <= pop_r12 && pop_r12 < bytes.size());
    CHECK(first({0xe9}) > pop_r12);
    CHECK(bytes[0] == 0x49 && bytes[1] == 0x8b && bytes[2] == 0xbf); // mov rdi, [r15 + the bag]
    uint32_t bag = 0;
    for (size_t i = 0; i < 4; ++i) bag |= uint32_t(bytes[3 + i]) << (8 * i);
    CHECK(bag == cheats::kUnitEquipmentOffset + cheats::kEquipmentBagOffset);
    CHECK(spec->reference[0] == cheats::Hooks()[21].offset && cheats::Hooks()[21].handler == cheats::SkillUseMirrorHook);
}

void TestKnownBuildsOnly() {
    const std::vector<uint8_t> everything = BuildSynthetic(5, true);
    const DcpExecRange range{0, everything.size()};
    const DcpImageView view{everything.data(), everything.size(), &range, 1};
    DcpFeatureSites out;

    // An executable that holds every signature is still nothing to the loader without a supported locator profile.
    CHECK(dcp_resolve_features(nullptr, &view, 1, 1, &out) == 0 && !out.cheats && !out.ultrawide);
    CHECK(std::string_view(out.cheat_failure).find("not a supported build") != std::string_view::npos);
    const DcpFeatureSites untouched = out;
    CHECK(dcp_resolve_features(nullptr, nullptr, 1, 1, &out) == 0);
    (void)untouched;

    for (const GameBuild& build : SupportedBuilds()) {
        const DcpGameBuild game = ProfileOf(build);
        const int resolved = dcp_resolve_features(&game, &view, 1, 1, &out);
        if (build.features_by_signature) {
            CHECK(resolved == 1);
            CHECK(out.cheats == (build.cheats ? 1 : 0) && out.ultrawide == (build.true_ultrawide ? 1 : 0));
        } else {
            CHECK(resolved == 0 && !out.cheats && !out.ultrawide);
        }
        // A feature the profile does not offer, or the caller did not ask for, is not resolved either.
        CHECK(dcp_resolve_features(&game, &view, 0, 0, &out) == 0);
        DcpGameBuild only_ultrawide = game;
        only_ultrawide.features_by_signature = 1;
        only_ultrawide.cheats = 0;
        dcp_resolve_features(&only_ultrawide, &view, 1, 1, &out);
        CHECK(!out.cheats && out.ultrawide == (build.true_ultrawide ? 1 : 0));
    }

    // Every build but CUSA10454 1.00, whose tables are the reference, uses the locators, with cheats and True Ultrawide;
    // Voice Language is located on every build, the Japanese ones included.
    for (const GameBuild& build : SupportedBuilds()) {
        const bool reference = std::string_view(build.label) == "CUSA10454 1.00 USA";
        CHECK(build.features_by_signature == !reference);
        CHECK(build.cheats && build.true_ultrawide && build.voice_language);
    }

    const std::string loader = [] {
        std::ifstream in(fs::path(DCP_SOURCE_DIR) / "src" / "loader.c", std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }();
    CHECK(loader.find("dcp_resolve_features(known_build ? &game_build : NULL,") != std::string::npos);
    CHECK(loader.find("dcp_resolve_voice_language(&game_build,") != std::string::npos);
    CHECK(loader.find("dcp_locate_") == std::string::npos);
    const std::string voices = [] {
        std::ifstream in(fs::path(DCP_SOURCE_DIR) / "src" / "voice_hooks.c", std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }();
    CHECK(voices.find("runtime_set_language") == std::string::npos && voices.find("text_language") == std::string::npos &&
          voices.find("settings_file") == std::string::npos);
}


// Voice Language on a synthetic executable laid out unlike any build: the seven mapper callers, the narrator tables and
// the five signatures planted where no build has them. CUSA10209 1.02 and CUSA10487 1.03 have no local image; this is
// what their locator is checked against.
struct VoiceMap {
    std::vector<uint8_t> bytes;
    uint64_t calls[DCP_VOICE_CUE_CALLS];
};

constexpr uint64_t kVoiceMapper = 0x10000, kVoiceAcquire = 0x30000, kVoiceBank = 0x31000, kVoiceBase = 0x32000,
                   kVoiceHelper = 0x33000, kVoiceTable = 0x60000, kVoiceBases = 0x61000, kVoiceNames = 0x62000,
                   kVoiceSize = 0x70000;

const char* VoicePattern(const char* name) {
    for (const scan::SiteSpec& spec : scan::Sites()) {
        if (std::string_view(spec.feature) == "Voice language" && std::string_view(spec.name) == name) return spec.pattern;
    }
    return "";
}

void PutRip(std::vector<uint8_t>& bytes, uint64_t displacement, uint64_t end, uint64_t target) {
    const int32_t value = int32_t(int64_t(target) - int64_t(end));
    std::memcpy(&bytes[displacement], &value, 4);
}

void PutBranch(std::vector<uint8_t>& bytes, uint64_t at, uint8_t opcode, uint64_t target) {
    bytes[at] = opcode;
    PutRip(bytes, at + 1, at + 5, target);
}

VoiceMap BuildVoiceMap(const uint32_t numbers[DCP_NARRATOR_BANKS]) {
    VoiceMap map;
    map.bytes.assign(kVoiceSize, 0);
    Plant(map.bytes, VoicePattern("cue mapper"), kVoiceMapper - 1);
    for (int i = 0; i < DCP_VOICE_CUE_CALLS; ++i) {
        map.calls[i] = 0x20000 + 0x180 * uint64_t(i);
        PutBranch(map.bytes, map.calls[i], i == DCP_VOICE_CUE_TAIL_JUMP ? 0xe9 : 0xe8, kVoiceMapper);
    }
    Plant(map.bytes, VoicePattern("resource acquire"), kVoiceAcquire);
    Plant(map.bytes, VoicePattern("narrator bank select"), kVoiceBank - 11);
    PutRip(map.bytes, kVoiceBank - 4, kVoiceBank, kVoiceTable);
    Plant(map.bytes, VoicePattern("narrator cue base"), kVoiceBase - 30);
    PutRip(map.bytes, kVoiceBase - 27, kVoiceBase - 23, kVoiceBases);
    Plant(map.bytes, VoicePattern("narrator cue helper"), kVoiceHelper - 26);
    PutRip(map.bytes, kVoiceHelper - 13, kVoiceHelper - 9, kVoiceBases);
    for (int i = 0; i < DCP_NARRATOR_BANKS; ++i) {
        const uint64_t acb = kVoiceNames + 64 * uint64_t(i), awb = acb + 32;
        std::snprintf(reinterpret_cast<char*>(&map.bytes[acb]), 32, "SND/NARRATION_GM%u.ACB", unsigned(numbers[i]));
        std::snprintf(reinterpret_cast<char*>(&map.bytes[awb]), 32, "SND/NARRATION_GM%u.AWB", unsigned(numbers[i]));
        std::memcpy(&map.bytes[kVoiceTable + 16 * uint64_t(i)], &acb, 8);
        std::memcpy(&map.bytes[kVoiceTable + 16 * uint64_t(i) + 8], &awb, 8);
        if (i < 8) std::memcpy(&map.bytes[kVoiceBases + 4 * uint64_t(i)], &numbers[i], 4);
    }
    return map;
}

bool LocateVoices(const std::vector<uint8_t>& bytes, DcpVoiceSites& out, DcpLocateReport& report) {
    const DcpExecRange range{0, kVoiceTable};
    const DcpImageView view{bytes.data(), bytes.size(), &range, 1};
    out = {};
    report = {};
    return dcp_locate_voice_sites(&view, &out, &report) != 0;
}

void TestVoiceLocatorSynthetic() {
    static const uint32_t us[DCP_NARRATOR_BANKS] = {120000, 20000, 130000, 140000, 150000, 160000, 170000,
                                                   180000, 30000,  40000,  50000,  60000,  70000,  80000};
    static const uint32_t japan[DCP_NARRATOR_BANKS] = {20000, 120000, 30000, 40000, 50000, 60000, 70000,
                                                      80000, 130000, 140000, 150000, 160000, 170000, 180000};
    for (const uint32_t* numbers : {us, japan}) {
        const VoiceMap map = BuildVoiceMap(numbers);
        DcpVoiceSites sites;
        DcpLocateReport report;
        CHECK(LocateVoices(map.bytes, sites, report));
        CHECK(sites.cue_mapper == kVoiceMapper && sites.resource_acquire == kVoiceAcquire &&
              sites.narrator_bank_select == kVoiceBank && sites.narrator_cue_base == kVoiceBase &&
              sites.narrator_cue_helper == kVoiceHelper && sites.narrator_banks == kVoiceTable &&
              sites.narrator_bases == kVoiceBases);
        for (int i = 0; i < DCP_VOICE_CUE_CALLS; ++i) CHECK(sites.cue_calls[i] == map.calls[i]);

        std::vector<uint8_t> memory(map.bytes);
        const size_t room = voice_hooks_stub_bytes();
        memory.resize(kVoiceSize + room);
        CHECK(voice_hooks_install(memory.data(), kVoiceSize, 0x1000, &sites, memory.data() + kVoiceSize, room) == 1);
        for (const uint64_t call : map.calls) CHECK(memory[call] == 0xe9);

        // Every supported profile resolves through the same gate; none without a profile or with the feature off.
        const DcpExecRange range{0, kVoiceTable};
        const DcpImageView view{map.bytes.data(), map.bytes.size(), &range, 1};
        char failure[192];
        for (const GameBuild& build : SupportedBuilds()) {
            DcpGameBuild profile = ProfileOf(build);
            DcpVoiceSites resolved{};
            CHECK(dcp_resolve_voice_language(&profile, &view, &resolved, failure, sizeof(failure)) == 1 &&
                  resolved.cue_mapper == kVoiceMapper);
            profile.voice_language = 0;
            CHECK(dcp_resolve_voice_language(&profile, &view, &resolved, failure, sizeof(failure)) == 0 &&
                  std::string_view(failure).find("not a supported game build") != std::string_view::npos);
        }
        DcpVoiceSites none{};
        CHECK(dcp_resolve_voice_language(nullptr, &view, &none, failure, sizeof(failure)) == 0);
    }

    const auto refused = [](std::vector<uint8_t> bytes, std::string_view why) {
        DcpVoiceSites sites;
        DcpLocateReport report;
        const bool located = LocateVoices(bytes, sites, report);
        if (located || std::string_view(report.failure).find(why) == std::string_view::npos) {
            std::printf("  voice locator: expected \"%.*s\", got \"%s\"\n", int(why.size()), why.data(), report.failure);
        }
        return !located && std::string_view(report.failure).find(why) != std::string_view::npos;
    };
    const VoiceMap good = BuildVoiceMap(us);
    {
        std::vector<uint8_t> bytes = good.bytes;
        bytes[good.calls[3]] = 0x90;
        CHECK(refused(bytes, "cue mapper callers"));
    }
    {
        std::vector<uint8_t> bytes = good.bytes;
        PutBranch(bytes, 0x28000, 0xe8, kVoiceMapper);
        CHECK(refused(bytes, "cue mapper callers"));
    }
    {
        std::vector<uint8_t> bytes = good.bytes;
        bytes[good.calls[DCP_VOICE_CUE_TAIL_JUMP]] = 0xe8;
        bytes[good.calls[2]] = 0xe9;
        CHECK(refused(bytes, "not the verified calls and tail jump"));
    }
    {
        std::vector<uint8_t> bytes = good.bytes;
        Plant(bytes, VoicePattern("cue mapper"), 0x40000);
        CHECK(refused(bytes, "cue mapper"));
    }
    {
        std::vector<uint8_t> bytes = good.bytes;
        Plant(bytes, VoicePattern("resource acquire"), 0x48000);
        CHECK(refused(bytes, "resource acquire"));
    }

    // A narrator that does not validate turns off the narrator route only: the mapper callers and the bank pin are
    // still located, and the hooks install without the narrator's.
    const auto narrator_off = [&](const std::vector<uint8_t>& bytes, DcpNarratorRoute route, std::string_view why) {
        DcpVoiceSites sites;
        DcpLocateReport report;
        const bool located = LocateVoices(bytes, sites, report);
        const bool ok = located && sites.narrator_route == route &&
                        std::string_view(sites.narrator_failure).find(why) != std::string_view::npos &&
                        sites.cue_mapper == kVoiceMapper && sites.resource_acquire == kVoiceAcquire &&
                        std::equal(std::begin(sites.cue_calls), std::end(sites.cue_calls), std::begin(good.calls)) &&
                        !sites.narrator_bank_select && !sites.narrator_cue_base && !sites.narrator_cue_helper &&
                        !sites.narrator_banks && !sites.narrator_bases;
        if (!ok) {
            std::printf("  voice locator: expected narrator %s \"%.*s\", got %d %s \"%s\" \"%s\"\n",
                        dcp_narrator_route_reason(route), int(why.size()), why.data(), sites.narrator_route,
                        located ? "located" : "refused", sites.narrator_failure, report.failure);
            return false;
        }
        std::vector<uint8_t> memory(bytes);
        const size_t room = voice_hooks_stub_bytes();
        memory.resize(kVoiceSize + room);
        voice_hooks_configure(DCP_VOICE_JAPANESE, DCP_VOICE_ALL_CATEGORIES);
        const bool installed = voice_hooks_install(memory.data(), kVoiceSize, 0x1000, &sites, memory.data() + kVoiceSize, room) == 1;
        const unsigned routed = voice_hooks_routed();
        voice_hooks_configure(DCP_VOICE_DEFAULT, 0);
        bool cue_hooks = true;
        for (const uint64_t call : good.calls) cue_hooks = cue_hooks && memory[call] == 0xe9;
        return installed && cue_hooks && memory[kVoiceAcquire] == 0xe9 &&
               std::memcmp(&memory[kVoiceBank], &bytes[kVoiceBank], 9) == 0 &&
               std::memcmp(&memory[kVoiceBase], &bytes[kVoiceBase], 8) == 0 &&
               std::memcmp(&memory[kVoiceHelper], &bytes[kVoiceHelper], 7) == 0 &&
               routed == (DCP_VOICE_ALL_CATEGORIES & ~DCP_VOICE_CATEGORY_BIT(DCP_VOICE_NARRATOR));
    };
    const auto with_numbers = [&](int index, uint32_t number) {
        uint32_t numbers[DCP_NARRATOR_BANKS];
        std::memcpy(numbers, us, sizeof(numbers));
        numbers[index] = number;
        return BuildVoiceMap(numbers).bytes;
    };
    const auto put_pointer = [](std::vector<uint8_t>& bytes, int index, int awb, uint64_t value) {
        std::memcpy(&bytes[kVoiceTable + 16 * uint64_t(index) + 8 * uint64_t(awb)], &value, 8);
    };
    {
        std::vector<uint8_t> bytes = good.bytes;
        PutRip(bytes, kVoiceHelper - 13, kVoiceHelper - 9, kVoiceBases + 4);
        CHECK(narrator_off(bytes, DCP_NARRATOR_ROUTE_TABLES, "read different tables"));
    }
    {
        std::vector<uint8_t> bytes = good.bytes;
        PutRip(bytes, kVoiceBank - 4, kVoiceBank, kVoiceSize - 16);
        CHECK(narrator_off(bytes, DCP_NARRATOR_ROUTE_TABLES, "narrator tables"));
    }
    {
        std::vector<uint8_t> bytes = good.bytes;
        bytes[kVoiceBase - 30 + 31] ^= 0xff;
        CHECK(narrator_off(bytes, DCP_NARRATOR_ROUTE_SITES, "narrator cue base"));
    }
    {
        std::vector<uint8_t> bytes = good.bytes;
        Plant(bytes, VoicePattern("narrator cue helper"), 0x48000);
        CHECK(narrator_off(bytes, DCP_NARRATOR_ROUTE_SITES, "narrator cue helper: 2 matches"));
    }
    {
        std::vector<uint8_t> bytes = good.bytes;
        bytes[kVoiceNames + 64 * 4 + 2] = 'X'; /* the fifth narrator's ACB is not a narrator name */
        CHECK(narrator_off(bytes, DCP_NARRATOR_ROUTE_NAMES, "not a narrator ACB/AWB name pair"));
    }
    {
        std::vector<uint8_t> bytes = good.bytes;
        bytes[kVoiceNames + 64 * 4 + 32 + 17] = '9'; /* the fifth narrator's AWB names another bank */
        CHECK(narrator_off(bytes, DCP_NARRATOR_ROUTE_NAMES, "not a narrator ACB/AWB name pair"));
    }
    {
        std::vector<uint8_t> bytes = good.bytes;
        std::memcpy(&bytes[kVoiceNames + 64 * 6 + 32 + 23], "AWX", 3); /* an ACB paired with a non-AWB file */
        CHECK(narrator_off(bytes, DCP_NARRATOR_ROUTE_NAMES, "not a narrator ACB/AWB name pair"));
    }
    {
        std::vector<uint8_t> bytes = good.bytes;
        put_pointer(bytes, 3, 1, 0); /* no AWB */
        CHECK(narrator_off(bytes, DCP_NARRATOR_ROUTE_NAMES, "not a narrator ACB/AWB name pair"));
        bytes = good.bytes;
        put_pointer(bytes, 3, 0, kVoiceSize + 0x100); /* an entry outside the image */
        CHECK(narrator_off(bytes, DCP_NARRATOR_ROUTE_NAMES, "not a narrator ACB/AWB name pair"));
        bytes = good.bytes;
        put_pointer(bytes, 3, 0, kVoiceNames + 64 * 4 + 32); /* an ACB entry pointing to another narrator's AWB */
        CHECK(narrator_off(bytes, DCP_NARRATOR_ROUTE_NAMES, "not a narrator ACB/AWB name pair"));
    }
    CHECK(narrator_off(with_numbers(13, 90000), DCP_NARRATOR_ROUTE_NAMES, "not a narrator ACB/AWB name pair"));
    CHECK(narrator_off(with_numbers(13, 15000), DCP_NARRATOR_ROUTE_NAMES, "not a narrator ACB/AWB name pair"));
    CHECK(narrator_off(with_numbers(13, 1080000), DCP_NARRATOR_ROUTE_NAMES, "not a narrator ACB/AWB name pair"));
    CHECK(narrator_off(with_numbers(13, 70000), DCP_NARRATOR_ROUTE_TWINS, "a narrator without its other language"));
    CHECK(narrator_off(with_numbers(7, 120000), DCP_NARRATOR_ROUTE_TWINS, "a narrator listed twice"));
    {
        std::vector<uint8_t> bytes = good.bytes; /* 180000 -> 190000 in both names: 80000 loses its English twin */
        bytes[kVoiceNames + 64 * 7 + 17] = '9';
        bytes[kVoiceNames + 64 * 7 + 32 + 17] = '9';
        CHECK(narrator_off(bytes, DCP_NARRATOR_ROUTE_NAMES, "not a narrator ACB/AWB name pair"));
    }
    {
        std::vector<uint8_t> bytes = good.bytes;
        const uint32_t other = 50000;
        std::memcpy(&bytes[kVoiceBases + 4 * 2], &other, 4);
        CHECK(narrator_off(bytes, DCP_NARRATOR_ROUTE_BASES, "differ from the bank table"));
    }
    {
        // The Japanese builds' order with the USA builds' cue bases: same 14 names, different table order.
        uint32_t bases[8];
        std::memcpy(bases, us, sizeof(bases));
        std::vector<uint8_t> bytes = BuildVoiceMap(japan).bytes;
        std::memcpy(&bytes[kVoiceBases], bases, sizeof(bases));
        CHECK(narrator_off(bytes, DCP_NARRATOR_ROUTE_BASES, "differ from the bank table"));
    }

    // The table as the loader leaves it: every entry relocated to its loaded address. Before Q7V2N's fix this refused
    // the whole feature ("not a narrator ACB/AWB name pair") and turned every route off.
    for (const uint32_t* numbers : {us, japan}) {
        const VoiceMap map = BuildVoiceMap(numbers);
        std::vector<uint8_t> memory(map.bytes);
        const uint64_t loaded = uint64_t(uintptr_t(memory.data()));
        for (int i = 0; i < 2 * DCP_NARRATOR_BANKS; ++i) {
            uint64_t entry = 0;
            std::memcpy(&entry, &memory[kVoiceTable + 8 * uint64_t(i)], 8);
            entry += loaded;
            std::memcpy(&memory[kVoiceTable + 8 * uint64_t(i)], &entry, 8);
        }
        uint64_t first = 0;
        std::memcpy(&first, &memory[kVoiceTable], 8);
        CHECK(PreFixNarratorName(memory.data(), kVoiceSize, first, "ACB") == 0);
        DcpVoiceSites sites;
        DcpLocateReport report;
        CHECK(LocateVoices(memory, sites, report) && sites.narrator_route == DCP_NARRATOR_ROUTE_OK &&
              sites.narrator_banks == kVoiceTable && sites.narrator_bases == kVoiceBases);
        put_pointer(memory, 2, 1, loaded + kVoiceSize + 0x100); /* a loaded address past the image */
        CHECK(narrator_off(memory, DCP_NARRATOR_ROUTE_NAMES, "not a narrator ACB/AWB name pair"));
    }
}


struct EarlyStoryReference {
    const char* label;
    DcpEarlyStorySites sites;
};

// {selector, online check, hint call, notice call, host record call, {advance, tier, talisman, flag}, prompt call,
// prompt function, prompt owner vtable, town portal call, menu press call, menu step call, menu kind store, hint create,
// hint text, hint hide, hint refresh, room wish, room wish location test, room info location store.
const EarlyStoryReference kEarlyStory[] = {
    {"CUSA10454 1.00 USA",
     {0x4763d0, 0x86f00, 0x39cd1f, 0x369d0e, 0x42a8dc, {0x476490, 0x478560, 0x4769e0, 0x4778c0}, 0x369a06, 0x369920, 0xb14ce8, 0x3cdbd9,
      0x432904, 0x432b21, 0x432cf2, 0x4b09f0, 0x4ec630, 0x4ec910, 0x5910a0, 0x43bc60, 0x43bd40, 0x439069}},
    {"CUSA07001 1.00 Japan",
     {0x4755d0, 0x865e0, 0x39c29f, 0x3692ae, 0x429acc, {0x475690, 0x477760, 0x475be0, 0x476ac0}, 0x368fa6, 0x368ec0, 0xb10958, 0x3cce49,
      0x431b34, 0x431d51, 0x431f22, 0x4aed00, 0x4ea490, 0x4ea770, 0x58f140, 0x43ae90, 0x43af70, 0x438299}},
    {"CUSA07001 1.02 Japan",
     {0x4759a0, 0x86880, 0x39c60f, 0x3695fe, 0x429eec, {0x475a60, 0x477b30, 0x475fb0, 0x476e90}, 0x3692f6, 0x369210, 0xb10958, 0x3cd259,
      0x431f14, 0x432131, 0x432302, 0x4af180, 0x4eaa10, 0x4eacf0, 0x58f070, 0x43b270, 0x43b350, 0x438679}},
    {"CUSA07001 1.03 Japan",
     {0x475a60, 0x86880, 0x39c65f, 0x36964e, 0x429fac, {0x475b20, 0x477bf0, 0x476070, 0x476f50}, 0x369346, 0x369260, 0xb10958, 0x3cd2a9,
      0x431fd4, 0x4321f1, 0x4323c2, 0x4af240, 0x4eaad0, 0x4eadb0, 0x58f130, 0x43b330, 0x43b410, 0x438739}},
    {"CUSA10209 1.00 Asia/Hong Kong",
     {0x475740, 0x86750, 0x39c40f, 0x36941e, 0x429c3c, {0x475800, 0x4778d0, 0x475d50, 0x476c30}, 0x369116, 0x369030, 0xb59978, 0x3ccfb9,
      0x431ca4, 0x431ec1, 0x432092, 0x4aee70, 0x4ea4a0, 0x4ea780, 0x58f1a0, 0x43b000, 0x43b0e0, 0x438409}},
};

bool SameEarlyStory(const DcpEarlyStorySites& a, const DcpEarlyStorySites& b) {
    return a.selector == b.selector && a.online_accessor == b.online_accessor && a.hint_call == b.hint_call &&
           a.notice_call == b.notice_call && a.host_record_call == b.host_record_call &&
           !std::memcmp(a.writers, b.writers, sizeof(a.writers)) && a.prompt_call == b.prompt_call &&
           a.prompt_function == b.prompt_function && a.prompt_owner_vtable == b.prompt_owner_vtable &&
           a.town_portal_call == b.town_portal_call && a.menu_press_call == b.menu_press_call &&
           a.menu_step_call == b.menu_step_call && a.menu_kind_store == b.menu_kind_store && a.hint_create == b.hint_create &&
           a.hint_text == b.hint_text && a.hint_hide == b.hint_hide && a.hint_refresh == b.hint_refresh &&
           a.room_wish_function == b.room_wish_function && a.room_wish_location == b.room_wish_location &&
           a.room_info_location == b.room_info_location;
}

uint64_t CallTarget(const std::vector<uint8_t>& bytes, uint64_t at) {
    int32_t displacement = 0;
    std::memcpy(&displacement, &bytes[at + 1], sizeof(displacement));
    return uint64_t(int64_t(at) + 5 + displacement);
}

size_t CallsTo(const std::vector<uint8_t>& bytes, uint64_t size, uint64_t function) {
    size_t calls = 0;
    for (uint64_t i = 0; i + 5 <= size; ++i) calls += bytes[i] == 0xe8 && CallTarget(bytes, i) == function;
    return calls;
}

// The game's own "Go online?" prompt (its Castle Gate and Stables facility step), shipped and with the feature installed.
void CheckOriginalPrompt(const std::vector<uint8_t>& shipped, const std::vector<uint8_t>& patched, uint64_t size,
                         const DcpEarlyStorySites& sites, const GuestHook (&hooks)[EARLY_STORY_HOOKS]) {
    const uint64_t wrapper = CallTarget(shipped, sites.prompt_call);
    CHECK(CallTarget(shipped, wrapper + 17) == sites.online_accessor && CallsTo(shipped, size, sites.online_accessor) == 1);
    // The prompt's own call still reaches the wrapper and the one accessor; the accessor answers as shipped (step > 0x2c)
    // or, for a host with the setting on, from step 4.
    const auto reaches_accessor = [&](const std::vector<uint8_t>& bytes) {
        return bytes[sites.prompt_call] == 0xe8 && CallTarget(bytes, sites.prompt_call) == wrapper &&
               CallTarget(bytes, wrapper + 17) == sites.online_accessor;
    };
    CHECK(reaches_accessor(shipped) && reaches_accessor(patched));
    CHECK(shipped[sites.online_accessor + 3] == 0x2c);
    const auto offered_shipped = [&](int step) { return step > shipped[sites.online_accessor + 3]; };
    CHECK(!offered_shipped(4) && !offered_shipped(5) && !offered_shipped(44) && offered_shipped(45) && offered_shipped(60));
    early_story_set_active(1);
    early_story_session_reset();
    early_story_set_hosting(0);
    for (int step = 0; step <= 60; ++step) CHECK(early_story_online_unlocked(step) == offered_shipped(step));
    early_story_set_hosting(1);
    for (int step = 0; step <= 60; ++step) CHECK(early_story_online_unlocked(step) == offered_shipped(step));
    early_story_grant_access();
    for (int step = 0; step <= 60; ++step) CHECK(early_story_online_unlocked(step));
    early_story_set_hosting(0);
    for (int step = 0; step <= 60; ++step) CHECK(early_story_online_unlocked(step) == offered_shipped(step));
    CHECK(patched[sites.online_accessor] == 0xe9 && hooks[8].offset == sites.online_accessor &&
          hooks[8].handler == early_story_online_check_hook);
    for (const GuestHook& hook : hooks)
        CHECK(hook.offset + hook.length <= sites.prompt_call || hook.offset >= sites.prompt_call + 5);

    // Once per run: the runtime byte [manager+0x14d] has a single writer in the game, the prompt's eligible branch.
    const uint64_t once = sites.prompt_call + 0x57;
    const uint8_t once_store[] = {0xc6, 0x83, 0x4d, 0x01, 0x00, 0x00, 0x01};
    CHECK(std::equal(std::begin(once_store), std::end(once_store), shipped.begin() + once));
    std::vector<uint64_t> once_writers;
    for (uint64_t i = 0; i + 6 <= size; ++i) {
        if ((shipped[i] == 0xc6 || shipped[i] == 0x88) && (shipped[i + 1] & 0xc0) == 0x80 && shipped[i + 2] == 0x4d &&
            shipped[i + 3] == 0x01 && !shipped[i + 4] && !shipped[i + 5])
            once_writers.push_back(i);
    }
    CHECK(once_writers == std::vector<uint64_t>{once});
    CHECK(std::equal(shipped.begin() + sites.prompt_call - 4, shipped.begin() + once + sizeof(once_store),
                     patched.begin() + sites.prompt_call - 4));

    // Reached only from its two facility callers, and every caller of the online wrapper but the hint and the notice
    // (the prompt, the Network Status line, the Network Menu rows) keeps calling it.
    const uint64_t prompt_function = sites.prompt_function;
    CHECK(prompt_function < sites.prompt_call && sites.prompt_call - prompt_function < 0x200);
    const uint8_t prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    CHECK(std::equal(std::begin(prologue), std::end(prologue), shipped.begin() + prompt_function));
    CHECK(CallsTo(shipped, size, prompt_function) == 2 && CallsTo(patched, size, prompt_function) == 2);
    CHECK(CallsTo(shipped, size, wrapper) == 13 && CallsTo(patched, size, wrapper) == 11);

    // The step-45 "Network Menu" notice keeps its save write behind the original threshold.
    const uint8_t notice_store[] = {0xc6, 0x83, 0x9d, 0x01, 0x00, 0x00, 0x01};
    CHECK(std::equal(std::begin(notice_store), std::end(notice_store), patched.begin() + sites.notice_call + 0x21));
    CHECK(hooks[2].offset == sites.notice_call && hooks[2].handler == early_story_original_online_hook);

    // Every other story gate (the Stables' step > 0x28 among them) compares the same step as shipped.
    size_t gates = 0, stables = 0;
    for (uint64_t i = 0; i + 4 <= size; ++i) {
        if (shipped[i] != 0x80 || shipped[i + 1] != 0x7f || shipped[i + 2] != 0x6f || i == sites.online_accessor)
            continue;
        ++gates;
        stables += shipped[i + 3] == 0x28;
        CHECK(std::equal(shipped.begin() + i, shipped.begin() + i + 4, patched.begin() + i));
    }
    CHECK(gates > 40 && stables >= 1);

    // The three other accessors with the same body (B routes, Act-2 stage events, one more) are not hooked.
    const uint8_t accessor_body[] = {0x80, 0x7f, 0x6f, 0x2c, 0x0f, 0x97, 0xc0, 0xc3};
    size_t same_body = 0, same_body_patched = 0;
    for (uint64_t i = 0; i + sizeof(accessor_body) <= size; ++i) {
        same_body += std::equal(std::begin(accessor_body), std::end(accessor_body), shipped.begin() + i);
        same_body_patched += std::equal(std::begin(accessor_body), std::end(accessor_body), patched.begin() + i);
    }
    CHECK(same_body >= 4 && same_body_patched == same_body - 1);
}

bool ContainsBytes(const std::vector<uint8_t>& bytes, uint64_t from, uint64_t length, std::initializer_list<uint8_t> needle) {
    return std::search(bytes.begin() + from, bytes.begin() + from + length, needle.begin(), needle.end()) !=
           bytes.begin() + from + length;
}

uint64_t StoredPointer(const std::vector<uint8_t>& bytes, uint64_t at) {
    uint64_t value = 0;
    std::memcpy(&value, &bytes[at], sizeof(value));
    return value;
}

// The Hydeland trigger: the game's prompt function runs from the town object's update, and everything it relies on is in
// the image as the native side assumes.
void CheckHydelandTrigger(const std::vector<uint8_t>& shipped, const std::vector<uint8_t>& patched, uint64_t size,
                          const DcpEarlyStorySites& sites, const GuestHook (&hooks)[EARLY_STORY_HOOKS]) {
    CHECK(hooks[9].offset == sites.town_portal_call && hooks[9].handler == hydeland_town_hook);
    CHECK(shipped[sites.town_portal_call] == 0xe8 && patched[sites.town_portal_call] == 0xe9);

    // The town menu window's creation is untouched.
    const std::vector<std::vector<uint8_t>> menu_code = {
        {0x80, 0xf9, 0x02, 0x75, 0x0a, 0x4c, 0x89, 0xf7, 0xe8},
        {0x84, 0xc0, 0x0f, 0x84}};
    const auto first = std::search(shipped.begin(), shipped.begin() + size, menu_code[0].begin(), menu_code[0].end());
    CHECK(first != shipped.begin() + size);
    if (first != shipped.begin() + size) {
        const uint64_t at = uint64_t(first - shipped.begin());
        CHECK(std::equal(shipped.begin() + at, shipped.begin() + at + 15, patched.begin() + at));
    }

    // The portal call is the town's own every-frame call with the town object in rdi (mov rdi, rbx before it).
    CHECK(shipped[sites.town_portal_call - 3] == 0x48 && shipped[sites.town_portal_call - 2] == 0x89 &&
          shipped[sites.town_portal_call - 1] == 0xdf);
    // The town state byte the trigger reads is the one the town's input check reads right after its two calls.
    const uint8_t state_load[] = {0x8a, 0x83, 0x04, 0x01, 0x00, 0x00, 0x3c, 0x01};
    CHECK(std::equal(std::begin(state_load), std::end(state_load), shipped.begin() + sites.town_portal_call + 5 + 3 + 5));

    // The prompt function is not patched and still has the two callers the game gave it (Castle Gate and Stables).
    const uint64_t function = sites.prompt_function;
    CHECK(std::equal(shipped.begin() + function, shipped.begin() + function + 0x100, patched.begin() + function));
    CHECK(CallsTo(shipped, size, function) == 2 && CallsTo(patched, size, function) == 2);
    // What it reads and writes of its owner and of the game manager is what the stand-in owner provides.
    CHECK(ContainsBytes(shipped, function, 0x400, {0x49, 0x8b, 0x87, 0xe0, 0x01, 0x00, 0x00}));       // owner + 0x1e0
    CHECK(ContainsBytes(shipped, function, 0x400, {0x41, 0x80, 0x4c, 0x24, 0x51, 0x80}));             // owner + 0x51 |= 0x80
    CHECK(ContainsBytes(shipped, function, 0x400, {0x41, 0x89, 0x84, 0x24, 0x74, 0x0a, 0x00, 0x00}));  // owner + 0xa74
    CHECK(ContainsBytes(shipped, function, 0x400, {0x80, 0xbb, 0x4d, 0x01, 0x00, 0x00, 0x00}));        // manager + 0x14d
    CHECK(sites.prompt_owner_vtable + 0xe0 < size);
    CHECK(ContainsBytes(shipped, StoredPointer(shipped, sites.prompt_owner_vtable + 0xa0), 8,
                        {0x83, 0xfe, 0x03, 0x77, 0x20, 0x48, 0x63, 0xc6}));
    CHECK(ContainsBytes(shipped, StoredPointer(shipped, sites.prompt_owner_vtable + 0xd0), 8,
                        {0x48, 0x8b, 0xbf, 0xe0, 0x01, 0x00, 0x00, 0xe9}));
    // Each manager and context offset the trigger uses is in the manager getter the town update calls first.
    const uint8_t getter[] = {0x48, 0x8b, 0x87, 0x98, 0x09, 0x00, 0x00, 0x48, 0x8b, 0x40, 0x20, 0xc3};
    size_t getters = 0;
    for (uint64_t i = 0; i + sizeof(getter) <= size; ++i)
        getters += std::equal(std::begin(getter), std::end(getter), shipped.begin() + i);
    CHECK(getters == 1);
}

// The Network Menu shortcut rides the scene director's Menu path: the press test is the action test with the Menu
// action, the step check selects the lead record, and the kind store ends the creation path that the creation flag
// guards. Every site is a hook on the shipped bytes.
void CheckMenuShortcut(const std::vector<uint8_t>& shipped, const std::vector<uint8_t>& patched,
                       const DcpEarlyStorySites& sites, const GuestHook (&hooks)[EARLY_STORY_HOOKS]) {
    CHECK(hooks[10].offset == sites.menu_press_call && hooks[10].handler == early_story_menu_press_hook);
    CHECK(hooks[11].offset == sites.menu_step_call && hooks[11].handler == early_story_menu_step_hook);
    CHECK(hooks[12].offset == sites.menu_kind_store && hooks[12].handler == early_story_menu_kind_hook);
    CHECK(hooks[10].length == 5 && hooks[11].length == 5 && hooks[12].length == EARLY_STORY_MENU_KIND_HOOK_LENGTH);
    const uint8_t menu_action[] = {0xbe, 0x23, 0x00, 0x00, 0x00, 0x48, 0x89, 0xc7};
    CHECK(std::equal(std::begin(menu_action), std::end(menu_action), shipped.begin() + sites.menu_press_call - 8));
    CHECK(shipped[sites.menu_press_call] == 0xe8 && shipped[sites.menu_step_call] == 0xe8);
    CHECK(patched[sites.menu_press_call] == 0xe9 && patched[sites.menu_step_call] == 0xe9 &&
          patched[sites.menu_kind_store] == 0xe9);
    // the creation flag's branch lands on the kind store hook, and the kind field is the one the window reads
    const uint64_t test = sites.menu_kind_store - EARLY_STORY_MENU_CREATED_BACK;
    int32_t over = 0;
    std::memcpy(&over, &shipped[test + 6], sizeof(over));
    CHECK(shipped[test] == 0x41 && shipped[test + 1] == 0xf6 && shipped[test + 2] == 0xc7 && shipped[test + 3] == 0x01 &&
          uint64_t(int64_t(test) + 10 + over) == sites.menu_kind_store);
    uint32_t kind_field = 0;
    std::memcpy(&kind_field, &shipped[sites.menu_kind_store - 10 + 2], sizeof(kind_field));
    CHECK(kind_field == EARLY_STORY_MENU_KIND);
    // the press test and the step check are the game's own functions
    const uint64_t press = CallTarget(shipped, sites.menu_press_call), step = CallTarget(shipped, sites.menu_step_call);
    const uint8_t press_prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x89, 0xf0, 0x49, 0x89, 0xfe, 0x48, 0x8d, 0x1c, 0x40};
    CHECK(std::equal(std::begin(press_prologue), std::end(press_prologue), shipped.begin() + press));
    CHECK(shipped[step + 4] == 0xe8 && CallTarget(shipped, step + 4) == sites.selector);
}

// The hint calls the game's top help dialog (window type 0x28) functions, none of them patched: the window function
// asks for that type and sets the top mode, the text function formats into the dialog's text object, the hide sets the
// dialog's hide state, and the dialog's own show method (the one the window's vtable holds) is the one the native side
// verifies before it calls it.
void CheckHintSites(const std::vector<uint8_t>& shipped, const std::vector<uint8_t>& patched, uint64_t size,
                    const DcpEarlyStorySites& sites) {
    CHECK(sites.hint_create && sites.hint_text && sites.hint_hide && sites.hint_refresh);
    for (const uint32_t site : {sites.hint_create, sites.hint_text, sites.hint_hide, sites.hint_refresh}) {
        CHECK(site + 8 <= size && std::equal(shipped.begin() + site, shipped.begin() + site + 8, patched.begin() + site));
    }
    const uint8_t type_request[] = {0xbe, 0x28, 0x00, 0x00, 0x00, 0xe8};
    CHECK(std::equal(std::begin(type_request), std::end(type_request), shipped.begin() + sites.hint_create + 10));
    const uint8_t top_mode[] = {0xbe, 0x07, 0x00, 0x00, 0x00};
    CHECK(std::search(shipped.begin() + sites.hint_create, shipped.begin() + sites.hint_create + 0x30, std::begin(top_mode),
                      std::end(top_mode)) != shipped.begin() + sites.hint_create + 0x30);
    const uint8_t text_object[] = {0x48, 0x81, 0xc7, 0xe0, 0x03, 0x00, 0x00};
    CHECK(std::search(shipped.begin() + sites.hint_text, shipped.begin() + sites.hint_text + 0x80, std::begin(text_object),
                      std::end(text_object)) != shipped.begin() + sites.hint_text + 0x80);
    CHECK(shipped[sites.hint_hide + 15] == 0x2b && shipped[sites.hint_refresh + 7] == 0xff && shipped[sites.hint_refresh + 8] == 0x7f);
    // the show method: a pointer to it sits at the show slot of a vtable that also holds the dialog's hide target
    const uint8_t show_body[] = {0x89, 0xb7, 0x34, 0x06, 0x00, 0x00, 0x88, 0x97, 0x39, 0x06, 0x00, 0x00,
                                 0xbe, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x47, 0x50};
    size_t methods = 0, slots = 0;
    for (uint64_t at = 0; at + sizeof(show_body) <= size; ++at) {
        if (shipped[at] != 0x89 || !std::equal(std::begin(show_body), std::end(show_body), shipped.begin() + at)) continue;
        ++methods;
        for (uint64_t slot = 0; slot + 8 <= size; slot += 8) slots += StoredPointer(shipped, slot) == at;
    }
    CHECK(methods == 2 && slots >= 6);
    CHECK(EARLY_STORY_HINT_WINDOW_TYPE == 0x28 && EARLY_STORY_HINT_SHOW_SLOT == 0x188 && EARLY_STORY_HINT_TEXT_OBJECT == 0x3e0);
}

// The host room: the room wish asks the online wrapper first; its town branch wants a room for the Castle Gate and Stables
// (`or eax, 8; cmp eax, 9` on the location) and otherwise drops it, and the hook sits on that location test.
void CheckRoomWish(const std::vector<uint8_t>& shipped, const std::vector<uint8_t>& patched, const DcpEarlyStorySites& sites,
                   const GuestHook (&hooks)[EARLY_STORY_HOOKS]) {
    CHECK(hooks[13].offset == sites.room_wish_location && hooks[13].handler == early_story_room_wish_hook &&
          hooks[13].length == EARLY_STORY_ROOM_WISH_HOOK_LENGTH);
    CHECK(shipped[sites.room_wish_function + 0x5b] == 0xe8 &&
          CallTarget(shipped, sites.room_wish_function + 0x5b) == CallTarget(shipped, sites.hint_call));
    const uint8_t location_test[] = {0x8b, 0x83, 0xf0, 0x00, 0x00, 0x00, 0x83, 0xc8, 0x08, 0x83, 0xf8, 0x09, 0x75};
    CHECK(std::equal(std::begin(location_test), std::end(location_test), shipped.begin() + sites.room_wish_location - 6));
    CHECK(patched[sites.room_wish_location] == 0xe9 &&
          std::equal(shipped.begin() + sites.room_wish_function, shipped.begin() + sites.room_wish_location,
                     patched.begin() + sites.room_wish_function));
    // the two branches: the Castle Gate path asks for a room (vtable +0x158), the other drops it (+0x160)
    const uint8_t keep[] = {0xff, 0x90, 0x58, 0x01, 0x00, 0x00};
    const uint8_t drop[] = {0xff, 0x90, 0x60, 0x01, 0x00, 0x00};
    const auto window = [&](const uint8_t* bytes, size_t n) {
        return std::search(shipped.begin() + sites.room_wish_location, shipped.begin() + sites.room_wish_location + 0x50, bytes,
                           bytes + n) != shipped.begin() + sites.room_wish_location + 0x50;
    };
    CHECK(window(keep, sizeof(keep)) && window(drop, sizeof(drop)));
}

// The room info friends read: the hook sits on the store of the scene's location, in the builder the room wish calls.
// A friend's game accepts a town host only at location 1 or 9 (`movsx eax, word [rcx+0x5c]; or eax, 8; cmp eax, 9`
// after the stage-type test, 6 = "cannot accept participants"): one such check per build.
void CheckRoomInfo(const std::vector<uint8_t>& shipped, const std::vector<uint8_t>& patched, uint64_t size,
                   const DcpEarlyStorySites& sites, const GuestHook (&hooks)[EARLY_STORY_HOOKS]) {
    CHECK(hooks[14].offset == sites.room_info_location && hooks[14].handler == early_story_room_info_hook &&
          hooks[14].length == EARLY_STORY_ROOM_INFO_HOOK_LENGTH);
    const uint8_t store[] = {0x41, 0x0f, 0xb7, 0x85, 0xf0, 0x00, 0x00, 0x00, 0x66, 0x41, 0x89, 0x47, 0x5c};
    CHECK(std::equal(std::begin(store), std::end(store), shipped.begin() + sites.room_info_location - 8));
    CHECK(patched[sites.room_info_location] == 0xe9);
    const uint64_t builder = CallTarget(shipped, sites.room_wish_function + 0x96);
    CHECK(shipped[sites.room_wish_function + 0x96] == 0xe8 && builder < sites.room_info_location &&
          sites.room_info_location - builder < 0x600);
    const uint8_t guest_check[] = {0x83, 0xf8, 0x09, 0x75};
    const uint8_t guest_location[] = {0x0f, 0xbf, 0x41, 0x5c, 0xbb, 0x06, 0x00, 0x00, 0x00, 0x83, 0xc8, 0x08, 0x83, 0xf8, 0x09};
    size_t checks = 0;
    for (uint64_t at = 0; at + sizeof(guest_location) <= size; ++at) {
        if (shipped[at] != 0x0f || !std::equal(std::begin(guest_location), std::end(guest_location), shipped.begin() + at)) continue;
        checks += at >= 6 && std::equal(std::begin(guest_check), std::end(guest_check), shipped.begin() + at - 5);
    }
    CHECK(checks == 1);
}

std::vector<std::pair<uint64_t, uint64_t>> EarlyStoryPatchRanges(const DcpEarlyStorySites& s) {
    std::vector<std::pair<uint64_t, uint64_t>> ranges = {{s.selector + EARLY_STORY_SELECTOR_EXIT, 8},
                                                         {s.online_accessor, 8},
                                                         {s.hint_call, 5},
                                                         {s.notice_call, 5},
                                                         {s.host_record_call, 5},
                                                         {s.town_portal_call, 5}};
    for (const uint32_t writer : s.writers) ranges.push_back({writer, 8});
    ranges.push_back({s.menu_press_call, 5});
    ranges.push_back({s.menu_step_call, 5});
    ranges.push_back({s.menu_kind_store, EARLY_STORY_MENU_KIND_HOOK_LENGTH});
    ranges.push_back({s.room_wish_location, EARLY_STORY_ROOM_WISH_HOOK_LENGTH});
    ranges.push_back({s.room_info_location, EARLY_STORY_ROOM_INFO_HOOK_LENGTH});
    return ranges;
}

void TestEarlyStory() {
    const DcpExecRange none{0, 0};
    CHECK(dcp_resolve_early_story(nullptr, nullptr, nullptr, nullptr, 0) == 0);
    for (const GameBuild& build : SupportedBuilds()) {
        const EarlyStoryReference* reference = nullptr;
        for (const EarlyStoryReference& r : kEarlyStory)
            if (std::string_view(r.label) == build.label) reference = &r;
        Image image;
        if (!reference || !LoadBootImage(build, image)) {
            std::printf("%s: Early Story Online: DATA UNAVAILABLE (no local boot image); synthetic proof only\n", build.label);
            continue;
        }
        const DcpExecRange range{0, image.bytes.size()};
        const DcpImageView view{image.bytes.data(), image.bytes.size(), &range, 1};
        DcpEarlyStorySites sites{};
        DcpLocateReport report{};
        CHECK(dcp_locate_early_story_sites(&view, &sites, &report) == 1);
        CHECK(SameEarlyStory(sites, reference->sites));
        const DcpGameBuild profile = ProfileOf(build);
        DcpEarlyStorySites resolved{};
        char failure[192] = {};
        CHECK(dcp_resolve_early_story(&profile, &view, &resolved, failure, sizeof(failure)) == 1 &&
              SameEarlyStory(resolved, reference->sites));
        CHECK(dcp_resolve_early_story(nullptr, &view, &resolved, failure, sizeof(failure)) == 0 && failure[0]);

        const size_t base = (image.bytes.size() + 0xfff) / 0x1000 * 0x1000;
        std::vector<uint8_t> copy(base + early_story_online_stub_bytes() + 0x100, 0);
        std::memcpy(copy.data(), image.bytes.data(), image.bytes.size());
        const std::vector<uint8_t> shipped = copy;
        GuestHook hooks[EARLY_STORY_HOOKS] = {};
        const int hook_count = EARLY_STORY_HOOKS;
        const char* why = nullptr;
        CHECK(early_story_online_hooks(copy.data(), image.bytes.size(), &sites, hooks, &why) == hook_count);
        early_story_set_active(0);
        CHECK(early_story_online_install(copy.data(), image.bytes.size(), image.entry, &sites, copy.data() + base,
                                         copy.size() - base) == 1);
        CHECK(early_story_active() == 1 && copy[sites.online_accessor] == 0xe9);
        const auto ranges = EarlyStoryPatchRanges(sites);
        size_t changed_outside = 0, jumps = 0;
        for (uint64_t i = 0; i < image.bytes.size(); ++i) {
            if (copy[i] == shipped[i]) continue;
            bool inside = false;
            for (const auto& [begin, length] : ranges) inside |= i >= begin && i < begin + length;
            changed_outside += !inside;
        }
        for (const auto& range : ranges) jumps += copy[range.first] == 0xe9;
        CHECK(changed_outside == 0 && jumps == size_t(hook_count));
        CHECK(hooks[1].handler == early_story_original_online_hook && hooks[2].handler == early_story_original_online_hook &&
              hooks[0].handler == early_story_selector_hook && hooks[3].handler == early_story_host_record_hook &&
              hooks[4].handler == early_story_advance_story_hook && hooks[7].handler == early_story_flag_hook &&
              hooks[8].handler == early_story_online_check_hook);
        CHECK(hooks[1].offset == sites.hint_call && hooks[2].offset == sites.notice_call);
        CheckOriginalPrompt(shipped, copy, image.bytes.size(), sites, hooks);
        CheckHydelandTrigger(shipped, copy, image.bytes.size(), sites, hooks);
        CheckMenuShortcut(shipped, copy, sites, hooks);
        CheckHintSites(shipped, copy, image.bytes.size(), sites);
        CheckRoomWish(shipped, copy, sites, hooks);
        CheckRoomInfo(shipped, copy, image.bytes.size(), sites, hooks);
        early_story_set_active(0);

        // One site that does not match leaves the game as it shipped.
        for (const size_t victim :
             {size_t(0), size_t(4), size_t(6), size_t(7), size_t(8), size_t(9), size_t(10), size_t(11), size_t(12), size_t(13),
              size_t(14), size_t(15)}) {
            std::vector<uint8_t> broken = shipped;
            const uint64_t at = victim == 0    ? sites.selector + EARLY_STORY_SELECTOR_EXIT + 4
                                : victim == 8  ? sites.online_accessor + 1
                                : victim == 9  ? sites.town_portal_call
                                : victim == 10 ? sites.menu_press_call - 3
                                : victim == 11 ? sites.menu_step_call + 6
                                : victim == 12 ? sites.menu_kind_store + 1
                                : victim == 13 ? sites.menu_kind_store - EARLY_STORY_MENU_CREATED_BACK + 3
                                : victim == 14 ? sites.room_wish_location + 4
                                : victim == 15 ? sites.room_info_location + 4
                                               : sites.writers[victim - 4] + 1;
            broken[at] ^= 0x40;
            const std::vector<uint8_t> before = broken;
            CHECK(early_story_online_install(broken.data(), image.bytes.size(), image.entry, &sites, broken.data() + base,
                                             broken.size() - base) == 0);
            CHECK(broken == before && early_story_active() == 0);
        }
        std::vector<uint8_t> wrong_check = shipped;
        wrong_check[sites.online_accessor + 3] = 0x2d;
        const std::vector<uint8_t> wrong_before = wrong_check;
        CHECK(early_story_online_install(wrong_check.data(), image.bytes.size(), image.entry, &sites,
                                         wrong_check.data() + base, wrong_check.size() - base) == 0 &&
              wrong_check == wrong_before);
        std::vector<uint8_t> no_room = shipped;
        CHECK(early_story_online_install(no_room.data(), image.bytes.size(), image.entry, &sites, no_room.data() + base, 1) == 0 &&
              no_room == shipped && early_story_active() == 0);

        // The locator itself refuses a build whose site changed.
        std::vector<uint8_t> altered = shipped;
        altered[sites.selector + 3] ^= 1;
        const DcpImageView altered_view{altered.data(), altered.size(), &range, 1};
        DcpEarlyStorySites none_found{};
        CHECK(dcp_locate_early_story_sites(&altered_view, &none_found, &report) == 0 && report.failure[0]);
        std::vector<uint8_t> altered_check = shipped;
        altered_check[sites.online_accessor + 3] = 0x2b;
        const DcpImageView check_view{altered_check.data(), altered_check.size(), &range, 1};
        CHECK(dcp_locate_early_story_sites(&check_view, &none_found, &report) == 0);
        std::vector<uint8_t> owner_changed = shipped;
        owner_changed[sites.prompt_owner_vtable + 0xa0] ^= 0x10;
        const DcpImageView owner_view{owner_changed.data(), owner_changed.size(), &range, 1};
        CHECK(dcp_locate_early_story_sites(&owner_view, &none_found, &report) == 0 &&
              std::string_view(report.failure).find("prompt owner") != std::string_view::npos);
        // As loaded: the loader relocates before resolving, so the owner's method slots hold loaded addresses.
        std::vector<uint8_t> relocated = shipped;
        const uint64_t loaded_base = uint64_t(uintptr_t(relocated.data()));
        for (uint64_t slot = 0; slot < 0x300; slot += 8) {
            const uint64_t at = sites.prompt_owner_vtable + slot;
            const uint64_t stored = StoredPointer(relocated, at);
            if (stored && stored < image.bytes.size()) {
                const uint64_t loaded = loaded_base + stored;
                std::memcpy(&relocated[at], &loaded, sizeof(loaded));
            }
        }
        const DcpImageView relocated_view{relocated.data(), relocated.size(), &range, 1};
        DcpEarlyStorySites relocated_sites{};
        CHECK(dcp_locate_early_story_sites(&relocated_view, &relocated_sites, &report) == 1 &&
              SameEarlyStory(relocated_sites, reference->sites));
        std::vector<uint8_t> relocated_wrong = relocated;
        const uint64_t wrong_method = loaded_base + StoredPointer(shipped, sites.prompt_owner_vtable + 0xd0);
        std::memcpy(&relocated_wrong[sites.prompt_owner_vtable + 0xa0], &wrong_method, sizeof(wrong_method));
        const DcpImageView relocated_wrong_view{relocated_wrong.data(), relocated_wrong.size(), &range, 1};
        CHECK(dcp_locate_early_story_sites(&relocated_wrong_view, &none_found, &report) == 0 &&
              std::string_view(report.failure).find("prompt owner") != std::string_view::npos);
        std::vector<uint8_t> outside = relocated;
        const uint64_t elsewhere = loaded_base + relocated.size() + 0x1000;
        std::memcpy(&outside[sites.prompt_owner_vtable + 0xd0], &elsewhere, sizeof(elsewhere));
        const DcpImageView outside_view{outside.data(), outside.size(), &range, 1};
        CHECK(dcp_locate_early_story_sites(&outside_view, &none_found, &report) == 0);
        std::vector<uint8_t> portal_moved = shipped;
        portal_moved[sites.town_portal_call + 1] ^= 0x10;
        const DcpImageView portal_view{portal_moved.data(), portal_moved.size(), &range, 1};
        DcpEarlyStorySites portal_sites{};
        CHECK(dcp_locate_early_story_sites(&portal_view, &portal_sites, &report) == 1 &&
              portal_sites.town_portal_call == sites.town_portal_call);

        // Each of the prompt function's control checks, and the table it loads the input mask from, is required.
        const std::vector<std::vector<uint8_t>> control_checks = {
            {0x41, 0x80, 0x7c, 0x24, 0x5b, 0x03}, {0x8b, 0x40, 0x04, 0x41, 0x85, 0x84, 0x24, 0x14, 0x05, 0x00, 0x00},
            {0x41, 0x80, 0x7e, 0x5b, 0x02}, {0x4c, 0x8b, 0xbb, 0x08, 0x01, 0x00, 0x00}};
        for (const auto& check : control_checks) {
            std::vector<uint8_t> altered_checks = shipped;
            const auto found = std::search(altered_checks.begin() + sites.prompt_function,
                                           altered_checks.begin() + sites.prompt_function + 0xc0, check.begin(), check.end());
            CHECK(found != altered_checks.begin() + sites.prompt_function + 0xc0);
            *found ^= 0x20;
            const std::vector<uint8_t> checks_before = altered_checks;
            CHECK(early_story_online_install(altered_checks.data(), image.bytes.size(), image.entry, &sites,
                                             altered_checks.data() + base, altered_checks.size() - base) == 0 &&
                  altered_checks == checks_before && early_story_active() == 0);
        }
        std::vector<uint8_t> no_mask_table = shipped;
        no_mask_table[sites.prompt_function + EARLY_STORY_PROMPT_MASK_LEA + 2] ^= 0x10;
        const std::vector<uint8_t> mask_before = no_mask_table;
        CHECK(early_story_online_install(no_mask_table.data(), image.bytes.size(), image.entry, &sites,
                                         no_mask_table.data() + base, no_mask_table.size() - base) == 0 &&
              no_mask_table == mask_before);
        std::vector<uint8_t> prompt_elsewhere = shipped;
        prompt_elsewhere[sites.prompt_call + 1] ^= 0x10;
        const DcpImageView prompt_view{prompt_elsewhere.data(), prompt_elsewhere.size(), &range, 1};
        CHECK(dcp_locate_early_story_sites(&prompt_view, &none_found, &report) == 0 &&
              std::string_view(report.failure).find("prompt") != std::string_view::npos);
        std::printf("%s: Early Story Online sites verified (hooks, atomic install, original Go online prompt)\n",
                    build.label);
    }
    const std::vector<uint8_t> empty(0x1000, 0);
    const DcpImageView empty_view{empty.data(), empty.size(), &none, 0};
    DcpEarlyStorySites nothing{};
    DcpLocateReport report{};
    CHECK(dcp_locate_early_story_sites(&empty_view, &nothing, &report) == 0 && report.failure[0]);
}

} // namespace

int main() {
    TestRealImages();
    TestSyntheticLocators();
    TestLayout();
    TestParty();
    TestExitGame();
    TestReportedSites("CUSA10487 1.03 Europe", {&kEuropeSites, &kEuropeSitesAfterScan});
    TestReportedSites("CUSA10209 1.02 Asia/Hong Kong", {&kAsia102Sites});
    TestSkillUseMirrorSite();
    TestKnownBuildsOnly();
    TestVoiceLocatorSynthetic();
    TestEarlyStory();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
