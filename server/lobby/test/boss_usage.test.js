import assert from 'node:assert/strict';
import { test } from 'node:test';

import { BOSS_NAMES, BOSS_STATE_ATTR, BOSS_TOP_SHOWN, activeBoss } from '../src/boss_usage.js';
import { BOSS_BATTLE_ATTR, STORY_SESSION_ATTR } from '../src/lobby.js';
import { ROOM, makeLobby } from './harness.js';

const MODERN = { online_protocol: 1, version: '1.3.0', platform: 'Windows', story_session: true, boss_battle_session: true };
const PHASE = { waiting: 1, starting: 2, fighting: 3, checkpoint: 4, returning: 5 };

const state = (phase, boss, sequence = 1) => {
  const bytes = [1, phase, 0, boss, sequence & 255, 0, 0, 0];
  return bytes.map((b) => b.toString(16).padStart(2, '0')).join('');
};

function player(env, name) {
  const client = env.connect(`198.51.100.${(env.ips = (env.ips ?? 0) + 1)}`);
  assert.equal(client.hello({ ...MODERN, name }).code, 'ok');
  return client;
}

function bossHost(env, name = 'Host') {
  const client = player(env, name);
  const reply = client.call('room.create', { ...ROOM, ints: [...ROOM.ints, [BOSS_BATTLE_ATTR, 1]] });
  assert.equal(reply.code, 'ok');
  return { client, room: reply.room };
}

function bossGuest(env, room, name = 'Guest') {
  const client = player(env, name);
  assert.equal(client.call('room.join', { room: room.id, boss_battle: true }).code, 'ok');
  return client;
}

const publish = (client, room, phase, boss, sequence) =>
  assert.equal(client.call('room.set_internal', { room: room.id, bin_int: [[BOSS_STATE_ATTR, state(phase, boss, sequence)]] }).code, 'ok');

const summary = (env) => env.lobby.metricsSummary().boss;
const plays = (env) => Object.fromEntries(summary(env).top.map((t) => [t.name, t.plays]));

test('the 20 encounters map to their names and a state is only a play when it starts or fights one', () => {
  assert.equal(BOSS_NAMES.length, 20);
  assert.equal(BOSS_NAMES[0], 'Harpy');
  assert.equal(BOSS_NAMES[19], 'Demon Lord');
  assert.equal(activeBoss(state(PHASE.starting, 19)), 19);
  assert.equal(activeBoss(state(PHASE.fighting, 0)), 0);
  for (const phase of [PHASE.waiting, PHASE.checkpoint, PHASE.returning]) assert.equal(activeBoss(state(phase, 3)), -1);
  assert.equal(activeBoss(state(PHASE.starting, 20)), -1);
  assert.equal(activeBoss(state(PHASE.starting, 255)), -1);
  assert.equal(activeBoss('02' + state(PHASE.starting, 1).slice(2)), -1);
  assert.equal(activeBoss(undefined), -1);
});

test('a party is two connected players in a Boss Battle room, counted once per room', () => {
  const env = makeLobby();
  assert.deepEqual(summary(env).totals, { parties_created: 0, plays: 0, users: 0 });
  const { client, room } = bossHost(env);
  assert.deepEqual(summary(env).live, { rooms: 1, parties: 0, waiting: 1, players: 1 });
  assert.equal(summary(env).totals.parties_created, 0);
  const guest = bossGuest(env, room);
  assert.deepEqual(summary(env).live, { rooms: 1, parties: 1, waiting: 0, players: 2 });
  assert.deepEqual(summary(env).totals, { parties_created: 1, plays: 0, users: 2 });
  publish(client, room, PHASE.waiting, 255, 1);
  publish(client, room, PHASE.waiting, 255, 2);
  assert.equal(summary(env).totals.parties_created, 1);
  guest.call('room.leave', { room: room.id });
  const second = bossGuest(env, room, 'Guest2');
  assert.deepEqual(summary(env).totals, { parties_created: 1, plays: 0, users: 3 });
  client.call('room.leave', { room: room.id });
  second.call('room.leave', { room: room.id });
  assert.deepEqual(summary(env).live, { rooms: 0, parties: 0, waiting: 0, players: 0 });
  assert.equal(summary(env).totals.parties_created, 1);
});

