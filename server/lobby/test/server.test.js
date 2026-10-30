import assert from 'node:assert/strict';
import { connect } from 'node:net';
import { after, before, test } from 'node:test';

import { startLobbyServer } from '../local/server.js';
import { ROOM } from './harness.js';

let server;
let url;

before(async () => {
  server = await startLobbyServer({ port: 0 });
  url = `ws://127.0.0.1:${server.port}/`;
});

after(async () => {
  await server.close();
});

class WsClient {
  static async open(account) {
    const c = new WsClient();
    c.account = account;
    c.ws = new WebSocket(url);
    c.messages = [];
    c.waiters = [];
    c.ws.addEventListener('message', (e) => {
      const msg = e.data === 'pong' ? 'pong' : JSON.parse(e.data);
      c.messages.push(msg);
      for (const w of c.waiters.splice(0)) w();
    });
    await new Promise((resolve, reject) => {
      c.ws.addEventListener('open', resolve, { once: true });
      c.ws.addEventListener('error', reject, { once: true });
    });
    c.seq = 0;
    return c;
  }

  async next(match, timeout = 2000) {
    const deadline = Date.now() + timeout;
    for (;;) {
      const i = this.messages.findIndex(match);
      if (i >= 0) return this.messages.splice(i, 1)[0];
      if (Date.now() > deadline) throw new Error('timed out');
      await new Promise((resolve) => {
        this.waiters.push(resolve);
        setTimeout(resolve, 50);
      });
    }
  }

  async call(op, body = {}) {
    const seq = ++this.seq;
    this.ws.send(JSON.stringify({ op, seq, ...body }));
    return this.next((m) => m.re === seq);
  }

  hello(name) {
    return this.call('hello', { proto: 1, game: 'CUSA10454-1.00', account: this.account, secret: 'ef'.repeat(32), name });
  }
}

test('two clients meet over WebSockets: hello, room, endpoints, keepalive, resume', async () => {
  const a = await WsClient.open('1111');
  const b = await WsClient.open('2222');
  assert.equal((await a.hello('Host')).ip, '127.0.0.1');
  assert.equal((await b.hello('Guest')).code, 'ok');
  a.ws.send('ping');
  assert.equal(await a.next((m) => m === 'pong'), 'pong');

  await a.call('udp', { port: 40001, lan: [{ ip: '127.0.0.1', port: 40001 }] });
  await b.call('udp', { port: 40002 });
  const { room } = await a.call('room.create', ROOM);
  const found = await b.call('room.search', { world: 1, filters: [[0x53, 1, 0x81102]] });
  assert.equal(found.rooms[0].id, room.id);
  assert.equal((await b.call('room.join', { room: room.id })).me, 0x20);
  const toB = await b.next((m) => m.ev === 'peer');
  assert.deepEqual([toB.member, toB.ip, toB.port, toB.lan], [0x10, '127.0.0.1', 40001, [{ ip: '127.0.0.1', port: 40001 }]]);
  const toA = await a.next((m) => m.ev === 'peer');
  assert.deepEqual([toA.member, toA.port], [0x20, 40002]);
  await a.next((m) => m.ev === 'room.member_joined');

  b.ws.close();
  await new Promise((resolve) => setTimeout(resolve, 100));
  const b2 = await WsClient.open('2222');
  await b2.hello('Guest');
  const resumed = await b2.call('room.resume', { room: room.id, member: 0x20 });
  assert.equal(resumed.code, 'ok');
  assert.equal(a.messages.filter((m) => m.ev === 'room.member_left').length, 0);
  a.ws.close();
  b2.ws.close();
});

test('the development server refuses unmasked frames and non-upgrade requests', async () => {
  const raw = connect(server.port, '127.0.0.1');
  await new Promise((resolve) => raw.once('connect', resolve));
  const chunks = [];
  raw.on('data', (d) => chunks.push(d));
  raw.write(['GET / HTTP/1.1', `Host: 127.0.0.1:${server.port}`, 'Upgrade: websocket', 'Connection: Upgrade',
    'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==', 'Sec-WebSocket-Version: 13', '', ''].join('\r\n'));
  await new Promise((resolve) => setTimeout(resolve, 100));
  assert.match(Buffer.concat(chunks).toString('latin1'), /101 Switching Protocols\r\n[\s\S]*s3pPLMBiTxaQ9kYGzzhZRbK\+xOo=/);
  raw.write(Buffer.from([0x81, 0x02, 0x68, 0x69]));
  const closed = await new Promise((resolve) => raw.once('close', () => resolve(true)));
  assert.ok(closed);

  const response = await fetch(`http://127.0.0.1:${server.port}/`);
  const body = await response.json();
  assert.equal(body.proto, 1);
});
