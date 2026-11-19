#include "net/relay_ends.h"

#include <utility>

namespace dcp::net {

void RelayEnds::Set(Forward forward, std::shared_ptr<PeerSocket> socket) {
    // Declared before the lock so it is destroyed after it: the socket's destructor sends CLOSE through Send().
    std::shared_ptr<PeerSocket> previous;
    std::lock_guard<std::mutex> lock(mutex_);
    forward_ = std::move(forward);
    previous = std::exchange(socket_, std::move(socket));
    paused_ = false;
}

void RelayEnds::Pause(bool paused) {
    std::lock_guard<std::mutex> lock(mutex_);
    paused_ = paused;
}

bool RelayEnds::Send(std::vector<uint8_t>&& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    return forward_ && !paused_ && forward_(std::move(frame));
}

void RelayEnds::Deliver(const uint8_t* data, size_t size) {
    std::shared_ptr<PeerSocket> socket;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        socket = socket_;
    }
    if (socket) socket->deliver_relay(data, size);
}

} // namespace dcp::net
