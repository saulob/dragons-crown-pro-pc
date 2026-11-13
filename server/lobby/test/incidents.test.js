import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';

import { startLobbyServer } from '../local/server.js';
import { ADMIN_CSS, ADMIN_HTML, ADMIN_JS } from '../src/admin_page.js';
import { handleAdmin, internalIncidentRoute } from '../src/admin.js';
import { PROTECTION } from '../src/abuse.js';
import { INCIDENT, INCIDENT_STATUSES, isTicket, logDownloadName, readIncident } from '../src/incidents.js';
import { LIMITS, Lobby } from '../src/lobby.js';
import { Client, makeLobby } from './harness.js';

const MINUTE_MS = 60 * 1000;
const HOUR_MS = 60 * MINUTE_MS;
const DAY_MS = 24 * HOUR_MS;
// Far enough apart that only the daily limit can refuse.
const SPACED_MS = 31 * MINUTE_MS;
const HOURLY = 'incident_hourly_limit';
const DAILY = 'incident_daily_limit';
const TOKEN = 'a-long-dashboard-token';
const ENV = { DCP_DASHBOARD_TOKEN: TOKEN };

const report = (fields = {}) => ({
  description: 'The game froze on the first boss.\nIt happened twice.',
  launcher_version: '0.9.3',
  game_build: 'CUSA10454 1.03 USA',
  game_version: '1.03',
  cusa: 'CUSA10454',
  operating_system: 'Windows 11 (build 26200)',
  log_file: 'dragons_crown_pro_042.log',
  log_clipped: 'no',
  log_text: 'Runtime: guest thread created\nError: lost device\n',
  ...fields,
});

let nextAddress = 1;

function greeted(env, ip = `203.0.113.${nextAddress++}`) {
  const client = env.connect(ip);
  assert.equal(client.hello().code, 'ok');
  return client;
}

const file = (client, fields) => client.call('incident.file', report(fields));

const quotaRows = (env) => env.host.sql.get('SELECT COUNT(*) AS n FROM incident_quota').n;

function fillAddressQuota(env, ip) {
  const key = env.lobby.incidents.key('address', ip);
  for (let i = 0; i < INCIDENT.perAddressPerDay; i++) {
    env.host.sql.run('INSERT INTO incident_quota (account_key, address_key, ms) VALUES (?, ?, ?)', `other${i}`, key, env.clock.now);
  }
}

function adminSource(env) {
  return {
    summary: async () => env.lobby.metricsSummary(),
    history: async (days) => env.lobby.metricsHistory(days),
    incidents: async () => env.lobby.incidentList(),
    incident: async (ticket) => env.lobby.incidentForAdmin(ticket),
    setIncidentStatus: async (ticket, change) => env.lobby.setIncidentStatus(ticket, change),
    deleteIncident: async (ticket) => env.lobby.deleteIncident(ticket),
  };
}

const adminGet = (env, path, token = TOKEN) => handleAdmin(new Request(`https://dcp-lobby.example${path}`,
  { headers: token ? { Authorization: `Bearer ${token}` } : {} }), ENV, adminSource(env));

const adminPost = (env, path, body, token = TOKEN) => handleAdmin(new Request(`https://dcp-lobby.example${path}`,
  { method: 'POST', body, headers: token ? { Authorization: `Bearer ${token}` } : {} }), ENV, adminSource(env));

const adminDelete = (env, path, token = TOKEN) => handleAdmin(new Request(`https://dcp-lobby.example${path}`,
  { method: 'DELETE', headers: token ? { Authorization: `Bearer ${token}` } : {} }), ENV, adminSource(env));

const saveStatus = (env, ticket, change) => adminPost(env, `/admin/api/incidents/${ticket}/status`, JSON.stringify(change));

test('a filed report answers a DCP ticket and stores exactly the sent fields', () => {
  const env = makeLobby();
  const client = greeted(env);
  const answer = file(client);
  assert.equal(answer.code, 'ok');
  assert.ok(isTicket(answer.ticket), answer.ticket);
  assert.deepEqual(Object.keys(answer).sort(), ['code', 're', 'receipt', 'submitted_at', 'ticket']);
  const stored = env.lobby.incident(answer.ticket);
  assert.equal(stored.ms, env.clock.now);
  for (const [field, value] of Object.entries(report())) assert.equal(stored[field], value, field);
  assert.deepEqual(Object.keys(stored).sort(), [...Object.keys(report()), 'ms', 'ticket'].sort());
});

test('a stored report holds no account, name, address or country', () => {
  const env = makeLobby();
  const client = env.connect('198.51.100.77', 'BR');
  client.hello({ name: 'SecretName' });
  const { ticket } = file(client);
  const columns = env.host.sql.all('PRAGMA table_info(incidents)').map((c) => c.name).sort();
  assert.deepEqual(columns, ['cusa', 'description', 'game_build', 'game_version', 'launcher_version', 'log_clipped', 'log_file',
    'log_text', 'ms', 'operating_system', 'ticket']);
  const text = JSON.stringify(env.lobby.incident(ticket));
  for (const secret of [client.account, 'SecretName', '198.51.100.77', 'BR"']) assert.ok(!text.includes(secret), secret);
  const quota = JSON.stringify(env.host.sql.all('SELECT * FROM incident_quota'));
  for (const secret of [client.account, '198.51.100.77']) assert.ok(!quota.includes(secret), secret);
});

test('malformed reports are refused and store nothing', () => {
  const env = makeLobby();
  const client = greeted(env);
  const cases = [
    { description: '' }, { description: '   \n ' }, { description: 12 }, { description: undefined },
    { log_text: '' }, { log_text: ' \n' }, { log_text: null },
    { log_file: '../../secret.ini' }, { log_file: 'dragons_crown_pro_042.log.bak' }, { log_file: 'last_run.log' },
    { cusa: 'CUSA1' }, { cusa: 'PPSA01234' }, { launcher_version: '' }, { launcher_version: '<b>1</b>' },
    { operating_system: '' }, { log_clipped: true }, { log_clipped: 'maybe' }, { game_build: 7 },
    { account_name: 'smuggled' }, { hostname: 'smuggled' },
  ];
  for (const fields of cases) {
    env.advance(120000);
    assert.equal(file(client, fields).code, 'bad_request', JSON.stringify(fields));
  }
  assert.equal(env.lobby.incidentList().incidents.length, 0);
  assert.equal(client.raw('{"op":"incident.file"').code, 'bad_request');
  const anonymous = env.connect('198.51.100.5');
  assert.equal(file(anonymous).code, 'no_session');
});

test('oversized text and logs are refused, whatever the client claims', () => {
  const env = makeLobby();
  const client = greeted(env);
  const send = (fields) => {
    env.advance(120000);
    return file(client, fields);
  };
  assert.equal(send({ description: 'x'.repeat(INCIDENT.descriptionChars + 1) }).code, 'too_large');
  assert.equal(send({ log_text: 'y'.repeat(INCIDENT.logBytes + 1) }).code, 'too_large');
  assert.equal(send({ log_text: 'é'.repeat(INCIDENT.logBytes / 2 + 1) }).code, 'too_large');
  assert.equal(send({ game_build: 'z'.repeat(INCIDENT.metadataChars + 1) }).code, 'too_large');
  assert.equal(send({ log_size: 10 }).code, 'bad_request');
  assert.equal(env.lobby.incidentList().incidents.length, 0);
  assert.equal(send({ description: '🐉'.repeat(INCIDENT.descriptionChars) }).code, 'ok');
  assert.equal(send({ log_text: 'y'.repeat(INCIDENT.logBytes) }).code, 'ok');
  // One frame over the limit is not even parsed.
  const huge = JSON.stringify({ op: 'incident.file', seq: 9, ...report(), log_text: 'y'.repeat(INCIDENT.frameChars) });
  assert.equal(client.raw(huge).code, 'too_large');
  // Other operations keep the small frame limit.
  assert.equal(client.call('worlds', { padding: 'x'.repeat(LIMITS.frameBytes) }).code, 'too_large');
});

test('the largest log the client can send, all newlines, fits the frame limit', () => {
  const body = report({ description: '\n'.repeat(1) + 'é'.repeat(INCIDENT.descriptionChars - 1), log_text: '\n'.repeat(INCIDENT.logBytes) });
  const frame = JSON.stringify({ op: 'incident.file', seq: 0x7fffffff, ...body });
  assert.ok(frame.length <= INCIDENT.frameChars, String(frame.length));
});

test('the account policy is two reports an hour and ten a day', () => {
  assert.equal(INCIDENT.perAccountPerHour, 2);
  assert.equal(INCIDENT.perAccountPerDay, 10);
});

test('an hour takes two reports and refuses the third without storing it', () => {
  const env = makeLobby();
  const client = greeted(env);
  assert.equal(file(client).code, 'ok');
  env.advance(MINUTE_MS);
  assert.equal(file(client).code, 'ok');
  env.advance(MINUTE_MS);
  const refused = file(client);
  assert.equal(refused.code, HOURLY);
  assert.deepEqual(Object.keys(refused).sort(), ['code', 're']);
  assert.equal(env.lobby.incidentList().incidents.length, 2);
  assert.equal(quotaRows(env), 2);
  assert.equal(env.lobby.metrics.pending.get('incident_hourly_limited'), 1);
});

const accountQuotaRows = (env, client) => env.host.sql.get('SELECT COUNT(*) AS n FROM incident_quota WHERE account_key = ?',
  env.lobby.incidents.key('account', client.account)).n;

function sameAccount(env, client, ip = client.ip) {
  const session = env.connect(ip);
  session.account = client.account;
  session.secret = client.secret;
  assert.equal(session.hello().code, 'ok');
  return session;
}

test('four reports in a row on one connection: accept, accept, refuse, refuse', () => {
  const env = makeLobby();
  const client = greeted(env);
  const answers = [file(client), file(client), file(client), file(client)];
  assert.deepEqual(answers.map((a) => a.code), ['ok', 'ok', HOURLY, HOURLY]);
  assert.deepEqual(env.lobby.incidentList().incidents.map((r) => r.ticket).sort(), [answers[0].ticket, answers[1].ticket].sort());
  assert.equal(accountQuotaRows(env, client), 2);
  assert.equal(quotaRows(env), 2);
  assert.equal(env.lobby.metrics.pending.get('incident_hourly_limited'), 2);
  assert.equal(env.lobby.metrics.pending.get('incident_limited'), undefined);
});

test('four reports within a minute, each on its own connection as the launcher sends them', () => {
  const env = makeLobby();
  const client = greeted(env);
  const codes = [];
  for (let i = 0; i < 4; i++) {
    const session = sameAccount(env, client);
    codes.push(file(session).code);
    session.drop();
    env.advance(15000);
  }
  assert.deepEqual(codes, ['ok', 'ok', HOURLY, HOURLY]);
  assert.equal(env.lobby.incidentList().incidents.length, 2);
  assert.equal(accountQuotaRows(env, client), 2);
});

test('of two reports arriving together after one accepted, only one is accepted', () => {
  const env = makeLobby();
  const first = greeted(env);
  const second = sameAccount(env, first);
  assert.equal(file(first).code, 'ok');
  const codes = [file(first).code, file(second).code];
  assert.deepEqual(codes, ['ok', HOURLY]);
  assert.equal(env.lobby.incidentList().incidents.length, 2);
  assert.equal(accountQuotaRows(env, first), 2);
});

function failingOn(env, statement) {
  const { run } = env.host.sql;
  env.host.sql.run = (query, ...args) => {
    if (query.startsWith(statement)) throw new Error('disk full');
    run(query, ...args);
  };
  return () => {
    env.host.sql.run = run;
  };
}

for (const statement of ['INSERT INTO incidents', 'INSERT INTO incident_quota', 'INSERT INTO incident_tracking', 'DELETE FROM incidents']) {
  test(`a report and its quota row are stored together: ${statement} failing leaves neither`, () => {
    const env = makeLobby({ log: () => {} });
    const client = greeted(env);
    const restore = failingOn(env, statement);
    assert.equal(file(client).code, 'internal');
    restore();
    assert.equal(env.lobby.incidentList().incidents.length, 0);
    assert.equal(quotaRows(env), 0);
    assert.equal(env.lobby.metrics.pending.get('incident_filed'), undefined);
    assert.equal(file(client).code, 'ok');
    assert.equal(file(client).code, 'ok');
    assert.equal(file(client).code, HOURLY);
  });
}

