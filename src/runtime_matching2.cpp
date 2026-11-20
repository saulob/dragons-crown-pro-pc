// libSceNpMatching2 over the lobby (docs/ONLINE.md). The layouts below are the ones the game's DcNetMatching reads and
// writes (requests it builds, responses and events its handlers parse); offsets are bytes.
//   request callback (u16 ctx, u32 request, u16 event, i32 error, void* data, void* arg); opt = {callback @0, arg @8}
//   context callback (u16 ctx, u16 event, u8 cause, i32 error, void* arg): 0x6f01 start over, 0x6f02 started, 0x6f03 stopped
//   room callback (u16 ctx, u64 room, u16 event, void* data, void* arg)
//   signaling callback (u16 ctx, u64 room, u16 member, u16 event, i32 error, void* arg): 0x5101 dead, 0x5102 established
// The game takes its own lock in every callback and holds it while calling Matching2, so callbacks are only ever made
// from pump() (sceNpCheckCallback, once per frame) and never with our lock held.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "net/json.h"
#include "net/lobby_link.h"
#include "net/online_version.h"
#include "appearance_session.h"
#include "boss_battle_online.h"
#include "dcp_party.h"
#include "early_story_session.h"
#include "net/relay_ends.h"
#include "net/relay_frame.h"
#include "runtime_online.h"

#define DCP_SYSV __attribute__((sysv_abi))

namespace dcp::online {
namespace {

using net::Json;

using RequestCallback = void(DCP_SYSV*)(uint16_t, uint32_t, uint16_t, int32_t, const void*, void*);
using ContextCallback = void(DCP_SYSV*)(uint16_t, uint16_t, uint8_t, int32_t, void*);
using RoomCallback = void(DCP_SYSV*)(uint16_t, uint64_t, uint16_t, const void*, void*);
using SignalingCallback = void(DCP_SYSV*)(uint16_t, uint64_t, uint16_t, uint16_t, int32_t, void*);

constexpr uint16_t kContextId = 1, kServerId = 1;
enum : uint16_t { kEvWorlds = 0x0002, kEvSetExternal = 0x0004, kEvSetUserInfo = 0x0007, kEvCreateJoin = 0x0101, kEvJoin = 0x0102,
                  kEvLeave = 0x0103, kEvSearch = 0x0106, kEvSetInternal = 0x0109, kEvKick = 0x0105 };
enum : uint16_t { kCtxStartOver = 0x6f01, kCtxStarted = 0x6f02, kCtxStopped = 0x6f03 };
enum : uint16_t { kRoomKicked = 0x1103, kRoomDestroyed = 0x1104, kRoomOwner = 0x1105, kRoomInternal = 0x1106,
                  kRoomMemberJoined = 0x8101, kRoomMemberLeft = 0x8102 };
enum : uint16_t { kSignalDead = 0x5101, kSignalEstablished = 0x5102 };

// The three server errors the game tells apart (join: no such room, closed, full); the rest are ours.
constexpr int32_t kErrNoRoom = int32_t(0x80550d0f), kErrClosed = int32_t(0x80550d15), kErrFull = int32_t(0x80550d19);
constexpr int32_t kErrLobbyDown = int32_t(0x80550ca1), kErrRefused = int32_t(0x80550ca2), kErrNotOwner = int32_t(0x80550ca5);
constexpr int32_t kErrInRoom = int32_t(0x80550ca6), kErrIdentity = int32_t(0x80550ca7), kErrLimit = int32_t(0x80550caa);
constexpr int32_t kErrInvalid = int32_t(0x80550cb1), kErrNotStarted = int32_t(0x80550cb2), kErrUnsupported = int32_t(0x80550cb3);

constexpr uint64_t kEndpointWaitMs = 5000;   // STUN and UPnP get this long after the bind before we report anyway
constexpr uint64_t kEstablishWaitMs = 8000;  // a peer is introduced to the game even if our report is not confirmed
constexpr uint64_t kSettleMs = 500;          // a peer's endpoint must be stable this long

template <class T> T Rd(const void* p, size_t offset) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t*>(p) + offset, sizeof(v));
    return v;
}
template <class T> void Wr(void* p, size_t offset, T v) { std::memcpy(static_cast<uint8_t*>(p) + offset, &v, sizeof(v)); }

int32_t ErrorOf(const std::string& code) {
    if (code == "ok") return 0;
    if (code == "not_found") return kErrNoRoom;
    if (code == "closed") return kErrClosed;
    if (code == "full") return kErrFull;
    if (code == "not_owner") return kErrNotOwner;
    if (code == "in_room") return kErrInRoom;
    if (code == "unauthorized") return kErrIdentity;
    if (code == "limit") return kErrLimit;
    if (code == net::kLinkLost || code == net::kUnreachable) return kErrLobbyDown;
    return kErrRefused;
}

// Zeroed blocks the game reads during one callback.
struct Block {
    std::vector<std::unique_ptr<uint8_t[]>> parts;
    uint8_t* alloc(size_t size) {
        parts.emplace_back(new uint8_t[size + 16]());
        return parts.back().get();
    }
    uint8_t* copy(const std::vector<uint8_t>& bytes) {
        uint8_t* p = alloc(bytes.size());
        if (!bytes.empty()) std::memcpy(p, bytes.data(), bytes.size());
        return p;
    }
};

struct Member {
    uint16_t id = 0;
    uint64_t account = 0;
    std::string name;
    int64_t joined_ms = 0;
};

struct Client {
    std::string platform, version;
};

struct Signal {
    uint64_t account = 0;
    net::Endpoint address;
    std::vector<net::Endpoint> lan;
    uint64_t changed_ms = 0;
    bool delivered = false;
};

enum class Kind { kGame, kInternal, kResume, kUdp, kPresence, kFriends, kBossRooms, kSearch, kCreateJoin, kWorlds, kStory, kBossBattle, kCommsReport,
                  kAppearance };

struct Pending {
    Kind kind = Kind::kInternal;
    uint32_t request = 0;
    RequestCallback callback = nullptr;
    void* arg = nullptr;
    uint16_t event = 0;
    int story_step = EARLY_STORY_NO_STEP;
    uint32_t boss_version = 0;
};

struct State {
    bool initialized = false, context = false, start_wanted = false;
    ContextCallback context_cb = nullptr;
    void* context_arg = nullptr;
    RoomCallback room_cb = nullptr;
    void* room_arg = nullptr;
    SignalingCallback signal_cb = nullptr;
    void* signal_arg = nullptr;
    RequestCallback default_cb = nullptr;
    void* default_arg = nullptr;
    std::unique_ptr<net::LobbyLink> link;
    bool up = false, lost = false, down = false;
    uint32_t your_ip = 0, public_ip = 0;
    bool lobby_on_lan = false;
    uint32_t next_request = 1;
    std::map<uint32_t, Pending> pending;
    std::vector<std::function<void()>> calls;
    // the room we are in
    uint64_t room = 0;
    uint16_t me = 0, owner = 0;
    net::RoomKey key{};
    std::map<uint16_t, Member> members;
    std::map<uint16_t, Signal> signals;
    // our endpoint as reported to the lobby
    std::shared_ptr<net::PeerSocket> socket;
    bool relay_capable = false;
    bool story_capable = false, story_room_hosted = false;
    // the live story step the room has from us (EARLY_STORY_NO_STEP: none, or the last one was refused)
    int story_sent = EARLY_STORY_NO_STEP;
    // we inherited a story room: our external updates clear its story attribute
    bool clear_story_attr = false;
    net::SessionWatch story_watch;
    // Boss Battle: the lobby keeps its rooms to capable clients; the tag our room carries and the host state it has
    bool boss_capable = false;
    int boss_attr = BOSS_BATTLE_ROOM_NONE;
    uint32_t boss_published = 0;
    uint64_t bound_ms = 0;
    uint16_t reported_port = 0;
    uint32_t reported_ip = 0;
    bool udp_inflight = false, udp_acked = false;
    std::string last_external;
    // presence and friends
    std::string presence_data, presence_status, presence_sent = "-";
    bool friends_busy = false;
    uint64_t friends_generation = 0;
    std::vector<FriendRoom> friends;
    std::vector<std::pair<uint64_t, std::string>> friend_room_owners;
    std::set<std::string> boss_friend_names;
    // what the Online page shows: the account's friend code and the friend list as the lobby sent it
    std::string friend_code;
    std::vector<OnlinePlayer> friend_list;
    bool friend_list_known = false;
    // what the game's player cards show: each player's platform and port version, by online id
    std::map<std::string, Client> clients;
    std::set<std::string> refused_friend_rooms;
    std::string notice;
    party::Reporter party;
    int comms_result = 0; // 1 the lobby accepted the last comms.report, 2 it refused it
    std::string comms_code;
};

std::mutex g_mutex;
State g;
void (*g_pump_hook)() = nullptr;

uint64_t RtcTick(int64_t unix_ms) { return uint64_t(unix_ms) * 1000 + UINT64_C(62135596800000000); }

std::string SafeName(const std::string& name) { return net::online_name(name, "Player"); }

void Remember(const std::string& name, const Json& client) {
    if (!client["platform"].as_string().empty())
        g.clients[SafeName(name)] = Client{client["platform"].as_string(), client["version"].as_string()};
}

bool Compatible(const Json& remote) { return net::online_compatible(int(remote["online_protocol"].as_int())); }

int64_t IntAttrOf(const Json& ints, int64_t id, int64_t fallback) {
    for (const Json& pair : ints.items())
        if (pair.items().size() == 2 && pair.items()[0].as_int() == id) return pair.items()[1].as_int();
    return fallback;
}

void AddIntAttr(Json& ints, int id, int value) {
    Json pair = Json::Array();
    pair.push(int64_t(id));
    pair.push(int64_t(value));
    ints.push(std::move(pair));
}

void AddStoryAttr(Json& ints, int value) { AddIntAttr(ints, EARLY_STORY_ROOM_ATTR, value); }

// One support line and one notice for the player per refused room; `who` names the other side in the notice.
void Refuse(const char* who, const std::string& about, const Json& remote) {
    const std::string& version = remote["version"].as_string();
    std::printf("Online: incompatible version local=v%s remote=%s protocol local=%d remote=%lld (%s); not joining\n",
                net::port_version().c_str(), version.empty() ? "unknown" : ("v" + version).c_str(), net::kOnlineProtocolVersion,
                (long long)remote["online_protocol"].as_int(), about.c_str());
    g.notice = version.empty() ? "This player is using an incompatible version of Dragon's Crown Pro PC."
                               : "Online version mismatch\nYou: v" + net::port_version() + "\n" + who + ": v" + version;
}

