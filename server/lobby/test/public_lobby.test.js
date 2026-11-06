import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';

import { PROTECTION } from '../src/abuse.js';
import { LIMITS } from '../src/lobby.js';
import { SAMPLE_MS, metricsDay } from '../src/metrics.js';
import { sha256Hex } from '../src/sha256.js';
import { makeLobby } from './harness.js';

let nextIp = 1;

function signedIn(env, name, fields = {}) {
  const client = env.connect(`198.51.100.${nextIp++ % 250 + 1}`);
  assert.equal(client.hello({ name, online_protocol: 1, ...fields }).code, 'ok');
  return client;
}

function listed(env, name) {
  const client = signedIn(env, name);
  assert.equal(client.call('lobby.publish', { visible: true }).code, 'ok');
  return client;
}

const rows = (client) => client.call('lobby.list').players;
const row = (client, name) => rows(client).find((p) => p.name === name);
const names = (client) => rows(client).map((p) => p.name).sort();

function countWrites(env) {
  const writes = [];
  const run = env.host.sql.run;
  env.host.sql.run = (query, ...args) => {
    if (/^\s*(INSERT|UPDATE|DELETE)/i.test(query)) writes.push(query);
    return run(query, ...args);
  };
  return writes;
}

test('nobody appears until they ask to, and only those listed can read the list', () => {
  const env = makeLobby();
  const hidden = signedIn(env, 'Hidden');
  const viewer = listed(env, 'Viewer');
  assert.deepEqual(rows(viewer), []);
  assert.equal(hidden.call('lobby.list').code, 'not_visible');
  assert.equal(signedIn(env, 'Old', { lobby: false }).call('lobby.list').code, 'not_visible');
});

test('a hello that asks for it lists the session, and a Connection test session never is', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  signedIn(env, 'InGame', { lobby: true });
  signedIn(env, 'Probe', { lobby: true, presence: false });
  assert.deepEqual(names(viewer), ['InGame']);
  const probe = signedIn(env, 'Probe2', { presence: false });
  assert.equal(probe.call('lobby.publish', { visible: true }).code, 'not_found');
});

test('turning it off removes the player at once, from every session of the account', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  const player = listed(env, 'Player1');
  const game = env.connect('198.51.100.200');
  Object.assign(game, { account: player.account, secret: player.secret });
  assert.equal(game.hello({ name: 'Player1', lobby: true }).code, 'ok');
  assert.deepEqual(names(viewer), ['Player1']);
  assert.equal(player.call('lobby.publish', { visible: false }).code, 'ok');
  assert.deepEqual(rows(viewer), []);
  assert.equal(player.call('lobby.list').code, 'not_visible');
});

test('a closed socket leaves the list at once and a reconnect is not listed by itself', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  const player = listed(env, 'Player1');
  assert.deepEqual(names(viewer), ['Player1']);
  player.drop();
  assert.deepEqual(rows(viewer), []);
  const back = env.connect('198.51.100.201');
  Object.assign(back, { account: player.account, secret: player.secret });
  assert.equal(back.hello({ name: 'Player1' }).code, 'ok');
  assert.deepEqual(rows(viewer), []);
});

test('the list never holds the viewer, even from a second session', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  const second = env.connect('198.51.100.202');
  Object.assign(second, { account: viewer.account, secret: viewer.secret });
  assert.equal(second.hello({ name: 'Viewer', lobby: true }).code, 'ok');
  listed(env, 'Other');
  assert.deepEqual(names(viewer), ['Other']);
});

test('presence without a keepalive or a request expires; the keepalive alone keeps it', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  const quiet = listed(env, 'Quiet');
  const pinging = listed(env, 'Pinging');
  for (let elapsed = 0; elapsed <= LIMITS.lobbyStaleMs; elapsed += 20_000) {
    env.clock.now += 20_000;
    pinging.ping();
    viewer.ping();
  }
  assert.deepEqual(names(viewer), ['Pinging']);
  assert.equal(quiet.call('lobby.list').code, 'ok');
  assert.deepEqual(names(viewer), ['Pinging', 'Quiet']);
});

test('the keepalive cadence fits inside the stale window, and the Worker reads it from the runtime', () => {
  assert.ok(LIMITS.lobbyStaleMs >= 3 * 20_000);
  const worker = readFileSync(join(import.meta.dirname, '../src/worker.js'), 'utf8');
  assert.match(worker, /getWebSocketAutoResponseTimestamp\(this\.ws\)/);
  assert.ok(!worker.includes('new WorkerConn(ws)'));
});

