#include "net/connection_test.h"

#include "net/addr.h"
#include "net/lobby_link.h"
#include "net/stun.h"
#include "net/upnp.h"

namespace dcp::net {

ConnectionReport run_connection_test(const OnlineConfig& config, const Identity& identity, const std::string& name,
                                     const std::string& game) {
    ConnectionReport r;
    auto line = [&r](bool ok, std::string label, std::string text) { r.lines.push_back({ok, std::move(label), std::move(text)}); };

    bool lobby_ok = false;
    std::string your_ip;
    if (config.lobby.empty()) {
        line(false, "Lobby", "No lobby server is set.");
    } else if (!identity.ok()) {
        line(false, "Lobby", "No online identity: " + identity.error);
    } else {
        LobbySession session;
        Json reply;
        std::string error;
        const uint64_t start = now_ms();
        if (session.open(config.lobby, hello_body(identity, name, game, false), &reply, &error)) {
            lobby_ok = true;
            your_ip = reply["ip"].as_string();
            r.friend_code = reply["friend_code"].as_string();
            line(true, "Lobby", "Connected in " + std::to_string(now_ms() - start) + " ms; it sees this PC as " + your_ip + ".");
        } else {
            line(false, "Lobby", "Cannot connect: " + error + ".");
        }
    }

    const auto servers = stun_servers(config.stun);
    bool udp_ok = false, punchable = false, public_address = false;
    Endpoint first, second;
    if (!sockets_ready()) {
        line(false, "UDP", "Sockets are unavailable.");
    } else if (servers.empty()) {
        line(true, "UDP", "STUN is turned off; the lobby's view of this address is used.");
    } else {
        const socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        const sockaddr_in any = to_sockaddr({0, 0});
        if (s != kNoSocket && bind(s, reinterpret_cast<const sockaddr*>(&any), sizeof(any)) == 0) {
            sockaddr_in bound{};
            socklen_t length = sizeof(bound);
            getsockname(s, reinterpret_cast<sockaddr*>(&bound), &length);
            std::vector<std::string> answered;
            for (const auto& [host, port] : servers) {
                const uint32_t ip = resolve_ipv4(host);
                Endpoint mapped;
                if (!ip || !stun::query(s, {ip, port}, 2500, mapped)) continue;
                (first.valid() ? second : first) = mapped;
                answered.push_back(host);
                if (second.valid()) break;
            }
            const uint32_t local = default_route_ip();
            udp_ok = first.valid();
            if (!udp_ok) {
                line(false, "UDP", "No STUN server answered: UDP may be blocked by a firewall.");
            } else {
                public_address = first.ip == local;
                punchable = !second.valid() || second == first;
                line(true, "Public address", endpoint_text(first) + " (via " + answered.front() + ").");
                if (public_address) {
                    line(true, "NAT", "None: this PC has a public address.");
                } else if (!second.valid()) {
                    line(true, "NAT", "Behind a router; only one STUN server answered, so the mapping type is unknown.");
                } else if (punchable) {
                    line(true, "NAT", "Behind a router that keeps one public port per socket: direct connections work.");
                } else {
                    line(false, "NAT", "Behind a router that changes the public port per destination (symmetric NAT): "
                                       "players behind a similar router may not reach you. UPnP can help.");
                }
                if (!your_ip.empty() && parse_ip(your_ip) && parse_ip(your_ip) != first.ip && classify(parse_ip(your_ip)) == AddressKind::kPublic) {
                    line(false, "Address", "The lobby and STUN see different addresses; your network may use several.");
                }
            }
        } else {
            line(false, "UDP", "Cannot open a UDP socket.");
        }
        close_socket(s);
    }

    bool upnp_public = false;
    if (config.upnp) {
        const upnp::Status u = upnp::probe(kGameUdpPort);
        if (u.state == upnp::State::kMapped) {
            const AddressKind wan = classify(u.external.ip);
            upnp_public = wan == AddressKind::kPublic;
            line(true, "UPnP", u.detail + (upnp_public ? "; router address " + ip_text(u.external.ip) + "."
                                           : "; but the router's own address " + ip_text(u.external.ip) +
                                                 " is not public (a second NAT, often the provider's)."));
        } else {
            line(false, "UPnP", "Not available: " + u.detail + ". Not required when the NAT check above passes.");
        }
    } else {
        line(true, "UPnP", "Turned off.");
    }

    r.ready = lobby_ok && (punchable || public_address || upnp_public || (udp_ok && !second.valid()));
    if (!lobby_ok) {
        r.verdict = "Not ready: the lobby cannot be reached.";
    } else if (r.ready) {
        r.verdict = "Ready for online play.";
    } else if (!udp_ok && !servers.empty()) {
        r.verdict = "Limited: UDP seems blocked; direct connections to other players will likely fail.";
    } else {
        r.verdict = "Limited: some players may not be able to connect to you.";
    }
    return r;
}

} // namespace dcp::net
