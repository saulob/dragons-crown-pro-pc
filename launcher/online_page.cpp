#include "online_page.h"

#include <algorithm>
#include <cstdio>
#include <exception>

#include "imgui.h"
#include "imgui_internal.h"
#include "net/online_version.h"
#include "online_settings.h"
#include "settings.h"

namespace dcp {
namespace {

const char* kOnlineHelp =
    "Lets the game's own Online menu create and join rooms. Players meet in the lobby; the game itself runs directly "
    "between your PCs.";
const char* kNameHelp = "What other players see: up to 16 letters, digits, - or _.";
const char* kFriendCodeHelp = "Share this code so another player can add you.";
const char* kTestHelp = "Checks that you can reach the online service and that other players can reach you. Takes a few "
                        "seconds.";
const char* kPingHelp = "Shows the round trip to the other players in game. F3 switches it while playing.";
const char* kUnreachable = "Cannot reach the online service right now. Run the Connection test for details.";

bool OnWindows() { return net::host_platform() == "Windows"; }

ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

void BeginCard(const ImVec4& background, const ImVec4& border, float border_width, ImGuiChildFlags flags = 0) {
    const float s = ImGui::GetFontSize() / 18.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, border_width * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * s, 10.0f * s));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, background);
    ImGui::PushStyleColor(ImGuiCol_Border, border);
    ImGui::BeginChild("##card", ImVec2(-FLT_MIN, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY | flags);
}

void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

net::OnlineConfig ConfigOf(const trg::SettingsStore& settings) {
    net::OnlineConfig config = OnlineConfigOf(settings);
    net::apply_environment(config);
    return config;
}

bool AcceptButton(trg::Ui& ui, const char* id) {
    const trg::Theme& t = ui.theme();
    ImGui::PushStyleColor(ImGuiCol_Button, t.accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, t.accent_hot);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, t.accent);
    ImGui::PushStyleColor(ImGuiCol_Text, t.on_accent);
    const float mark = ImGui::GetFontSize() * 0.8f;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::Button(id);
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    ImGui::RenderCheckMark(ImGui::GetWindowDrawList(), ImVec2(at.x + pad.x, at.y + pad.y + ImGui::GetFontSize() * 0.1f),
                           ImGui::GetColorU32(t.on_accent), mark);
    ImGui::PopStyleColor(4);
    return clicked;
}

// The name, then the status line in the row's help style with "Incompatible" in the attention colour. ui.Row has
// already moved to the control column, so the line is drawn back in the label column, under the name.
void FriendRow(trg::Ui& ui, const OnlinePlayer& player) {
    const FriendStatus status = FriendStatusOf(player);
    ui.Row(player.name.c_str());
    const bool in_table = ImGui::GetCurrentTable() != nullptr;
    if (in_table) {
        const float below_name = ImGui::GetItemRectMax().y + ImGui::GetStyle().ItemSpacing.y;
        ImGui::TableSetColumnIndex(0);
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, below_name));
    }
    ImGui::PushFont(nullptr, ui.fonts().size * 0.84f);
    ImGui::TextColored(ui.theme().dim, "%s", status.text.c_str());
    if (status.incompatible) {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(ui.theme().dim, " \xC2\xB7 ");
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(ui.theme().warn_dot, "Incompatible");
    }
    ImGui::PopFont();
    if (in_table) {
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
    }
}

} // namespace

OnlinePage::~OnlinePage() {
    if (worker_.joinable()) worker_.join();
}

void OnlinePage::Tick(LauncherSettings& settings, double now) {
    const net::OnlineConfig config = ConfigOf(settings);
    const bool on = config.enabled;
    if (on != was_on_) {
        was_on_ = on;
        ClearTest();
    }
    ForgetConnectionTestWhenOff(settings, on);
    RecordFinishedTest(settings, on);
    client_.Update(on, config, net::kOnlineGame, now);
}

bool OnlinePage::PlayNeedsTest(const trg::SettingsStore& settings) const {
    const net::OnlineConfig config = ConfigOf(settings);
    return ConnectionTestNeeded(settings, config.enabled, testing_, net::connection_test_port(config));
}

