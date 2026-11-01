// Small game-side helpers every port has wanted (from King Kong Recompiled).
// They don't touch the launcher UI and need no engine: call them from the
// game's own startup and frame code, and log the strings they return.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace trg {

// Asks the OS for game-friendly scheduling. On Windows: 1 ms timer resolution
// (since Windows 10 2004 a process's short sleeps last ~15.6 ms unless it asks
// itself) and no power throttling (Windows 11 may otherwise run the game on
// efficiency cores and drop the timer request while the window is covered).
// Returns a line for the log with the measured length of a 1 ms sleep.
std::string TuneProcessScheduling();

// If the game crashes, writes crash-<date-time>.txt (what happened, in short)
// and crash-<date-time>.dmp (a minidump) into `utf8_logs_dir`, then lets any
// earlier crash handler run. Windows only; call after the engine has set up
// its own handlers.
void InstallCrashReports(const std::string& utf8_logs_dir, const std::string& game_name = "The game");

// Collects frame times and, every `window_seconds`, returns a summary such as
// "Frame times: 60.0 FPS average, 58.9 FPS 1% low, worst 21 ms, 0 over 50 ms,
// 0 over 100 ms". Feed it the time between presented frames.
class FrameTimeStats {
 public:
  explicit FrameTimeStats(double window_seconds = 10.0) : window_(window_seconds) {}
  std::optional<std::string> Add(double frame_ms);

 private:
  double window_;
  double elapsed_ms_ = 0;
  std::vector<double> times_;
};

}  // namespace trg
