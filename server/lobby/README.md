# Lobby server

Players meet here: accounts, friends, presence, rooms and the exchange of UDP endpoints. Game traffic goes directly
between players; only a pair that cannot reach each other uses the lobby's relay (`src/relay.js`, binary WebSocket
messages, no new binding or storage). The protocol is in [docs/ONLINE.md](../../docs/ONLINE.md).

`src/lobby.js` is the whole lobby. It runs in two hosts:

- `src/worker.js`: a Cloudflare Worker with one SQLite-backed Durable Object (WebSocket Hibernation, `ping`/`pong`
  answered without waking the object, the seat grace as a Durable Object alarm).
- `local/server.js`: the development server on Node.js 22.13 or later (`node:http` + `node:sqlite`), no account needed. The
  tests that load `src/worker.js` under a model of the Hibernation runtime (`test/worker.test.js`) need 22.15 or later and
  are skipped, visibly, on older versions.

No dependencies are installed for either the tests or the local server.

## Production

Players, the launcher and the admin all use the deployed Worker, whose address is `net::kDefaultLobbyUrl`
(`src/net/online_config.h`). A bug report travels the same way as the rest of the online service:

Player → launcher's Bug Report page → production Worker (`incident.file` on the lobby WebSocket) → Durable Object storage
(`incidents` table, log included) → `/admin` with the dashboard token → **Bugs** tab → view, copy or download the stored log.

`/admin` has two tabs: **Dashboard** (the metrics) and **Bugs** (the reports). Both use `DCP_DASHBOARD_TOKEN`, so the
Worker needs no new secret, binding or migration for bug reports: its tables are created when the object starts.

## Developer and testing only

The local server is for the automated tests and diagnostics; it is not part of any normal workflow.

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
3. Optional, for `/admin` (Dashboard and Bugs): `wrangler secret put DCP_DASHBOARD_TOKEN` (a long random value, at least
   16 characters; without it the admin API answers 503). A Worker that already has the token needs nothing more.
4. `wrangler deploy --dry-run` bundles the Worker without uploading it; `wrangler deploy` publishes it. The lobby is
   then at `wss://dcp-lobby.<your subdomain>.workers.dev/`.
5. Check it: `curl https://dcp-lobby.<your subdomain>.workers.dev/` answers `{"service":"dcp-lobby","proto":1}`, and
   the Connection test of a test instance started with `DCP_LOBBY` set to its address says the lobby is reachable.
6. To make it the default for players, set `kDefaultLobbyUrl` in `src/net/online_config.h`.

Updating the deployed Worker is `.\tools\worker.ps1` from the repository root (`.\tools\worker.ps1 -DryRun` bundles without
uploading). It runs `npx wrangler deploy` in this folder, the same as `wrangler deploy` here or `npm run deploy`; run
`wrangler deploy --dry-run` first to bundle it without uploading. Reports are read in `/admin`, tab Bugs, or with the same
token from `/admin/api/incidents`, `/admin/api/incidents/<ticket>` and `/admin/api/incidents/<ticket>/log`. An older
deployment may still hold an empty `reports` table from before the Bug Report page; nothing reads, writes or deletes it.

The Worker uses `CF-Connecting-IP` as each player's address.

## Story sessions (Early Story Online)

