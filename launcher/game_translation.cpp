#include "game_translation.h"
#include "localization/ui_text.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <list>
#include <system_error>

#include <zlib.h>

#include "crypto.h"
#include "dcp_launcher.h"
#include "fms_sheet.h"
#include "game_files.h"
#include "net/json.h"
#include "settings.h"
#include "text_languages.h"
#include "utf8.h"

namespace dcp {

extern const TranslationFile kPtBrTranslation[];
extern const size_t kPtBrTranslationCount;
extern const TranslationFile kPtBrVisualPatches[];
extern const size_t kPtBrVisualPatchesCount;

namespace {

constexpr size_t kMaxProblems = 5;
// SystemMsg records 109-225 are the ##(NAME) keys the other records look up; 226-342 are their values.
constexpr size_t kFirstTagKey = 109, kLastTagKey = 225;

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

// Why the record cannot show `text`; empty when it can.
std::string RecordProblem(std::string_view sheet, const std::vector<std::string>& records, size_t index,
                          std::string_view source_sha256, std::string_view text) {
    if (index >= records.size()) return "no such record";
    if (sheet == "SystemMsg" && index >= kFirstTagKey && index <= kLastTagKey) return "a tag key, never translated";
    if (Sha256Hex(records[index]) != source_sha256) return "the English record changed";
    if (!ValidUtf8(text)) return "not UTF-8";
    if (std::any_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 && c != '\n'; })) {
        return "a control character";
    }
    if (!SameControlTokens(records[index], text)) return "control sequences differ from the English";
    return {};
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

std::optional<SheetResult> TranslateSheet(const CpkArchive& archive, std::string_view suffix, const TranslationFile& file,
                                          ArchiveTranslation& result) {
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
    SheetResult out{"msgsheet/" + sheet + "_" + std::string(suffix) + ".fms", {}};
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
    const std::vector<std::string> english = parsed->records;
    std::vector<bool> done(english.size());
    size_t translated = 0;
    for (const net::Json& item : (*json)["entries"].items()) {
        const std::vector<size_t> indices = Indices(item["index"]);
        const net::Json& hash = item["source_sha256"];
        const net::Json& text = item["pt_br"];
        if (indices.empty() || !hash.is_string() || !text.is_string()) {
            ++result.refused;
            problem(sheet + ": an entry without index, source_sha256 and pt_br");
            continue;
        }
        for (const size_t index : indices) {
            std::string why = RecordProblem(sheet, english, index, hash.as_string(), text.as_string());
            if (why.empty() && done[index]) why = "translated twice";
            if (!why.empty()) {
                ++result.refused;
                problem(sheet + " " + std::to_string(index) + ": " + why);
                continue;
            }
            done[index] = true;
            ++translated;
            parsed->records[index] = text.as_string();
        }
    }
    if (!translated) return std::nullopt;
    result.translated += translated;
    ++result.sheets;
    out.data = BuildFms(*parsed);
    return out;
}

} // namespace

