#include "online_client.h"

#include <algorithm>
#include <cstdio>
#include <thread>

#include "net/identity.h"

namespace dcp {
namespace {

std::string Describe(const std::string& code) {
    if (code == "not_found") return "No player has that friend code.";
    if (code == "bad_request") return "That is not a valid friend code.";
    if (code == "limit") return "Too many friends or pending requests.";
    if (code == net::kLinkLost || code == net::kUnreachable || code == "timeout") {
        return "The online service did not answer. Try again.";
    }
    return "The online service refused the request (" + code + ").";
}

std::string DescribeLobbyAdd(const std::string& code) {
    return code == "not_found" ? "That player is no longer in the Lobby." : Describe(code);
}

std::string ShownName(const net::Json& body) {
    const std::string& shown = body["display_name"].as_string();
    return shown.empty() ? body["name"].as_string() : shown;
}

bool SameAccounts(const std::vector<OnlinePlayer>& a, const std::vector<OnlinePlayer>& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](const OnlinePlayer& x, const OnlinePlayer& y) { return x.account == y.account; });
}

void Erase(std::vector<OnlinePlayer>& players, const std::string& account) {
    players.erase(std::remove_if(players.begin(), players.end(),
                                 [&](const OnlinePlayer& p) { return p.account == account; }),
                  players.end());
}

} // namespace

void OnlineClient::Update(bool on, const net::OnlineConfig& config, const std::string& game, double now) {
    if (!on) {
        if (link_ || state_.link != Link::kOff) {
            Stop();
            state_ = {};
            wanted_name_.clear();
            retry_at_ = 0;
        }
        return;
    }
    const std::string name = net::online_name(config.name);
    if (name != wanted_name_) {
        wanted_name_ = name;
        name_since_ = now;
    }
    const bool first = state_.link == Link::kOff;
    const bool settled = first || now - name_since_ >= kNameSettleSeconds;
    if (link_ && (config.lobby != url_ || game != game_ || (name != name_ && settled))) {
        Stop();
        state_.link = Link::kConnecting;
        retry_at_ = 0;
    }
    if (!link_ && settled && now >= retry_at_) {
        Start(config, game, name, now);
    }
    net::LobbyMessage message;
    while (link_ && link_->poll(message)) {
        Handle(message, now);
    }
    if (state_.link == Link::kUp && now >= next_fetch_) {
        Fetch(now);
    }
    SyncLobby(config.lobby_visible, now);
}

void OnlineClient::Start(const net::OnlineConfig& config, const std::string& game, const std::string& name, double now) {
    url_ = config.lobby;
    game_ = game;
    name_ = name;
    const net::Identity identity = net::load_identity(user_dir_);
    if (!identity.ok()) {
        std::printf("Online: no online identity (%s)\n", identity.error.c_str());
        state_.link = Link::kUnreachable;
        retry_at_ = now + kRetrySeconds;
        return;
    }
    net::LobbyLink::Options options;
    options.url = url_;
    options.hello = [identity, name, game] { return net::hello_body(identity, name, game, true); };
    link_ = std::make_unique<net::LobbyLink>(std::move(options));
    link_->start();
    state_.link = Link::kConnecting;
}

void OnlineClient::Stop() {
    // A link still connecting finishes its connect timeout before it stops: not on the UI thread.
    if (link_) std::thread([link = std::shared_ptr<net::LobbyLink>(std::move(link_))] { link->stop(); }).detach();
    pending_.clear();
    fetching_ = fetch_again_ = false;
    lobby_told_ = lobby_listed_ = false;
    lobby_refresh_.Reset();
    lobby_rev_.clear();
}

void OnlineClient::Fetch(double now) {
    next_fetch_ = now + kRefreshSeconds;
    if (fetching_) {
        fetch_again_ = true;
        return;
    }
    fetching_ = true;
    Send("friends.list", net::Json::Object(), Purpose::kFriends);
    Send("friends.requests", net::Json::Object(), Purpose::kRequests);
}

void OnlineClient::ForgetLobby() {
    state_.lobby.clear();
    state_.lobby_total = 0;
    state_.lobby_fetched = false;
    state_.lobby_problem.clear();
    lobby_rev_.clear();
}

