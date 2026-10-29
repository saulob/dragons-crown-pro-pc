// libSceRudp and the game's peer-to-peer socket (libSceNet type 6, "RUDP_NetId") over net::PeerSocket. The game's
// RudpMgr opens the socket after creating or joining a room, binds one Rudp context per peer slot and calls
// sceRudpInitiate on both ends with the address Matching2 signaling gave it; it then polls with timeout 0 every tick.
// Contracts it relies on: whole messages per Read/Write, events 1 = readable, 2 = writable (connected), 8 = closed.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "net/upnp.h"
#include "runtime_online.h"

#define DCP_SYSV __attribute__((sysv_abi))

namespace dcp::online {
namespace {

constexpr int32_t kRudpNotInit = int32_t(0x80770001);
constexpr int32_t kRudpInvalid = int32_t(0x80770004);
constexpr int32_t kRudpWouldBlock = int32_t(0x80770016);
constexpr int32_t kRudpClosed = int32_t(0x80770024);
constexpr int32_t kNetBadF = int32_t(0x80410109);
constexpr int32_t kNetInval = int32_t(0x80410116);
constexpr int32_t kNetAddrInUse = int32_t(0x80410130);
constexpr int32_t kNetUnreachable = int32_t(0x80410133);
constexpr int32_t kSockDgramP2P = 6;
constexpr uint16_t kRead = 1, kWrite = 2, kError = 8;
constexpr uint64_t kResolveTimeoutMs = 20000;

struct Context {
    int socket = 0;
    uint16_t vport = 0;
    net::Endpoint peer;
    uint64_t account = 0;
    int link = -1;
    bool initiated = false, connected = false, terminated = false;
    uint64_t initiated_ms = 0;
};

struct PollEntry {
    int32_t ctx;
    uint16_t requested;
    uint16_t returned;
};
static_assert(sizeof(PollEntry) == 8);

std::mutex g_mutex;
bool g_initialized = false;
std::map<int, bool> g_sockets; // id -> bound
int g_next_socket = 1, g_bound_socket = 0;
std::shared_ptr<net::PeerSocket> g_peer;
std::unique_ptr<net::upnp::PortMapping> g_upnp;
std::map<int, Context> g_contexts;
int g_next_context = 1;
std::map<int, std::map<int, uint16_t>> g_polls;
int g_next_poll = 1;

uint16_t Swap16(uint16_t v) { return uint16_t(v >> 8 | v << 8); }

// The link of a context once its peer is known to the socket (the lobby's endpoint may arrive after Initiate).
void Attach(int id, Context& c) {
    if (!c.initiated || c.link >= 0 || !g_peer) return;
    if (!c.account) c.account = g_peer->peer_at(c.peer);
    if (!c.account) return;
    c.link = g_peer->connect(c.account);
    if (c.link >= 0) std::printf("Rudp: context %d -> peer %llu, link %d\n", id, (unsigned long long)c.account, c.link);
}

void Release(int id, Context& c, const char* why) {
    if (c.link >= 0 && g_peer) {
        const net::LinkStats s = g_peer->stats(c.link);
        std::printf("Rudp: context %d %s: in %llu msgs / %llu B, out %llu msgs / %llu B, %llu resends\n", id, why,
                    (unsigned long long)s.messages_in, (unsigned long long)s.bytes_in, (unsigned long long)s.messages_out,
                    (unsigned long long)s.bytes_out, (unsigned long long)s.resends);
        g_peer->close(c.link);
    }
    c.link = -1;
}

uint16_t Events(int id, Context& c) {
    if (c.terminated) return kError;
    Attach(id, c);
    if (!c.initiated) return 0;
    if (c.link < 0) return net::now_ms() - c.initiated_ms > kResolveTimeoutMs ? kError : 0;
    const net::LinkState state = g_peer ? g_peer->state(c.link) : net::LinkState::kClosed;
    uint16_t ev = 0;
    if (g_peer && g_peer->readable(c.link)) ev |= kRead;
    if (state == net::LinkState::kConnected) {
        if (!c.connected) {
            c.connected = true;
            std::printf("Rudp: context %d connected to %s\n", id, net::endpoint_text(g_peer->remote(c.link)).c_str());
        }
        if (g_peer->queued_bytes(c.link) < net::PeerSocket::kMaxMessage) ev |= kWrite;
    } else if (state == net::LinkState::kClosed && !(ev & kRead)) {
        ev |= kError;
    }
    return ev;
}

// ---- libSceNet: the P2P socket only ----
DCP_SYSV int32_t net_socket(const char* name, int32_t family, int32_t type, int32_t protocol) {
    (void)protocol;
    if (family != 2 || type != kSockDgramP2P) return kNetUnreachable;
    std::lock_guard<std::mutex> lock(g_mutex);
    const int id = g_next_socket++;
    g_sockets[id] = false;
    std::printf("Net: P2P socket %d (\"%s\")\n", id, name ? name : "");
    return id;
}

DCP_SYSV int32_t net_setsockopt(int32_t s, int32_t level, int32_t option, const void* value, uint32_t size) {
    (void)level, (void)option, (void)value, (void)size;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_sockets.count(s) ? 0 : kNetBadF;
}

// SceNetSockaddrIn: len, family, port (network order) @2, address @4, vport (network order) @8.
DCP_SYSV int32_t net_bind(int32_t s, const uint8_t* address, uint32_t size) {
    if (!address || size < 16) return kNetInval;
    uint16_t port, vport;
    std::memcpy(&port, address + 2, 2);
    std::memcpy(&vport, address + 8, 2);
    port = Swap16(port), vport = Swap16(vport);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_sockets.find(s);
    if (it == g_sockets.end()) return kNetBadF;
    if (g_peer) return kNetAddrInUse;
    const Settings& cfg = settings();
    net::PeerSocket::Options options;
    options.port = cfg.config.udp_port ? cfg.config.udp_port : port;
    options.stun_servers = net::stun_servers(cfg.config.stun);
    std::string error;
    g_peer = net::PeerSocket::open(options, &error);
    if (!g_peer) {
        std::printf("Net: P2P socket %d cannot bind: %s\n", s, error.c_str());
        return kNetAddrInUse;
    }
    it->second = true;
    g_bound_socket = s;
    std::printf("Net: P2P socket %d bound (game port %u, vport %u) to UDP %u\n", s, port, vport, g_peer->port());
    if (cfg.config.upnp) {
        g_upnp = std::make_unique<net::upnp::PortMapping>();
        g_upnp->start(g_peer->port());
    }
    return 0;
}

DCP_SYSV int32_t net_socket_close(int32_t s) {
    std::unique_ptr<net::upnp::PortMapping> upnp;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_sockets.erase(s)) return kNetBadF;
        if (s != g_bound_socket) return 0;
        for (auto& [id, c] : g_contexts) Release(id, c, "closed with its socket");
        g_peer.reset();
        g_bound_socket = 0;
        upnp = std::move(g_upnp);
        std::printf("Net: P2P socket %d closed\n", s);
    }
    // Removing the router mapping can take seconds: not on the game's thread.
    if (upnp) std::thread([mapping = std::move(upnp)]() mutable { mapping.reset(); }).detach();
    return 0;
}

