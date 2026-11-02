import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';

import { handleAdmin, internalMetricsRoute } from '../src/admin.js';
import { PROTECTION } from '../src/abuse.js';
import { SAMPLE_MS, metricsDay } from '../src/metrics.js';
import { MAX_PARTY, PARTY_CLASSES, PARTY_COMPOSITIONS, PARTY_DIMENSIONS, PARTY_ROWS_PER_DAY, PARTY_VALUES, parsePartyReport,
  partyCounts } from '../src/party.js';
import { ROOM, makeLobby } from './harness.js';

const REPO = join(import.meta.dirname, '..', '..', '..');
const GAME = { online_protocol: 1, version: '0.9.2', platform: 'Windows' };
const ip = (n) => `198.51.100.${n}`;

function hosting(env, address = '203.0.113.7', fields = GAME) {
  const client = env.connect(address);
  assert.equal(client.hello(fields).code, 'ok');
  const created = client.call('room.create', ROOM);
  assert.equal(created.code, 'ok');
  client.room = created.room;
  return client;
}

const report = (client, body) => client.call('party.report', body);
const dimension = (env, name, day = metricsDay(env.clock.now)) => Object.fromEntries(env.host.sql.all(
  'SELECT value, count FROM metrics_daily_dimensions WHERE day = ? AND dimension = ?', day, name).map((r) => [r.value, r.count]));
const partyRows = (env) => env.host.sql.all(`SELECT * FROM metrics_daily_dimensions WHERE dimension IN
  (${PARTY_DIMENSIONS.map(() => '?').join(', ')})`, ...PARTY_DIMENSIONS);
const parse = (body) => {
  const msg = { op: 'party.report', seq: 1, ...body };
  return parsePartyReport(msg, JSON.stringify(msg));
};

test('the six classes are the game class indexes and the only valid values', () => {
  assert.deepEqual(PARTY_CLASSES, ['Fighter', 'Amazon', 'Elf', 'Dwarf', 'Sorceress', 'Wizard']);
  assert.deepEqual([...PARTY_VALUES.character], PARTY_CLASSES);
  for (let c = 0; c < PARTY_CLASSES.length; c++) {
    assert.deepEqual(parse({ classes: [c] }), { classes: [c], host: null }, `class ${c}`);
    assert.deepEqual(parse({ classes: [c], host: c }), { classes: [c], host: c });
  }
});

test('unknown classes, sizes outside 1 to 4 and malformed fields are refused', () => {
  const bad = [
    { classes: [6] }, { classes: [-1] }, { classes: [1.5] }, { classes: ['1'] }, { classes: ['Fighter'] }, { classes: [null] },
    { classes: [true] }, { classes: [[0]] }, { classes: [{}] }, { classes: [] }, { classes: [0, 0, 0, 0, 0] },
    { classes: Array(5000).fill(0) }, { classes: 0 }, { classes: '0' }, { classes: { 0: 0 } }, {},
    { classes: [0], host: 1 }, { classes: [0], host: 6 }, { classes: [0], host: -1 }, { classes: [0], host: null },
    { classes: [0], host: '0' }, { classes: [0], host: [0] }, { classes: [0], size: 1 }, { classes: [0], name: 'Alice' },
    { classes: [0], extra: {} },
  ];
  for (const body of bad) assert.equal(parse(body), null, JSON.stringify(body).slice(0, 80));
  assert.equal(parsePartyReport({ op: 'party.report', seq: 1, classes: [0] }, undefined), null);
});

test('a repeated key is refused even though JSON itself would take the last one', () => {
  const env = makeLobby();
  const a = hosting(env);
  for (const text of ['{"op":"party.report","seq":5,"classes":[0],"classes":[1]}',
    '{"op":"party.report","seq":5,"classes":[0],"host":0,"host":0}',
    '{"op":"party.report","seq":5,"seq":6,"classes":[0]}',
    '{"op":"party.report","seq":5,"classes":[0],"\\u0063lasses":[1]}']) {
    assert.equal(a.raw(text).code, 'bad_request', text);
  }
  assert.equal(a.raw('{"op":"party.report","seq":5,"classes":[0]}').code, 'ok');
  assert.deepEqual(dimension(env, 'character'), { Fighter: 1 });
  assert.deepEqual(dimension(env, 'party_size'), { 1: 1 });
});

