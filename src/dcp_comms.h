// Comms Wheel core: eight fixed cooperative messages, the radial layout, the hold-to-open input rules, the wire body and
// the usage counters. Only message ids exist here; every visible text comes from comms_text.h, resolved by the client
// that shows it. Nothing here reads or writes the game.
#ifndef DCP_COMMS_H
#define DCP_COMMS_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace dcp::comms {

inline constexpr int kMessageCount = 8;
inline constexpr int kSlots = 4;

enum class Message : uint8_t { kHello, kThanks, kGoodJob, kFollowMe, kGroupUp, kNeedHelp, kReady, kLetsGo };

constexpr bool ValidId(int id) {
    return id >= 0 && id < kMessageCount;
}

// The stable key of an id (telemetry, logs, icons); never shown to players.
constexpr const char* IdKey(int id) {
    constexpr const char* keys[kMessageCount] = {"hello",     "thanks",    "good_job", "follow_me",
                                                 "group_up",  "need_help", "ready",    "lets_go"};
    return ValidId(id) ? keys[id] : "";
}

// Wheel positions clockwise from the top; the order never changes.
inline constexpr int kSectors = 8;
inline constexpr std::array<Message, kSectors> kSectorMessages = {
    Message::kReady,  Message::kHello,  Message::kNeedHelp, Message::kLetsGo,
    Message::kGroupUp, Message::kFollowMe, Message::kGoodJob, Message::kThanks};

constexpr int MessageAtSector(int sector) {
    return sector >= 0 && sector < kSectors ? static_cast<int>(kSectorMessages[static_cast<size_t>(sector)]) : -1;
}

constexpr int SectorOfMessage(int id) {
    for (int i = 0; i < kSectors; ++i) {
        if (static_cast<int>(kSectorMessages[static_cast<size_t>(i)]) == id) {
            return i;
        }
    }
    return -1;
}

inline constexpr float kStickDeadZone = 0.5f;

// Direction in screen coordinates (x right, y down). Sector 0 is centred on up and each one spans 45 degrees; -1 inside
// the dead zone.
inline int SectorAt(float x, float y, float dead_zone) {
    if (!std::isfinite(x) || !std::isfinite(y) || std::hypot(x, y) < dead_zone || (x == 0.0f && y == 0.0f)) {
        return -1;
    }
    constexpr float kPi = 3.14159265358979f;
    float degrees = std::atan2(x, -y) * (180.0f / kPi);
    if (degrees < 0.0f) {
        degrees += 360.0f;
    }
    return static_cast<int>((degrees + 22.5f) / 45.0f) % kSectors;
}

// The wheel exists only for a cooperative online room of at least two players while the game is playing.
inline constexpr int kMinPlayers = 2;

constexpr bool WheelAvailable(bool online_enabled, int room_members, bool bridge_active, bool own_unit_running) {
    return online_enabled && room_members >= kMinPlayers && bridge_active && own_unit_running;
}

// Modal input: one press of D-pad Up (controller) or of the wheel key (keyboard) opens the wheel and it stays open until
// a message is confirmed or the wheel is cancelled. The controller picks with the right stick and confirms with Cross; the
// keyboard picks with the mouse or the direction keys and confirms with the left mouse button or the Cross key.
enum class Source : uint8_t { kNone, kController, kKeyboard };

inline constexpr uint8_t kNavUp = 1, kNavDown = 2, kNavLeft = 4, kNavRight = 8;

struct WheelInput {
    bool available = false;   // an online room of at least two players, no modal game UI
    bool pad_up = false;      // D-pad Up held
    bool key_down = false;    // the wheel key held
    float stick_x = 0.0f, stick_y = 0.0f; // right stick, screen coordinates
    float mouse_x = 0.0f, mouse_y = 0.0f; // mouse offset from the wheel centre, same units
    uint8_t nav = 0;          // direction keys held (kNav*)
    bool confirm_held = false; // Cross held, from the pad or its key
    bool cancel_held = false;  // Circle held
    bool click = false;       // left mouse button pressed since the last step
    bool cancel_click = false; // Escape or right mouse button pressed since the last step
};

