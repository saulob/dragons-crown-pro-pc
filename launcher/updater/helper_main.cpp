#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include "updater/install_transaction.h"
#include "updater/update_job.h"
#include "updater/update_paths.h"
#include "updater/update_platform.h"

namespace {

enum ExitCode {
  kSuccess = 0,
  kBadJob = 2,
  kParentWaitFailed = 3,
  kInvalidJob = 4,
  kBackupFailed = 5,
  kInstallFailedRolledBack = 6,
  kRestartFailedRolledBack = 7,
  kInstallFailedRollbackFailed = 8,
  kRestartFailedRollbackFailed = 9,
};

ExitCode CodeFor(trg::update::InstallStatus status) {
  using trg::update::InstallStatus;
  switch (status) {
    case InstallStatus::kSuccess:
      return kSuccess;
    case InstallStatus::kInvalidJob:
      return kInvalidJob;
    case InstallStatus::kBackupFailed:
      return kBackupFailed;
    case InstallStatus::kInstallFailedRolledBack:
      return kInstallFailedRolledBack;
    case InstallStatus::kInstallFailedRollbackFailed:
      return kInstallFailedRollbackFailed;
    case InstallStatus::kRestartFailedRolledBack:
      return kRestartFailedRolledBack;
    case InstallStatus::kRestartFailedRollbackFailed:
      return kRestartFailedRollbackFailed;
  }
  return kInvalidJob;
}

std::string ArgumentsText(const std::vector<std::string>& arguments) {
  std::string text;
  for (const std::string& argument : arguments) {
    if (!text.empty()) {
      text += ' ';
    }
    text += argument;
  }
  return text;
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  int argument_count = 0;
  wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
  if (!arguments) {
    return kBadJob;
  }
  std::filesystem::path job_path;
  for (int i = 1; i + 1 < argument_count; ++i) {
    if (std::wstring_view(arguments[i]) == L"--job") {
      job_path = std::filesystem::path(arguments[i + 1]);
      ++i;
    }
  }
  LocalFree(arguments);
  if (job_path.empty()) {
    return kBadJob;
  }

  std::ofstream log(job_path.parent_path() / "trg-updater.log", std::ios::binary | std::ios::app);
  auto write = [&log](std::string_view text) {
    log << text << '\n';
    log.flush();
  };

  write("trg-updater starting");
  write("job: " + trg::update::Utf8Path(job_path));

  std::string error;
  const std::optional<trg::update::UpdateJob> job = trg::update::ReadUpdateJob(job_path, error);
  if (!job) {
    write("job read failed: " + error);
    write("exit code: " + std::to_string(kBadJob));
    return kBadJob;
  }
  if (!job->log_file.empty()) {
    log.close();
    log.open(job->log_file, std::ios::binary | std::ios::app);
  }
  write("install root: " + trg::update::Utf8Path(job->install_dir));
  write("staging root: " + trg::update::Utf8Path(job->staging_dir));
  write("manifest: " + trg::update::Utf8Path(job->manifest_file));
  write("restart executable: " + trg::update::Utf8Path(job->restart_executable));
  write("restart arguments: " + ArgumentsText(job->restart_arguments));
  write("waiting for parent pid " + std::to_string(job->parent_pid));
  if (!trg::update::WaitForProcess(job->parent_pid, 120000, error)) {
    write("parent wait failed: " + error);
    write("exit code: " + std::to_string(kParentWaitFailed));
    return kParentWaitFailed;
  }
  write("parent exited");

  trg::update::InstallOptions options;
  options.log = write;
  const trg::update::InstallOutcome outcome = trg::update::InstallFiles(*job, options, true);
  const ExitCode code = CodeFor(outcome.status);
  if (outcome.status == trg::update::InstallStatus::kSuccess) {
    write("install result: success");
    write("update installed and restarted");
    std::error_code ec;
    std::filesystem::remove(job_path, ec);
  } else {
    write("install result: " + outcome.error);
    std::string recovery;
    if (trg::update::LaunchApplication(job->restart_executable, job->restart_arguments, job->install_dir,
                                       recovery, true)) {
      write("previous launcher restarted");
    } else {
      write("could not restart the previous launcher: " + recovery);
    }
  }
  write("exit code: " + std::to_string(code));
  return code;
}