void PutOnlineId(uint8_t* p, const std::string& name) { std::memcpy(p, name.data(), std::min<size_t>(name.size(), 16)); }

// ---- request bodies from the game's structs ----
Json BinAttrs(const uint8_t* array, uint64_t count) {
    Json out = Json::Array();
    for (uint64_t i = 0; array && i < count && i < 16; ++i) {
        const uint8_t* e = array + i * 0x18;
        const uint8_t* data = Rd<const uint8_t*>(e, 8);
        const uint64_t size = std::min<uint64_t>(Rd<uint64_t>(e, 0x10), 0x100);
        Json pair = Json::Array();
        pair.push(int64_t(Rd<uint16_t>(e, 0)));
        pair.push(data ? net::hex_encode(data, size_t(size)) : std::string());
        out.push(std::move(pair));
    }
    return out;
}

Json IntAttrs(const uint8_t* array, uint64_t count) {
    Json out = Json::Array();
    for (uint64_t i = 0; array && i < count && i < 32; ++i) {
        Json pair = Json::Array();
        pair.push(int64_t(Rd<uint16_t>(array, size_t(i * 8))));
        pair.push(int64_t(Rd<uint32_t>(array, size_t(i * 8 + 4))));
        out.push(std::move(pair));
    }
    return out;
}

std::vector<uint8_t> Unhex(const Json& j) {
    std::vector<uint8_t> v;
    net::hex_decode(j.as_string(), v);
    return v;
}

std::vector<uint8_t> Unhex(const Json& j);

// The private states in a room's internal attributes, which the game never sees.
struct PrivateStates {
    bool story = false, boss = false;
    int step = EARLY_STORY_NO_STEP;
    std::vector<uint8_t> boss_state;
};

// Internal attributes as the game may see them: the private live story and Boss Battle states are taken out, the rest
// kept in order. The story step is EARLY_STORY_NO_STEP if unreadable.
Json WithoutPrivateStates(const Json& attrs, PrivateStates& states) {
    Json kept = Json::Array();
    states = PrivateStates{};
    for (const Json& pair : attrs.items()) {
        const int64_t id = pair.items().empty() ? 0 : pair.items()[0].as_int();
        if (id != EARLY_STORY_STATE_ATTR && id != BOSS_BATTLE_STATE_ATTR) {
            kept.push(pair);
            continue;
        }
        const std::vector<uint8_t> data = Unhex(pair.items().size() > 1 ? pair.items()[1] : Json());
        if (id == BOSS_BATTLE_STATE_ATTR) {
            states.boss = true;
            states.boss_state = data;
            continue;
        }
        states.story = true;
        states.step = early_story_state_decode(data.data(), data.size());
    }
    return kept;
}

// SetRoomDataInternal as the lobby takes it.
Json InternalBody(uint64_t room, uint32_t flag_mask, uint32_t flag_value, Json bin_int) {
    Json body = Json::Object();
    body.set("room", net::u64_text(room));
    body.set("flag_mask", int64_t(flag_mask));
    body.set("flag_value", int64_t(flag_value));
    body.set("bin_int", std::move(bin_int));
    return body;
}

// ---- response structs for the game ----
uint8_t* MemberStruct(Block& b, const Member& m) {
    uint8_t* p = b.alloc(0x60);
    Wr<uint64_t>(p, 8, RtcTick(m.joined_ms));
    Wr<uint64_t>(p, 0x10, m.account);
    PutOnlineId(p + 0x20, m.name);
    Wr<uint16_t>(p, 0x38, m.id);
    return p;
}

Member MemberOf(const Json& j) {
    Member m;
    m.id = uint16_t(j["id"].as_int());
    m.account = j["account"].as_u64();
    m.name = SafeName(j["name"].as_string());
    m.joined_ms = j["joined"].as_int();
    return m;
}

// BinAttrInternal: update date @0, member @8, BinAttr {id @0x10, data @0x18, size @0x20}.
uint8_t* InternalAttrs(Block& b, const Json& attrs, uint32_t& count) {
    count = uint32_t(attrs.size());
    if (!count) return nullptr;
    uint8_t* array = b.alloc(0x28 * count);
    for (uint32_t i = 0; i < count; ++i) {
        const Json& pair = attrs.items()[i];
        const std::vector<uint8_t> data = Unhex(pair.items().size() > 1 ? pair.items()[1] : Json());
        uint8_t* e = array + i * 0x28;
        Wr<uint16_t>(e, 0x10, uint16_t(pair.items().empty() ? 0 : pair.items()[0].as_int()));
        Wr<uint8_t*>(e, 0x18, b.copy(data));
        Wr<uint64_t>(e, 0x20, data.size());
    }
    return array;
}

// BinAttr {id @0, data @8, size @0x10}; `first` goes to the front (the game reads the first only).
uint8_t* PlainAttrs(Block& b, const Json& attrs, int first, uint32_t& count) {
    std::vector<const Json*> order;
    for (const Json& pair : attrs.items()) {
        if (pair.items().size() != 2) continue;
        if (pair.items()[0].as_int() == first) order.insert(order.begin(), &pair);
        else order.push_back(&pair);
    }
    count = uint32_t(order.size());
    if (!count) return nullptr;
    uint8_t* array = b.alloc(0x18 * count);
    for (uint32_t i = 0; i < count; ++i) {
        const std::vector<uint8_t> data = Unhex(order[i]->items()[1]);
        Wr<uint16_t>(array + i * 0x18, 0, uint16_t(order[i]->items()[0].as_int()));
        Wr<uint8_t*>(array + i * 0x18, 8, b.copy(data));
        Wr<uint64_t>(array + i * 0x18, 0x10, data.size());
    }
    return array;
}

net::RoomKey KeyOf(const std::string& hex) {
    net::RoomKey key{};
    std::vector<uint8_t> bytes;
    if (net::hex_decode(hex, bytes) && bytes.size() == key.size()) std::memcpy(key.data(), bytes.data(), key.size());
    return key;
}

std::vector<net::Endpoint> Candidates(const Signal& s) {
    std::vector<net::Endpoint> list;
    const bool same_address = s.address.ip && s.address.ip == g.public_ip;
    if (same_address) list = s.lan;
    if (s.address.valid()) list.push_back(s.address);
    if (!same_address) list.insert(list.end(), s.lan.begin(), s.lan.end());
    return list;
}

net::Endpoint GameEndpoint(uint16_t member, const Signal& s) {
    return s.account ? net::signaling_endpoint(s.address, member) : s.address;
}

// The relay's two ends must not take g_mutex. While the lobby link reconnects, the seat is not ours again until
// room.resume answers: the lobby would count frames sent before that as faults of a sender with no room.
net::RelayEnds g_relay;

bool SendRelayFrame(std::vector<uint8_t>&& frame) { return g_relay.Send(std::move(frame)); }

void PauseRelay(bool paused) { g_relay.Pause(paused); }

void ReceiveRelayFrame(const uint8_t* data, size_t size) { g_relay.Deliver(data, size); }

void SetRelayEnds(net::LobbyLink* link, const std::shared_ptr<net::PeerSocket>& socket) {
    net::RelayEnds::Forward forward;
    if (link) forward = [link](std::vector<uint8_t>&& frame) { return link->send_binary(std::move(frame)); };
    g_relay.Set(std::move(forward), socket);
}

void ConfigureRelay() {
    if (!g.socket) return;
    if (g.relay_capable && g.room && g.link) {
        g.socket->enable_relay(net::relay_token(g.key), SendRelayFrame);
    } else {
        g.socket->disable_relay();
    }
}

void ApplyPeers() {
    if (!g.socket || !g.room) return;
    g.socket->set_room(g.key, settings().identity.account);
    for (const auto& [member, s] : g.signals)
        if (member != g.me && s.account) g.socket->set_peer(s.account, Candidates(s), member);
    ConfigureRelay();
}

void ClearRoom() {
    early_story_session_reset();
    appearance_session::Shared().Left();
    if (g.room) boss_battle_online_room_left("the room was left");
    g.boss_attr = BOSS_BATTLE_ROOM_NONE;
    g.boss_published = 0;
    g.story_room_hosted = g.clear_story_attr = false;
    g.story_sent = EARLY_STORY_NO_STEP;
    if (g.socket) {
        for (const auto& [member, s] : g.signals) g.socket->forget_peer(s.account);
        g.socket->disable_relay();
    }
    g.room = 0;
    g.me = g.owner = 0;
    g.members.clear();
    g.signals.clear();
    g.last_external.clear();
}

// The room from a create or join answer; also becomes our room. Caller holds g_mutex.
uint8_t* JoinResponse(Block& b, const Json& body, PrivateStates& states) {
    const Json& room = body["room"];
    g.room = room["id"].as_u64();
    g.me = uint16_t(body["me"].as_int());
    g.owner = uint16_t(room["owner"].as_int());
    g.key = KeyOf(room["key"].as_string());
    g.members.clear();
    g.signals.clear();
    g.last_external.clear();
    uint8_t* internal = b.alloc(0x80);
    Wr<uint16_t>(internal, 8, kServerId);
    Wr<uint16_t>(internal, 0xa, uint16_t(room["slots"].as_int(4)));
    Wr<uint32_t>(internal, 0xc, uint32_t(room["world"].as_int(1)));
    Wr<uint64_t>(internal, 0x18, g.room);
    Wr<uint32_t>(internal, 0x40, uint32_t(room["flags"].as_int()));
    uint32_t attrs = 0;
    Wr<uint8_t*>(internal, 0x48, InternalAttrs(b, WithoutPrivateStates(room["bin_int"], states), attrs));
    Wr<uint32_t>(internal, 0x50, attrs);
    uint8_t* response = b.alloc(0x30);
    Wr<uint8_t*>(response, 0, internal);
    uint8_t *head = nullptr, *previous = nullptr, *me = nullptr, *owner = nullptr;
    uint32_t count = 0;
    for (const Json& j : room["members"].items()) {
        const Member m = MemberOf(j);
        g.members[m.id] = m;
        Remember(m.name, j);
        uint8_t* p = MemberStruct(b, m);
        (previous ? Wr<uint8_t*>(previous, 0, p) : void(head = p));
        previous = p;
        if (m.id == g.me) me = p;
        if (m.id == g.owner) owner = p;
        ++count;
    }
    Wr<uint8_t*>(response, 8, head);
    Wr<uint32_t>(response, 0x10, count);
    Wr<uint8_t*>(response, 0x18, me);
    Wr<uint8_t*>(response, 0x20, owner);
    ApplyPeers();
    return response;
}

