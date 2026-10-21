#include "updater/update_job.h"

#include <charconv>
#include <fstream>
#include <string_view>

#include "updater/update_paths.h"

namespace trg::update {
namespace {

bool ValidValue(const std::string& value) {
  return value.find_first_of("\r\n") == std::string::npos;
}

void AppendField(std::string& text, std::string_view key, const std::string& value) {
  text += key;
  text += '=';
  text += value;
  text += '\n';
}

}  // namespace

bool WriteUpdateJob(const std::filesystem::path& file, const UpdateJob& job, std::string& error) {
  error.clear();
  const std::string install = Utf8Path(job.install_dir);
  const std::string staging = Utf8Path(job.staging_dir);
  const std::string backup = Utf8Path(job.backup_dir);
  const std::string manifest = Utf8Path(job.manifest_file);
  const std::string restart = Utf8Path(job.restart_executable);
  const std::string log = Utf8Path(job.log_file);
  for (const std::string* value : {&install, &staging, &backup, &manifest, &restart, &log}) {
    if (!ValidValue(*value) || value->empty()) {
      error = "The update job contains an invalid path.";
      return false;
    }
  }
  for (const std::string& argument : job.restart_arguments) {
    if (!ValidValue(argument)) {
      error = "The update job contains an invalid argument.";
      return false;
    }
  }

  std::string text = "format=1\n";
  AppendField(text, "parent_pid", std::to_string(job.parent_pid));
  AppendField(text, "install_dir", install);
  AppendField(text, "staging_dir", staging);
  AppendField(text, "backup_dir", backup);
  AppendField(text, "manifest", manifest);
  AppendField(text, "restart", restart);
  AppendField(text, "log", log);
  for (const std::string& argument : job.restart_arguments) {
    AppendField(text, "arg", argument);
  }

  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "Cannot write the update job.";
    return false;
  }
  out.write(text.data(), std::streamsize(text.size()));
  out.close();
  if (!out) {
    error = "Cannot write the update job.";
    return false;
  }
  return true;
}

std::optional<UpdateJob> ReadUpdateJob(const std::filesystem::path& file, std::string& error) {
  error.clear();
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    error = "Cannot read the update job.";
    return std::nullopt;
  }

  UpdateJob job;
  bool format_seen = false;
  bool parent_seen = false;
  bool install_seen = false;
  bool staging_seen = false;
  bool backup_seen = false;
  bool manifest_seen = false;
  bool restart_seen = false;
  bool log_seen = false;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    const size_t equals = line.find('=');
    if (equals == std::string::npos) {
      error = "The update job is damaged.";
      return std::nullopt;
    }
    const std::string key = line.substr(0, equals);
    const std::string value = line.substr(equals + 1);
    if (key == "format") {
      if (value != "1") {
        error = "The update job format is not supported.";
        return std::nullopt;
      }
      format_seen = true;
    } else if (key == "parent_pid") {
      unsigned long pid = 0;
      const auto result = std::from_chars(value.data(), value.data() + value.size(), pid);
      if (result.ec != std::errc() || result.ptr != value.data() + value.size() || pid == 0) {
        error = "The update job has an invalid process id.";
        return std::nullopt;
      }
      job.parent_pid = pid;
      parent_seen = true;
    } else if (key == "install_dir") {
      job.install_dir = PathFromUtf8(value);
      install_seen = true;
    } else if (key == "staging_dir") {
      job.staging_dir = PathFromUtf8(value);
      staging_seen = true;
    } else if (key == "backup_dir") {
      job.backup_dir = PathFromUtf8(value);
      backup_seen = true;
    } else if (key == "manifest") {
      job.manifest_file = PathFromUtf8(value);
      manifest_seen = true;
    } else if (key == "restart") {
      job.restart_executable = PathFromUtf8(value);
      restart_seen = true;
    } else if (key == "log") {
      job.log_file = PathFromUtf8(value);
      log_seen = true;
    } else if (key == "arg") {
      job.restart_arguments.push_back(value);
    }
  }
  if (!format_seen || !parent_seen || !install_seen || !staging_seen || !backup_seen || !manifest_seen ||
      !restart_seen || !log_seen) {
    error = "The update job is incomplete.";
    return std::nullopt;
  }
  return job;
}

}  // namespace trg::update