test('the row of a player holds a random handle, the name and the relation: nothing else', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  const player = listed(env, 'Player1');
  player.call('udp', { port: 3658, lan: [{ ip: '192.168.1.10', port: 3658 }] });
  const answer = viewer.call('lobby.list');
  assert.deepEqual(Object.keys(answer).sort(), ['code', 'players', 're', 'rev', 'total']);
  assert.deepEqual(Object.keys(answer.players[0]).sort(), ['name', 'player', 'state', 'version']);
  assert.match(answer.players[0].player, /^[0-9a-f]{32}$/);
  const text = JSON.stringify(answer);
  for (const secret of [player.account, player.secret, player.ip, '192.168.1.10', '3658', 'Windows', env.lobby.meta('salt')]) {
    assert.ok(!text.includes(secret), secret);
  }
  assert.ok(!text.includes(env.lobby.accountRow(player.account).code));
});

test('the handle changes with each publish, so it cannot follow a player', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  const player = listed(env, 'Player1');
  const first = rows(viewer)[0].player;
  player.call('lobby.publish', { visible: true });
  assert.equal(rows(viewer)[0].player, first);
  player.call('lobby.publish', { visible: false });
  player.call('lobby.publish', { visible: true });
  assert.notEqual(rows(viewer)[0].player, first);
});

test('the name and identity are the session\'s; request fields are ignored', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  const victim = signedIn(env, 'Victim');
  const spoof = signedIn(env, 'Spoofer');
  assert.equal(spoof.call('lobby.publish', { visible: true, name: 'Victim', account: victim.account }).code, 'ok');
  assert.deepEqual(names(viewer), ['Spoofer']);
  assert.equal(env.lobby.accountRow(spoof.account).name, 'Spoofer');
});

test('a name outside the Online name rules is never listed', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  for (const name of ['two words', 'Ana‮', 'José', '<b>x</b>']) {
    const client = signedIn(env, name);
    assert.equal(client.call('lobby.publish', { visible: true }).code, 'bad_request');
    signedIn(env, name, { lobby: true });
  }
  assert.deepEqual(rows(viewer), []);
});

test('relations are worked out for the viewer', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  const stranger = listed(env, 'Stranger');
  const friend = listed(env, 'Friend1');
  const waiting = listed(env, 'Waiting');
  const asking = listed(env, 'Asking');
  env.lobby.makeFriends(viewer.account, friend.account);
  assert.equal(viewer.call('friends.add', { player: row(viewer, 'Waiting').player }).state, 'pending');
  assert.equal(asking.call('friends.add', { player: row(asking, 'Viewer').player }).state, 'pending');
  const states = Object.fromEntries(rows(viewer).map((p) => [p.name, p.state]));
  assert.deepEqual(states, { Stranger: 'none', Friend1: 'friend', Waiting: 'pending', Asking: 'incoming' });
  assert.equal(row(stranger, 'Viewer').state, 'none');
  assert.equal(row(waiting, 'Viewer').state, 'incoming');
  assert.equal(row(asking, 'Viewer').state, 'pending');
});

test('a request past its expiry no longer reads as a relation, and reading it writes nothing', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  const player = listed(env, 'Player1');
  viewer.call('friends.add', { player: row(viewer, 'Player1').player });
  assert.equal(row(viewer, 'Player1').state, 'pending');
  env.clock.now += (LIMITS.requestDays + 1) * 24 * 3600 * 1000;
  player.ping();
  const writes = countWrites(env);
  assert.equal(row(viewer, 'Player1').state, 'none');
  assert.equal(row(player, 'Viewer').state, 'none');
  assert.deepEqual(writes, []);
});

test('a Lobby request is an ordinary friend request: the row goes Pending, then Friend, with the usual events', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  const player = listed(env, 'Player1');
  assert.equal(viewer.call('friends.add', { player: row(viewer, 'Player1').player }).state, 'pending');
  assert.equal(row(viewer, 'Player1').state, 'pending');
  assert.equal(player.events('friend.request').length, 1);
  assert.equal(player.call('friends.requests').incoming[0].name, 'Viewer');
  assert.equal(viewer.call('friends.requests').outgoing[0].name, 'Player1');
  assert.equal(player.call('friends.accept', { account: viewer.account }).code, 'ok');
  assert.equal(viewer.events('friend.added').length, 1);
  assert.equal(row(viewer, 'Player1').state, 'friend');
  assert.ok(env.lobby.areFriends(viewer.account, player.account));
  assert.equal(viewer.call('friends.add', { player: row(viewer, 'Player1').player }).state, 'friends');
});

