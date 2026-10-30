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

void OnlineClient::Send(const std::string& op, net::Json body, Purpose purpose) {
    if (!link_) {
        if (purpose == Purpose::kAdd || purpose == Purpose::kChange) state_.notice = Describe(net::kUnreachable);
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
            state_.incoming = ReadOnlinePlayers(message.body["incoming"]);
            state_.outgoing = ReadOnlinePlayers(message.body["outgoing"]);
        }
        fetching_ = false;
        if (fetch_again_) {
            fetch_again_ = false;
            Fetch(now);
        }
        break;
    case Purpose::kAdd: {
        const std::string name = message.body["name"].as_string();
        state_.notice = !ok ? Describe(message.code)
                      : message.body["state"].as_string() == "friends" ? name + " is now your friend."
                                                                      : "Request sent to " + name + ".";
        Fetch(now);
        break;
    }
    case Purpose::kChange:
        if (!ok) state_.notice = Describe(message.code);
        Fetch(now);
        break;
    }
}

void OnlineClient::AddFriend(const std::string& code) {
    state_.notice.clear();
    net::Json body = net::Json::Object();
    body.set("code", code);
    Send("friends.add", std::move(body), Purpose::kAdd);
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
