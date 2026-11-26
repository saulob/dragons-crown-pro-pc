# Online play

Dragon's Crown Pro's own multiplayer code runs unchanged: the port answers the console services it calls
(NpManager, NetCtl, NpMatching2, NpToolkit2, the P2P socket of libSceNet and libSceRudp). Players meet through a small
lobby; game traffic goes directly between their PCs over UDP, and through the lobby only for a pair of players that
cannot reach each other directly (see Relay). Nothing in the game executable is patched for online play, and with online play off every import keeps its offline behaviour (`src/runtime_services.c`, the bundled
NpToolkit2 module).

Status: two instances on one PC play together through the online service (stages, loot, a player who disconnects
becomes a CPU ally). Play between PCs on different Internet connections (NAT traversal) has not been confirmed yet.

## Playing online

1. In the launcher's **Online** page, turn **Online play** On. The rest of the page appears only then.
2. Choose your **Online name**: what other players see (up to 16 letters, digits, `-` or `_`; `Player` until you change
   it). Changing it later keeps your friend code and your friends.
3. Share **Your friend code** (`XXXX-XXXX`, **Copy** puts it on the clipboard) with the players you want to meet.
4. On the **Friends** tab, enter a friend's code and **Send request**. A request to you appears as a **New friend request**
   card at the top of the page, and the Online entry in the sidebar shows how many are waiting; **Accept** or
   **Decline** it there. The list updates by itself and shows each friend's Online name and whether they are online:
   Online while their launcher or game is connected, "Online, in a room" while they are in a game room. Under the
   name, an online friend also shows the platform and port version their own client reported
   (`Online · Windows · v0.9.0`), and **Incompatible** when their room uses another online protocol.
5. **Connection test** > **Run test**. PLAY waits for one finished test while Online play is On, whatever its result;
   until then the section is outlined in gold with **Connection test required** and a gold Run test button, and pressing
   PLAY opens the Online page at it;
   it is saved (`online_connection_test_port`), so later starts do not ask again. Turning Online play Off, or another
   UDP port (`online_udp_port`, `DCP_UDP_PORT`), asks for a new one. The test binds the game's UDP port (3658, or the
   configured one) and uses that one socket for STUN and the UPnP probe; when the port cannot be opened it says so
   instead of using another. On Windows this gives Windows Firewall the chance to ask for network access before the
   game starts. "Ready for online play" means other players can probably reach you directly, "Ready for online play
   through the relay" that a match will work but not directly, and "direct depends on your network" that the online
   service and the game's UDP see different public addresses (a proxy or VPN); otherwise the lines below it say what is
   missing. The log has the addresses and the endpoint a match would report.
   "Online service: Connected · N ms" is the time the online service took to answer the test's sign-in, measured once
   per test; it is not the Ping to players. "Direct connections" says a UDP endpoint is available; whether a given player
   can really reach it is only known when joining a match. "Relay fallback: Available" appears when the online service said in
   its sign-in answer that it relays (`capabilities.relay_supported`) and the UDP port opened; the test sends no relay
   frame, so it does not prove a path to any player.
6. **Ping display** On shows the in-game Ping from the start; **F3** shows or hides it while playing.
7. Press **Play**.
8. Play through the game's own Network menu: host or join a room, or pick a friend's room in Join Friends Room.

The game offers online play only after a character has passed the opening tutorial and story section, as on the PS4.
The Gameplay page's Story Skip Offer and Tutorial Skip Offer let a new character skip there.

Nothing else needs setting up. With Online play Off the game runs offline as before, and F3 does nothing.

### Early Story Online (experimental)

Off by default. The launcher's Online page has an **Early Story Online** switch (`early_story_online`, applied at the
next game start, only with Online play On; it is not in the in-game Options menu). The switch is for the **host**: with
it On, online play opens as soon as the character reaches controllable Hydeland after Dragon's Haven Inn, whatever its
story step, so the first story stage can be played with friends. The tutorial stays single-player. Off keeps the game's
own unlock at step 45 exactly. A guest does not need the
switch: every current client can join an early story room whatever its own setting is.

- **The game's own online UI.** Nothing new is drawn. Once Hydeland accepts input (the four checks the game's own
  prompt makes), the game's Network Status line and its Network Menu (Begin online; once online, Join a random
  room and Join a Friend's room) are available for the rest of the run, in Hydeland and in the stages, Yes or No. The
  same moment shows the game's own "Go online? Network games will become available.", also for a character loaded
  straight into Hydeland, and without moving or visiting the Town Gate. It is the game's own prompt function and rules,
  run from the town's update: once per run (the game's own flag), so returning from the Inn, the Guild or a shop, or
  walking to the Town Gate, asks nothing more. No stays offline with the menu still there. The access is a runtime
  latch, never saved, and the story step is never changed. If the town does not ask, the Castle Gate and the Stables still do, as
  the game does from step 45. The Stables keep their own story unlock, and the step-45 "Network Menu" story notice
  still waits for step 45. Nothing is saved for this.
- **Network Menu in Hydeland: Y / Triangle.** Hydeland has no character menu this early in the story, so with the
  switch On, Y (Triangle on a PlayStation layout, or the key bound to it) opens the game's own Network Menu there, once
  the prompt above is out of the way, offline or online; Cancel returns to Hydeland. MENU stays the game's: it opens the
  character menu when the game offers one, and Y keeps opening the Network Menu. While the shortcut is armed (Hydeland
  with the switch On, no menu or prompt open) the game does not receive player 1's Triangle. With the switch Off Y is
  the game's own button. While it is armed the game's own top help dialog shows the hint "[Triangle] Open Network Menu"
  (the glyph is the game's logical Triangle button: Y with Xbox prompts, Triangle with PlayStation ones; "Abrir Menu de
  Rede" with Brazilian Portuguese). It is hidden in the Network Menu, while the prompt is open, and everywhere but
  Hydeland. The hint always shows the controller glyph; it does not follow the keyboard. A build that lacks one of the
  dialog's functions keeps the shortcut and has no hint. The Network Status line is not shown.
- **An early host is joinable in Hydeland.** The game only keeps a room for a player at the Castle Gate, the Stables
  and in stages: being online in Hydeland is a flag, and Join a Friend's room finds nobody. With the switch On, an early
  host (below step 45) keeps the game's own room in every town location, as the game does at the Castle Gate: the room is
  created by the game's room machinery as soon as the host is online (Yes on the prompt, or Begin online in the Network
  Menu), carries the story session attribute and stays out of random search, and friends see it in Join a Friend's room.
  A friend's game accepts a host in town only at the Castle Gate or the Stables, so the early host's room info gives the
  Castle Gate as its town location; the friend's own game then leaves its room and joins, as it always does.
  A guest seated in a story room keeps it in town the same way until it leaves. It closes as any room does: End online,
  leaving, a disconnect, the title. A host at step 45 or later keeps the game's own room lifecycle.
- **Capability is not the setting.** A client declares `story_session` when its game hooks are installed, so it can
  join a story room, follow the host's story and keep its own progress, with the switch On or Off. The switch only
  lets that player open online play early and host. A guest below step 45 who has the switch Off still has no Network
  Menu of their own; this does not add an invitation flow.

- **The host owns the story.** A guest whose own story differs from the host's follows the host's story for the shared
  stage scripts and cutscenes while in the room. The guest's saved story step, level, gear, skills, gold, talismans and
  B routes are never rewritten, and the four functions that write campaign progression (story step, difficulty tier,
  talismans, story flags) do nothing for that guest. The guest's in-memory story step is also held at its value from the
  join: the game refills the character record during the session, and the step goes back to it. XP, gold, loot and items
  are ordinary rewards and stay normal. When the guest leaves, disconnects or the room closes, its own story applies again
  at once.
- **One session, one story.** A guest in a story session room follows the host's story for its whole membership, also
  while both are at the same step, through every town facility and stage; only leaving the room (or a disconnect, the
  room closing, End online) gives it its own story back. Two characters past step 45 in a normal room keep today's online
  play.
- **Advanced characters can replay the early story with friends.** An early character (below step 45) can join an
  advanced host only in a Normal difficulty story stage 0-8; Hard, Infernal, the Labyrinth and the Arena are refused.
- **Early story rooms are not public.** A room hosted below step 45 is hidden from random search and reached through
  friends or the join code. The tutorial stays solo.
- **Old versions.** A story room only admits clients that declare the `story_session` capability, and a host refuses to
  create one until the online service says it enforces that. This needs the updated Worker (see below); until it is
  deployed, Early Story Online can join rooms but cannot host early rooms.

