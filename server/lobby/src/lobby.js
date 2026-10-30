// The lobby: accounts, friends, presence, rooms and the endpoint exchange (docs/ONLINE.md). Game traffic never passes
// through it. State lives in SQL tables and in each socket's attachment, so a hibernated Durable Object resumes as if
// it had never stopped; the same class runs in the local development server (local/server.js).
import { Metrics, countryCode, platformCategory, sampleBucket, versionCategory, SAMPLE_MS } from './metrics.js';
import { sha256Hex, toHex } from './sha256.js';

export const PROTOCOL = 1;

export const LIMITS = {
  frameBytes: 32 * 1024,
  reportFrameBytes: 160 * 1024,
  nameChars: 16,
  gameChars: 32,
  binAttrBytes: 0x100,
  binAttrsPerArray: 16,
  intAttrsPerRoom: 32,
  filters: 16,
  searchMax: 20,
  lanCandidates: 4,
  presenceDataBytes: 0x80,
  presenceStatusBytes: 0xc0,
  chatChars: 120,
  friends: 200,
  pendingOut: 50,
  pendingIn: 50,
  requestDays: 14,
  registrationsPerIpPerDay: 20,
  socketsPerIp: 16,
  heldEvents: 32,
  graceMs: 30000,
  reportChars: 256,
  reportLogChars: 128 * 1024,
  reportVersionChars: 64,
  versionChars: 16,
  platformChars: 16,
  reportsPerAccountPerDay: 5,
  reportsPerIpPerDay: 10,
  reportsPerDay: 300,
  reportsKept: 1000,
};

const BUCKETS = {
  any: { burst: 60, perSecond: 20 },
  room: { burst: 30, perSecond: 1 },
  chat: { burst: 5, perSecond: 1 },
};
const ROOM_OPS = new Set(['room.create', 'room.join', 'room.leave', 'room.search', 'room.resume']);
// Operations after which the live counts can have grown.
const GROWING_OPS = new Set(['hello', 'room.create', 'room.join', 'room.resume']);

export const FLAG_CLOSED = 0x40000000;
export const FLAG_FULL = 0x20000000;
export const FLAG_HIDDEN = 0x10000000;

const CODE_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
const DAY_MS = 24 * 3600 * 1000;
const MEMBER_IDS = [0x10, 0x20, 0x30, 0x40];
const FIRST_ROOM_ID = 4096;

export class LobbyError extends Error {
  constructor(code, detail = '', body = {}) {
    super(detail || code);
    this.code = code;
    this.body = body;
  }
}

const fail = (code, detail, body) => {
  throw new LobbyError(code, detail, body);
};

const SCHEMA = [
  `CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)`,
  `CREATE TABLE IF NOT EXISTS accounts (account TEXT PRIMARY KEY, secret_hash TEXT NOT NULL, code TEXT NOT NULL UNIQUE,
     name TEXT NOT NULL, created INTEGER NOT NULL, seen INTEGER NOT NULL)`,
  `CREATE TABLE IF NOT EXISTS friends (account TEXT NOT NULL, friend TEXT NOT NULL, since INTEGER NOT NULL,
     PRIMARY KEY (account, friend))`,
  `CREATE TABLE IF NOT EXISTS friend_requests (sender TEXT NOT NULL, target TEXT NOT NULL, ms INTEGER NOT NULL,
     PRIMARY KEY (sender, target))`,
  `CREATE TABLE IF NOT EXISTS registrations (ip TEXT NOT NULL, ms INTEGER NOT NULL)`,
  `CREATE TABLE IF NOT EXISTS rooms (id INTEGER PRIMARY KEY, game TEXT NOT NULL, world INTEGER NOT NULL,
     slots INTEGER NOT NULL, flags INTEGER NOT NULL, owner INTEGER NOT NULL, code TEXT NOT NULL UNIQUE,
     key TEXT NOT NULL, ints TEXT NOT NULL, bin_int TEXT NOT NULL, bin_ext TEXT NOT NULL, created INTEGER NOT NULL)`,
  `CREATE TABLE IF NOT EXISTS members (room INTEGER NOT NULL, member INTEGER NOT NULL, account TEXT NOT NULL,
     name TEXT NOT NULL, joined INTEGER NOT NULL, bin TEXT NOT NULL, conn TEXT, ip TEXT NOT NULL DEFAULT '',
     port INTEGER NOT NULL DEFAULT 0, lan TEXT NOT NULL DEFAULT '[]', grace_until INTEGER,
     held TEXT NOT NULL DEFAULT '[]', PRIMARY KEY (room, member))`,
  `CREATE TABLE IF NOT EXISTS reports (id INTEGER PRIMARY KEY AUTOINCREMENT, ms INTEGER NOT NULL, account TEXT NOT NULL,
     ip TEXT NOT NULL, version TEXT NOT NULL, text TEXT NOT NULL, log TEXT NOT NULL)`,
];

// Applied once each, in order; meta 'schema' counts the ones done.
const MIGRATIONS = [
  [`ALTER TABLE rooms ADD COLUMN online_protocol INTEGER NOT NULL DEFAULT 0`,
    `ALTER TABLE members ADD COLUMN version TEXT NOT NULL DEFAULT ''`,
    `ALTER TABLE members ADD COLUMN platform TEXT NOT NULL DEFAULT ''`],
];

// ---- input validation ----
const isObject = (v) => v !== null && typeof v === 'object' && !Array.isArray(v);
const isInt = (v, lo, hi) => Number.isInteger(v) && v >= lo && v <= hi;
const uint32 = (v) => (isInt(v, 0, 0xffffffff) ? v : fail('bad_request', 'uint32 expected'));

function decimalId(v) {
  if (typeof v !== 'string' || !/^[1-9][0-9]{0,19}$/.test(v) || BigInt(v) > 0xffffffffffffffffn) {
    fail('bad_request', 'id expected');
  }
  return v;
}

export function cleanText(v, maxChars) {
  if (typeof v !== 'string') fail('bad_request', 'text expected');
  // eslint-disable-next-line no-control-regex
  return Array.from(v.replace(/[\u0000-\u001f\u007f-\u009f]/g, '').trim()).slice(0, maxChars).join('');
}

