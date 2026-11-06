// What a resumed seat learns about the room, and what the room learns about it.
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { LIMITS } from '../src/lobby.js';
import { encodeRelayFrame, parseRelayFrame, roomToken } from '../src/relay.js';
import { ROOM, makeLobby } from './harness.js';

const A_IP = '203.0.113.7';
const B_IP = '198.51.100.9';
const C_IP = '192.0.2.8';
const B_V6 = '2001:db8:2::20';
const ADVERTISED = '203.0.113.10';

function greeted(env, ip, fields = {}) {
  const client = env.connect(ip);
  assert.equal(client.hello(fields).code, 'ok');
  return client;
}

function openRoom(env, ip, port, fields) {
  const client = greeted(env, ip, fields);
  const reply = client.call('room.create', ROOM);
  assert.equal(reply.code, 'ok');
  client.room = reply.room;
  client.me = reply.me;
  client.call('udp', { port });
  return client;
}

function joined(env, room, ip, port, fields) {
  const client = greeted(env, ip, fields);
  const reply = client.call('room.join', { room: room.id });
  assert.equal(reply.code, 'ok');
  client.room = reply.room;
  client.me = reply.me;
  client.call('udp', { port });
  return client;
}

function twoPlayers(env, { b = B_IP, fields } = {}) {
  const a = openRoom(env, A_IP, 41000, fields);
  const guest = joined(env, a.room, b, 41001, fields);
  a.events('peer');
  guest.events('peer');
  return { a, b: guest };
}

function fresh(env, client, ip = client.ip, fields) {
  const back = env.connect(ip);
  back.account = client.account;
  back.secret = client.secret;
  assert.equal(back.hello(fields).code, 'ok');
  return back;
}

function resume(back, client) {
  const reply = back.call('room.resume', { room: client.room.id, member: client.me });
  assert.equal(reply.code, 'ok');
  back.room = reply.room;
  back.me = reply.me;
  return reply;
}

const endpoints = (client) => client.events('peer').map((e) => [e.member, e.ip, e.port]);

test('B drops and resumes: it gets A at once, and A gets B when B reports', () => {
  const env = makeLobby();
  const { a, b } = twoPlayers(env);
  b.drop();
  const back = fresh(env, b);
  assert.equal(resume(back, b).me, 0x20);
  assert.deepEqual(endpoints(back), [[0x10, A_IP, 41000]]);
  assert.deepEqual(endpoints(a), []);
  back.call('udp', { port: 41005 });
  assert.deepEqual(endpoints(a), [[0x20, B_IP, 41005]]);
  assert.deepEqual(env.host.sql.all('SELECT member, conn IS NOT NULL AS seated, grace_until FROM members ORDER BY member')
    .map((m) => [m.member, m.seated, m.grace_until]), [[0x10, 1, null], [0x20, 1, null]]);
});

test('a resumer that reported first reaches the others with the answer', () => {
  const env = makeLobby();
  const { a, b } = twoPlayers(env);
  b.drop();
  const back = fresh(env, b);
  back.call('udp', { port: 41006 });
  resume(back, b);
  assert.deepEqual(endpoints(a), [[0x20, B_IP, 41006]]);
  assert.deepEqual(endpoints(back), [[0x10, A_IP, 41000]]);
});

test('C joins while B is away: B resumes knowing C, after the event that names it', () => {
  const env = makeLobby();
  const { a, b } = twoPlayers(env);
  b.drop();
  const c = joined(env, a.room, C_IP, 41002);
  const back = fresh(env, b);
  resume(back, b);
  const order = back.inbox.map((e) => e.ev);
  assert.ok(order.indexOf('room.member_joined') < order.indexOf('peer'), order.join());
  assert.deepEqual(endpoints(back), [[0x10, A_IP, 41000], [0x30, C_IP, 41002]]);
  c.events('peer');
  back.call('udp', { port: 41007 });
  assert.deepEqual(endpoints(c), [[0x20, B_IP, 41007]]);
  assert.deepEqual(endpoints(a).at(-1), [0x20, B_IP, 41007]);
  assert.deepEqual(env.host.sql.all('SELECT member FROM members ORDER BY member').map((m) => m.member), [0x10, 0x20, 0x30]);
});

