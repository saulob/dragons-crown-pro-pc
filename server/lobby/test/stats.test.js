import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test, test as nodeTest } from 'node:test';

import { handleAdmin, internalMetricsRoute } from '../src/admin.js';
import { PROTECTION } from '../src/abuse.js';
import { metricsDay } from '../src/metrics.js';
import { LIFETIME_STALE_MS, LIVE_STALE_MS, MONTH_STALE_MS, STATS_PATH, STATS_RETRY_MS, STATS_TTL_MS, publicSections, statsHandler } from '../src/stats.js';
import { ROOM, makeLobby } from './harness.js';
import { DurableObjectState, available } from './support/hibernation.js';

const GAME = { online_protocol: 1, version: '1.4.1', platform: 'Windows' };
const DAY_MS = 24 * 3600 * 1000;
const KEYS = {
  top: ['generated', 'last_30_days', 'lifetime', 'live', 'today'],
  live: ['in_rooms', 'lobby', 'online', 'open_rooms', 'peak_today'],
  today: ['joins', 'rooms_created', 'sessions'],
  last_30_days: ['average_party_size', 'classes', 'parties_reported', 'rooms_created', 'rooms_with_guest', 'successful_joins'],
  lifetime: ['players', 'sessions', 'since'],
};

let nextIp = 1;

function online(env, fields = {}, country = 'BR') {
  const client = env.connect(`203.0.${Math.floor(nextIp / 250)}.${nextIp++ % 250 + 1}`, country);
  assert.equal(client.hello({ name: `Secret${nextIp}`, ...GAME, ...fields }).code, 'ok');
  return client;
}

const showcase = (env) => {
  env.lobby.monthMemo = null;
  return env.lobby.showcase();
};
const published = (env) => publicSections(showcase(env));
const get = (path = STATS_PATH, init = {}) => new Request(`https://dcp-lobby.example${path}`, init);

function assertOnlyApprovedFields(body) {
  assert.deepEqual(Object.keys(body).sort(), KEYS.top);
  for (const name of ['live', 'today', 'last_30_days', 'lifetime']) {
    if (body[name] !== null) assert.deepEqual(Object.keys(body[name]).sort(), KEYS[name]);
  }
}

function playRoom(env, { guests = 1, stay = 6 * 60_000 } = {}) {
  const host = online(env);
  const { room } = host.call('room.create', { ...ROOM, slots: 4 });
  const visitors = Array.from({ length: guests }, () => {
    const guest = online(env);
    assert.equal(guest.call('room.join', { room: room.id }).code, 'ok');
    return guest;
  });
  env.advance(stay);
  for (const guest of visitors) assert.equal(guest.call('room.leave', { room: room.id }).code, 'ok');
  assert.equal(host.call('room.leave', { room: room.id }).code, 'ok');
}

function insertDay(env, msAgo, { guests = 0, classes = {} } = {}) {
  const day = metricsDay(env.clock.now - msAgo);
  env.host.sql.run('INSERT OR IGNORE INTO metrics_daily (day) VALUES (?)', day);
  env.host.sql.run('UPDATE metrics_daily SET rooms_with_guest = rooms_with_guest + ? WHERE day = ?', guests, day);
  for (const [name, count] of Object.entries(classes)) {
    env.host.sql.run(`INSERT INTO metrics_daily_dimensions (day, dimension, value, count) VALUES (?, 'character', ?, ?)
      ON CONFLICT(day, dimension, value) DO UPDATE SET count = count + excluded.count`, day, name, count);
  }
}

const LAST_WORKING = { generated: 1795404171071, live: { online: 19, lobby: 8, open_rooms: 4 }, today: { rooms_created: 28 },
  last_30_days: { first: '2026-10-25', last: '2026-11-23', rooms_created: 10860, successful_joins: 4060, parties_reported: 2364,
    average_party_size: 2.24 } };

test('an empty service gives zero counts and no classes, never NaN', () => {
  const body = published(makeLobby());
  assert.deepEqual(body.live, { online: 0, lobby: 0, in_rooms: 0, open_rooms: 0, peak_today: 0 });
  assert.deepEqual(body.today, { sessions: 0, rooms_created: 0, joins: 0 });
  assert.deepEqual(body.last_30_days, { rooms_created: 0, successful_joins: 0, parties_reported: 0, average_party_size: null,
    rooms_with_guest: 0, classes: [] });
  assert.ok(!JSON.stringify(body).includes('NaN'));
});

test('online is the dashboard Online now: one per account with presence, never the Connection test', () => {
  const env = makeLobby();
  const a = online(env);
  online(env);
  const probe = env.connect('198.51.100.9');
  assert.equal(probe.hello({ ...GAME, presence: false }).code, 'ok');
  const second = env.connect('198.51.100.10');
  Object.assign(second, { account: a.account, secret: a.secret });
  assert.equal(second.hello(GAME).code, 'ok');
  assert.equal(showcase(env).live.online, 2);
  assert.equal(showcase(env).live.online, env.lobby.metricsSummary().live.online);
});

