import assert from 'node:assert/strict';
import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';

import { COUNTRY_HEADER, handleAdmin, internalMetricsRoute, tokenMatches, withEdgeCountry } from '../src/admin.js';
import { ADMIN_CSS, ADMIN_HTML, ADMIN_JS } from '../src/admin_page.js';
import { Lobby } from '../src/lobby.js';
import { SAMPLE_MS, countryCode, metricsDay, sampleBucket } from '../src/metrics.js';
import { LocalHost, sqliteStore } from '../local/host.js';
import { ROOM, makeLobby } from './harness.js';

const WINDOWS = { online_protocol: 1, version: '0.8.0', platform: 'Windows' };
const REPO = join(import.meta.dirname, '..', '..', '..');

function online(env, fields = WINDOWS, ip = '203.0.113.7', country = 'BR') {
  const client = env.connect(ip, country);
  assert.equal(client.hello(fields).code, 'ok');
  return client;
}

const today = (env) => env.lobby.metricsSummary().today;
const live = (env) => env.lobby.metricsSummary().live;
const dimension = (env, name, day = metricsDay(env.clock.now)) => Object.fromEntries(env.host.sql.all(
  'SELECT value, count FROM metrics_daily_dimensions WHERE day = ? AND dimension = ?', day, name).map((r) => [r.value, r.count]));

function metricsRows(env) {
  return ['metrics_daily', 'metrics_daily_dimensions', 'metrics_samples']
    .flatMap((table) => env.host.sql.all(`SELECT * FROM ${table}`));
}

test('a presence connection counts one session, however often it publishes presence', () => {
  const env = makeLobby();
  const a = online(env);
  for (let i = 0; i < 5; i++) assert.equal(a.call('presence.set', { data: 'ab'.repeat(8) }).code, 'ok');
  assert.equal(a.hello().code, 'bad_request');
  assert.equal(a.call('friends.list').code, 'ok');
  assert.equal(today(env).sessions, 1);
});

test('a reconnect is another session; the Connection test and refused sign-ins are none', () => {
  const env = makeLobby();
  const a = online(env);
  a.drop();
  const again = env.connect();
  Object.assign(again, { account: a.account, secret: a.secret });
  assert.equal(again.hello(WINDOWS).code, 'ok');
  assert.equal(today(env).sessions, 2);
  const probe = env.connect();
  Object.assign(probe, { account: a.account, secret: a.secret });
  assert.equal(probe.hello({ ...WINDOWS, presence: false }).code, 'ok');
  const forged = env.connect();
  Object.assign(forged, { account: a.account, secret: 'cd'.repeat(32) });
  assert.equal(forged.hello(WINDOWS).code, 'unauthorized');
  assert.equal(env.connect().hello({ ...WINDOWS, secret: 'short' }).code, 'bad_request');
  assert.equal(today(env).sessions, 2);
  assert.equal(live(env).online, 1);
});

test('rooms created and successful joins are counted; failed joins are not', () => {
  const env = makeLobby();
  const host = online(env);
  const { room } = host.call('room.create', { ...ROOM, slots: 2 });
  assert.equal(today(env).rooms_created, 1);
  const guest = online(env, WINDOWS, '198.51.100.4');
  assert.equal(guest.call('room.join', { room: room.id }).code, 'ok');
  assert.equal(today(env).room_joins, 1);
  const late = online(env, WINDOWS, '198.51.100.5');
  assert.equal(late.call('room.join', { room: room.id }).code, 'full');
  assert.equal(late.call('room.join', { room: '999999' }).code, 'not_found');
  assert.equal(late.call('room.join', { code: 'NOPE42' }).code, 'not_found');
  const t = today(env);
  assert.deepEqual([t.rooms_created, t.room_joins, t.incompatible_rejects], [1, 1, 0]);
});

test('only an explicit protocol refusal counts as an incompatibility reject', () => {
  const env = makeLobby();
  const host = online(env);
  const { room } = host.call('room.create', ROOM);
  const old = online(env, { online_protocol: undefined, version: '0.7.0', platform: 'Windows' }, '198.51.100.4');
  assert.equal(old.call('room.search', { world: 1 }).incompatible, 1);
  assert.equal(today(env).incompatible_rejects, 0);
  assert.equal(old.call('room.join', { room: room.id }).code, 'incompatible');
  const stranger = env.connect('198.51.100.9');
  assert.equal(stranger.hello({ proto: 9 }).code, 'proto');
  assert.equal(stranger.hello({ proto: 9 }).code, 'proto');
  assert.equal(today(env).incompatible_rejects, 2);
  assert.deepEqual(dimension(env, 'reject'), { lobby_protocol: 1, online_protocol: 1 });
  assert.equal(today(env).room_joins, 0);
});

