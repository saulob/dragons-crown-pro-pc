#include "my_reports.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <thread>
#include <utility>

#include "imgui.h"
#include "imgui_internal.h"
#include "localization/ui_text.h"
#include "net/online_version.h"
#include "online_settings.h"

namespace dcp {
namespace {

constexpr float kSentReportHeight = 300.0f;
constexpr float kFramePadding = 12.0f;
constexpr float kFrameRounding = 12.0f;
constexpr float kDetailGap = 8.0f;
constexpr float kRefreshPadding = 16.0f;
constexpr float kActionShare = 0.4f;
constexpr float kFactLabelShare = 0.28f, kFactLabelMin = 130.0f, kFactLabelMax = 220.0f;
constexpr float kHeaderRoom = 140.0f;
constexpr float kScrollRatio = 0.1f;
constexpr auto kFreshCheck = std::chrono::seconds(30);
constexpr char kDot[] = " \xC2\xB7 ";

std::string DateText(int64_t milliseconds) {
    return LogDateText(std::time_t(milliseconds / 1000), localization::PortugueseBrazilSelected());
}

net::OnlineConfig LobbyConfig(trg::Ui& ui) {
    net::OnlineConfig config = OnlineConfigOf(ui.settings());
    net::apply_environment(config);
    return config;
}

void HelpLine(const ImVec4& color, const std::string& text, bool wrap = false) {
    ImGui::PushFont(nullptr, trg::layout::kTextHelp);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    if (wrap) ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str());
    if (wrap) ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

std::string StatusText(const TrackedReport& report) {
    return ReportStatusLine(report) + (report.unseen ? kDot + std::string(Tr("Updated")) : std::string());
}

ImU32 Tint(const ImVec4& color, float alpha) { return ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, color.w * alpha)); }

// Drawn on the layer behind the report's content, which is only measured once it has been laid out.
void DrawFrame(trg::Ui& ui, const ImVec2& min, const ImVec2& max) {
    const float scale = ui.scale();
    const trg::Theme& theme = ui.theme();
    const float line = trg::layout::PanelBorderWidth(scale);
    const float rounding = kFrameRounding * scale;
    const ImVec2 outer(std::floor(min.x), std::floor(min.y)), outer_end(std::floor(max.x), std::floor(max.y));
    const ImVec2 inner(outer.x + line, outer.y + line), inner_end(outer_end.x - line, outer_end.y - line);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(outer, outer_end, Tint(theme.border_panel, 1.0f), rounding);
    draw->AddRectFilled(inner, inner_end, Tint(theme.panel, 1.0f), rounding - line);
    draw->AddRectFilled(inner, inner_end, Tint(theme.frame, 0.35f), rounding - line);
}

void KeepInView(float top, float scale) {
    const ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (top < window->InnerRect.Min.y || top > window->InnerRect.Max.y - kHeaderRoom * scale) ImGui::SetScrollHereY(kScrollRatio);
}

void DrawSummary(trg::Ui& ui, const TrackedReport& report, std::string& toggled) {
    const float scale = ui.scale();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float room = ImGui::GetContentRegionAvail().x;
    const float action = std::min(trg::layout::ControlColumnWidth(room, scale), room * kActionShare);
    if (!ImGui::BeginTable("##summary", 2, ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("summary", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, action);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    const float top = ImGui::GetCursorPosY();
    ImGui::PushFont(ui.fonts().semibold, trg::layout::kTextLabel);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, trg::layout::kLabelGap * scale));
    ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().text);
    ImGui::TextUnformatted(report.ticket.c_str());
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::PopFont();
    HelpLine(ui.theme().dim, ReportPreview(report.description), true);
    HelpLine(ui.theme().accent, StatusText(report), true);
    HelpLine(ui.theme().dim, DateText(report.submitted_at), true);
    const float height = ImGui::GetCursorPosY() - style.ItemSpacing.y - top;
    ImGui::TableSetColumnIndex(1);
    const float spare = height - ImGui::GetFrameHeight();
    if (spare > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + spare * 0.5f);
    if (ImGui::Button(Tr("Hide details"), ImVec2(-FLT_MIN, 0))) toggled = report.ticket;
    ImGui::EndTable();
}