test('a snapshot carries up to four classes, repeats allowed, ordering irrelevant', () => {
  assert.equal(MAX_PARTY, 4);
  assert.deepEqual(parse({ classes: [5, 0, 0, 3] }).classes, [0, 0, 3, 5]);
  assert.deepEqual(parse({ classes: [3, 0, 5, 0] }).classes, [0, 0, 3, 5]);
  assert.equal(parse({ classes: [0, 0, 0, 0] }).classes.length, 4);
  assert.equal(parse({ classes: [2, 2], host: 2 }).host, 2);
});

test('compositions are canonical: order never splits a party, repeated classes stay distinct', () => {
  const composition = (classes) => partyCounts({ classes, host: null }).find((r) => r[0] === 'party_composition')[1];
  assert.equal(composition([2, 0]), 'Fighter+Elf');
  assert.equal(composition([0, 2]), 'Fighter+Elf');
  assert.equal(composition([2, 0, 0]), 'Fighter+Fighter+Elf');
  assert.notEqual(composition([0, 0, 2]), composition([0, 2]));
  assert.equal(composition([5, 4, 2, 0]), 'Fighter+Elf+Sorceress+Wizard');
  assert.equal(composition([1, 4]), 'Amazon+Sorceress');
});

test('the composition space is finite: 6 + 21 + 56 + 126 values, every one reachable and nothing else', () => {
  assert.equal(PARTY_COMPOSITIONS.length, 209);
  assert.equal(new Set(PARTY_COMPOSITIONS).size, 209);
  assert.equal(PARTY_ROWS_PER_DAY, 6 + 6 + 4 + 209 + 2);
  const seen = new Set();
  const walk = (classes, from) => {
    if (classes.length) seen.add(partyCounts({ classes, host: null }).find((r) => r[0] === 'party_composition')[1]);
    if (classes.length === MAX_PARTY) return;
    for (let c = from; c < PARTY_CLASSES.length; c++) walk([...classes, c], c);
  };
  walk([], 0);
  assert.deepEqual([...seen].sort(), [...PARTY_COMPOSITIONS].sort());
});

test('counts are derived from class indexes only: characters, host, size, composition and repeats', () => {
  const rows = partyCounts({ classes: [3, 0, 0], host: 3 });
  assert.deepEqual(rows, [['character', 'Fighter', 2], ['character', 'Dwarf', 1], ['party_size', '3', 1],
    ['party_composition', 'Fighter+Fighter+Dwarf', 1], ['party_duplicate', 'yes', 1], ['host_character', 'Dwarf', 1]]);
  assert.deepEqual(partyCounts({ classes: [0, 2], host: null }).map((r) => r[0]),
    ['character', 'character', 'party_size', 'party_composition', 'party_duplicate']);
  for (const bad of [{ classes: ['Fighter'], host: null }, { classes: [0], host: 'Fighter' }, { classes: [0], host: 1 },
    { classes: [], host: null }, { classes: [0, 1, 2, 3, 4], host: null }, { classes: [7], host: null }]) {
    assert.throws(() => partyCounts(bad), RangeError, JSON.stringify(bad));
  }
});

test('the host reports a snapshot and every figure lands in the daily dimensions', () => {
  const env = makeLobby();
  const a = hosting(env);
  const reply = report(a, { classes: [3, 0, 0], host: 3 });
  assert.equal(reply.code, 'ok');
  assert.equal(reply.counted, true);
  assert.deepEqual(dimension(env, 'character'), { Fighter: 2, Dwarf: 1 });
  assert.deepEqual(dimension(env, 'host_character'), { Dwarf: 1 });
  assert.deepEqual(dimension(env, 'party_size'), { 3: 1 });
  assert.deepEqual(dimension(env, 'party_composition'), { 'Fighter+Fighter+Dwarf': 1 });
  assert.deepEqual(dimension(env, 'party_duplicate'), { yes: 1 });
  const b = hosting(env, ip(1));
  assert.equal(report(b, { classes: [2, 0] }).code, 'ok');
  assert.deepEqual(dimension(env, 'party_composition'), { 'Fighter+Fighter+Dwarf': 1, 'Fighter+Elf': 1 });
  assert.deepEqual(dimension(env, 'party_duplicate'), { yes: 1, no: 1 });
  assert.deepEqual(dimension(env, 'host_character'), { Dwarf: 1 });
  assert.deepEqual(dimension(env, 'party_size'), { 2: 1, 3: 1 });
});

