import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';

import { FLAG_CLOSED, FLAG_FULL, LIMITS, cleanText, ipv4Kind, normalizeFriendCode } from '../src/lobby.js';
import { sha256Hex } from '../src/sha256.js';
import { ROOM, makeLobby } from './harness.js';

function greeted(env, ip, fields) {
  const client = env.connect(ip);
  assert.equal(client.hello(fields).code, 'ok');
  return client;
}

function hostRoom(env, ip = '203.0.113.7') {
  const host = greeted(env, ip);
  const created = host.call('room.create', ROOM);
  assert.equal(created.code, 'ok');
  return { host, room: created.room };
}

test('sha256 matches node:crypto', () => {
  for (const text of ['', 'abc', 'a'.repeat(55), 'a'.repeat(56), 'a'.repeat(64), 'x'.repeat(1000), 'ünïcødé']) {
    assert.equal(sha256Hex(text), createHash('sha256').update(text, 'utf8').digest('hex'));
  }
});

test('hello registers an identity and refuses a wrong secret', () => {
  const env = makeLobby();
  const a = env.connect();
  const first = a.hello({ name: 'Alice' });
  assert.equal(first.code, 'ok');
  assert.match(first.friend_code, /^[A-Z2-9]{4}-[A-Z2-9]{4}$/);
  assert.equal(first.ip, '203.0.113.7');
  assert.equal(first.name, 'Alice');
  const again = env.connect();
  again.account = a.account;
  again.secret = 'cd'.repeat(32);
  assert.equal(again.hello().code, 'unauthorized');
  assert.equal(again.call('worlds').code, 'no_session');
  const same = env.connect();
  same.account = a.account;
  same.secret = a.secret;
  const second = same.hello();
  assert.equal(second.code, 'ok');
  assert.equal(second.friend_code, first.friend_code);
  assert.equal(same.hello().code, 'bad_request');
});

test('hello validates its fields and the protocol', () => {
  const env = makeLobby();
  const c = env.connect();
  assert.equal(c.hello({ proto: 2 }).code, 'proto');
  assert.equal(c.hello({ account: '0' }).code, 'bad_request');
  assert.equal(c.hello({ account: '18446744073709551616' }).code, 'bad_request');
  assert.equal(c.hello({ secret: 'short' }).code, 'bad_request');
  assert.equal(c.hello({ game: 'bad game!' }).code, 'bad_request');
  assert.equal(c.hello({ name: '\u0001\u0002' }).name, 'Player');
  const long = env.connect();
  assert.equal(long.hello({ name: 'A'.repeat(40) }).name, 'A'.repeat(16));
});

test('malformed messages get an error and leave the socket usable', () => {
  const env = makeLobby();
  const c = greeted(env);
  assert.equal(c.raw('not json').code, 'bad_request');
  assert.equal(c.raw('[]').code, 'bad_request');
  assert.equal(c.raw('{"op":"worlds"}').code, 'bad_request');
  assert.equal(c.raw('{"op":"worlds","seq":-1}').code, 'bad_request');
  assert.equal(c.call('constructor').code, 'bad_request');
  assert.equal(c.call('__proto__').code, 'bad_request');
  assert.equal(c.call('hasOwnProperty').code, 'bad_request');
  assert.equal(c.call('nope').code, 'bad_request');
  assert.equal(c.call('worlds', { pad: 'x'.repeat(LIMITS.frameBytes) }).code, 'too_large');
  assert.equal(c.call('room.create', { ...ROOM, slots: 9 }).code, 'bad_request');
  assert.equal(c.call('room.create', { ...ROOM, bin_int: [[0x57, 'zz']] }).code, 'bad_request');
  assert.equal(c.call('room.create', { ...ROOM, bin_int: [[0x57, '0'.repeat(0x202)]] }).code, 'bad_request');
  assert.equal(c.call('room.create', { ...ROOM, ints: [[0x4c, -1]] }).code, 'bad_request');
  assert.equal(c.call('room.join', { room: '12abc' }).code, 'bad_request');
  assert.equal(c.call('worlds').code, 'ok');
});

