#include "feature_sites.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "build_scan.h"
#include "dcp_launcher.h"
#include "dcp_resolution.h"

namespace {

using dcp::scan::Range;
using dcp::scan::SiteKind;
using dcp::scan::SiteSpec;

struct ProbeField {
    DcpLayoutField field;
    uint8_t position;
    uint8_t width;
};

// A few instructions of the game's own code that read or write the fields of one struct, with the displacements masked:
// the locator reads them back, so the layout comes from this build's instructions and not from the handlers' constants.
struct Probe {
    const char* name;
    uint8_t expected;
    const char* pattern;
    std::vector<ProbeField> fields;
};

const Probe kProbes[] = {
    {"damage frame", 1,
        "48 8b bd ?? ?? ?? ?? 48 8b 07 ff 90 ?? ?? ?? ?? "
        "48 8b 78 78 0f bf b3 ?? ?? ?? ??",
        {{DCP_LAYOUT_DAMAGE_UNIT_FRAME, 3, 4}, {DCP_LAYOUT_CONTEXT_VIRTUAL, 12, 4}}},
    {"unit identity", 1,
        "48 83 bb ?? ?? ?? ?? 00 0f 84 ?? ?? ?? ?? 44 8a "
        "bb ?? ?? ?? ?? 44 0f be b3 ?? ?? ?? ?? bf 05 00 "
        "00 00",
        {{DCP_LAYOUT_UNIT_OWNER, 3, 4}, {DCP_LAYOUT_UNIT_LEVEL, 17, 4}, {DCP_LAYOUT_UNIT_CLASS, 25, 4}}},
    {"owner class", 1,
        "41 8b bf ?? ?? ?? ?? 0f b6 db 89 de e8 ?? ?? ?? "
        "?? 49 89 c6 4d 85 f6 0f 84 ?? ?? ?? ??",
        {{DCP_LAYOUT_OWNER_CLASS, 3, 4}}},
    {"level bar", 1,
        "49 8b 8e ?? ?? ?? ?? 0f b7 49 ?? 0f b6 d9 39 c3 "
        "0f 8d ?? ?? ?? ?? c1 e9 08 89 de 44 0f be f9 44 "
        "89 ff e8 ?? ?? ?? ?? fe c3 44 89 ff 0f b6 f3 e8 "
        "?? ?? ?? ?? 49 8b 8e ?? ?? ?? ?? 31 d2 48 85 c0 "
        "8b 49 ?? 74 03 8b 50 ??",
        {{DCP_LAYOUT_RECORD_LEVEL, 10, 1}, {DCP_LAYOUT_RECORD_XP_PROGRESS, 66, 1}, {DCP_LAYOUT_LEVEL_XP, 71, 1}}},
    {"level up", 1,
        "8b 47 ?? 44 29 e8 0f 82 ?? ?? ?? ?? 8b 55 ?? 0f "
        "b6 5f ?? 39 d3 0f 8d ?? ?? ?? ?? ff c3 81 fa ?? "
        "?? ?? ?? 74 ?? 39 d3 75 ?? 41 80 8c 24 ?? ?? ?? "
        "?? ?? 39 d3 7c ?? 01 47 ?? 44 89 6f ??",
        {{DCP_LAYOUT_RECORD_XP_PROGRESS, 2, 1}, {DCP_LAYOUT_RECORD_LEVEL, 18, 1}, {DCP_LAYOUT_ABSOLUTE_MAX_LEVEL, 31, 4}, {DCP_LAYOUT_RECORD_XP_POOL, 56, 1}, {DCP_LAYOUT_RECORD_XP_PROGRESS, 60, 1}}},
    {"skill points word", 1,
        "0f bf 47 ?? 48 89 45 ?? 77 ?? 8d 40 01 83 fb 62 "
        "66 89 47 ??",
        {{DCP_LAYOUT_SKILL_POINTS, 3, 1}, {DCP_LAYOUT_SKILL_POINTS, 19, 1}}},
    {"unit stat block", 1,
        "8b 08 8b 50 ?? 8b 3e 8b 40 ?? 8b 76 ?? 89 8b ?? "
        "?? ?? ?? 89 93 ?? ?? ?? ?? 89 83 ?? ?? ?? ?? 89 "
        "bb ?? ?? ?? ??",
        {{DCP_LAYOUT_UNIT_STAT, 15, 4}, {DCP_LAYOUT_UNIT_XP, 21, 4}, {DCP_LAYOUT_UNIT_XP_PROGRESS, 27, 4}, {DCP_LAYOUT_UNIT_HP, 33, 4}}},
    {"unit level stats", 1,
        "41 b8 ff ff ff 01 44 23 83 ?? ?? ?? ?? 0f b6 88 "
        "?? ?? ?? ?? 48 89 c7 44 89 f2 48 81 c7 ?? ?? ?? "
        "?? e8 ?? ?? ?? ??",
        {{DCP_LAYOUT_UNIT_LEVEL, 16, 4}, {DCP_LAYOUT_UNIT_STAT, 29, 4}}},
    {"is dead", 1,
        "48 8b 8f ?? ?? ?? ?? 48 85 c9 74 ?? 83 b9 ?? ?? "
        "?? ?? 00 b0 01",
        {{DCP_LAYOUT_OWNER_UNIT, 3, 4}, {DCP_LAYOUT_UNIT_HP, 14, 4}}},
    {"equipment count", 1,
        "48 8b 87 ?? ?? ?? ?? 48 85 c0 74 ?? 8a 80 ?? ?? "
        "?? ?? b1 0a 3c 0a 7f ?? 84 c0 79 ?? 31 c0 c3",
        {{DCP_LAYOUT_EQUIPMENT_BAG, 3, 4}}},
    {"equipment lists", 1,
        "48 8b 83 ?? ?? ?? ?? 48 85 c0 0f 84 ?? ?? ?? ?? "
        "48 8b 83 ?? ?? ?? ?? 31 c9 48 85 c0 74 ??",
        {{DCP_LAYOUT_EQUIPMENT_OWNER, 3, 4}, {DCP_LAYOUT_EQUIPMENT_BAG, 19, 4}}},
    {"item flags", 2,
        "55 48 89 e5 41 56 53 48 89 fb 8b 7b ?? 81 e7 ff "
        "ff ff 01 e8 ?? ?? ?? ?? 41 b6 01 84 c0 75 ?? 8b "
        "7b ?? 81 e7 ff ff ff 01 e8",
        {{DCP_LAYOUT_ITEM_FLAGS, 12, 1}, {DCP_LAYOUT_ITEM_FLAGS, 33, 1}}},
    {"durability", 1,
        "48 8b 80 ?? ?? ?? ?? 8b 49 ?? 85 88 ?? ?? ?? ?? "
        "0f 84 ?? ?? ?? ?? 41 8b 46 ?? 89 85 ?? ?? ?? ?? "
        "45 8b 66 ?? 83 bd ?? ?? ?? ?? 00",
        {{DCP_LAYOUT_UNIT_OWNER, 3, 4}, {DCP_LAYOUT_ITEM_DURABILITY, 25, 1}, {DCP_LAYOUT_ITEM_MAX_DURABILITY, 35, 1}}},
    {"item uses", 1,
        "49 8d bf ?? ?? ?? ?? e8 ?? ?? ?? ?? 48 89 c3 48 "
        "85 db 74 ?? be 01 00 00 00 48 89 df e8 ?? ?? ?? "
        "?? 80 7b ?? 00",
        {{DCP_LAYOUT_UNIT_EQUIPMENT, 3, 4}, {DCP_LAYOUT_ITEM_USES, 35, 1}}},
    {"arrow count", 1,
        "8b 87 ?? ?? ?? ?? 03 b7 ?? ?? ?? ?? 39 f0 0f 4f "
        "c6 31 c9 85 f6 0f 49 c8 89 8f ?? ?? ?? ?? c3",
        {{DCP_LAYOUT_ARROW_COUNT, 8, 4}, {DCP_LAYOUT_ARROW_COUNT, 26, 4}}},
    {"owner category", 1,
        "4c 8b bd ?? ?? ?? ?? 8b 40 ?? 41 85 87 ?? ?? ?? "
        "?? 0f 84 ?? ?? ?? ??",
        {{DCP_LAYOUT_OWNER_CATEGORY, 13, 4}}},
    {"player slot", 2,
        "48 8b bd ?? ?? ?? ?? 48 8b 07 48 63 9f ?? ?? ?? "
        "?? ff 90 ?? ?? ?? ?? 48 83 fb 03 0f 87 ?? ?? ?? "
        "?? 48 8b 80 ?? ?? ?? ?? 48 69 cb ?? ?? ?? ?? 48 "
        "8d bc 08 ?? ?? ?? ??",
        {{DCP_LAYOUT_UNIT_PLAYER_SLOT, 13, 4}, {DCP_LAYOUT_GAME_SAVE_BASE, 36, 4}, {DCP_LAYOUT_SAVE_RECORD_SIZE, 43, 4}, {DCP_LAYOUT_SAVE_RECORD_OFFSET, 51, 4}}},
    {"save record", 1,
        "48 8b 80 ?? ?? ?? ?? 49 69 cc ?? ?? ?? ?? 49 83 "
        "fc 03 bf 00 00 00 00 48 8d 84 08 ?? ?? ?? ?? 48 "
        "0f 46 f8 e8 ?? ?? ?? ?? 48 89 c3 be 01 00 00 00 "
        "48 89 df e8 ?? ?? ?? ?? 66 44 89 ab ?? ?? ?? ?? "
        "66 45 89 6e ??",
        {{DCP_LAYOUT_GAME_SAVE_BASE, 3, 4}, {DCP_LAYOUT_SAVE_RECORD_SIZE, 10, 4}, {DCP_LAYOUT_SAVE_RECORD_OFFSET, 27, 4}, {DCP_LAYOUT_SKILL_POINTS_MIRROR, 60, 4}, {DCP_LAYOUT_SKILL_POINTS, 68, 1}}},
    {"game gold context", 1,
        "48 89 df e8 ?? ?? ?? ?? 48 8b b8 ?? ?? ?? ?? e8 "
        "?? ?? ?? ?? 84 c0 74 ?? 48 89 df e8 ?? ?? ?? ?? "
        "48 8b b8 ?? ?? ?? ?? e8 ?? ?? ?? ?? 84 c0 74 ?? "
        "83 bb ?? ?? ?? ?? 02",
        {{DCP_LAYOUT_GAME_GOLD_CONTEXT, 11, 4}, {DCP_LAYOUT_ARROW_COUNT, 50, 4}}},
    {"get gold", 1,
        "55 48 89 e5 48 8b 07 ff 90 ?? ?? ?? ?? 48 8b b8 "
        "?? ?? ?? ?? e8 ?? ?? ?? ?? 8b 80 ?? ?? ?? ?? 5d "
        "c3",
        {{DCP_LAYOUT_GOLD, 27, 4}}},
    {"add gold", 1,
        "55 48 89 e5 41 56 53 48 89 fb 41 89 f6 48 8b 03 "
        "ff 90 ?? ?? ?? ?? 48 8b b8 ?? ?? ?? ?? e8 ?? ?? "
        "?? ?? 8b 88 ?? ?? ?? ?? be ff e0 f5 05",
        {{DCP_LAYOUT_CONTEXT_VIRTUAL, 18, 4}, {DCP_LAYOUT_GOLD, 36, 4}}},
};

class Locator {
public:
    Locator(const DcpImageView& image, DcpLocateReport& report) : image_(image), report_(report) {
        for (size_t i = 0; i < image.executable_count; ++i) {
            ranges_.push_back({image.executable[i].begin, image.executable[i].end});
        }
        report_ = {};
    }

