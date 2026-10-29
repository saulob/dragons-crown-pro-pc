#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "game_profile.h"
#include "net/connection_test.h"
#include "net/identity.h"
#include "net/lobby_link.h"
#include "trg/launcher.h"

namespace dcp {

// The launcher's Online page: online settings, the friend code and friends, and the Connection test. Every lobby
// exchange is a short session on a worker thread (hello without presence, so the player is not shown online).
class OnlinePage {
public:
    explicit OnlinePage(std::filesystem::path user_dir) : user_dir_(std::move(user_dir)) {}
    ~OnlinePage();

    void SetBuild(const GameBuild* build) { build_ = build; }
    void Draw(trg::Ui& ui);

private:
    struct Friend {
        std::string account, name;
        bool online = false;
        bool in_room = false;
    };
    struct Request {
        std::string account, name;
    };
    struct Snapshot {
        bool fetched = false;
        std::string error;
        std::string friend_code;
        std::vector<Friend> friends;
        std::vector<Request> incoming, outgoing;
        std::string notice;
        bool tested = false;
        net::ConnectionReport report;
    };

    using Job = std::function<void(const net::OnlineConfig&, const net::Identity&, const std::string& name,
                                   const std::string& game, net::LobbySession&, Snapshot&)>;

    std::string Game() const;
    // One job at a time on the worker; with `needs_lobby` it runs inside a greeted session and the friends are re-read.
    void Start(trg::Ui& ui, Job job, bool needs_lobby = true);
    void Refresh(trg::Ui& ui);
    void Action(trg::Ui& ui, std::string op, std::string account, std::string code = {});
    void DrawFriends(trg::Ui& ui, const Snapshot& view);

    std::filesystem::path user_dir_;
    const GameBuild* build_ = nullptr;
    std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> busy_{false};
    Snapshot snapshot_;
    int last_frame_ = -10;
    double next_refresh_ = 0;
    char add_code_[16] = {};
};

} // namespace dcp
