// The Home page's numbers: GitHub release downloads and the online service's public aggregate statistics, fetched
// off the UI thread. Nothing here reads or writes the launcher settings or the online identity.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trg {
class Task;
}

namespace dcp::home {

inline constexpr char kReleasesUrl[] = "https://api.github.com/repos/saulob/dragons-crown-pro-pc/releases";
inline constexpr int kReleasesPerPage = 100;
inline constexpr int kMaxReleasePages = 10;
inline constexpr double kDownloadsTtlSeconds = 30 * 60;
inline constexpr double kServiceTtlSeconds = 60;
inline constexpr double kRetryAfterFailureSeconds = 5 * 60;

// A release asset people download to play: checksum and signature files are not.
bool CountsAsDownload(std::string_view asset_name);

struct ReleasePage {
    uint64_t downloads = 0;
    int releases = 0;  // every release on the page, drafts included, to know whether another page follows
};
// One page of GET /repos/{owner}/{repo}/releases. Drafts are skipped. nullopt when the page is not a release list.
std::optional<ReleasePage> SumReleaseDownloads(std::string_view json);
std::string ReleasesPageUrl(std::string_view releases_url, int page);

struct ClassCount {
    std::string name;  // Fighter, Amazon, Elf, Dwarf, Sorceress or Wizard
    int64_t count = 0;
};
// What the service calls online, in_rooms, open_rooms, peak_today; each value is empty when the service could not read it.
struct LiveStats {
    std::optional<int64_t> online_now, playing_online, open_games, peak_today;
};
// sessions, rooms_created, joins.
struct TodayStats {
    std::optional<int64_t> online_sessions, games_created, players_joined;
};
// rooms_with_guest, classes.
struct MonthStats {
    std::optional<int64_t> multiplayer_sessions;
    std::optional<std::vector<ClassCount>> classes;
};
// Totals since the service began tracking; an older service has none.
struct LifetimeStats {
    std::optional<int64_t> sessions, players;
};
// A section the service could not read, or answered with anything invalid, is empty: never zero.
struct ServiceStats {
    std::optional<LiveStats> live;
    std::optional<LifetimeStats> lifetime;
    std::optional<TodayStats> today;
    std::optional<MonthStats> month;
    bool complete() const {
        return live && live->online_now && live->playing_online && live->open_games && live->peak_today && today &&
               today->online_sessions && today->games_created && today->players_joined && month && month->multiplayer_sessions &&
               month->classes;
    }
};
// The lobby's GET /stats answer; nullopt when it is not one or no section of it is usable.
std::optional<ServiceStats> ParseServiceStats(std::string_view json);
// wss://host/... -> https://host/stats (ws:// -> http://, for a development lobby). "" for anything else.
std::string StatsUrlForLobby(std::string_view lobby_url);

std::string FormatCount(int64_t value, bool portuguese);

struct DownloadsCache {
    uint64_t downloads = 0;
    int64_t fetched_unix = 0;
};
std::optional<DownloadsCache> ReadDownloadsCache(const std::filesystem::path& file);
bool WriteDownloadsCache(const std::filesystem::path& file, const DownloadsCache& cache);

// When a source is fetched again: after its TTL, or after a pause once a request failed.
struct Freshness {
    std::optional<double> fetched_at;
    std::optional<double> failed_at;
};
bool RefreshDue(const Freshness& freshness, double now, double ttl);

enum class PrimaryAction { kPlay, kInstallGame };
PrimaryAction PrimaryActionFor(bool game_ready);

// Runs on a worker thread: GETs `url` into `body`, "" on success, else an error.
using Fetch = std::function<std::string(const std::string& url, std::string& body, trg::Task& task)>;
Fetch HttpFetch(std::string user_agent);

struct HomeSources {
    std::string releases_url = kReleasesUrl;
    std::string stats_url;  // "" leaves the multiplayer numbers out
    std::filesystem::path downloads_cache;
};

struct HomeView {
    std::optional<uint64_t> downloads;
    bool downloads_loading = false;
    std::optional<ServiceStats> service;
    bool service_loading = false;
    bool service_failed = false;  // the last request failed or came back incomplete; `service` keeps what is known
    std::optional<double> service_updated_at;
};

class HomeStats {
public:
    HomeStats(HomeSources sources, Fetch fetch);
    ~HomeStats();
    HomeStats(const HomeStats&) = delete;
    HomeStats& operator=(const HomeStats&) = delete;

    // Every frame: collects finished requests and, while Home is visible, starts the ones that are due.
    void Tick(double now, int64_t now_unix, bool visible);
    const HomeView& view() const { return view_; }

private:
    struct Request;
    void StartDownloads();
    void StartService();
    void Collect(double now, int64_t now_unix);

    HomeSources sources_;
    Fetch fetch_;
    HomeView view_;
    Freshness downloads_;
    Freshness service_;
    std::shared_ptr<Request> downloads_request_;
    std::shared_ptr<Request> service_request_;
    std::optional<int64_t> cached_unix_;
};

} // namespace dcp::home
