# Lobby server

Players meet here: accounts, friends, presence, rooms and the exchange of UDP endpoints. Game traffic goes directly
between players; only a pair that cannot reach each other uses the lobby's relay (`src/relay.js`, binary WebSocket
messages, no new binding or storage). The protocol is in [docs/ONLINE.md](../../docs/ONLINE.md).

`src/lobby.js` is the whole lobby. It runs in two hosts:

- `src/worker.js`: a Cloudflare Worker with one SQLite-backed Durable Object (WebSocket Hibernation, `ping`/`pong`
  answered without waking the object, the seat grace as a Durable Object alarm).
- `local/server.js`: the development server on Node.js 22.13 or later (`node:http` + `node:sqlite`), no account needed.

No dependencies are installed for either the tests or the local server.

## Local

The local server is for the automated tests and diagnostics; players and the normal two-instance test use the deployed
lobby.

```
node local/server.js                      # ws://127.0.0.1:8790/, data in memory
node local/server.js --db lobby.sqlite    # keeps accounts, friend codes and friends (rooms are removed at start)
node local/server.js --db lobby.sqlite --seed-friends a.ini --seed-friends b.ini   # also makes two identities friends
node local/server.js --host 0.0.0.0       # reachable from the home network
node --test "test/*.test.js"              # protocol tests
```

`tools\online-lobby.ps1` in the repository wraps the `--db` form with `user\online-test-lobby\lobby.sqlite`
(`-Reset` deletes it, `-SeedTestFriends`, `-Memory`).

## Cloudflare deployment (manual)

Nothing in the repository deploys or logs in. To publish a lobby:

1. Install the Wrangler CLI (`npm install -g wrangler`) and run `wrangler login` with your Cloudflare account.
2. In this folder, check `wrangler.toml` (name `dcp-lobby`, the `LOBBY` Durable Object with a SQLite migration, the
   `WS_UPGRADE_LIMITER` rate limit; its `namespace_id` must be unique in the account).
