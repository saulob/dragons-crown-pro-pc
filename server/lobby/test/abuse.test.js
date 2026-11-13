import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';

import { LIMITED_HEADER, COUNTRY_HEADER, limitedCount } from '../src/admin.js';
import { PROTECTION, RateLimiter } from '../src/abuse.js';
import { handleUpgrade, ipGroup, upgradeAllowed } from '../src/edge.js';
import { LIMITS, Lobby } from '../src/lobby.js';
import { SAMPLE_MS, metricsDay } from '../src/metrics.js';
import { RELAY, encodeRelayFrame, roomToken } from '../src/relay.js';
import { ROOM, makeLobby as makeHarness } from './harness.js';

const makeLobby = (options = {}) => makeHarness({
  log: (text) => {
    if (!text.startsWith('relay:')) throw new Error(text);
  },
  ...options,
});

const CLIENT = { online_protocol: 1, version: '0.9.1', platform: 'Windows', relay: true };

function countWrites(env) {
  const writes = [];
  const run = env.host.sql.run;
  env.host.sql.run = (query, ...args) => {
    if (/^\s*(INSERT|UPDATE|DELETE)/i.test(query)) writes.push(query.replace(/\s+/g, ' ').trim());
    return run(query, ...args);
  };
  return writes;
}

const writesTo = (writes, table) => writes.filter((q) => q.includes(table));

function signedIn(env, fields = CLIENT, ip = '203.0.113.7') {
  const client = env.connect(ip, 'BR');
  assert.equal(client.hello(fields).code, 'ok');
  return client;
}

function reconnect(env, client, fields = CLIENT, ip = client.ip) {
  const next = env.connect(ip, 'BR');
  Object.assign(next, { account: client.account, secret: client.secret });
  return { client: next, reply: next.hello({ ...fields, name: fields.name ?? `P${client.account.slice(-3)}` }) };
}

const abuse = (env) => Object.fromEntries(env.host.sql.all(
  `SELECT value, count FROM metrics_daily_dimensions WHERE dimension = 'abuse' AND day = ?`, metricsDay(env.clock.now))
  .map((r) => [r.value, r.count]));

const sessions = (env) => env.lobby.metricsSummary().today.sessions;

function upgradeRequest(headers = {}) {
  return new Request('https://dcp-lobby.example/', { headers: { Upgrade: 'websocket', 'CF-Connecting-IP': '203.0.113.7', ...headers } });
}

function fakeLobby() {
  const seen = [];
  return { seen, fetch: async (request) => { seen.push(request); return new Response(null, { status: 200 }); } };
}

function fakeLimiter(allow = () => true) {
  const keys = [];
  return { keys, limit: async ({ key }) => { keys.push(key); return { success: allow(key) }; } };
}

// ---- edge ----
test('the edge groups IPv6 clients by /64 whatever the spelling', () => {
  assert.equal(ipGroup('203.0.113.7'), '203.0.113.7');
  assert.equal(ipGroup('::ffff:203.0.113.7'), '203.0.113.7');
  assert.equal(ipGroup('2001:db8:1:2::5'), ipGroup('2001:0DB8:0001:0002:aaaa:bbbb:cccc:dddd'));
  assert.equal(ipGroup('2001:db8::1'), ipGroup('2001:db8:0:0:9::1'));
  assert.notEqual(ipGroup('2001:db8:1:2::5'), ipGroup('2001:db8:1:3::5'));
  assert.equal(ipGroup('::1'), '0:0:0:0');
});

test('an upgrade over the edge limit is a 429 that never reaches the lobby', async () => {
  const lobby = fakeLobby();
  const limiter = fakeLimiter((key) => !key.includes('198.51.100.9'));
  const env = { WS_UPGRADE_LIMITER: limiter };
  const refused = await handleUpgrade(upgradeRequest({ 'CF-Connecting-IP': '198.51.100.9' }), env, lobby);
  assert.equal(refused.status, 429);
  assert.equal(refused.headers.get('Retry-After'), '60');
  assert.equal(lobby.seen.length, 0);
  const allowed = await handleUpgrade(upgradeRequest(), env, lobby);
  assert.equal(allowed.status, 200);
  assert.equal(lobby.seen.length, 1);
  assert.equal(limiter.keys[0], 'ws:198.51.100.9');
});