test('lobby counts the players listed in the Lobby, one per account', () => {
  const env = makeLobby();
  online(env);
  assert.equal(showcase(env).live.lobby, 0);
  online(env, { lobby: true });
  online(env, { lobby: true });
  assert.equal(showcase(env).live.lobby, 2);
  assert.equal(showcase(env).live.online, 3);
});

test('in_rooms and open_rooms are the dashboard In rooms and Open rooms', () => {
  const env = makeLobby();
  const host = online(env);
  const guest = online(env);
  online(env);
  const created = host.call('room.create', ROOM);
  assert.equal(guest.call('room.join', { room: created.room.id }).code, 'ok');
  assert.equal(online(env).call('room.create', ROOM).code, 'ok');
  const { live } = showcase(env);
  const summary = env.lobby.metricsSummary().live;
  assert.equal(live.in_rooms, 3);
  assert.equal(live.in_rooms, summary.in_rooms);
  assert.equal(live.open_rooms, 2);
  assert.equal(live.open_rooms, summary.rooms);
});

test('peak_today is the dashboard Peak today, kept after players leave', () => {
  const env = makeLobby();
  const players = [online(env), online(env), online(env)];
  players[0].drop();
  players[1].drop();
  const { live } = showcase(env);
  assert.equal(live.online, 1);
  assert.equal(live.peak_today, 3);
  assert.equal(live.peak_today, env.lobby.metricsSummary().today.peak_online);
  env.advance(DAY_MS);
  online(env);
  assert.equal(showcase(env).live.peak_today, env.lobby.metricsSummary().today.peak_online);
});

test('sessions is the dashboard Sessions today: a reconnect inside the window is the same session', () => {
  const env = makeLobby();
  const a = online(env);
  online(env);
  a.drop();
  const again = env.connect('198.51.100.77');
  Object.assign(again, { account: a.account, secret: a.secret });
  assert.equal(again.hello(GAME).code, 'ok');
  assert.equal(showcase(env).today.sessions, 2);
  assert.equal(showcase(env).today.sessions, env.lobby.metricsSummary().today.sessions);
  again.drop();
  env.advance(PROTECTION.sessionWindowMs);
  const later = env.connect('198.51.100.78');
  Object.assign(later, { account: a.account, secret: a.secret });
  assert.equal(later.hello(GAME).code, 'ok');
  assert.equal(showcase(env).today.sessions, 3);
});

test('rooms_created and joins today are the dashboard Rooms today and Joins today', () => {
  const env = makeLobby();
  const host = online(env);
  const first = online(env);
  const second = online(env);
  const created = host.call('room.create', ROOM);
  assert.equal(first.call('room.join', { room: created.room.id }).code, 'ok');
  assert.equal(second.call('room.join', { room: created.room.id }).code, 'ok');
  assert.equal(online(env).call('room.create', ROOM).code, 'ok');
  const { today } = showcase(env);
  const summary = env.lobby.metricsSummary().today;
  assert.equal(today.rooms_created, 2);
  assert.equal(today.rooms_created, summary.rooms_created);
  assert.equal(today.joins, 2);
  assert.equal(today.joins, summary.room_joins);
  first.call('room.leave', { room: created.room.id });
  assert.equal(first.call('room.join', { room: created.room.id }).code, 'ok');
  assert.equal(showcase(env).today.joins, 3);
});

test('rooms_with_guest counts rooms that ended with a guest: a host-only room and every extra join add nothing', () => {
  const env = makeLobby();
  const lonely = online(env);
  const { room } = lonely.call('room.create', ROOM);
  env.advance(30_000);
  assert.equal(lonely.call('room.leave', { room: room.id }).code, 'ok');
  assert.equal(env.lobby.metricsSummary().today.rooms_closed, 1);
  assert.equal(showcase(env).last_30_days.rooms_with_guest, 0);

  playRoom(env, { guests: 3 });
  const month = showcase(env).last_30_days;
  assert.equal(month.rooms_with_guest, 1);
  assert.equal(month.rooms_with_guest, env.lobby.metricsHistory(30).totals.rooms_with_guest);
  assert.equal(month.rooms_created, env.lobby.metricsHistory(30).totals.rooms_created);
  assert.equal(month.successful_joins, 3);

  const open = online(env).call('room.create', ROOM);
  assert.equal(online(env).call('room.join', { room: open.room.id }).code, 'ok');
  assert.equal(showcase(env).last_30_days.rooms_with_guest, 1);
});

test('parties_reported and average_party_size follow the dashboard formula; none gives a null average', () => {
  const env = makeLobby();
  const sizes = (day, values) => Object.entries(values).forEach(([size, count]) => env.host.sql.run(
    `INSERT INTO metrics_daily_dimensions (day, dimension, value, count) VALUES (?, 'party_size', ?, ?)`, day, size, count));
  assert.equal(published(env).last_30_days.average_party_size, null);
  sizes(metricsDay(env.clock.now), { 2: 2, 3: 1 });
  const month = published(env).last_30_days;
  assert.equal(month.parties_reported, 3);
  assert.equal(month.average_party_size, 2.33);
});

