#include "scan_fixture.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "build_scan.h"
#include "cheat_hooks.h"
#include "cyrillic_glyphs.h"
#include "dcp_resolution.h"
#include "early_story_online.h"
#include "feature_sites.h"
#include "hydeland_town.h"
#include "lzcnt_compat.h"
#include "startup_save_hook.h"
#include "ultrawide_hooks.h"
#include "voice_hooks.h"

namespace parity {
namespace {

namespace scan = dcp::scan;

constexpr size_t kFontHeaderBytes = 16, kFontPages = 256, kGlyphEntry = 8, kGlyphPage = 256 * kGlyphEntry;
constexpr uint8_t kFontHeader[kFontHeaderBytes] = {0, 0, 0, 0, 0, 0x10, 0, 0x10, 1, 0x44, 0x44, 2, 2, 0x3c, 0x3c, 0};

std::vector<int> Parse(std::string_view text) {
    std::vector<int> bytes;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == ' ') {
            ++i;
        } else if (text[i] == '?') {
            bytes.push_back(-1);
            i += 2;
        } else {
            bytes.push_back(int(std::strtoul(std::string(text.substr(i, 2)).c_str(), nullptr, 16)));
            i += 2;
        }
    }
    return bytes;
}

void Plant(Fx* fx, std::string_view pattern, uint64_t at) {
    const std::vector<int> bytes = Parse(pattern);
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] < 0) continue;
        const auto byte = uint8_t(bytes[i]);
        fx_put(fx, at + i, &byte, 1);
    }
}

std::vector<uint64_t> Find(Fx* fx, std::string_view pattern, size_t* count = nullptr) {
    const scan::Range range{0, fx_size(fx)};
    size_t total = 0;
    std::vector<uint64_t> found =
        scan::FindPattern(fx_image(fx), fx_size(fx), std::span<const scan::Range>(&range, 1), pattern, 64, total);
    if (count) *count = total;
    return found;
}

const scan::SiteSpec& Spec(std::string_view feature, std::string_view name) {
    for (const scan::SiteSpec& spec : scan::Sites()) {
        if (feature == spec.feature && name == spec.name) return spec;
    }
    std::abort();
}

std::vector<uint64_t> SitesOf(Fx* fx, std::string_view feature, std::string_view name) {
    const scan::SiteSpec& spec = Spec(feature, name);
    std::vector<uint64_t> found = Find(fx, spec.pattern);
    for (uint64_t& at : found) at += spec.anchor;
    return found;
}

uint64_t SiteOf(Fx* fx, std::string_view feature, std::string_view name) {
    const std::vector<uint64_t> found = SitesOf(fx, feature, name);
    return found.empty() ? 0 : found.front();
}

// Every site at the place the reference build has it, then the further copies a pattern expects.
void PlantSites(Fx* fx, size_t reference) {
    std::vector<const scan::SiteSpec*> specs;
    for (const scan::SiteSpec& spec : scan::Sites()) specs.push_back(&spec);
    for (const scan::SiteSpec* spec : specs) {
        if (std::string_view(spec->feature) == "Startup save") continue;
        fx_context(fx, spec->name);
        if (spec->reference[reference]) Plant(fx, spec->pattern, spec->reference[reference] - spec->anchor);
    }
    for (const UltrawideRecord& record : ultrawide_records) {
        if (std::string_view(record.build) != scan::kReferenceBuilds[reference].label) continue;
        const scan::SiteSpec& fade = Spec("True ultrawide", "fade jumps");
        Plant(fx, fade.pattern, record.fade_jumps[1] - fade.anchor);
    }
    for (const scan::SiteSpec* spec : specs) {
        if (std::string_view(spec->feature) == "Startup save") continue;
        fx_context(fx, spec->name);
        size_t count = 0;
        Find(fx, spec->pattern, &count);
        for (; count < spec->expected; ++count) Plant(fx, spec->pattern, fx_alloc(fx, Parse(spec->pattern).size() + 0x20) + 0x10);
    }
}

// The displacements the cheat handlers expect, as this fixture's own oracle.
constexpr uint32_t kLayout[DCP_LAYOUT_COUNT] = {
    0xfffffe00, 0xd0,  0x1e0,  0xa70,  0xcb,  0xca,  0xb0, 0xa4, 0xa8, 0xac, 0x1e8, 0x2a0c, 0x9b8, 0x39e0, 0x3ad4, 0x508, 0x510,
    0x40,       0x44,  0x48,   0x4c,   0xaf238, 0x80, 0xa8, 0x23b0, 0x88, 0x24, 0xc8, 0x26, 0x4, 0x8, 0x24, 0xfe};

