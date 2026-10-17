# Trophies

Local, offline PlayStation trophies. Nothing connects to PSN, needs a PlayStation account or syncs anywhere.

## Where the data comes from

The trophy list (names, descriptions, grades, hidden flags) and the icons are in `sce_sys/trophy/trophy00.trp` of your own
game package. The launcher and the game unpack it once into `user/trophy/<NP Communication ID>/` (`Icons`, `Xml`); no trophy
content is part of this repository. Both US and Japanese packages use NP Communication ID `NPWR03852_00` and share progress.

The lists inside the pack are encrypted with a Trophy Key. Dragon's Crown Pro PC does not include, generate or download
this key; it stays on this PC and is never sent anywhere. The Trophies page offers:

- **Enter Trophy Key**: type your own key (32 hexadecimal digits, hidden while typing; "Show key" reveals it).
- **Import from shadPS4...**: pick an existing shadPS4 `keys.json` (`TrophyKeySet.ReleaseTrophyKey`) or an old `config.toml`
  (`[Keys]` `TrophyKey`). Only the key is read; nothing else from that file is copied and its location is not remembered.
  On Linux it needs the desktop's file chooser (`zenity` or `kdialog`); without one the page says so and the key can still be typed in.
- **Change...** and **Remove** once a key is configured. The page only shows "Configured", never the value. Replacing a
  configured key asks first.

The key is stored in `user/keys.json` in shadPS4's format, next to anything else that file holds:

    {"TrophyKeySet": {"ReleaseTrophyKey": "<32 hex digits>"}}

Anything other than exactly 32 hexadecimal digits is rejected. Saving or importing a key reloads the list at once. A key
that does not decrypt this game is reported on the page and kept until you change or remove it. Without a valid key the
icons are unpacked but the list is not, and the unlocks the game reports are still saved by id and appear once the key works.
Removing the key blanks `ReleaseTrophyKey` only: trophy progress, the unpacked list and your saves are untouched. The key
is never written to the log.

## Progress

`user/trophy/<NP Communication ID>/progress_user1.txt`: a text file, one `unlocked <id> <unix seconds>` line per trophy,
replaced atomically on every unlock. It is separate from `dcp.ini` and from save data, and reinstalling the game keeps it. An
unreadable file is set aside as `.corrupt`.

## Behaviour

The game's libSceNpTrophy calls (create/register context, unlock, game and trophy info, unlock state) are answered from these
files. The Platinum trophy unlocks automatically with the last of the trophies linked to it (`pid` in `TROPCONF.XML`). The unlock-state count is the number of trophies in the list. Trophy text uses English when the pack has
it. The launcher hides a locked hidden trophy's name and description; the game's own queries return the stored trophy data. There is no in-game notification yet.