test('the 30 days hold today and the 29 days before it in America/Sao_Paulo days, never older or future days', () => {
  const env = makeLobby();
  env.clock.now = Date.UTC(2026, 10, 23, 2, 59);
  assert.equal(metricsDay(env.clock.now), '2026-11-22');
  insertDay(env, 0, { guests: 2 });
  insertDay(env, 29 * DAY_MS, { guests: 3 });
  insertDay(env, 30 * DAY_MS, { guests: 100 });
  insertDay(env, -DAY_MS, { guests: 1000 });
  assert.equal(showcase(env).last_30_days.first, '2026-10-24');
  assert.equal(showcase(env).last_30_days.last, '2026-11-22');
  assert.equal(showcase(env).last_30_days.rooms_with_guest, 5);
  env.clock.now = Date.UTC(2026, 10, 23, 3, 0);
  assert.equal(metricsDay(env.clock.now), '2026-11-23');
  assert.equal(showcase(env).last_30_days.first, '2026-10-25');
  assert.equal(showcase(env).last_30_days.rooms_with_guest, 1002);
  assert.deepEqual(showcase(env).today, { sessions: 0, rooms_created: 0, joins: 0 });
});

test('today starts at 03:00 UTC: the same instant before it still counts for the day before', () => {
  const env = makeLobby();
  env.clock.now = Date.UTC(2026, 10, 23, 2, 59);
  online(env);
  assert.equal(showcase(env).today.sessions, 1);
  env.advance(60_000);
  assert.equal(showcase(env).today.sessions, 0);
});

test('classes are the dashboard Characters in parties for the 30 days, most played first, ties by name', () => {
  const env = makeLobby();
  assert.deepEqual(showcase(env).last_30_days.classes, []);
  insertDay(env, 0, { classes: { Wizard: 5, Sorceress: 40 } });
  insertDay(env, DAY_MS, { classes: { Fighter: 9, Elf: 20 } });
  insertDay(env, 2 * DAY_MS, { classes: { Wizard: 4, Amazon: 20 } });
  insertDay(env, 29 * DAY_MS, { classes: { Dwarf: 1 } });
  insertDay(env, 30 * DAY_MS, { classes: { Dwarf: 500 } });
  const classes = showcase(env).last_30_days.classes;
  assert.deepEqual(classes, [{ class: 'Sorceress', count: 40 }, { class: 'Amazon', count: 20 }, { class: 'Elf', count: 20 },
    { class: 'Fighter', count: 9 }, { class: 'Wizard', count: 9 }, { class: 'Dwarf', count: 1 }]);
  const history = env.lobby.metricsHistory(30).dimensions.character;
  const byName = (rows) => [...rows].sort((a, b) => a.value.localeCompare(b.value));
  assert.deepEqual(byName(classes.map(({ class: name, count }) => ({ value: name, count }))), byName(history));
});

test('a real party report feeds the classes the dashboard shows', () => {
  const env = makeLobby();
  const host = online(env);
  const { room } = host.call('room.create', ROOM);
  assert.equal(online(env).call('room.join', { room: room.id }).code, 'ok');
  assert.equal(host.call('party.report', { classes: [4, 2] }).code, 'ok');
  assert.deepEqual(showcase(env).last_30_days.classes, [{ class: 'Elf', count: 1 }, { class: 'Sorceress', count: 1 }]);
});

test('an optional value that cannot be read is null and nothing else is lost', () => {
  const log = [];
  const env = makeLobby({ log: (text) => log.push(text) });
  online(env);
  env.lobby.metrics.classUsage = () => {
    throw new Error('class table is locked');
  };
  const month = showcase(env).last_30_days;
  assert.equal(month.classes, null);
  assert.equal(month.rooms_with_guest, 0);
  assert.equal(showcase(env).live.online, 1);
  assert.equal(published(env).last_30_days.classes, null);
  assert.ok(published(env).last_30_days);

  const peak = makeLobby({ log: () => {} });
  online(peak);
  const todayRow = peak.lobby.metrics.todayRow.bind(peak.lobby.metrics);
  let calls = 0;
  peak.lobby.metrics.todayRow = () => {
    calls += 1;
    if (calls === 1) throw new Error('daily row unavailable');
    return todayRow();
  };
  const live = showcase(peak).live;
  assert.equal(live.peak_today, null);
  assert.equal(live.online, 1);
  assert.equal(live.in_rooms, 0);
  assert.ok(log.length >= 1);
});

test('a section that cannot be read is null while the others stay valid; it is never zero', () => {
  const env = makeLobby({ log: () => {} });
  online(env);
  env.lobby.metrics.showcaseMonth = () => {
    throw new Error('table is locked');
  };
  const body = showcase(env);
  assert.equal(body.last_30_days, null);
  assert.equal(body.live.online, 1);
  assert.deepEqual(body.today, { sessions: 1, rooms_created: 0, joins: 0 });

  const live = makeLobby({ log: () => {} });
  live.lobby.liveMetrics = () => {
    throw new Error('sockets unavailable');
  };
  assert.equal(showcase(live).live, null);
  assert.deepEqual(showcase(live).today, { sessions: 0, rooms_created: 0, joins: 0 });

  const today = makeLobby({ log: () => {} });
  today.lobby.metrics.showcaseToday = () => {
    throw new Error('table is locked');
  };
  assert.equal(showcase(today).today, null);
  assert.equal(showcase(today).live.online, 0);
});

