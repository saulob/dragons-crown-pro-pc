#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "incident_report.h"
#include "my_reports.h"
#include "trg/launcher.h"

namespace dcp {

// The launcher's Bug Report page: report the problem, choose one game log, preview what goes out if wanted, send it.
class IncidentPage {
public:
    struct Places {
        std::filesystem::path logs_dir;
        std::filesystem::path running_log;
        std::filesystem::path user_dir;
        std::vector<std::filesystem::path> install_folders;
    };

    explicit IncidentPage(Places places);

    void Draw(trg::Ui& ui, const ReportFacts& facts);
    // Starts a status check of My reports and returns at once.
    void CheckReports(const net::OnlineConfig& config) { my_reports_.Refresh(config); }
    bool HasReportUpdates() { return my_reports_.HasUpdates(); }

private:
    // Shared with the sending thread, which may outlive the launcher window.
    struct Delivery {
        std::atomic<bool> done{false};
        IncidentReceipt receipt;
        std::shared_ptr<const IncidentPackage> package;
    };

    void FindLogs();
    void CollectDelivery();
    void ForgetPreview();
    bool AcceptDraft();
    std::shared_ptr<const IncidentPackage> BuildPackage(const ReportFacts& facts);
    void TogglePreview(const ReportFacts& facts);
    void Send(trg::Ui& ui, const ReportFacts& facts);
    void DrawDescription(trg::Ui& ui, bool sending);
    void DrawLogs(trg::Ui& ui, bool sending);
    void DrawPreview(trg::Ui& ui, const ReportFacts& facts, bool sending);
    void DrawSend(trg::Ui& ui, const ReportFacts& facts, bool sending);
    void DrawResult(trg::Ui& ui);

    Places places_;
    char description_[kMostDescriptionCharacters * 4 + 1] = {};
    std::vector<GameLog> logs_;
    bool logs_found_ = false;
    size_t selected_ = 0;
    DraftProblem problem_ = DraftProblem::kNone;
    double problem_time_ = 0.0;
    bool bring_problem_into_view_ = false;
    std::shared_ptr<const IncidentPackage> previewed_;
    std::string preview_text_;
    bool log_failed_ = false;
    std::shared_ptr<Delivery> delivery_;
    std::optional<IncidentReceipt> result_;
    bool bring_result_into_view_ = false;
    bool sent_report_remembered_ = true;
    MyReports my_reports_;
    int last_drawn_frame_ = -2;
};

} // namespace dcp
