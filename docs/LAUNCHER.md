# Launcher

The PC launcher is [TRG Launcher](https://github.com/TekRantGaming/trg-launcher) 1.1.0 (pinned in `gpu/deps.txt`, the
same revision as geometry-wars-retro-evolved-2-recompiled), in the same loader process as the game. Pages: Play, Display,
Graphics, Gameplay, Online, Cheats, Controls, Trophies and About.

## Architecture

`dragons_crown_pro.exe` calls `dcp_launcher_run()` (`launcher/dcp_launcher.h`) early in `main`, after the PS4 address range is
reserved and before the boot image is read. The only other executable is the Windows update helper, `trg-updater.exe`. The loader
passes a graphics check to the launcher: the processor check on Windows, the processor check and `dcp_check_vulkan` on Linux. Play
runs it (the launcher itself needs no Vulkan) and, if it fails, `launcher/problem_dialog.cpp` explains why and the launcher stays
open. The graphics card list comes from a non-fatal Vulkan probe that runs after the launcher window is shown (see
[Launcher without Vulkan](#launcher-without-vulkan)); a system without usable Vulkan gets an empty list and a working launcher.

The launcher cannot live in the game's own window: the Vulkan presenter and its Dear ImGui context are created by the
guest, and a missing game has no guest at all. So it opens a window of its own (SDL3, an SDL renderer, Dear ImGui) and
closes all of it on Play, before the loader creates the game window. Only one SDL window and one Dear ImGui context
exist at a time. The Dear ImGui copy is the one compiled into the video core library.

With **Allow overlay programs** on (Windows only), the game does not run in the launcher's process. Play (or a start
without the launcher) creates a new `dragons_crown_pro.exe` with the same arguments minus `--launcher`,
`--dcp-skip-launcher`, `--launcher-renderer <name>` and `--log`, plus the internal `--dcp-game --log <this run's log>` (`launcher/game_process.cpp`).
It is created suspended; the launcher process writes its last log line, lets go of the log, resumes it and exits. The
game process never runs the launcher, so it never starts another one; it continues the same log and inherits the
environment and working folder. Overlay and injection tools then attach to a clean game process instead of one that
already holds what they put into the launcher. If the game process cannot be created, the launcher stays open with the
reason. Off, and on Linux, the game continues in the launcher's process as it always has.

| Path | Purpose |
|---|---|
| `launcher/launcher.cpp` | pages, Play flow, startup decision, the C entry point |
| `launcher/host.cpp` | the launcher window: SDL3, renderer, Dear ImGui, F11, placement |
| `launcher/problem_dialog.cpp` | the graphics error dialog, in the launcher or in a window of its own |
| `launcher/game_files.cpp` | `CheckGameFiles`, SHA-256 (BCrypt), param.sfo and marker reading |
| `launcher/settings.cpp` | `LauncherSettings`, the view of `user\dcp.ini` |
| `launcher/options.cpp`, `placement.h`, `play_gate.h` | Display options, window placement, the Play gate |
| `src/dcp_displays.h` | the display list shared by the Monitor row and the game window |
| `tests/launcher_tests.cpp`, `tests/cheats_tests.cpp` | focused tests (`tools\test.ps1`) |

## Folders and defaults

`dragons_crown_pro.exe` needs no arguments, Python or environment: explicit `boot.bin`, `--content-profile`, `--app0`, `--user`
and `--logs` win, and everything else is found next to the executable (`launcher/paths.cpp`).

| | Packaged (next to the executable) | Executable in `build\cmake\<toolchain>\bin` |
|---|---|---|
| Prepared game (`/app0`) | `game\prepared\<build>` | `<repository>\game\prepared\<build>` |
| Boot image | `game\image\<build>` | `<repository>\build\image\<build>` |
| User data, `dcp.ini` | `user` | `<repository>\user` |
| Logs | `logs` | `<repository>\build\logs` |

`<build>` is `v1.03` or `CUSA10454-1.01` for CUSA10454 (USA), `CUSA07001-1.00`, `CUSA07001-1.02` or `CUSA07001-1.03` for CUSA07001 (Japan),
`CUSA10209-1.00` or `CUSA10209-1.02` for CUSA10209 (Asia and Hong Kong) and `CUSA10487-1.03` for CUSA10487 (Europe). Each build has its own folders. With several installed, the one
selected last is active. Selecting a package makes its exact build active: a complete prepared folder of it is activated
without copying, and only the build that is already active is reported as up to date.

The settings file is always `<user>\dcp.ini`; the loader hands its path to the launcher and the video core. The
correctness defaults (draw preparation workers 0, asynchronous shaders off, vertex descriptor validation, linear image
readbacks, the stick dead zone) are code, and the environment cannot change them (docs/GRAPHICS.md).
Started with no folder arguments, the development executable resolves the development folders itself.

The loader writes the one numbered log itself, `logs\dragons_crown_pro_NNN.log` (the next free number, never an
existing file) in the folder above, and echoes it to the standard output the process started with, when it has one
(a console, pipe or file). `--logs <folder>` or `--log <file>` override it.

## Startup

| Game files | Show this launcher | Result |
|---|---|---|
| Missing, different version or unreadable | any | the launcher opens |
| Valid | On | the launcher opens, then PLAY starts the game |
| Valid | Off | the saved Controller prompts and Arachnophobia Safe Mode are applied to the game archives if they differ, then the game starts directly |
| Any | any, with Shift held or `--launcher` | the launcher opens |

On Linux Shift is read from the X11 server; in a Wayland session start with `--launcher` instead (see [Linux port](LINUX.md)). `--dcp-skip-launcher` skips it once for valid game files.
The two settings that live in the game archives, Controller prompts and Arachnophobia Safe Mode, are applied the same way whether the
launcher opens or not (`PlanSavedPatches` and `SetGamePatches`, `launcher/game_patches.cpp`): a run that skips the launcher applies them
before it starts the game, and stops with an explanation, without starting the game, if they cannot be applied. Everything else that
`dcp.ini` chooses is read by the loader on every run and changes no game file. `--check-imports` and `--cpu-only` never open it. The launcher is the only recovery path, so it cannot be hidden
while the game files are not valid. If its window cannot be opened, nothing starts: a message box of the system says why and
the process exits with code 1 (see [Launcher without Vulkan](#launcher-without-vulkan)). Turning the launcher off or `--dcp-skip-launcher`
still starts the game directly.

## Launcher without Vulkan

The launcher never needs Vulkan. Its own window is drawn by an SDL renderer and Dear ImGui; the game's renderer (the **Renderer**
setting) is a separate choice that the launcher only edits.

**Launcher renderer.** The order is Direct3D 11 then SDL's software renderer on Windows (`d3d11.dll` and `dxgi.dll` stay pinned
for Direct3D 11, issue #45), and SDL's first working renderer then software on Linux. A renderer counts as opened only after
its window, the renderer itself, one texture upload, clear, draw and present, and the Dear ImGui backends all succeeded; any
failure there releases everything that attempt created and the next renderer is tried (`launcher/launcher_renderer.h`,
`LauncherHost::OpenRenderer` in `launcher/host.cpp`). Direct3D 11 is not required to be a modern feature level. If no renderer
opens, a message box of the system (Windows `MessageBoxW`, SDL's message box on Linux) lists what failed and the process exits
with code 1; the game is not started and the log is kept.

**Software recovery mode.** For a system where the launcher does not open at all, start the program with the launcher forced and
SDL's software renderer:

```
dragons_crown_pro.exe --launcher --launcher-renderer software
```

`--launcher-renderer` takes `auto` (the default order above), `software`, or on Windows `direct3d11` (that renderer only, with no
fallback, for diagnosing it). It is checked before anything else starts, applies to the launcher windows of that run only, is never
written to `dcp.ini`, and is not passed to the game process. Software recovery mode also skips the Vulkan graphics card probe, so
the Graphics card row says detection is off; every other setting, including **Renderer**, can be changed and saved normally. A
shortcut with the option (Properties > Target on Windows, Launch Options on Steam) keeps the mode between starts.

**Game renderer.** `renderer` in `user\dcp.ini` is `vulkan` (the default and the supported renderer), `auto`, `opengl` or, on Windows,
`d3d11`; `auto` uses Vulkan and OpenGL when this system cannot run Vulkan. OpenGL and Direct3D 11 are experimental and render only
part of the game's features. The launcher's Graphics page and the Renderer row stay available without Vulkan, the saved renderer
and graphics card are never replaced because the probe failed, and **Renderer** takes effect the next time the game starts.

**Vulkan probe.** `dcp_gpu_probe` (`gpu/shim/dcp_gpu_list.cpp`, logic in `gpu/shim/dcp_vulkan_probe.h`) asks for the loader, its
entry points, the instance version, a bare instance and the physical devices, one step at a time, and reports the first that
fails: loader missing, Vulkan older than 1.3, no driver, instance or enumeration error, or no usable card. Nothing in it asserts
or ends the process for what Vulkan lacks. It leaves the shared Vulkan dispatcher as it found it, keeps the loader module
mapped, and runs once, after the launcher window is shown and has drawn its first frames (at least three frames and half a
second), so the pause it causes falls on a complete page. A driver that ends the process inside the probe cannot be caught by a
result code: the probe writes `gpu_probe.pending` into the user folder first and removes it afterwards, so the next start sees
the file, skips the probe (the Graphics card row says so) and the launcher opens; delete the file to ask again. A driver that
hangs inside the probe still freezes the launcher window, which is why software recovery mode skips it.

**Log lines** (`Launcher: startup: ...`, flushed as they are written): `renderer requested=... order=...`, `renderer trying=...`,
`renderer attempt=... result=ok|failed stage=pin|create|first_frame|imgui_backend reason=...`, `renderer effective=... fallback=0|1
fallback_reason=...`, `failed kind=sdl_video|no_display|window|renderer detail=... phase=...`, `game renderer setting=...`, `home cache downloads=loaded|missing
service=loaded|missing|expired`, `home refresh started|completed source=downloads|service ...`, `vulkan probe
starting`, `vulkan probe status=... api=... code=... devices=... usable=...`, and the startup phase of a crash report
(`Crash: startup phase:`). The CPU check lines (`CPU: missing ...`) and the game's own `Renderer:` and `GPU:` lines are separate,
which tells a processor failure, an SDL video failure, a Direct3D 11 or software failure, a Vulkan probe failure and a game
renderer failure apart.

## Game files

`CheckGameFiles` is the one readiness check (launcher start, Play and the installer). It reads, in this order:

1. `sce_sys\param.sfo` names a supported build: `TITLE_ID`, `CONTENT_ID`, `APP_VER` and `VERSION` of the finished game
   (the builds listed in `launcher/game_profile.cpp`, each identified by its finished game). The build is logged with its
   language archive: where the build's Menu & Subtitles sheets are, `DRACRO.CPK` for Japan.
2. `game\prepared\<build>\.dcp_view.json`: the build's version and scope `full` (written last by the installer, or by
   `prepare_game.py`).
3. The shared files, the build's regional archive (`DRACRO_US.CPK`, `DRACRO_PATCH_JP.CPK`, `DRACRO_CK.CPK` or `DRACRO_EU.CPK`, none for
   Japanese 1.00) and its update files (`sce_discmap_patch.plt` for Japanese 1.02 and 1.03) are present and not empty.
   The archives are not hashed: the marker records that their extraction finished (and the Controller prompts setting
   appends to them, see Controls).
4. `eboot.bin` SHA-256 equals the build's hash (about 21 MB).
5. `game\image\<build>`: `boot.bin`, `content.bin`, `image.json`, and `image.json` names the same title and eboot hash.

Steps 2 to 4 are `CheckGameTree`, which the installer also runs on the folder the player selects.

**USA builds.** The 1.00 package (CATEGORY `gd`, APP_VER 01.00) is a complete game, installed into `v1.03` (its legacy
folder name). The 1.01 patch (CATEGORY `gp`, APP_VER 01.01) is an update laid over a hard-linked copy of the 1.00 folder,
into `CUSA10454-1.01`. The two builds have different `eboot.bin` files. An update needs the 1.00 folder to be installed;
the launcher says so otherwise. The 1.01 install keeps the base game's English, French, German, Italian and Spanish
menu and subtitle resources: its `DRACRO_US.CPK` is the 1.00 archive with the update's entries outside the language
sets laid over it, so the 1.00 folder is never changed. The update's own archive holds Russian text in its US slot
(and has no Russian slot); that text is not installed, so English stays English. Russian is the port's built-in
translation, served over the English slot. A 1.01 folder installed by an older launcher keeps that text until the update is installed again.

**Japanese builds.** The base package (`drcpro.pkg`, CATEGORY `gd`, APP_VER 01.00) is a complete game: it is installed
into `CUSA07001-1.00`. The 1.02 and 1.03 updates (CATEGORY `gp`) are each laid over
a hard-linked copy of the 1.00 folder, into `CUSA07001-1.02` or `CUSA07001-1.03`. The base's data is linked, not copied.
Each result is checked as its finished game and gets its own boot image. The 1.03 update replaces every file the 1.02
update changes, so 1.03 does not need 1.02.

**Asia and Hong Kong builds.** The 1.00 package (CATEGORY `gd`) is a complete game, installed into `CUSA10209-1.00`. The 1.02 package
(CATEGORY `gp`, APP_VER 01.02) is an update laid over a hard-linked copy of the 1.00 folder, into `CUSA10209-1.02`. The two builds have
different `eboot.bin` files. An update needs the 1.00 folder to be installed; the launcher says so
otherwise.

**European build.** `EP2475-CUSA10487_00-DRAGONSCROWNPRO1` (CATEGORY `gd`, APP_VER 01.00, VERSION 01.03) is one complete
package, installed into `CUSA10487-1.03`, with `DRACRO_EU.CPK` as its regional archive. It has no update and needs no
`sce_discmap_patch.plt`. The build is named by its VERSION, which is how it is known; its APP_VER and VERSION are the same
as USA 1.00's, and the title tells them apart. The hash covers the whole `eboot.bin` file as the package holds it, signed
wrapper included, so a copy of the same game re-wrapped by a dump or packaging tool has another hash and is diagnosed as an
unknown executable.

**Update archives.** A finished update is its base plus the update. An archive (`.CPK`) that the update holds and that
lacks some entries of the base's archive at the same path is composed, not swapped: the staged game gets a copy of the
base's archive with every entry of the update's laid over it (the USA 1.01 regional archive keeps the base's language
sets, as above). A complete archive, or one the reader cannot open, replaces the base's as a whole. The base folder is
only read, and a failed step leaves the active build as it was.

**Features by build.** Every build offers every feature of USA 1.00:

| Feature | USA 1.00 | USA 1.01 | Japan 1.00, 1.02, 1.03 | Asia/HK 1.00 | Asia/HK 1.02 | Europe 1.03 |
| --- | --- | --- | --- | --- | --- | --- |
| Menu & Subtitles | 5 Western | 5 Western | Japanese, English (port) | Chinese, Korean, English (port) | Chinese, Korean, English (port) | 5 Western |
| Render resolution, Aspect Ratio | yes | yes (sites found by signature) | yes | yes | yes (sites found by signature) | yes (sites found by signature) |
| True Ultrawide | yes | yes (found by signature) | yes (found by signature) | yes (found by signature) | yes (found by signature) | yes (found by signature) |
| Story and Tutorial Skip Offers | yes | yes (found by signature) | yes | yes | yes (found by signature) | yes (found by signature) |
| Online Play, System type | yes | yes (found by signature) | yes | yes | yes (found by signature) | yes (found by signature) |
| Processors without LZCNT | yes | yes (found by signature) | yes | yes | yes (found by signature) | yes (found by signature) |
| Cheats | yes | yes (found by signature) | yes (found by signature) | yes (found by signature) | yes (found by signature) | yes (found by signature) |
| Voice Language | yes (found by signature) | yes (found by signature) | yes (found by signature) | yes (found by signature) | yes (found by signature) | yes (found by signature) |
| Xbox prompts | yes | yes, when its archives hold the patch's files | yes, in `DRACRO.CPK` | yes, in `DRACRO.CPK` | yes, in `DRACRO.CPK`, when it holds the patch's file | yes, when its archives hold the patch's files |
| Arachnophobia Safe Mode, Frame rate, Display, Graphics filters, Controls | yes | yes | yes | yes | yes | yes |

"yes" here says the build offers the feature and the loader can try it, not that it was seen working. USA 1.01, Asia and
Hong Kong 1.02 and Europe 1.03 have no local executable: `tools\test.ps1` reports what stands behind each feature on each
build and prints DATA UNAVAILABLE where nothing does. Installing or selecting one of these builds writes a read-only
`Dragon's Crown Pro build site report` to the log, with what the scan and the loader's searches find in its real executable.

"Found by signature" means the loader does not use a table: it finds each site in the build's own image from
the byte patterns that give exactly the tables of the five builds checked locally, uses them only when every pattern
matches the expected number of times, and otherwise leaves the feature off (Render resolution: the game does not start
with a custom size). Cheats, Voice Language and True Ultrawide need all of their sites (25 hooks and 15 functions,
11 sites and 2 tables, 22 sites) to resolve with the verified bytes and their relations, and, for the cheats, every struct displacement
the handlers use to be read back from the game's own instructions; one that does not resolve turns the whole feature
off and the log names it (`Cheats: unavailable, ...`). A successful start logs one line per feature, for example
`Cheats: 25/25 hooks, 15/15 functions, layout verified by signature`. The patterns are used only for the supported
builds whose profile asks for them, never for an executable the launcher does not know.

States: missing, valid, different version, read error. The identity lives in `launcher/game_profile.h` and mirrors
`scripts/game_profiles.py`; a test fails if they drift.

The Play card's title names the active build ("Ready to play · CUSA10454 1.00 USA"); its number is the package's
APP_VER, and a second line shows the finished package's metadata (`APP_VER 1.00 · VERSION 1.03`; PS4 metadata is `NN.NN`,
shown as `N.NN`). About's **Game** row shows the same label after `PS4 `, or `Not installed`. The log's `build` line uses
the same label.

