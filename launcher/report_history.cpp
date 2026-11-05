#include "report_history.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>

#include "incident_report.h"
#include "localization/ui_text.h"
#include "net/json.h"

namespace dcp {
namespace fs = std::filesystem;
namespace {

constexpr int kHistoryVersion = 1;
constexpr size_t kPreviewCharacters = 60;

constexpr std::string_view kStatuses[][2] = {
    {"new", "New"},
    {"investigating", "Investigating"},
    {"fixed", "Fixed"},
    {"cannot_reproduce", "Cannot reproduce"},
    {"duplicate", "Duplicate"},
    {"not_a_bug", "Not a bug"},
    {"closed", "Closed"},
};

net::Json EntryJson(const TrackedReport& report) {
    net::Json entry = net::Json::Object();
    entry.set("ticket", report.ticket);
    entry.set("receipt", report.receipt);
    entry.set("submitted_at", report.submitted_at);
    entry.set("description", report.description);
    entry.set("log_file", report.log_file);
    entry.set("launcher_version", report.launcher_version);
    entry.set("game_build", report.game_build);
    entry.set("cusa", report.cusa);
    entry.set("operating_system", report.operating_system);
    entry.set("status", report.status);
    entry.set("status_updated_at", report.status_updated_at);
    entry.set("fixed_version", report.fixed_version);
    entry.set("player_message", report.player_message);
    entry.set("unseen", report.unseen);
    return entry;
}

TrackedReport EntryOf(const net::Json& entry) {
    TrackedReport report;
    report.ticket = entry["ticket"].as_string();
    report.receipt = entry["receipt"].as_string();
    report.submitted_at = entry["submitted_at"].as_int();
    report.description = entry["description"].as_string();
    report.log_file = entry["log_file"].as_string();
    report.launcher_version = entry["launcher_version"].as_string();
    report.game_build = entry["game_build"].as_string();
    report.cusa = entry["cusa"].as_string();
    report.operating_system = entry["operating_system"].as_string();
    report.status = entry["status"].as_string();
    report.status_updated_at = entry["status_updated_at"].as_int();
    report.fixed_version = entry["fixed_version"].as_string();
    report.player_message = entry["player_message"].as_string();
    report.unseen = entry["unseen"].as_bool();
    return report;
}

void NewestFirst(std::vector<TrackedReport>& reports) {
    std::stable_sort(reports.begin(), reports.end(),
                     [](const TrackedReport& a, const TrackedReport& b) { return a.submitted_at > b.submitted_at; });
    if (reports.size() > kMostTrackedReports) reports.resize(kMostTrackedReports);
}

void SetAside(const fs::path& file) {
    fs::path damaged = file;
    damaged += ".damaged";
    std::error_code ec;
    fs::rename(file, damaged, ec);
    std::printf("Bug report: %s is damaged; My reports starts empty\n", file.filename().string().c_str());
}

} // namespace

const char* ReportStatusText(std::string_view status) {
    const auto found = std::find_if(std::begin(kStatuses), std::end(kStatuses), [&](const auto& pair) { return pair[0] == status; });
    return found == std::end(kStatuses) ? nullptr : (*found)[1].data();
}

std::string ReportStatusLine(const TrackedReport& report) {
    if (report.status == "fixed" && !report.fixed_version.empty()) return std::string(Tr("Fixed in version")) + " " + report.fixed_version;
    const char* text = ReportStatusText(report.status);
    return text ? Tr(text) : report.status;
}

std::string ReportPreview(std::string_view description) {
    std::string line(description.substr(0, BytesOfCharacters(description, kPreviewCharacters)));
    std::replace(line.begin(), line.end(), '\n', ' ');
    return line.size() < description.size() ? line + "..." : line;
}

void ReportHistory::Load() {
    reports_.clear();
    std::error_code ec;
    if (!fs::exists(file_, ec)) return;
    std::ifstream in(file_, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const std::optional<net::Json> root = net::Json::parse(text);
    if (!root || !root->is_object() || !(*root)["reports"].is_array()) {
        SetAside(file_);
        return;
    }
    for (const net::Json& entry : (*root)["reports"].items()) {
        TrackedReport report = EntryOf(entry);
        if (!report.ticket.empty() && !report.receipt.empty() && ReportStatusText(report.status)) reports_.push_back(std::move(report));
    }
    NewestFirst(reports_);
}

bool ReportHistory::Save() const {
    net::Json list = net::Json::Array();
    for (const TrackedReport& report : reports_) list.push(EntryJson(report));
    net::Json root = net::Json::Object();
    root.set("version", kHistoryVersion);
    root.set("reports", std::move(list));

    std::error_code ec;
    fs::create_directories(file_.parent_path(), ec);
    fs::path temporary = file_;
    temporary += ".new";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << root.dump();
        out.flush();
        if (!out) {
            out.close();
            fs::remove(temporary, ec);
            return false;
        }
    }
    fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write, ec);
    fs::rename(temporary, file_, ec);
    if (!ec) return true;
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return false;
}

bool ReportHistory::HasUnseen() const {
    return std::any_of(reports_.begin(), reports_.end(), [](const TrackedReport& report) { return report.unseen; });
}

bool ReportHistory::Add(TrackedReport report) {
    Remove(report.ticket);
    reports_.push_back(std::move(report));
    NewestFirst(reports_);
    return Save();
}

bool ReportHistory::Apply(const std::vector<ReportStatus>& statuses) {
    bool changed = false;
    for (const ReportStatus& status : statuses) {
        if (!ReportStatusText(status.status)) continue;
        const auto found = std::find_if(reports_.begin(), reports_.end(), [&](const TrackedReport& r) { return r.ticket == status.ticket; });
        if (found == reports_.end()) continue;
        if (found->status == status.status && found->status_updated_at == status.updated_at &&
            found->fixed_version == status.fixed_version && found->player_message == status.player_message) {
            continue;
        }
        found->status = status.status;
        found->status_updated_at = status.updated_at;
        found->fixed_version = status.fixed_version;
        found->player_message = status.player_message;
        found->unseen = true;
        changed = true;
    }
    return changed;
}

bool ReportHistory::MarkSeen(std::string_view ticket) {
    const auto found = std::find_if(reports_.begin(), reports_.end(), [&](const TrackedReport& r) { return r.ticket == ticket; });
    if (found == reports_.end() || !found->unseen) return false;
    found->unseen = false;
    return true;
}

bool ReportHistory::Remove(std::string_view ticket) {
    return std::erase_if(reports_, [&](const TrackedReport& r) { return r.ticket == ticket; }) > 0;
}

} // namespace dcp
