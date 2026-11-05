// The private dashboard (/admin): a static shell, its stylesheet and script, served same-origin under a strict CSP.
// The script keeps the token in memory (sessionStorage only when asked) and sends it as a Bearer header.
export const ADMIN_HTML = String.raw`<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="referrer" content="no-referrer">
<meta name="robots" content="noindex, nofollow">
<title>DCP Online Admin</title>
<link rel="stylesheet" href="/admin/app.css">
</head>
<body>
<header class="top">
  <h1>Dragon's Crown Pro PC · Online Admin</h1>
  <p class="muted">Dashboard: anonymous aggregate statistics of the online service, days in America/Sao_Paulo. Bugs: reports players sent from the launcher.</p>
</header>
<main>
  <form id="login" class="panel login" autocomplete="off">
    <label for="token">Dashboard token</label>
    <input id="token" type="password" autocomplete="off" spellcheck="false" required>
    <label class="check"><input id="remember" type="checkbox"> Keep for this tab</label>
    <button type="submit">Open dashboard</button>
    <p id="login-error" class="error" role="alert"></p>
  </form>
  <section id="dash" hidden>
    <nav class="tabs" role="tablist" aria-label="Admin sections">
      <button id="tab-dashboard" type="button" role="tab" aria-selected="true" aria-controls="view-dashboard">Dashboard</button>
      <button id="tab-bugs" type="button" role="tab" aria-selected="false" aria-controls="view-bugs">Bugs</button>
      <button id="signout" type="button" class="quiet">Sign out</button>
    </nav>
    <div id="view-dashboard" role="tabpanel" aria-labelledby="tab-dashboard">
    <div class="toolbar">
      <label for="range">Range</label>
      <select id="range">
        <option value="7">7 days</option>
        <option value="30" selected>30 days</option>
        <option value="90">90 days</option>
        <option value="365">365 days</option>
      </select>
      <button id="refresh" type="button">Refresh</button>
      <span id="status" class="muted" role="status"></span>
    </div>
    <div class="cards">
      <div class="card" tabindex="0" data-help="Players connected to the online service right now, counted once per connected account"><span class="label">Online now</span><span id="c-online" class="value">–</span></div>
      <div class="card" tabindex="0" data-help="Players currently occupying a seat in an online room"><span class="label">In rooms</span><span id="c-in-rooms" class="value">–</span></div>
      <div class="card" tabindex="0" data-help="Multiplayer rooms currently open"><span class="label">Open rooms</span><span id="c-rooms" class="value">–</span></div>
      <div class="card" tabindex="0" data-help="Highest number of players online at the same time today"><span class="label">Peak today</span><span id="c-peak" class="value">–</span></div>
      <div class="card" tabindex="0" data-help="Online sessions started today, with reconnects inside the existing 10-minute deduplication window not counted again"><span class="label">Sessions today</span><span id="c-sessions" class="value">–</span></div>
      <div class="card" tabindex="0" data-help="Multiplayer rooms created today"><span class="label">Rooms today</span><span id="c-rooms-today" class="value">–</span></div>
      <div class="card" tabindex="0" data-help="Successful room joins today, not unique players, and the same player can contribute more than one join"><span class="label">Joins today</span><span id="c-joins-today" class="value">–</span></div>
    </div>
    <h2 class="section">Period comparison</h2>
    <div class="panel">
      <p id="compare-note" class="muted"></p>
      <div class="scroll"><table id="compare"></table></div>
    </div>
    <h2 class="section">Multiplayer activity</h2>
    <div class="panel">
      <p id="activity-note" class="muted"></p>
      <div class="cards" id="activity"></div>
      <div class="grid">
        <div id="d-occupancy"></div>
        <div id="d-lifetime"></div>
      </div>
    </div>
    <h2 class="section">Characters &amp; parties</h2>
    <div class="panel">
      <p class="muted">Anonymous aggregate party snapshots from compatible clients: the classes in an online party, never who played them. Game versions that do not report parties are not counted here, so a range without data means nothing was reported, not that nobody played.</p>
      <p id="party-note" class="muted"></p>
      <div class="cards" id="party-cards"></div>
      <div class="grid">
        <div id="d-character"></div>
        <div id="d-host"></div>
        <div id="d-party-size"></div>
      </div>
      <div id="d-composition"></div>
    </div>
    <h2 class="section">Historical activity</h2>
    <div class="panel">
      <h2>Usage over time</h2>
      <p class="muted">Highest value in each 15-minute interval; kept for 90 days.</p>
      <div id="usage" class="chart"></div>
    </div>
    <div>
      <div class="panel"><h2>Sessions by day</h2><div id="h-sessions" class="chart"></div></div>
      <div class="panel"><h2>Rooms created by day</h2><div id="h-rooms" class="chart"></div></div>
      <div class="panel"><h2>Successful joins by day</h2><div id="h-joins" class="chart"></div></div>
      <div class="panel"><h2>Peak online by day</h2><div id="h-peak-online" class="chart"></div></div>
      <div class="panel"><h2>Peak in rooms by day</h2><div id="h-peak-rooms" class="chart"></div></div>
    </div>
    <div class="panel">
      <h2>Daily totals</h2>
      <div class="scroll"><table id="daily"></table></div>
    </div>
    <h2 class="section">Connection health</h2>
    <div class="panel">
      <p class="muted">Operational counts for the selected range. They describe the service, not players.</p>
      <div class="cards" id="health"></div>
    </div>
    <details class="panel">
      <summary>Incompatibility rejects by reason</summary>
      <div id="rejects"></div>
    </details>
    <h2 class="section">Platforms and countries</h2>
    <div class="grid">
      <div class="panel"><h2>Platform</h2><div id="d-platform"></div></div>
      <div class="panel"><h2>Port version</h2><div id="d-version"></div></div>
      <div class="panel"><h2>Country</h2><div id="d-country"></div></div>
    </div>
    <h2 class="section">Abuse protection</h2>
    <details class="panel">
      <summary>Protection counters</summary>
      <div id="abuse"></div>
    </details>
    <div class="panel notes">
      <h2>How to read this</h2>
      <ul>
        <li>A session is one connection of a launcher or game that went online. The launcher and the game each count,
          and a reconnect within ten minutes of a counted session does not count again. Sessions are not players: there is no unique-player count.</li>
        <li>Platform, version and country by day count sessions started. "Now" counts players online, one per account,
          from the connections open at this moment.</li>
        <li>Country comes from Cloudflare and is stored only as a daily count. Unknown: Cloudflare did not say.</li>
        <li>A room is counted when its last seat is gone. Its party size is the most seats it held at once; a room with a
          party of 1 never had a guest. Lifetime runs from creation to that moment and includes the 30-second seat grace.
          Rooms open when the Worker was updated are not counted.</li>
        <li>Joins are successful room joins. They are not unique players: a player entering several rooms counts once per room.</li>
        <li>Relay rooms are rooms in which the relay forwarded at least one frame. The lobby cannot see peer-to-peer
          traffic, so the share of rooms that connected directly is not measured.</li>
        <li>Period comparison sets the selected range (including today so far) against the same number of days before it.
          No percentage is shown when the earlier period has nothing to compare with.</li>
        <li>The statistics store no names, Friend Codes, accounts, addresses, rooms or gameplay. Bug Reports hold what a
          player chose to send, and a report is removed once the newest 500 no longer include it.</li>
      </ul>
    </div>
    </div>
    <div id="view-bugs" role="tabpanel" aria-labelledby="tab-bugs" hidden>
      <div class="panel">
        <p class="muted">Reports players sent from the launcher, newest first. The log of a report is stored as the player previewed it and is shown and downloaded exactly as stored. No account or address is kept with a report, and the newest 500 are kept.</p>
        <div class="toolbar">
          <label for="bug-version">Launcher version</label>
          <select id="bug-version"></select>
          <label for="bug-build">Game build</label>
          <select id="bug-build"></select>
          <label for="bug-day">Date</label>
          <input id="bug-day" type="date">
          <label for="bug-status-filter">Status</label>
          <select id="bug-status-filter">
            <option value="">All statuses</option>
            <option value="new">New</option>
            <option value="investigating">Investigating</option>
            <option value="fixed">Fixed</option>
            <option value="cannot_reproduce">Cannot reproduce</option>
            <option value="duplicate">Duplicate</option>
            <option value="not_a_bug">Not a bug</option>
            <option value="closed">Closed</option>
          </select>
          <button id="bug-refresh" type="button">Refresh</button>
          <span id="bug-count" class="muted" role="status"></span>
        </div>
        <div class="scroll bug-list"><table id="bug-list"></table></div>
      </div>
      <div id="bug-detail" class="panel bug-detail" hidden>
        <div class="toolbar bug-header">
          <h2 id="bug-title"></h2>
          <button id="bug-download" type="button">Download log</button>
          <button id="bug-copy" type="button" class="quiet">Copy log</button>
          <span id="bug-note" class="muted" role="status"></span>
          <button id="bug-close" type="button" class="quiet bug-close">Close report</button>
        </div>
        <p id="bug-text" class="bug-text"></p>
        <dl id="bug-facts" class="bug-facts"></dl>
        <h3>Report status</h3>
        <div class="bug-status-form">
          <label for="bug-status">Status</label>
          <select id="bug-status">
            <option value="new">New</option>
            <option value="investigating">Investigating</option>
            <option value="fixed">Fixed</option>
            <option value="cannot_reproduce">Cannot reproduce</option>
            <option value="duplicate">Duplicate</option>
            <option value="not_a_bug">Not a bug</option>
            <option value="closed">Closed</option>
          </select>
          <p id="bug-status-meaning" class="muted"></p>
          <div id="bug-fixed-field" class="bug-status-field" hidden>
            <label for="bug-fixed">Fixed in version</label>
            <input id="bug-fixed" type="text" maxlength="32" autocomplete="off" placeholder="0.9.3">
          </div>
          <div class="bug-status-field">
            <label for="bug-message">Message to player</label>
            <textarea id="bug-message" rows="3" maxlength="500"></textarea>
          </div>
          <div class="toolbar">
            <button id="bug-status-save" type="button">Save status</button>
            <span id="bug-status-note" class="muted" role="status"></span>
          </div>
        </div>
        <h3 id="bug-log-name"></h3>
        <p class="muted bug-snapshot">This is the sanitized log snapshot captured when the report was sent. Previous report IDs may appear if they were already present in the selected log.</p>
        <pre id="bug-log" class="bug-log" tabindex="0"></pre>
      </div>
    </div>
  </section>
</main>
<div id="tip" class="tip" hidden></div>
<script src="/admin/app.js"></script>
</body>
</html>
`;

