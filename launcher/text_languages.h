// The menu and subtitle languages of an installed game, read from its regional archive.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "game_profile.h"

namespace dcp {

inline constexpr std::string_view kRussianKey = "russian";
inline constexpr std::string_view kRussianName = "Russian";

// True when most letters of a UTF-8 message sheet are Cyrillic, from the Russian alphabet and including its yery.
bool IsRussianText(std::span<const uint8_t> sheet);

// The build's languages whose files the regional archive in `app0` holds completely (every file at least half of the
// build's languages there have), a language whose SystemMsg sheet is Russian text offered as Russian with the same
// system language. The build's own list when its languages are not archive slots, the archive cannot be read or it
// holds no complete language.
std::vector<TextLanguage> InstalledTextLanguages(const std::filesystem::path& app0, const GameBuild* build);

bool SameLanguages(std::span<const TextLanguage> a, std::span<const TextLanguage> b);
// "English, French": the names for the log.
std::string LanguageNames(std::span<const TextLanguage> languages);
// "saved language French unavailable for this build, using Russian"; empty when the saved key is empty or `used`.
std::string UnavailableLanguageNote(std::string_view saved, const TextLanguage& used);

} // namespace dcp
