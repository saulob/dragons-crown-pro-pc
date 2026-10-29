// Online play: settings, the import table, and the account services the game checks before it goes online (signed in,
// PS Plus, network connected). NpToolkit2 is answered here too, so the bundled module is not used for online play;
// its friend list is the lobby's (Friend::getFriends rows carry each friend's room info for "Join Friends Room").
#include "runtime_online.h"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "net/addr.h"
#include "net/online_version.h"
#include "online_settings.h"
#include "utf8.h"

#define DCP_SYSV __attribute__((sysv_abi))

extern "C" const char *runtime_symbol(const char *nid);

namespace dcp::online {
namespace {

Settings g_settings;
std::atomic<int32_t> g_next_request{0x100};

constexpr int32_t kNpInvalid = int32_t(0x80550003);
constexpr int32_t kNpUserNotFound = int32_t(0x80550004);
constexpr int32_t kSignedIn = 2;
constexpr int32_t kNetCtlIpObtained = 3;

template <class T> T Rd(const void* p, size_t offset) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t*>(p) + offset, sizeof(v));
    return v;
}
template <class T> void Wr(void* p, size_t offset, T v) { std::memcpy(static_cast<uint8_t*>(p) + offset, &v, sizeof(v)); }

bool PrimaryUser(int32_t user) { return user == 1; }

// ---- NpManager ----
DCP_SYSV int32_t np_ok() { return 0; }
DCP_SYSV int32_t np_new_request() { return g_next_request++; }
DCP_SYSV int32_t np_check_callback() {
    pump();
    return 0;
}
DCP_SYSV int32_t np_get_state(int32_t user, int32_t* state) {
    if (!state) return kNpInvalid;
    *state = PrimaryUser(user) ? kSignedIn : 1;
    return 0;
}
// SceNpId: online id (16 characters + terminator + 3 padding), then 16 reserved bytes.
DCP_SYSV int32_t np_get_np_id(int32_t user, uint8_t* id) {
    if (!id) return kNpInvalid;
    if (!PrimaryUser(user)) return kNpUserNotFound;
    std::memset(id, 0, 0x24);
    std::memcpy(id, g_settings.name.data(), std::min<size_t>(g_settings.name.size(), 16));
    return 0;
}
DCP_SYSV int32_t np_get_online_id(int32_t user, uint8_t* id) {
    if (!id) return kNpInvalid;
    if (!PrimaryUser(user)) return kNpUserNotFound;
    std::memset(id, 0, 0x14);
    std::memcpy(id, g_settings.name.data(), std::min<size_t>(g_settings.name.size(), 16));
    return 0;
}
DCP_SYSV int32_t np_get_account_id(int32_t user, uint64_t* account) {
    if (!account) return kNpInvalid;
    if (!PrimaryUser(user)) return kNpUserNotFound;
    *account = g_settings.identity.account;
    return 0;
}
DCP_SYSV int32_t np_get_country(int32_t user, char* country) {
    (void)user;
    if (!country) return kNpInvalid;
    std::memcpy(country, "us\0", 4);
    return 0;
}
DCP_SYSV int32_t np_user_of_account(uint64_t account, int32_t* user) {
    if (!user) return kNpInvalid;
    if (account != g_settings.identity.account) return kNpUserNotFound;
    *user = 1;
    return 0;
}
// Async requests finish at once with result 0.
DCP_SYSV int32_t np_poll_async(int32_t request, int32_t* result) {
    (void)request;
    if (result) *result = 0;
    return 0;
}
DCP_SYSV int32_t np_parental_info(int32_t request, int32_t user, int8_t* age, uint8_t* info) {
    (void)request, (void)user;
    if (age) *age = 20;
    if (info) std::memset(info, 0, 3);
    return 0;
}
// Byte 1 at result: the player has PS Plus, so the game never opens the store's upsell.
DCP_SYSV int32_t np_check_plus(int32_t request, const void* param, uint8_t* result) {
    (void)request, (void)param;
    if (result) result[0] = 1;
    return 0;
}
DCP_SYSV int32_t np_register_state_callback(void* callback, void* arg) {
    (void)callback, (void)arg;
    return 1;
}
DCP_SYSV int32_t np_in_game_message_init(uint64_t size, const void* param) {
    (void)size, (void)param;
    return 1;
}
DCP_SYSV int32_t np_in_game_message_handle(int32_t context) {
    (void)context;
    return 1;
}
DCP_SYSV int32_t np_in_game_message_send(int32_t context, int32_t handle, const void* to, const void* from, const void* data,
                                         uint64_t size) {
    (void)context, (void)handle, (void)to, (void)from, (void)data, (void)size;
    static std::once_flag once;
    std::call_once(once, [] { std::printf("Online: in-game messages between players are not delivered\n"); });
    return 0;
}