test('online now follows open presence connections, one per account', () => {
  const env = makeLobby();
  const a = online(env);
  const launcher = env.connect();
  Object.assign(launcher, { account: a.account, secret: a.secret });
  assert.equal(launcher.hello(WINDOWS).code, 'ok');
  const b = online(env, WINDOWS, '198.51.100.4');
  online(env, { ...WINDOWS, presence: false }, '198.51.100.5');
  assert.equal(live(env).online, 2);
  b.drop();
  assert.equal(live(env).online, 1);
  launcher.drop();
  assert.equal(live(env).online, 1);
  a.drop();
  assert.equal(live(env).online, 0);
});

test('lost sockets and a Durable Object restart leave no stale live counts', () => {
  const env = makeLobby();
  const a = online(env);
  a.call('room.create', ROOM);
  online(env, WINDOWS, '198.51.100.4');
  assert.deepEqual([live(env).online, live(env).in_rooms, live(env).rooms], [2, 1, 1]);
  // Sockets gone without a close callback: only the sockets the host still has count.
  env.host.conns.clear();
  assert.equal(live(env).online, 0);
  const restarted = new Lobby(env.host);
  const after = restarted.metricsSummary();
  assert.deepEqual([after.live.online, after.live.in_rooms, after.live.rooms], [0, 0, 1]);
  assert.equal(after.today.peak_online, 2);
  env.advance(31_000);
  restarted.alarm();
  assert.equal(restarted.metricsSummary().live.rooms, 0);
});

test('peaks only grow during a day: online, in rooms and rooms', () => {
  const env = makeLobby();
  const host = online(env);
  host.call('room.create', ROOM);
  const guests = [1, 2, 3].map((i) => online(env, WINDOWS, `198.51.100.${i}`));
  const id = env.host.sql.get('SELECT id FROM rooms').id;
  assert.equal(guests[0].call('room.join', { room: String(id) }).code, 'ok');
  const other = guests[1];
  assert.equal(other.call('room.create', ROOM).code, 'ok');
  let t = today(env);
  assert.deepEqual([t.peak_online, t.peak_in_rooms, t.peak_rooms], [4, 3, 2]);
  for (const g of guests) g.drop();
  env.advance(31_000);
  t = today(env);
  assert.deepEqual([live(env).online, live(env).in_rooms, live(env).rooms], [1, 1, 1]);
  assert.deepEqual([t.peak_online, t.peak_in_rooms, t.peak_rooms], [4, 3, 2]);
  const stored = env.host.sql.get('SELECT peak_online, peak_in_rooms, peak_rooms FROM metrics_daily');
  assert.deepEqual(stored, { peak_online: 4, peak_in_rooms: 3, peak_rooms: 2 });
});

test('days roll over at midnight in America/Sao_Paulo, not UTC', () => {
  const env = makeLobby();
  env.clock.now = Date.parse('2026-10-30T02:58:00Z'); // 23:58 on the 29th in Sao Paulo
  assert.equal(metricsDay(env.clock.now), '2026-10-29');
  const a = online(env);
  online(env, WINDOWS, '198.51.100.4').drop();
  env.advance(3 * 60_000);
  assert.equal(metricsDay(env.clock.now), '2026-10-30');
  online(env, WINDOWS, '198.51.100.5');
  const rows = Object.fromEntries(env.host.sql.all('SELECT day, sessions, peak_online FROM metrics_daily')
    .map((r) => [r.day, [r.sessions, r.peak_online]]));
  assert.deepEqual(rows, { '2026-10-29': [2, 2], '2026-10-30': [1, 2] });
  a.drop();
  const history = env.lobby.metricsHistory(2);
  assert.deepEqual(history.daily.map((d) => d.day), ['2026-10-29', '2026-10-30']);
  assert.equal(history.time_zone, 'America/Sao_Paulo');
});

