import assert from 'node:assert/strict';
import { test } from 'node:test';

import { handleAdmin } from '../src/admin.js';
import { STORY_SESSION_ATTR } from '../src/lobby.js';
import { ROOM, makeLobby } from './harness.js';

const MODERN = { online_protocol: 1, version: '1.2.0', platform: 'Windows', story_session: true };
const OLD = { online_protocol: 1, version: '1.1.0', platform: 'Windows' };
const TOKEN = 'test-dashboard-token-0123456789abcdef';

const storyRoom = (step = 1) => ({ ...ROOM, ints: [...ROOM.ints, [STORY_SESSION_ATTR, step]] });

function player(env, name, fields = MODERN, ip = `198.51.100.${(env.players = (env.players ?? 0) + 1)}`) {
  const client = env.connect(ip);
  assert.equal(client.hello({ ...fields, name }).code, 'ok');
  return client;
}

const story = (env) => env.lobby.metricsSummary().story;
const liveOf = (env) => story(env).live;
const userOf = (env, name) => story(env).users.find((u) => u.name === name);

function host(env, name = 'Hosty') {
  const client = player(env, name);
  const reply = client.call('room.create', storyRoom());
  assert.equal(reply.code, 'ok');
  return { client, room: reply.room };
}

function guestOf(env, room, name = 'Guesty') {
  const client = player(env, name);
  assert.equal(client.call('room.join', { room: room.id }).code, 'ok');
  return client;
}

test('a normal room, search, friends and lookups leave every Early Story figure at zero', () => {
  const env = makeLobby();
  const a = player(env, 'Plain');
  const room = a.call('room.create', ROOM).room;
  const b = player(env, 'Plain2');
  assert.equal(b.call('room.search', { world: 1 }).total, 1);
  assert.equal(b.call('room.search', { code: room.code }).total, 1);
  assert.equal(b.call('room.join', { room: room.id }).code, 'ok');
  const s = story(env);
  assert.deepEqual(s.live, { rooms: 0, parties: 0, waiting: 0, players: 0 });
  assert.deepEqual(s.totals, { rooms_created: 0, guest_joins: 0, sessions: 0, users: 0 });
  assert.deepEqual(s.users, []);
  assert.equal(env.lobby.metricsSummary().today.rooms_created, 1);
  assert.equal(env.lobby.metricsSummary().today.room_joins, 1);
});

test('a story host creates a waiting room and is recorded once', () => {
  const env = makeLobby();
  const { client } = host(env);
  assert.deepEqual(liveOf(env), { rooms: 1, parties: 0, waiting: 1, players: 1 });
  assert.deepEqual(story(env).totals, { rooms_created: 1, guest_joins: 0, sessions: 1, users: 1 });
  const user = userOf(env, 'Hosty');
  assert.deepEqual([user.account, user.role, user.sessions], [client.account, 'Host', 1]);
  assert.deepEqual([user.active.role, user.active.members], ['Host', 1]);
  assert.equal(env.lobby.metricsSummary().today.rooms_created, 1);
});

test('a guest makes the room a party and is recorded once; updates, presence and searches add nothing', () => {
  const env = makeLobby();
  const { client: hostClient, room } = host(env);
  const guest = guestOf(env, room);
  assert.deepEqual(liveOf(env), { rooms: 1, parties: 1, waiting: 0, players: 2 });
  assert.deepEqual(story(env).totals, { rooms_created: 1, guest_joins: 1, sessions: 2, users: 2 });
  assert.deepEqual([userOf(env, 'Guesty').role, userOf(env, 'Guesty').sessions], ['Guest', 1]);
  assert.deepEqual(userOf(env, 'Guesty').active.role, 'Guest');

  const before = JSON.stringify(story(env).totals);
  for (let i = 0; i < 5; i++) {
    guest.call('presence.set', { data: 'ab'.repeat(8) });
    guest.call('udp', { port: 40000 + i });
    guest.call('room.search', { code: room.code });
    guest.call('friends.list');
    hostClient.call('room.set_internal', { room: room.id, bin_int: [[0x7f02, `01${i.toString(16).padStart(2, '0')}`]] });
    hostClient.call('room.set_external', { room: room.id, ints: [[0x4d, i]] });
  }
  assert.equal(JSON.stringify(story(env).totals), before);
  assert.equal(userOf(env, 'Guesty').sessions, 1);
  assert.equal(userOf(env, 'Hosty').sessions, 1);
});

test('failed joins of a story room are not counted', () => {
  const env = makeLobby();
  const { room } = host(env);
  const old = player(env, 'Oldie', OLD);
  assert.equal(old.call('room.join', { room: room.id }).code, 'incompatible');
  const stranger = player(env, 'Nobody');
  assert.equal(stranger.call('room.join', { room: '999999' }).code, 'not_found');
  assert.deepEqual(story(env).totals, { rooms_created: 1, guest_joins: 0, sessions: 1, users: 1 });
  assert.equal(userOf(env, 'Oldie'), undefined);
  assert.deepEqual(liveOf(env), { rooms: 1, parties: 0, waiting: 1, players: 1 });
});

