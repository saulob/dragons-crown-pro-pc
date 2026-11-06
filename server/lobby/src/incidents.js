// Problem reports filed from the launcher's Bug Report page (docs/ONLINE.md, Bug reports). A filed report holds only the
// fields below, all of them shown to the player first; who filed it and from where stay apart, in a quota whose keys are
// salted hashes and whose rows are gone after a day.
import { digestsMatch, sha256Hex } from './sha256.js';

export const INCIDENT = {
  frameChars: 160 * 1024,
  descriptionChars: 500,
  logBytes: 64 * 1024,
  metadataChars: 64,
  previewChars: 140,
  perAccountPerHour: 2,
  perAccountPerDay: 10,
  // Above an account's own limit, so one player cannot lock out the others behind the same shared address.
  perAddressPerDay: 30,
  perDay: 300,
  kept: 500,
  messageChars: 500,
  statusBatch: 50,
  statusKept: 5000,
};

export const INCIDENT_STATUSES = ['new', 'investigating', 'fixed', 'cannot_reproduce', 'duplicate', 'not_a_bug', 'closed'];

// Answer code of a refused report, and the abuse counter it adds to.
export const INCIDENT_LIMITED = {
  incident_hourly_limit: 'incident_hourly_limited',
  incident_daily_limit: 'incident_daily_limited',
  limit: 'incident_limited',
};

const HOUR_MS = 3600 * 1000;
const DAY_MS = 24 * HOUR_MS;
const ENVELOPE = new Set(['op', 'seq']);
const LOG_FILE = /^dragons_crown_pro_[0-9]{3,10}\.log$/;
const VERSION = /^[0-9A-Za-z][0-9A-Za-z.+-]{0,31}$/;
const CUSA = /^(?:CUSA[0-9]{5})?$/;
const TICKET = /^DCP-[A-HJ-NP-Z2-9]{6}$/;
const RECEIPT = /^[0-9a-f]{64}$/;
const STATUS_FIELDS = new Set(['status', 'fixed_version', 'player_message']);

export const isTicket = (text) => typeof text === 'string' && TICKET.test(text);

// Built from the two validated names only, so nothing the client sent beyond them reaches a header.
export function logDownloadName(incident) {
  if (!isTicket(incident.ticket)) return 'dragons-crown-pro-report.log';
  return LOG_FILE.test(incident.log_file) ? `${incident.ticket}-${incident.log_file}` : `${incident.ticket}.log`;
}

const utf8Bytes = (text) => new TextEncoder().encode(text).length;

// What the launcher's filter leaves in text: line feeds and tabs, no other control characters, no carriage returns.
// eslint-disable-next-line no-control-regex
const HAS_FORBIDDEN = /[\u0000-\u0008\u000b-\u001f\u007f-\u009f]/;
// eslint-disable-next-line no-control-regex
const HAS_CONTROL = /[\u0000-\u001f\u007f-\u009f]/;

class Refusal extends Error {
  constructor(code, detail) {
    super(detail);
    this.code = code;
  }
}

// Fields are stored as received or refused, never rewritten, so the stored report is the one the player previewed.
function text(msg, field, maxChars) {
  const value = msg[field];
  if (typeof value !== 'string' || HAS_CONTROL.test(value) || value !== value.trim()) throw new Refusal('bad_request', field);
  if (Array.from(value).length > maxChars) throw new Refusal('too_large', field);
  return value;
}

function description(msg) {
  const value = msg.description;
  if (typeof value !== 'string' || HAS_FORBIDDEN.test(value) || value.includes('\r')) throw new Refusal('bad_request', 'description');
  if (!value.trim()) throw new Refusal('bad_request', 'empty description');
  if (Array.from(value).length > INCIDENT.descriptionChars) throw new Refusal('too_large', 'description');
  return value;
}

function logText(msg) {
  const value = msg.log_text;
  if (typeof value !== 'string') throw new Refusal('bad_request', 'log_text');
  if (utf8Bytes(value) > INCIDENT.logBytes) throw new Refusal('too_large', 'log_text');
  if (HAS_FORBIDDEN.test(value) || value.includes('\r')) throw new Refusal('bad_request', 'log_text');
  if (!value.trim()) throw new Refusal('bad_request', 'empty log');
  return value;
}

function matching(msg, field, pattern) {
  const value = text(msg, field, INCIDENT.metadataChars);
  if (!pattern.test(value)) throw new Refusal('bad_request', field);
  return value;
}

function yesNo(msg, field) {
  if (msg[field] !== 'yes' && msg[field] !== 'no') throw new Refusal('bad_request', field);
  return msg[field];
}

const READERS = {
  description,
  launcher_version: (msg) => matching(msg, 'launcher_version', VERSION),
  game_build: (msg) => text(msg, 'game_build', INCIDENT.metadataChars),
  game_version: (msg) => text(msg, 'game_version', INCIDENT.metadataChars),
  cusa: (msg) => matching(msg, 'cusa', CUSA),
  operating_system: (msg) => matching(msg, 'operating_system', /./),
  log_file: (msg) => matching(msg, 'log_file', LOG_FILE),
  log_clipped: (msg) => yesNo(msg, 'log_clipped'),
  log_text: logText,
};