test('rate limit answers limit and recovers over time', () => {
  const env = makeLobby();
  const c = greeted(env);
  let limited = 0;
  for (let i = 0; i < 80; i++) if (c.call('worlds').code === 'limit') limited++;
  assert.ok(limited >= 15);
  env.advance(5000);
  assert.equal(c.call('worlds').code, 'ok');
});

test('room lifecycle: create, search, join, members, leave, owner migration', () => {
  const env = makeLobby();
  const { host, room } = hostRoom(env);
  assert.equal(room.owner, 0x10);
  assert.equal(room.members.length, 1);
  assert.equal(room.members[0].id, 0x10);
  assert.match(room.code, /^[A-Z2-9]{6}$/);
  assert.match(room.key, /^[0-9a-f]{32}$/);
  assert.equal(host.call('room.create', ROOM).code, 'in_room');

  const guest = greeted(env, '198.51.100.9');
  const found = guest.call('room.search', { world: 1, from: 1, count: 20, filters: [[0x4c, 6, 1], [0x4d, 4, 8], [0x53, 1, 0x81102]] });
  assert.equal(found.total, 1);
  assert.equal(found.rooms[0].id, room.id);
  assert.equal(found.rooms[0].count, 1);
  assert.equal(found.rooms[0].owner_account, host.account);
  assert.equal(guest.call('room.search', { world: 1, filters: [[0x4d, 4, 1]] }).total, 0);

  const joined = guest.call('room.join', { room: room.id });
  assert.equal(joined.code, 'ok');
  assert.equal(joined.me, 0x20);
  assert.deepEqual(joined.room.members.map((m) => m.id), [0x10, 0x20]);
  assert.equal(joined.room.key, room.key);
  const seen = host.events('room.member_joined');
  assert.equal(seen.length, 1);
  assert.equal(seen[0].member.id, 0x20);

  const third = greeted(env, '192.0.2.4');
  assert.equal(third.call('room.join', { code: room.code.toLowerCase() }).me, 0x30);

  assert.equal(host.call('room.leave', { room: room.id }).code, 'ok');
  for (const c of [guest, third]) {
    const owner = c.events('room.owner');
    assert.deepEqual(owner.map((e) => [e.from, e.to]), [[0x10, 0x20]]);
    assert.deepEqual(c.events('room.member_left').map((e) => [e.member, e.why]), [[0x10, 'leave']]);
    assert.equal(c.events('peer.gone').length, 1);
  }
  const fourth = greeted(env, '192.0.2.5');
  assert.equal(fourth.call('room.join', { room: room.id }).me, 0x10);
  assert.equal(guest.call('room.leave', { room: room.id }).code, 'ok');
  assert.equal(third.call('room.leave', { room: room.id }).code, 'ok');
  assert.equal(fourth.call('room.leave', { room: room.id }).code, 'ok');
  assert.equal(fourth.call('room.join', { room: room.id }).code, 'not_found');
});

test('a full or closed room refuses joins and search hides it', () => {
  const env = makeLobby();
  const host = greeted(env);
  const { room } = host.call('room.create', { ...ROOM, slots: 2 });
  const b = greeted(env, '198.51.100.1');
  assert.equal(b.call('room.join', { room: room.id }).code, 'ok');
  const c = greeted(env, '198.51.100.2');
  assert.equal(c.call('room.search', { world: 1 }).total, 0);
  assert.equal(c.call('room.join', { room: room.id }).code, 'full');
  assert.equal(b.call('room.leave', { room: room.id }).code, 'ok');
  assert.equal(host.call('room.set_internal', { room: room.id, flag_mask: FLAG_CLOSED, flag_value: FLAG_CLOSED }).code, 'ok');
  assert.equal(c.call('room.search', { world: 1 }).total, 0);
  assert.equal(c.call('room.join', { room: room.id }).code, 'closed');
  assert.equal(host.call('room.set_internal', { room: room.id, flag_mask: FLAG_CLOSED, flag_value: 0 }).code, 'ok');
  const joined = c.call('room.join', { room: room.id });
  assert.equal(joined.code, 'ok');
  assert.ok(joined.room.flags & FLAG_FULL);
});