    bool Fail(const char* what, const char* name, const char* why) {
        if (report_.failure[0] == 0) {
            std::snprintf(report_.failure, sizeof(report_.failure), "%s %s: %s", what, name, why);
        }
        return false;
    }

    std::vector<uint64_t> Find(std::string_view pattern, size_t& total) const {
        return dcp::scan::FindPattern(image_.bytes, image_.size, ranges_, pattern, 16, total);
    }

    // Every match of the site's pattern, as the site's own offsets, sorted; exactly `expected` of them.
    bool Site(const SiteSpec& spec, std::vector<uint64_t>& sites) {
        ++report_.expected;
        size_t total = 0;
        sites = Find(spec.pattern, total);
        if (total != spec.expected) {
            char why[48];
            if (total == 0) {
                std::snprintf(why, sizeof(why), "missing");
            } else {
                std::snprintf(why, sizeof(why), "%zu matches, expected %u", total, unsigned(spec.expected));
            }
            return Fail("site", spec.name, why);
        }
        for (uint64_t& site : sites) {
            site += spec.anchor;
            if (site > UINT32_MAX) {
                return Fail("site", spec.name, "beyond the image");
            }
        }
        std::sort(sites.begin(), sites.end());
        ++report_.found;
        return true;
    }

    bool ReadProbe(const Probe& probe, uint32_t layout[DCP_LAYOUT_COUNT], std::array<bool, DCP_LAYOUT_COUNT>& known) {
        ++report_.expected;
        size_t total = 0;
        const std::vector<uint64_t> starts = Find(probe.pattern, total);
        if (total != probe.expected) {
            char why[48];
            std::snprintf(why, sizeof(why), total == 0 ? "missing" : "%zu matches, expected %u", total,
                          unsigned(probe.expected));
            return Fail("layout", probe.name, why);
        }
        for (const uint64_t start : starts) {
            for (const ProbeField& field : probe.fields) {
                uint32_t value = 0;
                std::memcpy(&value, image_.bytes + start + field.position, field.width);
                if (known[field.field] && layout[field.field] != value) {
                    return Fail("layout", dcp_layout_field_name(field.field), "conflicting displacements");
                }
                layout[field.field] = value;
                known[field.field] = true;
            }
        }
        ++report_.found;
        return true;
    }

