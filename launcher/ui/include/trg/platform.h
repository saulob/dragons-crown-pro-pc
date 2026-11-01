// Small platform helpers every launcher has needed: UTF-8 file access, a
// native "open file" dialog, opening a folder, the Shift-at-startup check and
// a worker thread for installs and downloads.
#pragma once

#include <atomic>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace trg {

// ---------------------------------------------------------------- files ---
FILE* OpenFile(const std::string& utf8_path, const char* mode);  // fopen with UTF-8 paths on Windows
long long FileSize(const std::string& utf8_path);                 // -1 when it cannot be opened
bool RemoveFile(const std::string& utf8_path);
bool MoveFileOver(const std::string& from, const std::string& to);  // rename over an existing file
std::string AbsolutePath(const std::string& utf8_path);           // with forward slashes

// --------------------------------------------------------------- dialogs ---
struct FileFilter {
  std::string name;      // "GameCube disc images"
  std::string patterns;  // "*.iso;*.gcm;*.ciso"
};

// The desktop's "open file" dialog: Windows' own, zenity or kdialog on Linux,
// AppleScript on macOS. Empty when cancelled or unavailable. An "All files"
// filter is always added at the end.
std::string BrowseForFile(const std::string& title, const std::vector<FileFilter>& filters = {});
std::string BrowseForFolder(const std::string& title);

// Shows a folder (or the folder holding a file, with it selected) in Explorer,
// Finder or the Linux file manager.
void OpenInFileManager(const std::string& utf8_path);
void OpenUrl(const std::string& url);

// True while Shift is held: the "bring the launcher back" override.
bool ShiftHeld();

// ------------------------------------------------------- background work ---
// Runs one job at a time on a worker thread. The job reports progress through
// Task::Progress and checks Task::cancelled(); the UI polls state() each frame
// and calls Finish() once to collect the result.
class Task {
 public:
  enum class State { kIdle, kRunning, kDone, kFailed, kCancelled };
  using Job = std::function<std::string(Task&)>;  // returns "" on success, else an error message

  ~Task();

  // Starts `job` unless one is running. `label` is shown while it runs.
  bool Start(std::string label, Job job);
  void Cancel() { cancel_ = true; }
  bool running() const { return state_ == State::kRunning; }
  bool cancelled() const { return cancel_; }
  State state() const { return state_; }

  // Called from the job.
  void Progress(long long done, long long total) {
    done_ = done;
    total_ = total;
  }
  void SetLabel(std::string label);

  // 0..1, or negative while the total is unknown.
  float fraction() const;
  std::string label() const;

  // After the job ends: returns its final state once, joins the thread and
  // resets to kIdle. Returns kIdle while running or when there was no job.
  State Finish(std::string* error = nullptr);

 private:
  std::thread worker_;
  std::atomic<State> state_{State::kIdle};
  std::atomic<bool> cancel_{false};
  std::atomic<long long> done_{0}, total_{0};
  mutable std::mutex mu_;
  std::string label_, error_;
};

// Copies a file on a Task, through `<to>.part`, reporting progress. For "copy
// the disc image into the game folder" installs.
std::string CopyFileJob(Task& task, const std::string& from, const std::string& to);

}  // namespace trg
