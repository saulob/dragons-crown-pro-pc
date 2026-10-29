#include "text_languages.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>

#include "cpk_patch.h"

namespace dcp {
namespace {

// The path with the language suffix taken out (stage/Menu_FR.ftx is stage/Menu.ftx), empty when it is not a file of
// that suffix.
std::string Unsuffixed(std::string_view path, std::string_view suffix) {
    const size_t dot = path.rfind('.');
    const size_t slash = path.rfind('/');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
        return {};
    }
    const std::string_view stem = path.substr(0, dot);
    const size_t start = slash == std::string_view::npos ? 0 : slash + 1;
    if (stem.size() < start + suffix.size() + 2 || !stem.ends_with(suffix) || stem[stem.size() - suffix.size() - 1] != '_') {
        return {};
    }
    return std::string(stem.substr(0, stem.size() - suffix.size() - 1)) + std::string(path.substr(dot));
}

std::set<std::string> FilesOf(const CpkArchive& archive, std::string_view suffix) {
    std::set<std::string> files;
    for (const CpkEntry& entry : archive.entries()) {
        std::string file = Unsuffixed(entry.path, suffix);
        if (!file.empty()) {
            files.insert(std::move(file));
        }
    }
    return files;
}

bool InRussianAlphabet(char32_t c) {
    return (c >= 0x410 && c <= 0x44f) || c == 0x401 || c == 0x451;
}

std::string_view SavedLanguageName(std::string_view key) {
    if (key == kRussianKey) {
        return kRussianName;
    }
    for (const GameBuild& build : SupportedBuilds()) {
        for (const TextLanguage& language : build.text_languages) {
            if (language.key == key) {
                return language.name;
            }
        }
    }
    return key;
}

} // namespace

bool IsRussianText(std::span<const uint8_t> sheet) {
    size_t latin = 0, cyrillic = 0, other_cyrillic = 0, yery = 0;
    for (size_t i = 0; i < sheet.size(); ++i) {
        const uint8_t b = sheet[i];
        if (b < 0x80) {
            latin += (b | 0x20) >= 'a' && (b | 0x20) <= 'z';
        } else if (b >= 0xd0 && b <= 0xd3 && i + 1 < sheet.size() && (sheet[i + 1] & 0xc0) == 0x80) {
            const char32_t c = (char32_t(b & 0x1f) << 6) | (sheet[++i] & 0x3f);
            ++cyrillic;
            other_cyrillic += !InRussianAlphabet(c);
            yery += c == 0x42b || c == 0x44b;
        }
    }
    return cyrillic > latin && yery > 0 && other_cyrillic * 100 < cyrillic;
}

std::vector<TextLanguage> InstalledTextLanguages(const std::filesystem::path& app0, const GameBuild* build) {
    const std::span<const TextLanguage> listed = TextLanguagesOf(build);
    const std::vector<TextLanguage> fallback(listed.begin(), listed.end());
    CpkArchive archive;
    const bool slotted = std::none_of(listed.begin(), listed.end(), [](const TextLanguage& l) { return l.suffix.empty(); });
    if (!build || !build->regional_archive || !slotted || app0.empty() || !archive.Open(app0 / build->regional_archive)) {
        return fallback;
    }
    std::vector<std::set<std::string>> files;
    std::map<std::string, size_t> holders;
    size_t present = 0;
    for (const TextLanguage& language : listed) {
        files.push_back(FilesOf(archive, language.suffix));
        present += !files.back().empty();
        for (const std::string& file : files.back()) {
            ++holders[file];
        }
    }
    // The US slot alone has msgsheet/StaffRollMsg_PS4_US.fms, so a file one language has is not one every language needs.
    std::set<std::string> required;
    for (const auto& [file, count] : holders) {
        if (count * 2 >= present) {
            required.insert(file);
        }
    }
    std::vector<TextLanguage> installed;
    for (size_t i = 0; i < listed.size(); ++i) {
        if (files[i].empty() || !std::includes(files[i].begin(), files[i].end(), required.begin(), required.end())) {
            continue;
        }
        TextLanguage language = listed[i];
        const CpkEntry* sheet = archive.Find("msgsheet/SystemMsg_" + std::string(language.suffix) + ".fms");
        if (sheet && IsRussianText(archive.Read(*sheet))) {
            if (std::any_of(installed.begin(), installed.end(),
                            [](const TextLanguage& other) { return other.key == kRussianKey; })) {
                continue;
            }
            language.key = kRussianKey;
            language.name = kRussianName;
        }
        installed.push_back(language);
    }
    return installed.empty() ? fallback : installed;
}

bool SameLanguages(std::span<const TextLanguage> a, std::span<const TextLanguage> b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const TextLanguage& x, const TextLanguage& y) {
        return x.key == y.key && x.ps4_id == y.ps4_id;
    });
}

std::string LanguageNames(std::span<const TextLanguage> languages) {
    std::string names;
    for (const TextLanguage& language : languages) {
        names += (names.empty() ? "" : ", ") + std::string(language.name);
    }
    return names;
}

std::string UnavailableLanguageNote(std::string_view saved, const TextLanguage& used) {
    if (saved.empty() || saved == used.key) {
        return {};
    }
    return "saved language " + std::string(SavedLanguageName(saved)) + " unavailable for this build, using " +
           std::string(used.name);
}

} // namespace dcp
