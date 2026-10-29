#include "net/lobby_link.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>

#include "net/addr.h"
#include "net/identity.h"
#include "net/online_version.h"
#include "net/websocket.h"

namespace dcp::net {
namespace {

std::string Envelope(const std::string& op, uint32_t seq, Json body) {
    Json message = Json::Object();
    message.set("op", op);
    message.set("seq", int64_t(seq));
    for (const auto& [key, value] : body.members()) message.set(key, value);
    return message.dump();
}

} // namespace

Json hello_body(const Identity& identity, const std::string& name, const std::string& game, bool presence) {
    Json h = Json::Object();
    h.set("proto", kLobbyProtocol);
    h.set("game", game);
    h.set("account", u64_text(identity.account));
    h.set("secret", identity.secret);
    h.set("name", name);
    h.set("presence", presence);
    h.set("online_protocol", kOnlineProtocolVersion);
    h.set("version", port_version());
    h.set("platform", host_platform());
    return h;
}

LobbyLink::LobbyLink(Options options) : options_(std::move(options)) {}

LobbyLink::~LobbyLink() { stop(); }

void LobbyLink::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (thread_.joinable() || stopping_) return;
    thread_ = std::thread([this] { Run(); });
}

void LobbyLink::stop() {
    std::shared_ptr<WebSocket> socket;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        socket = socket_;
    }
    wake_.notify_all();
    if (socket) socket->close();
    if (thread_.joinable()) thread_.join();
}

uint32_t LobbyLink::request(const std::string& op, Json body, bool urgent) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t seq = next_seq_++;
    Outgoing out{seq, Envelope(op, seq, std::move(body)), urgent};
    if (urgent) {
        outgoing_.push_front(std::move(out));
    } else {
        outgoing_.push_back(std::move(out));
    }
    wake_.notify_all();
    return seq;
}

void LobbyLink::release_held() {
    std::lock_guard<std::mutex> lock(mutex_);
    holding_ = false;
    wake_.notify_all();
}

bool LobbyLink::poll(LobbyMessage& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (inbox_.empty()) return false;
    out = std::move(inbox_.front());
    inbox_.pop_front();
    return true;
}

bool LobbyLink::up() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return greeted_;
}

void LobbyLink::Push(LobbyMessage message) {
    std::lock_guard<std::mutex> lock(mutex_);
    inbox_.push_back(std::move(message));
}

void LobbyLink::FailInFlight() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const uint32_t seq : in_flight_) {
        LobbyMessage m;
        m.kind = LobbyMessage::Kind::kResponse;
        m.seq = seq;
        m.code = kLinkLost;
        inbox_.push_back(std::move(m));
    }
    in_flight_.clear();
}

// One socket, from connect to its end.
LobbyLink::SessionEnd LobbyLink::Session(bool reconnecting) {
    std::string error;
    std::shared_ptr<WebSocket> socket(WebSocket::connect(options_.url, options_.connect_timeout_ms, &error));
    if (!socket) {
        std::printf("Lobby: cannot connect to %s: %s\n", options_.url.c_str(), error.c_str());
        return SessionEnd::kNoSocket;
    }
    bool greeted = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return SessionEnd::kNoSocket;
        socket_ = socket;
        greeted_ = false;
        holding_ = reconnecting;
        hello_seq_ = next_seq_++;
        last_rx_ms_ = now_ms();
    }
    std::printf("Lobby: connected to %s\n", options_.url.c_str());
    if (!socket->send_text(Envelope("hello", hello_seq_, options_.hello()))) {
        socket->close();
    }
    std::atomic<bool> reading{true};
    std::thread reader([&] {
        std::string text;
        while (socket->receive(text)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                last_rx_ms_ = now_ms();
            }
            if (text == "pong") continue;
            std::optional<Json> message = Json::parse(text);
            if (!message || !message->is_object()) continue;
            const Json& m = *message;
            LobbyMessage out;
            if (m.has("re")) {
                out.kind = LobbyMessage::Kind::kResponse;
                out.seq = uint32_t(m["re"].as_int());
                out.code = m["code"].as_string();
                out.body = m;
                std::lock_guard<std::mutex> lock(mutex_);
                if (out.seq == hello_seq_) {
                    if (out.code == "ok") {
                        greeted_ = true;
                        greeted = true;
                        out.kind = reconnecting ? LobbyMessage::Kind::kBack : LobbyMessage::Kind::kUp;
                        std::printf("Lobby: signed in (%s)\n", reconnecting ? "reconnected" : "first connection");
                    } else {
                        out.kind = LobbyMessage::Kind::kDown;
                        stopping_ = true;
                        std::printf("Lobby: sign-in refused (%s)\n", out.code.c_str());
                    }
                    inbox_.push_back(std::move(out));
                    wake_.notify_all();
                    continue;
                }
                std::erase(in_flight_, out.seq);
                inbox_.push_back(std::move(out));
            } else if (m["ev"].is_string()) {
                out.kind = LobbyMessage::Kind::kEvent;
                out.event = m["ev"].as_string();
                out.body = m;
                Push(std::move(out));
            }
        }
        reading = false;
        wake_.notify_all();
    });
    uint64_t last_ping = now_ms();
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_ && reading) {
        // While held, only urgent requests go out (they may sit behind held ones).
        while (greeted_) {
            auto next = std::find_if(outgoing_.begin(), outgoing_.end(), [this](const Outgoing& o) { return !holding_ || o.urgent; });
            if (next == outgoing_.end()) break;
            Outgoing out = std::move(*next);
            outgoing_.erase(next);
            in_flight_.push_back(out.seq);
            lock.unlock();
            const bool sent = socket->send_text(out.text);
            lock.lock();
            if (!sent) break;
        }
        const uint64_t now = now_ms();
        if (now - last_ping >= uint64_t(options_.keepalive_ms)) {
            last_ping = now;
            lock.unlock();
            socket->send_text("ping");
            lock.lock();
        }
        if (now - last_rx_ms_ > uint64_t(options_.silence_ms)) {
            std::printf("Lobby: no answer for %d s; the connection is dead\n", options_.silence_ms / 1000);
            break;
        }
        wake_.wait_for(lock, std::chrono::milliseconds(250));
    }
    greeted_ = false;
    socket_.reset();
    lock.unlock();
    socket->close();
    reader.join();
    FailInFlight();
    return greeted ? SessionEnd::kGreeted : SessionEnd::kUngreeted;
}