// ---- NetCtl: connected, IP obtained ----
DCP_SYSV int32_t netctl_get_state(int32_t* state) {
    if (!state) return int32_t(0x80412107);
    *state = kNetCtlIpObtained;
    return 0;
}
DCP_SYSV int32_t netctl_get_info(int32_t code, uint8_t* info) {
    if (!info) return int32_t(0x80412107);
    if (code == 2) {
        static const uint8_t kMac[6] = {0x02, 0xdc, 0x50, 0x00, 0x00, 0x01};
        std::memcpy(info, kMac, sizeof(kMac));
    } else if (code == 14) {
        std::memset(info, 0, 16);
        const std::string ip = net::ip_text(net::default_route_ip());
        std::memcpy(info, ip.data(), std::min<size_t>(ip.size(), 15));
    } else {
        std::memset(info, 0, 16);
    }
    return 0;
}
DCP_SYSV int32_t netctl_register(void* callback, void* arg, int32_t* id) {
    (void)callback, (void)arg;
    if (!id) return int32_t(0x80412107);
    *id = 1;
    return 0;
}
// {size, STUN status @4 (1 = done), NAT type @8, mapped address @0xc}.
DCP_SYSV int32_t netctl_nat(uint32_t* info) {
    if (!info) return int32_t(0x80412107);
    info[1] = 1;
    info[2] = 2;
    info[3] = 0;
    return 0;
}

// ---- dialogs reached on the online path ----
std::atomic<int32_t> g_signin_status{0};
DCP_SYSV int32_t signin_open(const void* param) {
    (void)param;
    g_signin_status = 3;
    return 0;
}
DCP_SYSV int32_t signin_status() { return g_signin_status.load(); }
DCP_SYSV int32_t signin_result(uint8_t* result) {
    if (result) std::memset(result, 0, 8);
    return 0;
}
std::atomic<int32_t> g_profile_status{0};
DCP_SYSV int32_t profile_init() {
    g_profile_status = 1;
    return 0;
}
DCP_SYSV int32_t profile_open(const void* param) {
    (void)param;
    std::printf("Online: profile dialog opened; it closes at once (no dialog UI)\n");
    g_profile_status = 3;
    return 0;
}
DCP_SYSV int32_t profile_status() { return g_profile_status.load(); }
DCP_SYSV int32_t profile_result(uint8_t* result) {
    if (result) std::memset(result, 0, 8);
    return 0;
}
DCP_SYSV int32_t profile_term() {
    g_profile_status = 0;
    return 0;
}

// ---- word filter: text passes unchanged ----
DCP_SYSV int32_t word_filter_sanitize(int32_t request, const char* in, char* out, void* option) {
    (void)request, (void)option;
    if (!in || !out) return kNpInvalid;
    std::memmove(out, in, std::strlen(in) + 1);
    return 0;
}

// ---- NpToolkit2 ----
// Getters of other results return an empty container (element count 0).
alignas(16) uint8_t g_empty[0x4000];
DCP_SYSV uint64_t tk_zero() { return 0; }
DCP_SYSV void* tk_empty() { return g_empty; }

std::mutex g_tk_mutex;
const void* g_friends_response = nullptr;
uint64_t g_friends_built = ~uint64_t(0);
alignas(16) uint8_t g_friends_container[0x20];
std::vector<uint8_t> g_friend_rows, g_friend_presences;