test('a guest leaving, reconnecting and rejoining the same room counts as one participation', () => {
  const env = makeLobby();
  const { room } = host(env);
  const guest = guestOf(env, room);
  assert.deepEqual(liveOf(env), { rooms: 1, parties: 1, waiting: 0, players: 2 });

  guest.drop();
  assert.deepEqual(liveOf(env), { rooms: 1, parties: 0, waiting: 1, players: 1 });
  const back = env.connect('198.51.100.77');
  Object.assign(back, { account: guest.account, secret: guest.secret });
  assert.equal(back.hello({ ...MODERN, name: 'Guesty' }).code, 'ok');
  assert.equal(back.call('room.resume', { room: room.id, member: 0x20 }).code, 'ok');
  assert.deepEqual(liveOf(env), { rooms: 1, parties: 1, waiting: 0, players: 2 });
  assert.deepEqual(story(env).totals, { rooms_created: 1, guest_joins: 1, sessions: 2, users: 2 });

  assert.equal(back.call('room.leave', { room: room.id }).code, 'ok');
  assert.deepEqual(liveOf(env), { rooms: 1, parties: 0, waiting: 1, players: 1 });
  assert.equal(back.call('room.join', { room: room.id }).code, 'ok');
  assert.deepEqual(story(env).totals, { rooms_created: 1, guest_joins: 2, sessions: 2, users: 2 });
  assert.equal(userOf(env, 'Guesty').sessions, 1);
});

test('a held seat is not a player; its grace ending closes the seat', () => {
  const env = makeLobby();
  const { room } = host(env);
  const guest = guestOf(env, room);
  guest.drop();
  assert.equal(liveOf(env).players, 1);
  assert.equal(userOf(env, 'Guesty').active, null);
  env.advance(10 * 60 * 1000);
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM members').n, 1);
  assert.deepEqual(liveOf(env), { rooms: 1, parties: 0, waiting: 1, players: 1 });
  assert.equal(userOf(env, 'Guesty').sessions, 1);
});

test('two guests are one party of three; the room closing returns every live figure to zero and keeps the history', () => {
  const env = makeLobby();
  const { client: hostClient, room } = host(env);
  const first = guestOf(env, room, 'Guest1');
  const second = guestOf(env, room, 'Guest2');
  assert.deepEqual(liveOf(env), { rooms: 1, parties: 1, waiting: 0, players: 3 });
  assert.equal(story(env).users.filter((u) => u.active).every((u) => u.active.members === 3), true);
  for (const c of [first, second, hostClient]) assert.equal(c.call('room.leave', { room: room.id }).code, 'ok');
  assert.deepEqual(liveOf(env), { rooms: 0, parties: 0, waiting: 0, players: 0 });
  assert.deepEqual(story(env).totals, { rooms_created: 1, guest_joins: 2, sessions: 3, users: 3 });
  assert.ok(story(env).users.every((u) => u.active === null));
});

test('the figures count rooms, parties, waiting rooms and players across several rooms', () => {
  const env = makeLobby();
  const a = host(env, 'HostA');
  guestOf(env, a.room, 'GuestA');
  const b = host(env, 'HostB');
  guestOf(env, b.room, 'GuestB');
  host(env, 'HostC');
  assert.deepEqual(liveOf(env), { rooms: 3, parties: 2, waiting: 1, players: 5 });
  const normal = player(env, 'Plain');
  normal.call('room.create', ROOM);
  assert.deepEqual(liveOf(env), { rooms: 3, parties: 2, waiting: 1, players: 5 });
});

test('hosting again counts again; hosting and guesting is both, and unique users stay distinct by account', () => {
  const env = makeLobby();
  const { client, room } = host(env, 'Frequent');
  assert.equal(client.call('room.leave', { room: room.id }).code, 'ok');
  const again = client.call('room.create', storyRoom());
  assert.equal(again.code, 'ok');
  assert.deepEqual([userOf(env, 'Frequent').role, userOf(env, 'Frequent').sessions], ['Host', 2]);
  assert.equal(client.call('room.leave', { room: again.room.id }).code, 'ok');

  const other = host(env, 'Other');
  assert.equal(client.call('room.join', { room: other.room.id }).code, 'ok');
  const user = userOf(env, 'Frequent');
  assert.deepEqual([user.role, user.sessions], ['Both', 3]);
  assert.deepEqual(story(env).totals, { rooms_created: 3, guest_joins: 1, sessions: 4, users: 2 });
});