// ---- libSceRudp ----
DCP_SYSV int32_t rudp_init(void* pool, int32_t size) {
    (void)pool, (void)size;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_initialized = true;
    return 0;
}

DCP_SYSV int32_t rudp_end() {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& [id, c] : g_contexts) Release(id, c, "ended");
    g_contexts.clear();
    g_polls.clear();
    g_initialized = false;
    return 0;
}

DCP_SYSV int32_t rudp_get_status(void* status, uint64_t size) {
    if (status) std::memset(status, 0, size);
    return 0;
}

DCP_SYSV int32_t rudp_ok() { return 0; }

DCP_SYSV int32_t rudp_create_context(void* handler, void* arg, int32_t* ctx) {
    (void)handler, (void)arg;
    if (!ctx) return kRudpInvalid;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) return kRudpNotInit;
    *ctx = g_next_context++;
    g_contexts[*ctx] = Context{};
    return 0;
}

DCP_SYSV int32_t rudp_bind(int32_t ctx, int32_t s, uint16_t vport, uint8_t mux) {
    (void)mux;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_contexts.find(ctx);
    if (it == g_contexts.end() || !g_sockets.count(s)) return kRudpInvalid;
    it->second.socket = s;
    it->second.vport = vport;
    return 0;
}

DCP_SYSV int32_t rudp_set_option(int32_t ctx, int32_t option, const void* value, uint32_t size) {
    (void)option, (void)value, (void)size;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_contexts.count(ctx) ? 0 : kRudpInvalid;
}

