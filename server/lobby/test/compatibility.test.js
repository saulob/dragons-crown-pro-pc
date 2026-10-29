import assert from 'node:assert/strict';
import { test } from 'node:test';

import { Lobby } from '../src/lobby.js';
import { LocalHost, sqliteStore } from '../local/host.js';
import { ROOM, makeLobby } from './harness.js';

const CURRENT = { online_protocol: 1, version: '0.8.0', platform: 'Windows' };
const LEGACY = { online_protocol: undefined, version: undefined, platform: undefined };

function greeted(env, fields, ip = '203.0.113.7') {
  const client = env.connect(ip);
  assert.equal(client.hello(fields).code, 'ok');
  return client;
}

function befriend(a, b) {
  assert.equal(a.call('friends.add', { code: b.friendCode }).state, 'pending');
  assert.equal(b.call('friends.accept', { account: a.account }).code, 'ok');
}

function greetedWithCode(env, fields) {
  const client = env.connect();
  const reply = client.hello(fields);
  assert.equal(reply.code, 'ok');
  client.friendCode = reply.friend_code;
  return client;
}

const searchAll = (client) => client.call('room.search', { world: 1 });

test('a room advertises its protocol; members and the search owner carry version and platform', () => {
  const env = makeLobby();
  const host = greeted(env, { ...CURRENT, platform: 'Steam Deck' });
  const created = host.call('room.create', ROOM);
  assert.equal(created.code, 'ok');
  const [owner] = created.room.members;
  assert.deepEqual([created.room.online_protocol, owner.version, owner.platform], [1, '0.8.0', 'Steam Deck']);
  const seeker = greeted(env, { ...CURRENT, platform: 'Linux' }, '198.51.100.4');
  const [brief] = searchAll(seeker).rooms;
  assert.deepEqual([brief.online_protocol, brief.version, brief.platform], [1, '0.8.0', 'Steam Deck']);
  const joined = seeker.call('room.join', { room: brief.id });
  assert.deepEqual(joined.room.members.map((m) => m.platform), ['Steam Deck', 'Linux']);
  const [event] = host.events('room.member_joined');
  assert.deepEqual([event.member.version, event.member.platform], ['0.8.0', 'Linux']);
});

test('same protocol, different port versions: found and joined', () => {
  const env = makeLobby();
  const host = greeted(env, { ...CURRENT, version: '0.8.0' });
  const room = host.call('room.create', ROOM).room;
  const guest = greeted(env, { ...CURRENT, version: '0.8.2', platform: 'Linux' }, '198.51.100.4');
  const found = searchAll(guest);
  assert.deepEqual([found.total, found.incompatible], [1, 0]);
  const joined = guest.call('room.join', { room: room.id });
  assert.equal(joined.code, 'ok');
  assert.equal(joined.me, 0x20);
  assert.equal(host.events('room.member_joined').length, 1);
});

test('another protocol: hidden from search, join refused with the host version, no member event, no endpoints', () => {
  const env = makeLobby();
  const host = greeted(env, { ...CURRENT, online_protocol: 2, version: '0.9.0' });
  const room = host.call('room.create', ROOM).room;
  assert.equal(host.call('udp', { port: 3658 }).code, 'ok');
  const guest = greeted(env, CURRENT, '198.51.100.4');
  assert.equal(guest.call('udp', { port: 3658 }).code, 'ok');
  const found = searchAll(guest);
  assert.deepEqual([found.total, found.rooms.length, found.incompatible], [0, 0, 1]);
  assert.equal(guest.call('room.search', { code: room.code }).rooms.length, 0);
  const refused = guest.call('room.join', { room: room.id });
  assert.equal(refused.code, 'incompatible');
  assert.deepEqual([refused.online_protocol, refused.version, refused.platform], [2, '0.9.0', 'Windows']);
  assert.equal(guest.call('room.join', { code: room.code }).code, 'incompatible');
  env.lobby.runDeferred();
  assert.equal(host.events('room.member_joined').length, 0);
  assert.equal(host.events('peer').length, 0);
  assert.equal(guest.events('peer').length, 0);
  assert.equal(host.call('room.search', { world: 1 }).code, 'ok');
  assert.equal(guest.call('room.create', ROOM).code, 'ok');
});

