// The lobby's host for Node: node:sqlite for the tables, plain objects for sockets. Attachments are copied on every
// read and write, as a Durable Object's serialized attachments are, so code that forgets to store one fails here too.
import { DatabaseSync } from 'node:sqlite';

export function sqliteStore(file = ':memory:') {
  const db = new DatabaseSync(file);
  db.exec('PRAGMA journal_mode = WAL');
  const cache = new Map();
  const prepare = (query) => {
    let statement = cache.get(query);
    if (!statement) {
      statement = db.prepare(query);
      cache.set(query, statement);
    }
    return statement;
  };
  return {
    all: (query, ...args) => prepare(query).all(...args).map((row) => ({ ...row })),
    get: (query, ...args) => {
      const row = prepare(query).get(...args);
      return row ? { ...row } : undefined;
    },
    run: (query, ...args) => {
      prepare(query).run(...args);
    },
    close: () => db.close(),
  };
}

export class LocalHost {
  constructor(sql, { log = () => {}, now = () => Date.now() } = {}) {
    this.sql = sql;
    this.conns = new Set();
    this.log = log;
    this.now = now;
    this.alarmAt = null;
    this.timer = null;
    this.onAlarm = null;
  }

  sockets() {
    return [...this.conns];
  }

  setAlarm(ms) {
    this.alarmAt = ms;
    if (this.timer) clearTimeout(this.timer);
    if (!this.onAlarm) return;
    this.timer = setTimeout(() => {
      this.timer = null;
      this.alarmAt = null;
      this.onAlarm();
    }, Math.max(0, ms - this.now()));
    this.timer.unref?.();
  }
}

// A socket for the lobby: `transport` sends text and closes the real connection.
export class LocalConn {
  constructor(host, transport) {
    this.host = host;
    this.transport = transport;
    this.stored = null;
  }

  attachment() {
    return this.stored ? structuredClone(this.stored) : null;
  }

  setAttachment(value) {
    this.stored = structuredClone(value);
  }

  send(text) {
    this.transport.send(text);
  }

  close(code, reason) {
    this.transport.close(code, reason);
  }
}
