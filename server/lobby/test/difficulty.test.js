import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';

import { DIFFICULTIES, difficultyCategory } from '../src/difficulty.js';
import { encodeRelayFrame, roomToken } from '../src/relay.js';
import { metricsDay } from '../src/metrics.js';
import { ROOM, makeLobby } from './harness.js';

const DAY_MS = 24 * 3600 * 1000;
const REPO = join(import.meta.dirname, '..', '..', '..');
const CLIENT = { online_protocol: 1, version: '0.9.3', platform: 'Windows', relay: true };

let nextIp = 1;

function player(env, platform = 'Windows') {
  const client = env.connect(`198.51.100.${nextIp++ % 250}`, 'BR');
  assert.equal(client.hello({ ...CLIENT, platform }).code, 'ok');
  return client;
}

// A room hosted with `difficulty` (omitted when undefined), joined by `guests` more players and emptied again.
function play(env, difficulty, { guests = 1, relay = false, platforms = [] } = {}) {
  const host = player(env, platforms[0]);
  const created = host.call('room.create', { ...ROOM, ...(difficulty !== undefined && { difficulty }) });
  assert.equal(created.code, 'ok');
  const seated = [];
  for (let i = 0; i < guests; i++) {
    const guest = player(env, platforms[i + 1]);
    const joined = guest.call('room.join', { room: created.room.id });
    assert.equal(joined.code, 'ok');
    seated.push({ guest, me: joined.me });
  }
  if (relay && seated[0]) host.relay(encodeRelayFrame(roomToken(created.room.key), seated[0].me, 1, new Uint8Array([1])));
  for (const { guest } of [...seated].reverse()) guest.call('room.leave', { room: created.room.id });
  host.call('room.leave', { room: created.room.id });
  return created;
}

const dimension = (env, name = 'difficulty', day = metricsDay(env.clock.now)) => Object.fromEntries(env.host.sql.all(
  'SELECT value, count FROM metrics_daily_dimensions WHERE day = ? AND dimension = ?', day, name).map((r) => [r.value, r.count]));
const today = (env) => env.lobby.metricsSummary().today;

test('every category and raw game value normalises to one category; 1 and 2 are Hard, 3 and 4 Infernal', () => {
  for (const name of DIFFICULTIES) assert.equal(difficultyCategory(name), name);
  const raw = { 0: 'normal', 1: 'hard', 2: 'hard', 3: 'infernal', 4: 'infernal', 99: 'ultimate' };
  for (const [value, category] of Object.entries(raw)) assert.equal(difficultyCategory(Number(value)), category, value);
  for (const value of [5, 98, 100, -1, 255, 'Hard', 'ULTIMATE', 'weird', '', '1']) {
    assert.equal(difficultyCategory(value), 'unknown', String(value));
  }
  assert.equal(difficultyCategory(undefined), '');
  for (const malformed of [null, true, {}, [], 1.5, NaN]) assert.equal(difficultyCategory(malformed), null, String(malformed));
});

test('the wire names the client sends are the categories the Worker counts', () => {
  const header = readFileSync(join(REPO, 'src', 'difficulty.h'), 'utf8');
  const wire = header.slice(header.indexOf('dcp_difficulty_wire'));
  const sent = [...wire.matchAll(/return "([a-z]+)";/g)].map((m) => m[1]);
  assert.deepEqual(sent, [...DIFFICULTIES, 'unknown']);
});

test('a room hosted on each difficulty counts once under its category when it ends with a guest', () => {
  const env = makeLobby({ log: () => {} });
  const expected = { normal: 0, hard: 0, infernal: 0, ultimate: 0 };
  const cases = [['normal', 'normal'], ['hard', 'hard'], ['infernal', 'infernal'], ['ultimate', 'ultimate'], [0, 'normal'], [1, 'hard'],
    [2, 'hard'], [3, 'infernal'], [4, 'infernal'], [99, 'ultimate']];
  for (const [sent, category] of cases) {
    play(env, sent);
    expected[category] += 1;
  }
  assert.deepEqual(dimension(env), Object.fromEntries(Object.entries(expected).filter(([, n]) => n)));
  assert.deepEqual(dimension(env), { normal: 2, hard: 3, infernal: 3, ultimate: 2 });
  assert.equal(today(env).rooms_with_guest, cases.length);
});

test('an unknown value or a legacy client without the field is accepted and is not counted under any difficulty', () => {
  const env = makeLobby({ log: () => {} });
  play(env, undefined);
  play(env, 'weird');
  play(env, 5);
  play(env, 98);
  assert.deepEqual(dimension(env), {});
  const t = today(env);
  assert.deepEqual([t.rooms_created, t.rooms_closed, t.rooms_with_guest], [4, 4, 4]);
});

test('the room remembers what its host reported, and unknown when it was not a known value', () => {
  const env = makeLobby({ log: () => {} });
  const host = player(env);
  const made = host.call('room.create', { ...ROOM, difficulty: 'ultimate' });
  assert.equal(env.lobby.roomRow(made.room.id).difficulty, 'ultimate');
  host.call('room.leave', { room: made.room.id });
  const other = player(env);
  assert.equal(env.lobby.roomRow(other.call('room.create', { ...ROOM, difficulty: 7 }).room.id).difficulty, 'unknown');
  const legacy = player(env);
  assert.equal(env.lobby.roomRow(legacy.call('room.create', ROOM).room.id).difficulty, '');
});

