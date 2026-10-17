// The Controller prompts setting: which controller's buttons the game's prompts show. The PlayStation glyphs are the
// game's own; Xbox ones are painted over them in the button atlas (Interface_arranged000 in stage/Interface.ftx and
// stage/Interface_US.ftx) by a block patch applied to the installed archives (cpk_patch.h), so the choice is made in
// the game files, not at run time, and is reversible.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cpk_patch.h"
#include "game_profile.h"

namespace dcp {

enum class PromptStyle { kPlayStation, kXbox };

inline constexpr char kPromptsPlayStation[] = "playstation";
inline constexpr char kPromptsXbox[] = "xbox";

const char* PromptStyleKey(PromptStyle style);
const char* PromptStyleLabel(PromptStyle style);
std::optional<PromptStyle> ParsePromptStyle(std::string_view key);

// The archives the patch touches, among those present in the prepared game folder. Only a build whose Xbox patch is
// validated has any: the regional archive holds the button atlas the game reads, DRACRO.CPK is patched too so the
// choice holds either way.
std::vector<std::filesystem::path> PromptArchives(const std::filesystem::path& app0, const GameBuild& build);

// What the installed archives show: Xbox once every archive present is patched, PlayStation otherwise (a half-patched
// install reads as PlayStation; choosing Xbox again completes it, choosing PlayStation cleans it up).
PromptStyle InstalledPromptStyle(const std::filesystem::path& app0, const GameBuild& build);

struct PromptsResult {
    bool ok = false;
    bool cancelled = false;
    std::string detail; // for the log
};

// Patches or reverts every archive so the game shows `style`. `xbox_patch` is the DCPX patch (the embedded
// xbox-prompts.dcpx); it is only needed for kXbox. Progress and cancellation go through the hooks.
PromptsResult SetPromptStyle(const std::filesystem::path& app0, const GameBuild& build, PromptStyle style,
                             std::span<const uint8_t> xbox_patch, const PatchHooks& hooks = {});

} // namespace dcp
