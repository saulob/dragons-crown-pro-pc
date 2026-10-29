#include "online_page.h"

#include <algorithm>
#include <cstdio>

#include "imgui.h"
#include "imgui_internal.h"
#include "online_settings.h"

namespace dcp {
namespace {

const char* kOnlineHelp =
    "Lets the game's own Online menu create and join rooms. Players meet in the lobby; the game itself runs directly "
    "between your PCs.";
const char* kUnavailableHelp =
    "Online play is not available for this game build. It is verified for the USA 1.00 and Japan 1.02 and 1.03 builds.";
const char* kNameHelp = "What other players see: up to 16 letters, digits, - or _.";
const char* kFriendCodeHelp = "Share this code so another player can add you.";
const char* kTestHelp = "Checks that you can reach the online service and that other players can reach you. Takes a few "
                        "seconds.";
const char* kUnreachable = "Cannot reach the online service right now. Run the Connection test for details.";

net::OnlineConfig ConfigOf(const trg::SettingsStore& settings) {
    net::OnlineConfig config;
    config.enabled = settings.GetBool(kOnlineEnabledKey, false);
    config.name = settings.Get(kOnlineNameKey, net::kDefaultOnlineName);
    config.stun = settings.Get(kOnlineStunKey, net::kDefaultStunServers);
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

} // namespace

OnlinePage::~OnlinePage() {
    if (worker_.joinable()) worker_.join();
}

std::string OnlinePage::Game() const {
    return build_ ? std::string(build_->title_id) + "-" + build_->version : "CUSA10454-1.00";
}

void OnlinePage::Tick(const trg::SettingsStore& settings, double now) {
    const net::OnlineConfig config = ConfigOf(settings);
    const bool on = config.enabled && Supported();
    if (on != was_on_) {
        was_on_ = on;
        ClearTest();
    }
    client_.Update(on, config, Game(), now);
}

void OnlinePage::RunTest(trg::Ui& ui) {
    if (testing_.exchange(true)) return;
    if (worker_.joinable()) worker_.join();
    const net::OnlineConfig config = ConfigOf(ui.settings());
    worker_ = std::thread([this, config, user_dir = user_dir_, game = Game()] {
        const net::Identity identity = net::load_identity(user_dir);
        net::ConnectionReport report = net::run_connection_test(config, identity, net::online_name(config.name), game);
        for (const std::string& detail : report.details) std::printf("Connection test: %s\n", detail.c_str());
        std::printf("Connection test: %s\n", report.verdict.c_str());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            test_ = {true, std::move(report)};
        }
        testing_ = false;
    });
}

void OnlinePage::ClearTest() {
    std::lock_guard<std::mutex> lock(mutex_);
    test_ = {};
}

void OnlinePage::Draw(trg::Ui& ui) {
    const OnlinePlaySwitch online = ResolveOnlinePlay(ui.settings().GetBool(kOnlineEnabledKey, false));
    const OnlineClient::State& state = client_.state();
    for (const OnlineSection section : OnlinePageSections(Supported(), online.on, state.incoming.size())) {
        switch (section) {
        case OnlineSection::kUnavailable: ui.Help(kUnavailableHelp); break;
        case OnlineSection::kOnlinePlay: DrawOnlinePlay(ui, online); break;
        case OnlineSection::kFriendRequests: DrawFriendRequests(ui); break;
        case OnlineSection::kOnlineName: ui.InputText("Online name", kNameHelp, kOnlineNameKey, net::kDefaultOnlineName); break;
        case OnlineSection::kFriendCode: DrawFriendCode(ui); break;
        case OnlineSection::kConnectionTest: DrawConnectionTest(ui); break;
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
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f * s);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f * s);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * s, 10.0f * s));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(t.panel.x, t.panel.y, t.panel.z, 0.96f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.55f));
        ImGui::BeginChild("##card", ImVec2(-FLT_MIN, 0),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY);
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
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
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

void OnlinePage::DrawConnectionTest(trg::Ui& ui) {
    const bool testing = testing_;
    if (ui.ButtonRow("Connection test", kTestHelp, testing ? "Testing..." : "Run test") && !testing) RunTest(ui);
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
        ui.Row(f.name.c_str(), FriendPresence(f));
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
