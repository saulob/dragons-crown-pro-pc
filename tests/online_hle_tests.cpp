// The game's side of online play without the game: this test calls the Matching2, libSceNet and libSceRudp entry
// points the way DcNetMatching and RudpMgr do (request structs, callbacks pumped like sceNpCheckCallback), while a
// second player joins through the lobby protocol and connects over UDP. Needs DCP_TEST_LOBBY=ws://127.0.0.1:<port>/
// pointing at a running development lobby (server/lobby/local/server.js).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <thread>
#include <vector>

#include "net/lobby_link.h"
#include "net/online_version.h"
#include "net/peer_socket.h"
#include "runtime_online.h"

#define SYSV __attribute__((sysv_abi))

namespace fs = std::filesystem;
using namespace dcp;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);          \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

online::Settings g_settings;

template <class T> T Rd(const void* p, size_t offset) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t*>(p) + offset, sizeof(v));
    return v;
}
template <class T> void Wr(void* p, size_t offset, T v) { std::memcpy(static_cast<uint8_t*>(p) + offset, &v, sizeof(v)); }

template <class F> F Fn(const char* name) {
    uintptr_t f = online::matching2_export(name);
    if (!f) f = online::rudp_export(name);
    if (!f) std::printf("missing export %s\n", name);
    return reinterpret_cast<F>(f);
}

struct Seen {
    std::vector<std::pair<uint16_t, int32_t>> context;
    struct Request {
        uint32_t id;
        uint16_t event;
        int32_t error;
        std::vector<uint8_t> copy; // what the test read from the data during the callback
        uint64_t room = 0;
        uint16_t me = 0, first_member = 0, slots = 0;
        int32_t count = 0;
    };
    std::vector<Request> requests;
    struct Room {
        uint16_t event;
        uint16_t member = 0, prev = 0, next = 0;
        std::string name;
    };
    std::vector<Room> rooms;
    std::vector<std::pair<uint16_t, uint16_t>> signals; // member, event
} g_seen;

SYSV void OnContext(uint16_t ctx, uint16_t event, uint8_t cause, int32_t error, void* arg) {
    (void)ctx, (void)cause, (void)arg;
    g_seen.context.push_back({event, error});
}

SYSV void OnRequest(uint16_t ctx, uint32_t id, uint16_t event, int32_t error, const void* data, void* arg) {
    (void)ctx, (void)arg;
    Seen::Request r{id, event, error, {}};
    if (data && !error && (event == 0x0101 || event == 0x0102)) {
        const uint8_t* internal = Rd<const uint8_t*>(data, 0);
        r.room = Rd<uint64_t>(internal, 0x18);
        r.slots = Rd<uint16_t>(internal, 0xa);
        r.first_member = Rd<uint16_t>(Rd<const uint8_t*>(data, 8), 0x38);
        r.me = Rd<uint16_t>(Rd<const uint8_t*>(data, 0x18), 0x38);
        const uint8_t* attrs = Rd<const uint8_t*>(internal, 0x48);
        if (attrs && Rd<uint32_t>(internal, 0x50)) {
            const uint8_t* bytes = Rd<const uint8_t*>(attrs, 0x18);
            r.copy.assign(bytes, bytes + Rd<uint64_t>(attrs, 0x20));
        }
    }
    if (data && !error && event == 0x0002) r.count = Rd<int32_t>(data, 8);
    if (data && !error && event == 0x0106) {
        r.count = Rd<int32_t>(data, 8);
        const uint8_t* room = Rd<const uint8_t*>(data, 0x10);
        if (room) {
            r.room = Rd<uint64_t>(room, 0x20);
            const uint8_t* ints = Rd<const uint8_t*>(room, 0x78);
            r.copy.assign(ints, ints + 64);
        }
    }
    g_seen.requests.push_back(std::move(r));
}

SYSV void OnRoom(uint16_t ctx, uint64_t room, uint16_t event, const void* data, void* arg) {
    (void)ctx, (void)room, (void)arg;
    Seen::Room r;
    r.event = event;
    if ((event == 0x8101 || event == 0x8102) && data) {
        const uint8_t* member = Rd<const uint8_t*>(data, 0);
        r.member = Rd<uint16_t>(member, 0x38);
        r.name = reinterpret_cast<const char*>(member + 0x20);
    }
    if (event == 0x1105 && data) r.prev = Rd<uint16_t>(data, 0), r.next = Rd<uint16_t>(data, 2);
    g_seen.rooms.push_back(r);
}