test('a message runs to its answer without yielding, so two requests cannot see the same quota', () => {
  assert.notEqual(Lobby.prototype.message.constructor.name, 'AsyncFunction');
  const worker = readFileSync(new URL('../src/worker.js', import.meta.url), 'utf8');
  assert.match(worker, /\n {2}webSocketMessage\(ws, message\) \{/);
  for (const file of ['../src/lobby.js', '../src/incidents.js']) {
    assert.doesNotMatch(readFileSync(new URL(file, import.meta.url), 'utf8'), /\b(async|await)\b/, file);
  }
});

test('the hour rolls: a report frees its place an hour after it was accepted, not on the clock hour', () => {
  const env = makeLobby();
  const client = greeted(env);
  assert.equal(file(client).code, 'ok');
  env.advance(40 * MINUTE_MS);
  assert.equal(file(client).code, 'ok');
  env.advance(19 * MINUTE_MS);
  assert.equal(file(client).code, HOURLY);
  env.advance(MINUTE_MS);
  assert.equal(file(client).code, 'ok');
  env.advance(MINUTE_MS);
  assert.equal(file(client).code, HOURLY);
  env.advance(39 * MINUTE_MS);
  assert.equal(file(client).code, 'ok');
  assert.equal(env.lobby.incidentList().incidents.length, 4);
});

test('a rolling day takes ten reports and refuses the eleventh without storing it', () => {
  const env = makeLobby();
  const client = greeted(env);
  const first = env.clock.now;
  for (let i = 0; i < INCIDENT.perAccountPerDay; i++) {
    assert.equal(file(client).code, 'ok', `report ${i}`);
    env.advance(SPACED_MS);
  }
  const refused = file(client);
  assert.equal(refused.code, DAILY);
  assert.deepEqual(Object.keys(refused).sort(), ['code', 're']);
  assert.equal(env.lobby.incidentList().incidents.length, INCIDENT.perAccountPerDay);
  assert.equal(env.lobby.metrics.pending.get('incident_daily_limited'), 1);
  env.clock.now = first + DAY_MS - 1;
  assert.equal(file(client).code, DAILY);
  env.clock.now = first + DAY_MS;
  assert.equal(file(client).code, 'ok');
  assert.equal(quotaRows(env), INCIDENT.perAccountPerDay);
});

test('with both account limits reached the player hears about the hour', () => {
  const env = makeLobby();
  const client = greeted(env);
  for (let i = 0; i < INCIDENT.perAccountPerDay - 1; i++) {
    assert.equal(file(client).code, 'ok', `report ${i}`);
    env.advance(SPACED_MS);
  }
  assert.equal(file(client).code, 'ok');
  fillAddressQuota(env, client.ip);
  env.advance(MINUTE_MS);
  assert.equal(file(client).code, HOURLY);
  env.advance(HOUR_MS);
  assert.equal(file(client).code, DAILY);
});

test('the quota is the same on another connection and after a restart of the object', () => {
  const env = makeLobby();
  const client = greeted(env);
  file(client);
  file(client);
  const elsewhere = env.connect('198.51.100.9');
  elsewhere.account = client.account;
  elsewhere.hello();
  assert.equal(file(elsewhere).code, HOURLY);
  const restarted = new Client(new Lobby(env.host), env.host, '198.51.100.10');
  restarted.account = client.account;
  restarted.hello();
  assert.equal(file(restarted).code, HOURLY);
  assert.equal(quotaRows(env), 2);
});

test('quota rows are deleted a day after their report and never counted again', () => {
  const env = makeLobby();
  const client = greeted(env);
  const key = env.lobby.incidents.key('account', client.account);
  const addOld = (age) => env.host.sql.run('INSERT INTO incident_quota (account_key, address_key, ms) VALUES (?, ?, ?)', key, 'old',
    env.clock.now - age);
  for (let i = 0; i < INCIDENT.perAccountPerDay; i++) addOld(DAY_MS + i);
  assert.equal(file(client).code, 'ok');
  assert.equal(quotaRows(env), 1);
});

test('a refused report costs no quota, so the two accepted ones still fit', () => {
  const env = makeLobby();
  const client = greeted(env);
  for (const fields of [{ description: '' }, { log_text: '' }, { log_file: 'x' }, { description: 'x'.repeat(INCIDENT.descriptionChars + 1) }]) {
    assert.notEqual(file(client, fields).code, 'ok');
  }
  assert.equal(client.raw('{"op":"incident.file"').code, 'bad_request');
  assert.equal(quotaRows(env), 0);
  assert.equal(file(client).code, 'ok');
  assert.equal(file(client).code, 'ok');
});

test('one player at their daily limit leaves room for the others behind the same address', () => {
  const env = makeLobby();
  const address = '198.51.100.30';
  const busy = greeted(env, address);
  for (let i = 0; i < INCIDENT.perAccountPerDay; i++) {
    assert.equal(file(busy).code, 'ok', `report ${i}`);
    env.advance(SPACED_MS);
  }
  assert.equal(file(busy).code, DAILY);
  assert.equal(file(greeted(env, address)).code, 'ok');
  assert.ok(INCIDENT.perAddressPerDay > INCIDENT.perAccountPerDay);
});

test('an address and the whole service have their own limits, refused with the plain limit code', () => {
  const env = makeLobby();
  const address = '198.51.100.20';
  fillAddressQuota(env, address);
  assert.equal(file(greeted(env, address)).code, 'limit');
  assert.equal(file(greeted(env, '198.51.100.21')).code, 'ok');
  assert.equal(env.lobby.metrics.pending.get('incident_limited'), 1);

  const flood = makeLobby();
  for (let i = 0; i < INCIDENT.perDay; i++) {
    flood.host.sql.run('INSERT INTO incident_quota (account_key, address_key, ms) VALUES (?, ?, ?)', `a${i}`, `b${i}`, flood.clock.now);
  }
  const refused = file(greeted(flood));
  assert.equal(refused.code, 'limit');
  assert.deepEqual(Object.keys(refused).sort(), ['code', 're']);
});

test('refused attempts spend the in-memory guard, not SQL, and are counted', () => {
  const env = makeLobby();
  const client = greeted(env);
  let lastCode = '';
  for (let i = 0; i < PROTECTION.incidentAccount.burst + 1; i++) lastCode = file(client, { description: '' }).code;
  assert.equal(lastCode, 'limit');
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM incident_quota').n, 0);
  assert.ok(env.lobby.metrics.pending.get('incident_limited') >= 1);
});

test('only the newest reports are kept', () => {
  const env = makeLobby();
  for (let i = 0; i < INCIDENT.kept; i++) {
    env.lobby.incidents.file(report({ description: `old ${i}` }), `DCP-OLD${String(i).padStart(3, '0')}`, 'a', 'b', env.clock.now + i);
  }
  env.host.sql.run('DELETE FROM incident_quota');
  env.clock.now += INCIDENT.kept + 1;
  const { ticket } = file(greeted(env));
  const { incidents } = env.lobby.incidentList();
  assert.equal(incidents.length, INCIDENT.kept);
  assert.equal(incidents[0].ticket, ticket);
  assert.equal(env.lobby.incident('DCP-OLD000'), null);
  assert.ok(env.lobby.incident('DCP-OLD001'));
});

test('the admin list is newest first and leaves the log out; one report has everything', async () => {
  const env = makeLobby();
  const client = greeted(env);
  const first = file(client, { description: `${'long '.repeat(90)}end` });
  env.advance(120000);
  const second = file(client);
  const list = await (await adminGet(env, '/admin/api/incidents')).json();
  assert.deepEqual(list.incidents.map((r) => r.ticket), [second.ticket, first.ticket]);
  assert.equal(list.incidents[1].preview.length, INCIDENT.previewChars);
  for (const row of list.incidents) {
    assert.equal(row.log_text, undefined);
    assert.equal(row.description, undefined);
    assert.equal(row.launcher_version, '0.9.3');
  }
  const one = await (await adminGet(env, `/admin/api/incidents/${second.ticket}`)).json();
  assert.equal(one.log_text, report().log_text);
  assert.equal(one.description, report().description);
});

test('admin report routes need the existing admin token and take only well-formed report ids', async () => {
  const env = makeLobby();
  const { ticket } = file(greeted(env));
  const paths = ['/admin/api/incidents', `/admin/api/incidents/${ticket}`, `/admin/api/incidents/${ticket}/log`];
  for (const path of paths) {
    for (const token of [null, 'wrong-token-of-enough-length']) {
      const response = await adminGet(env, path, token);
      assert.equal(response.status, 401, path);
      assert.ok(!(await response.text()).includes('first boss'), path);
    }
    assert.equal((await adminGet(env, path)).status, 200, path);
    const unconfigured = await handleAdmin(new Request(`https://dcp-lobby.example${path}`,
      { headers: { Authorization: `Bearer ${TOKEN}` } }), {}, adminSource(env));
    assert.equal(unconfigured.status, 503, path);
  }
  for (const suffix of ['', '/log']) {
    assert.equal((await adminGet(env, `/admin/api/incidents/DCP-ZZZZZZ${suffix}`)).status, 404);
    for (const id of ['nope', '1%20OR%201=1', 'dcp-abc234', 'DCP-ABC23', 'DCP-ABC2345', '..%2Fx']) {
      const response = await adminGet(env, `/admin/api/incidents/${id}${suffix}`);
      assert.equal(response.status, 400, id);
      assert.deepEqual(await response.json(), { error: 'bad_request', detail: 'invalid report id' });
      assert.equal((await adminGet(env, `/admin/api/incidents/${id}${suffix}`, null)).status, 401, id);
    }
  }
  for (const path of ['/admin/api/incidents/DCP-AAAAAA/x', `/admin/api/incidents/${ticket}/log/x`, `/admin/api/incidents/${ticket}/Log`]) {
    assert.equal((await adminGet(env, path)).status, 404, path);
  }
  for (const path of paths) {
    const post = await handleAdmin(new Request(`https://dcp-lobby.example${path}`, { method: 'POST',
      headers: { Authorization: `Bearer ${TOKEN}` } }), ENV, adminSource(env));
    assert.equal(post.status, 405, path);
  }
});

test('the log download is the stored log, byte for byte, as text/plain under a safe filename', async () => {
  const env = makeLobby();
  const logText = 'Runtime: tab\there\n\nLauncher: caf\u00e9 \u{1F409}\n\n\nError: no final newline';
  const { ticket } = file(greeted(env), { log_text: logText, log_file: 'dragons_crown_pro_042.log' });
  const response = await adminGet(env, `/admin/api/incidents/${ticket}/log`);
  assert.equal(response.status, 200);
  assert.equal(response.headers.get('Content-Type'), 'text/plain; charset=utf-8');
  assert.equal(response.headers.get('Content-Disposition'), `attachment; filename="${ticket}-dragons_crown_pro_042.log"`);
  assert.equal(response.headers.get('Cache-Control'), 'no-store');
  assert.equal(response.headers.get('X-Content-Type-Options'), 'nosniff');
  const body = Buffer.from(await response.arrayBuffer());
  assert.deepEqual(body, Buffer.from(logText, 'utf8'));
  assert.equal(env.host.sql.get('SELECT log_text FROM incidents WHERE ticket = ?', ticket).log_text, logText);
  assert.equal(JSON.stringify((await (await adminGet(env, `/admin/api/incidents/${ticket}`)).json()).log_text), JSON.stringify(logText));

  const attached = response.headers.get('Content-Disposition');
  assert.match(attached, /^attachment; filename="DCP-[A-HJ-NP-Z2-9]{6}-dragons_crown_pro_[0-9]{3,10}\.log"$/);
  assert.ok(!/[\r\n/\\;]/.test(attached.slice('attachment; filename="'.length, -1)));

  assert.equal(logDownloadName({ ticket: 'DCP-ABC234', log_file: 'dragons_crown_pro_007.log' }), 'DCP-ABC234-dragons_crown_pro_007.log');
  assert.equal(logDownloadName({ ticket: 'DCP-ABC234', log_file: '../../x"\r\nSet-Cookie: a=b' }), 'DCP-ABC234.log');
  assert.equal(logDownloadName({ ticket: 'DCP-ABC234', log_file: '' }), 'DCP-ABC234.log');
  assert.equal(logDownloadName({ ticket: 'x"\r\ny', log_file: 'dragons_crown_pro_007.log' }), 'dragons-crown-pro-report.log');
});

test('a report keeps its log in the Worker database and stays downloadable after a restart of the object', async () => {
  const env = makeLobby();
  const { ticket } = file(greeted(env), { log_text: 'Launcher: kept\n' });
  const columns = env.host.sql.all('PRAGMA table_info(incidents)').map((c) => c.name);
  assert.ok(columns.includes('log_text') && !columns.some((name) => /path|folder|dir|url/.test(name)));
  const restarted = new Lobby(env.host);
  assert.equal(restarted.incident(ticket).log_text, 'Launcher: kept\n');
  const response = await handleAdmin(new Request(`https://dcp-lobby.example/admin/api/incidents/${ticket}/log`,
    { headers: { Authorization: `Bearer ${TOKEN}` } }), ENV, { ...adminSource(env), incident: async (t) => restarted.incident(t) });
  assert.equal(await response.text(), 'Launcher: kept\n');
});

test('reports with the same timestamp still list newest first', async () => {
  const env = makeLobby();
  const tickets = [file(greeted(env)).ticket, file(greeted(env)).ticket, file(greeted(env)).ticket];
  const list = await (await adminGet(env, '/admin/api/incidents')).json();
  assert.deepEqual(list.incidents.map((r) => r.ticket), tickets.reverse());
});

test('the object answers its report routes only outside WebSocket upgrades', () => {
  const route = (path, headers = {}) => internalIncidentRoute(new Request(`https://lobby.internal${path}`, { headers }));
  assert.deepEqual(route('/incidents'), { kind: 'list' });
  assert.deepEqual(route('/incidents/DCP-ABC234'), { kind: 'one', ticket: 'DCP-ABC234' });
  assert.equal(route('/incidents/x'), null);
  assert.equal(route('/'), null);
  assert.equal(route('/incidents', { Upgrade: 'websocket' }), null);
});

test('fields are stored exactly as received, or refused: the Worker never rewrites a report', async () => {
  const exact = report({ description: '\n  two\tlines \n', log_text: '\n  indented\n\ttabbed\n\n', game_version: '' });
  const { incident } = readIncident(exact);
  assert.deepEqual({ ...incident }, { ...exact });
  const env = makeLobby();
  const { ticket } = file(greeted(env), exact);
  const stored = env.lobby.incident(ticket);
  for (const [field, value] of Object.entries(exact)) assert.equal(stored[field], value, field);

  for (const fields of [
    { description: 'one\r\ntwo' }, { description: 'one\u0000two' }, { description: 'one\u0085two' },
    { log_text: 'a\r\nb\n' }, { log_text: 'a\rb\n' }, { log_text: 'a\u0000b\n' }, { log_text: 'a\u007fb\n' },
    { game_build: ' CUSA10454 1.03 USA' }, { operating_system: 'Windows\n11' }, { launcher_version: '0.9.3 ' },
  ]) {
    assert.ok(readIncident(report(fields)).code, JSON.stringify(fields));
    assert.equal(readIncident(report(fields)).incident, undefined);
  }
});

class Node {
  constructor(tag) {
    Object.assign(this, { tag, children: [], listeners: {}, style: {}, className: '', hidden: false, value: '', ownText: '', attrs: {} });
  }

  get firstChild() { return this.children[0] ?? null; }

  get textContent() { return this.ownText + this.children.map((c) => c.textContent).join(''); }

  set textContent(text) {
    this.children = [];
    this.ownText = String(text);
  }

  appendChild(child) {
    this.children.push(child);
    return child;
  }

  removeChild(child) {
    this.children = this.children.filter((c) => c !== child);
    return child;
  }

  setAttribute(name, value) { this.attrs[name] = String(value); }

  addEventListener(type, fn) { (this.listeners[type] ??= []).push(fn); }

  fire(type, event = {}) { for (const fn of this.listeners[type] ?? []) fn({ clientX: 0, clientY: 0, preventDefault() {}, ...event }); }

  click() { this.clicked = true; }

  focus() {
    this.focused = true;
  }

  contains(node) { return node === this || this.children.some((c) => c.contains(node)); }

  find(predicate, found = []) {
    for (const child of this.children) {
      if (predicate(child)) found.push(child);
      child.find(predicate, found);
    }
    return found;
  }
}

const TOKEN_KEY = 'dcp-dashboard-token';
const VIEW_KEY = 'dcp-dashboard-view';

async function adminPage(env, { clipboard = true, stored = { [TOKEN_KEY]: TOKEN }, token = TOKEN, route } = {}) {
  const ids = new Map();
  const get = (id) => {
    if (!ids.has(id)) ids.set(id, new Node('div'));
    return ids.get(id);
  };
  const links = [];
  const document = {
    hidden: false,
    getElementById: get,
    createElement: (tag) => {
      const node = new Node(tag);
      if (tag === 'a') links.push(node);
      return node;
    },
    createElementNS: (ns, tag) => new Node(tag),
    createTextNode: (text) => Object.assign(new Node('#text'), { ownText: text }),
    querySelectorAll: () => [],
    addEventListener: (type, fn) => { (documentListeners[type] ??= []).push(fn); },
  };
  const documentListeners = {};
  get('range').value = '30';
  for (const [, id] of ADMIN_HTML.matchAll(/id="([a-z-]+)"[^>]*\shidden/g)) get(id).hidden = true;
  const requests = [];
  const fetch = async (path, options) => {
    requests.push({ path, authorization: options.headers.Authorization, method: options.method, body: options.body });
    if (route) return route(path, options);
    const sent = options.headers.Authorization.slice('Bearer '.length);
    if (options.method === 'POST') return adminPost(env, path, options.body, sent);
    if (options.method === 'DELETE') return adminDelete(env, path, sent);
    return adminGet(env, path, sent);
  };
  const blobs = [];
  const urls = { createObjectURL: (blob) => { blobs.push(blob); return 'blob:test'; }, revokeObjectURL() {} };
  const copied = [];
  const navigator = clipboard ? { clipboard: { writeText: async (text) => { copied.push(text); } } } : {};
  const store = new Map(Object.entries(stored));
  const storage = { getItem: (key) => store.get(key) ?? null, setItem: (key, value) => { store.set(key, String(value)); }, removeItem: (key) => { store.delete(key); } };
  const fire = (type, event = {}) => { for (const fn of documentListeners[type] ?? []) fn(event); };
  new Function('document', 'window', 'fetch', 'sessionStorage', 'setInterval', 'clearInterval', 'URL', 'navigator', ADMIN_JS)(
    document, { innerWidth: 1200, innerHeight: 800 }, fetch, storage, () => 0, () => {}, urls, navigator);
  const settle = async () => {
    for (let i = 0; i < 30; i++) await new Promise((resolve) => setImmediate(resolve));
  };
  await settle();
  return { get, settle, requests, links, blobs, copied, store, document, fire };
}

const cssRule = (selector) => ADMIN_CSS.split('\n').find((line) => line.startsWith(`${selector} {`)) ?? '';
const bugRows = (page) => page.get('bug-list').find((n) => n.tag === 'tr').map((row) => row.children.map((cell) => cell.textContent));
const ticketButtons = (page) => page.get('bug-list').find((n) => n.tag === 'button');
const paths = (page) => page.requests.map((r) => r.path).sort();

async function openBugs(page) {
  page.get('tab-bugs').fire('click');
  await page.settle();
}

test('the admin page has a Dashboard tab with every existing section and a Bugs tab', () => {
  const dashboard = ADMIN_HTML.indexOf('id="view-dashboard"');
  const bugs = ADMIN_HTML.indexOf('id="view-bugs"');
  assert.ok(dashboard > 0 && bugs > dashboard);
  assert.match(ADMIN_HTML, /<button id="tab-dashboard"[^>]*aria-selected="true"[^>]*>Dashboard<\/button>/);
  assert.match(ADMIN_HTML, /<button id="tab-bugs"[^>]*aria-selected="false"[^>]*>Bugs<\/button>/);
  assert.match(ADMIN_HTML, /id="view-bugs"[^>]*hidden/);
  assert.ok(!/id="view-dashboard"[^>]*hidden/.test(ADMIN_HTML));
  for (const id of ['range', 'refresh', 'c-online', 'c-peak', 'compare', 'activity', 'party-cards', 'usage', 'h-sessions', 'daily',
    'health', 'rejects', 'd-platform', 'd-version', 'd-country', 'abuse']) {
    const at = ADMIN_HTML.indexOf(`id="${id}"`);
    assert.ok(at > dashboard && at < bugs, id);
  }
  for (const id of ['bug-version', 'bug-build', 'bug-day', 'bug-list', 'bug-detail', 'bug-log', 'bug-download', 'bug-copy', 'bug-close']) {
    assert.ok(ADMIN_HTML.indexOf(`id="${id}"`) > bugs, id);
  }
  assert.ok(ADMIN_HTML.indexOf('id="signout"') < dashboard);
});

test('the Dashboard tab requests only its own data; the Bugs tab requests reports only when opened', async () => {
  const env = makeLobby();
  const page = await adminPage(env);
  assert.deepEqual(paths(page), ['/admin/api/history?days=30', '/admin/api/summary']);
  assert.equal(page.get('view-dashboard').hidden, false);
  assert.equal(page.get('c-online').textContent, '0');

  await openBugs(page);
  assert.deepEqual(paths(page), ['/admin/api/history?days=30', '/admin/api/incidents', '/admin/api/summary']);
  assert.equal(page.get('view-dashboard').hidden, true);
  assert.equal(page.get('view-bugs').hidden, false);
  assert.equal(page.get('tab-bugs').attrs['aria-selected'], 'true');
  assert.equal(page.get('tab-dashboard').attrs['aria-selected'], 'false');

  page.get('tab-dashboard').fire('click');
  await page.settle();
  assert.equal(page.get('view-dashboard').hidden, false);
  assert.equal(page.get('view-bugs').hidden, true);
  assert.equal(page.requests.filter((r) => r.path === '/admin/api/summary').length, 2);
  assert.ok(page.requests.every((r) => r.authorization === `Bearer ${TOKEN}` && !r.path.includes(TOKEN)));
});

test('the Bugs tab lists reports newest first, opens one and filters by version, build and date', async () => {
  const env = makeLobby();
  const client = greeted(env);
  const older = file(client, { launcher_version: '0.9.2', game_build: 'CUSA07001 1.03 Japan', cusa: 'CUSA07001', description: 'Older report' });
  env.advance(36 * 3600 * 1000);
  const newer = file(client, { description: 'Newer report' });
  const page = await adminPage(env);
  await openBugs(page);
  assert.deepEqual(ticketButtons(page).map((b) => b.textContent), [newer.ticket, older.ticket]);
  assert.equal(page.get('bug-count').textContent, '2 reports');
  const rows = bugRows(page);
  assert.deepEqual(rows[0], ['Report ID', 'Status', 'Submitted', 'Launcher', 'Game build', 'Operating system', 'Description']);
  assert.deepEqual([rows[2][0], rows[2][1], rows[2][3], rows[2][4], rows[2][5], rows[2][6]],
    [older.ticket, 'New', '0.9.2', 'CUSA07001 1.03 Japan', report().operating_system, 'Older report']);
  assert.equal(page.get('bug-detail').hidden, true);

  ticketButtons(page)[1].fire('click');
  await page.settle();
  assert.equal(page.get('bug-detail').hidden, false);
  assert.equal(page.get('bug-title').textContent, older.ticket);
  assert.equal(page.get('bug-text').textContent, 'Older report');
  assert.equal(page.get('bug-log').textContent, report().log_text);
  const facts = page.get('bug-facts').textContent;
  for (const part of [older.ticket, 'Sao Paulo', '0.9.2', 'CUSA07001 1.03 Japan', '1.03', 'CUSA07001', report().operating_system,
    report().log_file, 'Log clippedNo']) {
    assert.ok(facts.includes(part), part);
  }

  page.get('bug-version').value = '0.9.2';
  page.get('bug-version').fire('change');
  assert.deepEqual(ticketButtons(page).map((b) => b.textContent), [older.ticket]);
  assert.equal(page.get('bug-count').textContent, '1 of 2 reports');
  page.get('bug-version').value = '';
  page.get('bug-build').value = 'CUSA10454 1.03 USA';
  page.get('bug-build').fire('change');
  assert.deepEqual(ticketButtons(page).map((b) => b.textContent), [newer.ticket]);
  page.get('bug-build').value = '';
  page.get('bug-day').value = new Intl.DateTimeFormat('en-CA', { timeZone: 'America/Sao_Paulo' }).format(env.clock.now - 36 * 3600 * 1000);
  page.get('bug-day').fire('change');
  assert.deepEqual(ticketButtons(page).map((b) => b.textContent), [older.ticket]);
});

test('Download log fetches the stored log with the admin token and saves it under the server filename', async () => {
  const env = makeLobby();
  const logText = 'Launcher: line one\n\tindented\nError: last';
  const { ticket } = file(greeted(env), { log_text: logText });
  const page = await adminPage(env);
  await openBugs(page);
  ticketButtons(page)[0].fire('click');
  await page.settle();
  page.get('bug-download').fire('click');
  await page.settle();
  const download = page.requests.at(-1);
  assert.equal(download.path, `/admin/api/incidents/${ticket}/log`);
  assert.equal(download.authorization, `Bearer ${TOKEN}`);
  assert.equal(page.links.length, 1);
  assert.equal(page.links[0].download, `${ticket}-dragons_crown_pro_042.log`);
  assert.equal(page.links[0].href, 'blob:test');
  assert.equal(page.links[0].clicked, true);
  assert.equal(await page.blobs[0].text(), logText);
  assert.equal(page.get('bug-log').textContent, logText);
});

test('Copy log copies exactly the displayed log, and says so when the browser cannot', async () => {
  const env = makeLobby();
  const logText = 'Launcher: copy me\n';
  file(greeted(env), { log_text: logText });
  const page = await adminPage(env);
  await openBugs(page);
  ticketButtons(page)[0].fire('click');
  await page.settle();
  page.get('bug-copy').fire('click');
  await page.settle();
  assert.deepEqual(page.copied, [logText]);
  assert.equal(page.get('bug-note').textContent, 'Log copied.');

  const plain = await adminPage(env, { clipboard: false });
  await openBugs(plain);
  ticketButtons(plain)[0].fire('click');
  await plain.settle();
  plain.get('bug-copy').fire('click');
  assert.equal(plain.get('bug-note').textContent, 'Copying is not available in this browser.');
});

const selectedTickets = (page) => page.get('bug-list').find((n) => n.tag === 'tr' && n.className === 'selected')
  .map((row) => row.find((n) => n.tag === 'button')[0].textContent);

async function clickTicket(page, ticket) {
  ticketButtons(page).find((b) => b.textContent === ticket).fire('click');
  await page.settle();
}

async function twoReports() {
  const env = makeLobby();
  const client = greeted(env);
  const older = file(client, { launcher_version: '0.9.2', description: 'Older report', log_text: 'Launcher: older\n' });
  env.advance(HOUR_MS);
  const newer = file(client, { description: 'Newer report', log_text: 'Launcher: newer\n' });
  const page = await adminPage(env);
  await openBugs(page);
  return { env, page, older: older.ticket, newer: newer.ticket };
}

test('Close report hides the detail and the selection, keeps the list and filters, and asks the service nothing', async () => {
  const { page, older, newer } = await twoReports();
  await clickTicket(page, older);
  assert.equal(page.get('bug-detail').hidden, false);
  assert.deepEqual(selectedTickets(page), [older]);

  page.get('bug-version').value = '0.9.2';
  page.get('bug-version').fire('change');
  const asked = page.requests.length;
  page.get('bug-close').fire('click');
  await page.settle();
  assert.equal(page.requests.length, asked);
  assert.equal(page.get('bug-detail').hidden, true);
  assert.deepEqual(selectedTickets(page), []);
  assert.equal(page.get('view-bugs').hidden, false);
  assert.equal(page.get('tab-bugs').attrs['aria-selected'], 'true');
  assert.equal(page.get('bug-version').value, '0.9.2');
  assert.deepEqual(ticketButtons(page).map((b) => b.textContent), [older]);
  assert.equal(page.get('bug-count').textContent, '1 of 2 reports');

  page.get('bug-version').value = '';
  page.get('bug-version').fire('change');
  assert.deepEqual(ticketButtons(page).map((b) => b.textContent), [newer, older]);

  await clickTicket(page, older);
  assert.equal(page.get('bug-detail').hidden, false);
  assert.equal(page.get('bug-title').textContent, older);
  assert.deepEqual(selectedTickets(page), [older]);
});

test('a closed report cannot be copied or downloaded, and a report still loading when Close is pressed stays closed', async () => {
  const { page, older, newer } = await twoReports();
  await clickTicket(page, older);
  ticketButtons(page).find((b) => b.textContent === newer).fire('click');
  page.get('bug-close').fire('click');
  await page.settle();
  assert.equal(page.get('bug-detail').hidden, true);
  assert.deepEqual(selectedTickets(page), []);

  const asked = page.requests.length;
  page.get('bug-download').fire('click');
  page.get('bug-copy').fire('click');
  await page.settle();
  assert.equal(page.requests.length, asked);
  assert.deepEqual(page.copied, []);
});

test('another Report ID switches directly; the one on screen closes; one still loading does not', async () => {
  const { page, older, newer } = await twoReports();
  await clickTicket(page, older);
  await clickTicket(page, newer);
  assert.equal(page.get('bug-detail').hidden, false);
  assert.equal(page.get('bug-title').textContent, newer);
  assert.equal(page.get('bug-text').textContent, 'Newer report');
  assert.equal(page.get('bug-log').textContent, 'Launcher: newer\n');
  assert.deepEqual(selectedTickets(page), [newer]);

  await clickTicket(page, newer);
  assert.equal(page.get('bug-detail').hidden, true);
  assert.deepEqual(selectedTickets(page), []);

  ticketButtons(page).find((b) => b.textContent === older).fire('click');
  await clickTicket(page, older);
  assert.equal(page.get('bug-detail').hidden, false);
  assert.equal(page.get('bug-title').textContent, older);
});

test('a report that could not be opened can be tried again by clicking it', async () => {
  const { env, page, older } = await twoReports();
  const { sql } = env.host;
  const { get } = sql;
  sql.get = () => undefined;
  await clickTicket(page, older);
  assert.equal(page.get('bug-count').textContent, `Could not open ${older}.`);
  assert.equal(page.get('bug-detail').hidden, true);
  sql.get = get;
  await clickTicket(page, older);
  assert.equal(page.get('bug-detail').hidden, false);
  assert.equal(page.get('bug-title').textContent, older);
});

function failingToOpen(env, ticket) {
  env.lobby.incident = (wanted) => (wanted === ticket ? null : Object.getPrototypeOf(env.lobby).incident.call(env.lobby, wanted));
  return () => {
    delete env.lobby.incident;
  };
}

async function expectActive(page, ticket, logText) {
  assert.equal(page.get('bug-detail').hidden, false);
  assert.equal(page.get('bug-title').textContent, ticket);
  assert.ok(page.get('bug-facts').textContent.includes(`Report ID${ticket}`));
  assert.equal(page.get('bug-log').textContent, logText);
  assert.deepEqual(selectedTickets(page), [ticket]);

  page.copied.length = 0;
  page.get('bug-copy').fire('click');
  await page.settle();
  assert.deepEqual(page.copied, [logText]);

  page.get('bug-download').fire('click');
  await page.settle();
  assert.equal(page.requests.at(-1).path, `/admin/api/incidents/${ticket}/log`);
  assert.equal(page.links.at(-1).download, `${ticket}-dragons_crown_pro_042.log`);
  assert.equal(await page.blobs.at(-1).text(), logText);
}

test('a report that fails to open leaves the open one active in every action, and a retry switches them all', async () => {
  const { env, page, older, newer } = await twoReports();
  await clickTicket(page, older);
  const restore = failingToOpen(env, newer);
  await clickTicket(page, newer);
  assert.equal(page.get('bug-count').textContent, `Could not open ${newer}.`);
  await expectActive(page, older, 'Launcher: older\n');

  restore();
  ticketButtons(page).find((b) => b.textContent === newer).fire('click');
  assert.equal(page.get('bug-title').textContent, older);
  assert.deepEqual(selectedTickets(page), [older]);
  await page.settle();
  assert.equal(page.get('bug-count').textContent, '2 reports');
  await expectActive(page, newer, 'Launcher: newer\n');

  page.get('bug-close').fire('click');
  assert.equal(page.get('bug-detail').hidden, true);
  assert.deepEqual(selectedTickets(page), []);
});

test('Close report after a failed open closes the report on screen, and the failed one can still be opened', async () => {
  const { env, page, older, newer } = await twoReports();
  await clickTicket(page, older);
  const restore = failingToOpen(env, newer);
  await clickTicket(page, newer);
  page.get('bug-close').fire('click');
  await page.settle();
  assert.equal(page.get('bug-detail').hidden, true);
  assert.deepEqual(selectedTickets(page), []);

  restore();
  await clickTicket(page, newer);
  await expectActive(page, newer, 'Launcher: newer\n');
});

test('while another report loads the open one keeps every action, and clicking it closes it for good', async () => {
  const { page, older, newer } = await twoReports();
  await clickTicket(page, older);
  ticketButtons(page).find((b) => b.textContent === newer).fire('click');
  assert.deepEqual(selectedTickets(page), [older]);
  page.get('bug-download').fire('click');
  await page.settle();
  assert.equal(page.requests.at(-1).path, `/admin/api/incidents/${older}/log`);
  assert.equal(page.get('bug-title').textContent, newer);

  ticketButtons(page).find((b) => b.textContent === older).fire('click');
  await page.settle();
  assert.equal(page.get('bug-title').textContent, older);

  ticketButtons(page).find((b) => b.textContent === newer).fire('click');
  ticketButtons(page).find((b) => b.textContent === older).fire('click');
  await page.settle();
  assert.equal(page.get('bug-detail').hidden, true);
  assert.deepEqual(selectedTickets(page), []);
});

test('a failure of a request nobody is waiting for says nothing', async () => {
  const { env, page, older, newer } = await twoReports();
  await clickTicket(page, older);
  failingToOpen(env, newer);
  ticketButtons(page).find((b) => b.textContent === newer).fire('click');
  page.get('bug-close').fire('click');
  await page.settle();
  assert.equal(page.get('bug-count').textContent, '2 reports');
  assert.equal(page.get('bug-detail').hidden, true);
});

test('a long description is shown whole in the detail, wrapped; the list keeps its preview', async () => {
  const env = makeLobby();
  const description = `${'long words here '.repeat(10)}${'x'.repeat(340)}`;
  assert.equal(description.length, INCIDENT.descriptionChars);
  const { ticket } = file(greeted(env), { description });
  const page = await adminPage(env);
  await openBugs(page);
  await clickTicket(page, ticket);
  assert.equal(page.get('bug-text').textContent, description);
  const preview = page.get('bug-list').find((n) => n.className === 'bug-summary')[0].textContent;
  assert.equal(preview, description.slice(0, INCIDENT.previewChars));

  const rule = cssRule('.bug-text');
  assert.match(rule, /white-space: pre-wrap/);
  assert.match(rule, /overflow-wrap: anywhere/);
  assert.doesNotMatch(rule, /overflow: hidden|text-overflow|nowrap|max-height|line-clamp/);
});

test('the detail reads header, description, metadata, then the log with its snapshot note, with room between them', () => {
  const detail = ADMIN_HTML.slice(ADMIN_HTML.indexOf('id="bug-detail"'));
  const order = ['id="bug-title"', 'id="bug-download"', 'id="bug-copy"', 'id="bug-close"', 'id="bug-text"', 'id="bug-facts"', 'id="bug-log-name"',
    'bug-snapshot', 'id="bug-log"'].map((part) => detail.indexOf(part));
  assert.ok(order.every((at, i) => at >= 0 && (i === 0 || at > order[i - 1])), order.join());
  assert.match(detail, /<button id="bug-close" type="button" class="quiet bug-close">Close report<\/button>/);
  assert.ok(detail.includes('This is the sanitized log snapshot captured when the report was sent. '
    + 'Previous report IDs may appear if they were already present in the selected log.'));
  assert.match(cssRule('.bug-close'), /margin-left: auto/);
  assert.match(cssRule('.bug-header'), /margin-bottom: 20px/);
  assert.match(cssRule('.bug-text'), /margin: 0 0 24px/);
  assert.match(cssRule('.bug-facts'), /margin: 0 0 24px/);
  assert.match(cssRule('.bug-snapshot'), /margin-bottom: 8px/);
});

test('earlier report IDs in a stored log stay in it, and the report ID comes from the stored report alone', async () => {
  const env = makeLobby();
  const logText = 'Launcher: shown\nBug report: sent as DCP-CMT2DY\nBug report: sent as DCP-PUH82X\n';
  const { ticket } = file(greeted(env), { log_text: logText, log_file: 'dragons_crown_pro_186.log' });
  assert.ok(!logText.includes(ticket));
  const page = await adminPage(env);
  await openBugs(page);
  await clickTicket(page, ticket);

  assert.equal(page.get('bug-title').textContent, ticket);
  assert.ok(page.get('bug-facts').textContent.includes(`Report ID${ticket}`));
  assert.ok(!page.get('bug-title').textContent.includes('CMT2DY'));
  assert.ok(!page.get('bug-facts').textContent.includes('CMT2DY'));
  assert.equal(page.get('bug-log').textContent, logText);
  assert.equal(env.lobby.incident(ticket).log_text, logText);

  page.get('bug-copy').fire('click');
  page.get('bug-download').fire('click');
  await page.settle();
  assert.deepEqual(page.copied, [logText]);
  assert.equal(await page.blobs[0].text(), logText);
  assert.equal(page.links[0].download, `${ticket}-dragons_crown_pro_186.log`);
});

test('the Dashboard has none of the report detail controls and closing a report leaves it alone', async () => {
  const dashboard = ADMIN_HTML.slice(ADMIN_HTML.indexOf('id="view-dashboard"'), ADMIN_HTML.indexOf('id="view-bugs"'));
  assert.ok(!dashboard.includes('bug-'));
  const { page, older } = await twoReports();
  const shown = page.get('c-online').textContent;
  await clickTicket(page, older);
  page.get('bug-close').fire('click');
  await page.settle();
  assert.equal(page.get('view-dashboard').hidden, true);
  assert.equal(page.get('c-online').textContent, shown);
  assert.equal(page.requests.filter((r) => r.path === '/admin/api/summary').length, 1);
});

const anonymous = (env, ip = `198.18.0.${nextAddress++ % 250}`) => env.connect(ip);
const lookup = (client, reports) => client.call('incident.status', { reports });
// The launcher opens a socket for each lookup and closes it after the answer.
const askOnce = (env, ip, op, body) => {
  const client = anonymous(env, ip);
  const answer = client.call(op, body);
  client.drop();
  return answer;
};
const OTHER_RECEIPT = 'ab'.repeat(32);
const LEGACY_TICKET = 'DCP-ABCDEF';

function fileLegacyReport(env) {
  env.lobby.incidents.file(report({ description: 'From before receipts' }), LEGACY_TICKET, 'a', 'b', env.clock.now);
}

const everyRow = (env) => JSON.stringify(env.host.sql.all(`SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'`)
  .map((t) => env.host.sql.all(`SELECT * FROM ${t.name}`)));

test('a new report gets a 256-bit receipt of its own, unrelated to the ticket, the account or the address', () => {
  const env = makeLobby();
  const client = greeted(env);
  const first = file(client);
  const second = file(client);
  assert.match(first.receipt, /^[0-9a-f]{64}$/);
  assert.notEqual(first.receipt, second.receipt);
  for (const part of [first.ticket, client.account, client.ip]) assert.ok(!first.receipt.includes(part));
  assert.equal(first.submitted_at, env.clock.now);
});

test('the Worker keeps only a hash of the receipt, and the admin never sees either', async () => {
  const env = makeLobby();
  const { ticket, receipt } = file(greeted(env));
  assert.ok(!everyRow(env).includes(receipt));
  const stored = env.host.sql.get('SELECT receipt_hash FROM incident_tracking WHERE ticket = ?', ticket);
  assert.match(stored.receipt_hash, /^[0-9a-f]{64}$/);
  assert.notEqual(stored.receipt_hash, receipt);
  for (const path of ['/admin/api/incidents', `/admin/api/incidents/${ticket}`]) {
    const text = await (await adminGet(env, path)).text();
    assert.ok(!text.includes(receipt) && !text.includes(stored.receipt_hash) && !text.includes('receipt'), path);
  }
  assert.ok(!JSON.stringify(env.lobby.incident(ticket)).includes(receipt));
});

test('a ticket with its receipt reads its status, without a session and with only the allowed fields', () => {
  const env = makeLobby();
  const { ticket, receipt } = file(greeted(env));
  const answer = lookup(anonymous(env), [{ ticket, receipt }]);
  assert.equal(answer.code, 'ok');
  assert.deepEqual(answer.reports, [{ ticket, status: 'new', status_updated_at: env.clock.now }]);
  assert.deepEqual(Object.keys(answer).sort(), ['code', 're', 'reports']);
});

test('a status needs the right receipt: a wrong one, none, or another report\'s all fail', () => {
  const env = makeLobby();
  const client = greeted(env);
  const a = file(client);
  const b = file(client);
  assert.deepEqual(lookup(anonymous(env), [{ ticket: a.ticket, receipt: OTHER_RECEIPT }]).reports, []);
  assert.deepEqual(lookup(anonymous(env), [{ ticket: a.ticket, receipt: b.receipt }]).reports, []);
  assert.equal(lookup(anonymous(env), [{ ticket: a.ticket }]).code, 'bad_request');
  assert.equal(lookup(anonymous(env), [{ ticket: a.ticket, receipt: '' }]).code, 'bad_request');
  assert.equal(anonymous(env).call('incident.read', { ticket: a.ticket }).code, 'bad_request');
});

test('malformed lookups are refused and a batch has a limit', () => {
  const env = makeLobby();
  const { ticket, receipt } = file(greeted(env));
  const good = { ticket, receipt };
  for (const reports of [undefined, 'x', {}, [null], [[]], [{ ...good, ticket: 'dcp-abcdef' }], [{ ...good, receipt: 'AB'.repeat(32) }],
    [{ ...good, receipt: 'ab' }], [{ ...good, receipt: 7 }], [good, 'x']]) {
    assert.equal(anonymous(env).call('incident.status', { reports }).code, 'bad_request', JSON.stringify(reports));
    env.advance(HOUR_MS);
  }
  assert.equal(anonymous(env).call('incident.status', { reports: [good], extra: 1 }).code, 'bad_request');
  env.advance(HOUR_MS);
  assert.equal(lookup(anonymous(env), Array(INCIDENT.statusBatch + 1).fill(good)).code, 'bad_request');
  env.advance(HOUR_MS);
  assert.equal(lookup(anonymous(env), Array(INCIDENT.statusBatch).fill(good)).code, 'ok');
});

test('a batch shows only what its receipts prove: an unknown ticket and a wrong receipt look the same', () => {
  const env = makeLobby();
  const { ticket, receipt } = file(greeted(env));
  const other = file(greeted(env));
  const answer = lookup(anonymous(env), [{ ticket: other.ticket, receipt: OTHER_RECEIPT }, { ticket, receipt }, { ticket: 'DCP-ZZZZZZ', receipt: OTHER_RECEIPT }]);
  assert.deepEqual(answer.reports.map((r) => r.ticket), [ticket]);

  const known = lookup(anonymous(env), [{ ticket: other.ticket, receipt: OTHER_RECEIPT }]);
  const unknown = lookup(anonymous(env), [{ ticket: 'DCP-ZZZZZZ', receipt: OTHER_RECEIPT }]);
  assert.deepEqual({ ...known, re: 0 }, { ...unknown, re: 0 });
  const readKnown = anonymous(env).call('incident.read', { ticket: other.ticket, receipt: OTHER_RECEIPT });
  const readUnknown = anonymous(env).call('incident.read', { ticket: 'DCP-ZZZZZZ', receipt: OTHER_RECEIPT });
  assert.equal(readKnown.code, 'not_found');
  assert.deepEqual({ ...readKnown, re: 0 }, { ...readUnknown, re: 0 });
});

test('looking up costs no report quota and writes nothing, and it has a budget of its own', () => {
  const env = makeLobby();
  const client = greeted(env);
  const first = file(client);
  const before = everyRow(env);
  const codes = [];
  for (let i = 0; i < PROTECTION.incidentStatusAddress.burst + 1; i++) {
    codes.push(askOnce(env, '198.51.100.40', 'incident.status', { reports: [first] }).code);
  }
  assert.deepEqual(codes.slice(0, -1), Array(PROTECTION.incidentStatusAddress.burst).fill('ok'));
  assert.equal(codes.at(-1), 'limit');
  assert.equal(everyRow(env), before);
  assert.equal(quotaRows(env), 1);
  assert.equal(file(client).code, 'ok');

  for (let i = 0; i < PROTECTION.incidentReadAddress.burst; i++) {
    assert.equal(askOnce(env, '198.51.100.41', 'incident.read', first).code, 'ok');
  }
  assert.equal(askOnce(env, '198.51.100.41', 'incident.read', first).code, 'limit');
});

test('a lookup never meets an account: no hello is sent and report filing still needs one', () => {
  const env = makeLobby();
  const { ticket, receipt } = file(greeted(env));
  const client = anonymous(env);
  assert.equal(lookup(client, [{ ticket, receipt }]).code, 'ok');
  assert.equal(client.call('incident.read', { ticket, receipt }).code, 'ok');
  const unsigned = anonymous(env);
  assert.equal(file(unsigned).code, 'no_session');
  assert.equal(unsigned.call('worlds').code, 'no_session');
});

test('a report older than receipts cannot be claimed, whatever is tried, and the admin can still set its status', async () => {
  const env = makeLobby();
  fileLegacyReport(env);
  const page = await adminPage(env);
  await openBugs(page);
  assert.equal(page.get('bug-list').textContent.includes('New'), true);

  const client = anonymous(env);
  assert.deepEqual(lookup(client, [{ ticket: LEGACY_TICKET, receipt: OTHER_RECEIPT }]).reports, []);
  const answer = await saveStatus(env, LEGACY_TICKET, { status: 'investigating' });
  assert.equal(answer.status, 200);
  assert.equal((await answer.json()).status, 'investigating');
  assert.deepEqual(lookup(client, [{ ticket: LEGACY_TICKET, receipt: OTHER_RECEIPT }]).reports, []);
  assert.equal(client.call('incident.read', { ticket: LEGACY_TICKET, receipt: OTHER_RECEIPT }).code, 'not_found');
  assert.equal(env.host.sql.get('SELECT receipt_hash FROM incident_tracking WHERE ticket = ?', LEGACY_TICKET).receipt_hash, '');
  assert.equal(env.lobby.incidentForAdmin(LEGACY_TICKET).status, 'investigating');
});

test('the whole report comes back to its owner byte for byte, and only with the right receipt', () => {
  const env = makeLobby();
  const logText = 'Runtime: tab\there\n\nLauncher: caf\u00e9 \u{1F409}\nBug report: sent as DCP-CMT2DY\n';
  const client = greeted(env, '198.51.100.77');
  const { ticket, receipt } = file(client, { log_text: logText });
  const other = file(client);
  const reader = anonymous(env);
  const answer = reader.call('incident.read', { ticket, receipt });
  assert.equal(answer.code, 'ok');
  assert.equal(answer.report.log_text, logText);
  assert.deepEqual(Object.keys(answer.report).sort(), [...Object.keys(report()), 'submitted_at', 'ticket'].sort());
  assert.equal(answer.report.submitted_at, env.clock.now);
  assert.ok(!JSON.stringify(answer).includes(client.account) && !JSON.stringify(answer).includes('198.51.100.77'));
  assert.equal(reader.call('incident.read', { ticket, receipt: other.receipt }).code, 'not_found');
});

test('a status outlives the log it came with, and the status table has its own limit', async () => {
  const env = makeLobby();
  const { ticket, receipt } = file(greeted(env));
  await saveStatus(env, ticket, { status: 'fixed', fixed_version: '0.9.3' });
  env.host.sql.run('DELETE FROM incidents WHERE ticket = ?', ticket);
  assert.equal(lookup(anonymous(env), [{ ticket, receipt }]).reports[0].fixed_version, '0.9.3');
  assert.equal(anonymous(env).call('incident.read', { ticket, receipt }).code, 'not_found');
  assert.ok(env.lobby.incidents.taken(ticket));

  const kept = INCIDENT.statusKept;
  INCIDENT.statusKept = 2;
  try {
    env.advance(HOUR_MS);
    file(greeted(env));
    env.advance(HOUR_MS);
    const newest = file(greeted(env));
    assert.deepEqual(lookup(anonymous(env), [{ ticket, receipt }]).reports, []);
    assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM incident_tracking').n, 2);
    assert.equal(lookup(anonymous(env), [newest]).code, 'ok');
    assert.equal(env.lobby.incidentList().incidents.length, 2);
  } finally {
    INCIDENT.statusKept = kept;
  }
});

for (const status of INCIDENT_STATUSES) {
  test(`the admin can set ${status}, and only the owner reads it back`, async () => {
    const env = makeLobby();
    const { ticket, receipt } = file(greeted(env));
    env.advance(MINUTE_MS);
    const answer = await saveStatus(env, ticket, { status, player_message: 'A note' });
    assert.equal(answer.status, 200);
    const [read] = lookup(anonymous(env), [{ ticket, receipt }]).reports;
    assert.equal(read.status, status);
    assert.equal(read.player_message, 'A note');
    assert.equal(read.status_updated_at, env.clock.now);
    assert.equal(env.lobby.incidentList().incidents[0].status, status);
  });
}

test('the status list is exactly the seven statuses', () => {
  assert.deepEqual(INCIDENT_STATUSES, ['new', 'investigating', 'fixed', 'cannot_reproduce', 'duplicate', 'not_a_bug', 'closed']);
});

test('a bad status, version or message is refused and changes nothing', async () => {
  const env = makeLobby();
  const { ticket } = file(greeted(env));
  const path = `/admin/api/incidents/${ticket}/status`;
  for (const change of [{}, { status: 'Fixed' }, { status: 'done' }, { status: 7 }, { status: 'fixed', fixed_version: '<b>1</b>' },
    { status: 'fixed', fixed_version: 'v 1' }, { status: 'fixed', fixed_version: 'x'.repeat(33) }, { status: 'new', fixed_version: 3 },
    { status: 'new', player_message: 'x'.repeat(INCIDENT.messageChars + 1) }, { status: 'new', player_message: 'a\u0000b' },
    { status: 'new', player_message: 'a\r\nb' }, { status: 'new', player_message: 7 }, { status: 'new', owner: 'x' }]) {
    assert.equal((await saveStatus(env, ticket, change)).status, 400, JSON.stringify(change));
  }
  assert.equal((await adminPost(env, path, 'not json')).status, 400);
  assert.equal((await adminPost(env, path, '[]')).status, 400);
  assert.equal((await adminPost(env, path, 'x'.repeat(9000))).status, 413);
  assert.equal(env.lobby.incidentForAdmin(ticket).status, 'new');
  assert.equal((await saveStatus(env, ticket, { status: 'new', player_message: 'é'.repeat(INCIDENT.messageChars) })).status, 200);
});

test('saving a status needs the admin token, a POST and a real report', async () => {
  const env = makeLobby();
  const { ticket } = file(greeted(env));
  const path = `/admin/api/incidents/${ticket}/status`;
  const body = JSON.stringify({ status: 'fixed' });
  assert.equal((await adminPost(env, path, body, null)).status, 401);
  assert.equal((await adminPost(env, path, body, 'wrong-token-of-enough-length')).status, 401);
  const get = await adminGet(env, path);
  assert.equal(get.status, 405);
  assert.equal(get.headers.get('Allow'), 'POST');
  assert.equal((await adminPost(env, '/admin/api/incidents/DCP-ZZZZZZ/status', body)).status, 404);
  assert.equal((await adminPost(env, '/admin/api/incidents/nope/status', body)).status, 400);
  assert.equal(env.lobby.incidentForAdmin(ticket).status, 'new');
  assert.equal(internalIncidentRoute(new Request(`https://x/incidents/${ticket}/status`)), null);
  assert.deepEqual(internalIncidentRoute(new Request(`https://x/incidents/${ticket}/status`, { method: 'POST' })), { kind: 'status', ticket });
});

test('a fixed version is kept only on a Fixed report, and leaving Fixed clears it', async () => {
  const env = makeLobby();
  const { ticket, receipt } = file(greeted(env));
  const claim = [{ ticket, receipt }];
  await saveStatus(env, ticket, { status: 'investigating', fixed_version: '0.9.3' });
  assert.equal(env.lobby.incidentForAdmin(ticket).fixed_version, '');
  await saveStatus(env, ticket, { status: 'fixed', fixed_version: '0.10.0' });
  assert.equal(lookup(anonymous(env), claim).reports[0].fixed_version, '0.10.0');
  await saveStatus(env, ticket, { status: 'closed', fixed_version: '0.10.0' });
  const [read] = lookup(anonymous(env), claim).reports;
  assert.equal(read.status, 'closed');
  assert.equal('fixed_version' in read, false);
  assert.equal(env.lobby.incidentForAdmin(ticket).fixed_version, '');
});

test('a player message stays plain text', async () => {
  const env = makeLobby();
  const { ticket, receipt } = file(greeted(env));
  const message = '<script>alert(1)</script> **not bold**\nline two';
  await saveStatus(env, ticket, { status: 'fixed', player_message: message });
  assert.equal(env.lobby.incidentForAdmin(ticket).player_message, message);
  assert.equal(lookup(anonymous(env), [{ ticket, receipt }]).reports[0].player_message, message);
});

test('a change moves the status time and never the submission time; a save that changes nothing moves neither', async () => {
  const env = makeLobby();
  const { ticket, receipt } = file(greeted(env));
  const submitted = env.clock.now;
  env.advance(MINUTE_MS);
  await saveStatus(env, ticket, { status: 'investigating' });
  const first = env.clock.now;
  env.advance(MINUTE_MS);
  await saveStatus(env, ticket, { status: 'investigating' });
  assert.equal(env.lobby.incidentForAdmin(ticket).status_updated_at, first);
  await saveStatus(env, ticket, { status: 'investigating', player_message: 'Looking at it' });
  assert.equal(env.lobby.incidentForAdmin(ticket).status_updated_at, env.clock.now);
  env.advance(MINUTE_MS);
  await saveStatus(env, ticket, { status: 'fixed', player_message: 'Looking at it', fixed_version: '0.9.3' });
  assert.equal(env.lobby.incidentForAdmin(ticket).status_updated_at, env.clock.now);
  env.advance(MINUTE_MS);
  await saveStatus(env, ticket, { status: 'fixed', player_message: 'Looking at it', fixed_version: '0.9.4' });
  assert.equal(env.lobby.incidentForAdmin(ticket).status_updated_at, env.clock.now);
  assert.equal(env.lobby.incident(ticket).ms, submitted);
  assert.equal(env.lobby.incidents.report({ ticket, receipt }).submitted_at, submitted);
});

test('a filed report starts as New, and a stored report is unchanged by anything that happens to its status', async () => {
  const env = makeLobby();
  const { ticket } = file(greeted(env));
  const before = JSON.stringify(env.lobby.incident(ticket));
  assert.deepEqual(env.lobby.incidents.status(ticket), { status: 'new', status_updated_at: env.clock.now, fixed_version: '', player_message: '' });
  await saveStatus(env, ticket, { status: 'fixed', fixed_version: '0.9.3', player_message: 'Done' });
  assert.equal(JSON.stringify(env.lobby.incident(ticket)), before);
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM incidents').n, 1);
});

test('the Status column and filter list reports by status, and keep the other filters', async () => {
  const { env, page, older, newer } = await twoReports();
  assert.deepEqual(bugRows(page)[0].slice(0, 2), ['Report ID', 'Status']);
  await saveStatus(env, older, { status: 'cannot_reproduce' });
  await openBugs(page);
  const rows = bugRows(page);
  assert.equal(rows[1][1], 'New');
  assert.equal(rows[2][1], 'Cannot reproduce');

  page.get('bug-status-filter').value = 'cannot_reproduce';
  page.get('bug-status-filter').fire('change');
  assert.deepEqual(ticketButtons(page).map((b) => b.textContent), [older]);
  assert.equal(page.get('bug-count').textContent, '1 of 2 reports');
  page.get('bug-version').value = '0.9.3';
  page.get('bug-version').fire('change');
  assert.deepEqual(ticketButtons(page).map((b) => b.textContent), []);
  page.get('bug-version').value = '';
  page.get('bug-status-filter').value = '';
  page.get('bug-status-filter').fire('change');
  assert.deepEqual(ticketButtons(page).map((b) => b.textContent), [newer, older]);
  for (const name of ['All statuses', 'New', 'Investigating', 'Fixed', 'Cannot reproduce', 'Duplicate', 'Not a bug', 'Closed']) {
    assert.ok(ADMIN_HTML.includes(`>${name}</option>`), name);
  }
});

test('the status form shows the report, keeps changes until Save, and updates the list when saved', async () => {
  const { env, page, older } = await twoReports();
  await clickTicket(page, older);
  assert.equal(page.get('bug-status').value, 'new');
  assert.equal(page.get('bug-fixed-field').hidden, true);

  page.get('bug-status').value = 'fixed';
  page.get('bug-status').fire('change');
  assert.equal(page.get('bug-fixed-field').hidden, false);
  assert.equal(env.lobby.incidentForAdmin(older).status, 'new');

  page.get('bug-fixed').value = ' 0.9.3 ';
  page.get('bug-message').value = 'Fixed the crash';
  const asked = page.requests.length;
  page.get('bug-status-save').fire('click');
  await page.settle();
  assert.equal(page.requests.length, asked + 1);
  const saving = page.requests.at(-1);
  assert.equal(saving.path, `/admin/api/incidents/${older}/status`);
  assert.equal(saving.method, 'POST');
  assert.equal(saving.authorization, `Bearer ${TOKEN}`);
  assert.equal(page.get('bug-status-note').textContent, 'Saved.');
  const saved = env.lobby.incidentForAdmin(older);
  assert.deepEqual([saved.status, saved.fixed_version, saved.player_message], ['fixed', '0.9.3', 'Fixed the crash']);
  assert.equal(bugRows(page)[2][1], 'Fixed');
  assert.equal(page.get('bug-log').textContent, 'Launcher: older\n');

  page.get('bug-status').value = 'closed';
  page.get('bug-status').fire('change');
  assert.equal(page.get('bug-fixed-field').hidden, true);
  assert.ok(page.get('bug-status-meaning').textContent.includes('It does not mean Fixed.'));
  page.get('bug-status-save').fire('click');
  await page.settle();
  assert.equal(env.lobby.incidentForAdmin(older).fixed_version, '');
  assert.equal(JSON.parse(page.requests.at(-1).body).fixed_version, '');

  page.get('bug-close').fire('click');
  await clickTicket(page, older);
  assert.equal(page.get('bug-status').value, 'closed');
  assert.equal(page.get('bug-message').value, 'Fixed the crash');
  assert.equal(page.get('bug-status-note').textContent, '');
});

test('a refused save says so, keeps the form and leaves the report alone', async () => {
  const { env, page, older } = await twoReports();
  await clickTicket(page, older);
  page.get('bug-status').value = 'fixed';
  page.get('bug-status').fire('change');
  page.get('bug-fixed').value = '<b>';
  page.get('bug-status-save').fire('click');
  await page.settle();
  assert.ok(page.get('bug-status-note').textContent.startsWith('Could not save'));
  assert.equal(page.get('bug-fixed').value, '<b>');
  assert.equal(env.lobby.incidentForAdmin(older).status, 'new');
});

test('the status form sits with the metadata, has no secrets and leaves the Dashboard alone', () => {
  const detail = ADMIN_HTML.slice(ADMIN_HTML.indexOf('id="bug-detail"'));
  const order = ['id="bug-facts"', 'Report status', 'id="bug-status"', 'id="bug-fixed"', 'id="bug-message"', 'id="bug-status-save"', 'id="bug-log-name"']
    .map((part) => detail.indexOf(part));
  assert.ok(order.every((at, i) => at >= 0 && (i === 0 || at > order[i - 1])), order.join());
  assert.match(detail, /<input id="bug-fixed"[^>]*maxlength="32"/);
  assert.match(detail, /<textarea id="bug-message"[^>]*maxlength="500"/);
  assert.ok(!ADMIN_JS.includes('receipt') && !ADMIN_HTML.includes('receipt'));
  assert.ok(!/\balert\(|\bconfirm\(/.test(ADMIN_JS));
  const dashboard = ADMIN_HTML.slice(ADMIN_HTML.indexOf('id="view-dashboard"'), ADMIN_HTML.indexOf('id="view-bugs"'));
  assert.ok(!dashboard.includes('bug-') && !dashboard.includes('Report status'));
});

test('report text reaches the page as text and the displayed log is the stored value untouched', () => {
  const body = ADMIN_JS.slice(ADMIN_JS.indexOf('var bugReports'), ADMIN_JS.indexOf('function render(summary'));
  assert.ok(!/innerHTML|outerHTML|insertAdjacentHTML|document\.write/.test(ADMIN_JS));
  assert.ok(body.includes('bugLogText = r.log_text;') && body.includes("$('bug-log').textContent = bugLogText;"));
  assert.ok(body.includes('writeText(bugLogText)'));
  assert.ok(!/replace\(|slice\(|substr|scrub|mask/i.test(body.slice(body.indexOf('function bugDetail'), body.indexOf('function openBugReport'))));
});

test('the largest report crosses a real WebSocket, and a verbose server log shows neither its text nor its log', async () => {
  const lines = [];
  const server = await startLobbyServer({ port: 0, verbose: true, log: (text) => lines.push(text) });
  const socket = new WebSocket(`ws://127.0.0.1:${server.port}/`);
  const answers = [];
  const waiters = [];
  socket.addEventListener('message', (event) => {
    answers.push(JSON.parse(event.data));
    waiters.splice(0).forEach((wake) => wake());
  });
  await new Promise((resolve, reject) => {
    socket.addEventListener('open', resolve, { once: true });
    socket.addEventListener('error', reject, { once: true });
  });
  const ask = async (message) => {
    const before = answers.length;
    socket.send(JSON.stringify(message));
    while (answers.length === before) await new Promise((resolve) => waiters.push(resolve));
    return answers.at(-1);
  };
  const hello = await ask({ op: 'hello', seq: 1, proto: 1, game: 'DragonsCrownPro', account: '424242', secret: 'cd'.repeat(32),
    name: 'Tester', presence: false });
  assert.equal(hello.code, 'ok');
  const answer = await ask({ op: 'incident.file', seq: 2, ...report({ description: 'Private words here',
    log_text: `Private log line\n${'\n'.repeat(INCIDENT.logBytes - 17)}` }) });
  assert.equal(answer.code, 'ok');
  assert.ok(isTicket(answer.ticket));
  assert.equal(server.lobby.incident(answer.ticket).log_text.length, INCIDENT.logBytes);
  const claim = { ticket: answer.ticket, receipt: answer.receipt };
  assert.equal((await ask({ op: 'incident.status', seq: 3, reports: [claim] })).reports.length, 1);
  assert.equal((await ask({ op: 'incident.read', seq: 4, ...claim })).report.description, 'Private words here');
  socket.close();
  await server.close();
  assert.ok(lines.some((line) => line.includes('incident.file')));
  assert.ok(lines.some((line) => line.includes('incident.read')));
  assert.ok(lines.every((line) => !line.includes('Private') && !line.includes(answer.receipt)), lines.join('\n'));
});

test('an empty report list says so', async () => {
  const page = await adminPage(makeLobby());
  await openBugs(page);
  assert.equal(page.get('bug-count').textContent, 'No reports yet.');
  assert.equal(ticketButtons(page).length, 0);
});

// ---- Deleting a report ----

const deleteReport = (env, ticket, token) => adminDelete(env, `/admin/api/incidents/${ticket}`, token);
const claimOf = ({ ticket, receipt }) => ({ ticket, receipt });
const storedRows = (env, ticket) => ['incidents', 'incident_tracking']
  .map((table) => env.host.sql.get(`SELECT COUNT(*) AS n FROM ${table} WHERE ticket = ?`, ticket).n);

test('an authenticated delete removes the whole report: fields, log, status, message and receipt', async () => {
  const env = makeLobby();
  const gone = file(greeted(env), { description: 'A test report' });
  await saveStatus(env, gone.ticket, { status: 'fixed', fixed_version: '0.9.3', player_message: 'Thanks, fixed.' });
  const owner = anonymous(env);
  assert.equal(lookup(owner, [claimOf(gone)]).reports[0].player_message, 'Thanks, fixed.');
  assert.deepEqual(storedRows(env, gone.ticket), [1, 1]);

  const response = await deleteReport(env, gone.ticket);
  assert.equal(response.status, 200);
  assert.deepEqual(await response.json(), { deleted: gone.ticket });
  assert.deepEqual(storedRows(env, gone.ticket), [0, 0]);
  assert.equal(env.lobby.incidents.taken(gone.ticket), false);
  assert.equal(env.lobby.incidentForAdmin(gone.ticket), null);
});

test('a deleted report is gone for the admin and for its owner', async () => {
  const env = makeLobby();
  const gone = file(greeted(env), { description: 'Delete me', log_text: 'Launcher: private log\n' });
  const kept = file(greeted(env), { description: 'Keep me' });
  await deleteReport(env, gone.ticket);

  const list = await (await adminGet(env, '/admin/api/incidents')).json();
  assert.deepEqual(list.incidents.map((row) => row.ticket), [kept.ticket]);
  assert.equal((await adminGet(env, `/admin/api/incidents/${gone.ticket}`)).status, 404);
  assert.equal((await adminGet(env, `/admin/api/incidents/${gone.ticket}/log`)).status, 404);
  const owner = anonymous(env);
  assert.deepEqual(lookup(owner, [claimOf(gone)]).reports, []);
  assert.equal(owner.call('incident.read', claimOf(gone)).code, 'not_found');
  assert.deepEqual(lookup(owner, [claimOf(gone), claimOf(kept)]).reports.map((r) => r.ticket), [kept.ticket]);
});

test('deleting one report leaves the others, their statuses, their receipts and the quota alone', async () => {
  const env = makeLobby();
  const client = greeted(env);
  const gone = file(client, { description: 'Delete me' });
  env.advance(SPACED_MS);
  const kept = file(client, { description: 'Keep me', log_text: 'Launcher: kept\n' });
  await saveStatus(env, kept.ticket, { status: 'investigating', player_message: 'Looking' });
  const quota = quotaRows(env);
  await deleteReport(env, gone.ticket);

  assert.equal(quotaRows(env), quota);
  assert.deepEqual(storedRows(env, kept.ticket), [1, 1]);
  const one = await (await adminGet(env, `/admin/api/incidents/${kept.ticket}`)).json();
  assert.equal(one.description, 'Keep me');
  assert.equal(one.status, 'investigating');
  assert.equal(await (await adminGet(env, `/admin/api/incidents/${kept.ticket}/log`)).text(), 'Launcher: kept\n');
  const owner = anonymous(env);
  assert.equal(lookup(owner, [claimOf(kept)]).reports[0].player_message, 'Looking');
  assert.equal(owner.call('incident.read', claimOf(kept)).report.description, 'Keep me');
});

test('deleting needs the admin token: a missing or wrong one is 401 and the report stays', async () => {
  const env = makeLobby();
  const { ticket } = file(greeted(env));
  for (const token of [null, '', 'wrong-token-of-enough-length']) {
    const response = await deleteReport(env, ticket, token);
    assert.equal(response.status, 401, String(token));
    assert.equal(response.headers.get('WWW-Authenticate'), 'Bearer');
  }
  const unconfigured = await handleAdmin(new Request(`https://dcp-lobby.example/admin/api/incidents/${ticket}`,
    { method: 'DELETE', headers: { Authorization: `Bearer ${TOKEN}` } }), {}, adminSource(env));
  assert.equal(unconfigured.status, 503);
  assert.deepEqual(storedRows(env, ticket), [1, 1]);
  assert.equal((await deleteReport(env, ticket)).status, 200);
});

test('deleting a report that is not stored is 404, and a second delete is too', async () => {
  const env = makeLobby();
  const { ticket } = file(greeted(env));
  assert.equal((await deleteReport(env, 'DCP-ZZZZZZ')).status, 404);
  assert.deepEqual(await (await deleteReport(env, 'DCP-ZZZZZZ')).json(), { error: 'not_found' });
  assert.equal((await deleteReport(env, ticket)).status, 200);
  assert.equal((await deleteReport(env, ticket)).status, 404);
});

test('a malformed report id is refused after the token check and deletes nothing', async () => {
  const env = makeLobby();
  const { ticket } = file(greeted(env));
  for (const id of ['nope', '1%20OR%201=1', 'dcp-abc234', 'DCP-ABC23', 'DCP-ABC2345', '..%2Fx', 'DCP-ABC23%00']) {
    const response = await deleteReport(env, id);
    assert.equal(response.status, 400, id);
    assert.deepEqual(await response.json(), { error: 'bad_request', detail: 'invalid report id' });
    assert.equal((await deleteReport(env, id, null)).status, 401, id);
  }
  for (const path of ['/admin/api/incidents', `/admin/api/incidents/${ticket}/log`, `/admin/api/incidents/${ticket}/status`,
    '/admin/api/summary', '/admin/api/history', '/admin']) {
    assert.equal((await adminDelete(env, path)).status, 405, path);
  }
  assert.deepEqual(storedRows(env, ticket), [1, 1]);
});

test('only the report route takes DELETE, and the player-facing operations cannot delete', async () => {
  const env = makeLobby();
  const filed = file(greeted(env));
  const post = await handleAdmin(new Request(`https://dcp-lobby.example/admin/api/incidents/${filed.ticket}`,
    { method: 'POST', headers: { Authorization: `Bearer ${TOKEN}` } }), ENV, adminSource(env));
  assert.equal(post.status, 405);
  assert.equal(post.headers.get('Allow'), 'GET, DELETE');
  const owner = anonymous(env);
  for (const op of ['incident.delete', 'incident.remove', 'incident.forget']) {
    assert.notEqual(owner.call(op, claimOf(filed)).code, 'ok', op);
  }
  assert.deepEqual(storedRows(env, filed.ticket), [1, 1]);
  assert.deepEqual(internalIncidentRoute(new Request(`https://x/incidents/${filed.ticket}`, { method: 'DELETE' })),
    { kind: 'delete', ticket: filed.ticket });
  assert.deepEqual(internalIncidentRoute(new Request(`https://x/incidents/${filed.ticket}`)), { kind: 'one', ticket: filed.ticket });
  assert.equal(internalIncidentRoute(new Request(`https://x/incidents/${filed.ticket}/log`, { method: 'DELETE' })), null);
  const source = readFileSync(new URL('../src/lobby.js', import.meta.url), 'utf8');
  assert.ok(!/'incident\.(delete|remove|forget)'/.test(source));
});

test('deleting keeps the newest-reports limit: a delete frees a place and the limit still trims the oldest', async () => {
  const env = makeLobby();
  const kept = INCIDENT.kept;
  INCIDENT.kept = 3;
  try {
    const tickets = [];
    for (let i = 0; i < 3; i++) {
      env.advance(DAY_MS);
      tickets.push(file(greeted(env)).ticket);
    }
    await deleteReport(env, tickets[1]);
    assert.equal(env.lobby.incidentList().incidents.length, 2);
    for (let i = 0; i < 3; i++) {
      env.advance(DAY_MS);
      tickets.push(file(greeted(env)).ticket);
    }
    const left = env.lobby.incidentList().incidents.map((row) => row.ticket);
    assert.equal(left.length, 3);
    assert.deepEqual(left, tickets.slice(3).reverse());
  } finally {
    INCIDENT.kept = kept;
  }
});

// ---- The admin page: Bugs table, sign-in, menu and delete ----

const stateOf = (page, id) => page.get(id).hidden;
const dialogOpen = (page) => !page.get('delete-backdrop').hidden;

async function signedIn(env, options) {
  const page = await adminPage(env, options);
  assert.equal(stateOf(page, 'dash'), false);
  return page;
}

test('the Bugs table reads Report ID, Status, Submitted, Launcher, Game build, Operating system, Description', async () => {
  const env = makeLobby();
  const { ticket } = file(greeted(env), { game_build: 'CUSA10209 1.02 Asia/Hong Kong', cusa: 'CUSA10209' });
  const page = await adminPage(env);
  await openBugs(page);
  const [header, row] = bugRows(page);
  assert.deepEqual(header, ['Report ID', 'Status', 'Submitted', 'Launcher', 'Game build', 'Operating system', 'Description']);
  assert.ok(!header.includes('CUSA'));
  assert.equal(row[1], 'New');
  assert.equal(row[4], 'CUSA10209 1.02 Asia/Hong Kong');
  assert.ok(!row.includes('CUSA10209'));
  const detail = await (await adminGet(env, `/admin/api/incidents/${ticket}`)).json();
  assert.equal(detail.cusa, 'CUSA10209');
  await clickTicket(page, ticket);
  assert.ok(page.get('bug-facts').textContent.includes('CUSACUSA10209'));
});

test('the Bugs table keeps its important columns in view: wrapping text, columns that give way, a status near the left', () => {
  const columns = ADMIN_JS.match(/BUG_COLUMN_CLASSES = \[([^\]]+)\]/)[1];
  assert.match(columns, /^'col-id', 'col-status', 'col-submitted'/);
  assert.match(ADMIN_CSS, /#bug-list td\.bug-summary \{[^}]*white-space: normal[^}]*width: 100%/);
  assert.match(ADMIN_CSS, /#bug-list td\.col-build, #bug-list td\.col-os \{ white-space: normal/);
  assert.match(ADMIN_CSS, /@media \(max-width: 1000px\) \{\s*#bug-list \.col-os \{ display: none; \}/);
  assert.match(ADMIN_CSS, /@media \(max-width: 760px\) \{\s*#bug-list \.col-launcher \{ display: none; \}/);
  assert.ok(!/#bug-list \.col-(id|status|submitted|build)[^{]*\{[^}]*display: none/.test(ADMIN_CSS));
  assert.match(ADMIN_CSS, /\.bug-log \{[^}]*white-space: pre/);
  assert.match(cssRule('.bug-list'), /overflow-y: auto/);
});

test('hidden panels stay hidden: a display rule never overrides the hidden attribute', () => {
  assert.match(ADMIN_CSS, /\n\[hidden\] \{ display: none !important; \}/);
});

test('Delete report sits at the bottom of the open report, apart from the status form, and asks first', () => {
  const detail = ADMIN_HTML.slice(ADMIN_HTML.indexOf('id="bug-detail"'));
  assert.ok(detail.indexOf('id="bug-status-save"') < detail.indexOf('id="bug-log"'));
  assert.ok(detail.indexOf('id="bug-log"') < detail.indexOf('id="bug-delete"'));
  assert.match(ADMIN_HTML, /<div class="bug-danger">\s*<h3>Delete report<\/h3>[^]*id="bug-delete"[^>]*class="danger"/);
  assert.match(cssRule('.bug-danger'), /border-top/);
  assert.match(ADMIN_HTML, /role="alertdialog" aria-modal="true"/);
  assert.match(ADMIN_HTML, /This removes the stored report and log and cannot be undone\./);
  assert.match(ADMIN_HTML, /id="delete-cancel"[^>]*>Cancel</);
  assert.match(ADMIN_HTML, /id="delete-confirm"[^>]*>Delete report</);
});

test('the first click on Delete report asks and deletes nothing; Cancel, Escape and the backdrop all keep the report', async () => {
  const { env, page, older, newer } = await twoReports();
  await clickTicket(page, older);
  assert.equal(dialogOpen(page), false);
  page.get('bug-delete').fire('click');
  assert.equal(dialogOpen(page), true);
  assert.equal(page.get('delete-title').textContent, `Delete ${older} permanently?`);
  assert.ok(page.get('delete-cancel').focused);
  assert.ok(page.requests.every((r) => r.method !== 'DELETE'));

  page.get('delete-cancel').fire('click');
  assert.equal(dialogOpen(page), false);
  page.get('bug-delete').fire('click');
  page.fire('keydown', { key: 'Escape' });
  assert.equal(dialogOpen(page), false);
  page.get('bug-delete').fire('click');
  page.get('delete-backdrop').fire('click', { target: page.get('delete-dialog') });
  assert.equal(dialogOpen(page), true);
  page.get('delete-backdrop').fire('click', { target: page.get('delete-backdrop') });
  assert.equal(dialogOpen(page), false);

  await page.settle();
  assert.ok(page.requests.every((r) => r.method !== 'DELETE'));
  assert.equal(env.lobby.incidentForAdmin(older).ticket, older);
  assert.deepEqual(ticketButtons(page).map((b) => b.textContent), [newer, older]);
  assert.equal(page.get('bug-detail').hidden, false);
});

test('confirming deletes the report with the admin token, closes it and removes it from the list', async () => {
  const { env, page, older, newer } = await twoReports();
  await clickTicket(page, older);
  page.get('bug-delete').fire('click');
  page.get('delete-confirm').fire('click');
  assert.equal(page.get('delete-confirm').disabled, true);
  await page.settle();

  const deletes = page.requests.filter((r) => r.method === 'DELETE');
  assert.deepEqual(deletes.map((r) => r.path), [`/admin/api/incidents/${older}`]);
  assert.equal(deletes[0].authorization, `Bearer ${TOKEN}`);
  assert.equal(dialogOpen(page), false);
  assert.equal(page.get('bug-detail').hidden, true);
  assert.deepEqual(ticketButtons(page).map((b) => b.textContent), [newer]);
  assert.ok(page.get('bug-count').textContent.startsWith(`${older} deleted.`));
  assert.equal(env.lobby.incidentForAdmin(older), null);
  assert.equal(env.lobby.incidentForAdmin(newer).ticket, newer);
});

test('a report another admin already deleted leaves the list too, and a refused delete keeps the dialog with its message', async () => {
  const gone = await twoReports();
  await clickTicket(gone.page, gone.older);
  await deleteReport(gone.env, gone.older);
  gone.page.get('bug-delete').fire('click');
  gone.page.get('delete-confirm').fire('click');
  await gone.page.settle();
  assert.equal(dialogOpen(gone.page), false);
  assert.deepEqual(ticketButtons(gone.page).map((b) => b.textContent), [gone.newer]);

  const env = makeLobby();
  file(greeted(env));
  let answer = 500;
  const page = await adminPage(env, {
    route: (path, options) => {
      const sent = options.headers.Authorization.slice('Bearer '.length);
      if (options.method === 'DELETE') return new Response('{}', { status: answer });
      return options.method === 'POST' ? adminPost(env, path, options.body, sent) : adminGet(env, path, sent);
    },
  });
  await openBugs(page);
  await clickTicket(page, ticketButtons(page)[0].textContent);
  page.get('bug-delete').fire('click');
  page.get('delete-confirm').fire('click');
  await page.settle();
  assert.equal(dialogOpen(page), true);
  assert.equal(page.get('delete-error').textContent, 'Could not delete the report. Try again.');
  assert.equal(page.get('delete-confirm').disabled, false);
  assert.equal(ticketButtons(page).length, 1);

  answer = 401;
  page.get('delete-confirm').fire('click');
  await page.settle();
  assert.equal(stateOf(page, 'dash'), true);
  assert.equal(stateOf(page, 'login'), false);
  assert.equal(dialogOpen(page), false);
});

test('a delete cannot be confirmed twice', async () => {
  const { page, older, newer } = await twoReports();
  await clickTicket(page, older);
  page.get('bug-delete').fire('click');
  page.get('delete-confirm').fire('click');
  page.get('delete-confirm').fire('click');
  await page.settle();
  assert.equal(page.requests.filter((r) => r.method === 'DELETE').length, 1);
  assert.deepEqual(ticketButtons(page).map((b) => b.textContent), [newer]);
  page.get('delete-confirm').fire('click');
  await page.settle();
  assert.equal(page.requests.filter((r) => r.method === 'DELETE').length, 1);
});

test('without a stored token the login panel shows; Keep for this tab stores the token in sessionStorage only when checked', async () => {
  const env = makeLobby();
  const page = await adminPage(env, { stored: {} });
  assert.equal(stateOf(page, 'login'), false);
  assert.equal(stateOf(page, 'dash'), true);
  assert.equal(stateOf(page, 'session'), true);
  assert.equal(page.requests.length, 0);

  page.get('token').value = ` ${TOKEN} `;
  page.get('remember').checked = false;
  page.get('login').fire('submit');
  await page.settle();
  assert.equal(stateOf(page, 'login'), true);
  assert.equal(stateOf(page, 'dash'), false);
  assert.equal(page.store.size, 0);
  assert.equal(page.get('token').value, '');
  assert.ok(page.requests.every((r) => r.authorization === `Bearer ${TOKEN}`));
  assert.match(page.get('session-state').textContent, /memory only/);

  page.get('signout').fire('click');
  page.get('token').value = TOKEN;
  page.get('remember').checked = true;
  page.get('login').fire('submit');
  await page.settle();
  assert.deepEqual([...page.store], [[TOKEN_KEY, TOKEN]]);
  assert.match(page.get('session-state').textContent, /kept for this tab/);
  assert.ok(!page.get('session-state').textContent.includes(TOKEN));
});

test('a reload in the same tab signs in again from the kept token, on the same tab of the page', async () => {
  const env = makeLobby();
  file(greeted(env));
  const first = await adminPage(env, { stored: {} });
  first.get('token').value = TOKEN;
  first.get('remember').checked = true;
  first.get('login').fire('submit');
  await first.settle();
  assert.deepEqual(paths(first), ['/admin/api/history?days=30', '/admin/api/summary']);
  await openBugs(first);
  assert.equal(first.store.get(VIEW_KEY), 'bugs');

  const reloaded = await adminPage(env, { stored: Object.fromEntries(first.store) });
  assert.equal(stateOf(reloaded, 'login'), true);
  assert.equal(stateOf(reloaded, 'dash'), false);
  assert.equal(reloaded.get('remember').checked, true);
  assert.equal(stateOf(reloaded, 'view-bugs'), false);
  assert.equal(stateOf(reloaded, 'view-dashboard'), true);
  assert.equal(reloaded.get('tab-bugs').attrs['aria-selected'], 'true');
  assert.deepEqual(paths(reloaded), ['/admin/api/incidents']);
  assert.equal(ticketButtons(reloaded).length, 1);
  reloaded.get('tab-dashboard').fire('click');
  await reloaded.settle();
  assert.equal(reloaded.store.has(VIEW_KEY), false);
  assert.equal(reloaded.store.get(TOKEN_KEY), TOKEN);
});

test('a reload without Keep for this tab asks for the token again and remembers nothing, not even the tab', async () => {
  const env = makeLobby();
  const page = await adminPage(env, { stored: {} });
  page.get('token').value = TOKEN;
  page.get('login').fire('submit');
  await page.settle();
  await openBugs(page);
  assert.equal(page.store.size, 0);
  const reloaded = await adminPage(env, { stored: Object.fromEntries(page.store) });
  assert.equal(stateOf(reloaded, 'login'), false);
  assert.equal(stateOf(reloaded, 'dash'), true);
  assert.equal(reloaded.requests.length, 0);
});

test('a kept token the service refuses is removed, with the tab, and the login shows again', async () => {
  const env = makeLobby();
  const page = await adminPage(env, { stored: { [TOKEN_KEY]: 'an-old-token-that-no-longer-works', [VIEW_KEY]: 'bugs' } });
  assert.equal(stateOf(page, 'login'), false);
  assert.equal(stateOf(page, 'dash'), true);
  assert.equal(stateOf(page, 'session'), true);
  assert.equal(page.store.size, 0);
  assert.equal(page.get('login-error').textContent, 'The token was not accepted.');
});

test('Sign out clears the token, the tab and the report on screen, and returns to the login', async () => {
  const { page, older } = await twoReports();
  await clickTicket(page, older);
  page.get('menu-button').fire('click');
  page.get('signout').fire('click');
  assert.equal(page.store.size, 0);
  assert.equal(stateOf(page, 'dash'), true);
  assert.equal(stateOf(page, 'session'), true);
  assert.equal(stateOf(page, 'session-menu'), true);
  assert.equal(stateOf(page, 'login'), false);
  assert.equal(page.get('remember').checked, false);
  assert.equal(page.get('bug-detail').hidden, true);
  assert.equal(page.get('tab-dashboard').attrs['aria-selected'], 'true');
  const before = page.requests.length;
  page.get('refresh').fire('click');
  await page.settle();
  assert.ok(page.requests.slice(before).every((r) => r.authorization === 'Bearer '), 'no token is sent after signing out');
});

test('once signed in the login panel gives way to a compact menu button that opens Dashboard session and Sign out', async () => {
  const page = await signedIn(makeLobby());
  assert.equal(stateOf(page, 'login'), true);
  assert.equal(stateOf(page, 'session'), false);
  assert.equal(stateOf(page, 'session-menu'), true);
  assert.equal(page.get('menu-button').attrs['aria-expanded'], 'false');
  assert.equal(stateOf(page, 'tab-dashboard'), false);
  assert.ok(ADMIN_HTML.indexOf('id="menu-button"') < ADMIN_HTML.indexOf('id="login"'));
  assert.match(ADMIN_HTML, /id="menu-button"[^>]*aria-label="Dashboard session menu"/);
  assert.match(ADMIN_HTML, /<button id="menu-button"[^>]*><span class="bars" aria-hidden="true"><\/span><\/button>/);

  page.get('menu-button').fire('click');
  assert.equal(stateOf(page, 'session-menu'), false);
  assert.equal(page.get('menu-button').attrs['aria-expanded'], 'true');
  assert.ok(page.get('signout').focused);
  assert.ok(ADMIN_HTML.includes('<p class="menu-title">Dashboard session</p>'));
  const menu = ADMIN_HTML.slice(ADMIN_HTML.indexOf('id="session-menu"'), ADMIN_HTML.indexOf('id="login"'));
  assert.ok(menu.includes('Sign out') && !/token/i.test(menu));
  assert.ok(!page.get('session-state').textContent.includes(TOKEN));
  page.get('menu-button').fire('click');
  assert.equal(stateOf(page, 'session-menu'), true);
});

test('the menu closes on Escape and on a click outside it, and stays open for a click inside', async () => {
  const page = await signedIn(makeLobby());
  page.get('session').appendChild(page.get('menu-button'));
  page.get('session').appendChild(page.get('session-menu'));
  page.get('session-menu').appendChild(page.get('session-state'));

  page.get('menu-button').fire('click');
  page.fire('keydown', { key: 'Escape' });
  assert.equal(stateOf(page, 'session-menu'), true);
  assert.ok(page.get('menu-button').focused);

  page.get('menu-button').fire('click');
  page.fire('click', { target: page.get('session-state') });
  assert.equal(stateOf(page, 'session-menu'), false);
  page.fire('click', { target: page.get('tab-bugs') });
  assert.equal(stateOf(page, 'session-menu'), true);
});

test('the dashboard token never reaches a URL, cookie or localStorage, and only sessionStorage when asked', async () => {
  assert.ok(!/localStorage|document\.cookie|location\.(search|hash)|[?&]token=/.test(ADMIN_JS));
  assert.match(ADMIN_JS, /sessionStorage\.setItem\(key, value\)/);
  const page = await adminPage(makeLobby(), { stored: {} });
  page.get('token').value = TOKEN;
  page.get('remember').checked = true;
  page.get('login').fire('submit');
  await page.settle();
  await openBugs(page);
  assert.ok(page.requests.length > 0 && page.requests.every((r) => !r.path.includes(TOKEN) && r.authorization === `Bearer ${TOKEN}`));
  assert.deepEqual([...page.store.keys()].sort(), [TOKEN_KEY, VIEW_KEY].sort());
  assert.ok([...page.store.values()].every((value) => value === TOKEN || value === 'bugs'));
});
