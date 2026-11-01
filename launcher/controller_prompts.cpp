#include "controller_prompts.h"

#include <algorithm>
#include <system_error>

#include "utf8.h"

namespace dcp {
namespace {

constexpr const char* kSharedArchive = "DRACRO.CPK";

bool IsPromptEntry(std::string_view name) {
    return name.starts_with("stage/Interface") || name.starts_with("msgsheet/SystemMsg");
}

bool IsButtonAtlas(std::string_view name) {
    return name.starts_with("stage/Interface") && name.ends_with(".ftx");
}

// stage/Interface_US.ftx and msgsheet/SystemMsg_DE.fms are a language's copy; stage/Interface.ftx is the shared one.
bool IsLanguagePack(std::string_view name) {
    const size_t file = name.rfind('/') + 1;
    return IsPromptEntry(name) && name.find('_', file) != std::string_view::npos;
}

// An archive the game reads button atlases from. One that cannot be read is kept, so the fit check names it.
bool IsPromptArchive(const std::filesystem::path& archive) {
    CpkArchive cpk;
    if (!cpk.Open(archive)) {
        return true;
    }
    return std::any_of(cpk.entries().begin(), cpk.entries().end(),
                       [](const CpkEntry& entry) { return IsButtonAtlas(entry.path); });
}

bool HoldsPromptTargets(const std::filesystem::path& archive, const BlockPatch& patch) {
    CpkArchive cpk;
    if (!cpk.Open(archive)) {
        return false;
    }
    bool found = false;
    for (const BlockPatchTarget& target : patch.targets) {
        const CpkEntry* entry = cpk.Find(target.name);
        if (!entry) {
            continue;
        }
        const std::vector<uint8_t> data = cpk.Read(*entry);
        if (data.size() != target.size || Crc32(data) != target.crc_after) {
            return false;
        }
        found = true;
    }
    return found;
}

bool HasStalePromptEntry(const std::filesystem::path& archive, const BlockPatch& patch) {
    for (const std::string& entry : PatchedEntries(archive)) {
        if (!IsPromptEntry(entry)) {
            continue;
        }
        if (std::none_of(patch.targets.begin(), patch.targets.end(),
                         [&](const BlockPatchTarget& target) { return target.name == entry; })) {
            return true;
        }
    }
    return false;
}

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
    if (!build.xbox_prompts) {
        return out;
    }
    std::error_code ec;
    for (const char* name : {build.regional_archive, kSharedArchive}) {
        if (!name) {
            continue;
        }
        const std::filesystem::path archive = app0 / name;
        if (std::filesystem::is_regular_file(archive, ec) && IsPromptArchive(archive)) {
            out.push_back(archive);
        }
    }
    return out;
}

bool PromptPatchFits(const std::filesystem::path& app0, const GameBuild& build, const BlockPatch& patch,
                     std::string* detail) {
    const auto refuse = [&](std::string why) {
        if (detail) {
            *detail = std::move(why);
        }
        return false;
    };
    struct Opened {
        CpkArchive archive;
        std::vector<std::string> earlier; // entries an earlier patch rewrote; reverting puts their original back first
    };
    std::vector<Opened> archives;
    for (const std::filesystem::path& file : PromptArchives(app0, build)) {
        Opened& opened = archives.emplace_back();
        if (!opened.archive.Open(file)) {
            return refuse(Utf8(file.filename()) + " cannot be read");
        }
        opened.earlier = PatchedEntries(file);
    }
    if (archives.empty()) {
        return refuse("no prompt archives");
    }
    const auto is_target = [&](std::string_view name) {
        return std::any_of(patch.targets.begin(), patch.targets.end(),
                           [&](const BlockPatchTarget& target) { return target.name == name; });
    };
    bool language_packs = false;
    for (const Opened& opened : archives) {
        language_packs = language_packs || opened.archive.file().filename() != kSharedArchive;
        for (const CpkEntry& entry : opened.archive.entries()) {
            if (IsButtonAtlas(entry.path) && !is_target(entry.path)) {
                return refuse(entry.path + " in " + Utf8(opened.archive.file().filename()) +
                              " is not covered by the patch");
            }
        }
    }
    for (const BlockPatchTarget& target : patch.targets) {
        bool found = false;
        for (const Opened& opened : archives) {
            const CpkEntry* entry = opened.archive.Find(target.name);
            if (!entry) {
                continue;
            }
            found = true;
            const std::vector<uint8_t> data = opened.archive.Read(*entry);
            const uint32_t crc = Crc32(data);
            if (data.size() == target.size && (crc == target.crc_before || crc == target.crc_after)) {
                continue;
            }
            if (std::find(opened.earlier.begin(), opened.earlier.end(), entry->path) == opened.earlier.end()) {
                return refuse(target.name + " in " + Utf8(opened.archive.file().filename()) +
                              " is not the file the patch was made for");
            }
        }
        if (!found && (language_packs || !IsLanguagePack(target.name))) {
            return refuse(target.name + " is in none of the archives");
        }
    }
    return true;
}

bool XboxPromptsAvailable(const std::filesystem::path& app0, const GameBuild& build, std::span<const uint8_t> xbox_patch) {
    if (!build.xbox_prompts) {
        return false;
    }
    if (!BaseBuildOf(build)) {
        return true;
    }
    const std::optional<BlockPatch> patch = ParseBlockPatch(xbox_patch);
    return patch && PromptPatchFits(app0, build, *patch);
}

PromptStyle InstalledPromptStyle(const std::filesystem::path& app0, const GameBuild& build,
                                 std::span<const uint8_t> xbox_patch) {
    const std::vector<std::filesystem::path> archives = PromptArchives(app0, build);
    const std::optional<BlockPatch> patch = ParseBlockPatch(xbox_patch);
    if (archives.empty() || !patch) {
        return PromptStyle::kPlayStation;
    }
    for (const std::filesystem::path& archive : archives) {
        if (ArchiveState(archive) != ArchivePatchState::kPatched || !HoldsPromptTargets(archive, *patch) ||
            HasStalePromptEntry(archive, *patch)) {
            return PromptStyle::kPlayStation;
        }
    }
    return PromptStyle::kXbox;
}

PromptsResult SetPromptStyle(const std::filesystem::path& app0, const GameBuild& build, PromptStyle style,
                             std::span<const uint8_t> xbox_patch, const PatchHooks& hooks) {
    PromptsResult result;
    if (style == PromptStyle::kXbox && !XboxPromptsAvailable(app0, build, xbox_patch)) {
        result.detail = std::string("Xbox prompts are not available for ") + build.label;
        return result;
    }
    if (!build.xbox_prompts) {
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
