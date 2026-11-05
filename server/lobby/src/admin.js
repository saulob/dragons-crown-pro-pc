// The Worker's edge: the private dashboard under /admin (DCP_DASHBOARD_TOKEN as a Bearer header) and the country the
// lobby may count, which only the edge sets. No Cloudflare import, so Node runs it in the tests.
import { ADMIN_CSS, ADMIN_HTML, ADMIN_JS } from './admin_page.js';
import { isTicket, logDownloadName, readStatusChange } from './incidents.js';
import { HISTORY_MAX_DAYS, countryCode } from './metrics.js';
import { sha256Hex } from './sha256.js';

export const COUNTRY_HEADER = 'X-DCP-Edge-Country';
export const LIMITED_HEADER = 'X-DCP-Edge-Limited';
const MAX_LIMITED_COUNT = 1_000_000;
export const DEFAULT_HISTORY_DAYS = 30;
const MIN_TOKEN_CHARS = 16;
const MAX_STATUS_BODY_CHARS = 8 * 1024;

const SECURITY_HEADERS = {
  'Cache-Control': 'no-store',
  'X-Content-Type-Options': 'nosniff',
  'Referrer-Policy': 'no-referrer',
  'X-Frame-Options': 'DENY',
  'Cross-Origin-Opener-Policy': 'same-origin',
  'Cross-Origin-Resource-Policy': 'same-origin',
  'Content-Security-Policy': "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self'; " +
    "base-uri 'none'; form-action 'none'; frame-ancestors 'none'",
};

const ASSETS = {
  '/admin': [ADMIN_HTML, 'text/html; charset=utf-8'],
  '/admin/': [ADMIN_HTML, 'text/html; charset=utf-8'],
  '/admin/app.css': [ADMIN_CSS, 'text/css; charset=utf-8'],
  '/admin/app.js': [ADMIN_JS, 'text/javascript; charset=utf-8'],
};

export const isAdminPath = (pathname) => pathname === '/admin' || pathname.startsWith('/admin/');

function reply(body, status, type, extra = {}) {
  return new Response(body, { status, headers: { ...SECURITY_HEADERS, 'Content-Type': type, ...extra } });
}

const json = (value, status = 200, extra = {}) => reply(JSON.stringify(value), status, 'application/json; charset=utf-8', extra);

