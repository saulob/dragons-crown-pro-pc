// What one established socket can make the lobby do. Every frame that arrives is already a Durable Object invocation;
// these tests bound how many an abusive socket gets handled before it is closed, and what each one costs.
import assert from 'node:assert/strict';
import { serialize } from 'node:v8';
import { test } from 'node:test';

import { PROTECTION } from '../src/abuse.js';
import { DIAGNOSTICS } from '../src/metrics.js';
import { RELAY, encodeRelayFrame, roomToken } from '../src/relay.js';
import { ROOM, makeLobby } from './harness.js';

const STRIKES = PROTECTION.refusedRequests;
const CONTROL_BURST = 60;
const CONTROL_PER_SECOND = 20;
// PeerSocket's own relay cap (src/net/peer_socket.cpp), shared by all peers of one socket.
const CLIENT_RELAY = { frames: 300, burst: 600, bytesPerSecond: 96 * 1024 };
const ATTACHMENT_LIMIT = 2048;

let nextIp = 1;
const address = () => `198.18.${nextIp >> 8}.${nextIp++ & 0xff}`;

function signedIn(env, fields = {}) {
  const client = env.connect(address());
  assert.equal(client.hello({ relay: true, online_protocol: 1, ...fields }).code, 'ok');
  return client;
}

function room(env, size) {
  const members = Array.from({ length: size }, () => signedIn(env));
  const created = members[0].call('room.create', ROOM);
  members[0].me = created.me;
  for (const member of members.slice(1)) member.me = member.call('room.join', { room: created.room.id }).me;
  for (const member of members) member.room = created.room;
  return { members, token: roomToken(created.room.key) };
}

const changes = (env) => env.host.sql.get('SELECT total_changes() AS n').n;

// Feeds one socket until the lobby closes it (or `cap` frames), then checks that further frames are not handled at all.
function attack(env, client, send, cap = 100_000) {
  const before = changes(env);
  let handled = 0;
  while (!client.closedWith && handled < cap) send(handled++);
  const rows = changes(env) - before;
  const counted = () => [...env.lobby.metrics.pending.values()].reduce((a, b) => a + b, 0);
  const seen = counted();
  for (let i = 0; i < 1000; i++) send(handled + i);
  return { handled, closedWith: client.closedWith, rows, ignoredAfterClose: counted() === seen };
}

// Diagnostics are written as one row per counter at the next flush, however many frames they counted.
function flushRows(env) {
  const before = changes(env);
  env.lobby.track(() => env.lobby.metrics.flushDiagnostics());
  return changes(env) - before;
}

// ---- the real client's envelope ----

test('a launcher with the Lobby tab open for an hour is never refused', () => {
  const env = makeLobby({ log: () => {} });
  const launcher = env.connect(address());
  assert.equal(launcher.hello({ lobby: true, name: 'Watcher' }).code, 'ok');
  const fetch = () => ['friends.list', 'friends.requests'].map((op) => launcher.call(op).code);
  const codes = [...fetch(), launcher.call('lobby.publish', { visible: true }).code, launcher.call('lobby.list').code];
  for (let second = 1; second <= 3600; second++) {
    env.advance(1000);
    if (second % 30 === 0) codes.push(...fetch());
    if (second % 300 === 0) codes.push(launcher.call('lobby.list').code);
  }
  assert.ok(codes.every((code) => code === 'ok'), [...new Set(codes)].join());
  assert.equal(launcher.closedWith, null);
});

test('one request before sign-in is all the client sends, and a reconnect backlog fits the burst', () => {
  const env = makeLobby({ log: () => {} });
  const lookup = env.connect(address());
  assert.equal(lookup.call('incident.status', { reports: [] }).code, 'ok');
  assert.equal(lookup.closedWith, null);
  const game = signedIn(env);
  for (let i = 0; i < CONTROL_BURST - 1; i++) assert.equal(game.call('friends.list').code, 'ok');
  assert.equal(game.closedWith, null);
});

