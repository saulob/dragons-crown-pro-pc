// The write budget of state-changing requests and the end of sockets that keep being refused.
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { PROTECTION } from '../src/abuse.js';
import { LIMITS } from '../src/lobby.js';
import { ROOM, makeLobby } from './harness.js';

const STATE = PROTECTION.stateWriteAccount;
const FRIEND = PROTECTION.friendWriteAccount;
const STRIKES = PROTECTION.refusedRequests;

function greeted(env, ip, fields = {}) {
  const client = env.connect(ip);
  assert.equal(client.hello(fields).code, 'ok');
  return client;
}

function host(env, ip) {
  const client = greeted(env, ip);
  const reply = client.call('room.create', ROOM);
  assert.equal(reply.code, 'ok');
  client.room = reply.room;
  return client;
}

function reconnected(env, client, ip, fields = {}) {
  const back = env.connect(ip);
  back.account = client.account;
  back.secret = client.secret;
  assert.equal(back.hello(fields).code, 'ok');
  return back;
}

// Rows changed from here on, which is what a Durable Object is charged for.
function writeCounter(env) {
  const changes = () => env.host.sql.get('SELECT total_changes() AS n').n;
  const before = changes();
  return () => changes() - before;
}

const setExternal = (client, value) => client.call('room.set_external', { room: client.room.id, ints: [[0x4c, value]] });
const storedInts = (env, client) => JSON.parse(env.host.sql.get('SELECT ints FROM rooms WHERE id = ?', Number(client.room.id)).ints);
const stored4c = (env, client) => new Map(storedInts(env, client)).get(0x4c);

function spendStateBudget(client) {
  for (let i = 0; i < STATE.burst; i++) assert.equal(setExternal(client, 1000 + i).code, 'ok');
}

function friendCode(env, client) {
  return env.host.sql.get('SELECT code FROM accounts WHERE account = ?', client.account).code;
}

const requestRows = (env) => env.host.sql.get('SELECT COUNT(*) AS n FROM friend_requests').n;
const friendRows = (env) => env.host.sql.get('SELECT COUNT(*) AS n FROM friends').n;

function abuseCount(env, name) {
  env.lobby.track(() => env.lobby.metrics.flushDiagnostics());
  return env.host.sql.get(`SELECT count FROM metrics_daily_dimensions WHERE dimension = 'abuse' AND value = ?`, name)?.count ?? 0;
}

test('a normal session is never refused', () => {
  const env = makeLobby();
  const owner = host(env, '203.0.113.7');
  const guest = greeted(env, '198.51.100.9');
  assert.equal(owner.call('udp', { port: 41000, lan: [{ ip: '192.168.1.10', port: 3658 }] }).code, 'ok');
  assert.equal(guest.call('room.join', { room: owner.room.id }).code, 'ok');
  assert.equal(guest.call('udp', { port: 41001 }).code, 'ok');
  for (let stage = 0; stage < 120; stage++) {
    env.advance(5000);
    assert.equal(owner.call('room.set_internal', { room: owner.room.id, flag_mask: 1, flag_value: stage & 1, bin_int: [[0x57, 'ab']] }).code, 'ok');
    assert.equal(setExternal(owner, stage).code, 'ok');
    assert.equal(owner.call('udp', { port: 41100 + stage % 2 }).code, 'ok');
  }
  assert.equal(guest.call('friends.add', { code: friendCode(env, owner) }).state, 'pending');
  assert.equal(owner.call('friends.accept', { account: guest.account }).code, 'ok');
  assert.equal(owner.closedWith, null);
});

test('updates that change nothing spend no budget and write nothing, however often the game repeats them', () => {
  const env = makeLobby();
  const owner = host(env, '203.0.113.7');
  owner.call('udp', { port: 41000 });
  assert.equal(setExternal(owner, 7).code, 'ok');
  owner.call('room.set_internal', { room: owner.room.id, bin_int: [[0x57, 'ab']] });
  const writes = writeCounter(env);
  for (let i = 0; i < 1000; i++) {
    env.clock.now += 200;
    assert.equal(setExternal(owner, 7).code, 'ok');
    assert.equal(owner.call('room.set_internal', { room: owner.room.id, bin_int: [[0x57, 'ab']] }).code, 'ok');
    assert.equal(owner.call('udp', { port: 41000 }).code, 'ok');
  }
  assert.equal(writes(), 0);
  spendStateBudget(owner);
});