function hexBytes(v, maxBytes) {
  if (typeof v !== 'string' || v.length % 2 || v.length > maxBytes * 2 || !/^[0-9a-fA-F]*$/.test(v)) {
    fail('bad_request', 'hex expected');
  }
  return v.toLowerCase();
}

// [[id, hex], ...]: binary attributes, merged by id.
function binAttrs(v) {
  if (v === undefined) return [];
  if (!Array.isArray(v) || v.length > LIMITS.binAttrsPerArray) fail('bad_request', 'attributes expected');
  return v.map((e) => {
    if (!Array.isArray(e) || e.length !== 2 || !isInt(e[0], 0, 0xffff)) fail('bad_request', 'attribute expected');
    return [e[0], hexBytes(e[1], LIMITS.binAttrBytes)];
  });
}

function intAttrs(v) {
  if (v === undefined) return [];
  if (!Array.isArray(v) || v.length > LIMITS.intAttrsPerRoom) fail('bad_request', 'attributes expected');
  return v.map((e) => {
    if (!Array.isArray(e) || e.length !== 2 || !isInt(e[0], 0, 0xffff)) fail('bad_request', 'attribute expected');
    return [e[0], uint32(e[1])];
  });
}

function merge(old, changes, cap) {
  const byId = new Map(old);
  for (const [id, value] of changes) byId.set(id, value);
  if (byId.size > cap) fail('limit', 'too many attributes');
  return [...byId.entries()].sort((a, b) => a[0] - b[0]);
}

// The port version and platform a client reported, for players to read.
function clientOf(v) {
  return { version: v?.version ?? '', platform: v?.platform ?? '' };
}

// Only the online protocol decides who plays together; a client that sent none has 0.
function compatible(att, room) {
  return (att.online_protocol ?? 0) === room.online_protocol;
}

const IPV4 = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/;

export function ipv4Kind(ip) {
  const m = typeof ip === 'string' ? IPV4.exec(ip) : null;
  if (!m) return 'invalid';
  const o = m.slice(1).map(Number);
  if (o.some((x) => x > 255)) return 'invalid';
  if (o[0] === 127) return 'loopback';
  if (o[0] === 10 || (o[0] === 172 && o[1] >= 16 && o[1] < 32) || (o[0] === 192 && o[1] === 168)) return 'private';
  if (o[0] === 169 && o[1] === 254) return 'link';
  if (o[0] === 100 && o[1] >= 64 && o[1] < 128) return 'cgnat';
  if (o[0] === 0 || o[0] >= 224) return 'reserved';
  return 'public';
}

// Per-address limits group IPv6 clients by their /64.
const ipGroup = (ip) => (typeof ip === 'string' && ip.includes(':') ? ip.split(':').slice(0, 4).join(':') : String(ip));
const isLocalAddress = (ip) => ip === '::1' || ipv4Kind(ip) === 'loopback';

export function formatFriendCode(code) {
  return `${code.slice(0, 4)}-${code.slice(4)}`;
}

export function normalizeFriendCode(text) {
  if (typeof text !== 'string') fail('bad_request', 'code expected');
  const code = text.toUpperCase().replace(/[\s-]/g, '');
  if (code.length !== 8 || [...code].some((c) => !CODE_ALPHABET.includes(c))) fail('bad_request', 'malformed code');
  return code;
}

function takeToken(bucket, spec, now) {
  const [tokens, last] = bucket ?? [spec.burst, now];
  const refilled = Math.min(spec.burst, tokens + ((now - last) / 1000) * spec.perSecond);
  if (refilled < 1) return null;
  return [refilled - 1, now];
}

export class Lobby {
  // host: { sql: { all(q, ...a), get(q, ...a), run(q, ...a) }, sockets(): Conn[], now(): ms, setAlarm(ms), log(text) }
  // Conn: { send(text), close(code, reason), attachment(), setAttachment(object) }
  constructor(host, options = {}) {
    this.host = host;
    this.local = Boolean(options.local);
    for (const statement of SCHEMA) host.sql.run(statement);
    for (let done = Number(this.meta('schema') ?? 0); done < MIGRATIONS.length; done++) {
      for (const statement of MIGRATIONS[done]) host.sql.run(statement);
      this.setMeta('schema', done + 1);
    }
    if (!this.meta('salt')) this.setMeta('salt', this.randomHex(16));
    this.metrics = new Metrics(host.sql, () => host.now());
    this.alarmAt = null;
    this.reconcileSeats();
  }

  // Seats whose socket is gone (the object or the server restarted) get the usual grace instead of staying occupied.
  reconcileSeats() {
    const live = new Set(this.host.sockets().map((c) => c.attachment()?.cid).filter(Boolean));
    const until = this.host.now() + LIMITS.graceMs;
    for (const m of this.host.sql.all('SELECT room, member, conn FROM members WHERE conn IS NOT NULL')) {
      if (!live.has(m.conn)) {
        this.host.sql.run('UPDATE members SET conn = NULL, grace_until = ? WHERE room = ? AND member = ?', until, m.room, m.member);
      }
    }
    this.scheduleAlarm();
  }

  // A fresh development server has no sockets: rooms of the previous process are only ghosts.
  clearRooms() {
    const rooms = this.host.sql.get('SELECT COUNT(*) AS n FROM rooms').n;
    this.host.sql.run('DELETE FROM members');
    this.host.sql.run('DELETE FROM rooms');
    return rooms;
  }

  stats() {
    return {
      accounts: this.host.sql.get('SELECT COUNT(*) AS n FROM accounts').n,
      friendships: this.host.sql.get('SELECT COUNT(*) AS n FROM friends').n / 2,
      requests: this.host.sql.get('SELECT COUNT(*) AS n FROM friend_requests').n,
    };
  }

  registerAccount(account, hash, name) {
    let code;
    do code = this.randomCode(8);
    while (this.host.sql.get('SELECT 1 AS x FROM accounts WHERE code = ?', code));
    const now = this.host.now();
    this.host.sql.run('INSERT INTO accounts (account, secret_hash, code, name, created, seen) VALUES (?, ?, ?, ?, ?, ?)',
      account, hash, code, name, now, now);
    return this.accountRow(account);
  }

