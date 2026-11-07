# Launcher

The PC launcher is [TRG Launcher](https://github.com/TekRantGaming/trg-launcher) 1.1.0 (pinned in `gpu/deps.txt`, the
same revision as geometry-wars-retro-evolved-2-recompiled), in the same loader process as the game. Pages: Play, Display,
Graphics, Gameplay, Online, Cheats, Controls, Trophies and About.

## Architecture

`dragons_crown_pro.exe` calls `dcp_launcher_run()` (`launcher/dcp_launcher.h`) early in `main`, after the PS4 address range is
reserved and before the boot image is read. The only other executable is the Windows update helper, `trg-updater.exe`. The loader
passes a graphics check to the launcher: the processor check on Windows, the processor check and `dcp_check_vulkan` on Linux. Play
runs it (the launcher itself needs no Vulkan) and, if it fails, `launcher/problem_dialog.cpp` explains why and the launcher stays
open. The graphics card list comes from a headless Vulkan instance on both platforms (empty on Linux when Vulkan is unusable).

The launcher cannot live in the game's own window: the Vulkan presenter and its Dear ImGui context are created by the
guest, and a missing game has no guest at all. So it opens a window of its own (SDL3, an SDL renderer, Dear ImGui) and
closes all of it on Play, before the loader creates the game window. Only one SDL window and one Dear ImGui context
exist at a time. The Dear ImGui copy is the one compiled into the video core library.

With **Allow overlay programs** on (Windows only), the game does not run in the launcher's process. Play (or a start
without the launcher) creates a new `dragons_crown_pro.exe` with the same arguments minus `--launcher`,
`--dcp-skip-launcher` and `--log`, plus the internal `--dcp-game --log <this run's log>` (`launcher/game_process.cpp`).
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
| Valid | Off | the game starts directly |
| Any | any, with Shift held or `--launcher` | the launcher opens |

On Linux Shift is read from the X11 server; in a Wayland session start with `--launcher` instead (see [Linux port](LINUX.md)). `--dcp-skip-launcher` skips it once for valid game files. `--check-imports` and `--cpu-only` never open it. The launcher is the only recovery path, so it cannot be hidden
while the game files are not valid. If its window cannot be opened, the game starts when the files are valid.

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
(and has no Russian slot); that text is not installed, so English stays English and Russian is not a language of this
build. A 1.01 folder installed by an older launcher keeps that text until the update is installed again.

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

**Features by build.** Every build offers every feature of USA 1.00; Japanese NPC Voices is not applicable to the
Japanese builds, whose NPCs already speak Japanese:

| Feature | USA 1.00 | USA 1.01 | Japan 1.00, 1.02, 1.03 | Asia/HK 1.00 | Asia/HK 1.02 | Europe 1.03 |
| --- | --- | --- | --- | --- | --- | --- |
| Menu & Subtitles | 5 Western | 5 Western | Japanese, English (port) | Chinese, Korean, English (port) | Chinese, Korean, English (port) | 5 Western |
| Render resolution, Aspect Ratio | yes | yes (sites found by signature) | yes | yes | yes (sites found by signature) | yes (sites found by signature) |
| True Ultrawide | yes | yes (found by signature) | yes (found by signature) | yes (found by signature) | yes (found by signature) | yes (found by signature) |
| Story and Tutorial Skip Offers | yes | yes (found by signature) | yes | yes | yes (found by signature) | yes (found by signature) |
| Online Play, System type | yes | yes (found by signature) | yes | yes | yes (found by signature) | yes (found by signature) |
| Processors without LZCNT | yes | yes (found by signature) | yes | yes | yes (found by signature) | yes (found by signature) |
| Cheats | yes | yes (found by signature) | yes (found by signature) | yes (found by signature) | yes (found by signature) | yes (found by signature) |
| Japanese NPC Voices | yes | yes (found by signature) | not applicable | yes (found by signature) | yes (found by signature) | yes (found by signature) |
| Xbox prompts | yes | yes, when its archives hold the patch's files | yes, in `DRACRO.CPK` | yes, in `DRACRO.CPK` | yes, in `DRACRO.CPK`, when it holds the patch's file | yes, when its archives hold the patch's files |
| Arachnophobia Safe Mode, Frame rate, Display, Graphics filters, Controls | yes | yes | yes | yes | yes | yes |