void LobbyLink::Run() {
    auto stopping = [this] {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopping_;
    };
    auto down = [this] {
        LobbyMessage m;
        m.kind = LobbyMessage::Kind::kDown;
        m.code = kUnreachable;
        Push(std::move(m));
    };
    // The first socket is not retried: an unreachable lobby at the start leaves the game offline.
    SessionEnd end = Session(false);
    if (stopping()) return;
    if (end != SessionEnd::kGreeted) return down();
    for (;;) {
        std::printf("Lobby: connection lost; reconnecting for up to %d s\n", options_.reconnect_ms / 1000);
        LobbyMessage lost;
        lost.kind = LobbyMessage::Kind::kLost;
        Push(std::move(lost));
        const uint64_t deadline = now_ms() + uint64_t(options_.reconnect_ms);
        end = SessionEnd::kNoSocket;
        while (end != SessionEnd::kGreeted && now_ms() < deadline) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(1000), [this] { return stopping_; });
                if (stopping_) return;
            }
            end = Session(true);
            if (stopping()) return;
        }
        if (end != SessionEnd::kGreeted) {
            std::printf("Lobby: reconnect failed; online play ends\n");
            return down();
        }
    }
}

LobbySession::~LobbySession() { close(); }

bool LobbySession::open(const std::string& url, const Json& hello, Json* hello_reply, std::string* error, int timeout_ms) {
    close();
    socket_ = WebSocket::connect(url, timeout_ms, error);
    if (!socket_) return false;
    std::string code;
    std::optional<Json> reply = call("hello", hello, &code);
    if (!reply) {
        if (error) *error = code == "unauthorized" ? "the lobby refused this identity (unauthorized)"
                          : code == "proto" ? "the lobby speaks another protocol version"
                          : code == "limit" ? "the lobby refused a new account from this address today"
                                            : "the lobby did not accept the sign-in (" + code + ")";
        close();
        return false;
    }
    if (hello_reply) *hello_reply = std::move(*reply);
    return true;
}

std::optional<Json> LobbySession::call(const std::string& op, Json body, std::string* code) {
    if (!socket_) {
        if (code) *code = kUnreachable;
        return std::nullopt;
    }
    const uint32_t seq = ++seq_;
    if (!socket_->send_text(Envelope(op, seq, std::move(body)))) {
        if (code) *code = kLinkLost;
        return std::nullopt;
    }
    WebSocket* socket = socket_.get();
    auto answer = std::async(std::launch::async, [socket, seq]() -> std::optional<Json> {
        std::string text;
        while (socket->receive(text)) {
            std::optional<Json> m = Json::parse(text);
            if (m && m->is_object() && m->has("re") && (*m)["re"].as_int() == seq) return m;
        }
        return std::nullopt;
    });
    if (answer.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        socket_->close();
        answer.wait();
        socket_.reset();
        if (code) *code = "timeout";
        return std::nullopt;
    }
    std::optional<Json> m = answer.get();
    if (!m) {
        socket_.reset();
        if (code) *code = kLinkLost;
        return std::nullopt;
    }
    const std::string result = (*m)["code"].as_string();
    if (code) *code = result;
    if (result != "ok") return std::nullopt;
    return m;
}

void LobbySession::close() {
    if (socket_) socket_->close();
    socket_.reset();
}

} // namespace dcp::net