  // Development only (local/server.js --seed-friends): makes two identities friends, registering them if needed.
  seedFriends(a, b) {
    for (const id of [a, b]) {
      const hash = sha256Hex(id.secret);
      const row = this.accountRow(id.account);
      if (!row) this.registerAccount(id.account, hash, id.name);
      else if (row.secret_hash !== hash) fail('unauthorized', `account ${id.account} belongs to another secret`);
    }
    this.makeFriends(a.account, b.account);
  }

  // ---- connection lifecycle ----
  // Refuses a socket over the per-address limit; else returns its first attachment. `country` is the edge's, never the
  // client's.
  admit(ip, country = '') {
    const group = ipGroup(ip);
    if (!(this.local && isLocalAddress(ip))) {
      const open = this.host.sockets().filter((c) => ipGroup(c.attachment()?.ip) === group).length;
      if (open >= LIMITS.socketsPerIp) return null;
    }
    return { cid: this.randomHex(8), ip, opened: this.host.now(), country: countryCode(country) };
  }

  message(conn, text) {
    const att = conn.attachment();
    if (!att) return;
    let seq = null;
    this.current = att;
    try {
      if (typeof text !== 'string') fail('bad_request', 'text frames only');
      if (text.length > LIMITS.reportFrameBytes) fail('too_large');
      let msg;
      try {
        msg = JSON.parse(text);
      } catch {
        fail('bad_request', 'invalid JSON');
      }
      if (!isObject(msg) || typeof msg.op !== 'string' || !isInt(msg.seq, 0, 0x7fffffff)) fail('bad_request', 'envelope');
      seq = msg.seq;
      if (text.length > LIMITS.frameBytes && msg.op !== 'report.send') fail('too_large');
      const now = this.host.now();
      const any = takeToken(att.ra, BUCKETS.any, now);
      if (!any) fail('limit', 'slow down');
      att.ra = any;
      if (ROOM_OPS.has(msg.op)) {
        const room = takeToken(att.rr, BUCKETS.room, now);
        if (!room) fail('limit', 'slow down');
        att.rr = room;
      }
      if (!Object.hasOwn(HANDLERS, msg.op)) fail('bad_request', 'unknown op');
      if (msg.op !== 'hello' && !att.hello) fail('no_session');
      this.index = null;
      const body = HANDLERS[msg.op].call(this, att, msg, conn) ?? {};
      conn.setAttachment(att);
      conn.send(JSON.stringify({ re: seq, code: 'ok', ...body }));
      this.runDeferred();
      if (GROWING_OPS.has(msg.op)) this.observe();
    } catch (error) {
      this.deferred = null;
      conn.setAttachment(att);
      if (!(error instanceof LobbyError)) {
        this.host.log(`lobby: internal error in request ${seq}: ${error?.stack ?? error}`);
        conn.send(JSON.stringify({ re: seq, code: 'internal' }));
      } else {
        conn.send(JSON.stringify({ re: seq, code: error.code, ...error.body }));
      }
    } finally {
      this.current = null;
      this.index = null;
    }
  }

  // Work queued while handling a message runs after its answer went out (events follow the response).
  runDeferred() {
    while (this.deferred?.length) {
      const batch = this.deferred;
      this.deferred = null;
      for (const fn of batch) fn();
    }
  }

  // A socket closed: a seat is held for the grace period; presence ends with the account's last present socket.
  closed(conn) {
    const att = conn.attachment();
    if (!att?.hello) return;
    this.index = null;
    if (att.room) {
      const member = this.memberRow(att.room, att.member);
      if (member && member.conn === att.cid) {
        const until = this.host.now() + LIMITS.graceMs;
        this.host.sql.run('UPDATE members SET conn = NULL, grace_until = ? WHERE room = ? AND member = ?', until, att.room,
          att.member);
        this.scheduleAlarm();
      }
    }
    if (att.presence && !this.presentSockets(att.account, att.cid).length) {
      this.notifyFriends(att.account, { ev: 'friend.offline', account: att.account });
    }
    att.room = 0;
    att.hello = false;
    conn.setAttachment(att);
    this.runDeferred();
    this.index = null;
  }

  alarm() {
    this.index = null;
    this.alarmAt = null;
    const now = this.host.now();
    for (const m of this.host.sql.all('SELECT room, member FROM members WHERE grace_until IS NOT NULL AND grace_until <= ?', now)) {
      this.removeMember(m.room, m.member, 'disconnect');
    }
    this.runDeferred();
    this.index = null;
    this.observe();
  }

  // The next seat grace to end, and while anyone is connected the next usage sample.
  scheduleAlarm() {
    const grace = this.host.sql.get('SELECT MIN(grace_until) AS t FROM members WHERE grace_until IS NOT NULL')?.t ?? null;
    const sample = this.sockets().length ? sampleBucket(this.host.now()) + SAMPLE_MS : null;
    const at = grace && sample ? Math.min(grace, sample) : grace ?? sample;
    if (at && at !== this.alarmAt) {
      this.alarmAt = at;
      this.host.setAlarm(at);
    }
  }

  // ---- metrics ----
  // What is online now, from the open sockets and the room tables: a player with several connections counts once.
  liveMetrics() {
    this.index = null;
    const players = new Map();
    for (const { att } of this.sockets()) if (att.presence && !players.has(att.account)) players.set(att.account, att);
    const tally = (pick) => {
      const counts = new Map();
      for (const att of players.values()) counts.set(pick(att), (counts.get(pick(att)) ?? 0) + 1);
      return counts;
    };
    return {
      online: players.size,
      in_rooms: this.host.sql.get('SELECT COUNT(DISTINCT account) AS n FROM members WHERE conn IS NOT NULL').n,
      rooms: this.host.sql.get('SELECT COUNT(*) AS n FROM rooms').n,
      platform: tally((att) => platformCategory(att.platform)),
      version: tally((att) => versionCategory(att.version)),
      country: tally((att) => countryCode(att.country)),
    };
  }