test('four members relaying at the client cap to every peer for a minute lose nothing', () => {
  const env = makeLobby({ log: () => {} });
  const { members, token } = room(env, 4);
  const size = Math.floor(CLIENT_RELAY.bytesPerSecond / CLIENT_RELAY.frames) - RELAY.headerBytes;
  const body = new Uint8Array(size);
  let sent = 0;
  for (const from of members) {
    const peers = members.filter((m) => m !== from);
    for (let i = 0; i < CLIENT_RELAY.burst; i++) from.relay(encodeRelayFrame(token, peers[i % 3].me, i, body));
    sent += CLIENT_RELAY.burst;
  }
  for (let tick = 0; tick < 6000; tick++) {
    env.advance(10);
    for (const from of members) {
      for (const to of members) {
        if (to !== from) from.relay(encodeRelayFrame(token, to.me, tick, body));
      }
    }
    sent += 12;
  }
  assert.equal(members.reduce((n, m) => n + m.binary.length, 0), sent);
  for (const member of members) {
    assert.equal(member.closedWith, null);
    assert.equal(member.events('relay.closed').length, 0);
  }
});

test('the frames a client still sends around a peer leaving, its own leave or a peer resuming never close it', () => {
  const env = makeLobby({ log: () => {} });
  const { members: [a, b, c], token } = room(env, 3);
  const body = new Uint8Array(100);
  assert.equal(b.call('room.leave', { room: b.room.id }).code, 'ok');
  for (let i = 0; i < 3; i++) a.relay(encodeRelayFrame(token, b.me, i, body));
  for (let i = 0; i < 60; i++) b.relay(encodeRelayFrame(token, a.me, i, body));
  c.drop();
  for (let second = 0; second < 30; second++) {
    for (let i = 0; i < CLIENT_RELAY.frames / 2; i++) a.relay(encodeRelayFrame(token, c.me, i, body));
    env.advance(1000);
  }
  assert.equal(a.closedWith, null);
  assert.equal(b.closedWith, null);
});

test('the worst attachment a socket can carry, with every abuse field set, fits what the runtime stores', () => {
  const env = makeLobby({ log: () => {} });
  const client = env.connect('2001:db8:aaaa:bbbb:cccc:dddd:eeee:ffff', 'BR');
  assert.equal(client.hello({ relay: true, online_protocol: 1, version: '1.0.0-beta.1', platform: 'Steam Deck',
    name: 'P'.repeat(16), story_session: true, lobby: true, game: 'G'.repeat(32) }).code, 'ok');
  assert.equal(client.call('presence.set', { data: 'ab'.repeat(0x80), status: 'cd'.repeat(0xc0) }).code, 'ok');
  assert.equal(client.call('room.create', ROOM).code, 'ok');
  const bucket = [123.456789012, 1_700_000_000_000];
  const worst = { ...client.conn.attachment(), party: '4096:0123:-', rc: bucket, ra: bucket, rr: bucket, wstate: bucket,
    wfriend: bucket, strikes: bucket, early: 3, abusive: true, relayMalformed: bucket, relayStale: bucket, overruns: bucket,
    relayOff: 4_000_000 };
  const bytes = serialize(worst).length;
  assert.ok(bytes < ATTACHMENT_LIMIT - 400, `${bytes} bytes`);
});

// ---- the attack harness ----

test('before sign-in: text, malformed and binary floods each end after a handful of frames, costing no row', (t) => {
  const env = makeLobby({ log: () => {} });
  const frame = encodeRelayFrame(1, 0x20, 1, new Uint8Array(40));
  for (const [name, send] of [
    ['control', (c) => () => c.call('worlds')],
    ['malformed', (c) => () => c.raw('{')],
    ['unknown op', (c) => () => c.call('room.nuke')],
    ['binary', (c) => () => c.relay(frame)],
  ]) {
    const client = env.connect(address());
    const result = attack(env, client, send(client));
    t.diagnostic(`pre-auth ${name}: ${result.handled} frames handled before 1008`);
    assert.equal(result.handled, PROTECTION.preHelloFrames + 1);
    assert.deepEqual(result.closedWith, { code: 1008, reason: 'sign in first' });
    assert.equal(result.rows, 0);
    assert.ok(result.ignoredAfterClose);
  }
});

test('before sign-in, text and binary frames share one count', () => {
  const env = makeLobby({ log: () => {} });
  const client = env.connect(address());
  const frame = encodeRelayFrame(1, 0x20, 1, new Uint8Array(40));
  client.call('worlds');
  for (let i = 0; i < PROTECTION.preHelloFrames - 1; i++) client.relay(frame);
  assert.equal(client.closedWith, null);
  client.call('worlds');
  assert.deepEqual(client.closedWith, { code: 1008, reason: 'sign in first' });
});

