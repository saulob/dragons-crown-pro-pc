// The atlas signature table of Character Highlight, written beside the boot image from the game's own archive.
#pragma once

#include <filesystem>
#include <string>

namespace dcp {

// Writes out/atlas-signatures.txt from the player atlases of game/DRACRO.CPK. Empty on success and when the game has no
// archive (then no file is left); else the reason, with the previous table untouched.
std::string BuildAtlasSignatures(const std::filesystem::path& game, const std::filesystem::path& out);

} // namespace dcp
