import assert from 'node:assert/strict';
import { test } from 'node:test';

import { FLAG_HIDDEN, STORY_SESSION_ATTR } from '../src/lobby.js';
import { CAPABILITIES } from '../src/relay.js';
import { ROOM, makeLobby } from './harness.js';

const MODERN = { online_protocol: 1, version: '1.2.0', platform: 'Windows', story_session: true };
const OLD = { online_protocol: 1, version: '1.1.0', platform: 'Windows' };

const storyRoom = (step = 3, flags = 0) => ({ ...ROOM, flags, ints: [...ROOM.ints, [STORY_SESSION_ATTR, step]] });

function greeted(env, fields, ip) {
  const client = env.connect(ip);
  assert.equal(client.hello(fields).code, 'ok');
  return client;
}

function members(env, room) {
  return env.host.sql.get('SELECT COUNT(*) AS n FROM members WHERE room = ?', Number(room)).n;
}

test('the attribute id is private and hello advertises story_session next to the relay capabilities', () => {
  assert.equal(STORY_SESSION_ATTR, 0x7f01);
  const env = makeLobby();
  for (const fields of [MODERN, OLD]) {
    const reply = env.connect().hello(fields);
    assert.equal(reply.code, 'ok');
    assert.deepEqual(reply.capabilities, { ...CAPABILITIES, story_session: true });
    assert.equal(reply.capabilities.relay_supported, true);
    assert.equal(typeof reply.capabilities.relay_max_payload, 'number');
  }
});

test('a capable client creates a story room: stored hidden, out of random search, found by code', () => {
  const env = makeLobby();
  const host = greeted(env, MODERN);
  const created = host.call('room.create', storyRoom(3, 0));
  assert.equal(created.code, 'ok');
  const row = env.host.sql.get('SELECT flags, ints FROM rooms WHERE id = ?', Number(created.room.id));
  assert.ok(row.flags & FLAG_HIDDEN);
  assert.ok((created.room.flags & FLAG_HIDDEN) !== 0);
  const seeker = greeted(env, MODERN, '198.51.100.4');
  assert.equal(seeker.call('room.search', { world: 1 }).total, 0);
  const byCode = seeker.call('room.search', { code: created.room.code });
  assert.equal(byCode.total, 1);
  assert.equal(byCode.rooms[0].id, created.room.id);
  assert.deepEqual(byCode.rooms[0].ints.find(([id]) => id === STORY_SESSION_ATTR), [STORY_SESSION_ATTR, 3]);
});

test('an old client joining a story room by id or by code is refused and not inserted', () => {
  const env = makeLobby();
  const host = greeted(env, MODERN);
  const room = host.call('room.create', storyRoom()).room;
  const old = greeted(env, OLD, '198.51.100.4');
  for (const body of [{ room: room.id }, { code: room.code }]) {
    const reply = old.call('room.join', body);
    assert.equal(reply.code, 'incompatible');
    assert.equal(reply.story_session_required, true);
    assert.equal(reply.online_protocol, 1);
    assert.equal(reply.version, '1.2.0');
    assert.equal(members(env, room.id), 1);
  }
  assert.equal(host.events('room.member_joined').length, 0);
  assert.equal(old.call('room.search', { code: room.code }).code, 'ok');
});

test('a capable client joins a story room by id and by code', () => {
  const env = makeLobby();
  const host = greeted(env, MODERN);
  const room = host.call('room.create', storyRoom()).room;
  const first = greeted(env, MODERN, '198.51.100.4');
  assert.equal(first.call('room.join', { room: room.id }).code, 'ok');
  const second = greeted(env, MODERN, '198.51.100.5');
  assert.equal(second.call('room.join', { code: room.code }).code, 'ok');
  assert.equal(members(env, room.id), 3);
});

test('an old client cannot create a story room', () => {
  const env = makeLobby();
  const old = greeted(env, OLD);
  const reply = old.call('room.create', storyRoom());
  assert.equal(reply.code, 'bad_request');
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM rooms').n, 0);
  assert.equal(old.call('room.create', ROOM).code, 'ok');
});

test('an old owner cannot turn a normal room into a story room; a capable owner can', () => {
  const env = makeLobby();
  const old = greeted(env, OLD);
  const room = old.call('room.create', ROOM).room;
  const refused = old.call('room.set_external', { room: room.id, ints: [[STORY_SESSION_ATTR, 2]] });
  assert.equal(refused.code, 'bad_request');
  assert.equal(old.call('room.set_external', { room: room.id, ints: [[0x4c, 5]] }).code, 'ok');
  const modern = greeted(env, MODERN, '198.51.100.4');
  const own = modern.call('room.create', ROOM).room;
  assert.equal(modern.call('room.set_external', { room: own.id, ints: [[STORY_SESSION_ATTR, 2]] }).code, 'ok');
  const old2 = greeted(env, OLD, '198.51.100.5');
  assert.equal(old2.call('room.join', { room: own.id }).code, 'incompatible');
});

test('attribute value 0 is not a story session', () => {
  const env = makeLobby();
  const old = greeted(env, OLD);
  const zero = old.call('room.create', storyRoom(0));
  assert.equal(zero.code, 'ok');
  assert.equal(zero.room.flags & FLAG_HIDDEN, 0);
  const modern = greeted(env, MODERN, '198.51.100.4');
  const own = modern.call('room.create', storyRoom(0)).room;
  assert.equal(own.flags & FLAG_HIDDEN, 0);
  assert.equal(old.call('room.leave', { room: zero.room.id }).code, 'ok');
  assert.equal(old.call('room.join', { room: own.id }).code, 'ok');
});