test('nothing that names a player, an address, a platform or a country leaves the service', () => {
  const env = makeLobby();
  const host = online(env, { lobby: true }, 'US');
  host.call('room.create', ROOM);
  insertDay(env, 0, { guests: 1, classes: { Elf: 2 } });
  const text = JSON.stringify(published(env));
  for (const secret of ['203.0.', 'Secret', host.account, 'Windows', 'US', 'BR', '1.4.1', 'platform', 'version', 'name', 'country', 'countries']) {
    assert.ok(!text.includes(secret), secret);
  }
  const leaked = publicSections({ ip: '203.0.113.1',
    live: { online: 1, lobby: 0, in_rooms: 0, open_rooms: 0, peak_today: 1, countries: [{ code: 'US', count: 1 }], names: ['Bob'] },
    today: { sessions: 1, rooms_created: 1, joins: 4, users: ['Bob'] },
    last_30_days: { rooms_created: 9, successful_joins: 2, parties_reported: 3, rooms_with_guest: 2, users: ['Bob'],
      classes: [{ class: 'Elf', count: 2, account: '5001' }] } });
  assertOnlyApprovedFields({ generated: 1, ...leaked });
  assert.deepEqual(leaked.last_30_days.classes, [{ class: 'Elf', count: 2 }]);
  assert.ok(!JSON.stringify(leaked).includes('Bob'));
});

test('malformed fields become null, not zero, NaN, negatives or text; a bad core field drops only its section', () => {
  const live = { online: 1, lobby: 0, in_rooms: 0, open_rooms: 0, peak_today: 1 };
  const month = { rooms_created: 3, successful_joins: 2, parties_reported: 1, rooms_with_guest: 1 };
  assert.deepEqual(publicSections({ live: { ...live, online: -1 }, today: null, last_30_days: { ...month, rooms_created: Infinity } }),
    { live: null, today: null, last_30_days: null, lifetime: { sessions: null, players: null, since: null } });
  assert.deepEqual(publicSections(undefined), { live: null, today: null, last_30_days: null, lifetime: { sessions: null, players: null, since: null } });
  assert.equal(publicSections({ live: { online: 1, open_rooms: 2 } }).live, null);
  for (const bad of ['3', 1.5, -1, NaN, null]) {
    const parsed = publicSections({ live: { ...live, peak_today: bad, in_rooms: bad } }).live;
    assert.equal(parsed.peak_today, null);
    assert.equal(parsed.in_rooms, null);
    assert.equal(parsed.online, 1);
  }
  for (const bad of [[{ class: 'Paladin', count: 1 }], [{ class: 'Elf', count: -1 }], [{ class: 'Elf', count: 0 }], [{ class: 'Elf' }], [null], 'Elf', {}]) {
    const parsed = publicSections({ last_30_days: { ...month, classes: bad } }).last_30_days;
    assert.equal(parsed.classes, null);
    assert.equal(parsed.rooms_created, 3);
  }
  assert.deepEqual(publicSections({ last_30_days: { ...month, classes: [] } }).last_30_days.classes, []);
  assert.equal(publicSections({ last_30_days: { ...month, rooms_with_guest: 'x' } }).last_30_days.rooms_with_guest, null);
  assert.equal(publicSections({ last_30_days: { ...month, average_party_size: 3 } }).last_30_days.average_party_size, 3);
  assert.equal(publicSections({ last_30_days: { ...month, parties_reported: 0, average_party_size: 3 } }).last_30_days.average_party_size, null);
});

test('reading the statistics creates no session, presence, room or metrics row', () => {
  const env = makeLobby();
  const rows = () => ['metrics_daily', 'metrics_daily_dimensions', 'metrics_samples', 'accounts', 'rooms', 'members']
    .map((table) => JSON.stringify(env.host.sql.all(`SELECT * FROM ${table}`)));
  const before = rows();
  for (let i = 0; i < 5; i++) showcase(env);
  assert.deepEqual(rows(), before);
  assert.equal(env.lobby.metricsSummary().today.sessions, 0);
  assert.equal(env.lobby.metricsSummary().live.online, 0);
});

function fakeLobby(overrides = {}) {
  const calls = { n: 0 };
  const answer = { generated: 1, live: { online: 7, lobby: 3, in_rooms: 3, open_rooms: 2, peak_today: 11 },
    today: { sessions: 9, rooms_created: 5, joins: 4 },
    last_30_days: { rooms_created: 100, successful_joins: 40, parties_reported: 20, average_party_size: 2.24, rooms_with_guest: 60,
      classes: [{ class: 'Fighter', count: 30 }, { class: 'Wizard', count: 10 }] }, ...overrides };
  return { calls, read: async () => { calls.n += 1; return answer; }, answer };
}