// The game reads option 0xe after the first "writable" and closes the slot unless the call succeeds.
DCP_SYSV int32_t rudp_get_option(int32_t ctx, int32_t option, void* value, uint32_t size) {
    (void)option;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_contexts.count(ctx)) return kRudpInvalid;
    if (value && size) std::memset(value, 0, size);
    return 0;
}

DCP_SYSV int32_t rudp_initiate(int32_t ctx, const uint8_t* address, uint32_t size, int32_t flags) {
    (void)flags;
    if (!address || size < 16) return kRudpInvalid;
    uint16_t port;
    uint32_t ip;
    std::memcpy(&port, address + 2, 2);
    std::memcpy(&ip, address + 4, 4);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_contexts.find(ctx);
    if (it == g_contexts.end()) return kRudpInvalid;
    Context& c = it->second;
    c.peer = {__builtin_bswap32(ip), Swap16(port)};
    c.initiated = true;
    c.initiated_ms = net::now_ms();
    std::printf("Rudp: context %d initiate to %s\n", ctx, net::endpoint_text(c.peer).c_str());
    Attach(ctx, c);
    return 0;
}

DCP_SYSV int32_t rudp_read(int32_t ctx, void* buffer, uint64_t size, uint8_t flags, void* info) {
    (void)flags, (void)info;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_contexts.find(ctx);
    if (it == g_contexts.end()) return kRudpInvalid;
    Context& c = it->second;
    if (c.link < 0 || !g_peer) return c.terminated ? kRudpClosed : kRudpWouldBlock;
    std::vector<uint8_t> message;
    if (!g_peer->receive(c.link, message)) {
        return g_peer->state(c.link) == net::LinkState::kClosed ? kRudpClosed : kRudpWouldBlock;
    }
    if (message.size() > size) std::printf("Rudp: context %d: a %zu-byte message was cut to %llu bytes\n", ctx, message.size(), (unsigned long long)size);
    const size_t n = std::min<size_t>(message.size(), size);
    if (n && buffer) std::memcpy(buffer, message.data(), n);
    return int32_t(n);
}

DCP_SYSV int32_t rudp_write(int32_t ctx, const void* buffer, uint64_t size, uint8_t flags) {
    (void)flags;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_contexts.find(ctx);
    if (it == g_contexts.end() || (!buffer && size)) return kRudpInvalid;
    Context& c = it->second;
    if (c.link < 0 || !g_peer) return kRudpWouldBlock;
    const net::LinkState state = g_peer->state(c.link);
    if (state == net::LinkState::kClosed) return kRudpClosed;
    if (state != net::LinkState::kConnected || !g_peer->send(c.link, buffer, size_t(size))) return kRudpWouldBlock;
    return int32_t(size);
}

DCP_SYSV int32_t rudp_terminate(int32_t ctx) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_contexts.find(ctx);
    if (it == g_contexts.end()) return kRudpInvalid;
    Release(ctx, it->second, "terminated");
    g_contexts.erase(it);
    for (auto& [poll, members] : g_polls) members.erase(ctx);
    return 0;
}

DCP_SYSV int32_t rudp_poll_create(uint64_t size) {
    (void)size;
    std::lock_guard<std::mutex> lock(g_mutex);
    const int id = g_next_poll++;
    g_polls[id];
    return id;
}