    bool Layout(uint32_t layout[DCP_LAYOUT_COUNT]) {
        std::array<bool, DCP_LAYOUT_COUNT> known{};
        for (const Probe& probe : kProbes) {
            if (!ReadProbe(probe, layout, known)) {
                return false;
            }
        }
        for (size_t i = 0; i < known.size(); ++i) {
            if (!known[i]) {
                return Fail("layout", dcp_layout_field_name(DcpLayoutField(i)), "no instruction found");
            }
        }
        return true;
    }

    // Only the probes that read `fields`; each of them must be found and agree, and every field must come out.
    bool LayoutOf(std::initializer_list<DcpLayoutField> fields, uint32_t layout[DCP_LAYOUT_COUNT]) {
        std::array<bool, DCP_LAYOUT_COUNT> known{};
        for (const Probe& probe : kProbes) {
            const bool wanted = std::any_of(probe.fields.begin(), probe.fields.end(), [&](const ProbeField& pf) {
                return std::find(fields.begin(), fields.end(), pf.field) != fields.end();
            });
            if (wanted && !ReadProbe(probe, layout, known)) {
                return false;
            }
        }
        for (const DcpLayoutField field : fields) {
            if (!known[field]) {
                return Fail("layout", dcp_layout_field_name(field), "no instruction found");
            }
        }
        return true;
    }

