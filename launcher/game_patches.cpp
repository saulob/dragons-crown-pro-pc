#include "game_patches.h"

#include <algorithm>
#include <system_error>

#include "utf8.h"

namespace dcp {
namespace {

constexpr char kSharedArchive[] = "DRACRO.CPK";

std::optional<BlockPatch> ReadPatch(std::span<const uint8_t> bytes, const char* what, std::string& detail) {
    std::string error;
    std::optional<BlockPatch> patch = ParseBlockPatch(bytes, &error);
    if (!patch) {
        detail = std::string("the ") + what + " patch is unreadable: " + error;
    }
    return patch;
}

void Append(BlockPatch& into, const std::optional<BlockPatch>& from) {
    if (from) {
        into.targets.insert(into.targets.end(), from->targets.begin(), from->targets.end());
    }
}

bool Holds(const std::vector<std::string>& names, std::string_view name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

} // namespace

bool InstalledSafeMode(const std::filesystem::path& app0) {
    return Holds(PatchedEntries(app0 / kSharedArchive), kSpiderEntry);
}

PromptsResult SetGamePatches(const std::filesystem::path& app0, const GameBuild& build, PromptStyle style,
                             bool safe_mode, std::span<const uint8_t> xbox_patch, std::span<const uint8_t> spider_patch,
                             const PatchHooks& hooks) {
    PromptsResult result;
    const bool xbox = style == PromptStyle::kXbox;
    if (xbox && !build.xbox_prompts) {
        result.detail = std::string("Xbox prompts are not validated for ") + build.label;
        return result;
    }
    std::optional<BlockPatch> xbox_targets, spider_targets;
    if (xbox && !(xbox_targets = ReadPatch(xbox_patch, "Xbox prompts", result.detail))) {
        return result;
    }
    if (safe_mode && !(spider_targets = ReadPatch(spider_patch, "Arachnophobia Safe Mode", result.detail))) {
        return result;
    }

    std::vector<std::filesystem::path> archives = PromptArchives(app0, build);
    const std::filesystem::path shared = app0 / kSharedArchive;
    std::error_code ec;
    const bool has_shared = std::filesystem::is_regular_file(shared, ec);
    if (has_shared && std::find(archives.begin(), archives.end(), shared) == archives.end()) {
        archives.push_back(shared);
    }
    if (archives.empty()) {
        result.detail = "no game archives in " + Utf8(app0);
        return result;
    }

    const std::vector<std::filesystem::path> prompt_archives = PromptArchives(app0, build);
    for (const std::filesystem::path& archive : archives) {
        BlockPatch wanted;
        if (std::find(prompt_archives.begin(), prompt_archives.end(), archive) != prompt_archives.end()) {
            Append(wanted, xbox_targets);
        }
        if (archive == shared) {
            Append(wanted, spider_targets);
        }
        const std::vector<std::string> installed = PatchedEntries(archive);
        const bool leftover = std::any_of(installed.begin(), installed.end(), [&](const std::string& name) {
            return std::none_of(wanted.targets.begin(), wanted.targets.end(),
                                [&](const BlockPatchTarget& target) { return target.name == name; });
        });
        if (hooks.phase) {
            hooks.phase(((wanted.targets.empty() ? "Restoring " : "Patching ") + Utf8(archive.filename()) + "...").c_str());
        }
        const auto note = [&](const PatchResult& step) {
            result.detail += (result.detail.empty() ? "" : "; ") + step.detail;
            result.cancelled = step.cancelled;
            return step.ok;
        };
        if ((wanted.targets.empty() || leftover) && !note(RevertBlockPatch(archive))) {
            return result;
        }
        if (!wanted.targets.empty() && !note(ApplyBlockPatch(archive, wanted, hooks))) {
            return result;
        }
    }
    result.ok = true;
    return result;
}

} // namespace dcp
