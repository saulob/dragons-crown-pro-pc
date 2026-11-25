import assert from 'node:assert/strict';
import { test } from 'node:test';

import { BOSS_BATTLE_ATTR, FLAG_HIDDEN, STORY_SESSION_ATTR } from '../src/lobby.js';
import { CAPABILITIES } from '../src/relay.js';
import { ROOM, makeLobby } from './harness.js';

const MODERN = { online_protocol: 1, version: '1.3.0', platform: 'Windows', story_session: true, boss_battle_session: true };
const LINUX = { ...MODERN, platform: 'Linux' };
const OLD = { online_protocol: 1, version: '1.2.0', platform: 'Windows', story_session: true };

const bossRoom = (value = 1) => ({ ...ROOM, ints: [...ROOM.ints, [BOSS_BATTLE_ATTR, value]] });

function greeted(env, fields, ip) {
  const client = env.connect(ip);
  assert.equal(client.hello(fields).code, 'ok');
  return client;
}

function members(env, room) {
  return env.host.sql.get('SELECT COUNT(*) AS n FROM members WHERE room = ?', Number(room)).n;
}

test('the attribute is private and hello advertises boss_battle_session', () => {
  assert.equal(BOSS_BATTLE_ATTR, 0x7f03);
  assert.notEqual(BOSS_BATTLE_ATTR, STORY_SESSION_ATTR);
  const env = makeLobby();
  for (const fields of [MODERN, OLD]) {
    const reply = env.connect().hello(fields);
    assert.deepEqual(reply.capabilities, { ...CAPABILITIES, story_session: true, boss_battle_session: true, appearance: true, udp_endpoint: true });
  }
});

test('only a capable client creates or tags a Boss Battle room', () => {
  const env = makeLobby();
  const old = greeted(env, OLD);
  const refused = old.call('room.create', bossRoom());
  assert.equal(refused.code, 'bad_request');
  const room = old.call('room.create', ROOM).room;
  assert.equal(old.call('room.set_external', { room: room.id, ints: [[BOSS_BATTLE_ATTR, 1]] }).code, 'bad_request');
  const host = greeted(env, MODERN, '198.51.100.4');
  const created = host.call('room.create', bossRoom());
  assert.equal(created.code, 'ok');
  assert.equal(created.room.flags & FLAG_HIDDEN, 0);
});

test('search from the Boss Battle page lists only open Boss Battle rooms; any other search lists none of them', () => {
  const env = makeLobby();
  const boss = greeted(env, MODERN).call('room.create', bossRoom()).room;
  const fighting = greeted(env, MODERN, '198.51.100.5').call('room.create', bossRoom(2)).room;
  const normal = greeted(env, MODERN, '198.51.100.6').call('room.create', ROOM).room;
  const seeker = greeted(env, MODERN, '198.51.100.7');
  const bossFound = seeker.call('room.search', { world: 1, boss_battle: true });
  assert.deepEqual(bossFound.rooms.map((r) => r.id), [boss.id]);
  const normalFound = seeker.call('room.search', { world: 1 });
  assert.deepEqual(normalFound.rooms.map((r) => r.id), [normal.id]);
  assert.equal(seeker.call('room.search', { code: boss.code }).total, 0);
  assert.equal(seeker.call('room.search', { code: boss.code, boss_battle: true }).total, 1);
  assert.equal(seeker.call('room.search', { code: normal.code, boss_battle: true }).total, 0);
  assert.equal(seeker.call('room.search', { code: fighting.code, boss_battle: true }).total, 0);
});

test('a Boss Battle room takes any capable client by id or code, with or without the page flag; Windows and Linux alike', () => {
  const env = makeLobby();
  const host = greeted(env, MODERN);
  const room = host.call('room.create', bossRoom()).room;
  const old = greeted(env, OLD, '198.51.100.4');
  for (const body of [{ room: room.id }, { code: room.code }, { room: room.id, boss_battle: true }]) {
    const reply = old.call('room.join', body);
    assert.equal(reply.code, 'incompatible');
    assert.equal(reply.boss_battle_required, true);
  }
  assert.equal(members(env, room.id), 1);
  assert.equal(host.events('room.member_joined').length, 0);
  const campaign = greeted(env, MODERN, '198.51.100.5');
  assert.equal(campaign.call('room.join', { room: room.id }).code, 'ok');
  assert.equal(members(env, room.id), 2);
  assert.equal(campaign.call('room.leave', { room: room.id }).code, 'ok');
  const guest = greeted(env, LINUX, '198.51.100.6');
  const joined = guest.call('room.join', { room: room.id, boss_battle: true });
  assert.equal(joined.code, 'ok');
  assert.deepEqual(joined.room.ints.find(([id]) => id === BOSS_BATTLE_ATTR), [BOSS_BATTLE_ATTR, 1]);
  const third = greeted(env, MODERN, '198.51.100.7');
  assert.equal(third.call('room.join', { code: room.code, boss_battle: true }).code, 'ok');
  assert.equal(members(env, room.id), 3);
});

