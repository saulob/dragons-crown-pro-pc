import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';

import { PROTECTION } from '../src/abuse.js';
import { COMMS_KEYS, COMMS_MAX_PER_ID, COMMS_NAMES, commsSummary, parseCommsReport } from '../src/comms_usage.js';
import { Lobby } from '../src/lobby.js';
import { SAMPLE_MS, metricsDay } from '../src/metrics.js';
import { LocalHost, sqliteStore } from '../local/host.js';
import { Client, makeLobby } from './harness.js';

const GAME = { online_protocol: 1, version: '1.4.0', platform: 'Windows' };

function greeted(env, address = '203.0.113.7') {
  const client = env.connect(address);
  assert.equal(client.hello(GAME).code, 'ok');
  return client;
}

const counts = (...pairs) => {
  const out = Array(8).fill(0);
  for (const [id, n] of pairs) out[id] = n;
  return out;
};
const report = (client, c) => client.call('comms.report', { counts: c });
const summary = (env) => env.lobby.metricsSummary().comms;
const parse = (body) => {
  const msg = { op: 'comms.report', seq: 1, ...body };
  return parseCommsReport(msg, JSON.stringify(msg));
};

test('the eight stable ids are the telemetry keys, in order', () => {
  assert.deepEqual(COMMS_KEYS, ['hello', 'thanks', 'good_job', 'follow_me', 'group_up', 'need_help', 'ready', 'lets_go']);
  assert.deepEqual(COMMS_NAMES, ['Hello', 'Thanks', 'Good job', 'Follow me', 'Group up', 'Need help', 'Ready', "Let's go"]);
});

test('reports add to the total and to each id', () => {
  const env = makeLobby();
  const a = greeted(env);
  assert.equal(report(a, counts([5, 3], [0, 1])).code, 'ok');
  assert.equal(report(greeted(env, '203.0.113.8'), counts([5, 2], [7, 4])).code, 'ok');
  const comms = summary(env);
  assert.equal(comms.total, 10);
  assert.deepEqual(comms.messages.map((m) => m.uses), [1, 0, 0, 0, 0, 5, 0, 4]);
  assert.equal(comms.messages[5].key, 'need_help');
  assert.equal(comms.most_used, 'need_help');
});

test('percentages and the most used message; the lowest id wins a tie; nothing is used at first', () => {
  const empty = commsSummary(Array(8).fill(0));
  assert.equal(empty.total, 0);
  assert.equal(empty.most_used, null);
  assert.ok(empty.messages.every((m) => m.percent === 0));
  const tie = commsSummary(counts([6, 4], [2, 4], [3, 2]));
  assert.equal(tie.most_used, 'good_job');
  assert.deepEqual(tie.messages.map((m) => m.percent), [0, 0, 40, 20, 0, 0, 40, 0]);
  const example = commsSummary(counts([5, 1684], [1, 940], [0, 2696]));
  assert.equal(example.total, 5320);
  assert.equal(example.messages[5].percent, 31.7);
  assert.equal(example.messages[1].percent, 17.7);
  assert.equal(example.most_used, 'hello');
});

test('malformed reports are refused and count nothing', () => {
  const env = makeLobby();
  const a = greeted(env);
  const bad = [
    {}, { counts: 1 }, { counts: [] }, { counts: Array(7).fill(1) }, { counts: Array(9).fill(1) },
    { counts: Array(8).fill(0) }, { counts: counts([0, -1]) }, { counts: counts([0, 1.5]) }, { counts: counts([0, '1']) },
    { counts: counts([0, null]) }, { counts: counts([0, true]) }, { counts: counts([0, 1e300]) },
    { counts: counts([0, Number.MAX_SAFE_INTEGER + 2]) }, { counts: [[1], 0, 0, 0, 0, 0, 0, 0] },
    { counts: { 0: 1 } }, { counts: counts([0, 1]), text: 'Need help' }, { counts: counts([0, 1]), message: 5 },
    { counts: counts([0, 1]), name: 'Alice' }, { id: 5 },
  ];
  for (const body of bad) assert.equal(parse(body), null, JSON.stringify(body));
  for (const body of bad.slice(0, 6)) assert.equal(a.call('comms.report', body).code, 'bad_request');
  assert.equal(a.call('comms.report', { counts: counts([0, 1]), text: 'hello' }).code, 'bad_request');
  assert.equal(summary(env).total, 0);
});

test('a repeated key is refused even though JSON itself would take the last one', () => {
  const env = makeLobby();
  const a = greeted(env);
  for (const text of ['{"op":"comms.report","seq":5,"counts":[1,0,0,0,0,0,0,0],"counts":[0,0,0,0,0,0,0,9]}',
    '{"op":"comms.report","seq":5,"seq":6,"counts":[1,0,0,0,0,0,0,0]}']) {
    assert.equal(a.raw(text).code, 'bad_request', text);
  }
  assert.equal(a.raw('{"op":"comms.report","seq":5,"counts":[1,0,0,0,0,0,0,0]}').code, 'ok');
  assert.equal(summary(env).total, 1);
});

test('one report adds at most the clamp to an id', () => {
  const env = makeLobby();
  const a = greeted(env);
  assert.equal(report(a, counts([2, 10_000], [3, COMMS_MAX_PER_ID])).code, 'ok');
  assert.deepEqual(summary(env).messages.map((m) => m.uses), [0, 0, 50, 50, 0, 0, 0, 0]);
});

