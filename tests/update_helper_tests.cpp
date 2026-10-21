// Real trg-updater flow: parent wait, install, restart quoting and child environment cleanup.
// Run: dcp-update-helper-tests

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

class TempDir {
 public:
  TempDir() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() / ("DCP Update Test " + std::to_string(now));
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::filesystem::path path;
};

class ScopedEnvironment {
 public:
  ScopedEnvironment() {
    for (const wchar_t* name : kNames) {
      old_.push_back(Read(name));
      SetEnvironmentVariableW(name, L"test-value");
    }
  }
  ~ScopedEnvironment() {
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
      SetEnvironmentVariableW(kNames[i], old_[i] ? old_[i]->c_str() : nullptr);
    }
  }

 private:
  static constexpr const wchar_t* kNames[] = {
      L"DCP_UPDATE_TEST", L"DCP_UPDATE_TEST_CURRENT_VERSION", L"DCP_UPDATE_TEST_RELEASE_JSON",
      L"DCP_UPDATE_TEST_ASSET_SHA256"};

  static std::optional<std::wstring> Read(const wchar_t* name) {
    const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (length == 0) {
      return std::nullopt;
    }
    std::wstring value(size_t(length), L'\0');
    GetEnvironmentVariableW(name, value.data(), length);
    value.resize(size_t(length - 1));
    return value;
  }

  std::vector<std::optional<std::wstring>> old_;
};

void WriteFile(const std::filesystem::path& path, const std::string& content) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(content.data(), std::streamsize(content.size()));
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::wstring Quote(const std::wstring& argument) {
  if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    return argument;
  }
  std::wstring quoted = L"\"";
  size_t backslashes = 0;
  for (const wchar_t c : argument) {
    if (c == L'\\') {
      ++backslashes;
    } else if (c == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted += L'"';
      backslashes = 0;
    } else {
      quoted.append(backslashes, L'\\');
      backslashes = 0;
      quoted += c;
    }
  }
  quoted.append(backslashes * 2, L'\\');
  quoted += L'"';
  return quoted;
}

bool RunHelper(const std::filesystem::path& helper, const std::filesystem::path& job,
               const std::filesystem::path& working_directory, DWORD& exit_code, std::string& error) {
  std::wstring command = Quote(helper.wstring()) + L" --job " + Quote(job.wstring());
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      working_directory.c_str(), &startup, &process)) {
    error = "CreateProcessW failed (" + std::to_string(GetLastError()) + ")";
    return false;
  }
  WaitForSingleObject(process.hProcess, 30000);
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

bool WaitForMarker(const std::filesystem::path& path) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (std::filesystem::exists(path)) {
      const std::string text = ReadFile(path);
      if (text.find("done=1") != std::string::npos) {
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

bool SameFile(const std::filesystem::path& left, const std::filesystem::path& right) {
  return ReadFile(left) == ReadFile(right);
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  ScopedEnvironment environment;
  TempDir temp;
  const std::filesystem::path install = temp.path / "install";
  const std::filesystem::path staging = temp.path / "staging";
  const std::filesystem::path backup = temp.path / "backup";
  const std::filesystem::path manifest = temp.path / "manifest.txt";
  const std::filesystem::path job = temp.path / "job.txt";
  const std::filesystem::path log = temp.path / "trg-updater.log";
  const std::filesystem::path marker_output = temp.path / "marker output.txt";
  const std::filesystem::path parent_output = temp.path / "parent output.txt";
  const std::filesystem::path marker = DCP_MARKER_EXE;

  std::wstring parent_command = Quote(marker.wstring()) + L" --hold 750 " + Quote(parent_output.wstring());
  STARTUPINFOW parent_startup{};
  parent_startup.cb = sizeof(parent_startup);
  PROCESS_INFORMATION parent{};
  CHECK(CreateProcessW(marker.c_str(), parent_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       temp.path.c_str(), &parent_startup, &parent));
  CHECK(parent.dwProcessId != 0);

  std::filesystem::create_directories(install);
  std::filesystem::create_directories(staging);
  WriteFile(install / "app.exe", "DEVELOPMENT_BUILD");
  WriteFile(install / "user" / "dcp.ini", "player settings");
  WriteFile(install / "game" / "eboot.bin", "game files");
  WriteFile(install / "logs" / "session.log", "log");
  WriteFile(install / "notes.txt", "keep me");
  std::error_code ec;
  std::filesystem::copy_file(marker, staging / "app.exe", std::filesystem::copy_options::overwrite_existing, ec);
  CHECK(!ec);
  WriteFile(manifest, "app.exe\n");

  std::string job_text = "format=1\n";
  job_text += "parent_pid=" + std::to_string(parent.dwProcessId) + "\n";
  job_text += "install_dir=" + install.string() + "\n";
  job_text += "staging_dir=" + staging.string() + "\n";
  job_text += "backup_dir=" + backup.string() + "\n";
  job_text += "manifest=" + manifest.string() + "\n";
  job_text += "restart=" + (install / "app.exe").string() + "\n";
  job_text += "log=" + log.string() + "\n";
  job_text += "arg=" + marker_output.string() + "\n";
  job_text += "arg=--flag=with space\n";
  job_text += "arg=--launcher\n";
  WriteFile(job, job_text);

  DWORD exit_code = 1;
  std::string error;
  CHECK(RunHelper(DCP_UPDATER_EXE, job, temp.path, exit_code, error));
  if (!error.empty()) {
    std::printf("  helper: %s\n", error.c_str());
  }
  WaitForSingleObject(parent.hProcess, 5000);
  CloseHandle(parent.hThread);
  CloseHandle(parent.hProcess);
  CHECK(exit_code == 0);
  CHECK(WaitForMarker(marker_output));
  CHECK(SameFile(install / "app.exe", marker));

  const std::string marker_text = ReadFile(marker_output);
  CHECK(marker_text.find("arg1=" + marker_output.string()) != std::string::npos);
  CHECK(marker_text.find("arg2=--flag=with space") != std::string::npos);
  CHECK(marker_text.find("arg3=--launcher") != std::string::npos);
  CHECK(marker_text.find("env_DCP_UPDATE_TEST=absent") != std::string::npos);
  CHECK(marker_text.find("env_DCP_UPDATE_TEST_CURRENT_VERSION=absent") != std::string::npos);
  CHECK(marker_text.find("env_DCP_UPDATE_TEST_RELEASE_JSON=absent") != std::string::npos);
  CHECK(marker_text.find("env_DCP_UPDATE_TEST_ASSET_SHA256=absent") != std::string::npos);

  CHECK(ReadFile(install / "user" / "dcp.ini") == "player settings");
  CHECK(ReadFile(install / "game" / "eboot.bin") == "game files");
  CHECK(ReadFile(install / "logs" / "session.log") == "log");
  CHECK(ReadFile(install / "notes.txt") == "keep me");

  const std::string helper_log = ReadFile(log);
  CHECK(helper_log.find("install root: " + install.string()) != std::string::npos);
  CHECK(helper_log.find("waiting for parent pid ") != std::string::npos);
  CHECK(helper_log.find("parent exited") != std::string::npos);
  CHECK(helper_log.find("manifest entries: 1") != std::string::npos);
  CHECK(helper_log.find("backup start") != std::string::npos);
  CHECK(helper_log.find("backup complete") != std::string::npos);
  CHECK(helper_log.find("replacement complete") != std::string::npos);
  CHECK(helper_log.find("verification complete") != std::string::npos);
  CHECK(helper_log.find("restart complete") != std::string::npos);
  CHECK(helper_log.find("exit code: 0") != std::string::npos);

  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
