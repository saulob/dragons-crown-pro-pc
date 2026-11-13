// The lobby: accounts, friends, presence, rooms and the endpoint exchange (docs/ONLINE.md). Game traffic passes through
// it only as relay frames (relay.js). State lives in SQL tables and in each socket's attachment, so a hibernated
// Durable Object resumes as if it had never stopped; the same class runs in the local development server
// (local/server.js).
import { PROTECTION, RateLimiter } from './abuse.js';
import { ipGroup } from './edge.js';
import { INCIDENT, INCIDENT_LIMITED, IncidentDesk, readClaim, readClaims, readIncident } from './incidents.js';
import { CAPABILITIES, Relay } from './relay.js';
import { parsePartyReport } from './party.js';
import { Metrics, countryCode, platformBit, platformCategory, sampleBucket, versionCategory, SAMPLE_MS } from './metrics.js';
import { sha256Hex, toHex } from './sha256.js';
import { STORY_ROLES, STORY_SEEN_PER_ROOM, StoryUsage, storyRole } from './story_usage.js';

export const PROTOCOL = 1;

export const LIMITS = {
  frameBytes: 32 * 1024,
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
  versionChars: 16,
  platformChars: 16,
  lobbyPlayers: 100,
  // Three missed keepalives (LobbyLink::Options::keepalive_ms) and a margin.
  lobbyStaleMs: 75000,
};

const BUCKETS = {
  any: { burst: 60, perSecond: 20 },
  room: { burst: 30, perSecond: 1 },
  chat: { burst: 5, perSecond: 1 },
};
// Each kind is shared by a socket (kept in its attachment, so hibernation does not refill it), an account (a reconnect
// does not refill it) and an address group (every account behind it).
const WRITE_BUDGETS = {
  state: { socket: 'wstate', account: 'stateWriteAccount', address: 'stateWriteAddress' },
  friend: { socket: 'wfriend', account: 'friendWriteAccount', address: 'friendWriteAddress' },
};
const ROOM_OPS = new Set(['room.create', 'room.join', 'room.leave', 'room.search', 'room.resume']);
// A receipt is the whole proof for these, so looking up a report never involves an account.
const NO_SESSION_OPS = new Set(['hello', 'incident.status', 'incident.read']);
// Operations after which the live counts can have grown.
const GROWING_OPS = new Set(['hello', 'room.create', 'room.join', 'room.resume']);

export const FLAG_CLOSED = 0x40000000;
export const FLAG_FULL = 0x20000000;
export const FLAG_HIDDEN = 0x10000000;
export const STORY_SESSION_ATTR = 0x7f01;

const CODE_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
const DAY_MS = 24 * 3600 * 1000;
const MEMBER_IDS = [0x10, 0x20, 0x30, 0x40];
const FIRST_ROOM_ID = 4096;
// What net::online_name produces: the public list shows no text a real launcher could not have sent.
const LOBBY_NAME = /^[A-Za-z0-9_-]{1,16}$/;
const LOBBY_PLAYER = /^[0-9a-f]{32}$/;
const LOBBY_REV = /^[0-9a-f]{16}$/;
const LOBBY_VERSION = /^[0-9A-Za-z][0-9A-Za-z.+-]{0,15}$/;
const LOBBY_RANK = { incoming: 0, pending: 1, friend: 2, none: 3 };
const DEFAULT_NAME = 'Player';
const UNKNOWN_OP = 'unknown op';

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
];

