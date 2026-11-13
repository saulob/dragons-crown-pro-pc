import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';

import { PROTOCOL } from '../src/lobby.js';
import { CAPABILITIES, RELAY, encodeRelayFrame, parseRelayFrame, roomToken } from '../src/relay.js';
import { ROOM, makeLobby } from './harness.js';

const payload = (n, seed = 1) => Uint8Array.from({ length: n }, (_, i) => (seed + i * 31) & 0xff);

function env() {
  const logs = [];
  const e = makeLobby({ log: (text) => logs.push(text) });
  e.logs = logs;
  return e;
}

function player(e, { relay = true, ip = '203.0.113.7' } = {}) {
  const client = e.connect(ip);
  const reply = client.hello({ relay, online_protocol: 1, version: '0.9.0', platform: 'Windows' });
  assert.equal(reply.code, 'ok');
  client.helloReply = reply;
  return client;
}

function openRoom(host) {
  const reply = host.call('room.create', ROOM);
  assert.equal(reply.code, 'ok');
  host.me = reply.me;
  host.room = reply.room;
  return reply.room;
}

function join(client, room) {
  const reply = client.call('room.join', { room: room.id });
  assert.equal(reply.code, 'ok');
  client.me = reply.me;
  client.room = reply.room;
}

function threeInARoom(e) {
  const [a, b, c] = [player(e), player(e), player(e)];
  const room = openRoom(a);
  join(b, room);
  join(c, room);
  return { a, b, c, room };
}

const frameTo = (from, toMember, body = payload(40), seq = 1, token = roomToken(from.room.key)) =>
  encodeRelayFrame(token, toMember, seq, body);

const lastDelivered = (client) => parseRelayFrame(client.binary.at(-1));

test('hello advertises the relay and its largest payload; a client that did not ask still gets a normal answer', () => {
  const e = env();
  const modern = player(e);
  assert.deepEqual(modern.helloReply.capabilities, { ...CAPABILITIES, story_session: true });
  assert.equal(modern.helloReply.capabilities.relay_max_payload, RELAY.maxPayload);
  const legacy = player(e, { relay: false });
  assert.equal(legacy.helloReply.code, 'ok');
  assert.ok(e.logs.some((l) => l.includes('relay: negotiated')));
});

test('relay frames encode and decode; malformed ones say why', () => {
  const bytes = encodeRelayFrame(0xdeadbeef, 0x20, 7, payload(10));
  assert.deepEqual(parseRelayFrame(bytes), { token: 0xdeadbeef, member: 0x20, seq: 7, length: 10 });
  assert.equal(parseRelayFrame(bytes.subarray(0, 3)).error, 'truncated');
  assert.equal(parseRelayFrame(bytes.subarray(0, 10)).error, 'truncated');
  const wrongMagic = bytes.slice();
  wrongMagic[0] = 0x53;
  assert.equal(parseRelayFrame(wrongMagic).error, 'bad_magic');
  const wrongVersion = bytes.slice();
  wrongVersion[2] = 9;
  assert.equal(parseRelayFrame(wrongVersion).error, 'bad_version');
  const wrongType = bytes.slice();
  wrongType[3] = 9;
  assert.equal(parseRelayFrame(wrongType).error, 'bad_type');
  assert.equal(parseRelayFrame(bytes.subarray(0, bytes.length - 1)).error, 'bad_length');
  assert.equal(parseRelayFrame(encodeRelayFrame(1, 1, 1, new Uint8Array(0))).error, 'bad_length');
  assert.equal(parseRelayFrame(encodeRelayFrame(1, 1, 1, payload(RELAY.maxPayload + 1))).error, 'oversized');
  assert.equal(parseRelayFrame(encodeRelayFrame(1, 1, 1, payload(RELAY.maxPayload))).length, RELAY.maxPayload);
  assert.equal(parseRelayFrame('text').error, 'not_binary');
});

test('a member reaches another member of its room and back, unchanged but for the source', () => {
  const e = env();
  const { a, b } = threeInARoom(e);
  const body = payload(1175, 5);
  a.relay(frameTo(a, b.me, body, 11));
  assert.equal(b.binary.length, 1);
  const delivered = lastDelivered(b);
  assert.equal(delivered.member, a.me);
  assert.equal(delivered.seq, 11);
  assert.equal(delivered.token, roomToken(a.room.key));
  assert.deepEqual(b.binary[0].subarray(RELAY.headerBytes), body);
  b.relay(frameTo(b, a.me, payload(20), 3));
  assert.equal(lastDelivered(a).member, b.me);
  assert.equal(a.binary.length, 1);
});