  // Metrics never fail a lobby operation.
  track(fn) {
    try {
      fn();
    } catch (error) {
      this.host.log(`lobby: metrics error: ${error?.stack ?? error}`);
    }
  }

  observe() {
    this.track(() => this.metrics.observe(this.liveMetrics()));
    this.scheduleAlarm();
  }

  metricsSummary() {
    return this.metrics.summary(this.liveMetrics());
  }

  metricsHistory(days) {
    return this.metrics.history(days);
  }

  // ---- helpers ----
  meta(key) {
    return this.host.sql.get('SELECT v FROM meta WHERE k = ?', key)?.v ?? null;
  }

  setMeta(key, value) {
    this.host.sql.run('INSERT INTO meta (k, v) VALUES (?, ?) ON CONFLICT(k) DO UPDATE SET v = excluded.v', key, String(value));
  }

  randomHex(bytes) {
    return toHex(crypto.getRandomValues(new Uint8Array(bytes)));
  }

  randomCode(length) {
    const values = crypto.getRandomValues(new Uint8Array(length));
    return Array.from(values, (v) => CODE_ALPHABET[v % CODE_ALPHABET.length]).join('');
  }

  ipKey(ip) {
    return sha256Hex(`${this.meta('salt')}|${ipGroup(ip)}`).slice(0, 32);
  }

  // Greeted sockets; the one whose message is being handled carries its live attachment, not a stored copy.
  sockets() {
    if (!this.index) {
      this.index = [];
      for (const conn of this.host.sockets()) {
        const stored = conn.attachment();
        const att = stored && this.current && stored.cid === this.current.cid ? this.current : stored;
        if (att?.hello) this.index.push({ conn, att });
      }
    }
    return this.index;
  }

  socketByCid(cid) {
    return cid ? this.sockets().find((s) => s.att.cid === cid) ?? null : null;
  }

  presentSockets(account, exceptCid = null) {
    return this.sockets().filter((s) => s.att.account === account && s.att.presence && s.att.cid !== exceptCid);
  }

  send(conn, event) {
    try {
      conn.send(JSON.stringify(event));
    } catch {
      // A socket that is closing; its close handler runs next.
    }
  }

  later(fn) {
    (this.deferred ??= []).push(fn);
  }

  // ---- rooms ----
  roomRow(id) {
    return this.host.sql.get('SELECT * FROM rooms WHERE id = ?', id) ?? null;
  }

  memberRows(room) {
    return this.host.sql.all('SELECT * FROM members WHERE room = ? ORDER BY member', room);
  }

  memberRow(room, member) {
    return this.host.sql.get('SELECT * FROM members WHERE room = ? AND member = ?', room, member) ?? null;
  }

  roomFlags(room, count) {
    return (count >= room.slots ? room.flags | FLAG_FULL : room.flags & ~FLAG_FULL) >>> 0;
  }

  memberView(m) {
    return { id: m.member, account: m.account, name: m.name, joined: m.joined, bin: JSON.parse(m.bin), ...clientOf(m) };
  }

  fullRoom(room) {
    const members = this.memberRows(room.id);
    return {
      id: String(room.id), world: room.world, slots: room.slots, flags: this.roomFlags(room, members.length),
      owner: room.owner, code: room.code, key: room.key, members: members.map((m) => this.memberView(m)),
      ints: JSON.parse(room.ints), bin_int: JSON.parse(room.bin_int), bin_ext: JSON.parse(room.bin_ext),
      online_protocol: room.online_protocol,
    };
  }

  briefRoom(room, members) {
    const owner = members.find((m) => m.member === room.owner) ?? members[0];
    return {
      id: String(room.id), world: room.world, slots: room.slots, count: members.length,
      flags: this.roomFlags(room, members.length), code: room.code, owner_account: owner?.account ?? '0',
      owner_name: owner?.name ?? '', ints: JSON.parse(room.ints), bin_ext: JSON.parse(room.bin_ext),
      online_protocol: room.online_protocol, ...clientOf(owner),
    };
  }

  // Sends to every member of a room; members in their grace period get it later (at most heldEvents, else their seat
  // ends now).
  toRoom(roomId, event, exceptMember = 0, hold = true) {
    for (const m of this.memberRows(roomId)) {
      if (m.member === exceptMember) continue;
      if (m.conn) {
        const s = this.socketByCid(m.conn);
        if (s) this.send(s.conn, event);
      } else if (hold && m.grace_until !== null) {
        const held = JSON.parse(m.held);
        if (held.length >= LIMITS.heldEvents) {
          this.later(() => this.removeMember(roomId, m.member, 'disconnect'));
          continue;
        }
        held.push(event);
        this.host.sql.run('UPDATE members SET held = ? WHERE room = ? AND member = ?', JSON.stringify(held), roomId, m.member);
      }
    }
  }

  // The endpoint of `about` as `to` may use it: LAN candidates only reach a member behind the same public address.
  peerEvent(roomId, about, to) {
    const event = { ev: 'peer', room: String(roomId), member: about.member, account: about.account, ip: about.ip, port: about.port };
    if (about.ip && about.ip === to.ip) event.lan = JSON.parse(about.lan);
    return event;
  }

  sendPeer(roomId, about, to) {
    if (!about.port || !to.conn) return;
    const s = this.socketByCid(to.conn);
    if (s) this.send(s.conn, this.peerEvent(roomId, about, to));
  }

  removeMember(roomId, memberId, why) {
    const room = this.roomRow(roomId);
    const member = room && this.memberRow(roomId, memberId);
    if (!member) return;
    this.host.sql.run('DELETE FROM members WHERE room = ? AND member = ?', roomId, memberId);
    const rest = this.memberRows(roomId);
    if (!rest.length) {
      this.host.sql.run('DELETE FROM rooms WHERE id = ?', roomId);
      return;
    }
    const id = String(roomId);
    if (room.owner === memberId) {
      const next = (rest.find((m) => m.conn) ?? rest[0]).member;
      this.host.sql.run('UPDATE rooms SET owner = ? WHERE id = ?', next, roomId);
      this.toRoom(roomId, { ev: 'room.owner', room: id, from: memberId, to: next });
    }
    this.toRoom(roomId, { ev: 'peer.gone', room: id, member: memberId });
    this.toRoom(roomId, { ev: 'room.member_left', room: id, member: memberId, why });
  }