test('the endpoint needs no token, answers GET and HEAD only, and keeps one lobby read per 30 seconds', async () => {
  let clock = Date.UTC(2026, 10, 23, 12);
  const handler = statsHandler(() => clock);
  const lobby = fakeLobby();
  const first = await handler(get(), lobby.read);
  assert.equal(first.status, 200);
  assert.equal(first.headers.get('Content-Type'), 'application/json; charset=utf-8');
  assert.equal(first.headers.get('Cache-Control'), 'public, max-age=30');
  const body = await first.json();
  assertOnlyApprovedFields(body);
  assert.deepEqual(body.live, { online: 7, lobby: 3, in_rooms: 3, open_rooms: 2, peak_today: 11 });
  assert.deepEqual(body.today, { sessions: 9, rooms_created: 5, joins: 4 });
  assert.equal(body.last_30_days.rooms_with_guest, 60);
  assert.equal(body.generated, new Date(clock).toISOString());
  assert.equal((await handler(get('/stats?x=<script>'), lobby.read)).status, 200);
  const head = await handler(get(STATS_PATH, { method: 'HEAD' }), lobby.read);
  assert.equal(head.status, 200);
  assert.equal(await head.text(), '');
  assert.equal(lobby.calls.n, 1);
  clock += STATS_TTL_MS;
  await handler(get(), lobby.read);
  assert.equal(lobby.calls.n, 2);
  for (const method of ['POST', 'PUT', 'DELETE', 'PATCH']) {
    const refused = await handler(get(STATS_PATH, { method, body: method === 'DELETE' ? undefined : '{}' }), lobby.read);
    assert.equal(refused.status, 405);
    assert.equal(refused.headers.get('Allow'), 'GET, HEAD');
  }
  assert.equal(lobby.calls.n, 2);
});

test('a read that fails keeps the last good value of every field, then marks it unavailable', async () => {
  let clock = 1_000_000;
  const handler = statsHandler(() => clock);
  let failing = false;
  const read = async () => {
    if (failing) throw new Error('lobby answered 404');
    return fakeLobby().answer;
  };
  assert.equal((await handler(get(), read)).status, 200);
  failing = true;
  clock += STATS_TTL_MS;
  const stale = await handler(get(), read);
  assert.equal(stale.status, 200);
  assert.equal(stale.headers.get('Cache-Control'), 'no-store');
  const kept = await stale.json();
  assert.equal(kept.live.online, 7);
  assert.equal(kept.live.peak_today, 11);
  assert.equal(kept.last_30_days.rooms_with_guest, 60);

  clock += LIVE_STALE_MS;
  const partial = await (await handler(get(), read)).json();
  assert.equal(partial.live, null);
  assert.equal(partial.today, null);
  assert.equal(partial.last_30_days.rooms_with_guest, 60);

  clock += MONTH_STALE_MS;
  const gone = await handler(get(), read);
  assert.equal(gone.status, 503);
  assert.deepEqual(await gone.json(), { error: 'unavailable' });
  assert.equal(gone.headers.get('Cache-Control'), 'no-store');
});

test('one bad refresh of an optional value does not erase its last good value', async () => {
  let clock = 2_000_000;
  const handler = statsHandler(() => clock);
  const good = fakeLobby();
  assert.equal((await (await handler(get(), good.read)).json()).last_30_days.classes.length, 2);
  clock += STATS_TTL_MS;
  const degraded = fakeLobby({ live: { online: 8, lobby: 3, open_rooms: 2, in_rooms: null, peak_today: null },
    last_30_days: { ...good.answer.last_30_days, classes: null, rooms_with_guest: null } });
  const body = await (await handler(get(), degraded.read)).json();
  assert.equal(body.live.online, 8);
  assert.equal(body.live.peak_today, 11);
  assert.equal(body.live.in_rooms, 3);
  assert.equal(body.last_30_days.classes.length, 2);
  assert.equal(body.last_30_days.rooms_with_guest, 60);
});

test('a failed read is retried soon, not after a full cache period, and recovers by itself', async () => {
  let clock = 5_000_000;
  const handler = statsHandler(() => clock);
  let failing = true;
  let reads = 0;
  const read = async () => {
    reads += 1;
    if (failing) throw new Error('lobby answered 404');
    return fakeLobby().answer;
  };
  assert.equal((await handler(get(), read)).status, 503);
  assert.equal((await handler(get(), read)).status, 503);
  assert.equal(reads, 1);
  failing = false;
  clock += STATS_RETRY_MS;
  const healthy = await handler(get(), read);
  assert.equal(healthy.status, 200);
  assert.equal((await healthy.json()).live.in_rooms, 3);
  assert.equal(healthy.headers.get('Cache-Control'), 'public, max-age=30');
  assert.equal(reads, 2);
});