// Friend::getFriends rows (0x2e0 bytes): account @0, online id @8, online flag u32 @0x2c8 (1), presence type @0x2cc,
// presence list @0x2d0 and count @0x2d8. Presence entry (0x1e0 bytes): type @4, status @0x98 (0xc0 bytes), game data
// @0x158 (0x80 bytes, the host's room info), data size u64 @0x1d8. Only friends hosting a room of this build appear.
void BuildFriends() {
    uint64_t generation = 0;
    const std::vector<FriendRoom> friends = friends_result(&generation);
    if (generation == g_friends_built) return;
    g_friends_built = generation;
    g_friend_rows.assign(std::max<size_t>(friends.size(), 1) * 0x2e0, 0);
    g_friend_presences.assign(std::max<size_t>(friends.size(), 1) * 0x1e0, 0);
    for (size_t i = 0; i < friends.size(); ++i) {
        uint8_t* row = g_friend_rows.data() + i * 0x2e0;
        uint8_t* presence = g_friend_presences.data() + i * 0x1e0;
        Wr<uint64_t>(row, 0, friends[i].account);
        std::memcpy(row + 8, friends[i].name.data(), std::min<size_t>(friends[i].name.size(), 16));
        Wr<uint32_t>(row, 0x2c8, 1);
        Wr<uint32_t>(row, 0x2cc, 2);
        Wr<uint8_t*>(row, 0x2d0, presence);
        Wr<uint64_t>(row, 0x2d8, 1);
        Wr<uint32_t>(presence, 4, 2);
        std::memcpy(presence + 0x98, friends[i].status.data(), std::min<size_t>(friends[i].status.size(), 0xc0));
        const size_t n = std::min<size_t>(friends[i].data.size(), 0x80);
        std::memcpy(presence + 0x158, friends[i].data.data(), n);
        Wr<uint64_t>(presence, 0x1d8, n);
    }
    std::memset(g_friends_container, 0, sizeof(g_friends_container));
    Wr<uint8_t*>(g_friends_container, 8, g_friend_rows.data());
    Wr<uint64_t>(g_friends_container, 0x10, friends.size());
}

DCP_SYSV int32_t tk_get_friends(const void* request, const void* response) {
    (void)request;
    {
        std::lock_guard<std::mutex> lock(g_tk_mutex);
        g_friends_response = response;
    }
    friends_fetch();
    return 0;
}
DCP_SYSV uint64_t tk_is_locked(const void* response) {
    std::lock_guard<std::mutex> lock(g_tk_mutex);
    return response && response == g_friends_response && friends_busy() ? 1 : 0;
}
DCP_SYSV void* tk_friends(const void* response) {
    std::lock_guard<std::mutex> lock(g_tk_mutex);
    if (!response || response != g_friends_response) return g_empty;
    BuildFriends();
    return g_friends_container;
}
// Presence::Request::SetPresence: status @0x18 (0xc0 bytes), game data @0xe8, its size u64 @0x168.
DCP_SYSV int32_t tk_set_presence(const uint8_t* request, const void* response) {
    (void)response;
    if (request) presence_publish(request + 0xe8, size_t(Rd<uint64_t>(request, 0x168)), request + 0x18, 0xc0);
    return 0;
}
DCP_SYSV int32_t tk_delete_presence(const void* request, const void* response) {
    (void)request, (void)response;
    presence_clear();
    return 0;
}

struct Export {
    const char* name;
    void* function;
};