export const ADMIN_CSS = String.raw`:root {
  color-scheme: light;
  --surface-0: #f3f3f1;
  --surface-1: #fcfcfb;
  --border: #dcdbd6;
  --grid: #e7e6e2;
  --text-primary: #0b0b0b;
  --text-secondary: #52514e;
  --text-muted: #6d6c67;
  --accent: #2a78d6;
  --series-1: #2a78d6;
  --series-2: #eb6834;
  --series-3: #1baf7a;
  --error: #b42318;
}
@media (prefers-color-scheme: dark) {
  :root:where(:not([data-theme="light"])) {
    color-scheme: dark;
    --surface-0: #111110;
    --surface-1: #1a1a19;
    --border: #34332f;
    --grid: #2a2a27;
    --text-primary: #f0efec;
    --text-secondary: #c3c2b7;
    --text-muted: #9a998f;
    --accent: #3987e5;
    --series-1: #3987e5;
    --series-2: #d95926;
    --series-3: #199e70;
    --error: #f97066;
  }
}
:root[data-theme="dark"] {
  color-scheme: dark;
  --surface-0: #111110;
  --surface-1: #1a1a19;
  --border: #34332f;
  --grid: #2a2a27;
  --text-primary: #f0efec;
  --text-secondary: #c3c2b7;
  --text-muted: #9a998f;
  --accent: #3987e5;
  --series-1: #3987e5;
  --series-2: #d95926;
  --series-3: #199e70;
  --error: #f97066;
}
* { box-sizing: border-box; }
html { -webkit-text-size-adjust: 100%; }
body {
  margin: 0;
  background: var(--surface-0);
  color: var(--text-primary);
  font: 15px/1.45 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
}
.top, main { max-width: 1120px; margin: 0 auto; padding: 0 16px; }
.top { padding-top: 24px; }
h1 { font-size: 22px; margin: 0 0 4px; }
h2 { font-size: 15px; margin: 0 0 8px; }
h2.section { font-size: 17px; margin: 24px 0 10px; }
.up { color: var(--series-3); }
.down { color: var(--error); }
.muted { color: var(--text-muted); margin: 0 0 12px; font-size: 13px; }
.panel {
  background: var(--surface-1);
  border: 1px solid var(--border);
  border-radius: 10px;
  padding: 16px;
  margin: 0 0 16px;
  min-width: 0;
}
.login { max-width: 420px; display: grid; gap: 10px; }
.check { display: flex; gap: 8px; align-items: center; color: var(--text-secondary); }
input[type="password"], input[type="date"], select {
  font: inherit;
  padding: 8px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--surface-0);
  color: var(--text-primary);
  width: 100%;
}
select, input[type="date"] { width: auto; }
button {
  font: inherit;
  padding: 8px 14px;
  border-radius: 6px;
  border: 1px solid var(--accent);
  background: var(--accent);
  color: #fff;
  cursor: pointer;
}
button.quiet { background: transparent; color: var(--text-secondary); border-color: var(--border); }
.error { color: var(--error); margin: 0; min-height: 1.2em; }
.toolbar { display: flex; flex-wrap: wrap; gap: 10px; align-items: center; margin: 0 0 16px; }
.toolbar #status { flex: 1; margin: 0; }
.cards { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 12px; margin: 0 0 8px; }
.card {
  background: var(--surface-1);
  border: 1px solid var(--border);
  border-radius: 10px;
  padding: 14px 16px;
  display: flex;
  flex-direction: column;
}
.card[data-help] { cursor: help; }
.card[data-help]:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
.card .label { color: var(--text-secondary); font-size: 13px; }
.card .value { font-size: 30px; font-weight: 600; font-variant-numeric: tabular-nums; }
.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 16px; }
.grid .panel { margin: 0 0 16px; }
.chart svg { display: block; width: 100%; height: auto; }
.chart .axis { fill: var(--text-muted); font-size: 11px; }
.chart .gridline { stroke: var(--grid); stroke-width: 1; }
.chart .baseline { stroke: var(--border); stroke-width: 1; }
.chart .line { fill: none; stroke-width: 2; stroke-linejoin: round; }
.chart .s1 { stroke: var(--series-1); }
.chart .s2 { stroke: var(--series-2); }
.chart .s3 { stroke: var(--series-3); }
.chart .bar { fill: var(--series-1); }
.chart .bar:hover { opacity: 0.8; }
.chart .value-label { fill: var(--text-secondary); font-size: 10px; font-variant-numeric: tabular-nums; }
.chart .cursor { stroke: var(--text-muted); stroke-width: 1; }
.chart .hit { fill: transparent; }
.legend { display: flex; flex-wrap: wrap; gap: 14px; margin: 0 0 8px; color: var(--text-secondary); font-size: 13px; }
.legend i { display: inline-block; width: 14px; height: 3px; border-radius: 2px; margin-right: 6px; vertical-align: middle; }
.legend .k1 { background: var(--series-1); }
.legend .k2 { background: var(--series-2); }
.legend .k3 { background: var(--series-3); }
.dist h3 { font-size: 12px; font-weight: 600; color: var(--text-secondary); margin: 10px 0 6px; text-transform: uppercase; letter-spacing: .04em; }
.row { display: grid; grid-template-columns: minmax(0, 9em) 1fr auto; gap: 8px; align-items: center; margin: 4px 0; font-size: 13px; }
.row .name { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; color: var(--text-primary); }
.row .track { height: 8px; border-radius: 4px; background: var(--grid); overflow: hidden; }
.row .fill { display: block; height: 100%; border-radius: 4px; background: var(--series-1); }
.row .count { font-variant-numeric: tabular-nums; color: var(--text-secondary); }
.empty { color: var(--text-muted); font-size: 13px; margin: 4px 0; }
.scroll { overflow-x: auto; }
table { border-collapse: collapse; width: 100%; font-size: 13px; font-variant-numeric: tabular-nums; }
th, td { padding: 6px 8px; text-align: right; border-bottom: 1px solid var(--grid); white-space: nowrap; }
th:first-child, td:first-child { text-align: left; }
th { color: var(--text-secondary); font-weight: 600; }
details summary { cursor: pointer; font-weight: 600; }
.tabs { display: flex; flex-wrap: wrap; gap: 6px; align-items: center; margin: 0 0 16px; border-bottom: 1px solid var(--border); }
.tabs button[role="tab"] { background: transparent; color: var(--text-secondary); border: 0; border-bottom: 3px solid transparent; border-radius: 0; padding: 8px 14px; }
.tabs button[role="tab"][aria-selected="true"] { color: var(--text-primary); border-bottom-color: var(--accent); font-weight: 600; }
.tabs .quiet { margin-left: auto; }
.bug-detail h2 { margin: 0; }
.bug-list { max-height: 340px; overflow-y: auto; }
#bug-list th, #bug-list td { text-align: left; }
#bug-list td.bug-summary { max-width: 340px; overflow: hidden; text-overflow: ellipsis; }
#bug-list tr.selected td { background: var(--grid); }
button.ticket { padding: 0; background: transparent; border: 0; color: var(--accent); text-decoration: underline; font-variant-numeric: tabular-nums; }
.bug-detail { margin-top: 16px; }
.bug-header { margin-bottom: 20px; }
.bug-close { margin-left: auto; }
.bug-detail h3 { font-size: 14px; margin: 0 0 4px; }
.bug-text { white-space: pre-wrap; overflow-wrap: anywhere; margin: 0 0 24px; }
.bug-facts { display: grid; grid-template-columns: max-content 1fr; gap: 4px 12px; margin: 0 0 24px; font-size: 13px; }
.bug-snapshot { margin-bottom: 8px; }
.bug-status-form { margin: 0 0 24px; }
.bug-status-form > .muted { margin: 4px 0 12px; }
.bug-status-form .toolbar { margin: 12px 0 0; }
.bug-status-field { display: flex; flex-direction: column; gap: 4px; margin: 0 0 12px; }
.bug-status-field[hidden] { display: none; }
.bug-status-field input { max-width: 200px; }
.bug-status-field textarea { max-width: 640px; font: inherit; }
.bug-facts dt { color: var(--text-secondary); }
.bug-facts dd { margin: 0; overflow-wrap: anywhere; }
.bug-log { max-height: 480px; overflow: auto; margin: 0; padding: 10px; background: var(--surface-0); border: 1px solid var(--border); border-radius: 6px; font: 12px/1.45 ui-monospace, Consolas, monospace; white-space: pre; }
.notes ul { margin: 0; padding-left: 18px; color: var(--text-secondary); font-size: 13px; }
.notes li { margin: 4px 0; }
.tip {
  position: fixed;
  pointer-events: none;
  background: var(--surface-1);
  color: var(--text-primary);
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 6px 8px;
  font-size: 12px;
  box-shadow: 0 2px 8px rgba(0, 0, 0, .15);
  z-index: 10;
  white-space: nowrap;
}
`;

