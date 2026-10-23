# Cheats

Eleven optional cheats, in this order: **Infinite Health**, **Infinite Ally Health**, **Infinite Mana**, **Infinite Ally Mana**, **Infinite Lives**, **Infinite Durability**, **Infinite Ally Durability**, **Infinite Arrows**, **Infinite Skill Points**, **Infinite Skill Uses** and **Infinite Gold**. All default to Off. They apply to Player 1, except the three ally cheats, which apply to AI-controlled party allies (Infinite Gold to the whole save, see below). **Movement Speed** (Off, 2x or 4x) and **Experience Multiplier** (Off, 2x, 5x, 10x or 100x) are settings of their own (see below). The F10 menu also has the one-shot **Add 100 Skill Points**, **Add 1,000 Gold** and **Level Up (+1 Level)** actions, which are not saved. The launcher page and the in-game menu are described in [LAUNCHER.md](LAUNCHER.md). They are meant for offline and local play. Nothing switches them off automatically for an online session: the port has no online play and no way to detect one, so `OnlineSessionActive()` in `src/runtime_cheats.cpp` always returns false. It is the one place where such a check would go.

## Architecture

| Part | File |
| --- | --- |
| The cheat list (key, label, description, default), the session layer and the rules the hooks apply | `src/dcp_cheats.h` |
| The menu model (title, rows, hint, note) and the release gate | `src/dcp_cheat_menu.h` |
| The state the menu and the hooks share: the launcher's saved value, overridden for this session | `src/runtime_cheats.cpp` |
| The hooks on the game image | `src/cheat_hooks.cpp`, `src/guest_hook.c` |
| The F10 menu | `gpu/shim/dcp_overlay.cpp`, in the video core's existing overlay |
| The launcher page and the saved settings | `launcher/launcher.cpp`, `launcher/settings.cpp` |

- The launcher stores one `cheat_*` key per cheat in `user\dcp.ini` (the table in [LAUNCHER.md](LAUNCHER.md)) like any other setting: written by Save and Play, put back to Off by Reset all settings. The loader reads them after the launcher closes (`dcp_saved_cheats`) and hands them to `runtime_cheats_start`, which also clears every session override.
- F10 changes the cheats and the two multiplier settings for the running session only. A change is recorded as a difference from the saved value (`SessionCheats`, `SessionChoice`), so a value cycled back follows the launcher again. Nothing in the menu writes the settings file, and the next start uses the saved values.
- Every hook asks `runtime_cheat_enabled` when it runs. A hook of a cheat that is Off returns at once and the game runs its own code.

## The F10 menu

F10 opens and closes it over the game; it lists the eleven cheats with the **Add 100 Skill Points** action right below Infinite Skill Points, **Infinite Skill Uses** next, and **Add 1,000 Gold** and **Level Up (+1 Level)** right below Infinite Gold, then the **Movement Speed** and **Experience Multiplier** rows, and scrolls with the selection if the screen is too short; Up and Down (or the D-pad, or the left stick) move, Enter or A toggles a cheat, moves a multiplier to its next value or runs an action once without closing the menu, F10, Escape or B close. The panel is the display's content scale a step down (0.75x, at least 1.25x), and no more than 45% of the screen's width. It is drawn by the overlay the Insert menu already uses, in the top right corner, with the launcher's gold (`src/dcp_accent.h`).

While it is open the runtime reads neutral input for every pad and the keyboard (`dcpgpu_overlay_captures_input`). Closing starts a release gate: the menu waits until what was pressed in it is released (one second at most), and `runtime_pad.c` keeps hiding any button still held until it is released, so the closing press never reaches the game. F10 does nothing while the Insert menu, a game dialog or the start-up loading screen is shown; the menu does not exist before the game window.

## Native hooks

