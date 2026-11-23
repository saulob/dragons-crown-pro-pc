#include "home_stats.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include <trg/download.h>
#include <trg/platform.h>

#include "updater/json.h"
#include "updater/url_policy.h"

namespace dcp::home {
namespace {

using trg::update::json::Value;

constexpr long long kMaxResponseBytes = 8ll << 20;

std::string Lower(std::string_view text) {
    std::string lower(text);
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') {
            c = char(c - 'A' + 'a');
        }
    }
    return lower;
}

bool EndsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

std::optional<int64_t> Count(const Value* value) {
    if (!value || !value->IsNumber()) {
        return std::nullopt;
    }
    const std::optional<int64_t> number = value->AsInt64();
    return number && *number >= 0 ? number : std::nullopt;
}

const Value* Member(const Value& object, std::string_view section, std::string_view key) {
    const Value* inner = object.Find(section);
    return inner && inner->IsObject() ? inner->Find(key) : nullptr;
}

bool IsClassName(std::string_view name) {
    for (const char* known : {"Fighter", "Amazon", "Elf", "Dwarf", "Sorceress", "Wizard"}) {
        if (name == known) {
            return true;
        }
    }
    return false;
}

// Rows of {key: text, "count": n}; nullopt when the list or any row is not one, so a bad row never shows as a smaller list.
template <typename Row, typename Valid>
std::optional<std::vector<Row>> ParseRows(const Value* list, const char* key, Valid valid) {
    if (!list || !list->IsArray()) {
        return std::nullopt;
    }
    std::vector<Row> rows;
    for (const Value& item : list->AsArray()) {
        const Value* text = item.IsObject() ? item.Find(key) : nullptr;
        const std::optional<int64_t> count = item.IsObject() ? Count(item.Find("count")) : std::nullopt;
        if (!text || !text->IsString() || !count || *count <= 0 || !valid(text->AsString())) {
            return std::nullopt;
        }
        rows.push_back({text->AsString(), *count});
    }
    return rows;
}

std::optional<std::vector<ClassCount>> ParseClasses(const Value* list) {
    return ParseRows<ClassCount>(list, "class", IsClassName);
}

// "https://host[:port]" of an address, lower case; "" when it has none.
std::string OriginOf(std::string_view url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string_view::npos) {
        return "";
    }
    const size_t end = url.find_first_of("/?#", scheme + 3);
    return Lower(url.substr(0, end == std::string_view::npos ? url.size() : end));
}

} // namespace

bool CountsAsDownload(std::string_view asset_name) {
    const std::string name = Lower(asset_name);
    if (name.empty()) {
        return false;
    }
    for (const char* suffix : {".sha256", ".sha512", ".sha1", ".md5", ".sig", ".asc", ".minisig"}) {
        if (EndsWith(name, suffix)) {
            return false;
        }
    }
    const std::string stem = name.substr(0, name.find('.'));
    for (const char* manifest : {"sha256sums", "sha256sum", "sha512sums", "checksums", "checksum"}) {
        if (stem == manifest) {
            return false;
        }
    }
    return true;
}

std::optional<ReleasePage> SumReleaseDownloads(std::string_view json) {
    const std::optional<Value> root = trg::update::json::Parse(json);
    if (!root || !root->IsArray()) {
        return std::nullopt;
    }
    ReleasePage page;
    for (const Value& release : root->AsArray()) {
        if (!release.IsObject()) {
            return std::nullopt;
        }
        ++page.releases;
        const Value* draft = release.Find("draft");
        if (draft && (!draft->IsBool() || draft->AsBool())) {
            continue;
        }
        const Value* assets = release.Find("assets");
        if (!assets || !assets->IsArray()) {
            continue;
        }
        for (const Value& asset : assets->AsArray()) {
            const Value* name = asset.IsObject() ? asset.Find("name") : nullptr;
            const std::optional<int64_t> count = asset.IsObject() ? Count(asset.Find("download_count")) : std::nullopt;
            if (name && name->IsString() && count && CountsAsDownload(name->AsString())) {
                page.downloads += static_cast<uint64_t>(*count);
            }
        }
    }
    return page;
}

std::string ReleasesPageUrl(std::string_view releases_url, int page) {
    return std::string(releases_url) + "?per_page=" + std::to_string(kReleasesPerPage) + "&page=" + std::to_string(page);
}

