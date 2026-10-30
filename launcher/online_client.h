#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "net/lobby_link.h"
#include "net/online_config.h"
#include "online_view.h"

namespace dcp {

// The launcher's lobby link while online play is on. It shows the player online to friends and keeps the friend code,
// the friends and the requests current from the lobby's friend events. Used from one thread.
class OnlineClient {
public:
    enum class Link { kOff, kConnecting, kUp, kUnreachable };
    struct State {
        Link link = Link::kOff;
        std::string friend_code;
        std::vector<OnlinePlayer> friends, incoming, outgoing;
        bool fetched = false;
        std::string notice;
    };

    static constexpr double kNameSettleSeconds = 1.5;
    static constexpr double kRetrySeconds = 20.0;
    // Declines and renames by others send no event.
    static constexpr double kRefreshSeconds = 30.0;

    explicit OnlineClient(std::filesystem::path user_dir) : user_dir_(std::move(user_dir)) {}
    ~OnlineClient() {
        if (link_) link_->stop();
    }

    // Every launcher frame. Off stops the link; on keeps one, started again for another lobby, build or online name.
    void Update(bool on, const net::OnlineConfig& config, const std::string& game, double now);

    // The lists change at once; the lobby's answer and a fresh read follow.
    void AddFriend(const std::string& code);
    void Accept(const std::string& account);
    void Decline(const std::string& account); // also withdraws an outgoing request
    void Remove(const std::string& account);

    const State& state() const { return state_; }

private:
    enum class Purpose { kFriends, kRequests, kAdd, kChange };

    void Start(const net::OnlineConfig& config, const std::string& game, const std::string& name, double now);
    void Stop();
    void Fetch(double now);
    void Send(const std::string& op, net::Json body, Purpose purpose);
    void Handle(const net::LobbyMessage& message, double now);
    void Answer(Purpose purpose, const net::LobbyMessage& message, double now);

    std::filesystem::path user_dir_;
    std::unique_ptr<net::LobbyLink> link_;
    std::string url_, game_, name_;
    std::string wanted_name_;
    double name_since_ = 0;
    double retry_at_ = 0;
    double next_fetch_ = 0;
    bool fetching_ = false, fetch_again_ = false;
    std::map<uint32_t, Purpose> pending_;
    State state_;
};

} // namespace dcp