test('a report needs a signed-in account', () => {
  const env = makeLobby();
  const stranger = env.connect('203.0.113.50');
  assert.equal(report(stranger, counts([0, 1])).code, 'no_session');
  assert.equal(summary(env).total, 0);
});

test('one account is limited, the refusal is counted and written only by the alarm', () => {
  const env = makeLobby();
  const a = greeted(env);
  for (let i = 0; i < PROTECTION.commsReportAccount.burst; i++) assert.equal(report(a, counts([i, 1])).code, 'ok');
  const writes = env.host.sql.run;
  const sets = [];
  env.host.sql.run = (query, ...args) => { sets.push(query); return writes.call(env.host.sql, query, ...args); };
  for (let i = 0; i < 10; i++) assert.equal(report(a, counts([0, 1])).code, 'limit');
  assert.equal(sets.filter((q) => /comms_usage|metrics_daily_dimensions/.test(q)).length, 0);
  env.host.sql.run = writes;
  assert.equal(summary(env).total, 3);
  env.advance(SAMPLE_MS);
  const abuse = Object.fromEntries(env.host.sql.all(`SELECT value, count FROM metrics_daily_dimensions
    WHERE day = ? AND dimension = 'abuse'`, metricsDay(env.clock.now)).map((r) => [r.value, r.count]));
  assert.equal(abuse.comms_report_limited, 10);
  env.advance(120 * 1000);
  assert.equal(report(a, counts([0, 1])).code, 'ok');
});

test('one address is limited whatever the accounts behind it', () => {
  const env = makeLobby();
  const codes = [];
  for (let i = 0; i < PROTECTION.commsReportAddress.burst + 3; i++) {
    codes.push(report(greeted(env, '203.0.113.9'), counts([0, 1])).code);
  }
  assert.equal(codes.filter((c) => c === 'ok').length, PROTECTION.commsReportAddress.burst);
  assert.deepEqual([...new Set(codes.slice(PROTECTION.commsReportAddress.burst))], ['limit']);
});

test('the whole lobby is limited whatever the addresses and accounts', () => {
  const env = makeLobby();
  const codes = [];
  for (let i = 0; i < PROTECTION.commsReportAll.burst + 3; i++) {
    codes.push(report(greeted(env, `198.51.100.${i + 1}`), counts([0, 1])).code);
  }
  assert.equal(codes.filter((c) => c === 'ok').length, PROTECTION.commsReportAll.burst);
});

test('counts survive a restart and only message ids are stored', () => {
  const dir = mkdtempSync(join(tmpdir(), 'dcp-comms-'));
  const file = join(dir, 'lobby.sqlite');
  const clock = { now: 1_700_000_000_000 };
  const open = () => {
    const host = new LocalHost(sqliteStore(file), { now: () => clock.now, log: (text) => { throw new Error(text); } });
    return { host, lobby: new Lobby(host, { local: true }) };
  };
  try {
    const first = open();
    const client = new Client(first.lobby, first.host, '127.0.0.1');
    assert.equal(client.hello({ name: 'Alice', ...GAME }).code, 'ok');
    assert.equal(report(client, counts([4, 7], [1, 2])).code, 'ok');
    const since = first.lobby.metricsSummary().comms.since;
    first.host.sql.close();
    const second = open();
    const comms = second.lobby.metricsSummary().comms;
    assert.equal(comms.total, 9);
    assert.equal(comms.most_used, 'group_up');
    assert.equal(comms.since, since);
    const rows = second.host.sql.all('SELECT * FROM comms_usage ORDER BY message').map((r) => ({ ...r }));
    assert.deepEqual(rows, [{ message: 1, uses: 2 }, { message: 4, uses: 7 }]);
    const columns = second.host.sql.all('PRAGMA table_info(comms_usage)').map((c) => c.name);
    assert.deepEqual(columns, ['message', 'uses']);
    second.host.sql.close();
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('the summary has a fixed shape and nothing but counts is stored', () => {
  const env = makeLobby();
  const a = greeted(env);
  assert.equal(report(a, counts([0, 1])).code, 'ok');
  const comms = summary(env);
  assert.deepEqual(Object.keys(comms), ['since', 'total', 'most_used', 'messages']);
  assert.deepEqual(Object.keys(comms.messages[0]), ['id', 'key', 'name', 'uses', 'percent']);
  assert.equal(comms.messages.length, 8);
  const tables = env.host.sql.all("SELECT name FROM sqlite_master WHERE type = 'table' AND name LIKE '%comms%'")
    .map((t) => t.name);
  assert.deepEqual(tables, ['comms_usage']);
});

test('a local player reporting Hello once, Need help twice and Ready once changes exactly those counters', () => {
  const env = makeLobby();
  const zero = summary(env);
  assert.equal(zero.total, 0);
  assert.equal(zero.most_used, null);
  const client = greeted(env);
  assert.equal(report(client, counts([0, 1], [5, 2], [6, 1])).code, 'ok');
  const after = summary(env);
  assert.equal(after.total, 4);
  assert.deepEqual(after.messages.map((m) => m.uses), [1, 0, 0, 0, 0, 2, 1, 0]);
  assert.equal(after.most_used, 'need_help');
});
