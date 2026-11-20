#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "incident_report.h"
#include "report_history.h"
#include "trg/launcher.h"

namespace dcp {

// The Bug Report page's My reports section. Status checks run off the UI thread and never hold the launcher up.
class MyReports {
public:
    explicit MyReports(std::filesystem::path history_file);

    void Draw(trg::Ui& ui, const ReportFacts& facts);
    // Asks for the newest statuses unless a check is running, so callers may ask whenever it is useful. A failed check keeps
    // the last known statuses.
    void Refresh(const net::OnlineConfig& config, bool asked_by_player = false);
    // False when the report could not be added to the saved history; the report itself was sent.
    bool Remember(const IncidentPackage& package, const IncidentReceipt& receipt);
    bool HasUpdates();
    // An unseen developer message, which the page shows before a plain status change.
    bool HasNewReply();
    // Opens the report the page notice is about: a reply first, then the most recently updated.
    void ShowUpdate();
    void BringIntoView() { bring_into_view_ = true; }
    bool TakeNewReportRequest() { return std::exchange(new_report_requested_, false); }

private:
    // Shared with threads that may outlive the launcher window.
    struct StatusCheck {
        std::atomic<bool> done{false};
        std::optional<std::vector<ReportStatus>> statuses;
        std::vector<std::string> asked;
    };
    struct FeedbackSend {
        std::atomic<bool> done{false};
        std::string ticket;
        int64_t revision = 0;
        std::string result;
        FeedbackAnswer answer;
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
    void CollectFeedback();
    void SendFeedback(const TrackedReport& report, std::string_view result, const net::OnlineConfig& config);
    void ReadSentReport(const TrackedReport& report, const net::OnlineConfig& config);
    void DrawHeader(trg::Ui& ui);
    void DrawReports(trg::Ui& ui);
    void DrawRow(trg::Ui& ui, const TrackedReport& report, size_t index, std::string& toggled);
    void DrawExpanded(trg::Ui& ui, const TrackedReport& report, size_t index, std::string& toggled, std::string& removed);
    void DrawPlayerTest(trg::Ui& ui, const TrackedReport& report);
    void DrawActions(trg::Ui& ui, const TrackedReport& report, std::string& removed);
    void DrawSentReport(trg::Ui& ui);
    bool ConfirmRemoval(trg::Ui& ui);

    ReportHistory history_;
    std::shared_ptr<StatusCheck> check_;
    bool asked_by_player_ = false;
    bool check_failed_ = false;
    bool bring_into_view_ = false;
    bool bring_detail_into_view_ = false;
    std::string open_ticket_;
    // The open report whose developer message was unseen when it was opened; labelled "New reply" until it closes.
    std::string fresh_reply_;
    ReportFacts facts_;
    std::shared_ptr<FeedbackSend> feedback_;
    // The report whose last answer could not be sent, and why; cleared by the next try.
    std::string feedback_error_ticket_;
    FeedbackOutcome feedback_error_ = FeedbackOutcome::kFailed;
    bool refresh_wanted_ = false;
    bool new_report_requested_ = false;
    std::shared_ptr<SentReportRead> reading_;
    std::string sent_ticket_;
    std::string sent_text_;
    bool sent_unavailable_ = false;
};

} // namespace dcp