struct WheelOutput {
    bool open = false;
    int selected = -1; // sector, -1 for none
    int send = -1;     // message id to send now
    bool hide_pad_up = false;
    bool hide_confirm = false;
    bool hide_cancel = false;
    bool hide_right_stick = false;
    bool hide_keyboard_axes = false; // the keyboard session owns the direction keys
    Source source = Source::kNone;
};

class WheelController {
public:
    explicit WheelController(float dead_zone = kStickDeadZone) : dead_zone_(dead_zone) {}

    WheelOutput Step(const WheelInput& in) {
        const bool pad_pressed = in.pad_up && !prev_pad_up_;
        const bool key_pressed = in.key_down && !prev_key_down_;
        prev_pad_up_ = in.pad_up;
        prev_key_down_ = in.key_down;

        WheelOutput out;
        if (source_ == Source::kNone) {
            if (!in.available || (!pad_pressed && !key_pressed)) {
                Release(in);
                return Hidden(out);
            }
            Open(pad_pressed ? Source::kController : Source::kKeyboard, in);
            if (source_ == Source::kController) {
                held_pad_up_ = true;
            }
        } else if (!in.available) {
            Close();
        }

        if (source_ != Source::kNone) {
            const bool confirm_edge = in.click || (in.confirm_held && !prev_confirm_);
            const bool cancel_edge = in.cancel_click || (in.cancel_held && !prev_cancel_);
            Select(in);
            if (cancel_edge) {
                held_circle_ = in.cancel_held;
                Close();
            } else if (confirm_edge && selected_ >= 0) {
                out.send = MessageAtSector(selected_);
                held_cross_ = in.confirm_held;
                Close();
            }
        }
        prev_confirm_ = in.confirm_held;
        prev_cancel_ = in.cancel_held;
        prev_nav_ = in.nav;
        Release(in);

        out.source = source_;
        out.open = source_ != Source::kNone;
        out.selected = out.open ? selected_ : -1;
        out.hide_right_stick = source_ == Source::kController;
        out.hide_keyboard_axes = source_ == Source::kKeyboard;
        return Hidden(out);
    }

    bool Active() const { return source_ != Source::kNone; }
    void Reset() {
        source_ = Source::kNone;
        selected_ = -1;
        held_pad_up_ = held_cross_ = held_circle_ = false;
    }

private:
    void Open(Source source, const WheelInput& in) {
        source_ = source;
        selected_ = -1;
        last_stick_ = SectorAt(in.stick_x, in.stick_y, dead_zone_);
        last_mouse_ = SectorAt(in.mouse_x, in.mouse_y, dead_zone_);
        prev_confirm_ = in.confirm_held;
        prev_cancel_ = in.cancel_held;
        prev_nav_ = in.nav;
    }

    void Close() {
        source_ = Source::kNone;
        selected_ = -1;
    }

    // The selection stays on the last sector chosen: a stick back at centre or a released key changes nothing, and each
    // input only moves it when it changes sector, so a mouse left pointing elsewhere does not undo a key press.
    void Select(const WheelInput& in) {
        if (source_ == Source::kController) {
            Follow(SectorAt(in.stick_x, in.stick_y, dead_zone_), last_stick_);
            return;
        }
        Follow(SectorAt(in.mouse_x, in.mouse_y, dead_zone_), last_mouse_);
        if ((in.nav & ~prev_nav_) != 0) {
            const float x = static_cast<float>((in.nav & kNavRight ? 1 : 0) - (in.nav & kNavLeft ? 1 : 0));
            const float y = static_cast<float>((in.nav & kNavDown ? 1 : 0) - (in.nav & kNavUp ? 1 : 0));
            const int sector = SectorAt(x, y, dead_zone_);
            if (sector >= 0) {
                selected_ = sector;
            }
        }
    }

    void Follow(int sector, int& last) {
        if (sector >= 0 && sector != last) {
            selected_ = sector;
        }
        last = sector;
    }

    WheelOutput Hidden(WheelOutput out) const {
        const bool open = source_ != Source::kNone;
        out.hide_pad_up = open || held_pad_up_;
        out.hide_confirm = open || held_cross_;
        out.hide_cancel = open || held_circle_;
        return out;
    }

