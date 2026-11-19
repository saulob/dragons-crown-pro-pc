// The two ends of the lobby relay for the game's socket: frames out through the lobby link, frames in to the socket.
// Both ends live on other threads (the socket's, the lobby link's).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "net/peer_socket.h"

namespace dcp::net {

class RelayEnds {
public:
    using Forward = std::function<bool(std::vector<uint8_t>&&)>;

    // A replaced socket may be released here for the last time; that happens after the lock is released.
    void Set(Forward forward, std::shared_ptr<PeerSocket> socket);
    // While the lobby link reconnects, frames are refused at the source (the link layer retransmits them).
    void Pause(bool paused);
    bool Send(std::vector<uint8_t>&& frame);
    void Deliver(const uint8_t* data, size_t size);

private:
    std::mutex mutex_;
    Forward forward_;
    std::shared_ptr<PeerSocket> socket_;
    bool paused_ = false;
};

} // namespace dcp::net