void OnlineClient::SyncLobby(bool visible, double now) {
    if (!visible) ForgetLobby();
    if (state_.link != Link::kUp) return;
    if (visible != lobby_told_ && now >= lobby_retry_at_) {
        lobby_told_ = visible;
        if (!visible) lobby_listed_ = false;
        net::Json body = net::Json::Object();
        body.set("visible", visible);
        Send("lobby.publish", std::move(body), Purpose::kLobbyPublish);
    }
    const bool watched = now - lobby_watched_at_ < kLobbyWatchSeconds;
    if (!visible || !lobby_listed_ || !watched || !lobby_refresh_.Due(now)) return;
    lobby_refresh_.Start(now);
    net::Json body = net::Json::Object();
    if (!lobby_rev_.empty()) body.set("rev", lobby_rev_);
    Send("lobby.list", std::move(body), Purpose::kLobbyList);
}

void OnlineClient::Send(const std::string& op, net::Json body, Purpose purpose) {
    if (!link_) {
        if (purpose == Purpose::kAdd || purpose == Purpose::kChange || purpose == Purpose::kLobbyAdd ||
            purpose == Purpose::kLobbyChange) {
            state_.notice = Describe(net::kUnreachable);
        }
        return;
    }
    pending_[link_->request(op, std::move(body))] = purpose;
}

void OnlineClient::Handle(const net::LobbyMessage& message, double now) {
    using Kind = net::LobbyMessage::Kind;
    switch (message.kind) {
    case Kind::kUp:
    case Kind::kBack:
        if (message.kind == Kind::kBack) link_->release_held();
        state_.link = Link::kUp;
        state_.friend_code = message.body["friend_code"].as_string();
        fetching_ = false;
        lobby_told_ = lobby_listed_ = false;
        lobby_refresh_.Reset();
        lobby_retry_at_ = 0;
        Fetch(now);
        break;
    case Kind::kLost:
        state_.link = Link::kConnecting;
        break;
    case Kind::kDown:
        std::printf("Online: the lobby %s cannot be reached (%s); trying again in %.0f s\n", url_.c_str(),
                    message.code.c_str(), kRetrySeconds);
        Stop();
        state_.link = Link::kUnreachable;
        retry_at_ = now + kRetrySeconds;
        break;
    case Kind::kEvent:
        if (message.event.rfind("friend.", 0) == 0) Fetch(now);
        if (message.event == "friend.request" || message.event == "friend.added" || message.event == "friend.removed") {
            lobby_refresh_.Soon(now);
        }
        break;
    case Kind::kResponse:
        if (const auto it = pending_.find(message.seq); it != pending_.end()) {
            const Purpose purpose = it->second;
            pending_.erase(it);
            Answer(purpose, message, now);
        }
        break;
    }
}

void OnlineClient::Answer(Purpose purpose, const net::LobbyMessage& message, double now) {
    const bool ok = message.code == "ok";
    switch (purpose) {
    case Purpose::kFriends:
        if (ok) {
            state_.friends = ReadOnlinePlayers(message.body["friends"]);
            state_.fetched = true;
        }
        break;
    case Purpose::kRequests:
        if (ok) {
            std::vector<OnlinePlayer> incoming = ReadOnlinePlayers(message.body["incoming"]);
            std::vector<OnlinePlayer> outgoing = ReadOnlinePlayers(message.body["outgoing"]);
            // A decline by the other player sends no event.
            if (!SameAccounts(incoming, state_.incoming) || !SameAccounts(outgoing, state_.outgoing)) lobby_refresh_.Soon(now);
            state_.incoming = std::move(incoming);
            state_.outgoing = std::move(outgoing);
        }
        fetching_ = false;
        if (fetch_again_) {
            fetch_again_ = false;
            Fetch(now);
        }
        break;
    case Purpose::kAdd: {
        const std::string name = ShownName(message.body);
        state_.notice = !ok ? Describe(message.code)
                      : message.body["state"].as_string() == "friends" ? name + " is now your friend."
                                                                      : "Request sent to " + name + ".";
        Fetch(now);
        break;
    }
    case Purpose::kChange:
        if (!ok) state_.notice = Describe(message.code);
        lobby_refresh_.Soon(now);
        Fetch(now);
        break;
    case Purpose::kLobbyPublish:
    case Purpose::kLobbyList:
    case Purpose::kLobbyAdd:
    case Purpose::kLobbyChange:
        LobbyAnswer(purpose, message, now);
        break;
    }
}