test('normal rooms behave as before for old and new clients', () => {
  const env = makeLobby();
  const old = greeted(env, OLD);
  const room = old.call('room.create', ROOM).room;
  const modern = greeted(env, MODERN, '198.51.100.4');
  const found = modern.call('room.search', { world: 1 });
  assert.deepEqual([found.total, found.incompatible], [1, 0]);
  assert.equal(modern.call('room.join', { room: room.id }).code, 'ok');
  const second = greeted(env, MODERN, '198.51.100.5');
  const own = second.call('room.create', ROOM).room;
  const oldSeeker = greeted(env, OLD, '198.51.100.6');
  assert.equal(oldSeeker.call('room.search', { world: 1 }).total, 2);
  assert.equal(oldSeeker.call('room.join', { code: own.code }).code, 'ok');
});

test('a step-0 host room is hidden from random search but listed and joinable for a capable friend', () => {
  const env = makeLobby();
  const host = env.connect('203.0.113.1');
  const hostCode = host.hello({ ...MODERN, name: 'Saulo-A' }).friend_code;
  const guest = env.connect('198.51.100.1');
  assert.equal(guest.hello({ ...MODERN, name: 'Saulo-B' }).code, 'ok');
  assert.equal(guest.call('friends.add', { code: hostCode }).state, 'pending');
  assert.equal(host.call('friends.accept', { account: guest.account }).code, 'ok');

  const room = host.call('room.create', storyRoom(1)).room;
  host.call('presence.set', { data: 'ab'.repeat(0x78) });
  assert.equal(guest.call('room.search', { world: 1 }).total, 0);
  const friends = guest.call('friends.list').friends;
  assert.equal(friends.length, 1);
  assert.deepEqual([friends[0].online, friends[0].room, friends[0].code], [true, room.id, room.code]);
  assert.equal(friends[0].data, 'ab'.repeat(0x78));
  assert.equal(guest.call('room.join', { room: room.id }).code, 'ok');
  assert.equal(members(env, room.id), 2);
});

// The client's private live story state (EARLY_STORY_STATE_ATTR): the server stores and forwards it like any other
// internal attribute and knows nothing about it.
const STATE_ATTR = 0x7f02;
const state = (step) => `01${step.toString(16).padStart(2, '0')}`;

test('the live story state travels as an ordinary room internal attribute, in order, to members only', () => {
  const env = makeLobby();
  const host = greeted(env, MODERN);
  const created = host.call('room.create', storyRoom(1));
  const room = created.room;
  const guest = greeted(env, MODERN, '198.51.100.4');
  assert.equal(guest.call('room.join', { room: room.id }).code, 'ok');
  guest.events('room.member_joined');

  for (const step of [1, 2, 3, 4]) {
    assert.equal(host.call('room.set_internal', { room: room.id, bin_int: [[STATE_ATTR, state(step)]] }).code, 'ok');
  }
  const seen = guest.events('room.internal');
  assert.deepEqual(seen.map((e) => e.bin_int), [1, 2, 3, 4].map((step) => [[STATE_ATTR, state(step)]]));
  assert.ok(seen.every((e) => e.room === room.id && e.flags === created.room.flags));
  assert.equal(host.events('room.internal').length, 0);

  // unchanged: nothing goes out; the game's own update carries only its own attribute
  assert.equal(host.call('room.set_internal', { room: room.id, bin_int: [[STATE_ATTR, state(4)]] }).code, 'ok');
  assert.equal(guest.events('room.internal').length, 0);
  assert.equal(host.call('room.set_internal', { room: room.id, bin_int: [[0x57, 'cd']] }).code, 'ok');
  assert.deepEqual(guest.events('room.internal').map((e) => e.bin_int), [[[0x57, 'cd']]]);

  // a later member gets the current state with the join, after the game's attribute
  const late = greeted(env, MODERN, '198.51.100.5');
  const joined = late.call('room.join', { room: room.id });
  assert.deepEqual(joined.room.bin_int, [[0x57, 'cd'], [STATE_ATTR, state(4)]]);

  // only the owner sets it
  assert.equal(guest.call('room.set_internal', { room: room.id, bin_int: [[STATE_ATTR, state(9)]] }).code, 'not_owner');
  assert.equal(late.events('room.internal').length, 0);
});

test('room.set_external changes only the stored search attributes: members get no event', () => {
  const env = makeLobby();
  const host = greeted(env, MODERN);
  const room = host.call('room.create', storyRoom(1)).room;
  const guest = greeted(env, MODERN, '198.51.100.4');
  assert.equal(guest.call('room.join', { room: room.id }).code, 'ok');
  guest.events('room.member_joined');
  for (const value of [2, 3]) {
    assert.equal(host.call('room.set_external', { room: room.id, ints: [[STORY_SESSION_ATTR, value], [0x4d, value]] }).code, 'ok');
  }
  assert.deepEqual(guest.inbox.filter((m) => m.ev), []);
  const found = guest.call('room.search', { code: room.code }).rooms[0];
  assert.deepEqual(found.ints.find(([id]) => id === STORY_SESSION_ATTR), [STORY_SESSION_ATTR, 3]);
});