// Search results: RoomDataExternal list; the game reads the int attrs 0x4c..0x53 as eight values in order. Rooms of
// another online protocol never reach it; the lobby already leaves them out and counts them.
uint8_t* SearchResponse(Block& b, const Json& body) {
    uint8_t* response = b.alloc(0x20);
    uint8_t *head = nullptr, *previous = nullptr;
    int32_t n = 0;
    int64_t hidden = body["incompatible"].as_int();
    for (const Json& r : body["rooms"].items()) {
        if (!Compatible(r)) {
            ++hidden;
            continue;
        }
        Remember(r["owner_name"].as_string(), r);
        uint8_t* room = b.alloc(0xb0);
        Wr<uint16_t>(room, 8, uint16_t(r["slots"].as_int(4)));
        Wr<uint16_t>(room, 0xa, uint16_t(r["count"].as_int()));
        Wr<uint32_t>(room, 0xc, uint32_t(r["world"].as_int(1)));
        Wr<uint64_t>(room, 0x20, r["id"].as_u64());
        Wr<uint64_t>(room, 0x40, r["owner_account"].as_u64());
        PutOnlineId(room + 0x50, SafeName(r["owner_name"].as_string()));
        uint8_t* ints = b.alloc(8 * 8);
        for (int k = 0; k < 8; ++k) Wr<uint16_t>(ints, size_t(k * 8), uint16_t(0x4c + k));
        for (const Json& pair : r["ints"].items()) {
            const int64_t id = pair.items().empty() ? 0 : pair.items()[0].as_int();
            if (id >= 0x4c && id <= 0x53 && pair.items().size() == 2)
                Wr<uint32_t>(ints, size_t((id - 0x4c) * 8 + 4), uint32_t(pair.items()[1].as_int()));
        }
        Wr<uint8_t*>(room, 0x78, ints);
        Wr<uint64_t>(room, 0x80, 8);
        uint32_t attrs = 0;
        Wr<uint8_t*>(room, 0x98, PlainAttrs(b, r["bin_ext"], 0x55, attrs));
        Wr<uint64_t>(room, 0xa0, attrs);
        (previous ? Wr<uint8_t*>(previous, 0, room) : void(head = room));
        previous = room;
        ++n;
    }
    Wr<int32_t>(response, 8, n);
    Wr<uint8_t*>(response, 0x10, head);
    if (hidden)
        std::printf("Online: search left out %lld room(s) of an incompatible version (protocol local=%d)\n", (long long)hidden,
                    net::kOnlineProtocolVersion);
    return response;
}

// World list: {World* @0, i32 count @8}; World (0x28 bytes): world id @8, rooms @0x18, members @0x1c.
uint8_t* WorldsResponse(Block& b, const Json& body) {
    const Json& worlds = body["worlds"];
    uint8_t* response = b.alloc(0x10);
    uint8_t* array = b.alloc(0x28 * std::max<size_t>(worlds.size(), 1));
    for (size_t i = 0; i < worlds.size(); ++i) {
        Wr<uint32_t>(array + i * 0x28, 8, uint32_t(worlds.items()[i]["world"].as_int(1)));
        Wr<uint32_t>(array + i * 0x28, 0x18, uint32_t(worlds.items()[i]["rooms"].as_int()));
        Wr<uint32_t>(array + i * 0x28, 0x1c, uint32_t(worlds.items()[i]["members"].as_int()));
    }
    Wr<uint8_t*>(response, 0, array);
    Wr<int32_t>(response, 8, int32_t(worlds.size()));
    return response;
}

// ---- requests ----
bool LinkUsable() { return g.link && !g.down; }

void Complete(const Pending& p, int32_t error) {
    if (!p.callback) return;
    const RequestCallback cb = p.callback;
    void* arg = p.arg;
    const uint32_t request = p.request;
    const uint16_t event = p.event;
    g.calls.push_back([=] { cb(kContextId, request, event, error, nullptr, arg); });
}

Pending GamePending(Kind kind, const void* opt, uint16_t event) {
    Pending p;
    p.kind = kind;
    p.event = event;
    p.callback = opt ? Rd<RequestCallback>(opt, 0) : g.default_cb;
    p.arg = opt ? Rd<void*>(opt, 8) : g.default_arg;
    p.request = g.next_request++;
    return p;
}

// Sends a request for the game; answers it locally with "lobby unreachable" when there is no link.
uint32_t Issue(Kind kind, const std::string& op, Json body, const void* opt, uint16_t event, bool urgent = false) {
    Pending p = (opt || kind == Kind::kGame || kind == Kind::kSearch || kind == Kind::kCreateJoin || kind == Kind::kWorlds)
                    ? GamePending(kind, opt, event)
                    : Pending{kind, 0, nullptr, nullptr, event};
    if (!LinkUsable()) {
        Complete(p, kErrLobbyDown);
        return p.request;
    }
    const uint32_t seq = g.link->request(op, std::move(body), urgent);
    g.pending[seq] = p;
    return p.request;
}

uint32_t CompleteLocally(const void* opt, uint16_t event) {
    const Pending p = GamePending(Kind::kGame, opt, event);
    Complete(p, 0);
    return p.request;
}

void PushRoomEvent(uint16_t event, std::shared_ptr<Block> block, const uint8_t* data) {
    if (!g.room_cb) return;
    const RoomCallback cb = g.room_cb;
    void* arg = g.room_arg;
    const uint64_t room = g.room;
    g.calls.push_back([=] { cb(kContextId, room, event, data, arg); (void)block; });
}

void PushContextEvent(uint16_t event, int32_t error) {
    if (!g.context_cb) return;
    const ContextCallback cb = g.context_cb;
    void* arg = g.context_arg;
    g.calls.push_back([=] { cb(kContextId, event, 0, error, arg); });
}

void RoomGone(uint16_t event) {
    if (!g.room) return;
    auto block = std::make_shared<Block>();
    PushRoomEvent(event, block, block->alloc(0x10));
    ClearRoom();
}

Json HelloBody() {
    const Settings& s = settings();
    Json hello = net::hello_body(s.identity, s.name, s.game, true, true, s.config.lobby_visible);
    if (early_story_active()) hello.set("story_session", true);
    if (boss_battle_online_available()) hello.set("boss_battle_session", true);
    if (appearance_session_enabled()) hello.set("appearance", true);
    return hello;
}

// A joined room an Early Story Online or Boss Battle Online rule refuses: the seat is given back and the game is told
// the join failed.
bool RefuseJoin(const Json& room) {
    const Json& ints = room["ints"];
    const char* reason = "";
    const int boss_attr = int(IntAttrOf(ints, BOSS_BATTLE_ROOM_ATTR, BOSS_BATTLE_ROOM_NONE));
    const bool boss = boss_attr > 0 || boss_battle_online_search_wanted();
    const char* feature = boss ? "Boss Battle Online" : "Early Story Online";
    const bool allowed =
        boss ? boss_battle_online_join_allowed(boss_attr, &reason)
             : early_story_join_allowed(early_story_own_step(), int(IntAttrOf(ints, EARLY_STORY_ROOM_ATTR, 0)),
                                        int(IntAttrOf(ints, EARLY_STORY_STAGE_ATTR, EARLY_STORY_NO_STEP)),
                                        int(IntAttrOf(ints, EARLY_STORY_PACKED_ATTR, EARLY_STORY_NO_STEP)), &reason);
    if (allowed) return false;
    Json body = Json::Object();
    body.set("room", room["id"].as_string());
    Issue(Kind::kInternal, "room.leave", std::move(body), nullptr, 0);
    g.notice = std::string("Cannot join this room: ") + reason + ".";
    std::printf("%s: join refused, %s\n", feature, reason);
    return true;
}

// The owner of a Boss Battle room tells its members each new host state, in its own SetRoomDataInternal; a member gets
// it as room.internal. The host's start waits for the lobby's answer: by then the members have been sent it.
void PublishBossBattleState() {
    if (!g.room || g.me != g.owner || !LinkUsable() || g.boss_attr == BOSS_BATTLE_ROOM_NONE) return;
    const uint32_t version = boss_battle_online_publish_version();
    uint8_t state[BOSS_BATTLE_STATE_SIZE];
    if (version == g.boss_published || !boss_battle_online_publish(state)) return;
    Json attr = Json::Array();
    attr.push(int64_t(BOSS_BATTLE_STATE_ATTR));
    attr.push(net::hex_encode(state, sizeof(state)));
    Json attrs = Json::Array();
    attrs.push(std::move(attr));
    g.boss_published = version;
    Pending p;
    p.kind = Kind::kBossBattle;
    p.boss_version = version;
    g.pending[g.link->request("room.set_internal", InternalBody(g.room, 0, 0, std::move(attrs)))] = p;
}

// The room's Boss Battle tag follows the host: open, in a fight, or gone once the Boss Battle visit ended.
void PublishBossBattleTag() {
    if (!g.room || g.me != g.owner || !LinkUsable()) return;
    const int want = boss_battle_online_room_attr();
    if (want == g.boss_attr) return;
    g.boss_attr = want;
    boss_battle_online_room_tagged(want);
    Json ints = Json::Array();
    AddIntAttr(ints, BOSS_BATTLE_ROOM_ATTR, want);
    if (want != BOSS_BATTLE_ROOM_NONE && g.story_room_hosted) {
        AddStoryAttr(ints, 0);
        g.story_room_hosted = false;
    }
    Json body = Json::Object();
    body.set("room", net::u64_text(g.room));
    body.set("ints", std::move(ints));
    std::printf("Boss Battle Online: room tag %d, room %llu\n", want, (unsigned long long)g.room);
    g.pending[g.link->request("room.set_external", std::move(body))] = Pending{Kind::kInternal, 0, nullptr, nullptr, 0};
    g.last_external.clear();
}

