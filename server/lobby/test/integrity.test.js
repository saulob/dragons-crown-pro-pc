import assert from 'node:assert/strict';
import { test } from 'node:test';

import { SAVE_INTEGRITY_MARKED, SAVE_INTEGRITY_UNKNOWN } from '../src/lobby.js';
import { integrityCategory } from '../src/metrics.js';
import { ROOM, makeLobby } from './harness.js';

const CURRENT = { online_protocol: 1, version: '1.5.1', platform: 'Windows' };
const accounts = (env) => env.host.sql.get('SELECT COUNT(*) AS n FROM accounts').n;

test('hello keeps integrity_flags 0 to 3 on the attachment and a missing field as unknown, not 0', () => {
  const env = makeLobby();
  for (const flags of [0, 1, 2, 3]) {
    const client = env.connect();
    const reply = client.hello({ ...CURRENT, integrity_flags: flags });
    assert.equal(reply.code, 'ok');
    assert.equal(client.conn.attachment().integrity_flags, flags);
  }
  const old = env.connect();
  assert.equal(old.hello(CURRENT).code, 'ok');
  assert.equal(old.conn.attachment().integrity_flags, null);
  assert.equal(integrityCategory(null), 'unknown');
  assert.equal(integrityCategory(undefined), 'unknown');
  assert.deepEqual([0, 1, 2, 3].map(integrityCategory), ['clean', 'cheat', 'editor', 'both']);
});

test('hello refuses integrity_flags outside 0 to 3 before anything is written', () => {
  const env = makeLobby();
  for (const flags of [-1, 4, 1.5, '0', '1', null, true, [], {}]) {
    const client = env.connect();
    const reply = client.hello({ ...CURRENT, integrity_flags: flags });
    assert.equal(reply.code, 'bad_request', JSON.stringify(flags));
    assert.equal(client.conn.attachment().hello, undefined);
  }
  assert.equal(accounts(env), 0);
});

test('the hello answer announces save_integrity and the clean online policy', () => {
  const open = makeLobby();
  const relaxed = open.connect().hello({ ...CURRENT, integrity_flags: 3 });
  assert.equal(relaxed.code, 'ok');
  assert.equal(relaxed.capabilities.save_integrity, true);
  assert.equal(relaxed.capabilities.clean_online_required, false);
  for (const name of ['relay_supported', 'story_session', 'boss_battle_session', 'appearance', 'udp_endpoint']) {
    assert.ok(name in relaxed.capabilities, name);
  }
  const strict = makeLobby({ cleanOnlineRequired: true });
  const clean = strict.connect().hello({ ...CURRENT, integrity_flags: 0 });
  assert.equal(clean.code, 'ok');
  assert.equal(clean.capabilities.clean_online_required, true);
});

test('clean online required: 0 signs in, 1 2 and 3 are marked, a missing field is unknown; refusals write no account', () => {
  const env = makeLobby({ cleanOnlineRequired: true });
  assert.equal(env.connect().hello({ ...CURRENT, integrity_flags: 0 }).code, 'ok');
  for (const flags of [1, 2, 3]) {
    const client = env.connect();
    assert.equal(client.hello({ ...CURRENT, integrity_flags: flags }).code, SAVE_INTEGRITY_MARKED);
    assert.equal(client.conn.attachment().hello, undefined);
  }
  const old = env.connect();
  assert.equal(old.hello(CURRENT).code, SAVE_INTEGRITY_UNKNOWN);
  assert.equal(accounts(env), 1);
  const reasons = env.lobby.metricsHistory(7).dimensions.reject.map((r) => r.value);
  assert.ok(reasons.includes(SAVE_INTEGRITY_MARKED) && reasons.includes(SAVE_INTEGRITY_UNKNOWN));
});

test('clean online required: a presence false Connection test without the field still signs in and reads capabilities', () => {
  const env = makeLobby({ cleanOnlineRequired: true });
  const test = env.connect();
  const reply = test.hello({ ...CURRENT, presence: false });
  assert.equal(reply.code, 'ok');
  assert.equal(reply.capabilities.clean_online_required, true);
  assert.equal(reply.capabilities.save_integrity, true);
  assert.equal(env.lobby.liveMetrics().online, 0);
});

test('clean online required: clean clients keep story, boss battle, appearance, relay and rooms unchanged', () => {
  const env = makeLobby({ cleanOnlineRequired: true, log: () => {} });
  const fields = { ...CURRENT, integrity_flags: 0, relay: true, story_session: true, boss_battle_session: true, appearance: true };
  const host = env.connect();
  const hello = host.hello(fields);
  assert.equal(hello.code, 'ok');
  assert.equal(hello.capabilities.relay_supported, true);
  const created = host.call('room.create', ROOM);
  assert.equal(created.code, 'ok');
  const guest = env.connect('198.51.100.4');
  assert.equal(guest.hello(fields).code, 'ok');
  assert.equal(guest.call('room.join', { room: created.room.id }).code, 'ok');
  const att = guest.conn.attachment();
  assert.deepEqual([att.relay, att.story_session, att.boss_battle_session, att.appearance], [true, true, true, true]);
});

test('live integrity counts players once per account and sessions by category; nothing reaches the public statistics', () => {
  const env = makeLobby();
  const states = [0, 0, 1, 2, 3, undefined];
  for (const flags of states) {
    const client = env.connect();
    assert.equal(client.hello(flags === undefined ? CURRENT : { ...CURRENT, integrity_flags: flags }).code, 'ok');
  }
  const live = Object.fromEntries(env.lobby.metricsSummary().live.integrity.map((r) => [r.value, r.count]));
  assert.deepEqual(live, { clean: 2, cheat: 1, editor: 1, both: 1, unknown: 1 });
  const sessions = Object.fromEntries(env.lobby.metricsHistory(7).dimensions.integrity.map((r) => [r.value, r.count]));
  assert.deepEqual(sessions, { clean: 2, cheat: 1, editor: 1, both: 1, unknown: 1 });
  assert.equal(env.lobby.metricsSummary().clean_online_required, false);
  assert.ok(!JSON.stringify(env.lobby.showcase()).includes('integrity'));
});
