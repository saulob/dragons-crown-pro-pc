#include "game_translation.h"
#include "localization/ui_text.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <list>
#include <map>
#include <system_error>

#include <zlib.h>

#include "crypto.h"
#include "dcp_launcher.h"
#include "fms_sheet.h"
#include "game_files.h"
#include "latin_font.h"
#include "latin_glyphs.h"
#include "net/json.h"
#include "settings.h"
#include "text_languages.h"
#include "utf8.h"

namespace dcp {

namespace {

struct OverlayFile {
    const char* archive;
    uint64_t length;
    std::vector<DcpFilePatch> patches;
};
std::optional<OverlayFile> second_overlay; // a build whose stage files are not in its slot's archive
const std::vector<std::string>* served_default_bone = nullptr; // the records of the overlay in use
std::vector<DcpComposedGlyph> served_composed_glyphs;

} // namespace

extern const TranslationFile kPtBrTranslation[];
extern const size_t kPtBrTranslationCount;
extern const TranslationFile kPtBrVisualPatches[];
extern const size_t kPtBrVisualPatchesCount;
extern const TranslationFile kPtBrSources[];
extern const size_t kPtBrSourcesCount;
extern const TranslationFile kEnTranslation[];
extern const size_t kEnTranslationCount;
extern const TranslationFile kEnSources[];
extern const size_t kEnSourcesCount;

namespace {

constexpr size_t kMaxProblems = 5;
// SystemMsg records 109-225 are the ##(NAME) keys the other records look up; 226-342 are their values.
constexpr size_t kFirstTagKey = 109, kLastTagKey = 225;
constexpr size_t kSourceHashLength = 16;
constexpr char kMarkFontEntry[] = "other/fontUTF16.ftx";
constexpr char kSharedArchive[] = "DRACRO.CPK";

// The French layouts have the label boxes the Portuguese textures were made for. They are read from the user's own
// archive and served under the USA paths; only the embedded file name changes.
struct LayoutEdit {
    uint32_t offset;
    uint8_t before[4];
    uint8_t after[4];
};
// The TREASURE sprite's six position entries, moved right so RANKS DE meets TESOURO with a word space.
constexpr LayoutEdit kTreasureRankEdits[] = {
    {0x14880u, {0x00, 0x00, 0x0c, 0xc3}, {0x00, 0x00, 0xe4, 0xc2}},
    {0x14888u, {0x00, 0x00, 0x8c, 0xc3}, {0x00, 0x00, 0x7e, 0xc3}},
    {0x14890u, {0x00, 0x00, 0x8c, 0xc3}, {0x00, 0x00, 0x7e, 0xc3}},
    {0x14898u, {0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0xd0, 0x41}},
    {0x148a0u, {0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0xd0, 0x41}},
    {0x148a8u, {0x00, 0x00, 0x8c, 0xc3}, {0x00, 0x00, 0x7e, 0xc3}},
};
struct LayoutMirror {
    const char* source;
    const char* target;
    const char* embedded;
    const char* sha256;
    std::span<const LayoutEdit> edits;
};
constexpr LayoutMirror kLayoutMirrors[] = {
    {"stage/Interface_FR.mbs", "stage/Interface_US.mbs", "Interface_FR.mbs",
     "f8e74c3d05587048439a0e62679281df83659a6cb55af0ec71bc9a4d95623458", kTreasureRankEdits},
    {"stage/title_FR.mbs", "stage/title_US.mbs", "title_FR.mbs",
     "f9728947b789454b2d096a6a0cb607c29d763d8a8f85e0063c482f220bf70505", {}},
};

bool IsAsciiLetter(char c) { return (c | 0x20) >= 'a' && (c | 0x20) <= 'z'; }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// Through the ')' that closes the '(' at `open`, on the same line; npos when there is none.
size_t ClosingParen(std::string_view text, size_t open) {
    for (size_t i = open + 1; i < text.size() && text[i] != '\n' && text[i] != '('; ++i) {
        if (text[i] == ')') return i + 1;
    }
    return std::string_view::npos;
}

size_t PrintfEnd(std::string_view text, size_t at) {
    size_t i = at + 1;
    while (i < text.size() && std::string_view("-+ #0").find(text[i]) != std::string_view::npos) ++i;
    while (i < text.size() && IsDigit(text[i])) ++i;
    if (i < text.size() && text[i] == '.') {
        const size_t digits = ++i;
        while (i < text.size() && IsDigit(text[i])) ++i;
        if (i == digits) return std::string_view::npos;
    }
    for (std::string_view length : {"hh", "ll", "h", "l", "z"}) {
        if (text.substr(i, length.size()) == length) {
            i += length.size();
            break;
        }
    }
    return i < text.size() && std::string_view("diouxXeEfFgGcsp").find(text[i]) != std::string_view::npos
               ? i + 1
               : std::string_view::npos;
}

bool IsPrintf(std::string_view token) { return token.size() > 1 && token[0] == '%' && token != "%%"; }

// Text with a conversion the game fills in; elsewhere a % is prose ("50% of"), even where printf would read "% o".
bool Formatted(const std::vector<std::string>& tokens) {
    return std::any_of(tokens.begin(), tokens.end(),
                       [](const std::string& t) { return (IsPrintf(t) && t[1] != ' ') || t == "%%"; });
}

bool ValidUtf8(std::string_view text) {
    for (size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!n || i + n > text.size() || (n == 2 && c < 0xc2)) return false;
        uint32_t cp = n == 1 ? c : c & (0x7f >> n);
        for (size_t k = 1; k < n; ++k) {
            const auto b = static_cast<unsigned char>(text[i + k]);
            if ((b & 0xc0) != 0x80) return false;
            cp = cp << 6 | (b & 0x3f);
        }
        if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10ffff)) || (cp >= 0xd800 && cp < 0xe000)) {
            return false;
        }
        i += n;
    }
    return true;
}