test('the latest online name is shown for an account', () => {
  const env = makeLobby();
  const { client, room } = host(env, 'FirstName');
  client.call('room.leave', { room: room.id });
  const again = env.connect('198.51.100.90');
  Object.assign(again, { account: client.account, secret: client.secret });
  assert.equal(again.hello({ ...MODERN, name: 'SecondName' }).code, 'ok');
  assert.equal(again.call('room.create', storyRoom()).code, 'ok');
  assert.deepEqual(story(env).users.map((u) => [u.name, u.sessions]), [['SecondName', 2]]);
});

test('a room whose story attribute was cleared by a new owner stops counting as a story room', () => {
  const env = makeLobby();
  const { client: hostClient, room } = host(env);
  const guest = guestOf(env, room);
  assert.equal(hostClient.call('room.leave', { room: room.id }).code, 'ok');
  assert.deepEqual(liveOf(env), { rooms: 1, parties: 0, waiting: 1, players: 1 });
  assert.equal(guest.call('room.set_external', { room: room.id, ints: [[STORY_SESSION_ATTR, 0]] }).code, 'ok');
  assert.deepEqual(liveOf(env), { rooms: 0, parties: 0, waiting: 0, players: 0 });
  assert.deepEqual(story(env).totals, { rooms_created: 1, guest_joins: 1, sessions: 2, users: 2 });
});

test('room age and the dashboard rows come from the live room; users are listed most recent first', () => {
  const env = makeLobby();
  const { room } = host(env, 'Early');
  env.advance(90 * 1000);
  guestOf(env, room, 'Later');
  const users = story(env).users;
  assert.deepEqual(users.map((u) => u.name), ['Later', 'Early']);
  assert.ok(users.every((u) => u.active.age_ms === 90 * 1000));
  assert.ok(users[0].last_used > users[1].last_used);
});

test('metrics start counting when this was first started; the start never moves', () => {
  const env = makeLobby();
  const first = story(env).since;
  assert.equal(first, env.clock.now);
  env.advance(5000);
  host(env);
  assert.equal(story(env).since, first);
});

test('the summary keeps its existing fields beside the story section, and it holds no address, code or endpoint', () => {
  const env = makeLobby();
  const { client, room } = host(env);
  client.call('udp', { port: 41000 });
  guestOf(env, room).call('udp', { port: 41001 });
  const summary = env.lobby.metricsSummary();
  assert.deepEqual(Object.keys(summary).sort(), ['day', 'generated', 'live', 'story', 'time_zone', 'today']);
  const dump = JSON.stringify(summary.story);
  for (const secret of [room.code, room.key, '198.51.100', '41000', '41001', client.secret]) assert.ok(!dump.includes(secret), secret);
  assert.deepEqual(Object.keys(summary.story.users[0]).sort(), ['account', 'active', 'last_used', 'name', 'role', 'sessions']);
  assert.deepEqual(Object.keys(summary.story.users[0].active).sort(), ['age_ms', 'members', 'role']);
});

test('the admin endpoint returns the story section to the token only', async () => {
  const env = makeLobby();
  host(env);
  const metrics = { summary: async () => env.lobby.metricsSummary(), history: async (days) => env.lobby.metricsHistory(days) };
  const request = (authorization) => new Request('https://lobby.example/admin/api/summary',
    { headers: authorization ? { Authorization: authorization } : {} });
  const denied = await handleAdmin(request(), { DCP_DASHBOARD_TOKEN: TOKEN }, metrics);
  assert.equal(denied.status, 401);
  assert.ok(!(await denied.text()).includes('Hosty'));
  const ok = await handleAdmin(request(`Bearer ${TOKEN}`), { DCP_DASHBOARD_TOKEN: TOKEN }, metrics);
  assert.equal(ok.status, 200);
  const body = await ok.json();
  assert.deepEqual(body.story.live, { rooms: 1, parties: 0, waiting: 1, players: 1 });
  assert.equal(body.story.users[0].name, 'Hosty');
});

test('a database from before this metric gains its columns and table and keeps its history', () => {
  const env = makeLobby();
  host(env);
  env.host.sql.run('ALTER TABLE rooms DROP COLUMN story_seen');
  env.host.sql.run('DROP TABLE story_users');
  env.host.sql.run('ALTER TABLE metrics_daily DROP COLUMN story_rooms');
  env.host.sql.run('ALTER TABLE metrics_daily DROP COLUMN story_guest_joins');
  env.host.sql.run(`UPDATE meta SET v = '5' WHERE k = 'schema'`);
  env.host.sql.run(`DELETE FROM meta WHERE k = 'story_since'`);
  env.host.sql.run('DELETE FROM members');
  env.host.sql.run('DELETE FROM rooms');
  const again = new env.lobby.constructor(env.host);
  const s = again.storySummary();
  assert.deepEqual(s.totals, { rooms_created: 0, guest_joins: 0, sessions: 0, users: 0 });
  assert.equal(s.since, env.clock.now);
  assert.equal(again.metricsSummary().today.rooms_created, 1);
});