Details a test cannot see (story cutscenes with remote players, same-step guests receiving the stage clear, local
co-op players in an online session) are still unproven in the game, so the feature is experimental and its multiplayer
behaviour is not claimed to work yet. `DCP_EARLY_STORY_ONLINE=1` or `0` overrides the saved choice for test instances.
Technically: the lead character record selector returns a copy with the host's step for a visitor, the online check's
threshold is answered by a hook (step 45 as shipped; always once the host's access latch is set, and for a visitor; the
story hint and the Network Menu notice keep the original threshold), the prompt function the Castle Gate and Stables
run is also run on a stand-in owner object from the town object's update, and the room carries a private attribute
(`0x7f01`, the host's step plus one, so a step-0 host counts) plus the hidden flag; every site is located by its bytes and installed all together or not at all, on
each supported build. `0x7f01` is for finding the room and the guest's starting point. While the room lives, its owner
sends each new story step as a private room internal binary attribute (`0x7f02`: format 1, step) through the normal
SetRoomDataInternal path, and members get it as `room.internal`, also with the join answer. The guest's port takes it
out before the game's room callback (an update with nothing else is not delivered), so the game only ever sees its own
`0x57`. The latest live step wins; a replicated host record never overrides it. If the room changes owner, the story
session ends for everyone in it, the room follows the normal-room rule, and the new owner's search attributes drop
`0x7f01`.

### Boss Battle Online (experimental)

With Online Play on, opening Boss Battle in the Inn first runs the game's own "Go online?" prompt (the same prompt and
stand-in owner as Early Story Online, asked once per Boss Battle visit). No keeps the visit offline and Boss Battle
works solo as before; Yes is the game's own online activation. The page's help line shows the game's Network Status
(SystemMsg 1252/1253). While the page is online the Network Menu shortcut is Early Story's: the same pad filter and
the same top help dialog hint ("Open Network Menu" with the logical Triangle glyph, drawn as Y or Triangle by the
controller). One feature owns the shortcut at a time, Early Story over Boss Battle; the page owns it only while it
accepts it (the online prompt, the Network Menu, a start or another window hide it), and takes no input while the menu
is up. The Inn has no player character, so the scene director's Menu loop never reaches its Menu poll there; the page
makes the request the director makes after that loop (the game's menu window for pad 0 and player 1, the call found
from the director's code on every build), and both features make that window the Network Menu the same way. The page
is never closed for it: while the menu is up the page's list is hidden and reads no input (the game's own list flag and
its update), and once the menu is gone the page comes back on the same boss, with the button that closed the menu held
back from it until released. The game ends the Options page when the menu closes, so the page is opened again through
the Inn's own lifecycle; the scene fader covers the Inn menu that lifecycle shows and fades back in once the new page has
run. The Network Menu opened from the page has Join a random room, Join a Friend's room and Close Network Menu: its Return
to town row is not built, and its End online row only reads Close Network Menu (its text is set and marked for layout
again, as the game's own setters do). Confirm on that row is answered by the list as B at the list's Back query, so the
game takes the B path from there on: the same answer, sound and close, no Confirm handling and no list left locked by a
choice. The Return to town row is not built: the game's row helper is skipped, or, where the row is built inline,
the unadded row is released as the game's own list clear releases rows. A list the window kept from before the open was
known, and that still holds Return to town, is cleared and built again by the game's own setup. Every other Network Menu
is the game's own. The host's room is made while the page waits: once the visit is online the Inn asks the game's own room wish, the
one Early Story's town asks, with the Inn's scene (again only after the room was lost). A boss start then reuses that
room, and the room is joinable again when the page returns. With Online Play off the page is unchanged.

- **Setting and capability.** As in Early Story Online, the capability is the installed hooks and the setting only
  decides who hosts. With Boss Battle on, a player enters Boss Battle and hosts a room; with it off the Boss Battle row
  is absent and no room is ever originated. Every player on a build with the hooks declares `boss_battle_session` and
  can join a Boss Battle room by the Network Menu's friend join, whatever the setting: joining a room tagged `0x7f03`
  starts a visitor session that follows the host, shows "Waiting for the host" and ends with the room, leaving the
  saved setting and the Inn menu as they were.

- **Host authority.** The room owner chooses the boss, Fight All and the start. A guest's page shows "Waiting for the
  host" with the host's boss; a guest's confirm starts nothing. The host's game server takes every player to the arena
  and back, as for any online stage; each player's own session sets its own campaign aside and puts it back, merges
  only the progression Boss Battle keeps, saves its own campaign and records its own `boss_battle.dat`.
- **Rooms.** A Boss Battle room carries the private int attribute `0x7f03` (1 taking players, 2 in a fight or at a
  Fight All checkpoint) and its host's state as the private internal binary attribute `0x7f04` (format 1, phase,
  Fight All, boss index, start sequence), taken out before the game's room callback as `0x7f02` is. A host's start
  waits for the lobby's answer to that update, which the lobby sends after the members' copy. Stale sequences are
  ignored.
- **Isolation.** Only clients that declare `boss_battle_session` host or join one, whatever their Boss Battle setting;
  a search or join made from the Boss Battle page (`boss_battle` in the request) finds only open Boss Battle rooms, and
  a random search from anywhere else finds none. A normal room that already has other players is never tagged.
- **Ends.** Leaving Boss Battle clears the tag and tells the members; a change of owner, the host leaving or the room
  closing ends the session for its guests (a guest in a fight leaves it as a Quit from its own game). When the owner
  changes, the lobby removes the tag and the host state, so the next owner has a normal room. A Boss Battle room is no
  story session. Needs the updated Worker; until it is deployed a host refuses to create one.

### Bug reports

The launcher's Bug Report page sends a report only when the player presses Send report, and an optional preview shows exactly
what goes out. A report holds the player's description, the launcher version, the game build, game version and CUSA, the
operating system, the selected log's file name and that log after the privacy filter (`launcher/log_scrubber.cpp`), at
most its last 64 KiB. No personal user data is intentionally included, and the filter cannot promise to catch everything
a log might hold, which is why the page offers the preview.

The filter replaces IPv4 and IPv6 addresses (not loopback), the Windows or Linux account name, the computer name, the
profile folder, the folder the port runs from, other folders outside the system's (the file name stays), network paths,
the online secret, the account id, other players' account ids, room codes, Friend Codes and values labelled as passwords,
tokens or keys, and it drops chat lines. Ordinary diagnostics stay, including hashes of game files and driver versions.
The Online name the player chose is not removed, because a log shows it only for that player's own session.

To send, the launcher signs in with this PC's online account, as the Connection test does, without showing the player
online; a PC that never went online gets its random online identity then. The service uses the account and the
connection's address only to count the report limits: the quota table keeps salted hashes of them for 24 hours, and the
stored report holds neither.

An account may file 2 accepted reports per rolling hour and 10 per rolling 24 hours; a report leaves each window that long
after it was accepted, and only accepted reports count. The Worker alone decides: the launcher keeps no counter, shows no
numbers and leaves Send report available. A refused report is not stored, and the page keeps the description, the selected
log and any open preview. `incident_hourly_limit` shows "Too many reports were sent recently", `incident_daily_limit` shows
"Daily report limit reached"; with both reached the hourly one is answered. The service has further protections of its own,
which answer the plain `limit` code and show the same message as the hourly limit.

The Worker stores the report as received, with the log text itself in its Durable Object database, or refuses it; it filters
and rewrites nothing, so the preview, the request, the stored report, the admin view and the downloaded log are the same text.
Reports are readable only in the private `/admin` (`DCP_DASHBOARD_TOKEN`), tab Bugs, where each log can be viewed, copied or
downloaded as a `.log` file through the same token. The service keeps the newest 500; removing the local log does not remove
a sent report.

### Crash dumps in bug reports

On Windows a fatal exception in the game ends the process after the loader's textual crash report, and a minidump is written
beside the session log with the log's own name: `dragons_crown_pro_433.log` gets `dragons_crown_pro_433.dmp`
(`src/crash_dump.c`). A dedicated thread writes it, because `MiniDumpWriteDump` suspends the other threads, and the crashing thread
waits at most a minute, so a dump that cannot be written never keeps the process from ending; the log says what became of it. A dump is
never overwritten: when the name is taken the next one is `dragons_crown_pro_433_2.dmp`, which the launcher does not offer. A
log number whose `.dmp` still exists is never reused for a new session. The dump holds thread stacks, thread and module lists and
the exception, not the heap, and its comment names only the log file. It can still contain process memory used for diagnostics,
and unlike the log it is not filtered for privacy. Linux writes no minidump: it keeps the textual crash report and sends the log only.

The Bug Report page marks a log **Crash detected** only when its own dump exists, never because the word CRASH is in the text.
The dump belongs to the log when all of these hold: the file is named exactly after the log, in the same folder, and is a
regular file (a link is refused); it was written within 120 seconds of the log; it is 32 bytes to 8 MiB and begins with `MDMP`.
Then **Attach crash report** is on by default for that log, with a note that the dump may contain process memory. There is no file
chooser: only the log's own dump can go. A dump that exists but cannot be sent (too large, not a minidump) turns the box off and
the log is sent alone. The preview lists `Crash report: <name> (<size>, sent as a file, not shown here)` and never the bytes;
changing the box closes the preview.

The dump travels with the same report, never as a second one. `incident.file` carries `dump_file` (the log's name with `.dmp`;
the Worker checks it) and `dump_bytes` (32 to 8 MiB), together or not at all, and its answer adds `dump: "expected"`. The
launcher then sends the file on the same connection:

1. `incident.dump` with `ticket` and `receipt` answers `stored`, `bytes` and `complete`: where the Worker's copy ends.
2. Binary messages follow, each the four bytes `44 49 01 01` ("DI", version 1, chunk) and up to 64 KiB of the file, read from
   disk in chunks and sent in order from `stored`. A first chunk that does not begin with `MDMP` is not stored and
   `incident.dump_end` answers `bad_request`; a frame that is not valid for the announced size, or that this socket did not
   announce, closes the socket with 1008. No report is named in a frame, so a frame cannot reach another report.
3. `incident.dump_end` with `ticket` and `receipt` answers `complete: true` once exactly `dump_bytes` arrived and begin like a
   minidump, else `dump_incomplete` with `stored`; asking again for a finished dump changes nothing.

`incident.dump` and `incident.dump_end` take no `hello`; the receipt is the only proof, like `incident.status`, and a ticket
alone, a wrong receipt or a report that announced no dump answers `not_found` or `bad_request`. The launcher makes up to 3
attempts, each on a new connection when needed and resuming from what the Worker holds. When the report was filed but the
dump did not arrive, the page says so, shows **Retry crash report upload**, and the retry uses the same Report ID and receipt:
it never files again. Neither the receipt nor the dump's contents are logged.

A dump is not a report for the quota. `incident.file` counts as before, and `incident.dump` has a limit of its own per address
(12 burst then 1 per 20 s, `limit` beyond it) that also draws from the lookup budget; chunks cost nothing. The Worker keeps the newest
100 dumps in `incident_dumps` and `incident_dump_chunks` (64 KiB BLOB rows), two additive tables created when the object starts, so
reports from before crash dumps are unchanged and show no dump. An upload that never finishes is dropped a day after its report,
a dump leaves with its report when `/admin` deletes it or the 500-report limit drops it, and `/admin` → Bugs shows **Crash report**:
`Stored` with name and size, `Announced, upload not finished`, or `No crash report`, independent of the report's status and
player feedback. **Download minidump** appears only for a stored dump and is served through the dashboard token as the report's
`<Report ID>-<name>.dmp`, with no public link.

### Report status and My reports

A report's life: the player sends it and it starts as **New**; the developer sets a status in `/admin` → Bugs, with an optional
fixed version and a short message; the launcher checks later and My reports shows what changed.

| Status | Machine value | Meaning |
| --- | --- | --- |
| New | `new` | Not reviewed yet |
| Investigating | `investigating` | Being investigated |
| Awaiting player test | `awaiting_player_test` | A possible fix is ready: the launcher asks the player whether it worked (see Player test feedback) |
| Fixed | `fixed` | The problem has been corrected; the only status that says so, and the only one with a fixed version |
| Cannot reproduce | `cannot_reproduce` | Could not be reproduced with the available information |
| Duplicate | `duplicate` | The same problem is already tracked by another report |
| Not a bug | `not_a_bug` | The behavior is expected, or not a Dragon's Crown Pro bug |
| Closed | `closed` | Intentionally finished without a specific fix or a more precise status. It does not mean Fixed |

Who owns a report is known only to the PC that sent it. An accepted report answers with a **receipt**: 256 random bits, a
capability for that one report, derived from nothing. The launcher keeps it in `user/bug_reports.json`, never in `dcp.ini` and
never in a log. The Worker stores only a salted SHA-256 of it. There is no account or address in the status table, no query by
account or address, and no list of one player's reports; the temporary quota hashes above stay a separate matter.