    bool Branches(const std::vector<uint64_t>& from, uint64_t target, const char* name) {
        for (const uint64_t at : from) {
            int32_t displacement = 0;
            if (at + 5 > image_.size || (image_.bytes[at] != 0xe8 && image_.bytes[at] != 0xe9)) {
                return Fail("relation", name, "not a call or jump");
            }
            std::memcpy(&displacement, image_.bytes + at + 1, sizeof(displacement));
            if (int64_t(at) + 5 + displacement != int64_t(target)) {
                return Fail("relation", name, "calls another function");
            }
        }
        return true;
    }

    const uint8_t* bytes() const { return image_.bytes; }
    uint64_t size() const { return image_.size; }

private:
    const DcpImageView& image_;
    DcpLocateReport& report_;
    std::vector<Range> ranges_;
};

const SiteSpec* SpecNamed(std::string_view feature, std::string_view name) {
    for (const SiteSpec& spec : dcp::scan::Sites()) {
        if (feature == spec.feature && name == spec.name) {
            return &spec;
        }
    }
    return nullptr;
}

bool ValidView(const DcpImageView* image, DcpLocateReport* report) {
    return image && image->bytes && report && (image->executable_count == 0 || image->executable);
}

} // namespace

size_t dcp_layout_probe_count(void) {
    return std::size(kProbes);
}