test('when only one source fails the other sections are still served and the missing one is null', async () => {
  const handler = statsHandler(() => 10_000_000);
  const body = await (await handler(get(), fakeLobby({ live: null, today: null }).read)).json();
  assertOnlyApprovedFields(body);
  assert.equal(body.live, null);
  assert.equal(body.today, null);
  assert.equal(body.last_30_days.rooms_with_guest, 60);
  assert.deepEqual(body.last_30_days.classes, [{ class: 'Fighter', count: 30 }, { class: 'Wizard', count: 10 }]);
});

test('the last answer that worked in production is still served, its new values null', async () => {
  const handler = statsHandler(() => 20_000_000);
  const answer = await handler(get(), async () => LAST_WORKING);
  assert.equal(answer.status, 200);
  const body = await answer.json();
  assertOnlyApprovedFields(body);
  assert.deepEqual(body.live, { online: 19, lobby: 8, in_rooms: null, open_rooms: 4, peak_today: null });
  assert.deepEqual(body.today, { sessions: null, rooms_created: 28, joins: null });
  assert.deepEqual(body.last_30_days, { rooms_created: 10860, successful_joins: 4060, parties_reported: 2364, average_party_size: 2.24,
    rooms_with_guest: null, classes: null });
});

const sessionLater = (env, account) => {
  env.advance(PROTECTION.sessionWindowMs);
  const again = env.connect(`198.51.100.${nextIp++ % 250 + 1}`);
  Object.assign(again, { account: account.account, secret: account.secret });
  assert.equal(again.hello(GAME).code, 'ok');
  again.drop();
};

test('the 30 day section is read at most once per 10 minutes; an incomplete answer is not kept', () => {
  const env = makeLobby({ log: () => {} });
  insertDay(env, 0, { guests: 1, classes: { Elf: 1 } });
  const first = env.lobby.showcase().last_30_days;
  insertDay(env, 0, { guests: 1, classes: { Elf: 1 } });
  assert.deepEqual(env.lobby.showcase().last_30_days, first);
  env.advance(10 * 60_000);
  assert.equal(env.lobby.showcase().last_30_days.rooms_with_guest, first.rooms_with_guest + 1);
  env.lobby.monthMemo = null;
  env.lobby.metrics.classUsage = () => { throw new Error('unreadable'); };
  assert.equal(env.lobby.showcase().last_30_days.classes, null);
  assert.equal(env.lobby.monthMemo, null);
});

test('lifetime sessions sum every retained day of the dashboard Sessions metric', () => {
  const env = makeLobby();
  assert.deepEqual(showcase(env).lifetime, { sessions: 0, since: null, players: 0 });
  const day = (msAgo, sessions) => env.host.sql.run('INSERT INTO metrics_daily (day, sessions) VALUES (?, ?)', metricsDay(env.clock.now - msAgo), sessions);
  day(800 * DAY_MS, 100);
  day(400 * DAY_MS, 0);
  day(40 * DAY_MS, 25);
  env.advance(10 * 60_000);
  online(env);
  const { lifetime } = showcase(env);
  assert.equal(lifetime.sessions, 126);
  assert.equal(lifetime.since, metricsDay(env.clock.now - 800 * DAY_MS));
  assert.equal(lifetime.sessions, env.host.sql.get('SELECT SUM(sessions) AS n FROM metrics_daily').n);
  assert.ok(lifetime.sessions > env.lobby.metricsHistory(365).totals.sessions);
});

test('lifetime sessions keep the reconnect rule: a return inside the window is the same session', () => {
  const env = makeLobby();
  const a = online(env);
  a.drop();
  const quick = env.connect('198.51.100.60');
  Object.assign(quick, { account: a.account, secret: a.secret });
  assert.equal(quick.hello(GAME).code, 'ok');
  quick.drop();
  assert.equal(showcase(env).lifetime.sessions, 1);
  assert.equal(showcase(env).today.sessions, env.lobby.metricsSummary().today.sessions);
  sessionLater(env, a);
  env.advance(10 * 60_000);
  const { lifetime } = showcase(env);
  assert.equal(lifetime.sessions, 2);
  assert.equal(lifetime.sessions, env.host.sql.get('SELECT SUM(sessions) AS n FROM metrics_daily').n);
});

test('lifetime players are distinct accounts: many sessions of one account count once, other accounts separately', () => {
  const env = makeLobby();
  const a = online(env);
  a.drop();
  for (let i = 0; i < 4; i++) sessionLater(env, a);
  env.advance(10 * 60_000);
  assert.equal(showcase(env).lifetime.players, 1);
  assert.equal(showcase(env).lifetime.sessions, 5);
  online(env);
  online(env);
  const probe = env.connect('198.51.100.9');
  assert.equal(probe.hello({ ...GAME, presence: false }).code, 'ok');
  env.advance(10 * 60_000);
  assert.equal(showcase(env).lifetime.sessions, 7);
  assert.equal(showcase(env).lifetime.players, 4);
  assert.equal(showcase(env).lifetime.players, env.lobby.stats().accounts);
});