test('live platform, version and country breakdowns count players online', () => {
  const env = makeLobby();
  online(env, WINDOWS, '203.0.113.1', 'BR');
  online(env, { ...WINDOWS, platform: 'Steam Deck', version: '0.8.1' }, '203.0.113.2', 'US');
  online(env, { ...WINDOWS, platform: 'Linux' }, '203.0.113.3', 'BR');
  online(env, { ...WINDOWS, platform: 'FreeBSD 14.1', version: 'my build' }, '203.0.113.4', 'XX');
  online(env, { platform: undefined, version: undefined }, '203.0.113.5', null);
  const now = live(env);
  const map = (items) => Object.fromEntries(items.map((i) => [i.value, i.count]));
  assert.deepEqual(map(now.platform), { Windows: 1, 'Steam Deck': 1, Linux: 1, Other: 2 });
  assert.deepEqual(map(now.version), { '0.8.0': 2, '0.8.1': 1, Other: 1, Unknown: 1 });
  assert.deepEqual(map(now.country), { BR: 2, US: 1, Unknown: 2 });
});

test('daily dimensions count sessions started, not players', () => {
  const env = makeLobby();
  const a = online(env, WINDOWS, '203.0.113.1', 'JP');
  for (let i = 0; i < 2; i++) {
    const again = env.connect('203.0.113.1', 'JP');
    Object.assign(again, { account: a.account, secret: a.secret });
    assert.equal(again.hello(WINDOWS).code, 'ok');
    again.drop();
  }
  assert.deepEqual(dimension(env, 'platform'), { Windows: 3 });
  assert.deepEqual(dimension(env, 'version'), { '0.8.0': 3 });
  assert.deepEqual(dimension(env, 'country'), { JP: 3 });
  assert.equal(live(env).online, 1);
  const history = env.lobby.metricsHistory(30);
  assert.deepEqual(history.dimensions.platform, [{ value: 'Windows', count: 3 }]);
  assert.equal(history.daily.length, 30);
});

test('new values per dimension and day are bounded; further ones count as Other', () => {
  const env = makeLobby();
  for (let i = 0; i < 70; i++) online(env, { ...WINDOWS, version: `1.${i}.0` }, `203.0.113.${i + 1}`).drop();
  const versions = dimension(env, 'version');
  assert.equal(Object.keys(versions).length, 65);
  assert.equal(versions.Other, 6);
});

test('country: Unknown when Cloudflare gives none, and a client cannot choose its own', async () => {
  for (const value of [undefined, null, '', 'XX', 'T1', 'br', 'BRA', '<b>']) assert.equal(countryCode(value), 'Unknown');
  assert.equal(countryCode('DE'), 'DE');
  const spoofed = new Request('https://dcp-lobby.example/', { headers: { [COUNTRY_HEADER]: 'JP', 'CF-Connecting-IP': '203.0.113.7' } });
  assert.equal(withEdgeCountry(spoofed, { country: 'BR', city: 'Recife', postalCode: '50000' }).headers.get(COUNTRY_HEADER), 'BR');
  assert.equal(withEdgeCountry(spoofed, undefined).headers.get(COUNTRY_HEADER), 'Unknown');
  assert.equal(withEdgeCountry(spoofed, { country: 'BR' }).headers.get('CF-Connecting-IP'), '203.0.113.7');
  const env = makeLobby();
  online(env, { ...WINDOWS, country: 'JP' }, '203.0.113.7', null);
  assert.deepEqual(dimension(env, 'country'), { Unknown: 1 });
});

test('analytics tables hold no names, accounts, addresses, Friend Codes or room codes', () => {
  const env = makeLobby();
  const a = env.connect('203.0.113.77', 'BR');
  const hello = a.hello({ ...WINDOWS, name: 'SecretAlice' });
  const { room } = a.call('room.create', ROOM);
  a.call('udp', { port: 40123 });
  const b = env.connect('198.51.100.66', 'US');
  b.hello({ ...WINDOWS, name: 'HiddenBob' });
  b.call('room.join', { code: room.code });
  env.advance(SAMPLE_MS);
  const dump = JSON.stringify(metricsRows(env));
  for (const secret of ['SecretAlice', 'HiddenBob', a.account, b.account, '203.0.113.77', '198.51.100.66', '40123',
    hello.friend_code, hello.friend_code.replace('-', ''), room.code, room.key, a.secret]) {
    assert.ok(!dump.includes(secret), `analytics contain ${secret}`);
  }
  assert.ok(dump.includes('"BR"') && dump.includes('Windows'));
});

