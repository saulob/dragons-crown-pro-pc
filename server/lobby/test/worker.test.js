// The Worker's Durable Object under a model of the Hibernation runtime: what reaches the lobby, and what an open socket
// keeps when the object is rebuilt.
import assert from 'node:assert/strict';
import { mock, test as nodeTest } from 'node:test';

import { PROTECTION } from '../src/abuse.js';
import { RELAY, encodeRelayFrame, roomToken } from '../src/relay.js';
import { ROOM } from './harness.js';
import { DurableObjectState, available } from './support/hibernation.js';

mock.timers.enable({ apis: ['Date'], now: 1_700_000_000_000 });

const test = (name, run) => nodeTest(name, { skip: available ? false : 'needs Node.js 22.15 or later (module.registerHooks)' }, run);

const STRIKES = PROTECTION.refusedRequests;
let accounts = 0;
let seq = 0;

const hello = (fields = {}) => JSON.stringify({ op: 'hello', seq: ++seq, proto: 1, game: 'CUSA10454-1.00',
  account: String(5000 + ++accounts * 7919), secret: 'cd'.repeat(32), name: 'Tester', relay: true, online_protocol: 1, ...fields });
const call = (op, body = {}) => JSON.stringify({ op, seq: ++seq, ...body });
const answers = (ws) => ws.sent.filter((m) => typeof m === 'string' && m.startsWith('{')).map((m) => JSON.parse(m));

async function lobby() {
  const state = new DurableObjectState();
  const object = await state.wake();
  const handled = [];
  const message = object.lobby.message.bind(object.lobby);
  object.lobby.message = (conn, text) => {
    handled.push(text);
    message(conn, text);
  };
  return { state, object, handled };
}

async function signedIn(state, object) {
  const ws = await state.connect(object, `198.18.0.${state.sockets.length + 1}`);
  state.deliver(object, ws, hello());
  assert.equal(answers(ws).at(-1).code, 'ok');
  return ws;
}

async function roomOf(state, object, size) {
  const sockets = [];
  for (let i = 0; i < size; i++) sockets.push(await signedIn(state, object));
  state.deliver(object, sockets[0], call('room.create', ROOM));
  const created = answers(sockets[0]).at(-1);
  const me = [created.me];
  for (const ws of sockets.slice(1)) {
    state.deliver(object, ws, call('room.join', { room: created.room.id }));
    me.push(answers(ws).at(-1).me);
  }
  return { sockets, me, token: roomToken(created.room.key) };
}

test('the heartbeat is answered by the runtime and never reaches the lobby, before or after sign-in', async () => {
  const { state, object, handled } = await lobby();
  assert.equal(state.autoResponse.request, 'ping');
  assert.equal(state.autoResponse.response, 'pong');
  const ws = await state.connect(object);
  for (let i = 0; i < 100; i++) state.deliver(object, ws, 'ping');
  assert.equal(handled.length, 0);
  assert.equal(ws.sent.filter((m) => m === 'pong').length, 100);
  assert.equal(ws.closedWith, null);
  state.deliver(object, ws, hello());
  assert.equal(answers(ws).at(-1).code, 'ok');
  for (let i = 0; i < 100; i++) state.deliver(object, ws, 'ping');
  assert.equal(handled.length, 1);
  assert.equal(object.lobby.sockets()[0].conn.keepaliveAt(), Date.now());
});

test('only the exact heartbeat is auto-answered: anything else is an ordinary message', async () => {
  const { state, object, handled } = await lobby();
  const ws = await state.connect(object);
  state.deliver(object, ws, 'PING');
  state.deliver(object, ws, JSON.stringify({ op: 'ping', seq: 1 }));
  assert.equal(handled.length, 2);
  assert.deepEqual(answers(ws).map((a) => a.code), ['bad_request', 'bad_request']);
});

test('an upgrade gives the socket an attachment and refusing a frame flood before sign-in writes nothing', async () => {
  const { state, object, handled } = await lobby();
  const ws = await state.connect(object);
  assert.match(ws.deserializeAttachment().cid, /^[0-9a-f]{16}$/);
  const before = state.changes();
  for (let i = 0; i < 1000 && !ws.closedWith; i++) state.deliver(object, ws, call('worlds'));
  assert.deepEqual(ws.closedWith, { code: 1008, reason: 'sign in first' });
  assert.equal(handled.length, PROTECTION.preHelloFrames + 1);
  assert.equal(state.changes(), before);
});