test('a client without protocol never meets a versioned one; two such clients still meet', () => {
  const env = makeLobby();
  const legacyHost = greeted(env, LEGACY);
  const legacyRoom = legacyHost.call('room.create', ROOM).room;
  assert.equal(legacyRoom.online_protocol, 0);
  const current = greeted(env, CURRENT, '198.51.100.4');
  assert.deepEqual([searchAll(current).total, searchAll(current).incompatible], [0, 1]);
  assert.equal(current.call('room.join', { room: legacyRoom.id }).code, 'incompatible');
  const currentRoom = current.call('room.create', ROOM).room;
  const legacyGuest = greeted(env, LEGACY, '192.0.2.9');
  assert.equal(searchAll(legacyGuest).total, 1);
  assert.equal(legacyGuest.call('room.join', { room: currentRoom.id }).code, 'incompatible');
  assert.equal(legacyGuest.call('room.join', { room: legacyRoom.id }).code, 'ok');
});

test('friends stay listed and online whatever their version; the row carries their protocol', () => {
  const env = makeLobby();
  const a = greetedWithCode(env, { ...CURRENT, name: 'Alice' });
  const b = greetedWithCode(env, { ...CURRENT, online_protocol: 2, version: '0.9.0', platform: 'Linux', name: 'Bob' });
  befriend(a, b);
  const room = b.call('room.create', ROOM).room;
  assert.equal(b.call('presence.set', { data: 'ab'.repeat(0x78) }).code, 'ok');
  const [row] = a.call('friends.list').friends;
  assert.deepEqual([row.name, row.online, row.room, row.online_protocol, row.version, row.platform],
    ['Bob', true, room.id, 2, '0.9.0', 'Linux']);
  assert.equal(a.call('room.join', { room: room.id }).code, 'incompatible');
  const [back] = b.call('friends.list').friends;
  assert.deepEqual([back.name, back.online, back.online_protocol], ['Alice', true, 1]);
});

test('hello ignores a malformed protocol and refuses a non-text version', () => {
  const env = makeLobby();
  const odd = greeted(env, { ...CURRENT, online_protocol: 'one' });
  assert.equal(odd.call('room.create', ROOM).room.online_protocol, 0);
  const c = env.connect();
  assert.equal(c.hello({ ...CURRENT, version: 8 }).code, 'bad_request');
  const long = greeted(env, { ...CURRENT, version: '1'.repeat(40), platform: 'P'.repeat(40) });
  const [member] = long.call('room.create', ROOM).room.members;
  assert.deepEqual([member.version.length, member.platform.length], [16, 16]);
});

test('rooms of a database made before the protocol columns read as protocol 0', () => {
  const store = sqliteStore();
  store.run(`CREATE TABLE rooms (id INTEGER PRIMARY KEY, game TEXT NOT NULL, world INTEGER NOT NULL,
     slots INTEGER NOT NULL, flags INTEGER NOT NULL, owner INTEGER NOT NULL, code TEXT NOT NULL UNIQUE,
     key TEXT NOT NULL, ints TEXT NOT NULL, bin_int TEXT NOT NULL, bin_ext TEXT NOT NULL, created INTEGER NOT NULL)`);
  store.run(`INSERT INTO rooms VALUES (4096, 'CUSA10454-1.00', 1, 4, 0, 16, 'OLDONE', 'aa', '[]', '[]', '[]', 0)`);
  const host = new LocalHost(store, { now: () => 1_700_000_000_000, log: (text) => { throw new Error(text); } });
  const lobby = new Lobby(host);
  assert.equal(lobby.roomRow(4096).online_protocol, 0);
  assert.equal(lobby.meta('schema'), '1');
  const again = new Lobby(host);
  assert.equal(again.meta('schema'), '1');
});
