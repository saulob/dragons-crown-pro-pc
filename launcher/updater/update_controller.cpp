#include "updater/update_controller.h"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include <trg/download.h>

#include "updater/sha256.h"
#include "updater/update_job.h"
#include "updater/update_paths.h"
#include "updater/update_platform.h"
#include "updater/update_zip.h"

namespace trg::update {

struct UpdateController::CheckOutcome {
  std::optional<Release> release;
  std::optional<ReleaseAsset> asset;
  std::optional<ReleaseAsset> checksum_asset;
  bool ignored = false;
  bool up_to_date = false;
  bool available = false;
};

namespace {

constexpr long long kMaxDownloadBytes = 1ll << 30;
constexpr int kCheckTimeoutMs = 30000;
constexpr int kDownloadTimeoutMs = 30 * 60 * 1000;
constexpr long long kMaxReleaseJsonBytes = 4ll << 20;

trg::HttpOptions HttpOptionsFor(const UpdateConfig& config, int total_timeout_ms, long long max_bytes) {
  trg::HttpOptions options;
  options.user_agent = config.user_agent;
  options.connect_timeout_ms = 15000;
  options.receive_timeout_ms = 30000;
  options.total_timeout_ms = total_timeout_ms;
  options.max_bytes = max_bytes;
  options.headers = {
      {"Accept", "application/vnd.github+json"},
      {"X-GitHub-Api-Version", "2022-11-28"},
  };
  return options;
}

bool ReadTextFile(const std::filesystem::path& path, std::string& text, std::string& error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "Cannot read the release fixture.";
    return false;
  }
  std::ostringstream stream;
  stream << file.rdbuf();
  text = stream.str();
  if (text.size() > size_t(kMaxReleaseJsonBytes)) {
    error = "The release response is too large.";
    return false;
  }
  return true;
}

bool IsHttpsUrl(std::string_view url) {
  return url.rfind("https://", 0) == 0;
}

void Log(const UpdateConfig& config, std::string_view text) {
  if (config.log) {
    config.log(text);
  }
}

void CleanupStaleWorkspaces() {
  std::error_code ec;
  const std::filesystem::path root = std::filesystem::temp_directory_path(ec);
  if (ec || root.empty()) {
    return;
  }
  const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
  for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = Utf8Path(it->path().filename());
    if (name.rfind("trg-update-", 0) != 0) {
      continue;
    }
    const std::filesystem::file_status status = it->symlink_status(ec);
    if (ec || !std::filesystem::is_directory(status)) {
      ec.clear();
      continue;
    }
    const std::filesystem::file_time_type modified = it->last_write_time(ec);
    if (ec) {
      ec.clear();
      continue;
    }
    if (modified < cutoff) {
      std::filesystem::remove_all(it->path(), ec);
      ec.clear();
    }
  }
}

std::string DownloadExtension(const std::string& asset_name) {
  const size_t dot = asset_name.rfind('.');
  if (dot == std::string::npos || asset_name.size() - dot > 12) {
    return ".bin";
  }
  const std::string extension = asset_name.substr(dot);
  for (const char c : extension) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.') {
      return ".bin";
    }
  }
  return extension;
}

}  // namespace