void Fact(trg::Ui& ui, const char* label, const std::string& value, bool emphasized = false) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::PushFont(ui.fonts().semibold, trg::layout::kTextBody);
    ImGui::TextUnformatted(Tr(label));
    ImGui::PopFont();
    ImGui::TableSetColumnIndex(1);
    ImGui::PushStyleColor(ImGuiCol_Text, emphasized ? ui.theme().text : ui.theme().dim);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void DrawFacts(trg::Ui& ui, const TrackedReport& report) {
    const float scale = ui.scale();
    const float labels = std::clamp(ImGui::GetContentRegionAvail().x * kFactLabelShare, kFactLabelMin * scale, kFactLabelMax * scale);
    if (!ImGui::BeginTable("##facts", 2, ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, labels);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    const char* status = ReportStatusText(report.status);
    Fact(ui, "Report ID", report.ticket);
    Fact(ui, "Submitted", DateText(report.submitted_at));
    Fact(ui, "Description", report.description);
    Fact(ui, "Selected log", report.log_file);
    Fact(ui, "Game build", report.game_build);
    Fact(ui, "Launcher version", report.launcher_version);
    Fact(ui, "Status", status ? Tr(status) : report.status);
    Fact(ui, "Status updated", DateText(report.status_updated_at));
    if (report.status == "fixed" && !report.fixed_version.empty()) Fact(ui, "Fixed in version", report.fixed_version, true);
    if (!report.player_message.empty()) Fact(ui, "Message from developer", report.player_message, true);
    ImGui::EndTable();
}

} // namespace

MyReports::MyReports(std::filesystem::path history_file) : history_(std::move(history_file)) { history_.Load(); }

void MyReports::Refresh(const net::OnlineConfig& config, bool asked_by_player) {
    Collect();
    if (check_ || history_.reports().empty()) return;
    check_ = std::make_shared<StatusCheck>();
    last_check_ = std::chrono::steady_clock::now();
    asked_by_player_ = asked_by_player;
    std::thread([check = check_, config, reports = history_.reports()] {
        check->statuses = FetchReportStatuses(config, reports);
        check->done = true;
    }).detach();
}

void MyReports::RefreshIfStale(const net::OnlineConfig& config) {
    if (!last_check_ || std::chrono::steady_clock::now() - *last_check_ >= kFreshCheck) Refresh(config);
}

void MyReports::Collect() {
    if (reading_ && reading_->done) {
        if (reading_->ticket == sent_ticket_) {
            sent_unavailable_ = !reading_->package;
            sent_text_ = reading_->package ? IncidentPreviewText(*reading_->package) : std::string();
        }
        reading_.reset();
    }
    if (!check_ || !check_->done) return;
    check_failed_ = asked_by_player_ && !check_->statuses;
    if (check_->statuses && history_.Apply(*check_->statuses)) SaveHistory();
    check_.reset();
}

void MyReports::SaveHistory() {
    if (!history_.Save()) std::printf("Bug report: could not save My reports\n");
}

bool MyReports::Remember(const IncidentPackage& package, const IncidentReceipt& receipt) {
    if (receipt.credential.empty()) return false;
    if (history_.Add(TrackedReportOf(package, receipt))) return true;
    std::printf("Bug report: could not save My reports\n");
    return false;
}

bool MyReports::HasUpdates() {
    Collect();
    return history_.HasUnseen();
}

void MyReports::ForgetSentReport() {
    sent_ticket_.clear();
    sent_text_.clear();
    sent_unavailable_ = false;
}

void MyReports::Collapse() {
    open_ticket_.clear();
    ForgetSentReport();
}

void MyReports::Open(const std::string& ticket) {
    if (open_ticket_ == ticket) {
        Collapse();
        return;
    }
    open_ticket_ = ticket;
    bring_detail_into_view_ = true;
    ForgetSentReport();
}

void MyReports::ReadSentReport(const TrackedReport& report, const net::OnlineConfig& config) {
    sent_ticket_ = report.ticket;
    sent_text_.clear();
    sent_unavailable_ = false;
    reading_ = std::make_shared<SentReportRead>();
    reading_->ticket = report.ticket;
    std::thread([read = reading_, config, report] {
        read->package = FetchSentReport(config, report);
        read->done = true;
    }).detach();
}

void MyReports::Draw(trg::Ui& ui) {
    Collect();
    DrawHeader(ui);
    if (history_.reports().empty()) {
        ui.Paragraph("No reports sent from this PC");
        return;
    }
    DrawReports(ui);
}

void MyReports::DrawHeader(trg::Ui& ui) {
    ui.EndRows();
    const float scale = ui.scale();
    const bool any = !history_.reports().empty();
    ImGui::Dummy(ImVec2(0, trg::layout::kSectionGap * scale));
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(ui.fonts().semibold, trg::layout::kTextSection);
    const char* title = Tr("My reports");
    const float title_width = ImGui::CalcTextSize(title).x;
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (std::exchange(bring_into_view_, false)) ImGui::SetScrollHereY(0.0f);
    if (any) {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float width = std::max(ImGui::CalcTextSize(Tr("Refresh")).x, ImGui::CalcTextSize(Tr("Checking...")).x) +
                            style.FramePadding.x * 2 + kRefreshPadding * scale;
        const float room = ImGui::GetContentRegionAvail().x;
        if (room >= title_width + style.ItemSpacing.x + width) ImGui::SameLine(ImGui::GetCursorPosX() + room - width);
        ImGui::BeginDisabled(check_ != nullptr);
        if (ImGui::Button(Tr(check_ ? "Checking..." : "Refresh"), ImVec2(width, 0))) Refresh(LobbyConfig(ui), true);
        ImGui::EndDisabled();
    }
    ui.Help("Only reports sent from this PC are listed here.");
    if (check_failed_) HelpLine(ui.theme().warn_dot, Tr("Could not check for updates. The last known status is shown."));
}

