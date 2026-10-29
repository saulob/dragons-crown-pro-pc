# Lobby server

Players meet here: accounts, friends, presence, rooms and the exchange of UDP endpoints. Game traffic never goes
through the lobby. The protocol is in [docs/ONLINE.md](../../docs/ONLINE.md).

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
2. In this folder, check `wrangler.toml` (name `dcp-lobby`, the `LOBBY` Durable Object with a SQLite migration).
3. Optional, to read bug reports: `wrangler secret put REPORTS_TOKEN` (a long random value).
4. Optional, for the metrics dashboard: `wrangler secret put DCP_DASHBOARD_TOKEN` (a long random value, at least 16
   characters; without it the dashboard's API answers 503).
5. `wrangler deploy --dry-run` bundles the Worker without uploading it; `wrangler deploy` publishes it. The lobby is
   then at `wss://dcp-lobby.<your subdomain>.workers.dev/`.
6. Check it: `curl https://dcp-lobby.<your subdomain>.workers.dev/` answers `{"service":"dcp-lobby","proto":1}`, and
   the Connection test of a test instance started with `DCP_LOBBY` set to its address says the lobby is reachable.
7. To make it the default for players, set `kDefaultLobbyUrl` in `src/net/online_config.h`.

Reports: `curl -H "Authorization: Bearer <token>" https://dcp-lobby.<subdomain>.workers.dev/reports` (newest 100).

The Worker uses `CF-Connecting-IP` as each player's address. Rooms, members, accounts, friends, requests and reports
are SQLite tables in the Durable Object; per-connection state (presence blob, rate buckets) lives in each socket's
attachment and disappears with the socket.

## Usage statistics and the dashboard

`src/metrics.js` keeps anonymous aggregate service statistics in three tables of the same Durable Object; nothing else
is collected and clients send nothing for it (docs/ONLINE.md, Privacy and usage statistics).

| Table | Row | Columns |
|---|---|---|
| `metrics_daily` | one per day | `sessions`, `rooms_created`, `room_joins`, `incompatible_rejects`, `peak_online`, `peak_in_rooms`, `peak_rooms` |
| `metrics_daily_dimensions` | day, dimension, value | `count`: sessions started per `platform`, `version` and `country`; `reject` counts rejects by reason |
| `metrics_samples` | one per 15 minutes (UTC ms) | the highest `online`, `in_rooms` and `rooms` in the interval; 90 days kept |

- Days are America/Sao_Paulo calendar days (`YYYY-MM-DD`); samples are UTC timestamps.
- A **session** is a connection whose `hello` with `presence: true` succeeded, counted once per connection. The
  launcher's Friends link and the game's link are separate sessions and a reconnect is a new one; the Connection test
  (`presence: false`) and refused sign-ins are none. Sessions are not players: there is no unique-player, DAU or MAU
  count, and the daily platform, version and country counts are sessions started, not players.
- **Live** numbers are never stored: they come from the open sockets and the `rooms`/`members` tables when asked.
  Online now and the live breakdowns count accounts with an open presence connection (one per account), in rooms
  counts accounts seated with a connection, open rooms counts rooms. A socket that disappears without a close callback,
  or a Durable Object restart, leaves nothing stale.
- Peaks and samples are written only when a value grows (after `hello`, `room.create`, `room.join`, `room.resume`)
  and at each 15-minute alarm while someone is connected, never per message. Old samples go 500 at a time.
- **Rejects** count only explicit protocol refusals: `hello` with another lobby protocol (once per connection) and
  `room.join` of a room with another online protocol. Rooms only left out of a search are not counted.
- **Country** is Cloudflare's `request.cf.country`, set by the Worker in `X-DCP-Edge-Country` (replacing any value the
  client sent) and kept in the socket's attachment while it is open. Missing, `XX` or `T1` is `Unknown`. The local
  server counts every client as `Unknown`. Platforms other than Windows, Linux and Steam Deck are `Other`; versions that are not
  a dotted number such as `0.8.0` are `Other`, and after 64 values in a day new ones are `Other` too.

The tables are created with `CREATE TABLE IF NOT EXISTS` when the Durable Object starts, after the existing schema and
migrations; no existing table is changed or emptied, and no Wrangler migration is needed. They are plain tables, so
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
