# Discord Rich Presence

While the game runs, Discord shows **Playing Dragon's Crown Pro PC** with text only:

- your character's **class and level**, for example `Elf · Lv 42`, on the first line (the place or `In menus` when no
  character is on screen)
- **where you are**, on the second line: the title screen, the town street, Town Gate, Stables and each building
  (Dragon's Haven Inn, Adventurers Guild, Morgan's Magic Item Shop, Canaan Temple, Lucain's Tower, Castle), the camp, the
  Colosseum, the Labyrinth of Chaos and every stage, by the names the game itself uses
- **Solo**, **Couch co-op**, **Online co-op** or **Online, waiting for players**, with the party size out of 4 (offline:
  controllers on this PC, so a connected pad nobody plays with counts and AI allies do not; online: the room's players)
- elapsed time

Class and level are read from the game's player unit as it updates, the same observe-only reading the online party
statistics use; nothing in the game is changed. The place comes from which area file the game reads out of its CPK
archives: every area has its own map (`round/*.brb`), every town building its own scene (`stage/T_*`) and the town's
street, gate and Stables their own backgrounds (`stage/bg25a`, `bg01a`, `bg41a`), so reading one means the player is
entering it. The log prints each place as it changes (`Discord: place ...`), and `DCP_DISCORD_TRACE=1` logs every
archive read, to find what a part of the game loads. Talismans are not shown: where the game keeps them is not known yet.

The port talks to the Discord desktop app directly over its local connection (a named pipe on Windows, a socket on
Linux, including Flatpak and Snap Discord). Nothing is sent over the Internet by the port; Discord itself shows the
presence to whoever can see your status. If Discord is not running, nothing happens: starting Discord later picks the
presence up within a few seconds, and restarting it within about a minute. A failure here never reaches the game, the
presence just stops being updated.

## Setup

Nothing to do: builds carry the port's official Discord application, **Dragon's Crown Pro PC** (Application ID
`1568342665970458705`, `DCP_DISCORD_CLIENT_ID` in `CMakeLists.txt`), so the presence works as soon as Discord is running.
The application supplies the name shown after **Playing** and its icon from the Developer Portal. The presence sends no
image references, so no Rich Presence art has to exist. To turn the presence off, add to `user\dcp.ini`:

```ini
discord_presence = off
```

To show it under your own Discord application instead, create one at <https://discord.com/developers/applications> and
set `discord_client_id = <Application ID>` in `user\dcp.ini`. An id that is not a Discord application id (17 to 20 digits)
turns the presence off.

The place names are `kPlaces` in `src/discord_core.h`.

## What has been verified

Automated, on every test run (`dcp-discord-tests`, `dcp-runtime-overlay-tests`, `dcp-party-tests`): client id checks, IPC
frames with partial reads and writes, closed, oversized, empty and malformed replies, ping and refusal, the place lookup
by archive and offset, the activity text and that it names no art, level and class storage, and that the file read
observer changes nothing about a read, overlaid or not, and sees only successful ones.

Checked once against local game data: all 28 places exist under the same names in the archives of CUSA10454 1.00,
CUSA07001 1.00, 1.02 and 1.03 and CUSA10209 1.00. CUSA10454 1.01, CUSA10209 1.02 and CUSA10487 1.03 were not available.

Reported by the contributor from play on Windows, CUSA10454 1.00, with an earlier image-based presence under another
application: class, level, title, town, buildings, all nine stages, Lost Zone, Labyrinth of Chaos and camp.

Not verified by anyone yet: the official application and this text-only presence on real Discord, the Prison, any online
state (**Online co-op**, **Online, waiting for players**, the player's own class in a room) and real Discord on Linux,
Flatpak, Snap or Proton. On a build where a place is missing or the player-unit tick is unavailable, the presence stays
generic: that build's archives or log (`Cheats: unavailable`) show why.