const char* dcp_layout_probe_pattern(size_t index, size_t* occurrences) {
    if (index >= std::size(kProbes)) {
        return nullptr;
    }
    if (occurrences) {
        *occurrences = kProbes[index].expected;
    }
    return kProbes[index].pattern;
}

int dcp_layout_probe_field(size_t index, size_t i, DcpLayoutField* field, size_t* position, size_t* width) {
    if (index >= std::size(kProbes) || i >= kProbes[index].fields.size()) {
        return 0;
    }
    const ProbeField& item = kProbes[index].fields[i];
    *field = item.field;
    *position = item.position;
    *width = item.width;
    return 1;
}

int dcp_ranges_overlap(const DcpExecRange* ranges, size_t count) {
    std::vector<DcpExecRange> sorted(ranges, ranges + count);
    std::sort(sorted.begin(), sorted.end(), [](const DcpExecRange& a, const DcpExecRange& b) { return a.begin < b.begin; });
    for (size_t i = 1; i < sorted.size(); ++i) {
        if (sorted[i].begin < sorted[i - 1].end) {
            return 1;
        }
    }
    return 0;
}

int dcp_resolve_features(const DcpGameBuild* build, const DcpImageView* image, int cheats, int voices, int ultrawide,
                         DcpFeatureSites* out) {
    if (!out) {
        return 0;
    }
    *out = {};
    const char* denied = "not a supported build that uses signature locators";
    std::snprintf(out->cheat_failure, sizeof(out->cheat_failure), "%s", denied);
    std::snprintf(out->voice_failure, sizeof(out->voice_failure), "%s", denied);
    std::snprintf(out->ultrawide_failure, sizeof(out->ultrawide_failure), "%s", denied);
    if (!build || !build->features_by_signature || !image) {
        return 0;
    }
    DcpLocateReport report;
    if (cheats && build->cheats) {
        out->cheats = dcp_locate_cheat_sites(image, &out->cheat_sites, &report);
        std::snprintf(out->cheat_failure, sizeof(out->cheat_failure), "%s", report.failure);
    }
    if (voices && build->japanese_npc_voices) {
        out->voices = dcp_locate_voice_sites(image, &out->voice_sites, &report);
        std::snprintf(out->voice_failure, sizeof(out->voice_failure), "%s", report.failure);
    }
    if (ultrawide && build->true_ultrawide) {
        out->ultrawide = dcp_locate_ultrawide_sites(image, &out->ultrawide_sites, &report);
        std::snprintf(out->ultrawide_failure, sizeof(out->ultrawide_failure), "%s", report.failure);
    }
    return out->cheats || out->voices || out->ultrawide;
}


