// In-process lobby clients for the protocol tests: no sockets, a clock the test moves.
import { Lobby } from '../src/lobby.js';
import { LocalConn, LocalHost, sqliteStore } from '../local/host.js';

let accounts = 0;

export function makeLobby({ local = false } = {}) {
  const clock = { now: 1_700_000_000_000 };
  const host = new LocalHost(sqliteStore(), { now: () => clock.now, log: (text) => { throw new Error(text); } });
  const lobby = new Lobby(host, { local });
  return {
    lobby,
    host,
    clock,
    advance(ms) {
      clock.now += ms;
      if (host.alarmAt !== null && host.alarmAt <= clock.now) {
        host.alarmAt = null;
        lobby.alarm();
      }
    },
    connect(ip = '203.0.113.7', country) {
      return new Client(lobby, host, ip, country);
    },
  };
}

export class Client {
  constructor(lobby, host, ip, country) {
    this.lobby = lobby;
    this.host = host;
    this.ip = ip;
    this.inbox = [];
    this.closedWith = null;
    this.seq = 0;
    const first = lobby.admit(ip, country);
    this.admitted = Boolean(first);
    if (!first) return;
    this.conn = new LocalConn(host, {
      send: (text) => this.inbox.push(JSON.parse(text)),
      close: (code, reason) => {
        this.closedWith = { code, reason };
      },
    });
    this.conn.setAttachment(first);
    host.conns.add(this.conn);
    accounts += 1;
    this.account = String(1000 + accounts * 7919);
    this.secret = 'ab'.repeat(32);
  }

  raw(text) {
    const before = this.inbox.length;
    this.lobby.message(this.conn, text);
    const replies = this.inbox.splice(before);
    const response = replies.find((m) => 're' in m);
    this.inbox.push(...replies.filter((m) => m !== response));
    return response;
  }

  call(op, body = {}) {
    return this.raw(JSON.stringify({ op, seq: ++this.seq, ...body }));
  }

  hello(fields = {}) {
    return this.call('hello', { proto: 1, game: 'CUSA10454-1.00', account: this.account, secret: this.secret,
      name: fields.name ?? `P${this.account.slice(-3)}`, ...fields });
  }

  events(name) {
    const taken = this.inbox.filter((m) => m.ev === name);
    this.inbox = this.inbox.filter((m) => m.ev !== name);
    return taken;
  }

  drop() {
    this.lobby.closed(this.conn);
    this.host.conns.delete(this.conn);
  }
}

export const ROOM = {
  world: 1,
  slots: 4,
  flags: 0x86000000,
  ints: [[0x4c, 3], [0x4d, 2], [0x53, 0x81102]],
  bin_int: [[0x57, '00'.repeat(0x78)]],
  bin_ext: [[0x55, '11'.repeat(0x78)]],
};