std::optional<ServiceStats> ParseServiceStats(std::string_view json) {
    const std::optional<Value> root = trg::update::json::Parse(json);
    if (!root || !root->IsObject()) {
        return std::nullopt;
    }
    const auto field = [&](const char* section, const char* key) { return Count(Member(*root, section, key)); };
    ServiceStats stats;
    const LiveStats live{field("live", "online"), field("live", "in_rooms"), field("live", "open_rooms"), field("live", "peak_today")};
    if (live.online_now || live.playing_online || live.open_games || live.peak_today) {
        stats.live = live;
    }
    const TodayStats today{field("today", "sessions"), field("today", "rooms_created"), field("today", "joins")};
    if (today.online_sessions || today.games_created || today.players_joined) {
        stats.today = today;
    }
    const MonthStats month{field("last_30_days", "rooms_with_guest"), ParseClasses(Member(*root, "last_30_days", "classes"))};
    if (month.multiplayer_sessions || month.classes) {
        stats.month = month;
    }
    const LifetimeStats lifetime{field("lifetime", "sessions"), field("lifetime", "players")};
    if (lifetime.sessions || lifetime.players) {
        stats.lifetime = lifetime;
    }
    if (!stats.live && !stats.today && !stats.month) {
        return std::nullopt;
    }
    return stats;
}

std::string StatsUrlForLobby(std::string_view lobby_url) {
    std::string_view rest;
    std::string scheme;
    if (lobby_url.substr(0, 6) == "wss://") {
        scheme = "https://";
        rest = lobby_url.substr(6);
    } else if (lobby_url.substr(0, 5) == "ws://") {
        scheme = "http://";
        rest = lobby_url.substr(5);
    } else {
        return "";
    }
    const std::string_view authority = rest.substr(0, rest.find_first_of("/?#"));
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        return "";
    }
    return scheme + std::string(authority) + "/stats";
}

std::string FormatCount(int64_t value, bool portuguese) {
    const std::string digits = std::to_string(value < 0 ? -value : value);
    std::string text;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) {
            text.push_back(portuguese ? '.' : ',');
        }
        text.push_back(digits[i]);
    }
    return value < 0 ? "-" + text : text;
}

std::optional<DownloadsCache> ReadDownloadsCache(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) {
        return std::nullopt;
    }
    std::optional<uint64_t> downloads;
    std::optional<int64_t> fetched;
    std::string line;
    while (std::getline(in, line)) {
        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, equals), value = line.substr(equals + 1);
        if (value.empty() || value.size() > 19 || value.find_first_not_of("0123456789") != std::string::npos) {
            continue;
        }
        if (key == "downloads") {
            downloads = std::stoull(value);
        } else if (key == "fetched") {
            fetched = std::stoll(value);
        }
    }
    if (!downloads || !fetched) {
        return std::nullopt;
    }
    return DownloadsCache{*downloads, *fetched};
}

bool WriteDownloadsCache(const std::filesystem::path& file, const DownloadsCache& cache) {
    std::error_code ec;
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), ec);
    }
    std::filesystem::path part = file;
    part += ".part";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        out << "downloads=" << cache.downloads << "\nfetched=" << cache.fetched_unix << "\n";
        if (!out.flush()) {
            std::filesystem::remove(part, ec);
            return false;
        }
    }
    std::filesystem::rename(part, file, ec);
    if (ec) {
        std::filesystem::remove(part, ec);
        return false;
    }
    return true;
}

bool RefreshDue(const Freshness& freshness, double now, double ttl) {
    if (freshness.failed_at && (!freshness.fetched_at || *freshness.failed_at >= *freshness.fetched_at)) {
        return now - *freshness.failed_at >= kRetryAfterFailureSeconds;
    }
    return !freshness.fetched_at || now - *freshness.fetched_at >= ttl;
}

PrimaryAction PrimaryActionFor(bool game_ready) { return game_ready ? PrimaryAction::kPlay : PrimaryAction::kInstallGame; }

Fetch HttpFetch(std::string user_agent) {
    return [user_agent = std::move(user_agent)](const std::string& url, std::string& body, trg::Task& task) {
        trg::HttpOptions options;
        options.user_agent = user_agent;
        options.operation = "home statistics";
        options.connect_timeout_ms = 10000;
        options.receive_timeout_ms = 15000;
        options.total_timeout_ms = 20000;
        options.max_bytes = kMaxResponseBytes;
        options.max_redirects = 0;
        const std::string origin = OriginOf(url);
        options.allow_url = [origin](const std::string& next) {
            return !origin.empty() && OriginOf(next) == origin;
        };
        if (trg::update::UrlHostAllowed(url, {"api.github.com"})) {
            options.headers = {{"Accept", "application/vnd.github+json"}, {"X-GitHub-Api-Version", "2022-11-28"}};
        }
        return trg::HttpGet(url, body, &task, options);
    };
}