## Install

**Install game...** (or **Reinstall game...**) on the Play page is one button. It opens a small menu with **From PKG...** and
**From extracted folder...** (`launcher/install_source.cpp`): the first opens the file chooser (`*.pkg`), the second a folder
chooser, and the chosen path goes to the same `InstallGame` on a background task with progress and Cancel
(`launcher/game_install.cpp`). A cancelled chooser does nothing. The package flow:

1. `Package::Open` (`launcher/pkg_extract.cpp`) reads the header, the entry table and `param.sfo`; `CheckSfoIdentity`
   refuses another game or version before anything is unpacked. A package inside the port's own game folders is
   refused.
2. `Package::LoadFiles` derives the keys and reads the inner file system. A package encrypted for a console (image key
   index other than 3) is refused here.
3. `Package::Extract` writes the metadata entries to `sce_sys` and every file to `<prepared>.install-temp`. The
   `.dcp_view.json` marker follows, in the same format as `scripts/game_view.py`, then `CheckGameTree` checks the
   result: `param.sfo` identity, the required files and the `eboot.bin` hash.
4. `BuildBootImage` (`launcher/boot_image.cpp`, the native port of `scripts/prepare_image.py`; both write identical
   files) builds `boot.bin`, `content.bin` and `image.json` into `<image>.install-temp`.
5. `CheckGameFiles` must pass on the staging folders. Only then are the current game and boot image moved to
   `.install-backup`, the staging folders renamed into place and the backups deleted. Any failure or Cancel before
   that leaves the current installation as it was; `user\` and `logs\` are never inside these folders.

`pkg_extract.cpp` is shadPS4's PKG extractor (`src/core/file_format/pkg.cpp`, `pfs.h`, `pkg_type.cpp` and
`src/core/crypto`, as they were before upstream commit be22674f "code: Remove fpkg code"), with Windows CNG in place of
Crypto++, 64-bit file sizes (`DRACRO.CPK` is 5 GB) and a directory walk from the super-root, as `scripts/pkg_extract.py`
does. Both write byte-identical files from the game's package.

`InstallGame` takes the extracted game folder the same way: a directory goes to `InstallFromFolder`, a file to
`InstallFromPackage`. The folder is the game's root, the one with `eboot.bin`, `sce_module` and `sce_sys`, never `eboot.bin`
itself. `eboot.bin` and `sce_sys\param.sfo` must exist, a folder inside or holding the port's own game, image, staging or
backup folders is refused (`kOverlapping`), and `CheckGameTree` then applies the same checks as to an unpacked package: build identity,
the required files (including the three `sce_module` libraries) and the `eboot.bin` SHA-256, so a folder patched by another tool is
refused as an unsupported version. Files are hard-linked into staging on the same drive and copied otherwise; the selected folder is
only read (the later patches write a file of their own first). Staging, the boot image, the verification and the swap are the
package's, so a failure or Cancel leaves the installed game, `user\`, `logs\` and the shader cache as they were.

A folder must be the finished game. An extracted update alone is refused as incomplete when it lacks the base game's files (the
check cannot tell a partial archive from a complete one), and an extracted base of a build that has updates is another version; composing an extracted update over a base is not
supported, only package updates are. Without a desktop chooser (Linux without zenity or kdialog, or `DCP_FILE_CHOOSER=builtin`)
the launcher's own chooser lists the folders and **Select this folder** takes the one that is open.

## Headless commands

`dragons_crown_pro.exe` (`./dragons_crown_pro` or the AppImage on Linux) has two commands for front ends and scripts
(`launcher/headless.cpp`). `main` handles them first, before the runtime, the log, the launcher or any window exists, and they
never open a window, a dialog, a chooser or the game. The command must be the only argument.

| Command | What it does |
|---|---|
| `--install <source>` | `InstallGame` on the active build's folders (`dcp_resolve_paths`), exactly as **Install game...** does: a decrypted PKG file, or an already extracted complete game folder (`eboot.bin`, `sce_module`, `sce_sys` and the rest of the game). The same checks, hard links on the same drive, staging, boot image, verification and rollback; an extracted update alone or a folder that holds or sits inside the port's own folders is refused. It does not start the game and, unlike **Reinstall game...**, does not replace a build that is already the active, ready one (it reports `ready ... (already installed)`). With `DCP_GAME_BUILD` set it refuses, as the launcher's package selection is off then. |
| `--check-game` | `CheckGameFiles` on the active build, read-only: nothing is written, built or repaired (an older port's preparation marker is judged as updated, not rewritten). |

| Exit code | Meaning |
|---|---|
| `0` | installed, activated or already installed; or the game files are ready |
| `1` | the install failed, or the game files are not ready |
| `2` | invalid arguments: no source, an extra argument, or another option next to the command |

Progress and results go to the standard output, in a few lines: `Install: <phase>`, `Install: 50% 3227 / 6455 MB` in steps of
10 %, then `Install: ready <build label>`; failures go to the standard error as `Install: failed: <title>. <text>`. `--check-game`
prints `Game files: ready` with `Build:`, `Title ID:` and `Version:` lines, or `Game files: not ready` with `State:` and `Reason:` (not
installed, incomplete files, a boot image that is missing or built from another `eboot.bin`, another executable). No log file is written.
The executable has no console of its own on Windows: a front end reads a pipe, and a terminal shows the output when it is
piped (`dragons_crown_pro.exe --check-game | Out-Host`) or, otherwise, the parent console it attaches to. Because the program is a
Windows GUI executable, a shell does not wait for it: to read the exit code use `Start-Process -Wait -PassThru` (PowerShell) or
`start /wait` (cmd).

## Home

Home is the first sidebar page and the page the launcher opens on: the project in numbers, and a quick start.

- **Hero**: three lifetime totals side by side, each with an icon, and PLAY, ONLINE LOBBY and MANAGE GAME on the right.
  They wrap or shrink at narrow widths.
- **Downloads**: the sum of `download_count` over the assets of every published GitHub release of
  `saulob/dragons-crown-pro-pc` (drafts are skipped; `.sha256`, `.sha512`, `.sha1`, `.md5`, `.sig`, `.asc`, `.minisig`
  files and `SHA256SUMS`/`CHECKSUMS` manifests are not downloads). It is cached for 30 minutes in
  `user/home_stats.cache`, and an earlier value stays shown while GitHub cannot be reached. It does not depend on the
  online service.
- **First frame.** The cached Downloads value and the cached online statistics (`user/home_service.cache`: counters and class
  names only, kept for up to six hours) are read before the window is shown and drawn on its first frame. The requests only
  refresh them: a failed refresh keeps what was shown and never overwrites the cache. Without a usable cache the numbers show
  a dash, and a loading placeholder appears only for a request still pending after three quarters of a second.
- **Online sessions since launch** (`lifetime.sessions`): the sum of the dashboard's Sessions over every day the service
  has kept, so a reconnect inside the 10-minute window is still the same session. **Players since launch**
  (`lifetime.players`): the number of distinct online accounts the service has registered, one per account however
  often it connects. Both come from the same `/stats` request, and a service without them shows dashes while
  Downloads and everything else still work.
- **Right now**: Players online now, Players playing online, Open online games and Peak players today. **Today**: Online
  sessions today, Online games created today and Players joined today. **Last 30 days**: Multiplayer sessions. They
  come from the online service's anonymous aggregate statistics, `GET /stats` (server/lobby/README.md), and are the
  dashboard's own counters under player-facing names:

  | Home | `/stats` field | Dashboard |
  |---|---|---|
  | Players online now | `live.online` | Online now |
  | Players playing online | `live.in_rooms` | In rooms (players holding a seat in an online game) |
  | Open online games | `live.open_rooms` | Open rooms |
  | Peak players today | `live.peak_today` | Peak today |
  | Online sessions today | `today.sessions` | Sessions today |
  | Online games created today | `today.rooms_created` | Rooms today |
  | Players joined today | `today.joins` | Joins today (not unique players) |
  | Multiplayer sessions | `last_30_days.rooms_with_guest` | Rooms with a guest, 30 days: online games that ended with at least one guest in them |

  A number the service cannot read is a dash, never zero, and the page says "Live statistics unavailable" until the whole
  answer is complete. One request fills every number; it is repeated at most once a minute while Home is shown, and not
  at all while another page is open (five minutes after a failure).
- **Most played classes**: the classes of the party snapshots clients reported in the last 30 days (the dashboard's
  "Characters in parties"), most played first, up to six, each with its share. It counts class appearances in reported
  multiplayer parties, not unique players. Every face is the one the Editor shows, rendered from the installed game's
  archives on a worker thread (`launcher/home_portraits.cpp`, the Editor's `CharacterArt`). Without a readable game the
  classes are listed as text only.
- **PLAY** is the footer's PLAY. Without a ready game the button is **INSTALL GAME** and opens Play. **ONLINE LOBBY**
  opens the Online page, where the Lobby is; **MANAGE GAME** opens Play. Play stays the page for installing, reinstalling
  and Show this launcher.
- The artwork behind the page (`assets/launcher/home_background.png`) is drawn only inside Home's content panel, by the
  page backdrop hook that `gpu/patches/trg-launcher/0018-page-backdrop.patch` adds to TRG Launcher. It is cropped to
  the panel anchored to the right so the kingdom stays in view, scaled to the exact pixels shown, and darkened more on
  the left, where the numbers are (`launcher/home_art.h`). Without the file the panel is plain.

Reading these numbers is a plain HTTPS request off the UI thread: it never turns on Online play, never signs in, never
lists you in the Lobby, never creates a session or counts as Online now, and does not change any setting. Without a
network Home shows the last known downloads and dashes for the rest; PLAY works either way.

## Save and Play

- **Save** writes the pending settings and keeps the launcher open. **Quit** closes without saving.
- **PLAY** is rejected while the files are not valid. Otherwise it saves the pending settings once, shows "Settings
  saved. Starting game..." for 0.4 s with the controls disabled, closes the launcher window and lets the loader go on
  (with Allow overlay programs on, Windows: starts the game process, see Architecture). A failed save, or a game
  process that cannot be created, keeps the launcher open. No settings are written per frame.
- The window placement is saved about half a second after a move or resize, and when the launcher closes.
- **Sponsor on GitHub** (with the heart `assets/launcher/sponsor-heart.png`, left of Quit, and a card at the top of About) opens `https://github.com/sponsors/saulob` in the
  default browser when activated, and nothing else: no request is made to show it, it never takes the Enter default
  from PLAY, and the footer button is left out when the window is too narrow to fit it beside the hint text. It is
  patched into TRG Launcher by `gpu/patches/trg-launcher/0003-sponsor-link.patch`.