test('three members stay distinct: a frame reaches only the member it names', () => {
  const e = env();
  const { a, b, c } = threeInARoom(e);
  a.relay(frameTo(a, b.me, payload(30, 1)));
  a.relay(frameTo(a, c.me, payload(31, 2)));
  assert.equal(b.binary.length, 1);
  assert.equal(c.binary.length, 1);
  assert.equal(b.binary[0].length, RELAY.headerBytes + 30);
  assert.equal(c.binary[0].length, RELAY.headerBytes + 31);
  assert.equal(a.binary.length, 0);
  c.relay(frameTo(c, b.me, payload(32, 3)));
  assert.equal(lastDelivered(b).member, c.me);
  assert.equal(b.binary.length, 2);
});

test('the source is the sender\'s own member, whatever the frame says', () => {
  const e = env();
  const { a, b, c } = threeInARoom(e);
  a.relay(frameTo(a, b.me));
  assert.equal(lastDelivered(b).member, a.me);
  assert.notEqual(lastDelivered(b).member, c.me);
  a.relay(frameTo(a, a.me));
  assert.equal(a.binary.length, 0);
  assert.ok(e.logs.some((l) => l.includes('reason=self')));
});

test('nothing crosses into another room, even to the same member number', () => {
  const e = env();
  const { a, b } = threeInARoom(e);
  const [x, y] = [player(e), player(e)];
  const other = openRoom(x);
  join(y, other);
  assert.equal(y.me, b.me);
  a.relay(frameTo(a, b.me));
  assert.equal(b.binary.length, 1);
  assert.equal(y.binary.length, 0);
  x.relay(frameTo(x, y.me));
  assert.equal(y.binary.length, 1);
  assert.equal(b.binary.length, 1);
  x.relay(encodeRelayFrame(roomToken(a.room.key), y.me, 1, payload(10)));
  a.relay(encodeRelayFrame(roomToken(other.key), b.me, 1, payload(10)));
  assert.equal(y.binary.length, 1);
  assert.equal(b.binary.length, 1);
});

test('a connection that is not in a room, or never asked for the relay, relays nothing', () => {
  const e = env();
  const { a, b } = threeInARoom(e);
  const outsider = player(e);
  outsider.relay(encodeRelayFrame(roomToken(a.room.key), b.me, 1, payload(10)));
  assert.equal(b.binary.length, 0);
  const legacy = player(e, { relay: false });
  join(legacy, a.room);
  legacy.relay(encodeRelayFrame(roomToken(a.room.key), b.me, 1, payload(10)));
  a.relay(frameTo(a, legacy.me));
  assert.equal(b.binary.length, 0);
  assert.equal(legacy.binary.length, 0);
  assert.equal(legacy.inbox.filter((m) => m.ev === 'relay.closed').length, 0);
});

test('a frame to a member that does not exist, or with a stale token, is dropped', () => {
  const e = env();
  const { a, b } = threeInARoom(e);
  a.relay(frameTo(a, 0x99));
  a.relay(frameTo(a, b.me, payload(10), 1, roomToken(a.room.key) ^ 1));
  a.relay(frameTo(a, b.me, payload(10), 1, 0));
  assert.equal(b.binary.length, 0);
  assert.ok(e.logs.some((l) => l.includes('reason=no_destination')));
  assert.ok(e.logs.some((l) => l.includes('reason=bad_token')));
  assert.equal(a.closedWith, null);
});

test('malformed and oversized frames are dropped without closing the socket', () => {
  const e = env();
  const { a, b } = threeInARoom(e);
  const good = frameTo(a, b.me);
  const bad = [good.subarray(0, 5), good.subarray(0, good.length - 1), new Uint8Array(0),
    encodeRelayFrame(roomToken(a.room.key), b.me, 1, payload(RELAY.maxPayload + 1)), Uint8Array.of(1, 2, 3, 4, 5, 6, 7, 8)];
  for (const frame of bad) a.relay(frame);
  assert.equal(b.binary.length, 0);
  assert.equal(a.closedWith, null);
  a.relay(good);
  assert.equal(b.binary.length, 1);
});

test('leaving the room ends the route in both directions', () => {
  const e = env();
  const { a, b } = threeInARoom(e);
  a.relay(frameTo(a, b.me));
  b.relay(frameTo(b, a.me));
  assert.equal(b.binary.length + a.binary.length, 2);
  assert.equal(b.call('room.leave', { room: b.room.id }).code, 'ok');
  a.relay(frameTo(a, b.me));
  b.relay(frameTo(b, a.me));
  assert.equal(b.binary.length, 1);
  assert.equal(a.binary.length, 1);
});