// The owner of a story room it created sends its live step through SetRoomDataInternal when it changes; the members get
// it as room.internal. Its own request: the game's room updates are not tied to the story.
void PublishStoryState() {
    if (!g.story_room_hosted || !g.room || g.me != g.owner || !LinkUsable()) return;
    const int step = early_story_own_step();
    if (step < 0 || step == g.story_sent) return;
    uint8_t state[EARLY_STORY_STATE_SIZE];
    early_story_state_encode(step, state);
    Json attr = Json::Array();
    attr.push(int64_t(EARLY_STORY_STATE_ATTR));
    attr.push(net::hex_encode(state, sizeof(state)));
    Json attrs = Json::Array();
    attrs.push(std::move(attr));
    g.story_sent = step;
    Pending p;
    p.kind = Kind::kStory;
    p.story_step = step;
    g.pending[g.link->request("room.set_internal", InternalBody(g.room, 0, 0, std::move(attrs)))] = p;
}

// A Boss Battle room handed back to normal play becomes the story session a host of this step would have created: the
// room's story attribute and live step go out as they do for a room created so.
void ResumeStoryRoom() {
    if (!g.room || g.me != g.owner || g.story_room_hosted || !g.story_capable || !LinkUsable()) return;
    if (g.boss_attr != BOSS_BATTLE_ROOM_NONE || boss_battle_online_room_attr() != BOSS_BATTLE_ROOM_NONE) return;
    const int step = early_story_own_step();
    if (!early_story_session_resume_story_room(step)) return;
    g.story_room_hosted = true;
    g.clear_story_attr = false;
    g.story_sent = EARLY_STORY_NO_STEP;
    Json ints = Json::Array();
    AddStoryAttr(ints, early_story_room_attr_value(step));
    Json body = Json::Object();
    body.set("room", net::u64_text(g.room));
    body.set("ints", std::move(ints));
    std::printf("Early Story Online: the room is a story session again, step %d\n", step);
    g.pending[g.link->request("room.set_external", std::move(body))] = Pending{Kind::kInternal, 0, nullptr, nullptr, 0};
    g.last_external.clear();
    PublishStoryState();
}

void WatchStorySession(uint64_t now) {
    const EarlyStoryRole role = early_story_session_role();
    net::WatchSession session;
    session.role = role == EARLY_STORY_ROLE_HOST ? net::WatchRole::kHost
                 : role == EARLY_STORY_ROLE_GUEST ? net::WatchRole::kGuest
                                                  : net::WatchRole::kNone;
    session.story = g.room && (role == EARLY_STORY_ROLE_HOST ? g.story_room_hosted : early_story_story_room_guest());
    session.step = role == EARLY_STORY_ROLE_HOST ? early_story_own_step() : early_story_session_host_step();
    session.lobby_up = g.up && !g.lost && !g.down;
    session.pump_generation = g.socket ? g.socket->pump_generation() : 0;
    const std::vector<net::WatchLink> links = peer_links();
    for (const std::string& line : g.story_watch.Update(now, session, links)) std::printf("%s\n", line.c_str());
}

void OnOwnStoryStep(int) {
    std::lock_guard<std::mutex> lock(g_mutex);
    PublishStoryState();
}

// ---- lobby messages ----
void FinishFriends() {
    g.friends_busy = false;
    ++g.friends_generation;
}

// Which friends' rooms are Boss Battle rooms taking players: the lobby's own room search for them, one request per list.
void RequestFriendBossRooms() {
    if (g.friend_room_owners.empty() || !LinkUsable()) {
        g.boss_friend_names.clear();
        FinishFriends();
        return;
    }
    Json body = Json::Object();
    body.set("world", int64_t(0));
    body.set("boss_battle", true);
    const uint32_t seq = g.link->request("room.search", std::move(body));
    g.pending[seq] = Pending{Kind::kBossRooms, 0, nullptr, nullptr, 0};
}

int FriendInBossRoom(const char* online_id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g.boss_friend_names.count(online_id) ? 1 : 0;
}

void OnResponse(const net::LobbyMessage& m) {
    auto it = g.pending.find(m.seq);
    if (it == g.pending.end()) return;
    const Pending p = it->second;
    g.pending.erase(it);
    int32_t error = ErrorOf(m.code);
    switch (p.kind) {
    case Kind::kInternal: return;
    case Kind::kCommsReport:
        g.comms_result = error ? 2 : 1;
        g.comms_code = m.code;
        return;
    case Kind::kAppearance:
        appearance_session::Shared().PublishAnswered(m.code, m.body["retry_ms"].as_int(), net::now_ms());
        return;
    case Kind::kStory:
        if (error && g.story_sent == p.story_step) g.story_sent = EARLY_STORY_NO_STEP;
        return;
    case Kind::kBossBattle:
        boss_battle_online_published(p.boss_version, !error);
        if (error && g.boss_published == p.boss_version) g.boss_published = 0;
        return;
    case Kind::kUdp:
        g.udp_inflight = false;
        g.udp_acked = !error;
        std::printf("Matching2: endpoint %s (lobby has %s:%lld)\n", error ? "refused" : "reported",
                    net::lobby_endpoint_text(m.body["public_ip"].as_string(), m.body["ip"].as_string()).c_str(),
                    (long long)m.body["port"].as_int());
        if (error) g.reported_port = 0;
        return;
    case Kind::kPresence:
        if (error) std::printf("Matching2: presence not published (%s)\n", m.code.c_str());
        return;
    case Kind::kFriends: {
        g.friends.clear();
        g.friend_room_owners.clear();
        boss_battle_online_set_friend_room_lookup(&FriendInBossRoom);
        if (!error) {
            g.friend_list = ReadOnlinePlayers(m.body["friends"]);
            g.friend_list_known = true;
        }
        size_t total = 0;
        for (const Json& f : m.body["friends"].items()) {
            ++total;
            FriendRoom row;
            row.account = f["account"].as_u64();
            row.name = SafeName(f["name"].as_string());
            net::hex_decode(f["data"].as_string(), row.data);
            net::hex_decode(f["status"].as_string(), row.status);
            if (!f["online"].as_bool()) continue;
            Remember(row.name, f);
            if (!f["room"].as_u64() || row.data.empty() || f["game"].as_string() != settings().game) continue;
            if (!Compatible(f)) {
                if (g.refused_friend_rooms.insert(net::u64_text(row.account) + "/" + f["room"].as_string()).second)
                    Refuse("Friend", "friend " + row.name, f);
                continue;
            }
            g.friend_room_owners.emplace_back(row.account, row.name);
            g.friends.push_back(std::move(row));
        }
        std::printf("Matching2: friends: %zu, in a joinable room: %zu\n", total, g.friends.size());
        RequestFriendBossRooms();
        return;
    }
    case Kind::kBossRooms: {
        g.boss_friend_names.clear();
        if (!error) {
            for (const Json& room : m.body["rooms"].items())
                for (const auto& [account, name] : g.friend_room_owners)
                    if (account && room["owner_account"].as_u64() == account) g.boss_friend_names.insert(name);
        }
        std::printf("Matching2: friends in a Boss Battle room: %zu\n", g.boss_friend_names.size());
        FinishFriends();
        return;
    }
    case Kind::kResume:
        if (!error) {
            std::printf("Matching2: seat resumed in room %llu as member 0x%x\n", (unsigned long long)g.room, g.me);
            appearance_session::Shared().Entered(g.room, g.me, m.body, true);
        } else {
            std::printf("Matching2: seat lost after the reconnect (%s)\n", m.code.c_str());
            RoomGone(kRoomDestroyed);
        }
        PauseRelay(false);
        g.lost = false;
        g.reported_port = 0;
        g.udp_acked = false;
        g.presence_sent = "-";
        g.link->release_held();
        return;
    default: break;
    }
    if (!p.callback) return;
    auto block = std::make_shared<Block>();
    const void* data = nullptr;
    if (!error) {
        if (p.kind == Kind::kWorlds) data = WorldsResponse(*block, m.body);
        if (p.kind == Kind::kSearch) {
            data = SearchResponse(*block, m.body);
            std::printf("Matching2: search found %d room(s)\n", Rd<int32_t>(data, 8));
        }
        if (p.kind == Kind::kCreateJoin && p.event == kEvJoin && RefuseJoin(m.body["room"])) {
            error = kErrRefused;
        } else if (p.kind == Kind::kCreateJoin) {
            PrivateStates states;
            data = JoinResponse(*block, m.body, states);
            const Json& ints = m.body["room"]["ints"];
            g.boss_attr = int(IntAttrOf(ints, BOSS_BATTLE_ROOM_ATTR, BOSS_BATTLE_ROOM_NONE));
            // A Boss Battle room is no story session: each player keeps its own story.
            if (g.boss_attr > 0) early_story_session_room_handoff(g.me == g.owner, 1);
            else early_story_session_joined(g.me == g.owner, int(IntAttrOf(ints, EARLY_STORY_ROOM_ATTR, 0)));
            boss_battle_online_room_joined(g.me == g.owner, g.boss_attr, int(g.members.size()), party::MemberSlot(g.me));
            std::printf("Matching2: %s room %llu as member 0x%x (owner 0x%x, %zu member(s), code %s)\n",
                        p.event == kEvJoin ? "joined" : "created", (unsigned long long)g.room, g.me, g.owner, g.members.size(),
                        m.body["room"]["code"].as_string().c_str());
            early_story_session_internal_update(states.step);
            appearance_session::Shared().Entered(g.room, g.me, m.body, false);
            // A state left by a previous owner in an untagged room is stale.
            if (states.boss && g.boss_attr > 0)
                boss_battle_online_internal_update(states.boss_state.data(), states.boss_state.size());
            PublishStoryState();
            PublishBossBattleState();
        }
    } else {
        std::printf("Matching2: request 0x%04x failed: %s\n", p.event, m.code.c_str());
        if (p.event == kEvSetExternal) g.last_external.clear();
        if (p.event == kEvCreateJoin) {
            g.story_room_hosted = false;
            g.story_sent = EARLY_STORY_NO_STEP;
        }
        if (p.kind == Kind::kCreateJoin && m.code == "incompatible") {
            if (m.body["boss_battle_required"].as_bool() || m.body["boss_battle_only"].as_bool()) {
                g.notice = m.body["boss_battle_only"].as_bool()
                               ? "This room is not a Boss Battle session.\nThe Boss Battle page joins only Boss Battle rooms."
                               : "This room is a Boss Battle session.\nJoin it from the Boss Battle page.";
                std::printf("Boss Battle Online: the lobby refused the room (%s)\n",
                            m.body["boss_battle_only"].as_bool() ? "not a Boss Battle room" : "a Boss Battle room");
            } else if (m.body["story_session_required"].as_bool()) {
                g.notice = "This room is an Early Story Online session.\nThis game version cannot join it.";
                std::printf("Early Story Online: not declared as supported here, the room is refused\n");
            } else {
                Refuse("Host", "room host", m.body);
            }
        }
    }
    const RequestCallback cb = p.callback;
    void* arg = p.arg;
    const uint32_t request = p.request;
    const uint16_t event = p.event;
    g.calls.push_back([=] { cb(kContextId, request, event, error, data, arg); (void)block; });
}

