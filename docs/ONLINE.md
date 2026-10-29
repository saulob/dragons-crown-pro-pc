# Online play

Dragon's Crown Pro's own multiplayer code runs unchanged: the port answers the console services it calls
(NpManager, NetCtl, NpMatching2, NpToolkit2, the P2P socket of libSceNet and libSceRudp). Players meet through a small
lobby; game traffic goes directly between their PCs over UDP. Nothing in the game executable is patched for online
play, and with online play off every import keeps its offline behaviour (`src/runtime_services.c`, the bundled
NpToolkit2 module).

Status: two instances on one PC play together through the online service (stages, loot, a player who disconnects
becomes a CPU ally). Play between PCs on different Internet connections (NAT traversal) has not been confirmed yet.

## Playing online

1. In the launcher's **Online** page, turn **Online play** On. The rest of the page appears only then.
2. Choose your **Online name**: what other players see (up to 16 letters, digits, `-` or `_`; `Player` until you change
   it). Changing it later keeps your friend code and your friends.
3. Share **Your friend code** (`XXXX-XXXX`, **Copy** puts it on the clipboard) with the players you want to meet.
4. Under **Friends**, enter a friend's code and **Send request**. A request to you appears as a **New friend request**
   card at the top of the page, and the Online entry in the sidebar shows how many are waiting; **Accept** or
   **Decline** it there. The list updates by itself and shows each friend's Online name and whether they are online:
   Online while their launcher or game is connected, "Online, in a room" while they are in a game room. Under the
   name, an online friend also shows the platform and port version their own client reported
   (`Online · Windows · v0.8.0`), and **Incompatible** when their room uses another online protocol.
5. **Connection test** > **Run test**, if you want to check your network first. "Ready for online play" means other
   players can reach you; otherwise the lines below it say what is missing. "Online service: Connected · N ms" is the
   time the online service took to answer the test's sign-in, measured once per test; it is not the Ping to players.
6. **Ping display** On shows the in-game Ping from the start; **F3** shows or hides it while playing.
7. Press **Play**.
8. Play through the game's own Network menu: host or join a room, or pick a friend's room in Join Friends Room.

Nothing else needs setting up. With Online play Off the game runs offline as before, and F3 does nothing.

### Ping

The in-game Ping is the round trip to the other players over the direct P2P link: the smoothed round trip the peer
transport already measures from its acknowledgements (see Peer transport), read about three times a second. It sends no
extra packets and makes no request to the online service. With several players it shows the highest round trip among
the connected ones, the worst connection in the room. "Ping: --" means no player is connected yet, or the last one
left. The panel uses the frame counter's style and corner, next to it when both are shown.

### System type and versions

The game's player cards (as in Join Friends Room) show **System type** as the player's PC platform and port
version, for example `Windows · v0.8.0`, `Linux · v0.8.0` or `Steam Deck · v0.8.0`, instead of PlayStation 4.
Steam Deck is recognised from the machine's DMI identity (Valve Jupiter or Galileo); any other Linux PC shows Linux. The
version is the CMake project version, the same one the launcher and the packages show.

