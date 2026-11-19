// Custom looks travel with the room seat: published by its member, relayed to the members that declared appearance, given
// to a member that joins later, and gone with the seat.
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { LIMITS } from '../src/lobby.js';
import { PROTECTION } from '../src/abuse.js';
import { ROOM, makeLobby } from './harness.js';

const MODERN = { online_protocol: 1, version: '1.4.0', platform: 'Windows', appearance: true };
const OLD = { online_protocol: 1, version: '1.3.0', platform: 'Windows' };
// schema 1, Fighter, base 0, 2 parts: armor (1) coloured #22AA55, helmet (8) hidden
const FIGHTER = '01000002010222aa550801';
const FIGHTER_RED = '010000010102aa0000';
// Elf, base 1, hood (1) hidden
const ELF = '010201010101';

const look = (data) => ({ schema: parseInt(data.slice(0, 2), 16), data });

function greeted(env, ip, fields = MODERN) {
  const client = env.connect(ip);
  assert.equal(client.hello(fields).code, 'ok');
  return client;
}

function host(env, fields = MODERN) {
  const client = greeted(env, '203.0.113.7', fields);
  const reply = client.call('room.create', ROOM);
  assert.equal(reply.code, 'ok');
  client.room = reply.room;
  client.me = reply.me;
  client.reply = reply;
  return client;
}

function guest(env, room, ip, fields = MODERN) {
  const client = greeted(env, ip, fields);
  const reply = client.call('room.join', { room: room.id });
  assert.equal(reply.code, 'ok');
  client.room = reply.room;
  client.me = reply.me;
  client.reply = reply;
  return client;
}

function publish(client, data) {
  return client.call('room.appearance', { room: client.room.id, appearance: data === null ? null : look(data) });
}

function stored(env, room, member) {
  return env.host.sql.get('SELECT appearance FROM members WHERE room = ? AND member = ?', Number(room.id), member)?.appearance;
}

test('hello advertises appearance, and only a client that declared it may publish', () => {
  const env = makeLobby();
  assert.equal(env.connect().hello(MODERN).capabilities.appearance, true);
  const old = host(env, OLD);
  assert.equal(old.reply.appearances, undefined);
  const refused = publish(old, FIGHTER);
  assert.equal(refused.code, 'bad_request');
  assert.equal(stored(env, old.room, old.me), '');
});

test('a published look is stored with the seat and relayed to the other capable members only', () => {
  const env = makeLobby();
  const a = host(env);
  assert.deepEqual(a.reply.appearances, []);
  const b = guest(env, a.room, '198.51.100.4');
  const old = guest(env, a.room, '198.51.100.5', OLD);
  assert.equal(publish(a, FIGHTER).code, 'ok');
  assert.equal(stored(env, a.room, a.me), FIGHTER);
  const relayed = b.events('room.appearance');
  assert.equal(relayed.length, 1);
  assert.deepEqual(relayed[0], { ev: 'room.appearance', room: a.room.id, member: a.me, appearance: look(FIGHTER) });
  assert.equal(a.events('room.appearance').length, 0);
  assert.equal(old.events('room.appearance').length, 0);
  assert.ok(!old.inbox.some((m) => JSON.stringify(m).includes(FIGHTER)));
});

test('an identical publish is answered without a write, a relay or budget', () => {
  const env = makeLobby();
  const a = host(env);
  const b = guest(env, a.room, '198.51.100.4');
  assert.equal(publish(a, FIGHTER).code, 'ok');
  b.events('room.appearance');
  for (let i = 0; i < PROTECTION.appearanceAccount.burst * 3; i++) assert.equal(publish(a, FIGHTER).code, 'ok');
  assert.equal(b.events('room.appearance').length, 0);
  assert.equal(publish(a, FIGHTER_RED).code, 'ok');
  assert.equal(b.events('room.appearance').length, 1);
});

