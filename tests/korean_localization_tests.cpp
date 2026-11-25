// The Korean Menu & Subtitles language (localization/ko): the USA and European profiles, the translation corpus, the
// Hangul glyph file and the atlas/glyph-table rules that let the port serve it. The local USA game folder is read only,
// when present; no game, window or audio.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zlib.h>

#include "cpk_patch.h"
#include "crypto.h"
#include "fms_sheet.h"
#include "game_profile.h"
#include "game_translation.h"
#include "hangul_font.h"
#include "hangul_glyphs.h"
#include "latin_font.h"
#include "latin_glyphs.h"
#include "net/json.h"

namespace fs = std::filesystem;
using namespace dcp;

namespace {

int g_checks = 0, g_failures = 0;

#define CHECK(cond)                                             \
    do {                                                        \
        ++g_checks;                                             \
        if (!(cond)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond); \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

constexpr int kAtlasCells = LATIN_ATLAS_COLUMNS * LATIN_ATLAS_COLUMNS;
constexpr size_t kCellBlocks = size_t(kFontCellBlocks) * kFontCellBlocks * 16;
constexpr size_t kGlyphEntryBytes = 8 + kCellBlocks;

const GameBuild* BuildNamed(std::string_view label) {
    for (const GameBuild& build : SupportedBuilds()) {
        if (std::string_view(build.label) == label) return &build;
    }
    return nullptr;
}

bool IsUtf8(std::string_view text) {
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t length = 0;
        if (c < 0x80) length = 1;
        else if ((c & 0xE0) == 0xC0) length = 2;
        else if ((c & 0xF0) == 0xE0) length = 3;
        else if ((c & 0xF8) == 0xF0) length = 4;
        else return false;
        if (i + length > text.size()) return false;
        for (size_t k = 1; k < length; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return false;
        }
        i += length;
    }
    return true;
}

bool IsSha256Hex(std::string_view text) {
    return text.size() == 64 &&
           std::all_of(text.begin(), text.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

struct Corpus {
    std::map<std::string, size_t> counts;
    std::map<std::string, std::map<size_t, std::string>> texts;
};

Corpus LoadCorpus() {
    Corpus corpus;
    for (const TranslationFile& file : KoreanTranslationFiles()) {
        const std::string file_name(file.name);
        const std::string name = file_name.substr(0, file_name.rfind('.'));
        const std::optional<net::Json> sheet =
            net::Json::parse(std::string_view(reinterpret_cast<const char*>(file.data), file.size), 8);
        CHECK(sheet.has_value());
        if (!sheet) continue;
        CHECK((*sheet)["sheet"].as_string() == name);
        size_t count = 0;
        for (const net::Json& entry : (*sheet)["entries"].items()) {
            ++count;            const int64_t index = entry["index"].as_int(-1);
            const std::string& text = entry["ko"].as_string();
            CHECK(index >= 0);
            CHECK(!text.empty() && IsUtf8(text));
            CHECK(IsSha256Hex(entry["source_sha256"].as_string()));
            CHECK(corpus.texts[name].emplace(size_t(index), text).second);
        }
        corpus.counts[name] = count;
    }
    return corpus;
}

const std::string& TextAt(const Corpus& corpus, const std::string& sheet, size_t index) {
    static const std::string missing;
    const auto found = corpus.texts.find(sheet);
    if (found == corpus.texts.end()) return missing;
    const auto entry = found->second.find(index);
    return entry == found->second.end() ? missing : entry->second;
}

const std::map<size_t, std::string>& Entries(const Corpus& corpus, const std::string& sheet) {
    static const std::map<size_t, std::string> none;
    const auto found = corpus.texts.find(sheet);
    return found == corpus.texts.end() ? none : found->second;
}

void TestProfiles() {
    const GameBuild* usa = BuildNamed("CUSA10454 1.00 USA");
    const GameBuild* usa101 = BuildNamed("CUSA10454 1.01 USA");
    const GameBuild* europe = BuildNamed("CUSA10487 1.03 Europe");
    const GameBuild* asia = BuildNamed("CUSA10209 1.00 Asia/Hong Kong");
    const GameBuild* asia102 = BuildNamed("CUSA10209 1.02 Asia/Hong Kong");
    CHECK(usa && usa101 && europe && asia && asia102);
    if (!usa || !usa101 || !europe || !asia || !asia102) return;

    CHECK(kKoreanKey == "korean" && kPortKorean == "ko");
    const std::string_view western[] = {"english", "french",  "german", "italian",
                                        "korean",  "brazilian_portuguese", "russian", "spanish"};
    for (const GameBuild* build : {usa, usa101, europe}) {
        const std::span<const TextLanguage> languages = TextLanguagesOf(build);
        CHECK(languages.size() == 8);
        for (size_t i = 0; i < languages.size() && i < 8; ++i) CHECK(languages[i].key == western[i]);
        const TextLanguage& korean = ResolveTextLanguage(build, kKoreanKey);
        CHECK(korean.key == kKoreanKey && korean.name == "Korean");
        CHECK(korean.ps4_id == 1 && korean.suffix == "US" && korean.translation == kPortKorean);
        CHECK(build->translation->hangul.entry == "other/fontUTF16.ftx" && build->translation->hangul.probe == 0x4E9C);
    }
    CHECK(usa->translation->archive == std::string_view("DRACRO_US.CPK"));
    CHECK(usa101->translation == usa->translation);
    CHECK(europe->translation->archive == std::string_view("DRACRO_EU.CPK"));
    CHECK(europe->translation != usa->translation);

    // Asia/Hong Kong keeps its native Korean: slot 9, its own suffix, no port translation and no port Hangul.
    for (const GameBuild* build : {asia, asia102}) {
        const TextLanguage& korean = ResolveTextLanguage(build, kKoreanKey);
        CHECK(korean.key == kKoreanKey && korean.ps4_id == 9 && korean.suffix == "KR" && korean.translation.empty());
        CHECK(build->translation->hangul.entry.empty() && build->translation->hangul.probe == 0);
        CHECK(std::count_if(TextLanguagesOf(build).begin(), TextLanguagesOf(build).end(),
                            [](const TextLanguage& language) { return language.key == kKoreanKey; }) == 1);
    }

    // Japan never advertises the port's Korean; a saved Korean falls back to the build's default.
    for (const GameBuild& build : SupportedBuilds()) {
        if (std::string_view(build.title_id) != "CUSA07001") continue;
        CHECK(std::none_of(TextLanguagesOf(&build).begin(), TextLanguagesOf(&build).end(),
                           [](const TextLanguage& language) { return language.key == kKoreanKey; }));
        CHECK(ResolveTextLanguage(&build, kKoreanKey).key == "japanese");
        CHECK(build.translation->hangul.entry.empty());
    }

    for (const GameBuild* build : {usa, usa101, europe}) {
        CHECK(ResolveTextLanguage(build, kKoreanKey).translation == kPortKorean);
        CHECK(ResolveTextLanguage(build, kKoreanKey).ps4_id == 1);
    }
    CHECK(ResolveTextLanguage(asia, kKoreanKey).translation.empty());
    CHECK(ResolveTextLanguage(asia, kKoreanKey).ps4_id == 9);
    CHECK(ResolveTextLanguage(usa, "traditional_chinese").key == "english");
    CHECK(ResolveTextLanguage(asia, "english").key == "english");
    CHECK(ResolveTextLanguage(usa, {}).key == "english");
}

void TestCorpus() {
    const std::array<const char*, 13> sheets = {"AbilityList",   "BonusScore", "ChallengeQuest", "CustomItem",
                                                "DefaultBone",   "EventMsg",   "ItemList",       "ObjectParam",
                                                "RoundContinue", "SystemMsg",  "Temple",         "TreasureArt",
                                                "Tutorial"};
    const std::span<const TranslationFile> files = KoreanTranslationFiles();
    CHECK(files.size() == sheets.size());
    for (size_t i = 0; i < files.size() && i < sheets.size(); ++i) {
        CHECK(std::string_view(files[i].name) == std::string(sheets[i]) + ".json");
        CHECK(files[i].data && files[i].size);
    }
    CHECK(KoreanGlyphs() && KoreanGlyphs()->size);

    const Corpus corpus = LoadCorpus();
    const std::map<std::string, size_t> expected = {
        {"AbilityList", 807},  {"BonusScore", 13},   {"ChallengeQuest", 161}, {"CustomItem", 5238},
        {"DefaultBone", 2049}, {"EventMsg", 1045},   {"ItemList", 740},       {"ObjectParam", 98},
        {"RoundContinue", 4},  {"SystemMsg", 1494},  {"Temple", 40},          {"TreasureArt", 171},
        {"Tutorial", 93}};
    CHECK(corpus.counts == expected);
    size_t total = 0;
    for (const auto& entry : corpus.counts) total += entry.second;
    CHECK(total == 11953);

    // The Korean release never had these challenge quests: they stay in English and are the only missing records.
    CHECK(Entries(corpus, "ChallengeQuest").size() == 161);
    CHECK(Entries(corpus, "ChallengeQuest").begin()->first == 0 && Entries(corpus, "ChallengeQuest").rbegin()->first == 160);
    CHECK(!Entries(corpus, "ChallengeQuest").count(161) && !Entries(corpus, "ChallengeQuest").count(199));
    // SystemMsg's tag keys 109-225 are keys, never translated.
    CHECK(!Entries(corpus, "SystemMsg").count(109) && !Entries(corpus, "SystemMsg").count(225));
    CHECK(Entries(corpus, "SystemMsg").count(108) == 1 && Entries(corpus, "SystemMsg").count(226) == 1);

    // The eleven token adaptations korean.py makes so each record shares the USA record's conversions and buttons.
    CHECK(TextAt(corpus, "AbilityList", 46).find("@#(26)") == std::string::npos);
    for (const size_t index : {size_t(3996), size_t(3997), size_t(3998)}) {
        CHECK(TextAt(corpus, "CustomItem", index).find("위력 %d의 ") == std::string::npos);
    }
    for (const size_t index : {size_t(504), size_t(510), size_t(534)}) {
        CHECK(TextAt(corpus, "SystemMsg", index).find("@#(34)") != std::string::npos &&
              TextAt(corpus, "SystemMsg", index).find("@#(53)") == std::string::npos);
    }
    CHECK(TextAt(corpus, "SystemMsg", 750).find("@#(36)") == std::string::npos);
    CHECK(TextAt(corpus, "SystemMsg", 1257).find("@#(33)") == std::string::npos);
    CHECK(TextAt(corpus, "SystemMsg", 1444).find("@#(11)") == std::string::npos);
    CHECK(TextAt(corpus, "SystemMsg", 1473).find("PlayStation@#(52)Network") != std::string::npos);

    // Every record parses into control tokens; the text stays valid UTF-8.
    for (const auto& sheet : corpus.texts) {
        for (const auto& entry : sheet.second) {
            ControlTokens(entry.second);
            CHECK(IsUtf8(entry.second));
        }
    }
}

void TestGlyphFile(const std::vector<HangulCell>& cells) {
    CHECK(cells.size() == 1075);
    if (cells.size() != 1075) return;
    std::set<uint16_t> codes, taken;
    for (size_t i = 0; i < cells.size(); ++i) {
        const HangulGlyph& glyph = cells[i].glyph;
        CHECK(glyph.code >= 0xAC00 && glyph.code <= 0xD7A3);
        CHECK(!i || glyph.code > cells[i - 1].glyph.code);
        CHECK(glyph.width > 0 && glyph.width <= kLatinFontCellPixels);
        CHECK(glyph.spacing <= kLatinFontCellPixels);
        CHECK(glyph.cell < kAtlasCells);
        CHECK(codes.insert(glyph.code).second && taken.insert(glyph.cell).second);
        CHECK(cells[i].blocks.size() == kCellBlocks);
    }
}

std::vector<uint8_t> Compress(const std::vector<uint8_t>& raw) {
    uLongf size = compressBound(uLong(raw.size()));
    std::vector<uint8_t> out(4 + size);
    for (int i = 0; i < 4; ++i) out[size_t(i)] = uint8_t(uint32_t(raw.size()) >> (8 * i));
    CHECK(compress2(out.data() + 4, &size, raw.data(), uLong(raw.size()), 9) == Z_OK);
    out.resize(4 + size);
    return out;
}

std::vector<uint8_t> GlyphBody(size_t count) {
    std::vector<uint8_t> raw(16 + count * kGlyphEntryBytes);
    std::memcpy(raw.data(), "DCPKOR1\0", 8);
    const uint16_t header[4] = {uint16_t(count), uint16_t(kLatinFontCellPixels), uint16_t(kFontCellBlocks), 0};
    std::memcpy(raw.data() + 8, header, sizeof(header));
    for (size_t i = 0; i < count; ++i) {
        uint8_t* at = raw.data() + 16 + i * kGlyphEntryBytes;
        const uint16_t code = uint16_t(0xAC00 + i), cell = uint16_t(100 + i);
        std::memcpy(at, &code, 2);
        std::memcpy(at + 2, &cell, 2);
        at[4] = 60;
        at[5] = 2;
    }
    return raw;
}

void TestGlyphFileParsing() {
    CHECK(ParseHangulGlyphs({KoreanGlyphs()->data, KoreanGlyphs()->size}).has_value());
    CHECK(!ParseHangulGlyphs(std::span<const uint8_t>{}).has_value());

    const std::vector<uint8_t> raw = GlyphBody(2);
    const std::vector<uint8_t> valid = Compress(raw);
    CHECK(ParseHangulGlyphs(valid).has_value());
    const auto refused = [](const std::vector<uint8_t>& bytes) {
        return !ParseHangulGlyphs(Compress(bytes)).has_value();
    };
    CHECK(refused(GlyphBody(0)));
    CHECK(refused(GlyphBody(HANGUL_GLYPH_LIMIT + 1)));

    std::vector<uint8_t> bad = raw;
    bad[0] = 'X';
    CHECK(refused(bad));
    bad = raw;
    bad[10] = 64; // cell pixels
    CHECK(refused(bad));
    bad = raw;
    bad[16] = 0x00; // U+AC00 -> U+1100
    bad[17] = 0x11;
    CHECK(refused(bad));
    bad = raw;
    bad[20] = 0; // width 0
    CHECK(refused(bad));
    bad = raw;
    const uint16_t far_cell = uint16_t(kAtlasCells);
    std::memcpy(bad.data() + 18, &far_cell, 2); // past the 60 x 60 atlas
    CHECK(refused(bad));
    bad = raw;
    std::memcpy(bad.data() + 16 + kGlyphEntryBytes + 2, bad.data() + 18, 2); // the second syllable reuses the first cell
    CHECK(refused(bad));
    bad = raw;
    std::memcpy(bad.data() + 16, bad.data() + 16 + kGlyphEntryBytes, 2); // out of order
    CHECK(refused(bad));

    std::vector<uint8_t> truncated = valid;
    truncated.resize(truncated.size() / 2);
    CHECK(!ParseHangulGlyphs(truncated).has_value());
}

// A synthetic executable image: 256 page pointers, then the pages a font would list.
struct Table {
    std::vector<uint8_t> image = std::vector<uint8_t>(0x20000);
    const uint8_t* pages = image.data() + 0x1000;
    size_t page_count = 0;

    Table() {
        for (const unsigned high : {0x00u, 0x30u, 0x34u, 0x4Eu, 0xF9u}) AddPage(high);
        SetEntry(0x00, 0x41, 10); // Latin
        SetEntry(0x00, 0x42, 11);
        SetEntry(0x30, 0x60, 20); // kana
        SetEntry(0x34, 0x00, 32); // kanji
        SetEntry(0x4E, 0x00, 30); // kanji
        SetEntry(0x4E, 0x9C, 31);
        SetEntry(0xF9, 0x00, 33);
    }
    void AddPage(unsigned high) {
        const uint64_t offset = 0x2000 + 0x800 * page_count++;
        std::memcpy(image.data() + 0x1000 + size_t(high) * 8, &offset, 8);
    }
    void SetEntry(unsigned high, unsigned low, uint16_t cell) {
        uint8_t* entry = image.data() + PageOffset(high) + size_t(low) * 8;
        std::memcpy(entry, &cell, 2);
        entry[2] = 60;
        entry[3] = 2;
    }
    uint64_t PageOffset(unsigned high) const {
        uint64_t offset = 0;
        std::memcpy(&offset, image.data() + 0x1000 + size_t(high) * 8, 8);
        return offset;
    }
    uint64_t Pointer(unsigned high) const { return PageOffset(high); }
};

std::vector<HangulGlyph> KanjiGlyphs() {
    return {{0xAC00, 30, 60, 2}, {0xAC01, 31, 60, 2}, {0xAD00, 32, 60, 2}, {0xD7A3, 33, 60, 2}};
}

// The page pointer install wrote: the address the game itself dereferences.
const uint8_t* StoredPage(const Table& table, unsigned high) {
    uint64_t pointer = 0;
    std::memcpy(&pointer, table.image.data() + 0x1000 + size_t(high) * 8, 8);
    return reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(pointer));
}

void TestGlyphTable() {
    const std::vector<HangulGlyph> glyphs = KanjiGlyphs();
    const auto fits = [&](Table& table, const std::vector<HangulGlyph>& list) {
        return hangul_glyphs_fit(table.image.data(), table.image.size(), table.pages, list.data(), list.size());
    };
    {
        Table table;
        std::vector<uint64_t> pointers;
        for (unsigned high = 0; high < 256; ++high) pointers.push_back(table.Pointer(high));
        CHECK(fits(table, glyphs) == 1);
        CHECK(hangul_glyphs_install(table.image.data(), table.image.size(), table.pages, glyphs.data(), glyphs.size()) == 1);
        const uint8_t* page = StoredPage(table, 0xAC);
        CHECK(page != nullptr);
        if (page) CHECK((page[0] | page[1] << 8) == 30 && (page[8] | page[9] << 8) == 31 && page[2] == 60 && page[3] == 2);
        const uint8_t* second = StoredPage(table, 0xAD);
        CHECK(second && (second[0] | second[1] << 8) == 32);
        const uint8_t* last = StoredPage(table, 0xD7);
        CHECK(last && (last[0xA3 * 8] | last[0xA3 * 8 + 1] << 8) == 33);
        CHECK(StoredPage(table, 0xAE) == nullptr);
        for (unsigned high = 0; high < 256; ++high) {
            if (high == 0xAC || high == 0xAD || high == 0xD7) continue;
            CHECK(table.Pointer(high) == pointers[high]);
        }
        // A page stored as an image offset still resolves, as the loader reads the unrelocated image.
        CHECK(font_glyph_page(table.image.data(), table.image.size(), table.pages, 0x4E) != nullptr);
    }
    // The table already lists Hangul: nothing is ever installed twice.
    {
        Table existing;
        existing.AddPage(0xAC);
        CHECK(fits(existing, glyphs) == 0);
        const std::vector<uint8_t> before = existing.image;
        CHECK(hangul_glyphs_install(existing.image.data(), existing.image.size(), existing.pages, glyphs.data(),
                                    glyphs.size()) == 0);
        CHECK(existing.image == before);
    }
    // A Latin or kana cell is never overwritten.
    {
        Table latin;
        const std::vector<HangulGlyph> list = {{0xAC00, 10, 60, 2}};
        CHECK(fits(latin, list) == 0);
        Table kana;
        const std::vector<HangulGlyph> kana_list = {{0xAC00, 20, 60, 2}};
        CHECK(fits(kana, kana_list) == 0);
    }
    // Duplicate cells, invalid codes, a zero cell or width, an empty or oversized list, and out-of-range pointers.
    {
        Table table;
        const std::vector<HangulGlyph> duplicate = {{0xAC00, 30, 60, 2}, {0xAC01, 30, 60, 2}};
        CHECK(fits(table, duplicate) == 0);
        const std::vector<HangulGlyph> not_hangul = {{0x1100, 30, 60, 2}};
        CHECK(fits(table, not_hangul) == 0);
        const std::vector<HangulGlyph> no_cell = {{0xAC00, 0, 60, 2}};
        CHECK(fits(table, no_cell) == 0);
        const std::vector<HangulGlyph> no_width = {{0xAC00, 30, 0, 2}};
        CHECK(fits(table, no_width) == 0);
        CHECK(hangul_glyphs_fit(table.image.data(), table.image.size(), table.pages, nullptr, 0) == 0);
        CHECK(hangul_glyphs_fit(table.image.data(), table.image.size(), table.pages, glyphs.data(), 0) == 0);
        std::vector<HangulGlyph> oversized(HANGUL_GLYPH_LIMIT + 1, HangulGlyph{0xAC00, 30, 60, 2});
        CHECK(hangul_glyphs_fit(table.image.data(), table.image.size(), table.pages, oversized.data(), oversized.size()) == 0);
        CHECK(hangul_glyphs_fit(nullptr, 0, nullptr, glyphs.data(), glyphs.size()) == 0);
        CHECK(hangul_glyphs_fit(table.image.data(), table.image.size(), table.image.data() - 8, glyphs.data(), glyphs.size()) == 0);
        CHECK(hangul_glyphs_fit(table.image.data(), table.image.size(), table.image.data() + table.image.size(), glyphs.data(),
                                glyphs.size()) == 0);
    }
    // A failure leaves the image untouched.
    {
        Table table;
        const std::vector<uint8_t> before = table.image;
        const std::vector<HangulGlyph> latin = {{0xAC00, 10, 60, 2}};
        CHECK(hangul_glyphs_install(table.image.data(), table.image.size(), table.pages, latin.data(), latin.size()) == 0);
        CHECK(table.image == before);
    }
}

void TestComposeFailure(const std::vector<HangulCell>& cells) {
    std::vector<uint8_t> not_a_font(4096, 0xAB);
    const std::vector<uint8_t> before = not_a_font;
    std::string why;
    CHECK(!ComposeHangulFont(not_a_font, cells, &why));
    CHECK(why == "not a font atlas the port can read");
    CHECK(not_a_font == before);
    CHECK(!ComposeHangulFont(not_a_font, {}, &why) && why == "no Hangul syllables to draw");
}

std::vector<uint8_t> ReadArchiveEntry(CpkArchive& archive, const char* path) {
    const CpkEntry* entry = archive.Find(path);
    return entry ? archive.Read(*entry) : std::vector<uint8_t>();
}

std::string Sha256Of(std::string_view text) {
    uint8_t digest[32];
    if (!crypto::Sha256(reinterpret_cast<const uint8_t*>(text.data()), text.size(), digest)) return {};
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    for (const uint8_t byte : digest) {
        out += kHex[byte >> 4];
        out += kHex[byte & 15];
    }
    return out;
}

// The local USA folder: the real font atlas and the real message sheets, read only.
void TestLocalUsa(const std::vector<HangulCell>& cells) {
    const fs::path game = fs::path(DCP_SOURCE_DIR) / "game" / "prepared" / "v1.03";
    if (!fs::exists(game / "DRACRO_US.CPK") || !fs::exists(game / "DRACRO.CPK")) {
        std::printf("  local USA game folder absent: the atlas and translation checks run on synthetic data only\n");
        return;
    }
    CpkArchive archive;
    std::string error;
    CHECK(archive.Open(game / "DRACRO_US.CPK", &error));
    const std::vector<uint8_t> original = ReadArchiveEntry(archive, "other/fontUTF16.ftx");
    CHECK(!original.empty());

    // Every record against the real USA slot: its index is in bounds, that record is not empty, and it is the one the
    // Korean text was written from.
    size_t records = 0, empty_sources = 0;
    for (const TranslationFile& file : KoreanTranslationFiles()) {
        const std::string file_name(file.name);
        const std::string sheet_name = file_name.substr(0, file_name.rfind('.'));
        const std::optional<FmsSheet> native =
            ParseFms(ReadArchiveEntry(archive, ("msgsheet/" + sheet_name + "_US.fms").c_str()));
        const std::optional<net::Json> ko =
            net::Json::parse(std::string_view(reinterpret_cast<const char*>(file.data), file.size), 8);
        CHECK(native.has_value() && ko.has_value());
        if (!native || !ko) continue;
        for (const net::Json& entry : (*ko)["entries"].items()) {
            const size_t index = size_t(entry["index"].as_int(-1));
            if (index >= native->records.size()) {
                CHECK(false);
                continue;
            }
            const std::string& text = native->records[index];
            CHECK(!text.empty());
            empty_sources += text.empty();
            CHECK(Sha256Of(text) == entry["source_sha256"].as_string());
            ++records;
        }
    }
    CHECK(records == 11953 && empty_sources == 0);
    const std::optional<FmsSheet> native_quests = ParseFms(ReadArchiveEntry(archive, "msgsheet/ChallengeQuest_US.fms"));
    CHECK(native_quests && native_quests->records.size() == 200);
    if (!original.empty()) {
        std::vector<uint8_t> composed = original;
        std::string why;
        CHECK(ComposeHangulFont(composed, cells, &why));
        // Only the syllables' cells differ: a copy with the same blocks written into those cells is the composed font.
        std::vector<uint8_t> expected = original;
        std::set<int> targets;
        for (const HangulCell& cell : cells) {
            targets.insert(cell.glyph.cell);
            CHECK(WriteFontCell(expected, cell.glyph.cell, cell.blocks));
        }
        CHECK(expected == composed);
        for (const size_t index : {size_t(0), cells.size() / 2, cells.size() - 1}) {
            CHECK(FontCellInk(composed, cells[index].glyph.cell).value_or(0) > 0);
        }
        CHECK(targets.size() == cells.size());
        std::printf("KOREAN-ATLAS|%zu cells over the USA font, %zu untouched\n", targets.size(),
                    kAtlasCells - targets.size());
    }

    // The corpus over the real USA English slot, with the same composer the launcher runs.
    const GameBuild* usa = BuildNamed("CUSA10454 1.00 USA");
    CHECK(usa != nullptr);
    if (!usa) return;
    TranslationRecipe recipe{std::string_view("US"), KoreanTranslationFiles()};
    recipe.text_key = "ko";
    recipe.same_arguments = true;
    recipe.hangul = &usa->translation->hangul;
    recipe.hangul_glyphs = KoreanGlyphs();
    recipe.mark_archive = game / "DRACRO.CPK";
    recipe.glyph_table = true;
    recipe.french_layouts = true;
    const ArchiveTranslation result = TranslateArchive(game / "DRACRO_US.CPK", recipe);
    CHECK(result.error.empty() && result.overlay.has_value());
    CHECK(result.corpus == 11953 && result.translated == 11953 && result.refused == 0 && !result.folded);
    CHECK(result.sheets == 13 && result.hangul_glyphs.size() == 1075 && !result.latin_glyphs);
    CHECK(result.cyrillic_metrics.empty() && result.visuals == 0);
    std::printf("KOREAN-USA|%zu of %zu records, %zu sheets, %zu syllables\n", result.translated, result.corpus,
                result.sheets, result.hangul_glyphs.size());
    if (!result.overlay) return;

    // The served sheets: the Korean records where the official release has them, English elsewhere.
    size_t system = 0, quests = 0;
    for (const auto& [offset, data] : result.overlay->patches) {
        (void)offset;
        const std::optional<FmsSheet> sheet = ParseFms(data);
        if (!sheet) continue;
        if (sheet->records.size() == 1611) {
            ++system;
            CHECK(sheet->records[1517].find("HP") != std::string::npos); // the Korean wording of record 1517
            CHECK(sheet->records[109] != sheet->records[226]);
        } else if (sheet->records.size() == 200) {
            ++quests;
            CHECK(sheet->records[0] != "" && sheet->records[160] != "");
            size_t native_kept = 0;
            for (size_t index = 161; native_quests && index < sheet->records.size(); ++index) {
                CHECK(sheet->records[index] == native_quests->records[index]);
                native_kept += !sheet->records[index].empty();
            }
            std::printf("KOREAN-QUESTS|%zu of 39 challenge quests beyond the Korean sheet stay native\n", native_kept);
            CHECK(native_kept == 39);
        }
    }
    CHECK(system == 1 && quests == 1);
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestProfiles();
    TestCorpus();
    const std::optional<std::vector<HangulCell>> cells =
        KoreanGlyphs() ? ParseHangulGlyphs({KoreanGlyphs()->data, KoreanGlyphs()->size}) : std::nullopt;
    CHECK(cells.has_value());
    if (cells) {
        TestGlyphFile(*cells);
        TestComposeFailure(*cells);
        TestLocalUsa(*cells);
    }
    TestGlyphFileParsing();
    TestGlyphTable();
    std::printf("korean localization tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