SYSV void OnSignal(uint16_t ctx, uint64_t room, uint16_t member, uint16_t event, int32_t error, void* arg) {
    (void)ctx, (void)room, (void)error, (void)arg;
    g_seen.signals.push_back({member, event});
}

bool PumpUntil(const std::function<bool()>& done, int timeout_ms = 8000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        online::pump();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    return done();
}

const Seen::Request* RequestById(uint32_t id) {
    for (const auto& r : g_seen.requests)
        if (r.id == id) return &r;
    return nullptr;
}

} // namespace

namespace dcp::online {
const Settings& settings() { return g_settings; }
} // namespace dcp::online

int main() {
    const char* url = std::getenv("DCP_TEST_LOBBY");
    if (!url || !*url) {
        std::printf("skipped: set DCP_TEST_LOBBY to a running development lobby\n");
        return 0;
    }
    const fs::path base = fs::temp_directory_path() / "dcp-online-hle-tests";
    fs::remove_all(base);
    g_settings.enabled = true;
    g_settings.config.lobby = url;
    g_settings.config.upnp = false;
    g_settings.config.stun = "off";
    g_settings.identity = net::load_identity(base / "host");
    g_settings.name = "HostA";
    g_settings.game = "HLE-TEST";

    using Init = int32_t SYSV (*)(const void*);
    using CreateContext = int32_t SYSV (*)(const void*, uint16_t*);
    using RegisterCtx = int32_t SYSV (*)(void*, void*);
    using Start = int32_t SYSV (*)(uint16_t, uint64_t);
    using Stop = int32_t SYSV (*)(uint16_t);
    using Request = int32_t SYSV (*)(uint16_t, const void*, const void*, uint32_t*);
    using Register = int32_t SYSV (*)(uint16_t, void*, void*);
    using Status = int32_t SYSV (*)(uint16_t, uint64_t, uint16_t, int32_t*, uint32_t*, uint16_t*);

    // Bring-up: context, start (lobby hello), world list.
    CHECK(Fn<Init>("sceNpMatching2Initialize")(nullptr) == 0);
    uint16_t ctx = 0;
    CHECK(Fn<CreateContext>("sceNpMatching2CreateContextA")(nullptr, &ctx) == 0 && ctx == 1);
    Fn<RegisterCtx>("sceNpMatching2RegisterContextCallback")(reinterpret_cast<void*>(&OnContext), nullptr);
    CHECK(Fn<Start>("sceNpMatching2ContextStart")(ctx, 0) == 0);
    CHECK(PumpUntil([] { return !g_seen.context.empty(); }));
    CHECK(!g_seen.context.empty() && g_seen.context[0].first == 0x6f02 && g_seen.context[0].second == 0);
    void* opt[2] = {reinterpret_cast<void*>(&OnRequest), nullptr};
    uint32_t worlds = 0;
    uint8_t world_request[16] = {};
    CHECK(Fn<Request>("sceNpMatching2GetWorldInfoList")(ctx, world_request, opt, &worlds) == 0);
    CHECK(PumpUntil([&] { return RequestById(worlds); }));
    CHECK(RequestById(worlds) && RequestById(worlds)->error == 0 && RequestById(worlds)->count == 1);

    // CreateJoinRoomA as the game builds it: 4 slots, flags, world, the 0x78-byte room info, eight search attributes.
    std::vector<uint8_t> info(0x78);
    for (size_t i = 0; i < info.size(); ++i) info[i] = uint8_t(i);
    uint8_t bin_internal[0x18] = {}, bin_external[0x18] = {}, ints[8 * 8] = {};
    Wr<uint16_t>(bin_internal, 0, 0x57);
    Wr<const uint8_t*>(bin_internal, 8, info.data());
    Wr<uint64_t>(bin_internal, 0x10, info.size());
    Wr<uint16_t>(bin_external, 0, 0x55);
    Wr<const uint8_t*>(bin_external, 8, info.data());
    Wr<uint64_t>(bin_external, 0x10, info.size());
    for (int i = 0; i < 8; ++i) Wr<uint16_t>(ints, size_t(i * 8), uint16_t(0x4c + i)), Wr<uint32_t>(ints, size_t(i * 8 + 4), uint32_t(i));
    Wr<uint32_t>(ints, 7 * 8 + 4, 0x81102);
    uint8_t create[0xc0] = {};
    Wr<uint16_t>(create, 0, 4);
    Wr<uint32_t>(create, 8, 0x86000000u);
    Wr<uint32_t>(create, 0xc, 1);
    Wr<uint8_t*>(create, 0x60, bin_internal);
    Wr<uint64_t>(create, 0x68, 1);
    Wr<uint8_t*>(create, 0x70, ints);
    Wr<uint64_t>(create, 0x78, 8);
    Wr<uint8_t*>(create, 0x90, bin_external);
    Wr<uint64_t>(create, 0x98, 1);
    uint32_t created = 0;
    CHECK(Fn<Request>("sceNpMatching2CreateJoinRoomA")(ctx, create, opt, &created) == 0);
    CHECK(PumpUntil([&] { return RequestById(created); }));
    const Seen::Request* room = RequestById(created);
    CHECK(room && room->error == 0 && room->room >= 4096 && room->slots == 4 && room->me == 0x10 && room->first_member == 0x10);
    CHECK(room && room->copy == info);
    if (!room || room->error) {
        std::printf("FAILED (%d failures)\n", g_failures);
        return 1;
    }
    const uint64_t room_id = room->room;
    Fn<Register>("sceNpMatching2RegisterRoomEventCallback")(ctx, reinterpret_cast<void*>(&OnRoom), nullptr);
    Fn<Register>("sceNpMatching2RegisterSignalingCallback")(ctx, reinterpret_cast<void*>(&OnSignal), nullptr);

    // RudpMgr_OpenSocket: "RUDP_NetId", AF_INET, type 6, bound to the game's port (0 here: any).
    using Socket = int32_t SYSV (*)(const char*, int32_t, int32_t, int32_t);
    using Bind = int32_t SYSV (*)(int32_t, const uint8_t*, uint32_t);
    const int32_t sock = Fn<Socket>("sceNetSocket")("RUDP_NetId", 2, 6, 0);
    CHECK(sock > 0);
    uint8_t local[16] = {0x10, 2};
    Wr<uint16_t>(local, 8, uint16_t(0x2912)); // vport 4649, network order
    CHECK(Fn<Bind>("sceNetBind")(sock, local, 16) == 0);
    CHECK(Fn<Socket>("sceNetSocket")("other", 2, 1, 0) < 0);

    // The second player: the lobby protocol and its own peer socket.
    net::PeerSocket::Options options;
    std::string error;
    auto guest_socket = net::PeerSocket::open(options, &error);
    CHECK(guest_socket != nullptr);
    if (!guest_socket) return 1;
    const net::Identity guest_id = net::load_identity(base / "guest");
    net::LobbyLink::Options link_options;
    link_options.url = url;
    link_options.hello = [&] { return net::hello_body(guest_id, "GuestB", "HLE-TEST", true); };
    net::LobbyLink guest(link_options);
    guest.start();
    std::vector<net::LobbyMessage> guest_inbox;
    auto guest_next = [&](const std::function<bool(const net::LobbyMessage&)>& match) -> std::optional<net::LobbyMessage> {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (std::chrono::steady_clock::now() < deadline) {
            online::pump();
            net::LobbyMessage m;
            while (guest.poll(m)) guest_inbox.push_back(std::move(m));
            for (size_t i = 0; i < guest_inbox.size(); ++i) {
                if (match(guest_inbox[i])) {
                    net::LobbyMessage found = std::move(guest_inbox[i]);
                    guest_inbox.erase(guest_inbox.begin() + ptrdiff_t(i));
                    return found;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return std::nullopt;
    };
    CHECK(guest_next([](const net::LobbyMessage& m) { return m.kind == net::LobbyMessage::Kind::kUp; }));
    net::Json udp = net::Json::Object();
    udp.set("port", int64_t(guest_socket->port()));
    guest.request("udp", udp);
    net::Json search = net::Json::Object();
    search.set("world", 1);
    net::Json filter = net::Json::Array();
    filter.push(0x53);
    filter.push(1);
    filter.push(int64_t(0x81102));
    net::Json filters = net::Json::Array();
    filters.push(filter);
    search.set("filters", filters);
    const uint32_t search_seq = guest.request("room.search", search);
    auto found = guest_next([&](const net::LobbyMessage& m) { return m.kind == net::LobbyMessage::Kind::kResponse && m.seq == search_seq; });
    CHECK(found && found->body["total"].as_int() == 1 && found->body["rooms"].items()[0]["id"].as_u64() == room_id);
    net::Json join = net::Json::Object();
    join.set("room", net::u64_text(room_id));
    const uint32_t join_seq = guest.request("room.join", join);
    auto joined = guest_next([&](const net::LobbyMessage& m) { return m.kind == net::LobbyMessage::Kind::kResponse && m.seq == join_seq; });
    CHECK(joined && joined->code == "ok" && joined->body["me"].as_int() == 0x20);
    if (!joined) return 1;

    // The host sees the member join, reports its endpoint, and introduces the peer to the game.
    CHECK(PumpUntil([] { return !g_seen.rooms.empty(); }));
    CHECK(!g_seen.rooms.empty() && g_seen.rooms[0].event == 0x8101 && g_seen.rooms[0].member == 0x20 && g_seen.rooms[0].name == "GuestB");
    auto peer = guest_next([](const net::LobbyMessage& m) { return m.kind == net::LobbyMessage::Kind::kEvent && m.event == "peer"; });
    CHECK(peer && peer->body["member"].as_int() == 0x10 && peer->body["port"].as_int() > 0);
    CHECK(PumpUntil([] { return !g_seen.signals.empty(); }));
    CHECK(!g_seen.signals.empty() && g_seen.signals[0] == std::make_pair(uint16_t(0x20), uint16_t(0x5102)));
    int32_t status = 0;
    uint32_t peer_ip = 0;
    uint16_t peer_port = 0;
    CHECK(Fn<Status>("sceNpMatching2SignalingGetConnectionStatus")(ctx, room_id, 0x20, &status, &peer_ip, &peer_port) == 0);
    CHECK(status == 2 && __builtin_bswap32(peer_ip) == net::parse_ip("127.0.0.1") &&
          uint16_t(peer_port >> 8 | peer_port << 8) == guest_socket->port());

    // The guest's link to the host.
    net::RoomKey key{};
    std::vector<uint8_t> key_bytes;
    net::hex_decode(joined->body["room"]["key"].as_string(), key_bytes);
    CHECK(key_bytes.size() == key.size());
    std::memcpy(key.data(), key_bytes.data(), std::min(key.size(), key_bytes.size()));
    guest_socket->set_room(key, guest_id.account);
    std::vector<net::Endpoint> host_candidates = {{net::parse_ip(peer->body["ip"].as_string()), uint16_t(peer->body["port"].as_int())}};
    for (const net::Json& c : peer->body["lan"].items())
        host_candidates.push_back({net::parse_ip(c["ip"].as_string()), uint16_t(c["port"].as_int())});
    guest_socket->set_peer(g_settings.identity.account, host_candidates);
    const int guest_link = guest_socket->connect(g_settings.identity.account);

    // RudpMgr: context, bind (vport 20, mux 1), initiate to the signaled address, poll until writable.
    using RudpInit = int32_t SYSV (*)(void*, int32_t);
    using RudpCreate = int32_t SYSV (*)(void*, void*, int32_t*);
    using RudpBind = int32_t SYSV (*)(int32_t, int32_t, uint16_t, uint8_t);
    using RudpInitiate = int32_t SYSV (*)(int32_t, const uint8_t*, uint32_t, int32_t);
    using RudpPollCreate = int32_t SYSV (*)(uint64_t);
    using RudpPollControl = int32_t SYSV (*)(int32_t, int32_t, int32_t, uint16_t);
    using RudpPollWait = int32_t SYSV (*)(int32_t, void*, uint64_t, uint64_t);
    using RudpWrite = int32_t SYSV (*)(int32_t, const void*, uint64_t, uint8_t);
    using RudpRead = int32_t SYSV (*)(int32_t, void*, uint64_t, uint8_t, void*);
    using RudpOption = int32_t SYSV (*)(int32_t, int32_t, void*, uint32_t);
    CHECK(Fn<RudpInit>("sceRudpInit")(nullptr, 0x100000) == 0);
    int32_t rudp_ctx = 0;
    CHECK(Fn<RudpCreate>("sceRudpCreateContext")(nullptr, nullptr, &rudp_ctx) == 0 && rudp_ctx > 0);
    CHECK(Fn<RudpBind>("sceRudpBind")(rudp_ctx, sock, 20, 1) == 0);
    const int32_t poll = Fn<RudpPollCreate>("sceRudpPollCreate")(0x80);
    CHECK(Fn<RudpPollControl>("sceRudpPollControl")(poll, 1, rudp_ctx, 0xb) == 0);
    uint8_t remote[16] = {0x10, 2};
    Wr<uint16_t>(remote, 2, peer_port);
    Wr<uint32_t>(remote, 4, peer_ip);
    Wr<uint16_t>(remote, 8, uint16_t(0x2912));
    CHECK(Fn<RudpInitiate>("sceRudpInitiate")(rudp_ctx, remote, 16, 0) == 0);
    struct Event {
        int32_t ctx;
        uint16_t requested, returned;
    } events[8];
    uint16_t seen_events = 0;
    CHECK(PumpUntil([&] {
        const int32_t n = Fn<RudpPollWait>("sceRudpPollWait")(poll, events, 8, 0);
        for (int32_t i = 0; i < n; ++i) seen_events |= events[i].returned;
        return (seen_events & 2) != 0;
    }));
    uint32_t option = 1;
    CHECK(Fn<RudpOption>("sceRudpGetOption")(rudp_ctx, 0xe, &option, 4) == 0 && option == 0);
    CHECK(guest_socket->state(guest_link) == net::LinkState::kConnected);

    // Game messages both ways, whole and in order.
    const std::string hello_from_host(173, 'h');
    CHECK(Fn<RudpWrite>("sceRudpWrite")(rudp_ctx, hello_from_host.data(), hello_from_host.size(), 1) == int32_t(hello_from_host.size()));
    std::vector<uint8_t> got;
    CHECK(PumpUntil([&] { return guest_socket->receive(guest_link, got); }));
    CHECK(std::string(got.begin(), got.end()) == hello_from_host);
    const std::vector<uint8_t> big(0x30000, 0x5a);
    CHECK(guest_socket->send(guest_link, big.data(), big.size()));
    std::vector<uint8_t> buffer(0x40000);
    int32_t read = -1;
    CHECK(PumpUntil([&] {
        Fn<RudpPollWait>("sceRudpPollWait")(poll, events, 8, 0);
        read = Fn<RudpRead>("sceRudpRead")(rudp_ctx, buffer.data(), buffer.size(), 1, nullptr);
        return read > 0;
    }));
    CHECK(read == int32_t(big.size()) && std::memcmp(buffer.data(), big.data(), big.size()) == 0);
    CHECK(Fn<RudpRead>("sceRudpRead")(rudp_ctx, buffer.data(), buffer.size(), 1, nullptr) == int32_t(0x80770016));

    // Player cards: this PC and every player the lobby reported show a platform and port version, never a PlayStation.
    const std::string here = net::system_type_text(net::host_platform(), net::port_version());
    CHECK(online::system_type("HostA") == here);
    CHECK(online::system_type("GuestB") == here);
    CHECK(online::system_type("Stranger") == "PC");

    // The guest leaves: the host's game hears it (signaling dead, member left); then the host goes offline.
    net::Json leave = net::Json::Object();
    leave.set("room", net::u64_text(room_id));
    guest.request("room.leave", leave);
    CHECK(PumpUntil([] {
        for (const auto& r : g_seen.rooms)
            if (r.event == 0x8102 && r.member == 0x20) return true;
        return false;
    }));
    CHECK(std::find(g_seen.signals.begin(), g_seen.signals.end(), std::make_pair(uint16_t(0x20), uint16_t(0x5101))) != g_seen.signals.end());

    // Two friends host rooms: OldC sends no online protocol (a build from before it), NewD the same protocol with
    // another port version on Linux. Only NewD's room reaches the game, by friends or by search; joining OldC's is
    // refused by the lobby before any endpoint is exchanged.
    net::LobbySession self;
    net::Json self_reply;
    std::string error_text;
    CHECK(self.open(url, net::hello_body(g_settings.identity, "HostA", "HLE-TEST", false), &self_reply, &error_text));
    auto host_friend = [&](net::LobbySession& session, const char* dir, const char* name, bool old) -> uint64_t {
        const net::Identity id = net::load_identity(base / dir);
        const net::Json current = net::hello_body(id, name, "HLE-TEST", true);
        net::Json hello = net::Json::Object();
        for (const auto& [key, value] : current.members())
            if (!old || (key != "online_protocol" && key != "version" && key != "platform")) hello.set(key, value);
        if (!old) hello.set("version", "9.9.9"), hello.set("platform", "Linux");
        std::string code;
        const bool opened = session.open(url, hello, nullptr, &error_text);
        if (!opened) std::printf("%s: %s\n", name, error_text.c_str());
        CHECK(opened);
        net::Json add = net::Json::Object();
        add.set("code", self_reply["friend_code"].as_string());
        CHECK(session.call("friends.add", add, &code) && code == "ok");
        net::Json accept = net::Json::Object();
        accept.set("account", net::u64_text(id.account));
        CHECK(self.call("friends.accept", accept, &code) && code == "ok");
        net::Json create = net::Json::Object();
        create.set("world", 1);
        create.set("slots", 4);
        auto made = session.call("room.create", create, &code);
        net::Json presence = net::Json::Object();
        presence.set("data", net::hex_encode(info.data(), info.size()));
        CHECK(session.call("presence.set", presence, &code) && code == "ok");
        return made ? (*made)["room"]["id"].as_u64() : 0;
    };
    net::LobbySession old_session, new_session;
    const uint64_t old_room = host_friend(old_session, "old", "OldC", true);
    const uint64_t new_room = host_friend(new_session, "new", "NewD", false);
    CHECK(old_room && new_room);
    online::take_notice();
    online::friends_fetch();
    CHECK(PumpUntil([] { return !online::friends_busy(); }));
    const std::vector<online::FriendRoom> friend_rooms = online::friends_result(nullptr);
    CHECK(friend_rooms.size() == 1 && friend_rooms[0].name == "NewD");
    CHECK(online::take_notice() == "This player is using an incompatible version of Dragon's Crown Pro PC.");
    CHECK(online::system_type("NewD") == net::system_type_text("Linux", "9.9.9"));
    online::friends_fetch();
    CHECK(PumpUntil([] { return !online::friends_busy(); }));
    CHECK(online::take_notice().empty());

    uint8_t leave_request[16] = {};
    Wr<uint64_t>(leave_request, 0, room_id);
    uint32_t left = 0;
    CHECK(Fn<Request>("sceNpMatching2LeaveRoom")(ctx, leave_request, opt, &left) == 0);
    CHECK(PumpUntil([&] { return RequestById(left); }));
    uint8_t search_request[0x30] = {};
    Wr<uint32_t>(search_request, 4, 1);
    Wr<uint32_t>(search_request, 0x10, 1);
    Wr<uint32_t>(search_request, 0x14, 20);
    uint32_t searched = 0;
    CHECK(Fn<Request>("sceNpMatching2SearchRoom")(ctx, search_request, opt, &searched) == 0);
    CHECK(PumpUntil([&] { return RequestById(searched); }));
    CHECK(RequestById(searched) && RequestById(searched)->count == 1 && RequestById(searched)->room == new_room);
    const size_t signals_before = g_seen.signals.size(), rooms_before = g_seen.rooms.size();
    uint8_t join_request[16] = {};
    Wr<uint64_t>(join_request, 0, old_room);
    uint32_t refused = 0;
    CHECK(Fn<Request>("sceNpMatching2JoinRoomA")(ctx, join_request, opt, &refused) == 0);
    CHECK(PumpUntil([&] { return RequestById(refused); }));
    CHECK(RequestById(refused) && RequestById(refused)->error == int32_t(0x80550ca2));
    CHECK(online::take_notice() == "This player is using an incompatible version of Dragon's Crown Pro PC.");
    PumpUntil([] { return false; }, 300);
    CHECK(g_seen.signals.size() == signals_before && g_seen.rooms.size() == rooms_before);
    old_session.close();
    new_session.close();
    self.close();
    using Close = int32_t SYSV (*)(int32_t);
    CHECK(Fn<Close>("sceRudpTerminate")(rudp_ctx) == 0);
    CHECK(Fn<Close>("sceNetSocketClose")(sock) == 0);
    CHECK(Fn<Stop>("sceNpMatching2ContextStop")(ctx) == 0);
    CHECK(PumpUntil([] { return g_seen.context.size() >= 2; }));
    CHECK(g_seen.context.size() >= 2 && g_seen.context.back().first == 0x6f03);
    guest.stop();
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