A host still in the opening story can host a story session room. The client marks it with the room int attribute
`0x7f01` (`STORY_SESSION_ATTR`, the host's story step plus one; `0` means not a story session) and the server keeps the room
hidden. Old clients would desync inside such a room, so the server enforces a client capability:

- `hello` accepts `story_session: true` (kept on the connection's attachment) and its reply `capabilities` carries
  `story_session: true` beside `relay_supported` and `relay_max_payload`. A client sends it when it supports story
  sessions, whatever its player's Early Story Online setting is; the setting only decides who hosts.
- `room.create` with the attribute above 0 needs that capability (`bad_request`, `story_session`) and always stores
  `FLAG_HIDDEN` (`0x10000000`), so the room stays out of random `room.search`; `room.search` with `code` still finds it.
  `room.set_internal` and `room.set_external` keep `FLAG_HIDDEN` on a story room, also when the game reopens its room.
- `room.set_external` by an owner without the capability cannot add the attribute (same error).
- `room.join`, by id or by code, refuses a client without the capability before it becomes a member: `incompatible`
  with `story_session_required: true`, `online_protocol` and the owner's `version`/`platform`. It is counted as a
  reject with reason `story_session`.

Rooms without the attribute behave as before. The protocol versions are unchanged. The host's live story step travels
as an ordinary room internal binary attribute (`room.set_internal` / `room.internal`); the server does not interpret it. Deploying this needs a Worker
redeploy (`.\tools\worker.ps1`); nothing in the repository deploys it.

### Early Story Online on the dashboard

`/admin/api/summary` carries a `story` section; the game sends nothing for it. A room is a story room by the one test the
lobby already uses (`isStorySession`: the `0x7f01` room attribute above 0), checked on the live room, so a room whose new
owner cleared the attribute stops counting.

- **Live** (from the `rooms` and `members` tables, never stored): `rooms` story rooms; `parties` those with two or more
  connected members; `waiting` those with exactly one; `players` connected members in them. A held seat (reconnect grace)
  is not connected, as for `in_rooms`, so a room whose seats are all held is a room but neither a party nor waiting.
- **Totals** since `since` (the first start after this was deployed; earlier use is unknown): `rooms_created` and
  `guest_joins` are the daily counters `story_rooms` and `story_guest_joins` in `metrics_daily` summed; `sessions` and
  `users` come from `story_users`.
- **`story_users`** is the one table that names a player: `account`, the latest `name`, `hosted` and `guested` counts,
  `first_used`, `last_used`. An account is recorded when it creates a story room or successfully joins one, once per room
  (the room remembers up to 64 accounts in `rooms.story_seen`, which disappears with it), so reconnects, resumes, leaving
  and rejoining the same room, presence, search and room updates add nothing. `guest_joins` still counts each successful
  join. Nothing records an address, endpoint, room id or join code, and a failed join records nothing. The summary lists
  the 50 most recent users and every account playing now, with a live `active` entry (`role`, connected `members`, room
  `age_ms`).

## Abuse and quota protection

Every protection below is invisible to a client that behaves, and none changes the protocol or needs a client update.

- **Edge** (`src/edge.js`): a WebSocket upgrade is counted against `WS_UPGRADE_LIMITER` (30 per address group per 60 s, IPv6
  per /64) before the Durable Object is reached. Over the limit the Worker answers 429 with `Retry-After`; the object is
  not invoked and nothing is written. The service answer at `/` and `/admin` are not upgrades and are not
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
- **Public Lobby** (`lobby.publish`, `lobby.list`, `friends.add` by handle): nothing is written, presence lives in the
  socket attachment. Publishing is limited per account (burst 6, then one per 10 s), reading per account (burst 6, then
  one per 5 s) and per address group (burst 60, then 3 per second; a refusal spends neither), and a request by handle per
  account (burst 10, then one per 15 s) on top of the usual friend limits. Answers hold at most 100 rows and an unchanged
  list is not sent again. Refusals are counted as `public_lobby_limited`. See `docs/ONLINE.md`.
- **Established sockets**: a frame costs an invocation before any code can refuse it, so the lobby bounds how many an abusive
  socket gets and keeps that state in the socket's attachment, which a rebuilt object still has. Before sign-in a socket
  is handled for 3 frames, text and binary together (the client sends one), and the 4th closes it with 1008 (`sign in first`).
  Unknown operations, malformed envelopes, oversized frames and `limit` answers are strikes (see Refusals). Once closed for
  abuse a socket is ignored without a read or a write. A text frame over 160 Ki characters is closed with 1009 by the Worker.
- **Relay**: a sender of malformed frames (bad magic, version or type, truncated, oversized, bad length, relay not negotiated)
  is allowed 16 and then one per minute; frames a client only sends around leaving a room (stale seat, wrong token, no room,
  unknown member, relay suspended) 200 and then one per second; past either its connection is closed with 1008. A sender over
  its connection budget (400 frames/s, 128 KiB/s, bursts 800 and 256 KiB) is allowed 256 such frames and then 4 per second;
  past that the relay is suspended for it in that room (`relay.closed`) and its next frames there count as stale. The room
  budget (four members at the client's cap: 1200 frames/s, 384 KiB/s, bursts 2400 and 768 KiB) and a destination with more
  than 256 KiB buffered only drop the frame: neither is the sender's doing. A destination that is offline or still resuming
  is not a fault.
- **Writes**: unchanged `udp`, `room.set_internal` and `room.set_external` updates write nothing and send no event, and a
  woken object reads its stored alarm instead of setting it again. A request that would change a row spends a budget of
  its socket (kept in the attachment, so hibernation does not refill it), its account and its address group (IPv6 per
  /64): room data and endpoints in a room 40 burst then 1/s, 160 then 4/s per address; friend changes 20 burst then 1 per
  3 s, 60 then 1/s per address. A refusal answers `limit` with `retry_ms`, spends nothing, writes nothing and is counted
  in memory as `write_limited`. `room.resume` spends none of it.
- **Refusals**: more than 30 `limit`, unknown-operation, oversized or malformed requests on one socket, refilled 1 per 2 s,
  close it with 1008, counted as `lobby_abuse_closed`. Its seat gets the usual grace.
- **Diagnostics** are counted in memory and written together at the next 15-minute alarm as `abuse` rows of
  `metrics_daily_dimensions` (no address, no account): `ws_upgrade_accepted`, `ws_upgrade_limited` (reported by the
  Worker on the next upgrade it forwards), `ws_upgrade_ip_cap`, `hello_accepted`, `hello_reconnect_deduped`,
  `hello_limited`, `room_create_accepted`, `room_create_limited`, `room_join_limited`, `relay_abuse_closed`,
  `party_report_limited`, `public_lobby_limited`, `write_limited`, `lobby_abuse_closed`, `ws_text_messages`, `ws_binary_messages`,
  `ws_control_limited`, `ws_preauth_closed`, `relay_overrun`, `relay_invalid`, `relay_suspended`, and for reports `incident_filed`, `incident_limited`,
  `incident_hourly_limited`, `incident_daily_limited`. They
  are in `/admin/api/history` under `dimensions.abuse` and on the dashboard. Counts of the last interval are lost if
  the object is evicted first. Rooms, members, accounts, friends, requests and reports
are SQLite tables in the Durable Object; per-connection state (presence blob, rate buckets) lives in each socket's
attachment and disappears with the socket.

## Bug reports

`src/incidents.js` holds the launcher's reports in two tables of the same Durable Object. `incidents` has one row per report
with only the fields the player previewed; `incident_quota` has salted hashes of the account and address per report, deleted
after 24 hours. A request is refused on an unknown field, a malformed one, or a size the Worker measured itself: 500 characters
of description and 64 KiB of log in UTF-8. Refused attempts spend only the in-memory limits in `src/abuse.js`; the quota is
counted in SQL because it must survive the object's hibernation. The largest log `incident.file` can carry, with JSON
escaping, fits its 160 KiB frame.

An account may file 2 accepted reports per rolling hour (`incident_hourly_limit`) and 10 per rolling 24 hours
(`incident_daily_limit`), counted from the timestamps in `incident_quota`; the hourly code is answered when both are reached.
The check, the report and its quota row are one transaction, so a failed write stores neither row and costs no quota.
The per-address and service-wide ceilings in `INCIDENT` are infrastructure protection, not a player quota: they answer `limit`,
and the per-address one stays above the account limit so one player cannot lock out others on a shared address. The counters
`incident_hourly_limited`, `incident_daily_limited` and `incident_limited` tell the refusals apart in `metrics_daily_dimensions`.

A report is stored as received or refused: the Worker removes and rewrites nothing, so the stored log is the one the player
previewed. The log itself is in the `incidents` row (`log_text`), not a path or a link, so a report stays viewable and
downloadable after the player deletes the local file. The newest 500 reports are kept: filing a report deletes any older
one beyond that in the same step, and nothing else is deleted. At most 500 x about 66 KiB is about 32 MiB, against a
SQLite-backed Durable Object's 10 GB of storage (Cloudflare's FAQ states 1 GB on the Free plan) and 2 MB per row.

In `/admin`, Bugs lists reports newest first (report ID, date, launcher version, game build, CUSA, operating system,
description preview) with filters for launcher version, game build and date. A report's detail shows the description, the
metadata, the original log file name, whether only the end of the log was sent, and the stored log. **Close report**, or the
selected report ID, closes it. The report ID comes from the stored report, never from the log: a log is the snapshot taken
when the report was built, so it can hold earlier report IDs but never its own. **Download log** saves
that stored text as `<ticket>-<log file>.log` (`text/plain; charset=utf-8`, `Content-Disposition: attachment`; the name is
built from the validated ticket and log file name only). **Copy log** copies the same text. Both go through the token; there
is no public or permanent link.

**Report status** is set in the detail: a status, a fixed version (Fixed only; leaving Fixed clears it) and an optional message of
500 characters, stored and shown as plain text. **Save status** posts to `/admin/api/incidents/<ticket>/status`; nothing is
saved until then, and a save that changes nothing keeps `status_updated_at`. The list has a Status column and filter.

The list reads Report ID, Status, Submitted, Launcher, Game build, Operating system, Description, so the status sits next to the
ID; CUSA is only in the detail. The description takes the remaining width and wraps. On narrower windows Operating system hides
below 1000 px and Launcher below 760 px; the detail still shows both.

**Delete report**, at the bottom of an open report and apart from the status form, permanently removes a test or unwanted
report. It asks first ("Delete DCP-XXXXXX permanently? This removes the stored report and log and cannot be undone."), then calls
`DELETE /admin/api/incidents/<ticket>` with the dashboard token (401 without it, 400 for a malformed id, 404 for an unknown
report, otherwise `{"deleted": "<ticket>"}`). It deletes the `incidents` row (fields and log) and the `incident_tracking` row
(status, message, fixed version, receipt hash) in one transaction. The player operations cannot delete, the quota rows are left
alone, and the newest-500 limit is unchanged. The owner's `incident.status` then leaves the report out and `incident.read`
answers `not_found`.

`incident_tracking` holds what a report's owner may read: the ticket, a salted SHA-256 of the receipt, the submission time, the
status, its time, the fixed version and the message. It has no account and no address, and the newest 5000 rows are kept,
apart from the 500 logs, so a status outlives its log. A report from before this table has no row and counts as New; saving
its status adds a row with an empty hash, which no receipt matches. See [docs/ONLINE.md](../../docs/ONLINE.md#report-status-and-my-reports).

## Usage statistics and the dashboard

`src/metrics.js` keeps anonymous aggregate service statistics in three tables of the same Durable Object. Everything
comes from the sign-in and room requests clients already make, except the optional party snapshot described below;
nothing else is collected (docs/ONLINE.md, Privacy and usage statistics).

| Table | Row | Columns |
|---|---|---|
| `metrics_daily` | one per day | `sessions`, `rooms_created`, `room_joins`, `incompatible_rejects`, `peak_online`, `peak_in_rooms`, `peak_rooms`, `rooms_closed`, `rooms_with_guest`, `rooms_relayed`, `party_total` and the six platform room counters below |
| `metrics_daily_dimensions` | day, dimension, value | `count`: sessions started per `platform`, `version` and `country`; `reject` counts rejects by reason; `abuse` counts protection events; `occupancy` (`1` to `4`) and `lifetime` (`under_1m`, `1_5m`, `5_15m`, `15_60m`, `over_1h`) count ended rooms; `character`, `host_character`, `party_size`, `party_composition` and `party_duplicate` count party snapshots (below) |
| `metrics_samples` | one per 15 minutes (UTC ms) | the highest `online`, `in_rooms` and `rooms` in the interval; 90 days kept |

- Days are America/Sao_Paulo calendar days (`YYYY-MM-DD`); samples are UTC timestamps.
- A **session** is a `hello` with `presence: true` that succeeded for an account with no counted session in the last
  10 minutes. A reconnect (or the launcher and the game opening in turn) inside that window is the same session; the
  Connection test (`presence: false`) and refused sign-ins are none. Sessions are not players: there is no unique-player, DAU or MAU
  count, and the daily platform, version and country counts are sessions started, not players. The one exception is
  Early Story Online (below), which keeps a table of its users.
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
- **Platform mixes**: a room keeps a bitmask of the platform categories (`platformCategory`) that ever sat in it, and at close a
  multiplayer room (largest party above 1) adds `rooms_cross_platform` (two or more categories), `rooms_android`,
  `rooms_android_cross`, `rooms_android_pc` (Android with Windows, Linux or Steam Deck), `rooms_android_relayed` and
  `rooms_android_cross_relayed`, and one value of the `platform_mix` dimension: the categories in the fixed order Windows, Linux,
  Steam Deck, Android, Other joined by `+` (at most 31 values). Single-player rooms, rooms open when this was deployed and
  reconnects add nothing, and a room is counted once when its last seat goes. `platform_mix_first_day` in the history says
  when it began, and the dashboard shows it under Multiplayer activity. Used relay is the `relayed` flag below: the relay
  carried at least one frame, so a room can be both direct and relayed.
- **Relay rooms** (`rooms_relayed`) are ended rooms in which the relay forwarded at least one frame; the room is marked
  with one write, never per packet. Peer-to-peer traffic is invisible to the lobby, so direct success is not measured.
- **History** (`/admin/api/history`) also returns `totals` for the range, `previous` for the same number of days
  before it, and `comparison` (current, previous and percent change; `null` when the previous value is 0) for sessions,
  rooms created, joins, rooms with a guest and the peaks.
- **Country** is Cloudflare's `request.cf.country`, set by the Worker in `X-DCP-Edge-Country` (replacing any value the
  client sent) and kept in the socket's attachment while it is open. Missing, `XX` or `T1` is `Unknown`. The local
  server counts every client as `Unknown`. Platforms other than Windows, Linux, Steam Deck and Android are `Other` (Android sessions
  before this was added were counted as `Other` and are not rewritten); versions that are not
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
(in `sessionStorage` only with "Keep for this tab") and never puts it in a URL. With **Keep for this tab** a reload of the same tab
signs in again from `sessionStorage` and returns to the same Dashboard or Bugs tab; another tab, a new window or a browser restart
asks again, and without it a reload always asks again. A refused token is removed at once. Once signed in the login panel gives way
to a menu button at the top right (Dashboard session, Sign out); **Sign out** clears the token and the saved tab. Nothing is
stored in `localStorage` or a cookie. Every `/admin` answer is `no-store`
with a restrictive Content-Security-Policy (own script and style only, no inline code, no framing), `nosniff` and
`no-referrer`, and no CORS headers.

```
curl -H "Authorization: Bearer <token>" https://dcp-lobby.<subdomain>.workers.dev/admin/api/summary
curl -H "Authorization: Bearer <token>" "https://dcp-lobby.<subdomain>.workers.dev/admin/api/history?days=90"
```