test('C changes its endpoint while B is away: B gets the current one only', () => {
  const env = makeLobby();
  const { a, b } = twoPlayers(env);
  b.drop();
  const c = joined(env, a.room, C_IP, 41002);
  c.call('udp', { port: 41200, lan: [{ ip: '192.168.1.30', port: 3658 }] });
  const back = fresh(env, b);
  resume(back, b);
  assert.deepEqual(endpoints(back), [[0x10, A_IP, 41000], [0x30, C_IP, 41200]]);
});

test('a member who left while B was away is gone from what B receives', () => {
  const env = makeLobby();
  const { a, b } = twoPlayers(env);
  b.drop();
  const c = joined(env, a.room, C_IP, 41002);
  c.call('room.leave', { room: a.room.id });
  const back = fresh(env, b);
  resume(back, b);
  assert.deepEqual(back.inbox.filter((e) => ['room.member_joined', 'peer.gone', 'room.member_left'].includes(e.ev)).map((e) => e.ev),
    ['room.member_joined', 'peer.gone', 'room.member_left']);
  assert.deepEqual(endpoints(back), [[0x10, A_IP, 41000]]);
});

test('a member with no endpoint yet is not announced, and fills in when it reports', () => {
  const env = makeLobby();
  const { a, b } = twoPlayers(env);
  b.drop();
  const c = greeted(env, C_IP);
  assert.equal(c.call('room.join', { room: a.room.id }).code, 'ok');
  const back = fresh(env, b);
  resume(back, b);
  assert.deepEqual(endpoints(back), [[0x10, A_IP, 41000]]);
  c.call('udp', { port: 41002 });
  assert.deepEqual(endpoints(back), [[0x30, C_IP, 41002]]);
});

test('a relay-only peer resumes: seat, routes and fallback survive', () => {
  const env = makeLobby({ log: () => {} });
  const fields = { relay: true, online_protocol: 1 };
  const { a, b } = twoPlayers(env, { b: B_V6, fields });
  const token = roomToken(a.room.key);
  a.relay(encodeRelayFrame(token, b.me, 1, Uint8Array.of(1, 2, 3)));
  assert.equal(b.binary.length, 1);
  b.drop();
  const back = fresh(env, b, B_V6, fields);
  resume(back, b);
  assert.deepEqual(endpoints(back), [[0x10, A_IP, 41000]]);
  back.call('udp', { port: 41001 });
  const [{ ip, port, lan }] = a.events('peer');
  assert.deepEqual([ip, port, lan], [B_V6, 41001, undefined]);
  a.relay(encodeRelayFrame(token, b.me, 2, Uint8Array.of(4, 5, 6)));
  assert.equal(parseRelayFrame(back.binary.at(-1)).member, a.me);
  back.relay(encodeRelayFrame(token, a.me, 1, Uint8Array.of(7)));
  assert.equal(parseRelayFrame(a.binary.at(-1)).member, b.me);
});

test('a resumed IPv6 peer is offered by its public IPv4 when it has one, else by its IPv6', () => {
  const env = makeLobby();
  const { a, b } = twoPlayers(env, { b: B_V6 });
  b.drop();
  const back = fresh(env, b, B_V6);
  back.call('udp', { port: 41001, public_ip: ADVERTISED });
  resume(back, b);
  assert.deepEqual(endpoints(a), [[0x20, ADVERTISED, 41001]]);
  assert.deepEqual(env.host.sql.get('SELECT ip, public_ip FROM members WHERE member = 32'), { ip: B_V6, public_ip: ADVERTISED });

  const c = joined(env, a.room, '2001:db8:3::30', 41002);
  b.events('peer');
  back.events('peer');
  c.drop();
  const again = fresh(env, c, '2001:db8:3::30');
  resume(again, c);
  assert.deepEqual(endpoints(again), [[0x10, A_IP, 41000], [0x20, ADVERTISED, 41001]]);
  again.call('udp', { port: 41003 });
  assert.deepEqual(endpoints(back), [[0x30, '2001:db8:3::30', 41003]]);
});