int dcp_locate_cheat_sites(const DcpImageView* image, DcpCheatSites* out, DcpLocateReport* report) {
    if (!ValidView(image, report) || !out) {
        return 0;
    }
    Locator locator(*image, *report);
    DcpCheatSites sites{};
    size_t hooks = 0, functions = 0;
    for (const SiteSpec& spec : dcp::scan::Sites()) {
        if (std::string_view(spec.feature) != "Cheats") {
            continue;
        }
        std::vector<uint64_t> found;
        if (!locator.Site(spec, found)) {
            return 0;
        }
        if (spec.kind == SiteKind::kHook && hooks < DCP_CHEAT_HOOKS) {
            sites.hooks[hooks++] = uint32_t(found.front());
        } else if (spec.kind == SiteKind::kFunction && functions < DCP_CHEAT_FUNCTIONS) {
            sites.functions[functions++] = uint32_t(found.front());
        } else {
            locator.Fail("site", spec.name, "not in the cheat set");
            return 0;
        }
    }
    if (hooks != DCP_CHEAT_HOOKS || functions != DCP_CHEAT_FUNCTIONS) {
        locator.Fail("cheat set", "sites", "incomplete");
        return 0;
    }
    std::vector<uint32_t> all(sites.hooks, sites.hooks + DCP_CHEAT_HOOKS);
    std::sort(all.begin(), all.end());
    if (std::adjacent_find(all.begin(), all.end()) != all.end()) {
        locator.Fail("cheat set", "hooks", "two hooks share a site");
        return 0;
    }
    if (!locator.Layout(sites.layout)) {
        return 0;
    }
    *out = sites;
    return 1;
}

int dcp_resolve_exit_game(const DcpGameBuild* build, const DcpImageView* image, DcpExitGameSites* out, char* failure,
                          size_t failure_size) {
    DcpLocateReport report{};
    std::snprintf(report.failure, sizeof(report.failure), "not a supported game build");
    const int resolved = build && build->exit_game && image && out && dcp_locate_exit_game_sites(image, out, &report);
    if (failure && failure_size) {
        std::snprintf(failure, failure_size, "%s", report.failure);
    }
    return resolved;
}

int dcp_locate_exit_game_sites(const DcpImageView* image, DcpExitGameSites* out, DcpLocateReport* report) {
    if (!ValidView(image, report) || !out) {
        return 0;
    }
    Locator locator(*image, *report);
    std::vector<uint64_t> fill, widget, limits, loads, tick;
    static const char* const kTransitionNames[DCP_TRANSITION_PART_COUNT] = {
        "part TITLE_1", "part START_1", "part START_2", "part UPLOAD_1", "part UPLOAD_2", "part DOWNLOAD_1",
        "part DOWNLOAD_2", "part OPTION_1", "part OPTION_2", "part GALLERY_1", "part GALLERY_2"};
    const auto find = [&](const char* name, std::vector<uint64_t>& found) {
        const SiteSpec* spec = SpecNamed("Exit Game", name);
        return spec ? locator.Site(*spec, found) : locator.Fail("site", name, "not defined");
    };
    if (!find("sheet fill", fill) || !find("row widget", widget) || !find("help limit", limits) || limits.size() != 2 ||
        !find("part table", loads) || loads.size() != 2 || !find("menu tick", tick)) {
        return 0;
    }
    DcpExitGameSites sites{uint32_t(fill.front()), uint32_t(widget.front()),
                           {uint32_t(limits[0]), uint32_t(limits[1])}, {uint32_t(loads[0]), uint32_t(loads[1])},
                           uint32_t(tick.front()), {}};
    DcpExecRange ranges[7 + DCP_TRANSITION_PART_COUNT] = {{sites.sheet_fill, sites.sheet_fill + 10u},
                                                           {sites.row_widget, sites.row_widget + 5u},
                                                           {sites.help_limits[0], sites.help_limits[0] + 1u},
                                                           {sites.help_limits[1], sites.help_limits[1] + 1u},
                                                           {sites.part_table_loads[0], sites.part_table_loads[0] + 7u},
                                                           {sites.part_table_loads[1], sites.part_table_loads[1] + 7u},
                                                           {sites.menu_tick, sites.menu_tick + 6u}};
    for (size_t i = 0; i < DCP_TRANSITION_PART_COUNT; ++i) {
        std::vector<uint64_t> part;
        if (!find(kTransitionNames[i], part)) {
            return 0;
        }
        sites.transition_parts[i] = uint32_t(part.front());
        ranges[7 + i] = {sites.transition_parts[i], sites.transition_parts[i] + 1u};
    }
    if (dcp_ranges_overlap(ranges, std::size(ranges))) {
        return locator.Fail("patch", "ranges", "two patches overlap");
    }
    *out = sites;
    return 1;
}

