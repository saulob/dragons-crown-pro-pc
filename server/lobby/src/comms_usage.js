// Comms Wheel usage for the private dashboard: how often each of the eight fixed messages was sent, kept only as counts
// per stable message id. No text, name, account, address or room is kept, and a report names none of them.
export const COMMS_KEYS = ['hello', 'thanks', 'good_job', 'follow_me', 'group_up', 'need_help', 'ready', 'lets_go'];
export const COMMS_NAMES = ['Hello', 'Thanks', 'Good job', 'Follow me', 'Group up', 'Need help', 'Ready', "Let's go"];
// What one report may add to one id; a larger count is taken as this.
export const COMMS_MAX_PER_ID = 50;

const FIELDS = new Set(['op', 'seq', 'counts']);

export const COMMS_USAGE_SCHEMA = `CREATE TABLE IF NOT EXISTS comms_usage (message INTEGER PRIMARY KEY
  CHECK (message >= 0 AND message < ${COMMS_KEYS.length}), uses INTEGER NOT NULL)`;

// The eight counts of a comms.report request, or null when anything is off: an unknown field, a repeated key, a count
// array that is not exactly eight non-negative integers, or nothing to add. `text` is the frame as received: every quoted
// string of a valid one is a key or the op, so a repeated key adds quotes.
export function parseCommsReport(msg, text) {
  const keys = Object.keys(msg);
  if (keys.some((k) => !FIELDS.has(k)) || !Array.isArray(msg.counts) || msg.counts.length !== COMMS_KEYS.length) return null;
  if (typeof text !== 'string' || text.split('"').length - 1 !== 2 * (keys.length + 1)) return null;
  if (!msg.counts.every((n) => Number.isSafeInteger(n) && n >= 0)) return null;
  const counts = msg.counts.map((n) => Math.min(n, COMMS_MAX_PER_ID));
  return counts.some((n) => n > 0) ? counts : null;
}

export class CommsUsage {
  constructor(sql) {
    this.sql = sql;
    sql.run(COMMS_USAGE_SCHEMA);
  }

  add(counts) {
    counts.forEach((n, message) => {
      if (n > 0) {
        this.sql.run(`INSERT INTO comms_usage (message, uses) VALUES (?, ?)
          ON CONFLICT(message) DO UPDATE SET uses = uses + excluded.uses`, message, n);
      }
    });
  }

  uses() {
    const uses = COMMS_KEYS.map(() => 0);
    for (const row of this.sql.all('SELECT message, uses FROM comms_usage')) uses[row.message] = row.uses;
    return uses;
  }
}

const percent = (n, total) => (total ? Math.round((n / total) * 1000) / 10 : 0);

// { total, most_used: key or null, messages: [{ id, key, name, uses, percent }] in id order }; the lowest id wins a tie.
export function commsSummary(uses) {
  const total = uses.reduce((a, b) => a + b, 0);
  let best = -1;
  uses.forEach((n, i) => {
    if (n > 0 && (best < 0 || n > uses[best])) best = i;
  });
  return {
    total,
    most_used: best < 0 ? null : COMMS_KEYS[best],
    messages: uses.map((n, id) => ({ id, key: COMMS_KEYS[id], name: COMMS_NAMES[id], uses: n, percent: percent(n, total) })),
  };
}
