// The Worker's edge: the private dashboard under /admin (DCP_DASHBOARD_TOKEN as a Bearer header) and the country the
// lobby may count, which only the edge sets. No Cloudflare import, so Node runs it in the tests.
import { ADMIN_CSS, ADMIN_HTML, ADMIN_JS } from './admin_page.js';
import { HISTORY_MAX_DAYS, countryCode } from './metrics.js';
import { sha256Hex } from './sha256.js';

export const COUNTRY_HEADER = 'X-DCP-Edge-Country';
export const DEFAULT_HISTORY_DAYS = 30;
const MIN_TOKEN_CHARS = 16;

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

// metrics: { summary(): Promise<object>, history(days): Promise<object> }, read only after the token matched.
export async function handleAdmin(request, env, metrics, log = () => {}) {
  const url = new URL(request.url);
  if (request.method !== 'GET') return json({ error: 'method_not_allowed' }, 405, { Allow: 'GET' });
  const asset = ASSETS[url.pathname];
  if (asset) return reply(asset[0], 200, asset[1]);
  const api = url.pathname === '/admin/api/summary' ? 'summary' : url.pathname === '/admin/api/history' ? 'history' : null;
  if (!api) return json({ error: 'not_found' }, 404);
  if (typeof env.DCP_DASHBOARD_TOKEN !== 'string' || env.DCP_DASHBOARD_TOKEN.length < MIN_TOKEN_CHARS) {
    return json({ error: 'not_configured' }, 503);
  }
  if (!tokenMatches(request.headers.get('Authorization'), env.DCP_DASHBOARD_TOKEN)) {
    return json({ error: 'unauthorized' }, 401, { 'WWW-Authenticate': 'Bearer' });
  }
  let days = null;
  if (api === 'history') {
    days = historyDays(url.searchParams);
    if (days === null) return json({ error: 'bad_request', detail: `days must be 1 to ${HISTORY_MAX_DAYS}` }, 400);
  }
  try {
    return json(api === 'summary' ? await metrics.summary() : await metrics.history(days));
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

export const edgeCountry = (cf) => countryCode(cf?.country);

// The request the lobby receives: any country header from the client is replaced by Cloudflare's.
export function withEdgeCountry(request, cf = request.cf) {
  const headers = new Headers(request.headers);
  headers.set(COUNTRY_HEADER, edgeCountry(cf));
  return new Request(request, { headers });
}
