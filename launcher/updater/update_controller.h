#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <trg/platform.h>

#include "updater/github_release.h"
#include "updater/version.h"

namespace trg::update {

enum class UpdateState {
  kIdle,
  kChecking,
  kNoUpdate,
  kAvailable,
  kDownloading,
  kVerifying,
  kReadyToRestart,
  kFailed,
};

enum class UpdatePackageKind { kZip, kAppImage };

struct UpdateConfig {
  std::string current_version;
  std::string releases_url;
  std::string user_agent;
  UpdatePackageKind package_kind = UpdatePackageKind::kZip;
  std::function<std::string(const SemanticVersion&)> asset_name;
  std::function<std::string(const SemanticVersion&)> checksum_asset_name;
  std::filesystem::path install_dir;
  std::filesystem::path appimage_path;
  std::filesystem::path restart_executable;
  std::vector<std::string> restart_arguments;
  std::vector<std::string> required_staged_files;
  bool install_supported = false;
  std::string install_unsupported_reason;
  std::optional<std::filesystem::path> release_json_override;
  std::optional<std::string> sha256_override;
  std::function<void(std::string_view)> log;
};

struct UpdateSnapshot {
  UpdateState state = UpdateState::kIdle;
  std::string current_version;
  std::string latest_version;
  std::string release_url;
  std::string asset_name;
  std::string error;
  std::string install_unsupported_reason;
  float progress = -1.0f;
  bool install_supported = false;
  bool retry_available = false;
  bool retry_check = false;
  bool manual_check = false;
};

class UpdateController {
 public:
  explicit UpdateController(UpdateConfig config);
  ~UpdateController();

  UpdateController(const UpdateController&) = delete;
  UpdateController& operator=(const UpdateController&) = delete;

  void StartCheck(bool manual = false);
  void StartDownload();
  bool Retry();
  bool RetryNeedsInstall() const { return snapshot_.state == UpdateState::kFailed && retry_kind_ == RetryKind::kInstall; }
  bool BeginInstall();
  void Cancel();
  bool Busy() const { return task_.running(); }
  void Poll();

  const UpdateSnapshot& snapshot() const { return snapshot_; }

 private:
  enum class TaskKind { kNone, kCheck, kDownload };
  enum class RetryKind { kCheck, kDownload, kInstall };

  struct CheckOutcome;
  struct WorkspaceCleanup {
    std::filesystem::path path;
    bool enabled = false;
    ~WorkspaceCleanup();
  };

  void FinishCheck(trg::Task::State state, const std::string& error);
  void FinishDownload(trg::Task::State state, const std::string& error);
  void Fail(std::string error, RetryKind retry, bool check_failure);
  bool MakeWorkspace(std::string& error);
  void CleanupWorkspace();

  UpdateConfig config_;
  std::optional<SemanticVersion> current_version_;
  UpdateSnapshot snapshot_;
  WorkspaceCleanup cleanup_;
  trg::Task task_;
  TaskKind task_kind_ = TaskKind::kNone;
  RetryKind retry_kind_ = RetryKind::kCheck;
  std::shared_ptr<CheckOutcome> check_outcome_;
  std::optional<Release> release_;
  std::optional<ReleaseAsset> asset_;
  std::optional<ReleaseAsset> checksum_asset_;
  std::filesystem::path workspace_;
  std::filesystem::path download_path_;
  std::filesystem::path staging_path_;
  std::filesystem::path backup_path_;
  std::filesystem::path manifest_path_;
  std::shared_ptr<std::filesystem::path> staging_root_;
  std::atomic<int> worker_phase_{0};
  bool install_started_ = false;
};

}  // namespace trg::update