  requireRoom(att, roomText) {
    const roomId = Number(decimalId(roomText));
    if (att.room !== roomId) fail('not_found', 'not in that room');
    const room = this.roomRow(roomId);
    if (!room) fail('not_found');
    return room;
  }

  requireOwner(att, room) {
    if (room.owner !== att.member) fail('not_owner');
  }

  insertMember(att, roomId, memberId, bin) {
    const udp = att.udp ?? { ip: '', port: 0, lan: [] };
    this.host.sql.run(`INSERT INTO members (room, member, account, name, joined, bin, conn, ip, port, lan, version, platform)
      VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, roomId, memberId, att.account, att.name, this.host.now(), JSON.stringify(bin),
      att.cid, udp.ip, udp.port, JSON.stringify(udp.lan), att.version ?? '', att.platform ?? '');
    att.room = roomId;
    att.member = memberId;
  }

  exchangeEndpoints(roomId, joinerId) {
    const rows = this.memberRows(roomId);
    const joiner = rows.find((m) => m.member === joinerId);
    for (const other of rows) {
      if (other.member === joinerId) continue;
      this.sendPeer(roomId, other, joiner);
      this.sendPeer(roomId, joiner, other);
    }
  }

  // ---- friends ----
  accountRow(account) {
    return this.host.sql.get('SELECT * FROM accounts WHERE account = ?', account) ?? null;
  }

  areFriends(a, b) {
    return Boolean(this.host.sql.get('SELECT 1 AS x FROM friends WHERE account = ? AND friend = ?', a, b));
  }

  friendCount(account) {
    return this.host.sql.get('SELECT COUNT(*) AS n FROM friends WHERE account = ?', account).n;
  }

  friendView(account, viewerGame) {
    const row = this.accountRow(account);
    const view = { account, name: row?.name ?? '', online: false };
    const present = this.presentSockets(account);
    if (!present.length) return view;
    view.online = true;
    const inRoom = present.find((s) => s.att.room) ?? present[0];
    view.game = inRoom.att.game;
    Object.assign(view, { online_protocol: inRoom.att.online_protocol ?? 0 }, clientOf(inRoom.att));
    if (inRoom.att.room && inRoom.att.game === viewerGame) {
      const room = this.roomRow(inRoom.att.room);
      if (room) {
        view.room = String(room.id);
        view.code = room.code;
      }
    }
    const withData = present.find((s) => s.att.pdata) ?? null;
    if (withData && withData.att.game === viewerGame) {
      view.data = withData.att.pdata;
      if (withData.att.pstatus) view.status = withData.att.pstatus;
    }
    return view;
  }

  friendsOf(account) {
    return this.host.sql.all('SELECT friend FROM friends WHERE account = ? ORDER BY friend', account).map((r) => r.friend);
  }

  notifyAccount(account, event) {
    for (const s of this.sockets()) if (s.att.account === account) this.send(s.conn, event);
  }

  notifyFriends(account, event) {
    for (const friend of this.friendsOf(account)) this.notifyAccount(friend, event);
  }

  expireRequests() {
    this.host.sql.run('DELETE FROM friend_requests WHERE ms < ?', this.host.now() - LIMITS.requestDays * DAY_MS);
  }

  makeFriends(a, b) {
    const now = this.host.now();
    this.host.sql.run('INSERT OR IGNORE INTO friends (account, friend, since) VALUES (?, ?, ?)', a, b, now);
    this.host.sql.run('INSERT OR IGNORE INTO friends (account, friend, since) VALUES (?, ?, ?)', b, a, now);
    this.host.sql.run('DELETE FROM friend_requests WHERE (sender = ? AND target = ?) OR (sender = ? AND target = ?)', a, b, b, a);
  }

  requireFriendRoom(a, b) {
    if (this.friendCount(a) >= LIMITS.friends || this.friendCount(b) >= LIMITS.friends) fail('limit', 'friend list full');
  }
}

const HANDLERS = {
  hello(att, msg) {
    if (att.hello) fail('bad_request', 'already greeted');
    if (msg.proto !== PROTOCOL) {
      if (!att.protoRejected) this.track(() => this.metrics.reject('lobby_protocol'));
      att.protoRejected = true;
      fail('proto');
    }
    if (typeof msg.game !== 'string' || !/^[A-Za-z0-9._-]{1,32}$/.test(msg.game)) fail('bad_request', 'game');
    const account = decimalId(msg.account);
    if (typeof msg.secret !== 'string' || !/^[0-9a-f]{64}$/.test(msg.secret)) fail('bad_request', 'secret');
    const name = cleanText(msg.name ?? '', LIMITS.nameChars) || 'Player';
    const hash = sha256Hex(msg.secret);
    const now = this.host.now();
    let row = this.accountRow(account);
    if (!row) {
      const ipKey = this.ipKey(att.ip);
      if (!(this.local && isLocalAddress(att.ip))) {
        this.host.sql.run('DELETE FROM registrations WHERE ms < ?', now - DAY_MS);
        const recent = this.host.sql.get('SELECT COUNT(*) AS n FROM registrations WHERE ip = ?', ipKey).n;
        if (recent >= LIMITS.registrationsPerIpPerDay) fail('limit', 'too many new accounts');
      }
      row = this.registerAccount(account, hash, name);
      this.host.sql.run('INSERT INTO registrations (ip, ms) VALUES (?, ?)', ipKey, now);
    } else if (row.secret_hash !== hash) {
      fail('unauthorized');
    } else {
      this.host.sql.run('UPDATE accounts SET name = ?, seen = ? WHERE account = ?', name, now, account);
    }
    const presence = msg.presence !== false;
    const wasOnline = presence && this.presentSockets(account).length > 0;
    const online = isInt(msg.online_protocol, 1, 0xffff) ? msg.online_protocol : 0;
    const version = cleanText(msg.version ?? '', LIMITS.versionChars);
    const platform = cleanText(msg.platform ?? '', LIMITS.platformChars);
    Object.assign(att, { hello: true, account, name, presence, game: msg.game, room: 0, member: 0, online_protocol: online,
      version, platform });
    this.index = null;
    if (presence && !wasOnline) this.notifyFriends(account, { ev: 'friend.online', account, name });
    if (presence) this.track(() => this.metrics.session(att));
    return { account, name, friend_code: formatFriendCode(row.code), ip: att.ip, time: now };
  },

  worlds(att) {
    const rooms = this.host.sql.all('SELECT id FROM rooms WHERE game = ?', att.game);
    let members = 0;
    for (const r of rooms) members += this.host.sql.get('SELECT COUNT(*) AS n FROM members WHERE room = ?', r.id).n;
    return { worlds: [{ world: 1, rooms: rooms.length, members }] };
  },

  udp(att, msg) {
    if (!isInt(msg.port, 1, 0xffff)) fail('bad_request', 'port');
    const lanIn = msg.lan ?? [];
    if (!Array.isArray(lanIn) || lanIn.length > LIMITS.lanCandidates) fail('bad_request', 'lan');
    const lan = [];
    for (const c of lanIn) {
      if (!isObject(c) || !isInt(c.port, 1, 0xffff)) fail('bad_request', 'lan');
      if (['private', 'loopback', 'link'].includes(ipv4Kind(c.ip))) lan.push({ ip: c.ip, port: c.port });
    }
    // Peers are only ever sent toward the address this socket comes from: a client cannot point them elsewhere.
    att.udp = { ip: att.ip, port: msg.port, lan };
    if (att.room) {
      this.host.sql.run('UPDATE members SET ip = ?, port = ?, lan = ? WHERE room = ? AND member = ?', att.ip, msg.port,
        JSON.stringify(lan), att.room, att.member);
      const rows = this.memberRows(att.room);
      const me = rows.find((m) => m.member === att.member);
      if (me) for (const other of rows) if (other.member !== att.member) this.sendPeer(att.room, me, other);
    }
    return { ip: att.ip, port: msg.port };
  },

  'room.create'(att, msg) {
    if (att.room) fail('in_room');
    if (!isInt(msg.world, 1, 0xffff) || !isInt(msg.slots, 1, MEMBER_IDS.length)) fail('bad_request', 'room');
    const flags = uint32(msg.flags ?? 0);
    const ints = merge([], intAttrs(msg.ints), LIMITS.intAttrsPerRoom);
    const binInt = merge([], binAttrs(msg.bin_int), LIMITS.binAttrsPerArray);
    const binExt = merge([], binAttrs(msg.bin_ext), LIMITS.binAttrsPerArray);
    const memberBin = binAttrs(msg.member_bin);
    const last = Number(this.meta('room_id') ?? FIRST_ROOM_ID - 1);
    const id = last + 1;
    this.setMeta('room_id', id);
    let code;
    do code = this.randomCode(6);
    while (this.host.sql.get('SELECT 1 AS x FROM rooms WHERE code = ?', code));
    this.host.sql.run(`INSERT INTO rooms (id, game, world, slots, flags, owner, code, key, ints, bin_int, bin_ext, created,
      online_protocol) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, id, att.game, msg.world, msg.slots,
      (flags & ~FLAG_FULL) >>> 0, MEMBER_IDS[0], code, this.randomHex(16), JSON.stringify(ints), JSON.stringify(binInt),
      JSON.stringify(binExt), this.host.now(), att.online_protocol ?? 0);
    this.insertMember(att, id, MEMBER_IDS[0], memberBin);
    this.track(() => this.metrics.count('rooms_created'));
    return { room: this.fullRoom(this.roomRow(id)), me: MEMBER_IDS[0] };
  },