test('a room that never gains a player, and a story or normal room, add no Boss Battle figures', () => {
  const env = makeLobby();
  const { room } = bossHost(env);
  assert.equal(summary(env).totals.parties_created, 0);
  const other = player(env, 'Story');
  const story = other.call('room.create', { ...ROOM, ints: [...ROOM.ints, [STORY_SESSION_ATTR, 1]] }).room;
  const joiner = player(env, 'Joiner');
  assert.equal(joiner.call('room.join', { room: story.id }).code, 'ok');
  assert.deepEqual(summary(env).live, { rooms: 1, parties: 0, waiting: 1, players: 1 });
  assert.deepEqual(summary(env).totals, { parties_created: 0, plays: 0, users: 0 });
  assert.ok(room);
});

test('a boss start counts once however often it is republished, and Host and Guest do not double it', () => {
  const env = makeLobby();
  const { client, room } = bossHost(env);
  const guest = bossGuest(env, room);
  publish(client, room, PHASE.waiting, 255, 1);
  assert.equal(summary(env).totals.plays, 0);
  publish(client, room, PHASE.starting, 0, 2);
  publish(client, room, PHASE.starting, 0, 3);
  publish(client, room, PHASE.fighting, 0, 4);
  publish(client, room, PHASE.fighting, 0, 5);
  assert.deepEqual(plays(env), { Harpy: 1 });
  assert.equal(guest.call('room.set_internal', { room: room.id, bin_int: [[BOSS_STATE_ATTR, state(PHASE.starting, 1, 9)]] }).code,
    'not_owner');
  assert.deepEqual(plays(env), { Harpy: 1 });
  guest.call('room.leave', { room: room.id });
  client.call('room.leave', { room: room.id });
  assert.deepEqual(plays(env), { Harpy: 1 });
});

test('Fight All counts every boss it starts, and a boss fought again after a break counts again', () => {
  const env = makeLobby();
  const { client, room } = bossHost(env);
  bossGuest(env, room);
  publish(client, room, PHASE.starting, 0, 1);
  publish(client, room, PHASE.fighting, 0, 2);
  publish(client, room, PHASE.checkpoint, 0, 3);
  publish(client, room, PHASE.starting, 1, 4);
  publish(client, room, PHASE.fighting, 1, 5);
  publish(client, room, PHASE.returning, 1, 6);
  publish(client, room, PHASE.waiting, 255, 7);
  publish(client, room, PHASE.starting, 0, 8);
  assert.deepEqual(plays(env), { Harpy: 2, Wyvern: 1 });
  assert.equal(summary(env).totals.plays, 3);
});

test('a client that never publishes the state adds no plays', () => {
  const env = makeLobby();
  const { client, room } = bossHost(env);
  bossGuest(env, room);
  assert.equal(client.call('room.set_internal', { room: room.id, flag_mask: 0, flag_value: 0 }).code, 'ok');
  assert.equal(summary(env).totals.plays, 0);
  assert.deepEqual(summary(env).top, []);
});

test('Top Bosses lists at most 10, most played first, equal counts in encounter order', () => {
  const env = makeLobby();
  const { client, room } = bossHost(env);
  bossGuest(env, room);
  let sequence = 0;
  const play = (boss, times) => {
    for (let i = 0; i < times; i++) {
      env.advance(60 * 1000);
      publish(client, room, PHASE.starting, boss, ++sequence);
      env.advance(60 * 1000);
      publish(client, room, PHASE.waiting, 255, ++sequence);
    }
  };
  play(19, 5);
  for (let boss = 0; boss < 19; boss++) play(boss, boss < 12 ? 2 : 1);
  const top = summary(env).top;
  assert.equal(top.length, BOSS_TOP_SHOWN);
  assert.deepEqual(top[0], { rank: 1, boss: 19, name: 'Demon Lord', plays: 5 });
  assert.deepEqual(top.slice(1).map((t) => [t.rank, t.name, t.plays]), [2, 3, 4, 5, 6, 7, 8, 9, 10].map((rank) => [rank, BOSS_NAMES[rank - 2], 2]));
  assert.equal(summary(env).totals.plays, 5 + 24 + 7);
});

test('Boss Battle totals survive a restart of the lobby over the same storage', () => {
  const env = makeLobby();
  const { client, room } = bossHost(env);
  bossGuest(env, room);
  publish(client, room, PHASE.starting, 2, 1);
  const again = new env.lobby.constructor(env.host);
  assert.deepEqual(again.bossSummary().totals, { parties_created: 1, plays: 1, users: 2 });
  assert.equal(again.bossSummary().since, env.lobby.bossSummary().since);
});