void OnlineClient::LobbyAnswer(Purpose purpose, const net::LobbyMessage& message, double now) {
    const bool ok = message.code == "ok";
    switch (purpose) {
    case Purpose::kLobbyPublish:
        if (ok) {
            lobby_listed_ = lobby_told_;
            lobby_refresh_.Soon(now);
            state_.lobby_problem.clear();
        } else if (message.code != net::kLinkLost) {
            lobby_told_ = !lobby_told_;
            lobby_retry_at_ = now + kRetrySeconds;
            state_.lobby_problem = "The Lobby is not available right now.";
        }
        break;
    case Purpose::kLobbyList:
        if (ok) {
            lobby_refresh_.Finish();
            if (!message.body["unchanged"].as_bool()) {
                state_.lobby = ReadLobbyPlayers(message.body["players"]);
                lobby_rev_ = message.body["rev"].as_string();
            }
            state_.lobby_total = size_t(message.body["total"].as_int());
            state_.lobby_fetched = true;
            state_.lobby_updated_at = now;
            state_.lobby_problem.clear();
            break;
        }
        lobby_refresh_.Fail(now);
        if (message.code == "not_visible") {
            lobby_told_ = lobby_listed_ = false;
        } else if (message.code != net::kLinkLost) {
            state_.lobby_problem = "Could not refresh the Lobby.";
        }
        break;
    case Purpose::kLobbyAdd: {
        const std::string name = ShownName(message.body);
        state_.notice = !ok ? DescribeLobbyAdd(message.code)
                      : message.body["state"].as_string() == "friends" ? name + " is now your friend."
                                                                      : "Request sent to " + name + ".";
        lobby_rev_.clear();
        lobby_refresh_.Soon(now);
        Fetch(now);
        break;
    }
    case Purpose::kLobbyChange:
        if (!ok) state_.notice = DescribeLobbyAdd(message.code);
        lobby_rev_.clear();
        lobby_refresh_.Soon(now);
        Fetch(now);
        break;
    default: break;
    }
}

void OnlineClient::AddFriend(const std::string& code) {
    state_.notice.clear();
    net::Json body = net::Json::Object();
    body.set("code", code);
    Send("friends.add", std::move(body), Purpose::kAdd);
}

void OnlineClient::RequestFromLobby(const std::string& player) {
    state_.notice.clear();
    for (LobbyPlayer& row : state_.lobby) {
        if (row.player != player) continue;
        row.relation = row.relation == LobbyRelation::kIncoming ? LobbyRelation::kFriend : LobbyRelation::kPending;
    }
    SortLobbyPlayers(state_.lobby);
    lobby_rev_.clear();
    net::Json body = net::Json::Object();
    body.set("player", player);
    Send("friends.add", std::move(body), Purpose::kLobbyAdd);
}

void OnlineClient::DeclineFromLobby(const std::string& player) {
    state_.notice.clear();
    for (LobbyPlayer& row : state_.lobby) {
        if (row.player == player) row.relation = LobbyRelation::kNone;
    }
    SortLobbyPlayers(state_.lobby);
    lobby_rev_.clear();
    net::Json body = net::Json::Object();
    body.set("player", player);
    Send("friends.decline", std::move(body), Purpose::kLobbyChange);
}

void OnlineClient::RefreshLobby(double now) {
    if (state_.link == Link::kUp && lobby_listed_) lobby_refresh_.Manual(now);
}

void OnlineClient::Accept(const std::string& account) {
    const auto it = std::find_if(state_.incoming.begin(), state_.incoming.end(),
                                 [&](const OnlinePlayer& p) { return p.account == account; });
    if (it != state_.incoming.end()) {
        state_.friends.push_back(*it);
        state_.incoming.erase(it);
    }
    net::Json body = net::Json::Object();
    body.set("account", account);
    Send("friends.accept", std::move(body), Purpose::kChange);
}

void OnlineClient::Decline(const std::string& account) {
    Erase(state_.incoming, account);
    Erase(state_.outgoing, account);
    net::Json body = net::Json::Object();
    body.set("account", account);
    Send("friends.decline", std::move(body), Purpose::kChange);
}

void OnlineClient::Remove(const std::string& account) {
    Erase(state_.friends, account);
    net::Json body = net::Json::Object();
    body.set("account", account);
    Send("friends.remove", std::move(body), Purpose::kChange);
}

} // namespace dcp