  'room.search'(att, msg) {
    const from = isInt(msg.from, 1, 0x7fffffff) ? msg.from : 1;
    const count = isInt(msg.count, 1, LIMITS.searchMax) ? msg.count : LIMITS.searchMax;
    let rooms;
    if (msg.code !== undefined) {
      if (typeof msg.code !== 'string') fail('bad_request', 'code');
      rooms = this.host.sql.all('SELECT * FROM rooms WHERE game = ? AND code = ?', att.game, msg.code.toUpperCase().trim())
        .filter((room) => !(room.flags & FLAG_CLOSED));
    } else {
      if (!isInt(msg.world, 0, 0xffff)) fail('bad_request', 'world');
      const mask = uint32(msg.flag_mask ?? 0);
      const value = uint32(msg.flag_value ?? 0);
      const filters = msg.filters ?? [];
      if (!Array.isArray(filters)) fail('bad_request', 'filters');
      const checks = filters.slice(0, LIMITS.filters).map((f) => {
        if (!Array.isArray(f) || f.length !== 3 || !isInt(f[0], 0, 0xffff) || !isInt(f[1], 1, 6)) fail('bad_request', 'filter');
        return [f[0], f[1], uint32(f[2])];
      });
      rooms = this.host.sql.all('SELECT * FROM rooms WHERE game = ? AND (? = 0 OR world = ?) ORDER BY id', att.game,
        msg.world, msg.world).filter((room) => {
        if (room.flags & (FLAG_CLOSED | FLAG_HIDDEN)) return false;
        if (((room.flags & mask) >>> 0) !== ((value & mask) >>> 0)) return false;
        const ints = new Map(JSON.parse(room.ints));
        return checks.every(([id, op, want]) => {
          const have = ints.get(id) ?? 0;
          return [have === want, have !== want, have < want, have <= want, have > want, have >= want][op - 1];
        });
      });
    }
    const open = [];
    let incompatible = 0;
    for (const room of rooms) {
      const members = this.memberRows(room.id);
      if (members.length >= room.slots) continue;
      if (compatible(att, room)) open.push(this.briefRoom(room, members));
      else incompatible++;
    }
    return { total: open.length, from, rooms: open.slice(from - 1, from - 1 + count), incompatible };
  },