test('after sign-in: malformed envelopes, unknown ops and a message flood end the socket within a bounded count', (t) => {
  const env = makeLobby({ log: () => {} });
  for (const [name, send, bound] of [
    ['malformed', (c) => () => c.raw('not json'), STRIKES.burst + 1],
    ['unknown op', (c) => () => c.call('room.nuke'), STRIKES.burst + 1],
    ['oversized', (c) => () => c.call('worlds', { pad: 'x'.repeat(33 * 1024) }), STRIKES.burst + 1],
    ['flood', (c) => () => c.call('worlds'), CONTROL_BURST - 1 + STRIKES.burst + 1],
  ]) {
    const client = signedIn(env);
    const result = attack(env, client, send(client));
    t.diagnostic(`control ${name}: ${result.handled} frames handled before 1008`);
    assert.equal(result.handled, bound);
    assert.equal(result.closedWith.code, 1008);
    assert.equal(result.rows, 0);
    assert.ok(result.ignoredAfterClose);
  }
});

test('a sustained flood closes even while the budget refills; a sender at the refill rate is only slowed', (t) => {
  const env = makeLobby({ log: () => {} });
  const flood = signedIn(env);
  const result = attack(env, flood, () => {
    env.advance(10);
    flood.call('worlds');
  });
  t.diagnostic(`control flood at 100/s: ${result.handled} frames handled before 1008`);
  assert.ok(result.handled < CONTROL_BURST + 2 * STRIKES.burst);

  const steady = signedIn(env);
  const codes = new Set();
  for (let i = 0; i < 60 * CONTROL_PER_SECOND; i++) {
    env.advance(1000 / CONTROL_PER_SECOND);
    codes.add(steady.call('worlds').code);
  }
  assert.deepEqual([...codes], ['ok']);
  assert.equal(steady.closedWith, null);
});

test('the odd burst over the message budget is refused without closing the socket', () => {
  const env = makeLobby({ log: () => {} });
  const client = signedIn(env);
  for (let round = 0; round < 100; round++) {
    const codes = Array.from({ length: CONTROL_BURST + 1 }, () => client.call('worlds').code);
    assert.equal(codes.filter((code) => code === 'limit').length >= 1, true);
    env.advance(60_000);
  }
  assert.equal(client.closedWith, null);
});

test('relay: malformed, wrong-token and stale-member floods end the sender, never its peers', (t) => {
  const env = makeLobby({ log: () => {} });
  const body = new Uint8Array(40);
  for (const [name, frame, bound] of [
    ['malformed', () => new Uint8Array([0x44, 0x52, 9]), RELAY.malformed.burst + 1],
    ['bad token', (token, to) => encodeRelayFrame(token ^ 1, to, 1, body), RELAY.stale.burst + 1],
    ['no destination', (token) => encodeRelayFrame(token, 0x40, 1, body), RELAY.stale.burst + 1],
  ]) {
    const { members: [a, b], token } = room(env, 2);
    const result = attack(env, a, () => a.relay(frame(token, b.me)));
    t.diagnostic(`relay ${name}: ${result.handled} frames handled before 1008`);
    assert.equal(result.handled, bound);
    assert.deepEqual(result.closedWith, { code: 1008, reason: 'relay abuse' });
    assert.equal(result.rows, 0);
    assert.ok(result.ignoredAfterClose);
    assert.equal(b.closedWith, null);
    assert.equal(b.call('worlds').code, 'ok');
  }
});

test('relay: a sender over its packet or byte budget is suspended, then closed if it carries on', (t) => {
  for (const [name, size] of [['packets', 40], ['bytes', RELAY.maxPayload]]) {
    const env = makeLobby({ log: () => {} });
    const { members: [a, b, c], token } = room(env, 3);
    const body = new Uint8Array(size);
    const result = attack(env, a, (i) => a.relay(encodeRelayFrame(token, b.me, i, body)));
    const overBudget = result.handled - b.binary.length;
    t.diagnostic(`relay ${name} overrun: ${b.binary.length} forwarded, ${overBudget} over budget before 1008`);
    assert.equal(overBudget, RELAY.overruns.burst + 1 + RELAY.stale.burst + 1);
    assert.equal(a.events('relay.closed').length, 1);
    assert.equal(result.closedWith.code, 1008);
    assert.ok(result.rows <= 1);
    for (const peer of [b, c]) assert.equal(peer.closedWith, null);
    c.relay(encodeRelayFrame(token, b.me, 1, body));
    assert.equal(b.binary.at(-1).length, RELAY.headerBytes + size);
    assert.equal(c.call('udp', { port: 4000 }).code, 'ok');
  }
});