`incident.status` and `incident.read` take no `hello`, so a lookup never involves the account and Online Play can stay off. They
accept only a ticket with its receipt: a report ID alone reads nothing. A wrong receipt, an unknown ticket and a report from
before receipts all answer alike: left out of the list, or `not_found`. Lookups have their own limits, write nothing and
cost no report quota. Seen and unseen are local: the service never learns whether a player read a status.

A developer message is a message the admin typed with the status; an empty one is not a reply. The launcher shows a message that
is new or changed as a **new reply** and any other change as an **update**, and a reply is announced before a plain update. The
admin can also **Delete report** in `/admin` → Bugs: it removes the report, its log, its status and message, and the receipt hash,
so the owner's lookups no longer find it. The service does not say a report was deleted: it simply stops answering for it, like
an unknown ticket. A launcher whose check was answered without a report it asked about marks that report **No longer
available**, keeps its last known status and message, stops asking about it on its own (a manual **Refresh** asks again) and
never shows it as an update.

My reports belongs to one installation. It is not an account feature: another PC, or a removed file, sees none of these reports.
Reports from before receipts have none and cannot be claimed, so they stay out of My reports; they still have a status in
`/admin`. The local list keeps the newest 50 reports, and **Remove from this PC** deletes only the local entry and its receipt.

Logs and statuses have separate limits. `incidents` keeps the newest 500 reports with their logs; `incident_tracking` keeps the
newest 5000 statuses, a row of at most about 2 KB, so at most about 11 MB. A status therefore outlives its log: once the log is
gone, **What was sent** says the report is no longer available, and the status still updates. Past both limits the launcher keeps
showing the last status it knew.

### Player test feedback

**Awaiting player test** is a status the developer sets with a message for the player. It does not replace **Fixed**, which stays
the developer's final status with its fixed version, and the player's answer never changes the status.

While a report is Awaiting player test, My reports shows the message and **Did this fix the issue?** with **Yes, fixed** and
**No, still happening**. The answer is a separate, small state of the open test request: `pending` (no answer), `fixed` or
`not_fixed`, with the time it was given and the launcher version and game build it was given from. `incident.status` returns
`test_feedback` (and `test_feedback_at` once answered) only on an Awaiting player test report; an older service that leaves it out
reads as no answer.

A request is identified by its `status_updated_at`, which the launcher sends back as `revision` in `incident.feedback`. The
answer is taken only if that is still the report's open request: after any newer change (a new request, another status) the
service answers `stale_request` and keeps the newer request untouched. Saving an Awaiting player test with a changed message, or
returning to it from another status, is a new request and reads as `pending` again; an unchanged save keeps both time and answer.
The first answer to a request is final: the same answer again is accepted and changes nothing, a different one is
`already_answered`. A `bad_request` answer carries `detail`, the name of the refused field and never a value; an older service
that does not know the operation answers `bad_request` without one.

`incident.feedback` takes `ticket`, `receipt`, `revision`, `result` (`fixed` or `not_fixed`) and optional `launcher_version` and
`game_build`, nothing else and no free text. It needs no `hello`, and like `incident.status` it proves ownership only with the
ticket's receipt: a ticket alone, a wrong receipt and an unknown ticket all fail alike (`bad_request` for a malformed request,
`not_found` otherwise). It has its own per-address limit, separate from report quota. `incident_tracking` gains five additive
columns for the answer; existing rows are untouched and read as no answer, and an older answer stays stored when a new request
makes it `pending`.

The launcher sends the answer off the UI thread and shows the answer only once the service has it, so a failure keeps the buttons
for a retry. After **Yes, fixed** it shows "You confirmed that this issue is fixed". After **No, still happening** it says the
issue is still happening and asks the player to send a new Bug Report for fresh logs, with a **Send a new report** button that
scrolls to the form. It never sends a report or attaches logs by itself. The launcher log shows each answer as
`Bug report feedback: ticket=… result=… revision=…`, then `accepted`, or `failed` with `stage=connect|call`, the lobby's or the
transport's `code` and the `detail`; never the receipt. The answer is kept in `bug_reports.json`
(`test_feedback`, `test_feedback_revision`), so a restart does not offer it again; a newer request does.

In `/admin` → Bugs the **Player feedback** column sits beside Status and reads **Awaiting response**, **Player confirmed fixed**,
**Still happening**, or a dash when no test request is open. The **Player feedback** filter (All, Awaiting response, Confirmed
fixed, Still happening) uses the same value, which the Worker computes once per report. The detail has a Player feedback section
with the same label and, once answered, the response time and the launcher version and game build it came from. The answer
shows only while the report is Awaiting player test; the **Bugs** tab shows a count of the reports whose status is New, read when
the page signs in and each time the Bugs list is refreshed. Neither Investigating reports nor player feedback count.

### Privacy and usage statistics

The online service stores anonymous aggregate usage statistics: player counts, sessions, rooms created and joined,
how many players each room reached and how long it lasted, how many rooms needed the relay,
the Dragon's Crown Pro PC version, the platform (Windows, Linux, Steam Deck, Android) and the country. These statistics do not
store player names, Online names, Friend Codes, account IDs, IP addresses, room codes or friend lists, and there is no
per-player or per-session record, except the Early Story Online record described below. The same holds for the party
statistics below.

Android clients use this same lobby and the same `online_protocol`, so Android and PC players can share rooms. The `platform`
a client reports is informational: it never decides who can join, and it is the only device information the service sees (no
model, GPU or Android version). Android sessions were counted as Other before platform tracking learned the name; those rows
are not rewritten, and only sessions after the Worker was updated count as Android. Multiplayer rooms that end after the update
also count their platform mix: cross-platform rooms, Android rooms, Android cross-platform and PC and Android rooms, and
Android rooms that **used relay**, meaning the relay carried at least one frame of the room (a room can also have been direct).