test('every valid class is accepted, as a host and as part of a party', () => {
  const env = makeLobby();
  for (let c = 0; c < PARTY_CLASSES.length; c++) {
    const client = hosting(env, ip(c + 1));
    assert.equal(report(client, { classes: [c, (c + 1) % 6], host: c }).code, 'ok', `class ${c}`);
  }
  assert.deepEqual(Object.keys(dimension(env, 'host_character')).sort(), [...PARTY_CLASSES].sort());
  assert.ok(Object.values(dimension(env, 'character')).every((n) => n === 2));
});

test('a refused request writes nothing', () => {
  const env = makeLobby();
  const a = hosting(env);
  for (const body of [{ classes: [9] }, { classes: [] }, { classes: [0, 1, 2, 3, 4] }, { classes: [0], host: 4 },
    { classes: [0], name: 'x' }, {}]) {
    assert.equal(report(a, body).code, 'bad_request');
  }
  assert.equal(partyRows(env).length, 0);
});

test('a frame over the size limit is refused before it is read', () => {
  const env = makeLobby();
  const a = hosting(env);
  const reply = a.raw(JSON.stringify({ op: 'party.report', seq: 3, classes: [0], pad: 'x'.repeat(40_000) }));
  assert.equal(reply.code, 'too_large');
  assert.equal(partyRows(env).length, 0);
});

test('nothing is accepted before sign-in, from a connection outside a room, a guest or a test connection', () => {
  const env = makeLobby();
  assert.equal(report(env.connect(ip(1)), { classes: [0] }).code, 'no_session');

  const lonely = env.connect(ip(2));
  lonely.hello(GAME);
  assert.equal(report(lonely, { classes: [0] }).code, 'not_found');

  const probe = env.connect(ip(3));
  probe.hello({ ...GAME, presence: false });
  assert.equal(probe.call('room.create', ROOM).code, 'ok');
  assert.equal(report(probe, { classes: [0] }).code, 'not_found');

  const owner = hosting(env, ip(4));
  const guest = env.connect(ip(5));
  guest.hello(GAME);
  assert.equal(guest.call('room.join', { code: owner.room.code }).code, 'ok');
  assert.equal(report(guest, { classes: [0] }).code, 'not_owner');
  assert.equal(partyRows(env).length, 0);
  assert.equal(report(owner, { classes: [0, 1] }).code, 'ok');
});

test('a party cannot be larger than its room', () => {
  const env = makeLobby();
  const a = env.connect();
  a.hello(GAME);
  a.call('room.create', { ...ROOM, slots: 2 });
  assert.equal(report(a, { classes: [0, 1, 2] }).code, 'bad_request');
  assert.equal(report(a, { classes: [0, 1] }).code, 'ok');
});

test('the same snapshot from the same connection and room is counted once and spends no budget', () => {
  const env = makeLobby();
  const a = hosting(env);
  assert.equal(report(a, { classes: [0, 2], host: 0 }).counted, true);
  const tokens = env.lobby.guards.partyReportAccount.tokens(a.account, env.clock.now);
  for (let i = 0; i < 50; i++) assert.equal(report(a, { classes: [2, 0], host: 0 }).counted, false);
  assert.equal(env.lobby.guards.partyReportAccount.tokens(a.account, env.clock.now), tokens);
  assert.deepEqual(dimension(env, 'party_size'), { 2: 1 });
  assert.equal(report(a, { classes: [0, 2], host: 2 }).counted, true);
  assert.deepEqual(dimension(env, 'party_size'), { 2: 2 });
});

test('a reconnect may be counted again: a small duplicate, with no identity kept to prevent it', () => {
  const env = makeLobby();
  const a = hosting(env);
  report(a, { classes: [0, 2] });
  const again = env.connect();
  Object.assign(again, { account: a.account, secret: a.secret });
  again.hello(GAME);
  assert.equal(again.call('room.resume', { room: a.room.id, member: 0x10 }).code, 'ok');
  assert.equal(report(again, { classes: [0, 2] }).counted, true);
  assert.deepEqual(dimension(env, 'party_composition'), { 'Fighter+Elf': 2 });
});

