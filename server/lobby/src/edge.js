// The Worker's gate for WebSocket upgrades, before the Durable Object exists in the request's path. No Cloudflare
// import, so Node runs it in the tests.
import { withEdgeCountry } from './admin.js';

const MAX_PENDING_LIMITED = 1_000_000;
const RETRY_AFTER_SECONDS = '60';

// Per-address limits group IPv6 clients by their /64.
export function ipGroup(ip) {
  if (typeof ip !== 'string' || !ip.includes(':')) return String(ip);
  const mapped = /^::ffff:(\d{1,3}(?:\.\d{1,3}){3})$/i.exec(ip);
  if (mapped) return mapped[1];
  const [head, tail] = ip.split('::');
  const before = head ? head.split(':') : [];
  const after = tail ? tail.split(':') : [];
  const gap = tail === undefined ? [] : Array(Math.max(0, 8 - before.length - after.length)).fill('0');
  return [...before, ...gap, ...after].slice(0, 4).map((g) => g.toLowerCase().replace(/^0+(?=.)/, '')).join(':');
}

// Without the binding (development) or when it fails, upgrades pass: the lobby's own limits still apply.
export async function upgradeAllowed(limiter, ip) {
  if (!limiter || !ip) return true;
  try {
    return (await limiter.limit({ key: `ws:${ipGroup(ip)}` })).success !== false;
  } catch {
    return true;
  }
}

// Upgrades refused here never reach the lobby, so their count rides on the next one this isolate forwards.
let limitedSinceForward = 0;

export async function handleUpgrade(request, env, lobby) {
  if (!(await upgradeAllowed(env.WS_UPGRADE_LIMITER, request.headers.get('CF-Connecting-IP')))) {
    limitedSinceForward = Math.min(limitedSinceForward + 1, MAX_PENDING_LIMITED);
    return new Response('Too many connection attempts', { status: 429, headers: { 'Retry-After': RETRY_AFTER_SECONDS } });
  }
  const limited = limitedSinceForward;
  limitedSinceForward = 0;
  return lobby.fetch(withEdgeCountry(request, request.cf, limited));
}