void MyReports::DrawReports(trg::Ui& ui) {
    const std::vector<TrackedReport>& reports = history_.reports();
    std::string toggled, removed;
    for (size_t i = 0; i < reports.size(); ++i) {
        if (reports[i].ticket == open_ticket_) DrawExpanded(ui, reports[i], i, toggled, removed);
        else DrawRow(ui, reports[i], i, toggled);
    }
    if (!toggled.empty()) Open(toggled);
    if (removed.empty()) return;
    if (removed == open_ticket_) Collapse();
    history_.Remove(removed);
    SaveHistory();
}

void MyReports::DrawRow(trg::Ui& ui, const TrackedReport& report, size_t index, std::string& toggled) {
    const std::string preview = ReportPreview(report.description);
    ui.RowLeft(report.ticket.c_str(), preview.c_str());
    HelpLine(ui.theme().accent, StatusText(report));
    HelpLine(ui.theme().dim, DateText(report.submitted_at));
    ui.RowControl();
    ImGui::PushID(int(index));
    if (ImGui::Button(Tr("View details"), ImVec2(-FLT_MIN, 0))) toggled = report.ticket;
    ImGui::PopID();
}

void MyReports::DrawExpanded(trg::Ui& ui, const TrackedReport& report, size_t index, std::string& toggled, std::string& removed) {
    ui.EndRows();
    if (history_.MarkSeen(report.ticket)) SaveHistory();
    const float scale = ui.scale();
    const float pad = kFramePadding * scale;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImDrawListSplitter layers;
    layers.Split(draw, 2);
    layers.SetCurrentChannel(draw, 1);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    if (std::exchange(bring_detail_into_view_, false)) KeepInView(origin.y, scale);

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float work_right = window->WorkRect.Max.x, content_right = window->ContentRegionRect.Max.x;
    ImGui::PushID(int(index));
    ImGui::Dummy(ImVec2(0, pad));
    ImGui::Indent(pad);
    window->WorkRect.Max.x -= pad;
    window->ContentRegionRect.Max.x -= pad;

    DrawSummary(ui, report, toggled);
    ImGui::Dummy(ImVec2(0, kDetailGap * scale));
    DrawFacts(ui, report);
    ImGui::Dummy(ImVec2(0, kDetailGap * scale));
    DrawActions(ui, report, removed);
    if (sent_ticket_ == report.ticket) DrawSentReport(ui);
    ImGui::Dummy(ImVec2(0, pad));
    const float bottom = ImGui::GetItemRectMax().y;

    window->WorkRect.Max.x = work_right;
    window->ContentRegionRect.Max.x = content_right;
    ImGui::Unindent(pad);
    layers.SetCurrentChannel(draw, 0);
    DrawFrame(ui, origin, ImVec2(origin.x + width, bottom));
    layers.Merge(draw);
    ImGui::PopID();
}

void MyReports::DrawActions(trg::Ui& ui, const TrackedReport& report, std::string& removed) {
    const bool shown = sent_ticket_ == report.ticket;
    ImGui::BeginDisabled(reading_ != nullptr);
    const char* label = reading_ ? "Loading..." : shown ? "Hide report" : "What was sent";
    const bool toggle_sent = ImGui::Button(Tr(label));
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ConfirmRemoval(ui)) removed = report.ticket;
    HelpLine(ui.theme().dim, Tr("Removes it from this list only. The report stays with the developer."));
    if (!toggle_sent) return;
    if (shown) ForgetSentReport();
    else ReadSentReport(report, LobbyConfig(ui));
}

void MyReports::DrawSentReport(trg::Ui& ui) {
    if (sent_unavailable_) HelpLine(ui.theme().warn_dot, Tr("This report is no longer available online."));
    if (sent_text_.empty()) return;
    // ReadOnly: ImGui never writes to the text it is given.
    ImGui::InputTextMultiline("##sent", sent_text_.data(), sent_text_.size() + 1, ImVec2(-FLT_MIN, kSentReportHeight * ui.scale()),
                              ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
}

bool MyReports::ConfirmRemoval(trg::Ui& ui) {
    const float scale = ui.scale();
    if (ImGui::Button(Tr("Remove from this PC"))) ImGui::OpenPopup("##remove");
    bool confirmed = false;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * scale, 20 * scale));
    const bool open = ImGui::BeginPopupModal("##remove", nullptr,
                                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    if (!open) return false;
    ImGui::PushFont(ui.fonts().semibold, 0.0f);
    ImGui::TextUnformatted(Tr("Remove this report from this PC?"));
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(320 * scale, 6 * scale));
    if (ui.AccentButton("Remove", ImVec2(150 * scale, 0))) {
        confirmed = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr("Cancel"), ImVec2(150 * scale, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return confirmed;
}

} // namespace dcp