test('a socket keeps its pre-sign-in count when the object is rebuilt', async () => {
  const { state, object } = await lobby();
  const ws = await state.connect(object);
  for (let i = 0; i < PROTECTION.preHelloFrames - 1; i++) state.deliver(object, ws, 'not json');
  const woken = await state.wake();
  state.deliver(woken, ws, 'not json');
  assert.equal(ws.closedWith, null);
  state.deliver(woken, ws, 'not json');
  assert.deepEqual(ws.closedWith, { code: 1008, reason: 'sign in first' });
});

test('binary frames before sign-in share the pre-sign-in count with text', async () => {
  const { state, object } = await lobby();
  const ws = await state.connect(object);
  const frame = encodeRelayFrame(1, 0x20, 1, new Uint8Array(40));
  state.deliver(object, ws, 'not json');
  for (let i = 0; i < PROTECTION.preHelloFrames - 1; i++) state.deliver(object, ws, frame.buffer);
  assert.equal(ws.closedWith, null);
  state.deliver(object, ws, frame.buffer);
  assert.deepEqual(ws.closedWith, { code: 1008, reason: 'sign in first' });
});

test('a socket keeps its message budget and strikes when the object is rebuilt', async () => {
  const { state, object } = await lobby();
  const ws = await signedIn(state, object);
  let sent = 0;
  while (answers(ws).filter((a) => a.code === 'limit').length < STRIKES.burst - 1) {
    state.deliver(object, ws, call('worlds'));
    sent += 1;
  }
  const woken = await state.wake();
  const before = state.changes();
  state.deliver(woken, ws, call('worlds'));
  assert.equal(answers(ws).at(-1).code, 'limit');
  assert.equal(ws.closedWith, null);
  state.deliver(woken, ws, call('worlds'));
  assert.deepEqual(ws.closedWith, { code: 1008, reason: 'too many refused requests' });
  assert.ok(sent < 100);
  assert.equal(state.changes(), before);
});

test('unknown operations count as strikes that survive a rebuilt object', async () => {
  const { state, object } = await lobby();
  const ws = await signedIn(state, object);
  for (let i = 0; i < STRIKES.burst - 1; i++) state.deliver(object, ws, call('room.nuke'));
  const woken = await state.wake();
  state.deliver(woken, ws, call('room.nuke'));
  assert.equal(ws.closedWith, null);
  state.deliver(woken, ws, call('room.nuke'));
  assert.deepEqual(ws.closedWith, { code: 1008, reason: 'too many refused requests' });
});

test('a socket keeps its relay strikes when the object is rebuilt', async () => {
  const { state, object } = await lobby();
  const ws = await signedIn(state, object);
  const junk = new Uint8Array([1, 2, 3]).buffer;
  for (let i = 0; i < RELAY.malformed.burst - 1; i++) state.deliver(object, ws, junk);
  const woken = await state.wake();
  state.deliver(woken, ws, junk);
  assert.equal(ws.closedWith, null);
  state.deliver(woken, ws, junk);
  assert.deepEqual(ws.closedWith, { code: 1008, reason: 'relay abuse' });
});

test('stale relay frames are counted in the attachment too, and a rebuilt object finishes the count', async () => {
  const { state, object } = await lobby();
  const { sockets: [a], token } = await roomOf(state, object, 2);
  const stale = encodeRelayFrame(token, 0x40, 1, new Uint8Array(20)).buffer;
  for (let i = 0; i < RELAY.stale.burst; i++) state.deliver(object, a, stale);
  assert.equal(a.closedWith, null);
  const woken = await state.wake();
  state.deliver(woken, a, stale);
  assert.deepEqual(a.closedWith, { code: 1008, reason: 'relay abuse' });
});

test('relay suspension survives a rebuilt object, and a closed socket is ignored by the next one', async () => {
  const { state, object } = await lobby();
  const { sockets: [a, b], me, token } = await roomOf(state, object, 2);
  const frame = (i) => encodeRelayFrame(token, me[1], i, new Uint8Array(40)).buffer;
  let sent = 0;
  while (!answers(a).some((m) => m.ev === 'relay.closed')) state.deliver(object, a, frame(sent++));
  const forwarded = b.sent.filter((m) => typeof m !== 'string').length;
  const woken = await state.wake();
  state.deliver(woken, a, frame(sent++));
  assert.equal(b.sent.filter((m) => typeof m !== 'string').length, forwarded);
  while (!a.closedWith) state.deliver(woken, a, frame(sent++));
  assert.deepEqual(a.closedWith, { code: 1008, reason: 'relay abuse' });
  const again = await state.wake();
  const before = state.changes();
  state.deliver(again, a, frame(sent++));
  state.deliver(again, a, call('worlds'));
  assert.equal(b.sent.filter((m) => typeof m !== 'string').length, forwarded);
  assert.equal(state.changes(), before);
  assert.equal(b.closedWith, null);
});