// Compares digests so the time taken does not depend on where the texts differ.
export function tokenMatches(authorization, token) {
  if (typeof token !== 'string' || token.length < MIN_TOKEN_CHARS) return false;
  const given = typeof authorization === 'string' && authorization.startsWith('Bearer ') ? authorization.slice(7) : '';
  const a = sha256Hex(given);
  const b = sha256Hex(token);
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

// days=N for 1..HISTORY_MAX_DAYS, else null; no parameter is the default range.
export function historyDays(params) {
  const text = params.get('days');
  if (text === null) return DEFAULT_HISTORY_DAYS;
  if (!/^[1-9][0-9]{0,2}$/.test(text)) return null;
  const days = Number(text);
  return days <= HISTORY_MAX_DAYS ? days : null;
}

const INCIDENTS_PATH = '/admin/api/incidents';

// The dashboard API behind a path, or null. A report id is checked after the token, before anything reads it.
function adminApi(pathname) {
  if (pathname === '/admin/api/summary') return { name: 'summary' };
  if (pathname === '/admin/api/history') return { name: 'history' };
  if (pathname === INCIDENTS_PATH) return { name: 'incidents' };
  if (pathname.startsWith(`${INCIDENTS_PATH}/`)) {
    const [ticket, rest, ...more] = pathname.slice(INCIDENTS_PATH.length + 1).split('/');
    if (more.length === 0 && rest === undefined) return { name: 'incident', ticket };
    if (more.length === 0 && rest === 'log') return { name: 'incident_log', ticket };
    if (more.length === 0 && rest === 'status') return { name: 'incident_status', ticket };
  }
  return null;
}

async function saveIncidentStatus(request, ticket, metrics) {
  const text = await request.text();
  if (text.length > MAX_STATUS_BODY_CHARS) return json({ error: 'too_large' }, 413);
  let body = null;
  try {
    body = JSON.parse(text);
  } catch {
    return json({ error: 'bad_request', detail: 'invalid JSON' }, 400);
  }
  const { change, detail } = readStatusChange(body);
  if (!change) return json({ error: 'bad_request', detail }, 400);
  const saved = await metrics.setIncidentStatus(ticket, change);
  return saved ? json(saved) : json({ error: 'not_found' }, 404);
}

// metrics: { summary(), history(days), incidents(), incident(ticket), setIncidentStatus(ticket, change) } as promises, used
// only after the token matched.
export async function handleAdmin(request, env, metrics, log = () => {}) {
  const url = new URL(request.url);
  const api = adminApi(url.pathname);
  const method = api?.name === 'incident_status' ? 'POST' : 'GET';
  if (request.method !== method) return json({ error: 'method_not_allowed' }, 405, { Allow: method });
  const asset = ASSETS[url.pathname];
  if (asset) return reply(asset[0], 200, asset[1]);
  if (!api) return json({ error: 'not_found' }, 404);
  if (typeof env.DCP_DASHBOARD_TOKEN !== 'string' || env.DCP_DASHBOARD_TOKEN.length < MIN_TOKEN_CHARS) {
    return json({ error: 'not_configured' }, 503);
  }
  if (!tokenMatches(request.headers.get('Authorization'), env.DCP_DASHBOARD_TOKEN)) {
    return json({ error: 'unauthorized' }, 401, { 'WWW-Authenticate': 'Bearer' });
  }
  if (api.ticket !== undefined && !isTicket(api.ticket)) return json({ error: 'bad_request', detail: 'invalid report id' }, 400);
  let days = null;
  if (api.name === 'history') {
    days = historyDays(url.searchParams);
    if (days === null) return json({ error: 'bad_request', detail: `days must be 1 to ${HISTORY_MAX_DAYS}` }, 400);
  }
  try {
    if (api.name === 'summary') return json(await metrics.summary());
    if (api.name === 'history') return json(await metrics.history(days));
    if (api.name === 'incidents') return json(await metrics.incidents());
    if (api.name === 'incident_status') return await saveIncidentStatus(request, api.ticket, metrics);
    const incident = await metrics.incident(api.ticket);
    if (!incident) return json({ error: 'not_found' }, 404);
    if (api.name === 'incident') return json(incident);
    return reply(incident.log_text, 200, 'text/plain; charset=utf-8',
      { 'Content-Disposition': `attachment; filename="${logDownloadName(incident)}"` });
  } catch (error) {
    log(`admin: metrics unavailable: ${error?.message ?? error}`);
    return json({ error: 'unavailable' }, 502);
  }
}

// The Durable Object's internal metrics routes; a WebSocket upgrade is never one, whatever its path.
export function internalMetricsRoute(request) {
  if (request.headers.get('Upgrade')?.toLowerCase() === 'websocket') return null;
  const url = new URL(request.url);
  if (url.pathname === '/metrics/summary') return { kind: 'summary' };
  if (url.pathname === '/metrics/history') {
    const days = historyDays(url.searchParams);
    return days === null ? { kind: 'bad_request' } : { kind: 'history', days };
  }
  return null;
}

// The same rule for the object's report routes, which the Worker only calls after the token matched.
export function internalIncidentRoute(request) {
  if (request.headers.get('Upgrade')?.toLowerCase() === 'websocket') return null;
  const { pathname } = new URL(request.url);
  if (pathname === '/incidents') return { kind: 'list' };
  const [ticket, rest, ...more] = pathname.startsWith('/incidents/') ? pathname.slice('/incidents/'.length).split('/') : [];
  if (!isTicket(ticket) || more.length > 0) return null;
  if (rest === undefined) return { kind: 'one', ticket };
  return rest === 'status' && request.method === 'POST' ? { kind: 'status', ticket } : null;
}

export const edgeCountry = (cf) => countryCode(cf?.country);

// The request the lobby receives: any edge header from the client is replaced by the Worker's.
export function withEdgeCountry(request, cf = request.cf, limited = 0) {
  const headers = new Headers(request.headers);
  headers.set(COUNTRY_HEADER, edgeCountry(cf));
  if (limited > 0) headers.set(LIMITED_HEADER, String(limited));
  else headers.delete(LIMITED_HEADER);
  return new Request(request, { headers });
}

export function limitedCount(headers) {
  const count = Number(headers.get(LIMITED_HEADER));
  return Number.isInteger(count) && count > 0 ? Math.min(count, MAX_LIMITED_COUNT) : 0;
}
