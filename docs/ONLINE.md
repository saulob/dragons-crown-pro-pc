# Online play

Dragon's Crown Pro's own multiplayer code runs unchanged: the port answers the console services it calls
(NpManager, NetCtl, NpMatching2, NpToolkit2, the P2P socket of libSceNet and libSceRudp). Players meet through a small
lobby; game traffic goes directly between their PCs over UDP, and through the lobby only for a pair of players that
cannot reach each other directly (see Relay). Nothing in the game executable is patched for online play, and with online play off every import keeps its offline behaviour (`src/runtime_services.c`, the bundled
NpToolkit2 module).

Status: two instances on one PC play together through the online service (stages, loot, a player who disconnects
becomes a CPU ally). Play between PCs on different Internet connections (NAT traversal) has not been confirmed yet.

## Playing online

1. In the launcher's **Online** page, turn **Online play** On. The rest of the page appears only then.
2. Choose your **Online name**: what other players see (up to 16 letters, digits, `-` or `_`; `Player` until you change
   it). Changing it later keeps your friend code and your friends.
3. Share **Your friend code** (`XXXX-XXXX`, **Copy** puts it on the clipboard) with the players you want to meet.
4. On the **Friends** tab, enter a friend's code and **Send request**. A request to you appears as a **New friend request**
   card at the top of the page, and the Online entry in the sidebar shows how many are waiting; **Accept** or
   **Decline** it there. The list updates by itself and shows each friend's Online name and whether they are online:
   Online while their launcher or game is connected, "Online, in a room" while they are in a game room. Under the
   name, an online friend also shows the platform and port version their own client reported
   (`Online · Windows · v0.9.0`), and **Incompatible** when their room uses another online protocol.
5. **Connection test** > **Run test**. PLAY waits for one finished test while Online play is On, whatever its result;
   until then the section is outlined in gold with **Connection test required** and a gold Run test button, and pressing
   PLAY opens the Online page at it;
   it is saved (`online_connection_test_port`), so later starts do not ask again. Turning Online play Off, or another
   UDP port (`online_udp_port`, `DCP_UDP_PORT`), asks for a new one. The test binds the game's UDP port (3658, or the
   configured one) and uses that one socket for STUN and the UPnP probe; when the port cannot be opened it says so
   instead of using another. On Windows this gives Windows Firewall the chance to ask for network access before the
   game starts. "Ready for online play" means other
   players can probably reach you, directly or through the relay; otherwise the lines below it say what is missing.
   "Online service: Connected · N ms" is the time the online service took to answer the test's sign-in, measured once
   per test; it is not the Ping to players. "Direct connections" says a UDP endpoint is available; whether a given player
   can really reach it is only known when joining a match. "Relay: Available" appears when the online service said in
   its sign-in answer that it relays (`capabilities.relay_supported`) and the UDP port opened; the test sends no relay
   frame, so it does not prove a path to any player.
6. **Ping display** On shows the in-game Ping from the start; **F3** shows or hides it while playing.
7. Press **Play**.
8. Play through the game's own Network menu: host or join a room, or pick a friend's room in Join Friends Room.

The game offers online play only after a character has passed the opening tutorial and story section, as on the PS4.
The Gameplay page's Story Skip Offer and Tutorial Skip Offer let a new character skip there.

Nothing else needs setting up. With Online play Off the game runs offline as before, and F3 does nothing.

### Bug reports

The launcher's Bug Report page sends a report only when the player presses Send report, and an optional preview shows exactly
what goes out. A report holds the player's description, the launcher version, the game build, game version and CUSA, the
operating system, the selected log's file name and that log after the privacy filter (`launcher/log_scrubber.cpp`), at
most its last 64 KiB. No personal user data is intentionally included, and the filter cannot promise to catch everything
a log might hold, which is why the page offers the preview.

The filter replaces IPv4 and IPv6 addresses (not loopback), the Windows or Linux account name, the computer name, the
profile folder, the folder the port runs from, other folders outside the system's (the file name stays), network paths,
the online secret, the account id, other players' account ids, room codes, Friend Codes and values labelled as passwords,
tokens or keys, and it drops chat lines. Ordinary diagnostics stay, including hashes of game files and driver versions.
The Online name the player chose is not removed, because a log shows it only for that player's own session.

To send, the launcher signs in with this PC's online account, as the Connection test does, without showing the player
online; a PC that never went online gets its random online identity then. The service uses the account and the
connection's address only to count the report limits: the quota table keeps salted hashes of them for 24 hours, and the
stored report holds neither.

