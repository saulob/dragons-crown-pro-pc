// What the Save page shows and does, as plain data and functions: the saves, their overview and DATA slots, the backups,
// and the backup, export, import and restore operations. No window needed.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "save_backup.h"
#include "save_data.h"
#include "save_discovery.h"
#include "save_import.h"

namespace dcp {

// The save chosen on the Save page, shared with the Editor. `generation` changes whenever the Save page replaced a save,
// so a page holding a copy of it reloads before it can write.
struct SaveSelection {
    std::filesystem::path save0;
    uint64_t generation = 0;
};

// Every save of a Pro title id, as DiscoverSaves finds them.
std::vector<DiscoveredSave> DiscoverProSaves(const std::filesystem::path& user_dir, std::string_view active_title_id);

// "CUSA10454 / SAVEDATA00 (USA)".
std::string SaveIdentity(const DiscoveredSave& save);

enum class SaveFileStatus { kValid, kUnreadable, kWrongSize, kNotDragonsCrown, kBadCrc };
SaveFileStatus ClassifySave(const SaveModel& model);
const char* SaveStatusText(SaveFileStatus status); // English; drawn through Tr
const char* CrcStatusText(SaveFileStatus status);

struct SaveOverview {
    std::string platform;
    std::string region;
    std::string title_id;
    std::string save_name;
    SaveFileStatus status = SaveFileStatus::kUnreadable;
    std::string modified;
    std::string size;
    int populated_slots = 0;
    int characters = 0;
    bool current = false; // the installed game's title id
};
SaveOverview DescribeOverview(const DiscoveredSave& save);

struct SlotCharacter {
    std::string name;
    CharacterClass klass = CharacterClass::kFighter;
    int level = 1;
};
struct SlotOverview {
    std::string name;
    uint32_t gold = 0;
    std::vector<SlotCharacter> characters;
    bool empty() const { return characters.empty(); }
};
// The three DATA slots of a valid save; nothing for an invalid one.
std::vector<SlotOverview> DescribeSlots(const SaveModel& model);

// "20261122-101010-123" -> "2026-11-22 10:10:10"; anything else is shown as stored.
std::string BackupTimeText(std::string_view timestamp);
struct BackupRow {
    BackupInfo info;
    std::string when;
    std::string size;
};
// The backups of one save (same title id and savedata folder), in the order given. Rows with the same time read "(2)",
// "(3)"... so each row has a name of its own.
std::vector<BackupRow> BackupsOfSave(std::span<const BackupInfo> backups, const DiscoveredSave& save);

struct SaveGuards {
    bool editor_dirty = false;
    bool game_active = false;
};
enum class SaveAction { kBackup, kExport, kImport, kRestore };

struct ActionState {
    bool enabled = false;
    const char* reason = ""; // English; empty when enabled
};
// Restore and Import replace the save, so they also wait for the Editor's pending changes.
ActionState ActionStateFor(SaveAction action, const DiscoveredSave* save, const SaveGuards& guards);

struct SaveOpResult {
    bool ok = false;
    bool stale = false; // the save changed on disk after it was loaded; reload before trying again
    std::string error;
    std::optional<BackupInfo> backup;
};

// Reads the save again, validates it and copies it to a new backup; the save itself is only read.
SaveOpResult BackupSave(const DiscoveredSave& save, const std::filesystem::path& backup_folder);

struct ExportPlan {
    std::filesystem::path destination; // <folder>/SAVE0.DAT
    bool exists = false;               // replacing it needs the player's confirmation
    std::string refused;               // why it cannot be exported there; empty when it can
};
// A destination inside the user savedata folder is refused: that would replace a save without its backup.
ExportPlan PlanExport(const DiscoveredSave& save, const std::filesystem::path& folder, const std::filesystem::path& user_dir);
// Copies the save byte for byte, then reads the copy back and validates it. The save itself is only read.
SaveOpResult ExportSave(const DiscoveredSave& save, const ExportPlan& plan);

enum class ImportMode { kPs4, kPs3 };

struct ImportPlan {
    ImportMode mode = ImportMode::kPs4;
    ImportResult inspected;
    std::filesystem::path destination;
    std::string destination_identity;
    bool unverified_platform = false; // a Vita save: never verified against a real Dragon's Crown Vita file
    std::string refused;
};
// Inspects a source and refuses one that belongs to the other mode; nothing is written.
ImportPlan PlanImport(const std::filesystem::path& source, const DiscoveredSave& destination, ImportMode mode);
// Backs up the destination, replaces it atomically and reads it back; on any failure the destination is left as it was.
SaveOpResult CommitImportPlan(const ImportPlan& plan, const DiscoveredSave& destination,
                              const std::filesystem::path& backup_folder);

inline constexpr size_t kBackupDisplayLimit = 15;

struct BackupListing {
    std::vector<BackupRow> shown; // the newest kBackupDisplayLimit backups of the save
    size_t total = 0;             // every backup of the save; none is ever removed because of the limit
};
BackupListing LatestBackups(std::span<const BackupInfo> backups, const DiscoveredSave& save);
// Deletes one backup of `save`; any other backup, the save and the rest of the folder are left alone.
SaveOpResult DeleteSaveBackup(const BackupInfo& backup, const DiscoveredSave& save, const std::filesystem::path& backup_folder);

// Backs up the save, then replaces it with the backup.
SaveOpResult RestoreToSave(const BackupInfo& backup, const DiscoveredSave& destination,
                           const std::filesystem::path& backup_folder);

} // namespace dcp