void PlantLayout(Fx* fx) {
    fx_context(fx, "cheat layout");
    for (size_t probe = 0; probe < dcp_layout_probe_count(); ++probe) {
        size_t expected = 0, count = 0;
        const std::string pattern = dcp_layout_probe_pattern(probe, &expected);
        Find(fx, pattern, &count);
        for (; count < expected; ++count) Plant(fx, pattern, fx_alloc(fx, Parse(pattern).size() + 0x20) + 0x10);
        for (const uint64_t start : Find(fx, pattern)) {
            DcpLayoutField field;
            size_t position, width;
            for (size_t i = 0; dcp_layout_probe_field(probe, i, &field, &position, &width); ++i)
                fx_put(fx, start + position, &kLayout[field], width);
        }
    }
}

void WireUltrawide(Fx* fx) {
    fx_context(fx, "True Ultrawide");
    for (const scan::RelationSpec& relation : scan::Relations()) {
        if (std::string_view(relation.feature) != "True ultrawide") continue;
        const uint64_t target = SiteOf(fx, relation.feature, relation.to);
        for (const uint64_t from : SitesOf(fx, relation.feature, relation.from)) fx_rel_to(fx, from + 1, from + 5, target);
    }
    const auto site = [&](const char* name) { return SiteOf(fx, "True ultrawide", name); };
    fixture_ultrawide(fx, site("queue"), site("model draw"), site("worldmap draw"));
    for (size_t i = 0; i < DCP_ORTHO_LOADS; ++i) {
        const uint64_t load = SiteOf(fx, "True ultrawide", "pass ortho load " + std::to_string(i + 1));
        fx_put(fx, fx_rel(fx, load + 4, load + 8, 16), &dcp_pass_ortho_sites[i].value, 4);
    }
}

void WireVoice(Fx* fx) {
    fx_context(fx, "Voice Language");
    static constexpr uint32_t kNarrators[DCP_NARRATOR_BANKS] = {120000, 20000,  130000, 140000, 150000, 160000, 170000,
                                                                180000, 30000,  40000,  50000,  60000,  70000,  80000};
    const uint64_t mapper = SiteOf(fx, "Voice language", "cue mapper");
    for (int i = 0; i < DCP_VOICE_CUE_CALLS; ++i) {
        const uint64_t at = fx_alloc(fx, 0x10);
        if (i == DCP_VOICE_CUE_TAIL_JUMP) fx_jump_to(fx, at, mapper);
        else fx_call_to(fx, at, mapper);
    }
    const uint64_t bank = SiteOf(fx, "Voice language", "narrator bank select"), base = SiteOf(fx, "Voice language", "narrator cue base");
    const uint64_t helper = SiteOf(fx, "Voice language", "narrator cue helper");
    const uint64_t banks = fx_rel(fx, bank - 4, bank, 16 * DCP_NARRATOR_BANKS), bases = fx_rel(fx, base - 27, base - 23, 0x40);
    fx_rel_to(fx, helper - 13, helper - 9, bases);
    for (int i = 0; i < DCP_NARRATOR_BANKS; ++i) {
        const std::string name = "SND/NARRATION_GM" + std::to_string(kNarrators[i]);
        fx_pointer_to(fx, banks + 16 * uint64_t(i), fx_text(fx, (name + ".ACB").c_str()));
        fx_pointer_to(fx, banks + 16 * uint64_t(i) + 8, fx_text(fx, (name + ".AWB").c_str()));
        if (i < 8) fx_put(fx, bases + 4 * uint64_t(i), &kNarrators[i], 4);
    }
}

template <size_t N>
void Bytes(Fx* fx, uint64_t at, const uint8_t (&bytes)[N]) {
    fx_put(fx, at, bytes, N);
}

// A facade: it selects the lead record and asks one question about it.
uint64_t PlantFacade(Fx* fx, uint64_t call, uint64_t selector) {
    static constexpr uint8_t kHead[] = {0x55, 0x48, 0x89, 0xe5}, kTest[] = {0x48, 0x85, 0xc0, 0x74, 0x0a, 0x48, 0x89, 0xc7},
                             kTail[] = {0xeb, 0x02, 0x31, 0xc0, 0x5d, 0xc3};
    const uint64_t facade = fx_call(fx, call, 0x20);
    Bytes(fx, facade, kHead);
    fx_call_to(fx, facade + 4, selector);
    Bytes(fx, facade + 9, kTest);
    fx_call(fx, facade + 17, 16);
    Bytes(fx, facade + 22, kTail);
    return facade;
}