test('rooms of another game build are invisible', () => {
  const env = makeLobby();
  const { room } = hostRoom(env);
  const jp = env.connect('198.51.100.3');
  assert.equal(jp.hello({ game: 'CUSA07001-1.03' }).code, 'ok');
  assert.equal(jp.call('room.search', { world: 1 }).total, 0);
  assert.equal(jp.call('room.search', { code: room.code }).total, 0);
  assert.equal(jp.call('room.join', { room: room.id }).code, 'not_found');
});

test('search filter operators and paging', () => {
  const env = makeLobby();
  for (let i = 0; i < 5; i++) {
    const h = greeted(env, `203.0.113.${20 + i}`);
    assert.equal(h.call('room.create', { ...ROOM, ints: [[0x4c, i]] }).code, 'ok');
  }
  const s = greeted(env, '198.51.100.50');
  const count = (filters) => s.call('room.search', { world: 1, filters }).total;
  assert.equal(count([[0x4c, 1, 2]]), 1);
  assert.equal(count([[0x4c, 2, 2]]), 4);
  assert.equal(count([[0x4c, 3, 2]]), 2);
  assert.equal(count([[0x4c, 4, 2]]), 3);
  assert.equal(count([[0x4c, 5, 2]]), 2);
  assert.equal(count([[0x4c, 6, 2]]), 3);
  assert.equal(count([[0x99, 1, 0]]), 5);
  const page = s.call('room.search', { world: 1, from: 3, count: 2 });
  assert.equal(page.total, 5);
  assert.equal(page.rooms.length, 2);
  assert.equal(s.call('room.search', { world: 1, filters: [[0x4c, 7, 2]] }).code, 'bad_request');
});

test('endpoint exchange: public address from the socket, LAN candidates only behind the same address', () => {
  const env = makeLobby();
  const { host, room } = hostRoom(env, '203.0.113.7');
  const udp = host.call('udp', { port: 40001, lan: [{ ip: '192.168.1.10', port: 3658 }, { ip: '8.8.8.8', port: 1 }] });
  assert.deepEqual([udp.ip, udp.port], ['203.0.113.7', 40001]);
  const sameNat = greeted(env, '203.0.113.7');
  sameNat.call('udp', { port: 40002, lan: [{ ip: '192.168.1.11', port: 3659 }] });
  const remote = greeted(env, '198.51.100.20');
  remote.call('udp', { port: 50000, lan: [{ ip: '10.0.0.5', port: 3658 }] });
  assert.equal(sameNat.call('room.join', { room: room.id }).code, 'ok');
  const toSame = sameNat.events('peer');
  assert.equal(toSame.length, 1);
  assert.deepEqual([toSame[0].member, toSame[0].ip, toSame[0].port], [0x10, '203.0.113.7', 40001]);
  assert.deepEqual(toSame[0].lan, [{ ip: '192.168.1.10', port: 3658 }]);
  const toHost = host.events('peer');
  assert.deepEqual(toHost.map((e) => [e.member, e.port, e.lan?.length]), [[0x20, 40002, 1]]);
  assert.equal(remote.call('room.join', { room: room.id }).code, 'ok');
  const toRemote = remote.events('peer');
  assert.equal(toRemote.length, 2);
  assert.ok(toRemote.every((e) => e.lan === undefined));
  assert.ok(host.events('peer').every((e) => e.lan === undefined && e.ip === '198.51.100.20'));
  assert.deepEqual(sameNat.events('peer').map((e) => [e.port, e.lan]), [[50000, undefined]]);
  assert.equal(remote.call('udp', { port: 50001 }).code, 'ok');
  assert.deepEqual(host.events('peer').map((e) => e.port), [50001]);
  assert.deepEqual(sameNat.events('peer').map((e) => e.port), [50001]);
  assert.equal(host.call('udp', { port: 0 }).code, 'bad_request');
  assert.equal(host.call('udp', { port: 1, lan: new Array(9).fill({ ip: '10.0.0.1', port: 1 }) }).code, 'bad_request');
});

