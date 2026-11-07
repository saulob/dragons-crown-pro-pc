#include "text_languages.h"

#include <algorithm>
#include <cstdio>
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

bool IsUpper(char c) {
    return c >= 'A' && c <= 'Z';
}

// The two-letter slot suffix a file name ends with (stage/Menu_FR.ftx is FR), empty when it has none.
std::string_view SuffixOf(std::string_view path) {
    const size_t dot = path.rfind('.');
    const size_t slash = path.rfind('/');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
        return {};
    }
    const std::string_view stem = path.substr(0, dot);
    const size_t start = slash == std::string_view::npos ? 0 : slash + 1;
    if (stem.size() < start + 4 || stem[stem.size() - 3] != '_' || !IsUpper(stem[stem.size() - 2]) ||
        !IsUpper(stem[stem.size() - 1])) {
        return {};
    }
    return stem.substr(stem.size() - 2);
}

// The two-letter slot prefix a file name starts with (other/US_manual_001.ftx is US), empty when it has none.
std::string_view PrefixOf(std::string_view path) {
    const std::string_view name = path.substr(path.rfind('/') == std::string_view::npos ? 0 : path.rfind('/') + 1);
    return name.size() > 3 && IsUpper(name[0]) && IsUpper(name[1]) && name[2] == '_' ? name.substr(0, 2) : std::string_view{};
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

bool IsSlotted(std::span<const TextLanguage> languages) {
    return std::none_of(languages.begin(), languages.end(), [](const TextLanguage& l) { return l.suffix.empty(); });
}

std::string_view SavedLanguageName(std::string_view key) {
    for (const GameBuild& build : SupportedBuilds()) {
        for (const TextLanguage& language : TextLanguagesOf(&build)) {
            if (language.key == key) {
                return language.name;
            }
        }
    }
    return key;
}

} // namespace

std::vector<TextLanguage> InstalledTextLanguages(const std::filesystem::path& app0, const GameBuild* build) {
    const std::span<const TextLanguage> listed = TextLanguagesOf(build);
    const std::vector<TextLanguage> fallback(listed.begin(), listed.end());
    CpkArchive archive;
    if (!build || !build->regional_archive || !IsSlotted(listed) || app0.empty() ||
        !archive.Open(app0 / build->regional_archive)) {
        return fallback;
    }
    std::map<std::string_view, std::set<std::string>> files;
    std::map<std::string, size_t> holders;
    size_t present = 0;
    for (const TextLanguage& language : listed) {
        if (!language.translation.empty()) {
            continue;
        }
        const std::set<std::string>& slot = files[language.suffix] = FilesOf(archive, language.suffix);
        present += !slot.empty();
        for (const std::string& file : slot) {
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
    for (const TextLanguage& language : listed) {
        const std::set<std::string>& slot = files[language.suffix];
        if (slot.empty() || !std::includes(slot.begin(), slot.end(), required.begin(), required.end())) {
            continue;
        }
        installed.push_back(language);
    }
    return installed.empty() ? fallback : installed;
}

std::set<std::string> LanguageSlotSuffixes(const CpkArchive& archive, const GameBuild* build) {
    std::map<std::string, std::set<std::string>> files;
    size_t largest = 0;
    for (const CpkEntry& entry : archive.entries()) {
        if (const std::string_view suffix = SuffixOf(entry.path); !suffix.empty()) {
            std::set<std::string>& slot = files[std::string(suffix)];
            slot.insert(Unsuffixed(entry.path, suffix));
            largest = std::max(largest, slot.size());
        }
    }
    std::set<std::string> slots;
    for (const auto& [suffix, slot] : files) {
        if (slot.size() * 2 >= largest) {
            slots.insert(suffix);
        }
    }
    for (const TextLanguage& language : TextLanguagesOf(build)) {
        if (!language.suffix.empty()) {
            slots.insert(std::string(language.suffix));
        }
    }
    return slots;
}

bool InLanguageSlot(std::string_view path, const std::set<std::string>& slots) {
    const auto in_slot = [&](std::string_view suffix) { return !suffix.empty() && slots.count(std::string(suffix)) != 0; };
    return in_slot(SuffixOf(path)) || in_slot(PrefixOf(path));
}

std::string DescribeLanguageResources(const std::filesystem::path& app0, const GameBuild* build) {
    const std::span<const TextLanguage> listed = TextLanguagesOf(build);
    if (!build || !build->regional_archive || !IsSlotted(listed)) {
        return {};
    }
    std::string text = build->regional_archive;
    std::error_code ec;
    if (!std::filesystem::exists(app0 / build->regional_archive, ec)) {
        return text + " absent";
    }
    CpkArchive archive;
    if (!archive.Open(app0 / build->regional_archive)) {
        return text + " unreadable";
    }
    text += ": " + std::to_string(archive.entries().size()) + " entries";
    for (const TextLanguage& language : listed) {
        if (!language.translation.empty()) {
            continue;
        }
        text += "; " + std::string(language.suffix) + " " + std::to_string(FilesOf(archive, language.suffix).size()) + " files";
        const CpkEntry* sheet = archive.Find("msgsheet/SystemMsg_" + std::string(language.suffix) + ".fms");
        if (!sheet) {
            continue;
        }
        const std::vector<uint8_t> bytes = archive.Read(*sheet);
        char crc[16];
        std::snprintf(crc, sizeof(crc), "%08x", Crc32(bytes));
        const size_t high = size_t(std::count_if(bytes.begin(), bytes.end(), [](uint8_t b) { return b >= 0x80; }));
        text += std::string(", SystemMsg crc ") + crc + ", " + std::to_string(bytes.size()) + " bytes, " +
                std::to_string(high) + " non-ASCII";
    }
    return text + "; offered " + LanguageNames(InstalledTextLanguages(app0, build));
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
