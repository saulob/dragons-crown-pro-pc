#include "incident_page.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <thread>
#include <utility>

#include "imgui.h"
#include "imgui_internal.h"
#include "localization/ui_text.h"
#include "net/online_version.h"
#include "online_settings.h"
#include "page_margin.h"
#include "trg/platform.h"
#include "utf8.h"

namespace dcp {
namespace {

constexpr float kDescriptionHeight = 112.0f;
constexpr float kPreviewHeight = 300.0f;
constexpr float kAlertTint = 0.16f, kAlertFlashTint = 0.5f;
constexpr float kAlertFlashSeconds = 0.9f;
constexpr char kDot[] = " \xC2\xB7 ";

const char* kPrivacyHeadline = "No personal user data is intentionally included.";
const char* kPrivacyNote =
    "Before anything is sent, the launcher takes addresses, user and "
    "computer names, private folders, passwords, online secrets and other players' details out of the selected log. "
    "Automatic filtering cannot promise to catch everything, so open the preview if you want to check what goes out.";

int KeepDescriptionShort(ImGuiInputTextCallbackData* data) {
    const size_t keep = BytesOfCharacters(std::string_view(data->Buf, size_t(data->BufTextLen)), kMostDescriptionCharacters);
    if (keep < size_t(data->BufTextLen)) data->DeleteChars(int(keep), data->BufTextLen - int(keep));
    return 0;
}

std::string LogDetails(const GameLog& log) {
    return LogDateText(log.written, localization::PortugueseBrazilSelected()) + kDot + LogSizeText(log.bytes);
}

void AccentText(trg::Ui& ui, const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().accent);
    ImGui::TextUnformatted(Tr(text));
    ImGui::PopStyleColor();
}

void LogRoleLine(trg::Ui& ui, LogRole role) {
    const char* text = LogRoleText(role);
    if (!text) return;
    ImGui::PushFont(nullptr, trg::layout::kTextHelp);
    AccentText(ui, text);
    ImGui::PopFont();
}

ImVec4 AlertedColor(const ImVec4& color, const ImVec4& alert, double seconds) {
    const float flash = std::max(0.0f, 1.0f - float(seconds) / kAlertFlashSeconds);
    const float t = kAlertTint + (kAlertFlashTint - kAlertTint) * flash;
    return ImVec4(color.x + (alert.x - color.x) * t, color.y + (alert.y - color.y) * t, color.z + (alert.z - color.z) * t, color.w);
}

void WarningText(trg::Ui& ui, const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().warn_dot);
    ImGui::TextWrapped("%s", Tr(text));
    ImGui::PopStyleColor();
}

float SemiboldWidth(trg::Ui& ui, const char* text) {
    ImGui::PushFont(ui.fonts().semibold, trg::layout::kTextLabel);
    const float width = ImGui::CalcTextSize(Tr(text)).x;
    ImGui::PopFont();
    return width;
}

void FactLabel(trg::Ui& ui, const char* text) {
    ImGui::PushFont(ui.fonts().semibold, trg::layout::kTextLabel);
    ImGui::TextUnformatted(Tr(text));
    ImGui::PopFont();
}

void FactValue(trg::Ui& ui, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().dim);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void DrawFactLine(trg::Ui& ui, float left, float right, float y) {
    const float top = std::floor(y);
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(left, top), ImVec2(right, top + 1.0f), ImGui::GetColorU32(ui.theme().border_row));
}