test('room data: owner only, internal updates reach the others', () => {
  const env = makeLobby();
  const { host, room } = hostRoom(env);
  const guest = greeted(env, '198.51.100.9');
  guest.call('room.join', { room: room.id });
  assert.equal(guest.call('room.set_internal', { room: room.id, bin_int: [[0x57, 'aa']] }).code, 'not_owner');
  assert.equal(host.call('room.set_internal', { room: room.id, flag_mask: 0x04000000, flag_value: 0, bin_int: [[0x57, 'ab']] }).code, 'ok');
  const updates = guest.events('room.internal');
  assert.equal(updates.length, 1);
  assert.deepEqual(updates[0].bin_int, [[0x57, 'ab']]);
  assert.equal(updates[0].flags & 0x04000000, 0);
  assert.equal(host.events('room.internal').length, 0);
  assert.equal(host.call('room.set_external', { room: room.id, ints: [[0x4d, 9]], bin_ext: [[0x55, 'cc']] }).code, 'ok');
  const s = greeted(env, '198.51.100.30');
  const listed = s.call('room.search', { world: 1, filters: [[0x4d, 1, 9]] });
  assert.equal(listed.total, 1);
  assert.deepEqual(listed.rooms[0].bin_ext, [[0x55, 'cc']]);
  assert.equal(host.call('room.set_external', { room: '99999', ints: [] }).code, 'not_found');
});

test('kick removes the member and tells it', () => {
  const env = makeLobby();
  const { host, room } = hostRoom(env);
  const guest = greeted(env, '198.51.100.9');
  guest.call('room.join', { room: room.id });
  assert.equal(guest.call('room.kick', { room: room.id, member: 0x10 }).code, 'not_owner');
  assert.equal(host.call('room.kick', { room: room.id, member: 0x10 }).code, 'bad_request');
  assert.equal(host.call('room.kick', { room: room.id, member: 0x20 }).code, 'ok');
  assert.equal(guest.events('room.kicked').length, 1);
  assert.deepEqual(host.events('room.member_left').map((e) => e.why), ['kicked']);
  assert.equal(guest.call('room.create', ROOM).code, 'ok');
});

test('a dropped member keeps its seat for the grace period and resumes with the held events', () => {
  const env = makeLobby();
  const { host, room } = hostRoom(env);
  const guest = greeted(env, '198.51.100.9');
  guest.call('room.join', { room: room.id });
  host.events('room.member_joined');
  guest.drop();
  assert.equal(host.events('room.member_left').length, 0);
  const third = greeted(env, '192.0.2.8');
  assert.equal(third.call('room.join', { room: room.id }).me, 0x30);
  host.call('room.set_internal', { room: room.id, bin_int: [[0x57, 'ee']] });
  env.advance(LIMITS.graceMs - 1000);

  const back = env.connect('198.51.100.10');
  back.account = guest.account;
  back.secret = guest.secret;
  assert.equal(back.hello().code, 'ok');
  const resumed = back.call('room.resume', { room: room.id, member: 0x20 });
  assert.equal(resumed.code, 'ok');
  assert.equal(resumed.me, 0x20);
  assert.deepEqual(back.inbox.map((e) => e.ev), ['room.member_joined', 'room.internal']);
  env.advance(5000);
  assert.equal(host.events('room.member_left').length, 0);

  const stranger = greeted(env, '192.0.2.9');
  assert.equal(stranger.call('room.resume', { room: room.id, member: 0x30 }).code, 'not_found');
});

test('a seat whose grace ran out is a disconnect, with owner migration', () => {
  const env = makeLobby();
  const { host, room } = hostRoom(env);
  const guest = greeted(env, '198.51.100.9');
  guest.call('room.join', { room: room.id });
  host.drop();
  env.advance(LIMITS.graceMs + 1);
  assert.deepEqual(guest.events('room.owner').map((e) => [e.from, e.to]), [[0x10, 0x20]]);
  assert.deepEqual(guest.events('room.member_left').map((e) => e.why), ['disconnect']);
  const late = env.connect();
  late.account = host.account;
  late.secret = host.secret;
  late.hello();
  assert.equal(late.call('room.resume', { room: room.id, member: 0x10 }).code, 'not_found');
});

test('a seat whose held events overflow ends early', () => {
  const env = makeLobby();
  const { host, room } = hostRoom(env);
  const guest = greeted(env, '198.51.100.9');
  guest.call('room.join', { room: room.id });
  guest.drop();
  for (let i = 0; i <= LIMITS.heldEvents; i++) {
    env.advance(1000);
    host.call('room.set_internal', { room: room.id, bin_int: [[0x57, i.toString(16).padStart(2, '0')]] });
  }
  assert.deepEqual(host.events('room.member_left').map((e) => e.why), ['disconnect']);
});

