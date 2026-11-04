// The menu and subtitle languages of an installed game, read from its regional archive.
#pragma once

#include <cstdint>
#include <filesystem>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "game_profile.h"

namespace dcp {

class CpkArchive;

// The build's languages whose files the regional archive in `app0` holds completely (every file at least half of the
// build's languages there have). The build's own list when its languages are not archive slots, the archive cannot be
// read or it holds no complete language.
std::vector<TextLanguage> InstalledTextLanguages(const std::filesystem::path& app0, const GameBuild* build);

// The suffixes of the archive's language slots: the build's languages and every other suffix whose set of files is at
// least half the size of the largest set (the UK slot, not the single EU file).
std::set<std::string> LanguageSlotSuffixes(const CpkArchive& archive, const GameBuild* build);
// True for a file of one of the slots, named by suffix (stage/Menu_FR.ftx) or prefix (other/DE_manual_001.ftx).
bool InLanguageSlot(std::string_view path, const std::set<std::string>& slots);

// One log line on the language slots of the build's regional archive in `app0`: entry count, files per slot and, per
// slot, the SystemMsg sheet's checksum, size and non-ASCII byte count. Never text. Empty when the build's languages are
// not archive slots.
std::string DescribeLanguageResources(const std::filesystem::path& app0, const GameBuild* build);

bool SameLanguages(std::span<const TextLanguage> a, std::span<const TextLanguage> b);
// "English, French": the names for the log.
std::string LanguageNames(std::span<const TextLanguage> languages);
// "saved language French unavailable for this build, using Japanese"; empty when the saved key is empty or `used`.
std::string UnavailableLanguageNote(std::string_view saved, const TextLanguage& used);

} // namespace dcp
