// The files in the installed game that Japanese NPC Voices needs: the sound table and the Japanese voice banks the
// hooks keep loaded. All of them live in DRACRO.CPK, which an update leaves as the base's unless it replaces entries.
#pragma once

#include <filesystem>
#include <span>
#include <string_view>

namespace dcp {

std::span<const std::string_view> NpcVoiceResourceNames();

// True when DRACRO.CPK of the game folder holds every one of them.
bool NpcVoiceResourcesPresent(const std::filesystem::path& app0);

} // namespace dcp