// Applied once each, in order; meta 'schema' counts the ones done.
const MIGRATIONS = [
  [`ALTER TABLE rooms ADD COLUMN online_protocol INTEGER NOT NULL DEFAULT 0`,
    `ALTER TABLE members ADD COLUMN version TEXT NOT NULL DEFAULT ''`,
    `ALTER TABLE members ADD COLUMN platform TEXT NOT NULL DEFAULT ''`],
  [`ALTER TABLE accounts ADD COLUMN session INTEGER NOT NULL DEFAULT 0`],
  [`ALTER TABLE rooms ADD COLUMN peak_members INTEGER NOT NULL DEFAULT 0`,
    `ALTER TABLE rooms ADD COLUMN relayed INTEGER NOT NULL DEFAULT 0`],
  [`ALTER TABLE members ADD COLUMN public_ip TEXT NOT NULL DEFAULT ''`],
  [`ALTER TABLE rooms ADD COLUMN platforms INTEGER NOT NULL DEFAULT 0`],
  [`ALTER TABLE rooms ADD COLUMN story_seen TEXT NOT NULL DEFAULT '[]'`],
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

// A story session room carries STORY_SESSION_ATTR > 0; only clients that declared story_session may host or join one.
function isStorySession(ints) {
  return ints.some(([id, value]) => id === STORY_SESSION_ATTR && value > 0);
}

const roomIsStory = (room) => isStorySession(JSON.parse(room.ints));

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

const isLocalAddress = (ip) => ip === '::1' || ipv4Kind(ip) === 'loopback';
const memberEndpoint = (att) => ({ ip: '', public_ip: '', port: 0, lan: [], ...att.udp });

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
  // host: { sql: { all(q, ...a), get(q, ...a), run(q, ...a), transaction(work) }, sockets(): Conn[], now(): ms, setAlarm(ms), log(text) }
  // Conn: { send(text), close(code, reason), attachment(), setAttachment(object), keepaliveAt(): ms }
  constructor(host, options = {}) {
    this.host = host;
    this.local = Boolean(options.local);
    for (const statement of SCHEMA) host.sql.run(statement);
    for (let done = Number(this.meta('schema') ?? 0); done < MIGRATIONS.length; done++) {
      for (const statement of MIGRATIONS[done]) host.sql.run(statement);
      this.setMeta('schema', done + 1);
    }
    if (!this.meta('salt')) this.setMeta('salt', this.randomHex(16));
    this.salt = this.meta('salt');
    this.metrics = new Metrics(host.sql, () => host.now());
    this.story = new StoryUsage(host.sql, () => host.now());
    if (!this.meta('story_since')) this.setMeta('story_since', host.now());
    this.incidents = new IncidentDesk(host.sql, this.meta('salt'));
    this.alarmAt = options.alarmAt ?? null;
    this.guards = {
      hello: new RateLimiter(PROTECTION.hello),
      roomCreateAccount: new RateLimiter(PROTECTION.roomCreateAccount),
      roomCreateAddress: new RateLimiter(PROTECTION.roomCreateAddress),
      roomJoinAccount: new RateLimiter(PROTECTION.roomJoinAccount),
      publicEndpoint: new RateLimiter(PROTECTION.publicEndpoint),
      partyReportAll: new RateLimiter(PROTECTION.partyReportAll),
      partyReportAccount: new RateLimiter(PROTECTION.partyReportAccount),
      partyReportAddress: new RateLimiter(PROTECTION.partyReportAddress),
      incidentAll: new RateLimiter(PROTECTION.incidentAll),
      incidentAccount: new RateLimiter(PROTECTION.incidentAccount),
      incidentAddress: new RateLimiter(PROTECTION.incidentAddress),
      incidentLookupAll: new RateLimiter(PROTECTION.incidentLookupAll),
      incidentStatusAddress: new RateLimiter(PROTECTION.incidentStatusAddress),
      incidentReadAddress: new RateLimiter(PROTECTION.incidentReadAddress),
      lobbyPublishAccount: new RateLimiter(PROTECTION.lobbyPublishAccount),
      lobbyListAccount: new RateLimiter(PROTECTION.lobbyListAccount),
      lobbyListAddress: new RateLimiter(PROTECTION.lobbyListAddress),
      lobbyRequestAccount: new RateLimiter(PROTECTION.lobbyRequestAccount),
      stateWriteAccount: new RateLimiter(PROTECTION.stateWriteAccount),
      stateWriteAddress: new RateLimiter(PROTECTION.stateWriteAddress),
      friendWriteAccount: new RateLimiter(PROTECTION.friendWriteAccount),
      friendWriteAddress: new RateLimiter(PROTECTION.friendWriteAddress),
      rejects: new RateLimiter({ burst: 1, perSecond: 1000 / PROTECTION.rejectWindowMs }),
    };
    this.relays = new Relay(this);
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

  registerAccount(account, hash, name, session = 0) {
    let code;
    do code = this.randomCode(8);
    while (this.host.sql.get('SELECT 1 AS x FROM accounts WHERE code = ?', code));
    const now = this.host.now();
    this.host.sql.run(`INSERT INTO accounts (account, secret_hash, code, name, created, seen, session)
      VALUES (?, ?, ?, ?, ?, ?, ?)`, account, hash, code, name, now, now, session ? now : 0);
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
      if (open >= LIMITS.socketsPerIp) {
        this.diagnose('ws_upgrade_ip_cap');
        return null;
      }
    }
    this.diagnose('ws_upgrade_accepted');
    return { cid: this.randomHex(8), ip, opened: this.host.now(), country: countryCode(country) };
  }

  // Upgrades the Worker refused before they reached this object.
  edgeLimited(count) {
    if (count > 0) this.diagnose('ws_upgrade_limited', count);
  }

  message(conn, text) {
    const att = conn.attachment();
    if (!att || att.abusive) return;
    this.diagnose('ws_text_messages');
    if (!att.hello && !this.beforeHello(conn, att)) return;
    let seq = null;
    this.current = att;
    try {
      if (typeof text !== 'string') fail('bad_request', 'text frames only');
      if (text.length > INCIDENT.frameChars) fail('too_large');
      let msg;
      try {
        msg = JSON.parse(text);
      } catch {
        fail('bad_request', 'invalid JSON');
      }
      if (!isObject(msg) || typeof msg.op !== 'string' || !isInt(msg.seq, 0, 0x7fffffff)) fail('bad_request', 'envelope');
      seq = msg.seq;
      if (text.length > LIMITS.frameBytes && msg.op !== 'incident.file') fail('too_large');
      const now = this.host.now();
      const any = takeToken(att.ra, BUCKETS.any, now);
      if (!any) this.slowDown();
      att.ra = any;
      if (att.lobby) att.lobby.seen = now;
      if (ROOM_OPS.has(msg.op)) {
        const room = takeToken(att.rr, BUCKETS.room, now);
        if (!room) this.slowDown();
        att.rr = room;
      }
      if (!Object.hasOwn(HANDLERS, msg.op)) fail('bad_request', UNKNOWN_OP);
      if (!NO_SESSION_OPS.has(msg.op) && !att.hello) fail('no_session');
      this.index = null;
      const body = HANDLERS[msg.op].call(this, att, msg, conn, text) ?? {};
      conn.setAttachment(att);
      conn.send(JSON.stringify({ re: seq, code: 'ok', ...body }));
      this.runDeferred();
      if (GROWING_OPS.has(msg.op)) this.observe();
    } catch (error) {
      this.deferred = null;
      const abusive = error instanceof LobbyError && this.strike(att, error, seq);
      conn.setAttachment(att);
      if (!(error instanceof LobbyError)) {
        this.host.log(`lobby: internal error in request ${seq}: ${error?.stack ?? error}`);
        conn.send(JSON.stringify({ re: seq, code: 'internal' }));
      } else {
        conn.send(JSON.stringify({ re: seq, code: error.code, ...error.body }));
      }
      if (abusive) this.closeAbusive(conn, att);
    } finally {
      this.current = null;
      this.index = null;
    }
  }

  // One binary message: a relay frame, forwarded to a member of the sender's own room (relay.js).
  relay(conn, bytes) {
    this.relays.frame(conn, bytes);
  }

  slowDown() {
    this.diagnose('ws_control_limited');
    fail('limit', 'slow down');
  }

  // A refused or malformed request costs its sender nothing, so a socket that keeps producing them loses its connection.
  // True when this one was the last it gets.
  strike(att, error, seq) {
    const counted = error.code === 'limit' || error.code === 'too_large' || seq === null || error.message === UNKNOWN_OP;
    if (!counted) return false;
    const left = takeToken(att.strikes, PROTECTION.refusedRequests, this.host.now());
    if (left) att.strikes = left;
    return !left;
  }

  // Counted in the attachment, so a socket that never signs in cannot outlast the limit through hibernation. True while
  // the frame may be handled.
  beforeHello(conn, att) {
    att.early = (att.early ?? 0) + 1;
    if (att.early > PROTECTION.preHelloFrames) {
      this.closeAbusive(conn, att, 'ws_preauth_closed', 'sign in first');
      return false;
    }
    conn.setAttachment(att);
    return true;
  }

  // Nothing the socket sends afterwards is handled, even what the runtime still delivers.
  closeAbusive(conn, att, diagnostic = 'lobby_abuse_closed', reason = 'too many refused requests') {
    att.abusive = true;
    conn.setAttachment(att);
    this.diagnose(diagnostic);
    try {
      conn.close(1008, reason);
    } catch {
      // Already closing.
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
    if (att) this.relays.forgetConnection(att.cid);
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
    this.track(() => this.metrics.flushDiagnostics());
    this.observe();
  }

  // The next seat grace to end, and while anyone is connected the next usage sample.
  scheduleAlarm() {
    const grace = this.host.sql.get('SELECT MIN(grace_until) AS t FROM members WHERE grace_until IS NOT NULL')?.t ?? null;
    const sample = this.sockets().length || this.metrics.pending.size ? sampleBucket(this.host.now()) + SAMPLE_MS : null;
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

  diagnose(name, amount = 1) {
    if (!this.metrics.diagnose(name, amount)) return;
    this.scheduleAlarm();
    this.index = null;
  }

  // One rejection per reason and key per window: a reconnect loop must not write on every turn.
  countReject(reason, key) {
    if (this.guards.rejects.take(`${reason}|${key}`, this.host.now())) this.track(() => this.metrics.reject(reason));
  }

  markRelayed(roomId) {
    this.track(() => this.host.sql.run('UPDATE rooms SET relayed = 1 WHERE id = ? AND relayed = 0', roomId));
  }

  exempt(att) {
    return this.local && isLocalAddress(att.ip);
  }

  observe() {
    this.track(() => this.metrics.observe(this.liveMetrics()));
    this.scheduleAlarm();
  }

  metricsSummary() {
    return { ...this.metrics.summary(this.liveMetrics()), story: this.storySummary() };
  }

  // Early Story Online as the dashboard shows it: rooms and seats from the live tables, history from the counters.
  storySummary() {
    const now = this.host.now();
    const seats = new Map();
    for (const m of this.host.sql.all('SELECT room, member, account, name FROM members WHERE conn IS NOT NULL')) {
      if (!seats.has(m.room)) seats.set(m.room, []);
      seats.get(m.room).push(m);
    }
    const live = { rooms: 0, parties: 0, waiting: 0, players: 0 };
    const active = new Map();
    for (const room of this.host.sql.all('SELECT id, owner, ints, created FROM rooms')) {
      if (!roomIsStory(room)) continue;
      const connected = seats.get(room.id) ?? [];
      live.rooms += 1;
      live.players += connected.length;
      if (connected.length > 1) live.parties += 1;
      else if (connected.length === 1) live.waiting += 1;
      for (const m of connected) {
        active.set(m.account, { role: m.member === room.owner ? 'Host' : 'Guest', members: connected.length,
          age_ms: Math.max(0, now - room.created) });
      }
    }
    const used = this.story.totals();
    const counted = this.metrics.storyTotals();
    return {
      since: Number(this.meta('story_since')),
      live,
      totals: { rooms_created: counted.rooms_created, guest_joins: counted.guest_joins, sessions: used.sessions,
        users: used.users },
      users: this.story.users(active.keys()).map((u) => ({ name: u.name, account: u.account, role: storyRole(u),
        sessions: u.hosted + u.guested, last_used: u.last_used, active: active.get(u.account) ?? null })),
    };
  }

  // One account's first seat in this story room, as host or guest: counted once however it leaves, returns or resumes.
  noteStoryParticipation(roomId, att, column) {
    this.track(() => {
      const seen = JSON.parse(this.host.sql.get('SELECT story_seen FROM rooms WHERE id = ?', roomId)?.story_seen ?? '[]');
      if (seen.includes(att.account) || seen.length >= STORY_SEEN_PER_ROOM) return;
      this.host.sql.run('UPDATE rooms SET story_seen = ? WHERE id = ?', JSON.stringify([...seen, att.account]), roomId);
      this.story.record(att.account, att.name, column);
    });
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

  // The endpoint of `about` as `to` may use it. `ip` is the observed address, which alone decides who gets LAN candidates;
  // `public_ip` is unverified, so it is only ever something to dial.
  peerEvent(roomId, about, to) {
    const event = { ev: 'peer', room: String(roomId), member: about.member, account: about.account,
      ip: about.public_ip || about.ip, port: about.port };
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
    this.relays.forgetMember(roomId, memberId);
    const rest = this.memberRows(roomId);
    if (!rest.length) {
      this.track(() => this.metrics.roomClosed({ peak: room.peak_members, lifetimeMs: this.host.now() - room.created,
        relayed: room.relayed === 1, platforms: room.platforms }));
      this.host.sql.run('DELETE FROM rooms WHERE id = ?', roomId);
      this.relays.forgetRoom(roomId);
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
    const udp = memberEndpoint(att);
    this.host.sql.run(`INSERT INTO members (room, member, account, name, joined, bin, conn, ip, public_ip, port, lan, version,
      platform) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, roomId, memberId, att.account, att.name, this.host.now(),
      JSON.stringify(bin), att.cid, udp.ip, udp.public_ip, udp.port, JSON.stringify(udp.lan), att.version ?? '', att.platform ?? '');
    att.room = roomId;
    att.member = memberId;
  }

  exchangeEndpoints(roomId, joinerId) {
    const rows = this.memberRows(roomId);
    const joiner = rows.find((m) => m.member === joinerId);
    // A seat can be taken before its first report, and its socket always knows the observed address the LAN rule needs.
    const receiver = { ...joiner, ip: this.socketByCid(joiner.conn)?.att.ip ?? joiner.ip };
    for (const other of rows) {
      if (other.member === joinerId) continue;
      this.sendPeer(roomId, other, receiver);
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
    const view = { account, name: row?.name ?? '', online: false, ...this.shownAs(account, row?.name) };
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

  // Counted after the secret matched, so nobody can lock out another account by guessing at it.
  limitHello(att, account, now) {
    if (this.exempt(att) || this.guards.hello.take(account, now)) return;
    this.diagnose('hello_limited');
    fail('limit', 'too many sign-ins');
  }

  // A reconnect rewrites the account only when something changed or the last write is old.
  touchAccount(row, name, now, newSession) {
    if (newSession) {
      this.host.sql.run('UPDATE accounts SET name = ?, seen = ?, session = ? WHERE account = ?', name, now, now, row.account);
    } else if (row.name !== name || now - row.seen >= PROTECTION.seenRefreshMs) {
      this.host.sql.run('UPDATE accounts SET name = ?, seen = ? WHERE account = ?', name, now, row.account);
    }
  }

  limitRoomCreate(att) {
    if (this.exempt(att)) return;
    const now = this.host.now();
    const address = ipGroup(att.ip);
    const { roomCreateAccount, roomCreateAddress } = this.guards;
    if (!roomCreateAccount.allows(att.account, now) || !roomCreateAddress.allows(address, now)) {
      this.diagnose('room_create_limited');
      fail('limit', 'creating rooms too fast');
    }
    roomCreateAccount.take(att.account, now);
    roomCreateAddress.take(address, now);
  }

  limitRoomJoin(att) {
    if (this.exempt(att) || this.guards.roomJoinAccount.take(att.account, this.host.now())) return;
    this.diagnose('room_join_limited');
    fail('limit', 'joining rooms too fast');
  }

  // Spent only by a request about to change a stored row, once it is valid: the game repeats most of its updates, and those
  // write nothing. A refusal spends nothing and writes nothing.
  limitWrite(att, kind) {
    if (this.exempt(att)) return;
    const now = this.host.now();
    const budget = WRITE_BUDGETS[kind];
    const spec = PROTECTION[budget.account];
    const account = this.guards[budget.account];
    const address = this.guards[budget.address];
    const group = ipGroup(att.ip);
    const socket = takeToken(att[budget.socket], spec, now);
    if (!socket || !account.allows(att.account, now) || !address.allows(group, now)) {
      this.diagnose('write_limited');
      fail('limit', 'writing too fast', { retry_ms: Math.ceil(1000 / spec.perSecond) });
    }
    att[budget.socket] = socket;
    account.take(att.account, now);
    address.take(group, now);
  }

  claimedIp(att, msg) {
    return att.ip.includes(':') && ipv4Kind(msg.public_ip) === 'public' ? msg.public_ip : '';
  }

  // Peers send UDP toward a client's advertised IPv4 and nothing can verify it, so each new endpoint spends budget of the
  // observed address. Past that budget the client has no public IPv4 and its peers use the relay.
  advertisedIp(att, msg, lan) {
    if (!this.claimedIp(att, msg)) return '';
    const last = att.udp;
    if (last?.public_ip === msg.public_ip && last.port === msg.port && JSON.stringify(last.lan) === JSON.stringify(lan)) {
      return msg.public_ip;
    }
    if (this.exempt(att) || this.guards.publicEndpoint.take(ipGroup(att.ip), this.host.now())) return msg.public_ip;
    this.diagnose('public_endpoint_limited');
    return '';
  }

  // Party snapshots have their own budget, one lobby-wide and one per account and per address; a refusal spends none of
  // them, and none is shared with room.create or room.join.
  limitPartyReport(att) {
    if (this.exempt(att)) return;
    const now = this.host.now();
    const address = ipGroup(att.ip);
    const { partyReportAll, partyReportAccount, partyReportAddress } = this.guards;
    if (!partyReportAll.allows('all', now) || !partyReportAccount.allows(att.account, now) ||
        !partyReportAddress.allows(address, now)) {
      this.diagnose('party_report_limited');
      fail('limit', 'too many party reports');
    }
    partyReportAll.take('all', now);
    partyReportAccount.take(att.account, now);
    partyReportAddress.take(address, now);
  }

  // In memory, so a flood of refused attempts costs no SQL; the quota is counted in SQL because it has to outlive the
  // object's hibernation.
  limitIncidentAttempt(att) {
    const now = this.host.now();
    const address = ipGroup(att.ip);
    const { incidentAll, incidentAccount, incidentAddress } = this.guards;
    if (!incidentAll.allows('all', now) || !incidentAccount.allows(att.account, now) || !incidentAddress.allows(address, now)) {
      this.diagnose('incident_limited');
      fail('limit', 'too many report attempts');
    }
    incidentAll.take('all', now);
    incidentAccount.take(att.account, now);
    incidentAddress.take(address, now);
  }

  // Its own budget: a lookup is no report, and no report's quota pays for it.
  limitIncidentLookup(att, guard) {
    const now = this.host.now();
    const address = ipGroup(att.ip);
    if (!this.guards.incidentLookupAll.allows('all', now) || !this.guards[guard].allows(address, now)) fail('limit', 'too many lookups');
    this.guards.incidentLookupAll.take('all', now);
    this.guards[guard].take(address, now);
  }

  newTicket() {
    let ticket;
    do ticket = `DCP-${this.randomCode(6)}`;
    while (this.incidents.taken(ticket));
    return ticket;
  }

  incidentList() {
    return this.incidents.list();
  }

  incident(ticket) {
    return this.incidents.get(ticket);
  }

  incidentForAdmin(ticket) {
    const found = this.incident(ticket);
    return found ? { ...found, ...this.incidents.status(ticket) } : null;
  }

  setIncidentStatus(ticket, change) {
    return this.incidents.setStatus(ticket, change, this.host.now());
  }

  deleteIncident(ticket) {
    return this.incidents.remove(ticket);
  }

  requireFriendRoom(a, b) {
    if (this.friendCount(a) >= LIMITS.friends || this.friendCount(b) >= LIMITS.friends) fail('limit', 'friend list full');
  }

  // Players who kept the default name stay apart. Salted, so an alias cannot be checked against an account.
  displayName(account, name) {
    if (name !== DEFAULT_NAME) return name;
    return `${DEFAULT_NAME}-${sha256Hex(`${this.salt}|alias|${account}`).slice(0, 4).toUpperCase()}`;
  }

  // Beside `name`, never in place of it: the game matches room members to friends by the real name.
  shownAs(account, name) {
    const shown = this.displayName(account, name);
    return shown === name ? {} : { display_name: shown };
  }

  // ---- public lobby ----
  // Presence is the socket's own attachment, so publishing writes no SQL and it ends with the socket. `player` is random
  // for each publish: it names a row of this list and nothing else.
  showInLobby(att, now) {
    att.lobby = { player: this.randomHex(16), since: now, seen: now };
  }

  hideFromLobby(account) {
    for (const { conn, att } of this.sockets()) {
      if (att.account !== account || !att.lobby) continue;
      delete att.lobby;
      conn.setAttachment(att);
    }
  }

  // Everyone listed now, newest first, one row per account. Keepalives never reach this object, so the host says when
  // the last one passed.
  lobbyEntries() {
    const oldest = this.host.now() - LIMITS.lobbyStaleMs;
    const listed = this.sockets().filter(({ conn, att }) => att.lobby && Math.max(att.lobby.seen, conn.keepaliveAt()) >= oldest);
    listed.sort((a, b) => b.att.lobby.since - a.att.lobby.since || a.att.lobby.player.localeCompare(b.att.lobby.player));
    const entries = new Map();
    for (const s of listed) if (!entries.has(s.att.account)) entries.set(s.att.account, s);
    return [...entries.values()];
  }

  // Selects instead of expireRequests(): reading the list writes nothing.
  lobbyRelations(account) {
    const since = this.host.now() - LIMITS.requestDays * DAY_MS;
    const accounts = (query, key) => new Set(this.host.sql.all(query, account, since).map((r) => r[key]));
    const friends = new Set(this.friendsOf(account));
    const incoming = accounts('SELECT sender FROM friend_requests WHERE target = ? AND ms >= ?', 'sender');
    const outgoing = accounts('SELECT target FROM friend_requests WHERE sender = ? AND ms >= ?', 'target');
    return (other) => {
      if (friends.has(other)) return 'friend';
      if (incoming.has(other)) return 'incoming';
      return outgoing.has(other) ? 'pending' : 'none';
    };
  }

  lobbyRefused(what) {
    this.diagnose('public_lobby_limited');
    fail('limit', what);
  }

  limitLobbyPublish(att) {
    if (this.exempt(att) || this.guards.lobbyPublishAccount.take(att.account, this.host.now())) return;
    this.lobbyRefused('publishing too fast');
  }

  limitLobbyList(att) {
    if (this.exempt(att)) return;
    const now = this.host.now();
    const address = ipGroup(att.ip);
    const { lobbyListAccount, lobbyListAddress } = this.guards;
    if (!lobbyListAccount.allows(att.account, now) || !lobbyListAddress.allows(address, now)) this.lobbyRefused('listing too fast');
    lobbyListAccount.take(att.account, now);
    lobbyListAddress.take(address, now);
  }

  // Only someone listed themselves can reach a listed player, and only for as long as that player stays listed.
  lobbyTarget(att, player) {
    if (typeof player !== 'string' || !LOBBY_PLAYER.test(player)) fail('bad_request', 'player');
    if (!att.lobby) fail('not_visible');
    if (!this.exempt(att) && !this.guards.lobbyRequestAccount.take(att.account, this.host.now())) this.lobbyRefused('asking too fast');
    const entry = this.lobbyEntries().find((e) => e.att.lobby.player === player);
    if (!entry) fail('not_found');
    return this.accountRow(entry.att.account);
  }

  accountByCode(text) {
    return this.host.sql.get('SELECT * FROM accounts WHERE code = ?', normalizeFriendCode(text)) ?? fail('not_found');
  }
}

const HANDLERS = {
  hello(att, msg) {
    if (att.hello) fail('bad_request', 'already greeted');
    if (msg.proto !== PROTOCOL) {
      if (!att.protoRejected) this.countReject('lobby_protocol', ipGroup(att.ip));
      att.protoRejected = true;
      fail('proto');
    }
    if (typeof msg.game !== 'string' || !/^[A-Za-z0-9._-]{1,32}$/.test(msg.game)) fail('bad_request', 'game');
    const account = decimalId(msg.account);
    if (typeof msg.secret !== 'string' || !/^[0-9a-f]{64}$/.test(msg.secret)) fail('bad_request', 'secret');
    const name = cleanText(msg.name ?? '', LIMITS.nameChars) || 'Player';
    const hash = sha256Hex(msg.secret);
    const now = this.host.now();
    const presence = msg.presence !== false;
    let row = this.accountRow(account);
    if (row && row.secret_hash !== hash) fail('unauthorized');
    this.limitHello(att, account, now);
    let newSession = presence;
    if (!row) {
      const ipKey = this.ipKey(att.ip);
      if (!(this.local && isLocalAddress(att.ip))) {
        this.host.sql.run('DELETE FROM registrations WHERE ms < ?', now - DAY_MS);
        const recent = this.host.sql.get('SELECT COUNT(*) AS n FROM registrations WHERE ip = ?', ipKey).n;
        if (recent >= LIMITS.registrationsPerIpPerDay) fail('limit', 'too many new accounts');
      }
      row = this.registerAccount(account, hash, name, presence);
      this.host.sql.run('INSERT INTO registrations (ip, ms) VALUES (?, ?)', ipKey, now);
    } else {
      newSession = presence && now - row.session >= PROTECTION.sessionWindowMs;
      this.touchAccount(row, name, now, newSession);
    }
    const wasOnline = presence && this.presentSockets(account).length > 0;
    const online = isInt(msg.online_protocol, 1, 0xffff) ? msg.online_protocol : 0;
    const version = cleanText(msg.version ?? '', LIMITS.versionChars);
    const platform = cleanText(msg.platform ?? '', LIMITS.platformChars);
    Object.assign(att, { hello: true, account, name, presence, game: msg.game, room: 0, member: 0, online_protocol: online,
      version, platform, relay: msg.relay === true, story_session: msg.story_session === true });
    if (att.relay) this.relays.negotiated();
    if (msg.lobby === true && presence && LOBBY_NAME.test(name)) this.showInLobby(att, now);
    this.index = null;
    if (presence && !wasOnline) this.notifyFriends(account, { ev: 'friend.online', account, name });
    if (newSession) this.track(() => this.metrics.session(att));
    else if (presence) this.diagnose('hello_reconnect_deduped');
    this.diagnose('hello_accepted');
    return { account, name, friend_code: formatFriendCode(row.code), ip: att.ip, time: now,
      capabilities: { ...CAPABILITIES, story_session: true } };
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
    const rows = att.room ? this.memberRows(att.room) : [];
    const me = rows.find((m) => m.member === att.member);
    const lanText = JSON.stringify(lan);
    const differs = (ip) => me.ip !== att.ip || me.public_ip !== ip || me.port !== msg.port || me.lan !== lanText;
    if (me && differs(this.claimedIp(att, msg))) this.limitWrite(att, 'state');
    const publicIp = this.advertisedIp(att, msg, lan);
    att.udp = { ip: att.ip, public_ip: publicIp, port: msg.port, lan };
    if (me) {
      const changed = differs(publicIp);
      if (changed) {
        this.host.sql.run('UPDATE members SET ip = ?, public_ip = ?, port = ?, lan = ? WHERE room = ? AND member = ?', att.ip,
          publicIp, msg.port, lanText, att.room, att.member);
        Object.assign(me, { ip: att.ip, public_ip: publicIp, port: msg.port, lan: lanText });
      }
      if (changed || !publicIp) for (const other of rows) if (other.member !== att.member) this.sendPeer(att.room, me, other);
    }
    return { ip: att.ip, port: msg.port, ...(publicIp && { public_ip: publicIp }) };
  },

  'room.create'(att, msg) {
    if (att.room) fail('in_room');
    if (!isInt(msg.world, 1, 0xffff) || !isInt(msg.slots, 1, MEMBER_IDS.length)) fail('bad_request', 'room');
    let flags = uint32(msg.flags ?? 0);
    const ints = merge([], intAttrs(msg.ints), LIMITS.intAttrsPerRoom);
    if (isStorySession(ints)) {
      if (!att.story_session) fail('bad_request', 'story_session');
      flags = (flags | FLAG_HIDDEN) >>> 0;
    }
    const binInt = merge([], binAttrs(msg.bin_int), LIMITS.binAttrsPerArray);
    const binExt = merge([], binAttrs(msg.bin_ext), LIMITS.binAttrsPerArray);
    const memberBin = binAttrs(msg.member_bin);
    this.limitRoomCreate(att);
    const last = Number(this.meta('room_id') ?? FIRST_ROOM_ID - 1);
    const id = last + 1;
    this.setMeta('room_id', id);
    let code;
    do code = this.randomCode(6);
    while (this.host.sql.get('SELECT 1 AS x FROM rooms WHERE code = ?', code));
    this.host.sql.run(`INSERT INTO rooms (id, game, world, slots, flags, owner, code, key, ints, bin_int, bin_ext, created,
      online_protocol, peak_members, platforms) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1, ?)`, id, att.game, msg.world,
    msg.slots, (flags & ~FLAG_FULL) >>> 0, MEMBER_IDS[0], code, this.randomHex(16), JSON.stringify(ints), JSON.stringify(binInt),
    JSON.stringify(binExt), this.host.now(), att.online_protocol ?? 0, platformBit(att.platform));
    this.insertMember(att, id, MEMBER_IDS[0], memberBin);
    this.track(() => this.metrics.count('rooms_created'));
    if (isStorySession(ints)) {
      this.track(() => this.metrics.count('story_rooms'));
      this.noteStoryParticipation(id, att, STORY_ROLES.host);
    }
    this.diagnose('room_create_accepted');
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
      this.countReject('online_protocol', att.account);
      const owner = this.memberRow(room.id, room.owner);
      fail('incompatible', '', { online_protocol: room.online_protocol, ...clientOf(owner) });
    }
    if (!att.story_session && isStorySession(JSON.parse(room.ints))) {
      this.countReject('story_session', att.account);
      const owner = this.memberRow(room.id, room.owner);
      fail('incompatible', '', { online_protocol: room.online_protocol, story_session_required: true, ...clientOf(owner) });
    }
    const members = this.memberRows(room.id);
    if (members.length >= room.slots) fail('full');
    if (room.flags & FLAG_CLOSED) fail('closed');
    const taken = new Set(members.map((m) => m.member));
    const memberId = MEMBER_IDS.slice(0, room.slots).find((id) => !taken.has(id));
    if (!memberId) fail('full');
    const memberBin = binAttrs(msg.member_bin);
    this.limitRoomJoin(att);
    this.insertMember(att, room.id, memberId, memberBin);
    this.host.sql.run('UPDATE rooms SET peak_members = MAX(peak_members, ?) WHERE id = ? AND peak_members > 0', members.length + 1,
      room.id);
    this.host.sql.run('UPDATE rooms SET platforms = platforms | ? WHERE id = ? AND platforms > 0', platformBit(att.platform), room.id);
    const joined = this.memberView(this.memberRow(room.id, memberId));
    this.toRoom(room.id, { ev: 'room.member_joined', room: String(room.id), member: joined }, memberId);
    const roomId = room.id;
    this.later(() => this.exchangeEndpoints(roomId, memberId));
    this.track(() => this.metrics.count('room_joins'));
    if (roomIsStory(room)) {
      this.track(() => this.metrics.count('story_guest_joins'));
      this.noteStoryParticipation(room.id, att, STORY_ROLES.guest);
    }
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

  // Retakes a seat held after a lost connection (LIMITS.graceMs). The events held for it follow the answer, then the
  // endpoints: peer events are not held, so a member who joined meanwhile is known only from here.
  'room.resume'(att, msg, conn) {
    if (att.room) fail('in_room');
    const roomId = Number(decimalId(msg.room));
    if (!isInt(msg.member, 1, 0xffff)) fail('bad_request', 'member');
    const member = this.memberRow(roomId, msg.member);
    if (!member || member.account !== att.account) fail('not_found');
    if (member.grace_until !== null && member.grace_until <= this.host.now()) fail('not_found');
    const found = this.socketByCid(member.conn);
    const old = found && found.att.cid !== att.cid ? found : null;
    if (!old && member.grace_until === null && member.conn !== null) fail('not_found');
    this.relays.forgetMember(roomId, msg.member);
    if (old) {
      this.relays.forgetConnection(old.att.cid);
      old.att.room = 0;
      old.conn.setAttachment(old.att);
      try {
        old.conn.close(4001, 'seat taken by a new connection');
      } catch {
        // Already closing.
      }
    }
    const udp = memberEndpoint(att);
    this.host.sql.run(`UPDATE members SET conn = ?, grace_until = NULL, held = '[]', name = ?, ip = ?, public_ip = ?, port = ?,
      lan = ? WHERE room = ? AND member = ?`, att.cid, att.name, udp.ip, udp.public_ip, udp.port, JSON.stringify(udp.lan), roomId,
      msg.member);
    att.room = roomId;
    att.member = msg.member;
    const held = JSON.parse(member.held);
    this.later(() => {
      for (const event of held) this.send(conn, event);
      this.exchangeEndpoints(roomId, msg.member);
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
    let flags = (((room.flags & ~mask) | (value & mask)) & ~FLAG_FULL) >>> 0;
    // The game reopens its room with HIDDEN under the mask; a story room stays out of random search.
    if (roomIsStory(room)) flags = (flags | FLAG_HIDDEN) >>> 0;
    const binIntText = JSON.stringify(binInt);
    if (flags === room.flags && binIntText === room.bin_int) return {};
    this.limitWrite(att, 'state');
    this.host.sql.run('UPDATE rooms SET flags = ?, bin_int = ? WHERE id = ?', flags, binIntText, room.id);
    const count = this.memberRows(room.id).length;
    this.toRoom(room.id, { ev: 'room.internal', room: String(room.id), flags: this.roomFlags({ ...room, flags }, count),
      bin_int: changed }, att.member);
    return {};
  },

  'room.set_external'(att, msg) {
    const room = this.requireRoom(att, msg.room);
    this.requireOwner(att, room);
    const ints = merge(JSON.parse(room.ints), intAttrs(msg.ints), LIMITS.intAttrsPerRoom);
    if (!att.story_session && isStorySession(ints)) fail('bad_request', 'story_session');
    const binExt = merge(JSON.parse(room.bin_ext), binAttrs(msg.bin_ext), LIMITS.binAttrsPerArray);
    const intsText = JSON.stringify(ints);
    const binExtText = JSON.stringify(binExt);
    const flags = isStorySession(ints) ? (room.flags | FLAG_HIDDEN) >>> 0 : room.flags;
    if (intsText === room.ints && binExtText === room.bin_ext && flags === room.flags) return {};
    this.limitWrite(att, 'state');
    this.host.sql.run('UPDATE rooms SET ints = ?, bin_ext = ?, flags = ? WHERE id = ?', intsText, binExtText, flags, room.id);
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
    const named = (rows, key) => rows.map((r) => {
      const name = this.accountRow(r[key])?.name ?? '';
      return { account: r[key], name, ...this.shownAs(r[key], name), ms: r.ms };
    });
    return {
      incoming: named(this.host.sql.all('SELECT sender, ms FROM friend_requests WHERE target = ? ORDER BY ms', att.account), 'sender'),
      outgoing: named(this.host.sql.all('SELECT target, ms FROM friend_requests WHERE sender = ? ORDER BY ms', att.account), 'target'),
    };
  },

  'friends.add'(att, msg) {
    const target = msg.player !== undefined ? this.lobbyTarget(att, msg.player) : this.accountByCode(msg.code);
    if (target.account === att.account) fail('bad_request', 'own code');
    const result = { account: target.account, name: target.name, ...this.shownAs(target.account, target.name) };
    if (this.areFriends(att.account, target.account)) return { ...result, state: 'friends' };
    this.expireRequests();
    if (this.host.sql.get('SELECT 1 AS x FROM friend_requests WHERE sender = ? AND target = ?', target.account, att.account)) {
      this.requireFriendRoom(att.account, target.account);
      this.limitWrite(att, 'friend');
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
    this.limitWrite(att, 'friend');
    this.host.sql.run('INSERT INTO friend_requests (sender, target, ms) VALUES (?, ?, ?)', att.account, target.account, this.host.now());
    this.notifyAccount(target.account, { ev: 'friend.request', account: att.account, name: att.name });
    return { ...result, state: 'pending' };
  },

  'friends.accept'(att, msg) {
    const sender = decimalId(msg.account);
    this.expireRequests();
    if (!this.host.sql.get('SELECT 1 AS x FROM friend_requests WHERE sender = ? AND target = ?', sender, att.account)) fail('not_found');
    this.requireFriendRoom(att.account, sender);
    this.limitWrite(att, 'friend');
    this.makeFriends(att.account, sender);
    this.notifyAccount(sender, { ev: 'friend.added', friend: this.friendView(att.account, att.game) });
    return { friend: this.friendView(sender, att.game) };
  },

  'friends.decline'(att, msg) {
    const other = msg.player !== undefined ? this.lobbyTarget(att, msg.player).account : decimalId(msg.account);
    const before = this.host.sql.get(`SELECT COUNT(*) AS n FROM friend_requests WHERE (sender = ? AND target = ?)
      OR (sender = ? AND target = ?)`, other, att.account, att.account, other).n;
    if (!before) fail('not_found');
    this.limitWrite(att, 'friend');
    this.host.sql.run('DELETE FROM friend_requests WHERE (sender = ? AND target = ?) OR (sender = ? AND target = ?)', other,
      att.account, att.account, other);
    return {};
  },

  'friends.remove'(att, msg) {
    const other = decimalId(msg.account);
    const were = this.areFriends(att.account, other);
    const pending = this.host.sql.get(`SELECT 1 AS x FROM friend_requests WHERE (sender = ? AND target = ?)
      OR (sender = ? AND target = ?)`, other, att.account, att.account, other);
    if (!were && !pending) return {};
    this.limitWrite(att, 'friend');
    this.host.sql.run('DELETE FROM friends WHERE (account = ? AND friend = ?) OR (account = ? AND friend = ?)', att.account,
      other, other, att.account);
    this.host.sql.run('DELETE FROM friend_requests WHERE (sender = ? AND target = ?) OR (sender = ? AND target = ?)', other,
      att.account, att.account, other);
    if (were) this.notifyAccount(other, { ev: 'friend.removed', account: att.account });
    return {};
  },

  // Opt-in only. The name and the account are the session's: nothing in the request can set them.
  'lobby.publish'(att, msg) {
    if (typeof msg.visible !== 'boolean') fail('bad_request', 'visible');
    if (!msg.visible) {
      this.hideFromLobby(att.account);
      return { visible: false };
    }
    if (att.lobby) return { visible: true };
    if (!att.presence) fail('not_found', 'no presence');
    if (!LOBBY_NAME.test(att.name)) fail('bad_request', 'name');
    this.limitLobbyPublish(att);
    this.showInLobby(att, this.host.now());
    return { visible: true };
  },

  // Only those listed may read the list. `rev` is what the client holds: an unchanged list is not sent again.
  'lobby.list'(att, msg) {
    if (msg.rev !== undefined && (typeof msg.rev !== 'string' || !LOBBY_REV.test(msg.rev))) fail('bad_request', 'rev');
    if (!att.lobby) fail('not_visible');
    this.limitLobbyList(att);
    const others = this.lobbyEntries().filter((e) => e.att.account !== att.account);
    const relation = this.lobbyRelations(att.account);
    // Ranked before the cap, so a request waiting for an answer is never the row left out.
    const ranked = others.map((e) => ({ e, state: relation(e.att.account) }));
    ranked.sort((a, b) => LOBBY_RANK[a.state] - LOBBY_RANK[b.state]);
    const players = ranked.slice(0, LIMITS.lobbyPlayers).map(({ e, state }) => ({ player: e.att.lobby.player,
      name: this.displayName(e.att.account, e.att.name), version: LOBBY_VERSION.test(e.att.version) ? e.att.version : '', state }));
    const rev = sha256Hex(JSON.stringify(players)).slice(0, 16);
    return rev === msg.rev ? { rev, total: others.length, unchanged: true } : { rev, total: others.length, players };
  },

  // Anonymous party statistics (party.js): optional, sent by the room's owner on its own game connection, so a party is
  // counted once. Only class indexes are read; the sender's identity and room are used to check the request and are
  // not kept with the counts. An identical repeat on the same connection is not counted again.
  'party.report'(att, msg, conn, text) {
    const party = parsePartyReport(msg, text);
    if (!party) fail('bad_request', 'party');
    if (att.presence === false || !att.room) fail('not_found', 'not in a room');
    const room = this.roomRow(att.room);
    if (!room) fail('not_found');
    this.requireOwner(att, room);
    if (party.classes.length > room.slots) fail('bad_request', 'party');
    const signature = `${room.id}:${party.classes.join('')}:${party.host ?? '-'}`;
    if (att.party === signature) return { counted: false };
    this.limitPartyReport(att);
    this.track(() => this.metrics.partyReport(party));
    att.party = signature;
    return { counted: true };
  },

  // A report from the launcher's Bug Report page; only the /admin dashboard reads it back (incidents.js).
  'incident.file'(att, msg) {
    this.limitIncidentAttempt(att);
    const { incident, code, detail } = readIncident(msg);
    if (!incident) fail(code, detail);
    const now = this.host.now();
    const address = ipGroup(att.ip);
    const { refusal, ticket, receipt } = this.host.sql.transaction(() => {
      const limited = this.incidents.refusal(att.account, address, now);
      if (limited) return { refusal: limited };
      const filed = { ticket: this.newTicket(), receipt: this.randomHex(32) };
      this.incidents.file(incident, filed.ticket, att.account, address, now);
      this.incidents.track(filed.ticket, filed.receipt, now);
      return filed;
    });
    if (refusal) {
      this.diagnose(INCIDENT_LIMITED[refusal]);
      fail(refusal, 'report limit');
    }
    this.diagnose('incident_filed');
    return { ticket, receipt, submitted_at: now };
  },

  // The launcher's My reports: what became of the reports it holds receipts for.
  'incident.status'(att, msg) {
    this.limitIncidentLookup(att, 'incidentStatusAddress');
    const claims = readClaims(msg);
    if (!claims) fail('bad_request', 'reports');
    return { reports: this.incidents.statuses(claims) };
  },

  'incident.read'(att, msg) {
    this.limitIncidentLookup(att, 'incidentReadAddress');
    const claim = readClaim(msg);
    if (!claim) fail('bad_request', 'receipt');
    const report = this.incidents.report(claim);
    if (!report) fail('not_found');
    return { report };
  },
};