"yes" here says the build offers the feature and the loader can try it, not that it was seen working. USA 1.01, Asia and
Hong Kong 1.02 and Europe 1.03 have no local executable: `tools\test.ps1` reports what stands behind each feature on each
build and prints DATA UNAVAILABLE where nothing does. Installing or selecting one of these builds writes a read-only
`Dragon's Crown Pro build site report` to the log, with what the scan and the loader's searches find in its real executable.

"Found by signature" means the loader does not use a table: it finds each site in the build's own image from
the byte patterns that give exactly the tables of the five builds checked locally, uses them only when every pattern
matches the expected number of times, and otherwise leaves the feature off (Render resolution: the game does not start
with a custom size). Cheats, Japanese NPC Voices and True Ultrawide need all of their sites (25 hooks and 15 functions,
2 sites, 22 sites) to resolve with the verified bytes and their relations, and, for the cheats, every struct displacement
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

**Install game...** (or **Reinstall game...**) on the Play page takes the player's own fake PKG
(`launcher/game_install.cpp`, on a background task with progress and Cancel):

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

`InstallGame` also takes an already extracted game folder (the one with `eboot.bin`, `sce_module` and `sce_sys`):
`CheckGameTree` checks it, and its files are hard-linked into staging on the same drive and copied otherwise.

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
| Gameplay | Skip Intro, Japanese NPC Voices (builds and game folders that offer it), Frame rate, Story Skip Offer, Tutorial Skip Offer, Unlock Gallery | saved now, applies at the next start |
| Online (Online Play on) | your Online name and friend code (copy with A or Enter), the Connection test, your friends and their status | read only; the test runs on request |
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
| `detailed_logging` | 1, 0 | 1 | at exit |
| `key_*`, `hotkey_cheats_menu`, `hotkey_window_mode` | a key name (below) | the default layout | at game start |
| `arachnophobia_safe_mode` | 1, 0 | 0 | when toggled in the launcher |
| `dcp_japanese_npc_voices` | 1, 0 | 0 | at game start (the USA and Asia and Hong Kong builds; the Japanese builds always use them) |
| `skip_intro` | 1, 0 | 0 | at game start |
| `japanese_menu_buttons` | 1, 0 | 0 | at game start |
| `story_skip_offer`, `tutorial_skip_offer` | 1, 0 | 0 | at game start |
| `unlock_gallery` | 1, 0 | 0 | at game start |
| `allow_overlay_programs` | 1, 0 | 0 | at Play (Windows only) |
| `cheat_infinite_health`, `_ally_health`, `_mana`, `_ally_mana`, `_lives`, `_durability`, `_ally_durability`, `_arrows`, `_skill_points`, `_skill_uses`, `_gold` | 1, 0 | 0 | at game start (F10 changes the session only) |
| `cheat_movement_speed` | 1 (Off), 2, 4 | 1 | at game start (F10 changes the session only) |
| `cheat_movement_speed_2x` (old) | 1, 0 | 0 | read only when `cheat_movement_speed` is missing: 1 means 2x |
| `cheat_experience_multiplier` | 1 (Off), 2, 5, 10, 100 | 1 | at game start (F10 changes the session only) |
| `overlay_scale`, `overlay_corner` | 50-300 %; 0-3 (top left, top right, bottom left, bottom right) | 100; 1 | at game start (no launcher control) |
| `launcher_x`, `_y`, `_width`, `_height`, `_maximized`, `_fullscreen` | | | launcher window only |

The game window is created after the launcher closes, in the same process, so every setting applies without a restart.
**Reset all settings** (About) sets every row above to its default, pending until Save or Play. It leaves the launcher
window placement, the game files, saves, logs and caches alone.

None of the correctness defaults are settings: draw preparation workers (0), asynchronous shader compilation (off),
vertex descriptor validation and the 10% stick dead zone are not exposed.

## Display