// The stored fields of a request, or { code, detail } for the answer. Nothing here trusts a size the client states, and
// a field the page could not have shown is refused rather than dropped.
export function readIncident(msg) {
  for (const key of Object.keys(msg)) {
    if (!ENVELOPE.has(key) && !Object.hasOwn(READERS, key)) return { code: 'bad_request', detail: key };
  }
  try {
    const incident = {};
    for (const [field, read] of Object.entries(READERS)) incident[field] = read(msg);
    return { incident };
  } catch (error) {
    if (error instanceof Refusal) return { code: error.code, detail: error.message };
    throw error;
  }
}

// One report's receipt from a lookup, or null. Nothing here says whether the report exists.
export function readClaim(entry) {
  if (entry === null || typeof entry !== 'object' || Array.isArray(entry)) return null;
  const { ticket, receipt } = entry;
  return isTicket(ticket) && typeof receipt === 'string' && RECEIPT.test(receipt) ? { ticket, receipt } : null;
}

// The claims of an `incident.status` request, or null when it is malformed or too long.
export function readClaims(msg) {
  if (Object.keys(msg).some((key) => !ENVELOPE.has(key) && key !== 'reports')) return null;
  const { reports } = msg;
  if (!Array.isArray(reports) || reports.length > INCIDENT.statusBatch) return null;
  const claims = reports.map(readClaim);
  return claims.includes(null) ? null : claims;
}

// An admin's change: { change } or { detail }. A fixed version exists only on a Fixed report, so a stale one never shows.
export function readStatusChange(body) {
  if (body === null || typeof body !== 'object' || Array.isArray(body)) return { detail: 'body' };
  for (const key of Object.keys(body)) if (!STATUS_FIELDS.has(key)) return { detail: key };
  const { status, fixed_version: version = '', player_message: message = '' } = body;
  if (!INCIDENT_STATUSES.includes(status)) return { detail: 'status' };
  if (typeof version !== 'string' || (status === 'fixed' && version && !VERSION.test(version))) return { detail: 'fixed_version' };
  if (typeof message !== 'string' || HAS_FORBIDDEN.test(message) || message.includes('\r')) return { detail: 'player_message' };
  if (Array.from(message).length > INCIDENT.messageChars) return { detail: 'player_message' };
  return { change: { status, fixed_version: status === 'fixed' ? version : '', player_message: message.trim() } };
}

const LIST_COLUMNS = `incidents.ticket AS ticket, ms, launcher_version, game_build, game_version, cusa, operating_system, log_file,
  substr(description, 1, ${INCIDENT.previewChars}) AS preview, COALESCE(t.status, 'new') AS status,
  COALESCE(t.status_ms, ms) AS status_updated_at`;

const statusOf = (row) => ({
  status: row.status,
  status_updated_at: row.status_ms,
  fixed_version: row.fixed_version,
  player_message: row.player_message,
});

// The report store. `sql` as in Lobby's host.
export class IncidentDesk {
  constructor(sql, salt) {
    this.sql = sql;
    this.salt = salt;
    sql.run(`CREATE TABLE IF NOT EXISTS incidents (ticket TEXT PRIMARY KEY, ms INTEGER NOT NULL,
      launcher_version TEXT NOT NULL, game_build TEXT NOT NULL, game_version TEXT NOT NULL, cusa TEXT NOT NULL,
      operating_system TEXT NOT NULL, description TEXT NOT NULL, log_file TEXT NOT NULL, log_clipped TEXT NOT NULL,
      log_text TEXT NOT NULL)`);
    sql.run(`CREATE TABLE IF NOT EXISTS incident_quota (account_key TEXT NOT NULL, address_key TEXT NOT NULL,
      ms INTEGER NOT NULL)`);
    sql.run(`CREATE TABLE IF NOT EXISTS incident_tracking (ticket TEXT PRIMARY KEY, receipt_hash TEXT NOT NULL,
      submitted_ms INTEGER NOT NULL, status TEXT NOT NULL, status_ms INTEGER NOT NULL, fixed_version TEXT NOT NULL,
      player_message TEXT NOT NULL)`);
  }

  key(kind, value) {
    return sha256Hex(`${this.salt}|${kind}|${value}`).slice(0, 32);
  }

  // The key of INCIDENT_LIMITED that refuses a report now, or null. The windows roll: a report leaves them one hour and
  // one day after it was accepted. The account comes first, shortest window first, because that is the one to wait for.
  refusal(account, address, now) {
    this.sql.run('DELETE FROM incident_quota WHERE ms <= ?', now - DAY_MS);
    const accountKey = this.key('account', account);
    const used = this.sql.get(`SELECT COUNT(*) AS everyone, COALESCE(SUM(account_key = ?), 0) AS account_day,
      COALESCE(SUM(account_key = ? AND ms > ?), 0) AS account_hour, COALESCE(SUM(address_key = ?), 0) AS address
      FROM incident_quota`, accountKey, accountKey, now - HOUR_MS, this.key('address', address));
    if (used.account_hour >= INCIDENT.perAccountPerHour) return 'incident_hourly_limit';
    if (used.account_day >= INCIDENT.perAccountPerDay) return 'incident_daily_limit';
    if (used.everyone >= INCIDENT.perDay || used.address >= INCIDENT.perAddressPerDay) return 'limit';
    return null;
  }