    // A button the wheel took stays hidden from the game until it is let go.
    void Release(const WheelInput& in) {
        held_pad_up_ = held_pad_up_ && in.pad_up;
        held_cross_ = held_cross_ && in.confirm_held;
        held_circle_ = held_circle_ && in.cancel_held;
    }

    Source source_ = Source::kNone;
    int selected_ = -1;
    int last_stick_ = -1, last_mouse_ = -1;
    bool prev_pad_up_ = false, prev_key_down_ = false, prev_confirm_ = false, prev_cancel_ = false;
    bool held_pad_up_ = false, held_cross_ = false, held_circle_ = false;
    uint8_t prev_nav_ = 0;
    float dead_zone_;
};

// One message at most every two seconds from this player; a remote sender gets a little slack for network jitter.
inline constexpr uint64_t kLocalMinGapMs = 2000;
inline constexpr uint64_t kRemoteMinGapMs = 1500;

class RateLimit {
public:
    explicit RateLimit(uint64_t min_gap_ms) : gap_(min_gap_ms) {}

    bool Allow(uint64_t now_ms) {
        if (has_last_ && now_ms - last_ms_ < gap_) {
            return false;
        }
        has_last_ = true;
        last_ms_ = now_ms;
        return true;
    }

    void Reset() { has_last_ = false; }

private:
    uint64_t gap_;
    uint64_t last_ms_ = 0;
    bool has_last_ = false;
};

// What crosses the network: a version, the sender's party slot, the message id and a sequence number. Never any text.
inline constexpr uint8_t kWireVersion = 1;
inline constexpr size_t kWireBytes = 4;

struct WireMessage {
    uint8_t slot = 0;
    uint8_t id = 0;
    uint8_t seq = 0;
};

inline void EncodeWire(const WireMessage& message, uint8_t out[kWireBytes]) {
    out[0] = kWireVersion;
    out[1] = message.slot;
    out[2] = message.id;
    out[3] = message.seq;
}

inline std::optional<WireMessage> DecodeWire(const uint8_t* data, size_t size) {
    if (!data || size != kWireBytes || data[0] != kWireVersion || data[1] >= kSlots || !ValidId(data[2])) {
        return std::nullopt;
    }
    return WireMessage{data[1], data[2], data[3]};
}

// The messages a client accepts from remote senders: each slot is limited and a repeated sequence is dropped (the sender
// transmits every message twice).
class Inbox {
public:
    bool Accept(const WireMessage& message, uint64_t now_ms) {
        if (message.slot >= kSlots || !ValidId(message.id)) {
            return false;
        }
        Slot& slot = slots_[message.slot];
        if (slot.has_seq && slot.seq == message.seq) {
            return false;
        }
        if (slot.has_last && now_ms - slot.last_ms < kRemoteMinGapMs) {
            return false;
        }
        slot.has_seq = slot.has_last = true;
        slot.seq = message.seq;
        slot.last_ms = now_ms;
        return true;
    }

    void Reset() { slots_ = {}; }

private:
    struct Slot {
        bool has_seq = false, has_last = false;
        uint8_t seq = 0;
        uint64_t last_ms = 0;
    };
    std::array<Slot, kSlots> slots_{};
};

// Aggregate use of the wheel, keyed by message id.
struct UsageCounts {
    uint64_t total = 0;
    std::array<uint64_t, kMessageCount> by_id{};

    bool Add(int id, uint64_t count = 1) {
        if (!ValidId(id)) {
            return false;
        }
        by_id[static_cast<size_t>(id)] += count;
        total += count;
        return true;
    }

    double Percent(int id) const {
        return ValidId(id) && total ? 100.0 * static_cast<double>(by_id[static_cast<size_t>(id)]) / static_cast<double>(total) : 0.0;
    }

    // The most used id; the lowest wins a tie; none while nothing was used.
    std::optional<int> MostUsed() const {
        if (total == 0) {
            return std::nullopt;
        }
        int best = 0;
        for (int i = 1; i < kMessageCount; ++i) {
            if (by_id[static_cast<size_t>(i)] > by_id[static_cast<size_t>(best)]) {
                best = i;
            }
        }
        return best;
    }

    bool Empty() const { return total == 0; }
};

} // namespace dcp::comms

#endif