test('Accept in the list takes the normal path through friends.add', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  const asking = listed(env, 'Asking');
  asking.call('friends.add', { player: row(asking, 'Viewer').player });
  assert.equal(viewer.call('friends.add', { player: row(viewer, 'Asking').player }).state, 'friends');
  assert.equal(asking.events('friend.added').length, 1);
  assert.equal(env.lobby.friendsOf(viewer.account)[0], asking.account);
});

test('a request by Lobby handle obeys the friend limits and creates no room, invite or join', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  listed(env, 'Player1');
  env.lobby.friendCount = () => LIMITS.friends;
  assert.equal(viewer.call('friends.add', { player: row(viewer, 'Player1').player }).code, 'limit');
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM rooms').n, 0);
});

test('friend codes keep working as before', () => {
  const env = makeLobby();
  const a = signedIn(env, 'Alice');
  const b = signedIn(env, 'Bea');
  assert.equal(a.call('friends.add', { code: env.lobby.accountRow(b.account).code }).state, 'pending');
  assert.equal(a.call('friends.add', { code: 'nope' }).code, 'bad_request');
  assert.equal(a.call('friends.add', { code: 'ZZZZ-ZZZZ' }).code, 'not_found');
  assert.equal(a.call('friends.add', {}).code, 'bad_request');
});

test('a handle only reaches a player who is listed, and only for someone listed', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  const player = listed(env, 'Player1');
  const handle = row(viewer, 'Player1').player;
  assert.equal(signedIn(env, 'Hidden').call('friends.add', { player: handle }).code, 'not_visible');
  player.call('lobby.publish', { visible: false });
  assert.equal(viewer.call('friends.add', { player: handle }).code, 'not_found');
  assert.equal(viewer.call('friends.add', { player: 'ab'.repeat(16) }).code, 'not_found');
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM friend_requests').n, 0);
});

test('players on the default name get a stable alias that gives nothing away; a chosen name is left alone', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  const first = listed(env, 'Player');
  const second = listed(env, 'Player');
  listed(env, 'player');
  listed(env, 'Player-Chosen');
  const shown = names(viewer);
  assert.equal(shown.length, 4);
  assert.ok(shown.includes('player') && shown.includes('Player-Chosen'));
  const aliases = shown.filter((n) => /^Player-[0-9A-F]{4}$/.test(n));
  assert.equal(new Set(aliases).size, 2);
  assert.deepEqual(names(viewer), shown);
  first.call('lobby.publish', { visible: false });
  first.call('lobby.publish', { visible: true });
  assert.deepEqual(names(viewer), shown);
  for (const player of [first, second]) {
    const alias = env.lobby.displayName(player.account, 'Player');
    const code = env.lobby.accountRow(player.account).code;
    assert.ok(!player.account.toUpperCase().includes(alias.slice(-4)) && !code.includes(alias.slice(-4)));
    assert.notEqual(alias, `Player-${sha256Hex(player.account).slice(0, 4).toUpperCase()}`);
  }
  assert.equal(env.lobby.accountRow(first.account).name, 'Player');
});

test('the friends lists show the alias beside the real name the game matches by', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  const player = listed(env, 'Player');
  const alias = env.lobby.displayName(player.account, 'Player');
  const added = viewer.call('friends.add', { player: row(viewer, alias).player });
  assert.deepEqual([added.name, added.display_name], ['Player', alias]);
  const outgoing = viewer.call('friends.requests').outgoing[0];
  assert.deepEqual([outgoing.name, outgoing.display_name], ['Player', alias]);
  player.call('friends.accept', { account: viewer.account });
  const friend = viewer.call('friends.list').friends[0];
  assert.deepEqual([friend.name, friend.display_name], ['Player', alias]);
  const other = player.call('friends.list').friends[0];
  assert.deepEqual([other.name, other.display_name], ['Viewer', undefined]);
});

test('a row carries the version only when it is a plain version, and an older client stays listed', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  listed(env, 'Current', );
  signedIn(env, 'Tagged', { version: '0.9.2', lobby: true });
  signedIn(env, 'Odd', { version: '<b>1</b> x', lobby: true });
  const versions = Object.fromEntries(rows(viewer).map((p) => [p.name, p.version]));
  assert.deepEqual(versions, { Current: '', Tagged: '0.9.2', Odd: '' });
});