test('resume with the old socket still open takes the seat from it', () => {
  const env = makeLobby();
  const { room } = hostRoom(env);
  const guest = greeted(env, '198.51.100.9');
  guest.call('room.join', { room: room.id });
  const fresh = env.connect('198.51.100.9');
  fresh.account = guest.account;
  fresh.secret = guest.secret;
  fresh.hello();
  assert.equal(fresh.call('room.resume', { room: room.id, member: 0x20 }).code, 'ok');
  assert.equal(guest.closedWith?.code, 4001);
});

test('friends: request, accept, presence, room, remove', () => {
  const env = makeLobby();
  const a = env.connect('203.0.113.1');
  const codeA = a.hello({ name: 'Alice' }).friend_code;
  const b = env.connect('198.51.100.1');
  const codeB = b.hello({ name: 'Bob' }).friend_code;
  assert.equal(a.call('friends.add', { code: codeA }).code, 'bad_request');
  assert.equal(a.call('friends.add', { code: 'AAAA-AAAA' }).code, 'not_found');
  assert.equal(a.call('friends.add', { code: 'AB!' }).code, 'bad_request');
  const sent = a.call('friends.add', { code: ` ${codeB.toLowerCase()} ` });
  assert.deepEqual([sent.state, sent.name], ['pending', 'Bob']);
  assert.equal(a.call('friends.add', { code: codeB }).state, 'pending');
  assert.deepEqual(b.events('friend.request').map((e) => e.name), ['Alice']);
  const requests = b.call('friends.requests');
  assert.deepEqual(requests.incoming.map((r) => r.name), ['Alice']);
  assert.deepEqual(a.call('friends.requests').outgoing.map((r) => r.name), ['Bob']);
  const accepted = b.call('friends.accept', { account: a.account });
  assert.equal(accepted.friend.name, 'Alice');
  assert.equal(accepted.friend.online, true);
  assert.equal(a.events('friend.added')[0].friend.name, 'Bob');
  assert.equal(b.call('friends.accept', { account: a.account }).code, 'not_found');

  const { room } = (() => ({ room: a.call('room.create', ROOM).room }))();
  a.call('presence.set', { data: 'ab'.repeat(0x78), status: '00ff' });
  const list = b.call('friends.list').friends;
  assert.equal(list.length, 1);
  assert.deepEqual([list[0].name, list[0].online, list[0].room, list[0].code], ['Alice', true, room.id, room.code]);
  assert.equal(list[0].data, 'ab'.repeat(0x78));
  assert.equal(list[0].status, '00ff');
  assert.equal(a.call('presence.set', { data: 'ab'.repeat(0x81) }).code, 'bad_request');

  a.drop();
  assert.deepEqual(b.events('friend.offline').map((e) => e.account), [a.account]);
  const offline = b.call('friends.list').friends[0];
  assert.deepEqual([offline.online, offline.room, offline.data], [false, undefined, undefined]);

  assert.equal(b.call('friends.remove', { account: a.account }).code, 'ok');
  assert.equal(b.call('friends.list').friends.length, 0);
  assert.equal(b.call('friends.remove', { account: a.account }).code, 'ok');
});

test('friends: a new name keeps the friend code and the friendship; rows carry no address', () => {
  const env = makeLobby();
  const a = greeted(env, '203.0.113.1', { name: 'ClientA', presence: false });
  const b = env.connect('198.51.100.1');
  const codeB = b.hello({ name: 'Bob', presence: false }).friend_code;
  a.call('friends.add', { code: codeB });
  b.call('friends.accept', { account: a.account });
  a.drop();

  const renamed = env.connect('203.0.113.1');
  renamed.account = a.account;
  renamed.secret = a.secret;
  const again = renamed.hello({ name: 'Saulo', presence: false });
  assert.equal(again.account, a.account);
  assert.equal(b.call('friends.add', { code: again.friend_code }).state, 'friends');
  const [row] = b.call('friends.list').friends;
  assert.deepEqual([row.account, row.name], [a.account, 'Saulo']);
  for (const hidden of ['ip', 'port', 'lan', 'secret']) assert.equal(row[hidden], undefined);
  assert.deepEqual(renamed.call('friends.list').friends.map((f) => f.name), ['Bob']);
});

