#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace trg::update {

unsigned long CurrentProcessId();
std::vector<std::string> CurrentProcessArguments();
std::filesystem::path CurrentExecutablePath();

bool HasOriginalAppImage(std::filesystem::path& path);
bool LaunchApplication(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                       const std::filesystem::path& working_directory, std::string& error,
                       bool clear_update_test_environment = false);
bool LaunchUpdateHelper(const std::filesystem::path& helper, const std::filesystem::path& job_file,
                        std::string& error);
bool ApplyAppImageUpdate(const std::filesystem::path& downloaded, const std::filesystem::path& appimage,
                         const std::vector<std::string>& arguments, std::string& error);
bool WaitForProcess(unsigned long pid, int timeout_ms, std::string& error);

}  // namespace trg::update
