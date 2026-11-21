// The files in the installed game that Japanese NPC Voices needs: the sound table and the Japanese voice banks the
// hooks keep loaded. All of them live in DRACRO.CPK, which an update leaves as the base's unless it replaces entries.
#pragma once

#include <filesystem>
#include <span>
#include <string_view>

#include "game_profile.h"

namespace dcp {

std::span<const std::string_view> NpcVoiceResourceNames();

// True when DRACRO.CPK of the game folder holds every one of them.
bool NpcVoiceResourcesPresent(const std::filesystem::path& app0);

// What the launcher offers and the loader installs: the build supports the toggle and, when its hooks are found by
// signature, the installed archive holds the banks.
bool JapaneseNpcVoicesOffered(const std::filesystem::path& app0, const GameBuild* build);

} // namespace dcp