test('no per-session or per-player analytics table exists', () => {
  const env = makeLobby();
  online(env);
  const tables = env.host.sql.all(`SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'`)
    .map((r) => r.name).sort();
  assert.deepEqual(tables, ['accounts', 'friend_requests', 'friends', 'members', 'meta', 'metrics_daily',
    'metrics_daily_dimensions', 'metrics_samples', 'registrations', 'reports', 'rooms']);
  const forbidden = /account|name|ip|port|code|friend|room_id|session_id|user|device|machine|hash|city|lat|lon|postal/;
  for (const table of tables.filter((t) => t.startsWith('metrics_'))) {
    for (const { name } of env.host.sql.all(`PRAGMA table_info(${table})`)) {
      assert.ok(!forbidden.test(name) || name === 'rooms_created' || name === 'room_joins' || name === 'peak_rooms' ||
        name === 'in_rooms' || name === 'peak_in_rooms', `${table}.${name}`);
    }
  }
});

test('samples are written per 15-minute interval when counts grow, not per message', () => {
  const env = makeLobby();
  let sampleWrites = 0;
  const run = env.host.sql.run;
  env.host.sql.run = (query, ...args) => {
    if (/INSERT INTO metrics_samples/.test(query)) sampleWrites++;
    return run(query, ...args);
  };
  const a = online(env);
  assert.equal(sampleWrites, 1);
  for (let i = 0; i < 40; i++) {
    a.call('presence.set', { data: 'ab'.repeat(4) });
    a.call('room.search', { world: 1 });
    a.call('worlds');
  }
  assert.equal(sampleWrites, 1);
  const extra = online(env, WINDOWS, '198.51.100.4');
  assert.equal(sampleWrites, 2);
  extra.drop();
  online(env, WINDOWS, '198.51.100.5');
  assert.equal(sampleWrites, 2);
  const bucket = sampleBucket(env.clock.now);
  env.clock.now = bucket + SAMPLE_MS - 1;
  assert.equal(env.host.alarmAt, bucket + SAMPLE_MS);
  env.advance(1);
  assert.equal(sampleWrites, 3);
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM metrics_samples').n, 2);
  assert.equal(env.host.alarmAt, bucket + 2 * SAMPLE_MS);
});

test('sample retention keeps 90 days and deletes in bounded batches', () => {
  const env = makeLobby();
  const day = 24 * 3600 * 1000;
  const old = sampleBucket(env.clock.now - 91 * day);
  for (let i = 0; i < 1200; i++) {
    env.host.sql.run('INSERT INTO metrics_samples (bucket, online, in_rooms, rooms) VALUES (?, 1, 0, 0)', old - i * SAMPLE_MS);
  }
  const recent = sampleBucket(env.clock.now - 89 * day);
  env.host.sql.run('INSERT INTO metrics_samples (bucket, online, in_rooms, rooms) VALUES (?, 3, 1, 1)', recent);
  const count = () => env.host.sql.get('SELECT COUNT(*) AS n FROM metrics_samples WHERE bucket < ?', recent).n;
  online(env);
  assert.equal(count(), 700);
  env.advance(SAMPLE_MS);
  assert.equal(count(), 200);
  env.advance(SAMPLE_MS);
  assert.equal(count(), 0);
  assert.ok(env.host.sql.get('SELECT 1 AS x FROM metrics_samples WHERE bucket = ?', recent));
});

test('schema creation is idempotent and keeps existing lobby data', () => {
  const sql = sqliteStore();
  const host = new LocalHost(sql, { now: () => 1_700_000_000_000, log: (text) => { throw new Error(text); } });
  new Lobby(host);
  sql.run(`INSERT INTO accounts (account, secret_hash, code, name, created, seen) VALUES ('42', 'h', 'ABCDEFGH', 'Kept', 1, 1)`);
  sql.run(`INSERT INTO metrics_daily (day, sessions) VALUES ('2026-10-01', 7)`);
  new Lobby(host);
  new Lobby(host);
  assert.equal(sql.get('SELECT name FROM accounts WHERE account = ?', '42').name, 'Kept');
  assert.equal(sql.get(`SELECT sessions FROM metrics_daily WHERE day = '2026-10-01'`).sessions, 7);
});

