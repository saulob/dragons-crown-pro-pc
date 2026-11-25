// Backups and the safe write path for SAVE0.DAT. A write always lands in a temporary copy first, is validated, and
// only then atomically replaces the target; the original is never modified on any failure.
#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dcp {

struct BackupInfo {
    std::filesystem::path file;    // the .DAT backup
    std::filesystem::path meta;    // the .meta sidecar
    std::string title_id;
    std::string dir_name;
    std::string original_path;
    std::string timestamp;
};

// Writes `bytes` to `target` atomically: a temporary file in the same folder is written, validated by `validate`, and
// renamed over the target. Returns an empty optional on success, or an error string. On failure the target is untouched.
std::optional<std::string> WriteSaveAtomically(const std::filesystem::path& target, std::span<const uint8_t> bytes,
                                               bool (*validate)(std::span<const uint8_t>, std::vector<std::string>&));

// Copies SAVE0.DAT at `save0_path` into the backup folder. The backup name encodes the timestamp, title id and savedata
// folder (reduced to plain tokens) and never overwrites an existing file. Returns the backup, or empty on failure.
std::optional<BackupInfo> CreateBackup(const std::filesystem::path& backup_folder, const std::filesystem::path& save0_path,
                                       std::string_view title_id, std::string_view dir_name);

// Lists the backups in a folder, newest first. A .meta without its .DAT, or over 4 KiB, is skipped; fields that are not
// plain tokens read as empty.
std::vector<BackupInfo> ListBackups(const std::filesystem::path& backup_folder);

// Restores `backup` over `save0_path`, first backing up the current file under the destination's `title_id` and
// `dir_name`. Returns an error string, empty on success.
std::optional<std::string> RestoreBackup(const BackupInfo& backup, const std::filesystem::path& save0_path,
                                         const std::filesystem::path& backup_folder, std::string_view title_id,
                                         std::string_view dir_name);

// Deletes one backup (its .DAT and its .meta) of the save `title_id` / `dir_name`. The backup must be listed by ListBackups
// in `backup_folder` for that save; anything else is refused and nothing else is touched. Returns an error string, empty on success.
std::optional<std::string> DeleteBackup(const BackupInfo& backup, const std::filesystem::path& backup_folder,
                                        std::string_view title_id, std::string_view dir_name);

} // namespace dcp