void WireEarlyStory(Fx* fx) {
    const auto site = [&](const char* name) { return SiteOf(fx, "Early Story Online", name); };
    fx_context(fx, "Early Story Online");
    const uint64_t selector = site("lead selector"), wrapper = PlantFacade(fx, site("hint call"), selector);
    static constexpr uint8_t kCheck[] = {0x80, 0x7f, 0x6f, 0x2c, 0x0f, 0x97, 0xc0, 0xc3};
    Bytes(fx, fx_call(fx, wrapper + 17, 16), kCheck);
    for (const char* caller : {"notice call", "prompt call"}) fx_call_to(fx, site(caller), wrapper);
    fx_call_to(fx, site("town update") + 20, site("manager getter"));
    fx_call(fx, site("town portal call"), 16);
    static constexpr uint8_t kPress[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x89, 0xf0, 0x49, 0x89, 0xfe, 0x48, 0x8d, 0x1c,
                                         0x40, 0x48, 0xc1, 0xe3, 0x04, 0x41, 0x80, 0xbc, 0x1e, 0xb2, 0x00, 0x00, 0x00, 0x00};
    Bytes(fx, fx_call(fx, site("menu press call"), 0x30), kPress);
    PlantFacade(fx, site("menu step call"), selector);
    const uint64_t wish = site("room wish"), info = site("room info location");
    fx_call_to(fx, wish + 0x5b, wrapper);
    fx_call_to(fx, wish + 0x96, info - 0x400);
    fixture_room_leave(fx, wish);
    fx_call(fx, site("host record call"), 16);

    const uint64_t prompt = site("prompt function");
    fx_context(fx, "Go online prompt");
    uint64_t table = 0;
    for (const uint64_t ctor : SitesOf(fx, "Early Story Online", "prompt owner ctor")) {
        fx_call_to(fx, ctor, prompt - 0x40);
        if (!table) table = fx_rel(fx, ctor + 8, ctor + 12, 0x100);
    }
    static constexpr uint8_t kSetState[] = {0x83, 0xfe, 0x03, 0x77, 0x20, 0x48, 0x63, 0xc6},
                             kForward[] = {0x48, 0x8b, 0x07, 0x48, 0x8b, 0x80, 0xa0, 0x00, 0x00, 0x00, 0xff, 0xe0},
                             kSystem[] = {0x48, 0x8b, 0xbf, 0xe0, 0x01, 0x00, 0x00, 0xe9};
    Bytes(fx, fx_pointer(fx, table + 0x10 + 0xa0, 16), kSetState);
    Bytes(fx, fx_pointer(fx, table + 0x10 + 0xb0, 16), kForward);
    Bytes(fx, fx_pointer(fx, table + 0x10 + 0xd0, 16), kSystem);
    static constexpr uint8_t kLea[] = {0x48, 0x8d, 0x05}, kMode[] = {0x41, 0x80, 0x7c, 0x24, 0x5b, 0x03},
                             kMask[] = {0x8b, 0x40, 0x04, 0x41, 0x85, 0x84, 0x24, 0x14, 0x05, 0x00, 0x00},
                             kScene[] = {0x41, 0x80, 0x7e, 0x5b, 0x02}, kQueue[] = {0x4c, 0x8b, 0xbb, 0x08, 0x01, 0x00, 0x00};
    Bytes(fx, prompt + HYDELAND_PROMPT_MASK_LEA, kLea);
    fx_rel(fx, prompt + HYDELAND_PROMPT_MASK_LEA + 3, prompt + HYDELAND_PROMPT_MASK_LEA + 7, 16);
    Bytes(fx, prompt + 0x70, kMask);
    Bytes(fx, prompt + 0x80, kMode);
    Bytes(fx, prompt + 0x88, kScene);
    Bytes(fx, prompt + 0x90, kQueue);

    fx_context(fx, "story flag facade");
    const uint64_t body = site("flag body");
    bool first = true;
    for (const uint64_t facade : SitesOf(fx, "Early Story Online", "flag facade")) {
        fx_call_to(fx, facade + 8, selector);
        if (first) fx_jump_to(fx, facade + 30, body);
        else fx_jump(fx, facade + 30, 16);
        first = false;
    }

    fx_context(fx, "Network Menu request");
    static constexpr uint8_t kCreated[] = {0x41, 0xf6, 0xc7, 0x01, 0x0f, 0x84}, kPad[] = {0x44, 0x89, 0xe6},
                             kAfter[] = {0x48, 0x89, 0xc3, 0x44, 0x89, 0xab, 0xf8, 0x00, 0x00, 0x00, 0x48,
                                         0x89, 0xdf, 0x48, 0x8b, 0x03, 0xff, 0x90, 0x00, 0x01, 0x00, 0x00},
                             kCreate[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x41,
                                          0x89, 0xf6, 0xbe, 0x0e, 0x00, 0x00, 0x00, 0xe8};
    const uint64_t kind = site("menu kind store"), test = kind - EARLY_STORY_MENU_CREATED_BACK, create = kind - 0x85;
    Bytes(fx, test, kCreated);
    fx_rel_to(fx, test + sizeof(kCreated), test + sizeof(kCreated) + 4, kind);
    Bytes(fx, create - sizeof(kPad), kPad);
    Bytes(fx, fx_call(fx, create, 0x20), kCreate);
    Bytes(fx, create + 5, kAfter);
}

