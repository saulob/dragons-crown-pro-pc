#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "game_profile.h"
#include "net/connection_test.h"
#include "online_client.h"
#include "trg/launcher.h"
#include "trg/settings.h"

namespace dcp {

// The launcher's Online page: online play, friend requests, the online name, the friend code, the Connection test and
// friends. Its lobby link (OnlineClient) runs on every launcher frame, not only while the page is shown.
class OnlinePage {
public:
    explicit OnlinePage(std::filesystem::path user_dir) : user_dir_(user_dir), client_(std::move(user_dir)) {}
    ~OnlinePage();

    void SetBuild(const GameBuild* build) { build_ = build; }
    void Tick(const trg::SettingsStore& settings, double now);
    size_t PendingRequests() const { return client_.state().incoming.size(); }
    void Draw(trg::Ui& ui);

private:
    struct TestResult {
        bool done = false;
        net::ConnectionReport report;
    };

    std::string Game() const;
    bool Supported() const { return !build_ || build_->online; }
    void RunTest(trg::Ui& ui);
    void ClearTest();
    void DrawOnlinePlay(trg::Ui& ui, const OnlinePlaySwitch& online);
    void DrawTestInstanceNote(trg::Ui& ui, const net::OnlineConfig& config);
    void DrawFriendRequests(trg::Ui& ui);
    void DrawFriendCode(trg::Ui& ui);
    void DrawConnectionTest(trg::Ui& ui);
    void DrawFriends(trg::Ui& ui);

    std::filesystem::path user_dir_;
    const GameBuild* build_ = nullptr;
    OnlineClient client_;
    bool was_on_ = false;
    std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> testing_{false};
    TestResult test_;
    char add_code_[16] = {};
};

} // namespace dcp