test('a member who joins later gets every look in its join answer, and its own look reaches the others', () => {
  const env = makeLobby();
  const a = host(env);
  const b = guest(env, a.room, '198.51.100.4');
  assert.equal(publish(a, FIGHTER).code, 'ok');
  assert.equal(publish(b, ELF).code, 'ok');
  const c = guest(env, a.room, '192.0.2.8');
  const snapshot = new Map(c.reply.appearances.map((e) => [e.member, e.appearance]));
  assert.equal(snapshot.size, 2);
  assert.deepEqual(snapshot.get(a.me), look(FIGHTER));
  assert.deepEqual(snapshot.get(b.me), look(ELF));
  a.events('room.appearance');
  b.events('room.appearance');
  assert.equal(publish(c, FIGHTER_RED).code, 'ok');
  for (const other of [a, b]) {
    const [event] = other.events('room.appearance');
    assert.equal(event.member, c.me);
    assert.deepEqual(event.appearance, look(FIGHTER_RED));
  }
  const old = guest(env, a.room, '192.0.2.9', OLD);
  assert.equal(old.reply.appearances, undefined);
});

test('clearing a look relays null and leaves no snapshot entry', () => {
  const env = makeLobby();
  const a = host(env);
  const b = guest(env, a.room, '198.51.100.4');
  assert.equal(publish(a, FIGHTER).code, 'ok');
  b.events('room.appearance');
  assert.equal(publish(a, null).code, 'ok');
  assert.deepEqual(b.events('room.appearance').map((e) => e.appearance), [null]);
  assert.equal(stored(env, a.room, a.me), '');
  const c = guest(env, a.room, '192.0.2.8');
  assert.deepEqual(c.reply.appearances, []);
});

test('a look leaves with its seat: leave, kick and an expired grace; the room takes the rest with it', () => {
  const env = makeLobby();
  const a = host(env);
  const b = guest(env, a.room, '198.51.100.4');
  assert.equal(publish(b, ELF).code, 'ok');
  assert.equal(b.call('room.leave', { room: a.room.id }).code, 'ok');
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM members WHERE appearance != ?', '').n, 0);
  const b2 = guest(env, a.room, '198.51.100.4');
  assert.equal(b2.me, b.me);
  assert.deepEqual(b2.reply.appearances, []);
  assert.equal(stored(env, a.room, b2.me), '');

  assert.equal(publish(b2, ELF).code, 'ok');
  assert.equal(a.call('room.kick', { room: a.room.id, member: b2.me }).code, 'ok');
  assert.equal(stored(env, a.room, b2.me), undefined);

  const c = guest(env, a.room, '192.0.2.8');
  assert.equal(publish(c, FIGHTER).code, 'ok');
  c.drop();
  env.advance(LIMITS.graceMs + 1);
  assert.equal(stored(env, a.room, c.me), undefined);

  assert.equal(publish(a, FIGHTER).code, 'ok');
  assert.equal(a.call('room.leave', { room: a.room.id }).code, 'ok');
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM rooms').n, 0);
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM members').n, 0);
});

test('a resumed seat keeps its look and gets the room snapshot', () => {
  const env = makeLobby();
  const a = host(env);
  const b = guest(env, a.room, '198.51.100.4');
  assert.equal(publish(a, FIGHTER).code, 'ok');
  assert.equal(publish(b, ELF).code, 'ok');
  b.drop();
  assert.equal(publish(a, FIGHTER_RED).code, 'ok');
  const back = env.connect('198.51.100.4');
  back.account = b.account;
  back.secret = b.secret;
  assert.equal(back.hello(MODERN).code, 'ok');
  const resumed = back.call('room.resume', { room: a.room.id, member: b.me });
  assert.equal(resumed.code, 'ok');
  assert.deepEqual(resumed.appearances, [{ member: a.me, appearance: look(FIGHTER_RED) }]);
  assert.equal(stored(env, a.room, b.me), ELF);
  assert.equal(back.events('room.appearance').length, 0);
});

