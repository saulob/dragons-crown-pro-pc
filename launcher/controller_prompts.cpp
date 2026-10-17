#include "controller_prompts.h"

#include <system_error>

#include "utf8.h"

namespace dcp {
namespace {

constexpr const char* kSharedArchive = "DRACRO.CPK";

} // namespace

const char* PromptStyleKey(PromptStyle style) {
    return style == PromptStyle::kXbox ? kPromptsXbox : kPromptsPlayStation;
}

const char* PromptStyleLabel(PromptStyle style) {
    return style == PromptStyle::kXbox ? "Xbox" : "PlayStation (original)";
}

std::optional<PromptStyle> ParsePromptStyle(std::string_view key) {
    if (key == kPromptsPlayStation) {
        return PromptStyle::kPlayStation;
    }
    if (key == kPromptsXbox) {
        return PromptStyle::kXbox;
    }
    return std::nullopt;
}

std::vector<std::filesystem::path> PromptArchives(const std::filesystem::path& app0, const GameBuild& build) {
    std::vector<std::filesystem::path> out;
    if (!build.xbox_prompts || !build.regional_archive) {
        return out;
    }
    std::error_code ec;
    for (const char* name : {build.regional_archive, kSharedArchive}) {
        const std::filesystem::path archive = app0 / name;
        if (std::filesystem::is_regular_file(archive, ec)) {
            out.push_back(archive);
        }
    }
    return out;
}

PromptStyle InstalledPromptStyle(const std::filesystem::path& app0, const GameBuild& build) {
    const std::vector<std::filesystem::path> archives = PromptArchives(app0, build);
    if (archives.empty()) {
        return PromptStyle::kPlayStation;
    }
    for (const std::filesystem::path& archive : archives) {
        if (ArchiveState(archive) != ArchivePatchState::kPatched) {
            return PromptStyle::kPlayStation;
        }
    }
    return PromptStyle::kXbox;
}

PromptsResult SetPromptStyle(const std::filesystem::path& app0, const GameBuild& build, PromptStyle style,
                             std::span<const uint8_t> xbox_patch, const PatchHooks& hooks) {
    PromptsResult result;
    if (!build.xbox_prompts) {
        if (style == PromptStyle::kXbox) {
            result.detail = std::string("Xbox prompts are not validated for ") + build.label;
            return result;
        }
        result.ok = true;
        result.detail = "the game's own prompts";
        return result;
    }
    const std::vector<std::filesystem::path> archives = PromptArchives(app0, build);
    if (archives.empty()) {
        result.detail = "no game archives in " + Utf8(app0);
        return result;
    }
    std::optional<BlockPatch> patch;
    if (style == PromptStyle::kXbox) {
        std::string error;
        patch = ParseBlockPatch(xbox_patch, &error);
        if (!patch) {
            result.detail = "the Xbox prompts patch is unreadable: " + error;
            return result;
        }
    }
    for (const std::filesystem::path& archive : archives) {
        if (hooks.phase) {
            const std::string verb = style == PromptStyle::kXbox ? "Patching " : "Restoring ";
            hooks.phase((verb + Utf8(archive.filename()) + "...").c_str());
        }
        const PatchResult step =
            style == PromptStyle::kXbox ? ApplyBlockPatch(archive, *patch, hooks) : RevertBlockPatch(archive);
        result.detail += (result.detail.empty() ? "" : "; ") + step.detail;
        if (!step.ok) {
            result.cancelled = step.cancelled;
            return result;
        }
    }
    result.ok = true;
    return result;
}

} // namespace dcp
