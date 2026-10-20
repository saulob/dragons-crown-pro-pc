#include "updater/install_transaction.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "updater/sha256.h"
#include "updater/update_paths.h"
#include "updater/update_platform.h"

namespace trg::update {
namespace {

unsigned long CurrentProcessIdValue() {
#if defined(_WIN32)
  return GetCurrentProcessId();
#else
  return static_cast<unsigned long>(getpid());
#endif
}

struct ManifestEntry {
  std::string relative;
  std::filesystem::path source;
  std::filesystem::path target;
  std::filesystem::path backup;
  bool existed = false;
};

std::string RelativePath(const std::filesystem::path& path, const std::filesystem::path& root) {
  return Utf8Path(path.lexically_relative(root));
}

void Log(const InstallOptions& options, std::string_view text) {
  if (options.log) {
    options.log(text);
  }
}

bool ReadManifest(const UpdateJob& job, const InstallOptions& options, std::vector<ManifestEntry>& entries,
                  std::string& error) {
  std::ifstream in(job.manifest_file, std::ios::binary);
  if (!in) {
    error = "Cannot read the update manifest.";
    return false;
  }
  std::set<std::string> seen;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    std::string normalized;
    std::string reason;
    if (!NormalizeRelativePath(line, normalized, &reason)) {
      error = "The update manifest contains an unsafe path (" + reason + ").";
      return false;
    }
    if (IsProtectedRelativePath(normalized)) {
      error = "The update manifest contains a protected path: " + normalized + ".";
      return false;
    }
    if (!seen.insert(normalized).second) {
      continue;
    }
    ManifestEntry entry;
    entry.relative = normalized;
    entry.source = job.staging_dir / PathFromUtf8(normalized);
    entry.target = job.install_dir / PathFromUtf8(normalized);
    entry.backup = job.backup_dir / PathFromUtf8(normalized);
    if (!PathIsWithin(entry.source, job.staging_dir) || !IsRegularFile(entry.source)) {
      error = "The update is missing " + normalized + ".";
      return false;
    }
    if (!PathIsWithin(entry.target, job.install_dir)) {
      error = "The update manifest points outside the installation.";
      return false;
    }
    if (HasReparseAncestor(entry.target, job.install_dir)) {
      error = "The installation contains a link that the update will not follow.";
      return false;
    }
    entries.push_back(std::move(entry));
    if (entries.size() > 10000) {
      error = "The update manifest holds too many files.";
      return false;
    }
  }
  if (entries.empty()) {
    error = "The update manifest is empty.";
    return false;
  }
  Log(options, "manifest entries: " + std::to_string(entries.size()));
  return true;
}

bool ReplaceFile(const std::filesystem::path& from, const std::filesystem::path& to) {
#if defined(_WIN32)
  return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  return rename(from.c_str(), to.c_str()) == 0;
#endif
}

bool ReplaceFileOr(const std::filesystem::path& from, const std::filesystem::path& to,
                   const InstallOptions& options) {
  return options.replace ? options.replace(from, to) : ReplaceFile(from, to);
}

bool MakeBackup(const std::vector<ManifestEntry>& entries, const InstallOptions& options, std::string& error) {
  Log(options, "backup start");
  std::error_code ec;
  for (const ManifestEntry& entry : entries) {
    const std::filesystem::file_status status = std::filesystem::symlink_status(entry.target, ec);
    if (status.type() == std::filesystem::file_type::not_found) {
      continue;
    }
    if (ec) {
      error = "Cannot inspect " + entry.relative + ".";
      return false;
    }
    if (!std::filesystem::exists(status)) {
      continue;
    }
    if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) {
      error = "The installation has an unexpected file at " + entry.relative + ".";
      return false;
    }
    std::filesystem::create_directories(entry.backup.parent_path(), ec);
    if (ec) {
      error = "Cannot create the update backup folder.";
      return false;
    }
    std::filesystem::copy_file(entry.target, entry.backup, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      error = "Cannot back up " + entry.relative + ".";
      return false;
    }
  }
  Log(options, "backup complete");
  return true;
}

