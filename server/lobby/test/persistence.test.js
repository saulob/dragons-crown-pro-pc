import assert from 'node:assert/strict';
import { mkdtempSync, rmSync, writeFileSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';

import { LIMITS, Lobby } from '../src/lobby.js';
import { LocalHost, sqliteStore } from '../local/host.js';
import { readIdentity, startLobbyServer } from '../local/server.js';
import { Client, ROOM } from './harness.js';

// One lobby process on a database file; `restart` drops it the way a stopped server does and opens the file again.
class Run {
  constructor(file, { clearRooms = true, clock = { now: 1_700_000_000_000 } } = {}) {
    this.file = file;
    this.clock = clock;
    this.host = new LocalHost(sqliteStore(file), { now: () => clock.now, log: (text) => { throw new Error(text); } });
    this.lobby = new Lobby(this.host, { local: true });
    if (clearRooms) this.lobby.clearRooms();
  }

  connect(identity) {
    const client = new Client(this.lobby, this.host, '127.0.0.1');
    if (identity) Object.assign(client, identity);
    return client;
  }

  advance(ms) {
    this.clock.now += ms;
    if (this.host.alarmAt !== null && this.host.alarmAt <= this.clock.now) {
      this.host.alarmAt = null;
      this.lobby.alarm();
    }
  }

  stop() {
    this.host.sql.close();
  }
}

const identityOf = (client) => ({ account: client.account, secret: client.secret });

function tempDb() {
  const dir = mkdtempSync(join(tmpdir(), 'dcp-lobby-'));
  return { dir, file: join(dir, 'lobby.sqlite') };
}

test('accounts, friend codes and friendships survive a lobby restart; rooms and presence do not', () => {
  const { dir, file } = tempDb();
  const first = new Run(file);
  const a = first.connect();
  const codeA = a.hello({ name: 'ClientA' }).friend_code;
  const b = first.connect();
  const codeB = b.hello({ name: 'ClientB' }).friend_code;
  assert.notEqual(a.account, b.account);
  assert.equal(a.call('friends.add', { code: codeB }).state, 'pending');
  assert.equal(b.call('friends.accept', { account: a.account }).friend.name, 'ClientA');
  assert.equal(a.call('room.create', ROOM).code, 'ok');
  assert.equal(a.call('presence.set', { data: 'ab'.repeat(0x78) }).code, 'ok');
  const idA = identityOf(a), idB = identityOf(b);
  first.stop();

  const second = new Run(file);
  assert.deepEqual(second.lobby.stats(), { accounts: 2, friendships: 1, requests: 0 });
  assert.equal(second.host.sql.get('SELECT COUNT(*) AS n FROM rooms').n, 0);
  assert.equal(second.host.sql.get('SELECT COUNT(*) AS n FROM members').n, 0);
  const a2 = second.connect(idA);
  assert.equal(a2.hello({ name: 'ClientA' }).friend_code, codeA);
  const before = a2.call('friends.list').friends;
  assert.deepEqual(before.map((f) => [f.account, f.online, f.room, f.data]), [[idB.account, false, undefined, undefined]]);
  const b2 = second.connect(idB);
  assert.equal(b2.hello({ name: 'Renamed' }).friend_code, codeB);
  assert.equal(a2.events('friend.online').length, 1);
  const after = a2.call('friends.list').friends[0];
  assert.deepEqual([after.account, after.online, after.name], [idB.account, true, 'Renamed']);
  assert.equal(b2.call('room.search', { world: 1 }).total, 0);
  assert.equal(b2.call('friends.remove', { account: idA.account }).code, 'ok');
  second.stop();

  const third = new Run(file);
  assert.deepEqual(third.lobby.stats(), { accounts: 2, friendships: 0, requests: 0 });
  const a3 = third.connect(idA);
  assert.equal(a3.hello().friend_code, codeA);
  assert.equal(a3.call('friends.list').friends.length, 0);
  const forged = third.connect({ account: idA.account, secret: 'cd'.repeat(32) });
  assert.equal(forged.hello().code, 'unauthorized');
  third.stop();

  for (const suffix of ['', '-wal', '-shm']) rmSync(file + suffix, { force: true });
  const fresh = new Run(file);
  assert.deepEqual(fresh.lobby.stats(), { accounts: 0, friendships: 0, requests: 0 });
  const a4 = fresh.connect(idA);
  assert.notEqual(a4.hello().friend_code, codeA);
  fresh.stop();
  rmSync(dir, { recursive: true, force: true });
});

test('pending friend requests survive a restart', () => {
  const { dir, file } = tempDb();
  const first = new Run(file);
  const a = first.connect();
  a.hello();
  const b = first.connect();
  const codeB = b.hello().friend_code;
  a.call('friends.add', { code: codeB });
  const idA = identityOf(a), idB = identityOf(b);
  first.stop();
  const second = new Run(file);
  const b2 = second.connect(idB);
  b2.hello();
  assert.deepEqual(b2.call('friends.requests').incoming.map((r) => r.account), [idA.account]);
  second.stop();
  rmSync(dir, { recursive: true, force: true });
});

test('after a Durable Object restart, seats without a socket get the grace period, then go', () => {
  const { dir, file } = tempDb();
  const clock = { now: 1_700_000_000_000 };
  const first = new Run(file, { clock });
  const host = first.connect();
  host.hello();
  const { room } = host.call('room.create', ROOM);
  const guest = first.connect();
  guest.hello();
  guest.call('room.join', { room: room.id });
  const idHost = identityOf(host), idGuest = identityOf(guest);
  first.stop();

  const second = new Run(file, { clearRooms: false, clock });
  assert.equal(second.host.sql.get('SELECT COUNT(*) AS n FROM members WHERE conn IS NULL AND grace_until IS NOT NULL').n, 2);
  const back = second.connect(idGuest);
  back.hello();
  assert.equal(back.call('room.resume', { room: room.id, member: 0x20 }).code, 'ok');
  second.advance(LIMITS.graceMs + 1);
  assert.deepEqual(back.events('room.owner').map((e) => e.to), [0x20]);
  assert.deepEqual(back.events('room.member_left').map((e) => [e.member, e.why]), [[0x10, 'disconnect']]);
  back.call('room.leave', { room: room.id });
  assert.equal(second.host.sql.get('SELECT COUNT(*) AS n FROM rooms').n, 0);
  const late = second.connect(idHost);
  late.hello();
  assert.equal(late.call('room.resume', { room: room.id, member: 0x10 }).code, 'not_found');
  second.stop();
  rmSync(dir, { recursive: true, force: true });
});

test('the development server keeps its state file, seeds test friends on request and logs no secrets', async () => {
  const { dir, file } = tempDb();
  const secretA = '1a'.repeat(32), secretB = '2b'.repeat(32);
  writeFileSync(join(dir, 'a.ini'), `; identity\r\naccount=1111\r\nsecret=${secretA}\r\n`);
  writeFileSync(join(dir, 'b.ini'), `account=2222\nsecret=${secretB}\n`);
  assert.throws(() => readIdentity(join(dir, 'missing.ini'), 'X'));
  writeFileSync(join(dir, 'bad.ini'), 'account=12\nsecret=short\n');
  assert.throws(() => readIdentity(join(dir, 'bad.ini'), 'X'), /not an online identity/);

  const lines = [];
  const seeded = [readIdentity(join(dir, 'a.ini'), 'ClientA'), readIdentity(join(dir, 'b.ini'), 'ClientB')];
  let server = await startLobbyServer({ port: 0, db: file, seedFriends: seeded, log: (t) => lines.push(t) });
  assert.ok(lines.includes(`Lobby: persistent test state ${file}`));
  assert.ok(lines.some((l) => l.startsWith('Lobby: loaded 2 accounts, 1 friendships')));
  assert.ok(lines.every((l) => !l.includes(secretA) && !l.includes(secretB)));
  const codeA = server.lobby.accountRow('1111').code;
  await server.close();

  lines.length = 0;
  server = await startLobbyServer({ port: 0, db: file, seedFriends: seeded, log: (t) => lines.push(t) });
  assert.ok(lines.some((l) => l.startsWith('Lobby: loaded 2 accounts, 1 friendships')));
  assert.equal(server.lobby.accountRow('1111').code, codeA);
  assert.throws(() => server.lobby.seedFriends({ account: '1111', secret: '00'.repeat(32), name: 'X' }, seeded[1]),
    /another secret/);
  await server.close();
  assert.ok(existsSync(file));
  rmSync(dir, { recursive: true, force: true });
});