test('an abusive socket stays ignored after the object is rebuilt, whatever the runtime still delivers', async () => {
  const { state, object, handled } = await lobby();
  const ws = await state.connect(object);
  while (!ws.closedWith) state.deliver(object, ws, call('worlds'));
  const woken = await state.wake();
  const wokenHandled = [];
  const message = woken.lobby.message.bind(woken.lobby);
  woken.lobby.message = (conn, text) => {
    wokenHandled.push(text);
    message(conn, text);
  };
  const sent = ws.sent.length;
  const before = state.changes();
  for (let i = 0; i < 50; i++) woken.webSocketMessage(ws, call('worlds'));
  for (let i = 0; i < 50; i++) woken.webSocketMessage(ws, new Uint8Array([1, 2, 3]).buffer);
  assert.equal(handled.length, PROTECTION.preHelloFrames + 1);
  assert.equal(ws.sent.length, sent);
  assert.equal(state.changes(), before);
  assert.equal(woken.lobby.metrics.pending.get('ws_text_messages') ?? 0, 0);
  assert.equal(woken.lobby.metrics.pending.get('ws_binary_messages') ?? 0, 0);
  assert.equal(wokenHandled.length, 50);
});

test('a text frame over the size limit is closed by the Worker before the lobby sees it', async () => {
  const { state, object, handled } = await lobby();
  const ws = await state.connect(object);
  state.deliver(object, ws, 'x'.repeat(1024 * 1024));
  assert.equal(ws.closedWith?.code, 1009);
  assert.equal(handled.length, 0);
});

test('abuse paths log nothing that names a connection, account, room or member', async () => {
  const logs = [];
  const log = console.log;
  console.log = (text) => logs.push(String(text));
  try {
    const { state, object } = await lobby();
    const { sockets: [a], me, token } = await roomOf(state, object, 2);
    const frame = (i) => encodeRelayFrame(token, me[1], i, new Uint8Array(40)).buffer;
    for (let i = 0; i < 3000 && !a.closedWith; i++) state.deliver(object, a, frame(i));
    const silent = await state.connect(object);
    while (!silent.closedWith) state.deliver(object, silent, call('worlds'));
    const junk = await signedIn(state, object);
    while (!junk.closedWith) state.deliver(object, junk, new Uint8Array([9]).buffer);
  } finally {
    console.log = log;
  }
  assert.ok(logs.length > 0);
  for (const line of logs) assert.doesNotMatch(line, /cid=|room=|member=|account|[0-9a-f]{16}/);
});

test('the first relay frames after the object was rebuilt are forwarded both ways, with nothing struck', async () => {
  const { state, object } = await lobby();
  const { sockets: [a, b], me, token } = await roomOf(state, object, 2);
  for (const [ws, port] of [[a, 3658], [b, 63013], [b, 63672]]) {
    state.deliver(object, ws, call('udp', { port, lan: [{ ip: '192.168.68.62', port }] }));
    assert.equal(answers(ws).at(-1).code, 'ok');
  }
  state.deliver(object, b, call('presence.set', { data: '00'.repeat(0x78) }));
  const frames = (ws) => ws.sent.filter((m) => typeof m !== 'string');
  const fromTo = (index, seq) => encodeRelayFrame(token, me[1 - index], seq, new Uint8Array(24)).buffer;

  const woken = await state.wake();
  state.deliver(woken, a, fromTo(0, 1));
  state.deliver(woken, b, fromTo(1, 1));
  assert.equal(frames(b).length, 1);
  assert.equal(frames(a).length, 1);

  const again = await state.wake();
  state.deliver(again, a, fromTo(0, 2));
  state.deliver(again, b, fromTo(1, 2));
  assert.equal(frames(b).length, 2);
  assert.equal(frames(a).length, 2);

  for (const ws of [a, b]) {
    assert.equal(ws.closedWith, null);
    assert.equal(answers(ws).filter((m) => m.ev === 'relay.closed').length, 0);
    const att = ws.deserializeAttachment();
    for (const key of ['abusive', 'overruns', 'relayMalformed', 'relayStale', 'relayOff']) assert.equal(att[key], undefined, key);
  }
  for (const lobbyState of [woken.lobby, again.lobby]) {
    for (const name of ['relay_invalid', 'relay_overrun', 'relay_suspended', 'relay_abuse_closed']) {
      assert.equal(lobbyState.metrics.pending.get(name) ?? 0, 0, name);
    }
  }
});