The game runs as native x86-64 code from the loaded image. `guest_hook.c` writes nothing to `eboot.bin`: before the image is made executable it checks the image against the supported version (size 0x11f8060, entry 0x54c690, the boot image built from the eboot with SHA-256 `f9bff150c4c28507096e7e143a07492063ed89bf05ac6cb995a55a0946e3508a`, which the launcher's game file check also requires), then each site's original bytes, length and overlap. Every hook that passes becomes a `jmp` to a stub placed after the image, which saves all registers, calls the handler, restores them (the handler may change them), runs the replaced instructions and jumps back. A hook whose bytes do not match is skipped on its own with a line in the log (`Cheats: hook 'x' not installed, ...`) and the others still install; a wrong image installs none. The game runs either way.

All offsets are image offsets, the same as the eboot offsets cheat tools use. Every hook returns at once when its cheat is Off, so the game runs its own code.

| Cheat | Site | Replaced bytes | What the handler does |
| --- | --- | --- | --- |
| Health | `0xd8703`, in the damage function `0xd8360` | `89 8d dc fd ff ff` | A positive damage amount of Player 1, or of an AI ally while Infinite Ally Health is On, becomes 0 |
| Mana | `0xd6d70`, entry of `UseMP` | `55 48 89 e5 41 57` | The consume flag becomes 0 for Player 1, or for an AI ally while Infinite Ally Mana is On: the cast works and costs nothing |
| Mana | `0x345380`, entry of `AddMP` | `55 48 89 e5 41 56` | A negative delta of Player 1 or of an AI ally (the sustained drain) returns at once |
| Lives | `0x2ec7cb`, the respawn's life count `lp - 1` | `ff c8 83 f8 ff` | Player 1's count is raised by one before the decrement |
| Durability | `0xfe183`, the endurance event (`DC_BUFFID_COMMON_UPDATE_ENDURANCE`) | `83 bd 04 fd ff ff 00` | A negative delta (wear) on Player 1's items becomes 0, and on an AI ally's items while Infinite Ally Durability is On; repairs are positive and pass |
| Arrows | `0x1b06f0`, entry of `ConsumeArrow` | `55 48 89 e5 41 57` | The amount becomes 0 for Player 1's Elf; the shot still goes out |
| Skill Points | `0x537e8a`, in `BuySkill`, before the points are stored | `66 44 89 ab c8 00 00 00` | The stored points stay at what Player 1 had |
| Skill Uses | `0x1af0dc`, in the skill-item use function, at the count decrement | `e8 6f 13 e7 ff` | The decrement is skipped for Player 1 while the item has uses left |
| Skill Uses | `0x1af12a`, the same function's mirror decrement | `e9 21 13 e7 ff` | The mirror decrement is skipped the same way |
| Skill Uses | `0x225f90`, in the equipment wear function `0x225ce0`, at the count decrement | `e8 bb a4 df ff` | The decrement is skipped for Player 1's skill items while the item has uses left |
| Skill Uses | `0x225fb1`, the same function's mirror decrement | `e8 9a a4 df ff` | The mirror decrement is skipped the same way |
| Skill Points | `0x8ab24`, in the record's skill budget check `0x8aa00`, before it compares owned with allowed | `8b 5d a4 45 01 f4 41 01 dd 45 29 ec` | An excess of owned over allowed is added to the allowance, so the check finds nothing to repair |
| Gold | ten stores `mov [base+0xaf238], reg` (`0x52e0b3` shop, `0x43d562` continue, `0x3bc693`, `0x3bff22`, `0x3c2420`, `0x3c3b82`, `0x3c40da`, `0x4953cd`, `0x49562d`, `0x4cc47a`) | the store itself, 6 to 8 bytes | A store that would lower gold keeps the old value |
| Movement Speed | `0x2ecd0a`, in the pad-driven walk `0x2ec9d0` | `48 8b 5d 80 48 8b 03` | Multiplies the walk's speed multiplier (a float at `[rbp-0x48]`) by 2 or 4 for Player 1 |
| Refill, Add 100 Skill Points, Add 1,000 Gold, Level Up (+1 Level), Arrows | `0x2ea420`, entry of the player unit's per-tick update | `55 48 89 e5 41 57` | Applies a pending Health or Mana refill, Add 100 Skill Points, Add 1,000 Gold and Level Up (+1 Level) to Player 1; holds Player 1's Elf at 10 arrows while Infinite Arrows is On; keeps AI ally health and mana full while their cheats are On |
| Experience | `0x27511`, the return of the experience-gain function `0x27430` | `48 83 c4 08 5b 41 5c` | The gain (rax) is multiplied by 2, 5, 10 or 100 for every award; see Experience Multiplier |

### Infinite Health

`0xd8360` is the unit's receive-damage virtual. Just before it subtracts from HP it holds the signed amount in `ecx` (a loss is positive, a healing hit negative). With the cheat On the hook zeroes a positive amount for Player 1. The HP computation then runs unchanged with nothing to subtract, so the lethal-damage adjuster and the knock-out are not reached. Healing hits, the buff and potion paths that write HP directly, max HP changes, equipment, the HUD and scripted kills that set HP themselves are not touched. Enemies and bosses take their damage as always; AI allies take theirs unless Infinite Ally Health is On.

### Infinite Ally Health

The same damage hook protects AI-controlled party allies. An ally is a player-category unit with a valid player class (0 to 5), a party slot other than Player 1's, and no live human controller on that slot. When switched On, a living ally is filled to its own max HP through the game's fill function on the next player tick, and the tick keeps it full while the cheat remains On; max HP itself is unchanged. Player 1, human-controlled Players 2 to 4, enemies, bosses and non-player NPCs are not affected. Dead allies are not revived or filled, and healing hits are unchanged. Infinite Health and Infinite Ally Health are independent.

### Infinite Mana

Mana is a signed 16-bit value at unit `+0xb8`. The Sorceress and the Wizard share one cast path, `UseMP`: it takes the spell's cost from a table, applies the buffs, checks there is enough and deducts only when its consume flag is 1. With the flag 0 the cast still succeeds and costs nothing, a path the game's own non-player casters already use. The Magic Seal status still blocks casting. A second hook stops the sustained drain (`AddMP(-1)` once per interval of a held state); positive deltas, the recovery of Gather Mana and the buffs that restore MP always go through. Arrows, items, skill charges, cooldowns and the other classes never reach either function. Mana drained by a hit (DAMAGE_MP buffs) is not touched.

### Infinite Ally Mana

The same two mana hooks apply to AI-controlled party allies, using the same classifier as Infinite Ally Health. When switched On, a living ally that genuinely uses mana (Sorceress or Wizard) is filled to its own max MP through the game's fill function on the next player tick, and the tick keeps it full while the cheat remains On; max MP itself is unchanged. Other classes, Player 1, human-controlled Players 2 to 4 and enemies are not affected, and cooldowns are untouched. Infinite Mana and Infinite Ally Mana are independent.

### Refill on Off to On

Turning Infinite Health, Infinite Mana or Infinite Durability On, in the menu or as the launcher's saved value at the start, files one request (`RefillRequests`, `runtime_cheats.cpp`); turning it Off cancels it. The per-tick hook runs on the game thread for the units of players only. When a request is pending and the unit is Player 1's, it takes the request and calls the game's own fill function `0xe3be0(unit, health, mana, only_below_max)` with `only_below_max` set, so nothing is ever lowered. It fills HP to the unit's own maximum (`(int)float[+0x104]`, capped at the game's 999) and MP to `(int)float[+0x108]`, writes nothing else and posts no events. A dead Player 1 (the game's own `0x281ba0` test) leaves the request pending for the revive; a mana request for a class without mana is dropped. Nothing is stored between ticks, so a new character or a recreated player cannot leave a stale pointer. Turning a cheat Off changes no value. Infinite Durability uses the same request; see below.

### Infinite Lives

A life is consumed in one place: the RESPAWN_NOTIFY handler computes `lp - 1` (a signed byte at unit `+0xce`) and posts it as event `0x13f1`. The hook hands that code `lp + 1` for Player 1, so the count stays where it was. Death, the respawn itself and the continue flow are unchanged; at 0 lives the count stays 0 instead of becoming -1 (the calculation floors at -1). What the game does with a count of -1, and so what a death at 0 lives does without the cheat, was not traced and has not been tried in play. Increases (the GET_LP buff, ally revive, continue) are other code. Players 2 to 4 lose lives as always.

### Infinite Durability

Item durability is stored in the item instance as 100 times the displayed value (current at `+0x44`, maximum at `+0x48`). Every wear (weapon swings, hits taken, staff casts) is the buff event `DC_BUFFID_COMMON_UPDATE_ENDURANCE` with a signed delta; the handler that applies it clamps `delta + current` and writes it through one setter. The hook zeroes a negative delta for Player 1's unit; repairs, recovery, maximums, equipment changes and the shop (which writes the item directly) are untouched.

Turning it On also restores Player 1's worn gear once, for free. On the player tick the refill reads the unit's equipment list (`unit+0x1e8`, up to 10 slots of 0x7c bytes, the item at slot+0x10) through the game's own functions (`0x224b40` count, `0x2251b0` item, `0x21d20` is-gear) and, for each item flagged equipped (`item+0x43` bit 6) with a maximum above 0 and a current value below it, calls the game's durability setter `0x225fe0(list, slot, maximum)` with that item's own maximum (`item+0x48`): the function the wear and recovery events end in, so it charges nothing and opens no shop. Empty slots, non-gear, unworn items and items without durability are skipped. The request waits until the unit's list has its owner and bag (`+0x508`, `+0x510`), is taken once, and is cancelled by turning the cheat Off; gear equipped later is not repaired until the cheat is turned Off and On again. Nothing is kept between ticks. The four functions' first bytes are checked when the hooks are installed; if they differ, the refill is unavailable and the wear protection still works. The Health and Mana refill (`0xe3be0`, `0x281ba0`) and Add 1,000 Gold (`0x43d460`, `0x43d490`) are checked the same way and are unavailable on their own if their functions differ.

### Infinite Ally Durability

The same endurance hook protects AI-controlled party allies, using the same classifier as Infinite Ally Health. Wear on an ally's items is suppressed while the cheat is On; repairs and recovery pass, max durability is unchanged, already broken items are not repaired, and the ally's consumable and skill-use counts are untouched. Player 1's gear remains controlled only by Infinite Durability, and human Players 2 to 4 and enemies are not affected.

### Infinite Arrows

The Elf's arrow count is an int at owner `+0x3ad4` (maximum at `+0x3ad8`) and falls in one function, `ConsumeArrow`, reached from the Elf's shot event only (species 8). The hook sets the amount to 0 for Player 1's Elf, so the count is unchanged and the shot goes out. `AddArrows` (pickups) is a different function. The other classes use the same fields for something else and never reach it.

While Infinite Arrows is On, the player tick holds Player 1's Elf at 10: the count is set to 10 at the next tick, so enabling it takes effect at once, and it is set back to 10 whenever it differs, so a pickup does not stay above 10. Turning it Off stops the hold and leaves the count as it is. `ConsumeArrow` also caps the count at the maximum (`+0x3ad8`) after a shot, so the hold of 10 assumes that maximum is at least 10; the Elf's maximum was not checked in play.

### Infinite Skill Points

Skill points are an int16 at `+0x24` of the player's save record (record 0 is Player 1). Only the skill menu's purchase (`BuySkill`) subtracts the cost; earning at the stage result and the refund of Reset skills are other code. The hook writes back the points Player 1 had, for the record and for the unit's mirror at `+0xc8`. The game's own checks still apply (level, prerequisites, maximum level, enough points to afford the skill: with fewer points than the cost the purchase is still refused). The game also re-checks every character record when a save is loaded, when a player is created from a record and when the party returns to town (`0x8aa00`): a record that owns more skill points (bought levels plus points held) than its level allows (`min(level, 98) + 1`, plus the rewards of completed challenge requests) loses its held points first, then its skill levels from the lowest skill id up. While the cheat is On, a second hook at the comparison widens the allowance by the excess, so purchases, `Add 100 Skill Points` and loaded saves keep their skills and points; the rest of that function (the equipped-skill refresh) still runs. The allowance is computed, not stored in the save, so with the cheat Off the next check trims the record as the game always did. The check has no player context, so while the cheat is On it applies to every record the game checks.

### Infinite Skill Uses

Equipped skill items keep a byte count at `+0x4c` of their item record. The count is decremented in one function, `0x20450`, which has four call sites in two paths. The skill-item use path (`0x1af070`) finds an item by effect id and handles only the Elf's two skill effects (`0x1d9`, `0x1da`). Every other skill item, the Dwarf's Bomb Satchel and its two siblings (effects `0x1d6` to `0x1d8`) among them, is counted down by the equipment wear function (`0x225ce0`), which skips those two effects. Four hooks sit on the four decrement calls and skip them for Player 1 when the item still has at least one use; on the wear function's two sites the item must also pass the game's own skill-item test (`0x206b0`: a skill effect `0x1d1` to `0x1f8` as the first effect), so gear and consumables are not touched. The skill action, animation, mana cost and cooldown are unchanged. A skill with 0 uses is not refilled and keeps the game's own empty handling. Players 2 to 4 and enemies are not affected. The recharge to the maximum on returning to town and at stage change (`0x205d0`, `0x1fa40`) is untouched.

### Add 100 Skill Points

A one-shot action of the F10 menu, not a cheat: it has no Off / On, no launcher entry and no saved setting. Each press files one request; the player tick hook runs it on the game thread for Player 1's unit, reads Player 1's save record at `+0x24`, adds 100 and writes the result to the record and the unit's mirror at `+0xc8`. At the int16 maximum (32,767) it stops there and adds nothing, so the value never wraps. Two presses add 200; the action never runs every frame, and requests left when the menu closes are dropped. Infinite Skill Points is unchanged and does not interfere.

### Infinite Gold

Gold is a single value for the whole save (`[saveData+0xaf238]`, at most 99,999,999), not one per player, and most spending paths (item shop, Stable, Inn, Beggar and the rest) compute the new value inline instead of calling a shared function. One hook sits on each of the ten stores and keeps the old value when the new one is lower. Because the pool is shared the cheat applies to every buyer while it is On, which in a local game means Player 2 spends nothing as well; gains, prices and the score are untouched.

### Add 1,000 Gold

A one-shot action of the F10 menu, not a cheat: it has no Off / On, no launcher entry and no saved setting. Each press files one request (`ActionRequests`); the player tick hook runs it on the game thread for Player 1's unit, asking the game for its own `GetGold` (`0x43d460`) and `AddGold` (`0x43d490`) with the object the continue payment uses (`unit->vtable[+0xd0]()` then `+0x88`). `AddGold` clamps to the game's maximum, 99,999,999, and keeps the per-player gold counters; when the gold is within 1,000 of it only what fits is added, and at the maximum the game is not called. It changes the running game's gold, not the save file, and Dragon's Crown saves it as it saves any gold. One physical press is one addition: ImGui's activation fires on the press only, so a held Enter or A does not repeat; requests that were not run when the menu closes are dropped.

### Level Up (+1 Level)

A one-shot action of the F10 menu, not a cheat: it has no launcher entry and no saved setting. Each press files one request; the player tick hook runs it on Player 1's save record. It reads the level and class, asks the game's own level cap function for the current mode's maximum, and does nothing when Player 1 is already there. Otherwise it advances the record and the live unit by exactly one level, moves the XP bar to the start of the new level (carrying any excess XP back to the record's pool), grants the one skill point the game gives per level up to level 99, and recomputes the record and unit base stats through the game's own level table and stat function (`0x82000`). It does not run every frame, and requests left when the menu closes are dropped. Live derived HP/MP catch up when the game next refreshes the character; the save record is already consistent.