test('malformed or oversized looks are refused and change nothing', () => {
  const env = makeLobby();
  const a = host(env);
  const b = guest(env, a.room, '198.51.100.4');
  assert.equal(publish(a, FIGHTER).code, 'ok');
  b.events('room.appearance');
  const bad = [
    {},
    { schema: 1 },
    { schema: 0, data: '00' },
    { schema: 256, data: '01' },
    { schema: 1.5, data: '01' },
    { schema: 1, data: '' },
    { schema: 1, data: '0' },
    { schema: 1, data: 'zz' },
    { schema: 1, data: '02aa' },
    { schema: 1, data: 7 },
    { schema: 1, data: `01${'00'.repeat(LIMITS.appearanceBytes)}` },
    'x',
    [],
  ];
  for (const appearance of bad) {
    const reply = a.call('room.appearance', { room: a.room.id, appearance });
    assert.equal(reply.code, 'bad_request', JSON.stringify(appearance));
  }
  assert.equal(a.call('room.appearance', { room: a.room.id }).code, 'bad_request');
  assert.equal(a.call('room.appearance', { room: '999999', appearance: look(ELF) }).code, 'not_found');
  assert.equal(stored(env, a.room, a.me), FIGHTER);
  assert.equal(b.events('room.appearance').length, 0);
  const largest = `01${'00'.repeat(LIMITS.appearanceBytes - 1)}`;
  assert.equal(publish(a, largest).code, 'ok');
});

test('a client outside a room has nothing to publish', () => {
  const env = makeLobby();
  const lone = greeted(env, '203.0.113.7');
  assert.equal(lone.call('room.appearance', { room: '4096', appearance: look(ELF) }).code, 'not_found');
});

test('appearance updates have their own budget, which a reconnect does not refill', () => {
  const env = makeLobby();
  const a = host(env);
  const b = guest(env, a.room, '198.51.100.4');
  const looks = [FIGHTER, FIGHTER_RED];
  let sent = 0;
  for (; sent < PROTECTION.appearanceAccount.burst; sent++) assert.equal(publish(a, looks[sent % 2]).code, 'ok');
  const limited = publish(a, looks[sent % 2]);
  assert.equal(limited.code, 'limit');
  assert.ok(limited.retry_ms > 0);
  assert.equal(b.events('room.appearance').length, PROTECTION.appearanceAccount.burst);
  assert.equal(publish(b, ELF).code, 'ok');
  assert.equal(a.call('room.set_internal', { room: a.room.id, bin_int: [[0x57, '01']] }).code, 'ok');
  env.advance(Math.ceil(1000 / PROTECTION.appearanceAccount.perSecond));
  assert.equal(publish(a, looks[sent % 2]).code, 'ok');
  a.drop();
  const back = env.connect(a.ip);
  back.account = a.account;
  back.secret = a.secret;
  assert.equal(back.hello(MODERN).code, 'ok');
  back.room = a.room;
  assert.equal(back.call('room.resume', { room: a.room.id, member: a.me }).code, 'ok');
  assert.equal(publish(back, looks[(sent + 1) % 2]).code, 'limit');
});

test('the lobby never logs a look and the dashboard shows none', async () => {
  const lines = [];
  const env = makeLobby({ log: (text) => lines.push(text) });
  const a = host(env);
  const b = guest(env, a.room, '198.51.100.4');
  assert.equal(publish(a, FIGHTER).code, 'ok');
  assert.equal(publish(b, ELF).code, 'ok');
  for (let i = 0; i < PROTECTION.appearanceAccount.burst; i++) publish(a, i % 2 ? FIGHTER : FIGHTER_RED);
  env.lobby.alarm();
  assert.ok(!lines.some((line) => line.includes(FIGHTER) || line.includes(ELF) || line.includes('22aa55')));
  const dashboard = JSON.stringify({ summary: env.lobby.metricsSummary(), live: env.lobby.liveMetrics(),
    history: env.lobby.metricsHistory(30) });
  for (const secret of [FIGHTER, FIGHTER_RED, ELF, '22aa55', 'appearance"']) assert.ok(!dashboard.includes(secret), secret);
});
