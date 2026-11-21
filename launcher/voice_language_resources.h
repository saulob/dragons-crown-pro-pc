// The files of the installed game that Voice Language needs: the sound table and, for each voice category, every bank
// of the chosen language. All of them live in DRACRO.CPK, which an update leaves as the base's unless it replaces entries.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "game_profile.h"

namespace dcp {

// The banks of `category` (DcpVoiceCategory) in `language` (DCP_VOICE_ENGLISH or DCP_VOICE_JAPANESE).
std::vector<std::string> VoiceLanguageBankNames(int category, int language);

// The DCP_VOICE_CATEGORY_BIT categories whose banks in `language` DRACRO.CPK of the game folder holds; none without
// the sound table, the archive or a language to route to.
unsigned VoiceLanguageCategories(const std::filesystem::path& app0, int language);

// The languages the launcher offers besides Game Default: those the build supports and the archive holds banks for.
std::vector<int> VoiceLanguagesOffered(const std::filesystem::path& app0, const GameBuild* build);

} // namespace dcp