export const ADMIN_JS = String.raw`(function () {
  'use strict';
  var KEY = 'dcp-dashboard-token';
  var ZONE = 'America/Sao_Paulo';
  var SVG = 'http://www.w3.org/2000/svg';
  var token = '';
  var timer = null;
  var $ = function (id) { return document.getElementById(id); };
  var regionNames = null;
  try { regionNames = new Intl.DisplayNames(['en'], { type: 'region' }); } catch (e) { regionNames = null; }
  var timeLabel = new Intl.DateTimeFormat('en-GB', { timeZone: ZONE, day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit' });
  var dayLabel = new Intl.DateTimeFormat('en-GB', { timeZone: 'UTC', day: 'numeric', month: 'short' });
  var ABUSE = {
    ws_upgrade_accepted: 'WebSocket upgrades accepted', ws_upgrade_limited: 'WebSocket upgrades refused at the edge',
    ws_upgrade_ip_cap: 'WebSocket upgrades refused: too many sockets from one address', hello_accepted: 'Sign-ins accepted',
    hello_reconnect_deduped: 'Reconnects not counted as new sessions', hello_limited: 'Sign-ins refused: too many',
    room_create_accepted: 'Rooms created', room_create_limited: 'Room creations refused', room_join_limited: 'Room joins refused',
    relay_abuse_closed: 'Relay connections closed for abuse', party_report_limited: 'Party snapshots refused: too many',
    public_endpoint_limited: 'Public IPv4 claims ignored: too many', incident_filed: 'Bug reports accepted',
    incident_limited: 'Bug reports refused: too many', incident_hourly_limited: 'Bug reports refused: hourly limit',
    incident_daily_limited: 'Bug reports refused: daily limit'
  };
  var REJECTS = { lobby_protocol: 'Sign-in refused: different lobby protocol', online_protocol: 'Room join refused: different online protocol' };

  function storedToken() {
    try { return sessionStorage.getItem(KEY) || ''; } catch (e) { return ''; }
  }
  function storeToken(value) {
    try { if (value) sessionStorage.setItem(KEY, value); else sessionStorage.removeItem(KEY); } catch (e) { /* storage blocked */ }
  }

  function el(tag, cls, text) {
    var node = document.createElement(tag);
    if (cls) node.className = cls;
    if (text !== undefined) node.textContent = text;
    return node;
  }
  function svg(tag, attrs) {
    var node = document.createElementNS(SVG, tag);
    for (var k in attrs) node.setAttribute(k, attrs[k]);
    return node;
  }
  function clear(node) { while (node.firstChild) node.removeChild(node.firstChild); }
  function fmt(n) { return Number(n || 0).toLocaleString('en-US'); }
  function dayText(day) { return dayLabel.format(new Date(day + 'T12:00:00Z')); }
  function countryText(code) {
    if (code === 'Unknown' || code === 'Other' || !regionNames) return code;
    try { return code + ' · ' + regionNames.of(code); } catch (e) { return code; }
  }

  function AuthError() {}

  function request(path, body) {
    return fetch(path, { method: body ? 'POST' : 'GET', body: body, headers: { Authorization: 'Bearer ' + token }, cache: 'no-store', credentials: 'omit' })
      .then(function (r) {
        if (r.status === 401) throw new AuthError();
        if (!r.ok) throw new Error('The service answered ' + r.status + '.');
        return r;
      });
  }
  function api(path) {
    return request(path).then(function (r) { return r.json(); });
  }

  var tip = $('tip');
  function showTip(event, lines) {
    clear(tip);
    lines.forEach(function (line, i) { tip.appendChild(el('div', i ? '' : 'tip-head', line)); });
    tip.hidden = false;
    var x = event.clientX + 14;
    var y = event.clientY + 14;
    var w = tip.offsetWidth;
    var h = tip.offsetHeight;
    if (x + w > window.innerWidth - 8) x = event.clientX - w - 14;
    if (y + h > window.innerHeight - 8) y = event.clientY - h - 14;
    tip.style.left = Math.max(8, x) + 'px';
    tip.style.top = Math.max(8, y) + 'px';
  }
  function hideTip() { tip.hidden = true; }

  Array.prototype.forEach.call(document.querySelectorAll('.card[data-help]'), function (card) {
    var help = card.getAttribute('data-help');
    card.addEventListener('mousemove', function (event) { showTip(event, [help]); });
    card.addEventListener('mouseleave', hideTip);
    card.addEventListener('focus', function () {
      var box = card.getBoundingClientRect();
      showTip({ clientX: box.left, clientY: box.bottom - 14 }, [help]);
    });
    card.addEventListener('blur', hideTip);
  });

  function niceMax(v) {
    if (v <= 4) return 4;
    var p = Math.pow(10, Math.floor(Math.log10(v)));
    var steps = [1, 2, 2.5, 5, 10];
    for (var i = 0; i < steps.length; i++) if (steps[i] * p >= v) return steps[i] * p;
    return 10 * p;
  }

  function frame(width, height, max, pad) {
    var root = svg('svg', { viewBox: '0 0 ' + width + ' ' + height, role: 'img' });
    for (var i = 0; i <= 4; i++) {
      var y = pad.top + (height - pad.top - pad.bottom) * (1 - i / 4);
      root.appendChild(svg('line', { x1: pad.left, x2: width - pad.right, y1: y, y2: y, 'class': i ? 'gridline' : 'baseline' }));
      var label = svg('text', { x: pad.left - 6, y: y + 4, 'text-anchor': 'end', 'class': 'axis' });
      label.textContent = fmt(Math.round(max * i / 4));
      root.appendChild(label);
    }
    return root;
  }

  var SERIES = [
    { key: 'online', label: 'Online', cls: 's1', key_cls: 'k1' },
    { key: 'in_rooms', label: 'In rooms', cls: 's2', key_cls: 'k2' },
    { key: 'rooms', label: 'Open rooms', cls: 's3', key_cls: 'k3' }
  ];

  function usageChart(node, history) {
    clear(node);
    var samples = history.samples;
    if (!samples.length) { node.appendChild(el('p', 'empty', 'No samples in this range yet.')); return; }
    var step = history.sample_ms;
    var byBucket = {};
    samples.forEach(function (s) { byBucket[s.bucket] = s; });
    var start = samples[0].bucket;
    var end = Math.max(samples[samples.length - 1].bucket, history.generated - (history.generated % step));
    var points = [];
    for (var b = start; b <= end; b += step) points.push(byBucket[b] || { bucket: b, online: 0, in_rooms: 0, rooms: 0 });
    var legend = el('div', 'legend');
    SERIES.forEach(function (s) {
      var item = el('span');
      item.appendChild(el('i', s.key_cls));
      item.appendChild(document.createTextNode(s.label));
      legend.appendChild(item);
    });
    node.appendChild(legend);
    var W = 900, H = 260, pad = { top: 10, right: 12, bottom: 26, left: 44 };
    var max = niceMax(Math.max.apply(null, points.map(function (p) { return Math.max(p.online, p.in_rooms, p.rooms); })));
    var root = frame(W, H, max, pad);
    var plotW = W - pad.left - pad.right;
    var plotH = H - pad.top - pad.bottom;
    var n = points.length;
    var xAt = function (i) { return pad.left + (n === 1 ? plotW / 2 : plotW * i / (n - 1)); };
    var yAt = function (v) { return pad.top + plotH * (1 - v / max); };
    var ticks = Math.min(6, n);
    for (var t = 0; t < ticks; t++) {
      var i = ticks === 1 ? 0 : Math.round((n - 1) * t / (ticks - 1));
      var label = svg('text', { x: xAt(i), y: H - 8, 'text-anchor': t === 0 ? 'start' : t === ticks - 1 ? 'end' : 'middle', 'class': 'axis' });
      label.textContent = timeLabel.format(points[i].bucket);
      root.appendChild(label);
    }
    SERIES.slice().reverse().forEach(function (s) {
      var d = points.map(function (p, i) { return (i ? 'L' : 'M') + xAt(i).toFixed(1) + ' ' + yAt(p[s.key]).toFixed(1); }).join('');
      root.appendChild(svg('path', { d: d, 'class': 'line ' + s.cls }));
    });
    var cursor = svg('line', { y1: pad.top, y2: pad.top + plotH, 'class': 'cursor', visibility: 'hidden' });
    root.appendChild(cursor);
    var hit = svg('rect', { x: pad.left, y: pad.top, width: plotW, height: plotH, 'class': 'hit' });
    root.appendChild(hit);
    hit.addEventListener('mousemove', function (event) {
      var box = root.getBoundingClientRect();
      var x = (event.clientX - box.left) * W / box.width;
      var i = Math.max(0, Math.min(n - 1, Math.round((x - pad.left) / plotW * (n - 1))));
      cursor.setAttribute('x1', xAt(i));
      cursor.setAttribute('x2', xAt(i));
      cursor.setAttribute('visibility', 'visible');
      var p = points[i];
      showTip(event, [timeLabel.format(p.bucket)].concat(SERIES.map(function (s) { return s.label + ': ' + fmt(p[s.key]); })));
    });
    hit.addEventListener('mouseleave', function () { cursor.setAttribute('visibility', 'hidden'); hideTip(); });
    node.appendChild(root);
  }

  function barChart(node, daily, key, metric) {
    clear(node);
    var W = 900, H = 200, pad = { top: 22, right: 12, bottom: 26, left: 44 };
    var labelled = daily.length <= 30;
    var max = niceMax(Math.max.apply(null, daily.map(function (d) { return d[key]; })));
    var root = frame(W, H, max, pad);
    var plotW = W - pad.left - pad.right;
    var plotH = H - pad.top - pad.bottom;
    var slot = plotW / daily.length;
    var gap = Math.min(2, slot * 0.2);
    daily.forEach(function (d, i) {
      var h = plotH * d[key] / max;
      var x = pad.left + slot * i + gap / 2;
      var w = Math.max(1, slot - gap);
      if (h > 0) {
        var r = Math.min(4, w / 2, h);
        var y = pad.top + plotH - h;
        var path = 'M' + x + ' ' + (pad.top + plotH) + 'V' + (y + r) + 'Q' + x + ' ' + y + ' ' + (x + r) + ' ' + y +
          'H' + (x + w - r) + 'Q' + (x + w) + ' ' + y + ' ' + (x + w) + ' ' + (y + r) + 'V' + (pad.top + plotH) + 'Z';
        root.appendChild(svg('path', { d: path, 'class': 'bar' }));
        if (labelled) {
          var value = svg('text', { x: x + w / 2, y: y - 5, 'text-anchor': 'middle', 'class': 'value-label' });
          value.textContent = fmt(d[key]);
          root.appendChild(value);
        }
      }
      var hit = svg('rect', { x: pad.left + slot * i, y: pad.top, width: slot, height: plotH, 'class': 'hit' });
      hit.addEventListener('mousemove', function (event) { showTip(event, [dayText(d.day), metric + ': ' + fmt(d[key])]); });
      hit.addEventListener('mouseleave', hideTip);
      root.appendChild(hit);
    });
    var ticks = Math.min(6, daily.length);
    for (var t = 0; t < ticks; t++) {
      var i = ticks === 1 ? 0 : Math.round((daily.length - 1) * t / (ticks - 1));
      var label = svg('text', { x: pad.left + slot * (i + 0.5), y: H - 8, 'text-anchor': 'middle', 'class': 'axis' });
      label.textContent = dayText(daily[i].day);
      root.appendChild(label);
    }
    node.appendChild(root);
  }

  function rows(node, title, items, name) {
    var box = el('div', 'dist');
    box.appendChild(el('h3', '', title));
    var total = items.reduce(function (a, b) { return a + b.count; }, 0);
    if (!(total > 0)) {
      box.appendChild(el('p', 'empty', 'No data yet'));
      node.appendChild(box);
      return;
    }
    var top = Math.max.apply(null, items.map(function (i) { return i.count; }));
    items.slice(0, 12).forEach(function (item) {
      var row = el('div', 'row');
      var label = name ? name(item.value) : item.value;
      row.appendChild(el('span', 'name', label)).title = label;
      var track = el('span', 'track');
      var fill = el('span', 'fill');
      fill.style.width = (item.count > 0 ? Math.max(2, 100 * item.count / top) : 0) + '%';
      track.appendChild(fill);
      row.appendChild(track);
      row.appendChild(el('span', 'count', fmt(item.count) + ' · ' + Math.round(100 * item.count / total) + '%'));
      box.appendChild(row);
    });
    if (items.length > 12) box.appendChild(el('p', 'empty', (items.length - 12) + ' more not shown'));
    node.appendChild(box);
  }

  function distribution(id, live, range, days, name) {
    var node = $(id);
    clear(node);
    rows(node, 'Now · players online', live, name);
    rows(node, 'Sessions · last ' + days + ' days', range, name);
  }

  function table(node, head, body, rowClass) {
    clear(node);
    var thead = el('thead');
    var tr = el('tr');
    head.forEach(function (h) { tr.appendChild(el('th', '', h)); });
    thead.appendChild(tr);
    node.appendChild(thead);
    var tbody = el('tbody');
    body.forEach(function (cells, i) {
      var row = el('tr', rowClass ? rowClass(i) : '');
      cells.forEach(function (c) { row.appendChild(typeof c === 'string' ? el('td', '', c) : c); });
      tbody.appendChild(row);
    });
    node.appendChild(tbody);
  }

  function changeCell(c) {
    var td = el('td');
    if (c.change === null) { td.textContent = c.current > 0 ? 'new' : '-'; return td; }
    var diff = c.current - c.previous;
    td.className = diff > 0 ? 'up' : diff < 0 ? 'down' : '';
    td.textContent = (diff > 0 ? '+' : '') + fmt(diff) + ' (' + (c.change > 0 ? '+' : '') + c.change + '%)';
    return td;
  }

  var COMPARE = [['sessions', 'Sessions'], ['rooms_created', 'Rooms created'], ['room_joins', 'Successful joins'],
    ['rooms_with_guest', 'Rooms with a guest'], ['peak_online', 'Peak online'], ['peak_in_rooms', 'Peak in rooms']];

  function shiftDay(day, offset) {
    var date = new Date(day + 'T12:00:00Z');
    date.setUTCDate(date.getUTCDate() + offset);
    return date.toISOString().slice(0, 10);
  }

  function comparison(history) {
    $('compare-note').textContent = history.first + ' to ' + history.last + ' compared with ' + shiftDay(history.first, -history.days) +
      ' to ' + shiftDay(history.first, -1) + '.';
    table($('compare'), ['Metric', 'This period', 'Previous period', 'Change'], COMPARE.map(function (m) {
      var c = history.comparison[m[0]];
      return [m[1], fmt(c.current), fmt(c.previous), changeCell(c)];
    }));
  }

  function card(label, value) {
    var node = el('div', 'card');
    node.appendChild(el('span', 'label', label));
    node.appendChild(el('span', 'value', value));
    return node;
  }

  function share(part, whole) { return whole > 0 ? Math.round(100 * part / whole) + '%' : '–'; }
  function sum(items, names) {
    return items.reduce(function (a, i) { return names.indexOf(i.value) >= 0 ? a + i.count : a; }, 0);
  }

  var OCCUPANCY = { '1': '1 player', '2': '2 players', '3': '3 players', '4': '4 players' };
  var LIFETIME = { under_1m: 'Under 1 minute', '1_5m': '1 to 5 minutes', '5_15m': '5 to 15 minutes', '15_60m': '15 to 60 minutes',
    over_1h: 'Over 1 hour' };

  function ordered(items, order) {
    return Object.keys(order).map(function (k) {
      var found = items.filter(function (i) { return i.value === k; })[0];
      return { value: k, count: found ? found.count : 0 };
    });
  }

  function activity(history) {
    var t = history.totals;
    var node = $('activity');
    clear(node);
    var without = t.rooms_closed - t.rooms_with_guest;
    var average = t.rooms_closed > 0 ? (t.party_total / t.rooms_closed).toFixed(2) : '–';
    node.appendChild(card('Rooms created', fmt(t.rooms_created)));
    node.appendChild(card('Successful joins', fmt(t.room_joins)));
    node.appendChild(card('Rooms with a guest', fmt(t.rooms_with_guest) + ' · ' + share(t.rooms_with_guest, t.rooms_closed)));
    node.appendChild(card('Rooms without a guest', fmt(without) + ' · ' + share(without, t.rooms_closed)));
    node.appendChild(card('Average largest party', average));
    var joins = ' Joins are successful room joins, not players: a player entering several rooms counts each time.';
    $('activity-note').textContent = t.rooms_closed > 0
      ? 'Last ' + history.days + ' days. Guest shares and the average count rooms that ended (' + fmt(t.rooms_closed) + ').' + joins
      : 'Room lifecycle statistics begin as rooms end.' + joins;
    var occ = $('d-occupancy');
    var life = $('d-lifetime');
    clear(occ);
    clear(life);
    rows(occ, 'Largest party reached', ordered(history.dimensions.occupancy, OCCUPANCY), function (v) { return OCCUPANCY[v]; });
    rows(life, 'Room lifetime', ordered(history.dimensions.lifetime, LIFETIME), function (v) { return LIFETIME[v]; });
  }

  var CHARACTERS = { Fighter: 'Fighter', Amazon: 'Amazon', Dwarf: 'Dwarf', Elf: 'Elf', Wizard: 'Wizard', Sorceress: 'Sorceress' };
  var PARTY_SIZE = { '1': '1 player', '2': '2 players', '3': '3 players', '4': '4 players' };

  function total(items) { return items.reduce(function (a, i) { return a + i.count; }, 0); }

  function parties(history) {
    var d = history.dimensions;
    var sizes = d.party_size || [];
    var reports = total(sizes);
    var since = history.party_first_day || null;
    var note = $('party-note');
    var cards = $('party-cards');
    clear(cards);
    if (reports > 0) {
      var seats = sizes.reduce(function (a, i) { return a + Number(i.value) * i.count; }, 0);
      var repeated = sum(d.party_duplicate || [], ['yes']);
      cards.appendChild(card('Parties reported', fmt(reports)));
      cards.appendChild(card('Average party size', (seats / reports).toFixed(2)));
      cards.appendChild(card('With repeated classes', fmt(repeated) + ' · ' + share(repeated, reports)));
      note.textContent = 'Last ' + history.days + ' days.' + (since ? ' Reporting began on ' + since + '; earlier rooms are not part of these counts.' : '');
    } else if (since) {
      note.textContent = 'No data yet in the last ' + history.days + ' days. The first party snapshot was reported on ' + since + '.';
    } else {
      note.textContent = 'No data yet. No game version has reported party snapshots to this service.';
    }
    var characters = $('d-character');
    var hosts = $('d-host');
    var size = $('d-party-size');
    var compositions = $('d-composition');
    [characters, hosts, size, compositions].forEach(clear);
    rows(characters, 'Characters in parties', ordered(d.character || [], CHARACTERS));
    rows(hosts, 'Host character', ordered(d.host_character || [], CHARACTERS));
    rows(size, 'Party size', ordered(sizes, PARTY_SIZE), function (v) { return PARTY_SIZE[v]; });
    rows(compositions, 'Most common compositions · top 12', d.party_composition || [], function (v) { return v.split('+').join(' + '); });
  }

  function health(history) {
    var t = history.totals;
    var abuseCounts = history.dimensions.abuse;
    var node = $('health');
    clear(node);
    node.appendChild(card('Reconnects not counted as sessions', fmt(sum(abuseCounts, ['hello_reconnect_deduped']))));
    node.appendChild(card('Incompatibility rejects', fmt(t.incompatible_rejects)));
    node.appendChild(card('Rooms that used the relay', fmt(t.rooms_relayed) + ' · ' + share(t.rooms_relayed, t.rooms_closed)));
    node.appendChild(card('Room operations refused', fmt(sum(abuseCounts, ['room_create_limited', 'room_join_limited']))));
  }

  function dailyTable(daily) {
    var table = $('daily');
    clear(table);
    var cols = [['day', 'Day'], ['sessions', 'Sessions'], ['peak_online', 'Peak online'], ['peak_in_rooms', 'Peak in rooms'],
      ['peak_rooms', 'Peak rooms'], ['rooms_created', 'Rooms created'], ['room_joins', 'Joins'], ['rooms_with_guest', 'With guest'],
      ['rooms_relayed', 'Relay rooms'], ['incompatible_rejects', 'Rejects']];
    var head = el('tr');
    cols.forEach(function (c) { head.appendChild(el('th', '', c[1])); });
    var thead = el('thead');
    thead.appendChild(head);
    table.appendChild(thead);
    var body = el('tbody');
    daily.slice().reverse().forEach(function (d) {
      var tr = el('tr');
      cols.forEach(function (c) { tr.appendChild(el('td', '', c[0] === 'day' ? d.day : fmt(d[c[0]]))); });
      body.appendChild(tr);
    });
    table.appendChild(body);
  }

  function rejects(history) {
    var node = $('rejects');
    clear(node);
    var total = history.daily.reduce(function (a, d) { return a + d.incompatible_rejects; }, 0);
    node.appendChild(el('p', 'muted', fmt(total) + ' operations refused for an incompatible protocol in the last ' + history.days +
      ' days. Rooms only hidden from a search are not counted.'));
    rows(node, 'By reason', history.dimensions.reject, function (v) { return REJECTS[v] || v; });
  }

  function abuse(history) {
    var node = $('abuse');
    clear(node);
    var items = history.dimensions.abuse;
    node.appendChild(el('p', 'muted', 'Counts over the last ' + history.days + ' days, written about every 15 minutes.'));
    if (!items.length) node.appendChild(el('p', 'empty', 'None'));
    var table = el('table');
    var body = el('tbody');
    items.forEach(function (item) {
      var tr = el('tr');
      tr.appendChild(el('td', '', ABUSE[item.value] || item.value));
      tr.appendChild(el('td', '', fmt(item.count)));
      body.appendChild(tr);
    });
    table.appendChild(body);
    node.appendChild(table);
  }

  var bugReports = [];
  var bugTicket = '';
  var bugLoadingTicket = '';
  var bugLogText = '';
  var view = 'dashboard';
  var stampLabel = new Intl.DateTimeFormat('en-GB', { timeZone: ZONE, day: 'numeric', month: 'short', year: 'numeric', hour: '2-digit', minute: '2-digit' });
  var bugDayLabel = new Intl.DateTimeFormat('en-CA', { timeZone: ZONE, year: 'numeric', month: '2-digit', day: '2-digit' });
  var BUG_STATUSES = {
    new: ['New', 'The report has not been reviewed yet.'],
    investigating: ['Investigating', 'The report is being investigated.'],
    fixed: ['Fixed', 'The problem has been corrected.'],
    cannot_reproduce: ['Cannot reproduce', 'The problem could not be reproduced with the available information.'],
    duplicate: ['Duplicate', 'The same problem is already tracked by another report.'],
    not_a_bug: ['Not a bug', 'The reported behavior is expected or is not a Dragon\u2019s Crown Pro bug.'],
    closed: ['Closed', 'The report was intentionally finished without a specific fix or another more precise status. It does not mean Fixed.']
  };

  function bugOptions(id, key) {
    var select = $(id);
    var chosen = select.value;
    var seen = {};
    bugReports.forEach(function (r) { seen[r[key]] = true; });
    clear(select);
    var all = el('option', '', 'All');
    all.value = '';
    select.appendChild(all);
    Object.keys(seen).sort().forEach(function (value) {
      var option = el('option', '', value);
      option.value = value;
      select.appendChild(option);
    });
    select.value = seen[chosen] ? chosen : '';
  }

  function bugShown() {
    var version = $('bug-version').value;
    var build = $('bug-build').value;
    var day = $('bug-day').value;
    var status = $('bug-status-filter').value;
    return bugReports.filter(function (r) {
      return (!version || r.launcher_version === version) && (!build || r.game_build === build) &&
        (!day || bugDayLabel.format(r.ms) === day) && (!status || r.status === status);
    });
  }

  function bugFact(list, name, value) {
    list.appendChild(el('dt', '', name));
    list.appendChild(el('dd', '', value || '–'));
  }

  function showBugStatusMeaning() {
    var status = $('bug-status').value;
    $('bug-status-meaning').textContent = BUG_STATUSES[status][1];
    $('bug-fixed-field').hidden = status !== 'fixed';
  }

  function bugStatusForm(r) {
    $('bug-status').value = r.status;
    $('bug-fixed').value = r.fixed_version;
    $('bug-message').value = r.player_message;
    $('bug-status-note').textContent = '';
    showBugStatusMeaning();
  }

  function bugDetail(r) {
    bugTicket = r.ticket;
    bugLoadingTicket = '';
    bugLogText = r.log_text;
    $('bug-title').textContent = r.ticket;
    $('bug-text').textContent = r.description;
    var facts = $('bug-facts');
    clear(facts);
    bugFact(facts, 'Report ID', r.ticket);
    bugFact(facts, 'Submitted', stampLabel.format(r.ms) + ' (Sao Paulo)');
    bugFact(facts, 'Launcher version', r.launcher_version);
    bugFact(facts, 'Game build', r.game_build);
    bugFact(facts, 'Game version', r.game_version);
    bugFact(facts, 'CUSA', r.cusa);
    bugFact(facts, 'Operating system', r.operating_system);
    bugFact(facts, 'Original log file', r.log_file);
    bugFact(facts, 'Log clipped', r.log_clipped === 'yes' ? 'Yes, only its end was sent' : 'No');
    $('bug-log-name').textContent = 'Sanitized log';
    $('bug-log').textContent = bugLogText;
    $('bug-note').textContent = '';
    bugStatusForm(r);
    $('bug-detail').hidden = false;
    renderBugList();
  }

  function openBugReport(ticket) {
    bugLoadingTicket = ticket;
    return api('/admin/api/incidents/' + encodeURIComponent(ticket)).then(function (report) {
      if (report.ticket === bugLoadingTicket) bugDetail(report);
    }).catch(function (error) {
      if (error instanceof AuthError) { signOut('The token was not accepted.'); return; }
      if (ticket !== bugLoadingTicket) return;
      bugLoadingTicket = '';
      $('bug-count').textContent = 'Could not open ' + ticket + '.';
    });
  }

  function forgetBugReport() {
    bugTicket = '';
    bugLoadingTicket = '';
    bugLogText = '';
    $('bug-detail').hidden = true;
  }

  function closeBugReport() {
    forgetBugReport();
    renderBugList();
  }

  function toggleBugReport(ticket) {
    if (ticket === bugTicket) closeBugReport();
    else openBugReport(ticket);
  }

  function downloadBugLog() {
    var ticket = bugTicket;
    if (!ticket) return Promise.resolve();
    return request('/admin/api/incidents/' + encodeURIComponent(ticket) + '/log').then(function (r) {
      var named = /filename="([^"]+)"/.exec(r.headers.get('Content-Disposition') || '');
      return r.blob().then(function (blob) {
        var url = URL.createObjectURL(blob);
        var link = el('a');
        link.href = url;
        link.download = named ? named[1] : ticket + '.log';
        link.click();
        setTimeout(function () { URL.revokeObjectURL(url); }, 1000);
      });
    }).catch(function (error) {
      if (error instanceof AuthError) { signOut('The token was not accepted.'); return; }
      $('bug-note').textContent = 'Could not download the log.';
    });
  }

  function copyBugLog() {
    if (!bugLogText) return Promise.resolve();
    var say = function (text) { $('bug-note').textContent = text; };
    if (!navigator.clipboard || !navigator.clipboard.writeText) { say('Copying is not available in this browser.'); return Promise.resolve(); }
    return navigator.clipboard.writeText(bugLogText).then(function () { say('Log copied.'); }, function () { say('Could not copy the log.'); });
  }

  function saveBugStatus() {
    var ticket = bugTicket;
    if (!ticket) return Promise.resolve();
    var status = $('bug-status').value;
    var change = { status: status, fixed_version: status === 'fixed' ? $('bug-fixed').value.trim() : '', player_message: $('bug-message').value };
    return request('/admin/api/incidents/' + encodeURIComponent(ticket) + '/status', JSON.stringify(change)).then(function (r) {
      return r.json();
    }).then(function (saved) {
      bugReports.forEach(function (r) {
        if (r.ticket === ticket) { r.status = saved.status; r.status_updated_at = saved.status_updated_at; }
      });
      renderBugList();
      if (ticket !== bugTicket) return;
      bugStatusForm(saved);
      $('bug-status-note').textContent = 'Saved.';
    }).catch(function (error) {
      if (error instanceof AuthError) { signOut('The token was not accepted.'); return; }
      if (ticket === bugTicket) $('bug-status-note').textContent = 'Could not save the status. Check the version and the message.';
    });
  }

  function bugTicketCell(r) {
    var td = el('td');
    var button = el('button', 'ticket', r.ticket);
    button.type = 'button';
    button.addEventListener('click', function () { toggleBugReport(r.ticket); });
    td.appendChild(button);
    return td;
  }

  function renderBugList() {
    var shown = bugShown();
    table($('bug-list'), ['Report ID', 'Submitted', 'Launcher', 'Game build', 'CUSA', 'Operating system', 'Description', 'Status'], shown.map(function (r) {
      return [bugTicketCell(r), stampLabel.format(r.ms), r.launcher_version, r.game_build || '–', r.cusa || '–', r.operating_system,
        el('td', 'bug-summary', r.preview), BUG_STATUSES[r.status][0]];
    }), function (i) { return shown[i].ticket === bugTicket ? 'selected' : ''; });
    $('bug-count').textContent = !bugReports.length ? 'No reports yet.'
      : shown.length === bugReports.length ? fmt(shown.length) + ' reports' : fmt(shown.length) + ' of ' + fmt(bugReports.length) + ' reports';
  }

  function loadBugReports() {
    $('bug-count').textContent = 'Loading…';
    return api('/admin/api/incidents').then(function (r) {
      bugReports = r.incidents;
      bugOptions('bug-version', 'launcher_version');
      bugOptions('bug-build', 'game_build');
      renderBugList();
    }).catch(function (error) {
      if (error instanceof AuthError) { signOut('The token was not accepted.'); return; }
      $('bug-count').textContent = error && error.message ? error.message : 'Could not load the reports.';
    });
  }

  function showView(name) {
    view = name;
    $('view-dashboard').hidden = name !== 'dashboard';
    $('view-bugs').hidden = name !== 'bugs';
    $('tab-dashboard').setAttribute('aria-selected', String(name === 'dashboard'));
    $('tab-bugs').setAttribute('aria-selected', String(name === 'bugs'));
  }

  function render(summary, history) {
    $('c-online').textContent = fmt(summary.live.online);
    $('c-in-rooms').textContent = fmt(summary.live.in_rooms);
    $('c-rooms').textContent = fmt(summary.live.rooms);
    $('c-peak').textContent = fmt(summary.today.peak_online);
    $('c-sessions').textContent = fmt(summary.today.sessions);
    $('c-rooms-today').textContent = fmt(summary.today.rooms_created);
    $('c-joins-today').textContent = fmt(summary.today.room_joins);
    comparison(history);
    activity(history);
    parties(history);
    health(history);
    usageChart($('usage'), history);
    barChart($('h-sessions'), history.daily, 'sessions', 'Sessions');
    barChart($('h-rooms'), history.daily, 'rooms_created', 'Rooms created');
    barChart($('h-joins'), history.daily, 'room_joins', 'Successful joins');
    barChart($('h-peak-online'), history.daily, 'peak_online', 'Peak online');
    barChart($('h-peak-rooms'), history.daily, 'peak_in_rooms', 'Peak in rooms');
    distribution('d-platform', summary.live.platform, history.dimensions.platform, history.days);
    distribution('d-version', summary.live.version, history.dimensions.version, history.days,
      function (v) { return /^[0-9]/.test(v) ? 'v' + v : v; });
    distribution('d-country', summary.live.country, history.dimensions.country, history.days, countryText);
    dailyTable(history.daily);
    rejects(history);
    abuse(history);
    $('status').textContent = 'Updated ' + timeLabel.format(summary.generated) + ' (Sao Paulo)';
  }

  function load() {
    var days = $('range').value;
    $('status').textContent = 'Loading…';
    return Promise.all([api('/admin/api/summary'), api('/admin/api/history?days=' + encodeURIComponent(days))])
      .then(function (r) { render(r[0], r[1]); })
      .catch(function (error) {
        if (error instanceof AuthError) { signOut('The token was not accepted.'); return; }
        $('status').textContent = error && error.message ? error.message : 'Could not load the metrics.';
      });
  }

  function showDashboard() {
    $('login').hidden = true;
    $('dash').hidden = false;
    load();
    if (timer) clearInterval(timer);
    timer = setInterval(function () { if (!document.hidden && view === 'dashboard') load(); }, 60000);
  }

  function signOut(message) {
    token = '';
    storeToken('');
    if (timer) clearInterval(timer);
    timer = null;
    bugReports = [];
    forgetBugReport();
    showView('dashboard');
    $('dash').hidden = true;
    $('login').hidden = false;
    $('token').value = '';
    $('login-error').textContent = message || '';
    $('token').focus();
  }

  $('login').addEventListener('submit', function (event) {
    event.preventDefault();
    token = $('token').value.trim();
    $('token').value = '';
    if (!token) return;
    storeToken($('remember').checked ? token : '');
    $('login-error').textContent = '';
    showDashboard();
  });
  $('tab-dashboard').addEventListener('click', function () { showView('dashboard'); load(); });
  $('tab-bugs').addEventListener('click', function () { showView('bugs'); loadBugReports(); });
  $('bug-refresh').addEventListener('click', loadBugReports);
  $('bug-download').addEventListener('click', downloadBugLog);
  $('bug-copy').addEventListener('click', copyBugLog);
  $('bug-close').addEventListener('click', closeBugReport);
  $('bug-status').addEventListener('change', showBugStatusMeaning);
  $('bug-status-save').addEventListener('click', saveBugStatus);
  $('refresh').addEventListener('click', load);
  $('range').addEventListener('change', load);
  ['bug-version', 'bug-build', 'bug-day', 'bug-status-filter'].forEach(function (id) { $(id).addEventListener('change', renderBugList); });
  $('signout').addEventListener('click', function () { signOut(''); });

  token = storedToken();
  if (token) { $('remember').checked = true; showDashboard(); }
}());
`;
