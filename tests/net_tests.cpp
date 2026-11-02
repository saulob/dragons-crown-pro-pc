// Focused tests of the online layer (src/net): JSON, hashes, identity, settings, STUN, lobby URLs and the peer
// transport over loopback (including a lossy relay). With DCP_TEST_LOBBY=ws://127.0.0.1:<port>/ set to a running
// development lobby (server/lobby/local/server.js) the lobby link and two-player room flow are exercised too.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <random>
#include <thread>
#include <vector>

#include "net/addr.h"
#include "net/connection_test.h"
#include "net/crypto.h"
#include "net/identity.h"
#include "net/json.h"
#include "net/lobby_link.h"
#include "net/online_config.h"
#include "net/online_version.h"
#include "net/peer_socket.h"
#include "net/relay_frame.h"
#include "net/stun.h"
#include "net/websocket.h"

namespace fs = std::filesystem;
using namespace dcp::net;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);          \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

bool WaitFor(const std::function<bool()>& done, int timeout_ms) {
    const uint64_t deadline = now_ms() + uint64_t(timeout_ms);
    while (now_ms() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

void Group(const char* name, const std::function<void()>& run) {
    const int before = g_failures;
    run();
    std::printf("GROUP|%s|%s\n", name, g_failures == before ? "PASS" : "FAIL");
    std::fflush(stdout);
}

std::string ReadSource(const char* relative) {
    std::ifstream in(fs::path(DCP_SOURCE_DIR) / relative, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void TestJson() {
    auto parsed = Json::parse(R"({"op":"hello","seq":7,"big":"18446744073709551615","neg":-5,"f":1.5,"ok":true,
        "list":[1,"two",null,{"x":[]}],"esc":"a\"b\\c\n\u00e9\ud83d\ude00"})");
    CHECK(parsed && parsed->is_object());
    const Json& j = *parsed;
    CHECK(j["op"].as_string() == "hello" && j["seq"].as_int() == 7);
    CHECK(j["big"].as_u64() == UINT64_MAX && j["neg"].as_int() == -5 && j["neg"].as_u64() == 0);
    CHECK(j["f"].as_int() == 1 && j["ok"].as_bool() && j["missing"].is_null() && j["missing"]["deeper"].is_null());
    CHECK(j["list"].size() == 4 && j["list"].items()[1].as_string() == "two" && j["list"].items()[3]["x"].is_array());
    CHECK(j["esc"].as_string() == "a\"b\\c\n\xc3\xa9\xf0\x9f\x98\x80");
    const auto again = Json::parse(j.dump());
    CHECK(again && again->dump() == j.dump());
    for (const char* bad : {"", "{", "[1,]", "{\"a\":}", "nul", "\"unterminated", "{\"a\" 1}", "01x", "[1] 2", "\"\x01\"",
                            "{\"a\":\"\\q\"}", "1e999"}) {
        CHECK(!Json::parse(bad));
    }
    std::string deep(40, '[');
    deep += std::string(40, ']');
    CHECK(!Json::parse(deep));
    CHECK(Json::parse("[[[1]]]"));
    Json o = Json::Object();
    o.set("b", 1);
    o.set("a", "x\x01");
    o.set("b", 2);
    CHECK(o.dump() == R"({"b":2,"a":"x\u0001"})");
    CHECK(Json("12345678901234567890").as_u64() == 12345678901234567890ull);
    CHECK(Json("123456789012345678901").as_u64() == 0 && Json("12a").as_u64() == 0);

    std::vector<uint8_t> bytes;
    CHECK(hex_decode("00ff10Ab", bytes) && bytes.size() == 4 && bytes[1] == 0xff && bytes[3] == 0xab);
    CHECK(hex_encode(bytes.data(), bytes.size()) == "00ff10ab");
    CHECK(!hex_decode("abc", bytes) && !hex_decode("zz", bytes));
}

void TestCrypto() {
    const Digest abc = sha256("abc", 3);
    CHECK(hex_encode(abc.data(), abc.size()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string long_text(1000, 'a');
    const Digest longer = sha256(long_text.data(), long_text.size());
    CHECK(hex_encode(longer.data(), longer.size()) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
    // RFC 4231 test case 2.
    const Digest mac = hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28);
    CHECK(hex_encode(mac.data(), mac.size()) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    uint8_t a[16] = {}, b[16] = {};
    CHECK(random_bytes(a, sizeof(a)) && random_bytes(b, sizeof(b)) && std::memcmp(a, b, sizeof(a)) != 0);
}

void TestIdentity(const fs::path& base) {
    const fs::path a = base / "test-client-a", b = base / "test-client-b";
    fs::remove_all(a);
    fs::remove_all(b);
    CHECK(!load_identity(a, false).ok());
    const Identity first = load_identity(a);
    CHECK(first.ok() && first.account != 0 && first.account < (UINT64_C(1) << 63) && first.secret.size() == 64);
    CHECK(fs::exists(a / kIdentityFileName));
    const Identity same = load_identity(a);
    CHECK(same.ok() && same.account == first.account && same.secret == first.secret);
    const Identity other = load_identity(b);
    CHECK(other.ok() && other.account != first.account && other.secret != first.secret);
    std::ofstream(b / kIdentityFileName) << "account=12\nsecret=nothex\n";
    const Identity damaged = load_identity(b);
    CHECK(!damaged.ok() && !damaged.error.empty());
    std::ofstream(b / kIdentityFileName) << "account=12\nsecret=" << std::string(64, 'a') << "\n";
    CHECK(load_identity(b).ok() && load_identity(b).account == 12);

    // Each test instance keeps its own identity file: the script copies only settings and saves, once.
    std::ifstream script(fs::path(DCP_SOURCE_DIR) / "tools" / "online-instance.ps1", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(script)), std::istreambuf_iterator<char>());
    CHECK(!text.empty() && text.find("online_identity") == std::string::npos);
    CHECK(text.find("\"user\\test-client-$Name\"") != std::string::npos);
    size_t copies = 0;
    for (size_t at = text.find("Copy-Item"); at != std::string::npos; at = text.find("Copy-Item", at + 1)) ++copies;
    CHECK(copies == 2 && text.find("$settings -Destination") != std::string::npos && text.find("$saves -Destination") != std::string::npos);
    CHECK(text.find("if (-not (Test-Path -LiteralPath $user))") != std::string::npos);

    CHECK(online_name("Saulo B.") == "Saulo_B");
    CHECK(online_name("  ") == "Player" && online_name("", "Guest") == "Guest");
    CHECK(online_name("abcdefghijklmnopqrstu") == "abcdefghijklmnop");
    CHECK(online_name("Jo\xc3\xa3o-99") == "Jo_o-99");
}

void TestConfig() {
    const auto servers = stun_servers(" stun.example.org:3479, plain.example.org ,bad:0,:5");
    CHECK(servers.size() == 3);
    CHECK(servers[0].first == "stun.example.org" && servers[0].second == 3479);
    CHECK(servers[1].first == "plain.example.org" && servers[1].second == 3478);
    CHECK(stun_servers("off").empty() && stun_servers("").empty());
    CHECK(stun_servers(kDefaultStunServers).size() == 2);

    OnlineConfig config;
    SetEnv("DCP_ONLINE", "1");
    SetEnv("DCP_LOBBY", "ws://127.0.0.1:8790/");
    SetEnv("DCP_ONLINE_NAME", "ClientB");
    SetEnv("DCP_UDP_PORT", "40002");
    SetEnv("DCP_UPNP", "off");
    apply_environment(config);
    CHECK(config.enabled && config.lobby == "ws://127.0.0.1:8790/" && config.name == "ClientB" && config.udp_port == 40002 &&
          !config.upnp);
    for (const char* name : {"DCP_ONLINE", "DCP_LOBBY", "DCP_ONLINE_NAME", "DCP_UDP_PORT", "DCP_UPNP"}) SetEnv(name, nullptr);
    OnlineConfig untouched;
    apply_environment(untouched);
    CHECK(!untouched.enabled && untouched.lobby == kDefaultLobbyUrl && untouched.upnp && untouched.udp_port == 0);
    CHECK(std::string(kDefaultLobbyUrl) == "wss://dcp-lobby.saulobenigno.workers.dev" && untouched.name == "Player");

    WsUrl url;
    CHECK(parse_ws_url("wss://lobby.example.workers.dev/", url, nullptr) && url.tls && url.port == 443 && url.path == "/");
    CHECK(parse_ws_url("ws://127.0.0.1:8790", url, nullptr) && !url.tls && url.port == 8790 && url.host == "127.0.0.1");
    std::string error;
    CHECK(!parse_ws_url("http://x/", url, &error) && !error.empty());
    CHECK(!parse_ws_url("ws://host:99999/", url, nullptr) && !parse_ws_url("ws:///", url, nullptr));
    CHECK(!parse_ws_url("ws://bad host/", url, nullptr));
    CHECK(parse_ws_url(kDefaultLobbyUrl, url, nullptr) && url.tls && url.port == 443 &&
          url.host == "dcp-lobby.saulobenigno.workers.dev");
}

// The Connection test's verdict: UPnP is one way among others; a usable network shows only what makes it usable, and
// router or NAT details stay in the log unless they are why it is not.
void TestConnectionSummary() {
    using Mark = ConnectionReport::Mark;
    using Router = ConnectionFacts::Router;
    const auto labels = [](const ConnectionReport& r) {
        std::vector<std::string> out;
        for (const auto& line : r.lines) out.push_back(line.label);
        return out;
    };
    const auto mark_of = [](const ConnectionReport& r, const std::string& label) {
        for (const auto& line : r.lines) {
            if (line.label == label) return line.mark;
        }
        return Mark::kNote;
    };
    const auto logged = [](const ConnectionReport& r, const std::string& text) {
        for (const std::string& detail : r.details) {
            if (detail.find(text) != std::string::npos) return true;
        }
        return false;
    };
    ConnectionFacts cone;
    cone.lobby_ok = cone.udp_ok = cone.second_answer = cone.same_mapping = true;
    for (const Router router : {Router::kNone, Router::kMapped, Router::kMappedBehindNat}) {
        ConnectionFacts facts = cone;
        facts.router = router;
        const ConnectionReport ready = summarize_connection(facts);
        CHECK(ready.ready && ready.verdict == "Ready for online play");
        CHECK(labels(ready) == (std::vector<std::string>{"Online service", "Direct connections"}));
        CHECK(mark_of(ready, "Online service") == Mark::kOk && mark_of(ready, "Direct connections") == Mark::kOk);
        for (const auto& line : ready.lines) {
            for (const char* hidden : {"stun", "STUN", "UPnP", "NAT", "router", "Router", "ws://", "wss://", "Cloudflare",
                                       "Worker"}) {
                CHECK(line.text.find(hidden) == std::string::npos && line.label.find(hidden) == std::string::npos);
            }
        }
    }
    ConnectionFacts double_nat = cone;
    double_nat.router = Router::kMappedBehindNat;
    CHECK(logged(summarize_connection(double_nat), "double NAT or carrier-grade NAT"));
    CHECK(logged(summarize_connection(cone), "no UPnP mapping"));

    ConnectionFacts one_server = cone;
    one_server.second_answer = one_server.same_mapping = false;
    CHECK(summarize_connection(one_server).ready && logged(summarize_connection(one_server), "only one STUN server"));
    ConnectionFacts public_pc = cone;
    public_pc.same_mapping = false;
    public_pc.public_address = true;
    CHECK(summarize_connection(public_pc).ready);

    ConnectionFacts symmetric = cone;
    symmetric.same_mapping = false;
    const ConnectionReport limited = summarize_connection(symmetric);
    CHECK(!limited.ready && limited.verdict == "Limited: some players may not be able to connect to you");
    CHECK(mark_of(limited, "Direct connections") == Mark::kProblem && mark_of(limited, "Router configuration") == Mark::kProblem);
    CHECK(logged(limited, "symmetric NAT"));
    symmetric.router = Router::kMapped;
    const ConnectionReport mapped = summarize_connection(symmetric);
    CHECK(mapped.ready && labels(mapped) == (std::vector<std::string>{"Online service", "Direct connections"}));
    symmetric.router = Router::kMappedBehindNat;
    const ConnectionReport behind = summarize_connection(symmetric);
    CHECK(!behind.ready);
    for (const auto& line : behind.lines) {
        if (line.label == "Router configuration") CHECK(line.text.find("Another router is in front") != std::string::npos);
    }

    ConnectionFacts blocked = cone;
    blocked.udp_ok = blocked.second_answer = blocked.same_mapping = false;
    const ConnectionReport no_udp = summarize_connection(blocked);
    CHECK(!no_udp.ready && no_udp.verdict == "Limited: direct connections may be blocked by your network");
    CHECK(mark_of(no_udp, "Direct connections") == Mark::kProblem);

    ConnectionFacts offline = cone;
    offline.lobby_ok = false;
    const ConnectionReport unreachable = summarize_connection(offline);
    CHECK(!unreachable.ready && unreachable.verdict == "Not ready: cannot reach the online service");
    CHECK(mark_of(unreachable, "Online service") == Mark::kProblem);
    ConnectionFacts no_identity = cone;
    no_identity.identity_ok = false;
    CHECK(!summarize_connection(no_identity).ready);

    // The service latency is the sign-in round trip the test already makes, shown on the service line only.
    const auto text_of = [](const ConnectionReport& r, const std::string& label) {
        for (const auto& line : r.lines) {
            if (line.label == label) return line.text;
        }
        return std::string();
    };
    CHECK(text_of(summarize_connection(cone), "Online service") == "Connected.");
    ConnectionFacts timed = cone;
    timed.lobby_latency_ms = 32;
    const ConnectionReport timed_report = summarize_connection(timed);
    CHECK(timed_report.verdict == "Ready for online play");
    CHECK(text_of(timed_report, "Online service") == "Connected \xC2\xB7 32 ms");
    CHECK(text_of(timed_report, "Direct connections") ==
          "UDP endpoint available. Direct peer connectivity will be verified when joining a match");
    for (const auto& line : timed_report.lines) CHECK(line.label.find("Ping") == std::string::npos);
    timed.lobby_ok = false;
    CHECK(text_of(summarize_connection(timed), "Online service") ==
          "Cannot reach the online service. Check your internet connection.");

    // The launcher's test names the port it opened, and one it could not open is the reason, not a free one instead.
    ConnectionFacts on_port = cone;
    on_port.udp_port = 3658;
    const ConnectionReport port_report = summarize_connection(on_port);
    CHECK(port_report.ready &&
          text_of(port_report, "Direct connections") ==
              "UDP endpoint available on UDP port 3658. Direct peer connectivity will be verified when joining a match");
    CHECK(labels(port_report) == (std::vector<std::string>{"Online service", "Direct connections"}));
    CHECK(logged(port_report, "relay: not offered by the online service"));

    // The relay line says the service offers its relay and the UDP port opened; it proves no peer path.
    ConnectionFacts relayed = on_port;
    relayed.udp_ok = relayed.second_answer = relayed.same_mapping = false;
    relayed.relay_ok = true;
    const ConnectionReport relay_report = summarize_connection(relayed);
    CHECK(relay_report.ready && relay_report.verdict == "Ready for online play");
    CHECK(mark_of(relay_report, "Direct connections") == Mark::kNote && mark_of(relay_report, "Relay") == Mark::kOk);
    CHECK(text_of(relay_report, "Relay") ==
          "Available. A match that cannot connect directly connects through the online service");
    CHECK(logged(relay_report, "relay: advertised by the online service"));
    ConnectionFacts relay_and_direct = cone;
    relay_and_direct.udp_port = 3658;
    relay_and_direct.relay_ok = true;
    const ConnectionReport both = summarize_connection(relay_and_direct);
    CHECK(labels(both) == (std::vector<std::string>{"Online service", "Direct connections", "Relay"}));
    CHECK(mark_of(both, "Direct connections") == Mark::kOk);
    ConnectionFacts relay_port_taken = relayed;
    relay_port_taken.port_open = false;
    const ConnectionReport no_port = summarize_connection(relay_port_taken);
    CHECK(!no_port.ready && text_of(no_port, "Relay").empty() && logged(no_port, "relay: needs the UDP port"));
    ConnectionFacts relay_service_down = relayed;
    relay_service_down.lobby_ok = false;
    CHECK(!summarize_connection(relay_service_down).ready && text_of(summarize_connection(relay_service_down), "Relay").empty());
    on_port.udp_ok = on_port.second_answer = on_port.same_mapping = false;
    CHECK(text_of(summarize_connection(on_port), "Direct connections") ==
          "May be blocked by your network or firewall on UDP port 3658.");
    ConnectionFacts taken = cone;
    taken.udp_port = 3658;
    taken.port_open = false;
    taken.udp_ok = taken.second_answer = taken.same_mapping = false;
    taken.router = Router::kMapped;
    const ConnectionReport taken_report = summarize_connection(taken);
    CHECK(!taken_report.ready && taken_report.verdict == "Limited: the multiplayer UDP port could not be opened");
    CHECK(mark_of(taken_report, "Direct connections") == Mark::kProblem &&
          text_of(taken_report, "Direct connections") ==
              "Cannot open UDP port 3658. Another program may already be using it, or network security may be blocking it.");
    CHECK(labels(taken_report) == (std::vector<std::string>{"Online service", "Direct connections"}));

    OnlineConfig config;
    CHECK(connection_test_port(config) == kGameUdpPort && kGameUdpPort == 3658);
    config.udp_port = 40001;
    CHECK(connection_test_port(config) == 40001);

    const std::string test = ReadSource("src/net/connection_test.cpp");
    size_t opens = 0;
    for (size_t at = test.find("session.open("); at != std::string::npos; at = test.find("session.open(", at + 1)) ++opens;
    CHECK(opens == 1 && test.find("facts.lobby_latency_ms = int64_t(now_ms() - start);") != std::string::npos);
    CHECK(test.find("send_binary") == std::string::npos && test.find("enable_relay") == std::string::npos &&
          test.find("deliver_relay") == std::string::npos);
    CHECK(test.find("facts.relay_ok = capability.usable();") != std::string::npos);
    const std::string page = ReadSource("launcher/online_page.cpp");
    size_t runs = 0;
    for (size_t at = page.find("RunTest("); at != std::string::npos; at = page.find("RunTest(", at + 1)) ++runs;
    CHECK(runs == 2 && page.find("ui.ButtonRow(\"Connection test\"") < page.find("RunTest(ui);"));
    const size_t tick = page.find("void OnlinePage::Tick(");
    CHECK(tick != std::string::npos && page.find("run_connection_test", tick) > page.find("void OnlinePage::RunTest(", tick));
}

// The in-game Ping is the worst smoothed round trip among connected links that measured one.
void TestWorstRtt() {
    const auto view = [](LinkState state, bool sampled, uint32_t rtt) {
        LinkView v;
        v.state = state;
        v.stats.rtt_sampled = sampled;
        v.stats.rtt_ms = rtt;
        return v;
    };
    CHECK(worst_rtt_ms({}) == -1);
    CHECK(worst_rtt_ms({view(LinkState::kConnected, false, 0)}) == -1);
    CHECK(worst_rtt_ms({view(LinkState::kConnected, true, 0)}) == 0); // one PC: under a millisecond
    CHECK(worst_rtt_ms({view(LinkState::kConnected, true, 22), view(LinkState::kConnected, true, 41),
                        view(LinkState::kConnected, true, 30)}) == 41);
    CHECK(worst_rtt_ms({view(LinkState::kConnected, true, 22), view(LinkState::kClosed, true, 90),
                        view(LinkState::kConnecting, true, 70)}) == 22);
    CHECK(worst_rtt_ms({view(LinkState::kClosed, true, 90)}) == -1);

    // The game's Ping reads the P2P links of its Rudp peers, never the lobby.
    const std::string rudp = ReadSource("src/runtime_rudp.cpp");
    const size_t begin = rudp.find("int peer_ping_ms() {");
    const size_t end = rudp.find("\n}\n", begin);
    CHECK(begin != std::string::npos && end != std::string::npos);
    const std::string body = rudp.substr(begin, end - begin);
    CHECK(body.find("g_peer->stats(c.link)") != std::string::npos && body.find("net::worst_rtt_ms(links)") != std::string::npos);
    for (const char* service : {"lobby", "Lobby", "LobbySession", "request(", "connection_test"}) {
        CHECK(body.find(service) == std::string::npos);
    }
    const std::string online = ReadSource("src/runtime_online.cpp");
    CHECK(online.find("return online::g_settings.enabled ? online::peer_ping_ms() : -1;") != std::string::npos);
    const std::string overlay = ReadSource("gpu/shim/dcp_overlay.cpp");
    CHECK(overlay.find("runtime_online_peer_ping_ms()") != std::string::npos && overlay.find("lobby") == std::string::npos);
}

// The player card's System type: platform and port version (the CMake project VERSION), never a PlayStation; and the
// one online protocol every client advertises in its hello.
void TestOnlineVersion() {
    CHECK(platform_name(true, "", "") == "Windows" && platform_name(true, "Valve", "Jupiter") == "Windows");
    CHECK(platform_name(false, "Valve", "Jupiter") == "Steam Deck" && platform_name(false, "Valve", "Galileo") == "Steam Deck");
    CHECK(platform_name(false, "", "") == "Linux" && platform_name(false, "Valve", "Steam Machine") == "Linux");
    CHECK(platform_name(false, "LENOVO", "Jupiter") == "Linux" && platform_name(false, "valve", "jupiter") == "Linux");
#ifdef _WIN32
    CHECK(host_platform() == "Windows");
#else
    CHECK(host_platform() == "Linux" || host_platform() == "Steam Deck");
#endif

    const std::string cmake = ReadSource("CMakeLists.txt");
    const size_t at = cmake.find(" VERSION ", cmake.find("project(dragons_crown_pro_pc"));
    const size_t end = cmake.find_first_of(" )", at + 9);
    const std::string version = at == std::string::npos ? std::string() : cmake.substr(at + 9, end - at - 9);
    CHECK(!version.empty() && port_version() == version);
    CHECK(system_type_text("Windows", port_version()) == "Windows \xC2\xB7 v" + version);
    CHECK(system_type_text("Linux", "0.8.0") == "Linux \xC2\xB7 v0.8.0");
    CHECK(system_type_text("Steam Deck", "0.8.0") == "Steam Deck \xC2\xB7 v0.8.0");
    CHECK(system_type_text("Linux", "") == "Linux");
    CHECK(system_type_text(host_platform(), port_version()).find("PlayStation") == std::string::npos);

    CHECK(online_compatible(kOnlineProtocolVersion));
    CHECK(!online_compatible(0) && !online_compatible(kOnlineProtocolVersion + 1) && !online_compatible(-1));

    Identity id;
    id.account = 42;
    id.secret = std::string(64, 'a');
    const Json hello = hello_body(id, "A", "TEST-1.00", true);
    CHECK(hello["online_protocol"].as_int() == kOnlineProtocolVersion && hello["version"].as_string() == port_version() &&
          hello["platform"].as_string() == host_platform());

    // The number lives in net/online_version.h only; the lobby compares, it never names one.
    for (const char* file : {"src/net/online_version.cpp", "src/net/lobby_link.cpp", "src/runtime_matching2.cpp",
                             "src/runtime_online.cpp", "launcher/online_client.cpp"}) {
        const std::string source = ReadSource(file);
        CHECK(source.find("kOnlineProtocolVersion =") == std::string::npos);
        CHECK(source.find('"' + version + '"') == std::string::npos);
    }
    CHECK(ReadSource("src/net/online_version.h").find("inline constexpr int kOnlineProtocolVersion = ") != std::string::npos);
    const std::string lobby = ReadSource("server/lobby/src/lobby.js");
    CHECK(lobby.find("(att.online_protocol ?? 0) === room.online_protocol") != std::string::npos);
    CHECK(lobby.find("online_protocol === 1") == std::string::npos && lobby.find("ONLINE_PROTOCOL") == std::string::npos);
}

void TestAddresses() {
    CHECK(parse_ip("192.168.1.20") == 0xc0a80114 && parse_ip("1.2.3") == 0 && parse_ip("256.0.0.1") == 0);
    CHECK(ip_text(0x7f000001) == "127.0.0.1" && endpoint_text({0x0a000001, 3658}) == "10.0.0.1:3658");
    CHECK(classify(parse_ip("127.0.0.1")) == AddressKind::kLoopback && classify(parse_ip("10.1.2.3")) == AddressKind::kPrivate);
    CHECK(classify(parse_ip("100.64.0.13")) == AddressKind::kCarrierNat && classify(parse_ip("203.0.113.9")) == AddressKind::kPublic);
    CHECK(is_lan(parse_ip("169.254.0.1")) && !is_lan(parse_ip("8.8.8.8")));
}

void TestStun() {
    stun::TransactionId id{};
    for (size_t i = 0; i < id.size(); ++i) id[i] = uint8_t(i + 1);
    uint8_t request[stun::kRequestSize];
    stun::make_request(id, request);
    CHECK(request[0] == 0 && request[1] == 1 && stun::looks_like_stun(request, sizeof(request)));
    // A Binding success with XOR-MAPPED-ADDRESS 192.0.2.1:32853 (RFC 5769 section 2.2's address).
    uint8_t response[32] = {0x01, 0x01, 0x00, 0x0c, 0x21, 0x12, 0xa4, 0x42};
    std::memcpy(response + 8, id.data(), id.size());
    const uint8_t attr[] = {0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xa1, 0x47, 0xe1, 0x12, 0xa6, 0x43};
    std::memcpy(response + 20, attr, sizeof(attr));
    Endpoint mapped;
    CHECK(stun::parse_response(response, sizeof(response), id, mapped) && mapped.ip == parse_ip("192.0.2.1") && mapped.port == 32853);
    stun::TransactionId other = id;
    other[0] ^= 1;
    CHECK(!stun::parse_response(response, sizeof(response), other, mapped));
    CHECK(!stun::parse_response(response, 24, id, mapped));
    uint8_t garbage[32] = {'D', 'N', 1, 3};
    CHECK(!stun::looks_like_stun(garbage, sizeof(garbage)));
}

// A loopback STUN server: answers each Binding request with the sender's own address and remembers its port.
class FakeStun {
public:
    FakeStun() {
        s_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        const sockaddr_in local = to_sockaddr({parse_ip("127.0.0.1"), 0});
        bind(s_, reinterpret_cast<const sockaddr*>(&local), sizeof(local));
        sockaddr_in bound{};
        socklen_t length = sizeof(bound);
        getsockname(s_, reinterpret_cast<sockaddr*>(&bound), &length);
        port_ = ntohs(bound.sin_port);
        set_nonblocking(s_);
        thread_ = std::thread([this] { Run(); });
    }
    ~FakeStun() {
        stop_ = true;
        thread_.join();
        close_socket(s_);
    }
    uint16_t port() const { return port_; }
    uint16_t last_source() const { return source_; }

private:
    void Run() {
        uint8_t buffer[1500];
        while (!stop_) {
            sockaddr_in from{};
            socklen_t length = sizeof(from);
            const int n = int(recvfrom(s_, reinterpret_cast<char*>(buffer), sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &length));
            if (n < int(stun::kRequestSize)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            const uint16_t port = ntohs(from.sin_port);
            const uint32_t ip = ntohl(from.sin_addr.s_addr);
            source_ = port;
            uint8_t response[32] = {0x01, 0x01, 0x00, 0x0c, 0x21, 0x12, 0xa4, 0x42};
            std::memcpy(response + 8, buffer + 8, 12);
            const uint16_t xport = port ^ 0x2112;
            const uint32_t xip = ip ^ 0x2112a442u;
            const uint8_t attr[] = {0x00, 0x20, 0x00, 0x08, 0x00, 0x01, uint8_t(xport >> 8), uint8_t(xport),
                                    uint8_t(xip >> 24), uint8_t(xip >> 16), uint8_t(xip >> 8), uint8_t(xip)};
            std::memcpy(response + 20, attr, sizeof(attr));
            sendto(s_, reinterpret_cast<const char*>(response), int(sizeof(response)), 0,
                   reinterpret_cast<const sockaddr*>(&from), sizeof(from));
        }
    }
    socket_t s_;
    uint16_t port_ = 0;
    std::atomic<uint16_t> source_{0};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

uint16_t FreeUdpPort() {
    const socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    const sockaddr_in any = to_sockaddr({0, 0});
    bind(s, reinterpret_cast<const sockaddr*>(&any), sizeof(any));
    sockaddr_in bound{};
    socklen_t length = sizeof(bound);
    getsockname(s, reinterpret_cast<sockaddr*>(&bound), &length);
    close_socket(s);
    return ntohs(bound.sin_port);
}

// The launcher's Connection test binds the requested port, asks STUN from that same socket and releases it; a taken
// port is reported, never replaced. No lobby (no identity) and no UPnP: nothing leaves this PC.
void TestConnectionTestSocket() {
    const auto has = [](const ConnectionReport& r, const std::string& text) {
        for (const std::string& detail : r.details) {
            if (detail.find(text) != std::string::npos) return true;
        }
        for (const auto& line : r.lines) {
            if (line.text.find(text) != std::string::npos) return true;
        }
        return false;
    };
    FakeStun stun;
    OnlineConfig config;
    config.upnp = false;
    config.stun = "127.0.0.1:" + std::to_string(stun.port());
    const uint16_t port = FreeUdpPort();
    const std::string port_text = std::to_string(port);

    const ConnectionReport report = run_connection_test(config, Identity{}, "Player", "game", port);
    CHECK(stun.last_source() == port);
    CHECK(has(report, "UDP port " + port_text + " opened"));
    CHECK(has(report, "STUN 127.0.0.1: 127.0.0.1:" + port_text));
    CHECK(has(report, "available on UDP port " + port_text + "."));
    const socket_t again = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    const sockaddr_in same = to_sockaddr({0, port});
    CHECK(bind(again, reinterpret_cast<const sockaddr*>(&same), sizeof(same)) == 0); // released before returning

    const ConnectionReport taken = run_connection_test(config, Identity{}, "Player", "game", port);
    close_socket(again);
    CHECK(has(taken, "cannot open UDP port " + port_text));
    CHECK(has(taken, "Cannot open UDP port " + port_text + ". Another program may already be using it"));
    CHECK(has(taken, "UPnP: not tried") && !has(taken, "STUN 127.0.0.1"));
    CHECK(!taken.ready && !taken.verdict.empty());

    // The in-game test (port 0) keeps any free port: the game's own socket holds the game's.
    const ConnectionReport in_game = run_connection_test(config, Identity{}, "Player", "game");
    CHECK(stun.last_source() != 0 && stun.last_source() != port);
    CHECK(has(in_game, "available.") && !has(in_game, "on UDP port"));

    const std::string source = ReadSource("src/net/connection_test.cpp");
    size_t sockets = 0;
    for (size_t at = source.find("socket(AF_INET"); at != std::string::npos; at = source.find("socket(AF_INET", at + 1)) ++sockets;
    CHECK(sockets == 1 && source.find("stun::query(s, ") != std::string::npos);
    CHECK(source.find("upnp::probe(udp_port ? udp_port : kGameUdpPort)") != std::string::npos);
    for (const char* firewall : {"netsh", "advfirewall", "INetFwPolicy", "New-NetFirewallRule", "runas"}) {
        CHECK(source.find(firewall) == std::string::npos);
    }
}

// A UDP relay between two ports that drops a share of the datagrams both ways.
class LossyRelay {
public:
    LossyRelay(uint16_t a, uint16_t b, int drop_percent) : a_(a), b_(b), drop_(drop_percent) {
        s_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        const sockaddr_in local = to_sockaddr({parse_ip("127.0.0.1"), 0});
        bind(s_, reinterpret_cast<const sockaddr*>(&local), sizeof(local));
        sockaddr_in bound{};
        socklen_t length = sizeof(bound);
        getsockname(s_, reinterpret_cast<sockaddr*>(&bound), &length);
        port_ = ntohs(bound.sin_port);
        set_nonblocking(s_);
        thread_ = std::thread([this] { Run(); });
    }
    ~LossyRelay() {
        stop_ = true;
        thread_.join();
        close_socket(s_);
    }
    uint16_t port() const { return port_; }

private:
    void Run() {
        std::mt19937 random(12345);
        uint8_t buffer[2048];
        while (!stop_) {
            sockaddr_in from{};
            socklen_t length = sizeof(from);
            const int n = int(recvfrom(s_, reinterpret_cast<char*>(buffer), sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &length));
            if (n <= 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (int(random() % 100) < drop_) continue;
            const uint16_t source = ntohs(from.sin_port);
            const sockaddr_in to = to_sockaddr({parse_ip("127.0.0.1"), source == a_ ? b_ : a_});
            sendto(s_, reinterpret_cast<const char*>(buffer), n, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
        }
    }
    socket_t s_;
    uint16_t a_, b_, port_ = 0;
    int drop_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

std::vector<uint8_t> Pattern(size_t size, uint8_t seed) {
    std::vector<uint8_t> v(size);
    for (size_t i = 0; i < size; ++i) v[i] = uint8_t(seed + i * 31 + (i >> 8));
    return v;
}

// Two players on one PC: both ends start the link, a dead candidate is skipped, whole messages arrive in order.
void TestPeerLink(int drop_percent) {
    PeerSocket::Options options;
    options.connect_timeout_ms = 8000;
    std::string error;
    auto a = PeerSocket::open(options, &error);
    auto b = PeerSocket::open(options, &error);
    CHECK(a && b);
    if (!a || !b) return;
    CHECK(a->port() != b->port());
    std::unique_ptr<LossyRelay> relay;
    uint16_t to_b = b->port(), to_a = a->port();
    if (drop_percent) {
        relay = std::make_unique<LossyRelay>(a->port(), b->port(), drop_percent);
        to_b = to_a = relay->port();
    }
    RoomKey key{};
    key[0] = 42;
    a->set_room(key, 1001);
    b->set_room(key, 2002);
    const uint32_t loopback = parse_ip("127.0.0.1");
    a->set_peer(2002, {{loopback, 1}, {loopback, to_b}});
    b->set_peer(1001, {{loopback, to_a}});
    CHECK(a->peer_at({loopback, to_b}) == 2002 && a->peer_at({loopback, 9}) == 0);
    const int la = a->connect(2002), lb = b->connect(1001);
    CHECK(la > 0 && lb > 0);
    CHECK(WaitFor([&] { return a->state(la) == LinkState::kConnected && b->state(lb) == LinkState::kConnected; }, 8000));
    std::vector<std::vector<uint8_t>> sent;
    for (const size_t size : {size_t(0), size_t(16), size_t(1150), size_t(1151), size_t(40000), size_t(200000)})
        sent.push_back(Pattern(size, uint8_t(size)));
    for (const auto& m : sent) CHECK(a->send(la, m.data(), m.size()));
    const std::vector<uint8_t> reply = Pattern(3000, 7);
    CHECK(b->send(lb, reply.data(), reply.size()));
    std::vector<std::vector<uint8_t>> got;
    CHECK(WaitFor([&] {
        std::vector<uint8_t> m;
        while (b->receive(lb, m)) got.push_back(m);
        return got.size() == sent.size();
    }, 20000));
    CHECK(got == sent);
    std::vector<uint8_t> back;
    CHECK(WaitFor([&] { return a->receive(la, back); }, 5000) && back == reply);
    const std::vector<uint8_t> too_big(PeerSocket::kMaxMessage + 1);
    CHECK(!a->send(la, too_big.data(), too_big.size()));
    if (drop_percent) CHECK(a->stats(la).resends > 0);
    // The acknowledged messages measured the link's round trip; a closed link no longer counts.
    CHECK(a->stats(la).rtt_sampled && a->stats(la).rtt_ms < 2000);
    CHECK(worst_rtt_ms({{a->state(la), a->stats(la)}}) == int(a->stats(la).rtt_ms));
    a->close(la);
    CHECK(WaitFor([&] { return b->state(lb) == LinkState::kClosed; }, 3000));
    CHECK(worst_rtt_ms({{a->state(la), a->stats(la)}, {b->state(lb), b->stats(lb)}}) == -1);
}

void TestPeerRefusesWrongRoom() {
    PeerSocket::Options options;
    options.connect_timeout_ms = 1500;
    std::string error;
    auto a = PeerSocket::open(options, &error);
    auto b = PeerSocket::open(options, &error);
    CHECK(a && b);
    if (!a || !b) return;
    RoomKey key_a{}, key_b{};
    key_b[3] = 1;
    a->set_room(key_a, 1);
    b->set_room(key_b, 2);
    const uint32_t loopback = parse_ip("127.0.0.1");
    a->set_peer(2, {{loopback, b->port()}});
    b->set_peer(1, {{loopback, a->port()}});
    const int la = a->connect(2), lb = b->connect(1);
    CHECK(WaitFor([&] { return a->state(la) == LinkState::kClosed; }, 4000));
    CHECK(b->state(lb) != LinkState::kConnected);
    CHECK(a->connect(99) == -1);
}

// The game's fixed port is taken by the first instance: the second gets another one.
void TestSecondInstancePort() {
    PeerSocket::Options options;
    options.port = 47123;
    std::string error;
    auto first = PeerSocket::open(options, &error);
    auto second = PeerSocket::open(options, &error);
    CHECK(first && second);
    if (first && second) CHECK(first->port() == 47123 && second->port() != 47123 && second->port() != 0);
}

// Requires a development lobby (DCP_TEST_LOBBY): sign-in, a room, a search, a join and the endpoint exchange.
void TestLobby(const fs::path& base) {
    const char* url = std::getenv("DCP_TEST_LOBBY");
    if (!url || !*url) {
        std::printf("lobby tests skipped (set DCP_TEST_LOBBY to a running development lobby)\n");
        return;
    }
    const Identity host_id = load_identity(base / "lobby-a"), guest_id = load_identity(base / "lobby-b");
    LobbyLink::Options options;
    options.url = url;
    options.hello = [&] { return hello_body(host_id, "HostA", "TEST-1.00", true); };
    LobbyLink host(options);
    options.hello = [&] { return hello_body(guest_id, "GuestB", "TEST-1.00", true); };
    LobbyLink guest(options);
    host.start();
    guest.start();
    auto next = [](LobbyLink& link, const std::function<bool(const LobbyMessage&)>& match, LobbyMessage& out) {
        return WaitFor([&] {
            LobbyMessage m;
            while (link.poll(m)) {
                if (match(m)) {
                    out = std::move(m);
                    return true;
                }
            }
            return false;
        }, 5000);
    };
    LobbyMessage m;
    CHECK(next(host, [](const LobbyMessage& x) { return x.kind == LobbyMessage::Kind::kUp; }, m));
    CHECK(!m.body["friend_code"].as_string().empty());
    CHECK(next(guest, [](const LobbyMessage& x) { return x.kind == LobbyMessage::Kind::kUp; }, m));
    Json udp = Json::Object();
    udp.set("port", 40001);
    const uint32_t udp_seq = host.request("udp", udp);
    CHECK(next(host, [&](const LobbyMessage& x) { return x.kind == LobbyMessage::Kind::kResponse && x.seq == udp_seq; }, m) && m.code == "ok");
    Json room = Json::Object();
    room.set("world", 1);
    room.set("slots", 4);
    room.set("flags", int64_t(0x86000000u));
    const uint32_t create = host.request("room.create", room);
    CHECK(next(host, [&](const LobbyMessage& x) { return x.kind == LobbyMessage::Kind::kResponse && x.seq == create; }, m));
    CHECK(m.code == "ok" && m.body["me"].as_int() == 0x10);
    const std::string room_id = m.body["room"]["id"].as_string();
    Json search = Json::Object();
    search.set("world", 1);
    const uint32_t found = guest.request("room.search", search);
    CHECK(next(guest, [&](const LobbyMessage& x) { return x.kind == LobbyMessage::Kind::kResponse && x.seq == found; }, m));
    CHECK(m.body["total"].as_int() >= 1);
    Json join = Json::Object();
    join.set("room", room_id);
    const uint32_t joined = guest.request("room.join", join);
    CHECK(next(guest, [&](const LobbyMessage& x) { return x.kind == LobbyMessage::Kind::kResponse && x.seq == joined; }, m));
    CHECK(m.code == "ok" && m.body["me"].as_int() == 0x20 && m.body["room"]["key"].as_string().size() == 32);
    CHECK(next(guest, [](const LobbyMessage& x) { return x.kind == LobbyMessage::Kind::kEvent && x.event == "peer"; }, m));
    CHECK(m.body["port"].as_int() == 40001 && m.body["ip"].as_string() == "127.0.0.1");
    CHECK(next(host, [](const LobbyMessage& x) { return x.kind == LobbyMessage::Kind::kEvent && x.event == "room.member_joined"; }, m));
    CHECK(m.body["member"]["name"].as_string() == "GuestB");

    LobbySession session;
    Json reply;
    std::string error;
    CHECK(session.open(url, hello_body(host_id, "HostA", "TEST-1.00", false), &reply, &error));
    std::string code;
    CHECK(session.call("friends.list", Json::Object(), &code) && code == "ok");
    CHECK(!session.call("no.such.op", Json::Object(), &code) && code == "bad_request");
    session.close();
    LobbySession refused;
    Identity forged = host_id;
    forged.secret = std::string(64, '0');
    CHECK(!refused.open(url, hello_body(forged, "Mallory", "TEST-1.00", false), nullptr, &error) &&
          error.find("unauthorized") != std::string::npos);
    host.stop();
    guest.stop();
}

// The AppImage's OpenSSL knows only its build prefix: the lobby's certificate is checked against the distribution's
// bundle, Debian's layout first.
void TestSystemCaBundle() {
    const auto only = [](std::vector<std::string> present) {
        return [present](const std::string& path) {
            return std::find(present.begin(), present.end(), path) != present.end();
        };
    };
    CHECK(system_ca_bundle(only({})).empty());
    CHECK(system_ca_bundle(only({"/etc/pki/tls/certs/ca-bundle.crt"})) == "/etc/pki/tls/certs/ca-bundle.crt");
    CHECK(system_ca_bundle(only({"/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/certs/ca-certificates.crt"})) ==
          "/etc/ssl/certs/ca-certificates.crt");
    CHECK(system_ca_bundle(only({"/etc/ssl/ca-bundle.pem"})) == "/etc/ssl/ca-bundle.pem");
    CHECK(system_ca_bundle(only({"/usr/local/ssl/cert.pem"})).empty());
    const std::string posix = ReadSource("src/net/websocket_posix.cpp");
    const size_t defaults = posix.find("SSL_CTX_set_default_verify_paths(context);");
    const size_t bundle = posix.find("SSL_CTX_load_verify_locations(context, bundle.c_str(), nullptr);");
    CHECK(defaults != std::string::npos && bundle != std::string::npos && defaults < bundle &&
          posix.find("SSL_VERIFY_PEER") != std::string::npos && posix.find("SSL_set1_host") != std::string::npos);
}

// Linux runs the same online, build and patch code as Windows: no host guard in it, the same sources in the game on
// both, and tools/build.sh --test runs every focused test tools/test.ps1 runs that is not about a Windows API.
// The relay's frame: a PeerSocket datagram wrapped for the lobby's WebSocket.
void TestRelayFrame() {
    const std::vector<uint8_t> datagram = Pattern(1175, 9);
    const std::vector<uint8_t> bytes = encode_relay_frame(0xdeadbeef, 0x20, 0x01020304, datagram.data(), datagram.size());
    CHECK(bytes.size() == kRelayHeaderBytes + datagram.size() && bytes[0] == 'D' && bytes[1] == 'R');
    RelayFrameError error = RelayFrameError::kBadType;
    const auto frame = decode_relay_frame(bytes.data(), bytes.size(), &error);
    CHECK(frame && error == RelayFrameError::kNone);
    if (frame) CHECK(frame->token == 0xdeadbeef && frame->member == 0x20 && frame->seq == 0x01020304 && frame->payload == datagram);
    CHECK(kRelayMaxPayload >= 12 + 13 + 1150);
    for (const size_t size : {size_t(1), size_t(12), size_t(20), size_t(48), size_t(1175)}) {
        std::vector<uint8_t> dn = Pattern(size, uint8_t(size));
        if (size >= 4) dn[0] = 'D', dn[1] = 'N', dn[2] = 1, dn[3] = 3;
        const auto wrapped = encode_relay_frame(7, 0x10, 99, dn.data(), dn.size());
        const auto unwrapped = decode_relay_frame(wrapped.data(), wrapped.size());
        CHECK(unwrapped && unwrapped->payload == dn && unwrapped->member == 0x10 && unwrapped->seq == 99);
    }

    const auto fails = [&](std::vector<uint8_t> data, RelayFrameError expected) {
        RelayFrameError why = RelayFrameError::kNone;
        return !decode_relay_frame(data.data(), data.size(), &why) && why == expected;
    };
    std::vector<uint8_t> broken = bytes;
    broken[0] = 'S';
    CHECK(fails(broken, RelayFrameError::kBadMagic));
    broken = bytes;
    broken[2] = 2;
    CHECK(fails(broken, RelayFrameError::kBadVersion));
    broken = bytes;
    broken[3] = 2;
    CHECK(fails(broken, RelayFrameError::kBadType));
    CHECK(fails({bytes.begin(), bytes.begin() + 3}, RelayFrameError::kTruncated));
    CHECK(fails({bytes.begin(), bytes.begin() + 10}, RelayFrameError::kTruncated));
    CHECK(fails({bytes.begin(), bytes.end() - 1}, RelayFrameError::kBadLength));
    broken = bytes;
    broken.push_back(0);
    CHECK(fails(broken, RelayFrameError::kBadLength));
    std::vector<uint8_t> big(kRelayMaxPayload + 1, 1);
    CHECK(encode_relay_frame(1, 1, 1, big.data(), big.size()).empty() && encode_relay_frame(1, 1, 1, big.data(), 0).empty());
    big.resize(kRelayMaxPayload);
    CHECK(!encode_relay_frame(1, 1, 1, big.data(), big.size()).empty());
    broken = bytes;
    broken[14] = uint8_t((kRelayMaxPayload + 1) >> 8);
    broken[15] = uint8_t(kRelayMaxPayload + 1);
    CHECK(fails(broken, RelayFrameError::kOversized));
    broken = encode_relay_frame(1, 1, 1, big.data(), 1);
    broken[14] = broken[15] = 0;
    CHECK(fails(broken, RelayFrameError::kBadLength));

    RoomKey key{};
    key[0] = 0x12, key[1] = 0x34, key[2] = 0x56, key[3] = 0x78, key[4] = 0xff;
    CHECK(relay_token(key) == 0x12345678);
    CHECK(relay_capability(*Json::parse(R"({"capabilities":{"relay_supported":true,"relay_max_payload":1200}})")).supported);
    CHECK(relay_capability(*Json::parse(R"({"capabilities":{"relay_supported":true,"relay_max_payload":1200}})")).max_payload == 1200);
    CHECK(!relay_capability(*Json::parse(R"({"account":"1"})")).supported);
    CHECK(!relay_capability(*Json::parse(R"({"capabilities":{"relay_supported":false,"relay_max_payload":9}})")).supported);
}

// Stands in for the lobby: forwards each relay frame to the member it names with the sender as source, off the
// sender's thread as the real link does. Faults: frames dropped at random, all of them swallowed, sends refused.
class RelayBroker {
public:
    explicit RelayBroker(uint32_t token) : token_(token), thread_([this] { Run(); }) {}
    ~RelayBroker() {
        {
            std::lock_guard<std::mutex> lock(gate_->mutex);
            gate_->broker = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }

    void Add(uint16_t member, PeerSocket* socket) {
        std::lock_guard<std::mutex> lock(mutex_);
        sockets_[member] = socket;
    }
    void SetDropPercent(int percent) {
        std::lock_guard<std::mutex> lock(mutex_);
        drop_percent_ = percent;
    }
    void SetSwallowAll(bool swallow) {
        std::lock_guard<std::mutex> lock(mutex_);
        swallow_ = swallow;
    }
    void SetRefuse(bool refuse) {
        std::lock_guard<std::mutex> lock(mutex_);
        refuse_ = refuse;
    }
    // Sockets outlive the broker in these tests and may still call this while they close.
    PeerSocket::RelaySend From(uint16_t source) {
        return [gate = gate_, source](std::vector<uint8_t>&& frame) {
            std::lock_guard<std::mutex> lock(gate->mutex);
            return gate->broker && gate->broker->Enqueue(source, std::move(frame));
        };
    }
    uint64_t Count(uint16_t from, uint16_t to) {
        std::lock_guard<std::mutex> lock(mutex_);
        return counts_[{from, to}];
    }
    uint64_t Total() {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t total = 0;
        for (const auto& [pair, n] : counts_) total += n;
        return total;
    }
    std::vector<std::vector<uint8_t>> Payloads() {
        std::lock_guard<std::mutex> lock(mutex_);
        return payloads_;
    }

private:
    struct Gate {
        explicit Gate(RelayBroker* owner) : broker(owner) {}
        std::mutex mutex;
        RelayBroker* broker;
    };

    bool Enqueue(uint16_t source, std::vector<uint8_t>&& frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (refuse_ || pending_.size() >= 512) return false;
        pending_.emplace_back(source, std::move(frame));
        wake_.notify_all();
        return true;
    }

    void Run() {
        std::mt19937 random(4242);
        for (;;) {
            std::pair<uint16_t, std::vector<uint8_t>> item;
            PeerSocket* target = nullptr;
            std::vector<uint8_t> out;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return stop_ || !pending_.empty(); });
                if (stop_) return;
                item = std::move(pending_.front());
                pending_.pop_front();
                if (swallow_ || int(random() % 100) < drop_percent_) continue;
                const auto frame = decode_relay_frame(item.second.data(), item.second.size());
                const auto it = frame ? sockets_.find(frame->member) : sockets_.end();
                if (!frame || frame->token != token_ || it == sockets_.end()) continue;
                ++counts_[{item.first, frame->member}];
                payloads_.push_back(frame->payload);
                target = it->second;
                out = encode_relay_frame(frame->token, item.first, frame->seq, frame->payload.data(), frame->payload.size());
            }
            target->deliver_relay(out.data(), out.size());
        }
    }

    const uint32_t token_;
    std::shared_ptr<Gate> gate_ = std::make_shared<Gate>(this);
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::pair<uint16_t, std::vector<uint8_t>>> pending_;
    std::map<uint16_t, PeerSocket*> sockets_;
    std::map<std::pair<uint16_t, uint16_t>, uint64_t> counts_;
    std::vector<std::vector<uint8_t>> payloads_;
    int drop_percent_ = 0;
    bool swallow_ = false;
    bool refuse_ = false;
    bool stop_ = false;
    std::thread thread_;
};

// A UDP port that listens and never answers: a direct candidate that is reachable but silent.
class SilentPeer {
public:
    SilentPeer() {
        s_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        set_nonblocking(s_);
        const sockaddr_in local = to_sockaddr({parse_ip("127.0.0.1"), 0});
        bind(s_, reinterpret_cast<const sockaddr*>(&local), sizeof(local));
        sockaddr_in bound{};
        socklen_t length = sizeof(bound);
        getsockname(s_, reinterpret_cast<sockaddr*>(&bound), &length);
        port_ = ntohs(bound.sin_port);
        thread_ = std::thread([this] { Run(); });
    }
    ~SilentPeer() {
        stop_ = true;
        thread_.join();
        close_socket(s_);
    }
    uint16_t port() const { return port_; }
    uint64_t received() const { return received_; }

private:
    void Run() {
        uint8_t buffer[2048];
        while (!stop_) {
            const int n = int(recvfrom(s_, reinterpret_cast<char*>(buffer), sizeof(buffer), 0, nullptr, nullptr));
            if (n > 0) ++received_;
            else std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    socket_t s_;
    uint16_t port_ = 0;
    std::atomic<uint64_t> received_{0};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

constexpr uint16_t kMemberA = 0x10, kMemberB = 0x20, kMemberC = 0x30;
constexpr uint32_t kRoomToken = 0x5a5a1234;

RoomKey TestRoomKey() {
    RoomKey key{};
    key[0] = 0x5a, key[1] = 0x5a, key[2] = 0x12, key[3] = 0x34;
    key[9] = 77;
    return key;
}

struct Peer {
    std::unique_ptr<PeerSocket> socket;
    uint64_t account = 0;
    uint16_t member = 0;

    // A closing socket still sends its CLOSE through the relay; the broker must not be reached after it is gone.
    ~Peer() {
        if (socket) socket->disable_relay();
    }
};

Peer OpenPeer(uint64_t account, uint16_t member, int connect_timeout_ms = 8000) {
    PeerSocket::Options options;
    options.connect_timeout_ms = connect_timeout_ms;
    std::string error;
    std::unique_ptr<PeerSocket> socket = PeerSocket::open(options, &error);
    if (socket) socket->set_room(TestRoomKey(), account);
    return Peer{std::move(socket), account, member};
}

void Introduce(Peer& at, const Peer& other, bool reachable_directly, bool relayable = true) {
    const uint32_t loopback = parse_ip("127.0.0.1");
    at.socket->set_peer(other.account, {{loopback, reachable_directly ? other.socket->port() : uint16_t(1)}},
                        relayable ? other.member : uint16_t(0));
}

void EnableRelay(Peer& at, RelayBroker& broker) {
    broker.Add(at.member, at.socket.get());
    at.socket->enable_relay(kRoomToken, broker.From(at.member));
}

bool Exchange(Peer& from, int link_from, Peer& to, int link_to, size_t size, uint8_t seed) {
    const std::vector<uint8_t> message = Pattern(size, seed);
    if (!from.socket->send(link_from, message.data(), message.size())) return false;
    std::vector<uint8_t> got;
    return WaitFor([&] { return to.socket->receive(link_to, got); }, 15000) && got == message;
}

// Direct works, so the relay carries nothing and the path is direct.
void TestRelayLeavesDirectAlone() {
    Peer a = OpenPeer(1001, kMemberA), b = OpenPeer(2002, kMemberB);
    CHECK(a.socket && b.socket);
    if (!a.socket || !b.socket) return;
    RelayBroker broker(kRoomToken);
    EnableRelay(a, broker);
    EnableRelay(b, broker);
    Introduce(a, b, true);
    Introduce(b, a, true);
    const int la = a.socket->connect(b.account), lb = b.socket->connect(a.account);
    CHECK(WaitFor([&] { return a.socket->state(la) == LinkState::kConnected && b.socket->state(lb) == LinkState::kConnected; }, 4000));
    CHECK(a.socket->path(la) == LinkPath::kDirect && b.socket->path(lb) == LinkPath::kDirect);
    CHECK(Exchange(a, la, b, lb, 20000, 3) && Exchange(b, lb, a, la, 500, 4));
    std::this_thread::sleep_for(std::chrono::milliseconds(2800));
    CHECK(a.socket->path(la) == LinkPath::kDirect && broker.Total() == 0);
    CHECK(a.socket->relay_stats().frames_out == 0 && a.socket->relay_stats().frames_in == 0);
    CHECK(b.socket->relay_stats().frames_out == 0 && b.socket->relay_stats().frames_in == 0);
}

// No direct answer: after the head start the relay is offered beside direct, and the first authenticated path wins.
void TestRelayTakesOverWhenDirectIsSilent() {
    Peer a = OpenPeer(1001, kMemberA), b = OpenPeer(2002, kMemberB);
    CHECK(a.socket && b.socket);
    if (!a.socket || !b.socket) return;
    RelayBroker broker(kRoomToken);
    EnableRelay(a, broker);
    EnableRelay(b, broker);
    Introduce(a, b, false);
    Introduce(b, a, false);
    const uint64_t started = now_ms();
    const int la = a.socket->connect(b.account), lb = b.socket->connect(a.account);
    std::this_thread::sleep_for(std::chrono::milliseconds(1800));
    CHECK(a.socket->state(la) == LinkState::kConnecting && a.socket->path(la) == LinkPath::kConnectingDirect);
    CHECK(broker.Total() == 0);
    CHECK(WaitFor([&] { return a.socket->state(la) == LinkState::kConnected && b.socket->state(lb) == LinkState::kConnected; }, 6000));
    const uint64_t elapsed = now_ms() - started;
    CHECK(elapsed >= 2400 && elapsed < 7000);
    CHECK(a.socket->path(la) == LinkPath::kRelay && b.socket->path(lb) == LinkPath::kRelay);
    CHECK(is_relay_endpoint(a.socket->remote(la)) && a.socket->remote(la).port == kMemberB);
    CHECK(Exchange(a, la, b, lb, 40000, 5) && Exchange(b, lb, a, la, 3000, 6) && Exchange(a, la, b, lb, 0, 7));
    for (const auto& datagram : broker.Payloads()) {
        CHECK(datagram.size() >= 12 && datagram[0] == 'D' && datagram[1] == 'N' && datagram[2] == 1);
        CHECK(datagram.size() <= kRelayMaxPayload);
    }
    CHECK(broker.Count(kMemberA, kMemberB) > 0 && broker.Count(kMemberB, kMemberA) > 0);
    CHECK(a.socket->relay_stats().frames_out == broker.Count(kMemberA, kMemberB));
    CHECK(a.socket->relay_stats().send_failures == 0 && a.socket->relay_stats().rejected == 0);
}

// The service offers no relay (or the peer is not relayable): direct alone, until the usual timeout.
void TestNoRelayKeepsTheDirectTimeout() {
    for (const bool relayable_peer : {false, true}) {
        Peer a = OpenPeer(1001, kMemberA, 3000), b = OpenPeer(2002, kMemberB, 3000);
        CHECK(a.socket && b.socket);
        if (!a.socket || !b.socket) return;
        RelayBroker broker(kRoomToken);
        if (relayable_peer) {
            broker.Add(a.member, a.socket.get());
            broker.Add(b.member, b.socket.get());
        }
        Introduce(a, b, false, relayable_peer);
        Introduce(b, a, false, relayable_peer);
        CHECK(!a.socket->relay_enabled());
        const uint64_t started = now_ms();
        const int la = a.socket->connect(b.account);
        std::this_thread::sleep_for(std::chrono::milliseconds(2800));
        CHECK(a.socket->path(la) == LinkPath::kConnectingDirect);
        CHECK(WaitFor([&] { return a.socket->state(la) == LinkState::kClosed; }, 3000));
        CHECK(now_ms() - started >= 2900 && a.socket->path(la) == LinkPath::kFailed);
        CHECK(broker.Total() == 0 && a.socket->relay_stats().frames_out == 0);
    }
    Peer c = OpenPeer(1001, kMemberA, 3000), d = OpenPeer(2002, kMemberB, 3000);
    RelayBroker broker(kRoomToken);
    EnableRelay(c, broker);
    Introduce(c, d, false, false);
    const int link = c.socket->connect(d.account);
    std::this_thread::sleep_for(std::chrono::milliseconds(2800));
    CHECK(c.socket->path(link) == LinkPath::kConnectingDirect && broker.Total() == 0);
}

// A three-player room: A-B reach each other directly, A-C and B-C only through the relay, each link on its own path.
void TestMixedRoomPicksAPathPerPeer() {
    Peer a = OpenPeer(1001, kMemberA), b = OpenPeer(2002, kMemberB), c = OpenPeer(3003, kMemberC);
    CHECK(a.socket && b.socket && c.socket);
    if (!a.socket || !b.socket || !c.socket) return;
    RelayBroker broker(kRoomToken);
    for (Peer* peer : {&a, &b, &c}) EnableRelay(*peer, broker);
    Introduce(a, b, true);
    Introduce(b, a, true);
    Introduce(a, c, false);
    Introduce(c, a, false);
    Introduce(b, c, false);
    Introduce(c, b, false);
    const int ab = a.socket->connect(b.account), ba = b.socket->connect(a.account);
    const int ac = a.socket->connect(c.account), ca = c.socket->connect(a.account);
    const int bc = b.socket->connect(c.account), cb = c.socket->connect(b.account);
    const auto connected = [](Peer& p, int link) { return p.socket->state(link) == LinkState::kConnected; };
    CHECK(WaitFor([&] { return connected(a, ab) && connected(b, ba) && connected(a, ac) && connected(c, ca) &&
                               connected(b, bc) && connected(c, cb); }, 8000));
    CHECK(a.socket->path(ab) == LinkPath::kDirect && b.socket->path(ba) == LinkPath::kDirect);
    CHECK(a.socket->path(ac) == LinkPath::kRelay && c.socket->path(ca) == LinkPath::kRelay);
    CHECK(b.socket->path(bc) == LinkPath::kRelay && c.socket->path(cb) == LinkPath::kRelay);
    CHECK(Exchange(a, ab, b, ba, 5000, 1) && Exchange(a, ac, c, ca, 5000, 2) && Exchange(b, bc, c, cb, 5000, 3) &&
          Exchange(c, ca, a, ac, 700, 4) && Exchange(c, cb, b, bc, 700, 5) && Exchange(b, ba, a, ab, 700, 6));
    CHECK(broker.Count(kMemberA, kMemberB) == 0 && broker.Count(kMemberB, kMemberA) == 0);
    CHECK(broker.Count(kMemberA, kMemberC) > 0 && broker.Count(kMemberC, kMemberA) > 0);
    CHECK(broker.Count(kMemberB, kMemberC) > 0 && broker.Count(kMemberC, kMemberB) > 0);
    CHECK(a.socket->peer_at(relay_endpoint(kMemberB)) == b.account && a.socket->peer_at(relay_endpoint(kMemberC)) == c.account);
    CHECK(a.socket->peer_at(relay_endpoint(0x40)) == 0);
}

void IntroduceRelayOnly(Peer& at, const Peer& other) { at.socket->set_peer(other.account, {}, other.member); }

// Issue #49: the lobby gave no usable endpoint for the peer. The game's Rudp address is the relay's for that member,
// the link starts with nothing direct to try, and the relay is offered at once instead of after the head start.
void TestRelayOnlyPeerConnectsAtOnce() {
    const Endpoint unusable{0, 3658};
    const Endpoint public_endpoint{parse_ip("203.0.113.9"), 3658};
    CHECK(signaling_endpoint(public_endpoint, kMemberB) == public_endpoint);
    CHECK(signaling_endpoint(unusable, kMemberB) == relay_endpoint(kMemberB) && relay_endpoint(kMemberB).valid());
    CHECK(!signaling_endpoint(unusable, 0).valid());

    Peer a = OpenPeer(1001, kMemberA), b = OpenPeer(2002, kMemberB);
    CHECK(a.socket && b.socket);
    if (!a.socket || !b.socket) return;
    RelayBroker broker(kRoomToken);
    EnableRelay(a, broker);
    EnableRelay(b, broker);
    IntroduceRelayOnly(a, b);
    IntroduceRelayOnly(b, a);
    CHECK(a.socket->peer_at(signaling_endpoint(unusable, kMemberB)) == b.account);
    const uint64_t started = now_ms();
    const int la = a.socket->connect(b.account), lb = b.socket->connect(a.account);
    CHECK(la > 0 && lb > 0);
    CHECK(WaitFor([&] { return a.socket->state(la) == LinkState::kConnected && b.socket->state(lb) == LinkState::kConnected; }, 2000));
    CHECK(now_ms() - started < 2000);
    CHECK(a.socket->path(la) == LinkPath::kRelay && b.socket->path(lb) == LinkPath::kRelay);
    CHECK(Exchange(a, la, b, lb, 20000, 1) && Exchange(b, lb, a, la, 700, 2));
    CHECK(a.socket->relay_stats().send_failures == 0 && a.socket->relay_stats().rejected == 0);
}

// A peer with a real direct candidate keeps its head start next to a relay-only peer in the same room.
void TestDirectHeadStartSurvivesRelayOnlyNeighbour() {
    Peer a = OpenPeer(1001, kMemberA), b = OpenPeer(2002, kMemberB), c = OpenPeer(3003, kMemberC);
    CHECK(a.socket && b.socket && c.socket);
    if (!a.socket || !b.socket || !c.socket) return;
    RelayBroker broker(kRoomToken);
    for (Peer* peer : {&a, &b, &c}) EnableRelay(*peer, broker);
    IntroduceRelayOnly(a, b);
    IntroduceRelayOnly(b, a);
    Introduce(a, c, false);
    Introduce(c, a, false);
    const int ab = a.socket->connect(b.account), ba = b.socket->connect(a.account);
    const int ac = a.socket->connect(c.account);
    CHECK(WaitFor([&] { return a.socket->state(ab) == LinkState::kConnected && b.socket->state(ba) == LinkState::kConnected; }, 2000));
    CHECK(broker.Count(kMemberA, kMemberC) == 0 && a.socket->path(ac) == LinkPath::kConnectingDirect);
    CHECK(WaitFor([&] { return broker.Count(kMemberA, kMemberC) > 0; }, 3500));
    CHECK(a.socket->path(ac) != LinkPath::kConnectingDirect);
}

// A relay-only link that is already offered still takes a direct path when one shows up and answers first.
void TestRelayOnlyLinkTakesDirectWhenItAppears() {
    Peer a = OpenPeer(1001, kMemberA), b = OpenPeer(2002, kMemberB);
    CHECK(a.socket && b.socket);
    if (!a.socket || !b.socket) return;
    RelayBroker broker(kRoomToken);
    broker.SetSwallowAll(true);
    EnableRelay(a, broker);
    EnableRelay(b, broker);
    IntroduceRelayOnly(a, b);
    IntroduceRelayOnly(b, a);
    const int la = a.socket->connect(b.account), lb = b.socket->connect(a.account);
    CHECK(WaitFor([&] { return a.socket->path(la) == LinkPath::kRelayOffered; }, 1000));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(a.socket->state(la) == LinkState::kConnecting);
    Introduce(a, b, true);
    Introduce(b, a, true);
    CHECK(WaitFor([&] { return a.socket->state(la) == LinkState::kConnected && b.socket->state(lb) == LinkState::kConnected; }, 4000));
    CHECK(a.socket->path(la) == LinkPath::kDirect && b.socket->path(lb) == LinkPath::kDirect);
    CHECK(Exchange(a, la, b, lb, 3000, 3) && broker.Total() == 0);
}

// Three relay-only peers resolve to their own accounts and links; the members never cross.
void TestRelayOnlyPeersStaySeparate() {
    Peer a = OpenPeer(1001, kMemberA), b = OpenPeer(2002, kMemberB), c = OpenPeer(3003, kMemberC);
    CHECK(a.socket && b.socket && c.socket);
    if (!a.socket || !b.socket || !c.socket) return;
    RelayBroker broker(kRoomToken);
    for (Peer* peer : {&a, &b, &c}) EnableRelay(*peer, broker);
    IntroduceRelayOnly(a, b);
    IntroduceRelayOnly(a, c);
    IntroduceRelayOnly(b, a);
    IntroduceRelayOnly(b, c);
    IntroduceRelayOnly(c, a);
    IntroduceRelayOnly(c, b);
    const Endpoint unusable{0, 3658};
    CHECK(a.socket->peer_at(signaling_endpoint(unusable, kMemberB)) == b.account);
    CHECK(a.socket->peer_at(signaling_endpoint(unusable, kMemberC)) == c.account);
    CHECK(a.socket->peer_at(unusable) == 0);
    const int ab = a.socket->connect(b.account), ba = b.socket->connect(a.account);
    const int ac = a.socket->connect(c.account), ca = c.socket->connect(a.account);
    const int bc = b.socket->connect(c.account), cb = c.socket->connect(b.account);
    CHECK(ab != ac);
    const auto connected = [](Peer& p, int link) { return p.socket->state(link) == LinkState::kConnected; };
    CHECK(WaitFor([&] { return connected(a, ab) && connected(b, ba) && connected(a, ac) && connected(c, ca) &&
                               connected(b, bc) && connected(c, cb); }, 3000));
    CHECK(Exchange(a, ab, b, ba, 3000, 1) && Exchange(a, ac, c, ca, 3000, 2) && Exchange(b, bc, c, cb, 3000, 3));
    CHECK(Exchange(c, ca, a, ac, 600, 4) && Exchange(c, cb, b, bc, 600, 5) && Exchange(b, ba, a, ab, 600, 6));
    CHECK(a.socket->remote(ab) == relay_endpoint(kMemberB) && a.socket->remote(ac) == relay_endpoint(kMemberC));
}

// A member nobody introduced, member 0 and a seat taken over by another account never resolve to a wrong peer.
void TestRelayEndpointNeverMapsToAnotherPeer() {
    Peer a = OpenPeer(1001, kMemberA), b = OpenPeer(2002, kMemberB), c = OpenPeer(3003, kMemberC);
    CHECK(a.socket && b.socket && c.socket);
    if (!a.socket || !b.socket || !c.socket) return;
    IntroduceRelayOnly(a, b);
    CHECK(a.socket->peer_at(relay_endpoint(kMemberB)) == b.account);
    CHECK(a.socket->peer_at(relay_endpoint(kMemberC)) == 0 && a.socket->peer_at(relay_endpoint(0)) == 0);
    CHECK(a.socket->peer_at(relay_endpoint(0x40)) == 0 && a.socket->peer_at({0, 3658}) == 0);
    CHECK(a.socket->connect(c.account) == -1);
    a.socket->set_peer(c.account, {}, kMemberB);
    CHECK(a.socket->peer_at(relay_endpoint(kMemberB)) == c.account);
    a.socket->forget_peer(c.account);
    CHECK(a.socket->peer_at(relay_endpoint(kMemberB)) == 0);
}

// No direct candidate and no relay (or a relay that carries nothing): the link ends at the connect timeout.
void TestRelayOnlyPeerFailsAtTheTimeout() {
    for (const bool relay_enabled : {false, true}) {
        Peer a = OpenPeer(1001, kMemberA, 2500), b = OpenPeer(2002, kMemberB, 2500);
        CHECK(a.socket && b.socket);
        if (!a.socket || !b.socket) return;
        RelayBroker broker(kRoomToken);
        broker.SetSwallowAll(true);
        broker.Add(kMemberA, a.socket.get());
        broker.Add(kMemberB, b.socket.get());
        if (relay_enabled) a.socket->enable_relay(kRoomToken, broker.From(kMemberA));
        IntroduceRelayOnly(a, b);
        const uint64_t started = now_ms();
        const int la = a.socket->connect(b.account);
        CHECK(la > 0);
        if (relay_enabled) {
            CHECK(WaitFor([&] { return a.socket->path(la) == LinkPath::kRelayOffered; }, 1000));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            CHECK(a.socket->path(la) == LinkPath::kConnectingDirect && a.socket->relay_stats().frames_out == 0);
        }
        CHECK(WaitFor([&] { return a.socket->state(la) == LinkState::kClosed; }, 4000));
        CHECK(now_ms() - started >= 2400 && a.socket->path(la) == LinkPath::kFailed);
        a.socket->disable_relay();
    }
}

// Relay frames that are not this room's, not from a known peer, or not a PeerSocket datagram never reach a link.
void TestRelayRefusesForeignFrames() {
    Peer a = OpenPeer(1001, kMemberA), b = OpenPeer(2002, kMemberB);
    CHECK(a.socket && b.socket);
    if (!a.socket || !b.socket) return;
    const std::vector<uint8_t> datagram = Pattern(48, 1);
    const auto frame = [&](uint32_t token, uint16_t member) {
        return encode_relay_frame(token, member, 1, datagram.data(), datagram.size());
    };
    std::vector<uint8_t> f = frame(kRoomToken, kMemberB);
    CHECK(!a.socket->deliver_relay(f.data(), f.size()));
    RelayBroker broker(kRoomToken);
    EnableRelay(a, broker);
    Introduce(a, b, false);
    f = frame(kRoomToken + 1, kMemberB);
    CHECK(!a.socket->deliver_relay(f.data(), f.size()));
    f = frame(kRoomToken, kMemberC);
    CHECK(!a.socket->deliver_relay(f.data(), f.size()));
    f = frame(kRoomToken, kMemberB);
    f.pop_back();
    CHECK(!a.socket->deliver_relay(f.data(), f.size()));
    std::vector<uint8_t> stun(20, 0);
    stun[0] = 0x01, stun[1] = 0x01, stun[4] = 0x21, stun[5] = 0x12, stun[6] = 0xa4, stun[7] = 0x42;
    f = encode_relay_frame(kRoomToken, kMemberB, 1, stun.data(), stun.size());
    CHECK(!a.socket->deliver_relay(f.data(), f.size()));
    CHECK(a.socket->relay_stats().rejected == 5);
    f = frame(kRoomToken, kMemberB);
    CHECK(a.socket->deliver_relay(f.data(), f.size()));
    CHECK(a.socket->relay_stats().frames_in == 1);
    a.socket->disable_relay();
    CHECK(!a.socket->relay_enabled() && !a.socket->deliver_relay(f.data(), f.size()));
}

// Real PeerSocket datagrams, carried and unwrapped, are what the sender wrote: handshakes, data, acknowledgements and
// retransmissions, with a lossy relay forcing resends.
void TestRelayKeepsDatagramsIntact() {
    Peer a = OpenPeer(1001, kMemberA), b = OpenPeer(2002, kMemberB);
    CHECK(a.socket && b.socket);
    if (!a.socket || !b.socket) return;
    RelayBroker broker(kRoomToken);
    broker.SetDropPercent(20);
    EnableRelay(a, broker);
    EnableRelay(b, broker);
    Introduce(a, b, false);
    Introduce(b, a, false);
    const int la = a.socket->connect(b.account), lb = b.socket->connect(a.account);
    CHECK(WaitFor([&] { return a.socket->state(la) == LinkState::kConnected && b.socket->state(lb) == LinkState::kConnected; }, 12000));
    CHECK(a.socket->path(la) == LinkPath::kRelay);
    for (const size_t size : {size_t(1), size_t(1149), size_t(1150), size_t(1151), size_t(2300), size_t(100000)}) {
        CHECK(Exchange(a, la, b, lb, size, uint8_t(size)) && Exchange(b, lb, a, la, size, uint8_t(size + 1)));
    }
    CHECK(a.socket->stats(la).resends + b.socket->stats(lb).resends > 0);
    size_t largest_data = 0;
    std::map<uint8_t, size_t> types;
    for (const auto& datagram : broker.Payloads()) {
        CHECK(datagram.size() >= 12 && datagram[0] == 'D' && datagram[1] == 'N' && datagram[2] == 1);
        const uint8_t type = datagram[3];
        ++types[type];
        if (type == 1 || type == 2) CHECK(datagram.size() == 48);
        if (type == 4) CHECK(datagram.size() == 20);
        if (type == 3) largest_data = std::max(largest_data, datagram.size());
        CHECK(type >= 1 && type <= 8);
    }
    CHECK(largest_data == 12 + 13 + 1150);
    CHECK(types[1] > 0 && types[2] > 0 && types[3] > 0 && types[4] > 0);
    CHECK(a.socket->relay_stats().frames_out >= broker.Count(kMemberA, kMemberB));
}

// Another room's key never authenticates over the relay either.
void TestRelayDoesNotBypassTheRoomKey() {
    Peer a = OpenPeer(1001, kMemberA, 5000), b = OpenPeer(2002, kMemberB, 5000);
    CHECK(a.socket && b.socket);
    if (!a.socket || !b.socket) return;
    RoomKey other{};
    other[5] = 9;
    b.socket->set_room(other, b.account);
    RelayBroker broker(kRoomToken);
    EnableRelay(a, broker);
    EnableRelay(b, broker);
    Introduce(a, b, false);
    Introduce(b, a, false);
    const int la = a.socket->connect(b.account), lb = b.socket->connect(a.account);
    CHECK(WaitFor([&] { return a.socket->state(la) == LinkState::kClosed; }, 7000));
    CHECK(b.socket->state(lb) != LinkState::kConnected && a.socket->relay_stats().frames_out > 0 && broker.Total() > 0);
}

// Timing: direct hellos start at once and keep going; the relay stays silent until the window, then joins them.
void TestRelayWindowAndDirectCadence() {
    int hellos_without_relay = 0, hellos_with_relay = 0;
    for (const bool with_relay : {false, true}) {
        SilentPeer silent;
        Peer a = OpenPeer(1001, kMemberA, 6000);
        Peer b = OpenPeer(2002, kMemberB, 6000);
        CHECK(a.socket && b.socket);
        if (!a.socket || !b.socket) return;
        RelayBroker broker(kRoomToken);
        broker.SetSwallowAll(true);
        if (with_relay) EnableRelay(a, broker);
        const uint32_t loopback = parse_ip("127.0.0.1");
        a.socket->set_peer(b.account, {{loopback, silent.port()}}, b.member);
        const uint64_t started = now_ms();
        const int la = a.socket->connect(b.account);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(silent.received() >= 1);
        uint64_t relay_at = 0;
        while (now_ms() - started < 2200) {
            if (with_relay && !relay_at && a.socket->relay_stats().frames_out > 0) relay_at = now_ms() - started;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const int direct_in_window = int(silent.received());
        (with_relay ? hellos_with_relay : hellos_without_relay) = direct_in_window;
        CHECK(direct_in_window >= 15 && direct_in_window <= 30);
        CHECK(relay_at == 0 && a.socket->path(la) == LinkPath::kConnectingDirect);
        if (!with_relay) continue;
        while (!relay_at && now_ms() - started < 4000) {
            if (a.socket->relay_stats().frames_out > 0) relay_at = now_ms() - started;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(relay_at >= 2400 && relay_at <= 3400);
        CHECK(a.socket->path(la) == LinkPath::kRelayOffered);
        const uint64_t before = silent.received();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        CHECK(silent.received() >= before + 4);
    }
    CHECK(std::abs(hellos_with_relay - hellos_without_relay) <= 4);
}

// A direct path that only becomes reachable after the relay was offered still wins: nothing is cancelled.
void TestDirectStillWinsAfterTheRelayWindow() {
    Peer a = OpenPeer(1001, kMemberA), b = OpenPeer(2002, kMemberB);
    CHECK(a.socket && b.socket);
    if (!a.socket || !b.socket) return;
    RelayBroker broker(kRoomToken);
    broker.SetSwallowAll(true);
    EnableRelay(a, broker);
    EnableRelay(b, broker);
    Introduce(a, b, false);
    Introduce(b, a, false);
    const int la = a.socket->connect(b.account), lb = b.socket->connect(a.account);
    std::this_thread::sleep_for(std::chrono::milliseconds(3300));
    CHECK(a.socket->path(la) == LinkPath::kRelayOffered && a.socket->state(la) == LinkState::kConnecting);
    CHECK(a.socket->relay_stats().frames_out > 0);
    Introduce(a, b, true);
    Introduce(b, a, true);
    CHECK(WaitFor([&] { return a.socket->state(la) == LinkState::kConnected && b.socket->state(lb) == LinkState::kConnected; }, 4000));
    CHECK(a.socket->path(la) == LinkPath::kDirect && b.socket->path(lb) == LinkPath::kDirect);
    CHECK(Exchange(a, la, b, lb, 3000, 1) && broker.Total() == 0);
}

// The shipped timeouts: 15 s to connect, 10 s idle, 2.5 s before the relay. Nothing about them depends on the relay.
void TestTimeoutsAreUnchanged() {
    const PeerSocket::Options defaults;
    CHECK(defaults.connect_timeout_ms == 15000 && defaults.idle_timeout_ms == 10000 && defaults.relay_after_ms == 2500);
    PeerSocket::Options options;
    std::string error;
    auto a = PeerSocket::open(options, &error);
    CHECK(a != nullptr);
    if (!a) return;
    a->set_room(TestRoomKey(), 1001);
    RelayBroker broker(kRoomToken);
    broker.SetSwallowAll(true);
    broker.Add(kMemberA, a.get());
    a->enable_relay(kRoomToken, broker.From(kMemberA));
    a->set_peer(2002, {{parse_ip("127.0.0.1"), 1}}, kMemberB);
    const uint64_t started = now_ms();
    const int link = a->connect(2002);
    CHECK(WaitFor([&] { return a->state(link) == LinkState::kClosed; }, 18000));
    const uint64_t elapsed = now_ms() - started;
    CHECK(elapsed >= 14500 && elapsed <= 17500);
    CHECK(a->path(link) == LinkPath::kFailed && a->relay_stats().frames_out > 0);
    a->disable_relay();
}

// The relay cannot take frames (binary transport unavailable or refusing): direct alone, original timeout, no stuck link.
void TestRelayRefusalFailsLikeDirectOnly() {
    Peer a = OpenPeer(1001, kMemberA, 3500), b = OpenPeer(2002, kMemberB, 3500);
    CHECK(a.socket && b.socket);
    if (!a.socket || !b.socket) return;
    RelayBroker broker(kRoomToken);
    broker.SetRefuse(true);
    EnableRelay(a, broker);
    EnableRelay(b, broker);
    Introduce(a, b, false);
    Introduce(b, a, false);
    const uint64_t started = now_ms();
    const int la = a.socket->connect(b.account);
    CHECK(WaitFor([&] { return a.socket->state(la) == LinkState::kClosed; }, 6000));
    CHECK(now_ms() - started >= 3300 && a.socket->path(la) == LinkPath::kFailed);
    CHECK(a.socket->relay_stats().send_failures > 0 && a.socket->relay_stats().frames_out == 0 && broker.Total() == 0);
    a.socket->close(la);
    CHECK(a.socket->state(la) == LinkState::kClosed && a.socket->connect(b.account) > 0);
}

// A relayed link whose relay goes away (lobby gone, frames swallowed, token changed, sends refused) closes by the idle
// rule instead of hanging, and the game-facing state becomes closed.
void TestRelayLossAfterConnectionEndsTheLink() {
    enum Fault { kSwallow, kDisable, kStaleToken, kRefuse };
    for (const Fault fault : {kSwallow, kDisable, kStaleToken, kRefuse}) {
        PeerSocket::Options options;
        options.idle_timeout_ms = 2500;
        options.connect_timeout_ms = 8000;
        std::string error;
        Peer a{PeerSocket::open(options, &error), 1001, kMemberA}, b{PeerSocket::open(options, &error), 2002, kMemberB};
        CHECK(a.socket && b.socket);
        if (!a.socket || !b.socket) return;
        a.socket->set_room(TestRoomKey(), a.account);
        b.socket->set_room(TestRoomKey(), b.account);
        RelayBroker broker(kRoomToken);
        EnableRelay(a, broker);
        EnableRelay(b, broker);
        Introduce(a, b, false);
        Introduce(b, a, false);
        const int la = a.socket->connect(b.account), lb = b.socket->connect(a.account);
        CHECK(WaitFor([&] { return a.socket->state(la) == LinkState::kConnected && b.socket->state(lb) == LinkState::kConnected; }, 8000));
        CHECK(a.socket->path(la) == LinkPath::kRelay && Exchange(a, la, b, lb, 500, 1));
        switch (fault) {
        case kSwallow: broker.SetSwallowAll(true); break;
        case kDisable:
            a.socket->disable_relay();
            b.socket->disable_relay();
            break;
        case kStaleToken:
            a.socket->enable_relay(kRoomToken + 1, broker.From(kMemberA));
            b.socket->enable_relay(kRoomToken + 1, broker.From(kMemberB));
            break;
        case kRefuse: broker.SetRefuse(true); break;
        }
        CHECK(WaitFor([&] { return a.socket->state(la) == LinkState::kClosed && b.socket->state(lb) == LinkState::kClosed; }, 8000));
        CHECK(a.socket->path(la) == LinkPath::kFailed);
        CHECK(!a.socket->send(la, "x", 1));
    }
}

// The capability matrix between clients and lobbies: relay fields are additive and a missing one is direct-only.
void TestRelayCompatibilityMatrix() {
    const auto reply = [](const char* text) { return *Json::parse(text); };
    const Json updated = reply(R"({"account":"1","capabilities":{"relay_supported":true,"relay_max_payload":1200}})");
    const Json older = reply(R"({"account":"1","name":"x","friend_code":"AAAA-BBBB","ip":"1.2.3.4","time":1})");
    const Json small = reply(R"({"capabilities":{"relay_supported":true,"relay_max_payload":800}})");
    const Json odd = reply(R"({"capabilities":{"relay_supported":"yes","relay_max_payload":-5}})");
    const Json odd_payload = reply(R"({"capabilities":{"relay_supported":true,"relay_max_payload":-1}})");
    CHECK(relay_capability(updated).usable());
    CHECK(!relay_capability(older).usable() && !relay_capability(older).supported);
    CHECK(!relay_capability(small).usable() && relay_capability(small).supported);
    CHECK(!relay_capability(odd).usable() && !relay_capability(odd_payload).usable());
    CHECK(!relay_capability(reply("{}")).usable() && !relay_capability(reply(R"({"capabilities":null})")).usable());

    Identity identity;
    identity.account = 5;
    identity.secret = std::string(64, 'a');
    const auto keys = [](const Json& j) {
        std::vector<std::string> out;
        for (const auto& [key, value] : j.members()) out.push_back(key);
        return out;
    };
    const std::vector<std::string> old_keys{"proto", "game", "account", "secret", "name", "presence", "online_protocol", "version", "platform"};
    CHECK(keys(hello_body(identity, "n", "g", true)) == old_keys);
    std::vector<std::string> new_keys = old_keys;
    new_keys.push_back("relay");
    CHECK(keys(hello_body(identity, "n", "g", true, true)) == new_keys);
    CHECK(kOnlineProtocolVersion == 1 && kLobbyProtocol == 1);
    CHECK(online_compatible(kOnlineProtocolVersion) && !online_compatible(0));
}

// Requires a development lobby (DCP_TEST_LOBBY): relay frames over real WebSockets between two room members.
void TestLobbyRelay(const fs::path& base) {
    const char* url = std::getenv("DCP_TEST_LOBBY");
    if (!url || !*url) return;
    const Identity host_id = load_identity(base / "relay-a"), guest_id = load_identity(base / "relay-b");
    std::mutex mutex;
    std::vector<std::vector<uint8_t>> received;
    LobbyLink::Options options;
    options.url = url;
    options.hello = [&] { return hello_body(host_id, "RelayA", "TEST-1.00", true, true); };
    LobbyLink host(options);
    options.hello = [&] { return hello_body(guest_id, "RelayB", "TEST-1.00", true, true); };
    LobbyLink guest(options);
    guest.set_binary_sink([&](const uint8_t* data, size_t size) {
        std::lock_guard<std::mutex> lock(mutex);
        received.emplace_back(data, data + size);
    });
    CHECK(!host.send_binary({1, 2, 3}) && host.binary_dropped() == 1);
    host.start();
    guest.start();
    auto next = [](LobbyLink& link, const std::function<bool(const LobbyMessage&)>& match, LobbyMessage& out) {
        return WaitFor([&] {
            LobbyMessage m;
            while (link.poll(m)) {
                if (match(m)) {
                    out = std::move(m);
                    return true;
                }
            }
            return false;
        }, 5000);
    };
    LobbyMessage m;
    CHECK(next(host, [](const LobbyMessage& x) { return x.kind == LobbyMessage::Kind::kUp; }, m));
    CHECK(relay_capability(m.body).supported && relay_capability(m.body).max_payload == int(kRelayMaxPayload));
    CHECK(next(guest, [](const LobbyMessage& x) { return x.kind == LobbyMessage::Kind::kUp; }, m));
    Json room = Json::Object();
    room.set("world", 1);
    room.set("slots", 4);
    room.set("flags", int64_t(0x86000000u));
    const uint32_t create = host.request("room.create", room);
    CHECK(next(host, [&](const LobbyMessage& x) { return x.kind == LobbyMessage::Kind::kResponse && x.seq == create; }, m));
    const std::string room_id = m.body["room"]["id"].as_string();
    const uint16_t host_member = uint16_t(m.body["me"].as_int());
    std::vector<uint8_t> key_bytes;
    CHECK(hex_decode(m.body["room"]["key"].as_string(), key_bytes) && key_bytes.size() == 16);
    RoomKey key{};
    std::copy(key_bytes.begin(), key_bytes.end(), key.begin());
    Json join = Json::Object();
    join.set("room", room_id);
    const uint32_t joined = guest.request("room.join", join);
    CHECK(next(guest, [&](const LobbyMessage& x) { return x.kind == LobbyMessage::Kind::kResponse && x.seq == joined; }, m));
    const uint16_t guest_member = uint16_t(m.body["me"].as_int());

    const std::vector<uint8_t> datagram = Pattern(1175, 4);
    CHECK(host.send_binary(encode_relay_frame(relay_token(key), guest_member, 5, datagram.data(), datagram.size())));
    CHECK(WaitFor([&] {
        std::lock_guard<std::mutex> lock(mutex);
        return !received.empty();
    }, 5000));
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto frame = decode_relay_frame(received[0].data(), received[0].size());
        CHECK(frame && frame->member == host_member && frame->seq == 5 && frame->token == relay_token(key));
        if (frame) CHECK(frame->payload == datagram);
    }

    // Text requests keep their answers while binary frames flow in both directions.
    size_t answers = 0;
    const size_t frames_before = [&] {
        std::lock_guard<std::mutex> lock(mutex);
        return received.size();
    }();
    const std::vector<uint8_t> small = Pattern(200, 2);
    for (int i = 0; i < 300; ++i) {
        CHECK(host.send_binary(encode_relay_frame(relay_token(key), guest_member, 100 + i, small.data(), small.size())));
        if (i % 50 == 0) host.request("worlds", Json::Object());
    }
    CHECK(WaitFor([&] {
        LobbyMessage reply;
        while (host.poll(reply)) answers += reply.kind == LobbyMessage::Kind::kResponse && reply.code == "ok" ? 1 : 0;
        std::lock_guard<std::mutex> lock(mutex);
        return received.size() >= frames_before + 300 && answers >= 6;
    }, 10000));
    {
        std::lock_guard<std::mutex> lock(mutex);
        CHECK(received.size() == frames_before + 300);
        for (size_t i = frames_before; i < received.size(); ++i) {
            const auto frame = decode_relay_frame(received[i].data(), received[i].size());
            CHECK(frame && frame->seq == 100 + i - frames_before && frame->payload == small);
        }
    }

    // A wrong token is dropped by the lobby and the connection stays up.
    const size_t before = [&] {
        std::lock_guard<std::mutex> lock(mutex);
        return received.size();
    }();
    CHECK(host.send_binary(encode_relay_frame(relay_token(key) ^ 1, guest_member, 6, datagram.data(), datagram.size())));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {
        std::lock_guard<std::mutex> lock(mutex);
        CHECK(received.size() == before);
    }
    CHECK(host.up());

    // Far more than the queue holds in one burst: the excess is dropped and counted, never buffered without bound.
    const std::vector<uint8_t> burst = encode_relay_frame(relay_token(key), guest_member, 7, datagram.data(), datagram.size());
    size_t accepted = 0;
    for (int i = 0; i < 5000; ++i) accepted += host.send_binary(std::vector<uint8_t>(burst)) ? 1 : 0;
    CHECK(accepted + host.binary_dropped() - 1 == 5000 && host.binary_dropped() > 1);
    CHECK(host.up());
    guest.stop();
    CHECK(!guest.send_binary({1}) && guest.binary_dropped() >= 1);
    host.stop();
    CHECK(!host.send_binary({1}));
}

void TestLinuxParity() {
    for (const char* file :
         {"src/runtime_online.cpp", "src/runtime_matching2.cpp", "src/runtime_rudp.cpp", "src/runtime_cheats.cpp",
          "src/skip_offers.c", "src/system_type_hook.c", "src/lzcnt_compat.c", "src/isa_emulate.c",
          "src/net/lobby_link.cpp", "src/net/relay_frame.cpp", "src/net/connection_test.cpp", "src/net/online_config.cpp", "src/net/identity.cpp",
          "src/net/websocket.cpp", "launcher/online_client.cpp", "launcher/online_page.cpp", "launcher/online_view.cpp",
          "launcher/online_settings.cpp", "launcher/text_languages.cpp", "launcher/game_profile.cpp",
          "launcher/settings.cpp", "gpu/shim/dcp_ping.h", "gpu/shim/dcp_amd_water_fix.h", "gpu/shim/dcp_water_diag.h"}) {
        const std::string source = ReadSource(file);
        const bool guarded = source.find("_WIN32") != std::string::npos || source.find("__linux__") != std::string::npos;
        if (source.empty() || guarded) std::printf("not shared by Linux and Windows: %s\n", file);
        CHECK(!source.empty() && !guarded);
    }

    for (const char* file : {"src/net/relay_frame.h", "src/net/relay_frame.cpp", "src/net/lobby_link.h", "src/net/lobby_link.cpp",
                             "src/net/websocket.h", "src/net/connection_test.h"}) {
        const std::string source = ReadSource(file);
        for (const char* windows_only : {"windows.h", "winsock", "winhttp", "HINTERNET", "WSA", "SOCKET ", "_WIN32", "DWORD", "HANDLE"}) {
            if (source.find(windows_only) != std::string::npos) std::printf("%s names %s\n", file, windows_only);
            CHECK(source.find(windows_only) == std::string::npos);
        }
    }
    const std::string peer = ReadSource("src/net/peer_socket.cpp");
    const size_t relay_from = peer.find("void PeerSocket::enable_relay"), relay_to = peer.find("void PeerSocket::SendHello");
    CHECK(relay_from != std::string::npos && relay_to > relay_from);
    for (const char* windows_only : {"windows.h", "winsock", "WSA", "_WIN32", "DWORD", "HANDLE", "closesocket"})
        CHECK(peer.substr(relay_from, relay_to - relay_from).find(windows_only) == std::string::npos);
    CHECK(ReadSource("src/net/websocket_posix.cpp").find("bool send_binary(") != std::string::npos);
    CHECK(ReadSource("src/net/websocket_win.cpp").find("WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE") != std::string::npos);
    CHECK(ReadSource("src/net/websocket_posix.cpp").find("binary = true") != std::string::npos &&
          ReadSource("src/net/websocket_win.cpp").find("binary = true") != std::string::npos);

    const std::string cmake = ReadSource("CMakeLists.txt");
    CHECK(cmake.find("src/net/relay_frame.cpp") != std::string::npos);
    const size_t main_at = cmake.find("set(DCP_MAIN_SOURCES ");
    const std::string main_sources = cmake.substr(main_at, cmake.find(')', main_at) - main_at);
    for (const char* file : {"src/skip_offers.c", "src/system_type_hook.c", "src/lzcnt_compat.c", "src/isa_emulate.c",
                             "src/runtime_cheats.cpp", "${DCP_RUNTIME_SOURCES}"})
        CHECK(main_at != std::string::npos && main_sources.find(file) != std::string::npos);
    const size_t runtime_at = cmake.find("set(DCP_RUNTIME_SOURCES");
    const std::string runtime_sources = cmake.substr(runtime_at, cmake.find(')', runtime_at) - runtime_at);
    for (const char* file : {"src/runtime_online.cpp", "src/runtime_matching2.cpp", "src/runtime_rudp.cpp"})
        CHECK(runtime_at != std::string::npos && runtime_sources.find(file) != std::string::npos);
    CHECK(cmake.find("$<IF:$<BOOL:${WIN32}>,src/net/websocket_win.cpp,src/net/websocket_posix.cpp>") != std::string::npos);

    const std::string windows = ReadSource("tools/test.ps1");
    const std::string build_sh = ReadSource("tools/build.sh");
    const size_t ninja = windows.find("& ninja -C $buildDir ");
    CHECK(ninja != std::string::npos);
    const std::string line = windows.substr(ninja, windows.find('\n', ninja) - ninja);
    size_t targets = 0;
    for (size_t at = line.find(" dcp-"); at != std::string::npos; at = line.find(" dcp-", at + 1)) {
        const std::string target = line.substr(at + 1, line.find_first_of(" \r", at + 1) - at - 1);
        ++targets;
        if (target == "dcp-memory-windows-tests" || target == "dcp-update-helper-tests" || target == "dcp-log-tests")
            continue;
        const bool runs = build_sh.find("\"$build_dir/bin/" + target + "\"") != std::string::npos;
        if (!runs) std::printf("tools/build.sh --test does not run %s\n", target.c_str());
        CHECK(runs);
    }
    CHECK(targets > 20);

    for (const char* doc : {"README.md", "packaging/README.txt", "packaging/README-linux.txt"}) {
        const std::string text = ReadSource(doc);
        CHECK(!text.empty() && text.find("Online play is not supported") == std::string::npos);
    }
    for (const char* doc : {"packaging/README.txt", "packaging/README-linux.txt"}) {
        const std::string text = ReadSource(doc);
        CHECK(text.find("Online Play") != std::string::npos && text.find("F3 shows the Ping") != std::string::npos);
    }
}

} // namespace

int main() {
    if (!sockets_ready()) {
        std::printf("FAILED: no sockets\n");
        return 1;
    }
    const fs::path base = fs::temp_directory_path() / "dcp-net-tests";
    fs::create_directories(base);
    const char* lobby = std::getenv("DCP_TEST_LOBBY");
    const bool lobby_set = lobby && *lobby;
    Group("foundations", [&] {
        TestJson();
        TestCrypto();
        TestIdentity(base);
        TestConfig();
        TestOnlineVersion();
        TestAddresses();
        TestStun();
        TestSecondInstancePort();
    });
    Group("connection-test", [&] {
        TestConnectionSummary();
        TestConnectionTestSocket();
        TestWorstRtt();
    });
    Group("direct", [&] {
        TestPeerLink(0);
        TestPeerLink(15);
        TestPeerRefusesWrongRoom();
        TestRelayLeavesDirectAlone();
        TestDirectStillWinsAfterTheRelayWindow();
        TestTimeoutsAreUnchanged();
    });
    Group("relay-codec", [&] {
        TestRelayFrame();
        TestRelayRefusesForeignFrames();
    });
    Group("relay-fallback", [&] {
        TestRelayWindowAndDirectCadence();
        TestRelayTakesOverWhenDirectIsSilent();
        TestMixedRoomPicksAPathPerPeer();
        TestRelayOnlyPeerConnectsAtOnce();
        TestDirectHeadStartSurvivesRelayOnlyNeighbour();
        TestRelayOnlyLinkTakesDirectWhenItAppears();
        TestRelayOnlyPeersStaySeparate();
        TestRelayEndpointNeverMapsToAnotherPeer();
        TestRelayKeepsDatagramsIntact();
    });
    Group("failure-paths", [&] {
        TestNoRelayKeepsTheDirectTimeout();
        TestRelayOnlyPeerFailsAtTheTimeout();
        TestRelayRefusalFailsLikeDirectOnly();
        TestRelayLossAfterConnectionEndsTheLink();
        TestRelayDoesNotBypassTheRoomKey();
    });
    Group("protocol", [&] { TestRelayCompatibilityMatrix(); });
    Group("linux-parity", [&] {
        TestSystemCaBundle();
        TestLinuxParity();
    });
    if (lobby_set) {
        Group("lobby-binary", [&] {
            TestLobby(base);
            TestLobbyRelay(base);
        });
    } else {
        std::printf("GROUP|lobby-binary|SKIPPED|DCP_TEST_LOBBY is not set\n");
    }
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
