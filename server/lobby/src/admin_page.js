// The private dashboard (/admin): a static shell, its stylesheet and script, served same-origin under a strict CSP.
// The script keeps the token in memory (sessionStorage only when asked) and sends it as a Bearer header.
export const ADMIN_HTML = String.raw`<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="referrer" content="no-referrer">
<meta name="robots" content="noindex, nofollow">
<title>DCP Online Metrics</title>
<link rel="stylesheet" href="/admin/app.css">
</head>
<body>
<header class="top">
  <h1>Dragon's Crown Pro PC · Online Metrics</h1>
  <p class="muted">Anonymous aggregate statistics of the online service. Days are America/Sao_Paulo.</p>
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
      <button id="signout" type="button" class="quiet">Sign out</button>
    </div>
    <div class="cards">
      <div class="card"><span class="label">Online now</span><span id="c-online" class="value">–</span></div>
      <div class="card"><span class="label">In rooms</span><span id="c-in-rooms" class="value">–</span></div>
      <div class="card"><span class="label">Open rooms</span><span id="c-rooms" class="value">–</span></div>
      <div class="card"><span class="label">Peak today</span><span id="c-peak" class="value">–</span></div>
      <div class="card"><span class="label">Sessions today</span><span id="c-sessions" class="value">–</span></div>
    </div>
    <p id="today-line" class="muted"></p>
    <div class="panel">
      <h2>Usage over time</h2>
      <p class="muted">Highest value in each 15-minute interval; kept for 90 days.</p>
      <div id="usage" class="chart"></div>
    </div>
    <div class="panel">
      <h2>Sessions by day</h2>
      <div id="sessions" class="chart"></div>
    </div>
    <div class="grid">
      <div class="panel"><h2>Platform</h2><div id="d-platform"></div></div>
      <div class="panel"><h2>Port version</h2><div id="d-version"></div></div>
      <div class="panel"><h2>Country</h2><div id="d-country"></div></div>
    </div>
    <div class="panel">
      <h2>Daily totals</h2>
      <div class="scroll"><table id="daily"></table></div>
    </div>
    <details class="panel">
      <summary>Diagnostics: incompatibility rejects</summary>
      <div id="rejects"></div>
    </details>
    <div class="panel notes">
      <h2>How to read this</h2>
      <ul>
        <li>A session is one connection of a launcher or game that went online. The launcher and the game each count,
          and a reconnect counts again. Sessions are not players: there is no unique-player count.</li>
        <li>Platform, version and country by day count sessions started. "Now" counts players online, one per account,
          from the connections open at this moment.</li>
        <li>Country comes from Cloudflare and is stored only as a daily count. Unknown: Cloudflare did not say.</li>
        <li>Nothing here stores names, Friend Codes, accounts, addresses, rooms or gameplay.</li>
      </ul>
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
input[type="password"], select {
  font: inherit;
  padding: 8px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--surface-0);
  color: var(--text-primary);
  width: 100%;
}
select { width: auto; }
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

  function api(path) {
    return fetch(path, { headers: { Authorization: 'Bearer ' + token }, cache: 'no-store', credentials: 'omit' })
      .then(function (r) {
        if (r.status === 401) throw new AuthError();
        if (!r.ok) throw new Error('The service answered ' + r.status + '.');
        return r.json();
      });
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

  function sessionsChart(node, daily) {
    clear(node);
    var W = 900, H = 220, pad = { top: 10, right: 12, bottom: 26, left: 44 };
    var max = niceMax(Math.max.apply(null, daily.map(function (d) { return d.sessions; })));
    var root = frame(W, H, max, pad);
    var plotW = W - pad.left - pad.right;
    var plotH = H - pad.top - pad.bottom;
    var slot = plotW / daily.length;
    var gap = Math.min(2, slot * 0.2);
    daily.forEach(function (d, i) {
      var h = plotH * d.sessions / max;
      var x = pad.left + slot * i + gap / 2;
      var w = Math.max(1, slot - gap);
      if (h > 0) {
        var r = Math.min(4, w / 2, h);
        var y = pad.top + plotH - h;
        var path = 'M' + x + ' ' + (pad.top + plotH) + 'V' + (y + r) + 'Q' + x + ' ' + y + ' ' + (x + r) + ' ' + y +
          'H' + (x + w - r) + 'Q' + (x + w) + ' ' + y + ' ' + (x + w) + ' ' + (y + r) + 'V' + (pad.top + plotH) + 'Z';
        root.appendChild(svg('path', { d: path, 'class': 'bar' }));
      }
      var hit = svg('rect', { x: pad.left + slot * i, y: pad.top, width: slot, height: plotH, 'class': 'hit' });
      hit.addEventListener('mousemove', function (event) { showTip(event, [dayText(d.day), 'Sessions: ' + fmt(d.sessions)]); });
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
    if (!items.length) box.appendChild(el('p', 'empty', 'None'));
    var total = items.reduce(function (a, b) { return a + b.count; }, 0);
    var top = items.length ? items[0].count : 1;
    items.slice(0, 12).forEach(function (item) {
      var row = el('div', 'row');
      var label = name ? name(item.value) : item.value;
      row.appendChild(el('span', 'name', label)).title = label;
      var track = el('span', 'track');
      var fill = el('span', 'fill');
      fill.style.width = Math.max(2, 100 * item.count / top) + '%';
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

  function dailyTable(daily) {
    var table = $('daily');
    clear(table);
    var cols = [['day', 'Day'], ['sessions', 'Sessions'], ['peak_online', 'Peak online'], ['peak_in_rooms', 'Peak in rooms'],
      ['peak_rooms', 'Peak rooms'], ['rooms_created', 'Rooms created'], ['room_joins', 'Joins'], ['incompatible_rejects', 'Rejects']];
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

  function render(summary, history) {
    $('c-online').textContent = fmt(summary.live.online);
    $('c-in-rooms').textContent = fmt(summary.live.in_rooms);
    $('c-rooms').textContent = fmt(summary.live.rooms);
    $('c-peak').textContent = fmt(summary.today.peak_online);
    $('c-sessions').textContent = fmt(summary.today.sessions);
    var t = summary.today;
    $('today-line').textContent = 'Today (' + summary.day + '): ' + fmt(t.rooms_created) + ' rooms created, ' + fmt(t.room_joins) +
      ' joins, peak ' + fmt(t.peak_in_rooms) + ' in rooms and ' + fmt(t.peak_rooms) + ' open rooms, ' + fmt(t.incompatible_rejects) +
      ' incompatibility rejects.';
    usageChart($('usage'), history);
    sessionsChart($('sessions'), history.daily);
    distribution('d-platform', summary.live.platform, history.dimensions.platform, history.days);
    distribution('d-version', summary.live.version, history.dimensions.version, history.days,
      function (v) { return /^[0-9]/.test(v) ? 'v' + v : v; });
    distribution('d-country', summary.live.country, history.dimensions.country, history.days, countryText);
    dailyTable(history.daily);
    rejects(history);
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
    timer = setInterval(function () { if (!document.hidden) load(); }, 60000);
  }

  function signOut(message) {
    token = '';
    storeToken('');
    if (timer) clearInterval(timer);
    timer = null;
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
  $('refresh').addEventListener('click', load);
  $('range').addEventListener('change', load);
  $('signout').addEventListener('click', function () { signOut(''); });

  token = storedToken();
  if (token) { $('remember').checked = true; showDashboard(); }
}());
`;