test('presence: any present socket of the account; it ends with the last one, a held seat keeps no ghost', () => {
  const env = makeLobby();
  const a = greeted(env, '203.0.113.1', { name: 'Alice' });
  const launcher = env.connect('198.51.100.1');
  const codeB = launcher.hello({ name: 'Bob' }).friend_code;
  a.call('friends.add', { code: codeB });
  launcher.call('friends.accept', { account: a.account });
  const row = () => a.call('friends.list').friends[0];
  assert.equal(row().online, true);
  a.events('friend.online');

  const game = env.connect('198.51.100.1');
  game.account = launcher.account;
  game.secret = launcher.secret;
  assert.equal(game.hello({ name: 'Bob' }).code, 'ok');
  assert.equal(a.events('friend.online').length, 0);
  launcher.drop();
  assert.equal(a.events('friend.offline').length, 0);
  assert.equal(row().online, true);

  const { room } = game.call('room.create', ROOM);
  assert.deepEqual([row().online, row().room], [true, room.id]);
  game.drop();
  assert.deepEqual(a.events('friend.offline').map((e) => e.account), [launcher.account]);
  assert.deepEqual([row().online, row().room], [false, undefined]);
  env.advance(LIMITS.graceMs + 1);
  assert.deepEqual([row().online, row().room], [false, undefined]);
  assert.equal(a.events('friend.online').length, 0);

  const test = env.connect('198.51.100.1');
  test.account = launcher.account;
  test.secret = launcher.secret;
  test.hello({ name: 'Bob', presence: false });
  assert.equal(row().online, false);
  assert.equal(a.events('friend.online').length, 0);
});

test('the Worker and the development server drive the same lobby code', () => {
  const source = (path) => readFileSync(new URL(path, import.meta.url), 'utf8');
  for (const host of [source('../src/worker.js'), source('../local/server.js')]) {
    assert.match(host, /import \{[^}]*\bLobby\b[^}]*\} from '\.\.\/src\/lobby\.js'|import \{[^}]*\bLobby\b[^}]*\} from '\.\/lobby\.js'/);
    for (const call of ['.admit(', '.message(', '.closed(', '.alarm()']) assert.ok(host.includes(call), call);
  }
});

test('friends: a crossed request makes friends at once; decline withdraws', () => {
  const env = makeLobby();
  const a = env.connect('203.0.113.1');
  const codeA = a.hello().friend_code;
  const b = env.connect('198.51.100.1');
  const codeB = b.hello().friend_code;
  assert.equal(a.call('friends.add', { code: codeB }).state, 'pending');
  assert.equal(b.call('friends.add', { code: codeA }).state, 'friends');
  assert.equal(a.events('friend.added').length, 1);
  assert.equal(a.call('friends.add', { code: codeB }).state, 'friends');
  const c = env.connect('192.0.2.1');
  c.hello();
  assert.equal(c.call('friends.add', { code: codeA }).state, 'pending');
  assert.equal(c.call('friends.decline', { account: a.account }).code, 'ok');
  assert.equal(a.call('friends.requests').incoming.length, 0);
  assert.equal(a.call('friends.decline', { account: c.account }).code, 'not_found');
});

test('friends: pending request limits and expiry', () => {
  const env = makeLobby({ local: true });
  const target = env.connect('127.0.0.1');
  const code = target.hello().friend_code;
  let refused = 0;
  for (let i = 0; i < LIMITS.pendingIn + 3; i++) {
    const c = env.connect('127.0.0.1');
    c.hello();
    if (c.call('friends.add', { code }).code === 'limit') refused++;
  }
  assert.equal(refused, 3);
  env.advance(LIMITS.requestDays * 24 * 3600 * 1000 + 1);
  assert.equal(target.call('friends.requests').incoming.length, 0);
});

