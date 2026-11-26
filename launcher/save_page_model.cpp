#include "save_page_model.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <system_error>

#include "editor_model.h"
#include "file_browser.h"

namespace dcp {
namespace {

bool IsProTitle(std::string_view title_id) {
    return std::find(std::begin(kProTitleIds), std::end(kProTitleIds), title_id) != std::end(kProTitleIds);
}

bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool SameFile(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    return std::filesystem::exists(a, ec) && std::filesystem::exists(b, ec) && std::filesystem::equivalent(a, b, ec);
}

bool Inside(const std::filesystem::path& path, const std::filesystem::path& folder) {
    std::error_code ec;
    const std::filesystem::path child = std::filesystem::weakly_canonical(path, ec);
    if (ec) return false;
    const std::filesystem::path parent = std::filesystem::weakly_canonical(folder, ec);
    if (ec) return false;
    const std::filesystem::path relative = child.lexically_relative(parent);
    return !relative.empty() && *relative.begin() != "..";
}

constexpr const char* kStaleDestination = "the selected save changed on disk since it was loaded";

// The destination must still be the valid save the page loaded: an invalid or changed one is never overwritten from here.
std::optional<std::string> DestinationProblem(const DiscoveredSave& destination) {
    const SaveModel now = SaveModel::FromFile(destination.save0_path);
    if (ClassifySave(now) != SaveFileStatus::kValid) return "the selected save is no longer valid";
    if (!std::ranges::equal(now.bytes(), destination.model.bytes())) return kStaleDestination;
    return std::nullopt;
}

} // namespace

std::vector<DiscoveredSave> DiscoverProSaves(const std::filesystem::path& user_dir, std::string_view active_title_id) {
    std::vector<DiscoveredSave> saves = DiscoverSaves(user_dir, active_title_id);
    std::erase_if(saves, [](const DiscoveredSave& save) { return !IsProTitle(save.title_id); });
    return saves;
}

std::string SaveIdentity(const DiscoveredSave& save) {
    std::string text = save.title_id + " / " + save.dir_name;
    if (!save.region.empty()) text += " (" + save.region + ")";
    return text;
}

SaveFileStatus ClassifySave(const SaveModel& model) {
    if (model.valid()) return SaveFileStatus::kValid;
    const std::span<const uint8_t> bytes = model.bytes();
    if (bytes.empty()) return SaveFileStatus::kUnreadable;
    if (bytes.size() != kSaveFileSize) return SaveFileStatus::kWrongSize;
    const uint32_t magic = uint32_t(bytes[kSaveMagicOffset]) | uint32_t(bytes[kSaveMagicOffset + 1]) << 8 |
                           uint32_t(bytes[kSaveMagicOffset + 2]) << 16 | uint32_t(bytes[kSaveMagicOffset + 3]) << 24;
    if (magic != kSaveMagic) return SaveFileStatus::kNotDragonsCrown;
    return SaveFileStatus::kBadCrc;
}

const char* SaveStatusText(SaveFileStatus status) {
    switch (status) {
    case SaveFileStatus::kValid: return "Valid";
    case SaveFileStatus::kUnreadable: return "Unreadable";
    case SaveFileStatus::kWrongSize: return "Invalid: wrong file size";
    case SaveFileStatus::kNotDragonsCrown: return "Invalid: not a Dragon's Crown save";
    case SaveFileStatus::kBadCrc: return "Corrupt: CRC mismatch";
    }
    return "Unreadable";
}

const char* CrcStatusText(SaveFileStatus status) {
    switch (status) {
    case SaveFileStatus::kValid: return "Valid";
    case SaveFileStatus::kBadCrc: return "Invalid";
    default: return "Not checked";
    }
}

const char* IntegrityStatusText(std::optional<uint32_t> flags) {
    if (!flags) return "Integrity status unavailable";
    switch (*flags) {
    case 0: return "Clean save, eligible for clean shared online";
    case 1: return "Gameplay cheats were previously used with this save";
    case 2: return "Gameplay Editor changes were previously applied to this save";
    default: return "Gameplay cheats and Editor changes were previously used with this save";
    }
}

SaveOverview DescribeOverview(const DiscoveredSave& save) {
    SaveOverview overview;
    const TitleInfo* info = ProTitleInfo(save.title_id);
    overview.platform = info ? std::string(info->platform) : std::string("Unknown");
    overview.region = save.region.empty() ? std::string("Unknown") : save.region;
    overview.title_id = save.title_id;
    overview.save_name = save.dir_name;
    overview.status = ClassifySave(save.model);
    overview.modified = ModifiedTimeText(save.modified);
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(save.save0_path, ec);
    overview.size = ec ? std::string("Unknown") : FormatSize(size);
    overview.current = save.current;
    for (const SlotOverview& slot : DescribeSlots(save.model)) {
        overview.populated_slots += slot.empty() ? 0 : 1;
        overview.characters += int(slot.characters.size());
    }
    return overview;
}

std::vector<SlotOverview> DescribeSlots(const SaveModel& model) {
    std::vector<SlotOverview> slots;
    if (!model.valid()) return slots;
    for (size_t index = 0; index < kDataSlotCount; ++index) {
        const DataSlotView& view = model.slots()[index];
        SlotOverview slot;
        slot.name = DataSlotName(int(index));
        slot.gold = view.gold;
        for (const CharacterView& character : view.characters) {
            if (!character.present) continue;
            slot.characters.push_back({character.name, character.klass, SaveModel::DisplayedLevel(character.level_stored)});
        }
        slots.push_back(std::move(slot));
    }
    return slots;
}

std::string BackupTimeText(std::string_view timestamp) {
    const auto digits = [&](size_t from, size_t count) {
        return timestamp.size() >= from + count &&
               std::all_of(timestamp.begin() + from, timestamp.begin() + from + count, [](char ch) { return ch >= '0' && ch <= '9'; });
    };
    if (!digits(0, 8) || timestamp.size() < 15 || timestamp[8] != '-' || !digits(9, 6)) return std::string(timestamp);
    const std::string_view t = timestamp;
    return std::string(t.substr(0, 4)) + "-" + std::string(t.substr(4, 2)) + "-" + std::string(t.substr(6, 2)) + " " +
           std::string(t.substr(9, 2)) + ":" + std::string(t.substr(11, 2)) + ":" + std::string(t.substr(13, 2));
}

namespace {

BackupRow RowOf(const BackupInfo& backup) {
    BackupRow row;
    row.info = backup;
    row.when = BackupTimeText(backup.timestamp);
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(backup.file, ec);
    row.size = ec ? std::string("Unknown") : FormatSize(size);
    return row;
}

void NumberRepeatedTimes(std::vector<BackupRow>& rows) {
    std::map<std::string, int> seen;
    for (BackupRow& row : rows) {
        if (const int count = ++seen[row.when]; count > 1) row.when += " (" + std::to_string(count) + ")";
    }
}

bool BelongsTo(const BackupInfo& backup, const DiscoveredSave& save) {
    return backup.title_id == save.title_id && backup.dir_name == save.dir_name;
}

} // namespace

std::vector<BackupRow> BackupsOfSave(std::span<const BackupInfo> backups, const DiscoveredSave& save) {
    std::vector<BackupRow> rows;
    for (const BackupInfo& backup : backups) {
        if (BelongsTo(backup, save)) rows.push_back(RowOf(backup));
    }
    NumberRepeatedTimes(rows);
    return rows;
}

BackupListing LatestBackups(std::span<const BackupInfo> backups, const DiscoveredSave& save) {
    BackupListing listing;
    for (const BackupInfo& backup : backups) {
        if (!BelongsTo(backup, save)) continue;
        if (listing.shown.size() < kBackupDisplayLimit) listing.shown.push_back(RowOf(backup));
        ++listing.total;
    }
    NumberRepeatedTimes(listing.shown);
    return listing;
}

ActionState ActionStateFor(SaveAction action, const DiscoveredSave* save, const SaveGuards& guards) {
    if (!save) return {false, "No save is selected."};
    if (guards.game_active) return {false, "The game is starting. Saves cannot be changed now."};
    if (ClassifySave(save->model) != SaveFileStatus::kValid) {
        return {false, "This save is not valid, so it cannot be backed up, exported or replaced from here."};
    }
    const bool replaces = action == SaveAction::kImport || action == SaveAction::kRestore;
    if (replaces && guards.editor_dirty) {
        return {false, "The Editor has unapplied changes. Apply or discard them on the Editor page before you import or restore a save."};
    }
    return {true, ""};
}

SaveOpResult BackupSave(const DiscoveredSave& save, const std::filesystem::path& backup_folder) {
    SaveOpResult result;
    std::vector<uint8_t> bytes;
    std::vector<std::string> problems;
    if (!ReadFileBytes(save.save0_path, bytes) || !ValidateSave(bytes, problems)) {
        result.error = "the save is not valid";
        return result;
    }
    result.backup = CreateBackup(backup_folder, save.save0_path, save.title_id, save.dir_name);
    if (!result.backup) {
        result.error = "could not create the backup";
        return result;
    }
    std::vector<uint8_t> copy;
    if (!ReadFileBytes(result.backup->file, copy) || copy != bytes) {
        result.error = "the backup did not read back identically";
        return result;
    }
    result.ok = true;
    return result;
}

ExportPlan PlanExport(const DiscoveredSave& save, const std::filesystem::path& folder, const std::filesystem::path& user_dir) {
    ExportPlan plan;
    std::error_code ec;
    if (folder.empty() || !std::filesystem::is_directory(folder, ec)) {
        plan.refused = "Choose an existing folder.";
        return plan;
    }
    plan.destination = folder / "SAVE0.DAT";
    if (SameFile(plan.destination, save.save0_path)) {
        plan.refused = "That is the selected save itself. Choose another folder.";
        return plan;
    }
    if (Inside(folder, user_dir / std::string(kSavedataFolder))) {
        plan.refused = "Exporting into the launcher's savedata folder would replace a save without a backup. Use Import instead.";
        return plan;
    }
    if (std::filesystem::is_directory(plan.destination, ec)) {
        plan.refused = "That folder holds a folder named SAVE0.DAT. Choose another folder.";
        return plan;
    }
    plan.exists = std::filesystem::exists(plan.destination, ec);
    return plan;
}

SaveOpResult ExportSave(const DiscoveredSave& save, const ExportPlan& plan) {
    SaveOpResult result;
    if (!plan.refused.empty() || plan.destination.empty()) {
        result.error = "the export destination was refused";
        return result;
    }
    std::vector<uint8_t> bytes;
    std::vector<std::string> problems;
    if (!ReadFileBytes(save.save0_path, bytes) || !ValidateSave(bytes, problems)) {
        result.error = "the save is not valid";
        return result;
    }
    if (std::optional<std::string> error = WriteSaveAtomically(plan.destination, bytes, ValidateSave)) {
        result.error = *error;
        return result;
    }
    std::vector<uint8_t> copy;
    problems.clear();
    if (!ReadFileBytes(plan.destination, copy) || copy != bytes || !ValidateSave(copy, problems)) {
        result.error = "the exported copy did not read back identically";
        return result;
    }
    result.ok = true;
    return result;
}

namespace {

const char* WrongModeReason(const ImportResult& found, ImportMode mode) {
    const bool ps3 = found.platform == "PS3";
    if (mode == ImportMode::kPs4) return ps3 ? "This is a PS3 save. Use Import PS3 save." : "";
    if (ps3 || found.block != ImportBlock::kNone) return "";
    return found.platform == "Vita" ? "This is a PS Vita save, not a PS3 save. Use Import PS4 save."
                                    : "This is a Dragon's Crown Pro save. Use Import PS4 save.";
}

} // namespace

ImportPlan PlanImport(const std::filesystem::path& source, const DiscoveredSave& destination, ImportMode mode) {
    ImportPlan plan;
    plan.mode = mode;
    plan.inspected = InspectImport(source);
    plan.destination = destination.save0_path;
    plan.destination_identity = SaveIdentity(destination);
    plan.unverified_platform = plan.inspected.platform == "Vita";
    if (const std::string wrong = WrongModeReason(plan.inspected, mode); !wrong.empty()) {
        plan.refused = wrong;
    } else if (plan.inspected.block != ImportBlock::kNone || !plan.inspected.valid) {
        plan.refused = ImportBlockText(plan.inspected.block);
        if (plan.refused.empty()) plan.refused = "This is not a valid Dragon's Crown Pro SAVE0.DAT.";
        const ImportBlock block = plan.inspected.block;
        const bool detailed = block == ImportBlock::kInvalidSave || block == ImportBlock::kPs3PfdInvalid ||
                              block == ImportBlock::kPs3Undecryptable;
        if (detailed && !plan.inspected.errors.empty()) {
            plan.refused += " (" + plan.inspected.errors.front() + ")";
        }
    } else if (SameFile(plan.inspected.file, destination.save0_path)) {
        plan.refused = "the source is the selected save itself";
    } else if (std::ranges::equal(plan.inspected.bytes, destination.model.bytes())) {
        plan.refused = "the source is identical to the selected save";
    }
    return plan;
}

SaveOpResult CommitImportPlan(const ImportPlan& plan, const DiscoveredSave& destination,
                              const std::filesystem::path& backup_folder) {
    SaveOpResult result;
    if (!plan.refused.empty() || plan.destination != destination.save0_path) {
        result.error = plan.refused.empty() ? "the import was planned for another save" : plan.refused;
        return result;
    }
    if (std::optional<std::string> problem = DestinationProblem(destination)) {
        result.error = *problem;
        result.stale = *problem == kStaleDestination;
        return result;
    }
    if (std::optional<std::string> error =
            CommitImport(plan.inspected, destination.save0_path, backup_folder, destination.title_id, destination.dir_name)) {
        result.error = *error;
        return result;
    }
    result.ok = true;
    return result;
}

SaveOpResult DeleteSaveBackup(const BackupInfo& backup, const DiscoveredSave& save, const std::filesystem::path& backup_folder) {
    SaveOpResult result;
    if (std::optional<std::string> error = DeleteBackup(backup, backup_folder, save.title_id, save.dir_name)) {
        result.error = *error;
        return result;
    }
    result.ok = true;
    return result;
}

SaveOpResult RestoreToSave(const BackupInfo& backup, const DiscoveredSave& destination,
                           const std::filesystem::path& backup_folder) {
    SaveOpResult result;
    if (std::optional<std::string> problem = DestinationProblem(destination)) {
        result.error = *problem;
        result.stale = *problem == kStaleDestination;
        return result;
    }
    if (std::optional<std::string> error =
            RestoreBackup(backup, destination.save0_path, backup_folder, destination.title_id, destination.dir_name)) {
        result.error = *error;
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace dcp