int dcp_resolve_party(const DcpGameBuild* build, const DcpImageView* image, DcpPartySites* out, char* failure,
                      size_t failure_size) {
    DcpLocateReport report{};
    std::snprintf(report.failure, sizeof(report.failure), "not a supported game build");
    const int resolved = build && image && out && dcp_locate_party_sites(image, out, &report);
    if (failure && failure_size) {
        std::snprintf(failure, failure_size, "%s", report.failure);
    }
    return resolved;
}

int dcp_locate_party_sites(const DcpImageView* image, DcpPartySites* out, DcpLocateReport* report) {
    if (!ValidView(image, report) || !out) {
        return 0;
    }
    Locator locator(*image, *report);
    const SiteSpec* tick = SpecNamed("Cheats", "hook: player tick");
    std::vector<uint64_t> found;
    uint32_t layout[DCP_LAYOUT_COUNT] = {};
    if (!tick || !locator.Site(*tick, found) ||
        !locator.LayoutOf({DCP_LAYOUT_UNIT_OWNER, DCP_LAYOUT_UNIT_PLAYER_SLOT, DCP_LAYOUT_UNIT_CLASS, DCP_LAYOUT_OWNER_CATEGORY},
                          layout)) {
        return 0;
    }
    *out = {uint32_t(found.front()), layout[DCP_LAYOUT_UNIT_OWNER], layout[DCP_LAYOUT_UNIT_PLAYER_SLOT],
            layout[DCP_LAYOUT_UNIT_CLASS], layout[DCP_LAYOUT_OWNER_CATEGORY]};
    return 1;
}

int dcp_locate_voice_sites(const DcpImageView* image, DcpVoiceSites* out, DcpLocateReport* report) {
    if (!ValidView(image, report) || !out) {
        return 0;
    }
    Locator locator(*image, *report);
    const SiteSpec* cue = SpecNamed("Japanese NPC voices", "NPC cue site");
    const SiteSpec* acquire = SpecNamed("Japanese NPC voices", "resource acquire");
    std::vector<uint64_t> cue_sites, acquire_sites;
    if (!cue || !acquire || !locator.Site(*cue, cue_sites) || !locator.Site(*acquire, acquire_sites)) {
        return 0;
    }
    out->npc_cue = uint32_t(cue_sites.front());
    out->resource_acquire = uint32_t(acquire_sites.front());
    return 1;
}

namespace {

} // namespace