test('a disconnect ends the route; the peer gets nothing more', () => {
  const e = env();
  const { a, b } = threeInARoom(e);
  a.relay(frameTo(a, b.me));
  assert.equal(b.binary.length, 1);
  b.drop();
  a.relay(frameTo(a, b.me));
  assert.equal(b.binary.length, 1);
  b.relay(frameTo(b, a.me));
  assert.equal(a.binary.length, 0);
});

test('a resumed seat replaces its old socket: the stale one relays nothing', () => {
  const e = env();
  const { a, b } = threeInARoom(e);
  a.relay(frameTo(a, b.me));
  assert.equal(b.binary.length, 1);
  const fresh = e.connect('203.0.113.9');
  fresh.account = a.account;
  assert.equal(fresh.hello({ relay: true, online_protocol: 1 }).code, 'ok');
  assert.equal(fresh.call('room.resume', { room: a.room.id, member: a.me }).code, 'ok');
  fresh.room = a.room;
  fresh.me = a.me;
  a.relay(frameTo(a, b.me));
  assert.equal(b.binary.length, 1);
  assert.deepEqual(a.closedWith?.code, 4001);
  fresh.relay(frameTo(fresh, b.me));
  assert.equal(b.binary.length, 2);
  assert.equal(lastDelivered(b).member, a.me);
});

test('a kicked member and a destroyed room stop relaying', () => {
  const e = env();
  const { a, b, c } = threeInARoom(e);
  a.relay(frameTo(a, b.me));
  assert.equal(a.call('room.kick', { room: a.room.id, member: b.me }).code, 'ok');
  a.relay(frameTo(a, b.me));
  b.relay(frameTo(b, a.me));
  assert.equal(b.binary.length, 1);
  assert.equal(a.binary.length, 0);
  assert.equal(c.call('room.leave', { room: c.room.id }).code, 'ok');
  assert.equal(a.call('room.leave', { room: a.room.id }).code, 'ok');
  a.relay(frameTo(a, c.me));
  assert.equal(c.binary.length, 0);
});

test('rate limits drop a flood and then close the relay for the sender; normal rates pass', () => {
  const e = env();
  const { a, b } = threeInARoom(e);
  const frame = frameTo(a, b.me, payload(100));
  for (let i = 0; i < 70; i++) {
    a.relay(frame);
    e.advance(1000 / 65);
  }
  assert.equal(b.binary.length, 70);
  for (let i = 0; i < RELAY.connection.packets.burst * 2; i++) a.relay(frame);
  assert.ok(b.binary.length < 70 + RELAY.connection.packets.burst + 10);
  assert.ok(e.logs.some((l) => l.includes('reason=rate_limit')));
  for (let i = 0; i < RELAY.overrunsToClose; i++) a.relay(frame);
  assert.equal(a.events('relay.closed').length, 1);
  const forwarded = b.binary.length;
  e.advance(60000);
  a.relay(frame);
  assert.equal(b.binary.length, forwarded);
});

test('the room budget is shared by its members', () => {
  const e = env();
  const { a, b, c } = threeInARoom(e);
  const senders = [a, b, c];
  const frame = (s, to) => frameTo(s, to.me, payload(100));
  let sent = 0;
  for (let round = 0; round < 700; round++) {
    for (const s of senders) {
      const to = senders[(senders.indexOf(s) + 1) % 3];
      s.relay(frame(s, to));
      sent += 1;
    }
  }
  const delivered = a.binary.length + b.binary.length + c.binary.length;
  assert.ok(sent > RELAY.room.packets.burst);
  assert.ok(delivered <= RELAY.room.packets.burst + 5);
  assert.ok(e.logs.some((l) => l.includes('reason=room_rate_limit')));
});

test('a peer socket with a full buffer gets nothing; it gets frames again once it drains', () => {
  const e = env();
  const { a, b } = threeInARoom(e);
  b.bufferedAmount = RELAY.bufferedBytes + 1;
  a.relay(frameTo(a, b.me));
  assert.equal(b.binary.length, 0);
  assert.ok(e.logs.some((l) => l.includes('reason=backpressure')));
  b.bufferedAmount = 0;
  e.advance(1000);
  a.relay(frameTo(a, b.me));
  assert.equal(b.binary.length, 1);
});

