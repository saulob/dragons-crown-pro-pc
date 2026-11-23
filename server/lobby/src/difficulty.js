// The difficulty a multiplayer room is played on: the host's character difficulty, reported once when the room is created.
// Clients send a category name; a raw game value is normalised the same way. Anything else a client may say is unknown.
export const DIFFICULTIES = ['normal', 'hard', 'infernal', 'ultimate'];
export const UNKNOWN_DIFFICULTY = 'unknown';

const RAW = new Map([[0, 'normal'], [1, 'hard'], [2, 'hard'], [3, 'infernal'], [4, 'infernal'], [99, 'ultimate']]);

// A category from DIFFICULTIES or 'unknown'; '' when the client did not report one; null when the value is not a string
// or an integer.
export function difficultyCategory(value) {
  if (value === undefined) return '';
  if (typeof value === 'string') return DIFFICULTIES.includes(value) ? value : UNKNOWN_DIFFICULTY;
  if (Number.isInteger(value)) return RAW.get(value) ?? UNKNOWN_DIFFICULTY;
  return null;
}