std::string Sha256Hex(std::string_view text) {
    uint8_t digest[32];
    if (!crypto::Sha256(reinterpret_cast<const uint8_t*>(text.data()), text.size(), digest)) return {};
    return net::hex_encode(digest, sizeof(digest));
}

std::vector<size_t> Indices(const net::Json& index) {
    std::vector<size_t> out;
    const auto add = [&](const net::Json& value) {
        if (value.type() != net::Json::Type::kInt || value.as_int() < 0) return false;
        out.push_back(size_t(value.as_int()));
        return true;
    };
    if (index.is_array()) {
        for (const net::Json& value : index.items()) {
            if (!add(value)) return {};
        }
    } else if (!add(index)) {
        return {};
    }
    return out;
}

// A slot's records by sheet, each translated index with the leading hex of its record's SHA-256.
using SourceHashes = std::map<size_t, std::string>;
using SourceMap = std::map<std::string, SourceHashes, std::less<>>;

std::optional<SourceMap> ParseSourceMap(const TranslationFile& file) {
    const std::optional<net::Json> json =
        net::Json::parse(std::string_view(reinterpret_cast<const char*>(file.data), file.size), 8);
    if (!json || !json->is_object() || !(*json)["sheets"].is_array()) return std::nullopt;
    SourceMap map;
    for (const net::Json& sheet : (*json)["sheets"].items()) {
        if (!sheet["sheet"].is_string() || !sheet["hashes"].is_array()) return std::nullopt;
        SourceHashes& hashes = map[sheet["sheet"].as_string()];
        for (const net::Json& row : sheet["hashes"].items()) {
            if (!row.is_array() || row.items().size() != 2 || row.items()[0].type() != net::Json::Type::kInt ||
                !row.items()[1].is_string()) {
                return std::nullopt;
            }
            hashes[size_t(row.items()[0].as_int())] = row.items()[1].as_string();
        }
    }
    return map;
}

// A delta's hashes join the map; its excluded records leave it.
bool ApplySourceDelta(SourceMap& map, const TranslationFile& file) {
    const std::optional<SourceMap> added = ParseSourceMap(file);
    const std::optional<net::Json> json =
        net::Json::parse(std::string_view(reinterpret_cast<const char*>(file.data), file.size), 8);
    if (!added || !json) return false;
    for (const auto& [sheet, hashes] : *added) map[sheet].insert(hashes.begin(), hashes.end());
    for (const net::Json& sheet : (*json)["sheets"].items()) {
        SourceHashes& hashes = map[sheet["sheet"].as_string()];
        for (const net::Json& row : sheet["excluded"].items()) {
            if (!row.is_array() || row.items().empty() || row.items()[0].type() != net::Json::Type::kInt) return false;
            hashes.erase(size_t(row.items()[0].as_int()));
        }
    }
    return true;
}