// The launcher's two-pair grid (Ui::InfoGrid) with the button in its empty second-row cell, flush right like the page's other
// buttons. True when it was pressed.
bool DrawLogFacts(trg::Ui& ui, const GameLog& log) {
    const float scale = ui.scale();
    const float padding = trg::layout::kInfoGridPadding * scale;
    const std::string written = LogDateText(log.written, localization::PortugueseBrazilSelected());
    const std::string size = LogSizeText(log.bytes);
    const float left = ImGui::GetCursorScreenPos().x;
    const float room = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0, padding));
    if (!ImGui::BeginTable("##selected_log", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoClip)) {
        ImGui::PopStyleVar();
        return false;
    }
    const float weights[4] = {std::max(SemiboldWidth(ui, "Selected log"), SemiboldWidth(ui, "Size")),
                              std::max(ImGui::CalcTextSize(log.name.c_str()).x, ImGui::CalcTextSize(size.c_str()).x),
                              SemiboldWidth(ui, "Last written"), ImGui::CalcTextSize(written.c_str()).x};
    for (const float weight : weights) ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch, std::max(weight, 1.0f));
    ImGui::TableNextColumn();
    FactLabel(ui, "Selected log");
    ImGui::TableNextColumn();
    FactValue(ui, log.name);
    ImGui::TableNextColumn();
    FactLabel(ui, "Last written");
    ImGui::TableNextColumn();
    FactValue(ui, written);

    ImGui::TableNextColumn();
    const float second_row = ImGui::GetCursorScreenPos().y - padding;
    ImGui::AlignTextToFramePadding();
    FactLabel(ui, "Size");
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    FactValue(ui, size);
    ImGui::TableNextColumn();
    const ImVec2 cell = ImGui::GetCursorScreenPos();
    const float width = std::min(trg::layout::ControlColumnWidth(room, scale), left + room - cell.x);
    ImGui::SetCursorScreenPos(ImVec2(left + room - width, cell.y));
    const bool pressed = ImGui::Button(Tr("View selected log"), ImVec2(width, 0));
    ImGui::EndTable();
    ImGui::PopStyleVar();
    DrawFactLine(ui, left, left + room, second_row);
    DrawFactLine(ui, left, left + room, ImGui::GetItemRectMax().y);
    return pressed;
}

} // namespace

IncidentPage::IncidentPage(Places places) : places_(std::move(places)), my_reports_(places_.user_dir / kReportHistoryFile) {}

void IncidentPage::Draw(trg::Ui& ui, const ReportFacts& facts) {
    if (!logs_found_) FindLogs();
    CollectDelivery();
    if (ImGui::GetFrameCount() != std::exchange(last_drawn_frame_, ImGui::GetFrameCount()) + 1) {
        net::OnlineConfig config = OnlineConfigOf(ui.settings());
        net::apply_environment(config);
        my_reports_.RefreshIfStale(config);
    }
    const bool sending = delivery_ != nullptr;
    if (CheckDraft(description_, !logs_.empty()) != problem_) problem_ = DraftProblem::kNone;
    KeepClearOfScrollbar(std::floor(trg::layout::kContentPadding * ui.scale()));
    AccentText(ui, kPrivacyHeadline);
    ui.Paragraph(kPrivacyNote);
    if (my_reports_.HasUpdates()) {
        const bool reply = my_reports_.HasNewReply();
        if (ui.ButtonRow(reply ? "A report has a new reply" : "A report has an update",
                         reply ? "The developer replied to one of your reports." : "Its status changed since you last looked.", "Show")) {
            my_reports_.ShowUpdate();
        }
    }
    ui.Heading("1. Report the problem");
    DrawDescription(ui, sending);
    ui.Heading("2. Choose one game log");
    DrawLogs(ui, sending);
    ui.Heading("3. Preview what will be sent");
    DrawPreview(ui, facts, sending);
    ui.Heading("4. Send");
    DrawSend(ui, facts, sending);
    ui.EndRows();
    my_reports_.Draw(ui);
    ui.EndRows();
}

void IncidentPage::FindLogs() {
    logs_ = FindGameLogs(places_.logs_dir, places_.running_log);
    logs_found_ = true;
    selected_ = DefaultGameLog(logs_);
}

void IncidentPage::CollectDelivery() {
    if (!delivery_ || !delivery_->done) return;
    result_ = delivery_->receipt;
    if (result_->outcome == IncidentOutcome::kSent) sent_report_remembered_ = my_reports_.Remember(*delivery_->package, *result_);
    delivery_.reset();
    bring_result_into_view_ = true;
    if (result_->outcome == IncidentOutcome::kSent) {
        description_[0] = '\0';
        ForgetPreview();
    }
}