test('lifetime totals are kept for ten minutes, then follow the data again', () => {
  const env = makeLobby();
  online(env);
  assert.equal(showcase(env).lifetime.sessions, 1);
  online(env);
  assert.equal(showcase(env).lifetime.sessions, 1);
  env.advance(10 * 60_000);
  assert.deepEqual([showcase(env).lifetime.sessions, showcase(env).lifetime.players], [2, 2]);
});

test('a lifetime value that cannot be read is null and /stats keeps everything else', () => {
  const env = makeLobby({ log: () => {} });
  online(env);
  env.lobby.metrics.showcaseLifetime = () => {
    throw new Error('daily table is locked');
  };
  assert.deepEqual(showcase(env).lifetime, { sessions: null, since: null, players: 1 });
  assert.equal(showcase(env).live.online, 1);
  assert.deepEqual(publicSections(showcase(env)).lifetime, { sessions: null, players: 1, since: null });

  const accounts = makeLobby({ log: () => {} });
  online(accounts);
  const read = accounts.host.sql.get.bind(accounts.host.sql);
  accounts.host.sql.get = (query, ...args) => {
    if (query.includes('FROM accounts')) throw new Error('accounts unavailable');
    return read(query, ...args);
  };
  assert.equal(showcase(accounts).lifetime.players, null);
  assert.equal(showcase(accounts).lifetime.sessions, 1);
  assert.equal(showcase(accounts).live.online, 1);
});

test('lifetime is additive: the older sections are exactly what they were', () => {
  const env = makeLobby();
  online(env);
  const body = showcase(env);
  const { lifetime, generated, ...others } = body;
  assert.ok(generated > 0);
  assert.deepEqual(Object.keys(others).sort(), ['last_30_days', 'live', 'today']);
  assert.deepEqual(publicSections(body).live, { online: 1, lobby: 0, in_rooms: 0, open_rooms: 0, peak_today: 1 });
  assert.deepEqual(publicSections(body).today, { sessions: 1, rooms_created: 0, joins: 0 });
  assert.equal(lifetime.sessions, 1);
});

test('a missing, partial or malformed lifetime section never fails /stats', async () => {
  for (const lifetime of [undefined, null, {}, { sessions: 'x', players: -1, since: 'yesterday' }, [], 7]) {
    const handler = statsHandler(() => 30_000_000);
    const answer = await handler(get(), fakeLobby({ lifetime }).read);
    assert.equal(answer.status, 200);
    const body = await answer.json();
    assert.deepEqual(body.lifetime, { sessions: null, players: null, since: null });
    assert.equal(body.live.online, 7);
    assert.equal(body.last_30_days.rooms_with_guest, 60);
  }
  const handler = statsHandler(() => 31_000_000);
  const half = await (await handler(get(), fakeLobby({ lifetime: { sessions: 11349, since: '2026-09-01', players: null } }).read)).json();
  assert.deepEqual(half.lifetime, { sessions: 11349, players: null, since: '2026-09-01' });
});

test('lifetime values keep their last good value for an hour and never decide the status', async () => {
  let clock = 40_000_000;
  const handler = statsHandler(() => clock);
  const good = fakeLobby({ lifetime: { sessions: 11349, players: 3842, since: '2026-09-01' } });
  assert.deepEqual((await (await handler(get(), good.read)).json()).lifetime, { sessions: 11349, players: 3842, since: '2026-09-01' });
  clock += STATS_TTL_MS;
  const bad = fakeLobby({ lifetime: { sessions: null, players: null, since: null } });
  assert.deepEqual((await (await handler(get(), bad.read)).json()).lifetime, { sessions: 11349, players: 3842, since: '2026-09-01' });
  clock += LIFETIME_STALE_MS;
  const expired = await (await handler(get(), bad.read)).json();
  assert.deepEqual(expired.lifetime, { sessions: null, players: null, since: null });
  assert.equal(expired.live.online, 7);

  const only = statsHandler(() => 50_000_000);
  const answer = await only(get(), async () => ({ lifetime: { sessions: 5, players: 2, since: '2026-09-01' } }));
  assert.equal(answer.status, 503);
});

test('the public lifetime holds totals only', () => {
  const env = makeLobby();
  const a = online(env, { name: 'SecretName' });
  const text = JSON.stringify(publicSections(showcase(env)).lifetime);
  for (const secret of [a.account, 'SecretName', '203.0.', 'account', 'name']) assert.ok(!text.includes(secret), secret);
  assert.deepEqual(Object.keys(JSON.parse(text)).sort(), KEYS.lifetime);
});

const workerTest = (name, run) => nodeTest(name, { skip: available ? false : 'needs Node.js 22.15 or later (module.registerHooks)' }, run);

let isolates = 0;
async function workerWith(stub) {
  const { default: worker } = await import(`../src/worker.js?isolate=${isolates++}`);
  return (path, init) => worker.fetch(get(path, init), { LOBBY: { idFromName: () => 'global', get: () => stub } });
}

