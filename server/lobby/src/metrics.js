// Aggregate service metrics for the private dashboard (README.md, Telemetry): daily counters, daily counts of sessions
// per platform, version and country, and 15-minute usage samples. No row names a player, an account, an address or a
// room, and nothing here keeps per-session state: live numbers come from the lobby's sockets and tables. Who used Early
// Story Online is kept apart, in story_usage.js.
import { MAX_PARTY, PARTY_DIMENSIONS, partyCounts } from './party.js';

export { MAX_PARTY };
export const METRICS_TIME_ZONE = 'America/Sao_Paulo';
export const SAMPLE_MS = 15 * 60 * 1000;
export const SAMPLE_DAYS = 90;
export const HISTORY_MAX_DAYS = 365;
export const DIMENSIONS = ['platform', 'platform_mix', 'version', 'country', 'reject', 'abuse', 'occupancy', 'lifetime',
  ...PARTY_DIMENSIONS];
export const LIFETIMES = [['under_1m', 60e3], ['1_5m', 5 * 60e3], ['5_15m', 15 * 60e3], ['15_60m', 60 * 60e3], ['over_1h', Infinity]];
export const DIAGNOSTICS = new Set(['ws_upgrade_accepted', 'ws_upgrade_limited', 'ws_upgrade_ip_cap', 'hello_accepted',
  'hello_reconnect_deduped', 'hello_limited', 'room_create_accepted', 'room_create_limited', 'room_join_limited',
  'relay_abuse_closed', 'party_report_limited', 'public_endpoint_limited', 'incident_filed', 'incident_limited',
  'incident_hourly_limited', 'incident_daily_limited', 'public_lobby_limited', 'write_limited', 'lobby_abuse_closed']);
export const PLATFORMS = ['Windows', 'Linux', 'Steam Deck', 'Android'];
export const PLATFORM_ORDER = [...PLATFORMS, 'Other'];
const PC_PLATFORMS = ['Windows', 'Linux', 'Steam Deck'];

const DAY_MS = 24 * 3600 * 1000;
// Bounds what a client can add to the table: further new versions in a day count as Other.
const VALUES_PER_DIMENSION_PER_DAY = 64;
const CLEANUP_ROWS = 500;
const STORY_COLUMNS = ['story_rooms', 'story_guest_joins'];
const COUNTERS = new Set(['sessions', 'rooms_created', 'room_joins', 'incompatible_rejects', ...STORY_COLUMNS]);

const PLATFORM_ROOM_COLUMNS = ['rooms_cross_platform', 'rooms_android', 'rooms_android_cross', 'rooms_android_pc',
  'rooms_android_relayed', 'rooms_android_cross_relayed'];
const ROOM_COLUMNS = ['rooms_closed', 'rooms_with_guest', 'rooms_relayed', 'party_total', ...PLATFORM_ROOM_COLUMNS];
const DAY_COLUMNS = ['sessions', 'rooms_created', 'room_joins', 'incompatible_rejects', 'peak_online', 'peak_in_rooms',
  'peak_rooms', ...ROOM_COLUMNS];
const SUMMED = ['sessions', 'rooms_created', 'room_joins', 'incompatible_rejects', ...ROOM_COLUMNS];
const PEAKS = ['peak_online', 'peak_in_rooms', 'peak_rooms'];

export const METRICS_SCHEMA = [
  `CREATE TABLE IF NOT EXISTS metrics_daily (day TEXT PRIMARY KEY, sessions INTEGER NOT NULL DEFAULT 0,
     rooms_created INTEGER NOT NULL DEFAULT 0, room_joins INTEGER NOT NULL DEFAULT 0,
     incompatible_rejects INTEGER NOT NULL DEFAULT 0, peak_online INTEGER NOT NULL DEFAULT 0,
     peak_in_rooms INTEGER NOT NULL DEFAULT 0, peak_rooms INTEGER NOT NULL DEFAULT 0)`,
  `CREATE TABLE IF NOT EXISTS metrics_daily_dimensions (day TEXT NOT NULL, dimension TEXT NOT NULL, value TEXT NOT NULL,
     count INTEGER NOT NULL, PRIMARY KEY (day, dimension, value))`,
  `CREATE TABLE IF NOT EXISTS metrics_samples (bucket INTEGER PRIMARY KEY, online INTEGER NOT NULL,
     in_rooms INTEGER NOT NULL, rooms INTEGER NOT NULL)`,
];

const dayFormat = new Intl.DateTimeFormat('en-CA', { timeZone: METRICS_TIME_ZONE, year: 'numeric', month: '2-digit',
  day: '2-digit' });

export function metricsDay(ms) {
  const parts = Object.fromEntries(dayFormat.formatToParts(ms).map((p) => [p.type, p.value]));
  return `${parts.year}-${parts.month}-${parts.day}`;
}