test('chat: bounded, cleaned, rate limited, echoed to the room, never held', () => {
  const env = makeLobby();
  const { host, room } = hostRoom(env);
  const guest = greeted(env, '198.51.100.9');
  guest.call('room.join', { room: room.id });
  const loner = greeted(env, '192.0.2.1');
  assert.equal(loner.call('chat', { text: 'hi' }).code, 'not_found');
  assert.equal(host.call('chat', { text: ' \u0007 ' }).code, 'bad_request');
  assert.equal(host.call('chat', { text: 42 }).code, 'bad_request');
  assert.equal(host.call('chat', { text: `hello\nworld${'x'.repeat(200)}` }).code, 'ok');
  const got = guest.events('chat');
  assert.equal(got.length, 1);
  assert.equal(got[0].text.length, LIMITS.chatChars);
  assert.ok(got[0].text.startsWith('helloworld'));
  assert.equal(host.events('chat').length, 1);
  let limited = 0;
  for (let i = 0; i < 8; i++) if (host.call('chat', { text: 'spam' }).code === 'limit') limited++;
  assert.ok(limited >= 3);
  guest.drop();
  env.advance(2000);
  host.call('chat', { text: 'while away' });
  const back = env.connect('198.51.100.9');
  back.account = guest.account;
  back.secret = guest.secret;
  back.hello();
  back.call('room.resume', { room: room.id, member: 0x20 });
  assert.equal(back.events('chat').length, 0);
});

test('registrations and sockets per address are limited; loopback is exempt in local mode', () => {
  const env = makeLobby();
  let refused = 0;
  for (let i = 0; i < LIMITS.registrationsPerIpPerDay + 2; i++) {
    const c = env.connect('198.51.100.77');
    if (!c.admitted) break;
    if (c.hello().code === 'limit') refused++;
    c.drop();
  }
  assert.equal(refused, 2);
  for (let i = 0; i < LIMITS.socketsPerIp; i++) assert.ok(env.connect('192.0.2.200').admitted);
  assert.equal(env.connect('192.0.2.200').admitted, false);
  const local = makeLobby({ local: true });
  for (let i = 0; i < LIMITS.socketsPerIp + 4; i++) assert.ok(local.connect('127.0.0.1').admitted);
});

test('bug reports: stored, trimmed, limited per account', () => {
  const env = makeLobby();
  const c = greeted(env);
  assert.equal(c.call('report.send', { text: '', log: '' }).code, 'bad_request');
  const first = c.call('report.send', { text: 'It crashed', log: 'line\n'.repeat(10), version: '0.9.0' });
  assert.equal(first.code, 'ok');
  assert.equal(first.left, LIMITS.reportsPerAccountPerDay - 1);
  for (let i = 1; i < LIMITS.reportsPerAccountPerDay; i++) assert.equal(c.call('report.send', { text: 'again', log: '' }).code, 'ok');
  env.advance(60000);
  assert.equal(c.call('report.send', { text: 'too many', log: '' }).code, 'limit');
  const big = greeted(env, '198.51.100.40');
  assert.equal(big.call('report.send', { text: 'big', log: 'y'.repeat(LIMITS.reportLogChars + 500) }).code, 'ok');
  const stored = env.host.sql.get('SELECT log FROM reports ORDER BY id DESC LIMIT 1');
  assert.equal(stored.log.length, LIMITS.reportLogChars);
});

test('helpers: address kinds, text cleaning, friend codes', () => {
  assert.equal(ipv4Kind('127.0.0.1'), 'loopback');
  assert.equal(ipv4Kind('192.168.0.1'), 'private');
  assert.equal(ipv4Kind('172.31.255.255'), 'private');
  assert.equal(ipv4Kind('172.32.0.1'), 'public');
  assert.equal(ipv4Kind('100.64.0.13'), 'cgnat');
  assert.equal(ipv4Kind('169.254.1.1'), 'link');
  assert.equal(ipv4Kind('224.0.0.1'), 'reserved');
  assert.equal(ipv4Kind('256.1.1.1'), 'invalid');
  assert.equal(ipv4Kind('example.com'), 'invalid');
  assert.equal(cleanText('  a\u0000b\u0085c  ', 10), 'abc');
  assert.equal(cleanText('😀😀😀', 2), '😀😀');
  assert.equal(normalizeFriendCode('abcd-efgh'), 'ABCDEFGH');
  assert.throws(() => normalizeFriendCode('ABCD-EFG1'));
});