test('one account is limited, the refusal is counted in memory and written only by the alarm', () => {
  const env = makeLobby();
  const a = hosting(env);
  for (let c = 0; c < 3; c++) assert.equal(report(a, { classes: [c] }).code, 'ok', `report ${c}`);
  const rows = partyRows(env).length;
  const sets = [];
  const writes = env.host.sql.run;
  env.host.sql.run = (query, ...args) => { sets.push(query); return writes.call(env.host.sql, query, ...args); };
  for (let i = 0; i < 20; i++) {
    const reply = report(a, { classes: [3 + (i % 3)] });
    assert.equal(reply.code, 'limit', `report ${i}`);
  }
  assert.equal(partyRows(env).length, rows);
  assert.equal(sets.filter((q) => /metrics_daily_dimensions/.test(q)).length, 0);
  env.host.sql.run = writes;
  env.advance(SAMPLE_MS);
  assert.deepEqual(dimension(env, 'abuse').party_report_limited, 20);
  const tokens = 1 / PROTECTION.partyReportAccount.perSecond;
  env.advance(tokens * 1000);
  assert.equal(report(a, { classes: [4] }).code, 'ok');
});

test('one address is limited whatever the accounts behind it', () => {
  const env = makeLobby();
  const results = [];
  for (let i = 0; i < PROTECTION.partyReportAddress.burst + 3; i++) {
    const client = hosting(env, '203.0.113.9');
    results.push(report(client, { classes: [i % 6] }).code);
  }
  assert.deepEqual(results.filter((c) => c === 'ok').length, PROTECTION.partyReportAddress.burst);
  assert.deepEqual([...new Set(results.slice(PROTECTION.partyReportAddress.burst))], ['limit']);
});

test('the lobby as a whole is limited, however many addresses and accounts report', () => {
  const env = makeLobby();
  const results = [];
  for (let i = 0; i < PROTECTION.partyReportAll.burst + 5; i++) {
    const client = hosting(env, `203.0.${Math.floor(i / 200)}.${(i % 200) + 1}`);
    results.push(report(client, { classes: [i % 6] }).code);
  }
  assert.equal(results.filter((c) => c === 'ok').length, PROTECTION.partyReportAll.burst);
  assert.ok(partyRows(env).length <= PARTY_ROWS_PER_DAY);
});

test('party reports use no room.create, room.join, sign-in or relay budget', () => {
  const env = makeLobby();
  const a = hosting(env);
  const before = ['hello', 'roomCreateAccount', 'roomCreateAddress', 'roomJoinAccount'].map((g) => [g, env.lobby.guards[g].buckets.size]);
  const room = env.lobby.guards.roomCreateAccount.tokens(a.account, env.clock.now);
  const join = env.lobby.guards.roomJoinAccount.tokens(a.account, env.clock.now);
  for (let i = 0; i < 10; i++) report(a, { classes: [i % 6] });
  assert.equal(env.lobby.guards.roomCreateAccount.tokens(a.account, env.clock.now), room);
  assert.equal(env.lobby.guards.roomJoinAccount.tokens(a.account, env.clock.now), join);
  assert.deepEqual(before.map(([g]) => [g, env.lobby.guards[g].buckets.size]), before);
  assert.equal(a.call('room.leave', { room: a.room.id }).code, 'ok');
  for (let i = 0; i < PROTECTION.roomCreateAccount.burst - 1; i++) {
    const created = a.call('room.create', ROOM);
    assert.equal(created.code, 'ok', `room ${i}`);
    assert.equal(a.call('room.leave', { room: created.room.id }).code, 'ok');
  }
  assert.equal(a.call('room.create', ROOM).code, 'limit');
});

test('the per-socket message budget still applies to the new operation', () => {
  const env = makeLobby();
  const a = hosting(env);
  const codes = new Set();
  for (let i = 0; i < 100; i++) codes.add(report(a, { classes: [0] }).code);
  assert.ok(codes.has('ok'));
  assert.ok(codes.has('limit'));
});

test('a client that never sends party reports behaves exactly as before and adds no party rows', () => {
  const env = makeLobby();
  const a = hosting(env, ip(1), { online_protocol: 1, version: '0.9.1', platform: 'Windows' });
  const b = env.connect(ip(2));
  assert.equal(b.hello({ online_protocol: 1, version: '0.9.1', platform: 'Linux' }).code, 'ok');
  assert.equal(b.call('room.join', { code: a.room.code }).code, 'ok');
  assert.equal(a.call('room.leave', { room: a.room.id }).code, 'ok');
  assert.equal(b.call('room.leave', { room: a.room.id }).code, 'ok');
  assert.equal(partyRows(env).length, 0);
  const summary = env.lobby.metricsSummary();
  assert.equal(summary.today.sessions, 2);
  assert.equal(summary.today.rooms_created, 1);
  assert.equal(summary.today.room_joins, 1);
  const history = env.lobby.metricsHistory(30);
  for (const name of PARTY_DIMENSIONS) assert.deepEqual(history.dimensions[name], [], name);
  assert.equal(history.party_first_day, null);
});