### Experience Multiplier

The launcher's Cheats page has an **Experience Multiplier** combo (Off, 2x, 5x, 10x, 100x), saved as `cheat_experience_multiplier` with the values 1, 2, 5, 10 or 100; a missing or other value is Off. It is a setting, not a cheat: F10 moves it to the next value for the session, and the saved value starts each session.

The award is the function at `0x27430`, which computes the experience one player's stage result gains (the score fields and a base score, with a bonus and a rate) and returns it as an int64 in `rax`. The four per-player blocks of the CALCSCORETOEXP case of the server command dispatch add it to the character's total (a 32-bit field at character record `+0xa8`, clamped to the class's maximum after the add), and the result screen calls the same function to show the gain. The hook sits at the function's only return, so the total, the level and the displayed gain all follow the multiplied value. Each call is multiplied once and nothing is kept between calls; a zero or negative gain is left alone, and a product above the int32 maximum stops at that maximum.

The scope is the function's: every player the dispatch case awards (Players 1 to 4 in local play), not only Player 1. Enemy kills and treasure give score, which the stage result converts; that comes from the static reading and the real game test should confirm it.

### Movement Speed

The launcher's Cheats page has a **Movement Speed** combo (Off, 2x, 4x), saved as `cheat_movement_speed` with the values 1, 2 or 4; a missing or other value is Off. The old `cheat_movement_speed_2x` boolean is read only when the new setting is absent, and its On means 2x. It is a setting, not a cheat: F10 moves it to the next value (Off, 2x, 4x, Off) for the session, and the saved value starts each session.

The game turns the pad into a walk message through `0x2ec9d0`, which builds a multiplier from MOVE_UP buffs (1.0 without any) that scales the class speeds and the look-ahead distance, and the chara then steers toward that target through the normal movement and collision code. The hook multiplies that multiplier by 2 or 4 for Player 1, as a MOVE_UP buff of the same size would, and writes no coordinate. Each frame starts from the game's own multiplier, so it is applied once. Walking on the ground in all six classes is covered, and any MOVE_UP buff Player 1 already has is multiplied too. The walk speed is truncated to an integer by the game, so the gain is between 2.0x and about 2.3x at 2x, and between 4.0x and about 4.6x at 4x. The walk animation is not sped up. Jumps, air movement, dashes, backsteps, slides, knockback, scripted moves, the Sorceress's levitation, enemies, NPCs and Players 2 to 4 are not changed. Whether a dash starting from a faster walk gets faster, and how jump air control moves, are not covered and are what the real game test should watch.

### Player 1

A unit belongs to a human player when its owner's category word (owner `+0x2a0c`) has bit 0, the same bit the game's own player test (`owner->vtable[+0xdd8]`) reads, and its player slot (unit `+0xa70`, stored by the CREATE_PLAYER handler at `0x436fb0`, 0 to 3) is 0. Hooks that see the owner (arrows) find the unit through `[owner+0x9b8]` and require it to point back. Hooks in menus (skill points) use the player index of the menu, which is the save record's index. Slot 0 being the player on the first pad rests on the order the runtime logs users in (user 1 is player 1).

An AI party ally has the same player category, a valid player class, a party slot other than Player 1's and no live human controller on that slot. The slot is the same field the game stores for players (unit `+0xa70`); the runtime's live gamepad mapping is what separates an AI slot from a human-controlled one, so a controller joining a slot removes the ally protection immediately. Locally controlled Players 2 to 4 keep their own behavior, and class 255 or non-player-category actors are excluded.

## Tests

`tools\test.ps1` builds and runs `dcp-cheats-tests` (the list, the session override, the menu model, the damage, mana and durability rules, the ally health/mana refill, Player 1 against players 2 to 4, AI allies, human-controller exclusion, enemies and NPCs, the one-shot actions including Level Up (+1 Level), the Movement Speed and Experience Multiplier values including 100x clamping, the skill-use count rule, the handlers on synthetic memory, and the hook layer: it executes real stubs, checks register and flag preservation, stack alignment, the early return and every refusal) and the launcher tests (the page order, persistence, migration and reset). `dcp-cheats-tests --check-image build\image\v1.03\boot.bin` checks the hook table and the level-up functions against the real boot image and installs the hook table on a copy, without a window.

## Not covered

Pits and scripted kills that set HP directly, damage that arrives through the replicated hit packet instead of `0xd8360`, whether slot 0 is always the player on the first pad, jump and air speed, dashes under Movement Speed, and whether any scripted path lowers lives, durability or arrows without the hooked code were not proved statically; they are what the real-game test should watch. For the ally cheats, whether every AI companion the game offers uses the same player-slot model and whether a controller joining or leaving a slot updates the protection immediately are what the real-game test should watch. For Level Up (+1 Level), the live derived HP/MP refresh timing is what the real-game test should watch. For Infinite Skill Uses, which skill items share the two hooked call sites and whether every class's displayed count follows the mirror is what the real-game test should watch; Players 2 to 4 are intentionally outside its scope. For the Experience Multiplier: whether the stage result is the only path that gives a player experience (quest rewards were not checked, and the `UPDATE_THEIF_EXP` message, whose name suggests the thief NPC, is left alone), whether the award runs once per stage in play, and whether the result screen shows the multiplied gain.