bool VerifyCopy(const ManifestEntry& entry, std::string& error) {
  std::string hash_error;
  const std::string source_hash = Sha256FileHex(entry.source, hash_error);
  if (source_hash.empty()) {
    error = "Cannot verify " + entry.relative + ".";
    return false;
  }
  const std::string target_hash = Sha256FileHex(entry.target, hash_error);
  if (target_hash != source_hash) {
    error = "The installed file does not match the update: " + entry.relative + ".";
    return false;
  }
  return true;
}

bool Rollback(std::vector<ManifestEntry>& installed, const InstallOptions& options, std::string& error) {
  Log(options, "rollback start");
  bool ok = true;
  for (auto it = installed.rbegin(); it != installed.rend(); ++it) {
    ManifestEntry& entry = *it;
    std::error_code ec;
    if (!entry.existed) {
      std::filesystem::remove(entry.target, ec);
      if (ec) {
        ok = false;
      }
      continue;
    }
    std::filesystem::path temporary;
    bool found = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
      temporary = entry.target.parent_path() / (".trg-update-rollback-" + std::to_string(CurrentProcessIdValue()) +
                                                 "-" + std::to_string(attempt) + ".tmp");
      if (!std::filesystem::exists(temporary, ec) && !ec) {
        found = true;
        break;
      }
      ec.clear();
    }
    if (!found) {
      ok = false;
      continue;
    }
    std::filesystem::copy_file(entry.backup, temporary, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec || !ReplaceFile(temporary, entry.target)) {
      std::filesystem::remove(temporary, ec);
      ok = false;
    }
  }
  if (!ok) {
    error = "The previous files could not be restored.";
    Log(options, "rollback failed");
  } else {
    Log(options, "rollback complete");
  }
  return ok;
}

}  // namespace

bool ValidateInstallJob(const UpdateJob& job, std::string& error) {
  error.clear();
  if (job.parent_pid == 0) {
    error = "The update job has no launcher process.";
    return false;
  }
  for (const std::filesystem::path* path : {&job.install_dir, &job.staging_dir, &job.backup_dir, &job.manifest_file,
                                            &job.restart_executable, &job.log_file}) {
    if (path->empty() || !path->is_absolute()) {
      error = "The update job contains a relative path.";
      return false;
    }
  }
  std::error_code ec;
  if (!std::filesystem::is_directory(job.install_dir, ec) || !std::filesystem::is_directory(job.staging_dir, ec)) {
    error = "The update job names a missing folder.";
    return false;
  }
  if (PathIsWithin(job.staging_dir, job.install_dir) || PathIsWithin(job.install_dir, job.staging_dir) ||
      PathIsWithin(job.backup_dir, job.install_dir) || PathIsWithin(job.install_dir, job.backup_dir) ||
      PathIsWithin(job.backup_dir, job.staging_dir) || PathIsWithin(job.staging_dir, job.backup_dir)) {
    error = "The update staging and backup folders must be separate from the installation and each other.";
    return false;
  }
  if (PathIsWithin(job.manifest_file, job.install_dir) || PathIsWithin(job.log_file, job.install_dir)) {
    error = "The update job must not write into the installation before replacement.";
    return false;
  }
  if (!IsRegularFile(job.manifest_file)) {
    error = "The update manifest is missing.";
    return false;
  }
  if (!PathIsWithin(job.restart_executable, job.install_dir)) {
    error = "The restart program is outside the installation.";
    return false;
  }
  return true;
}