struct HomeStats::Request {
    std::unique_ptr<trg::Task> task = std::make_unique<trg::Task>();
    std::shared_ptr<std::optional<uint64_t>> downloads = std::make_shared<std::optional<uint64_t>>();
    std::shared_ptr<std::optional<ServiceStats>> service = std::make_shared<std::optional<ServiceStats>>();

    ~Request() {
        task->Cancel();
        if (task->running()) {
            // The worker owns copies of what it reads; leaving its thread unjoined keeps a slow network from
            // holding up the launcher's exit.
            (void)task.release();
        }
    }
};

HomeStats::HomeStats(HomeSources sources, Fetch fetch) : sources_(std::move(sources)), fetch_(std::move(fetch)) {
    if (const std::optional<DownloadsCache> cached = ReadDownloadsCache(sources_.downloads_cache)) {
        view_.downloads = cached->downloads;
        cached_unix_ = cached->fetched_unix;
    }
}

HomeStats::~HomeStats() = default;

void HomeStats::Tick(double now, int64_t now_unix, bool visible) {
    if (cached_unix_) {
        const int64_t age = now_unix - *cached_unix_;
        if (age >= 0 && age < static_cast<int64_t>(kDownloadsTtlSeconds)) {
            downloads_.fetched_at = now - static_cast<double>(age);
        }
        cached_unix_.reset();
    }
    Collect(now, now_unix);
    if (visible && fetch_) {
        if (!downloads_request_ && RefreshDue(downloads_, now, kDownloadsTtlSeconds)) {
            StartDownloads();
        }
        if (!service_request_ && !sources_.stats_url.empty() && RefreshDue(service_, now, kServiceTtlSeconds)) {
            StartService();
        }
    }
    view_.downloads_loading = downloads_request_ != nullptr && !view_.downloads;
    view_.service_loading = service_request_ != nullptr && !view_.service;
}

void HomeStats::StartDownloads() {
    auto request = std::make_shared<Request>();
    const bool started = request->task->Start(
        "", [fetch = fetch_, base = sources_.releases_url, out = request->downloads](trg::Task& task) -> std::string {
            uint64_t total = 0;
            for (int page = 1; page <= kMaxReleasePages; ++page) {
                std::string body;
                if (std::string error = fetch(ReleasesPageUrl(base, page), body, task); !error.empty()) {
                    return error;
                }
                const std::optional<ReleasePage> counted = SumReleaseDownloads(body);
                if (!counted) {
                    return "The release list could not be read.";
                }
                total += counted->downloads;
                if (counted->releases < kReleasesPerPage) {
                    break;
                }
            }
            *out = total;
            return "";
        });
    if (started) {
        downloads_request_ = std::move(request);
    }
}

void HomeStats::StartService() {
    auto request = std::make_shared<Request>();
    const bool started = request->task->Start(
        "", [fetch = fetch_, url = sources_.stats_url, out = request->service](trg::Task& task) -> std::string {
            std::string body;
            if (std::string error = fetch(url, body, task); !error.empty()) {
                return error;
            }
            *out = ParseServiceStats(body);
            return *out ? "" : "The statistics could not be read.";
        });
    if (started) {
        service_request_ = std::move(request);
    }
}

void HomeStats::Collect(double now, int64_t now_unix) {
    if (downloads_request_) {
        const trg::Task::State state = downloads_request_->task->Finish();
        if (state != trg::Task::State::kIdle) {
            if (state == trg::Task::State::kDone && *downloads_request_->downloads) {
                view_.downloads = **downloads_request_->downloads;
                downloads_.fetched_at = now;
                WriteDownloadsCache(sources_.downloads_cache, {*view_.downloads, now_unix});
            } else {
                downloads_.failed_at = now;
            }
            downloads_request_.reset();
        }
    }
    if (service_request_) {
        const trg::Task::State state = service_request_->task->Finish();
        if (state != trg::Task::State::kIdle) {
            if (state == trg::Task::State::kDone && *service_request_->service) {
                view_.service = **service_request_->service;
                view_.service_updated_at = now;
                view_.service_failed = !view_.service->complete();
                service_.fetched_at = now;
            } else {
                view_.service_failed = true;
                service_.failed_at = now;
            }
            service_request_.reset();
        }
    }
}

} // namespace dcp::home