#define FN(name, fn) {name, reinterpret_cast<void*>(&fn)}
const Export kExports[] = {
    FN("sceNpCheckCallback", np_check_callback),
    FN("sceNpGetState", np_get_state),
    FN("sceNpGetNpId", np_get_np_id),
    FN("sceNpGetOnlineId", np_get_online_id),
    FN("sceNpGetAccountIdA", np_get_account_id),
    FN("sceNpGetAccountCountryA", np_get_country),
    FN("sceNpGetUserIdByAccountId", np_user_of_account),
    FN("sceNpCreateAsyncRequest", np_new_request),
    FN("sceNpCreateRequest", np_new_request),
    FN("sceNpPollAsync", np_poll_async),
    FN("sceNpAbortRequest", np_ok),
    FN("sceNpDeleteRequest", np_ok),
    FN("sceNpCheckNpAvailabilityA", np_ok),
    FN("sceNpGetParentalControlInfoA", np_parental_info),
    FN("sceNpCheckPlus", np_check_plus),
    FN("sceNpRegisterStateCallbackA", np_register_state_callback),
    FN("sceNpUnregisterStateCallbackA", np_ok),
    FN("sceNpRegisterPlusEventCallback", np_ok),
    FN("sceNpUnregisterPlusEventCallback", np_ok),
    FN("sceNpNotifyPlusFeature", np_ok),
    FN("sceNpSetNpTitleId", np_ok),
    FN("sceNpSetContentRestriction", np_ok),
    FN("sceNpInGameMessageInitialize", np_in_game_message_init),
    FN("sceNpInGameMessageTerminate", np_ok),
    FN("sceNpInGameMessageCreateHandle", np_in_game_message_handle),
    FN("sceNpInGameMessageDeleteHandle", np_ok),
    FN("sceNpInGameMessagePrepareA", np_ok),
    FN("sceNpInGameMessageSendDataA", np_in_game_message_send),
    FN("sceNpInGameMessageGetMemoryPoolStatistics", np_ok),
    FN("sceNetCtlGetState", netctl_get_state),
    FN("sceNetCtlGetInfo", netctl_get_info),
    FN("sceNetCtlRegisterCallback", netctl_register),
    FN("sceNetCtlUnregisterCallback", np_ok),
    FN("sceNetCtlCheckCallback", np_ok),
    FN("sceNetCtlGetNatInfo", netctl_nat),
    FN("sceSigninDialogInitialize", np_ok),
    FN("sceSigninDialogTerminate", np_ok),
    FN("sceSigninDialogOpen", signin_open),
    FN("sceSigninDialogUpdateStatus", signin_status),
    FN("sceSigninDialogGetResult", signin_result),
    FN("sceNpProfileDialogInitialize", profile_init),
    FN("sceNpProfileDialogOpenA", profile_open),
    FN("sceNpProfileDialogOpen", profile_open),
    FN("sceNpProfileDialogUpdateStatus", profile_status),
    FN("sceNpProfileDialogGetResult", profile_result),
    FN("sceNpProfileDialogTerminate", profile_term),
    FN("sceNpCommerceDialogGetResult", signin_result),
    FN("sceNpCommerceShowPsStoreIcon", np_ok),
    FN("sceNpCommerceHidePsStoreIcon", np_ok),
    FN("sceNpWordFilterCreateTitleCtxA", np_register_state_callback),
    FN("sceNpWordFilterDeleteTitleCtx", np_ok),
    FN("sceNpWordFilterCreateRequest", np_new_request),
    FN("sceNpWordFilterCreateAsyncRequest", np_new_request),
    FN("sceNpWordFilterDeleteRequest", np_ok),
    FN("sceNpWordFilterAbortRequest", np_ok),
    FN("sceNpWordFilterPollAsync", np_poll_async),
    FN("sceNpWordFilterSanitizeComment", word_filter_sanitize),
    // NpToolkit2 (C++ API: `this` first). Constructors, destructors and calls without results succeed and do nothing.
    FN("_ZN3sce7Toolkit2NP2V26Friend10getFriendsERKNS3_7Request10GetFriendsEPNS2_4Core8ResponseINS3_7FriendsEEE", tk_get_friends),
    FN("_ZN3sce7Toolkit2NP2V24Core12ResponseBase8isLockedEv", tk_is_locked),
    FN("_ZN3sce7Toolkit2NP2V24Core8ResponseINS2_6Friend7FriendsEE3getEv", tk_friends),
    FN("_ZN3sce7Toolkit2NP2V28Presence11setPresenceERKNS3_7Request11SetPresenceEPNS2_4Core8ResponseINS8_5EmptyEEE", tk_set_presence),
    FN("_ZN3sce7Toolkit2NP2V28Presence14deletePresenceERKNS3_7Request14DeletePresenceEPNS2_4Core8ResponseINS8_5EmptyEEE", tk_delete_presence),
    FN("_ZN3sce7Toolkit2NP2V24Core8ResponseINS2_6Friend12BlockedUsersEE3getEv", tk_empty),
    FN("_ZN3sce7Toolkit2NP2V24Core8ResponseINS2_8Presence8PresenceEE3getEv", tk_empty),
    FN("_ZN3sce7Toolkit2NP2V24Core8ResponseINS2_12ActivityFeed14PlayedWithFeedEE3getEv", tk_empty),
    FN("_ZN3sce7Toolkit2NP2V24Core8ResponseINS2_9Messaging12Notification16NewInGameMessageEE3getEv", tk_empty),
};
#undef FN

constexpr char kToolkitPrefix[] = "_ZN3sce7Toolkit2NP2V2";

} // namespace

const Settings& settings() { return g_settings; }

} // namespace dcp::online

using namespace dcp;