- The country is worked out by Cloudflare from your connection and kept only as a count per day (for example "Brazil:
  12 sessions"). The service never keeps your city, region or address for these statistics, and the game never sends a
  country.
- A session is a sign-in of the launcher or the game to the online service that has no counted sign-in of the same
  account in the previous 10 minutes, so a quick reconnect is not counted again and sessions are not a number of players.
  The only count of players is the total number of registered accounts, read from the account table; there is no
  per-day or per-player count.
- The one exception is Early Story Online. The online service keeps, for each account that hosted or joined an Early Story
  Online room, its online name, its account id, how many such rooms it took part in as host and as guest, and when it last
  did. It exists so the maintainer can see who uses the feature; it holds no address, endpoint, room or join code, only the
  maintainer sees it on the same private page, and merely turning the option on records nothing. The game sends nothing
  extra for it: it comes from the room requests the game already makes.
- Gameplay traffic still goes directly between the players' PCs (P2P), except for a pair that cannot connect directly: the lobby forwards that pair's packets unread and unstored. Saves, character names, levels, equipment, gold,
  progress, quests, Ping and FPS are not collected; a sign-in's save integrity (Save integrity, below) is kept only as a
  count per day for each of clean, cheat, editor, both and unknown. Apart from the optional party snapshot below and
  the host character's difficulty in the request that creates a room, kept only as a count per day, the game sends nothing
  extra for these statistics: they come from the sign-in and room requests it already makes.
- A few totals are public (`GET /stats`, server/lobby/README.md): players online, in rooms and in the Lobby, open rooms, the
  day's and the last 30 days' rooms, joins and sessions, the classes in reported parties, and the sessions and accounts
  since the start. The launcher's Home page reads them and GitHub's download count, also while Online Play is off, without
  an account or identifier. Everything else is seen only by the maintainer, on a private, password-protected page.

The online service itself still needs your account, Online name, friends and, while you are in a room, your address and
port to connect you to other players; that is described below and is not part of these statistics.

### Party statistics

The lobby can count which character classes appear in online parties. This is optional on both sides: a game build
sends nothing unless it implements it, a lobby that does not know the request answers `bad_request`, and a build that
never sends it signs in, hosts, joins, uses P2P and the relay and counts in every other statistic exactly as before.
Builds up to 0.9.1 do not send it and keep working unchanged; the lobby accepts it before any build does, so the
service can be updated first. Releases up to 0.9.1 never send it; the first build that does is the one that adds the
observer described below.

- When it is sent: only while Online Play is on and the game is signed in to the lobby, by the owner of a room that
  holds two or more players (a room of one is every online player's idle state, so it is not a party), once every
  player of the room has a live unit and the party has been the same for 3 seconds. The same party is not sent again in
  that room; a changed party is sent once it is stable, at most once a minute. Offline play, Online Play off, a missing
  link or an unrecognised game executable send nothing, and a refused or lost report changes nothing in the game.
- Where the classes come from: an observer at the entry of the game's per-tick update of player units, installed only
  while the game is online and found by signature in every supported build (`dcp_resolve_party`, the same site the
  Cheats hook, which stay off online). It reads each human player unit's slot and class and writes nothing. The slot of a
  room member is the game's own `(member id >> 4) - 1`; a report is made only when every member of the room has a unit
  in its slot, so computer-controlled allies are not part of it and a layout that does not fit sends nothing.
- The host's class is not sent: the report carries class numbers only.

- What is counted: one snapshot of the party, sent by the room's host on the lobby connection the game already has
  (no other connection, address or request exists for it): the class of each party member (Fighter, Amazon, Elf,
  Dwarf, Sorceress or Wizard; a class may repeat) and, when known, the host's class. The client sends class numbers
  only, never text, and the lobby derives everything it keeps from them.
- What is kept: daily counts of how often each class appeared, of host classes, of party sizes (1 to 4), of the
  party composition (the classes of a party in a fixed order, such as `Fighter+Elf`) and of parties with a repeated
  class. At most 227 different counts exist per day, whatever is sent. No player name, Online name, character name,
  Friend Code, account, address, room, session, installation or machine identifier, level, equipment, progress or
  free text is stored with them, and there is no per-player, per-account or per-room history or event log.
- How it is limited: the request passes the same sign-in and limits as every lobby request, then has its own budget per
  lobby, per account and per address, separate from creating and joining rooms; an identical repeat from the same
  connection is not counted twice. A client that reconnects can be counted twice for one party, which is accepted
  rather than keeping an identifier to prevent it.
- The transport is the authenticated lobby connection, so the service sees the connection's account and address while
  it handles the request, as it does for every request; the statistics it retains are aggregate counts without them.
  Only the maintainer can see the counts, on the private dashboard ("Characters & parties").
- Statistics begin when builds that report parties are used. Earlier rooms have no character figures, and the dashboard
  shows "No data yet" for a range without a report rather than zero use.
- Offline play and Online play off send nothing. If the lobby is unreachable or refuses the request, play continues:
  the snapshot is only a best-effort report.

### Comms Wheel

A radial wheel of eight fixed cooperative messages, for online rooms of two or more players. It is not text chat: nothing
is typed and no text travels. Clockwise from the top: Ready, Hello, Need help, Let's go, Group up, Follow me, Good job,
Thanks. The message is shown through the game's own greeting balloon of the sender's player, so its place, font,
duration and Player 1 to 4 colour are the game's; only the text it carries is replaced. Each client looks the message id up
in its own Menu & Subtitles language (`src/comms_text.c`, the same ten languages as Boss Battle), so an English player
reads "Need help" while a Brazilian Portuguese player reads "Preciso de ajuda".

- **Controller.** Press the configured Controller Comms Wheel Shortcut (**View / Back + D-pad Up** by default; configurable
  in Controls > Controller shortcuts to `View / Back + D-pad Up`, `L1 + D-pad Up`, `R1 + D-pad Up`, legacy `D-pad Up`, or
  `Off`) once: the wheel opens and stays open. Bare D-pad directions pass to the game unchanged unless `D-pad Up` is
  explicitly selected. Move the right stick to pick a sector (the pick stays when the stick returns to the centre), press
  Cross (A) to send and close, or Circle (B) to close without sending. While the wheel is open the game sees no opener
  buttons, D-pad Up, right stick, Cross or Circle.
- **Keyboard and mouse.** Press the Comms Wheel key (T by default, rebindable in Controls) once: the wheel opens and stays
  open. Pick with the mouse or with the direction keys (the left and right stick keys, WASD and the arrows by default),
  send with the left mouse button or the Cross key, cancel with Escape or the right mouse button. While the wheel is open
  the mouse buttons, mouse motion, Escape and the direction keys do not reach the game.
- **When it opens.** Online Play on, a room of at least two players, and this player's unit running in the game.
- **Limits.** One message every 2 s from a player; a receiver accepts one every 1.5 s per sender slot.
- **Network.** Peer datagram type 9 (see Peer transport). Builds without the Comms Wheel ignore it and show nothing.
- **Native hook.** One hook at the text call inside the game's greeting balloon function, found by its bytes on every
  supported build (`src/comms_bridge.cpp`); the balloon is shown from the player unit's own per-tick update, the same
  site as the party snapshot. Located on the six builds with a local executable (the release matrix of
  `tools/test.ps1`); Asia/Hong Kong 1.02 and Europe 1.03 rely on the same structural search and are unproven.

### Comms Wheel statistics

The Comms Wheel (eight fixed cooperative messages) reports how often each message is sent, so the private dashboard can show
which ones are used. A client sends `comms.report` with eight counts, one per stable message id (0 Hello, 1 Thanks, 2 Good
job, 3 Follow me, 4 Group up, 5 Need help, 6 Ready, 7 Let's go), at most once every two minutes and not while it has nothing to
add. The lobby keeps one total per id and nothing else: no text (the messages are ids, never words), no player, account,
address, room, time of use or history. The dashboard's "Comms Wheel" section shows Total Uses, each message with its count
and share, and the most used message. Offline play, Online play off and a lobby that predates the request send or lose nothing
that matters: an older lobby answers `bad_request` and the client drops the counts.

### Custom appearance online

Each player's Editor look (character_appearance.dat) is drawn on every client of the room. Only a recipe travels: the
class, the colour preset the character is drawn with and the parts it changes, each by a stable network id with an
optional colour and a hidden bit. Every client builds the textures from its own game files with the same code as a saved
look (`launcher/appearance_runtime.cpp`), so builds that share the character files draw the same look. No texture, save
or file crosses the network.

- **Recipe, schema 1** (`src/appearance_net.h`): `[schema][class 0-5][preset 0-4][n]` then `n` parts `[id][flags]`
  (bit 0 hidden, bit 1 coloured) with `[r g b]` when coloured; ids per class are fixed in `PartTable`, labels are not ids.
  At most 16 parts, 84 bytes; a Fighter with four changed parts is 21 bytes. Unknown schema, class, preset or part,
  duplicates, bad flags, truncation or trailing bytes: the player is drawn native.
- **Lobby.** A client that declares `appearance` in hello publishes its look with `room.appearance` once its seat exists,
  and again only when the look the game draws for it changes (`null` = native). The lobby keeps it with the seat, answers
  create, join and resume with the other members' looks (`appearances`), relays each change as `room.appearance` to the
  members that declared `appearance`, and forgets it with the seat. It reads only the envelope (schema, hex size) and
  never logs or counts the look. Clients that did not declare `appearance` get nothing and are drawn native by the others.
- **Drawing.** A unit in another player's seat (seat owner 1) wears the look of the member whose Matching2 member id maps
  to that party slot, while its class and preset match the recipe; it never wears this client's saved looks. Textures are
  reserved at launch per class and preset (3 each, `PLAYER/<CLASS>0N_NETk.FTX` and its default weapons) and filled off
  the game thread; a look built after the game created the unit shows from the next time the unit is created. Default
  weapons follow the saved-look rule: replaced only while the game would draw the default one. With Online Play on, the
  Fighter model with helmet copies is always served so another player's hidden helmet draws as it does locally.

### Save integrity

A save container (a title id and its savedata directory, such as `CUSA10454` / `SAVEDATA00`, the whole SAVE0.DAT with all
of its DATA slots) carries one integrity bitmask, `integrity_flags`, so a shared online service can ask for clean saves:

| Value | Meaning |
|---|---|
| `0` | Clean: the official client recorded neither bit for this save |
| bit 0 (`1`) | Gameplay cheats took effect while this save was the mounted campaign save |
| bit 1 (`2`) | A successful Editor Apply persisted a gameplay change to this save |
| `3` | Both |

Any nonzero value means the save is not eligible for a clean shared environment. A bit is never cleared: backups,
restores, exports, imports (PS4, PS3), restarts and turning cheats Off keep it, and a clean save imported over a marked
`SAVEDATA00` keeps that container's mark. There is no reset.

- **File.** `user/online_integrity.dat`, written by the launcher and the game, never inside SAVE0.DAT: magic `DCPI`
  (little-endian `0x49504344`), u32 version `1`, u32 record count, then per record, sorted by title id and directory,
  `[u8 n][title id]`, `[u8 n][directory]` and a u32 bitmask with only bits 0 and 1 and never 0. Every write re-reads the
  file, adds its bits and replaces it atomically. A file that cannot be read, is malformed or has another version is never
  overwritten, and its state is unknown, never clean; a later format bumps the version.
- **Editor.** Apply records bit 1 before anything is written, and only when the persisted bytes change outside a present
  character's name, its reading copy and its native colour (`IsGameplayEdit`): level, skill points, HP, the six stats,
  difficulty, Gold, every Inventory Editor change and any later field count. Names, native colours and custom appearances
  (colours, hidden parts, randomize, reset, online sync; character_appearance.dat) never do. Opening the Editor, pending,
  discarded or failed changes record nothing: a failed Apply puts the previous file back.
- **Cheats.** Bit 0 is recorded the first time a cheat, a multiplier (Experience Multiplier,
  Damage Multiplier, Damage Taken, Movement Speed) or an F10 action (Add Skill Points, Add Gold, Level Up, the health actions) actually changes
  a game value, for the campaign save last mounted. A cheat that is On but never changes anything, or one turned On and Off
  again, records nothing. Nothing is recorded before a campaign save was mounted, or while Boss Battle uses its isolated
  copy (`boss_battle/savedata`). Online Play still turns every cheat off for the whole process.
- **Earlier use.** Past cheat use cannot be reconstructed and is not guessed. Past Editor use is recorded only where
  character_overrides.dat holds a stat override for a character the save still has (same slot, class and identity);
  past Inventory Editor changes cannot be told from values the game makes and are not guessed.
- **Hello.** The game sends `integrity_flags` for the campaign save it last mounted. Before one is mounted, and from the
  launcher, it is the union of every container of the installed title. A state that cannot be read is left out. A hello
  without the field is unknown, never clean.
- **Lobby.** It accepts only an integer 0 to 3 (`bad_request`, `integrity_flags` otherwise), keeps it on the connection
  and tells no other player. Its hello answer carries `capabilities.save_integrity: true` and
  `capabilities.clean_online_required`. With clean online required, a `presence: true` hello is refused with
  `save_integrity_marked` for 1, 2 or 3 and `save_integrity_unknown` without the field; `presence: false` (the Connection
  test) still signs in and reads the capabilities.

This is a signal from official clients, not anti-cheat: the client is open source and can be changed to send anything.

### Ping

The in-game Ping is the round trip to the other players over their P2P links (direct or relayed): the smoothed round
trip the peer transport already measures from its acknowledgements and its once-a-second PING (see Peer transport),
read about three times a second. The panel adds no packets of its own and makes no request to the online service. A
link's close line and its quality lines in the log print the same value as `srtt`. With several players it shows the highest round trip among
the connected ones, the worst connection in the room. "Ping: --" means no player is connected yet, or the last one
left. The panel uses the frame counter's style and corner, next to it when both are shown.

### System type and versions

The game's player cards (as in Join Friends Room) show **System type** as the player's PC platform and port
version, for example `Windows · v0.9.0`, `Linux · v0.9.0` or `Steam Deck · v0.9.0`, instead of PlayStation 4.
Steam Deck is recognised from the machine's DMI identity (Valve Jupiter or Galileo); any other Linux PC shows Linux. The
version is the CMake project version, the same one the launcher and the packages show.

Who can play together is decided by an internal online protocol number (`net::kOnlineProtocolVersion`,
`src/net/online_version.h`), not by the port version: v0.8.1 and v0.8.2 still play together as long as the number did
not change. It is raised only when a change stops a build from playing with the previous one. Rooms of another
protocol are left out of searches and of Join Friends Room, and joining one is refused by the online service before
any connection to the other player is made; the game shows "Online version mismatch" with both versions (or "This
player is using an incompatible version of Dragon's Crown Pro PC." when the other build is too old to say). Such
friends still appear in Friends, online and in a room. A build from before this check sends no protocol and counts as
0: it never meets a current build, only other builds like it.

The card's System type comes from a small hook (`src/system_type_hook.c`, every supported build): every copy of
the game marks its own player info as PS4, so the text cannot come from the data the players exchange. At the card's
two calls that set the PS4 record, the hook sets the text for the Online ID the card shows, from what the online service
reported for that player (`PC` when it reported nothing).

## Reference and acknowledgement

[OpenCrown-PC](https://github.com/Nezara/OpenCrown-PC) by Nezara was an important technical reference during
development of Online Co-op.

Its research documents how Dragon's Crown Pro interacts with Matching2, RUDP, room attributes, member IDs and other
online interfaces. Struct offsets and similar ABI details were cross-checked against the calls the game makes.

OpenCrown's own lobby protocol documentation (`notes/map/lobby_protocol.md`) also influenced parts of this project's
lobby design, including reconnect and seat-grace behavior, event buffering, service limits, rate limits and related
operational choices.

This project's client, lobby protocol, server implementation, P2P transport and supporting code were written
separately for Dragon's Crown Pro PC, and no OpenCrown source code was copied.

Thanks to Nezara for publishing the research and implementation details that helped make the game's online behavior
easier to understand and reproduce.

## Pieces

| Part | Files | Role |
|---|---|---|
| Game services | `src/runtime_online.cpp` | settings, import table, NpManager (signed in, PS Plus), NetCtl (connected), NpToolkit2 (friends, presence), sign-in and profile dialogs, word filter |
| Matching2 | `src/runtime_matching2.cpp` | the game's room requests and callbacks over the lobby; endpoint report and peer introduction |
| P2P socket and Rudp | `src/runtime_rudp.cpp` | `sceNetSocket` type 6 ("RUDP_NetId"), `sceNetBind`, Rudp contexts, polls, reads and writes over `net::PeerSocket` |
| Network library | `src/net/` | JSON, identity, WebSocket client (WinHTTP; sockets and OpenSSL on Linux), lobby link, STUN, UPnP, peer transport, connection test |
| Lobby | `server/lobby/` | one JavaScript lobby for the Cloudflare Worker and for the local development server |
| Launcher | `launcher/online_page.cpp`, `launcher/online_view.cpp`, `launcher/online_settings.cpp` | Online page: online play, then the Settings (online name, friend code, connection test, ping), Friends and Lobby tabs |

Online play is available for every supported build. Their online imports are the same entry points except that
CUSA07001 1.00 and CUSA10209 1.00 call `ActivityFeed::getPlayedWith` (answered like the other Toolkit2 calls) where the
later builds call the TUS library. Before the imports are bound, the game checks that every import of an NP, network
or Rudp library that the game itself binds has an implementation (NpToolkit2 ones in this online layer, since the
bundled module is not set up for online play), and stays offline, logging each missing import, otherwise. Title user
storage is not provided: every TUS call, synchronous or not, answers signed out. Every build sends one lobby game
key (`net::kOnlineGame`), so players of different regions and versions meet; only the online protocol keeps them apart.
The System type, Story Skip Offer and Tutorial Skip Offer sites are verified for each local build
(`tests/system_type_tests.c`, `tests/skip_offers_tests.c`, `tests/online_builds_tests.cpp`); a supported build without
a known site (CUSA10454 1.01, CUSA10209 1.02, CUSA10487 1.03) uses the single place its image has the same verified code, and is left
unchanged when there is none.

## Settings

`dcp.ini` (Online page) and environment overrides for test instances:

| Key | Variable | Default | Meaning |
|---|---|---|---|
| `online_enabled` | `DCP_ONLINE` | 0 | online play on |
| `online_name` | `DCP_ONLINE_NAME` | `Player` | in-game online id (16 of `A-Z a-z 0-9 - _`) |
| `online_udp_port` | `DCP_UDP_PORT` | 0 | 0: the game's port 3658, or any free one when it is taken |
| `online_stun` | `DCP_STUN` | `stun.cloudflare.com:3478,stun.l.google.com:19302` | STUN servers; `off` disables |
| `online_connection_test_port` | - | 0 | the UDP port of the last finished Connection test; 0 after Online play is turned off |
| `show_in_lobby` | - | 0 | list the Online name in the public Lobby; Reset turns it off |
| `early_story_online` | `DCP_EARLY_STORY_ONLINE` | 0 | experimental, for the host: online once Hydeland is reachable, whatever the story step, and early story rooms; guests do not need it (needs Online play on; restart) |
| - | `DCP_LOBBY` | `net::kDefaultLobbyUrl` | developer override of the production lobby (`ws://` or `wss://`), for diagnostics |
| - | `DCP_UPNP` | 1 | ask the router to map the UDP port; `0` for diagnostics |
| - | `DCP_JOIN_CODE` | - | the game's room search returns only the room with this code |

The lobby address and UPnP are not settings of the Online page or of `dcp.ini`: players always use the production
lobby, and UPnP is tried automatically next to STUN and hole punching; a router without UPnP does not stop online play.

The online identity is `<user folder>/online_identity.ini`: a random 63-bit account id and a 32-byte secret, made on
first use, never overwritten. The lobby stores only the secret's SHA-256 and refuses a known account with another
secret. The secret is never logged.

## How the game goes online

1. `sceNpCheckNpAvailabilityA`, `sceNpGetState` (signed in), `sceNpGetParentalControlInfoA`, `sceNpCheckPlus` (Plus).
2. `sceNpMatching2CreateContextA`, `ContextStart`: the lobby link opens and sends `hello`; the context callback gets
   0x6f02 when it is answered (with an error when the lobby is unreachable: the game stays offline).
3. `GetServerId`, `GetWorldInfoList` (one world, id 1), `SetUserInfo` (answered locally).
4. Every online player hosts a room first: `CreateJoinRoomA` (4 slots, flags 0x86000000, int attrs 0x4c..0x53, the
   0x78-byte room info as bin attrs 0x57 internal and 0x55 external). "Join Random Room" is `SearchRoom` with the
   game's own conditions (0x4c, 0x4d, packed 0x53), then `LeaveRoom` of its own room and `JoinRoomA`. A room is found by
   a random search only while its host is inside a quest stage (attr 0x4d <= 8); a host in town is found by friends
   (Toolkit2 `getFriends` rows) or by `DCP_JOIN_CODE`.
5. After the create/join answer the game opens its P2P socket (`sceNetSocket("RUDP_NetId", 2, 6)`, `sceNetBind` port
   3658, vport 4649) and registers the room and signaling callbacks.
6. The port reports the socket's endpoint (`udp`), receives each member's (`peer`) and raises signaling 0x5102 for a
   member once its endpoint is known and has not changed for 0.5 s. The game reads it with
   `SignalingGetConnectionStatus` and starts `sceRudpInitiate`; both ends do, at the same time.
7. Rudp contexts map to peer links: events 1 (readable), 2 (connected and writable), 8 (closed); one whole message per
   Read/Write, up to 0x40000 bytes.

Matching2 callbacks run only from `sceNpCheckCallback` (once per frame on a game thread), never inside a Matching2 call
and never with a port lock held: the game holds its own lock while calling Matching2 and takes it in its callbacks.

Member ids are 0x10, 0x20, 0x30, 0x40: the game computes `slot = (memberId >> 4) - 1`.

## Lobby protocol (version 1)

One WebSocket per process, text frames, one JSON object each. The literal text `ping` is answered `pong` (the client
sends it every 20 s; 45 s without any frame closes the socket). 64-bit ids are decimal strings, binary attributes
lower-case hex.

- Request `{"op": "...", "seq": n, ...}`; answer `{"re": n, "code": "ok" | <error>, ...}`; event `{"ev": "...", ...}`.
- Errors: `bad_request`, `too_large`, `no_session` (no hello yet), `proto`, `unauthorized`, `not_owner`, `in_room`,
  `not_found`, `closed`, `full`, `incompatible`, `limit`, `incident_hourly_limit`, `incident_daily_limit`,
  `save_integrity_unknown`, `save_integrity_marked` (hello, clean online required; see Save integrity), `internal`. The port maps `not_found`/`closed`/`full` to the three Matching2
  errors the game tells apart (0x80550d0f / 0x80550d15 / 0x80550d19) and the rest to 0x80550ca1.. (ours).

| op | request | answer |
|---|---|---|
| `hello` | `proto` 1, `game`, `account`, `secret` (64 hex), `name`, `presence` (false: launcher session, not shown online), `online_protocol`, `version`, `platform`, `relay` (optional, true: this client can use the relay), `story_session`, `boss_battle_session`, `appearance` (optional), `integrity_flags` (optional, 0-3; missing is unknown) | `account`, `name`, `friend_code`, `ip` (as the lobby sees it), `time`, `capabilities: {relay_supported, relay_max_payload, story_session, boss_battle_session, appearance, udp_endpoint, save_integrity, clean_online_required}` (absent from an older lobby) |
| `worlds` | | `worlds: [{world, rooms, members}]` |
| `udp` | `port` (on the address the lobby sees), `lan: [{ip, port}]` (<= 4, LAN/loopback only), `endpoint: {ip, port}` (optional, the client's public IPv4 UDP endpoint when it is on another address; port >= 1024; only to a lobby with `udp_endpoint`), `public_ip` (clients up to 1.5.0 connected over IPv6: the address `port` is on) | `ip` (as the lobby sees it), and what peers dial: `port`, `public_ip` (when an endpoint was accepted) |
| `room.create` | `world`, `slots` (1-4), `flags`, `ints: [[id, u32]]`, `bin_int`, `bin_ext`, `member_bin: [[id, hex]]`, optional `difficulty` (`normal`, `hard`, `infernal`, `ultimate`, `unknown`, or the raw game value; any other type is `bad_request`) | `room`, `me`, `appearances` (with `appearance` declared) |
| `room.search` | `world`, `from` (1-based), `count` (<= 20), `flag_mask`, `flag_value`, `filters: [[id, op, value]]` (op 1 = 2 != 3 < 4 <= 5 > 6 >=); or `code` | `total`, `from`, `rooms`, `incompatible` (rooms of another protocol left out) |
| `room.join` | `room` or `code`, `member_bin` | `room`, `me`, `appearances: [{member, appearance: {schema, data}}]` (the other members' looks, with `appearance` declared); `incompatible` with the room's `online_protocol` and its owner's `version`, `platform` |
| `room.leave` / `room.resume` | `room` / `room`, `member` | - / `room`, `me`, `appearances` |
| `room.appearance` | `room`, `appearance: {schema 1-255, data}` (hex, <= 128 bytes, first byte = schema) or `null`; only with `appearance` declared (see Custom appearance online) | - |
| `room.set_internal` | `room`, `flag_mask`, `flag_value`, `bin_int` (owner only) | - |
| `room.set_external` | `room`, `ints`, `bin_ext` (owner only) | - |
| `room.kick` | `room`, `member` (owner only) | - |
| `chat` | `text` | - |
| `presence.set` | `data` (<= 0x80 bytes hex), `status` (<= 0xc0) | - |
| `friends.list` / `friends.requests` | | `friends: [{account, name, online, game?, online_protocol?, version?, platform?, room?, code?, data?, status?}]` / `incoming`, `outgoing` |
| `friends.add` | `code` (`XXXX-XXXX`, any case), or `player` (a `lobby.list` handle) | `account`, `name`, optional `display_name`, `state` (`pending` / `friends`) |
| `lobby.publish` | `visible` (bool) | `visible` |
| `lobby.list` | optional `rev` (the one last received) | `rev`, `total`, and `players` (`player`, `name`, `version`, `state`), or `unchanged: true` |
| `friends.accept` / `.decline` / `.remove` | `account` | `friend` / - / - |
| `incident.file` | `description` (<= 500 characters), `launcher_version`, `game_build`, `game_version`, `cusa`, `operating_system`, `log_file`, `log_clipped` (`yes`/`no`), `log_text` (<= 64 KiB of UTF-8), optional `dump_file` and `dump_bytes` (together); see Bug reports | `ticket` (`DCP-XXXXXX`), `receipt` (64 hex digits), `submitted_at`, and `dump: "expected"`, `dump_chunk_bytes` when a dump was announced |
| `incident.dump` | `ticket`, `receipt`; no `hello` needed; binary `DI` frames follow; see Crash dumps in bug reports | `stored`, `bytes`, `complete`, or `not_found`, `bad_request`, `limit` |
| `incident.dump_end` | `ticket`, `receipt`; no `hello` needed | `stored`, `bytes`, `complete: true`, or `dump_incomplete` with `stored` |
| `incident.status` | `reports: [{ticket, receipt}]` (<= 50); no `hello` needed | `reports: [{ticket, status, status_updated_at, fixed_version?, player_message?, test_feedback?, test_feedback_at?}]`, only the reports the receipts prove |
| `incident.read` | `ticket`, `receipt`; no `hello` needed | `report` (the stored fields, `ticket`, `submitted_at`), or `not_found` |
| `incident.feedback` | `ticket`, `receipt`, `revision` (the report's `status_updated_at`), `result` (`fixed`/`not_fixed`), `launcher_version?`, `game_build?`; no `hello` needed; see Player test feedback | `ticket`, `revision`, `test_feedback`, `test_feedback_at`, or `not_found`, `stale_request`, `already_answered` |
| `party.report` | `classes` (1-4 class numbers 0-5: Fighter, Amazon, Elf, Dwarf, Sorceress, Wizard), `host` (optional, one of `classes`); room owner only, optional (see Party statistics) | `counted` |
| `comms.report` | `counts` (exactly 8 non-negative integers, index = Comms Wheel message id 0-7: Hello, Thanks, Good job, Follow me, Group up, Need help, Ready, Let's go); optional, see Comms Wheel statistics | - |

Room (`room`): `id`, `world`, `slots`, `flags` (0x20000000 added while full), `owner`, `code` (6 characters, A-Z 2-9
without I O 0 1), `key` (16 random bytes, hex: the peer handshake secret), `members: [{id, account, name, joined, bin, version, platform}]`,
`ints`, `bin_int`, `bin_ext`, `online_protocol` (its creator's). Search result rooms carry `count`, `owner_account`,
`owner_name`, `ints`, `bin_ext`, `online_protocol` and the owner's `version`, `platform`. Search skips full, closed
(0x40000000) and hidden (0x10000000) rooms, rooms of another game key and rooms of another online protocol.

Events: `room.member_joined {room, member}`, `room.member_left {room, member, why: leave | kicked | disconnect}`,
`room.owner {room, from, to}` (before `member_left`; the new owner is the lowest connected member), `room.internal
{room, flags, bin_int}` (not to the sender; `bin_int` holds the attributes that request carried), `room.kicked`, `room.gone`, `peer {room, member, account, ip, port, lan?}`,
`peer.gone {room, member}`, `room.appearance {room, member, appearance}` (to the other members that declared `appearance`;
never held for a seat in its grace period), `chat {room, member, account, name, text}` (to everyone in the room, the sender included),
`friend.request`, `friend.added`, `friend.removed`, `friend.online`, `friend.offline`.

Rules the lobby enforces:
- A member has two addresses. The observed one is that of its own connection: it never comes from the client, and it
  alone decides who shares LAN candidates and every per-address limit. The advertised one, `endpoint`, is optional and
  unverified, and is only ever a direct candidate for peers: the public IPv4 endpoint STUN or UPnP found, for a client
  whose UDP leaves through another address than its lobby connection (IPv6, proxy, VPN, two links). It is taken whole or
  not at all. A `peer` event carries its address and port, else the observed address with `port`: never the address of
  one with the port of the other.
- LAN candidates reach only members with the same observed address, whatever they advertise. Two members on different
  IPv6 addresses of one household do not get them: they use the public IPv4 or the relay.
- Each new advertised endpoint costs a token of the observed address (8, then one per 30 s, IPv6 per /64), of the account
  (the same) and of the address it names (24, then one per 5 s, whoever names it); past any of them it is ignored and the
  peers use the relay. Repeating an unchanged advertised endpoint sends peers nothing. It reaches only the other members
  of the room (at most three), who each send a few dozen small datagrams there while they connect, never game traffic
  before that endpoint has answered with the room's key. Ports below 1024 are refused: those are services, not games.
- Seat grace: when a greeted socket in a room closes, its seat is kept 30 s and room events for it are held (<= 32;
  more ends the seat at once). `room.resume` on a new socket of the same account retakes it and receives the held
  events after the answer, then the current endpoint of every other member, including one who joined meanwhile. The
  seat starts without an endpoint until the client reports one. Afterwards it is a disconnect (owner migration,
  `peer.gone`, `member_left`). Chat is never held or stored.
- Limits: frames 32 KB (160 KB for `incident.file`; crash dump chunks are binary, 64 KiB each, 8 MiB per dump); names 16 characters; 16 bin attrs of <= 0x100 bytes per array;
  32 int attrs; 16 filters; 200 friends; 50 pending requests each way (expire after 14 days); chat 120 characters, 5
  burst then 1/s; 60 messages burst then 20/s per socket, room operations 30 burst then 1/s; requests that change a stored
  row, per socket and account: room data and endpoints in a room 40 burst then 1/s (160 then 4/s per address), friend
  changes 20 burst then 1 per 3 s (60 then 1/s per address), answered `limit` with `retry_ms`; unchanged updates are free;
  more than 30 refused, unknown-operation or malformed requests, refilled 1 per 2 s, close the socket with 1008, as does a
  4th frame before sign-in (the client sends one); 16 sockets and 20 new
  accounts per day per address (IPv6 per /64; loopback exempt on the development server); 30 new connections per 60 s per
  address at the Worker (HTTP 429); per account, sign-ins 12 burst then 1 per 15 s, `room.create` 6 burst then 1 per
  20 s (20 burst then 1 per 6 s per address), `room.join` 20 burst then 1 per 5 s, so a reconnect does not reset them; changed
  `room.appearance` 10 burst then 1 per 30 s per account (40 then 1 per 5 s per address), unchanged free; party
  reports 3 burst then 1 per 2 minutes per account, 12 then 1 per 30 s per address and 30 then 1 per 30 s for the lobby; bug reports
  2 per account per rolling hour and 10 per rolling 24 hours, newest 500 kept; their attempt, address and service protections are
  in `server/lobby/src/abuse.js` and `incidents.js`.

## Peer transport

One UDP socket per game P2P socket. Every datagram starts with `'D' 'N'`, version 1, a type byte, the destination link
tag and the source link tag (u32 little endian). STUN answers on the same socket are told apart by their magic cookie.

| Type | Body |
|---|---|
| 1 HELLO / 2 HELLO_ACK | sender account u64, target account u64, link nonce u32, HMAC-SHA-256(room key, first 32 bytes)[0..16] |
| 3 DATA | seq u32, cumulative ack u32, SACK bits u32 (ack+1..ack+32), flags (1 first, 2 last fragment), <= 1150 bytes |
| 4 ACK | ack u32, SACK u32 |
| 5 PING / 6 PONG | time u32, echoed by the PONG (every second on a connected link; also the relay probe, below) |
| 7 CLOSE | optional reason byte: 1 the game closed the link, 2 the P2P socket closed, 3 an oversized message (older clients send none and ignore it) |
| 8 PUNCH | - |
| 9 COMMS | Comms Wheel message: version u8 (1), sender party slot u8 (0-3), message id u8 (0-7), sequence u8; no text |

COMMS carries only a message id; each client turns it into text in its own language. It is one unauthenticated-by-content
but link-authenticated datagram (both link tags must match, like PING), sent twice 120 ms apart and deduplicated by
sequence; a body with another size, version, slot or an id above 7 is dropped. It also travels through the relay like any
other datagram. Clients that do not know type 9 ignore it (they simply do not show the message), so
`net::kOnlineProtocolVersion` stays 1 and builds from before the Comms Wheel keep playing with it; there is no version
gate. The receiver requires the body's slot to be the slot of the room member that owns the link, and accepts at most one
message per 1.5 s per slot.

Both ends send HELLO to every candidate of the peer every 100 ms (250 ms after 3 s) for 15 s; the first authenticated
HELLO or HELLO_ACK fixes the address (a reply from an unexpected port is taken as it is: NATs that change ports). A
HELLO must name this player's account, a peer the lobby introduced, and carry the room key's MAC. DATA, ACK and PING
must carry both link tags. A PING is answered on the path it came from.

Counters: a message is one game write; a packet is one DATA datagram (a message of more than 1150 bytes is several);
resends count packets. The close line prints both: `out <messages> msgs / <bytes> B in <packets> packets, <resends>
resends (<resends / packets>% of packets), srtt <ms>` (`n/a` when the link never measured a round trip).

Retransmission:

- Round trip: one sample per ACK, from the newest packet it covers that was sent once, and none from an ACK that also
  covers a retransmission (Karn's rule; such an ACK may have been released by a filled hole). The echoed PING time is a
  second, unambiguous sample every second, so a path slower than the current RTO is still measured. RTO = smoothed RTT
  + max(4 x variance, 50 ms), 100 ms - 2 s, 1 s before the first sample.
- Loss: a packet is lost when a packet sent more than a quarter round trip after it has been acknowledged (this also
  catches a retransmission lost again), or when its timer expires. A timer waits RTO x 2^(tries-1), at most 2 x RTO while
  the peer is still sending and at most 16 x RTO when it is silent. Timers fire only within the 32 packets an ACK can
  report past its cumulative point; later ones wait until that point moves.
- Window: 64-512 packets not yet confirmed (packets reported by SACK behind a hole do not count), halved at most once
  per loss episode. At the floor a 500 ms path still carries about 120 packets/s while random loss keeps halving it.
- A link closes after 10 s without any datagram (`direct path timed out` / `relay path timed out`), after 30 tries of
  one packet, or on CLOSE.

### Link quality

Each connected link is judged once a second from the last five seconds; one bad second never changes anything.

| Signal | Meaning |
|---|---|
| resend share | resends among all DATA sends (judged with at least 20 new packets); tracks the path's loss |
| ack stall | time since the oldest outstanding packet last saw progress |
| send queue | how long the oldest message the game wrote has waited to be sent |
| rx silence | time since anything arrived (both ends PING every second, so 2.5 s is never idle traffic) |

Thresholds are relative to the round trip, never the round trip itself: a steady 250 ms path with no loss is healthy.

- Degraded: for 3 s in a row, resend share >= 8%, ack stall or send queue >= max(1 s, 2 x srtt), or anything critical.
- Critical (only from degraded): 2 s in a row of ack stall or send queue >= max(2 s, 4 x srtt) or rx silence >= 2.5 s; or 8 s in a row of
  resend share >= 18% (longer than the 5 s window, so one outage's retransmissions are not enough); or 15 s of
  degradation during which delivery itself stalled in at least 3 seconds (steady loss that retransmission hides stays
  direct).
- Healthy again after 5 good seconds. Recovering: the state right after a move to the relay.

Measured with the deterministic simulator (`dcp-net-tests transport-quality`, tests/transport_sim.h): up to 5% loss
stays direct at every round trip from 50 to 500 ms; 20% loss moves to the relay; 10% loss stays direct unless delivery
stalls; a 1 s or 3 s outage recovers on the direct path; a dead direct path moves within about 4 s.

### Relay fallback for a running link

A direct link that becomes critical moves to the relay without the game noticing.

1. Any non-healthy second starts a relay probe: a PING through the relay once a second, its stamp marked so its PONG is
   told apart. A PONG on either path proves the peer hears the relay (`relay answered in N ms`). Nothing else uses the
   relay while direct is still in use, and no data goes both ways at once.
2. Direct recovery comes first: RUDP keeps retransmitting on the direct path, and a peer that is heard again after a
   silence gets its backed-off packets resent at once.
3. When the link is critical and the relay answered within the last 10 s, the link's address becomes the relay's for
   that member. Tags, sequence numbers, acknowledgements, buffers and the game's Rudp context stay the same; the first
   32 outstanding packets are resent through the relay, the round trip estimate restarts from the probe's, and the
   window starts at 64.
4. A packet sent on the old path that arrives late is a duplicate by sequence number and is dropped; delivery to the
   game stays in order and exactly once. ACKs are cumulative, so an old one changes nothing.
5. The peer follows on the first DATA or ACK it receives from the relay for that link (`the peer moved this link to the
   relay`). An older client does not follow: its traffic keeps arriving directly, which still works for the half that is
   healthy. No protocol field was added.
6. A moved link stays on the relay for the rest of that peer session; it never probes direct again.
7. Without a relay (not offered, not relayable, or never answering) nothing changes: the link keeps the 10 s rule and its
   close line says why (`no relay for this peer`, `relay fallback failed: the relay never answered`).

The relay sender stays below the lobby's per-connection budget: 300 frames/s and 96 KB/s (bursts 600 and 192 KB),
keeping 60 frames and 8 KB for ACK and PING; frames over it are held back and retransmitted like lost ones
(`RelayStats::throttled`).

Log lines from the simulator, a 250 ms direct path that stops delivering (endpoint addresses never appear in them):

```
P2P: link 1 relay answered in 310 ms; ready if the direct path fails
P2P: link 1 quality degraded: transport direct, srtt 267 ms, rto 316 ms, resends 41.1% of 241 sends in 5 s, ack stall 2880 ms, rx silence 1690 ms, send queue 1590 ms
P2P: link 1 quality critical: transport direct, srtt 267 ms, rto 316 ms, resends 53.7% of 214 sends in 5 s, ack stall 3880 ms, rx silence 2690 ms, send queue 2590 ms
P2P: link 1 direct quality critical; switching to the relay (transport direct, srtt 267 ms, ...)
P2P: link 1 migrated to the relay; session preserved (transport relay (moved from direct), srtt 310 ms, ...)
P2P: link 1 quality recovered: transport relay (moved from direct), srtt 310 ms, rto 360 ms, resends 0.0% of 206 sends in 5 s, ...
```

On the peer that did not decide: `P2P: link 1 the peer moved this link to the relay; switching to the relay (...)`. With
no fallback the close line says so: `closed: direct path timed out: no traffic from the peer for 10 s (no relay for this
peer); transport direct, ...`.

Each link runs on the socket's own thread, never on the game's: a game busy in a cutscene, a loading screen or a save
still acknowledges and pings, so its peer sees no silence (`TestPausedGameKeepsTheLink`). Silence on both the direct
path and the relay probe means the peer's whole process or network stopped (`TestStoppedPeerTimesOut`).

### Early Story session telemetry

While seated in an Early Story session (net/session_watch.h), the Matching2 pump writes rate-limited lines
`Early Story Online telemetry: event=<name> role=<host|guest> step=<n> transition=<steady|after_step_change> ...
count=<n>`, at most 8 per event per run. Fields hold enums, buckets and small counts only; never an address, account,
name, room or save data.

| Event | When |
|---|---|
| `early_story_transition_enter` / `_exit` | the session's step changed (`from=`); exit 60 s later or at session end (`result=session_kept`, `link_lost`, `session_ended`) |
| `early_story_network_stall` | a link became critical (`silence=` bucket) |
| `early_story_relay_fallback_attempt` | a stalled link probed or moved to the relay |
| `early_story_session_recovered` | a stalled link is healthy again (`via=direct` or `relay`) |
| `early_story_peer_timeout` | the peer went silent (`timeout=10s` or `ack`), with `relay=` and our own `local_lobby=` and `local_pump=` |
| `early_story_relay_fallback_failed` | a timed-out link had used the relay (`reason=never_answered`, `relay_path_lost`, `not_proven_in_time`) |
| `early_story_peer_closed` | the peer sent CLOSE (`reason=game`, `socket`, ...): a deliberate leave, not a crash |
| `early_story_peer_left` | the lobby's member_left for that peer (`why=peer_left_room`, `peer_lobby_lost`, `peer_kicked`), `after_timeout=` when it followed a timeout |

`peer_lobby_lost` about 30 s after a timeout (the lobby's seat grace) means the peer lost its lobby connection too: its
process ended or its network failed.

## NAT traversal

- STUN (RFC 5389 Binding) from the game's own socket: the mapped endpoint is what peers will hit. The first answer is
  used at once; a second server then confirms it ("sees the same endpoint") or shows a NAT that maps each destination
  apart, for which direct is not guaranteed. The first server is asked again every 25 s, which keeps the mapping open for
  players who join later; a new answer is reported again.
- The endpoint reported to the lobby is always one source's, address and port together. The lobby connection and the
  game's UDP may leave through different public addresses (a system proxy applies only to the former), so the address
  the lobby sees is never combined with a port STUN or UPnP found. In order: the local port when the lobby sees a
  loopback, private, IPv6 unique-local or link-local address; the UPnP mapping when its address is the STUN one, when STUN
  has none, or when STUN's changes with the destination; the STUN endpoint; the address the lobby sees with the local
  port, a guess; nothing, and the relay alone.
- An endpoint on another address than the observed one is sent only to a lobby that says `udp_endpoint` (or, over IPv6, as
  `public_ip`). An older lobby gets the observed address with the local port, and the relay does the rest.
- The report waits up to 5 s for STUN and UPnP; when the paths differ and two STUN servers agree, it does not wait for
  UPnP. Members that end up with the same address and port keep apart: the game then gets `127.82.76.89` and the member.
- LAN candidates (the loopback address and the PC's private addresses, local port) are sent too; peers behind the same
  public address try them first, so two PCs on one router, and two instances on one PC, connect without hairpinning.
- Punching: on every `peer` event, and when the socket is bound after one, a few PUNCH datagrams go to the peer's
  candidates, and both ends then send HELLOs at the same time.
- UPnP: SSDP discovery, WANIPConnection/WANPPPConnection `AddPortMapping` (UDP, lease 1 h renewed at half life, the next
  four ports on conflict, permanent lease if the router wants one), removed when the socket closes. LAN routers only.
- Two symmetric NATs cannot be punched; such a pair uses the relay (below). Peer traffic is IPv4; a client with no public
  IPv4 uses the relay.

## Relay

Direct UDP is always tried first. The lobby's relay is a second path for one pair of players at a time, used only when
the direct one does not answer or a running direct link turns critical (Peer transport). There is no setting for it; it
needs no router configuration. It works the same on Windows and Linux and for every supported game build, because it
lives below the game's Matching2 and Rudp entry points, in `net::PeerSocket`.

- A client says `relay: true` in `hello` and uses the relay only when the answer carries
  `capabilities.relay_supported` with `relay_max_payload` of at least 1200. A client or lobby without the capability
  behaves exactly as before: direct only, with the same 15 s timeout.
- Per peer, not per room: a link tries the peer's direct candidates alone for 2.5 s (`Options::relay_after_ms`). If it
  is still not authenticated, the relay is offered beside them: the same authenticated HELLOs also go through the lobby.
  A new endpoint that arrives in that time gets its own 2.5 s.
  Direct is not cancelled; the first HELLO or HELLO_ACK that authenticates fixes the path of that link
  (`LinkPath`: `kConnectingDirect`, `kRelayOffered`, `kDirect`, `kRelay`, `kFailed`). A link that came up on the relay
  stays on it for the session; one that came up directly uses it only when its quality fails (Peer transport, "Relay
  fallback for a running link"). In a room of three, A-B can be direct while A-C and B-C are relayed. The 15 s
  connection timeout is unchanged.
- A peer with no usable direct candidate (the lobby has no IPv4 endpoint for it, because it connects over IPv6 and found
  no public IPv4, and no LAN candidate) does not wait for the head start: its link is created at once and the relay is
  offered immediately, since there is nothing direct to try. The game is given `127.82.76.89` with that member as the port as
  the peer's address, which resolves only to that room member's account; an unknown member resolves to nobody. If the
  lobby offers no relay, the link ends at the usual 15 s timeout.
- The relay carries the existing `DN` datagrams byte for byte, so the HMAC, fragmentation, ACKs, retransmission and
  Rudp above it are unchanged. A relayed peer's address, as the link layer sees it, is `127.82.76.89` with the peer's
  room member as the port.
- Frame (a binary WebSocket message on the existing lobby connection): `"DR"`, version 1, type 1, room token u32, member
  u16, sequence u32, payload length u16, payload; big-endian, 16 bytes of header, payload 1 to 1200 bytes (a `DN`
  datagram is at most 1175). The sender puts the destination member; the lobby replaces it with the sender's member
  before forwarding. The token is the first four bytes of the room's key, which only its members know.
- The lobby forwards a frame only when the sender completed `hello` with `relay: true`, belongs to a room and still
  owns that seat, the token is the room's, and the destination is another live member of the same room that also asked
  for the relay and has the same online protocol. It never looks at the payload. Routes are cached in memory, rebuilt
  from the tables on a miss, and dropped when a member leaves, is kicked, disconnects or resumes its seat on a new
  socket, and when the room ends.
- Limits (`server/lobby/src/relay.js`): per connection 400 frames/s and 128 KB/s (bursts 800 and 256 KB, a third above
  the client's own cap); per room 1200 frames/s and 384 KB/s (bursts 2400 and 768 KB, four members at that cap); a
  destination socket with more than 256 KB buffered gets nothing. Excess frames are dropped (the link's own retransmission
  covers them). Only a sender's own connection budget counts against it: 256 overruns, then 4 per second, suspend the relay
  for it in that room with a `relay.closed` event, and its frames there then count as stale. The room budget and a full
  destination buffer drop frames without any penalty for the sender. A sender of malformed frames (bad magic, version or
  type, truncated, oversized, bad length, no relay negotiated) is allowed 16 and then one per minute; one of frames only sent
  around leaving a room (wrong token, not in a room, stale seat, unknown member, suspended relay) 200 and then one per
  second, before its connection is closed (1008). Frames to a member that is offline or resuming are only dropped. Drops
  are logged for the first few of each reason, then one in a thousand, without room, member or connection.
- The client queues at most 512 frames for the socket (`kMaxQueuedBinary`); when full, or with no greeted socket, a
  frame is dropped and counted. A relayed frame is delivered to the link layer directly on receipt, so nothing queues
  on the way in.
- While the lobby link reconnects, the game's client sends no relay frames until `room.resume` answers (before that the
  lobby would count them as frames of a sender with no room); the links retransmit what was held back. Direct links
  are not touched by a lobby reconnect.
- Relay frames pass through the lobby's Durable Object, so unlike signalling their volume grows with the traffic of
  relayed pairs. The relay is part of the production lobby; an older lobby does not offer it.

## Friends, presence, chat, reports

- Friend codes are 8 characters per account. Friendships are keyed by account, never by name.
- Presence is per account: online while any of its greeted `presence: true` sockets is open (the launcher's, the
  game's, a reconnected one), offline when the last one closes, never stored. A seat held for its grace period does not
  count. While online play is on, the launcher keeps one such link (`launcher/online_client.cpp`) for its whole
  window, not only the Online page; it re-reads the friends and requests on every `friend.*` event and every 30 s
  (declines and renames send no event), and is started again 1.5 s after the Online name stops changing. Off closes
  it. The Connection test uses its own short session with `presence: false`.
- The in-game Options menu's Online page (docs/LAUNCHER.md) only reads the game's own lobby link, never starts or
  serves it: its friend code is the one the hello answer carried and its friends are the last `friends.list` answer the
  game itself asked for. Only its Run test sends anything (the Connection test, on its own short session).
- The game publishes its room info with Toolkit2 `setPresence` (forwarded as `presence.set`); its "Join Friends Room"
  list is Toolkit2 `getFriends`, answered from `friends.list` with one row per friend online in a room of the same
  game key and online protocol with a presence blob. The game then joins by the room id inside that blob.
- Chat is implemented in the lobby (room scoped, bounded, rate limited, not stored) and received lines are written to
  the game log (`Chat:`). The game has no chat UI of its own and the port has none yet.
- Bug reports: the launcher's Bug Report page files them with `incident.file` on a short session of its own, so Online
  Play does not have to be on; see Bug reports.

## Public Lobby

**Show me in Lobby** on the Online page's Lobby tab is off until the player turns it on (`show_in_lobby` in `dcp.ini`;
Reset turns it off). It lists the player's Online name so others can send a friend request. It is not matchmaking, a room
browser or chat: a request made here is the normal `friends.add`, and nothing invites, creates or joins a room.

- **Presence** is the socket's attachment, not a table row, so publishing costs no SQL write. The launcher sends
  `lobby.publish` after each hello while the option is on. The launcher closes its link when the game starts, so the game
  asks with `lobby: true` in its own hello, read from the saved setting; a player in game stays listed and finds requests
  in the launcher next time. A reconnect is a new hello, so the game stays listed and the launcher publishes again.
- **Leaving**: `visible: false` removes every socket of the account at once, and a closed socket is gone at once. For a
  client that vanished, a row ends `lobbyStaleMs` (75 s) after the last sign of life: any request, or the 20 s keepalive,
  which the runtime answers without waking the object and which the Worker reads with
  `getWebSocketAutoResponseTimestamp`. Nothing is swept: stale rows are left out when the list is read.
- **What a row holds**: a random handle made for each publish, the shown name, the client's version (empty unless it is a
  plain version), and `state`, worked out for the reader (`none`, `pending`, `friend` or `incoming`). No account, friend
  code, address or platform. Sending a request
  gives the sender the target's account afterwards, as a friend code request always has: `friends.requests` needs it to
  cancel. An account is not a credential.
- **Who can read**: only a socket that is listed itself may call `lobby.list` or use a handle, and a handle only works
  while its player is listed; otherwise the code is `not_visible` or `not_found`. The player is never in their own list.
- **Identity**: the name and account are the session's. Nothing in these requests can set them, and a session name outside
  the `net::online_name` rules (`A-Z a-z 0-9 _ -`, 16 at most) is never listed.
- **Default name**: a player still called `Player` is shown as `Player-7A3C`: four characters of a hash of the account
  under the lobby's private salt, stable for that account and unrelated to its friend code. Friend rows and answers carry
  it as `display_name`, because the game matches room members by the real `name`, which never changes.
- **Limits**: 100 rows, requests first, then newest, with `total` counting all of them. An unchanged list is answered with
  `rev` alone. The lobby sends no list events: while the tab is on screen the launcher reads every 5 minutes, at once for
  **Refresh lobby** (one read at a time, 5 s apart) and after a friend event or a change in the requests. See
  `server/lobby/README.md` for the rate limits.
- **Older clients** never send these requests and see no change. Against an older lobby the launcher shows that the Lobby
  is not available and keeps working. The production lobby provides it.

## Developer and testing notes

Nothing in this section is needed for normal play.

Headless tests (no game, no window):

```
cd server\lobby; node --test "test/*.test.js"            # protocol, rooms, friends, grace, limits, real WebSockets
tools\test.ps1                                             # dcp-net-tests (transport, relay fallback, failure paths), the Worker tests,
                                                           # the Linux net tests through WSL (-SkipLinux skips them), eight-build parity
tools\online-lobby.ps1 -Port 8791                          # in another window, then:
$env:DCP_TEST_LOBBY='ws://127.0.0.1:8791/'; build\cmake\clang64\bin\dcp-online-hle-tests.exe
```

`tools\test.ps1` starts a development lobby on a free loopback port for the net and build tests and prints a relay summary:
direct networking, relay transport, automatic fallback, Worker relay, protocol compatibility, failure paths, the
Connection test, address classification, endpoint selection, the endpoint path, Worker address security, the public
endpoint budget, Windows and Linux parity, and the eight builds. For the builds it separates structural parity (the same
code path and tables, no build-specific relay code; always checked) from runtime validation (needs the local boot images
in `build\image`; reported as DATA UNAVAILABLE or PARTIAL when they are missing). Nothing there starts the game, and the
production lobby is never contacted.

`dcp-online-hle-tests` calls the Matching2, libSceNet and Rudp entry points as the game does while a second player
joins through the lobby and connects over UDP. With `--endpoint-scenario <address>` it runs the path a public lobby takes:
the lobby treats it as coming from that address and a loopback stand-in answers STUN, and the test checks what a peer is
told. `tools\test.ps1` runs it for a global IPv6 and a public IPv4 address, and for a UDP path that is not the lobby
connection's (a second address after the first), and also checks the log lines. It needs a
development lobby started with `--test-observed-address` (`/?observed=<address>` from loopback; the Worker has no such
input) and reports SKIPPED otherwise. Every socket in these tests is bound to loopback: they need no firewall rule and
never send anything to the public addresses they report.

Two instances on one PC, each with its own user folder (settings, saves, caches, identity), logs and UDP port, both
on the production service:

```
tools\online-instance.ps1 -Name a         # terminal 1: user\test-client-a, build\logs\test-client-a
tools\online-instance.ps1 -Name b         # terminal 2: user\test-client-b, build\logs\test-client-b
tools\online-instance.ps1 -Name a -Build CUSA10454-1.00     # pinned to one prepared build
tools\online-instance.ps1 -Name b -Build CUSA10209-1.00
```

`-Build <title ID>-<version>` pins an instance to one prepared build through `DCP_GAME_BUILD`, set for that instance
only: its launcher and game use that build's shared folders whatever another instance activates, its package selection
is off, and the game stops if the loaded image is another build. A build that is not prepared is an error, never the
active build. Without `-Build` an instance follows the active build, as the normal launcher does.

Online play is each instance's own switch; `-ForceOnline` sets `DCP_ONLINE=1` for the run, and the page then shows
the switch locked as "On". Each instance shows the Online name of its own Online page (set it there to
tell them apart); `-OnlineName` overrides it for that run. The instance labels (`test-client-a`) stay in folder names
and script output only.

This checks the production Worker and its Durable Object, identities, friends, presence, signaling, Matching2, the
exchange of endpoints, P2P/Rudp and gameplay. It does not check NAT traversal: both instances share one address, so
they connect through their LAN candidates. That needs two PCs on unrelated Internet connections, both on the
production service with nothing to set up:

1. Both Connection tests start with "Ready for online play".
2. Exchange friend codes and become friends.
3. A hosts a room; B uses Join Friends Room.
4. Both logs show the P2P link and the Rudp contexts connected (lines below), with an address that is not a LAN one.
5. Play a whole stage: loot, area transitions, the stage results.
6. Disconnect one player, start again and join again.

`DCP_LOBBY` replaces the production lobby for a run, for diagnostics: `tools\online-lobby.ps1` runs the local
development lobby (Node.js 22.13 or later) on `ws://127.0.0.1:8790/`, which the headless tests above use. It keeps
accounts, friend codes, friends and pending requests in `user\online-test-lobby\lobby.sqlite`; rooms and presence are
never kept. `-Reset` deletes that file only; `-SeedTestFriends` makes instances a and b friends; `-Memory` keeps
nothing.

What to look for in each instance's log: `Online: on as ...`, `Matching2: signed in to the lobby`, `Matching2: created
room N`, `Net: P2P socket ... bound`, `Matching2: reporting ... endpoint`, `Matching2: member 0x20 established`, `P2P: link N
... connected via`, `Rudp: context N connected`, and at the end the `P2P: link ... closed` line with message counts.

## Deploying the lobby

See [server/lobby/README.md](../server/lobby/README.md). The production address is `net::kDefaultLobbyUrl`
(`src/net/online_config.h`), the only place it is written in the code.