void OnPeer(const Json& b) {
    const uint16_t member = uint16_t(b["member"].as_int());
    if (member == g.me) return;
    Signal& s = g.signals[member];
    const net::Endpoint address{net::parse_ip(b["ip"].as_string()), uint16_t(b["port"].as_int())};
    std::vector<net::Endpoint> lan;
    for (const Json& c : b["lan"].items()) {
        const net::Endpoint e{net::parse_ip(c["ip"].as_string()), uint16_t(c["port"].as_int())};
        if (e.valid() && net::is_lan(e.ip) && lan.size() < 4) lan.push_back(e);
    }
    s.account = b["account"].as_u64();
    if (address != s.address || lan != s.lan) {
        if (s.delivered) std::printf("Matching2: member 0x%x moved to %s\n", member, net::endpoint_text(address).c_str());
        s.address = address;
        s.lan = lan;
        s.changed_ms = net::now_ms();
    }
    std::printf("Matching2: member 0x%x is at %s (%zu LAN candidate(s))\n", member, net::endpoint_text(address).c_str(), lan.size());
    if (g.socket && s.account) {
        g.socket->set_peer(s.account, Candidates(s), member);
        for (const net::Endpoint& e : Candidates(s)) g.socket->punch(e);
    }
}

void OnEvent(const net::LobbyMessage& m) {
    const Json& b = m.body;
    if (m.event == "chat") {
        std::printf("Chat: %s: %s\n", SafeName(b["name"].as_string()).c_str(), b["text"].as_string().substr(0, 120).c_str());
        return;
    }
    if (m.event.rfind("friend.", 0) == 0) return;
    if (!g.room || b["room"].as_u64() != g.room) return;
    auto block = std::make_shared<Block>();
    if (m.event == "room.member_joined") {
        const Member joined = MemberOf(b["member"]);
        g.members[joined.id] = joined;
        Remember(joined.name, b["member"]);
        boss_battle_online_members_changed(int(g.members.size()));
        std::printf("Matching2: member 0x%x joined\n", joined.id);
        uint8_t* data = block->alloc(0x20);
        Wr<uint8_t*>(data, 0, MemberStruct(*block, joined));
        PushRoomEvent(kRoomMemberJoined, block, data);
    } else if (m.event == "room.member_left") {
        const uint16_t id = uint16_t(b["member"].as_int());
        Member left;
        left.id = id;
        if (g.members.count(id)) left = g.members[id];
        for (const std::string& line : g.story_watch.PeerLeft(net::now_ms(), left.account, b["why"].as_string()))
            std::printf("%s\n", line.c_str());
        g.members.erase(id);
        boss_battle_online_members_changed(int(g.members.size()));
        if (g.socket && g.signals.count(id)) g.socket->forget_peer(g.signals[id].account);
        g.signals.erase(id);
        std::printf("Matching2: member 0x%x left (%s)\n", id, b["why"].as_string().c_str());
        appearance_session::Shared().MemberLeft(id);
        uint8_t* data = block->alloc(0x20);
        Wr<uint8_t*>(data, 0, MemberStruct(*block, left));
        PushRoomEvent(kRoomMemberLeft, block, data);
    } else if (m.event == "room.owner") {
        const uint16_t from = uint16_t(b["from"].as_int()), to = uint16_t(b["to"].as_int());
        const bool story_guest = early_story_story_room_guest();
        g.owner = to;
        early_story_session_owner_changed(g.me == g.owner);
        boss_battle_online_owner_changed(g.me == g.owner);
        g.boss_attr = BOSS_BATTLE_ROOM_NONE; // the lobby untags a room when its owner changes
        g.boss_published = 0;
        if (g.me != g.owner) g.story_room_hosted = false;
        else if (story_guest) g.clear_story_attr = true;
        if (story_guest) std::printf("Early Story Online: the room changed owner, the story session ended (own story restored)\n");
        std::printf("Matching2: room owner 0x%x -> 0x%x\n", from, to);
        uint8_t* data = block->alloc(0x10);
        Wr<uint16_t>(data, 0, from);
        Wr<uint16_t>(data, 2, to);
        PushRoomEvent(kRoomOwner, block, data);
    } else if (m.event == "room.internal") {
        PrivateStates states;
        const Json game_attrs = WithoutPrivateStates(b["bin_int"], states);
        if (states.story) early_story_session_internal_update(states.step);
        if (states.boss) boss_battle_online_internal_update(states.boss_state.data(), states.boss_state.size());
        // Only the owner's private state changed: the game would never have been called.
        if ((states.story || states.boss) && game_attrs.size() == 0) return;
        uint8_t* data = block->alloc(0x50);
        uint8_t* flags = block->alloc(8);
        Wr<uint32_t>(flags, 0, uint32_t(b["flags"].as_int()));
        Wr<uint8_t*>(data, 8, flags);
        uint32_t n = 0;
        uint8_t* attrs = InternalAttrs(*block, game_attrs, n);
        if (n) {
            uint8_t* pointers = block->alloc(8 * n);
            for (uint32_t i = 0; i < n; ++i) Wr<uint8_t*>(pointers, i * 8, attrs + i * 0x28);
            Wr<uint8_t*>(data, 0x38, pointers);
            Wr<uint64_t>(data, 0x40, n);
        }
        PushRoomEvent(kRoomInternal, block, data);
    } else if (m.event == "room.kicked" || m.event == "room.gone") {
        std::printf("Matching2: %s\n", m.event == "room.kicked" ? "kicked from the room" : "the room was closed");
        RoomGone(m.event == "room.kicked" ? kRoomKicked : kRoomDestroyed);
    } else if (m.event == "room.appearance") {
        appearance_session::Shared().Received(uint16_t(b["member"].as_int()), b["appearance"]);
    } else if (m.event == "relay.closed") {
        std::printf("Matching2: the lobby stopped relaying for this connection (%s)\n", b["reason"].as_string().c_str());
        if (g.socket) g.socket->disable_relay();
    } else if (m.event == "peer") {
        OnPeer(b);
    } else if (m.event == "peer.gone") {
        const uint16_t id = uint16_t(b["member"].as_int());
        auto it = g.signals.find(id);
        const bool delivered = it != g.signals.end() && it->second.delivered;
        if (it != g.signals.end()) {
            if (g.socket) g.socket->forget_peer(it->second.account);
            g.signals.erase(it);
        }
        if (delivered && g.signal_cb) {
            const SignalingCallback cb = g.signal_cb;
            void* arg = g.signal_arg;
            const uint64_t room = g.room;
            g.calls.push_back([=] { cb(kContextId, room, id, kSignalDead, 0, arg); });
        }
    }
}

// This client's look, to the lobby for the room's other members; never with a lost link or a seat being resumed.
void PublishAppearance(uint64_t now) {
    if (!LinkUsable() || !g.up || g.lost || !g.room) return;
    std::optional<Json> body = appearance_session::Shared().TakePublish(now);
    if (!body) return;
    g.pending[g.link->request("room.appearance", std::move(*body))] = Pending{Kind::kAppearance, 0, nullptr, nullptr, 0};
}

void NoteRelayCapability(const Json& hello_reply) {
    const net::RelayCapability capability = net::relay_capability(hello_reply);
    g.relay_capable = capability.usable();
    g.story_capable = net::story_session_capability(hello_reply);
    g.boss_capable = net::boss_battle_session_capability(hello_reply);
    boss_battle_online_set_capable(g.boss_capable);
    appearance_session::Shared().SetCapable(appearance_session_enabled() && net::appearance_capability(hello_reply));
    std::printf("Matching2: lobby relay %s\n", g.relay_capable ? "available as a fallback" : "not offered");
    ConfigureRelay();
}

void NoteLobbyAddress(const std::string& ip) {
    g.your_ip = g.public_ip = net::parse_ip(ip);
    g.lobby_on_lan = net::is_lan(net::classify_text(ip));
}

void OnLink(const net::LobbyMessage& m) {
    using K = net::LobbyMessage::Kind;
    if (m.kind == K::kUp) {
        NoteRelayCapability(m.body);
        g.up = true;
        g.friend_code = m.body["friend_code"].as_string();
        NoteLobbyAddress(m.body["ip"].as_string());
        std::printf("Matching2: signed in to the lobby as %s; it sees %s\n", settings().name.c_str(), m.body["ip"].as_string().c_str());
        if (g.start_wanted) PushContextEvent(kCtxStarted, 0);
    } else if (m.kind == K::kLost) {
        g.lost = true;
        g.udp_inflight = false;
        PauseRelay(true);
        std::printf("Matching2: lobby connection lost; the game is not told while it reconnects\n");
    } else if (m.kind == K::kBack) {
        NoteRelayCapability(m.body);
        g.friend_code = m.body["friend_code"].as_string();
        NoteLobbyAddress(m.body["ip"].as_string());
        if (g.room) {
            Json body = Json::Object();
            body.set("room", net::u64_text(g.room));
            body.set("member", int64_t(g.me));
            const uint32_t seq = g.link->request("room.resume", std::move(body), true);
            g.pending[seq] = Pending{Kind::kResume, 0, nullptr, nullptr, 0};
        } else {
            PauseRelay(false);
            g.lost = false;
            g.reported_port = 0;
            g.presence_sent = "-";
            g.link->release_held();
        }
    } else if (m.kind == K::kDown) {
        PauseRelay(false);
        const bool was_up = g.up;
        g.relay_capable = false;
        g.story_capable = false;
        g.boss_capable = false;
        boss_battle_online_set_capable(0);
        appearance_session::Shared().SetCapable(false);
        ConfigureRelay();
        g.up = false;
        g.down = true;
        g.lost = false;
        std::printf("Matching2: lobby unavailable (%s)\n", m.code.c_str());
        for (auto& [seq, p] : g.pending) {
            if (p.kind == Kind::kBossBattle) boss_battle_online_published(p.boss_version, 0);
            Complete(p, kErrLobbyDown);
        }
        g.pending.clear();
        g.friends_busy = false;
        g.friends.clear();
        g.boss_friend_names.clear();
        g.friend_list.clear();
        g.friend_list_known = false;
        ++g.friends_generation;
        RoomGone(kRoomDestroyed);
        if (g.start_wanted) PushContextEvent(was_up ? kCtxStartOver : kCtxStarted, m.code == "unauthorized" ? kErrIdentity : kErrLobbyDown);
    }
}