void PlantResolution(Fx* fx, const dcp::GameBuild& layout) {
    fx_context(fx, "render resolution");
    for (size_t i = 0; i < layout.resolution_sites.size(); ++i) {
        const DcpResolutionSignature& signature = dcp_render_size_signatures[i];
        const uint64_t at = layout.resolution_sites[i].offset - signature.lead;
        for (unsigned k = 0; k < signature.length; ++k) {
            if (signature.compare[k]) fx_put(fx, at + k, &signature.pattern[k], 1);
        }
        fx_put(fx, at + signature.lead, signature.opcode, signature.opcode_length);
        fx_put(fx, at + signature.lead + signature.opcode_length, &signature.value, sizeof(signature.value));
    }
}

void PlantLzcnt(Fx* fx, std::string_view label) {
    fx_context(fx, "LZCNT");
    for (const LzcntBuild& build : lzcnt_builds) {
        if (label != build.build) continue;
        for (size_t i = 0; i < LZCNT_SITE_COUNT; ++i) fx_put(fx, build.offsets[i], lzcnt_shapes[i].bytes, lzcnt_shapes[i].length);
    }
}

void PlantFonts(Fx* fx, const dcp::TranslationProfile& profile) {
    fx_context(fx, "font");
    const uint64_t header = fx_alloc(fx, kFontHeaderBytes + kFontPages * 8), pages = header + kFontHeaderBytes;
    fx_put(fx, header, kFontHeader, kFontHeaderBytes);
    const auto glyph = [&](uint16_t code, uint16_t cell) {
        const uint64_t page = fx_pointer(fx, pages + uint64_t(code >> 8) * 8, kGlyphPage);
        fx_put(fx, page + uint64_t(code & 0xff) * kGlyphEntry, &cell, sizeof(cell));
    };
    for (uint16_t code = 0x20; code < 0x7f; ++code) glyph(code, uint16_t(code - 0x1f));
    if (profile.cyrillic.cells == CYRILLIC_CELLS_KANA) {
        for (int i = 0; i < CYRILLIC_GLYPH_COUNT; ++i) glyph(cyrillic_kana(i), uint16_t(cyrillic_cell(CYRILLIC_CELLS_KANA, i)));
    } else {
        glyph(profile.cyrillic.probe, kFontProbeCell);
    }
    if (profile.hangul.probe) glyph(profile.hangul.probe, kFontProbeCell);
}

} // namespace

void PlantStartupSave(Fx* fx, std::string_view label) {
    fx_context(fx, "startup save");
    for (const StartupSaveRecord& record : startup_save_records) {
        if (label != record.build) continue;
        fx_put(fx, record.step_offset, record.step_original, STARTUP_SAVE_SITE_LENGTH);
        fx_put(fx, record.save_offset, record.save_prologue, STARTUP_SAVE_PROLOGUE_LENGTH);
        fx_put(fx, record.accept_offset, record.accept_prologue, STARTUP_SAVE_PROLOGUE_LENGTH);
    }
}

void PlantScanned(Fx* fx, const dcp::GameBuild& layout, const dcp::TranslationProfile& profile, std::string_view label,
                  size_t reference) {
    PlantSites(fx, reference);
    if (reference == 0) {
        fx_context(fx, "cheat table");
        for (const GuestHook& hook : dcp::cheats::Hooks()) fx_put(fx, hook.offset, hook.original, hook.length);
    }
    PlantLayout(fx);
    WireUltrawide(fx);
    WireVoice(fx);
    WireEarlyStory(fx);
    const std::vector<uint64_t> loads = SitesOf(fx, "Exit Game", "part table");
    fixture_exit_game(fx, SiteOf(fx, "Exit Game", "row widget"), SiteOf(fx, "Exit Game", "menu tick"), loads.data());
    PlantResolution(fx, layout);
    PlantLzcnt(fx, label);
    PlantFonts(fx, profile);
}

uint64_t TextChangedSite(Fx* fx) { return SiteOf(fx, "Early Story Online", "hint refresh"); }

size_t ReferenceOf(std::string_view label) {
    for (size_t i = 0; i < scan::kReferenceBuildCount; ++i) {
        if (label == scan::kReferenceBuilds[i].label) return i;
    }
    return SIZE_MAX;
}

} // namespace parity