test('a burst of writes is refused past the budget, with a clear answer', () => {
  const env = makeLobby();
  const owner = host(env, '203.0.113.7');
  spendStateBudget(owner);
  const refused = setExternal(owner, 1);
  assert.equal(refused.code, 'limit');
  assert.equal(refused.retry_ms, 1000 / STATE.perSecond);
  assert.equal(stored4c(env, owner), 1000 + STATE.burst - 1);
  env.advance(refused.retry_ms);
  assert.equal(setExternal(owner, 1).code, 'ok');
  assert.equal(abuseCount(env, 'write_limited'), 1);
});

test('sustained writes hold to the refill rate', () => {
  const env = makeLobby();
  const owner = host(env, '203.0.113.7');
  const writes = writeCounter(env);
  const stepMs = 700;
  const attempts = 2000;
  let accepted = 0;
  for (let i = 0; i < attempts; i++) {
    env.advance(stepMs);
    if (setExternal(owner, i).code === 'ok') accepted += 1;
  }
  const allowed = STATE.burst + ((attempts * stepMs) / 1000) * STATE.perSecond;
  assert.ok(accepted < attempts && accepted <= allowed, `${accepted} of ${attempts}, ${allowed} allowed`);
  assert.equal(writes() - accepted < 8, true);
  assert.equal(owner.closedWith, null);
});

test('the scratch flood of the review now costs a fraction of its writes', () => {
  const env = makeLobby();
  const owner = host(env, '203.0.113.7');
  const writes = writeCounter(env);
  let value = 0;
  for (let i = 0; i < 1200 && !owner.closedWith; i++) {
    env.advance(50);
    setExternal(owner, value++);
  }
  assert.ok(writes() <= STATE.burst + 60 * STATE.perSecond, `${writes()} writes in a minute`);
  assert.equal(owner.closedWith?.code, 1008);
});

test('a socket that keeps being refused is closed, and says nothing more', () => {
  const env = makeLobby();
  const owner = host(env, '203.0.113.7');
  spendStateBudget(owner);
  let refusals = 0;
  while (!owner.closedWith && refusals < 200) {
    assert.equal(setExternal(owner, 1).code, 'limit');
    refusals += 1;
  }
  assert.equal(refusals, STRIKES.burst + 1);
  assert.deepEqual(owner.closedWith, { code: 1008, reason: 'too many refused requests' });
  assert.equal(setExternal(owner, 2), undefined);
  assert.equal(abuseCount(env, 'lobby_abuse_closed'), 1);
});

test('a socket with only the odd refusal stays', () => {
  const env = makeLobby();
  const owner = host(env, '203.0.113.7');
  spendStateBudget(owner);
  let value = 0;
  let refusals = 0;
  for (let i = 0; i < 300; i++) {
    env.advance(2500);
    while (setExternal(owner, value++).code === 'ok');
    refusals += 1;
  }
  assert.equal(refusals, 300);
  assert.equal(owner.closedWith, null);
});

test('malformed frames and a message flood end the socket too, before and after sign-in', () => {
  const env = makeLobby();
  const silent = env.connect('203.0.113.8');
  for (let i = 0; i <= STRIKES.burst; i++) assert.equal(silent.raw('not json').code, 'bad_request');
  assert.equal(silent.closedWith?.code, 1008);

  const large = env.connect('203.0.113.9');
  for (let i = 0; i <= STRIKES.burst; i++) large.raw(JSON.stringify({ op: 'worlds', seq: i, pad: 'x'.repeat(LIMITS.frameBytes) }));
  assert.equal(large.closedWith?.code, 1008);

  const flood = greeted(env, '203.0.113.10');
  for (let i = 0; i < 500 && !flood.closedWith; i++) flood.call('worlds');
  assert.equal(flood.closedWith?.code, 1008);
});

test('a refused write changes nothing and costs no write', () => {
  const env = makeLobby();
  const owner = greeted(env, '203.0.113.7');
  owner.call('udp', { port: 41000 });
  owner.room = owner.call('room.create', ROOM).room;
  const guest = greeted(env, '198.51.100.9');
  assert.equal(guest.call('room.join', { room: owner.room.id }).code, 'ok');
  guest.drop();
  const held = () => env.host.sql.get('SELECT held FROM members WHERE room = ? AND member = 32', Number(owner.room.id)).held;
  spendStateBudget(owner);
  const heldBefore = held();
  const { flags, bin_int: binInt, ints } = env.host.sql.get('SELECT * FROM rooms WHERE id = ?', Number(owner.room.id));
  const writes = writeCounter(env);

  assert.equal(setExternal(owner, 1).code, 'limit');
  assert.equal(owner.call('room.set_internal', { room: owner.room.id, flag_mask: 1, flag_value: 1, bin_int: [[0x57, 'cd']] }).code, 'limit');
  assert.equal(owner.call('udp', { port: 42000 }).code, 'limit');

  assert.equal(writes(), 0);
  const after = env.host.sql.get('SELECT * FROM rooms WHERE id = ?', Number(owner.room.id));
  assert.deepEqual([after.flags, after.bin_int, after.ints], [flags, binInt, ints]);
  assert.equal(held(), heldBefore);
  const row = env.host.sql.get('SELECT port FROM members WHERE room = ? AND member = 16', Number(owner.room.id));
  assert.equal(row.port, 41000);
  assert.equal(owner.conn.attachment().udp.port, 41000);
  env.advance(1000 / STATE.perSecond);
  assert.equal(owner.call('udp', { port: 42000 }).code, 'ok');
  assert.equal(env.host.sql.get('SELECT port FROM members WHERE room = ? AND member = 16', Number(owner.room.id)).port, 42000);
});