void IncidentPage::ForgetPreview() {
    previewed_.reset();
    preview_text_.clear();
    log_failed_ = false;
}

bool IncidentPage::AcceptDraft() {
    problem_ = CheckDraft(description_, !logs_.empty());
    problem_time_ = ImGui::GetTime();
    bring_problem_into_view_ = problem_ != DraftProblem::kNone;
    return problem_ == DraftProblem::kNone;
}

std::shared_ptr<const IncidentPackage> IncidentPage::BuildPackage(const ReportFacts& facts) {
    log_failed_ = false;
    const PrivateFacts private_facts = GatherPrivateFacts(places_.install_folders, places_.user_dir);
    std::optional<IncidentPackage> package = BuildIncidentPackage(description_, logs_[selected_], facts, private_facts);
    if (!package) {
        log_failed_ = true;
        bring_problem_into_view_ = true;
        FindLogs();
        return nullptr;
    }
    return std::make_shared<const IncidentPackage>(std::move(*package));
}

void IncidentPage::TogglePreview(const ReportFacts& facts) {
    if (previewed_) {
        ForgetPreview();
        return;
    }
    if (!AcceptDraft()) return;
    previewed_ = BuildPackage(facts);
    if (previewed_) preview_text_ = IncidentPreviewText(*previewed_);
}

void IncidentPage::Send(trg::Ui& ui, const ReportFacts& facts) {
    if (!AcceptDraft()) return;
    std::shared_ptr<const IncidentPackage> package = previewed_ ? previewed_ : BuildPackage(facts);
    if (!package) return;
    net::OnlineConfig config = OnlineConfigOf(ui.settings());
    net::apply_environment(config);
    result_.reset();
    delivery_ = std::make_shared<Delivery>();
    delivery_->package = package;
    bring_result_into_view_ = true;
    std::thread([delivery = delivery_, package = std::move(package), config = std::move(config), user_dir = places_.user_dir] {
        delivery->receipt = FileIncident(config, user_dir, net::kOnlineGame, *package);
        delivery->done = true;
    }).detach();
}

void IncidentPage::DrawDescription(trg::Ui& ui, bool sending) {
    const bool too_short = problem_ == DraftProblem::kDescriptionTooShort;
    if (too_short && std::exchange(bring_problem_into_view_, false)) ImGui::SetKeyboardFocusHere();
    const ImVec4 frame = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    ImGui::BeginDisabled(sending);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, too_short ? AlertedColor(frame, ui.theme().warn_dot, ImGui::GetTime() - problem_time_) : frame);
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_CallbackEdit;
    if (ImGui::InputTextMultiline("##description", description_, sizeof(description_),
                                  ImVec2(-FLT_MIN, kDescriptionHeight * ui.scale()), flags, KeepDescriptionShort)) {
        ForgetPreview();
    }
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    const size_t used = CountCharacters(description_);
    const std::string counter = std::to_string(used) + " / " + std::to_string(kMostDescriptionCharacters);
    ImGui::PushFont(nullptr, trg::layout::kTextHelp);
    ImGui::TextColored(used >= kMostDescriptionCharacters ? ui.theme().warn_dot : ui.theme().dim, "%s", counter.c_str());
    if (too_short) {
        ImGui::SameLine();
        ImGui::TextColored(ui.theme().warn_dot, "%s", Tr("Please describe the problem, minimum 10 characters"));
    }
    ImGui::PopFont();
    ui.Help("What were you doing, what went wrong, and does it happen every time?");
}

