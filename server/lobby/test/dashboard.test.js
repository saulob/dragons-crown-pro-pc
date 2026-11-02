import assert from 'node:assert/strict';
import { test } from 'node:test';

import { ADMIN_HTML, ADMIN_JS } from '../src/admin_page.js';
import { metricsDay } from '../src/metrics.js';
import { makeLobby } from './harness.js';

const DAY_MS = 24 * 3600 * 1000;

class Node {
  constructor(tag) {
    this.tag = tag;
    this.children = [];
    this.attrs = {};
    this.listeners = {};
    this.style = {};
    this.className = '';
    this.hidden = false;
    this.value = '';
    this.offsetWidth = 50;
    this.offsetHeight = 20;
    this.ownText = '';
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

  getAttribute(name) { return this.attrs[name] ?? null; }

  addEventListener(type, fn) { (this.listeners[type] ??= []).push(fn); }

  fire(type, event = { clientX: 10, clientY: 10 }) { for (const fn of this.listeners[type] ?? []) fn(event); }

  getBoundingClientRect() { return { left: 0, top: 0, right: 900, bottom: 200, width: 900, height: 200 }; }

  focus() {}

  find(predicate, found = []) {
    for (const child of this.children) {
      if (predicate(child)) found.push(child);
      child.find(predicate, found);
    }
    return found;
  }

  hasClass(name) { return (this.attrs.class ?? this.className).split(' ').includes(name); }
}

function cardsOf(html) {
  return [...html.matchAll(/<div class="card"[^>]*data-help="([^"]*)"/g)].map((m) => {
    const card = new Node('div');
    card.setAttribute('data-help', m[1]);
    return card;
  });
}

async function dashboard(env, days) {
  const ids = new Map();
  const get = (id) => {
    if (!ids.has(id)) ids.set(id, new Node('div'));
    return ids.get(id);
  };
  const cards = cardsOf(ADMIN_HTML);
  const document = {
    hidden: false,
    getElementById: get,
    createElement: (tag) => new Node(tag),
    createElementNS: (ns, tag) => new Node(tag),
    createTextNode: (text) => Object.assign(new Node('#text'), { ownText: text }),
    querySelectorAll: () => cards,
  };
  get('range').value = String(days);
  get('tip').hidden = true;
  const fetch = async (path) => ({
    ok: true,
    status: 200,
    json: async () => (path.startsWith('/admin/api/summary') ? env.lobby.metricsSummary()
      : env.lobby.metricsHistory(Number(new URL(path, 'https://x').searchParams.get('days')))),
  });
  const storage = { getItem: () => 'token', setItem() {}, removeItem() {} };
  new Function('document', 'window', 'fetch', 'sessionStorage', 'setInterval', 'clearInterval', ADMIN_JS)(
    document, { innerWidth: 1200, innerHeight: 800 }, fetch, storage, () => 0, () => {});
  for (let i = 0; i < 10; i++) await new Promise((resolve) => setImmediate(resolve));
  return { ids, get, cards, tip: get('tip') };
}

const dayAgo = (env, n) => metricsDay(env.clock.now - n * DAY_MS);

function putDay(env, n, columns) {
  const names = Object.keys(columns);
  env.host.sql.run(`INSERT INTO metrics_daily (day, ${names.join(', ')}) VALUES (?, ${names.map(() => '?').join(', ')})`,
    dayAgo(env, n), ...Object.values(columns));
}

const everyText = (page) => [...page.ids.values()].map((n) => n.textContent).join('\n');
const tipLines = (page) => page.tip.children.map((c) => c.textContent);
const valueLabels = (page, id) => page.get(id).find((n) => n.hasClass('value-label')).map((n) => n.textContent);
const hits = (page, id) => page.get(id).find((n) => n.hasClass('hit'));