3. Optional, to read bug reports: `wrangler secret put REPORTS_TOKEN` (a long random value).
4. Optional, for the metrics dashboard: `wrangler secret put DCP_DASHBOARD_TOKEN` (a long random value, at least 16
   characters; without it the dashboard's API answers 503).
5. `wrangler deploy --dry-run` bundles the Worker without uploading it; `wrangler deploy` publishes it. The lobby is
   then at `wss://dcp-lobby.<your subdomain>.workers.dev/`.
6. Check it: `curl https://dcp-lobby.<your subdomain>.workers.dev/` answers `{"service":"dcp-lobby","proto":1}`, and
   the Connection test of a test instance started with `DCP_LOBBY` set to its address says the lobby is reachable.
7. To make it the default for players, set `kDefaultLobbyUrl` in `src/net/online_config.h`.

Reports: `curl -H "Authorization: Bearer <token>" https://dcp-lobby.<subdomain>.workers.dev/reports` (newest 100).

The Worker uses `CF-Connecting-IP` as each player's address.

## Abuse and quota protection

Every protection below is invisible to a client that behaves, and none changes the protocol or needs a client update.

- **Edge** (`src/edge.js`): a WebSocket upgrade is counted against `WS_UPGRADE_LIMITER` (30 per address group per 60 s, IPv6
  per /64) before the Durable Object is reached. Over the limit the Worker answers 429 with `Retry-After`; the object is
  not invoked and nothing is written. The service answer at `/`, `/admin` and `/reports` are not upgrades and are not
  counted. The binding is per Cloudflare location and eventually consistent, so the limit is approximate. Without the
  binding (development) or when it fails, upgrades pass.
- **Sign-in** (`src/abuse.js`): after the secret matched, one account may sign in 12 times in a burst and then once per
  15 s. A wrong secret never counts, so nobody can lock another account out. The existing account row is rewritten only
  when the name changed or its `seen` is 15 minutes old; `accounts.session` (added by a migration) holds the last
  counted session, so a reconnect within 10 minutes is neither a new session nor a write, even after the object was
  evicted.
- **Rooms**: `room.create` is limited per account (burst 6, then one per 20 s) and per address group (burst 20, then one
  per 6 s); `room.join` per account (burst 20, then one per 5 s, failed joins are free). `room.search` and `room.leave`
  are not limited beyond the existing per-socket buckets. These limits live in the object's memory.
- **Refusals** counted for the dashboard (`lobby_protocol`, `online_protocol`) are written once per reason and
  account or address per 10 minutes.
- **Party snapshots** (`party.report`, see Usage statistics): over the lobby's one WebSocket like every other request,
  so they pass the same edge, sign-in and per-socket limits first. They then have budgets of their own, separate from
  `room.create` and `room.join`: the whole lobby (burst 30, then one per 30 s), one account (burst 3, then one per 2
  minutes) and one address group (burst 12, then one per 30 s). A refusal spends none of the three and is counted as
  `party_report_limited`; a repeat of the last snapshot of the same connection and room is answered without
  counting and without spending a token. At most 227 rows can exist per day (see below) however many are sent.
- **Relay**: a sender that keeps producing frames no well-behaved client sends (malformed, wrong token, no room,
  stale seat, unknown member, relay not negotiated, no sign-in) is allowed 200 of them and then 5 per second; past that
  its connection is closed with 1008. A destination that is offline or still resuming is not a fault.
- **Writes**: unchanged `udp`, `room.set_internal` and `room.set_external` updates write nothing, and a woken object
  reads its stored alarm instead of setting it again.
- **Diagnostics** are counted in memory and written together at the next 15-minute alarm as `abuse` rows of
  `metrics_daily_dimensions` (no address, no account): `ws_upgrade_accepted`, `ws_upgrade_limited` (reported by the
  Worker on the next upgrade it forwards), `ws_upgrade_ip_cap`, `hello_accepted`, `hello_reconnect_deduped`,
  `hello_limited`, `room_create_accepted`, `room_create_limited`, `room_join_limited`, `relay_abuse_closed`,
  `party_report_limited`. They
  are in `/admin/api/history` under `dimensions.abuse` and on the dashboard. Counts of the last interval are lost if
  the object is evicted first. Rooms, members, accounts, friends, requests and reports
are SQLite tables in the Durable Object; per-connection state (presence blob, rate buckets) lives in each socket's
attachment and disappears with the socket.

## Usage statistics and the dashboard

`src/metrics.js` keeps anonymous aggregate service statistics in three tables of the same Durable Object. Everything
comes from the sign-in and room requests clients already make, except the optional party snapshot described below;
nothing else is collected (docs/ONLINE.md, Privacy and usage statistics).

| Table | Row | Columns |
|---|---|---|
| `metrics_daily` | one per day | `sessions`, `rooms_created`, `room_joins`, `incompatible_rejects`, `peak_online`, `peak_in_rooms`, `peak_rooms`, `rooms_closed`, `rooms_with_guest`, `rooms_relayed`, `party_total` |
| `metrics_daily_dimensions` | day, dimension, value | `count`: sessions started per `platform`, `version` and `country`; `reject` counts rejects by reason; `abuse` counts protection events; `occupancy` (`1` to `4`) and `lifetime` (`under_1m`, `1_5m`, `5_15m`, `15_60m`, `over_1h`) count ended rooms; `character`, `host_character`, `party_size`, `party_composition` and `party_duplicate` count party snapshots (below) |
| `metrics_samples` | one per 15 minutes (UTC ms) | the highest `online`, `in_rooms` and `rooms` in the interval; 90 days kept |

- Days are America/Sao_Paulo calendar days (`YYYY-MM-DD`); samples are UTC timestamps.
- A **session** is a `hello` with `presence: true` that succeeded for an account with no counted session in the last
  10 minutes. A reconnect (or the launcher and the game opening in turn) inside that window is the same session; the
  Connection test (`presence: false`) and refused sign-ins are none. Sessions are not players: there is no unique-player, DAU or MAU
  count, and the daily platform, version and country counts are sessions started, not players.
- **Live** numbers are never stored: they come from the open sockets and the `rooms`/`members` tables when asked.
  Online now and the live breakdowns count accounts with an open presence connection (one per account), in rooms
  counts accounts seated with a connection, open rooms counts rooms. A socket that disappears without a close callback,
  or a Durable Object restart, leaves nothing stale.
- Peaks and samples are written only when a value grows (after `hello`, `room.create`, `room.join`, `room.resume`)
  and at each 15-minute alarm while someone is connected, never per message. Old samples go 500 at a time.
- **Rejects** count only explicit protocol refusals: `hello` with another lobby protocol (once per connection) and
  `room.join` of a room with another online protocol. Rooms only left out of a search are not counted.
- **Room lifecycle** is counted once per room, when its last seat is gone (the last player left, or the 30 s seat grace
  ended). `rooms_closed` counts those rooms; `rooms_with_guest` the ones whose largest party was 2 or more (the rest
  never had a guest); `party_total` is the sum of largest parties, so the average largest party is
  `party_total / rooms_closed`. `occupancy` is the largest party (seats taken at once, 1 to 4) and `lifetime` the time
  from creation to that moment, in five fixed buckets. The room table carries two counters for this, `peak_members`
  and `relayed`, which disappear with the room; nothing in the metrics names a room. Rooms already open when this was
  deployed (`peak_members` 0) are not counted. Failed or malformed requests never reach these counters.
- **Relay rooms** (`rooms_relayed`) are ended rooms in which the relay forwarded at least one frame; the room is marked
  with one write, never per packet. Peer-to-peer traffic is invisible to the lobby, so direct success is not measured.
- **History** (`/admin/api/history`) also returns `totals` for the range, `previous` for the same number of days
  before it, and `comparison` (current, previous and percent change; `null` when the previous value is 0) for sessions,
  rooms created, joins, rooms with a guest and the peaks.
- **Country** is Cloudflare's `request.cf.country`, set by the Worker in `X-DCP-Edge-Country` (replacing any value the
  client sent) and kept in the socket's attachment while it is open. Missing, `XX` or `T1` is `Unknown`. The local
  server counts every client as `Unknown`. Platforms other than Windows, Linux and Steam Deck are `Other`; versions that are not
  a dotted number such as `0.8.0` are `Other`, and after 64 values in a day new ones are `Other` too.