function daysBefore(day, n) {
  const [y, m, d] = day.split('-').map(Number);
  return new Date(Date.UTC(y, m - 1, d - n)).toISOString().slice(0, 10);
}

export const sampleBucket = (ms) => ms - (ms % SAMPLE_MS);

export const platformCategory = (text) => (PLATFORMS.includes(text) ? text : 'Other');

// A room remembers which platform categories ever sat in it as bits in PLATFORM_ORDER; 0 means it was not tracked.
export const platformBit = (platform) => 1 << PLATFORM_ORDER.indexOf(platformCategory(platform));

export const platformsOfMask = (mask) => PLATFORM_ORDER.filter((_, i) => mask & (1 << i));

export const platformMix = (mask) => platformsOfMask(mask).join('+');

export function versionCategory(text) {
  if (!text) return 'Unknown';
  return /^\d{1,4}(\.\d{1,4}){1,3}$/.test(text) ? text : 'Other';
}

// Cloudflare's country code, or Unknown (XX is Cloudflare's own unknown, T1 is Tor).
export function countryCode(text) {
  return typeof text === 'string' && /^[A-Z]{2}$/.test(text) && text !== 'XX' ? text : 'Unknown';
}

const sorted = (counts) => [...counts].map(([value, count]) => ({ value, count }))
  .sort((a, b) => b.count - a.count || a.value.localeCompare(b.value));

export class Metrics {
  constructor(sql, now) {
    this.sql = sql;
    this.now = now;
    for (const statement of METRICS_SCHEMA) sql.run(statement);
    const have = new Set(sql.all('PRAGMA table_info(metrics_daily)').map((c) => c.name));
    for (const column of [...ROOM_COLUMNS, ...STORY_COLUMNS]) {
      if (!have.has(column)) sql.run(`ALTER TABLE metrics_daily ADD COLUMN ${column} INTEGER NOT NULL DEFAULT 0`);
    }
    this.peaks = null;
    this.sample = null;
    this.pending = new Map();
  }

  // Abuse diagnostics are counted in memory and written together by flushDiagnostics; true when this is the first
  // pending one.
  diagnose(name, amount = 1) {
    if (!DIAGNOSTICS.has(name)) throw new Error(`unknown diagnostic ${name}`);
    const first = this.pending.size === 0;
    this.pending.set(name, (this.pending.get(name) ?? 0) + amount);
    return first;
  }

  flushDiagnostics() {
    if (!this.pending.size) return;
    const day = metricsDay(this.now());
    const counts = [...this.pending];
    this.pending.clear();
    for (const [name, count] of counts) {
      this.sql.run(`INSERT INTO metrics_daily_dimensions (day, dimension, value, count) VALUES (?, 'abuse', ?, ?)
        ON CONFLICT(day, dimension, value) DO UPDATE SET count = count + excluded.count`, day, name, count);
    }
  }

  count(column, day = metricsDay(this.now())) {
    if (!COUNTERS.has(column)) throw new Error(`unknown counter ${column}`);
    this.sql.run(`INSERT INTO metrics_daily (day, ${column}) VALUES (?, 1)
      ON CONFLICT(day) DO UPDATE SET ${column} = ${column} + 1`, day);
  }

  addDimension(day, dimension, value) {
    const known = this.sql.get('SELECT 1 AS x FROM metrics_daily_dimensions WHERE day = ? AND dimension = ? AND value = ?',
      day, dimension, value);
    if (!known) {
      const values = this.sql.get('SELECT COUNT(*) AS n FROM metrics_daily_dimensions WHERE day = ? AND dimension = ?',
        day, dimension).n;
      if (values >= VALUES_PER_DIMENSION_PER_DAY) value = 'Other';
    }
    this.sql.run(`INSERT INTO metrics_daily_dimensions (day, dimension, value, count) VALUES (?, ?, ?, 1)
      ON CONFLICT(day, dimension, value) DO UPDATE SET count = count + 1`, day, dimension, value);
  }

  // A room whose last seat is gone: `peak` seats at most were taken (0: unknown, not counted), `relayed` when the relay
  // carried a frame of it.
  roomClosed({ peak, lifetimeMs, relayed, platforms = 0 }) {
    if (!Number.isInteger(peak) || peak < 1) return;
    const day = metricsDay(this.now());
    const party = Math.min(peak, MAX_PARTY);
    const added = { rooms_closed: 1, rooms_with_guest: party > 1 ? 1 : 0, rooms_relayed: relayed ? 1 : 0, party_total: party,
      ...(party > 1 ? platformRoom(platforms, relayed) : {}) };
    const columns = Object.keys(added);
    this.sql.run(`INSERT INTO metrics_daily (day, ${columns.join(', ')}) VALUES (?, ${columns.map(() => '?').join(', ')})
      ON CONFLICT(day) DO UPDATE SET ${columns.map((c) => `${c} = ${c} + excluded.${c}`).join(', ')}`, day,
    ...Object.values(added));
    if (party > 1 && platforms) this.addDimension(day, 'platform_mix', platformMix(platforms));
    this.addDimension(day, 'occupancy', String(party));
    this.addDimension(day, 'lifetime', LIFETIMES.find(([, limit]) => (Number(lifetimeMs) || 0) < limit)[0]);
  }

