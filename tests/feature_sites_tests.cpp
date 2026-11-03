// The structural locators for Cheats, Japanese NPC Voices and True Ultrawide (src/feature_sites.h), without the game:
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

struct Image {
    std::vector<uint8_t> bytes;
    uint64_t entry = 0;
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
    in.seekg(std::streamoff(bindings * 24 + segments * 24 + names * 128 + relocs * 32), std::ios::cur);
    out.bytes.resize(size);
    return size && in.read(reinterpret_cast<char*>(out.bytes.data()), std::streamsize(size)).good();
}

struct Located {
    DcpFeatureSites sites;
    DcpLocateReport cheat_report{}, voice_report{}, ultrawide_report{};
    bool cheats = false, voices = false, ultrawide = false;
};

Located LocateAll(const std::vector<uint8_t>& bytes) {
    const DcpExecRange range{0, bytes.size()};
    const DcpImageView view{bytes.data(), bytes.size(), &range, 1};
    Located result{};
    result.cheats = dcp_locate_cheat_sites(&view, &result.sites.cheat_sites, &result.cheat_report) != 0;
    result.voices = dcp_locate_voice_sites(&view, &result.sites.voice_sites, &result.voice_report) != 0;
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
// image bytes must be identical. The cheat and NPC voice hooks install at the located sites on every build's image.
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
    CHECK(voice_hooks_install_located(located_voices.data(), size, image.entry, &located.sites.voice_sites,
                                      located_voices.data() + base, room) == 1);
    if (std::string_view(build.label) == "CUSA10454 1.00 USA") {
        std::vector<uint8_t> c(base + room, 0);
        std::memcpy(c.data(), image.bytes.data(), size);
        CHECK(runtime_cheat_hooks_install(c.data(), size, image.entry, c.data() + base, room) == 1);
        CHECK(std::memcmp(c.data(), located_cheats.data(), size) == 0);
        std::vector<uint8_t> e(base + room, 0), f(base + room, 0);
        std::memcpy(e.data(), image.bytes.data(), size);
        std::memcpy(f.data(), image.bytes.data(), size);
        CHECK(voice_hooks_install(e.data(), size, image.entry, e.data() + base, room) == 1);
        CHECK(voice_hooks_install_located(f.data(), size, image.entry, &located.sites.voice_sites, f.data() + base, room) == 1);
        CHECK(std::memcmp(e.data(), f.data(), size) == 0);
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
            CHECK(located.sites.voice_sites.npc_cue == VOICE_NPC_SITE_OFFSET);
            CHECK(located.sites.voice_sites.resource_acquire == VOICE_RES_ACQUIRE_SITE_OFFSET);
        }
        CheckPartyOnRealImage(build, image, located);
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
        if (feature == "Cheats" || feature == "True ultrawide" || feature == "Japanese NPC voices") {
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
    if (layout_values) {
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
        const uint64_t constant = kConstants + i * 16;
        const int32_t displacement = int32_t(int64_t(constant) - int64_t(site) - 8);
        std::memcpy(&bytes[site + 4], &displacement, 4);
        std::memcpy(&bytes[constant], &dcp_pass_ortho_sites[i].value, 4);
    }
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
    offsets.insert(located.sites.voice_sites.npc_cue);
    offsets.insert(located.sites.voice_sites.resource_acquire);
    return offsets;
}

void ExpectAll(const Located& located, const char* what) {
    if (!located.cheats || !located.voices || !located.ultrawide) {
        std::printf("  %s: %s | %s | %s\n", what, located.cheat_report.failure, located.voice_report.failure,
                    located.ultrawide_report.failure);
    }
    CHECK(located.cheats && located.voices && located.ultrawide);
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
    real.insert(VOICE_NPC_SITE_OFFSET);
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
        CHECK(!(result.cheats && result.voices && result.ultrawide));
        for (const uint32_t site : AllOffsets(result)) CHECK(site == 0 || site < shorter.size());
        if (cut == kConstants + 0x20) CHECK(result.cheats && result.voices && !result.ultrawide); // the constants are cut
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
            CHECK(result.cheats && result.voices);
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
    CHECK(SupportedBuilds().size() == 7);
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
// half there, and the same sites for a build that has no real image here (CUSA10454 1.01, CUSA10209 1.02).
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

void TestKnownBuildsOnly() {
    const std::vector<uint8_t> everything = BuildSynthetic(5, true);
    const DcpExecRange range{0, everything.size()};
    const DcpImageView view{everything.data(), everything.size(), &range, 1};
    DcpFeatureSites out;

    // An executable that holds every signature is still nothing to the loader without a supported locator profile.
    CHECK(dcp_resolve_features(nullptr, &view, 1, 1, 1, &out) == 0 && !out.cheats && !out.voices && !out.ultrawide);
    CHECK(std::string_view(out.cheat_failure).find("not a supported build") != std::string_view::npos);
    const DcpFeatureSites untouched = out;
    CHECK(dcp_resolve_features(nullptr, nullptr, 1, 1, 1, &out) == 0);
    (void)untouched;

    for (const GameBuild& build : SupportedBuilds()) {
        const DcpGameBuild game = ProfileOf(build);
        const int resolved = dcp_resolve_features(&game, &view, 1, 1, 1, &out);
        if (build.features_by_signature) {
            CHECK(resolved == 1);
            CHECK(out.cheats == (build.cheats ? 1 : 0) && out.voices == (build.japanese_npc_voices ? 1 : 0) &&
                  out.ultrawide == (build.true_ultrawide ? 1 : 0));
        } else {
            CHECK(resolved == 0 && !out.cheats && !out.voices && !out.ultrawide);
        }
        // A feature the profile does not offer, or the caller did not ask for, is not resolved either.
        CHECK(dcp_resolve_features(&game, &view, 0, 0, 0, &out) == 0);
        DcpGameBuild only_ultrawide = game;
        only_ultrawide.features_by_signature = 1;
        only_ultrawide.cheats = 0;
        only_ultrawide.japanese_npc_voices = 0;
        dcp_resolve_features(&only_ultrawide, &view, 1, 1, 1, &out);
        CHECK(!out.cheats && !out.voices && out.ultrawide == (build.true_ultrawide ? 1 : 0));
    }

    // Every build but CUSA10454 1.00, whose tables are the reference, uses the locators, with cheats and True Ultrawide;
    // the Japanese builds have no NPC voice toggle because they already speak Japanese.
    for (const GameBuild& build : SupportedBuilds()) {
        const bool reference = std::string_view(build.label) == "CUSA10454 1.00 USA";
        CHECK(build.features_by_signature == !reference);
        CHECK(build.cheats && build.true_ultrawide);
        CHECK(build.japanese_npc_voices == !NativeJapaneseNpcVoices(&build));
    }

    // Without the files the voices need, a profile asking for them resolves nothing for them.
    const std::string loader = [] {
        std::ifstream in(fs::path(DCP_SOURCE_DIR) / "src" / "loader.c", std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }();
    CHECK(loader.find("dcp_resolve_features(known_build ? &game_build : NULL,") != std::string::npos);
    CHECK(loader.find("dcp_locate_") == std::string::npos);
    const std::string voices = [] {
        std::ifstream in(fs::path(DCP_SOURCE_DIR) / "src" / "voice_hooks.c", std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }();
    CHECK(voices.find("runtime_set_language") == std::string::npos && voices.find("text_language") == std::string::npos &&
          voices.find("settings_file") == std::string::npos);
}

} // namespace

int main() {
    TestRealImages();
    TestSyntheticLocators();
    TestLayout();
    TestParty();
    TestExitGame();
    TestKnownBuildsOnly();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