// The room owner tells the lobby which classes the party plays once it has been the same for a few seconds; the
// lobby counts it without knowing who played. Best effort: a refusal or a lost link only means no report.
void ReportParty(uint64_t now) {
    if (!party::CanReport(settings().enabled, LinkUsable() && g.up && !g.lost, g.room, g.me, g.owner)) {
        g.party.Idle();
        return;
    }
    const int64_t at = int64_t(now);
    if (!g.party.Due(at)) return;
    std::vector<uint16_t> ids;
    for (const auto& member : g.members) ids.push_back(member.first);
    const std::optional<party::Snapshot> send = g.party.Poll(g.room, party::BuildSnapshot(ids, party::SharedUnits(), at), at);
    if (!send) return;
    Json classes = Json::Array();
    for (uint8_t i = 0; i < send->size; ++i) classes.push(int64_t(send->classes[i]));
    Json body = Json::Object();
    body.set("classes", std::move(classes));
    const uint32_t seq = g.link->request("party.report", std::move(body));
    g.pending[seq] = Pending{Kind::kInternal, 0, nullptr, nullptr, 0};
    g.party.Sent(g.room, *send, at);
    std::printf("Matching2: party report sent (%u classes)\n", unsigned(send->size));
}

// Our endpoint goes to the lobby once the game's socket is bound and STUN / UPnP had their chance.
void ReportEndpoint(uint64_t now) {
    std::shared_ptr<net::PeerSocket> socket = peer_socket();
    if (socket != g.socket) {
        g.socket = socket;
        SetRelayEnds(g.link.get(), socket);
        g.bound_ms = now;
        g.reported_port = 0;
        g.udp_acked = false;
        g.udp_inflight = false;
        ApplyPeers();
    }
    if (!socket || !g.up || g.lost || g.udp_inflight) return;
    net::Endpoint mapped;
    const net::StunState stun = socket->stun(&mapped);
    const UpnpView upnp = upnp_view();
    const bool pending = stun == net::StunState::kRunning || upnp.working;
    if (!g.lobby_on_lan && pending && now - g.bound_ms < kEndpointWaitMs) return;
    const net::EndpointChoice choice = net::choose_endpoint(g.your_ip, g.lobby_on_lan, socket->port(), stun, mapped,
                                                            upnp.mapped ? upnp.external : net::Endpoint{});
    g.public_ip = choice.ip;
    if (choice.port == g.reported_port && choice.ip == g.reported_ip) return;
    Json body = Json::Object();
    body.set("port", int64_t(choice.port));
    if (choice.ip != g.your_ip) body.set("public_ip", net::ip_text(choice.ip));
    Json lan = Json::Array();
    std::vector<uint32_t> addresses = net::local_ipv4_addresses();
    addresses.insert(addresses.begin(), net::parse_ip("127.0.0.1"));
    for (const uint32_t ip : addresses) {
        if (!net::is_lan(ip) || lan.size() >= 4) continue;
        Json c = Json::Object();
        c.set("ip", net::ip_text(ip));
        c.set("port", int64_t(socket->port()));
        lan.push(std::move(c));
    }
    body.set("lan", std::move(lan));
    std::printf("Matching2: %s (local %u)\n", net::endpoint_choice_text(choice, g.relay_capable).c_str(), socket->port());
    g.reported_port = choice.port;
    g.reported_ip = choice.ip;
    g.udp_inflight = true;
    const uint32_t seq = g.link->request("udp", std::move(body));
    g.pending[seq] = Pending{Kind::kUdp, 0, nullptr, nullptr, 0};
}

// A peer is introduced to the game (signaling "established") when its endpoint is known and settled and ours has
// reached the lobby; the game then starts its Rudp connection to it at once.
void Introduce(uint64_t now) {
    if (!g.room || !g.signal_cb || !g.socket) return;
    const bool ready = g.udp_acked || now - g.bound_ms > kEstablishWaitMs;
    for (auto& [member, s] : g.signals) {
        const net::Endpoint at = GameEndpoint(member, s);
        if (s.delivered || member == g.me || !g.members.count(member) || !at.valid()) continue;
        if (!ready || now - s.changed_ms < kSettleMs) continue;
        s.delivered = true;
        std::printf("Matching2: member 0x%x established at %s%s\n", member, net::endpoint_text(at).c_str(),
                    net::is_relay_endpoint(at) ? " (relay only: no direct endpoint)" : "");
        const SignalingCallback cb = g.signal_cb;
        void* arg = g.signal_arg;
        const uint64_t room = g.room;
        const uint16_t id = member;
        g.calls.push_back([=] { cb(kContextId, room, id, kSignalEstablished, 0, arg); });
    }
}

void FlushPresence() {
    if (!g.up || g.lost || g.down) return;
    const std::string key = g.presence_data + "/" + g.presence_status;
    if (key == g.presence_sent) return;
    g.presence_sent = key;
    Json body = Json::Object();
    body.set("data", g.presence_data);
    if (!g.presence_data.empty()) body.set("status", g.presence_status);
    const uint32_t seq = g.link->request("presence.set", std::move(body));
    g.pending[seq] = Pending{Kind::kPresence, 0, nullptr, nullptr, 0};
}

// ---- entry points ----
DCP_SYSV int32_t m2_initialize(const void* param) {
    (void)param;
    std::lock_guard<std::mutex> lock(g_mutex);
    g.initialized = true;
    early_story_session_observe_own_step(OnOwnStoryStep);
    return 0;
}

DCP_SYSV int32_t m2_terminate() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g.initialized = false;
    early_story_session_observe_own_step(nullptr);
    return 0;
}

DCP_SYSV int32_t m2_create_context(const void* param, uint16_t* context) {
    (void)param;
    if (!context) return kErrInvalid;
    std::lock_guard<std::mutex> lock(g_mutex);
    *context = kContextId;
    g.context = true;
    return 0;
}

DCP_SYSV int32_t m2_register_context_callback(void* callback, void* arg) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g.context_cb = reinterpret_cast<ContextCallback>(callback);
    g.context_arg = arg;
    return 0;
}

DCP_SYSV int32_t m2_context_start(uint16_t context, uint64_t timeout) {
    (void)context, (void)timeout;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g.context) return kErrNotStarted;
    g.start_wanted = true;
    if (g.link && !g.down) {
        if (g.up) PushContextEvent(kCtxStarted, 0);
        return 0;
    }
    const Settings& s = settings();
    if (s.config.lobby.empty()) {
        std::printf("Matching2: no lobby server is set; online play is unavailable\n");
        PushContextEvent(kCtxStarted, kErrLobbyDown);
        return 0;
    }
    SetRelayEnds(nullptr, g.socket);
    g.link.reset();
    g.down = g.up = g.lost = false;
    g.relay_capable = false;
    g.story_capable = false;
    net::LobbyLink::Options options;
    options.url = s.config.lobby;
    options.hello = HelloBody;
    g.link = std::make_unique<net::LobbyLink>(std::move(options));
    g.link->set_binary_sink(ReceiveRelayFrame);
    SetRelayEnds(g.link.get(), g.socket);
    g.link->start();
    std::printf("Matching2: going online through %s\n", s.config.lobby.c_str());
    return 0;
}

DCP_SYSV int32_t m2_context_stop(uint16_t context) {
    (void)context;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g.room && LinkUsable()) {
        Json body = Json::Object();
        body.set("room", net::u64_text(g.room));
        Issue(Kind::kInternal, "room.leave", std::move(body), nullptr, 0);
    }
    ClearRoom();
    g.start_wanted = false;
    g.reported_port = 0;
    g.udp_acked = false;
    PushContextEvent(kCtxStopped, 0);
    std::printf("Matching2: offline\n");
    return 0;
}

DCP_SYSV int32_t m2_destroy_context(uint16_t context) {
    (void)context;
    std::lock_guard<std::mutex> lock(g_mutex);
    g.context = false;
    return 0;
}

DCP_SYSV int32_t m2_get_server_id(uint16_t context, uint16_t* server) {
    (void)context;
    if (!server) return kErrInvalid;
    *server = kServerId;
    return 0;
}

DCP_SYSV int32_t m2_set_default_request_opt(uint16_t context, const void* opt) {
    (void)context;
    std::lock_guard<std::mutex> lock(g_mutex);
    g.default_cb = opt ? Rd<RequestCallback>(opt, 0) : nullptr;
    g.default_arg = opt ? Rd<void*>(opt, 8) : nullptr;
    return 0;
}

DCP_SYSV int32_t m2_get_world_info_list(uint16_t context, const void* request, const void* opt, uint32_t* id) {
    (void)context, (void)request;
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t r = Issue(Kind::kWorlds, "worlds", Json::Object(), opt, kEvWorlds);
    if (id) *id = r;
    return 0;
}

// The user info (one byte, attr 0x5f) is not needed by other players: answered here.
DCP_SYSV int32_t m2_set_user_info(uint16_t context, const void* request, const void* opt, uint32_t* id) {
    (void)context, (void)request;
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t r = CompleteLocally(opt, kEvSetUserInfo);
    if (id) *id = r;
    return 0;
}