**Party snapshots.** `party.report` is an optional request: a client that never sends it signs in, creates and joins
rooms, uses P2P and the relay and appears in every other figure exactly as before, and the sign-in answer, the lobby
protocol and the online protocol are unchanged (an object from before this operation answers it `bad_request`, which a
client treats as "not collected"). It carries only numbers:

```
{"op": "party.report", "seq": 7, "classes": [0, 2, 3], "host": 0}
```

- `classes`: 1 to 4 class indexes of the game (0 Fighter, 1 Amazon, 2 Elf, 3 Dwarf, 4 Sorceress, 5 Wizard), in any
  order; a class may repeat. `host` (optional): the class index of the room's owner, which must be one of `classes`.
  Anything else is `bad_request`: a missing or non-array `classes`, another size, an index outside 0 to 5 or not an
  integer, a `host` that is not in the party, any other field, and any repeated key. The server never reads a name,
  a string or a count from the request.
- Only the room's owner may send it, on a connection with `presence: true` that is in a room, and no more classes than
  the room has slots (`not_found`, `not_owner`, `bad_request`). The Connection test never qualifies. The owner, not
  every member, reports, so a party is counted once.
- It is answered `{"counted": true}`, or `{"counted": false}` for an exact repeat from the same connection in the same
  room. That repeat check lives in the connection's attachment (gone with the socket), so a reconnect can count a party a
  second time: a small duplicate is accepted in preference to keeping any identity.
- Accepted snapshots are added to today's rows of `metrics_daily_dimensions`, in at most eight upserts, with values the
  server builds from the class indexes: `character` (each class in the party, so a repeated class counts twice),
  `host_character`, `party_size` (`1` to `4`), `party_composition` (the classes in the game's index order joined with `+`,
  such as `Fighter+Elf+Elf`; 209 possible values) and `party_duplicate` (`yes` or `no`). The dimensions hold 6, 6, 4, 209
  and 2 values, 227 rows per day at most, and a value outside those sets cannot be written. Guest classes are not stored
  separately: they are `character` minus `host_character`. Seat order is not kept. The 64-value cap of the other
  dimensions does not apply to them.
- Nothing about the sender or the room is stored with the counts, and no event log exists. `/admin/api/history` returns
  the dimensions and `party_first_day`, the first day any snapshot was counted (`null` before the first one), so the
  dashboard can tell "nothing reported yet" from "nothing in this range". Rooms from before the first snapshot have no
  character figures at all.
- A failure to write the counts never fails the request.

The tables are created with `CREATE TABLE IF NOT EXISTS` when the Durable Object starts, after the existing schema and
migrations; the four room columns are added to `metrics_daily` (and `peak_members`, `relayed` to `rooms` by migration
3) only when missing, with default 0, so no existing row is changed or emptied, and no Wrangler migration is needed. They are plain tables, so
Cloudflare's Durable Object data browser shows them too.

**Dashboard**: `https://dcp-lobby.<subdomain>.workers.dev/admin` (the production lobby: `/admin` on
`https://dcp-lobby.saulobenigno.workers.dev`). The page and its script and stylesheet are public static files; the
numbers come from `GET /admin/api/summary` and `GET /admin/api/history?days=N` (1 to 365, 30 by default), which need
`Authorization: Bearer <DCP_DASHBOARD_TOKEN>`. The page asks for the token in a password field, keeps it in memory
(in `sessionStorage` only with "Keep for this tab") and never puts it in a URL. Every `/admin` answer is `no-store`
with a restrictive Content-Security-Policy (own script and style only, no inline code, no framing), `nosniff` and
`no-referrer`, and no CORS headers.

```
curl -H "Authorization: Bearer <token>" https://dcp-lobby.<subdomain>.workers.dev/admin/api/summary
curl -H "Authorization: Bearer <token>" "https://dcp-lobby.<subdomain>.workers.dev/admin/api/history?days=90"
```
