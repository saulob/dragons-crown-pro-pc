#include "online_page.h"

#include <cstdio>
#include <cstdlib>

#include "imgui.h"
#include "net/lobby_link.h"
#include "online_settings.h"

namespace dcp {
namespace {

constexpr double kRefreshSeconds = 30.0;

const char* kOnlineHelp =
    "Lets the game's own Online menu create and join rooms. Players meet in the lobby; the game itself runs directly "
    "between your PCs.";
const char* kNameHelp = "What other players see: up to 16 letters, digits, - or _. Empty uses your Windows user name.";
const char* kLobbyHelp = "The lobby server address (ws:// or wss://). It only introduces players to each other.";
const char* kUpnpHelp = "Asks your router to open the game's port. Most networks also work without it.";

std::string Describe(const std::string& code) {
    if (code == "not_found") return "No player has that friend code.";
    if (code == "bad_request") return "That is not a valid friend code.";
    if (code == "limit") return "Too many friends or pending requests.";
    if (code == "timeout") return "The lobby did not answer.";
    return "The lobby refused it (" + code + ").";
}

net::OnlineConfig ConfigOf(trg::Ui& ui) {
    net::OnlineConfig config;
    config.enabled = ui.settings().GetBool(kOnlineEnabledKey, false);
    config.lobby = ui.settings().Get(kOnlineLobbyKey, net::kDefaultLobbyUrl);
    config.name = ui.settings().Get(kOnlineNameKey);
    config.upnp = ui.settings().GetBool(kOnlineUpnpKey, true);
    config.stun = ui.settings().Get(kOnlineStunKey, net::kDefaultStunServers);
    net::apply_environment(config);
    return config;
}

} // namespace

OnlinePage::~OnlinePage() {
    if (worker_.joinable()) worker_.join();
}

std::string OnlinePage::Game() const {
    return build_ ? std::string(build_->title_id) + "-" + build_->version : "CUSA10454-1.00";
}

void OnlinePage::Start(trg::Ui& ui, Job job, bool needs_lobby) {
    if (busy_.exchange(true)) return;
    if (worker_.joinable()) worker_.join();
    const net::OnlineConfig config = ConfigOf(ui);
    const std::filesystem::path user_dir = user_dir_;
    const std::string game = Game();
    worker_ = std::thread([this, config, user_dir, game, needs_lobby, job = std::move(job)] {
        Snapshot result;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            result = snapshot_;
        }
        result.notice.clear();
        const net::Identity identity = net::load_identity(user_dir);
        const std::string name = net::online_name(config.name.empty() ? net::system_user_name() : config.name);
        net::LobbySession session;
        net::Json hello;
        std::string error;
        if (!needs_lobby) {
            job(config, identity, name, game, session, result);
        } else if (!identity.ok()) {
            result.error = "No online identity: " + identity.error;
        } else if (config.lobby.empty()) {
            result.error = "Set a lobby server first.";
        } else if (!session.open(config.lobby, net::hello_body(identity, name, game, false), &hello, &error)) {
            result.error = "Cannot reach the lobby: " + error + ".";
        } else {
            result.error.clear();
            result.friend_code = hello["friend_code"].as_string();
            job(config, identity, name, game, session, result);
            std::string code;
            if (auto list = session.call("friends.list", net::Json::Object(), &code)) {
                result.friends.clear();
                for (const net::Json& f : (*list)["friends"].items())
                    result.friends.push_back({f["account"].as_string(), f["name"].as_string(), f["online"].as_bool(),
                                              f["room"].as_u64() != 0});
            }
            if (auto requests = session.call("friends.requests", net::Json::Object(), &code)) {
                result.incoming.clear();
                result.outgoing.clear();
                for (const net::Json& r : (*requests)["incoming"].items())
                    result.incoming.push_back({r["account"].as_string(), r["name"].as_string()});
                for (const net::Json& r : (*requests)["outgoing"].items())
                    result.outgoing.push_back({r["account"].as_string(), r["name"].as_string()});
            }
            result.fetched = true;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_ = std::move(result);
        }
        busy_ = false;
    });
}

void OnlinePage::Refresh(trg::Ui& ui) {
    Start(ui, [](const net::OnlineConfig&, const net::Identity&, const std::string&, const std::string&, net::LobbySession&,
                 Snapshot&) {});
}

void OnlinePage::Action(trg::Ui& ui, std::string op, std::string account, std::string code) {
    Start(ui, [op = std::move(op), account = std::move(account), code = std::move(code)](
                  const net::OnlineConfig&, const net::Identity&, const std::string&, const std::string&,
                  net::LobbySession& session, Snapshot& s) {
        net::Json body = net::Json::Object();
        if (!account.empty()) body.set("account", account);
        if (!code.empty()) body.set("code", code);
        std::string result;
        const auto reply = session.call(op, body, &result);
        if (op == "friends.add") {
            s.notice = !reply ? Describe(result)
                     : (*reply)["state"].as_string() == "friends" ? (*reply)["name"].as_string() + " is now your friend."
                                                                   : "Request sent to " + (*reply)["name"].as_string() + ".";
        } else if (!reply) {
            s.notice = Describe(result);
        }
    });
}

