// Focused tests of the online layer (src/net): JSON, hashes, identity, settings, STUN, lobby URLs and the peer
// transport over loopback (including a lossy relay). With DCP_TEST_LOBBY=ws://127.0.0.1:<port>/ set to a running
// development lobby (server/lobby/local/server.js) the lobby link and two-player room flow are exercised too.
#include <atomic>
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

#include "net/addr.h"
#include "net/connection_test.h"
#include "net/crypto.h"
#include "net/identity.h"
#include "net/json.h"
#include "net/lobby_link.h"
#include "net/online_config.h"
#include "net/online_version.h"
#include "net/peer_socket.h"
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
    CHECK(text_of(timed_report, "Direct connections") == "Available.");
    for (const auto& line : timed_report.lines) CHECK(line.label.find("Ping") == std::string::npos);
    timed.lobby_ok = false;
    CHECK(text_of(summarize_connection(timed), "Online service") ==
          "Cannot reach the online service. Check your internet connection.");

    const std::string test = ReadSource("src/net/connection_test.cpp");
    size_t opens = 0;
    for (size_t at = test.find("session.open("); at != std::string::npos; at = test.find("session.open(", at + 1)) ++opens;
    CHECK(opens == 1 && test.find("facts.lobby_latency_ms = int64_t(now_ms() - start);") != std::string::npos);
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

} // namespace

int main() {
    if (!sockets_ready()) {
        std::printf("FAILED: no sockets\n");
        return 1;
    }
    const fs::path base = fs::temp_directory_path() / "dcp-net-tests";
    fs::create_directories(base);
    TestJson();
    TestCrypto();
    TestIdentity(base);
    TestConfig();
    TestConnectionSummary();
    TestWorstRtt();
    TestOnlineVersion();
    TestAddresses();
    TestStun();
    TestPeerLink(0);
    TestPeerLink(15);
    TestPeerRefusesWrongRoom();
    TestSecondInstancePort();
    TestLobby(base);
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
