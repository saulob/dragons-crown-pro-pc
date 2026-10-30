#include "net/connection_test.h"

#include "net/addr.h"
#include "net/lobby_link.h"
#include "net/stun.h"
#include "net/upnp.h"

namespace dcp::net {

ConnectionReport summarize_connection(const ConnectionFacts& f) {
    using Mark = ConnectionReport::Mark;
    using Router = ConnectionFacts::Router;
    ConnectionReport r;
    auto line = [&r](Mark mark, std::string label, std::string text) {
        r.lines.push_back({mark, std::move(label), std::move(text)});
    };

    if (!f.identity_ok) {
        line(Mark::kProblem, "Online service", "Your online identity could not be created.");
    } else if (f.lobby_ok) {
        line(Mark::kOk, "Online service",
             f.lobby_latency_ms >= 0 ? "Connected \xC2\xB7 " + std::to_string(f.lobby_latency_ms) + " ms" : "Connected.");
    } else {
        line(Mark::kProblem, "Online service", "Cannot reach the online service. Check your internet connection.");
    }

    const bool direct = f.udp_ok && (f.public_address || !f.second_answer || f.same_mapping);
    const bool router_opened = f.router == Router::kMapped;
    if (f.stun_off) {
        line(Mark::kNote, "Direct connections", "Not checked.");
    } else if (!f.udp_ok) {
        line(Mark::kProblem, "Direct connections", "May be blocked by your network or firewall.");
    } else if (direct) {
        line(Mark::kOk, "Direct connections", f.second_answer || f.public_address ? "Available." : "Probably available.");
    } else if (router_opened) {
        line(Mark::kOk, "Direct connections", "Available through your router.");
    } else {
        line(Mark::kProblem, "Direct connections", "Your router may stop some players from connecting to you.");
        line(Mark::kProblem, "Router configuration",
             f.router == Router::kMappedBehindNat
                 ? "Another router is in front of yours (often your provider's), so it cannot be opened automatically."
                 : "Automatic setup is not available on your router.");
    }

    if (f.udp_ok && f.second_answer && !f.same_mapping && !f.public_address) {
        r.details.push_back("NAT: another public port per destination (symmetric NAT)");
    } else if (f.udp_ok && !f.second_answer) {
        r.details.push_back("NAT: type unknown, only one STUN server answered");
    }
    r.details.push_back(f.router == Router::kMapped ? "router: UPnP mapping made"
                        : f.router == Router::kMappedBehindNat
                            ? "router: UPnP mapping made, but the router's own address is not public (double NAT or "
                              "carrier-grade NAT)"
                            : "router: no UPnP mapping");

    r.ready = f.identity_ok && f.lobby_ok && (direct || router_opened);
    if (!f.identity_ok || !f.lobby_ok) {
        r.verdict = "Not ready: cannot reach the online service";
    } else if (r.ready) {
        r.verdict = "Ready for online play";
    } else if (!f.udp_ok && !f.stun_off) {
        r.verdict = "Limited: direct connections may be blocked by your network";
    } else {
        r.verdict = "Limited: some players may not be able to connect to you";
    }
    return r;
}

ConnectionReport run_connection_test(const OnlineConfig& config, const Identity& identity, const std::string& name,
                                     const std::string& game) {
    ConnectionFacts facts;
    std::vector<std::string> details;
    std::string friend_code, lobby_ip;

    facts.identity_ok = identity.ok();
    if (!facts.identity_ok) {
        details.push_back("no online identity: " + identity.error);
    } else {
        LobbySession session;
        Json reply;
        std::string error;
        const uint64_t start = now_ms();
        facts.lobby_ok = session.open(config.lobby, hello_body(identity, name, game, false), &reply, &error);
        if (facts.lobby_ok) {
            facts.lobby_latency_ms = int64_t(now_ms() - start);
            lobby_ip = reply["ip"].as_string();
            friend_code = reply["friend_code"].as_string();
            details.push_back("lobby " + config.lobby + " answered in " + std::to_string(facts.lobby_latency_ms) +
                              " ms; it sees " + lobby_ip);
        } else {
            details.push_back("lobby " + config.lobby + ": " + error);
        }
    }

    const auto servers = stun_servers(config.stun);
    facts.stun_off = servers.empty();
    if (!sockets_ready()) {
        details.push_back("sockets are unavailable");
    } else if (!servers.empty()) {
        const socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        const sockaddr_in any = to_sockaddr({0, 0});
        if (s != kNoSocket && bind(s, reinterpret_cast<const sockaddr*>(&any), sizeof(any)) == 0) {
            Endpoint first, second;
            for (const auto& [host, port] : servers) {
                const uint32_t ip = resolve_ipv4(host);
                Endpoint mapped;
                if (!ip || !stun::query(s, {ip, port}, 2500, mapped)) {
                    details.push_back("STUN " + host + ": no answer");
                    continue;
                }
                (first.valid() ? second : first) = mapped;
                details.push_back("STUN " + host + ": " + endpoint_text(mapped));
                if (second.valid()) break;
            }
            facts.udp_ok = first.valid();
            facts.public_address = facts.udp_ok && first.ip == default_route_ip();
            facts.second_answer = second.valid();
            facts.same_mapping = second.valid() && second == first;
            const uint32_t seen = parse_ip(lobby_ip);
            if (facts.udp_ok && seen && seen != first.ip && classify(seen) == AddressKind::kPublic) {
                details.push_back("the lobby and STUN see different addresses");
            }
        } else {
            details.push_back("cannot open a UDP socket");
        }
        close_socket(s);
    }

    if (config.upnp) {
        const upnp::Status u = upnp::probe(kGameUdpPort);
        if (u.state == upnp::State::kMapped) {
            facts.router = classify(u.external.ip) == AddressKind::kPublic ? ConnectionFacts::Router::kMapped
                                                                            : ConnectionFacts::Router::kMappedBehindNat;
            details.push_back("UPnP: " + u.detail + "; router address " + ip_text(u.external.ip));
        } else {
            details.push_back("UPnP: not available: " + u.detail);
        }
    } else {
        details.push_back("UPnP: turned off by DCP_UPNP");
    }

    ConnectionReport report = summarize_connection(facts);
    report.friend_code = std::move(friend_code);
    details.insert(details.end(), report.details.begin(), report.details.end());
    report.details = std::move(details);
    return report;
}

} // namespace dcp::net