test('every address of one /64 shares an edge limit key', async () => {
  const limiter = fakeLimiter();
  await upgradeAllowed(limiter, '2001:db8:1:2::5');
  await upgradeAllowed(limiter, '2001:db8:1:2:ffff::1');
  await upgradeAllowed(limiter, '2001:db8:1:3::5');
  assert.equal(limiter.keys[0], limiter.keys[1]);
  assert.notEqual(limiter.keys[0], limiter.keys[2]);
});

test('refused upgrades are counted on the next one forwarded, and a client cannot forge the count', async () => {
  const lobby = fakeLobby();
  let allow = false;
  const env = { WS_UPGRADE_LIMITER: fakeLimiter(() => allow) };
  for (let i = 0; i < 3; i++) await handleUpgrade(upgradeRequest(), env, lobby);
  allow = true;
  await handleUpgrade(upgradeRequest(), env, lobby);
  assert.equal(limitedCount(lobby.seen[0].headers), 3);
  await handleUpgrade(upgradeRequest({ [LIMITED_HEADER]: '999999' }), env, lobby);
  assert.equal(lobby.seen[1].headers.get(LIMITED_HEADER), null);
  assert.equal(lobby.seen[1].headers.get(COUNTRY_HEADER), 'Unknown');
  for (const bad of ['-4', '1.5', 'x', '']) assert.equal(limitedCount(new Headers({ [LIMITED_HEADER]: bad })), 0);
});

test('without the binding, or if it fails, upgrades pass', async () => {
  assert.equal(await upgradeAllowed(undefined, '203.0.113.7'), true);
  assert.equal(await upgradeAllowed({ limit: async () => { throw new Error('down'); } }, '203.0.113.7'), true);
  assert.equal(await upgradeAllowed(fakeLimiter(), ''), true);
  const lobby = fakeLobby();
  assert.equal((await handleUpgrade(upgradeRequest(), {}, lobby)).status, 200);
});

