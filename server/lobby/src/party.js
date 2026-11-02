// Anonymous party snapshots (docs/ONLINE.md, Party statistics): which classes make up an online party, kept only as counts
// per day. A class is the game's own class index; nothing here names a player, an account or a room.
export const PARTY_CLASSES = ['Fighter', 'Amazon', 'Elf', 'Dwarf', 'Sorceress', 'Wizard'];
export const MAX_PARTY = 4;

const FIELDS = new Set(['op', 'seq', 'classes', 'host']);
const isClass = (v) => Number.isInteger(v) && v >= 0 && v < PARTY_CLASSES.length;

// Every multiset of 1 to MAX_PARTY classes, in canonical order: the whole space a composition can take.
function compositions() {
  const found = [];
  const extend = (names, from) => {
    if (names.length) found.push(names.join('+'));
    if (names.length === MAX_PARTY) return;
    for (let c = from; c < PARTY_CLASSES.length; c++) extend([...names, PARTY_CLASSES[c]], c);
  };
  extend([], 0);
  return found;
}

export const PARTY_COMPOSITIONS = compositions();

// What each party dimension may hold: a client never supplies a value, only class indexes.
export const PARTY_VALUES = {
  character: new Set(PARTY_CLASSES),
  host_character: new Set(PARTY_CLASSES),
  party_size: new Set(Array.from({ length: MAX_PARTY }, (_, i) => String(i + 1))),
  party_composition: new Set(PARTY_COMPOSITIONS),
  party_duplicate: new Set(['yes', 'no']),
};

export const PARTY_DIMENSIONS = Object.keys(PARTY_VALUES);
export const PARTY_ROWS_PER_DAY = PARTY_DIMENSIONS.reduce((n, d) => n + PARTY_VALUES[d].size, 0);

// { classes: ascending class indexes, host: a class index or null } from a party.report request, or null when anything is
// off: an unknown field, a repeated key, a size outside 1 to 4, a class outside the six, a host that is not in the party.
// `text` is the frame as received: every quoted string of a valid one is a key or the op, so a repeated key adds quotes.
export function parsePartyReport(msg, text) {
  const keys = Object.keys(msg);
  if (keys.some((k) => !FIELDS.has(k)) || !Array.isArray(msg.classes)) return null;
  if (typeof text !== 'string' || text.split('"').length - 1 !== 2 * (keys.length + 1)) return null;
  const { classes, host } = msg;
  if (classes.length < 1 || classes.length > MAX_PARTY || !classes.every(isClass)) return null;
  if (host !== undefined && !(isClass(host) && classes.includes(host))) return null;
  return { classes: [...classes].sort((a, b) => a - b), host: host ?? null };
}

// The counts one snapshot adds, as [dimension, value, amount]; throws on anything parsePartyReport would refuse. The class
// indexes are already in canonical order, so sorting them orders the composition.
export function partyCounts({ classes, host }) {
  if (!Array.isArray(classes) || classes.length < 1 || classes.length > MAX_PARTY || !classes.every(isClass) ||
      (host !== null && !(isClass(host) && classes.includes(host)))) {
    throw new RangeError('invalid party');
  }
  const names = [...classes].sort((a, b) => a - b).map((c) => PARTY_CLASSES[c]);
  const rows = [...new Set(names)].map((name) => ['character', name, names.filter((n) => n === name).length]);
  rows.push(['party_size', String(names.length), 1], ['party_composition', names.join('+'), 1],
    ['party_duplicate', new Set(names).size < names.length ? 'yes' : 'no', 1]);
  if (host !== null) rows.push(['host_character', PARTY_CLASSES[host], 1]);
  return rows;
}
