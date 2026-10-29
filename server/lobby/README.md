# Lobby server

Players meet here: accounts, friends, presence, rooms and the exchange of UDP endpoints. Game traffic never goes
through the lobby. The protocol is in [docs/ONLINE.md](../../docs/ONLINE.md).

`src/lobby.js` is the whole lobby. It runs in two hosts:

- `src/worker.js`: a Cloudflare Worker with one SQLite-backed Durable Object (WebSocket Hibernation, `ping`/`pong`
  answered without waking the object, the seat grace as a Durable Object alarm).
- `local/server.js`: the development server on Node.js 22.13 or later (`node:http` + `node:sqlite`), no account needed.

No dependencies are installed for either the tests or the local server.

## Local

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
4. `wrangler deploy`. The lobby is then at `wss://dcp-lobby.<your subdomain>.workers.dev/`.
5. Check it: `curl https://dcp-lobby.<your subdomain>.workers.dev/` answers `{"service":"dcp-lobby","proto":1}`, and
   the launcher's Connection test with that address says the lobby is reachable.
6. To make it the default for players, set `kDefaultLobbyUrl` in `src/net/online_config.h`.

Reports: `curl -H "Authorization: Bearer <token>" https://dcp-lobby.<subdomain>.workers.dev/reports` (newest 100).

The Worker uses `CF-Connecting-IP` as each player's address. Rooms, members, accounts, friends, requests and reports
are SQLite tables in the Durable Object; per-connection state (presence blob, rate buckets) lives in each socket's
attachment and disappears with the socket.
