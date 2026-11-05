# Discord Rich Presence

While the game runs, Discord shows **Playing Dragon's Crown Pro** with:

- your character's **class and level**, for example `Elf · Lv 42`, as the large image (the class portrait; the game's
  icon and `In menus` when no character is on screen)
- **where you are**, as the small image and in the status line: the title screen, the town street, Town Gate, Stables
  and each building (Dragon's Haven Inn, Adventurers Guild, Morgan's Magic Item Shop, Canaan Temple, Lucain's Tower,
  Castle), the camp, the Colosseum, the Labyrinth of Chaos and every stage, by the names the game itself uses
- **Solo**, **Couch co-op**, **Online co-op** or **Online, waiting for players**, with the party size out of 4 (players
  on this PC offline, AI allies not counted; the room's players online)
- elapsed time

Class and level are read from the game's player unit as it updates, the same observe-only reading the online party
statistics use; nothing in the game is changed. The place comes from which area file the game reads out of its CPK
archives: every area has its own map (`round/*.brb`), every town building its own scene (`stage/T_*`) and the town's
street, gate and Stables their own backgrounds (`stage/bg25a`, `bg01a`, `bg41a`), so reading one means the player is
entering it. The log prints each place as it changes (`Discord: place ...`), and `DCP_DISCORD_TRACE=1` logs every
archive read, to find what a part of the game loads. Talismans are not shown: where the game keeps them is not known yet.

The port talks to the Discord desktop app directly over its local connection (a named pipe on Windows, a socket on
Linux, including Flatpak and Snap Discord). Nothing is sent over the Internet by the port. If Discord is not running,
nothing happens; starting Discord later picks the presence up within a few seconds.

## Setup

Nothing to do: builds carry the port's Discord application id (`DCP_DISCORD_CLIENT_ID` in `CMakeLists.txt`), so the
presence works as soon as Discord is running. To turn it off, add to `user\dcp.ini`:

```ini
discord_presence = off
```

To show it under your own Discord application instead, create one at <https://discord.com/developers/applications>,
upload 512×512 art assets and set `discord_client_id = <Application ID>` in `user\dcp.ini`. The asset keys:

- `icon`, and the classes: `fighter`, `amazon`, `elf`, `dwarf`, `sorceress`, `wizard`
- places: `menu`, `town`, `gate`, `stables`, `inn`, `guild`, `magicshop`, `lucain`, `temple`, `castle`, `prison`, `camp`,
  `tutorial`, `arena`, `labyrinth`, `ancientlabyrinth`, `castleofthedead`, `sanctuary`, `ghostshipcove`, `lostwoods`,
  `lostzone`, `magetower`, `moldydungeon`, `templeruins`, `oldcapital`

The full list with names is `kPlaces` in `src/discord_presence.cpp`.