- **Display mode**: Windowed or Borderless Fullscreen (SDL desktop fullscreen, no display mode change).
- **Window size**: 1280 × 720, 1600 × 900, 1920 × 1080 (the game's own size), 2560 × 1440, 3840 × 2160, in pixels, listed
  when they fit the selected monitor's usable area with the title bar; the current value is shown. A size that
  does not fit when the game opens is reduced keeping its shape, so that the whole window, title bar included, takes at
  most 80% of the monitor's usable area, and the window opens centred there. This is applied at start only: the saved
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
  `GPU: Vulkan device <name>`. It is the game's device; the launcher's own window uses SDL's software renderer on Windows (no Direct3D device) and SDL's first working renderer on Linux.
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
  | CUSA10454 1.00, 1.01 USA | English, French, German, Italian, Portuguese (Brazil), Spanish (`english`, `french`, `german`, `italian`, `brazilian_portuguese`, `spanish`) |
  | CUSA07001 1.00, 1.02, 1.03 Japan | Japanese, English, Portuguese (Brazil) (`japanese`, `english`, `brazilian_portuguese`): the executable does not read the system language |
  | CUSA10209 1.00, 1.02 Asia/Hong Kong | Traditional Chinese, Korean, English, Portuguese (Brazil) (`traditional_chinese`, `korean`, `english`, `brazilian_portuguese`) |
  | CUSA10487 1.03 Europe | English, French, German, Italian, Portuguese (Brazil), Spanish (`english`, `french`, `german`, `italian`, `brazilian_portuguese`, `spanish`) |

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

  The DefaultBone character messages are translated too. While a character is being created, an empty message row
  shows the default it will receive (src/default_message_hook.c): the variant previewed for a class is held and given
  to the game's one pick when the character is added, so the saved text is the previewed one. The guest's random
  number is still drawn once, nothing is written before the character is added, and characters already in a save
  keep the messages they have. Other languages keep the game's own "no message" row.

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

  For the USA, Asia/Hong Kong and European builds the list is read from the installed regional archive (`DRACRO_US.CPK`,
  `DRACRO_CK.CPK`, `DRACRO_EU.CPK`): a language is offered only when the archive holds its set of message sheets and stage files. Russian
  is never offered: the 1.01 update's archive has no Russian slot, only Russian text in its US slot, which the install
  does not take over (see USA builds). `DRACRO_EU.CPK` has a UK slot beside the US one, as `DRACRO_US.CPK` does; it is
  not offered as a separate language. When the archive cannot be read or holds no set, the build's list stands. Korean is not yet tested in game. A saved
  language the installed game does not have is replaced by its first one, and the log says so
  (`Settings: saved language Korean unavailable for this build, using Japanese`). An explicit `DCP_LANGUAGE`
  environment variable (a PS4 language number) overrides it. Voices are chosen inside the game.
- **Japanese NPC Voices**: Off by default (`dcp_japanese_npc_voices`). USA, Asia and Hong Kong and European builds; on the builds
  whose hooks are found by signature the option appears only while the installed `DRACRO.CPK` holds the sound table and
  the Japanese banks (`launcher/npc_voice_resources.h`), and it never changes the Menu & Subtitles language. It is hidden
  for the Japanese builds, which already use the Japanese NPC voices. The Japanese and English NPC voice banks are both in the shared `DRACRO.CPK`; this routes the six
  town/event NPC voice cue families to the Japanese cue IDs at run time. The narrator and player-character voice choices
  are untouched.
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
- **Skip Intro**: Off by default (`skip_intro`; `DCP_SKIP_INTRO` overrides it for a run). The opening scene asks every
  frame whether any player pressed a button and, when one did, fades out and ends itself. With the setting on, the first
  answer is "pressed" once, so the game takes that skip by itself. The Japanese builds also open with a legal notice
  about unauthorized distribution; with the setting on they take the step the other builds take there, so the notice is
  never built and the game goes on with its normal loading. Saves, the tutorial and the story are untouched, and it is
  separate from the two skip offers. It does not depend on the text language. The log shows
  `Skip intro: on, native skip armed` when installed (plus `Skip intro: regional notice bypass installed` on builds that
  have the notice) and `Skip intro: startup sequence skipped` only when the press was used. Changing it in the F1
  Options menu needs a restart.
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
Points**, **Infinite Skill Uses**, **Infinite Gold**, each with its one-line description (`src/dcp_cheats.h`), and
below them the **Movement Speed** combo
(Off, 2x, 4x) and the **Experience Multiplier** combo (Off, 2x, 5x, 10x, 100x).
Cheats work on every supported build: the tables of CUSA10454 1.00, or the sites found by signature on the others. All
are ordinary saved settings, Off by default: Save and Play keep them, Reset all settings puts
them back to Off. The saved values are the start of every game session; the F10 menu changes the running session only
and never writes them. The hooks, the menu and Player 1 targeting are in [CHEATS.md](CHEATS.md).

## Controls

Xbox controllers work without setup; the keyboard is always on. Under Wine/Proton (`wine_get_version` exported by `ntdll.dll`) the game process sets `SDL_JOYSTICK_HIDAPI=0` before SDL's gamepad subsystem starts (`src/dcp_sdl_compat.h`, called from `loader.c` ahead of the build lookup; issue #42, Bazzite Gaming Mode), for every supported build and language; an explicit `SDL_JOYSTICK_HIDAPI` is kept, and native Windows and Linux are unchanged. Whether Steam Input must also be off there is untested. The page lists the layout implemented in
`runtime_pad.c` (W A S D and the arrow keys for the sticks, Space, Left Shift, E, Q, 1, 3, R, F, Z, C, I K J L, Enter,
Tab, Backspace) and that F10 opens the Cheats menu in the game (it also has the one-shot **Add 100 Skill Points**, **Add 1,000 Gold** and **Level Up (+1 Level)**, which are not settings). The dead zone is not a setting.

The page has two sections. **Keyboard controls** are the PS4 inputs (the sticks' four directions each, the face buttons, L1/R1/L2/R2/L3/R3, the D-pad, Options and the two touchpad sides); **Port hotkeys** are the Cheats menu and Window mode keys. Click a row (or focus it and press Enter), then press the new key; Esc cancels and is never bound. **Reset to defaults** puts these keys back and changes no other setting.

Each binding is one line in `dcp.ini`: `key_cross = space`, `key_left_stick_up = w`, `hotkey_cheats_menu = f10`, `hotkey_window_mode = f11` (the full list is `src/dcp_keys.c`). The value is a key name, case-insensitive: the shadPS4 names for common keys (`space`, `enter`, `lshift`, `rctrl`, `pgup`, `kp5`, ...) or SDL's own key names (`F10`, `Left Shift`, `Keypad 5`); the launcher writes lowercase. A missing or invalid line is the default, so a config from an older build keeps the old layout. The same key may be bound to several controls and all of them follow it; a port hotkey is not exclusive of a game control either. The bindings are read when the game starts (`dcp_keys.c`, shared by the launcher, `runtime_pad.c` and the video core); the controller mapping and `DCP_PAD_MAP` are unchanged, and `DCP_KEY_MAP` still overrides the buttons it names.

**Local multiplayer**: up to four SDL gamepads, player N on pad handle N and user N (`runtime_pad.c`, `runtime_services.c`).
A gamepad that is found takes the lowest free slot and logs its user in once; the game opens that user's pad on the
`LOGIN` event. The keyboard, `DCP_PAD_FILE`, pad recording and replay and the save list's controls belong to Player 1
only. Unplugging a gamepad closes it and frees its slot, but no `LOGOUT` is sent: the player stays logged in, and the
next gamepad connected takes the lowest free slot, which is not necessarily the same player. A fifth gamepad is ignored.
The cheats act on Player 1 only (Infinite Gold, which is one pool for the save, excepted); Infinite Ally Health, Infinite Ally Mana and Infinite Ally Durability act on AI-controlled party allies instead. Add 100 Skill Points and Level Up (+1 Level) change Player 1's save record. The Experience Multiplier applies to every player's experience award; see [CHEATS.md](CHEATS.md).

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
   The newest log that started the game is selected by default (`DefaultGameLog`), so a game that failed before its code ran is
   chosen over an older session that played. Without one, the newest log other than the launcher's own; failing that, its own.
3. **Preview** (optional): **Open preview** builds the report (`BuildIncidentPackage`, `launcher/incident_report.cpp`) and shows
   it in full; **Close preview** hides it. The last 256 KiB of the log are filtered (`launcher/log_scrubber.cpp`), then its last
   64 KiB are kept, from a whole line on. Changing the description or the log closes the preview, and opening it again rebuilds it.
4. **Send report**: sends the open preview, or builds the same report through the same path when none is open. A description that
   is too short does not send: the field takes the focus, flashes, and "Please describe the problem, minimum 10 characters" appears beside the
   count. The page scrolls down to the sending message and again to the result, which stays on the page: the Report ID, or a rate limit, connection, refused or service message; after a failure
   the text stays and Send report tries again.

5. **My reports** lists the reports this PC sent, newest first, from `user/bug_reports.json` (`launcher/report_history.cpp`, saved
   through a new file moved into place; a damaged file is set aside as `.damaged`). Each row shows the report ID, a preview, the
   status (with the fixed version on a Fixed report) and the date. **View details** expands that report in place, as one framed
   item that holds its summary, its facts (status, fixed version, the developer's message), **What was sent**, which reads the stored
   report back, and **Remove from this PC**, which deletes only the local entry. Only one report is open at a time, and **Hide details**
   closes it. **Refresh** sits at the top right of the section.
   The statuses are checked at startup, when the page opens and on **Refresh**, off the UI thread and silently when offline. A
   change marks the report **Updated** and puts a dot on the sidebar entry, until the report is opened. If the list cannot be saved
   the report still counts as sent: the Report ID shows normally with a short note.

The request and the privacy rules are in [ONLINE.md](ONLINE.md#bug-reports). Online Play does not have to be on.

## Launcher input

Mouse, keyboard (arrows, Enter = Play, Alt+F4 = Quit, Ctrl+S = Save) and controller (D-pad or left stick, A, B, Start =
Play). The launcher closes before the game starts, so its input cannot reach the game.

## Window and display scaling

SDL3 runs the process per-monitor-v2 DPI aware (also declared in the manifest). The launcher is 1280 × 720 logical,
scaled by the display's content scale and capped to 90% of the usable area, so at 225% it is 2880 × 1620 pixels on a
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
render resolution, aspect ratio and effective True Ultrawide; the Menu & Subtitles language, Japanese NPC Voices and
Arachnophobia Safe Mode; and the Story and Tutorial Skip Offers. The values are the ones in effect for this run (after
build gating and `DCP_ONLINE`). It never shows the Online name, Friend Code or account; the `Online: on as ...` line
keeps those details. `dcp-launcher-tests --log-settings <dcp.ini> <image dir> [<app0>]` prints the same lines from a
settings file.

**Detailed logging** (About, Diagnostics) is on by default and keeps that log after every session. Turned off, a run
still writes its log while it runs, but a normal exit discards it, so normal sessions do not accumulate files. A crash
or abnormal exit keeps the log. Crash diagnostics are never discarded. **Open logs folder** opens the folder that holds
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
| Navigation coherence | `TestPageNavigation` | one commit per frame, no mixed frames under rapid requests, one scroll window per page |
| TRG patch chain | `tests\launcher_gate_tests.ps1` | the series applies to the pinned revision in a scratch clone, and equals the checkout the tests read |
| Compiler warnings | `tests\launcher_gate_tests.ps1` | the launcher sources and `launcher_tests.cpp`, syntax-only with the build's own `-Wall -Wextra`: zero warnings |
| Windows/Linux parity | `TestSharedLauncherSource`, the same script, `tools/test-linux-launcher.sh` | shared sources free of Windows-only code; the portable ones compile with g++ in WSL (skipped only without WSL) |
| Source hygiene | `tests\launcher_gate_tests.ps1` | no temporary debug markers, local paths, CR line endings, scratch files, stray assets, test images, removed icon code or UI diagnostics; `git diff --check` |

A group that prints nothing, or an executable that exits early, is a FAIL with its exit code.

Saulo checks by eye what the gate cannot: the header artwork, sidebar proportions, fonts, row spacing, separator
appearance, the About heart, the footer and the overall balance. The gate complements that check and does not replace it.

## Manual test

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