test('a peer with a full buffer or a busy room drops frames without striking the sender', () => {
  const env = makeLobby({ log: () => {} });
  const { members: [a, b], token } = room(env, 2);
  b.bufferedAmount = RELAY.bufferedBytes + 1;
  for (let i = 0; i < 5000; i++) {
    a.relay(encodeRelayFrame(token, b.me, i, new Uint8Array(40)));
    env.advance(5);
  }
  assert.equal(b.binary.length, 0);
  assert.equal(a.closedWith, null);
  assert.equal(a.events('relay.closed').length, 0);
  assert.equal(a.conn.attachment().overruns, undefined);
});

test('a room pushed past its budget by its busiest member drops frames, and strikes nobody', () => {
  const env = makeLobby({ log: () => {} });
  const { members, token } = room(env, 4);
  const [noisy, ...quiet] = members;
  const body = new Uint8Array(100);
  let sent = 0;
  for (let tick = 0; tick < 6000; tick++) {
    env.advance(10);
    for (let i = 0; i < 4; i++, sent++) noisy.relay(encodeRelayFrame(token, quiet[i % 3].me, tick, body));
    for (const [n, from] of quiet.entries()) {
      for (let i = 0; i < 3; i++, sent++) from.relay(encodeRelayFrame(token, quiet[(n + 1 + (i % 2)) % 3].me, tick, body));
    }
  }
  const delivered = members.reduce((n, m) => n + m.binary.length, 0);
  assert.ok(delivered < sent, `${delivered} of ${sent}`);
  for (const member of members) {
    assert.equal(member.closedWith, null);
    assert.equal(member.events('relay.closed').length, 0);
    assert.equal(member.conn.attachment().overruns, undefined);
  }
});

test('whatever a flood counted, its diagnostics cost one row per counter at the next flush', () => {
  const env = makeLobby({ log: () => {} });
  for (let i = 0; i < 20; i++) {
    const client = env.connect(address());
    attack(env, client, () => client.call('worlds'));
  }
  const { members: [a, b], token } = room(env, 2);
  attack(env, a, (i) => a.relay(encodeRelayFrame(token, b.me, i, new Uint8Array(40))));
  attack(env, b, () => b.call('worlds'));
  const rows = flushRows(env);
  assert.ok(rows <= DIAGNOSTICS.size + 4, `${rows} rows`);
  assert.equal(flushRows(env), 0);
});

test('the counters a future incident needs are all there, and an incident leaves them aggregate', () => {
  const env = makeLobby({ log: () => {} });
  const silent = env.connect(address());
  attack(env, silent, () => silent.call('worlds'));
  const flood = signedIn(env);
  attack(env, flood, () => flood.call('worlds'));
  const { members: [a, b], token } = room(env, 2);
  attack(env, a, (i) => a.relay(encodeRelayFrame(token, b.me, i, new Uint8Array(40))));
  const junk = signedIn(env);
  attack(env, junk, () => junk.relay(new Uint8Array([1])));
  const pending = Object.fromEntries(env.lobby.metrics.pending);
  for (const name of ['ws_text_messages', 'ws_binary_messages', 'ws_control_limited', 'ws_preauth_closed', 'relay_overrun',
    'relay_invalid', 'relay_suspended', 'relay_abuse_closed', 'lobby_abuse_closed']) {
    assert.ok(pending[name] > 0, name);
  }
  assert.equal(Object.keys(pending).every((name) => DIAGNOSTICS.has(name)), true);
  assert.equal(flushRows(env) > 0, true);
  const rows = env.host.sql.all(`SELECT value FROM metrics_daily_dimensions WHERE dimension = 'abuse'`).map((r) => r.value);
  assert.ok(rows.every((name) => DIAGNOSTICS.has(name)));
});