std::span<const TranslationFile> PtBrTranslationFiles() { return {kPtBrTranslation, kPtBrTranslationCount}; }
std::span<const TranslationFile> PtBrVisualPatches() { return {kPtBrVisualPatches, kPtBrVisualPatchesCount}; }

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
            if (result.problems.size() < kMaxProblems) result.problems.push_back(std::string(file.name) + ": unreadable visual patch");
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
            if (result.problems.size() < kMaxProblems) result.problems.push_back(names[i] + ": not in the archive");
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
            if (result.problems.size() < kMaxProblems) result.problems.push_back(names[i] + ": source changed, kept English");
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
            if (result.problems.size() < kMaxProblems) result.problems.push_back(std::string(mirror.source) + ": not in the archive");
            continue;
        }
        if (Sha256Hex(std::string_view(reinterpret_cast<const char*>(data.data()), data.size())) != mirror.sha256) {
            ++result.visual_skipped;
            if (result.problems.size() < kMaxProblems) result.problems.push_back(std::string(mirror.source) + ": source changed");
            continue;
        }
        const std::string old = mirror.embedded;
        const std::string name = std::filesystem::path(mirror.target).filename().string();
        if (old.size() != name.size()) continue;
        auto at = std::search(data.begin(), data.end(), old.begin(), old.end());
        if (at == data.end() || std::search(at + old.size(), data.end(), old.begin(), old.end()) != data.end()) {
            ++result.visual_skipped;
            if (result.problems.size() < kMaxProblems) result.problems.push_back(std::string(mirror.source) + ": layout name not found");
            continue;
        }
        std::copy(name.begin(), name.end(), at);
        const bool edited = std::all_of(mirror.edits.begin(), mirror.edits.end(), [&](const LayoutEdit& edit) {
            return size_t(edit.offset) + 4 <= data.size() && std::equal(edit.before, edit.before + 4, data.begin() + edit.offset);
        });
        if (!edited) {
            ++result.visual_skipped;
            if (result.problems.size() < kMaxProblems) result.problems.push_back(std::string(mirror.source) + ": layout edit does not match");
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

ArchiveTranslation TranslateArchive(const std::filesystem::path& archive_path, std::string_view suffix,
                                    std::span<const TranslationFile> files,
                                    std::span<const TranslationFile> visual_patches) {
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
    for (const TranslationFile& file : files) {
        std::optional<SheetResult> sheet = TranslateSheet(archive, suffix, file, result);
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
        AddLayoutMirrors(archive, sheets, result);
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

extern "C" const char* dcp_text_translation(const char* settings_file, const char* image_dir, const char* app0,
                                            uint64_t* length, DcpFilePatch** patches, size_t* count, char* message,
                                            size_t message_size) {
    using namespace dcp;
    static std::list<ArchiveTranslation> retained; // the runtime borrows the overlay data for the life of the process
    static std::vector<DcpFilePatch> table;
    if (message_size) *message = '\0';
    const GameBuild* build = BuildOfImageDir(image_dir);
    const LauncherSettings settings(PathFromUtf8(settings_file ? settings_file : ""));
    const std::filesystem::path folder = PathFromUtf8(app0 ? app0 : "");
    const std::vector<TextLanguage> languages = InstalledTextLanguages(folder, build);
    const TextLanguage& language = ResolveTextLanguage(languages, settings.Get(kTextLanguageKey));
    localization::SetActiveLanguage(language.key);
    if (language.translation.empty() || !build || !build->regional_archive) return nullptr;
    const std::string name(language.name);
    const auto started = std::chrono::steady_clock::now();
    retained.push_back(TranslateArchive(folder / build->regional_archive, language.suffix,
                                   language.translation == "pt-BR" ? PtBrTranslationFiles()
                                                                   : std::span<const TranslationFile>(),
                                   language.translation == "pt-BR" ? PtBrVisualPatches()
                                                                   : std::span<const TranslationFile>()));
    const ArchiveTranslation& translation = retained.back();
    const long long total_ms = static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count());
    std::printf("Menu & Subtitles: overlay prepared in %lld ms (sheets %u, visual patches %u [inflate %u, read %u], layouts %u, overlay %u)\n",
                total_ms, translation.sheets_ms, translation.visual_ms, translation.inflate_ms, translation.read_ms,
                translation.mirror_ms, translation.overlay_ms);
    std::string text;
    if (!translation.error.empty()) {
        text = name + " unavailable (" + translation.error + "), showing English";
    } else if (!translation.overlay) {
        text = name + ": no record translated yet, showing English";
    } else {
        text = name + ": " + std::to_string(translation.translated) + " records translated in " +
               std::to_string(translation.sheets) + " sheets, " + std::to_string(translation.visuals) +
               " visual replacements, the others in English";
    }
    if (translation.refused || translation.visual_skipped) {
        const size_t refused = translation.refused + translation.visual_skipped;
        text += "; " + std::to_string(refused) + " kept in English (";
        for (size_t i = 0; i < translation.problems.size(); ++i) text += (i ? "; " : "") + translation.problems[i];
        text += refused > translation.problems.size() ? "; ...)" : ")";
    }
    std::snprintf(message, message_size, "%s", text.c_str());
    if (!translation.error.empty() || !translation.overlay) return nullptr;
    table.clear();
    for (const auto& [offset, data] : translation.overlay->patches) table.push_back({offset, data.size(), data.data()});
    *length = translation.overlay->length;
    *patches = table.data();
    *count = table.size();
    return build->regional_archive;
}