// Why the record cannot show `text`; empty when it can. `mapped`: the slot's source map, for a slot not in English.
std::string RecordProblem(std::string_view sheet, const std::vector<std::string>& records, size_t index,
                          std::string_view source_sha256, std::string_view text, const SourceHashes* mapped) {
    if (index >= records.size()) return "no such record";
    if (sheet == "SystemMsg" && index >= kFirstTagKey && index <= kLastTagKey) return "a tag key, never translated";
    if (mapped) {
        const auto found = mapped->find(index);
        if (found == mapped->end()) return "no record of this slot maps to it";
        if (Sha256Hex(records[index]).substr(0, kSourceHashLength) != found->second) return "the source record changed";
    } else if (Sha256Hex(records[index]) != source_sha256) {
        return "the English record changed";
    }
    if (!ValidUtf8(text)) return "not UTF-8";
    if (std::any_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 && c != '\n'; })) {
        return "a control character";
    }
    if (mapped) {
        if (!SameArguments(records[index], text)) return "printf or button sequences differ from the source";
    } else if (!SameControlTokens(records[index], text)) {
        return "control sequences differ from the English";
    }
    return {};
}

bool TextHashMatches(const SourceMap& expected, const std::string& sheet, size_t index, std::string_view text) {
    const auto found = expected.find(sheet);
    if (found == expected.end()) return false;
    const auto hash = found->second.find(index);
    return hash != found->second.end() && Sha256Hex(text).substr(0, kSourceHashLength) == hash->second;
}

class Stopwatch {
public:
    uint32_t Lap() {
        const auto now = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_).count();
        last_ = now;
        return static_cast<uint32_t>(ms);
    }

private:
    std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
};

struct SheetResult {
    std::string entry;
    std::vector<uint8_t> data;
};

// DRACRO.CPK's unsuffixed sheets name ObjectParam differently.
std::string SheetEntry(std::string_view sheet, std::string_view suffix) {
    if (suffix.empty()) return "msgsheet/" + std::string(sheet == "ObjectParam" ? "ObjectParamMsg" : sheet) + ".fms";
    return "msgsheet/" + std::string(sheet) + "_" + std::string(suffix) + ".fms";
}

struct SheetSources {
    const SourceMap* slot = nullptr; // the slot's records by index
    const SourceMap* text = nullptr; // the hash each entry's text must have
    std::string_view text_key;
};

std::optional<SheetResult> TranslateSheet(const CpkArchive& archive, std::string_view suffix, const TranslationFile& file,
                                          const SheetSources& sources, bool fold, ArchiveTranslation& result) {
    const SourceMap* source = sources.slot;
    const auto problem = [&](const std::string& text) {
        if (result.problems.size() < kMaxProblems) result.problems.push_back(text);
    };
    const std::optional<net::Json> json =
        net::Json::parse(std::string_view(reinterpret_cast<const char*>(file.data), file.size), 8);
    if (!json || !json->is_object() || !(*json)["sheet"].is_string() || !(*json)["entries"].is_array()) {
        ++result.refused;
        problem(std::string(file.name) + ": not a translation file");
        return std::nullopt;
    }
    const std::string& sheet = (*json)["sheet"].as_string();
    SheetResult out{SheetEntry(sheet, suffix), {}};
    const CpkEntry* entry = archive.Find(out.entry);
    std::string error;
    const std::vector<uint8_t> original = entry ? archive.Read(*entry, &error) : std::vector<uint8_t>{};
    std::optional<FmsSheet> parsed = ParseFms(original);
    if (!parsed || BuildFms(*parsed) != original) {
        ++result.refused;
        problem(out.entry + (entry ? error.empty() ? ": not a message sheet the port can rebuild" : ": " + error
                                   : ": not in the archive"));
        return std::nullopt;
    }
    const std::vector<std::string> native = parsed->records;
    std::vector<bool> done(native.size());
    size_t translated = 0;
    static const SourceHashes kNoHashes;
    const SourceHashes* mapped = nullptr;
    if (source) {
        const auto found = source->find(sheet);
        mapped = found == source->end() ? &kNoHashes : &found->second;
    }
    for (const net::Json& item : (*json)["entries"].items()) {
        const std::vector<size_t> indices = Indices(item["index"]);
        const net::Json& hash = item["source_sha256"];
        const net::Json& text = item[sources.text_key];
        if (indices.empty() || !hash.is_string() || !text.is_string()) {
            ++result.refused;
            problem(sheet + ": an entry without index, source_sha256 and " + std::string(sources.text_key));
            continue;
        }
        for (const size_t index : indices) {
            ++result.corpus;
            std::string why = RecordProblem(sheet, native, index, hash.as_string(), text.as_string(), mapped);
            if (why.empty() && sources.text && !TextHashMatches(*sources.text, sheet, index, text.as_string())) {
                why = "not the expected text";
            }
            if (why.empty() && done[index]) why = "translated twice";
            if (!why.empty()) {
                ++result.refused;
                problem(sheet + " " + std::to_string(index) + ": " + why);
                continue;
            }
            done[index] = true;
            ++translated;
            parsed->records[index] = fold ? FoldLatinLetters(text.as_string()) : text.as_string();
        }
    }
    if (!translated) return std::nullopt;
    if (sheet == "DefaultBone") result.default_bone = parsed->records;
    result.translated += translated;
    ++result.sheets;
    out.data = BuildFms(*parsed);
    return out;
}

} // namespace

