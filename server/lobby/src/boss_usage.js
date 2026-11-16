// Boss Battle Online counters for the private dashboard: parties created, boss plays by encounter, and the accounts that
// joined a party. No name, address, room or join code is kept.
export const BOSS_NAMES = ['Harpy', 'Wyvern', 'Doom Beetle', 'Golem', 'Gazer', 'Pirates', 'Minotaur', 'Vampire', 'Warlock',
  'Medusa', 'Wraith', 'Killer Rabbit', 'Kraken', 'Cyclops', 'Chimera', 'Gargoyle Gate', 'Red Dragon', 'Arch Demon',
  'Ancient Dragon', 'Demon Lord'];
export const BOSS_TOP_SHOWN = 10;
export const BOSS_STATE_ATTR = 0x7f04;

const STATE_FORMAT = 1;
const PHASE_STARTING = 2;
const PHASE_FIGHTING = 3;

export const BOSS_USAGE_SCHEMA = [
  `CREATE TABLE IF NOT EXISTS boss_plays (boss INTEGER PRIMARY KEY, plays INTEGER NOT NULL)`,
  `CREATE TABLE IF NOT EXISTS boss_parties (id INTEGER PRIMARY KEY CHECK (id = 1), created INTEGER NOT NULL)`,
  `CREATE TABLE IF NOT EXISTS boss_users (account TEXT PRIMARY KEY)`,
];

// The boss of a host state that is an encounter being started or fought, else -1.
export function activeBoss(hex) {
  if (typeof hex !== 'string' || !/^[0-9a-f]{16}$/i.test(hex)) return -1;
  const bytes = hex.match(/../g).map((b) => parseInt(b, 16));
  const active = bytes[0] === STATE_FORMAT && (bytes[1] === PHASE_STARTING || bytes[1] === PHASE_FIGHTING);
  return active && bytes[3] < BOSS_NAMES.length ? bytes[3] : -1;
}

export class BossUsage {
  constructor(sql) {
    this.sql = sql;
    for (const statement of BOSS_USAGE_SCHEMA) sql.run(statement);
    if (!sql.all('PRAGMA table_info(rooms)').some((c) => c.name === 'boss_party')) {
      sql.run('ALTER TABLE rooms ADD COLUMN boss_party INTEGER NOT NULL DEFAULT 0');
    }
  }

  partyCreated(accounts) {
    this.sql.run('INSERT INTO boss_parties (id, created) VALUES (1, 1) ON CONFLICT(id) DO UPDATE SET created = created + 1');
    for (const account of accounts) this.noteUser(account);
  }

  noteUser(account) {
    this.sql.run('INSERT OR IGNORE INTO boss_users (account) VALUES (?)', account);
  }

  // A play is a state that starts or fights a boss after one that was not that same boss.
  notePublished(before, after) {
    const boss = activeBoss(after);
    if (boss < 0 || activeBoss(before) === boss) return;
    this.sql.run('INSERT INTO boss_plays (boss, plays) VALUES (?, 1) ON CONFLICT(boss) DO UPDATE SET plays = plays + 1', boss);
  }

  totals() {
    return {
      parties_created: this.sql.get('SELECT COALESCE(SUM(created), 0) AS n FROM boss_parties').n,
      plays: this.sql.get('SELECT COALESCE(SUM(plays), 0) AS n FROM boss_plays').n,
      users: this.sql.get('SELECT COUNT(*) AS n FROM boss_users').n,
    };
  }

  // Most played first; equal counts in encounter order.
  top() {
    return this.sql.all('SELECT boss, plays FROM boss_plays ORDER BY plays DESC, boss ASC LIMIT ?', BOSS_TOP_SHOWN)
      .map((r, i) => ({ rank: i + 1, boss: r.boss, name: BOSS_NAMES[r.boss] ?? `Boss ${r.boss + 1}`, plays: r.plays }));
  }
}