An account may file 2 accepted reports per rolling hour and 10 per rolling 24 hours; a report leaves each window that long
after it was accepted, and only accepted reports count. The Worker alone decides: the launcher keeps no counter, shows no
numbers and leaves Send report available. A refused report is not stored, and the page keeps the description, the selected
log and any open preview. `incident_hourly_limit` shows "Too many reports were sent recently", `incident_daily_limit` shows
"Daily report limit reached"; with both reached the hourly one is answered. The service has further protections of its own,
which answer the plain `limit` code and show the same message as the hourly limit.

The Worker stores the report as received, with the log text itself in its Durable Object database, or refuses it; it filters
and rewrites nothing, so the preview, the request, the stored report, the admin view and the downloaded log are the same text.
Reports are readable only in the private `/admin` (`DCP_DASHBOARD_TOKEN`), tab Bugs, where each log can be viewed, copied or
downloaded as a `.log` file through the same token. The service keeps the newest 500; removing the local log does not remove
a sent report.

### Report status and My reports

A report's life: the player sends it and it starts as **New**; the developer sets a status in `/admin` → Bugs, with an optional
fixed version and a short message; the launcher checks later and My reports shows what changed.

| Status | Machine value | Meaning |
| --- | --- | --- |
| New | `new` | Not reviewed yet |
| Investigating | `investigating` | Being investigated |
| Fixed | `fixed` | The problem has been corrected; the only status that says so, and the only one with a fixed version |
| Cannot reproduce | `cannot_reproduce` | Could not be reproduced with the available information |
| Duplicate | `duplicate` | The same problem is already tracked by another report |
| Not a bug | `not_a_bug` | The behavior is expected, or not a Dragon's Crown Pro bug |
| Closed | `closed` | Intentionally finished without a specific fix or a more precise status. It does not mean Fixed |

Who owns a report is known only to the PC that sent it. An accepted report answers with a **receipt**: 256 random bits, a
capability for that one report, derived from nothing. The launcher keeps it in `user/bug_reports.json`, never in `dcp.ini` and
never in a log. The Worker stores only a salted SHA-256 of it. There is no account or address in the status table, no query by
account or address, and no list of one player's reports; the temporary quota hashes above stay a separate matter.

`incident.status` and `incident.read` take no `hello`, so a lookup never involves the account and Online Play can stay off. They
accept only a ticket with its receipt: a report ID alone reads nothing. A wrong receipt, an unknown ticket and a report from
before receipts all answer alike: left out of the list, or `not_found`. Lookups have their own limits, write nothing and
cost no report quota. Seen and unseen are local: the service never learns whether a player read a status.

My reports belongs to one installation. It is not an account feature: another PC, or a removed file, sees none of these reports.
Reports from before receipts have none and cannot be claimed, so they stay out of My reports; they still have a status in
`/admin`. The local list keeps the newest 50 reports, and **Remove from this PC** deletes only the local entry and its receipt.

Logs and statuses have separate limits. `incidents` keeps the newest 500 reports with their logs; `incident_tracking` keeps the
newest 5000 statuses, a row of at most about 2 KB, so at most about 11 MB. A status therefore outlives its log: once the log is
gone, **What was sent** says the report is no longer available, and the status still updates. Past both limits the launcher keeps
showing the last status it knew.

### Privacy and usage statistics

The online service stores anonymous aggregate usage statistics: player counts, sessions, rooms created and joined,
how many players each room reached and how long it lasted, how many rooms needed the relay,
the Dragon's Crown Pro PC version, the platform (Windows, Linux, Steam Deck) and the country. These statistics do not
store player names, Online names, Friend Codes, account IDs, IP addresses, room codes or friend lists, and there is no
per-player or per-session record. The same holds for the party statistics below.