// CreateJoinRoomA: max slots u16 @0, flags @8, world @0xc, internal attrs @0x60/@0x68, int attrs @0x70/@0x78,
// external attrs @0x90/@0x98, member attrs @0xa0/@0xa8.
DCP_SYSV int32_t m2_create_join_room(uint16_t context, const uint8_t* request, const void* opt, uint32_t* id) {
    (void)context;
    if (!request) return kErrInvalid;
    std::lock_guard<std::mutex> lock(g_mutex);
    Json body = Json::Object();
    body.set("world", int64_t(std::max<uint32_t>(1, Rd<uint32_t>(request, 0xc))));
    body.set("slots", int64_t(std::clamp<uint16_t>(Rd<uint16_t>(request, 0), 1, 4)));
    const int own_step = early_story_own_step();
    const int boss_attr = boss_battle_online_room_attr();
    if (boss_attr != BOSS_BATTLE_ROOM_NONE && !g.boss_capable) {
        g.notice = "Boss Battle Online is not available yet:\nthe online service has to be updated first.";
        std::printf("Boss Battle Online: not hosting, the online service does not keep Boss Battle rooms\n");
        return kErrRefused;
    }
    const bool story_room = boss_attr == BOSS_BATTLE_ROOM_NONE && early_story_hosts_story_room(own_step);
    if (story_room && !g.story_capable) {
        g.notice = "Early Story Online is not available yet:\nthe online service has to be updated first.";
        std::printf("Early Story Online: not hosting, the online service does not keep story rooms to capable players\n");
        return kErrRefused;
    }
    body.set("flags", int64_t(early_story_room_flags(Rd<uint32_t>(request, 8), own_step)));
    Json ints = IntAttrs(Rd<const uint8_t*>(request, 0x70), Rd<uint64_t>(request, 0x78));
    if (story_room) AddStoryAttr(ints, early_story_room_attr_value(own_step));
    if (boss_attr != BOSS_BATTLE_ROOM_NONE) AddIntAttr(ints, BOSS_BATTLE_ROOM_ATTR, boss_attr);
    body.set("ints", std::move(ints));
    body.set("bin_int", BinAttrs(Rd<const uint8_t*>(request, 0x60), Rd<uint64_t>(request, 0x68)));
    body.set("bin_ext", BinAttrs(Rd<const uint8_t*>(request, 0x90), Rd<uint64_t>(request, 0x98)));
    body.set("member_bin", BinAttrs(Rd<const uint8_t*>(request, 0xa0), Rd<uint64_t>(request, 0xa8)));
    std::printf("Matching2: create room (slots %lld, flags 0x%08llx%s)\n", (long long)body["slots"].as_int(),
                (unsigned long long)body["flags"].as_int(),
                story_room ? ", story session" : boss_attr != BOSS_BATTLE_ROOM_NONE ? ", Boss Battle session" : "");
    g.story_room_hosted = story_room;
    g.clear_story_attr = false;
    g.story_sent = story_room ? own_step : EARLY_STORY_NO_STEP;
    const uint32_t r = Issue(Kind::kCreateJoin, "room.create", std::move(body), opt, kEvCreateJoin);
    if (id) *id = r;
    return 0;
}

// JoinRoomA: room id u64 @0.
DCP_SYSV int32_t m2_join_room(uint16_t context, const uint8_t* request, const void* opt, uint32_t* id) {
    (void)context;
    if (!request) return kErrInvalid;
    std::lock_guard<std::mutex> lock(g_mutex);
    Json body = Json::Object();
    body.set("room", net::u64_text(Rd<uint64_t>(request, 0)));
    if (boss_battle_online_search_wanted()) {
        body.set("boss_battle", true);
        std::printf("Boss Battle Online: joining friend room %llu\n", (unsigned long long)Rd<uint64_t>(request, 0));
    }
    std::printf("Matching2: join room %llu\n", (unsigned long long)Rd<uint64_t>(request, 0));
    g.story_room_hosted = g.clear_story_attr = false;
    g.story_sent = EARLY_STORY_NO_STEP;
    const uint32_t r = Issue(Kind::kCreateJoin, "room.join", std::move(body), opt, kEvJoin);
    if (id) *id = r;
    return 0;
}

DCP_SYSV int32_t m2_leave_room(uint16_t context, const uint8_t* request, const void* opt, uint32_t* id) {
    (void)context;
    if (!request) return kErrInvalid;
    std::lock_guard<std::mutex> lock(g_mutex);
    Json body = Json::Object();
    body.set("room", net::u64_text(Rd<uint64_t>(request, 0)));
    std::printf("Matching2: leave room %llu\n", (unsigned long long)Rd<uint64_t>(request, 0));
    const uint32_t r = Issue(Kind::kGame, "room.leave", std::move(body), opt, kEvLeave);
    if (id) *id = r;
    ClearRoom();
    return 0;
}

// SearchRoom: world @4, start @0x10, max @0x14, flag filter @0x18, flag attr @0x1c, filters @0x20/@0x28 (16 bytes:
// operator u8 @0, attr u16 @8, value u32 @0xc).
DCP_SYSV int32_t m2_search_room(uint16_t context, const uint8_t* request, const void* opt, uint32_t* id) {
    (void)context;
    if (!request) return kErrInvalid;
    std::lock_guard<std::mutex> lock(g_mutex);
    Json body = Json::Object();
    if (boss_battle_online_search_wanted()) body.set("boss_battle", true);
    body.set("from", int64_t(std::max<uint32_t>(1, Rd<uint32_t>(request, 0x10))));
    body.set("count", int64_t(std::clamp<uint32_t>(Rd<uint32_t>(request, 0x14), 1, 20)));
    const std::string& code = settings().join_code;
    if (!code.empty()) {
        // A join code names one room: the game's own conditions (which follow the searcher's stage) do not apply.
        body.set("code", code);
        std::printf("Matching2: search for the room with code %s\n", code.c_str());
    } else {
        body.set("world", int64_t(Rd<uint32_t>(request, 4)));
        body.set("flag_mask", int64_t(Rd<uint32_t>(request, 0x18)));
        body.set("flag_value", int64_t(Rd<uint32_t>(request, 0x1c)));
        Json filters = Json::Array();
        const uint8_t* list = Rd<const uint8_t*>(request, 0x20);
        const uint64_t n = Rd<uint64_t>(request, 0x28);
        for (uint64_t i = 0; list && i < n && i < 16; ++i) {
            const uint8_t op = list[i * 16];
            if (op < 1 || op > 6) continue;
            Json f = Json::Array();
            f.push(int64_t(Rd<uint16_t>(list, size_t(i * 16 + 8))));
            f.push(int64_t(op));
            f.push(int64_t(Rd<uint32_t>(list, size_t(i * 16 + 0xc))));
            filters.push(std::move(f));
        }
        std::printf("Matching2: search %s\n", filters.dump().c_str());
        body.set("filters", std::move(filters));
    }
    const uint32_t r = Issue(Kind::kSearch, "room.search", std::move(body), opt, kEvSearch);
    if (id) *id = r;
    return 0;
}

// SetRoomDataInternal: room u64 @0, flag filter @8, flag attr @0xc, attrs @0x10/@0x18.
DCP_SYSV int32_t m2_set_room_internal(uint16_t context, const uint8_t* request, const void* opt, uint32_t* id) {
    (void)context;
    if (!request) return kErrInvalid;
    std::lock_guard<std::mutex> lock(g_mutex);
    // A refused story update goes out again with the game's next one.
    PublishStoryState();
    Json body = InternalBody(Rd<uint64_t>(request, 0), Rd<uint32_t>(request, 8), Rd<uint32_t>(request, 0xc),
                             BinAttrs(Rd<const uint8_t*>(request, 0x10), Rd<uint64_t>(request, 0x18)));
    std::printf("Matching2: room flags 0x%08llx under mask 0x%08llx\n", (unsigned long long)body["flag_value"].as_int(),
                (unsigned long long)body["flag_mask"].as_int());
    const uint32_t r = Issue(Kind::kGame, "room.set_internal", std::move(body), opt, kEvSetInternal);
    if (id) *id = r;
    return 0;
}

// SetRoomDataExternal: room u64 @0, int attrs @8/@0x10, external attrs @0x28/@0x30. The game repeats it every few
// frames, mostly unchanged: repeats are answered here.
DCP_SYSV int32_t m2_set_room_external(uint16_t context, const uint8_t* request, const void* opt, uint32_t* id) {
    (void)context;
    if (!request) return kErrInvalid;
    std::lock_guard<std::mutex> lock(g_mutex);
    Json body = Json::Object();
    body.set("room", net::u64_text(Rd<uint64_t>(request, 0)));
    Json ints = IntAttrs(Rd<const uint8_t*>(request, 8), Rd<uint64_t>(request, 0x10));
    if (g.story_room_hosted) AddStoryAttr(ints, early_story_room_attr_value(early_story_own_step()));
    else if (g.clear_story_attr) AddStoryAttr(ints, 0);
    if (g.boss_attr != BOSS_BATTLE_ROOM_NONE || boss_battle_online_room_attr() != BOSS_BATTLE_ROOM_NONE) {
        g.boss_attr = boss_battle_online_room_attr();
        boss_battle_online_room_tagged(g.boss_attr);
        AddIntAttr(ints, BOSS_BATTLE_ROOM_ATTR, g.boss_attr);
    }
    body.set("ints", std::move(ints));
    body.set("bin_ext", BinAttrs(Rd<const uint8_t*>(request, 0x28), Rd<uint64_t>(request, 0x30)));
    const std::string text = body.dump();
    uint32_t r;
    if (text == g.last_external) {
        r = CompleteLocally(opt, kEvSetExternal);
    } else {
        g.last_external = text;
        std::printf("Matching2: room search attributes %s\n", body["ints"].dump().c_str());
        r = Issue(Kind::kGame, "room.set_external", std::move(body), opt, kEvSetExternal);
    }
    if (id) *id = r;
    return 0;
}

DCP_SYSV int32_t m2_kick_member(uint16_t context, const uint8_t* request, const void* opt, uint32_t* id) {
    (void)context;
    if (!request) return kErrInvalid;
    std::lock_guard<std::mutex> lock(g_mutex);
    Json body = Json::Object();
    body.set("room", net::u64_text(Rd<uint64_t>(request, 0)));
    body.set("member", int64_t(Rd<uint16_t>(request, 8)));
    const uint32_t r = Issue(Kind::kGame, "room.kick", std::move(body), opt, kEvKick);
    if (id) *id = r;
    return 0;
}

DCP_SYSV int32_t m2_register_room_callback(uint16_t context, void* callback, void* arg) {
    (void)context;
    std::lock_guard<std::mutex> lock(g_mutex);
    g.room_cb = reinterpret_cast<RoomCallback>(callback);
    g.room_arg = arg;
    return 0;
}

DCP_SYSV int32_t m2_register_signaling_callback(uint16_t context, void* callback, void* arg) {
    (void)context;
    std::lock_guard<std::mutex> lock(g_mutex);
    g.signal_cb = reinterpret_cast<SignalingCallback>(callback);
    g.signal_arg = arg;
    return 0;
}