  file(incident, ticket, account, address, now) {
    this.sql.run(`INSERT INTO incidents (ticket, ms, launcher_version, game_build, game_version, cusa, operating_system,
      description, log_file, log_clipped, log_text) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, ticket, now, incident.launcher_version,
    incident.game_build, incident.game_version, incident.cusa, incident.operating_system, incident.description, incident.log_file,
    incident.log_clipped, incident.log_text);
    this.sql.run('INSERT INTO incident_quota (account_key, address_key, ms) VALUES (?, ?, ?)', this.key('account', account),
      this.key('address', address), now);
    this.sql.run(`DELETE FROM incidents WHERE rowid IN
      (SELECT rowid FROM incidents ORDER BY ms DESC, rowid DESC LIMIT -1 OFFSET ?)`, INCIDENT.kept);
  }

  receiptHash(receipt) {
    return sha256Hex(`${this.salt}|receipt|${receipt}`);
  }

  // A status outlives the log it came with, so its table has a longer limit of its own.
  track(ticket, receipt, now) {
    this.sql.run(`INSERT INTO incident_tracking (ticket, receipt_hash, submitted_ms, status, status_ms, fixed_version, player_message)
      VALUES (?, ?, ?, 'new', ?, '', '')`, ticket, this.receiptHash(receipt), now, now);
    this.sql.run(`DELETE FROM incident_tracking WHERE rowid IN
      (SELECT rowid FROM incident_tracking ORDER BY submitted_ms DESC, rowid DESC LIMIT -1 OFFSET ?)`, INCIDENT.statusKept);
  }

  // Every claim costs the same hash, so a wrong receipt and an unknown ticket cannot be told apart. A report from before
  // receipts has an empty hash, which no digest matches.
  claimed({ ticket, receipt }) {
    const hash = this.receiptHash(receipt);
    const row = this.sql.get('SELECT * FROM incident_tracking WHERE ticket = ?', ticket);
    return row && digestsMatch(row.receipt_hash, hash) ? row : null;
  }

  // What the launcher may know of the claimed reports; a claim that fails is left out.
  statuses(claims) {
    return claims.map((claim) => this.claimed(claim)).filter(Boolean).map((row) => ({
      ticket: row.ticket,
      status: row.status,
      status_updated_at: row.status_ms,
      ...(row.fixed_version ? { fixed_version: row.fixed_version } : {}),
      ...(row.player_message ? { player_message: row.player_message } : {}),
    }));
  }

  // The stored report for its owner, or null once its log is gone.
  report(claim) {
    const row = this.claimed(claim);
    const stored = row && this.get(row.ticket);
    if (!stored) return null;
    const { ms, ...report } = stored;
    return { ...report, submitted_at: ms };
  }

  status(ticket) {
    const row = this.sql.get('SELECT * FROM incident_tracking WHERE ticket = ?', ticket);
    if (row) return statusOf(row);
    const stored = this.sql.get('SELECT ms FROM incidents WHERE ticket = ?', ticket);
    return stored ? { status: 'new', status_updated_at: stored.ms, fixed_version: '', player_message: '' } : null;
  }

  // The status after the change, or null for an unknown report. A save that changes nothing keeps its time.
  setStatus(ticket, change, now) {
    const current = this.status(ticket);
    if (!current) return null;
    if (Object.keys(change).every((field) => current[field] === change[field])) return current;
    const submitted = this.sql.get('SELECT ms FROM incidents WHERE ticket = ?', ticket)?.ms ?? current.status_updated_at;
    this.sql.run(`INSERT INTO incident_tracking (ticket, receipt_hash, submitted_ms, status, status_ms, fixed_version, player_message)
      VALUES (?, '', ?, ?, ?, ?, ?) ON CONFLICT(ticket) DO UPDATE SET status = excluded.status, status_ms = excluded.status_ms,
      fixed_version = excluded.fixed_version, player_message = excluded.player_message`, ticket, submitted, change.status, now,
    change.fixed_version, change.player_message);
    return this.status(ticket);
  }

  taken(ticket) {
    return Boolean(this.sql.get(`SELECT 1 AS x FROM incidents WHERE ticket = ?
      UNION ALL SELECT 1 AS x FROM incident_tracking WHERE ticket = ?`, ticket, ticket));
  }

  list() {
    return {
      incidents: this.sql.all(`SELECT ${LIST_COLUMNS} FROM incidents LEFT JOIN incident_tracking t ON t.ticket = incidents.ticket
        ORDER BY ms DESC, incidents.rowid DESC`),
      kept: INCIDENT.kept,
    };
  }

  get(ticket) {
    return this.sql.get('SELECT * FROM incidents WHERE ticket = ?', ticket) ?? null;
  }
}