test('the hello answer is unchanged: no capability announces party reports', () => {
  const env = makeLobby();
  const reply = env.connect().hello(GAME);
  assert.deepEqual(reply.capabilities, { relay_supported: true, relay_max_payload: 1200 });
  assert.deepEqual(Object.keys(reply).sort(), ['account', 'capabilities', 'code', 'friend_code', 'ip', 'name', 're', 'time']);
});

test('a metrics failure never fails the request', () => {
  const env = makeLobby({ log: () => {} });
  const a = hosting(env);
  env.host.sql.run('DROP TABLE metrics_daily_dimensions');
  assert.equal(report(a, { classes: [0] }).code, 'ok');
  assert.equal(a.call('room.leave', { room: a.room.id }).code, 'ok');
});

test('spam cannot grow the table beyond the finite party space', () => {
  const env = makeLobby();
  for (const composition of PARTY_COMPOSITIONS) {
    const classes = composition.split('+').map((name) => PARTY_CLASSES.indexOf(name));
    for (const host of [null, ...new Set(classes)]) env.lobby.metrics.partyReport({ classes, host });
  }
  for (let i = 0; i < 2000; i++) env.lobby.metrics.partyReport({ classes: [i % 6, (i * 7) % 6], host: null });
  const rows = partyRows(env);
  assert.ok(rows.length <= PARTY_ROWS_PER_DAY, String(rows.length));
  assert.equal(rows.length, PARTY_ROWS_PER_DAY);
  for (const { dimension: name, value } of rows) assert.ok(PARTY_VALUES[name].has(value), `${name}=${value}`);
  assert.throws(() => env.lobby.metrics.partyReport({ classes: ['Fighter; DROP TABLE x'], host: null }), RangeError);
  assert.throws(() => env.lobby.metrics.partyReport({ classes: [0], host: 'Elf' }), RangeError);
  assert.equal(partyRows(env).length, PARTY_ROWS_PER_DAY);
});

test('a client cannot name a dimension, a value or a composition', () => {
  const env = makeLobby();
  const a = hosting(env);
  for (const body of [{ classes: [0], dimension: 'platform' }, { classes: [0], value: 'x' }, { classes: [0], composition: 'Fighter+Elf' },
    { classes: ['Fighter+Elf'] }, { classes: [0], host: 'Fighter' }]) {
    assert.equal(report(a, body).code, 'bad_request', JSON.stringify(body));
  }
  assert.equal(partyRows(env).length, 0);
  assert.equal(env.host.sql.all(`SELECT 1 FROM metrics_daily_dimensions WHERE dimension NOT IN
    ('platform','version','country','reject','abuse','occupancy','lifetime')`).length, 0);
});

test('history covers the selected range only and names the first day anything was reported', () => {
  const env = makeLobby();
  const first = metricsDay(env.clock.now);
  report(hosting(env), { classes: [0, 2] });
  env.advance(10 * 24 * 3600 * 1000);
  report(hosting(env, ip(1)), { classes: [1, 1, 3], host: 1 });
  const wide = env.lobby.metricsHistory(30);
  assert.equal(wide.party_first_day, first);
  assert.deepEqual(wide.dimensions.party_size.map((r) => r.value).sort(), ['2', '3']);
  assert.equal(wide.dimensions.character.find((r) => r.value === 'Amazon').count, 2);
  const narrow = env.lobby.metricsHistory(7);
  assert.equal(narrow.party_first_day, first);
  assert.deepEqual(narrow.dimensions.party_composition, [{ value: 'Amazon+Amazon+Dwarf', count: 1 }]);
  assert.deepEqual(narrow.dimensions.host_character, [{ value: 'Amazon', count: 1 }]);
  assert.deepEqual(narrow.dimensions.party_duplicate, [{ value: 'yes', count: 1 }]);
});

