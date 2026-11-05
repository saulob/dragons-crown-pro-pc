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

Discord needs an application id, which names the game in your profile.

1. Create an application named `Dragon's Crown Pro` at <https://discord.com/developers/applications>.
2. Optional: under **Rich Presence → Art Assets**, upload a 512×512 image with the key `icon`, and one per class with
   the keys `fighter`, `amazon`, `elf`, `dwarf`, `sorceress` and `wizard`.
3. Copy the **Application ID** into `user\dcp.ini`:

```ini
discord_client_id = 123456789012345678
```

A release can carry an id instead: configure with `-DDCP_DISCORD_CLIENT_ID=<id>`. `dcp.ini` still overrides it.

To turn it off: `discord_presence = off`.