InstallOutcome InstallFiles(const UpdateJob& job, const InstallOptions& options, bool restart) {
  InstallOutcome outcome;
  if (!ValidateInstallJob(job, outcome.error)) {
    outcome.status = InstallStatus::kInvalidJob;
    return outcome;
  }

  std::vector<ManifestEntry> entries;
  if (!ReadManifest(job, options, entries, outcome.error)) {
    outcome.status = InstallStatus::kInvalidJob;
    return outcome;
  }

  std::string restart_relative;
  if (!NormalizeRelativePath(RelativePath(job.restart_executable, job.install_dir), restart_relative)) {
    outcome.error = "The restart program is not inside the installation.";
    outcome.status = InstallStatus::kInvalidJob;
    return outcome;
  }
  if (std::none_of(entries.begin(), entries.end(),
                   [&](const ManifestEntry& entry) { return entry.relative == restart_relative; })) {
    outcome.error = "The update does not contain the launcher program.";
    outcome.status = InstallStatus::kInvalidJob;
    return outcome;
  }

  for (ManifestEntry& entry : entries) {
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::symlink_status(entry.target, ec);
    if (status.type() == std::filesystem::file_type::not_found) {
      entry.existed = false;
      continue;
    }
    if (ec) {
      outcome.error = "Cannot inspect " + entry.relative + ".";
      outcome.status = InstallStatus::kInvalidJob;
      return outcome;
    }
    entry.existed = std::filesystem::exists(status);
  }

  if (!MakeBackup(entries, options, outcome.error)) {
    Log(options, "backup failed: " + outcome.error);
    std::error_code ec;
    std::filesystem::remove_all(job.backup_dir, ec);
    std::filesystem::remove_all(job.staging_dir, ec);
    outcome.status = InstallStatus::kBackupFailed;
    return outcome;
  }

  Log(options, "replacing " + std::to_string(entries.size()) + " files");
  std::vector<ManifestEntry> installed;
  std::string failure;
  for (size_t index = 0; index < entries.size(); ++index) {
    ManifestEntry& entry = entries[index];
    std::error_code ec;
    std::filesystem::create_directories(entry.target.parent_path(), ec);
    if (ec) {
      failure = "Cannot create the folder for " + entry.relative + ".";
      break;
    }
    std::filesystem::path temporary;
    bool found = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
      temporary = entry.target.parent_path() / (".trg-update-" + std::to_string(CurrentProcessIdValue()) + "-" +
                                                 std::to_string(index) + "-" + std::to_string(attempt) + ".tmp");
      if (!std::filesystem::exists(temporary, ec) && !ec) {
        found = true;
        break;
      }
      ec.clear();
    }
    if (!found) {
      failure = "Cannot find a free temporary name for " + entry.relative + ".";
      break;
    }
    std::filesystem::copy_file(entry.source, temporary, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      std::filesystem::remove(temporary, ec);
      failure = "Cannot write " + entry.relative + ".";
      break;
    }
    if (!ReplaceFileOr(temporary, entry.target, options)) {
      std::filesystem::remove(temporary, ec);
      failure = "Cannot replace " + entry.relative + ".";
      break;
    }
    installed.push_back(entry);
    if (!VerifyCopy(entry, failure)) {
      break;
    }
  }

  if (!failure.empty()) {
    Log(options, "install failed: " + failure);
    outcome.error = failure;
    const bool restored = Rollback(installed, options, outcome.error);
    if (restored) {
      std::error_code ec;
      std::filesystem::remove_all(job.backup_dir, ec);
      std::filesystem::remove_all(job.staging_dir, ec);
    }
    outcome.status = restored ? InstallStatus::kInstallFailedRolledBack
                              : InstallStatus::kInstallFailedRollbackFailed;
    return outcome;
  }
  Log(options, "replacement complete");
  Log(options, "verification complete");

  if (restart) {
    Log(options, "restart start");
    std::string restart_error;
    if (!LaunchApplication(job.restart_executable, job.restart_arguments, job.install_dir, restart_error, true)) {
      Log(options, "restart failed: " + restart_error);
      outcome.error = restart_error;
      const bool restored = Rollback(installed, options, outcome.error);
      if (restored) {
        std::error_code ec;
        std::filesystem::remove_all(job.backup_dir, ec);
        std::filesystem::remove_all(job.staging_dir, ec);
      }
      outcome.status = restored ? InstallStatus::kRestartFailedRolledBack
                                : InstallStatus::kRestartFailedRollbackFailed;
      return outcome;
    }
    Log(options, "restart complete");
  }

  std::error_code ec;
  std::filesystem::remove_all(job.backup_dir, ec);
  std::filesystem::remove_all(job.staging_dir, ec);
  outcome.status = InstallStatus::kSuccess;
  return outcome;
}

}  // namespace trg::update