// ---- the dashboard ----
const TOKEN = 'test-dashboard-token-0123456789abcdef';
const ENV = { DCP_DASHBOARD_TOKEN: TOKEN };

function fakeMetrics() {
  const calls = [];
  return {
    calls,
    summary: async () => { calls.push(['summary']); return { live: { online: 3 } }; },
    history: async (days) => { calls.push(['history', days]); return { days }; },
  };
}

const get = (path, headers = {}) => new Request(`https://dcp-lobby.example${path}`, { headers });
const bearer = (token) => ({ Authorization: `Bearer ${token}` });

async function everything(response) {
  return `${response.status} ${JSON.stringify([...response.headers])} ${await response.clone().text()}`;
}

test('the metrics API refuses a missing or wrong token before reading anything', async () => {
  for (const headers of [{}, bearer('wrong-token-wrong-token-wrong'), bearer(`${TOKEN}x`), bearer(TOKEN.slice(0, -1)),
    { Authorization: TOKEN }, { Authorization: `Basic ${TOKEN}` }]) {
    const metrics = fakeMetrics();
    for (const path of ['/admin/api/summary', '/admin/api/history?days=30']) {
      const response = await handleAdmin(get(path, headers), ENV, metrics);
      assert.equal(response.status, 401);
      assert.equal(response.headers.get('Cache-Control'), 'no-store');
    }
    assert.deepEqual(metrics.calls, []);
  }
  const unset = fakeMetrics();
  assert.equal((await handleAdmin(get('/admin/api/summary', bearer('')), {}, unset)).status, 503);
  assert.equal((await handleAdmin(get('/admin/api/summary', bearer('short')), { DCP_DASHBOARD_TOKEN: 'short' }, unset)).status, 503);
  assert.deepEqual(unset.calls, []);
  assert.equal(tokenMatches(`Bearer ${TOKEN}`, TOKEN), true);
  assert.equal(tokenMatches('Bearer ', ''), false);
});

test('the right token reads the summary and a bounded history; the token never comes back', async () => {
  const metrics = fakeMetrics();
  const logs = [];
  const log = (text) => logs.push(text);
  const summary = await handleAdmin(get('/admin/api/summary', bearer(TOKEN)), ENV, metrics, log);
  assert.equal(summary.status, 200);
  assert.deepEqual(await summary.clone().json(), { live: { online: 3 } });
  const responses = [summary];
  for (const [query, status, days] of [['', 200, 30], ['?days=1', 200, 1], ['?days=365', 200, 365], ['?days=366', 400],
    ['?days=0', 400], ['?days=-5', 400], ['?days=1e3', 400], ['?days=7.5', 400], ['?days=30%3B%20DROP%20TABLE%20accounts', 400],
    ['?days=SELECT%20*%20FROM%20accounts', 400]]) {
    const response = await handleAdmin(get(`/admin/api/history${query}`, bearer(TOKEN)), ENV, metrics, log);
    assert.equal(response.status, status, query);
    if (days) assert.deepEqual(await response.clone().json(), { days });
    responses.push(response);
  }
  assert.deepEqual(metrics.calls.filter((c) => c[0] === 'history').map((c) => c[1]), [30, 1, 365]);
  for (const path of ['/admin/api/query?sql=SELECT%201', '/admin/api/sql', '/admin/api/accounts', '/admin/api/summary/../reports']) {
    const response = await handleAdmin(get(path, bearer(TOKEN)), ENV, metrics, log);
    assert.notEqual(response.status, 200, path);
    responses.push(response);
  }
  const failing = { summary: async () => { throw new Error('lobby answered 500'); }, history: async () => ({}) };
  const broken = await handleAdmin(get('/admin/api/summary', bearer(TOKEN)), ENV, failing, log);
  assert.equal(broken.status, 502);
  responses.push(broken);
  const post = await handleAdmin(new Request('https://dcp-lobby.example/admin/api/summary', { method: 'POST',
    headers: bearer(TOKEN), body: 'SELECT 1' }), ENV, metrics, log);
  assert.equal(post.status, 405);
  responses.push(post);
  for (const response of responses) {
    const text = await everything(response);
    assert.ok(!text.includes(TOKEN), 'token in a response');
    assert.equal(response.headers.get('Access-Control-Allow-Origin'), null);
  }
  assert.ok(logs.length > 0 && !logs.join('\n').includes(TOKEN));
});