std::span<const TranslationFile> PtBrTranslationFiles() { return {kPtBrTranslation, kPtBrTranslationCount}; }
std::span<const TranslationFile> EnglishTranslationFiles() { return {kEnTranslation, kEnTranslationCount}; }
std::span<const TranslationFile> PtBrVisualPatches() { return {kPtBrVisualPatches, kPtBrVisualPatchesCount}; }

std::vector<TranslationFile> PtBrVisualPatches(VisualSet set) {
    std::vector<TranslationFile> chosen;
    for (const TranslationFile& file : PtBrVisualPatches()) {
        if (std::string_view(file.name).starts_with("base-") == (set == VisualSet::kBaseStage)) chosen.push_back(file);
    }
    return chosen;
}

const TranslationFile* PtBrSource(std::string_view source) {
    const std::string name = std::string(source) + ".json";
    for (const TranslationFile& file : std::span<const TranslationFile>(kPtBrSources, kPtBrSourcesCount)) {
        if (file.name == name) return &file;
    }
    return nullptr;
}

const TranslationFile* EnglishSource(std::string_view source) {
    const std::string name = std::string(source) + ".json";
    for (const TranslationFile& file : std::span<const TranslationFile>(kEnSources, kEnSourcesCount)) {
        if (file.name == name) return &file;
    }
    return nullptr;
}