extern "C" void runtime_online_configure(const char* settings_file, const char* user_dir, const char* build) {
    online::Settings& s = online::g_settings;
    s.enabled = false;
    s.config = ReadOnlineConfig(settings_file ? settings_file : "");
    net::apply_environment(s.config);
    s.game = net::kOnlineGame;
    if (const char* code = std::getenv("DCP_JOIN_CODE"); code && *code) {
        for (const char* p = code; *p && s.join_code.size() < 8; ++p)
            if (std::isalnum(static_cast<unsigned char>(*p))) s.join_code += char(std::toupper(static_cast<unsigned char>(*p)));
    }
    if (!s.config.enabled) {
        std::printf("Online: off\n");
        return;
    }
    if (!build || !*build) {
        std::printf("Online: not available for an unknown game build; playing offline\n");
        return;
    }
    s.identity = net::load_identity(PathFromUtf8(user_dir ? user_dir : "user"));
    if (!s.identity.ok()) {
        std::printf("Online: no online identity (%s); playing offline\n", s.identity.error.c_str());
        return;
    }
    s.name = net::online_name(s.config.name);
    s.enabled = true;
    std::printf("Online: on as %s (account %llu), build %s, protocol %d, lobby %s, UDP port %s, UPnP %s%s%s\n",
                s.name.c_str(), (unsigned long long)s.identity.account, build, net::kOnlineProtocolVersion,
                s.config.lobby.empty() ? "not set" : s.config.lobby.c_str(),
                s.config.udp_port ? std::to_string(s.config.udp_port).c_str() : "automatic", s.config.upnp ? "on" : "off",
                s.join_code.empty() ? "" : ", join code ", s.join_code.c_str());
}

extern "C" int runtime_online_play_enabled(void) {
    return online::g_settings.config.enabled ? 1 : 0;
}

extern "C" int runtime_online_peer_ping_ms(void) {
    return online::g_settings.enabled ? online::peer_ping_ms() : -1;
}

extern "C" int runtime_online_active(void) {
    return online::g_settings.enabled ? 1 : 0;
}

extern "C" const char* runtime_online_system_type(const char* online_id) {
    thread_local std::string text;
    text = online::system_type(online_id ? online_id : "");
    return text.c_str();
}

extern "C" int runtime_online_take_notice(char* text, int size) {
    if (!online::g_settings.enabled || !text || size <= 0) return 0;
    const std::string notice = online::take_notice();
    if (notice.empty()) return 0;
    std::snprintf(text, size_t(size), "%s", notice.c_str());
    return 1;
}

extern "C" uintptr_t runtime_online_resolve(const char* name) {
    if (!online::g_settings.enabled || !name) return 0;
    const char* symbol = runtime_symbol(name);
    if (!symbol) return 0;
    const std::string s(symbol);
    for (const online::Export& e : online::kExports)
        if (s == e.name) return reinterpret_cast<uintptr_t>(e.function);
    if (const uintptr_t f = online::matching2_export(s)) return f;
    if (const uintptr_t f = online::rudp_export(s)) return f;
    // Every other NpToolkit2 entry the game imports (constructors, destructors, init, callbacks): success, no effect.
    if (s.rfind(online::kToolkitPrefix, 0) == 0) return reinterpret_cast<uintptr_t>(&online::tk_zero);
    return 0;
}

extern "C" int runtime_online_library(const char* name) {
    const char* library = name ? std::strchr(name, '#') : nullptr;
    if (!library) return 0;
    ++library;
    return !std::strncmp(library, "libSceNp", 8) || !std::strncmp(library, "libSceNet", 9) ||
           !std::strcmp(library, "libSceRudp");
}

extern "C" int runtime_online_check_imports(const char (*names)[128], const unsigned char* game_imports, uint64_t count,
                                            uintptr_t (*resolve)(const char* name, int is_data)) {
    if (!online::g_settings.enabled) return 1;
    uint64_t missing = 0;
    for (uint64_t i = 0; i < count; ++i) {
        if (!game_imports[i] || !runtime_online_library(names[i])) continue;
        // A Toolkit2 import left to the bundled module would run it uninitialized: it must be answered here.
        const bool toolkit = std::strstr(names[i], "#libSceNpToolkit2") != nullptr;
        if (toolkit ? runtime_online_resolve(names[i]) : resolve(names[i], 0)) continue;
        const char* symbol = runtime_symbol(names[i]);
        std::printf("Online: import %s (%s) has no implementation\n", names[i], symbol ? symbol : "unnamed");
        ++missing;
    }
    if (!missing) return 1;
    std::printf("Online: %llu online imports have no implementation; playing offline\n", (unsigned long long)missing);
    online::g_settings.enabled = false;
    return 0;
}