test('a malformed difficulty is refused without creating a room; an absent one is not', () => {
  const env = makeLobby({ log: () => {} });
  const host = player(env);
  for (const malformed of [null, true, {}, [], 1.5]) {
    const reply = host.call('room.create', { ...ROOM, difficulty: malformed });
    assert.equal(reply.code, 'bad_request', JSON.stringify(malformed));
  }
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM rooms').n, 0);
  assert.equal(today(env).rooms_created, 0);
  assert.equal(host.call('room.create', ROOM).code, 'ok');
  assert.equal(today(env).rooms_created, 1);
});

test('joins, rejoins and extra guests never add a difficulty sample; the room counts once when it ends', () => {
  const env = makeLobby({ log: () => {} });
  const host = player(env);
  const created = host.call('room.create', { ...ROOM, difficulty: 'hard' });
  const guests = [player(env), player(env), player(env)];
  for (const guest of guests) assert.equal(guest.call('room.join', { room: created.room.id }).code, 'ok');
  guests[0].call('room.leave', { room: created.room.id });
  assert.equal(guests[0].call('room.join', { room: created.room.id }).code, 'ok');
  assert.deepEqual(dimension(env), {});
  assert.equal(today(env).room_joins, 4);
  for (const guest of guests) guest.call('room.leave', { room: created.room.id });
  assert.deepEqual(dimension(env), {});
  host.call('room.leave', { room: created.room.id });
  assert.deepEqual(dimension(env), { hard: 1 });
  assert.equal(today(env).room_joins, 4);
});

test('a room whose host leaves first keeps the difficulty it was created with', () => {
  const env = makeLobby({ log: () => {} });
  const host = player(env);
  const created = host.call('room.create', { ...ROOM, difficulty: 'infernal' });
  const guest = player(env);
  assert.equal(guest.call('room.join', { room: created.room.id }).code, 'ok');
  host.call('room.leave', { room: created.room.id });
  assert.equal(env.lobby.roomRow(created.room.id).difficulty, 'infernal');
  assert.deepEqual(dimension(env), {});
  guest.call('room.leave', { room: created.room.id });
  assert.deepEqual(dimension(env), { infernal: 1 });
});

test('a host-only room is not a multiplayer room and adds no difficulty', () => {
  const env = makeLobby({ log: () => {} });
  play(env, 'ultimate', { guests: 0 });
  const t = today(env);
  assert.deepEqual([t.rooms_closed, t.rooms_with_guest], [1, 0]);
  assert.deepEqual(dimension(env), {});
});

test('relayed rooms, direct rooms and rooms with several guests each count once', () => {
  const env = makeLobby({ log: () => {} });
  play(env, 'normal', { relay: true });
  play(env, 'normal');
  play(env, 'hard', { guests: 3 });
  assert.deepEqual(dimension(env), { normal: 2, hard: 1 });
  const t = today(env);
  assert.deepEqual([t.rooms_with_guest, t.rooms_relayed], [3, 1]);
});

test('adding difficulty leaves every platform and room metric as it was', () => {
  const plain = makeLobby({ log: () => {} });
  const rated = makeLobby({ log: () => {} });
  for (const env of [plain, rated]) {
    const flag = env === rated ? 'infernal' : undefined;
    play(env, flag, { platforms: ['Windows', 'Android'], relay: true });
    play(env, flag, { platforms: ['Android', 'Android'] });
    play(env, flag, { guests: 0 });
  }
  assert.deepEqual(today(rated), today(plain));
  for (const name of ['platform', 'platform_mix', 'occupancy', 'lifetime']) {
    assert.deepEqual(dimension(rated, name), dimension(plain, name), name);
  }
  assert.deepEqual(dimension(rated), { infernal: 2 });
  assert.deepEqual(dimension(plain), {});
});

test('history groups the last days by difficulty, names the first day and leaves older rooms out', () => {
  const env = makeLobby({ log: () => {} });
  assert.equal(env.lobby.metricsHistory(30).difficulty_first_day, null);
  assert.deepEqual(env.lobby.metricsHistory(30).dimensions.difficulty, []);
  play(env, 'hard');
  play(env, 'ultimate');
  const old = metricsDay(env.clock.now - 40 * DAY_MS);
  env.host.sql.run(`INSERT INTO metrics_daily_dimensions VALUES (?, 'difficulty', 'normal', 9)`, old);
  const month = env.lobby.metricsHistory(30);
  assert.deepEqual(month.dimensions.difficulty.map((d) => [d.value, d.count]), [['hard', 1], ['ultimate', 1]]);
  assert.equal(month.difficulty_first_day, old);
  const quarter = env.lobby.metricsHistory(90);
  assert.deepEqual(Object.fromEntries(quarter.dimensions.difficulty.map((d) => [d.value, d.count])), { normal: 9, hard: 1, ultimate: 1 });
});