  'room.join'(att, msg) {
    if (att.room) fail('in_room');
    let room;
    if (msg.code !== undefined) {
      if (typeof msg.code !== 'string') fail('bad_request', 'code');
      room = this.host.sql.get('SELECT * FROM rooms WHERE code = ?', msg.code.toUpperCase().trim()) ?? null;
    } else {
      room = this.roomRow(Number(decimalId(msg.room)));
    }
    if (!room || room.game !== att.game) fail('not_found');
    if (!compatible(att, room)) {
      this.track(() => this.metrics.reject('online_protocol'));
      const owner = this.memberRow(room.id, room.owner);
      fail('incompatible', '', { online_protocol: room.online_protocol, ...clientOf(owner) });
    }
    const members = this.memberRows(room.id);
    if (members.length >= room.slots) fail('full');
    if (room.flags & FLAG_CLOSED) fail('closed');
    const taken = new Set(members.map((m) => m.member));
    const memberId = MEMBER_IDS.slice(0, room.slots).find((id) => !taken.has(id));
    if (!memberId) fail('full');
    this.insertMember(att, room.id, memberId, binAttrs(msg.member_bin));
    const joined = this.memberView(this.memberRow(room.id, memberId));
    this.toRoom(room.id, { ev: 'room.member_joined', room: String(room.id), member: joined }, memberId);
    const roomId = room.id;
    this.later(() => this.exchangeEndpoints(roomId, memberId));
    this.track(() => this.metrics.count('room_joins'));
    return { room: this.fullRoom(this.roomRow(room.id)), me: memberId };
  },

  'room.leave'(att, msg) {
    if (!att.room || Number(decimalId(msg.room)) !== att.room) return {};
    const { room, member } = att;
    att.room = 0;
    att.member = 0;
    this.removeMember(room, member, 'leave');
    return {};
  },

  // Retakes a seat held after a lost connection (LIMITS.graceMs); the events held for it follow the answer.
  'room.resume'(att, msg, conn) {
    if (att.room) fail('in_room');
    const roomId = Number(decimalId(msg.room));
    if (!isInt(msg.member, 1, 0xffff)) fail('bad_request', 'member');
    const member = this.memberRow(roomId, msg.member);
    if (!member || member.account !== att.account) fail('not_found');
    const old = this.socketByCid(member.conn);
    if (old && old.att.cid !== att.cid) {
      old.att.room = 0;
      old.conn.setAttachment(old.att);
      try {
        old.conn.close(4001, 'seat taken by a new connection');
      } catch {
        // Already closing.
      }
    } else if (member.grace_until === null && member.conn !== null) {
      fail('not_found');
    }
    const udp = att.udp ?? { ip: '', port: 0, lan: [] };
    this.host.sql.run(`UPDATE members SET conn = ?, grace_until = NULL, held = '[]', name = ?, ip = ?, port = ?, lan = ?
      WHERE room = ? AND member = ?`, att.cid, att.name, udp.ip, udp.port, JSON.stringify(udp.lan), roomId, msg.member);
    att.room = roomId;
    att.member = msg.member;
    const held = JSON.parse(member.held);
    this.later(() => {
      for (const event of held) this.send(conn, event);
    });
    return { room: this.fullRoom(this.roomRow(roomId)), me: msg.member };
  },

  'room.set_internal'(att, msg) {
    const room = this.requireRoom(att, msg.room);
    this.requireOwner(att, room);
    const mask = uint32(msg.flag_mask ?? 0);
    const value = uint32(msg.flag_value ?? 0);
    const changed = binAttrs(msg.bin_int);
    const binInt = merge(JSON.parse(room.bin_int), changed, LIMITS.binAttrsPerArray);
    const flags = (((room.flags & ~mask) | (value & mask)) & ~FLAG_FULL) >>> 0;
    this.host.sql.run('UPDATE rooms SET flags = ?, bin_int = ? WHERE id = ?', flags, JSON.stringify(binInt), room.id);
    const count = this.memberRows(room.id).length;
    this.toRoom(room.id, { ev: 'room.internal', room: String(room.id), flags: this.roomFlags({ ...room, flags }, count),
      bin_int: changed }, att.member);
    return {};
  },

  'room.set_external'(att, msg) {
    const room = this.requireRoom(att, msg.room);
    this.requireOwner(att, room);
    const ints = merge(JSON.parse(room.ints), intAttrs(msg.ints), LIMITS.intAttrsPerRoom);
    const binExt = merge(JSON.parse(room.bin_ext), binAttrs(msg.bin_ext), LIMITS.binAttrsPerArray);
    this.host.sql.run('UPDATE rooms SET ints = ?, bin_ext = ? WHERE id = ?', JSON.stringify(ints), JSON.stringify(binExt), room.id);
    return {};
  },

  'room.kick'(att, msg) {
    const room = this.requireRoom(att, msg.room);
    this.requireOwner(att, room);
    if (!isInt(msg.member, 1, 0xffff) || msg.member === att.member) fail('bad_request', 'member');
    const target = this.memberRow(room.id, msg.member);
    if (!target) fail('not_found');
    const s = this.socketByCid(target.conn);
    if (s) {
      s.att.room = 0;
      s.att.member = 0;
      s.conn.setAttachment(s.att);
      this.send(s.conn, { ev: 'room.kicked', room: String(room.id) });
    }
    this.removeMember(room.id, msg.member, 'kicked');
    return {};
  },

  // Room chat: never stored, never held for a seat in its grace period.
  chat(att, msg) {
    if (!att.room) fail('not_found', 'not in a room');
    const bucket = takeToken(att.rc, BUCKETS.chat, this.host.now());
    if (!bucket) fail('limit', 'slow down');
    att.rc = bucket;
    const text = cleanText(msg.text, LIMITS.chatChars);
    if (!text) fail('bad_request', 'empty');
    const roomId = att.room;
    const event = { ev: 'chat', room: String(roomId), member: att.member, account: att.account, name: att.name, text };
    this.later(() => this.toRoom(roomId, event, 0, false));
    return {};
  },

  'presence.set'(att, msg) {
    const data = hexBytes(msg.data ?? '', LIMITS.presenceDataBytes);
    const status = hexBytes(msg.status ?? '', LIMITS.presenceStatusBytes);
    att.pdata = data || undefined;
    att.pstatus = data && status ? status : undefined;
    return {};
  },

