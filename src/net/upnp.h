// UPnP port mapping on the home router (Internet Gateway Device: WANIPConnection / WANPPPConnection). Optional: when
// the router has no UPnP, hole punching still connects most players (docs/ONLINE.md).
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <condition_variable>

#include "net/addr.h"

namespace dcp::net::upnp {

enum class State { kOff, kWorking, kMapped, kFailed };

struct Status {
    State state = State::kOff;
    Endpoint external;  // the router's WAN address and the mapped port
    std::string detail; // what happened, for the log and the connection test
};

// Maps UDP `local_port` on a helper thread, keeps the lease renewed, and removes the mapping when stopped.
class PortMapping {
public:
    PortMapping() = default;
    ~PortMapping();
    PortMapping(const PortMapping&) = delete;
    PortMapping& operator=(const PortMapping&) = delete;

    void start(uint16_t local_port);
    void stop();
    Status status() const;

private:
    void Run(uint16_t local_port);

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    Status status_;
    bool stopping_ = false;
};

// The connection test: map `port` for a minute and remove it again.
Status probe(uint16_t port);

} // namespace dcp::net::upnp