UpdateController::WorkspaceCleanup::~WorkspaceCleanup() {
  if (enabled && !path.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
}

UpdateController::UpdateController(UpdateConfig config) : config_(std::move(config)) {
  CleanupStaleWorkspaces();
  snapshot_.current_version = config_.current_version;
  snapshot_.install_supported = config_.install_supported;
  snapshot_.install_unsupported_reason = config_.install_unsupported_reason;
  current_version_ = ParseVersion(config_.current_version);
  if (!current_version_) {
    snapshot_.state = UpdateState::kFailed;
    snapshot_.error = "This build's version could not be read.";
    Log(config_, "Update check disabled: invalid local version");
  }
}

UpdateController::~UpdateController() {
  task_.Cancel();
}

void UpdateController::StartCheck(bool manual) {
  if (!current_version_ || task_kind_ != TaskKind::kNone) {
    return;
  }
  if (snapshot_.state == UpdateState::kAvailable || snapshot_.state == UpdateState::kDownloading ||
      snapshot_.state == UpdateState::kVerifying || snapshot_.state == UpdateState::kReadyToRestart ||
      snapshot_.state == UpdateState::kNoUpdate) {
    return;
  }
  if (snapshot_.state == UpdateState::kChecking) {
    return;
  }

  snapshot_.state = UpdateState::kChecking;
  snapshot_.manual_check = manual;
  snapshot_.error.clear();
  snapshot_.progress = -1.0f;
  snapshot_.retry_available = false;
  snapshot_.retry_check = false;
  retry_kind_ = RetryKind::kCheck;
  check_outcome_ = std::make_shared<CheckOutcome>();
  task_kind_ = TaskKind::kCheck;

  const UpdateConfig config = config_;
  const SemanticVersion current = *current_version_;
  const std::shared_ptr<CheckOutcome> outcome = check_outcome_;
  const bool started = task_.Start("Checking for updates...", [config, current, outcome](trg::Task& task) {
    std::string json;
    std::string error;
    if (config.release_json_override) {
      if (!ReadTextFile(*config.release_json_override, json, error)) {
        return error;
      }
    } else {
      if (!IsHttpsUrl(config.releases_url)) {
        return std::string("The update address is not secure.");
      }
      error = trg::HttpGet(config.releases_url, json, &task, HttpOptionsFor(config, kCheckTimeoutMs, kMaxReleaseJsonBytes));
      if (!error.empty()) {
        return error;
      }
    }

    const std::optional<Release> release = ParseRelease(json, &error);
    if (!release) {
      return error.empty() ? std::string("The update response could not be read.") : error;
    }
    outcome->release = *release;
    if (!IsInstallableRelease(*release)) {
      outcome->ignored = true;
      return std::string();
    }
    if (SemanticVersion::Compare(release->version, current) <= 0) {
      outcome->up_to_date = true;
      return std::string();
    }
    if (!config.asset_name) {
      return std::string("The updater has no package name for this build.");
    }
    const std::string name = config.asset_name(release->version);
    if (name.empty()) {
      return std::string("The updater has no package name for this build.");
    }
    const ReleaseAsset* asset = FindAsset(*release, name);
    if (!asset) {
      return "The latest release does not include " + name + ".";
    }
    if (!IsHttpsUrl(asset->download_url)) {
      return std::string("The update package address is not secure.");
    }
    if (asset->size == 0 || asset->size > std::uint64_t(kMaxDownloadBytes)) {
      return std::string("The update package has an invalid size.");
    }
    outcome->asset = *asset;
    if (config.checksum_asset_name) {
      const std::string checksum_name = config.checksum_asset_name(release->version);
      if (!checksum_name.empty()) {
        if (const ReleaseAsset* found = FindAsset(*release, checksum_name); found) {
          if (!IsHttpsUrl(found->download_url)) {
            return std::string("The checksum address is not secure.");
          }
          outcome->checksum_asset = *found;
        }
      }
    }
    outcome->available = true;
    return std::string();
  });
  if (!started) {
    task_kind_ = TaskKind::kNone;
    Fail("The update check could not start.", RetryKind::kCheck, true);
  }
}

void UpdateController::FinishCheck(trg::Task::State state, const std::string& error) {
  snapshot_.manual_check = false;
  const std::shared_ptr<CheckOutcome> outcome = std::move(check_outcome_);
  if (state == trg::Task::State::kFailed || state == trg::Task::State::kCancelled || !outcome) {
    Fail(error.empty() ? "The update check failed." : error, RetryKind::kCheck, true);
    return;
  }
  if (!outcome->release) {
    Fail("The update check failed.", RetryKind::kCheck, true);
    return;
  }

  release_ = std::move(outcome->release);
  if (outcome->ignored || outcome->up_to_date || !outcome->available) {
    asset_.reset();
    checksum_asset_.reset();
    snapshot_.state = UpdateState::kNoUpdate;
    snapshot_.progress = -1.0f;
    snapshot_.error.clear();
    snapshot_.retry_available = false;
    Log(config_, "No update available");
    return;
  }

  asset_ = std::move(outcome->asset);
  checksum_asset_ = std::move(outcome->checksum_asset);
  snapshot_.state = UpdateState::kAvailable;
  snapshot_.latest_version = release_->version.ToString();
  snapshot_.release_url = IsHttpsUrl(release_->html_url) ? release_->html_url : std::string();
  snapshot_.asset_name = asset_->name;
  snapshot_.progress = -1.0f;
  snapshot_.error.clear();
  snapshot_.retry_available = false;
  Log(config_, "Update available: " + snapshot_.latest_version + " (current " + snapshot_.current_version + ")");
}

void UpdateController::StartDownload() {
  if (task_kind_ != TaskKind::kNone || !release_ || !asset_) {
    return;
  }
  if (!config_.install_supported) {
    Fail(config_.install_unsupported_reason.empty() ? "Automatic installation is not available in this build."
                                                    : config_.install_unsupported_reason,
         RetryKind::kCheck, false);
    return;
  }

  std::string error;
  if (!MakeWorkspace(error)) {
    Fail(error, RetryKind::kDownload, false);
    return;
  }
  download_path_ = workspace_ / ("update" + DownloadExtension(asset_->name));
  staging_path_ = workspace_ / "staging";
  backup_path_ = workspace_ / "backup";
  manifest_path_ = workspace_ / "manifest.txt";
  staging_root_ = std::make_shared<std::filesystem::path>();
  worker_phase_ = 0;
  snapshot_.state = UpdateState::kDownloading;
  snapshot_.progress = 0.0f;
  snapshot_.error.clear();
  snapshot_.retry_available = false;
  task_kind_ = TaskKind::kDownload;

  const UpdateConfig config = config_;
  const ReleaseAsset asset = *asset_;
  const std::optional<ReleaseAsset> checksum_asset = checksum_asset_;
  const std::filesystem::path download_path = download_path_;
  const std::filesystem::path staging_path = staging_path_;
  const std::filesystem::path manifest_path = manifest_path_;
  const std::shared_ptr<std::filesystem::path> staging_root = staging_root_;

  const bool started = task_.Start("Downloading update...", [this, config, asset, checksum_asset, download_path,
                                                               staging_path, manifest_path, staging_root](trg::Task& task) {
    std::string expected;
    if (config.sha256_override && !config.sha256_override->empty()) {
      if (!IsValidSha256(*config.sha256_override)) {
        return std::string("The update test checksum is not valid.");
      }
      expected = NormalizeSha256(*config.sha256_override);
    } else if (const std::optional<std::string> digest = asset.sha256(); digest) {
      expected = *digest;
    } else if (checksum_asset) {
      std::string checksum;
      std::string error = trg::HttpGet(checksum_asset->download_url, checksum, &task,
                                       HttpOptionsFor(config, 20000, 64ll << 10));
      if (!error.empty()) {
        return error;
      }
      if (const std::optional<std::string> checksum_digest = checksum_asset->sha256(); checksum_digest) {
        if (Sha256Hex(checksum) != *checksum_digest) {
          return std::string("The published checksum file does not match its own digest.");
        }
      }
      const std::optional<std::string> parsed = ParseSha256File(checksum);
      if (!parsed) {
        return std::string("The published checksum file is not valid.");
      }
      expected = *parsed;
    } else {
      return std::string("The release does not provide a trusted SHA-256 checksum.");
    }

    trg::HttpOptions download_options = HttpOptionsFor(config, kDownloadTimeoutMs, kMaxDownloadBytes);
    download_options.progress_total = static_cast<long long>(asset.size);
    download_options.max_bytes = static_cast<long long>(asset.size) + (1ll << 20);
    std::string error = trg::DownloadFile(task, asset.download_url, Utf8Path(download_path), download_options);
    if (!error.empty()) {
      return error;
    }

    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(download_path, ec);
    if (ec || size != asset.size) {
      return std::string("The update download is incomplete.");
    }
    std::string hash_error;
    const std::string actual = Sha256FileHex(download_path, hash_error);
    if (actual.empty()) {
      return hash_error;
    }
    if (actual != expected) {
      return std::string("The downloaded update does not match its SHA-256 checksum.");
    }

    worker_phase_ = 1;
    if (config.package_kind == UpdatePackageKind::kZip) {
      ZipLimits limits;
      if (!ExtractZip(download_path, staging_path, limits, error)) {
        return error;
      }
      std::filesystem::path content_root;
      if (!ResolveStagingRoot(staging_path, config.required_staged_files, content_root, error)) {
        return error;
      }
      if (!ValidateStaging(content_root, config.required_staged_files, error)) {
        return error;
      }
      const std::vector<std::string> files = ListStagingFiles(content_root, error);
      if (!error.empty()) {
        return error;
      }
      *staging_root = content_root;
      std::ofstream manifest(manifest_path, std::ios::binary | std::ios::trunc);
      if (!manifest) {
        return std::string("Cannot write the update manifest.");
      }
      for (const std::string& file : files) {
        manifest << file << '\n';
      }
      manifest.close();
      if (!manifest) {
        return std::string("Cannot write the update manifest.");
      }
    }
    return std::string();
  });
  if (!started) {
    task_kind_ = TaskKind::kNone;
    CleanupWorkspace();
    Fail("The update download could not start.", RetryKind::kDownload, false);
  }
}

void UpdateController::FinishDownload(trg::Task::State state, const std::string& error) {
  snapshot_.manual_check = false;
  if (state != trg::Task::State::kDone || !error.empty()) {
    CleanupWorkspace();
    Fail(error.empty() ? "The update download failed." : error, RetryKind::kDownload, false);
    return;
  }
  if (config_.package_kind == UpdatePackageKind::kZip && staging_root_ && !staging_root_->empty()) {
    staging_path_ = *staging_root_;
  }
  snapshot_.state = UpdateState::kReadyToRestart;
  snapshot_.progress = 1.0f;
  snapshot_.error.clear();
  snapshot_.retry_available = false;
  Log(config_, "Update downloaded and verified: " + snapshot_.latest_version);
}

bool UpdateController::Retry() {
  if (snapshot_.state != UpdateState::kFailed) {
    return false;
  }
  switch (retry_kind_) {
    case RetryKind::kCheck:
      StartCheck(true);
      return false;
    case RetryKind::kDownload:
      StartDownload();
      return false;
    case RetryKind::kInstall:
      return BeginInstall();
  }
  return false;
}

bool UpdateController::BeginInstall() {
  if (snapshot_.state != UpdateState::kReadyToRestart &&
      !(snapshot_.state == UpdateState::kFailed && retry_kind_ == RetryKind::kInstall)) {
    return false;
  }
  if (!config_.install_supported) {
    snapshot_.state = UpdateState::kFailed;
    snapshot_.error = config_.install_unsupported_reason.empty()
                          ? "Automatic installation is not available in this build."
                          : config_.install_unsupported_reason;
    snapshot_.retry_available = false;
    return false;
  }

  std::string error;
  if (config_.package_kind == UpdatePackageKind::kZip) {
#ifdef _WIN32
    const std::filesystem::path helper_source = config_.install_dir / "trg-updater.exe";
    const std::filesystem::path helper = workspace_ / "trg-updater.exe";
    std::error_code ec;
    std::filesystem::copy_file(helper_source, helper, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      Fail("The update helper is missing from this installation.", RetryKind::kInstall, false);
      return false;
    }
    UpdateJob job;
    job.parent_pid = CurrentProcessId();
    job.install_dir = config_.install_dir;
    job.staging_dir = staging_path_;
    job.backup_dir = backup_path_;
    job.manifest_file = manifest_path_;
    job.restart_executable = config_.restart_executable;
    job.restart_arguments = config_.restart_arguments;
    job.log_file = workspace_ / "trg-updater.log";
    const std::filesystem::path job_file = workspace_ / "job.txt";
    cleanup_.enabled = false;
    if (!WriteUpdateJob(job_file, job, error)) {
      Fail(error, RetryKind::kInstall, false);
      return false;
    }
    if (!LaunchUpdateHelper(helper, job_file, error)) {
      Fail(error, RetryKind::kInstall, false);
      return false;
    }
    install_started_ = true;
    Log(config_, "Update helper started");
    return true;
#else
    Fail("The update helper is only used on Windows.", RetryKind::kInstall, false);
    return false;
#endif
  }

  if (!ApplyAppImageUpdate(download_path_, config_.appimage_path, config_.restart_arguments, error)) {
    Fail(error, RetryKind::kInstall, false);
    return false;
  }
  Log(config_, "Updated AppImage started");
  return true;
}

void UpdateController::Cancel() {
  task_.Cancel();
}

void UpdateController::Poll() {
  if (task_kind_ == TaskKind::kNone) {
    return;
  }
  if (task_.running()) {
    if (task_kind_ == TaskKind::kCheck) {
      snapshot_.state = UpdateState::kChecking;
      snapshot_.progress = task_.fraction();
    } else {
      snapshot_.state = worker_phase_.load(std::memory_order_acquire) == 0 ? UpdateState::kDownloading
                                                                            : UpdateState::kVerifying;
      snapshot_.progress = task_.fraction();
    }
    return;
  }

  const TaskKind kind = task_kind_;
  task_kind_ = TaskKind::kNone;
  std::string error;
  const trg::Task::State state = task_.Finish(&error);
  if (kind == TaskKind::kCheck) {
    FinishCheck(state, error);
  } else {
    FinishDownload(state, error);
  }
}

void UpdateController::Fail(std::string error, RetryKind retry, bool check_failure) {
  snapshot_.state = UpdateState::kFailed;
  snapshot_.manual_check = false;
  snapshot_.error = std::move(error);
  snapshot_.progress = -1.0f;
  snapshot_.retry_available = true;
  snapshot_.retry_check = check_failure;
  retry_kind_ = retry;
  Log(config_, "Update error: " + snapshot_.error);
}

bool UpdateController::MakeWorkspace(std::string& error) {
  CleanupWorkspace();
  std::error_code ec;
  const std::filesystem::path root = std::filesystem::temp_directory_path(ec);
  if (ec || root.empty()) {
    error = "Cannot find a temporary folder for the update.";
    return false;
  }
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  std::filesystem::path workspace;
  bool found = false;
  for (int attempt = 0; attempt < 1000; ++attempt) {
    workspace = root / ("trg-update-" + std::to_string(CurrentProcessId()) + "-" + std::to_string(now) +
                        (attempt == 0 ? std::string() : "-" + std::to_string(attempt)));
    if (!std::filesystem::exists(workspace, ec) && !ec) {
      found = true;
      break;
    }
    ec.clear();
  }
  if (!found) {
    error = "Cannot find a free temporary folder for the update.";
    return false;
  }
  if (PathIsWithin(workspace, config_.install_dir) || PathIsWithin(config_.install_dir, workspace)) {
    error = "The update workspace cannot be inside the installation.";
    return false;
  }
  std::filesystem::create_directories(workspace / "staging", ec);
  if (ec) {
    std::filesystem::remove_all(workspace, ec);
    error = "Cannot create the update workspace.";
    return false;
  }
  std::filesystem::create_directories(workspace / "backup", ec);
  if (ec) {
    std::filesystem::remove_all(workspace, ec);
    error = "Cannot create the update workspace.";
    return false;
  }
  workspace_ = workspace;
  cleanup_.path = workspace;
  cleanup_.enabled = true;
  return true;
}

void UpdateController::CleanupWorkspace() {
  if (install_started_) {
    return;
  }
  if (!workspace_.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(workspace_, ec);
  }
  workspace_.clear();
  download_path_.clear();
  staging_path_.clear();
  backup_path_.clear();
  manifest_path_.clear();
  staging_root_.reset();
  cleanup_.path.clear();
  cleanup_.enabled = false;
}

}  // namespace trg::update