int dcp_locate_ultrawide_sites(const DcpImageView* image, DcpUltrawideSites* out, DcpLocateReport* report) {
    if (!ValidView(image, report) || !out) {
        return 0;
    }
    Locator locator(*image, *report);
    static constexpr const char* kNames[] = {"submit",         "fill call 1",       "fill call 2",       "fill call 3",
                                             "frame commit",   "queue",             "part queue call",   "model draw",
                                             "widget draw call", "map draw call",   "facility draw call", "worldmap draw",
                                             "fade jumps",     "camera view call",  "view build",        "pass ortho load 1",
                                             "pass ortho load 2", "pass ortho load 3", "pass ortho load 4",
                                             "pass ortho load 5", "pass ortho load 6"};
    std::map<std::string_view, std::vector<uint64_t>> found;
    for (const char* name : kNames) {
        const SiteSpec* spec = SpecNamed("True ultrawide", name);
        if (!spec) {
            locator.Fail("site", name, "not defined");
            return 0;
        }
        if (!locator.Site(*spec, found[name])) {
            return 0;
        }
    }
    for (const dcp::scan::RelationSpec& relation : dcp::scan::Relations()) {
        if (std::string_view(relation.feature) != "True ultrawide") {
            continue;
        }
        if (relation.kind != dcp::scan::RelationKind::kCallsSite ||
            !locator.Branches(found[relation.from], found[relation.to].front(), relation.from)) {
            locator.Fail("relation", relation.from, "does not hold");
            return 0;
        }
    }

    DcpUltrawideSites sites{};
    sites.submit = uint32_t(found["submit"].front());
    for (size_t i = 0; i < 3; ++i) {
        sites.fill_calls[i] = uint32_t(found[std::string_view(kNames[1 + i])].front());
    }
    sites.frame_commit = uint32_t(found["frame commit"].front());
    sites.queue = uint32_t(found["queue"].front());
    sites.part_queue_call = uint32_t(found["part queue call"].front());
    sites.model_draw = uint32_t(found["model draw"].front());
    sites.widget_draw_call = uint32_t(found["widget draw call"].front());
    sites.map_draw_call = uint32_t(found["map draw call"].front());
    sites.facility_draw_call = uint32_t(found["facility draw call"].front());
    sites.worldmap_draw = uint32_t(found["worldmap draw"].front());
    sites.fade_jumps[0] = uint32_t(found["fade jumps"][0]);
    sites.fade_jumps[1] = uint32_t(found["fade jumps"][1]);
    sites.camera_view_call = uint32_t(found["camera view call"].front());
    sites.view_build = uint32_t(found["view build"].front());

    struct Patch {
        uint64_t begin, end;
        const char* name;
    };
    std::vector<Patch> patches;
    for (size_t i = 0; i < 3; ++i) patches.push_back({sites.fill_calls[i], sites.fill_calls[i] + 5u, "fill call"});
    patches.push_back({sites.camera_view_call, sites.camera_view_call + 5u, "camera view call"});
    patches.push_back({sites.frame_commit, sites.frame_commit + 11u, "frame commit"});
    patches.push_back({sites.part_queue_call, sites.part_queue_call + 5u, "part queue call"});
    patches.push_back({sites.fade_jumps[0], sites.fade_jumps[0] + 5u, "fade jump"});
    patches.push_back({sites.fade_jumps[1], sites.fade_jumps[1] + 5u, "fade jump"});
    for (size_t i = 0; i < DCP_ORTHO_LOADS; ++i) {
        const std::vector<uint64_t>& load = found[kNames[15 + i]];
        sites.ortho_loads[i] = uint32_t(load.front());
        const uint8_t* at = locator.bytes() + load.front();
        int32_t displacement = 0;
        float value = 0;
        if (load.front() + 8 > locator.size() || std::memcmp(at, "\xc5\xfa\x10\x05", 4) != 0) {
            locator.Fail("site", kNames[15 + i], "not a vmovss load");
            return 0;
        }
        std::memcpy(&displacement, at + 4, sizeof(displacement));
        const int64_t constant = int64_t(load.front()) + 8 + displacement;
        if (constant < 0 || uint64_t(constant) + 4 > locator.size()) {
            locator.Fail("site", kNames[15 + i], "constant outside the image");
            return 0;
        }
        std::memcpy(&value, locator.bytes() + constant, sizeof(value));
        if (value != dcp_pass_ortho_sites[i].value) {
            locator.Fail("site", kNames[15 + i], "constant differs");
            return 0;
        }
        patches.push_back({load.front(), load.front() + 8u, "pass ortho load"});
    }
    std::vector<DcpExecRange> ranges;
    for (const Patch& patch : patches) {
        if (patch.end > locator.size()) {
            locator.Fail("patch", patch.name, "outside the image");
            return 0;
        }
        ranges.push_back({patch.begin, patch.end});
    }
    if (dcp_ranges_overlap(ranges.data(), ranges.size())) {
        locator.Fail("patch", "ranges", "two patches overlap");
        return 0;
    }
    *out = sites;
    return 1;
}
