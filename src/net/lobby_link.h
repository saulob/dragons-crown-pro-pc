// The game's connection to the lobby (docs/ONLINE.md): requests with numbered answers, events, a keepalive, and a
// silent reconnect when an established socket drops. Owners poll it from their own thread.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "net/json.h"
#include "net/websocket.h"

namespace dcp::net {

struct LobbyMessage {
    enum class Kind {
        kResponse, // `seq`, `code` ("ok" or an error), `body`
        kEvent,    // `event`, `body`
        kUp,       // first hello answered: `body` is its answer
        kLost,     // the socket dropped; reconnecting
        kBack,     // reconnected and greeted again (`body`); held requests wait for release_held()
        kDown,     // no lobby: `code` says why
    };
    Kind kind = Kind::kEvent;
    uint32_t seq = 0;
    std::string code;
    std::string event;
    Json body;
};

// Codes the link itself produces.
inline constexpr char kLinkLost[] = "link_lost";       // the request went out on a socket that dropped
inline constexpr char kUnreachable[] = "unreachable";  // no socket at all

inline constexpr int kLobbyProtocol = 1;

struct Identity;

// The hello request: protocol, game build ("CUSA10454-1.00"), identity and name. `presence` false keeps a launcher
// session from showing the player online to friends.
Json hello_body(const Identity& identity, const std::string& name, const std::string& game, bool presence);

class LobbyLink {
public:
    struct Options {
        std::string url;
        std::function<Json()> hello;   // the hello request body, built again for each socket
        int connect_timeout_ms = 8000;
        int keepalive_ms = 20000;
        int silence_ms = 45000;        // no frame for this long: the socket is dead
        int reconnect_ms = 25000;      // how long a dropped socket is retried before kDown
    };

    explicit LobbyLink(Options options);
    ~LobbyLink();
    LobbyLink(const LobbyLink&) = delete;
    LobbyLink& operator=(const LobbyLink&) = delete;

    void start();
    void stop();

    // Queues a request; returns its number. Requests wait while there is no greeted socket, and after kBack until
    // release_held(); `urgent` ones go first and do not wait for the release.
    uint32_t request(const std::string& op, Json body, bool urgent = false);
    void release_held();
    bool poll(LobbyMessage& out);
    bool up() const;
    const std::string& url() const { return options_.url; }

private:
    struct Outgoing {
        uint32_t seq;
        std::string text;
        bool urgent;
    };

    enum class SessionEnd { kNoSocket, kUngreeted, kGreeted };

    void Run();
    SessionEnd Session(bool reconnecting);
    void Push(LobbyMessage message);
    void FailInFlight();

    Options options_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    std::shared_ptr<WebSocket> socket_;
    std::deque<Outgoing> outgoing_;
    std::vector<uint32_t> in_flight_;
    std::deque<LobbyMessage> inbox_;
    uint32_t next_seq_ = 1;
    uint32_t hello_seq_ = 0;
    bool greeted_ = false;
    bool holding_ = false;
    bool stopping_ = false;
    uint64_t last_rx_ms_ = 0;
};

// A short session for the launcher: connect, greet, a few requests, close. Not thread-safe; blocks.
class LobbySession {
public:
    ~LobbySession();
    // False with the reason; `hello_reply` receives the hello answer.
    bool open(const std::string& url, const Json& hello, Json* hello_reply, std::string* error, int timeout_ms = 8000);
    // The answer, or nullopt with `code` set ("timeout" or the lobby's error code).
    std::optional<Json> call(const std::string& op, Json body, std::string* code);
    void close();

private:
    std::unique_ptr<WebSocket> socket_;
    uint32_t seq_ = 0;
};

} // namespace dcp::net
