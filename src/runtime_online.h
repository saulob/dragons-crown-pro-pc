// Online play (docs/ONLINE.md): the console services the game's own multiplayer code calls, over the lobby and the
// direct peer link. Everything here is used only when online play is on for a build that supports it; otherwise the
// imports keep their offline behaviour (runtime_services.c) and the bundled NpToolkit2 module.
#ifndef DCP_RUNTIME_ONLINE_H
#define DCP_RUNTIME_ONLINE_H
#include <stdint.h>

#include "runtime_online_overlay.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reads the online settings (dcp.ini in user_dir, DCP_* overrides) before the imports are bound. game_key names the
 * build for the lobby ("CUSA10454-1.00"); build_online is 0 for builds whose imports are not verified for online. */
void runtime_online_configure(const char *settings_file, const char *user_dir, const char *game_key, int build_online);
/* The online implementation of an import (NID#library), or 0: offline play, or not an online import. */
uintptr_t runtime_online_resolve(const char *name);

#ifdef __cplusplus
}

#include <memory>
#include <string>
#include <vector>

#include "net/identity.h"
#include "net/online_config.h"
#include "net/peer_socket.h"

namespace dcp::online {

struct Settings {
    bool enabled = false;
    net::OnlineConfig config;
    net::Identity identity;
    std::string name; // the in-game online id
    std::string game;
    std::string join_code; // DCP_JOIN_CODE: the game's room search finds only this room
};

const Settings& settings();

// Per-frame work: lobby messages, endpoint exchange and every Matching2 callback (sceNpCheckCallback).
void pump();
uintptr_t matching2_export(const std::string& symbol);
uintptr_t rudp_export(const std::string& symbol);

// The game's peer-to-peer socket (sceNetBind of the "RUDP_NetId" socket), when one is open.
std::shared_ptr<net::PeerSocket> peer_socket();
int peer_ping_ms();
struct UpnpView {
    bool working = false;
    bool mapped = false;
    net::Endpoint external;
};
UpnpView upnp_view();

// Toolkit2 presence and friends over the game's lobby connection.
struct FriendRoom {
    uint64_t account = 0;
    std::string name;
    std::vector<uint8_t> data, status;
};
void presence_publish(const uint8_t *data, size_t size, const uint8_t *status, size_t status_size);
void presence_clear();
void friends_fetch();
bool friends_busy();
std::vector<FriendRoom> friends_result(uint64_t *generation);

} // namespace dcp::online
#endif
#endif