  // One connection that became an online session; `client` is its platform, version and country as reported.
  session(client) {
    const day = metricsDay(this.now());
    this.count('sessions', day);
    this.addDimension(day, 'platform', platformCategory(client.platform));
    this.addDimension(day, 'version', versionCategory(client.version));
    this.addDimension(day, 'country', countryCode(client.country));
  }

  // One accepted party snapshot (party.js): bounded counts in the party dimensions, whose values the server derives from
  // class indexes, never from client text. Callers have applied the lobby's limits; nothing about the sender is kept.
  partyReport(party) {
    const day = metricsDay(this.now());
    for (const [dimension, value, amount] of partyCounts(party)) {
      this.sql.run(`INSERT INTO metrics_daily_dimensions (day, dimension, value, count) VALUES (?, ?, ?, ?)
        ON CONFLICT(day, dimension, value) DO UPDATE SET count = count + excluded.count`, day, dimension, value, amount);
    }
  }

  reject(reason) {
    const day = metricsDay(this.now());
    this.count('incompatible_rejects', day);
    this.addDimension(day, 'reject', reason);
  }

  // Raises today's peaks and the current sample to `live` ({ online, in_rooms, rooms }); writes only what grew.
  observe(live) {
    const now = this.now();
    const day = metricsDay(now);
    if (this.peaks?.day !== day) {
      const row = this.sql.get('SELECT peak_online, peak_in_rooms, peak_rooms FROM metrics_daily WHERE day = ?', day);
      this.peaks = row ? { day, online: row.peak_online, in_rooms: row.peak_in_rooms, rooms: row.peak_rooms }
        : { day, online: -1, in_rooms: -1, rooms: -1 };
    }
    if (grew(this.peaks, live)) {
      this.sql.run(`INSERT INTO metrics_daily (day, peak_online, peak_in_rooms, peak_rooms) VALUES (?, ?, ?, ?)
        ON CONFLICT(day) DO UPDATE SET peak_online = MAX(peak_online, excluded.peak_online),
        peak_in_rooms = MAX(peak_in_rooms, excluded.peak_in_rooms), peak_rooms = MAX(peak_rooms, excluded.peak_rooms)`,
      day, live.online, live.in_rooms, live.rooms);
      raise(this.peaks, live);
    }
    const bucket = sampleBucket(now);
    if (this.sample?.bucket !== bucket) {
      const row = this.sql.get('SELECT online, in_rooms, rooms FROM metrics_samples WHERE bucket = ?', bucket);
      this.sample = row ? { bucket, ...row } : { bucket, online: -1, in_rooms: -1, rooms: -1 };
      this.dropOldSamples(now);
    }
    if (grew(this.sample, live)) {
      this.sql.run(`INSERT INTO metrics_samples (bucket, online, in_rooms, rooms) VALUES (?, ?, ?, ?)
        ON CONFLICT(bucket) DO UPDATE SET online = MAX(online, excluded.online),
        in_rooms = MAX(in_rooms, excluded.in_rooms), rooms = MAX(rooms, excluded.rooms)`,
      bucket, live.online, live.in_rooms, live.rooms);
      raise(this.sample, live);
    }
  }

  dropOldSamples(now) {
    this.sql.run(`DELETE FROM metrics_samples WHERE bucket IN
      (SELECT bucket FROM metrics_samples WHERE bucket < ? ORDER BY bucket LIMIT ?)`, now - SAMPLE_DAYS * DAY_MS,
    CLEANUP_ROWS);
  }

  // `live`: the counts plus Maps of platform, version and country to connected players.
  summary(live) {
    const now = this.now();
    const day = metricsDay(now);
    const today = this.sql.get(`SELECT ${DAY_COLUMNS.join(', ')} FROM metrics_daily WHERE day = ?`, day) ?? emptyDay();
    return {
      generated: now,
      time_zone: METRICS_TIME_ZONE,
      day,
      live: { online: live.online, in_rooms: live.in_rooms, rooms: live.rooms, platform: sorted(live.platform),
        version: sorted(live.version), country: sorted(live.country) },
      today: { ...today, peak_online: Math.max(today.peak_online, live.online),
        peak_in_rooms: Math.max(today.peak_in_rooms, live.in_rooms), peak_rooms: Math.max(today.peak_rooms, live.rooms) },
    };
  }