workerTest('regression: the Worker serves the answer shape that worked in production, not 503', async () => {
  const request = await workerWith({ fetch: async () => Response.json(LAST_WORKING) });
  const answer = await request(STATS_PATH);
  assert.equal(answer.status, 200);
  const body = await answer.json();
  assert.equal(body.live.online, 19);
  assert.equal(body.live.lobby, 8);
  assert.equal(body.live.open_rooms, 4);
  assert.equal(body.today.rooms_created, 28);
  assert.equal(body.last_30_days.successful_joins, 4060);
  assert.deepEqual(body.lifetime, { sessions: null, players: null, since: null });
});

workerTest('production path: /stats through the Worker and the Durable Object returns every field', async () => {
  const state = new DurableObjectState();
  const object = await state.wake();
  const ws = await state.connect(object, '198.18.0.1');
  state.deliver(object, ws, JSON.stringify({ op: 'hello', seq: 1, proto: 1, game: 'CUSA10454-1.00', account: '5001', secret: 'cd'.repeat(32),
    name: 'Tester', relay: true, online_protocol: 1, lobby: true }));
  const request = await workerWith({ fetch: (url, init) => object.fetch(new Request(url, init)) });
  const answer = await request(STATS_PATH);
  assert.equal(answer.status, 200);
  const body = await answer.json();
  assertOnlyApprovedFields(body);
  assert.deepEqual(body.live, { online: 1, lobby: 1, in_rooms: 0, open_rooms: 0, peak_today: 1 });
  assert.deepEqual(body.today, { sessions: 1, rooms_created: 0, joins: 0 });
  assert.deepEqual(body.last_30_days, { rooms_created: 0, successful_joins: 0, parties_reported: 0, average_party_size: null,
    rooms_with_guest: 0, classes: [] });
  assert.equal(body.lifetime.sessions, 1);
  assert.equal(body.lifetime.players, 1);
  assert.match(body.lifetime.since, /^\d{4}-\d{2}-\d{2}$/);
  assert.equal((await request(STATS_PATH, { method: 'HEAD' })).status, 200);
});

workerTest('a Durable Object that cannot read its optional values still answers through the Worker', async () => {
  const state = new DurableObjectState();
  const object = await state.wake();
  object.lobby.metrics.classUsage = () => {
    throw new Error('class table is locked');
  };
  object.lobby.host.log = () => {};
  const request = await workerWith({ fetch: (url, init) => object.fetch(new Request(url, init)) });
  const body = await (await request(STATS_PATH)).json();
  assert.equal(body.last_30_days.classes, null);
  assert.equal(body.last_30_days.rooms_with_guest, 0);
  assert.equal(body.live.online, 0);
});

workerTest('an object that does not know the statistics route (older code answering during a rollout) is a 503 that recovers', async () => {
  const state = new DurableObjectState();
  const object = await state.wake();
  let old = true;
  const stub = { fetch: (url, init) => (old ? Promise.resolve(new Response('Not found', { status: 404 })) : object.fetch(new Request(url, init))) };
  const request = await workerWith(stub);
  assert.deepEqual(await (await request(STATS_PATH)).json(), { error: 'unavailable' });
  old = false;
  const realNow = Date.now;
  try {
    Date.now = () => realNow() + STATS_RETRY_MS;
    assert.equal((await request(STATS_PATH)).status, 200);
  } finally {
    Date.now = realNow;
  }
});

test('the admin API stays behind its token and the showcase route stays internal', async () => {
  const env = { DCP_DASHBOARD_TOKEN: 'a-long-dashboard-token-value' };
  const never = { summary: async () => assert.fail('summary read without a token') };
  for (const path of ['/admin/api/summary', '/admin/api/history', '/admin/api/incidents']) {
    assert.equal((await handleAdmin(get(path), env, never)).status, 401);
    assert.equal((await handleAdmin(get(path, { headers: { Authorization: 'Bearer wrong-token-of-sixteen+' } }), env, never)).status, 401);
  }
  assert.deepEqual(internalMetricsRoute(get('/metrics/showcase')), { kind: 'showcase' });
  assert.equal(internalMetricsRoute(get('/metrics/showcase', { headers: { Upgrade: 'websocket' } })), null);

  const worker = readFileSync(new URL('../src/worker.js', import.meta.url), 'utf8');
  const stats = worker.indexOf('url.pathname === STATS_PATH');
  assert.ok(stats > 0 && stats < worker.indexOf('isAdminPath(url.pathname)'));
  assert.ok(worker.includes("serveStats(request, () => internal('/metrics/showcase'))"));
  assert.equal(worker.match(/stub\.fetch\(/g).length, 2);
  const local = readFileSync(new URL('../local/server.js', import.meta.url), 'utf8');
  assert.ok(local.includes('lobby.showcase()') && local.includes('statsHandler()'));
});

test('the dashboard still has its own countries; the public statistics never had them', () => {
  const env = makeLobby();
  online(env, {}, 'US');
  assert.deepEqual(env.lobby.metricsSummary().live.country, [{ value: 'US', count: 1 }]);
  assert.ok(!JSON.stringify(showcase(env)).includes('US'));
});