test('text signalling keeps working while frames are relayed', () => {
  const e = env();
  const { a, b } = threeInARoom(e);
  a.relay(frameTo(a, b.me));
  assert.equal(a.call('room.set_external', { room: a.room.id, ints: [[0x4c, 2]] }).code, 'ok');
  assert.equal(b.call('worlds').code, 'ok');
  b.relay(frameTo(b, a.me));
  assert.equal(a.binary.length, 1);
});

test('a client that never sends relay in hello behaves as before', () => {
  const e = env();
  const legacy = player(e, { relay: false });
  const room = openRoom(legacy);
  assert.equal(room.members.length, 1);
  const other = player(e, { relay: false });
  join(other, room);
  assert.equal(other.call('room.leave', { room: room.id }).code, 'ok');
  assert.ok(!e.logs.some((l) => l.startsWith('relay: route')));
});

test('the byte limits bind before the packet limits: per connection, then per room', () => {
  const e = env();
  const { a, b, c } = threeInARoom(e);
  const big = (from, to) => frameTo(from, to.me, payload(RELAY.maxPayload));
  const frameBytes = RELAY.headerBytes + RELAY.maxPayload;
  let forwarded = 0;
  for (let i = 0; i < 400; i++) a.relay(big(a, b));
  forwarded = b.binary.length;
  assert.ok(forwarded * frameBytes <= RELAY.connection.bytes.burst + frameBytes);
  assert.ok(forwarded < RELAY.connection.packets.burst);
  assert.ok(e.logs.some((l) => l.includes('reason=rate_limit')));
  const room = env();
  const trio = threeInARoom(room);
  let delivered = 0;
  for (let i = 0; i < 400; i++) {
    for (const [from, to] of [[trio.a, trio.b], [trio.b, trio.c], [trio.c, trio.a]]) from.relay(big(from, to));
  }
  delivered = trio.a.binary.length + trio.b.binary.length + trio.c.binary.length;
  assert.ok(delivered * frameBytes <= RELAY.room.bytes.burst + frameBytes);
  assert.ok(room.logs.some((l) => l.includes('reason=room_rate_limit')));
  assert.equal(c.closedWith, null);
});

test('a closed relay stays closed for that connection and its state is released with the socket', () => {
  const e = env();
  const { a, b } = threeInARoom(e);
  const frame = frameTo(a, b.me, payload(50));
  for (let i = 0; i < RELAY.connection.packets.burst + RELAY.overrunsToClose + 5; i++) a.relay(frame);
  assert.equal(a.events('relay.closed').length, 1);
  assert.ok(e.lobby.relays.connections.size > 0 && e.lobby.relays.routes.size > 0);
  const delivered = b.binary.length;
  e.advance(120000);
  a.relay(frame);
  assert.equal(b.binary.length, delivered);
  a.drop();
  b.drop();
  assert.equal(e.lobby.relays.routes.size, 0);
  assert.equal(e.lobby.relays.connections.size, 0);
});

test('route and room state do not outlive their members', () => {
  const e = env();
  const { a, b, c } = threeInARoom(e);
  for (const [from, to] of [[a, b], [b, c], [c, a]]) from.relay(frameTo(from, to.me));
  assert.ok(e.lobby.relays.routes.size >= 3 && e.lobby.relays.rooms.size === 1);
  for (const client of [a, b, c]) assert.equal(client.call('room.leave', { room: a.room.id }).code, 'ok');
  assert.equal(e.lobby.relays.routes.size, 0);
  assert.equal(e.lobby.relays.rooms.size, 0);
});

test('relay fields are additive: older clients and the room protocol are unaffected', () => {
  const e = env();
  const legacy = e.connect();
  const reply = legacy.hello({ online_protocol: undefined, version: undefined, platform: undefined });
  assert.equal(reply.code, 'ok');
  for (const field of ['account', 'name', 'friend_code', 'ip', 'time']) assert.ok(field in reply, field);
  assert.deepEqual(Object.keys(reply).sort(), ['account', 'capabilities', 'code', 'friend_code', 'ip', 're', 'name', 'time'].sort());
  const created = legacy.call('room.create', ROOM);
  assert.equal(created.code, 'ok');
  assert.ok(!('relay' in created) && !('relay_token' in created.room));
  assert.equal(PROTOCOL, 1);
  const read = (file) => readFileSync(new URL(`../../../${file}`, import.meta.url), 'utf8');
  assert.match(read('src/net/online_version.h'), /kOnlineProtocolVersion = 1;/);
  assert.match(read('src/net/lobby_link.h'), /kLobbyProtocol = 1;/);
  assert.match(read('src/net/peer_socket.cpp'), /kVersion = 1;/);
});
