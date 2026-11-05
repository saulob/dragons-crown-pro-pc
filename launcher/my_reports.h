#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "incident_report.h"
#include "report_history.h"
#include "trg/launcher.h"

namespace dcp {

// The Bug Report page's My reports section. Status checks run off the UI thread and never hold the launcher up.
class MyReports {
public:
    explicit MyReports(std::filesystem::path history_file);

    void Draw(trg::Ui& ui);
    // Asks for the newest statuses unless a check is running. A failed check keeps the last known statuses.
    void Refresh(const net::OnlineConfig& config, bool asked_by_player = false);
    // The same, unless a check ran moments ago.
    void RefreshIfStale(const net::OnlineConfig& config);
    // False when the report could not be added to the saved history; the report itself was sent.
    bool Remember(const IncidentPackage& package, const IncidentReceipt& receipt);
    bool HasUpdates();
    void BringIntoView() { bring_into_view_ = true; }

private:
    // Shared with threads that may outlive the launcher window.
    struct StatusCheck {
        std::atomic<bool> done{false};
        std::optional<std::vector<ReportStatus>> statuses;
    };
    struct SentReportRead {
        std::atomic<bool> done{false};
        std::string ticket;
        std::optional<IncidentPackage> package;
    };

    void Collect();
    void SaveHistory();
    void Open(const std::string& ticket);
    void Collapse();
    void ForgetSentReport();
    void ReadSentReport(const TrackedReport& report, const net::OnlineConfig& config);
    void DrawHeader(trg::Ui& ui);
    void DrawReports(trg::Ui& ui);
    void DrawRow(trg::Ui& ui, const TrackedReport& report, size_t index, std::string& toggled);
    void DrawExpanded(trg::Ui& ui, const TrackedReport& report, size_t index, std::string& toggled, std::string& removed);
    void DrawActions(trg::Ui& ui, const TrackedReport& report, std::string& removed);
    void DrawSentReport(trg::Ui& ui);
    bool ConfirmRemoval(trg::Ui& ui);

    ReportHistory history_;
    std::shared_ptr<StatusCheck> check_;
    std::optional<std::chrono::steady_clock::time_point> last_check_;
    bool asked_by_player_ = false;
    bool check_failed_ = false;
    bool bring_into_view_ = false;
    bool bring_detail_into_view_ = false;
    std::string open_ticket_;
    std::shared_ptr<SentReportRead> reading_;
    std::string sent_ticket_;
    std::string sent_text_;
    bool sent_unavailable_ = false;
};

} // namespace dcp
