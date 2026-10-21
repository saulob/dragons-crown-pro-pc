#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace trg::update {

struct UpdateJob {
  unsigned long parent_pid = 0;
  std::filesystem::path install_dir;
  std::filesystem::path staging_dir;
  std::filesystem::path backup_dir;
  std::filesystem::path manifest_file;
  std::filesystem::path restart_executable;
  std::filesystem::path log_file;
  std::vector<std::string> restart_arguments;
};

bool WriteUpdateJob(const std::filesystem::path& file, const UpdateJob& job, std::string& error);
std::optional<UpdateJob> ReadUpdateJob(const std::filesystem::path& file, std::string& error);

}  // namespace trg::update