std::string FoldLatinLetters(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if ((lead == 0xC3) && i + 1 < text.size()) {
            const uint16_t code = uint16_t((lead & 0x1f) << 6 | (static_cast<unsigned char>(text[i + 1]) & 0x3f));
            const auto found = std::find_if(std::begin(latin_glyphs), std::end(latin_glyphs),
                                            [&](const LatinGlyph& glyph) { return glyph.code == code; });
            if (found != std::end(latin_glyphs)) {
                out += found->base;
                ++i;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

std::vector<std::string> ControlTokens(std::string_view text) {
    std::vector<std::string> tokens;
    for (size_t i = 0; i < text.size();) {
        size_t end = std::string_view::npos;
        const char c = text[i];
        const char next = i + 1 < text.size() ? text[i + 1] : '\0';
        if (text.substr(i, 3) == "##(") {
            end = ClosingParen(text, i + 2);
        } else if (c == '@') {
            if (next == '/' && i + 2 < text.size() && IsAsciiLetter(text[i + 2])) {
                end = i + 3;
            } else if ((IsAsciiLetter(next) || next == '#') && i + 2 < text.size() && text[i + 2] == '(') {
                end = ClosingParen(text, i + 2);
            }
            if (end == std::string_view::npos) end = next && next != '\n' ? i + 2 : i + 1;
        } else if (c == '%') {
            end = next == '%' ? i + 2 : PrintfEnd(text, i);
            if (end == std::string_view::npos) end = i + 1;
        } else if (c == '$') {
            end = i + 1;
        }
        if (end == std::string_view::npos) {
            ++i;
            continue;
        }
        tokens.emplace_back(text.substr(i, end - i));
        i = end;
    }
    return tokens;
}

bool SameControlTokens(std::string_view source, std::string_view translation) {
    std::vector<std::string> a = ControlTokens(source), b = ControlTokens(translation);
    if (!Formatted(a)) {
        if (Formatted(b)) return false;
        const auto percent = [](const std::string& t) { return t[0] == '%'; };
        std::erase_if(a, percent);
        std::erase_if(b, percent);
    }
    std::vector<std::string> printf_a, printf_b;
    std::copy_if(a.begin(), a.end(), std::back_inserter(printf_a), IsPrintf);
    std::copy_if(b.begin(), b.end(), std::back_inserter(printf_b), IsPrintf);
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return printf_a == printf_b && a == b;
}

bool SameArguments(std::string_view source, std::string_view translation) {
    const auto arguments = [](std::string_view text) {
        std::vector<std::string> conversions, buttons;
        for (std::string& token : ControlTokens(text)) {
            if (IsPrintf(token) && token[1] != ' ') conversions.push_back(std::move(token));
            else if (token.rfind("@#(", 0) == 0) buttons.push_back(std::move(token));
        }
        std::sort(buttons.begin(), buttons.end());
        return std::pair(std::move(conversions), std::move(buttons));
    };
    return arguments(source) == arguments(translation);
}

std::vector<uint8_t> InflateVisual(const TranslationFile& file) {
    if (file.size < 4) return {};
    const uint32_t size = uint32_t(file.data[0]) | uint32_t(file.data[1]) << 8 | uint32_t(file.data[2]) << 16 |
                          uint32_t(file.data[3]) << 24;
    if (size == 0 || size > 64u << 20) return {};
    std::vector<uint8_t> out(size);
    uLongf dest = size;
    if (uncompress(out.data(), &dest, file.data + 4, uLong(file.size - 4)) != Z_OK || dest != size) return {};
    return out;
}

void AddVisualPatches(const CpkArchive& archive, std::span<const TranslationFile> blobs,
                      std::vector<std::pair<std::string, std::vector<uint8_t>>>& entries,
                      ArchiveTranslation& result) {
    std::vector<BlockPatch> patches;
    Stopwatch watch;
    for (const TranslationFile& file : blobs) {
        const std::vector<uint8_t> data = InflateVisual(file);
        std::optional<BlockPatch> patch = data.empty() ? std::nullopt : ParseBlockPatch(data);
        if (!patch) {
            ++result.visual_skipped;
            result.visual_problems.push_back(std::string(file.name) + ": unreadable visual patch");
            continue;
        }
        patches.push_back(std::move(*patch));
    }
    result.inflate_ms = watch.Lap();
    std::vector<std::string> names;
    std::vector<std::vector<const BlockPatchTarget*>> groups;
    for (const BlockPatch& patch : patches) {
        for (const BlockPatchTarget& target : patch.targets) {
            const auto found = std::find(names.begin(), names.end(), target.name);
            if (found == names.end()) {
                names.push_back(target.name);
                groups.push_back({&target});
            } else {
                groups[size_t(found - names.begin())].push_back(&target);
            }
        }
    }
    for (size_t i = 0; i < names.size(); ++i) {
        const CpkEntry* entry = archive.Find(names[i]);
        watch.Lap();
        std::vector<uint8_t> data = entry ? archive.Read(*entry) : std::vector<uint8_t>();
        result.read_ms += watch.Lap();
        if (!entry || (data.empty() && entry->extract_size != 0)) {
            ++result.visual_skipped;
            result.visual_problems.push_back(names[i] + ": not in the archive");
            continue;
        }
        bool applied = false;
        for (const BlockPatchTarget* target : groups[i]) {
            if (data.size() != target->size) continue;
            const uint32_t crc = Crc32(data);
            if (crc != target->crc_before && crc != target->crc_after) continue;
            std::vector<uint8_t> candidate = data;
            if (crc == target->crc_before) {
                for (const BlockRun& run : target->runs) std::memcpy(candidate.data() + run.offset, run.data.data(), 16);
            }
            if (Crc32(candidate) == target->crc_after) {
                data = std::move(candidate);
                applied = true;
                break;
            }
        }
        if (!applied) {
            ++result.visual_skipped;
            result.visual_problems.push_back(names[i] + ": source fingerprint not recognized");
            continue;
        }
        const bool repeated = std::any_of(entries.begin(), entries.end(),
                                          [&](const auto& e) { return e.first == names[i]; });
        if (repeated) {
            ++result.visual_skipped;
            continue;
        }
        entries.emplace_back(names[i], std::move(data));
        ++result.visuals;
    }
}

void AddLayoutMirrors(const CpkArchive& archive, std::vector<std::pair<std::string, std::vector<uint8_t>>>& entries,
                      ArchiveTranslation& result) {
    for (const LayoutMirror& mirror : kLayoutMirrors) {
        const CpkEntry* entry = archive.Find(mirror.source);
        std::vector<uint8_t> data = entry ? archive.Read(*entry) : std::vector<uint8_t>();
        if (!entry || data.empty()) {
            ++result.visual_skipped;
            result.visual_problems.push_back(std::string(mirror.source) + ": not in the archive");
            continue;
        }
        if (Sha256Hex(std::string_view(reinterpret_cast<const char*>(data.data()), data.size())) != mirror.sha256) {
            ++result.visual_skipped;
            result.visual_problems.push_back(std::string(mirror.source) + ": source fingerprint not recognized");
            continue;
        }
        const std::string old = mirror.embedded;
        const std::string name = std::filesystem::path(mirror.target).filename().string();
        if (old.size() != name.size()) continue;
        auto at = std::search(data.begin(), data.end(), old.begin(), old.end());
        if (at == data.end() || std::search(at + old.size(), data.end(), old.begin(), old.end()) != data.end()) {
            ++result.visual_skipped;
            result.visual_problems.push_back(std::string(mirror.source) + ": layout name not found");
            continue;
        }
        std::copy(name.begin(), name.end(), at);
        const bool edited = std::all_of(mirror.edits.begin(), mirror.edits.end(), [&](const LayoutEdit& edit) {
            return size_t(edit.offset) + 4 <= data.size() && std::equal(edit.before, edit.before + 4, data.begin() + edit.offset);
        });
        if (!edited) {
            ++result.visual_skipped;
            result.visual_problems.push_back(std::string(mirror.source) + ": layout edit does not match");
            continue;
        }
        for (const LayoutEdit& edit : mirror.edits) std::copy(edit.after, edit.after + 4, data.begin() + edit.offset);
        const bool repeated = std::any_of(entries.begin(), entries.end(),
                                          [&](const auto& e) { return e.first == mirror.target; });
        if (repeated) {
            ++result.visual_skipped;
            continue;
        }
        entries.emplace_back(mirror.target, std::move(data));
        ++result.visuals;
    }
}

namespace {

// The font with the letters, or why not. The marks come from DRACRO.CPK's font in every region.
std::optional<std::vector<uint8_t>> CompletedFont(const CpkArchive& archive, const TranslationRecipe& recipe, std::string& why,
                                                  std::vector<LatinCell>& cells) {
    if (!recipe.glyph_table) {
        why = "no glyph table for the letters in the executable";
        return std::nullopt;
    }
    const CpkEntry* entry = archive.Find(recipe.font_entry);
    std::vector<uint8_t> font = entry ? archive.Read(*entry, &why) : std::vector<uint8_t>();
    if (font.empty()) {
        if (why.empty()) why = "not in the archive";
        return std::nullopt;
    }
    CpkArchive shared;
    const CpkEntry* marks_entry = nullptr;
    std::vector<uint8_t> marks;
    if (shared.Open(recipe.mark_archive, &why) && (marks_entry = shared.Find(kMarkFontEntry))) marks = shared.Read(*marks_entry, &why);
    if (marks.empty()) {
        if (why.empty()) why = "the mark font is missing";
        return std::nullopt;
    }
    if (!ComposeLatinFont(font, marks, &why, &cells)) return std::nullopt;
    return font;
}

} // namespace

ArchiveTranslation TranslateArchive(const std::filesystem::path& archive_path, std::string_view suffix,
                                    std::span<const TranslationFile> files,
                                    std::span<const TranslationFile> visual_patches) {
    return TranslateArchive(archive_path, TranslationRecipe{suffix, files, visual_patches});
}

ArchiveTranslation TranslateArchive(const std::filesystem::path& archive_path, const TranslationRecipe& recipe) {
    const std::span<const TranslationFile> files = recipe.files, visual_patches = recipe.visual_patches;
    ArchiveTranslation result;
    if (files.empty() && visual_patches.empty()) return result;
    CpkArchive archive;
    std::error_code ec;
    const uint64_t length = std::filesystem::file_size(archive_path, ec);
    if (ec || !archive.Open(archive_path, &result.error)) {
        result.error = Utf8(archive_path.filename()) + ": " + (ec ? "cannot read its size" : result.error);
        return result;
    }
    std::vector<std::pair<std::string, std::vector<uint8_t>>> sheets;
    Stopwatch watch;
    std::optional<SourceMap> source, text_hashes;
    if (recipe.source) {
        source = ParseSourceMap(*recipe.source);
        if (!source) {
            result.error = std::string(recipe.source->name) + ": not a source map";
            return result;
        }
        if (recipe.source_delta && !ApplySourceDelta(*source, *recipe.source_delta)) {
            result.error = std::string(recipe.source_delta->name) + ": not a source map delta";
            return result;
        }
    }
    if (recipe.text_hashes && !(text_hashes = ParseSourceMap(*recipe.text_hashes))) {
        result.error = std::string(recipe.text_hashes->name) + ": not a hash map";
        return result;
    }
    const SheetSources sources{source ? &*source : nullptr, text_hashes ? &*text_hashes : nullptr, recipe.text_key};
    bool fold = false;
    if (!recipe.font_entry.empty()) {
        std::string why;
        std::optional<std::vector<uint8_t>> font = CompletedFont(archive, recipe, why, result.latin_cells);
        if (font) {
            sheets.emplace_back(std::string(recipe.font_entry), std::move(*font));
            result.latin_glyphs = true;
        } else {
            fold = result.folded = true;
            if (result.problems.size() < kMaxProblems) {
                result.problems.push_back(std::string(recipe.font_entry) + ": " + why + ", letters shown without accents");
            }
        }
    }
    for (const TranslationFile& file : files) {
        std::optional<SheetResult> sheet = TranslateSheet(archive, recipe.suffix, file, sources, fold, result);
        if (!sheet) continue;
        const bool repeated = std::any_of(sheets.begin(), sheets.end(), [&](const auto& s) { return s.first == sheet->entry; });
        if (repeated) {
            ++result.refused;
            if (result.problems.size() < kMaxProblems) result.problems.push_back(sheet->entry + ": translated twice");
            continue;
        }
        sheets.emplace_back(std::move(sheet->entry), std::move(sheet->data));
    }
    result.sheets_ms = watch.Lap();
    if (!visual_patches.empty()) {
        AddVisualPatches(archive, visual_patches, sheets, result);
        result.visual_ms = watch.Lap();
        if (recipe.french_layouts) AddLayoutMirrors(archive, sheets, result);
        result.mirror_ms = watch.Lap();
    }
    if (!sheets.empty()) {
        result.overlay = OverlayEntries(archive, length, sheets, &result.error);
        if (!result.overlay) result.error = Utf8(archive_path.filename()) + ": " + result.error;
        result.overlay_ms = watch.Lap();
    }
    return result;
}

} // namespace dcp

extern "C" void dcp_activate_text_language(const char* settings_file, const char* image_dir, const char* app0) {
    using namespace dcp;
    const GameBuild* build = BuildOfImageDir(image_dir);
    const LauncherSettings settings(PathFromUtf8(settings_file ? settings_file : ""));
    const std::vector<TextLanguage> languages = InstalledTextLanguages(PathFromUtf8(app0 ? app0 : ""), build);
    localization::SetActiveLanguage(ResolveTextLanguage(languages, settings.Get(kTextLanguageKey)).key);
}

namespace dcp {
namespace {

OverlayFile OverlayTable(const ArchiveTranslation& translation, const char* archive) {
    OverlayFile file{archive, translation.overlay->length, {}};
    for (const auto& [offset, data] : translation.overlay->patches) file.patches.push_back({offset, data.size(), data.data()});
    return file;
}

} // namespace
} // namespace dcp

extern "C" const char* dcp_text_default_bone(unsigned record) {
    if (!dcp::served_default_bone || record >= dcp::served_default_bone->size()) return nullptr;
    return (*dcp::served_default_bone)[record].c_str();
}

extern "C" const DcpComposedGlyph* dcp_text_composed_glyphs(size_t* count, int* cell_pixels) {
    *count = dcp::served_composed_glyphs.size();
    *cell_pixels = dcp::kLatinFontCellPixels;
    return *count ? dcp::served_composed_glyphs.data() : nullptr;
}

extern "C" const char* dcp_text_stage_overlay(uint64_t* length, DcpFilePatch** patches, size_t* count) {
    if (!dcp::second_overlay) return nullptr;
    *length = dcp::second_overlay->length;
    *patches = dcp::second_overlay->patches.data();
    *count = dcp::second_overlay->patches.size();
    return dcp::second_overlay->archive;
}

extern "C" unsigned dcp_text_font_probe(const char* settings_file, const char* image_dir, const char* app0) {
    using namespace dcp;
    const GameBuild* build = BuildOfImageDir(image_dir);
    if (!build || build->translation->font_entry.empty()) return 0;
    const LauncherSettings settings(PathFromUtf8(settings_file ? settings_file : ""));
    const std::vector<TextLanguage> languages = InstalledTextLanguages(PathFromUtf8(app0 ? app0 : ""), build);
    const TextLanguage& language = ResolveTextLanguage(languages, settings.Get(kTextLanguageKey));
    return language.translation == "pt-BR" ? build->translation->font_probe : 0;
}

extern "C" const char* dcp_text_translation(const char* settings_file, const char* image_dir, const char* app0,
                                            uint64_t* length, DcpFilePatch** patches, size_t* count, char* message,
                                            size_t message_size, int glyph_table, int* glyphs_composed) {
    using namespace dcp;
    static std::list<ArchiveTranslation> retained; // the runtime borrows the overlay data for the life of the process
    static std::vector<DcpFilePatch> table;
    if (message_size) *message = '\0';
    if (glyphs_composed) *glyphs_composed = 0;
    const GameBuild* build = BuildOfImageDir(image_dir);
    const LauncherSettings settings(PathFromUtf8(settings_file ? settings_file : ""));
    const std::filesystem::path folder = PathFromUtf8(app0 ? app0 : "");
    const std::vector<TextLanguage> languages = InstalledTextLanguages(folder, build);
    const TextLanguage& language = ResolveTextLanguage(languages, settings.Get(kTextLanguageKey));
    localization::SetActiveLanguage(language.key);
    second_overlay.reset();
    served_default_bone = nullptr;
    served_composed_glyphs.clear();
    if (language.translation.empty() || !build) return nullptr;
    const TranslationProfile& profile = *build->translation;
    const std::string name(language.name);
    const char* const native = profile.source.empty() ? "English" : "the game's own language";
    const auto started = std::chrono::steady_clock::now();
    const std::vector<TranslationFile> visuals = PtBrVisualPatches(profile.visuals);
    const bool same_archive = std::string_view(profile.archive) == profile.visual_archive;
    TranslationRecipe recipe{language.suffix, {}};
    if (language.translation == "pt-BR") {
        recipe.files = PtBrTranslationFiles();
        if (same_archive) recipe.visual_patches = visuals;
        if (!profile.source.empty()) recipe.source = PtBrSource(profile.source);
        recipe.font_entry = profile.font_entry;
    } else if (language.translation == kPortEnglish) {
        recipe.files = EnglishTranslationFiles();
        recipe.text_key = "en";
        recipe.text_hashes = EnglishSource("US");
        recipe.source = PtBrSource(profile.source);
        recipe.source_delta = EnglishSource(profile.source);
    }
    recipe.mark_archive = folder / kSharedArchive;
    recipe.glyph_table = glyph_table != 0;
    recipe.french_layouts = profile.visuals == VisualSet::kEnglishSlot;
    retained.push_back(TranslateArchive(folder / profile.archive, recipe));
    ArchiveTranslation& translation = retained.back();
    if (!same_archive && language.translation == "pt-BR") {
        TranslationRecipe stage{{}, {}};
        stage.visual_patches = visuals;
        stage.french_layouts = false;
        retained.push_back(TranslateArchive(folder / profile.visual_archive, stage));
        const ArchiveTranslation& second = retained.back();
        translation.visuals += second.visuals;
        translation.visual_skipped += second.visual_skipped;
        translation.visual_problems.insert(translation.visual_problems.end(), second.visual_problems.begin(), second.visual_problems.end());
        if (!second.error.empty()) translation.visual_problems.push_back(second.error);
        if (second.overlay) second_overlay = OverlayTable(second, profile.visual_archive);
    }
    const long long total_ms = static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count());
    std::printf("Menu & Subtitles: overlay prepared in %lld ms (sheets %u, visual patches %u [inflate %u, read %u], layouts %u, overlay %u)\n",
                total_ms, translation.sheets_ms, translation.visual_ms, translation.inflate_ms, translation.read_ms,
                translation.mirror_ms, translation.overlay_ms);
    std::string text;
    if (!translation.error.empty()) {
        text = name + " unavailable (" + translation.error + "), showing " + native;
    } else if (!translation.overlay) {
        text = name + ": no record translated yet, showing " + native;
    } else {
        text = name + ": " + std::to_string(translation.translated) + " records translated of " +
               std::to_string(translation.corpus) + " in " + std::to_string(translation.sheets) + " sheets, " +
               std::to_string(translation.visuals) + " visual replacements, the others in " + native;
    }
    if (translation.latin_glyphs) text += "; accented letters composed from the installed font";
    for (const std::string& skipped : translation.visual_problems) std::printf("Menu & Subtitles: PT-BR visual skipped: %s\n", skipped.c_str());
    if (translation.refused || translation.folded) {
        const size_t refused = translation.refused;
        text += "; " + std::to_string(refused) + " kept in " + native + " (";
        for (size_t i = 0; i < translation.problems.size(); ++i) text += (i ? "; " : "") + translation.problems[i];
        text += refused > translation.problems.size() ? "; ...)" : ")";
    }
    std::snprintf(message, message_size, "%s", text.c_str());
    if (!translation.error.empty() || !translation.overlay) return nullptr;
    if (glyphs_composed) *glyphs_composed = translation.latin_glyphs;
    served_default_bone = &translation.default_bone;
    if (translation.latin_glyphs) {
        for (const LatinCell& cell : translation.latin_cells) {
            served_composed_glyphs.push_back({cell.code, uint8_t(cell.base), cell.alpha.data(), cell.base_alpha.data()});
        }
    }
    table.clear();
    for (const auto& [offset, data] : translation.overlay->patches) table.push_back({offset, data.size(), data.data()});
    *length = translation.overlay->length;
    *patches = table.data();
    *count = table.size();
    return profile.archive;
}