test('LAN candidates go only to a resumer behind the same observed address', () => {
  const env = makeLobby();
  const lan = [{ ip: '192.168.1.10', port: 3658 }];
  const a = openRoom(env, A_IP, 41000);
  a.call('udp', { port: 41000, lan });
  const near = joined(env, a.room, A_IP, 41001);
  const far = joined(env, a.room, B_IP, 41002);
  near.drop();
  far.drop();
  const nearBack = fresh(env, near);
  resume(nearBack, near);
  const farBack = fresh(env, far);
  resume(farBack, far);
  assert.deepEqual(nearBack.events('peer').find((e) => e.member === 0x10).lan, lan);
  assert.equal(farBack.events('peer').find((e) => e.member === 0x10).lan, undefined);
});

test('after the seat grace a resume fails, with the alarm run or still due, and tells nobody', () => {
  const env = makeLobby();
  const { a, b } = twoPlayers(env);
  b.drop();
  env.clock.now += LIMITS.graceMs;
  const late = fresh(env, b);
  assert.equal(late.call('room.resume', { room: a.room.id, member: 0x20 }).code, 'not_found');
  assert.deepEqual(endpoints(late), []);
  assert.deepEqual(endpoints(a), []);
  assert.equal(env.host.sql.get('SELECT conn FROM members WHERE member = 32').conn, null);
  env.advance(1);
  assert.deepEqual(a.events('room.member_left').map((e) => e.why), ['disconnect']);
  assert.equal(late.call('room.resume', { room: a.room.id, member: 0x20 }).code, 'not_found');
});

test('a resume that is refused keeps the relay routes of the seat', () => {
  const env = makeLobby({ log: () => {} });
  const fields = { relay: true, online_protocol: 1 };
  const { a, b } = twoPlayers(env, { fields });
  const token = roomToken(a.room.key);
  a.relay(encodeRelayFrame(token, b.me, 1, Uint8Array.of(1)));
  assert.ok(env.lobby.relays.routes.size > 0);
  env.host.conns.delete(b.conn);
  const back = fresh(env, b, B_IP, fields);
  assert.equal(back.call('room.resume', { room: a.room.id, member: b.me }).code, 'not_found');
  assert.ok(env.lobby.relays.routes.size > 0);
});

test('a resumer receives nothing about another room', () => {
  const env = makeLobby();
  const { a, b } = twoPlayers(env);
  const other = openRoom(env, '203.0.113.40', 42000);
  const neighbour = joined(env, other.room, '203.0.113.41', 42001);
  other.events('peer');
  neighbour.events('peer');
  b.drop();
  const back = fresh(env, b);
  resume(back, b);
  const peers = back.events('peer');
  assert.deepEqual(peers.map((e) => [e.room, e.member]), [[a.room.id, 0x10]]);
  back.call('udp', { port: 41008 });
  assert.deepEqual(endpoints(other), []);
  assert.deepEqual(endpoints(neighbour), []);
});

test('someone else cannot resume into a seat to read its room', () => {
  const env = makeLobby();
  const { a, b } = twoPlayers(env);
  b.drop();
  const stranger = greeted(env, C_IP);
  assert.equal(stranger.call('room.resume', { room: a.room.id, member: 0x20 }).code, 'not_found');
  assert.deepEqual(endpoints(stranger), []);
});

test('a new connection taking a seat from a live socket learns the room as well', () => {
  const env = makeLobby();
  const { a, b } = twoPlayers(env);
  const c = joined(env, a.room, C_IP, 41002);
  b.events('peer');
  const back = fresh(env, b);
  resume(back, b);
  assert.equal(b.closedWith?.code, 4001);
  assert.deepEqual(endpoints(back), [[0x10, A_IP, 41000], [0x30, C_IP, 41002]]);
  assert.deepEqual(endpoints(b), []);
});