test('zero distributions show No data yet: no NaN, Infinity, percentages or bars', async () => {
  const env = makeLobby();
  const page = await dashboard(env, 30);
  for (const id of ['d-occupancy', 'd-lifetime']) {
    const box = page.get(id);
    assert.ok(box.textContent.includes('No data yet'), id);
    assert.ok(!box.textContent.includes('%'), id);
    assert.equal(box.find((n) => n.hasClass('fill')).length, 0, id);
    assert.equal(box.find((n) => n.hasClass('row')).length, 0, id);
  }
  assert.ok(page.get('activity-note').textContent.startsWith('Room lifecycle statistics begin as rooms end.'));
  assert.ok(!/NaN|Infinity|undefined|\[object/.test(everyText(page)));
});

test('a distribution with data shows counts, percentages and no bar for zero values', async () => {
  const env = makeLobby();
  putDay(env, 0, { rooms_closed: 20, rooms_with_guest: 15, party_total: 35 });
  const day = dayAgo(env, 0);
  env.host.sql.run(`INSERT INTO metrics_daily_dimensions VALUES (?, 'occupancy', '1', 5)`, day);
  env.host.sql.run(`INSERT INTO metrics_daily_dimensions VALUES (?, 'occupancy', '2', 15)`, day);
  const page = await dashboard(env, 30);
  const rows = page.get('d-occupancy').find((n) => n.hasClass('row'));
  const counts = rows.map((r) => r.find((n) => n.hasClass('count'))[0].textContent);
  assert.deepEqual(counts, ['5 · 25%', '15 · 75%', '0 · 0%', '0 · 0%']);
  const widths = rows.map((r) => r.find((n) => n.hasClass('fill'))[0].style.width);
  assert.deepEqual(widths.map((w) => Math.round(parseFloat(w))), [33, 100, 0, 0]);
  assert.ok(page.get('d-lifetime').textContent.includes('No data yet'));
  assert.ok(page.get('activity-note').textContent.includes('rooms that ended (20)'));
  assert.ok(!/NaN|Infinity|undefined/.test(everyText(page)));
});

test('the period comparison names both exact ranges and labels the previous period', async () => {
  const env = makeLobby();
  putDay(env, 0, { sessions: 5, rooms_created: 0, room_joins: 15 });
  putDay(env, 40, { sessions: 0, rooms_created: 0, room_joins: 10 });
  const page = await dashboard(env, 30);
  const first = dayAgo(env, 29);
  const last = dayAgo(env, 0);
  assert.equal(page.get('compare-note').textContent, `${first} to ${last} compared with ${dayAgo(env, 59)} to ${dayAgo(env, 30)}.`);
  const table = page.get('compare');
  assert.deepEqual(table.find((n) => n.tag === 'th').map((n) => n.textContent), ['Metric', 'This period', 'Previous period', 'Change']);
  const row = (name) => table.find((n) => n.tag === 'tr' && n.children[0]?.textContent === name);
  const change = (name) => row(name)[0].children[3].textContent;
  assert.equal(change('Sessions'), 'new');
  assert.equal(change('Rooms created'), '-');
  assert.equal(change('Successful joins'), '+5 (+50%)');
  assert.ok(!/NaN|Infinity|undefined/.test(table.textContent));
});

test('overview cards carry their explanations and show them on hover and keyboard focus', async () => {
  const env = makeLobby();
  const page = await dashboard(env, 7);
  const helps = page.cards.map((c) => c.getAttribute('data-help'));
  assert.equal(helps.length, 7);
  assert.deepEqual(helps, [
    'Players connected to the online service right now, counted once per connected account',
    'Players currently occupying a seat in an online room',
    'Multiplayer rooms currently open',
    'Highest number of players online at the same time today',
    'Online sessions started today, with reconnects inside the existing 10-minute deduplication window not counted again',
    'Multiplayer rooms created today',
    'Successful room joins today, not unique players, and the same player can contribute more than one join',
  ]);
  assert.equal([...ADMIN_HTML.matchAll(/class="card" tabindex="0" data-help=/g)].length, 7);
  assert.ok(!/\son[a-z]+\s*=/i.test(ADMIN_HTML));
  assert.ok(ADMIN_HTML.includes('Joins today'));
  for (const [i, card] of page.cards.entries()) {
    assert.ok(card.listeners.mousemove && card.listeners.mouseleave && card.listeners.focus && card.listeners.blur);
    card.fire('mousemove');
    assert.equal(page.tip.hidden, false);
    assert.deepEqual(tipLines(page), [helps[i]]);
    card.fire('mouseleave');
    assert.equal(page.tip.hidden, true);
    card.fire('focus', {});
    assert.equal(page.tip.hidden, false);
    assert.deepEqual(tipLines(page), [helps[i]]);
    card.fire('blur', {});
    assert.equal(page.tip.hidden, true);
  }
});

const CHARTS = [['h-sessions', 'sessions', 'Sessions'], ['h-rooms', 'rooms_created', 'Rooms created'],
  ['h-joins', 'room_joins', 'Successful joins'], ['h-peak-online', 'peak_online', 'Peak online'],
  ['h-peak-rooms', 'peak_in_rooms', 'Peak in rooms']];

function seedDaily(env) {
  putDay(env, 0, { sessions: 1175, rooms_created: 805, room_joins: 415, peak_online: 18, peak_in_rooms: 10 });
  putDay(env, 2, { sessions: 3, rooms_created: 2, room_joins: 1, peak_online: 4, peak_in_rooms: 2 });
}

test('daily bars carry value labels for 7 and 30 days, none for zero bars', async () => {
  for (const days of [7, 30]) {
    const env = makeLobby();
    seedDaily(env);
    const page = await dashboard(env, days);
    assert.deepEqual(valueLabels(page, 'h-sessions'), ['3', '1,175'], `${days} days`);
    assert.deepEqual(valueLabels(page, 'h-peak-online'), ['4', '18'], `${days} days`);
    for (const [id] of CHARTS) assert.equal(valueLabels(page, id).length, 2, id);
    for (const [id] of CHARTS) assert.equal(hits(page, id).length, days, id);
  }
});

test('daily bars carry no value labels for 90 and 365 days, and keep their tooltips', async () => {
  for (const days of [90, 365]) {
    const env = makeLobby();
    seedDaily(env);
    const page = await dashboard(env, days);
    for (const [id] of CHARTS) {
      assert.deepEqual(valueLabels(page, id), [], `${id} ${days}`);
      assert.equal(hits(page, id).length, days, id);
    }
  }
});

test('daily chart tooltips show the metric name and value, never a DOM object', async () => {
  const env = makeLobby();
  seedDaily(env);
  const page = await dashboard(env, 30);
  const expected = { 'h-sessions': 'Sessions: 1,175', 'h-rooms': 'Rooms created: 805', 'h-joins': 'Successful joins: 415',
    'h-peak-online': 'Peak online: 18', 'h-peak-rooms': 'Peak in rooms: 10' };
  for (const [id] of CHARTS) {
    const slots = hits(page, id);
    slots.at(-1).fire('mousemove');
    const [date, metric] = tipLines(page);
    assert.ok(/^\d{1,2} [A-Z][a-z]{2}$/.test(date), date);
    assert.equal(metric, expected[id]);
    slots.at(0).fire('mousemove');
    assert.ok(/^[A-Za-z ]+: 0$/.test(tipLines(page)[1]));
    slots.at(0).fire('mouseleave');
    assert.equal(page.tip.hidden, true);
  }
  assert.ok(!/\[object/.test(everyText(page)) && !ADMIN_JS.includes('[object'));
});
