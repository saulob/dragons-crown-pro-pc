// HTTPS downloads for launcher pages (shader packs, updates, ...). Windows uses
// WinHTTP; Linux and macOS use the system's curl. Nothing is contacted unless
// the game calls these, so a launcher stays offline until the player asks.
#pragma once

#include <string>
#include <vector>

namespace trg {

class Task;

struct HttpHeader {
  std::string name;
  std::string value;
};

struct HttpOptions {
  std::vector<HttpHeader> headers;
  std::string user_agent = "TRG-Launcher/1.1";
  int connect_timeout_ms = 15000;
  int receive_timeout_ms = 30000;
  int total_timeout_ms = 0;
  long long max_bytes = 0;
  long long progress_total = 0;
};

// GETs `url` (following redirects) into `out`. When `task` is given it reports
// progress and stops on Cancel. Returns "" on success, else an error message.
std::string HttpGet(const std::string& url, std::string& out, Task* task = nullptr,
                    const HttpOptions& options = {});

// Downloads `url` to `utf8_path` on a Task, through `<path>.part`.
std::string DownloadFile(Task& task, const std::string& url, const std::string& utf8_path,
                         const HttpOptions& options = {});

}  // namespace trg
