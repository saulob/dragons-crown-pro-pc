/* The game's character difficulty byte as a category: shared by the Save Editor, the runtime log and the online room
 * report. 1 and 2 are both Hard, 3 and 4 both Infernal; 99 is Ultimate; any other value is Unknown. */
#ifndef DCP_DIFFICULTY_H
#define DCP_DIFFICULTY_H

#define DCP_DIFFICULTY_ULTIMATE_RAW 99
#define DCP_DIFFICULTY_RECORD_OFFSET 0x21a9 /* the byte in a runtime character record */
#define DCP_RECORD_STATE_OFFSET 0x21e0      /* a dword; -1 when the record holds no character, as the game's setter checks */

typedef enum { DCP_DIFFICULTY_UNKNOWN = 0, DCP_DIFFICULTY_NORMAL, DCP_DIFFICULTY_HARD, DCP_DIFFICULTY_INFERNAL, DCP_DIFFICULTY_ULTIMATE } DcpDifficulty;

static inline DcpDifficulty dcp_difficulty_of(int raw) {
    switch (raw) {
    case 0: return DCP_DIFFICULTY_NORMAL;
    case 1:
    case 2: return DCP_DIFFICULTY_HARD;
    case 3:
    case 4: return DCP_DIFFICULTY_INFERNAL;
    case DCP_DIFFICULTY_ULTIMATE_RAW: return DCP_DIFFICULTY_ULTIMATE;
    default: return DCP_DIFFICULTY_UNKNOWN;
    }
}

static inline const char *dcp_difficulty_name(DcpDifficulty difficulty) {
    switch (difficulty) {
    case DCP_DIFFICULTY_NORMAL: return "Normal";
    case DCP_DIFFICULTY_HARD: return "Hard";
    case DCP_DIFFICULTY_INFERNAL: return "Infernal";
    case DCP_DIFFICULTY_ULTIMATE: return "Ultimate";
    default: return "Unknown";
    }
}

/* The online protocol's room difficulty: the name in lower case, "unknown" for any other value. */
static inline const char *dcp_difficulty_wire(DcpDifficulty difficulty) {
    switch (difficulty) {
    case DCP_DIFFICULTY_NORMAL: return "normal";
    case DCP_DIFFICULTY_HARD: return "hard";
    case DCP_DIFFICULTY_INFERNAL: return "infernal";
    case DCP_DIFFICULTY_ULTIMATE: return "ultimate";
    default: return "unknown";
    }
}

#endif
