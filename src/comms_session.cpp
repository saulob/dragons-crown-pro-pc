#include "comms_session.h"

#include <cstdio>
#include <cstring>
#include <memory>

#include "comms_bridge.h"
#include "comms_session_logic.h"
#include "net/peer_socket.h"
#include "runtime_online.h"

namespace {

using namespace dcp;

comms::Room RoomNow() {
    const online::CommsRoom room = online::comms_room();
    comms::Room out;
    out.slot = room.slot;
    out.members = room.members;
    for (const online::CommsPeer& peer : room.peers) out.peers.push_back({peer.account, peer.slot});
    return out;
}

bool SendTo(uint64_t account, const uint8_t* body, size_t size) {
    const std::shared_ptr<net::PeerSocket> socket = online::peer_socket();
    return socket && socket->send_comms(account, body, size);
}

std::vector<comms::Received> TakeReceived() {
    std::vector<comms::Received> out;
    const std::shared_ptr<net::PeerSocket> socket = online::peer_socket();
    if (!socket) return out;
    for (const net::PeerSocket::CommsPacket& packet : socket->take_comms()) {
        comms::Received received;
        received.account = packet.account;
        std::memcpy(received.body.data(), packet.body.data(), received.body.size());
        out.push_back(received);
    }
    return out;
}

comms::ReportResult LobbyReportResult() {
    const online::CommsReportResult result = online::comms_report_result();
    comms::ReportResult out;
    out.state = result.accepted ? comms::ReportState::kAccepted : result.failed ? comms::ReportState::kFailed : comms::ReportState::kNone;
    out.code = result.code;
    return out;
}

comms::Session& Shared() {
    static comms::Session session([] {
        comms::Host host;
        host.now_ms = [] { return net::now_ms(); };
        host.room = RoomNow;
        host.bridge_active = [] { return comms_bridge_active() != 0; };
        host.unit_running = [](int slot) { return comms_bridge_unit_running(slot) != 0; };
        host.send = SendTo;
        host.show = [](int slot, int id) { comms_bridge_show(slot, id); };
        host.take = TakeReceived;
        host.report_open = [] { return online::comms_report_open(); };
        host.report_send = [](const std::array<uint64_t, comms::kMessageCount>& counts) { return online::comms_report_send(counts); };
        host.report_result = LobbyReportResult;
        host.log = [](const std::string& line) { std::printf("%s\n", line.c_str()); };
        return host;
    }());
    return session;
}

void Pump() {
    Shared().Pump();
}

} // namespace

extern "C" void comms_session_register(void) {
    online::set_pump_hook(Pump);
    std::printf("Comms Wheel telemetry: reporter registered\n");
}

extern "C" int comms_session_available(void) {
    return Shared().Available() ? 1 : 0;
}

extern "C" int comms_session_send(int id) {
    return Shared().Send(id) ? 1 : 0;
}