DCP_SYSV int32_t m2_ok() { return 0; }
DCP_SYSV int32_t m2_unsupported() { return kErrUnsupported; }

// {u64 size, local address @8, mapped address @0xc (network order), NAT type @0x10}.
DCP_SYSV int32_t m2_local_net_info(uint8_t* info) {
    if (!info) return kErrInvalid;
    std::lock_guard<std::mutex> lock(g_mutex);
    uint32_t local = net::default_route_ip();
    uint32_t mapped = g.your_ip;
    if (g.socket) {
        net::Endpoint stun;
        if (g.socket->stun(&stun) == net::StunState::kDone) mapped = stun.ip;
    }
    Wr<uint32_t>(info, 8, __builtin_bswap32(local));
    Wr<uint32_t>(info, 0xc, __builtin_bswap32(mapped ? mapped : local));
    Wr<int32_t>(info, 0x10, 2);
    return 0;
}

// (ctx, room, member, status*, address* and port* in network order); status 2 = connected.
DCP_SYSV int32_t m2_connection_status(uint16_t context, uint64_t room, uint16_t member, int32_t* status, uint32_t* address,
                                      uint16_t* port) {
    (void)context, (void)room;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g.signals.find(member);
    const net::Endpoint at = it != g.signals.end() ? GameEndpoint(member, it->second) : net::Endpoint{};
    const bool known = at.valid();
    if (status) *status = known ? 2 : 0;
    if (address) *address = known ? __builtin_bswap32(at.ip) : 0;
    if (port) *port = known ? uint16_t(at.port >> 8 | at.port << 8) : 0;
    return known ? 0 : kErrInvalid;
}

// (ctx, room, target, member ids out, capacity) -> number of members.
DCP_SYSV int32_t m2_member_ids(uint16_t context, uint64_t room, int32_t target, uint16_t* ids, uint32_t capacity) {
    (void)context, (void)target;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g.room || room != g.room) return kErrNoRoom;
    uint32_t n = 0;
    for (const auto& [id, m] : g.members) {
        if (ids && n < capacity) ids[n] = id;
        ++n;
    }
    return int32_t(std::min(n, capacity));
}

struct Export {
    const char* name;
    void* function;
};

#define M2(name, fn) {name, reinterpret_cast<void*>(&fn)}
const Export kExports[] = {
    M2("sceNpMatching2Initialize", m2_initialize),
    M2("sceNpMatching2Terminate", m2_terminate),
    M2("sceNpMatching2CreateContextA", m2_create_context),
    M2("sceNpMatching2CreateContext", m2_create_context),
    M2("sceNpMatching2RegisterContextCallback", m2_register_context_callback),
    M2("sceNpMatching2ContextStart", m2_context_start),
    M2("sceNpMatching2ContextStop", m2_context_stop),
    M2("sceNpMatching2DestroyContext", m2_destroy_context),
    M2("sceNpMatching2GetServerId", m2_get_server_id),
    M2("sceNpMatching2SetDefaultRequestOptParam", m2_set_default_request_opt),
    M2("sceNpMatching2GetWorldInfoList", m2_get_world_info_list),
    M2("sceNpMatching2SetUserInfo", m2_set_user_info),
    M2("sceNpMatching2CreateJoinRoomA", m2_create_join_room),
    M2("sceNpMatching2JoinRoomA", m2_join_room),
    M2("sceNpMatching2LeaveRoom", m2_leave_room),
    M2("sceNpMatching2SearchRoom", m2_search_room),
    M2("sceNpMatching2SetRoomDataInternal", m2_set_room_internal),
    M2("sceNpMatching2SetRoomDataExternal", m2_set_room_external),
    M2("sceNpMatching2KickoutRoomMember", m2_kick_member),
    M2("sceNpMatching2RegisterRoomEventCallback", m2_register_room_callback),
    M2("sceNpMatching2RegisterSignalingCallback", m2_register_signaling_callback),
    M2("sceNpMatching2RegisterLobbyEventCallback", m2_ok),
    M2("sceNpMatching2RegisterRoomMessageCallback", m2_ok),
    M2("sceNpMatching2SignalingGetLocalNetInfo", m2_local_net_info),
    M2("sceNpMatching2SignalingGetConnectionStatus", m2_connection_status),
    M2("sceNpMatching2GetRoomMemberIdListLocal", m2_member_ids),
    M2("sceNpMatching2SetSignalingOptParam", m2_ok),
    M2("sceNpMatching2GetSignalingOptParamLocal", m2_unsupported),
    M2("sceNpMatching2SignalingGetConnectionInfoA", m2_unsupported),
    M2("sceNpMatching2SignalingGetPingInfo", m2_unsupported),
    M2("sceNpMatching2GetMemoryInfo", m2_unsupported),
    M2("sceNpMatching2GetSslMemoryInfo", m2_unsupported),
    M2("sceNpMatching2GetRoomDataInternal", m2_unsupported),
    M2("sceNpMatching2GetRoomMemberDataInternal", m2_unsupported),
    M2("sceNpMatching2GetRoomDataExternalList", m2_unsupported),
    M2("sceNpMatching2GetUserInfoListA", m2_unsupported),
    M2("sceNpMatching2SetRoomMemberDataInternal", m2_unsupported),
    M2("sceNpMatching2SendRoomMessage", m2_unsupported),
    M2("sceNpMatching2SendRoomChatMessage", m2_unsupported),
    M2("sceNpMatching2JoinLobby", m2_unsupported),
    M2("sceNpMatching2LeaveLobby", m2_unsupported),
    M2("sceNpMatching2GetLobbyInfoList", m2_unsupported),
    M2("sceNpMatching2GrantRoomOwner", m2_unsupported),
};
#undef M2

} // namespace

void pump() {
    std::vector<std::function<void()>> calls;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g.link) {
            net::LobbyMessage m;
            while (g.link->poll(m)) {
                if (m.kind == net::LobbyMessage::Kind::kResponse) OnResponse(m);
                else if (m.kind == net::LobbyMessage::Kind::kEvent) OnEvent(m);
                else OnLink(m);
            }
        }
        const uint64_t now = net::now_ms();
        ReportEndpoint(now);
        Introduce(now);
        FlushPresence();
        ReportParty(now);
        PublishBossBattleState();
        PublishBossBattleTag();
        ResumeStoryRoom();
        PublishAppearance(now);
        WatchStorySession(now);
        calls.swap(g.calls);
    }
    for (auto& call : calls) call();
    if (void (*hook)() = g_pump_hook) hook();
}

uintptr_t matching2_export(const std::string& symbol) {
    for (const Export& e : kExports)
        if (symbol == e.name) return reinterpret_cast<uintptr_t>(e.function);
    return 0;
}

void presence_publish(const uint8_t* data, size_t size, const uint8_t* status, size_t status_size) {
    std::lock_guard<std::mutex> lock(g_mutex);
    size = data ? std::min<size_t>(size, 0x80) : 0;
    status_size = status ? std::min<size_t>(status_size, 0xc0) : 0;
    while (status_size && !status[status_size - 1]) --status_size;
    g.presence_data = size ? net::hex_encode(data, size) : std::string();
    g.presence_status = status_size ? net::hex_encode(status, status_size) : std::string();
    std::printf("Matching2: presence %zu bytes (room %llu)\n", size, (unsigned long long)g.room);
}

void presence_clear() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g.presence_data.clear();
    g.presence_status.clear();
}

void friends_fetch() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g.friends_busy) return;
    if (!g.up || g.down) {
        g.friends.clear();
        ++g.friends_generation;
        return;
    }
    g.friends_busy = true;
    const uint32_t seq = g.link->request("friends.list", Json::Object());
    g.pending[seq] = Pending{Kind::kFriends, 0, nullptr, nullptr, 0};
}

CommsRoom comms_room() {
    CommsRoom out;
    if (!settings().enabled) return out;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g.room) return out;
    out.slot = party::MemberSlot(g.me);
    out.members = int(g.members.size());
    for (const auto& [id, member] : g.members) {
        if (id == g.me) continue;
        CommsPeer peer;
        peer.account = member.account;
        peer.slot = party::MemberSlot(id);
        out.peers.push_back(peer);
    }
    return out;
}

bool comms_report_open() {
    if (!settings().enabled) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    return LinkUsable() && g.up && !g.lost;
}

bool comms_report_send(const std::array<uint64_t, 8>& counts) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!LinkUsable() || !g.up || g.lost) return false;
    Json list = Json::Array();
    for (const uint64_t count : counts) list.push(int64_t(std::min<uint64_t>(count, 1000000)));
    Json body = Json::Object();
    body.set("counts", std::move(list));
    const uint32_t seq = g.link->request("comms.report", std::move(body));
    g.pending[seq] = Pending{Kind::kCommsReport, 0, nullptr, nullptr, 0};
    return true;
}

CommsReportResult comms_report_result() {
    std::lock_guard<std::mutex> lock(g_mutex);
    CommsReportResult out;
    out.accepted = g.comms_result == 1;
    out.failed = g.comms_result == 2;
    out.code = std::move(g.comms_code);
    g.comms_result = 0;
    g.comms_code.clear();
    return out;
}

void set_pump_hook(void (*hook)()) { g_pump_hook = hook; }

RoomView room_view() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g.room) return {};
    return {party::MemberSlot(g.me), int(g.members.size())};
}

PageLink page_link() {
    PageLink out;
    if (!settings().enabled) return out;
    std::lock_guard<std::mutex> lock(g_mutex);
    out.link = !g.link          ? RUNTIME_ONLINE_LINK_OFF
               : g.down         ? RUNTIME_ONLINE_LINK_UNREACHABLE
               : g.up && !g.lost ? RUNTIME_ONLINE_LINK_UP
                                : RUNTIME_ONLINE_LINK_CONNECTING;
    out.friend_code = g.friend_code;
    out.friends_known = g.friend_list_known;
    out.friends = g.friend_list;
    return out;
}

bool friends_busy() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g.friends_busy;
}

std::vector<FriendRoom> friends_result(uint64_t* generation) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (generation) *generation = g.friends_generation;
    return g.friends;
}

std::string system_type(const std::string& online_id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (online_id == settings().name) return net::system_type_text(net::host_platform(), net::port_version());
    const auto it = g.clients.find(online_id);
    if (it == g.clients.end()) return "PC";
    return net::system_type_text(it->second.platform, it->second.version);
}

std::string take_notice() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return std::exchange(g.notice, std::string());
}

} // namespace dcp::online