test('rows come by importance, and the cap never drops a waiting request', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  const asking = listed(env, 'Asking');
  asking.call('friends.add', { player: row(asking, 'Viewer').player });
  listed(env, 'Stranger');
  const waiting = listed(env, 'Waiting');
  viewer.call('friends.add', { player: row(viewer, 'Waiting').player });
  const friend = listed(env, 'Pal');
  env.lobby.makeFriends(viewer.account, friend.account);
  assert.ok(waiting);
  assert.deepEqual(rows(viewer).map((p) => p.state), ['incoming', 'pending', 'friend', 'none']);
  const now = env.host.now();
  const fakes = Array.from({ length: LIMITS.lobbyPlayers + 5 }, (_, i) => ({ attachment: () => ({ hello: true, account: String(9000 + i),
    name: `Bot${i}`, cid: `b${i}`, lobby: { player: i.toString(16).padStart(32, 'a'), since: now + 1000 + i, seen: now } }), keepaliveAt: () => 0 }));
  const sockets = env.host.sockets.bind(env.host);
  env.host.sockets = () => [...sockets(), ...fakes];
  env.lobby.index = null;
  const answer = viewer.call('lobby.list');
  assert.equal(answer.players.length, LIMITS.lobbyPlayers);
  assert.deepEqual(answer.players.slice(0, 3).map((p) => p.state), ['incoming', 'pending', 'friend']);
});

test('Decline in the list is the normal friends.decline: the request goes, and nothing else changes', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  const asking = listed(env, 'Asking');
  asking.call('friends.add', { player: row(asking, 'Viewer').player });
  assert.equal(viewer.call('friends.decline', { player: row(viewer, 'Asking').player }).code, 'ok');
  assert.deepEqual(viewer.call('friends.requests').incoming, []);
  assert.equal(row(viewer, 'Asking').state, 'none');
  assert.ok(!env.lobby.areFriends(viewer.account, asking.account));
  assert.equal(viewer.call('friends.decline', { player: row(viewer, 'Asking').player }).code, 'not_found');
  assert.equal(viewer.call('friends.decline', { player: 'nope' }).code, 'bad_request');
  assert.equal(signedIn(env, 'Hidden').call('friends.decline', { player: 'ab'.repeat(16) }).code, 'not_visible');
  asking.call('friends.add', { player: row(asking, 'Viewer').player });
  assert.equal(viewer.call('friends.decline', { account: asking.account }).code, 'ok');
});

test('malformed and oversized Lobby requests are refused', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  for (const visible of [undefined, 1, 'true', null]) assert.equal(viewer.call('lobby.publish', { visible }).code, 'bad_request');
  for (const rev of [1, 'xyz', '', 'ab'.repeat(32), null, {}]) assert.equal(viewer.call('lobby.list', { rev }).code, 'bad_request');
  for (const player of [1, '', 'XYZ', viewer.account, 'ab'.repeat(17), null, []]) {
    assert.equal(viewer.call('friends.add', { player }).code, 'bad_request');
  }
  assert.equal(viewer.call('lobby.list', { pad: 'x'.repeat(LIMITS.frameBytes) }).code, 'too_large');
  assert.equal(env.connect('198.51.100.203').call('lobby.list').code, 'no_session');
  assert.equal(env.connect('198.51.100.204').call('lobby.publish', { visible: true }).code, 'no_session');
});

test('publishing, listing and asking are each limited and recover, and a refused list spends no address budget', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  const player = listed(env, 'Player1');
  const flip = (client, n) => {
    const codes = [];
    for (let i = 0; i < n; i += 1) {
      client.call('lobby.publish', { visible: false });
      codes.push(client.call('lobby.publish', { visible: true }).code);
    }
    return codes;
  };
  assert.equal(flip(player, PROTECTION.lobbyPublishAccount.burst + 1).at(-1), 'limit');
  env.clock.now += 10_000;
  assert.equal(player.call('lobby.publish', { visible: true }).code, 'ok');

  const list = [];
  for (let i = 0; i < PROTECTION.lobbyListAccount.burst + 1; i += 1) list.push(viewer.call('lobby.list').code);
  assert.deepEqual([list.at(-2), list.at(-1)], ['ok', 'limit']);
  env.clock.now += 5_000;
  assert.equal(viewer.call('lobby.list').code, 'ok');
  const address = env.lobby.guards.lobbyListAddress.tokens(viewer.ip, env.clock.now);
  assert.equal(viewer.call('lobby.list').code, 'limit');
  assert.equal(env.lobby.guards.lobbyListAddress.tokens(viewer.ip, env.clock.now), address);

  env.clock.now += 60_000;
  const asks = [];
  for (let i = 0; i < PROTECTION.lobbyRequestAccount.burst + 1; i += 1) {
    asks.push(viewer.call('friends.add', { player: 'ab'.repeat(16) }).code);
  }
  assert.deepEqual([asks.at(-2), asks.at(-1)], ['not_found', 'limit']);
  env.advance(SAMPLE_MS);
  const refused = env.host.sql.get(`SELECT count FROM metrics_daily_dimensions WHERE dimension = 'abuse' AND value = 'public_lobby_limited'
    AND day = ?`, metricsDay(env.clock.now));
  assert.ok(refused.count >= 3);
});