void OnlinePage::RunTest(trg::Ui& ui) {
    if (testing_.exchange(true)) return;
    if (worker_.joinable()) worker_.join();
    const net::OnlineConfig config = ConfigOf(ui.settings());
    const uint16_t port = net::connection_test_port(config);
    worker_ = std::thread([this, config, port, user_dir = user_dir_, game = std::string(net::kOnlineGame)] {
        net::ConnectionReport report;
        try {
            const net::Identity identity = net::load_identity(user_dir);
            report = net::run_connection_test(config, identity, net::online_name(config.name), game, port);
        } catch (const std::exception& e) {
            std::printf("Connection test: stopped: %s\n", e.what());
            testing_ = false;
            return;
        }
        for (const std::string& detail : report.details) std::printf("Connection test: %s\n", detail.c_str());
        std::printf("Connection test: %s\n", report.verdict.c_str());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            test_ = {true, std::move(report), port};
        }
        testing_ = false;
    });
}

void OnlinePage::RecordFinishedTest(LauncherSettings& settings, bool online_on) {
    uint16_t port = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!test_.done || test_.recorded) return;
        test_.recorded = true;
        port = test_.port;
    }
    if (online_on && !RecordConnectionTest(settings, port)) {
        std::printf("Connection test: could not save that it finished\n");
    }
}

void OnlinePage::ClearTest() {
    std::lock_guard<std::mutex> lock(mutex_);
    test_ = {};
}

void OnlinePage::Draw(trg::Ui& ui) {
    const OnlinePlaySwitch online = ResolveOnlinePlay(ui.settings().GetBool(kOnlineEnabledKey, false));
    const OnlineClient::State& state = client_.state();
    for (const OnlineSection section : OnlinePageSections(online.on, state.incoming.size())) {
        switch (section) {
        case OnlineSection::kOnlinePlay: DrawOnlinePlay(ui, online); break;
        case OnlineSection::kFriendRequests: DrawFriendRequests(ui); break;
        case OnlineSection::kOnlineName: ui.InputText("Online name", kNameHelp, kOnlineNameKey, net::kDefaultOnlineName); break;
        case OnlineSection::kFriendCode: DrawFriendCode(ui); break;
        case OnlineSection::kConnectionTest: DrawConnectionTest(ui); break;
        case OnlineSection::kPingDisplay: ui.Toggle("Ping display", kPingHelp, kPingDisplayKey, false, "Off", "On"); break;
        case OnlineSection::kFriends: DrawFriends(ui); break;
        }
    }
    ui.EndRows();
}

void OnlinePage::DrawOnlinePlay(trg::Ui& ui, const OnlinePlaySwitch& online) {
    if (!online.locked) {
        ui.Toggle("Online play", kOnlineHelp, kOnlineEnabledKey, false);
        return;
    }
    ui.Info("Online play", online.on ? "On" : "Off");
}

void OnlinePage::DrawFriendRequests(trg::Ui& ui) {
    const std::vector<OnlinePlayer> incoming = client_.state().incoming;
    const trg::Theme& t = ui.theme();
    const float s = ImGui::GetFontSize() / 18.0f;
    ui.Spacer(4);
    for (const OnlinePlayer& sender : incoming) {
        ImGui::PushID(("request" + sender.account).c_str());
        BeginCard(ImVec4(t.panel.x, t.panel.y, t.panel.z, 0.96f), ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.55f), 1.0f);
        ImGui::PushFont(ui.fonts().semibold, 0.0f);
        ImGui::TextUnformatted("New friend request");
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, t.dim);
        ImGui::TextUnformatted(FriendRequestText(sender).c_str());
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 2.0f * s));
        if (AcceptButton(ui, "     Accept")) client_.Accept(sender.account);
        ImGui::SameLine();
        if (ImGui::Button("Decline")) client_.Decline(sender.account);
        EndCard();
        ImGui::PopID();
        ui.Spacer(4);
    }
}

void OnlinePage::DrawFriendCode(trg::Ui& ui) {
    const OnlineClient::State& state = client_.state();
    ui.Row("Your friend code", kFriendCodeHelp);
    if (state.friend_code.empty()) {
        ImGui::TextDisabled("%s", state.link == OnlineClient::Link::kUnreachable ? "Not available right now" : "Connecting...");
        return;
    }
    ImGui::TextUnformatted(state.friend_code.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) {
        ImGui::SetClipboardText(state.friend_code.c_str());
        ui.SetStatus("Friend code copied.");
    }
}