void IncidentPage::DrawLogs(trg::Ui& ui, bool sending) {
    if (logs_.empty()) {
        ui.Paragraph("No game log is available yet, so a report cannot be sent. Start the game once and try again. With "
                     "Detailed logging off on the About page, only sessions that crashed keep a log.");
        if (problem_ == DraftProblem::kNoLog) {
            if (std::exchange(bring_problem_into_view_, false)) ImGui::SetScrollHereY();
            WarningText(ui, "There is no game log to send.");
        }
        return;
    }
    ui.Help("The newest logs of the game, most recent first. Only the one you choose is sent.");
    for (size_t i = 0; i < logs_.size(); ++i) {
        const std::string details = LogDetails(logs_[i]);
        ui.RowLeft(logs_[i].name.c_str(), details.c_str());
        LogRoleLine(ui, logs_[i].role);
        ui.RowControl();
        ImGui::PushID(int(i));
        ImGui::BeginDisabled(sending);
        ImGui::AlignTextToFramePadding();
        if (ImGui::RadioButton(Tr("Send this log"), selected_ == i) && selected_ != i) {
            selected_ = i;
            ForgetPreview();
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ui.EndRows();
    if (log_failed_) {
        if (std::exchange(bring_problem_into_view_, false)) ImGui::SetScrollHereY();
        WarningText(ui, "That log could not be read. Choose another one and try again.");
    }
    const GameLog& log = logs_[selected_];
    if (DrawLogFacts(ui, log)) trg::OpenUrl(Utf8(log.path));
}

void IncidentPage::DrawPreview(trg::Ui& ui, const ReportFacts& facts, bool sending) {
    ImGui::BeginDisabled(sending);
    const bool clicked = ui.ButtonRow("Preview", "Shows exactly what will be sent, after the privacy filter.",
                                      PreviewButtonText(previewed_ != nullptr));
    ImGui::EndDisabled();
    if (clicked) TogglePreview(facts);
    ui.EndRows();
    if (previewed_) {
        // ReadOnly: ImGui never writes to the text it is given.
        ImGui::InputTextMultiline("##preview", preview_text_.data(), preview_text_.size() + 1,
                                  ImVec2(-FLT_MIN, kPreviewHeight * ui.scale()),
                                  ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
    }
}

void IncidentPage::DrawSend(trg::Ui& ui, const ReportFacts& facts, bool sending) {
    ImGui::BeginDisabled(sending);
    const bool clicked = ui.ButtonRow("Send report", "Sends the selected log after the privacy filter is applied.",
                                      sending ? "Sending..." : "Send report", true);
    ImGui::EndDisabled();
    if (clicked) Send(ui, facts);
    ui.EndRows();
    DrawResult(ui);
    if (std::exchange(bring_result_into_view_, false)) ImGui::SetScrollHereY(1.0f);
}

void IncidentPage::DrawResult(trg::Ui& ui) {
    if (delivery_) {
        ui.Spacer(4);
        ui.StatusCard(trg::Status::kBusy, "Sending your report...", "This takes a few seconds.");
        return;
    }
    if (!result_) return;
    ui.Spacer(4);
    switch (result_->outcome) {
    case IncidentOutcome::kSent: {
        const std::string id = std::string(Tr("Report ID:")) + " " + result_->ticket;
        ui.StatusCard(trg::Status::kReady, "Report sent successfully", id.c_str());
        ui.Spacer(4);
        if (ImGui::Button(Tr("Copy report ID"))) {
            ImGui::SetClipboardText(result_->ticket.c_str());
            ui.SetStatus("Report ID copied.");
        }
        if (!sent_report_remembered_) ui.Help("The report was sent, but it could not be added to My reports on this PC.");
        break;
    }
    case IncidentOutcome::kRecentLimit:
    case IncidentOutcome::kDailyLimit:
        ui.StatusCard(trg::Status::kAttention, IncidentLimitTitle(result_->outcome), "Please try again later");
        break;
    case IncidentOutcome::kUnreachable:
        ui.StatusCard(trg::Status::kAttention, "Cannot reach the online service",
                      "Check your connection and press Send report again; your text is kept.");
        break;
    case IncidentOutcome::kRefused:
        ui.StatusCard(trg::Status::kAttention, "The report was not accepted",
                      "Change the description or choose another log, then send it again.");
        break;
    case IncidentOutcome::kServiceError:
        ui.StatusCard(trg::Status::kAttention, "The online service could not take the report",
                      "This is not your fault. Try again in a few minutes; your text is kept.");
        break;
    }
}

} // namespace dcp