Who can play together is decided by an internal online protocol number (`net::kOnlineProtocolVersion`,
`src/net/online_version.h`), not by the port version: v0.8.1 and v0.8.2 still play together as long as the number did
not change. It is raised only when a change stops a build from playing with the previous one. Rooms of another
protocol are left out of searches and of Join Friends Room, and joining one is refused by the online service before
any connection to the other player is made; the game shows "Online version mismatch" with both versions (or "This
player is using an incompatible version of Dragon's Crown Pro PC." when the other build is too old to say). Such
friends still appear in Friends, online and in a room. A build from before this check sends no protocol and counts as
0: it never meets a current build, only other builds like it.

The card's System type comes from a small hook (`src/system_type_hook.c`, US 1.00 and Japan 1.02/1.03): every copy of
the game marks its own player info as PS4, so the text cannot come from the data the players exchange. At the card's
two calls that set the PS4 record, the hook sets the text for the Online ID the card shows, from what the online service
reported for that player (`PC` when it reported nothing).

## Reference and licence

[OpenCrown-PC](https://github.com/Nezara/OpenCrown-PC) (another PC port of this game) documents how the game drives its
online code: the Matching2 request and event structures, the memberId-to-slot rule, the Rudp contract and the room
attributes. That repository has no licence (checked at commit `49844a54`: no LICENSE or COPYING file, no SPDX
headers, no licence field in package metadata; only third-party notices). No code was copied from it. This
implementation, its lobby protocol, wire format and server are written for this project, using OpenCrown's notes as a
description of the game's behaviour. Struct offsets are facts of the game's ABI and were cross-checked against the
calls the game makes.

## Pieces

| Part | Files | Role |
|---|---|---|
| Game services | `src/runtime_online.cpp` | settings, import table, NpManager (signed in, PS Plus), NetCtl (connected), NpToolkit2 (friends, presence), sign-in and profile dialogs, word filter |
| Matching2 | `src/runtime_matching2.cpp` | the game's room requests and callbacks over the lobby; endpoint report and peer introduction |
| P2P socket and Rudp | `src/runtime_rudp.cpp` | `sceNetSocket` type 6 ("RUDP_NetId"), `sceNetBind`, Rudp contexts, polls, reads and writes over `net::PeerSocket` |
| Network library | `src/net/` | JSON, identity, WebSocket client (WinHTTP; sockets and OpenSSL on Linux), lobby link, STUN, UPnP, peer transport, connection test |
| Lobby | `server/lobby/` | one JavaScript lobby for the Cloudflare Worker and for the local development server |
| Launcher | `launcher/online_page.cpp`, `launcher/online_view.cpp`, `launcher/online_settings.cpp` | Online page: online play, online name, friend code, connection test, friends |

Online play is available for the builds whose network imports match this implementation: CUSA10454 1.00 (USA, folder
`v1.03`), CUSA07001 1.02 and 1.03 (Japan). `GameBuild::online` marks them. CUSA07001 1.00 and CUSA10209 import three
NpToolkit2 entries this port does not name yet, so they stay offline. The lobby keeps each build apart (`hello.game` is
`<title id>-<version>`), so different builds never meet.

## Settings

`dcp.ini` (Online page) and environment overrides for test instances:

| Key | Variable | Default | Meaning |
|---|---|---|---|
| `online_enabled` | `DCP_ONLINE` | 0 | online play on |
| `online_name` | `DCP_ONLINE_NAME` | `Player` | in-game online id (16 of `A-Z a-z 0-9 - _`) |
| `online_udp_port` | `DCP_UDP_PORT` | 0 | 0: the game's port 3658, or any free one when it is taken |
| `online_stun` | `DCP_STUN` | `stun.cloudflare.com:3478,stun.l.google.com:19302` | STUN servers; `off` disables |
| - | `DCP_LOBBY` | `net::kDefaultLobbyUrl` | developer override of the production lobby (`ws://` or `wss://`), for diagnostics |
| - | `DCP_UPNP` | 1 | ask the router to map the UDP port; `0` for diagnostics |
| - | `DCP_JOIN_CODE` | - | the game's room search returns only the room with this code |

The lobby address and UPnP are not settings of the Online page or of `dcp.ini`: players always use the production
lobby, and UPnP is tried automatically next to STUN and hole punching; a router without UPnP does not stop online play.

The online identity is `<user folder>/online_identity.ini`: a random 63-bit account id and a 32-byte secret, made on
first use, never overwritten. The lobby stores only the secret's SHA-256 and refuses a known account with another
secret. The secret is never logged.

## How the game goes online

1. `sceNpCheckNpAvailabilityA`, `sceNpGetState` (signed in), `sceNpGetParentalControlInfoA`, `sceNpCheckPlus` (Plus).
2. `sceNpMatching2CreateContextA`, `ContextStart`: the lobby link opens and sends `hello`; the context callback gets
   0x6f02 when it is answered (with an error when the lobby is unreachable: the game stays offline).
3. `GetServerId`, `GetWorldInfoList` (one world, id 1), `SetUserInfo` (answered locally).
4. Every online player hosts a room first: `CreateJoinRoomA` (4 slots, flags 0x86000000, int attrs 0x4c..0x53, the
   0x78-byte room info as bin attrs 0x57 internal and 0x55 external). "Join Random Room" is `SearchRoom` with the
   game's own conditions (0x4c, 0x4d, packed 0x53), then `LeaveRoom` of its own room and `JoinRoomA`. A room is found by
   a random search only while its host is inside a quest stage (attr 0x4d <= 8); a host in town is found by friends
   (Toolkit2 `getFriends` rows) or by `DCP_JOIN_CODE`.
5. After the create/join answer the game opens its P2P socket (`sceNetSocket("RUDP_NetId", 2, 6)`, `sceNetBind` port
   3658, vport 4649) and registers the room and signaling callbacks.
6. The port reports the socket's endpoint (`udp`), receives each member's (`peer`) and raises signaling 0x5102 for a
   member once its endpoint is known and has not changed for 0.5 s. The game reads it with
   `SignalingGetConnectionStatus` and starts `sceRudpInitiate`; both ends do, at the same time.
7. Rudp contexts map to peer links: events 1 (readable), 2 (connected and writable), 8 (closed); one whole message per
   Read/Write, up to 0x40000 bytes.

Matching2 callbacks run only from `sceNpCheckCallback` (once per frame on a game thread), never inside a Matching2 call
and never with a port lock held: the game holds its own lock while calling Matching2 and takes it in its callbacks.

Member ids are 0x10, 0x20, 0x30, 0x40: the game computes `slot = (memberId >> 4) - 1`.

## Lobby protocol (version 1)

One WebSocket per process, text frames, one JSON object each. The literal text `ping` is answered `pong` (the client
sends it every 20 s; 45 s without any frame closes the socket). 64-bit ids are decimal strings, binary attributes
lower-case hex.

- Request `{"op": "...", "seq": n, ...}`; answer `{"re": n, "code": "ok" | <error>, ...}`; event `{"ev": "...", ...}`.
- Errors: `bad_request`, `too_large`, `no_session` (no hello yet), `proto`, `unauthorized`, `not_owner`, `in_room`,
  `not_found`, `closed`, `full`, `incompatible`, `limit`, `internal`. The port maps `not_found`/`closed`/`full` to the three Matching2
  errors the game tells apart (0x80550d0f / 0x80550d15 / 0x80550d19) and the rest to 0x80550ca1.. (ours).

| op | request | answer |
|---|---|---|
| `hello` | `proto` 1, `game`, `account`, `secret` (64 hex), `name`, `presence` (false: launcher session, not shown online), `online_protocol`, `version`, `platform` | `account`, `name`, `friend_code`, `ip` (as the lobby sees it), `time` |
| `worlds` | | `worlds: [{world, rooms, members}]` |
| `udp` | `port`, `lan: [{ip, port}]` (<= 4, LAN/loopback only) | `ip`, `port` |
| `room.create` | `world`, `slots` (1-4), `flags`, `ints: [[id, u32]]`, `bin_int`, `bin_ext`, `member_bin: [[id, hex]]` | `room`, `me` |
| `room.search` | `world`, `from` (1-based), `count` (<= 20), `flag_mask`, `flag_value`, `filters: [[id, op, value]]` (op 1 = 2 != 3 < 4 <= 5 > 6 >=); or `code` | `total`, `from`, `rooms`, `incompatible` (rooms of another protocol left out) |
| `room.join` | `room` or `code`, `member_bin` | `room`, `me`; `incompatible` with the room's `online_protocol` and its owner's `version`, `platform` |
| `room.leave` / `room.resume` | `room` / `room`, `member` | - / `room`, `me` |
| `room.set_internal` | `room`, `flag_mask`, `flag_value`, `bin_int` (owner only) | - |
| `room.set_external` | `room`, `ints`, `bin_ext` (owner only) | - |
| `room.kick` | `room`, `member` (owner only) | - |
| `chat` | `text` | - |
| `presence.set` | `data` (<= 0x80 bytes hex), `status` (<= 0xc0) | - |
| `friends.list` / `friends.requests` | | `friends: [{account, name, online, game?, online_protocol?, version?, platform?, room?, code?, data?, status?}]` / `incoming`, `outgoing` |
| `friends.add` | `code` (`XXXX-XXXX`, any case) | `account`, `name`, `state` (`pending` / `friends`) |
| `friends.accept` / `.decline` / `.remove` | `account` | `friend` / - / - |
| `report.send` | `text` (<= 256), `log` (tail kept, <= 128 K), `version` | `id`, `left` |

Room (`room`): `id`, `world`, `slots`, `flags` (0x20000000 added while full), `owner`, `code` (6 characters, A-Z 2-9
without I O 0 1), `key` (16 random bytes, hex: the peer handshake secret), `members: [{id, account, name, joined, bin, version, platform}]`,
`ints`, `bin_int`, `bin_ext`, `online_protocol` (its creator's). Search result rooms carry `count`, `owner_account`,
`owner_name`, `ints`, `bin_ext`, `online_protocol` and the owner's `version`, `platform`. Search skips full, closed
(0x40000000) and hidden (0x10000000) rooms, other builds' rooms and rooms of another online protocol.

Events: `room.member_joined {room, member}`, `room.member_left {room, member, why: leave | kicked | disconnect}`,
`room.owner {room, from, to}` (before `member_left`; the new owner is the lowest connected member), `room.internal
{room, flags, bin_int}` (not to the sender), `room.kicked`, `room.gone`, `peer {room, member, account, ip, port, lan?}`,
`peer.gone {room, member}`, `chat {room, member, account, name, text}` (to everyone in the room, the sender included),
`friend.request`, `friend.added`, `friend.removed`, `friend.online`, `friend.offline`.

Rules the lobby enforces:
- A peer endpoint's address is always the address of that player's own connection; only the port comes from the
  client. LAN candidates reach only members connecting from the same address. A client cannot point others elsewhere.
- Seat grace: when a greeted socket in a room closes, its seat is kept 30 s and room events for it are held (<= 32;
  more ends the seat at once). `room.resume` on a new socket of the same account retakes it and receives the held
  events after the answer. Afterwards it is a disconnect (owner migration, `peer.gone`, `member_left`). Chat is never
  held or stored.
- Limits: frames 32 KB (160 KB for `report.send`); names 16 characters; 16 bin attrs of <= 0x100 bytes per array;
  32 int attrs; 16 filters; 200 friends; 50 pending requests each way (expire after 14 days); chat 120 characters, 5
  burst then 1/s; 60 messages burst then 20/s per socket, room operations 30 burst then 1/s; 16 sockets and 20 new
  accounts per day per address (IPv6 per /64; loopback exempt on the development server); reports 5 per account, 10 per
  address, 300 in all per day, newest 1000 kept, addresses kept only as salted hashes.

## Peer transport

One UDP socket per game P2P socket. Every datagram starts with `'D' 'N'`, version 1, a type byte, the destination link
tag and the source link tag (u32 little endian). STUN answers on the same socket are told apart by their magic cookie.

| Type | Body |
|---|---|
| 1 HELLO / 2 HELLO_ACK | sender account u64, target account u64, link nonce u32, HMAC-SHA-256(room key, first 32 bytes)[0..16] |
| 3 DATA | seq u32, cumulative ack u32, SACK bits u32 (ack+1..ack+32), flags (1 first, 2 last fragment), <= 1150 bytes |
| 4 ACK | ack u32, SACK u32 |
| 5 PING / 6 PONG | time u32 (keepalive after 1 s without traffic) |
| 7 CLOSE, 8 PUNCH | - |

Both ends send HELLO to every candidate of the peer every 100 ms (250 ms after 3 s) for 15 s; the first authenticated
HELLO or HELLO_ACK fixes the address (a reply from an unexpected port is taken as it is: NATs that change ports). A
HELLO must name this player's account, a peer the lobby introduced, and carry the room key's MAC. DATA, ACK and PING
must carry both link tags. Retransmission: RTO from smoothed RTT (100 ms - 2 s, doubling per try), fast resend after
three later packets are acknowledged, congestion window 16-512 packets halved on loss. A link closes after 10 s without
any datagram, after 30 tries of one packet, or on CLOSE.

## NAT traversal

- STUN (RFC 5389 Binding) from the game's own socket: the mapped port is what peers will hit.
- The endpoint reported to the lobby: the local port with a LAN or local lobby; else the UPnP mapping when the
  router's WAN address is the address the lobby sees; else the STUN port; else the local port.
- LAN candidates (the loopback address and the PC's private addresses, local port) are sent too; peers behind the same
  public address try them first, so two PCs on one router, and two instances on one PC, connect without hairpinning.
- Punching: on every `peer` event a few PUNCH datagrams go to the peer's candidates, and both ends then send HELLOs
  at the same time.
- UPnP: SSDP discovery, WANIPConnection/WANPPPConnection `AddPortMapping` (UDP, lease 1 h renewed at half life, the next
  four ports on conflict, permanent lease if the router wants one), removed when the socket closes. LAN routers only.
- Not handled: two symmetric NATs (no relay); IPv6-only networks.

## Friends, presence, chat, reports

- Friend codes are 8 characters per account. Friendships are keyed by account, never by name.
- Presence is per account: online while any of its greeted `presence: true` sockets is open (the launcher's, the
  game's, a reconnected one), offline when the last one closes, never stored. A seat held for its grace period does not
  count. While online play is on, the launcher keeps one such link (`launcher/online_client.cpp`) for its whole
  window, not only the Online page; it re-reads the friends and requests on every `friend.*` event and every 30 s
  (declines and renames send no event), and is started again 1.5 s after the Online name stops changing. Off closes
  it. The Connection test uses its own short session with `presence: false`.
- The game publishes its room info with Toolkit2 `setPresence` (forwarded as `presence.set`); its "Join Friends Room"
  list is Toolkit2 `getFriends`, answered from `friends.list` with one row per friend online in a room of the same
  build and online protocol with a presence blob. The game then joins by the room id inside that blob.
- Chat is implemented in the lobby (room scoped, bounded, rate limited, not stored) and received lines are written to
  the game log (`Chat:`). The game has no chat UI of its own and the port has none yet.
- Bug reports: `report.send` is implemented in the lobby (`/reports` behind `REPORTS_TOKEN` on the Worker); the launcher
  has no report page yet.

## Developer and testing notes

Nothing in this section is needed for normal play.

Headless tests (no game, no window):

```
cd server\lobby; node --test "test/*.test.js"            # protocol, rooms, friends, grace, limits, real WebSockets
tools\test.ps1                                             # includes dcp-net-tests (transport over loopback, 15 % loss)
tools\online-lobby.ps1 -Port 8791                          # in another window, then:
$env:DCP_TEST_LOBBY='ws://127.0.0.1:8791/'; build\cmake\clang64\bin\dcp-online-hle-tests.exe
```

`dcp-online-hle-tests` calls the Matching2, libSceNet and Rudp entry points as the game does while a second player
joins through the lobby and connects over UDP.

Two instances on one PC, each with its own user folder (settings, saves, caches, identity), logs and UDP port, both
on the production service:

```
tools\online-instance.ps1 -Name a         # terminal 1: user\test-client-a, build\logs\test-client-a
tools\online-instance.ps1 -Name b         # terminal 2: user\test-client-b, build\logs\test-client-b
```

Online play is each instance's own switch; `-ForceOnline` sets `DCP_ONLINE=1` for the run, and the page then shows
the switch locked as "On". Each instance shows the Online name of its own Online page (set it there to
tell them apart); `-OnlineName` overrides it for that run. The instance labels (`test-client-a`) stay in folder names
and script output only.

This checks the production Worker and its Durable Object, identities, friends, presence, signaling, Matching2, the
exchange of endpoints, P2P/Rudp and gameplay. It does not check NAT traversal: both instances share one address, so
they connect through their LAN candidates. That needs two PCs on unrelated Internet connections, both on the
production service with nothing to set up:

1. Both Connection tests say "Ready for online play".
2. Exchange friend codes and become friends.
3. A hosts a room; B uses Join Friends Room.
4. Both logs show the P2P link and the Rudp contexts connected (lines below), with an address that is not a LAN one.
5. Play a whole stage: loot, area transitions, the stage results.
6. Disconnect one player, start again and join again.

`DCP_LOBBY` replaces the production lobby for a run, for diagnostics: `tools\online-lobby.ps1` runs the local
development lobby (Node.js 22.13 or later) on `ws://127.0.0.1:8790/`, which the headless tests above use. It keeps
accounts, friend codes, friends and pending requests in `user\online-test-lobby\lobby.sqlite`; rooms and presence are
never kept. `-Reset` deletes that file only; `-SeedTestFriends` makes instances a and b friends; `-Memory` keeps
nothing.

What to look for in each instance's log: `Online: on as ...`, `Matching2: signed in to the lobby`, `Matching2: created
room N`, `Net: P2P socket ... bound`, `Matching2: reporting UDP port`, `Matching2: member 0x20 established`, `P2P: link N
... connected via`, `Rudp: context N connected`, and at the end the `P2P: link ... closed` line with message counts.

## Deploying the lobby

See [server/lobby/README.md](../server/lobby/README.md). The production address is `net::kDefaultLobbyUrl`
(`src/net/online_config.h`), the only place it is written in the code.
