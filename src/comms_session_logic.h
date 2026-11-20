// Comms Wheel session rules without the transport: who may send and receive, rate limits, the version gate and the
// telemetry batches. The runtime supplies the room, the peer socket and the game's balloon through Host.
#ifndef DCP_COMMS_SESSION_LOGIC_H
#define DCP_COMMS_SESSION_LOGIC_H

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "dcp_comms.h"

namespace dcp::comms {

struct Peer {
    uint64_t account = 0;
    int slot = -1;
};

struct Room {
    int slot = -1;   // this player's party slot
    int members = 0; // players in the room, this one included; 0 outside a room or with Online Play off
    std::vector<Peer> peers;
};

struct Received {
    uint64_t account = 0;
    std::array<uint8_t, kWireBytes> body{};
};

// The first use is reported after a few seconds so the lobby shows it early; later uses are batched, at most one report
// every two minutes, the lobby's per-account refill (commsReportAccount). Counts stay pending until the lobby accepts
// them, and return to pending when it does not.
inline constexpr uint64_t kFirstReportDelayMs = 5000;
inline constexpr uint64_t kReportGapMs = 120000;
inline constexpr uint64_t kReportRetryMs = 120000;
inline constexpr uint64_t kReportReplyMs = 30000;

enum class ReportState : uint8_t { kNone, kAccepted, kFailed };

struct ReportResult {
    ReportState state = ReportState::kNone;
    std::string code;
};

class ReportBatch {
public:
    // True when this use started a new pending batch.
    bool Add(int id, uint64_t now_ms) {
        const bool first = pending_.Empty();
        if (!pending_.Add(id)) {
            return false;
        }
        if (first) {
            due_ms_ = reported_ ? std::max(now_ms, last_sent_ms_ + kReportGapMs) : now_ms + kFirstReportDelayMs;
        }
        return first;
    }

    std::optional<std::array<uint64_t, kMessageCount>> Take(uint64_t now_ms, bool leaving) {
        if (!in_flight_.Empty() || pending_.Empty() || now_ms < retry_ms_ || (!leaving && now_ms < due_ms_)) {
            return std::nullopt;
        }
        in_flight_ = pending_;
        pending_ = UsageCounts{};
        sent_ms_ = last_sent_ms_ = now_ms;
        reported_ = true;
        return in_flight_.by_id;
    }

    uint64_t Accepted() {
        const uint64_t uses = in_flight_.total;
        in_flight_ = UsageCounts{};
        return uses;
    }

    void Failed(uint64_t now_ms) {
        Requeue();
        retry_ms_ = now_ms + kReportRetryMs;
    }

    void NotSent() { Requeue(); }

    void Expire(uint64_t now_ms) {
        if (!in_flight_.Empty() && now_ms - sent_ms_ > kReportReplyMs) {
            Failed(now_ms);
        }
    }

    uint64_t Pending() const { return pending_.total + in_flight_.total; }

private:
    void Requeue() {
        for (int i = 0; i < kMessageCount; ++i) {
            pending_.Add(i, in_flight_.by_id[static_cast<size_t>(i)]);
        }
        in_flight_ = UsageCounts{};
        due_ms_ = 0;
    }

    UsageCounts pending_, in_flight_;
    bool reported_ = false;
    uint64_t due_ms_ = 0, last_sent_ms_ = 0, sent_ms_ = 0, retry_ms_ = 0;
};

struct Host {
    std::function<uint64_t()> now_ms;
    std::function<Room()> room;
    std::function<bool()> bridge_active;
    std::function<bool(int slot)> unit_running;
    std::function<bool(uint64_t account, const uint8_t* body, size_t size)> send;
    std::function<void(int slot, int id)> show;
    std::function<std::vector<Received>()> take;
    std::function<bool()> report_open;
    std::function<bool(const std::array<uint64_t, kMessageCount>& counts)> report_send;
    std::function<ReportResult()> report_result;
    std::function<void(const std::string& line)> log;
};

class Session {
public:
    explicit Session(Host host) : host_(std::move(host)) {}