void OnlinePage::Draw(trg::Ui& ui) {
    const int frame = ImGui::GetFrameCount();
    const double now = ImGui::GetTime();
    const net::OnlineConfig effective = ConfigOf(ui);
    const bool enabled = effective.enabled;
    const bool has_lobby = !effective.lobby.empty();
    if (enabled && has_lobby && (frame > last_frame_ + 1 || now >= next_refresh_)) {
        next_refresh_ = now + kRefreshSeconds;
        Refresh(ui);
    }
    last_frame_ = frame;
    Snapshot view;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        view = snapshot_;
    }

    const bool supported = !build_ || build_->online;
    if (!supported) {
        ui.StatusCard(trg::Status::kAttention, "Online play is not available for this game build",
                      "Online play is verified for the USA 1.00 and Japan 1.02 and 1.03 builds.");
    } else if (!enabled) {
        ui.StatusCard(trg::Status::kAttention, "Online play is off", "Turn it on to play with others through the lobby.");
    } else if (!has_lobby) {
        ui.StatusCard(trg::Status::kAttention, "No lobby server", "Enter the lobby's address below.");
    } else if (!view.error.empty()) {
        ui.StatusCard(trg::Status::kAttention, "Lobby unreachable", view.error.c_str());
    } else {
        ui.StatusCard(busy_ ? trg::Status::kBusy : trg::Status::kReady, view.fetched ? "Online play is on" : "Connecting...",
                      "Use the game's own online menus to host and join; friends in a room appear in its Join Friends Room list.");
    }

    if (std::getenv("DCP_ONLINE") || std::getenv("DCP_LOBBY") || std::getenv("DCP_ONLINE_NAME")) {
        const std::string text = "This session uses DCP_* environment settings (a test instance): " + effective.lobby + " as " +
                                 net::online_name(effective.name.empty() ? net::system_user_name() : effective.name) + ".";
        ui.Help(text.c_str());
    }
    ui.Toggle("Online play", kOnlineHelp, kOnlineEnabledKey, false);
    const std::string user = net::system_user_name();
    ui.InputText("Online name", kNameHelp, kOnlineNameKey, user.empty() ? "Player" : user.c_str());
    ui.InputText("Lobby server", kLobbyHelp, kOnlineLobbyKey, "wss://lobby.example.org/");
    ui.Toggle("UPnP", kUpnpHelp, kOnlineUpnpKey, true);
    if (!view.friend_code.empty()) {
        ui.Row("Friend code", "Give this code to a friend so they can add you.");
        ImGui::TextUnformatted(view.friend_code.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy")) {
            ImGui::SetClipboardText(view.friend_code.c_str());
            ui.SetStatus("Friend code copied.");
        }
    }
    if (ui.ButtonRow("Connection test", "Checks the lobby, your public address, UDP and UPnP. Takes a few seconds.",
                     busy_ ? "Testing..." : "Run test") && !busy_) {
        Start(ui, [](const net::OnlineConfig& config, const net::Identity& identity, const std::string& name,
                     const std::string& game, net::LobbySession&, Snapshot& s) {
            s.report = net::run_connection_test(config, identity, name, game);
            s.tested = true;
            if (!s.report.friend_code.empty()) s.friend_code = s.report.friend_code;
        }, false);
    }
    ui.EndRows();
    if (view.tested) {
        ui.Heading(view.report.verdict.c_str());
        for (const auto& line : view.report.lines) {
            ImGui::PushStyleColor(ImGuiCol_Text, line.ok ? ui.theme().good_dot : ui.theme().warn_dot);
            ImGui::TextUnformatted(line.ok ? "OK" : "!!");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextWrapped("%s: %s", line.label.c_str(), line.text.c_str());
        }
        ui.Spacer();
    }
    if (enabled && has_lobby) DrawFriends(ui, view);
}

void OnlinePage::DrawFriends(trg::Ui& ui, const Snapshot& view) {
    ui.Heading("Friends");
    if (!view.notice.empty()) ui.Help(view.notice.c_str());
    ui.Row("Add a friend", "Their friend code, for example ABCD-EFGH.");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
    ImGui::InputTextWithHint("##add_code", "XXXX-XXXX", add_code_, sizeof(add_code_), ImGuiInputTextFlags_CharsUppercase);
    ImGui::SameLine();
    if (ImGui::Button("Send request") && add_code_[0] && !busy_) {
        Action(ui, "friends.add", {}, add_code_);
        add_code_[0] = 0;
    }
    for (const Request& r : view.incoming) {
        ui.Row(r.name.c_str(), "Wants to be your friend.");
        ImGui::PushID(("in" + r.account).c_str());
        if (ImGui::Button("Accept") && !busy_) Action(ui, "friends.accept", r.account);
        ImGui::SameLine();
        if (ImGui::Button("Decline") && !busy_) Action(ui, "friends.decline", r.account);
        ImGui::PopID();
    }
    for (const Request& r : view.outgoing) {
        ui.Row(r.name.c_str(), "Your request is waiting for an answer.");
        ImGui::PushID(("out" + r.account).c_str());
        if (ImGui::Button("Cancel request") && !busy_) Action(ui, "friends.decline", r.account);
        ImGui::PopID();
    }
    for (const Friend& f : view.friends) {
        const char* state = f.in_room ? "Online, in a room" : f.online ? "Online" : "Offline";
        ui.Row(f.name.c_str(), state);
        ImGui::PushID(("f" + f.account).c_str());
        if (ImGui::Button("Remove") && !busy_) Action(ui, "friends.remove", f.account);
        ImGui::PopID();
    }
    if (view.fetched && view.friends.empty() && view.incoming.empty() && view.outgoing.empty()) {
        ui.EndRows();
        ui.Help("No friends yet. A friend in a room appears in the game's Join Friends Room list.");
    }
    ui.EndRows();
    if (ImGui::Button(busy_ ? "Refreshing..." : "Refresh") && !busy_) Refresh(ui);
}

} // namespace dcp