DCP_SYSV int32_t rudp_poll_destroy(int32_t poll) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_polls.erase(poll) ? 0 : kRudpInvalid;
}

// op 1 add, 2 modify, 3 remove.
DCP_SYSV int32_t rudp_poll_control(int32_t poll, int32_t op, int32_t ctx, uint16_t events) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_polls.find(poll);
    if (it == g_polls.end()) return kRudpInvalid;
    if (op == 3) it->second.erase(ctx);
    else if (op == 1 || op == 2) it->second[ctx] = events;
    else return kRudpInvalid;
    return 0;
}

DCP_SYSV int32_t rudp_poll_wait(int32_t poll, PollEntry* out, uint64_t max, uint64_t timeout_us) {
    const uint64_t deadline = net::now_ms() + timeout_us / 1000;
    for (;;) {
        int32_t n = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_polls.find(poll);
            if (it == g_polls.end()) return kRudpInvalid;
            for (const auto& [ctx, requested] : it->second) {
                if (uint64_t(n) >= max || !out) break;
                auto c = g_contexts.find(ctx);
                const uint16_t ev = c == g_contexts.end() ? kError : Events(ctx, c->second);
                const uint16_t hit = ev & (requested | kError);
                if (!hit) continue;
                out[n++] = {ctx, requested, hit};
            }
        }
        if (n || net::now_ms() >= deadline) return n;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

struct Export {
    const char* name;
    void* function;
};

const Export kExports[] = {
    {"sceNetSocket", reinterpret_cast<void*>(&net_socket)},
    {"sceNetSetsockopt", reinterpret_cast<void*>(&net_setsockopt)},
    {"sceNetBind", reinterpret_cast<void*>(&net_bind)},
    {"sceNetSocketClose", reinterpret_cast<void*>(&net_socket_close)},
    {"sceRudpInit", reinterpret_cast<void*>(&rudp_init)},
    {"sceRudpEnd", reinterpret_cast<void*>(&rudp_end)},
    {"sceRudpGetStatus", reinterpret_cast<void*>(&rudp_get_status)},
    {"sceRudpEnableInternalIOThread", reinterpret_cast<void*>(&rudp_ok)},
    {"sceRudpSetEventHandler", reinterpret_cast<void*>(&rudp_ok)},
    {"sceRudpCreateContext", reinterpret_cast<void*>(&rudp_create_context)},
    {"sceRudpBind", reinterpret_cast<void*>(&rudp_bind)},
    {"sceRudpSetOption", reinterpret_cast<void*>(&rudp_set_option)},
    {"sceRudpGetOption", reinterpret_cast<void*>(&rudp_get_option)},
    {"sceRudpInitiate", reinterpret_cast<void*>(&rudp_initiate)},
    {"sceRudpRead", reinterpret_cast<void*>(&rudp_read)},
    {"sceRudpWrite", reinterpret_cast<void*>(&rudp_write)},
    {"sceRudpTerminate", reinterpret_cast<void*>(&rudp_terminate)},
    {"sceRudpPollCreate", reinterpret_cast<void*>(&rudp_poll_create)},
    {"sceRudpPollDestroy", reinterpret_cast<void*>(&rudp_poll_destroy)},
    {"sceRudpPollControl", reinterpret_cast<void*>(&rudp_poll_control)},
    {"sceRudpPollWait", reinterpret_cast<void*>(&rudp_poll_wait)},
};

} // namespace

std::shared_ptr<net::PeerSocket> peer_socket() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_peer;
}

UpnpView upnp_view() {
    std::lock_guard<std::mutex> lock(g_mutex);
    UpnpView view;
    if (!g_upnp) return view;
    const net::upnp::Status s = g_upnp->status();
    view.working = s.state == net::upnp::State::kWorking;
    view.mapped = s.state == net::upnp::State::kMapped;
    view.external = s.external;
    return view;
}

uintptr_t rudp_export(const std::string& symbol) {
    for (const Export& e : kExports)
        if (symbol == e.name) return reinterpret_cast<uintptr_t>(e.function);
    return 0;
}

} // namespace dcp::online