test('reconnecting does not reset the Lobby limits', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  for (let i = 0; i < PROTECTION.lobbyListAccount.burst; i += 1) viewer.call('lobby.list');
  viewer.drop();
  const back = env.connect('198.51.100.205');
  Object.assign(back, { account: viewer.account, secret: viewer.secret });
  back.hello({ name: 'Viewer', lobby: true });
  assert.equal(back.call('lobby.list').code, 'limit');
});

test('players behind one address share a wider list limit, and other addresses are unaffected', () => {
  const env = makeLobby();
  const clients = [];
  for (let i = 0; i < PROTECTION.lobbyListAddress.burst / PROTECTION.lobbyListAccount.burst + 2; i += 1) {
    const client = env.connect('203.0.113.99');
    assert.equal(client.hello({ name: `Lan${i}`, lobby: true }).code, 'ok');
    clients.push(client);
  }
  const codes = [];
  for (let round = 0; round < PROTECTION.lobbyListAccount.burst; round += 1) {
    for (const client of clients) codes.push(client.call('lobby.list').code);
  }
  assert.equal(codes.filter((c) => c === 'ok').length, PROTECTION.lobbyListAddress.burst);
  assert.equal(listed(env, 'Elsewhere').call('lobby.list').code, 'ok');
});

test('the list is bounded, and the count says how many it left out', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  const now = env.host.now();
  const fakes = Array.from({ length: LIMITS.lobbyPlayers + 25 }, (_, i) => ({
    conn: { attachment: () => ({ hello: true, account: String(5000 + i), name: `Bot${i}`, cid: `c${i}`,
      lobby: { player: i.toString(16).padStart(32, '0'), since: now + i, seen: now } }), keepaliveAt: () => 0 },
  }));
  const sockets = env.host.sockets.bind(env.host);
  env.host.sockets = () => [...sockets(), ...fakes.map((f) => f.conn)];
  const answer = viewer.call('lobby.list');
  assert.equal(answer.players.length, LIMITS.lobbyPlayers);
  assert.equal(answer.total, LIMITS.lobbyPlayers + 25);
  assert.equal(answer.players[0].name, `Bot${LIMITS.lobbyPlayers + 24}`);
  assert.ok(JSON.stringify(answer).length < LIMITS.frameBytes);
});

test('an unchanged list is answered without its rows, and any change sends them again', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  listed(env, 'Player1');
  const first = viewer.call('lobby.list');
  assert.equal(first.players.length, 1);
  const again = viewer.call('lobby.list', { rev: first.rev });
  assert.deepEqual([again.unchanged, again.rev, again.total, again.players], [true, first.rev, 1, undefined]);
  listed(env, 'Player2');
  const changed = viewer.call('lobby.list', { rev: first.rev });
  assert.equal(changed.players.length, 2);
  assert.notEqual(changed.rev, first.rev);
});

test('presence and listing write nothing to the tables, and nobody is sent an event', () => {
  const env = makeLobby();
  const viewer = listed(env, 'Viewer');
  const player = signedIn(env, 'Player1');
  const writes = countWrites(env);
  player.call('lobby.publish', { visible: true });
  viewer.call('lobby.list');
  player.call('lobby.publish', { visible: false });
  assert.deepEqual(writes, []);
  assert.deepEqual([...viewer.inbox, ...player.inbox], []);
});

test('a client that never uses the Lobby sees the same protocol', () => {
  const env = makeLobby();
  const old = signedIn(env, 'Old');
  assert.deepEqual(Object.keys(old.call('friends.list')).sort(), ['code', 'friends', 're']);
  assert.equal(old.conn.attachment().lobby, undefined);
  old.drop();
});

test('the development server stamps a real keepalive, so it ages presence as the Worker does', async () => {
  const source = readFileSync(join(import.meta.dirname, '../local/server.js'), 'utf8');
  assert.match(source, /if \(text === 'ping'\) \{\s+conn\.notePing\(\);/);
});