bool OnlinePage::DrawRequiredTestRow(trg::Ui& ui, const ConnectionTestLook& look, const char* button, bool focus) {
    ui.Row("Connection test", kTestHelp);
    const bool in_table = ImGui::GetCurrentTable() != nullptr;
    if (in_table) {
        const float below_help = ImGui::GetItemRectMax().y + ImGui::GetStyle().ItemSpacing.y;
        ImGui::TableSetColumnIndex(0);
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, below_help));
    }
    ImGui::PushFont(ui.fonts().semibold, 0.0f);
    ImGui::TextColored(ui.theme().accent, "%s", look.required_text);
    ImGui::PopFont();
    if (in_table) {
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
    }
    ImGui::PushID("Connection test");
    if (focus) ImGui::SetKeyboardFocusHere();
    const bool clicked = look.primary_button ? ui.AccentButton(button) : ImGui::Button(button, ImVec2(-FLT_MIN, 0));
    ImGui::PopID();
    return clicked;
}

void OnlinePage::DrawConnectionTest(trg::Ui& ui) {
    const bool testing = testing_;
    const ConnectionTestLook look = ConnectionTestLookOf(PlayNeedsTest(ui.settings()));
    const char* button = testing ? "Testing..." : "Run test";
    const bool point = look.panel && pointed_;
    pointed_ = false;
    bool run = false;
    if (look.panel) {
        const trg::Theme& t = ui.theme();
        ui.EndRows();
        if (point) {
            pointed_at_ = ImGui::GetTime();
            ImGui::SetScrollHereY(0.3f);
        }
        const float emphasis = ConnectionTestEmphasis(ImGui::GetTime() - pointed_at_);
        const ImVec4 background = Mix(ImVec4(t.panel.x, t.panel.y, t.panel.z, 0.96f),
                                      ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.96f), 0.06f + 0.12f * emphasis);
        const ImVec4 border = Mix(ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.85f),
                                  ImVec4(t.accent_hot.x, t.accent_hot.y, t.accent_hot.z, 1.0f), emphasis);
        ui.Spacer(4);
        ImGui::PushID("connection-test");
        BeginCard(background, border, 1.5f + 1.5f * emphasis, ImGuiChildFlags_NavFlattened);
        run = DrawRequiredTestRow(ui, look, button, point);
        ui.EndRows();
        if (testing) {
            if (const char* note = ConnectionTestRunningNote(OnWindows())) ui.Help(note);
        }
        EndCard();
        ImGui::PopID();
        ui.Spacer(4);
    } else {
        run = ui.ButtonRow("Connection test", kTestHelp, button);
    }
    if (run && !testing) RunTest(ui);
    TestResult result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result = test_;
    }
    if (!result.done || testing) return;
    ui.EndRows();
    ui.Heading(result.report.verdict.c_str());
    for (const auto& line : result.report.lines) {
        using Mark = net::ConnectionReport::Mark;
        const ImVec4 color = line.mark == Mark::kOk ? ui.theme().good_dot : line.mark == Mark::kNote ? ui.theme().dim
                                                                                                     : ui.theme().warn_dot;
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(line.mark == Mark::kOk ? "OK" : line.mark == Mark::kNote ? "--" : "!!");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextWrapped("%s: %s", line.label.c_str(), line.text.c_str());
    }
    ui.Spacer();
}

void OnlinePage::DrawFriends(trg::Ui& ui) {
    const OnlineClient::State state = client_.state();
    ui.Heading("Friends");
    if (state.link == OnlineClient::Link::kUnreachable) {
        ui.Help(kUnreachable);
    } else if (!state.notice.empty()) {
        ui.Help(state.notice.c_str());
    }
    ui.Row("Add a friend", "Their friend code, for example ABCD-EFGH.");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
    ImGui::InputTextWithHint("##add_code", "XXXX-XXXX", add_code_, sizeof(add_code_), ImGuiInputTextFlags_CharsUppercase);
    ImGui::SameLine();
    if (ImGui::Button("Send request") && add_code_[0]) {
        client_.AddFriend(add_code_);
        add_code_[0] = 0;
    }
    for (const OnlinePlayer& r : state.outgoing) {
        ui.Row(r.name.c_str(), "Your request is waiting for an answer.");
        ImGui::PushID(("out" + r.account).c_str());
        if (ImGui::Button("Cancel request")) client_.Decline(r.account);
        ImGui::PopID();
    }
    for (const OnlinePlayer& f : state.friends) {
        FriendRow(ui, f);
        ImGui::PushID(("f" + f.account).c_str());
        if (ImGui::Button("Remove")) client_.Remove(f.account);
        ImGui::PopID();
    }
    if (state.fetched && state.friends.empty() && state.incoming.empty() && state.outgoing.empty()) {
        ui.EndRows();
        ui.Help("No friends yet. A friend in a room appears in the game's Join Friends Room list.");
    }
    ui.EndRows();
}

} // namespace dcp