    // The wheel may open: an online room of at least two players, the balloon bridge in and this player's unit running.
    bool Available() const {
        const Room room = host_.room();
        return AvailableIn(room);
    }

    // A message chosen on the wheel; false when it was dropped.
    bool Send(int id) {
        if (!ValidId(id)) {
            return false;
        }
        const Room room = host_.room();
        if (!AvailableIn(room)) {
            return false;
        }
        const uint64_t now = host_.now_ms();
        uint8_t seq = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!local_.Allow(now)) {
                return false;
            }
            seq = ++seq_;
            NoteLocalSend(id, now);
        }
        WireMessage message;
        message.slot = static_cast<uint8_t>(room.slot);
        message.id = static_cast<uint8_t>(id);
        message.seq = seq;
        uint8_t body[kWireBytes];
        EncodeWire(message, body);
        for (const Peer& peer : room.peers) {
            if (peer.account) {
                host_.send(peer.account, body, kWireBytes);
            }
        }
        host_.show(room.slot, id);
        return true;
    }

    // Once per game frame, outside the runtime's locks.
    void Pump() {
        const Room room = host_.room();
        const uint64_t now = host_.now_ms();
        const bool in_room = room.members >= kMinPlayers;
        bool leaving = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            leaving = was_in_room_ && !in_room;
            if (leaving) {
                inbox_.Reset();
            }
            was_in_room_ = in_room;
        }
        for (const Received& received : host_.take()) {
            Accept(room, received, now);
        }
        ReportResult result;
        if (host_.report_result) {
            result = host_.report_result();
        }
        if (result.state == ReportState::kAccepted) {
            uint64_t uses = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                uses = report_.Accepted();
            }
            Log("reported " + std::to_string(uses) + " use(s)");
        } else if (result.state == ReportState::kFailed) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                report_.Failed(now);
            }
            Log("report failed: " + result.code);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            report_.Expire(now);
        }
        if (host_.report_open && host_.report_open()) {
            std::optional<std::array<uint64_t, kMessageCount>> counts;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                counts = report_.Take(now, leaving);
            }
            if (counts && !host_.report_send(*counts)) {
                std::lock_guard<std::mutex> lock(mutex_);
                report_.NotSent();
            }
        }
    }

    uint64_t PendingReport() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return report_.Pending();
    }

private:
    // The only telemetry increment: one accepted local send.
    void NoteLocalSend(int id, uint64_t now) {
        if (report_.Add(id, now)) {
            Log("report pending");
        }
    }

    void Log(const std::string& text) const {
        if (host_.log) {
            host_.log("Comms Wheel telemetry: " + text);
        }
    }

    bool AvailableIn(const Room& room) const {
        if (!ValidSlotIndex(room.slot) ||
            !WheelAvailable(room.members > 0, room.members, host_.bridge_active(), host_.unit_running(room.slot))) {
            return false;
        }
        return true;
    }

    static bool ValidSlotIndex(int slot) { return slot >= 0 && slot < kSlots; }

    void Accept(const Room& room, const Received& received, uint64_t now) {
        const auto message = DecodeWire(received.body.data(), received.body.size());
        if (!message || room.members < kMinPlayers) {
            return;
        }
        for (const Peer& peer : room.peers) {
            if (peer.account != received.account || !received.account) {
                continue;
            }
            if (peer.slot != message->slot || peer.slot == room.slot) {
                return;
            }
            bool accepted = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                accepted = inbox_.Accept(*message, now);
            }
            if (accepted) {
                host_.show(message->slot, message->id);
            }
            return;
        }
    }

    Host host_;
    mutable std::mutex mutex_;
    RateLimit local_{kLocalMinGapMs};
    Inbox inbox_;
    ReportBatch report_;
    uint8_t seq_ = 0;
    bool was_in_room_ = false;
};

} // namespace dcp::comms

#endif