test('the Worker gates upgrades at the edge and declares the limiter', () => {
  const root = join(import.meta.dirname, '..');
  const worker = readFileSync(join(root, 'src/worker.js'), 'utf8');
  assert.equal(worker.match(/stub\.fetch\(/g).length, 2);
  assert.ok(worker.includes('handleUpgrade(request, env, stub)'));
  const toml = readFileSync(join(root, 'wrangler.toml'), 'utf8');
  assert.match(toml, /name = "WS_UPGRADE_LIMITER"/);
  assert.match(toml, /simple = \{ limit = 30, period = 60 \}/);
});

test('the lobby counts what the edge refused and the upgrades it accepted, in memory until the alarm', () => {
  const env = makeLobby();
  const writes = countWrites(env);
  env.lobby.edgeLimited(7);
  env.lobby.edgeLimited(0);
  env.connect();
  assert.equal(writesTo(writes, 'metrics_daily_dimensions').length, 0);
  env.advance(SAMPLE_MS);
  assert.deepEqual(abuse(env), { ws_upgrade_accepted: 1, ws_upgrade_limited: 7 });
});

// ---- rate limiter ----
test('the limiter refills, refuses without spending, and stays bounded', () => {
  const limiter = new RateLimiter({ burst: 2, perSecond: 1 }, 8);
  assert.ok(limiter.take('a', 0) && limiter.take('a', 0));
  assert.equal(limiter.take('a', 0), false);
  assert.equal(limiter.allows('a', 1000), true);
  assert.equal(limiter.take('a', 1000), true);
  for (let i = 0; i < 100; i++) limiter.take(`k${i}`, 5000);
  assert.ok(limiter.buckets.size <= 8);
});

// ---- hello ----
test('reconnecting does not rewrite the account row', () => {
  const env = makeLobby();
  const first = signedIn(env);
  const writes = countWrites(env);
  let client = first;
  for (let i = 0; i < 5; i++) {
    client.drop();
    env.advance(30_000);
    const next = reconnect(env, first);
    assert.equal(next.reply.code, 'ok');
    client = next.client;
  }
  assert.equal(writesTo(writes, 'accounts').length, 0);
  assert.deepEqual(writes.filter((q) => q.includes('metrics_daily') && !q.includes("'abuse'")), []);
});

test('the account row is refreshed when the name changes or the last write is old, and not otherwise', () => {
  const env = makeLobby();
  const first = signedIn(env, { ...CLIENT, name: 'Alice' });
  first.drop();
  const writes = countWrites(env);
  const probe = { ...CLIENT, name: 'Alicia', presence: false };
  const { client: renamed, reply } = reconnect(env, first, probe);
  assert.equal(reply.name, 'Alicia');
  assert.equal(writesTo(writes, 'UPDATE accounts').length, 1);
  assert.equal(env.lobby.accountRow(first.account).name, 'Alicia');
  renamed.drop();
  env.advance(PROTECTION.seenRefreshMs - 1000);
  reconnect(env, first, probe).client.drop();
  assert.equal(writesTo(writes, 'UPDATE accounts').length, 1);
  env.advance(1000);
  reconnect(env, first, probe);
  assert.equal(writesTo(writes, 'UPDATE accounts').length, 2);
  assert.equal(env.lobby.accountRow(first.account).seen, env.clock.now);
});

test('authentication is unchanged: a wrong secret is refused and writes nothing', () => {
  const env = makeLobby();
  const owner = signedIn(env);
  const writes = countWrites(env);
  const thief = env.connect();
  Object.assign(thief, { account: owner.account, secret: 'cd'.repeat(32) });
  assert.equal(thief.hello(CLIENT).code, 'unauthorized');
  assert.equal(writes.length, 0);
});

test('a rapid reconnect of one account is one session; after the window it is another', () => {
  const env = makeLobby();
  const writes = countWrites(env);
  const first = signedIn(env);
  const counted = writes.length;
  assert.equal(sessions(env), 1);
  let client = first;
  for (let i = 0; i < 4; i++) {
    client.drop();
    env.advance(5_000);
    client = reconnect(env, first).client;
  }
  assert.equal(sessions(env), 1);
  assert.equal(writes.length, counted);
  client.drop();
  env.advance(PROTECTION.sessionWindowMs);
  assert.equal(reconnect(env, first).reply.code, 'ok');
  assert.equal(sessions(env), 2);
  const dimensions = env.host.sql.all(`SELECT dimension, SUM(count) AS n FROM metrics_daily_dimensions
    WHERE dimension IN ('platform', 'version', 'country') GROUP BY dimension`);
  assert.deepEqual(dimensions.map((d) => d.n), [2, 2, 2]);
  env.advance(SAMPLE_MS);
  assert.equal(abuse(env).hello_reconnect_deduped, 4);
});

test('the Connection test does not use up the session window, and each account counts for itself', () => {
  const env = makeLobby();
  const owner = env.connect();
  assert.equal(owner.hello({ ...CLIENT, presence: false }).code, 'ok');
  assert.equal(sessions(env), 0);
  owner.drop();
  assert.equal(reconnect(env, owner).reply.code, 'ok');
  assert.equal(sessions(env), 1);
  signedIn(env);
  assert.equal(sessions(env), 2);
});

test('a hello flood on one account is limited, and legitimate reconnects fit well inside the limit', () => {
  const env = makeLobby();
  const first = signedIn(env);
  first.drop();
  for (let i = 0; i < PROTECTION.hello.burst - 1; i++) {
    const next = reconnect(env, first);
    assert.equal(next.reply.code, 'ok');
    next.client.drop();
  }
  assert.equal(reconnect(env, first).reply.code, 'limit');
  env.advance(15_000);
  assert.equal(reconnect(env, first).reply.code, 'ok');
  env.advance(SAMPLE_MS);
  assert.equal(abuse(env).hello_limited, 1);
});

test('guessing at an account cannot lock its owner out', () => {
  const env = makeLobby();
  const owner = signedIn(env);
  owner.drop();
  for (let i = 0; i < 40; i++) {
    const thief = env.connect();
    Object.assign(thief, { account: owner.account, secret: 'cd'.repeat(32) });
    assert.equal(thief.hello(CLIENT).code, 'unauthorized');
    thief.drop();
  }
  assert.equal(reconnect(env, owner).reply.code, 'ok');
});

test('the development server is not limited on loopback', () => {
  const env = makeLobby({ local: true });
  const first = signedIn(env, CLIENT, '127.0.0.1');
  first.drop();
  for (let i = 0; i < 30; i++) {
    const next = reconnect(env, first, CLIENT, '127.0.0.1');
    assert.equal(next.reply.code, 'ok');
    next.client.drop();
  }
});

test('refusals of one reason are written once per window, not on every reconnect', () => {
  const env = makeLobby();
  const writes = countWrites(env);
  for (let i = 0; i < 5; i++) assert.equal(env.connect('203.0.113.7').hello({ proto: 2 }).code, 'proto');
  assert.equal(env.host.sql.get('SELECT incompatible_rejects AS n FROM metrics_daily').n, 1);
  assert.equal(writesTo(writes, 'metrics').length, 2);
  env.advance(PROTECTION.rejectWindowMs);
  assert.equal(env.connect('203.0.113.7').hello({ proto: 2 }).code, 'proto');
  assert.equal(env.host.sql.get('SELECT incompatible_rejects AS n FROM metrics_daily').n, 2);
});

test('the wire answer of hello is unchanged', () => {
  const env = makeLobby();
  const reply = signedIn(env);
  const again = reconnect(env, reply).reply;
  assert.deepEqual(Object.keys(again).sort(), ['account', 'capabilities', 'code', 'friend_code', 'ip', 'name', 're', 'time']);
});

test('a database from before the session column gains it and keeps its accounts', () => {
  const env = makeLobby();
  const owner = signedIn(env);
  owner.drop();
  env.host.sql.run('ALTER TABLE accounts DROP COLUMN session');
  env.host.sql.run('ALTER TABLE rooms DROP COLUMN peak_members');
  env.host.sql.run('ALTER TABLE rooms DROP COLUMN relayed');
  env.host.sql.run('ALTER TABLE members DROP COLUMN public_ip');
  env.host.sql.run('ALTER TABLE rooms DROP COLUMN platforms');
  env.host.sql.run('ALTER TABLE rooms DROP COLUMN story_seen');
  env.host.sql.run(`UPDATE meta SET v = '1' WHERE k = 'schema'`);
  env.lobby = new Lobby(env.host);
  assert.equal(env.lobby.meta('schema'), '6');
  assert.equal(env.lobby.accountRow(owner.account).session, 0);
  const next = env.connect();
  Object.assign(next, { account: owner.account, secret: owner.secret });
  assert.equal(next.hello(CLIENT).code, 'ok');
});

// ---- rooms ----
test('a player creating rooms in a loop is limited, reconnecting does not reset it, and leaving still works', () => {
  const env = makeLobby();
  const first = signedIn(env);
  let client = first;
  const burst = PROTECTION.roomCreateAccount.burst;
  for (let i = 0; i < burst; i++) {
    const created = client.call('room.create', ROOM);
    assert.equal(created.code, 'ok');
    assert.equal(client.call('room.leave', { room: created.room.id }).code, 'ok');
  }
  assert.equal(client.call('room.create', ROOM).code, 'limit');
  client.drop();
  client = reconnect(env, first).client;
  assert.equal(client.call('room.create', ROOM).code, 'limit');
  assert.equal(client.call('room.search', { world: 1 }).code, 'ok');
  env.advance(20_000);
  const created = client.call('room.create', ROOM);
  assert.equal(created.code, 'ok');
  assert.equal(client.call('room.leave', { room: created.room.id }).code, 'ok');
  env.advance(SAMPLE_MS);
  assert.deepEqual([abuse(env).room_create_accepted, abuse(env).room_create_limited], [burst + 1, 2]);
});

test('a refused room.create writes nothing', () => {
  const env = makeLobby();
  const client = signedIn(env);
  for (let i = 0; i < PROTECTION.roomCreateAccount.burst; i++) {
    client.call('room.leave', { room: client.call('room.create', ROOM).room.id });
  }
  const writes = countWrites(env);
  assert.equal(client.call('room.create', ROOM).code, 'limit');
  assert.equal(writes.length, 0);
});

test('hosting and joining as usual is never limited', () => {
  const env = makeLobby();
  const host = signedIn(env);
  const guest = signedIn(env, CLIENT, '198.51.100.4');
  for (let round = 0; round < 3; round++) {
    const room = host.call('room.create', ROOM).room;
    assert.equal(guest.call('room.join', { room: room.id }).code, 'ok');
    assert.equal(guest.call('room.leave', { room: room.id }).code, 'ok');
    assert.equal(host.call('room.leave', { room: room.id }).code, 'ok');
    env.advance(60_000);
  }
});

test('players behind one address share a wider room limit, other addresses are unaffected', () => {
  const env = makeLobby();
  const results = [];
  const known = Array.from({ length: PROTECTION.roomCreateAddress.burst + 3 }, (_, i) => {
    const client = signedIn(env, CLIENT, `192.0.2.${i + 1}`);
    client.drop();
    return client;
  });
  for (const account of known) {
    const { client } = reconnect(env, account, CLIENT, '203.0.113.50');
    results.push(client.call('room.create', ROOM).code);
    client.drop();
  }
  assert.equal(results.filter((c) => c === 'ok').length, PROTECTION.roomCreateAddress.burst);
  assert.equal(results.at(-1), 'limit');
  assert.equal(signedIn(env, CLIENT, '203.0.113.51').call('room.create', ROOM).code, 'ok');
});

test('joining rooms in a loop is limited; joins that fail do not count', () => {
  const env = makeLobby();
  const host = signedIn(env);
  const room = host.call('room.create', ROOM).room;
  const guest = signedIn(env, CLIENT, '198.51.100.4');
  for (let i = 0; i < PROTECTION.roomJoinAccount.burst + 5; i++) {
    assert.equal(guest.call('room.join', { room: '999999' }).code, 'not_found');
  }
  const first = signedIn(env, CLIENT, '198.51.100.5');
  let looper = first;
  const codes = [];
  for (let i = 0; i < PROTECTION.roomJoinAccount.burst + 2; i++) {
    if (i % 5 === 0 && i > 0) {
      looper.drop();
      looper = reconnect(env, first).client;
    }
    const joined = looper.call('room.join', { room: room.id });
    codes.push(joined.code);
    if (joined.code === 'ok') looper.call('room.leave', { room: room.id });
  }
  assert.equal(codes.filter((c) => c === 'ok').length, PROTECTION.roomJoinAccount.burst);
  assert.equal(codes.at(-1), 'limit');
});

test('repeating an unchanged room or endpoint update writes nothing', () => {
  const env = makeLobby();
  const host = signedIn(env);
  const room = host.call('room.create', ROOM).room;
  const update = { room: room.id, ints: [[0x4c, 3]], bin_ext: [[0x55, '11'.repeat(0x78)]] };
  assert.equal(host.call('room.set_external', { ...update, ints: [[0x4c, 9]] }).code, 'ok');
  assert.equal(host.call('udp', { port: 4000 }).code, 'ok');
  const writes = countWrites(env);
  assert.equal(host.call('room.set_external', { ...update, ints: [[0x4c, 9]] }).code, 'ok');
  assert.equal(host.call('udp', { port: 4000 }).code, 'ok');
  assert.equal(host.call('room.set_internal', { room: room.id, flag_mask: 0, flag_value: 0 }).code, 'ok');
  assert.equal(writes.length, 0);
  assert.equal(host.call('room.set_external', { ...update, ints: [[0x4c, 10]] }).code, 'ok');
  assert.equal(writes.length, 1);
});

// ---- relay ----
function pair(env) {
  const a = signedIn(env);
  const b = signedIn(env, CLIENT, '198.51.100.4');
  const room = a.call('room.create', ROOM).room;
  a.room = room;
  const joined = b.call('room.join', { room: room.id });
  assert.equal(joined.code, 'ok');
  return { a, b, token: roomToken(room.key) };
}

const payload = new Uint8Array(40).fill(7);

test('normal relay traffic passes and is never counted as a fault', () => {
  const env = makeLobby();
  const { a, b, token } = pair(env);
  for (let i = 0; i < 100; i++) a.relay(encodeRelayFrame(token, 0x20, i, payload));
  assert.equal(b.binary.length, 100);
  assert.equal(a.closedWith, null);
});

test('a sender of malformed frames loses its connection after a bounded number', () => {
  const env = makeLobby();
  const { a } = pair(env);
  const junk = new Uint8Array([1, 2, 3]);
  for (let i = 0; i < RELAY.invalid.burst; i++) a.relay(junk);
  assert.equal(a.closedWith, null);
  a.relay(junk);
  assert.deepEqual(a.closedWith, { code: 1008, reason: 'relay abuse' });
  env.advance(SAMPLE_MS);
  assert.equal(abuse(env).relay_abuse_closed, 1);
});

test('frames with a bad token, to a missing member, from a sender without a room or relay, or from a stranger end the same way', () => {
  const env = makeLobby();
  const { token } = pair(env);
  const member = (ip) => {
    const host = signedIn(env, CLIENT, ip);
    host.call('room.create', ROOM);
    return host;
  };
  const cases = [
    [member('198.51.100.8'), encodeRelayFrame(token ^ 1, 0x20, 1, payload)],
    [member('198.51.100.9'), encodeRelayFrame(token, 0x30, 1, payload)],
    [signedIn(env, CLIENT, '198.51.100.10'), encodeRelayFrame(token, 0x20, 1, payload)],
    [signedIn(env, { ...CLIENT, relay: false }, '198.51.100.11'), encodeRelayFrame(token, 0x20, 1, payload)],
    [env.connect('198.51.100.12'), encodeRelayFrame(token, 0x20, 1, payload)],
  ];
  for (const [sender, frame] of cases) {
    for (let i = 0; i < RELAY.invalid.burst + 1; i++) sender.relay(frame);
    assert.equal(sender.closedWith?.code, 1008);
  }
});

test('invalid frames are budgeted over time: an occasional stray one never closes a sender', () => {
  const env = makeLobby();
  const { a } = pair(env);
  for (let i = 0; i < 600; i++) {
    a.relay(new Uint8Array([9]));
    env.advance(1000);
  }
  assert.equal(a.closedWith, null);
});

test('a sender whose peer is offline or resuming keeps its connection however long the peer takes', () => {
  const env = makeLobby();
  const { a, b, token } = pair(env);
  b.drop();
  for (let round = 0; round < 20; round++) {
    for (let i = 0; i < 100; i++) a.relay(encodeRelayFrame(token, 0x20, round * 100 + i, payload));
    env.advance(1000);
  }
  assert.equal(a.closedWith, null);
});

test('a closed sender is ignored, and a connection that never greeted cannot relay at all', () => {
  const env = makeLobby();
  const { a, b, token } = pair(env);
  const junk = new Uint8Array([1]);
  for (let i = 0; i <= RELAY.invalid.burst; i++) a.relay(junk);
  assert.equal(a.closedWith?.code, 1008);
  a.relay(encodeRelayFrame(token, 0x20, 1, payload));
  assert.equal(b.binary.length, 0);
  const silent = env.connect('198.51.100.30');
  for (let i = 0; i <= RELAY.invalid.burst; i++) silent.relay(encodeRelayFrame(token, 0x20, 1, payload));
  assert.equal(silent.closedWith?.code, 1008);
});

test('relay state of a connection that never greeted is released when it closes', () => {
  const env = makeLobby();
  const silent = env.connect('198.51.100.30');
  silent.relay(new Uint8Array([1]));
  assert.equal(env.lobby.relays.connections.size, 1);
  silent.drop();
  assert.equal(env.lobby.relays.connections.size, 0);
});

// ---- alarms ----
test('a woken object that already knows its alarm does not set it again', () => {
  const env = makeLobby();
  signedIn(env);
  assert.notEqual(env.host.alarmAt, null);
  const sets = [];
  const setAlarm = env.host.setAlarm.bind(env.host);
  env.host.setAlarm = (ms) => {
    sets.push(ms);
    setAlarm(ms);
  };
  void new Lobby(env.host, { alarmAt: env.host.alarmAt });
  assert.equal(sets.length, 0);
  void new Lobby(env.host);
  assert.equal(sets.length, 1);
});

test('diagnostics flush once per interval and an idle object schedules no further alarm', () => {
  const env = makeLobby();
  env.lobby.edgeLimited(3);
  assert.equal(env.host.alarmAt, env.clock.now - (env.clock.now % SAMPLE_MS) + SAMPLE_MS);
  const writes = countWrites(env);
  env.lobby.edgeLimited(2);
  assert.equal(writes.length, 0);
  env.advance(SAMPLE_MS);
  assert.deepEqual(abuse(env), { ws_upgrade_limited: 5 });
  assert.equal(env.host.alarmAt, null);
});

test('abuse diagnostics hold counter names only', () => {
  const env = makeLobby();
  const client = signedIn(env);
  for (let i = 0; i < 9; i++) client.call('room.leave', { room: client.call('room.create', ROOM).room?.id ?? '1' });
  env.advance(SAMPLE_MS);
  const rows = env.host.sql.all(`SELECT * FROM metrics_daily_dimensions WHERE dimension = 'abuse'`);
  const text = JSON.stringify(rows);
  assert.ok(rows.length > 0);
  assert.ok(!text.includes(client.account) && !text.includes('203.0.113'));
});

const OBSERVED = '2001:db8:1::10';
const ADVERTISED = '198.51.100.25';

test('an advertised IPv4 is never a rate-limit identity: every limit keys on the observed address', () => {
  const env = makeLobby();
  const client = signedIn(env, CLIENT, OBSERVED);
  assert.equal(client.call('udp', { port: 41000, public_ip: ADVERTISED }).public_ip, ADVERTISED);
  assert.equal(client.call('room.create', ROOM).code, 'ok');
  const { guards } = env.lobby;
  assert.ok(guards.roomCreateAddress.buckets.has(ipGroup(OBSERVED)) && !guards.roomCreateAddress.buckets.has(ADVERTISED));
  assert.ok(guards.publicEndpoint.buckets.has(ipGroup(OBSERVED)) && !guards.publicEndpoint.buckets.has(ADVERTISED));
  const registered = (ip) => env.host.sql.get('SELECT COUNT(*) AS n FROM registrations WHERE ip = ?', env.lobby.ipKey(ip)).n;
  assert.deepEqual([registered(OBSERVED), registered(ADVERTISED)], [1, 0]);

  const fromAdvertised = [];
  for (let i = 0; i < LIMITS.socketsPerIp; i += 1) fromAdvertised.push(env.connect(ADVERTISED));
  assert.ok(fromAdvertised.every((c) => c.admitted));
  const sameGroup = [];
  for (let i = 1; i < LIMITS.socketsPerIp; i += 1) sameGroup.push(env.connect(`2001:db8:1::${i + 0x20}`));
  assert.ok(sameGroup.every((c) => c.admitted));
  assert.equal(env.connect(`2001:db8:1::ff`).admitted, false);
});

test('advertised endpoints spend a budget of the observed /64; peers are not told past it', () => {
  const env = makeLobby();
  const diagnosed = [];
  const diagnose = env.lobby.diagnose.bind(env.lobby);
  env.lobby.diagnose = (name, amount) => {
    diagnosed.push(name);
    return diagnose(name, amount);
  };
  const client = signedIn(env, CLIENT, OBSERVED);
  const room = client.call('room.create', ROOM).room;
  const peer = signedIn(env, CLIENT, '203.0.113.20');
  assert.equal(peer.call('room.join', { room: room.id }).code, 'ok');
  peer.events('peer');
  const { burst } = PROTECTION.publicEndpoint;
  for (let i = 0; i < burst; i += 1) {
    assert.equal(client.call('udp', { port: 41000 + i, public_ip: ADVERTISED }).public_ip, ADVERTISED);
  }
  assert.equal(peer.events('peer').filter((e) => e.ip === ADVERTISED).length, burst);
  const sibling = signedIn(env, CLIENT, '2001:db8:1::77');
  assert.equal(sibling.call('udp', { port: 42000, public_ip: ADVERTISED }).public_ip, undefined);
  const past = client.call('udp', { port: 43000, public_ip: '198.51.100.99' });
  assert.deepEqual([past.code, past.ip, past.public_ip], ['ok', OBSERVED, undefined]);
  assert.ok(peer.events('peer').every((e) => e.ip === OBSERVED));
  assert.ok(diagnosed.includes('public_endpoint_limited'));

  assert.equal(signedIn(env, CLIENT, '2001:db8:2::10').call('udp', { port: 42000, public_ip: ADVERTISED }).public_ip, ADVERTISED);
  env.advance(1000 / PROTECTION.publicEndpoint.perSecond);
  assert.equal(client.call('udp', { port: 43000, public_ip: '198.51.100.99' }).public_ip, '198.51.100.99');
});

test('repeating an advertised endpoint costs nothing and does not make peers punch again', () => {
  const env = makeLobby();
  const client = signedIn(env, CLIENT, OBSERVED);
  const room = client.call('room.create', ROOM).room;
  const peer = signedIn(env, CLIENT, '203.0.113.20');
  assert.equal(peer.call('room.join', { room: room.id }).code, 'ok');
  peer.events('peer');
  const report = { port: 41000, public_ip: ADVERTISED, lan: [{ ip: '192.168.1.10', port: 3658 }] };
  client.call('udp', report);
  assert.equal(peer.events('peer').length, 1);
  const writes = countWrites(env);
  for (let i = 0; i < PROTECTION.publicEndpoint.burst * 3; i += 1) {
    assert.equal(client.call('udp', report).public_ip, ADVERTISED);
    env.advance(100);
  }
  assert.deepEqual([writes.length, peer.events('peer').length], [0, 0]);
  client.call('udp', { ...report, lan: [{ ip: '192.168.1.11', port: 3658 }] });
  assert.equal(peer.events('peer').length, 1);
});

test('flipping the LAN list under an advertised endpoint spends the same budget', () => {
  const env = makeLobby();
  const client = signedIn(env, CLIENT, OBSERVED);
  const report = (i) => client.call('udp', { port: 41000, public_ip: ADVERTISED, lan: [{ ip: '192.168.1.10', port: 3658 + i }] });
  for (let i = 0; i < PROTECTION.publicEndpoint.burst; i += 1) assert.equal(report(i).public_ip, ADVERTISED);
  assert.equal(report(PROTECTION.publicEndpoint.burst).public_ip, undefined);
});

test('clients that advertise nothing keep every report and spend no endpoint budget', () => {
  const env = makeLobby();
  const client = signedIn(env, CLIENT, '203.0.113.5');
  const room = client.call('room.create', ROOM).room;
  const peer = signedIn(env, CLIENT, '203.0.113.20');
  assert.equal(peer.call('room.join', { room: room.id }).code, 'ok');
  peer.events('peer');
  for (let i = 0; i < 3; i += 1) assert.equal(client.call('udp', { port: 50000 }).code, 'ok');
  assert.equal(peer.events('peer').length, 3);
  assert.equal(env.lobby.guards.publicEndpoint.buckets.size, 0);
});
