# Discord Rich Presence

While the game runs, Discord shows **Playing Dragon's Crown Pro** with:

- your character's **class and level**, for example `Elf · Lv 42`, with the class icon (`In menus` when no character is
  on screen, such as the title menu)
- **Solo**, **Couch co-op**, **Online co-op** or **Online, waiting for players**, with the party size out of 4 (players
  on this PC offline, AI allies not counted; the room's players online)
- elapsed time

Class and level are read from the game's player unit as it updates, the same observe-only reading the online party
statistics use; nothing in the game is changed. Talismans are not shown: where the game keeps them is not known yet.

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
upload art assets with the keys `icon`, `fighter`, `amazon`, `elf`, `dwarf`, `sorceress` and `wizard` (512×512), and set
`discord_client_id = <Application ID>` in `user\dcp.ini`.