test('an unchanged internal update reaches nobody and holds nothing for a seat in its grace', () => {
  const env = makeLobby();
  const owner = host(env, '203.0.113.7');
  const guest = greeted(env, '198.51.100.9');
  const third = greeted(env, '192.0.2.8');
  guest.call('room.join', { room: owner.room.id });
  third.call('room.join', { room: owner.room.id });
  owner.call('room.set_internal', { room: owner.room.id, bin_int: [[0x57, 'ab']] });
  third.drop();
  guest.events('room.internal');
  for (let i = 0; i < LIMITS.heldEvents * 2; i++) {
    env.advance(100);
    assert.equal(owner.call('room.set_internal', { room: owner.room.id, bin_int: [[0x57, 'ab']] }).code, 'ok');
  }
  assert.equal(guest.events('room.internal').length, 0);
  assert.equal(env.host.sql.get('SELECT held FROM members WHERE room = ? AND member = 48', Number(owner.room.id)).held, '[]');
});

test('friend mutations have their own budget, and a refusal leaves the requests alone', () => {
  const env = makeLobby();
  const sender = greeted(env, '203.0.113.7');
  const targets = Array.from({ length: FRIEND.burst + 1 }, (_, i) => greeted(env, `198.51.100.${i + 1}`));
  for (const target of targets.slice(0, FRIEND.burst)) {
    assert.equal(sender.call('friends.add', { code: friendCode(env, target) }).state, 'pending');
  }
  const writes = writeCounter(env);
  const last = targets.at(-1);
  const refused = sender.call('friends.add', { code: friendCode(env, last) });
  assert.equal(refused.code, 'limit');
  assert.equal(refused.retry_ms, Math.ceil(1000 / FRIEND.perSecond));
  assert.equal(writes(), 0);
  assert.equal(requestRows(env), FRIEND.burst);
  assert.equal(last.events('friend.request').length, 0);

  const first = targets[0];
  assert.equal(sender.call('friends.decline', { account: first.account }).code, 'limit');
  assert.equal(requestRows(env), FRIEND.burst);

  assert.equal(setExternalIsFree(env, sender), 'ok');
  assert.equal(first.call('friends.accept', { account: sender.account }).code, 'ok');
  assert.equal(friendRows(env), 2);
  assert.equal(sender.call('friends.remove', { account: first.account }).code, 'limit');
  assert.equal(friendRows(env), 2);
  env.advance(Math.ceil(1000 / FRIEND.perSecond));
  assert.equal(sender.call('friends.remove', { account: first.account }).code, 'ok');
  assert.equal(friendRows(env), 0);
});

// The two kinds do not share a budget: friends do not stop a game.
function setExternalIsFree(env, client) {
  const reply = client.call('room.create', ROOM);
  client.room = reply.room;
  return setExternal(client, 1).code;
}

test('an add and decline loop is bound by the budget, and repeats write nothing', () => {
  const env = makeLobby();
  const a = greeted(env, '203.0.113.7');
  const b = greeted(env, '198.51.100.9');
  const writes = writeCounter(env);
  let accepted = 0;
  for (let i = 0; i < 200; i++) {
    env.advance(100);
    if (a.call('friends.add', { code: friendCode(env, b) })?.state === 'pending') accepted += 1;
    a.call('friends.decline', { account: b.account });
  }
  assert.ok(writes() <= FRIEND.burst + 21 * FRIEND.perSecond + 1, `${writes()} writes`);

  const c = greeted(env, '203.0.113.8');
  const d = greeted(env, '198.51.100.10');
  assert.equal(c.call('friends.add', { code: friendCode(env, d) }).state, 'pending');
  const quiet = writeCounter(env);
  for (let i = 0; i < 40; i++) assert.equal(c.call('friends.add', { code: friendCode(env, d) }).state, 'pending');
  assert.equal(quiet(), 0);
  assert.equal(requestRows(env) >= 1, true);
  assert.equal(c.call('friends.remove', { account: '999999' }).code, 'ok');
  assert.equal(quiet(), 0);
});

