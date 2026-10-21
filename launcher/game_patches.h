// The archive patches the launcher keeps in step with its settings: Xbox controller prompts (controller_prompts.h) and
// Arachnophobia Safe Mode, which replaces the Giant Spider's atlas (character/GiantSpider00.ftx in DRACRO.CPK) with
// a simplified creature. An archive has one undo record, so both patches are applied together, per archive.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

#include "controller_prompts.h"

namespace dcp {

inline constexpr char kSpiderEntry[] = "character/GiantSpider00.ftx";

// Whether DRACRO.CPK holds the Giant Spider replacement.
bool InstalledSafeMode(const std::filesystem::path& app0);

// Makes the archives show `style` and, when `safe_mode`, the simplified spider; whatever else an archive carries
// from an earlier choice is reverted first. The patches are only read for what is asked.
PromptsResult SetGamePatches(const std::filesystem::path& app0, const GameBuild& build, PromptStyle style,
                             bool safe_mode, std::span<const uint8_t> xbox_patch, std::span<const uint8_t> spider_patch,
                             const PatchHooks& hooks = {});

} // namespace dcp