  'friends.list'(att) {
    return { friends: this.friendsOf(att.account).map((f) => this.friendView(f, att.game)) };
  },

  'friends.requests'(att) {
    this.expireRequests();
    const named = (rows, key) => rows.map((r) => ({ account: r[key], name: this.accountRow(r[key])?.name ?? '', ms: r.ms }));
    return {
      incoming: named(this.host.sql.all('SELECT sender, ms FROM friend_requests WHERE target = ? ORDER BY ms', att.account), 'sender'),
      outgoing: named(this.host.sql.all('SELECT target, ms FROM friend_requests WHERE sender = ? ORDER BY ms', att.account), 'target'),
    };
  },

  'friends.add'(att, msg) {
    const code = normalizeFriendCode(msg.code);
    const target = this.host.sql.get('SELECT * FROM accounts WHERE code = ?', code);
    if (!target) fail('not_found');
    if (target.account === att.account) fail('bad_request', 'own code');
    const result = { account: target.account, name: target.name };
    if (this.areFriends(att.account, target.account)) return { ...result, state: 'friends' };
    this.expireRequests();
    if (this.host.sql.get('SELECT 1 AS x FROM friend_requests WHERE sender = ? AND target = ?', target.account, att.account)) {
      this.requireFriendRoom(att.account, target.account);
      this.makeFriends(att.account, target.account);
      this.notifyAccount(target.account, { ev: 'friend.added', friend: this.friendView(att.account, att.game) });
      return { ...result, state: 'friends' };
    }
    if (this.host.sql.get('SELECT 1 AS x FROM friend_requests WHERE sender = ? AND target = ?', att.account, target.account)) {
      return { ...result, state: 'pending' };
    }
    const outgoing = this.host.sql.get('SELECT COUNT(*) AS n FROM friend_requests WHERE sender = ?', att.account).n;
    const incoming = this.host.sql.get('SELECT COUNT(*) AS n FROM friend_requests WHERE target = ?', target.account).n;
    if (outgoing >= LIMITS.pendingOut || incoming >= LIMITS.pendingIn) fail('limit', 'too many pending requests');
    this.requireFriendRoom(att.account, target.account);
    this.host.sql.run('INSERT INTO friend_requests (sender, target, ms) VALUES (?, ?, ?)', att.account, target.account, this.host.now());
    this.notifyAccount(target.account, { ev: 'friend.request', account: att.account, name: att.name });
    return { ...result, state: 'pending' };
  },

  'friends.accept'(att, msg) {
    const sender = decimalId(msg.account);
    this.expireRequests();
    if (!this.host.sql.get('SELECT 1 AS x FROM friend_requests WHERE sender = ? AND target = ?', sender, att.account)) fail('not_found');
    this.requireFriendRoom(att.account, sender);
    this.makeFriends(att.account, sender);
    this.notifyAccount(sender, { ev: 'friend.added', friend: this.friendView(att.account, att.game) });
    return { friend: this.friendView(sender, att.game) };
  },

  'friends.decline'(att, msg) {
    const other = decimalId(msg.account);
    const before = this.host.sql.get(`SELECT COUNT(*) AS n FROM friend_requests WHERE (sender = ? AND target = ?)
      OR (sender = ? AND target = ?)`, other, att.account, att.account, other).n;
    if (!before) fail('not_found');
    this.host.sql.run('DELETE FROM friend_requests WHERE (sender = ? AND target = ?) OR (sender = ? AND target = ?)', other,
      att.account, att.account, other);
    return {};
  },

  'friends.remove'(att, msg) {
    const other = decimalId(msg.account);
    const were = this.areFriends(att.account, other);
    this.host.sql.run('DELETE FROM friends WHERE (account = ? AND friend = ?) OR (account = ? AND friend = ?)', att.account,
      other, other, att.account);
    this.host.sql.run('DELETE FROM friend_requests WHERE (sender = ? AND target = ?) OR (sender = ? AND target = ?)', other,
      att.account, att.account, other);
    if (were) this.notifyAccount(other, { ev: 'friend.removed', account: att.account });
    return {};
  },

  // Bug reports for the deployment's maintainer; the address is kept only as a salted hash for the limits.
  'report.send'(att, msg) {
    const text = cleanText(msg.text, LIMITS.reportChars);
    if (!text) fail('bad_request', 'empty');
    if (typeof msg.log !== 'string') fail('bad_request', 'log');
    const log = msg.log.slice(-LIMITS.reportLogChars);
    const version = cleanText(msg.version ?? '', LIMITS.reportVersionChars);
    const now = this.host.now();
    const since = now - DAY_MS;
    const ip = this.ipKey(att.ip);
    const mine = this.host.sql.get('SELECT COUNT(*) AS n FROM reports WHERE account = ? AND ms >= ?', att.account, since).n;
    const fromIp = this.host.sql.get('SELECT COUNT(*) AS n FROM reports WHERE ip = ? AND ms >= ?', ip, since).n;
    const all = this.host.sql.get('SELECT COUNT(*) AS n FROM reports WHERE ms >= ?', since).n;
    if (mine >= LIMITS.reportsPerAccountPerDay || fromIp >= LIMITS.reportsPerIpPerDay || all >= LIMITS.reportsPerDay) {
      fail('limit', 'report limit');
    }
    this.host.sql.run('INSERT INTO reports (ms, account, ip, version, text, log) VALUES (?, ?, ?, ?, ?, ?)', now, att.account,
      ip, version, text, log);
    const id = this.host.sql.get('SELECT MAX(id) AS id FROM reports').id;
    this.host.sql.run('DELETE FROM reports WHERE id <= ?', id - LIMITS.reportsKept);
    this.host.sql.run(`UPDATE reports SET ip = '' WHERE ms < ?`, since);
    return { id, left: LIMITS.reportsPerAccountPerDay - mine - 1 };
  },
};

// Reports for a deployment's maintainer (worker.js serves them behind REPORTS_TOKEN).
export function listReports(sql, limit = 100) {
  return sql.all('SELECT id, ms, account, version, text, log FROM reports ORDER BY id DESC LIMIT ?', limit);
}