  history(days) {
    if (!Number.isInteger(days) || days < 1 || days > HISTORY_MAX_DAYS) throw new RangeError('days out of range');
    const now = this.now();
    const last = metricsDay(now);
    const first = daysBefore(last, days - 1);
    const rows = new Map(this.sql.all(`SELECT day, ${DAY_COLUMNS.join(', ')} FROM metrics_daily WHERE day >= ? AND day <= ?
      ORDER BY day`, first, last)
      .map((r) => [r.day, r]));
    const daily = [];
    for (let i = days - 1; i >= 0; i--) {
      const day = daysBefore(last, i);
      daily.push(rows.get(day) ?? { day, ...emptyDay() });
    }
    const dimensions = Object.fromEntries(DIMENSIONS.map((d) => [d, []]));
    for (const r of this.sql.all(`SELECT dimension, value, SUM(count) AS count FROM metrics_daily_dimensions
      WHERE day >= ? AND day <= ? GROUP BY dimension, value ORDER BY count DESC, value`, first, last)) {
      dimensions[r.dimension]?.push({ value: r.value, count: r.count });
    }
    const since = Math.max(now - days * DAY_MS, now - SAMPLE_DAYS * DAY_MS);
    const samples = this.sql.all('SELECT bucket, online, in_rooms, rooms FROM metrics_samples WHERE bucket >= ? ORDER BY bucket',
      sampleBucket(since));
    const partyFirst = this.sql.get(`SELECT MIN(day) AS day FROM metrics_daily_dimensions WHERE dimension = 'party_size'`).day;
    const mixFirst = this.sql.get(`SELECT MIN(day) AS day FROM metrics_daily_dimensions WHERE dimension = 'platform_mix'`).day;
    const totals = this.totals(first, last);
    const previous = this.totals(daysBefore(first, days), daysBefore(first, 1));
    return { generated: now, time_zone: METRICS_TIME_ZONE, days, first, last, sample_ms: SAMPLE_MS, daily, dimensions,
      party_first_day: partyFirst ?? null, platform_mix_first_day: mixFirst ?? null, samples, totals, previous, comparison: compare(totals, previous) };
  }

  // Early Story Online rooms created and guest joins since the columns exist; the dashboard sums them, never a period.
  storyTotals() {
    return this.sql.get(`SELECT COALESCE(SUM(story_rooms), 0) AS rooms_created,
      COALESCE(SUM(story_guest_joins), 0) AS guest_joins FROM metrics_daily`);
  }

  totals(from, to) {
    const select = [...SUMMED.map((c) => `COALESCE(SUM(${c}), 0) AS ${c}`), ...PEAKS.map((c) => `COALESCE(MAX(${c}), 0) AS ${c}`)];
    return this.sql.get(`SELECT ${select.join(', ')} FROM metrics_daily WHERE day >= ? AND day <= ?`, from, to);
  }
}

// The platform counters one multiplayer room adds; none when its platforms were not tracked.
function platformRoom(mask, relayed) {
  const platforms = platformsOfMask(mask);
  if (!platforms.length) return {};
  const android = platforms.includes('Android');
  const cross = platforms.length > 1;
  const used = relayed ? 1 : 0;
  return {
    rooms_cross_platform: cross ? 1 : 0,
    rooms_android: android ? 1 : 0,
    rooms_android_cross: android && cross ? 1 : 0,
    rooms_android_pc: android && platforms.some((p) => PC_PLATFORMS.includes(p)) ? 1 : 0,
    rooms_android_relayed: android ? used : 0,
    rooms_android_cross_relayed: android && cross ? used : 0,
  };
}

const emptyDay = () => Object.fromEntries(DAY_COLUMNS.map((c) => [c, 0]));

// Relative change in percent; null when there was nothing before to compare with.
export const percentChange = (current, previous) => (previous > 0 ? Math.round(1000 * (current - previous) / previous) / 10 : null);

export const COMPARED = ['sessions', 'rooms_created', 'room_joins', 'rooms_with_guest', 'peak_online', 'peak_in_rooms'];

export function compare(current, previous) {
  return Object.fromEntries(COMPARED.map((c) => [c, { current: current[c], previous: previous[c],
    change: percentChange(current[c], previous[c]) }]));
}

const grew = (seen, live) => live.online > seen.online || live.in_rooms > seen.in_rooms || live.rooms > seen.rooms;

function raise(seen, live) {
  seen.online = Math.max(seen.online, live.online);
  seen.in_rooms = Math.max(seen.in_rooms, live.in_rooms);
  seen.rooms = Math.max(seen.rooms, live.rooms);
}