- The sidebar's Online entry shows the number of friend requests waiting (`trg::Page::badge`, patched in by
  `gpu/patches/trg-launcher/0004-sidebar-badge.patch`).

## The Options menu

**F1**, or **L3 + R3** together on a controller, opens it over the game; **F1**, Escape, **B** or **L3 + R3** again close it (`gpu/shim/dcp_options_menu.cpp`, model in `dcp_options_menu.h`, shared input rules in `src/dcp_menu_input.h`). It is the launcher's settings, changed while playing, written to the same `dcp.ini` the moment they change; lines it does not own are kept. It does not open over the F10 Cheats menu, a game dialog or the start-up loading screen, and F10 does not open over it.

| Page | Setting | When it applies |
|---|---|---|
| Display | Display mode (also F11), VSync | at once |
| Display | Aspect Ratio (also saves the window width that goes with it), True Ultrawide | saved now, applies at the next start; shown only for builds that support them |
| Graphics | Presentation, Upscaling (FSR 1), Sharpening, Sharpening strength, Anti-aliasing | at once |
| Graphics | Renderer, Render resolution (builds that support it), Texture filtering | saved now, applies at the next start |
| Gameplay | Frame rate in title, Render resolution in title, Frame counter | at once |
| Gameplay | Enable Boss Battle (builds that support it; the host's side of Boss Battle Online) | the next time the Inn menu opens |
| Gameplay | Skip Intro, Voice Language (builds and game folders that offer it), Frame rate, Story Skip Offer, Tutorial Skip Offer, Unlock Gallery, Unlock Labyrinth of Chaos, Unlock Ultimate + Tower of Mirages | saved now, applies at the next start |
| Online (Online Play on) | your Online name and friend code (copy with A or Enter), the Connection test, your friends and their status | read only; the test runs on request |
| Controls | Controller Rumble, Mouse Controls, Pointer Control Mode, Mouse Sensitivity | at once |
| Controls | Japanese Menu Buttons, the hotkeys (read only) | saved now, applies at the next start |
| Cheats (Online Play off) | the F10 menu's rows, through the same session state | session only, never saved |

Window size, Monitor, Graphics card, language, Arachnophobia Safe Mode, controller prompts, keyboard bindings, Allow overlay programs, detailed logging and the Online settings stay in the launcher: they size the window, enumerate devices, rewrite installed archives or change how the game process starts, and the pages say so. A setting that applies at the next start shows "(restart)" and the footer reminds you; the Renderer is chosen when the video core starts and cannot be switched in a running game, and the game has no restart of its own, so close it and start it again. Keyboard: Up / Down rows, Left / Right values, Enter changes, Q / E or Tab pages, Escape closes. Controller: D-pad or left stick moves, A changes, LB / RB pages, B closes. The mouse works too. While it is open the game reads neutral input; closing it waits until what was pressed in it is released (one second at most), and `runtime_pad.c` hides L3 and R3 together from the game and any button still held afterwards, so the opening and closing presses never reach the game.

With Online Play on the Online page takes the place of the Cheats page, which does not exist then: the cheats stay locked in `runtime_cheats` as for F10. The Online page shows a copy of what the game's own lobby connection knows (`page_link` in `src/runtime_matching2.cpp`, taken for the overlay by `runtime_online_page_update`): the Online name as the game uses it, the account's friend code, the friends with the launcher's status words (Offline, Online, Online, in a room, Incompatible) and the Connection test, the same `net::run_connection_test` as the launcher's but on any free UDP port, since the game holds its own, run on a worker thread when you select **Run test**. The page only reads: it never turns Online Play on or off, connects, reconnects or refreshes anything, so what it shows is what the game's own Online runtime already has (the friend list appears once the game has loaded it). Only **Run test** touches the network. Online Play itself, the Online name and friend management (adding, removing, accepting and declining) stay in the launcher. On a build without the cheat hooks, with Online Play off, the Cheats page is there and its rows read "Unavailable".

## Settings

File: `user\dcp.ini` in the `--user` folder, the file the video core reads at start; the game writes only `display_mode` (F11) and the keys the Options menu changes. Lines
the launcher or F11 do not manage (comments, the other side's keys) are kept when either saves. Booleans are written
`1`/`0` and read as `1`/`on`/`yes`/`true` or `0`/`off`/`no`/`false`. A key that is missing, or holds a value the game does
not accept, has the default below, in the launcher and in the game alike (`tests/launcher_tests.cpp` checks that both
use the same defaults).

| Key | Values | Default | Applied |
|---|---|---|---|
| `show_launcher` | 1, 0 | 1 | next start |
| `display_mode` | `windowed`, `borderless` | `windowed` | at window creation; live with F11 |
| `window_width`, `window_height` | pixels | 1920, 1080 | at window creation |
| `monitor` | 1-based display number | 1 | at window creation |
| `graphics_card` | an exact Vulkan device name | the first usable device | at start |
| `vsync` | 1, 0 | 1 (FIFO) | live |
| `fsr1` | 1, 0 | 0 | live |
| `rcas`, `rcas_strength` | 1, 0; 0-100 | 1; 75 | live |
| `smaa` | 1, 0 | 0 | live |
| `anisotropy` | 0 (game), 2, 4, 8, 16 | 0 | live |
| `render_resolution` | `native`, `480p`, `720p`, `1440p`, `1800p`, `4K` | `native` | at start |
| `aspect_ratio` | `16:9`, `16:10`, `21:9`, `32:9`, `4:3`, `5:4` | `16:9` | at start |
| `true_ultrawide` | 1, 0 (`true`, `false` are read too) | 0 | at start (21:9 and 32:9 only) |
| `frame_rate` | 30, 60, 90, 120, 144, 165, 240, 0 (Unlimited) | 60 | at start |
| `show_fps_title` | 1, 0 | 0 | live |
| `show_resolution_title` | 1, 0 | 0 | at start |
| `show_fps` | 1, 0 | 0 | live; F2 changes the session |
| `show_ping` | 1, 0 | 0 | at game start, Online Play only; F3 changes the session |
| `early_story_online` | 1, 0 (`DCP_EARLY_STORY_ONLINE` overrides it) | 0 | at game start (restart), Online Play only; experimental, for the host: online right after the tutorial, the host controls the story, guests keep their own campaign progress and do not need it, early-story rooms are friend/direct only |
| `detailed_logging` | 1, 0 | 1 | at exit |
| `key_*`, `hotkey_cheats_menu`, `hotkey_window_mode` | a key name (below) | the default layout | at game start |
| `arachnophobia_safe_mode` | 1, 0 | 0 | when toggled in the launcher |
| `voice_language` | default, english, japanese | default | at game start (every build; a language whose banks the installed `DRACRO.CPK` lacks is not offered) |
| `dcp_japanese_npc_voices` | 1, 0 | 0 | legacy, read only while `voice_language` is absent or invalid: 1 resolves to japanese, anything else to default; never written |
| `skip_intro` | 1, 0 | 0 | at game start |
| `japanese_menu_buttons` | 1, 0 | 0 | at game start |
| `controller_rumble` | 1, 0 | 1 | at game start; F1 changes it at once |
| `controller_exit_shortcut` | 1, 0 | 1 | at game start; L1 + R1 + Options held 2 seconds on one controller closes the game |
| `hotkey_exit_game` | a key name | `f9` | at game start; closes the game like the title's Exit Game row |
| `hotkey_character_highlight` | a key name | `f7` | at game start; toggles Character Highlight for the primary local player |
| `controller_character_highlight` | `back+r3`, `back+l3`, `l1+r3`, `r1+l3`, `off` | `back+r3` | at game start; this chord on a controller toggles Character Highlight for the player that controller drives |
| `controller_comms_wheel` | `back+dpad_up`, `l1+dpad_up`, `r1+dpad_up`, `dpad_up`, `off` | `back+dpad_up` | at game start; controller shortcut that opens the Comms Wheel in online rooms |
| `mouse_controls` | 1, 0 | 1 | at game start; F1 changes it at once |
| `story_skip_offer`, `tutorial_skip_offer` | 1, 0 | 0 | at game start |
| `unlock_gallery` | 1, 0 | 0 | at game start |
| `enable_boss_battle` | 1, 0 | 0 | at game start; F1 changes it the next time the Inn menu opens |
| `unlock_labyrinth`, `unlock_ultimate_tower` | 1, 0 | 0 | at game start |
| `allow_overlay_programs` | 1, 0 | 0 | at Play (Windows only) |
| `cheat_infinite_health`, `_ally_health`, `_mana`, `_ally_mana`, `_lives`, `_durability`, `_ally_durability`, `_arrows`, `_skill_points`, `_skill_uses`, `_gold` | 1, 0 | 0 | at game start (F10 changes the session only) |
| `cheat_movement_speed` | 1 (Off), 2, 4 | 1 | at game start (F10 changes the session only) |
| `cheat_movement_speed_2x` (old) | 1, 0 | 0 | read only when `cheat_movement_speed` is missing: 1 means 2x |
| `cheat_experience_multiplier` | 1 (Off), 2, 5, 10, 100 | 1 | at game start (F10 changes the session only) |
| `cheat_damage_taken` | 100 (Off), 75, 50, 25, 0 | 100 | at game start (F10 changes the session only) |
| `cheat_damage_multiplier` | 1 (Off), 2, 5, 10, 100 | 1 | at game start (F10 changes the session only) |
| `overlay_scale`, `overlay_corner` | 50-300 %; 0-3 (top left, top right, bottom left, bottom right) | 100; 1 | at game start (no launcher control) |
| `launcher_x`, `_y`, `_width`, `_height`, `_maximized`, `_fullscreen` | | | launcher window only |

The game window is created after the launcher closes, in the same process, so every setting applies without a restart.
**Reset all settings** (About) sets every row above to its default, pending until Save or Play. It leaves the launcher
window placement, the game files, saves, logs and caches alone.

None of the correctness defaults are settings: draw preparation workers (0), asynchronous shader compilation (off),
vertex descriptor validation and the 10% stick dead zone are not exposed.

## Display

- **Display mode**: Windowed or Borderless Fullscreen (SDL desktop fullscreen, no display mode change).
- **Window size**: 1280 × 720, 1600 × 900, 1920 × 1080 (the game's own size), 2560 × 1440, 3840 × 2160, in pixels, always
  all listed, whatever the monitor; the saved value is exactly the one chosen. A size that
  does not fit when the game opens is reduced keeping its shape until the whole window, title bar included, fits the
  monitor's usable area; a size that fits opens unchanged. The window opens centred there. This is applied at start only: the saved
  Window size is not changed, and a window you resize afterwards is left alone. The picture keeps its shape and is
  letterboxed or pillarboxed in the window. The listed sizes (heights 720, 900, 1080, 1440, 2160) follow the Aspect Ratio.
- **Aspect Ratio**: 16:9 Original, 16:10, 21:9 Ultrawide, 32:9 Super Ultrawide, 4:3, 5:4 (`aspect_ratio` in `dcp.ini`;
  an unknown value is 16:9). It sets the shape of the game's render target. The game's 2D scene is still laid out for its
  original 1280 × 720 canvas, so only 16:9 Original is undistorted: 21:9 and 32:9 stretch the gameplay horizontally instead
  of showing more of the world (issue #1) unless True Ultrawide is on, and 16:10, 4:3 and 5:4 squeeze it. In those narrower shapes the port skips the one draw that painted black rectangles in water (issue #32); 16:9 and wider are untouched. The HUD, menus and
  prerendered videos are not adjusted. Changing it moves the saved Window size to the same height in the new shape. Borderless Fullscreen on a monitor
  of another shape shows bars, as set by Presentation. The Japanese and Asia and Hong Kong builds are not yet tested in game.
  Every supported build offers every shape (see Render resolution for how the USA 1.01, Asia and Hong Kong 1.02 and
  Europe 1.03 sites are found).
- **True Ultrawide** (experimental, Off by default; `true_ultrawide` in `dcp.ini`, a missing or unknown value is Off): with
  21:9 or 32:9 the game draws more of the world instead of stretching it. The vertical view stays the one of 16:9 and the
  horizontal view grows by the render target's shape over 16:9 (× 1.3125 for 21:9, × 2 for 32:9), so characters and
  scenery keep their proportions. Distant background layers fill the whole width; the playfield and the layers that
  move with it stop where the stage's camera limits and the playfield layer end (never narrower than 16:9), showing
  the background behind them. On a single-screen town facility picture (the guilds, the shop, the inn) the character,
  props, titles and text keep their 16:9 size and place and only the room behind them is scaled up evenly to fill the
  sides, within the picture's own top and bottom, while its top and bottom bars, frame lines and dialogue backing
  reach the frame edges; the route map keeps its 16:9 composition, centred. The HUD, menus and text keep their 16:9 layout and proportions,
  centred; fades, cinematic bars, the loading and saving fades and the bottom dialogue and narration bands reach the
  frame edges, other message bands keep their 16:9 width, and the town map is scaled up evenly to fill the width (at
  32:9 only as far as its markers stay on screen, with black beyond). The row is disabled for the other shapes, where it
  changes nothing. Off gives back the Aspect Ratio's own picture exactly. It is available on
  every supported build; on CUSA10454 1.01, CUSA10209 1.02 and CUSA10487 1.03 its sites are found by signature, and if any of them does
  not resolve the feature stays off and the log says which.
- **Monitor**: `Display N`, the primary display first, then SDL's order; the same list creates the game window.
- **Renderer**: Vulkan (the default), Auto, OpenGL (experimental) or, on Windows, Direct3D 11 (experimental). Auto uses
  Vulkan and falls back to OpenGL when this system cannot run Vulkan; a renderer chosen by name starts or fails as itself.
  Saved as `renderer=vulkan`, `auto`, `opengl` or `d3d11` in `dcp.ini`; a missing or unknown value is Vulkan, and so is
  `d3d11` on Linux. It takes effect the next time the game starts, and the log shows
  `Renderer: selected <setting>, active <backend>`. See docs/GRAPHICS.md for what OpenGL does not render yet, and
  docs/d3d11-renderer.md for Direct3D 11.
- **Graphics card**: each Vulkan device the renderer can use (the launcher enumerates them without creating
  a device; a device must meet the renderer's Vulkan version, extension and robustness2 requirements), by its exact Vulkan
  name. The first listed device is the default. A name that is not there any more, or not usable, falls back to the first
  usable device, and the launcher saves that name when you press Play. The game's log shows `GPU: selected <name>`, then
  `GPU: Vulkan device <name>`. It is the game's device; the launcher's own window is drawn separately (Direct3D 11, then SDL's software renderer, on Windows; SDL's first working renderer, then software, on Linux). Without usable Vulkan the row says so and the saved card is kept.
- **VSync**: On is FIFO (the stable default). Off uses Mailbox, else Immediate.
- **Presentation**: Letterbox (the default) or Stretch. It controls how the finished frame fits the window or display when
  their shape differs: Letterbox keeps the frame's aspect ratio with bars, Stretch fills the window and may distort the
  picture. It does not change the render target or the Aspect Ratio, and it applies in Windowed and Borderless Fullscreen
  alike. Saved as `presentation=letterbox` or `presentation=stretch` in `dcp.ini`; a missing or unknown value is Letterbox.
  The game reads it when it starts, so a change takes effect the next time the game starts.
- **F11** in the game switches Windowed and Borderless Fullscreen at once and saves the Display mode. In the launcher it
  switches the launcher's own fullscreen, saved in `launcher_fullscreen`, and never changes the Display mode.
- **Mouse cursor**: mouse movement, clicks and the wheel show the cursor in Windowed and Borderless Fullscreen; keyboard or
  controller input hides it. It stays visible while the cheat menu or a dialog is open and when the game window loses focus.
  While Pointer Control is held, or whenever it is Always On (Controls page), the cursor is hidden and the mouse is captured by the window.

Scaling and sharpening, below those on the Display page, are what the presenter already supports: **Upscaling** (AMD
FSR 1, off by default) enlarges the game's picture (the render resolution) in a bigger window; **Sharpening** (AMD
RCAS, on) with a
**strength** (75). They run on the finished frame at present time.

## Graphics

**Render resolution** (a drop-down; `launcher/dcp_resolution.h`): Native (1080p) 1920 × 1080, 480p 854 × 480, 720p
1280 × 720, 1440p 2560 × 1440, 1800p 3200 × 1800, 4K 3840 × 2160. It is the game's own render size, separate from the window size: the picture is scaled to the window.

The game chooses its display render targets with `mov reg, imm32` of 1920 / 3840 and 1080 / 2160, picked by
`sceKernelIsNeoMode`. Each supported build with verified sites has its own 23 of them (`launcher/game_profile.cpp`); the
USA 1.01, Asia and Hong Kong 1.02 and Europe 1.03 builds have no table and find their 23 in their own image by the instruction context
of each site (`dcp_render_size_signatures`, `launcher/resolution_probe.c`), which gives exactly the table of every build
that has one. All 23 must be found once, with the original instruction and without overlap, or the game does not start
with a custom size. The loader refuses to patch a build the launcher does not know. Unless the render target is
the unchanged Native 16:9 one, the loader rewrites them (`launcher/resolution.c`) to the chosen width and height after
checking that each still holds the original instruction; if one does not, the game does not start. Native 16:9 leaves the
code untouched. `sceKernelIsNeoMode` answers "PS4 Pro" for a target larger than 1920 × 1080 in either dimension or in
area (1440p, 1800p, 4K and the wider shapes), which is the game's larger graphics memory path, taken to be sized for
3840 × 2160; the other targets use the base PS4 path. The size
reported by `sceVideoOutGetResolutionStatus` plays no part. The run log prints `DCP: render resolution WxH (key)`.

**Aspect Ratio** reuses the same 23 sites: the height sites get the Render resolution's height and the width sites the
width of the ratio at that height, rounded to a multiple of 8 (the tile width). 16:9 Original writes the Render
resolution unchanged and, with Native, still patches nothing. A target above 3840 × 2160 pixels keeps its ratio and loses
height until it fits: 21:9 at 4K is 4384 × 1880, and 32:9 at 1800p and at 4K are both 5408 × 1520. That cap is a
conservative memory guard, not a measured limit, and the Render resolution list shows the nominal sizes whatever the
ratio. The render width is rounded to a multiple of 8, so a window of the same height can differ by a few pixels (5:4). The run log prints
`DCP: Aspect ratio: 21:9 Ultrawide, render target 2520x1080`. The presenter already sizes its frame from the game's image
shape.

**True Ultrawide** changes the game's scene pass projections, not the presenter (docs/GRAPHICS.md). The run log prints one
line, for example `DCP: True Ultrawide requested on, effective on (21:9 Ultrawide, 2520x1080, CUSA10454 1.00 USA): pass
projection 6 sites, horizontal x1.3125, vertical unchanged; fill hooks installed; world window hooks installed; widget,
map and fade hooks installed`, or the reason it is
off: not requested, a shape not wider than 16:9, a build whose sites are not verified, or a site that does not match (the
game then starts with the plain Aspect Ratio picture).

**Anti-aliasing** is SMAA 1x (tlg_pc's host pass, MIT): a post-process on the finished picture at the game's render
resolution, before FSR 1 and sharpening. **Texture filtering** forces 2x to 16x anisotropic filtering on the game's
mipmapped, bilinear samplers of graphics draws (Bloodborne's generic sampler override, without its title hooks); Game keeps
the game's own filters. MSAA is not offered: the references only respect the game's own multisampling or switch it off for
one title, and none lets the user choose a sample count.

Not offered: HDR (the video core's switch exists, but the game's `param.sfo` does not declare HDR support, so it would do
nothing). The frame rate is on the Gameplay page. There is no Performance page. Asynchronous shader compilation is fixed off:
`dcp.ini` and the environment cannot enable it.

## Gameplay

- **Menu & Subtitles**: the language of the game's menus and subtitle text (`text_language`). Each build offers only
  the languages it can show; the first is the default:

  | Build | Languages |
  | --- | --- |
  | CUSA10454 1.00, 1.01 USA | English, French, German, Italian, Korean, Portuguese (Brazil), Russian, Spanish (`english`, `french`, `german`, `italian`, `korean`, `brazilian_portuguese`, `russian`, `spanish`) |
  | CUSA07001 1.00, 1.02, 1.03 Japan | Japanese, English, Portuguese (Brazil), Russian (`japanese`, `english`, `brazilian_portuguese`, `russian`): the executable does not read the system language |
  | CUSA10209 1.00, 1.02 Asia/Hong Kong | Traditional Chinese, Korean, English, Portuguese (Brazil), Russian (`traditional_chinese`, `korean`, `english`, `brazilian_portuguese`, `russian`) |
  | CUSA10487 1.03 Europe | English, French, German, Italian, Korean, Portuguese (Brazil), Russian, Spanish (`english`, `french`, `german`, `italian`, `korean`, `brazilian_portuguese`, `russian`, `spanish`) |

  Portuguese (Brazil) is the port's own translation, not a language of the game, so it is offered on every supported
  build and a saved choice survives switching builds. The game runs in a native slot and the port serves translated
  copies of that slot's sheets when the game reads them; the installed archive is never written:

  | Builds | Slot | Archive | Sheets |
  | --- | --- | --- | --- |
  | CUSA10454 1.00, 1.01 | English | `DRACRO_US.CPK` | checked against the corpus's English hashes |
  | CUSA10487 1.03 | English | `DRACRO_EU.CPK` | checked against the corpus's English hashes |
  | CUSA07001 1.00, 1.02, 1.03 | Japanese | `DRACRO.CPK` | matched by record index, `localization/pt-BR/sources/JP.json` |
  | CUSA10209 1.00, 1.02 | Traditional Chinese | `DRACRO_CK.CPK` | matched by record index, `localization/pt-BR/sources/CT.json` |

  It covers the game text, the launcher, the F1 options, the F10 overlay and the online screens. Text the port has no
  translation of stays in the slot's language: trophy text and the credits are the build's own.

  The launcher itself is shown in English or Portuguese (Brazil) only (`launcher_language`, no Options row). Choosing
  English or Portuguese (Brazil) in Menu & Subtitles sets it; any other game language changes the game and leaves the
  launcher as it was, English when nothing was chosen before. Voice Language never changes it.

  The Editor shows the game's own words in the launcher language: equipment names, name words, modifier texts and the
  appearance colours are the records of `ItemList`, `CustomItem` and `SystemMsg`, in English or translated into
  Portuguese (Brazil), laid over the slot's sheets by the same hash checks the game's overlay uses
  (`TranslatedSheetRecords`, `launcher/game_translation.h`), so a name reads as it does in the game. Searching an item also
  matches its English name, and sorting and searching ignore accents. Only presentation changes: item definitions,
  modifier ids, colour indices and the save bytes are the same in every language.

  The DefaultBone character messages are translated too. While a character is being created, an empty message row
  shows the default it will receive (src/default_message_hook.c): the variant previewed for a class is held and given
  to the game's one pick when the character is added, so the saved text is the previewed one. The guest's random
  number is still drawn once and nothing is written before the character is added. Characters already in a save keep
  their stored messages unchanged, but a stored message that is exactly one of the slot's own defaults for the
  character's class and category (any of its 15 variants) is shown as the translated default whenever the game reads
  it: the four getters that copy a character's stored message are hooked at their load, and the text they copy is the
  translated one. The comparison is on the whole UTF-16 text; a message that differs in any unit stays as it is, and
  the save is never written. Without a port-side language selected the game runs unhooked. Other languages keep the
  game's own "no message" row.

  The released image labels (PONTOS, VAI!, RANKS DE TESOURO, ENTRE, CARREGANDO, SALVANDO, SIM, NÃO, DADOS, NOVO and the
  copyright line) reach every build. The USA packs draw them from `*_Add` atlases; the Japanese and Asia/Hong Kong
  builds read the base `stage/Interface` and `stage/title` files of `DRACRO.CPK`, which have no such atlases, so
  `base-*.dcpx.z` puts the same art into the sprites those files draw (NOW + LOADING becomes CARREGANDO) and moves their
  quads. In Asia/Hong Kong the text is in `DRACRO_CK.CPK` and the stage files in `DRACRO.CPK`, so two archives are
  overlaid. The manual stays the build's own on those builds: their pages are other images, and most of a page would
  have to be shipped. A visual whose installed file is not a known source is left as it is, and the log says so
  (`PT-BR visual skipped: stage/Interface.ftx: source fingerprint not recognized`); the others still apply.

  The Japanese and Asia/Hong Kong fonts have no accented letters, and their executables list none that the font draws.
  When Portuguese (Brazil) is selected there, `launcher/latin_font.cpp` draws the letters into free atlas cells of a
  copy of the installed font, from its base letters and marks made from `DRACRO.CPK`'s punctuation, and
  `src/latin_glyphs.c` adds them to the executable's glyph table in memory. If that cannot be done the text is shown
  without accents and the log says why. The Japanese and Asia/Hong Kong marks are composed on the player's machine;
  nothing of the game is stored by the port.

  Records of a slot that cannot take the translation safely stay in that slot's language and are counted in the log
  (`Menu & Subtitles: ... kept in the game's own language`); the rest of the sheet is translated. See
  `localization/pt-BR/README.md`.

  English on the Japanese and Asia/Hong Kong builds is provided by the PC port; the PS4 releases have none. The port
  ships the game's English text as its own corpus (`localization/en/sheets`, embedded at build time) and serves it over
  the Japanese slot or the Traditional Chinese one, like Portuguese (Brazil): the same `english` key plays native English
  on the USA and European builds and the built-in English on the others. A record that cannot map safely keeps the
  slot's own text, as does one whose English uses a character the slot never shows (Japan 11,970 and Asia/Hong Kong
  1.00 11,686 of 11,992 records). No other game or setup is needed. English uses the regional font as it is (no composed
  letters) and no visual patch: the regional labels (NOW LOADING, YES, NO...) are already English. Exit Game and the
  DefaultBone character messages follow the served text. See `localization/en/README.md`.

  Russian is the fan translation by Exclusive Studio, with PS4 porting work by AlchemistLab
  (https://ex-s.ru/dc.html), built into the port from `localization/ru`; no fan package is needed. It is served over
  the same slot as Portuguese (Brazil) and uses the English and Portuguese record maps on the Japanese and Asia/Hong Kong
  builds, plus a small delta of its own (USA 9,946, Japan 9,926 and Asia/Hong Kong 1.00 9,791 of 9,946 records). A
  record whose printf conversions or buttons differ from the slot's keeps the slot's text; tags and colours may differ,
  as the fan text writes some tag values out. No regional font has Cyrillic, so `launcher/cyrillic_font.cpp` copies the
  fan font's 66 letters into a copy of the installed font and `src/cyrillic_glyphs.c` lists them in the executable's
  glyph table: on the USA and European builds over the kana cells the fan package uses, with the text written as those
  kana, and on the Japanese and Asia/Hong Kong builds as Unicode in blank cells after the Portuguese letters. When the
  table or the font cannot take them, Russian is not served and the log says why
  (`Menu & Subtitles: Russian unavailable (...)`). The character messages are drawn by the system font, whose OpenType
  fonts have Cyrillic in every region. The launcher, the F1 options, the F10 overlay and the online screens stay in
  English: the fan translation has no text for them. Exit Game shows `Выйти`. See `localization/ru/README.md`.

  Korean on the USA and European builds is the game's official Korean translation, contributed by dyumadel
  ([PR #87](https://github.com/saulob/dragons-crown-pro-pc/pull/87)), built into the port from `localization/ko`. Its
  message sheets are the USA ones record for record, so each USA record takes the Korean at its index (11,953 records in
  13 sheets; the 39 challenge quests the Korean release never translated and the staff roll stay in English). It is
  served over the English slot like Portuguese (Brazil), and, as for Russian, a record must share only the slot's printf
  conversions and buttons. The USA and European fonts have no Hangul: `launcher/hangul_font.cpp` draws the 1,075
  syllables the text uses (drawn with Noto Sans CJK KR) into atlas cells only a kanji uses, as the official Korean font
  puts its Hangul where the Japanese font has kanji, and `src/hangul_glyphs.c` adds their code pages (U+AC00–U+D7A3) to
  the executable's glyph table after proving every such cell is listed by kanji only. When the glyph file, the font or
  the table cannot take them, Korean is not served at all: the launcher says why and the game keeps its English text
  (`Menu & Subtitles: Korean unavailable (...)`); no missing squares are ever shown. The character messages are drawn by
  the system font: while Korean is selected, the European set opens the embedded Noto Sans KR instead of Noto Sans. Every
  other language, system font set and the Asia/Hong Kong build's native Korean are unaffected. The launcher, the F1
  options, the F10 overlay and the online screens stay in English; the Korean release has no text for them. See
  `localization/ko/README.md`.

  For the USA, Asia/Hong Kong and European builds the list is read from the installed regional archive (`DRACRO_US.CPK`,
  `DRACRO_CK.CPK`, `DRACRO_EU.CPK`): a language is offered only when the archive holds its set of message sheets and stage files. Russian
  is offered wherever the native slot is complete, like Portuguese (Brazil); the 1.01 update's own Russian text in its
  US slot is not taken over (see USA builds). `DRACRO_EU.CPK` has a UK slot beside the US one, as `DRACRO_US.CPK` does; it is
  not offered as a separate language. When the archive cannot be read or holds no set, the build's list stands. A saved
  language the installed game does not have is replaced by its first one, and the log says so
  (`Settings: saved language Korean unavailable for this build, using Japanese`). An explicit `DCP_LANGUAGE`
  environment variable (a PS4 language number) overrides it. It never changes the spoken voices: Voice Language does.
- **Voice Language**: Game Default by default (`voice_language`: `default`, `english`, `japanese`). Every supported
  build. The game gives every voice cue a Japanese ID and an English twin 100000 above it, in banks that are all in the
  shared `DRACRO.CPK` (player classes `se_player1000_v` to `se_player1500_v` and `se_player101000_v` to
  `se_player101500_v`, camp voices `se_voice2900` to `se_voice2950` and `se_voice102900` to `se_voice102950`, NPCs,
  enemies, and the seven narrators `narration_gm20000` to `narration_gm80000` and `narration_gm120000` to
  `narration_gm180000`). Its cue mapper picks the twin by a mode the speaker passes: the USA and European executables
  pass English for every character, resurrected ones included, and the Japanese and Asian ones derive it from the
  game's language setting; no character record stores a language. English or Japanese sets that mode at the mapper's
  seven callers, which covers your characters, other players, resurrected characters and AI party members, NPCs,
  enemies and the camp voices, and points the narrator manager at the same narrator's bank in that language, so the
  narrator chosen in the game stays the same person. Each class keeps its own voice. Game Default installs nothing. The
  launcher offers a language only when the installed `DRACRO.CPK` holds the sound table and that language's banks
  (`launcher/voice_language_resources.h`); at game start a category whose banks are missing keeps the game's own voice.
  The narrator sites and tables are checked on their own: when they do not validate, only the narrator keeps the game's
  own voice and the other routes still apply. An extra reference on the chosen language's NPC and enemy banks keeps them loaded, as the game's own lifetime can drop
  the bank it does not expect. Nothing in the game files, the saves or online play changes; other players hear their
  own setting. It never changes the Menu & Subtitles language. The log names the result with `Voice telemetry:` lines:
  `route_ready` at start, one `capability_unavailable` line with a short reason for each category left off, then the
  first and every tenfold `route_used` count of each category and one `bank_route`
  line per voice family that changed language. The former `dcp_japanese_npc_voices` is read only while
  `voice_language` is absent: 1 resolves to Japanese, anything else to Game Default; the launcher and the F1 Options
  write only `voice_language`.
- **Frame rate** (30 FPS, 60 FPS (Original), 90, 120, 144, 165, 240 FPS, Unlimited; `frame_rate`, 60 FPS (Original) by
  default; applied at start, every build) sets how often frames reach the window. 60 is the game's own rate. 30 FPS
  shows every second frame. The game makes one frame per logic tick at 59.94 Hz, builds that frame's draw lists inside
  the last tick of the frame and holds itself to that rate, so nothing is changed in the game and a faster target does
  not make it faster: the video core keeps the 60 Hz vblank and, between two of the game's flips, shows the last frame
  again at the target rate (`RepeatLastFrame`, scheduled by the limiter in `gpu/shim/dcp_frame_pacing.h`). The extra
  frames repeat the last logic step, so motion is as smooth as at 60 FPS; there is no interpolation. Unlimited repeats
  as often as the display allows, up to 480 a second. With VSync on, presentations never go past the display's refresh
  rate, and a display at 60 Hz or less has nothing to repeat. Frame rate in title and the frame counter count every
  presentation. The log has the frame rate in its `Settings:` lines and `VideoOut: vblank ... frame rate ... repeat cadence ...
  VSync ...`.
- **Frame rate in title**: `Dragon's Crown Pro | FPS: 60.00`, measured from the presented game frames, refreshed about once
  a second on the window thread.
- **Render resolution in title**: `Dragon's Crown Pro | 1920x1080`, the render resolution of the game's presented image (not the
  window size). It follows a change of the game's resolution while it runs.
- **Frame counter**: the video core's corner readout (`show_fps`).
- **Arachnophobia Safe Mode**: Off / On (`arachnophobia_safe_mode`). Replaces the Giant Spider's atlas
  (`character/GiantSpider00.ftx` in `DRACRO.CPK`) with a redrawn one: a cream rear body, a cream front body with two
  orange eyes, no legs, fangs or claws; web and smoke effects are kept. The enemy's animations, hitboxes and behaviour
  are the game's own. The drawn atlas is `launcher/prompts/arachnophobia-spider-atlas.png` (the preview next to it is for
  looking at only); `scripts/prompts/draw_spider.py` draws both. Like the Controller prompts it is a block patch on the installed archive (`launcher/cpk_patch.h`,
  `launcher/game_patches.h`, `launcher/prompts/arachnophobia-spider.dcpx`) applied when toggled, in the background, and
  reapplied after a reinstall. An archive has one undo record, so the prompts and this patch are applied together:
  changing either reverts what the other left and writes both again.
- **Skip Intro**: Off by default (`skip_intro`; `DCP_SKIP_INTRO` overrides it for a run). At startup the game checks the
  save data (and, on a first start, asks to create it), then plays the opening scene, which holds the ATLUS, Basiscape
  and CRIWARE logos and the opening movie, then shows the title. Before creating that scene the game asks whether the
  opening is needed; with the setting on, the first answer at startup is "not needed", so the game goes from the save
  data check straight to the title without building the scene. The save data check and its prompt always run first.
  The title's idle replay of the opening and the return to the title from the game are unchanged. The Japanese builds
  also open with a legal notice about unauthorized distribution; with the setting on they take the step the other builds
  take there, so the notice is never built and the game goes on with its normal loading. Saves, the tutorial and the
  story are untouched, and it is separate from the two skip offers. It does not depend on the text language. The log
  shows `Skip Intro: installed build=<build> route=save_check->title` when installed (plus
  `Skip Intro: regional notice bypass installed` on builds that have the notice), then
  `Skip Intro telemetry: event=startup_redirect from=opening_logos to=title count=1` when the game took the route and
  `Skip Intro telemetry: event=title_reached count=1` when it created the title. Changing it in the F1 Options menu
  needs a restart.
- **Japanese Menu Buttons** (Controls page, under Controller prompts): Off by default (`japanese_menu_buttons`;
  `DCP_JAPANESE_MENU_BUTTONS` overrides it for a run). The setting picks the menu layout the same way in every build and
  with every text language. Off: Cross confirms and Circle cancels (A and B with Xbox prompts). On: Circle confirms and
  Cross cancels (B and A). The game has one "Cross confirms" setting that its key setup and some button pictures read.
  The USA builds start with it set, so On zeroes the two places that set it (the European build's are found by
  signature, not yet proven on its executable); the Japanese and the Asia and Hong Kong
  builds start with it clear, so Off makes their five readers see it set. Every site is checked byte for byte first and
  nothing is written on a mismatch (the log says why). The row's description names A and B instead of Cross and Circle
  when the Xbox controller prompts are installed, and follows the Controller prompts choice as soon as it is applied;
  the prompt style never changes the setting itself. The Exit Game row accepts with the same button (South when Off,
  East when On, and Enter). Combat, movement and the pad mapping are untouched. The log shows
  `Japanese menu buttons: on|off, ...`. Changing it in the F1 Options menu (Controls page) needs a restart. Prompts
  written into the game's text files are not rewritten, so some of them may keep showing the other button order.
- **Story Skip Offer** and **Tutorial Skip Offer**: Off by default (`story_skip_offer`, `tutorial_skip_offer`;
  `DCP_STORY_SKIP` / `DCP_TUTORIAL_SKIP` override them for a run). The game's own questions, asked for every new
  character: whether to start from the second half of the story (normally offered only once a class on the file has
  finished the game) and "Begin the tutorial?" (normally skipped for a file's first character, which starts the tutorial).
  Existing characters are never asked. On, one branch each is patched in memory after its bytes are checked
  (`src/skip_offers.c`); Off leaves the code as it is. Shown for every build.
- **Unlock Gallery** (Gameplay page, after the skip offers): Off by default (`unlock_gallery`; `DCP_UNLOCK_GALLERY`
  overrides it for a run). The title menu's Gallery lists 57 Treasure Art pictures and asks the save, through one getter,
  which of them are found (a 59 byte array: 0 not found, 1 new, 2 seen). On, only the call that builds the Gallery list
  is answered with a temporary copy of that array in which every picture is shown; the save itself, the code that
  records a found picture, the trophy count, the title screen's own reads and every other reader keep the real array.
  Pictures the save holds as new stay new, and opening one marks it seen as usual; a picture that is only shown is never
  marked, so turning the option Off, or restarting without it, gives back exactly what the save holds. It changes
  nothing about trophies, characters, quests or Online Play, so it is allowed online. Every site is checked byte for
  byte first and nothing is written on a mismatch (the log says why). The log shows
  `Unlock Gallery: on, 57 legitimate entries exposed (<build>)` when installed and `Unlock Gallery: off` otherwise.
  Changing it in the F1 Options menu needs a restart. The launcher hides the row for a build whose profile does not
  list the Gallery list builder; all eight supported builds do. The builder is found by its code, so a build whose code
  differs gets the log line `Unlock Gallery: requested but unavailable ...` and the Gallery stays as the save has it.
- **Enable Boss Battle** (Gameplay page, after Unlock Gallery): Off by default (`enable_boss_battle`). On, the
  Dragon's Haven Inn menu gains a Boss Battle row after Report adventurers; Off leaves the Inn menu as the game has it.
  The row opens the Boss Battle screen on the game's own Options page: Fight All at the top (selectable; the twenty
  bosses in menu order, one after the other: each Win puts the campaign back, keeps the character's experience,
  level and skill points, saves them through the game, and starts the next boss without showing the Inn or this
  screen, the screen staying black until the next arena appears; a Death, Quit, the twentieth Win, or a failed safety
  check ends the run and returns to the screen on Fight All), the twenty bosses in the selectable right column (six at
  a time, scrolling), the highlighted boss's attempts, wins, deaths, quits, win rate and best time in the left column
  (Fight All shows its runs, clears, deaths, quits, best progress and best time, the combat time of a full clear),
  and Return to tavern (or Back) at the bottom, all with the game's menu sounds; prepare the party
  first with Select player and Choose allies. Starting a boss sets the campaign aside: the save on disk is protected and
  the campaign state in memory is put back when the fight returns to the Inn, so no boss clear, story, route, item or
  gold change of a Boss Battle reaches the campaign. The character is the campaign's own: a fight starts at its current
  level and experience, and a win's experience, level-ups and skill points stay with it in the campaign, which the game
  saves right away. A win shows the game's experience award and level-up, then returns straight to the Inn, without
  the continue choice, items, General results or story; going down with no lives left (no paid continue in Boss
  Battle) and quitting (F4 or the pause menu) return straight to the Inn. In a fight Start opens the game's own pause
  menu with the boss's name as its title and one row, Quit Boss Battle (with the game's Yes/No question), for every
  character whatever its level or progress; Back resumes the fight. The screen then opens again
  on the same boss with the new numbers. The Inn menu is not shown between the screen and the arena, nor between the
  fight and the reopened screen; only Return to tavern and Back show it. Per character and boss, `boss_battle.dat` in the user folder keeps attempts
  (counted once the fight is playable), wins, deaths, quits and the best win time, and per character the Fight All runs,
  clears, deaths, quits, best progress and best time (an older
  `boss_battle_progress.dat` is no longer read). With Online Play on, only the host needs this option (as Early Story
  Online): a guest with a compatible version joins the host's Boss Battle room whatever its own setting, follows the
  host for that session and does not get the row afterwards. The F1 Options menu changes it the next time the
  Inn menu opens.
- **Unlock Labyrinth of Chaos** and **Unlock Ultimate + Tower of Mirages** (Gameplay page, after Enable Boss Battle):
  two independent options, Off by default (`unlock_labyrinth`, `unlock_ultimate_tower`; `DCP_UNLOCK_LABYRINTH` and
  `DCP_UNLOCK_ULTIMATE_TOWER` override them). Neither does anything until the character's own story has opened the
  Stables, by playing or through the game's Story Skip. From then on, Unlock Labyrinth of Chaos answers the game's own
  Labyrinth check, so the Stables list the Labyrinth from its first floor with the game's floors, rewards and saves.
  Unlock Ultimate + Tower of Mirages lets the Temple offer the game's own "Change difficulty to Ultimate" prayer (and
  "Change difficulty to Normal" while the character is on Ultimate); on Ultimate the game's Labyrinth entry is the
  Tower of Mirages, so that option also opens it, and only then. Nothing is marked as cleared; what the character earns
  is saved by the game as usual. The difficulty the Temple sets is the game's own saved choice: a character left on
  Ultimate stays on it after the option is turned Off, so change back to Normal at the Temple first. Every site is
  checked byte for byte and nothing is written on a mismatch; the log shows
  `Endgame unlocks: Labyrinth on|off, Ultimate + Tower on|off, after the Stables open (<build>)`. Changing them in the F1
  Options menu needs a restart.
- **Allow overlay programs**: Off / On (`allow_overlay_programs`, Off by default; Windows only, hidden elsewhere). For
  recording, chat, store and performance overlays and other tools that inject into games. On, PLAY starts the game as a
  separate process from the launcher (see Architecture), so these tools attach to a clean game process. Off keeps the
  usual start in the launcher's process. Neither setting filters Vulkan layers or blocks any program; a
  `VK_LOADER_LAYERS_DISABLE` set in the environment reaches the game as it is.

No skip-intro.

## Title menu

The title menu has four rows, in this order: Start Game, Options, Gallery and Exit Game. The game's fifth and sixth rows
(Upload Save Data and Download Save Data, the PlayStation Network save transfer) are not shown, because that service does
not exist in the PC port; their code stays in the game and is simply never reached. Exit Game is the game's own spare
PlayStation Store row with SystemMsg text in the selected game language (`src/exit_game_text.c`); the game only draws it,
and the port closes the game when the game's confirm button (from the controller or its keyboard key; Circle with the
Japanese menu buttons) or Enter is pressed while it is selected.

The menu is made of the game's own pieces (`src/exit_game_hook.c`, `src/title_layout.c`): the title adds six items and
builds five row widgets, then cuts the list to five. The port builds one row widget fewer, adds the items in the order
Start Game, Options, Gallery, Exit Game, Upload, Download and cuts the list to four; the menu result, the idle layout part
and the help text of each row then follow the position of its action. The art is the game's `title.mbs`: it lays out six
rows, so the loaded layout's parts are rewritten (once, from the part setter, after checking every byte they replace)
to four row plates, with the selected-row strip and highlight of Options, Gallery and Exit Game moved up to their rows.
`scripts/title_layout_patch.py title.mbs ...` regenerates and checks that patch for any language's layout. Every site is
checked byte for byte first; nothing is written to the game's code on a mismatch, and a title layout that is not exactly
the known one is left as it is (the log says `Exit Game: title layout differs ...`).

## Cheats

Sidebar order: Play, Display, Graphics, Gameplay, Online, Cheats, Controls, Trophies, About. Subtitle "Optional gameplay modifiers", with the
line "Press F10 while playing to open the Cheats menu. Changes made there apply only to the current session. Meant for
offline and local play." and Off / On
rows in this order: **Infinite Health**, **Infinite Ally Health**, **Infinite Mana**, **Infinite Ally Mana**,
**Infinite Lives**, **Infinite Durability**, **Infinite Ally Durability**, **Infinite Arrows**, **Infinite Skill
Points**, **Infinite Skill Uses**, **Infinite Items**, **Infinite Ally Skill Uses**, **Infinite Ally Items**, **Infinite Gold**, each with its one-line description (`src/dcp_cheats.h`), and
below them the **Movement Speed** combo
(Off, 2x, 4x), the **Experience Multiplier** combo (Off, 2x, 5x, 10x, 100x), the **Damage Taken** combo (100%, 75%, 50%, 25%, 0%) and the **Damage Multiplier** combo (Off, 2x, 5x, 10x, 100x).
Cheats work on every supported build: the tables of CUSA10454 1.00, or the sites found by signature on the others. All
are ordinary saved settings, Off by default: Save and Play keep them, Reset all settings puts
them back to Off. The saved values are the start of every game session; the F10 menu changes the running session only
and never writes them. The hooks, the menu and the player targeting are in [CHEATS.md](CHEATS.md).

## Controls

Xbox controllers work without setup; the keyboard is always on. SDL offers a device as a gamepad only when it has a mapping, so a device without one (a remote-play virtual pad, for example) is logged once per connection as `Runtime: joystick without a gamepad mapping: name (guid ..., vid:pid)`. A line for that GUID in `gamecontrollerdb.txt` in the user folder (the format of SDL's `SDL_GameControllerDB`) makes it a gamepad at the next start; `SDL_GAMECONTROLLERCONFIG`, when set, keeps priority over that file. Under Wine/Proton (`wine_get_version` exported by `ntdll.dll`) the game process sets `SDL_JOYSTICK_HIDAPI=0` before SDL's gamepad subsystem starts (`src/dcp_sdl_compat.h`, called from `loader.c` ahead of the build lookup; issue #42, Bazzite Gaming Mode), for every supported build and language; an explicit `SDL_JOYSTICK_HIDAPI` is kept, and native Windows and Linux are unchanged. Whether Steam Input must also be off there is untested. The page lists the layout implemented in
`runtime_pad.c` (W A S D and the arrow keys for the sticks, Space, Left Shift, E, Q, 1, 3, R, F, Z, C, I K J L, Enter,
Tab, Backspace) and that F10 opens the Cheats menu in the game (it also has the one-shot **Add 100 Skill Points**, **Add 1,000 Gold** and **Level Up (+1 Level)**, which are not settings). The dead zone is not a setting.

The page has two sections. **Keyboard controls** are the PS4 inputs (the sticks' four directions each, the face buttons, L1/R1/L2/R2/L3/R3, the D-pad, Options and the two touchpad sides); **Port hotkeys** are the Cheats menu, Window mode, Comms Wheel, Exit Game and Character Highlight keys (Exit Game F9 and Character Highlight F7 by default). A third section, **Controller shortcuts**, lists the fixed L3 + R3 Options menu chord, **Controller Exit Shortcut** (On by default: hold L1 + R1 + Options on one controller for 2 seconds to close the game), **Controller Comms Wheel Shortcut** (`View / Back + D-pad Up` by default, or `L1 + D-pad Up`, `R1 + D-pad Up`, legacy `D-pad Up`, or `Off`), and **Character Highlight**. Click a keyboard row (or focus it and press Enter), then press the new key; Esc cancels and is never bound. The controller shortcuts are choices on their rows. **Reset to defaults** puts these keys back and changes no other setting.

Exit Game shortcuts (`gpu/shim/window.cpp`, policy in `src/dcp_exit_shortcut.h`): the key closes on its press, never on a held key's repeats, and also works with the F1 and F10 menus and the game's dialogs open; typing a name never reaches it. The controller chord is judged for each controller alone, with a monotonic clock: all three buttons must stay down 2 seconds, releasing one restarts the count, and losing focus, unplugging the controller or turning the setting Off ends it; one hold gives one request. While the three are down the game sees none of them, and buttons still held afterwards stay hidden until released. Both routes call `dcpgpu_request_close()`, the function behind the title's Exit Game row, so the log shows `Exit Game: host exit scheduled` then `Exit Game: hard exit`. Neither depends on the game build, language, Online Play or Steam.

Each binding is one line in `dcp.ini`: `key_cross = space`, `key_left_stick_up = w`, `hotkey_cheats_menu = f10`, `hotkey_window_mode = f11` (the full list is `src/dcp_keys.c`). The value is a key name, case-insensitive: the shadPS4 names for common keys (`space`, `enter`, `lshift`, `rctrl`, `pgup`, `kp5`, ...) or SDL's own key names (`F10`, `Left Shift`, `Keypad 5`); the launcher writes lowercase. A missing or invalid line is the default, so a config from an older build keeps the old layout. The same key may be bound to several controls and all of them follow it; a port hotkey is not exclusive of a game control either. The bindings are read when the game starts (`dcp_keys.c`, shared by the launcher, `runtime_pad.c` and the video core); the controller mapping and `DCP_PAD_MAP` are unchanged, and `DCP_KEY_MAP` still overrides the buttons it names.

**Mouse** (the Controls page's **Mouse** section, below the keyboard controls; **Mouse defaults** puts these back and changes nothing else). While Mouse Controls is On, the mouse works together with the keyboard and the controller and belongs to Player 1.

| Row | `dcp.ini` key | Default |
| --- | --- | --- |
| Mouse Controls | `mouse_controls` | `1` (On) |
| Left Mouse, Right Mouse, Middle Mouse, Mouse 4, Mouse 5 | one `mouse_<control>` line per control, below | Square on Left, Circle on Right, nothing else |
| Pointer Control Mode | `mouse_pointer_mode` | `hold` (`hold` or `always_on`) |
| Pointer Control | `mouse_pointer_control` | `middle` |
| Pointer Interaction | `mouse_pointer_interact` | `left` |
| Steam Deck Trackpad (Steam Deck only) | `mouse_steam_deck_trackpad` | `auto` (`auto` or `off`) |
| Mouse Sensitivity | `mouse_pointer_sensitivity` | `100` |
| Wheel up / Wheel down | `mouse_wheel_up` / `mouse_wheel_down` | `none` |

A mouse button is saved as `left`, `right`, `middle`, `x1` (Mouse 4), `x2` (Mouse 5) or `none`; `mouse1` to `mouse5` are read too. There is one row per mouse button (Left Mouse, Right Mouse, Middle Mouse, Mouse 4 and Mouse 5) showing the PS4 control it presses; choose another from its list, or `(none)` to unbind it. A control is saved by the button that presses it, as `mouse_<control> = <button>`, with the same control names as the keyboard's `key_<control>`: `square` and `circle` (the 1.4.0 lines, unchanged), `cross`, `triangle`, `l1`, `r1`, `l2`, `r2`, `l3`, `r3`, `dpad_up`, `dpad_down`, `dpad_left`, `dpad_right`, `options`, `touchpad_left` and `touchpad_right`; the port's own hotkeys are not offered. The launcher gives a button one control and moves a control from the button that had it; a hand-edited file may give one button several, and all of them press. A line that is missing or not usable keeps its default, and no line is ever read as another control. The Pointer Control and Pointer Interaction rows are chosen by pressing the button (Esc cancels, **Clear** unbinds Pointer Control). In Hold mode the Pointer Control button's own row shows its role, and in Always On the Pointer Interaction button's does, because a control on either would never press; the saved controls are kept. **Pointer Control Mode** is Hold or Always On (`hold` or `always_on`); the Pointer Control row is dimmed in Always On and its button is kept for Hold. Pointer Control and Pointer Interaction cannot share a button, and the interaction cannot be unbound; a saved file that breaks this reads as the two defaults. Mouse Sensitivity is a slider from 25 to 200 percent in steps of 5, saved as an integer; a value out of range clamps, a value between steps rounds, and a malformed one is 100. The **Default** button beside the slider sets only the sensitivity back to 100 (not Mouse Controls, the bindings or the wheel), and the slider is dimmed with the other mouse rows while Mouse Controls is Off. The game's log says `Mouse controls: on, sensitivity N%, pointer control hold, N extra buttons` (or `always_on`) or `Mouse controls: off` at start. The F1 Controls page has Mouse Controls, Pointer Control Mode and Mouse Sensitivity as live rows; the wheel keys take `none` or any control name above, and a wheel step holds that control for four game reads. A bad or empty value is the default. **Mouse Controls** is the first row. Off makes the game ignore the mouse completely (no buttons, wheel, Pointer Control, finger or mouse capture, and a moving mouse no longer shows the cursor in the game); the launcher, the Options and Cheats menus and the game's dialogs and text entry keep using the mouse. The other rows stay visible, dimmed, and keep their values. Turning it Off while Pointer Control is held releases the finger, the capture and every held button at once. The Options menu's Controls page (F1) changes it, Pointer Control Mode and Mouse Sensitivity live; the button and wheel bindings apply the next time the game starts.

Steam Deck Trackpad appears only on a Steam Deck. Auto, in Gaming Mode, drives the game's pointer from the trackpad like Always On with no button held, and the trackpad click is the Pointer Interaction; Off keeps ordinary mouse behavior, for a mouse plugged into the Steam Deck (see `docs/LINUX.md`).

A physical touchscreen is not a mouse: it is the DualShock 4 touch surface (a contact is a touchpad contact, a short tap a touchpad click), including under SteamOS / Gamescope (see `docs/LINUX.md`).

The Touchscreen section of the Controls page, independent of Mouse Controls, has one setting, Touchscreen Mode (`touchscreen_mode`, applied at the next game start):

| Mode | Saved value | Behavior |
| --- | --- | --- |
| PS4 Touchpad (default) | `ps4_touchpad` | dragging the screen moves the game's pointer like the DualShock 4 touchpad |
| PS Vita Direct Touch | `ps_vita_direct_touch` | the game's pointer is placed under the finger and follows it; a quick tap is a touchpad click there |

A missing, empty or unknown value is PS4 Touchpad. The touchscreen is player 1's: only the controller port that carries player 1's pad gets Direct Touch, so another local player's touchpad keeps its own mode. The mode only affects physical touchscreens: DualShock 4 and DualSense touchpads, Steam Deck trackpads and the mouse are unchanged, and the game's own Touch Pad Cursor Control option is never modified (Disable still disables it). Direct Touch uses the game's own Absolute position mode and is installed only where the locator finds the pointer update exactly once; otherwise the log says so and the touchscreen stays a PS4 Touchpad.

Behaviour in the game: in Hold mode, moving the mouse alone does nothing. A button presses its PS4 control (Square on Left Mouse and Circle on Right Mouse by default) while it is down, and a Touchpad left or right control clicks that side of the pad. While the Pointer Control button is held the game window captures the mouse (relative mouse mode, cursor hidden) and the movement becomes one synthetic finger on the PS4 touch surface (1920 x 943, as the port reports it), so the game moves its own pointer exactly as it does for a finger on the touchpad; nothing is drawn by the port. The finger lands in the middle of the surface when the button goes down; when it leaves the middle three fifths it lifts for one game read and lands in the middle again, which the game takes as a new finger and moves the pointer by nothing, so the pointer never jumps and the mouse never runs out of surface. The Pointer Interaction button presses the touchpad button while the pointer is held, and neither it nor the Pointer Control button presses Square or Circle until it is released. Releasing the button lifts the finger on the next read. Focus loss, the Options and Cheats menus, a text dialog, a Windowed and Borderless switch, closing the game, or a touch on a controller's own touchpad end Pointer Control and release the mouse; the touchpad case stays off until the Pointer Control button is released and pressed again. Controller buttons, sticks and triggers never end it. Mouse input is ordinary pad data, so Online Play needs nothing extra. In **Always On** the same synthetic finger follows the mouse whenever the game has the mouse and Mouse Controls is On, with no button held and the same sensitivity, so a trackpad that SDL or Steam Input exposes as a mouse (the Steam Deck's) moves the pointer directly; the window keeps relative mouse mode on, so the edge of the window never stops it. The finger lands after one read with no finger, so the game sees a landing and not a jump. The Pointer Interaction button takes its click as in Hold, always, and the Pointer Control button is an ordinary button. Focus loss, the Options and Cheats menus, a text dialog, the Comms Wheel's mouse session and the 0.3 s after a Windowed and Borderless switch pause it and release the mouse, and it resumes by itself when the game has the mouse again; turning Mouse Controls Off releases everything. A finger on a controller's touchpad, or a touchscreen contact, takes the touch surface at once and the pointer lands again, recentred, after the last finger lifts. The game's log records which of these happened: `Mouse telemetry: event=...` lines for `mouse_extended_binding_used`, `pointer_control_always_on_used`, `pointer_control_suspended` (with `reason=blocked`, `controller_touch` or `disabled`), `pointer_control_resumed` and `mouse_binding_invalid_fallback`, each with a count, the first time and every tenth power; no positions, movement, devices or text are logged.

**Local multiplayer**: up to four SDL gamepads, player N on pad handle N and user N (`runtime_pad.c`, `runtime_services.c`).
A gamepad that is found takes the lowest free slot and logs its user in once; the game opens that user's pad on the
`LOGIN` event. The keyboard, `DCP_PAD_FILE`, pad recording and replay and the save list's controls belong to Player 1
only. Unplugging a gamepad closes it and frees its slot, but no `LOGOUT` is sent: the player stays logged in, and the
next gamepad connected takes the lowest free slot, which is not necessarily the same player. A fifth gamepad is ignored.
The cheats act on every local human player, Players 1 to 4 (Infinite Gold, which is one pool for the save, excepted); the five Ally cheats act on AI-controlled party allies only, never on a human Player 2 to 4. Fill Health, Set Low Health, Add 100 Skill Points and Level Up (+1 Level) stay on Player 1; Online Play turns every cheat Off. The Experience Multiplier applies to every player's experience award; see [CHEATS.md](CHEATS.md).

**Controller prompts**: PlayStation (original) or Xbox. The game draws its button prompts from one 1024 × 512 BC7
atlas (`Interface_arranged000`) in `stage/Interface.ftx` and in each language's `stage/Interface_XX.ftx` (DE, ES, FR, IT,
UK and US, the same bytes in all of them), and the port has no way to swap textures at run time, so the choice is made
in the installed game files: Xbox applies a block patch to the archives that hold a button atlas, `DRACRO_US.CPK` and
`DRACRO.CPK` for the USA builds and `DRACRO.CPK` alone for the Japanese and Asia and Hong Kong builds
(`launcher/controller_prompts.h`, `launcher/cpk_patch.h`), PlayStation reverts it. USA 1.01 offers Xbox only while its
finished archives hold every file the patch was made for (or its patched result): each is read and its CRC checked when
the launcher starts or the build changes, and an update that changed one of them shows PlayStation only, with nothing
written. The Japanese and Asia and Hong Kong 1.02 updates and the European build, whose atlases were never read here, are checked
the same way, and an archive holding a button atlas
the patch does not cover is refused. Their `stage/Interface.ftx` is the same file as the USA one; the translated
touchpad wording of the Western message sheets has no counterpart in their own sheets.
The patch (`launcher/prompts/xbox-prompts.dcpx`, compiled into the executable as a byte array by `launcher/embed.cmake`) holds the
4 × 4 blocks that differ between the game's atlas and the repainted one, about 2 MB, with the CRC32 of each pack
before and after, and the repainted glyphs sit in the same rectangles as the game's, so the sprite definitions
(`Interface_XX.mbs`) are untouched: Circle, Triangle, Cross and Square become B, Y, A and X in the Xbox colours, L1/R1
and L2/R2 become LB/RB and LT/RT on the game's own shoulder shapes, START/OPTIONS and SELECT/SHARE become MENU and VIEW,
the touchpad and its TOUCH PAD word become VIEW (the port maps View to the touchpad button, `src/runtime_pad.c`), the
PS-logo discs become a plain guide ring, and the finger-cursor hand stays. The translated message sheets
(`msgsheet/SystemMsg_XX.fms`, DE, ES, FR and IT) take the glyph the English text has where they name the touchpad button
(same length, so the patch stays a block patch, and every other string is untouched). `scripts/prompts/README.md`
describes how the patch is made.

Applying is a background task on the page (a few seconds; Play waits for it): each pack is read from the archive,
decompressed (CRILAYLA), checked against the patch's CRC, patched, written uncompressed to the end of the archive (the
archives grow by about 250 MB and 31 MB) and its table-of-contents row pointed at the copy; the original data is not
overwritten. The original TOC and header bytes and the original length go to `DRACRO_US.CPK.undo` and
`DRACRO.CPK.undo` next to the archives, and PlayStation puts them back byte for byte. The header's `EnableTocCrc` flag
is cleared so the rewritten TOC is accepted. The page shows what the archives hold, not the setting: an install counts as
Xbox only when its patched entries are this patch's files and no others, so an install patched by an earlier patch shows
PlayStation and is restored and patched again. When the launcher starts, and after a reinstall from the Play page, a
saved Xbox setting applies the patch again, and a patch left by an earlier build is removed even when PlayStation is
selected. A pack that is not the one the patch was made for is skipped and named in the log.
An archive that is a hard link to the game folder the install was made from (same drive) first gets a file of its own,
so that folder is never changed. If a step fails, the archive is put back as it was; an undo record that does not belong
to the archive, or an interrupted earlier run, is detected before anything is written.

## Saves

Subtitle "Your SAVE0.DAT files: overview, backups, export, import and restore.". The page sits between Gameplay and Editor and
manages whole SAVE0.DAT files; the Editor keeps characters and their appearance. `launcher/save_page_model.cpp` holds what
the page shows and does, without ImGui; `launcher/save_page.cpp` draws it with the TRG rows.

- **Installed game** names the configured build (the one the Editor uses). Only the saves of that build's Title ID are shown
  (`SavesOfTitle`, shared with the Editor); there is no way to pick another Title ID, and without an installed game the page
  says so. **Selected save** appears only when that Title ID has more than one save folder (SAVEDATA00, SAVEDATA01...) and
  chooses among those; choosing only reads. The first open shows **Loading saves...**; every later entry reads the saves and
  backups again.
- **Overview**: Platform, Save folder, Status, CRC, DATA slots, Characters, Modified and File size. Status is Valid,
  Unreadable, Invalid (size or magic) or Corrupt (CRC); the full path is under **Details**.
- **DATA Slots**: read-only; each slot's characters (name, class, level) and gold, or Empty. **Open Editor** goes to the
  Editor page, where characters are edited, when the selected save belongs to the installed game.
- **Create backup** validates the save and copies it to `user/save_backups` as `SAVE0_<timestamp>_<title>_<folder>.DAT` with
  a `.meta` beside it; a name that is taken gets a numbered suffix, so nothing is overwritten. The list under **Backups** shows
  the selected save's backups, newest first.
- **Restore...** and **Import and replace...** ask through `Ui::ConfirmRow`, naming the backup or source and the save they
  replace. They validate the new bytes, back the current save up, write through a validated temporary file renamed over the
  save, and read it back; any failure leaves the old save as it was. Afterwards the page reloads and the shared
  `SaveSelection` generation changes, so the Editor reloads before it can write again.
- **Export PS4 save...** chooses a folder (TRG has no Save File dialog) and writes `SAVE0.DAT` there byte for byte, then reads it
  back and validates it. A file already there is replaced only after a confirmation; the save's own folder and the
  launcher's `savedata` folder are refused, because a copy there would replace a save without its backup.
- **Import PS4 save...** and **Import PS3 save...** are two entry points (`ImportMode::kPs4`, `ImportMode::kPs3`) into one
  inspection and commit (`PlanImport`, `CommitImportPlan`). The PS4 mode takes a SAVE0.DAT, a folder (up to four levels
  down) or a ZIP archive and refuses a PS3 source with "This is a PS3 save. Use Import PS3 save."; the PS3 mode takes a PS3
  save folder or ZIP and refuses a native one with "This is a Dragon's Crown Pro save. Use Import PS4 save." The mode never
  switches by itself. The preview (`InspectImport`) shows:
  source and type, platform, region, Title ID, format, SAVE0.DAT status, whether a conversion is required, DATA slots,
  characters, the save it replaces and that a backup is made first. The Title ID comes from PARAM.SFO (`SAVEDATA_DIRECTORY`
  on PS3, `PARENT_DIRECTORY` on Vita), else from the folder names. A file is accepted only when it passes `ValidateSave`
  (size, magic, CRC); its name alone is not enough, and several different SAVE0.DAT files in one folder or archive are
  refused as ambiguous. A source that cannot be imported still gets its preview, with the exact reason.
  - **PS3**: imported after an in-memory conversion. `save_ps3.cpp` reads PARAM.PFD (version 4; a 64-byte-aligned entry table whose
    entry holds the file's logical size, an encrypted file key and an HMAC-SHA-1 of the stored bytes), checks the entry hash,
    unwraps `SAVE0.DAT` in memory with Dragon's Crown's per-game key (the eight save-directory IDs it knows: `BLUS30767`,
    `BLES01950`, `NPEB01836`, `NPUB31235`, `BLJM61041`, `BCAS20298`, `BCAS20290`, `BCAS20288`) and checks the result: 2,157,336
    bytes, `DCSD` at offset 4, and a big-endian CRC-32 at offset 0. No console ID is needed. A save somebody already decrypted is
    converted the same way. A `SAVE0.DAT` alone cannot be read (`kPs3NeedsPfd`); a PFD that is truncated, unsupported or
    inconsistent (`kPs3PfdInvalid`), a mismatching hash (`kPs3HashMismatch`), another game's save (`kPs3UnknownTitle`) and a result
    that is not a save (`kPs3Undecryptable`) are each reported.
    `ConvertPs3ToPro` (`save_ps3_layout.cpp`) accepts layout version 36 only (header counts 490 and 532); any other version is
    refused as `kPs3UnsupportedLayout`. The PS3 file is a big-endian image of the same structures the Pro game keeps, so the
    conversion rebuilds the Pro file field by field instead of reversing the whole file: a 127-node program of leaves and
    nested repeats describes one DATA slot (264,641 fields of one, two or four bytes), applied to the three slots; the 59 trailer
    bytes, the six-byte struct and the play-time dword after the slots are converted by a small table; the header is the Pro
    header (version 38, counts 660 and 979); the CRC-32 is the Pro one, little-endian. An item record's packed 32-bit word
    (25, 4, 1, 1 and 1 bits) is re-packed because the PS3 compiler allocates bitfields from the most significant bit and Pro from
    the least. Padding is zero: every byte the program does not name is alignment padding or a constant `0xCC` fill, and the
    original game's own cross-save does the same. The result must pass `ValidateSave` before it is kept; the PS3 plaintext
    and the converted save live in memory only until the import commit writes the Pro save, after the usual backup, atomically.
  - **Vita**: accepted only when SAVE0.DAT is decrypted and passes `ValidateSave`; the preview warns that no Dragon's
    Crown Vita save has been verified. An encrypted one is refused.
  - **ZIP** (`save_zip.cpp`): read in memory, nothing is extracted to disk. Absolute paths, `..`, drive letters, control
    characters, ZIP64, encryption and methods other than stored and deflate are refused; entry count, per-file and total
    sizes, and the archive size are limited, and each entry is checked against its declared size and CRC-32.
  - **RAR**: recognized by its signature and refused with "extract it first"; import the extracted folder instead.
- The Saves page has no editing controls: characters and the DATA slot's Gold are edited on the Editor page (`SaveEdit` stays the Editor's model).
- Mutations follow `ActionStateFor`: nothing for an invalid save; Restore and Import wait while the Editor has unapplied
  changes; nothing while Play is starting. The launcher closes when the game starts, so the page never runs beside the game
  it started.
- Restore and Import replace only `SAVE0.DAT`. The Editor's own files, `user/character_appearance.dat` and
  `user/character_overrides.dat`, are left in place; an override is applied only while its character's identity is still
  found in the save.

## Trophies

Subtitle "Your trophies on this PC.". Shows the installed game's PlayStation trophies: "12 / 49 unlocked  24%", a progress bar,
the count per grade, then one row per trophy (icon, name, grade, description; unlocked rows are highlighted with their date).
A hidden trophy that is still locked shows "Hidden Trophy" and no description. Without an installed game the page says
"Install a supported game to view its trophies."; when the list is encrypted and no valid Trophy Key is configured it shows Enter Trophy Key and Import from shadPS4... buttons, and with a key a "Trophy Key: Configured" row with Change... and Remove. The list scrolls inside the page.
The page reads the same files the game writes. See [TROPHIES.md](TROPHIES.md).

## Bug Report

Subtitle "Report a problem and send it with one game log.". The page sits before About, keeps its content clear of the page
scrollbar, and has four steps:

1. **Report the problem**: 10 to 500 characters, with a live count; spaces around the text do not count toward the minimum.
   The text stays while the launcher is open, also after a failed send.
2. **Choose one game log**: this launcher's own log first, then the newest logs in the logs folder by modification time, five
   at most, each with its date and size, and one radio choice. Left out are other files and sessions that closed in the launcher
   without starting the game, so opening the launcher does not push a real log off the list. A session that ended without
   closing the launcher stays: that is a launcher crash. **View selected log**, in the empty cell of the selected log's details, opens it.
   The launcher's own log is labelled **Current launcher session**. The newest other log in which the game's own code ran
   (`Runtime: guest thread`) is labelled **Last gameplay**. The launcher closes when the game starts, so a running log holds no
   gameplay today; if one did it would read **Current session · Launcher + gameplay** and no other log would be Last gameplay.
   Every other log that reached the game's code reads **Gameplay session**, and every other launcher-only log reads
   **Launcher only**; a session that started the game without reaching its code has no label. Every label is
   drawn in the theme's accent colour.
   A log whose own minidump sits beside it (`dragons_crown_pro_NNN.dmp`, Windows only, see Crash dumps in
   [ONLINE.md](ONLINE.md#crash-dumps-in-bug-reports)) also reads **Crash detected**, and an **Attach crash report** box appears under the
   log's details, on by default when the dump can be sent. The page has no file chooser: only that paired dump can go, it
   explains that the dump may contain process memory used for diagnostics, and a dump that cannot be sent (over 8 MiB, not a
   minidump) turns the box off and sends the log alone. A Linux session, or any log without a matching dump, shows neither.
   The newest log that started the game is selected by default (`DefaultGameLog`), so a game that failed before its code ran is
   chosen over an older session that played. Without one, the newest log other than the launcher's own; failing that, its own.
3. **Preview** (optional): **Open preview** builds the report (`BuildIncidentPackage`, `launcher/incident_report.cpp`) and shows
   it in full; **Close preview** hides it. The last 256 KiB of the log are filtered (`launcher/log_scrubber.cpp`), then its last
   64 KiB are kept, from a whole line on. Changing the description or the log closes the preview, and opening it again rebuilds it.
4. **Send report**: sends the open preview, or builds the same report through the same path when none is open. A description that
   is too short does not send: the field takes the focus, flashes, and "Please describe the problem, minimum 10 characters" appears beside the
   count. The page scrolls down to the sending message and again to the result, which stays on the page: the Report ID, or a rate limit, connection, refused or service message; after a failure
   the text stays and Send report tries again. With **Attach crash report** on, the preview adds `Crash report: <name> (<size>,
   sent as a file, not shown here)` and never the bytes, and the box closes an open preview when it changes. The dump follows
   the report under the same Report ID, read from disk in 64 KiB chunks. If it fails after the report was filed, the result says
   so and **Retry crash report upload** sends it again with the same Report ID and receipt, resuming where the service stopped;
   it never files the report a second time.

5. **My reports** lists the reports this PC sent, newest first, from `user/bug_reports.json` (`launcher/report_history.cpp`, saved
   through a new file moved into place; a damaged file is set aside as `.damaged`). Each row shows the report ID, a preview, the
   status (with the fixed version on a Fixed report) and the date. **View details** expands that report in place, as one framed
   item that holds its summary, the developer's message when there is one, the player-test question while the report is Awaiting
   player test (see Player test feedback in docs/ONLINE.md), its facts (status, fixed version), **What was sent**,
   which reads the stored report back, and **Remove from this PC**, which deletes only the local entry. Only one report is open at a
   time, and **Close details** closes it. **Refresh** sits at the top right of the section.
   The statuses are checked at startup, every time the page is entered and on **Refresh**, off the UI thread and silently when
   offline; a check already running is not repeated, and the saved reports stay on screen meanwhile. The first time the page
   opens it shows **Loading Bug Report...** (the Character Editor's centered text and spinner, shared in `loading_view.h`) while
   the game logs are listed in the background. The Editor and Trophies pages show the same view, **Loading Editor...** and
   **Loading Trophies...**, on their first open, when they read saves and the trophy pack. A
   change marks the report **Updated** and puts a dot on the sidebar entry, until that report is opened. Opening the page
   does not clear anything: each report clears when it is opened. If the list cannot be saved the report still counts as sent:
   the Report ID shows normally with a short note.
   A new or changed developer message is a **new reply**, kept as `message_unseen` in `bug_reports.json` (an older file without
   it loads safely: an unseen report with a message counts as a reply). A reply shows a **New reply** marker in its row and a
   banner at the top of the page, "A report has a new reply", which comes before the plain "A report has an update". **Show**
   opens the report with an unseen reply, otherwise the most recently updated one, and the next unread report can then take the
   banner. The message sits in its own highlighted panel, **Message from developer**, apart from the status, with a **New reply**
   badge until the report is closed. A status with no message is only an update.
   A report the service no longer has (the admin deleted it) is marked `unavailable`: its row says **No longer available**, its
   details keep the last known status and message and say it was removed, it is never shown as an update, and it is left out of
   the automatic checks. **Refresh** asks about it again, and the mark clears if the service answers. **Remove from this PC**
   deletes the local entry as before.

The request and the privacy rules are in [ONLINE.md](ONLINE.md#bug-reports). Online Play does not have to be on.

## Launcher input

Mouse, keyboard (arrows, Enter = Play, Alt+F4 = Quit, Ctrl+S = Save) and controller (D-pad or left stick, A, B, Start =
Play). The launcher closes before the game starts, so its input cannot reach the game.

## Window and display scaling

SDL3 runs the process per-monitor-v2 DPI aware (also declared in the manifest). The launcher is 1280 × 720 logical (at
least 900 × 610, the height at which all thirteen sidebar pages fit at the compact row), scaled by the display's content
scale and capped to 90% of the usable area, so at 225% it is 2880 × 1620 pixels on a
4K display. Window sizes are real pixels. A saved launcher position that no display shows any more is ignored and the
window is centred on the primary display.

Sizes are logical pixels, kept together in `trg::layout` (`gpu/patches/trg-launcher/0006-compact-layout.patch`): text sizes,
the 34 control height, row spacing, and the header, sidebar and footer. Geometry is multiplied by `Ui::scale()` and text
is scaled by `PushFont`, so there is no second scaling path. A row's control sits in a fixed right-hand column
(`ControlColumnWidth`), and `Ui::RowLeft` and `Ui::RowControl` split a row that needs extra lines under its help text.
The update banner takes its place from the same header height.

Sidebar icons are the SVGs of `assets/launcher/icons`, compiled in by `launcher/embed_files.cmake`. `launcher/page_icons.cpp`
maps each `PageId` to its file and optical scale and rasterizes with nanosvg at the exact pixel size; the launcher caches one
texture per page and size, and the sidebar tints it. A page whose icon fails keeps its row, with one line in the log.

Sidebar labels and footer text are centred on their capitals, measured from the active font, so Segoe UI and DejaVu sit
the same way. The footer's shortcuts come from `LauncherConfig::hints`, two spaces inside a shortcut and more between
shortcuts, and its helper text from `note`, one sentence per line. The metrics are in `trg/layout.h`
(`gpu/patches/trg-launcher/0008-sidebar-footer-alignment.patch`), with `Theme::border_row` and `Theme::border_panel` for
passive lines (`0009-visual-polish.patch`); interactive controls keep their stronger colours.

In a window too short for every page, even at the compact row, only the sidebar's page list scrolls, with the launcher's
usual scrollbar in place of its right padding (`0019-sidebar-scroll.patch`). The wheel over the sidebar scrolls it, the
rows are keyboard and controller targets that ImGui keeps in view, and the current page is brought into view when it
changes or the window is resized; otherwise the scroll stays where the player left it.

A frame draws one page: `GoToPage` only records the request and `Launcher::Frame` applies it before anything is drawn
(`gpu/patches/trg-launcher/0007-coherent-page-frames.patch`). Each page has its own scroll window, so it keeps its scroll
position.

## Logging

The launcher prints `Launcher: ...` lines (shown or skipped and why, game file state and hashes, settings path, settings
saved, install request, controller prompts installed and switched, closing) into the run log, `build\logs\dragons_crown_pro_NNN.log`.
`Overlay compatibility: original game process` or `separate game process` says where the game ran. A separate game
process appends to the same log after `Launcher: the game process continues this log`, starting with its own `[run]`
lines and `Startup: game-only child process`; a session is still one file. On Windows the log also lists the DLLs
outside the Windows folder that the game process had at its start (separate process only) and the ones loaded before its
first frame (`Modules: ...`), and the Vulkan layers that are installed. The game window logs
`Window: display ... VSync ... frame rate in title ...`.

Before the game starts, a short `Settings:` block sums up the run for support: port version, platform, online protocol
and game build; Online Play (with the Ping display and the cheat lock); window mode, frame rate and frame counter; the
render resolution, aspect ratio and effective True Ultrawide; the Menu & Subtitles language, Voice Language and
Arachnophobia Safe Mode; and the Story and Tutorial Skip Offers. The values are the ones in effect for this run (after
build gating and `DCP_ONLINE`). It never shows the Online name, Friend Code or account; the `Online: on as ...` line
keeps those details. `dcp-launcher-tests --log-settings <dcp.ini> <image dir> [<app0>]` prints the same lines from a
settings file.

**Detailed logging** (About, Diagnostics) is on by default and keeps that log after every session. Turned off, a run
still writes its log while it runs, but a normal exit discards it, so normal sessions do not accumulate files. A crash
or abnormal exit keeps the log. Crash diagnostics are never discarded, and neither is a crash's `.dmp`, which keeps its log's
number for good and is never part of a release package or an update. **Open logs folder** opens the folder that holds
these logs, creating it first when it does not exist yet.

## Diagnostics

`ninja dcp-launcher-shots` builds a harness that renders every page into PNGs with a hidden window and quits: `dcp-launcher-shots.exe <app0> <image dir> <out dir>`. It never shows a window, starts the game or
touches `user\` (settings go to a temporary folder). `dcp-launcher-tests.exe --check-game <app0> <image dir>` prints the
game file state of real folders without writing anything.

## Launcher UI gate

`tools\test.ps1` protects the launcher's structure and geometry, not its look. No window opens and nothing is compared
pixel by pixel. Its "Launcher UI" summary has one row per group:

| Row | Where | Checks |
|---|---|---|
| Core tests | `dcp-launcher-tests`, relinked on every run | every launcher test; the exit code and the check count |
| SVG icons | `TestPageIcons` | the ten expected icons, one `PageId` each, the embedded copy equal to the file, nothing read from the source tree at run time |
| Icon and NanoSVG licenses | `TestIconLicenses` | `SOURCES.md` and `THIRD-PARTY-LICENSES.txt`: the Display attribution, eight CC0 rows, vendored NanoSVG with its license in both packages |
| Header art | `TestHeaderArt` | assets and package inclusion; one size-matched texture rebuilt only on a size change; strips, never one quad, at the supported scales; right anchoring and no overlap with the branding |
| DPI/layout | `TestChromeLayout`, `TestLauncherDpiLayout`, `TestFooterAndCards` | sidebar fit, control column, scrollbar gutter, separator endpoint, footer, About card and friend code at 100, 150 and 225 percent |
| Scrollbar stability | `TestStablePageWidth`, `TestScrollbarSettling` | width, control column, wrap width and separator end identical before and after the scrollbar shows, and after a resize |
| Separators/row spacing | `TestRowSeparators`, `TestSeparatorPages`, `TestRowRhythm`, `TestRowKinds` | full-width lines independent of the column clip, theme-only border colours, short, wrapped and control rows |
| Sidebar EN/PT-BR | `TestSidebarProportions`, `TestSidebarLabels` | labels (longest: Jogabilidade), clamp, nine rows without a scrollbar, width handed to the page |
| Sidebar scrolling | `TestSidebarScrollRange`, `TestSidebarScrollFrames` | scroll range and clamps, reveal of the current page, then real headless frames at 100, 150 and 225 percent: wheel ownership, clicks on scrolled rows, arrows, Tab and D-pad keeping focus in view, badge on its row, clipping, no scrollbar when every page fits |
| Navigation coherence | `TestPageNavigation` | one commit per frame, no mixed frames under rapid requests, one scroll window per page |
| TRG patch chain | `tests\launcher_gate_tests.ps1` | the series applies to the pinned revision in a scratch clone, and equals the checkout the tests read |
| Compiler warnings | `tests\launcher_gate_tests.ps1` | the launcher sources and `launcher_tests.cpp`, syntax-only with the build's own `-Wall -Wextra`: zero warnings |
| Windows/Linux parity | `TestSharedLauncherSource`, the same script, `tools/test-linux-launcher.sh` | shared sources free of Windows-only code; the portable ones compile with g++ in WSL (skipped only without WSL) |
| Source hygiene | `tests\launcher_gate_tests.ps1` | no temporary debug markers, local paths, CR line endings, scratch files, stray assets, test images, removed icon code or UI diagnostics; `git diff --check` |

A group that prints nothing, or an executable that exits early, is a FAIL with its exit code.

Saulo checks by eye what the gate cannot: the header artwork, sidebar proportions, fonts, row spacing, separator
appearance, the About heart, the footer and the overall balance. The gate complements that check and does not replace it.

## Manual test

Headless commands, on a console: `--check-game` prints the build and exits `0`; `--install` with an extracted folder or a PKG shows
its progress, opens no window and does not start the game; Show this launcher Off with Xbox prompts saved starts the game with Xbox
prompts, and with PlayStation (original) saved starts it with the original ones.

1. Build with `tools\build.ps1` and start the game yourself: the launcher opens on Play with "Ready to play", the gold accent and the controller icon.
2. Change Display mode and Window size, press Save, quit, start again: the values are kept.
3. Press PLAY: "Settings saved. Starting game...", then the game window opens on the chosen monitor at the chosen size.
4. In the game press F11 twice: Windowed, Borderless Fullscreen, back; the picture keeps its size.
5. Turn Frame rate in title and Render resolution in title On and play: the title shows `| 1920x1080 | FPS: ...` (the
   resolution appears with the first frame, the FPS after about a second).
6. Start with Show this launcher Off: the game starts directly; hold Shift while starting: the launcher opens.
7. Navigate the launcher with the keyboard and an Xbox controller.
8. On Controls set Controller prompts to Xbox: a progress bar runs for a few seconds, then "Controller prompts:
   Xbox.". Play: the tutorial and menus show A/B/X/Y, LB/RB, MENU and VIEW. Set PlayStation (original): the archives
   are back to their original size and the `.undo` files are gone. Reinstall the game with Xbox chosen: it is
   re-applied after the install.

### Character Highlight

A shortcut feature with no Gameplay setting. The key (**Port hotkeys**, F7 by default) toggles a colored outline around Player 1; the **Character Highlight** choice in **Controller shortcuts** (`View / Back + R3` by default, or `View / Back + L3`, `L1 + R3`, `R1 + L3`, Off) toggles it for the player the controller that pressed it drives. Every player starts Off in every session; the state is never saved. It is a local visual effect only, offline and online: it outlines the characters this client controls on this screen, is never sent to another client, and never outlines another player. The key toggles the primary local player (the first seat this client owns, which on an online guest can be the second party slot), a controller toggles the local player it drives. Colors follow the party slot: first red, second blue, third green, fourth yellow. Custom looks made with the Appearance Editor are recognised for the local players. The outline is matched by the character's exact atlas, so two local players with the same class and look cannot be told apart: that atlas is left without an outline and the log says `shared atlas ambiguous`.