test('analytics hold no player, account, address, room or save information after party reports', () => {
  const env = makeLobby();
  const a = env.connect('203.0.113.77', 'BR');
  const hello = a.hello({ ...GAME, name: 'SecretAlice' });
  const { room } = a.call('room.create', ROOM);
  a.call('udp', { port: 40123 });
  const b = env.connect('198.51.100.66', 'US');
  b.hello({ ...GAME, name: 'HiddenBob' });
  b.call('room.join', { code: room.code });
  assert.equal(report(a, { classes: [0, 2], host: 0 }).code, 'ok');
  env.advance(SAMPLE_MS);
  const dump = JSON.stringify(env.host.sql.all('SELECT * FROM metrics_daily_dimensions'));
  for (const secret of ['SecretAlice', 'HiddenBob', a.account, b.account, '203.0.113.77', '198.51.100.66', '40123',
    hello.friend_code, hello.friend_code.replace('-', ''), room.code, room.key, a.secret, `"${room.id}"`]) {
    assert.ok(!dump.includes(secret), `analytics contain ${secret}`);
  }
  for (const table of ['metrics_daily', 'metrics_samples']) {
    assert.ok(!JSON.stringify(env.host.sql.all(`SELECT * FROM ${table}`)).includes(room.code), table);
  }
  const tables = env.host.sql.all(`SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'`)
    .map((r) => r.name).sort();
  assert.deepEqual(tables, ['accounts', 'friend_requests', 'friends', 'members', 'meta', 'metrics_daily',
    'metrics_daily_dimensions', 'metrics_samples', 'registrations', 'reports', 'rooms']);
  assert.deepEqual(env.host.sql.all('PRAGMA table_info(metrics_daily_dimensions)').map((c) => c.name),
    ['day', 'dimension', 'value', 'count']);
});

test('the signature that stops repeats lives in the connection only: no SQL column or table holds it', () => {
  const env = makeLobby();
  const a = hosting(env);
  report(a, { classes: [0, 1, 2], host: 0 });
  const columns = env.host.sql.all(`SELECT name FROM sqlite_master WHERE type = 'table'`).flatMap((t) =>
    env.host.sql.all(`PRAGMA table_info(${t.name})`).map((c) => c.name));
  assert.ok(!columns.some((c) => /class|character|signature|^party$/i.test(c)), columns.join());
  assert.ok(a.conn.attachment().party);
});

test('there is no HTTP route for telemetry and no second WebSocket: it rides the lobby connection', async () => {
  const worker = readFileSync(join(REPO, 'server/lobby/src/worker.js'), 'utf8');
  const admin = readFileSync(join(REPO, 'server/lobby/src/admin.js'), 'utf8');
  const edge = readFileSync(join(REPO, 'server/lobby/src/edge.js'), 'utf8');
  for (const [name, text] of [['worker.js', worker], ['admin.js', admin], ['edge.js', edge]]) {
    assert.ok(!/party|telemetry|character/i.test(text), name);
  }
  assert.equal([...worker.matchAll(/acceptWebSocket\(/g)].length, 1);
  assert.equal([...worker.matchAll(/new WebSocketPair\(/g)].length, 1);
  for (const path of ['/party', '/party/report', '/party.report', '/telemetry', '/metrics/party', '/admin/api/party']) {
    assert.equal(internalMetricsRoute(new Request(`https://x.example${path}`)), null, path);
  }
  const post = await handleAdmin(new Request('https://x.example/admin/api/party', { method: 'POST', body: '{}' }),
    { DCP_DASHBOARD_TOKEN: 'x'.repeat(32) }, {});
  assert.equal(post.status, 405);
  const get = await handleAdmin(new Request('https://x.example/admin/api/party', { headers: { Authorization: `Bearer ${'x'.repeat(32)}` } }),
    { DCP_DASHBOARD_TOKEN: 'x'.repeat(32) }, {});
  assert.equal(get.status, 404);
});

test('party.report is a message of the lobby socket: it needs the sign-in and is not a room operation', () => {
  const lobby = readFileSync(join(REPO, 'server/lobby/src/lobby.js'), 'utf8');
  const roomOps = lobby.slice(lobby.indexOf('const ROOM_OPS'), lobby.indexOf('// Operations after which'));
  assert.ok(!roomOps.includes('party'));
  const growing = lobby.slice(lobby.indexOf('const GROWING_OPS'), lobby.indexOf('export const FLAG_CLOSED'));
  assert.ok(!growing.includes('party'));
});
