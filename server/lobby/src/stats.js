// The public statistics the launcher's Home page reads: anonymous aggregate counts, no token. No Cloudflare import, so
// Node runs it in the tests.
import { PARTY_CLASSES } from './party.js';

export const STATS_PATH = '/stats';
export const STATS_TTL_MS = 30_000;
export const STATS_RETRY_MS = 5_000;
export const LIVE_STALE_MS = 3 * 60_000;
export const MONTH_STALE_MS = 30 * 60_000;
export const LIFETIME_STALE_MS = 60 * 60_000;

// A section is served when its core fields are valid; the optional ones are null when missing and never take the section
// down with them.
const SCHEMA = {
  live: { core: ['online', 'lobby', 'open_rooms'], optional: ['in_rooms', 'peak_today'], stale: LIVE_STALE_MS },
  today: { core: ['rooms_created'], optional: ['sessions', 'joins'], stale: LIVE_STALE_MS },
  last_30_days: { core: ['rooms_created', 'successful_joins', 'parties_reported'],
    optional: ['rooms_with_guest', 'classes'], stale: MONTH_STALE_MS },
  lifetime: { core: [], optional: ['sessions', 'players', 'since'], stale: LIFETIME_STALE_MS, lenient: true },
};
const SECTIONS = Object.keys(SCHEMA);
const REQUIRED_SECTIONS = SECTIONS.filter((name) => SCHEMA[name].core.length > 0);

const HEADERS = {
  'Content-Type': 'application/json; charset=utf-8',
  'X-Content-Type-Options': 'nosniff',
  'Referrer-Policy': 'no-referrer',
};
const HEALTHY = `public, max-age=${STATS_TTL_MS / 1000}`;

const isCount = (value) => Number.isSafeInteger(value) && value >= 0;
const classes = (list) => (Array.isArray(list) && list.length <= PARTY_CLASSES.length * 4 && list.every((row) => row && typeof row === 'object'
  && PARTY_CLASSES.includes(row.class) && isCount(row.count) && row.count > 0)
  ? list.map((row) => ({ class: row.class, count: row.count })) : null);
const day = (value) => (typeof value === 'string' && /^\d{4}-\d{2}-\d{2}$/.test(value) ? value : null);
const valid = (field, value) => (field === 'classes' ? classes(value) : field === 'since' ? day(value) : isCount(value) ? value : null);

// The fields of each section the lobby answered, whitelisted one by one; null where missing or malformed. A section whose
// core is incomplete is null as a whole.
export function publicSections(showcase) {
  const sections = {};
  for (const name of SECTIONS) {
    const source = showcase?.[name];
    const { core, optional, lenient } = SCHEMA[name];
    const fields = Object.fromEntries([...core, ...optional].map((field) => [field, valid(field, source?.[field])]));
    sections[name] = (lenient || (source && typeof source === 'object')) && core.every((field) => fields[field] !== null)
      ? { ...fields, ...(name === 'last_30_days' ? { average_party_size: averageOf(source, fields) } : {}) } : null;
  }
  return sections;
}

function averageOf(source, fields) {
  const average = source.average_party_size;
  return fields.parties_reported > 0 && Number.isFinite(average) && average > 0 ? average : null;
}

function reply(body, status, cache, extra = {}) {
  return new Response(body, { status, headers: { ...HEADERS, 'Cache-Control': cache, ...extra } });
}

// Built once per isolate. Each field keeps its last good value, so one failed or partial refresh erases nothing. `read()` is
// the lobby's showcase as a promise, called at most once per STATS_TTL_MS, or STATS_RETRY_MS after a core section failed.
export function statsHandler(now = () => Date.now()) {
  const kept = new Map();
  let answer = null;

  async function refresh(read) {
    const at = now();
    let fresh;
    try {
      fresh = publicSections(await read());
    } catch {
      fresh = {};
    }
    const sections = {};
    for (const name of SECTIONS) {
      const { core, optional, stale } = SCHEMA[name];
      for (const field of [...core, ...optional]) {
        const value = fresh[name]?.[field];
        if (value !== null && value !== undefined) kept.set(`${name}.${field}`, { at, value });
      }
      const fields = {};
      for (const field of [...core, ...optional]) {
        const last = kept.get(`${name}.${field}`);
        fields[field] = last && at - last.at < stale ? last.value : null;
      }
      const average = fresh[name]?.average_party_size;
      sections[name] = core.every((field) => fields[field] !== null)
        ? { ...fields, ...(name === 'last_30_days' ? { average_party_size: average ?? null } : {}) } : null;
    }
    const complete = REQUIRED_SECTIONS.every((name) => fresh[name]);
    const empty = REQUIRED_SECTIONS.every((name) => sections[name] === null);
    answer = {
      at: complete ? at : at - STATS_TTL_MS + STATS_RETRY_MS,
      status: empty ? 503 : 200,
      cache: complete ? HEALTHY : 'no-store',
      body: JSON.stringify(empty ? { error: 'unavailable' } : { generated: new Date(at).toISOString(), ...sections }),
    };
  }

  return async (request, read) => {
    if (request.method !== 'GET' && request.method !== 'HEAD') {
      return reply(JSON.stringify({ error: 'method_not_allowed' }), 405, 'no-store', { Allow: 'GET, HEAD' });
    }
    if (!answer || now() - answer.at >= STATS_TTL_MS) await refresh(read);
    return reply(request.method === 'HEAD' ? null : answer.body, answer.status, answer.cache);
  };
}