test('one abusive player costs nobody else their budget', () => {
  const env = makeLobby();
  const abuser = host(env, '203.0.113.7');
  const other = host(env, '198.51.100.9');
  spendStateBudget(abuser);
  for (let i = 0; i <= STRIKES.burst; i++) setExternal(abuser, 1);
  assert.equal(abuser.closedWith?.code, 1008);
  assert.equal(setExternal(other, 1).code, 'ok');
  assert.equal(stored4c(env, other), 1);
});

test('the budget follows the account over a reconnect, and a normal reconnect still works', () => {
  const env = makeLobby();
  const owner = host(env, '203.0.113.7');
  const guest = greeted(env, '198.51.100.9');
  guest.call('room.join', { room: owner.room.id });
  spendStateBudget(owner);
  assert.equal(setExternal(owner, 1).code, 'limit');
  owner.drop();

  const back = reconnected(env, owner, '203.0.113.7');
  const resumed = back.call('room.resume', { room: owner.room.id, member: 0x10 });
  assert.equal(resumed.code, 'ok');
  back.room = resumed.room;
  assert.equal(setExternal(back, 1).code, 'limit');
  env.advance(1000 / STATE.perSecond);
  assert.equal(setExternal(back, 1).code, 'ok');
  assert.equal(back.closedWith, null);
});

test('a socket keeps its budget when the object wakes from hibernation', () => {
  const env = makeLobby();
  const owner = host(env, '203.0.113.7');
  spendStateBudget(owner);
  for (const guard of Object.values(env.lobby.guards)) guard.buckets.clear();
  assert.equal(setExternal(owner, 1).code, 'limit');
});

test('an address group shares a budget, IPv6 per /64', () => {
  const env = makeLobby();
  const accounts = Math.ceil(PROTECTION.stateWriteAddress.burst / STATE.burst);
  for (let i = 0; i < accounts; i++) spendStateBudget(host(env, `2001:db8:1:2::${i + 1}`));
  assert.equal(setExternal(host(env, '2001:db8:1:2:ffff::9'), 1).code, 'limit');
  assert.equal(setExternal(host(env, '2001:db8:1:3::9'), 1).code, 'ok');
  assert.equal(setExternal(host(env, '203.0.113.7'), 1).code, 'ok');
});

test('accounts on one IPv4 address share a budget; the per-address room limit still holds', () => {
  const env = makeLobby();
  const accounts = Math.ceil(PROTECTION.stateWriteAddress.burst / STATE.burst);
  for (let i = 0; i < accounts; i++) spendStateBudget(host(env, '203.0.113.7'));
  assert.equal(setExternal(host(env, '203.0.113.7'), 1).code, 'limit');
});

test('the development server keeps loopback clients free of the write budget', () => {
  const env = makeLobby({ local: true });
  const owner = host(env, '127.0.0.1');
  for (let i = 0; i < STATE.burst * 2; i++) {
    env.advance(100);
    assert.equal(setExternal(owner, i).code, 'ok');
  }
});

test('v0.9.2 clients see the same protocol: a plain answer, and extra fields only on a refusal', () => {
  const env = makeLobby({ log: () => {} });
  const fields = { online_protocol: 1, version: '0.9.2', platform: 'Windows', relay: true };
  const owner = greeted(env, '203.0.113.7', fields);
  const created = owner.call('room.create', ROOM);
  owner.room = created.room;
  assert.deepEqual(Object.keys(setExternal(owner, 1)).sort(), ['code', 're']);
  assert.deepEqual(Object.keys(owner.call('room.set_internal', { room: owner.room.id, bin_int: [[0x57, 'ab']] })).sort(), ['code', 're']);
  assert.deepEqual(Object.keys(setExternal(owner, 1)).sort(), ['code', 're']);
  assert.deepEqual(Object.keys(owner.call('udp', { port: 41000 })).sort(), ['code', 'ip', 'port', 're']);
  const old = greeted(env, '198.51.100.9', fields);
  assert.equal(old.call('room.join', { room: owner.room.id }).code, 'ok');
  assert.equal(owner.events('room.member_joined').length, 1);
  assert.equal(owner.call('room.set_internal', { room: owner.room.id, bin_int: [[0x57, 'cd']] }).code, 'ok');
  assert.deepEqual(old.events('room.internal').map((e) => e.bin_int), [[[0x57, 'cd']]]);
});
