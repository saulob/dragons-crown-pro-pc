// Who has taken part in an Early Story Online room, for the private dashboard only. The one place besides the lobby's own
// accounts and members tables that keeps a name and an account id; it holds no address, endpoint, room or join code.
export const STORY_USERS_SCHEMA = `CREATE TABLE IF NOT EXISTS story_users (account TEXT PRIMARY KEY, name TEXT NOT NULL,
  hosted INTEGER NOT NULL DEFAULT 0, guested INTEGER NOT NULL DEFAULT 0, first_used INTEGER NOT NULL, last_used INTEGER NOT NULL)`;

export const STORY_USERS_SHOWN = 10;
// Accounts remembered per room to count each once; a room that churns through more is not counted further.
export const STORY_SEEN_PER_ROOM = 64;

export const STORY_ROLES = { host: 'hosted', guest: 'guested' };

export class StoryUsage {
  constructor(sql, now) {
    this.sql = sql;
    this.now = now;
    sql.run(STORY_USERS_SCHEMA);
  }

  // One participation of `account` in one room, as its host (`hosted`) or as a guest (`guested`).
  record(account, name, column) {
    if (column !== STORY_ROLES.host && column !== STORY_ROLES.guest) throw new Error(`unknown role ${column}`);
    const now = this.now();
    this.sql.run(`INSERT INTO story_users (account, name, ${column}, first_used, last_used) VALUES (?, ?, 1, ?, ?)
      ON CONFLICT(account) DO UPDATE SET name = excluded.name, ${column} = ${column} + 1, last_used = excluded.last_used`,
    account, name, now, now);
  }

  totals() {
    return this.sql.get(`SELECT COUNT(*) AS users, COALESCE(SUM(hosted + guested), 0) AS sessions FROM story_users`);
  }

  // At most STORY_USERS_SHOWN users: those in `active` first, then the most recent.
  users(active) {
    const playing = new Set(active);
    const columns = 'account, name, hosted, guested, last_used';
    const rows = new Map(this.sql.all(`SELECT ${columns} FROM story_users ORDER BY last_used DESC, account LIMIT ?`,
      STORY_USERS_SHOWN).map((r) => [r.account, r]));
    for (const account of playing) {
      if (rows.has(account)) continue;
      const row = this.sql.get(`SELECT ${columns} FROM story_users WHERE account = ?`, account);
      if (row) rows.set(account, row);
    }
    return [...rows.values()]
      .sort((a, b) => playing.has(b.account) - playing.has(a.account) || b.last_used - a.last_used
        || (a.account < b.account ? -1 : 1))
      .slice(0, STORY_USERS_SHOWN);
  }
}

export const storyRole = (row) => (row.hosted && row.guested ? 'Both' : row.hosted ? 'Host' : 'Guest');
