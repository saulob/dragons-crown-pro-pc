// SPDX-License-Identifier: GPL-2.0-or-later
// The in-game frame counter (F2) and Ping (F3) switches, and the Ping readout. The Ping is the P2P round trip to the
// room's peers (runtime_online_peer_ping_ms), never the online service's.
#pragma once

#include <cstdint>
#include <string>

#include <SDL3/SDL_scancode.h>

namespace DcpPing {

inline constexpr int kFpsKey = SDL_SCANCODE_F2;
inline constexpr int kPingKey = SDL_SCANCODE_F3;

enum class HudKey { None, Fps, Ping };

// F3 belongs to Ping only while Online Play is on; a held key does nothing.
constexpr HudKey HudKeyFor(int scancode, bool repeat, bool online) {
    if (repeat) {
        return HudKey::None;
    }
    if (scancode == kFpsKey) {
        return HudKey::Fps;
    }
    return scancode == kPingKey && online ? HudKey::Ping : HudKey::None;
}

// The shown value follows the transport's own smoothed round trip, read about three times a second. A first value
// and a lost peer show at the next read, so a peer that left never keeps its last value.
class Readout {
public:
    static constexpr int64_t kRefreshMs = 300;

    template <typename Sample>
    int Shown(int64_t now_ms, Sample sample) {
        if (!started_ || now_ms >= next_ms_) {
            value_ = sample();
            next_ms_ = now_ms + kRefreshMs;
            started_ = true;
        }
        return value_;
    }

    void Reset() { started_ = false; }

private:
    bool started_ = false;
    int value_ = -1;
    int64_t next_ms_ = 0;
};

inline std::string Text(int ping_ms) {
    return ping_ms < 0 ? "Ping: --" : "Ping: " + std::to_string(ping_ms) + " ms";
}

} // namespace DcpPing