test('a Boss Battle join never enters a normal room', () => {
  const env = makeLobby();
  const room = greeted(env, MODERN).call('room.create', ROOM).room;
  const reply = greeted(env, MODERN, '198.51.100.4').call('room.join', { room: room.id, boss_battle: true });
  assert.equal(reply.code, 'incompatible');
  assert.equal(reply.boss_battle_only, true);
  assert.equal(members(env, room.id), 1);
});

test('a host in a fight refuses joins until it is back on the Boss Battle page; the tag clears to a normal room', () => {
  const env = makeLobby();
  const host = greeted(env, MODERN);
  const room = host.call('room.create', bossRoom()).room;
  assert.equal(host.call('room.set_external', { room: room.id, ints: [[BOSS_BATTLE_ATTR, 2]] }).code, 'ok');
  const guest = greeted(env, MODERN, '198.51.100.4');
  assert.equal(guest.call('room.join', { room: room.id, boss_battle: true }).code, 'closed');
  assert.equal(host.call('room.set_external', { room: room.id, ints: [[BOSS_BATTLE_ATTR, 1]] }).code, 'ok');
  assert.equal(guest.call('room.join', { room: room.id, boss_battle: true }).code, 'ok');
  assert.equal(guest.call('room.leave', { room: room.id }).code, 'ok');
  assert.equal(host.call('room.set_external', { room: room.id, ints: [[BOSS_BATTLE_ATTR, 0]] }).code, 'ok');
  assert.equal(guest.call('room.join', { room: room.id, boss_battle: true }).boss_battle_only, true);
  assert.equal(guest.call('room.join', { room: room.id }).code, 'ok');
});

test('the host state reaches members through the private internal attribute before the owner hears ok', () => {
  const env = makeLobby();
  const host = greeted(env, MODERN);
  const room = host.call('room.create', bossRoom()).room;
  const guest = greeted(env, MODERN, '198.51.100.4');
  assert.equal(guest.call('room.join', { room: room.id, boss_battle: true }).code, 'ok');
  const state = '0102000005000000';
  assert.equal(host.call('room.set_internal', { room: room.id, flag_mask: 0, flag_value: 0, bin_int: [[0x7f04, state]] }).code,
    'ok');
  const update = guest.events('room.internal').at(-1);
  assert.deepEqual(update.bin_int, [[0x7f04, state]]);
  assert.equal(guest.call('room.set_internal', { room: room.id, bin_int: [[0x7f04, state]] }).code, 'not_owner');
});

test('a Boss Battle room whose host leaves passes to the next owner as a normal room, tag and host state gone', () => {
  const env = makeLobby();
  const host = greeted(env, MODERN);
  const room = host.call('room.create', bossRoom()).room;
  const guest = greeted(env, MODERN, '198.51.100.4');
  assert.equal(guest.call('room.join', { room: room.id, boss_battle: true }).code, 'ok');
  assert.equal(host.call('room.set_internal', { room: room.id, bin_int: [[0x7f04, '0101000002000000']] }).code, 'ok');
  assert.equal(host.call('room.leave', { room: room.id }).code, 'ok');
  assert.equal(guest.events('room.owner').length, 1);

  const seeker = greeted(env, MODERN, '198.51.100.5');
  assert.equal(seeker.call('room.search', { code: room.code, boss_battle: true }).total, 0);
  assert.equal(seeker.call('room.search', { code: room.code }).total, 1);
  assert.equal(seeker.call('room.join', { room: room.id, boss_battle: true }).boss_battle_only, true);
  const joined = seeker.call('room.join', { room: room.id });
  assert.equal(joined.code, 'ok');
  assert.equal(joined.room.ints.some(([id]) => id === BOSS_BATTLE_ATTR), false);
  assert.equal(joined.room.bin_int.some(([id]) => id === 0x7f04), false);
  assert.equal(env.host.sql.get('SELECT ints FROM rooms WHERE id = ?', Number(room.id)).ints.includes(String(BOSS_BATTLE_ATTR)),
    false);
});

test('a normal room keeps its attributes when its owner leaves', () => {
  const env = makeLobby();
  const host = greeted(env, MODERN);
  const room = host.call('room.create', ROOM).room;
  const guest = greeted(env, MODERN, '198.51.100.4');
  assert.equal(guest.call('room.join', { room: room.id }).code, 'ok');
  assert.equal(host.call('room.set_internal', { room: room.id, bin_int: [[0x57, 'aa']] }).code, 'ok');
  const before = env.host.sql.get('SELECT ints, bin_int FROM rooms WHERE id = ?', Number(room.id));
  assert.equal(host.call('room.leave', { room: room.id }).code, 'ok');
  assert.deepEqual(env.host.sql.get('SELECT ints, bin_int FROM rooms WHERE id = ?', Number(room.id)), before);
});