test('the dashboard and API are no-store, same-origin and script-free beyond their own file', async () => {
  for (const [path, type] of [['/admin', 'text/html'], ['/admin/app.js', 'text/javascript'], ['/admin/app.css', 'text/css']]) {
    const response = await handleAdmin(get(path), ENV, fakeMetrics());
    assert.equal(response.status, 200);
    assert.ok(response.headers.get('Content-Type').startsWith(type));
    assert.equal(response.headers.get('Cache-Control'), 'no-store');
    assert.equal(response.headers.get('X-Content-Type-Options'), 'nosniff');
    assert.equal(response.headers.get('Referrer-Policy'), 'no-referrer');
    const csp = response.headers.get('Content-Security-Policy');
    assert.match(csp, /default-src 'none'/);
    assert.match(csp, /script-src 'self';/);
    assert.match(csp, /frame-ancestors 'none'/);
    assert.ok(!/unsafe-inline|unsafe-eval|https?:/.test(csp));
    assert.ok(!(await response.text()).includes(TOKEN));
  }
  assert.ok(!/<script>|<style>| style=| on[a-z]+=/i.test(ADMIN_HTML));
  for (const text of [ADMIN_HTML, ADMIN_CSS, ADMIN_JS]) {
    assert.ok(!/(src|href)=["']?https?:|@import|url\(|googleapis|gtag|analytics/i.test(text));
  }
  assert.ok(!/\beval\(|new Function|localStorage|document\.cookie|innerHTML|[?&]token=/.test(ADMIN_JS));
  assert.match(ADMIN_JS, /'Authorization': |Authorization: 'Bearer ' \+ token/);
  assert.doesNotThrow(() => new Function(ADMIN_JS)); // parses; never run
});

test('the Durable Object serves metrics only to the Worker, never to a WebSocket upgrade', () => {
  const route = (path, headers = {}) => internalMetricsRoute(get(path, headers));
  assert.deepEqual(route('/metrics/summary'), { kind: 'summary' });
  assert.deepEqual(route('/metrics/history?days=90'), { kind: 'history', days: 90 });
  assert.deepEqual(route('/metrics/history?days=9999'), { kind: 'bad_request' });
  assert.equal(route('/metrics/summary', { Upgrade: 'websocket' }), null);
  assert.equal(route('/'), null);
  const worker = readFileSync(join(REPO, 'server/lobby/src/worker.js'), 'utf8');
  assert.ok(worker.indexOf('isAdminPath(url.pathname)') < worker.indexOf("=== 'websocket') return stub.fetch(withEdgeCountry(request))"));
  assert.ok(!/console\.log\([^)]*(token|Authorization)/i.test(worker));
});

test('game clients send no telemetry: same sign-in fields, no new requests, P2P untouched', () => {
  const lobbyLink = readFileSync(join(REPO, 'src/net/lobby_link.cpp'), 'utf8');
  const hello = lobbyLink.slice(lobbyLink.indexOf('Json hello_body('), lobbyLink.indexOf('LobbyLink::LobbyLink('));
  assert.deepEqual([...hello.matchAll(/h\.set\("([a-z_]+)"/g)].map((m) => m[1]),
    ['proto', 'game', 'account', 'secret', 'name', 'presence', 'online_protocol', 'version', 'platform']);
  const files = [
    ...readdirSync(join(REPO, 'src/net')).map((f) => join('src/net', f)),
    ...readdirSync(join(REPO, 'src')).filter((f) => /^runtime_(online|matching2|rudp)/.test(f)).map((f) => join('src', f)),
    ...readdirSync(join(REPO, 'launcher')).filter((f) => /^online_/.test(f)).map((f) => join('launcher', f)),
  ];
  assert.ok(files.length > 10);
  for (const file of files) {
    const text = readFileSync(join(REPO, file), 'utf8');
    assert.ok(!/telemetry|analytics|\/admin|metrics/i.test(text), file);
  }
  const env = makeLobby();
  const c = online(env);
  for (const op of ['metrics', 'metrics.report', 'telemetry', 'stats']) assert.equal(c.call(op).code, 'bad_request');
});