- The country is worked out by Cloudflare from your connection and kept only as a count per day (for example "Brazil:
  12 sessions"). The service never keeps your city, region or address for these statistics, and the game never sends a
  country.
- A session is a sign-in of the launcher or the game to the online service that has no counted sign-in of the same
  account in the previous 10 minutes, so a quick reconnect is not counted again and sessions are not a number of players.
  There is no unique-player count.
- Gameplay traffic still goes directly between the players' PCs (P2P), except for a pair that cannot connect directly: the lobby forwards that pair's packets unread and unstored. Saves, character names, levels, equipment, gold,
  progress, quests, Ping and FPS are not collected. Apart from the optional party snapshot below, the game sends nothing
  extra for these statistics: they come from the sign-in and room requests it already makes.
- Only the maintainer can see them, on a private, password-protected page of the online service.

The online service itself still needs your account, Online name, friends and, while you are in a room, your address and
port to connect you to other players; that is described below and is not part of these statistics.

### Party statistics

The lobby can count which character classes appear in online parties. This is optional on both sides: a game build
sends nothing unless it implements it, a lobby that does not know the request answers `bad_request`, and a build that
never sends it signs in, hosts, joins, uses P2P and the relay and counts in every other statistic exactly as before.
Builds up to 0.9.1 do not send it and keep working unchanged; the lobby accepts it before any build does, so the
service can be updated first. Releases up to 0.9.1 never send it; the first build that does is the one that adds the
observer described below.

- When it is sent: only while Online Play is on and the game is signed in to the lobby, by the owner of a room that
  holds two or more players (a room of one is every online player's idle state, so it is not a party), once every
  player of the room has a live unit and the party has been the same for 3 seconds. The same party is not sent again in
  that room; a changed party is sent once it is stable, at most once a minute. Offline play, Online Play off, a missing
  link or an unrecognised game executable send nothing, and a refused or lost report changes nothing in the game.
- Where the classes come from: an observer at the entry of the game's per-tick update of player units, installed only
  while the game is online and found by signature in every supported build (`dcp_resolve_party`, the same site the
  Cheats hook, which stay off online). It reads each human player unit's slot and class and writes nothing. The slot of a
  room member is the game's own `(member id >> 4) - 1`; a report is made only when every member of the room has a unit
  in its slot, so computer-controlled allies are not part of it and a layout that does not fit sends nothing.
- The host's class is not sent: the report carries class numbers only.

- What is counted: one snapshot of the party, sent by the room's host on the lobby connection the game already has
  (no other connection, address or request exists for it): the class of each party member (Fighter, Amazon, Elf,
  Dwarf, Sorceress or Wizard; a class may repeat) and, when known, the host's class. The client sends class numbers
  only, never text, and the lobby derives everything it keeps from them.
- What is kept: daily counts of how often each class appeared, of host classes, of party sizes (1 to 4), of the
  party composition (the classes of a party in a fixed order, such as `Fighter+Elf`) and of parties with a repeated
  class. At most 227 different counts exist per day, whatever is sent. No player name, Online name, character name,
  Friend Code, account, address, room, session, installation or machine identifier, level, equipment, progress or
  free text is stored with them, and there is no per-player, per-account or per-room history or event log.
- How it is limited: the request passes the same sign-in and limits as every lobby request, then has its own budget per
  lobby, per account and per address, separate from creating and joining rooms; an identical repeat from the same
  connection is not counted twice. A client that reconnects can be counted twice for one party, which is accepted
  rather than keeping an identifier to prevent it.
- The transport is the authenticated lobby connection, so the service sees the connection's account and address while
  it handles the request, as it does for every request; the statistics it retains are aggregate counts without them.
  Only the maintainer can see the counts, on the private dashboard ("Characters & parties").
- Statistics begin when builds that report parties are used. Earlier rooms have no character figures, and the dashboard
  shows "No data yet" for a range without a report rather than zero use.
- Offline play and Online play off send nothing. If the lobby is unreachable or refuses the request, play continues:
  the snapshot is only a best-effort report.

### Ping

The in-game Ping is the round trip to the other players over the direct P2P link: the smoothed round trip the peer
transport already measures from its acknowledgements (see Peer transport), read about three times a second. It sends no
extra packets and makes no request to the online service. With several players it shows the highest round trip among
the connected ones, the worst connection in the room. "Ping: --" means no player is connected yet, or the last one
left. The panel uses the frame counter's style and corner, next to it when both are shown.

### System type and versions

The game's player cards (as in Join Friends Room) show **System type** as the player's PC platform and port
version, for example `Windows · v0.9.0`, `Linux · v0.9.0` or `Steam Deck · v0.9.0`, instead of PlayStation 4.
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

The card's System type comes from a small hook (`src/system_type_hook.c`, every supported build): every copy of
the game marks its own player info as PS4, so the text cannot come from the data the players exchange. At the card's
two calls that set the PS4 record, the hook sets the text for the Online ID the card shows, from what the online service
reported for that player (`PC` when it reported nothing).

## Reference and acknowledgement

[OpenCrown-PC](https://github.com/Nezara/OpenCrown-PC) by Nezara was an important technical reference during
development of Online Co-op.

Its research documents how Dragon's Crown Pro interacts with Matching2, RUDP, room attributes, member IDs and other
online interfaces. Struct offsets and similar ABI details were cross-checked against the calls the game makes.

OpenCrown's own lobby protocol documentation (`notes/map/lobby_protocol.md`) also influenced parts of this project's
lobby design, including reconnect and seat-grace behavior, event buffering, service limits, rate limits and related
operational choices.

This project's client, lobby protocol, server implementation, P2P transport and supporting code were written
separately for Dragon's Crown Pro PC, and no OpenCrown source code was copied.

Thanks to Nezara for publishing the research and implementation details that helped make the game's online behavior
easier to understand and reproduce.

## Pieces

| Part | Files | Role |
|---|---|---|
| Game services | `src/runtime_online.cpp` | settings, import table, NpManager (signed in, PS Plus), NetCtl (connected), NpToolkit2 (friends, presence), sign-in and profile dialogs, word filter |
| Matching2 | `src/runtime_matching2.cpp` | the game's room requests and callbacks over the lobby; endpoint report and peer introduction |
| P2P socket and Rudp | `src/runtime_rudp.cpp` | `sceNetSocket` type 6 ("RUDP_NetId"), `sceNetBind`, Rudp contexts, polls, reads and writes over `net::PeerSocket` |
| Network library | `src/net/` | JSON, identity, WebSocket client (WinHTTP; sockets and OpenSSL on Linux), lobby link, STUN, UPnP, peer transport, connection test |
| Lobby | `server/lobby/` | one JavaScript lobby for the Cloudflare Worker and for the local development server |
| Launcher | `launcher/online_page.cpp`, `launcher/online_view.cpp`, `launcher/online_settings.cpp` | Online page: online play, then the Settings (online name, friend code, connection test, ping), Friends and Lobby tabs |

Online play is available for every supported build. Their online imports are the same entry points except that
CUSA07001 1.00 and CUSA10209 1.00 call `ActivityFeed::getPlayedWith` (answered like the other Toolkit2 calls) where the
later builds call the TUS library. Before the imports are bound, the game checks that every import of an NP, network
or Rudp library that the game itself binds has an implementation (NpToolkit2 ones in this online layer, since the
bundled module is not set up for online play), and stays offline, logging each missing import, otherwise. Title user
storage is not provided: every TUS call, synchronous or not, answers signed out. Every build sends one lobby game
key (`net::kOnlineGame`), so players of different regions and versions meet; only the online protocol keeps them apart.
The System type, Story Skip Offer and Tutorial Skip Offer sites are verified for each local build
(`tests/system_type_tests.c`, `tests/skip_offers_tests.c`, `tests/online_builds_tests.cpp`); a supported build without
a known site (CUSA10454 1.01, CUSA10209 1.02, CUSA10487 1.03) uses the single place its image has the same verified code, and is left
unchanged when there is none.

## Settings

`dcp.ini` (Online page) and environment overrides for test instances:

| Key | Variable | Default | Meaning |
|---|---|---|---|
| `online_enabled` | `DCP_ONLINE` | 0 | online play on |
| `online_name` | `DCP_ONLINE_NAME` | `Player` | in-game online id (16 of `A-Z a-z 0-9 - _`) |
| `online_udp_port` | `DCP_UDP_PORT` | 0 | 0: the game's port 3658, or any free one when it is taken |
| `online_stun` | `DCP_STUN` | `stun.cloudflare.com:3478,stun.l.google.com:19302` | STUN servers; `off` disables |
| `online_connection_test_port` | - | 0 | the UDP port of the last finished Connection test; 0 after Online play is turned off |
| `show_in_lobby` | - | 0 | list the Online name in the public Lobby; Reset turns it off |
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
  `not_found`, `closed`, `full`, `incompatible`, `limit`, `incident_hourly_limit`, `incident_daily_limit`, `internal`. The port maps `not_found`/`closed`/`full` to the three Matching2
  errors the game tells apart (0x80550d0f / 0x80550d15 / 0x80550d19) and the rest to 0x80550ca1.. (ours).

| op | request | answer |
|---|---|---|
| `hello` | `proto` 1, `game`, `account`, `secret` (64 hex), `name`, `presence` (false: launcher session, not shown online), `online_protocol`, `version`, `platform`, `relay` (optional, true: this client can use the relay) | `account`, `name`, `friend_code`, `ip` (as the lobby sees it), `time`, `capabilities: {relay_supported, relay_max_payload}` (absent from an older lobby) |
| `worlds` | | `worlds: [{world, rooms, members}]` |
| `udp` | `port`, `lan: [{ip, port}]` (<= 4, LAN/loopback only), `public_ip` (optional, public IPv4 of a client connected over IPv6) | `ip` (as the lobby sees it), `port`, `public_ip` (when accepted) |
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
| `friends.add` | `code` (`XXXX-XXXX`, any case), or `player` (a `lobby.list` handle) | `account`, `name`, optional `display_name`, `state` (`pending` / `friends`) |
| `lobby.publish` | `visible` (bool) | `visible` |
| `lobby.list` | optional `rev` (the one last received) | `rev`, `total`, and `players` (`player`, `name`, `version`, `state`), or `unchanged: true` |
| `friends.accept` / `.decline` / `.remove` | `account` | `friend` / - / - |
| `incident.file` | `description` (<= 500 characters), `launcher_version`, `game_build`, `game_version`, `cusa`, `operating_system`, `log_file`, `log_clipped` (`yes`/`no`), `log_text` (<= 64 KiB of UTF-8); see Bug reports | `ticket` (`DCP-XXXXXX`), `receipt` (64 hex digits), `submitted_at` |
| `incident.status` | `reports: [{ticket, receipt}]` (<= 50); no `hello` needed | `reports: [{ticket, status, status_updated_at, fixed_version?, player_message?}]`, only the reports the receipts prove |
| `incident.read` | `ticket`, `receipt`; no `hello` needed | `report` (the stored fields, `ticket`, `submitted_at`), or `not_found` |
| `party.report` | `classes` (1-4 class numbers 0-5: Fighter, Amazon, Elf, Dwarf, Sorceress, Wizard), `host` (optional, one of `classes`); room owner only, optional (see Party statistics) | `counted` |

Room (`room`): `id`, `world`, `slots`, `flags` (0x20000000 added while full), `owner`, `code` (6 characters, A-Z 2-9
without I O 0 1), `key` (16 random bytes, hex: the peer handshake secret), `members: [{id, account, name, joined, bin, version, platform}]`,
`ints`, `bin_int`, `bin_ext`, `online_protocol` (its creator's). Search result rooms carry `count`, `owner_account`,
`owner_name`, `ints`, `bin_ext`, `online_protocol` and the owner's `version`, `platform`. Search skips full, closed
(0x40000000) and hidden (0x10000000) rooms, rooms of another game key and rooms of another online protocol.

Events: `room.member_joined {room, member}`, `room.member_left {room, member, why: leave | kicked | disconnect}`,
`room.owner {room, from, to}` (before `member_left`; the new owner is the lowest connected member), `room.internal
{room, flags, bin_int}` (not to the sender), `room.kicked`, `room.gone`, `peer {room, member, account, ip, port, lan?}`,
`peer.gone {room, member}`, `chat {room, member, account, name, text}` (to everyone in the room, the sender included),
`friend.request`, `friend.added`, `friend.removed`, `friend.online`, `friend.offline`.

Rules the lobby enforces:
- A member has two addresses. The observed one is that of its own connection: it never comes from the client, and it
  alone decides who shares LAN candidates and every per-address limit. The advertised one, `public_ip`, is optional and
  unverified, and is only ever a direct candidate for peers. It is honoured from a connection on IPv6, which peers cannot
  reach, for a public IPv4 found by STUN or UPnP; anything else is ignored, so a client on IPv4 cannot point others
  elsewhere. A `peer` event carries `public_ip` as `ip` when there is one, else the observed address.
- LAN candidates reach only members with the same observed address, whatever they advertise. Two members on different
  IPv6 addresses of one household do not get them: they use the public IPv4 or the relay.
- Each new advertised endpoint costs a token of the observed address (8, then one per 30 s, IPv6 per /64); past that it
  is ignored and the peers use the relay. Repeating an unchanged advertised endpoint sends peers nothing.
- Seat grace: when a greeted socket in a room closes, its seat is kept 30 s and room events for it are held (<= 32;
  more ends the seat at once). `room.resume` on a new socket of the same account retakes it and receives the held
  events after the answer. Afterwards it is a disconnect (owner migration, `peer.gone`, `member_left`). Chat is never
  held or stored.
- Limits: frames 32 KB (160 KB for `incident.file`); names 16 characters; 16 bin attrs of <= 0x100 bytes per array;
  32 int attrs; 16 filters; 200 friends; 50 pending requests each way (expire after 14 days); chat 120 characters, 5
  burst then 1/s; 60 messages burst then 20/s per socket, room operations 30 burst then 1/s; 16 sockets and 20 new
  accounts per day per address (IPv6 per /64; loopback exempt on the development server); 30 new connections per 60 s per
  address at the Worker (HTTP 429); per account, sign-ins 12 burst then 1 per 15 s, `room.create` 6 burst then 1 per
  20 s (20 burst then 1 per 6 s per address), `room.join` 20 burst then 1 per 5 s, so a reconnect does not reset them; party
  reports 3 burst then 1 per 2 minutes per account, 12 then 1 per 30 s per address and 30 then 1 per 30 s for the lobby; bug reports
  2 per account per rolling hour and 10 per rolling 24 hours, newest 500 kept; their attempt, address and service protections are
  in `server/lobby/src/abuse.js` and `incidents.js`.

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
- The endpoint reported to the lobby: the local port when the lobby sees a loopback, private, IPv6 unique-local or
  link-local address; else the UPnP mapping when the router's WAN address is the public IPv4 address; else the STUN port;
  else the local port. A global IPv6 lobby address is not a LAN: the client waits for STUN and UPnP like any other, and
  reports the public IPv4 they found in `public_ip`. With neither, it reports the local port and peers fall back to the
  relay.
- LAN candidates (the loopback address and the PC's private addresses, local port) are sent too; peers behind the same
  public address try them first, so two PCs on one router, and two instances on one PC, connect without hairpinning.
- Punching: on every `peer` event a few PUNCH datagrams go to the peer's candidates, and both ends then send HELLOs
  at the same time.
- UPnP: SSDP discovery, WANIPConnection/WANPPPConnection `AddPortMapping` (UDP, lease 1 h renewed at half life, the next
  four ports on conflict, permanent lease if the router wants one), removed when the socket closes. LAN routers only.
- Two symmetric NATs cannot be punched; such a pair uses the relay (below). Peer traffic is IPv4; a client with no public
  IPv4 uses the relay.

## Relay

Direct UDP is always tried first and is unchanged. The lobby's relay is a second path for one pair of players at a
time, used only when the direct one does not answer. There is no setting for it; it needs no router configuration. It
works the same on Windows and Linux and for every supported game build, because it lives below the game's Matching2 and
Rudp entry points, in `net::PeerSocket`.

- A client says `relay: true` in `hello` and uses the relay only when the answer carries
  `capabilities.relay_supported` with `relay_max_payload` of at least 1200. A client or lobby without the capability
  behaves exactly as before: direct only, with the same 15 s timeout.
- Per peer, not per room: a link tries the peer's direct candidates alone for 2.5 s (`Options::relay_after_ms`). If it
  is still not authenticated, the relay is offered beside them: the same authenticated HELLOs also go through the lobby.
  Direct is not cancelled; the first HELLO or HELLO_ACK that authenticates fixes the path of that link
  (`LinkPath`: `kConnectingDirect`, `kRelayOffered`, `kDirect`, `kRelay`, `kFailed`). A link that came up on the relay
  stays on it for the session; one that came up directly never uses it. In a room of three, A-B can be direct while A-C
  and B-C are relayed. The 15 s connection timeout is unchanged.
- A peer with no usable direct candidate (the lobby has no IPv4 endpoint for it, because it connects over IPv6 and found
  no public IPv4, and no LAN candidate) does not wait for the head start: its link is created at once and the relay is
  offered immediately, since there is nothing direct to try. The game is given `127.82.76.89` with that member as the port as
  the peer's address, which resolves only to that room member's account; an unknown member resolves to nobody. If the
  lobby offers no relay, the link ends at the usual 15 s timeout.
- The relay carries the existing `DN` datagrams byte for byte, so the HMAC, fragmentation, ACKs, retransmission and
  Rudp above it are unchanged. A relayed peer's address, as the link layer sees it, is `127.82.76.89` with the peer's
  room member as the port.
- Frame (a binary WebSocket message on the existing lobby connection): `"DR"`, version 1, type 1, room token u32, member
  u16, sequence u32, payload length u16, payload; big-endian, 16 bytes of header, payload 1 to 1200 bytes (a `DN`
  datagram is at most 1175). The sender puts the destination member; the lobby replaces it with the sender's member
  before forwarding. The token is the first four bytes of the room's key, which only its members know.
- The lobby forwards a frame only when the sender completed `hello` with `relay: true`, belongs to a room and still
  owns that seat, the token is the room's, and the destination is another live member of the same room that also asked
  for the relay and has the same online protocol. It never looks at the payload. Routes are cached in memory, rebuilt
  from the tables on a miss, and dropped when a member leaves, is kicked, disconnects or resumes its seat on a new
  socket, and when the room ends.
- Limits (`server/lobby/src/relay.js`, to be tuned after real measurements): per connection 400 frames/s and 128 KB/s
  (bursts 800 and 256 KB); per room twice that; a destination socket with more than 256 KB buffered gets nothing. Excess
  frames are dropped (the link's own retransmission covers them); 256 of them close the relay for that connection with a
  `relay.closed` event. A sender of frames that are never valid (malformed, wrong token, not in a room, stale seat,
  unknown member, no relay negotiated) is allowed 200 and then 5 per second before its connection is closed (1008);
  frames to a member that is offline or resuming are only dropped. Drops are logged for the first few of each reason, then one in a thousand.
- The client queues at most 512 frames for the socket (`kMaxQueuedBinary`); when full, or with no greeted socket, a
  frame is dropped and counted. A relayed frame is delivered to the link layer directly on receipt, so nothing queues
  on the way in.
- Relay frames pass through the lobby's Durable Object, so unlike signalling their volume grows with the traffic of
  relayed pairs. The relay exists only in the updated lobby: it has to be deployed before the relay can be used.

## Friends, presence, chat, reports

- Friend codes are 8 characters per account. Friendships are keyed by account, never by name.
- Presence is per account: online while any of its greeted `presence: true` sockets is open (the launcher's, the
  game's, a reconnected one), offline when the last one closes, never stored. A seat held for its grace period does not
  count. While online play is on, the launcher keeps one such link (`launcher/online_client.cpp`) for its whole
  window, not only the Online page; it re-reads the friends and requests on every `friend.*` event and every 30 s
  (declines and renames send no event), and is started again 1.5 s after the Online name stops changing. Off closes
  it. The Connection test uses its own short session with `presence: false`.
- The in-game Options menu's Online page (docs/LAUNCHER.md) only reads the game's own lobby link, never starts or
  serves it: its friend code is the one the hello answer carried and its friends are the last `friends.list` answer the
  game itself asked for. Only its Run test sends anything (the Connection test, on its own short session).
- The game publishes its room info with Toolkit2 `setPresence` (forwarded as `presence.set`); its "Join Friends Room"
  list is Toolkit2 `getFriends`, answered from `friends.list` with one row per friend online in a room of the same
  game key and online protocol with a presence blob. The game then joins by the room id inside that blob.
- Chat is implemented in the lobby (room scoped, bounded, rate limited, not stored) and received lines are written to
  the game log (`Chat:`). The game has no chat UI of its own and the port has none yet.
- Bug reports: the launcher's Bug Report page files them with `incident.file` on a short session of its own, so Online
  Play does not have to be on; see Bug reports.

## Public Lobby

**Show me in Lobby** on the Online page's Lobby tab is off until the player turns it on (`show_in_lobby` in `dcp.ini`;
Reset turns it off). It lists the player's Online name so others can send a friend request. It is not matchmaking, a room
browser or chat: a request made here is the normal `friends.add`, and nothing invites, creates or joins a room.

- **Presence** is the socket's attachment, not a table row, so publishing costs no SQL write. The launcher sends
  `lobby.publish` after each hello while the option is on. The launcher closes its link when the game starts, so the game
  asks with `lobby: true` in its own hello, read from the saved setting; a player in game stays listed and finds requests
  in the launcher next time. A reconnect is a new hello, so the game stays listed and the launcher publishes again.
- **Leaving**: `visible: false` removes every socket of the account at once, and a closed socket is gone at once. For a
  client that vanished, a row ends `lobbyStaleMs` (75 s) after the last sign of life: any request, or the 20 s keepalive,
  which the runtime answers without waking the object and which the Worker reads with
  `getWebSocketAutoResponseTimestamp`. Nothing is swept: stale rows are left out when the list is read.
- **What a row holds**: a random handle made for each publish, the shown name, the client's version (empty unless it is a
  plain version), and `state`, worked out for the reader (`none`, `pending`, `friend` or `incoming`). No account, friend
  code, address or platform. Sending a request
  gives the sender the target's account afterwards, as a friend code request always has: `friends.requests` needs it to
  cancel. An account is not a credential.
- **Who can read**: only a socket that is listed itself may call `lobby.list` or use a handle, and a handle only works
  while its player is listed; otherwise the code is `not_visible` or `not_found`. The player is never in their own list.
- **Identity**: the name and account are the session's. Nothing in these requests can set them, and a session name outside
  the `net::online_name` rules (`A-Z a-z 0-9 _ -`, 16 at most) is never listed.
- **Default name**: a player still called `Player` is shown as `Player-7A3C`: four characters of a hash of the account
  under the lobby's private salt, stable for that account and unrelated to its friend code. Friend rows and answers carry
  it as `display_name`, because the game matches room members by the real `name`, which never changes.
- **Limits**: 100 rows, requests first, then newest, with `total` counting all of them. An unchanged list is answered with
  `rev` alone. The lobby sends no list events: while the tab is on screen the launcher reads every 5 minutes, at once for
  **Refresh lobby** (one read at a time, 5 s apart) and after a friend event or a change in the requests. See
  `server/lobby/README.md` for the rate limits.
- **Older clients** never send these requests and see no change. Against an older lobby the launcher shows that the Lobby
  is not available and keeps working; the Worker has to be deployed before the feature can be used.

## Developer and testing notes

Nothing in this section is needed for normal play.

Headless tests (no game, no window):

```
cd server\lobby; node --test "test/*.test.js"            # protocol, rooms, friends, grace, limits, real WebSockets
tools\test.ps1                                             # dcp-net-tests (transport, relay fallback, failure paths), the Worker tests,
                                                           # the Linux net tests through WSL (-SkipLinux skips them), eight-build parity
tools\online-lobby.ps1 -Port 8791                          # in another window, then:
$env:DCP_TEST_LOBBY='ws://127.0.0.1:8791/'; build\cmake\clang64\bin\dcp-online-hle-tests.exe
```

`tools\test.ps1` starts a development lobby on a free loopback port for the net and build tests and prints a relay summary:
direct networking, relay transport, automatic fallback, Worker relay, protocol compatibility, failure paths, the
Connection test, address classification, endpoint selection, the endpoint path, Worker address security, the public
endpoint budget, Windows and Linux parity, and the eight builds. For the builds it separates structural parity (the same
code path and tables, no build-specific relay code; always checked) from runtime validation (needs the local boot images
in `build\image`; reported as DATA UNAVAILABLE or PARTIAL when they are missing). Nothing there starts the game, and the
production lobby is never contacted.

`dcp-online-hle-tests` calls the Matching2, libSceNet and Rudp entry points as the game does while a second player
joins through the lobby and connects over UDP. With `--endpoint-scenario <address>` it runs the path a public lobby takes:
the lobby treats it as coming from that address and a loopback stand-in answers STUN, and the test checks what a peer is
told. `tools\test.ps1` runs it for a global IPv6 and a public IPv4 address and also checks the log lines. It needs a
development lobby started with `--test-observed-address` (`/?observed=<address>` from loopback; the Worker has no such
input) and reports SKIPPED otherwise. Every socket in these tests is bound to loopback: they need no firewall rule and
never send anything to the public addresses they report.

Two instances on one PC, each with its own user folder (settings, saves, caches, identity), logs and UDP port, both
on the production service:

```
tools\online-instance.ps1 -Name a         # terminal 1: user\test-client-a, build\logs\test-client-a
tools\online-instance.ps1 -Name b         # terminal 2: user\test-client-b, build\logs\test-client-b
tools\online-instance.ps1 -Name a -Build CUSA10454-1.00     # pinned to one prepared build
tools\online-instance.ps1 -Name b -Build CUSA10209-1.00
```

`-Build <title ID>-<version>` pins an instance to one prepared build through `DCP_GAME_BUILD`, set for that instance
only: its launcher and game use that build's shared folders whatever another instance activates, its package selection
is off, and the game stops if the loaded image is another build. A build that is not prepared is an error, never the
active build. Without `-Build` an instance follows the active build, as the normal launcher does.

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
room N`, `Net: P2P socket ... bound`, `Matching2: reporting ... endpoint`, `Matching2: member 0x20 established`, `P2P: link N
... connected via`, `Rudp: context N connected`, and at the end the `P2P: link ... closed` line with message counts.

## Deploying the lobby

See [server/lobby/README.md](../server/lobby/README.md). The production address is `net::kDefaultLobbyUrl`
(`src/net/online_config.h`), the only place it is written in the code.
