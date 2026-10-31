#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "net/connection_test.h"
#include "online_client.h"
#include "online_view.h"
#include "trg/launcher.h"
#include "trg/settings.h"

namespace dcp {

class LauncherSettings;

// The launcher's Online page: online play, friend requests, the online name, the friend code, the Connection test and
// friends. Its lobby link (OnlineClient) runs on every launcher frame, not only while the page is shown.
class OnlinePage {
public:
    explicit OnlinePage(std::filesystem::path user_dir) : user_dir_(user_dir), client_(std::move(user_dir)) {}
    ~OnlinePage();

    void Tick(LauncherSettings& settings, double now);
    size_t PendingRequests() const { return client_.state().incoming.size(); }
    // Online play is on and no Connection test has finished on the game's UDP port, or one is running.
    bool PlayNeedsTest(const trg::SettingsStore& settings) const;
    // PLAY was pressed while it waits for the test: the next Draw scrolls to the section, brightens it for a moment and
    // focuses Run test.
    void PointToConnectionTest() { pointed_ = true; }
    void Draw(trg::Ui& ui);

private:
    struct TestResult {
        bool done = false;
        net::ConnectionReport report;
        uint16_t port = 0;
        bool recorded = false;
    };

    void RunTest(trg::Ui& ui);
    void ClearTest();
    void RecordFinishedTest(LauncherSettings& settings, bool online_on);
    void DrawOnlinePlay(trg::Ui& ui, const OnlinePlaySwitch& online);
    void DrawFriendRequests(trg::Ui& ui);
    void DrawFriendCode(trg::Ui& ui);
    void DrawConnectionTest(trg::Ui& ui);
    bool DrawRequiredTestRow(trg::Ui& ui, const ConnectionTestLook& look, const char* button, bool focus);
    void DrawFriends(trg::Ui& ui);

    std::filesystem::path user_dir_;
    OnlineClient client_;
    bool was_on_ = false;
    bool pointed_ = false;
    double pointed_at_ = -1e9;
    std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> testing_{false};
    TestResult test_;
    char add_code_[16] = {};
};

} // namespace dcp
