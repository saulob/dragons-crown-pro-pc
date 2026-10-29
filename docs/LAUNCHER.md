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

`<build>` is `v1.03` or `CUSA10454-1.01` for CUSA10454 (USA), `CUSA07001-1.00`, `CUSA07001-1.02` or `CUSA07001-1.03` for CUSA07001 (Japan) and
`CUSA10209-1.00` or `CUSA10209-1.02` for CUSA10209 (Asia and Hong Kong). Each build has its own folders. With several installed, the one
selected last is active. Selecting a package makes its exact build active: a complete prepared folder of it is activated
without copying, and only the build that is already active is reported as up to date.

The settings file is always `<user>\dcp.ini`; the loader hands its path to the launcher and the video core. The
correctness defaults (draw preparation workers 0, asynchronous shaders off, vertex descriptor validation, linear image
readbacks, the stick dead zone) are code, and the environment cannot change them (docs/GRAPHICS.md).
`tools\build.ps1 -Run` starts the executable with no folder arguments: it resolves the development folders itself.

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

On Linux Shift is read from the X11 server; in a Wayland session start with `--launcher` instead (see [Linux port](LINUX.md)). `--dcp-skip-launcher` skips it once for valid game files (automated runs: `tools\build.ps1 -Run -TimeoutSeconds N` adds
it). `--check-imports` and `--cpu-only` never open it. The launcher is the only recovery path, so it cannot be hidden
while the game files are not valid. If its window cannot be opened, the game starts when the files are valid.

## Game files

`CheckGameFiles` is the one readiness check (launcher start, Play and the installer). It reads, in this order:

1. `sce_sys\param.sfo` names a supported build: `TITLE_ID`, `CONTENT_ID`, `APP_VER` and `VERSION` of the finished game
   (the builds listed in `launcher/game_profile.cpp`, each identified by its finished game). The build is logged with its
   regional archive, or `none`.
2. `game\prepared\<build>\.dcp_view.json`: the build's version and scope `full` (written last by the installer, or by
   `prepare_game.py`).
3. The shared files, the build's regional archive (`DRACRO_US.CPK`, `DRACRO_PATCH_JP.CPK` or `DRACRO_CK.CPK`, none for
   Japanese 1.00) and its update files (`sce_discmap_patch.plt` for Japanese 1.02 and 1.03) are present and not empty.
   The archives are not hashed: the marker records that their extraction finished (and the Controller prompts setting
   appends to them, see Controls).
4. `eboot.bin` SHA-256 equals the build's hash (about 21 MB).
5. `game\image\<build>`: `boot.bin`, `content.bin`, `image.json`, and `image.json` names the same title and eboot hash.

Steps 2 to 4 are `CheckGameTree`, which the installer also runs on the folder the player selects.

**USA builds.** The 1.00 package (CATEGORY `gd`, APP_VER 01.00) is a complete game, installed into `v1.03` (its legacy
folder name). The 1.01 patch (CATEGORY `gp`, APP_VER 01.01) is an update laid over a hard-linked copy of the 1.00 folder,
into `CUSA10454-1.01`. The two builds have different `eboot.bin` files. An update needs the 1.00 folder to be installed;
the launcher says so otherwise.

**Japanese builds.** The base package (`drcpro.pkg`, CATEGORY `gd`, APP_VER 01.00) is a complete game: it is installed
into `CUSA07001-1.00`. The 1.02 and 1.03 updates (CATEGORY `gp`) are each laid over
a hard-linked copy of the 1.00 folder, into `CUSA07001-1.02` or `CUSA07001-1.03`. The base's data is linked, not copied.
Each result is checked as its finished game and gets its own boot image. The 1.03 update replaces every file the 1.02
update changes, so 1.03 does not need 1.02.

**Asia and Hong Kong builds.** The 1.00 package (CATEGORY `gd`) is a complete game, installed into `CUSA10209-1.00`. The 1.02 package
(CATEGORY `gp`, APP_VER 01.02) is an update laid over a hard-linked copy of the 1.00 folder, into `CUSA10209-1.02`. The two builds have
different `eboot.bin` files. An update needs the 1.00 folder to be installed; the launcher says so
otherwise.

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

## Settings

File: `user\dcp.ini` in the `--user` folder, the file the video core reads at start and its in-game menu writes. Lines
the launcher or the menu do not manage (comments, the other side's keys) are kept when either saves. Booleans are written
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
| `dcp_japanese_npc_voices` | 1, 0 | 0 | at game start (CUSA10454 1.00 only) |
| `story_skip_offer`, `tutorial_skip_offer` | 1, 0 | 0 | at game start |
| `allow_overlay_programs` | 1, 0 | 0 | at Play (Windows only) |
| `cheat_infinite_health`, `_ally_health`, `_mana`, `_ally_mana`, `_lives`, `_durability`, `_ally_durability`, `_arrows`, `_skill_points`, `_skill_uses`, `_gold` | 1, 0 | 0 | at game start (F10 changes the session only) |
| `cheat_movement_speed` | 1 (Off), 2, 4 | 1 | at game start (F10 changes the session only) |
| `cheat_movement_speed_2x` (old) | 1, 0 | 0 | read only when `cheat_movement_speed` is missing: 1 means 2x |
| `cheat_experience_multiplier` | 1 (Off), 2, 5, 10, 100 | 1 | at game start (F10 changes the session only) |
| `overlay_scale`, `overlay_corner` | 50-300 %; 0-3 (top left, top right, bottom left, bottom right) | 100; 1 | live (in-game menu only) |
| `menu_tab` | 0 (Image), 1 (Overlay) | 0 | the in-game menu's last tab |
| `launcher_x`, `_y`, `_width`, `_height`, `_maximized`, `_fullscreen` | | | launcher window only |

The game window is created after the launcher closes, in the same process, so every setting applies without a restart.
**Reset all settings** (About) sets every row above to its default, pending until Save or Play. It leaves the launcher
window placement, the game files, saves, logs and caches alone.

None of the correctness defaults are settings: draw preparation workers (0), asynchronous shader compilation (off),
vertex descriptor validation and the 10% stick dead zone are not exposed.

## Display

- **Display mode**: Windowed or Borderless Fullscreen (SDL desktop fullscreen, no display mode change). The in-game menu
  offers the same two.
- **Window size**: 1280 × 720, 1600 × 900, 1920 × 1080 (the game's own size), 2560 × 1440, 3840 × 2160, in pixels, listed
  when they fit the selected monitor's usable area with the title bar; the current value is shown. A size that
  does not fit when the game opens is reduced keeping its shape, so that the whole window, title bar included, takes at
  most 80% of the monitor's usable area, and the window opens centred there. This is applied at start only: the saved
  Window size is not changed, and a window you resize afterwards is left alone. The picture keeps its shape and is
  letterboxed or pillarboxed in the window. The listed sizes (heights 720, 900, 1080, 1440, 2160) follow the Aspect Ratio.
- **Aspect Ratio**: 16:9 Original, 16:10, 21:9 Ultrawide, 32:9 Super Ultrawide, 4:3, 5:4 (`aspect_ratio` in `dcp.ini`;
  an unknown value is 16:9). It sets the shape of the game's render target. The game's 2D scene is still laid out for its
  original 1280 × 720 canvas, so only 16:9 Original is undistorted: 21:9 and 32:9 stretch the gameplay horizontally instead
  of showing more of the world (issue #1) unless True Ultrawide is on, and 16:10, 4:3 and 5:4 squeeze it. The HUD, menus and
  prerendered videos are not adjusted. Changing it moves the saved Window size to the same height in the new shape. Borderless Fullscreen on a monitor
  of another shape shows bars, as set by Presentation. The Japanese and Asia and Hong Kong builds are not yet tested in game.
  The Asia and Hong Kong 1.02 build has no verified render resolution sites of its own, so it renders at its original size in
  16:9 and offers no other shape, whatever was saved.
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
  CUSA10454 1.00, CUSA07001 1.00, 1.02 and 1.03 and CUSA10209 1.00, where its code was checked; elsewhere a saved On
  is ignored and the log says why.
- **Monitor**: `Display N`, the primary display first, then SDL's order; the same list creates the game window.
- **Graphics card**: each Vulkan device the renderer can use (the launcher enumerates them without creating
  a device; a device must meet the renderer's Vulkan version, extension and robustness2 requirements), by its exact Vulkan
  name. The first listed device is the default. A name that is not there any more, or not usable, falls back to the first
  usable device, and the launcher saves that name when you press Play. The game's log shows `GPU: selected <name>`, then
  `GPU: Vulkan device <name>`. It is the game's device; the launcher's own window uses Direct3D 11 on Windows and SDL's first working renderer on Linux.
- **VSync**: On is FIFO (the stable default). Off uses Mailbox, else Immediate.
- **Presentation**: Letterbox (the default) or Stretch. It controls how the finished frame fits the window or display when
  their shape differs: Letterbox keeps the frame's aspect ratio with bars, Stretch fills the window and may distort the
  picture. It does not change the render target or the Aspect Ratio, and it applies in Windowed and Borderless Fullscreen
  alike. Saved as `presentation=letterbox` or `presentation=stretch` in `dcp.ini`; a missing or unknown value is Letterbox.
  The game reads it when it starts, so a change takes effect the next time the game starts.
- **F11** in the game switches Windowed and Borderless Fullscreen at once and saves the Display mode. In the launcher it
  switches the launcher's own fullscreen, saved in `launcher_fullscreen`, and never changes the Display mode.
- **Mouse cursor**: mouse movement, clicks and the wheel show the cursor in Windowed and Borderless Fullscreen; keyboard or
  controller input hides it. It stays visible while the in-game menu or a dialog is open and when the game window loses focus.

Scaling and sharpening, below those on the Display page, are what the presenter already supports: **Upscaling** (AMD
FSR 1, off by default) enlarges the game's picture (the render resolution) in a bigger window; **Sharpening** (AMD
RCAS, on) with a
**strength** (75). They run on the finished frame at present time, the same live settings as the in-game menu.

## Graphics

**Render resolution** (a drop-down; `launcher/dcp_resolution.h`): Native (1080p) 1920 × 1080, 480p 854 × 480, 720p
1280 × 720, 1440p 2560 × 1440, 1800p 3200 × 1800, 4K 3840 × 2160. It is the game's own render size, separate from the window size: the picture is scaled to the window.

The game chooses its display render targets with `mov reg, imm32` of 1920 / 3840 and 1080 / 2160, picked by
`sceKernelIsNeoMode`. Each supported build with verified sites has its own 23 of them (`launcher/game_profile.cpp`); the
USA 1.01 and Asia and Hong Kong 1.02 builds have none yet, so they render only at their native size and their Render
resolution and Aspect Ratio rows offer Native 16:9 alone (a saved custom value falls back to Native 16:9 for them, and the
loader refuses to patch a build with no sites). Unless the render target is
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
  | CUSA10454 1.00 USA | English, French, German, Italian, Spanish (`english`, `french`, `german`, `italian`, `spanish`) |
  | CUSA10454 1.01 USA | the ones its `DRACRO_US.CPK` holds (see below) |
  | CUSA07001 1.00, 1.02, 1.03 Japan | Japanese (`japanese`): the executable does not read the system language |
  | CUSA10209 1.00, 1.02 Asia/Hong Kong | Traditional Chinese, Korean (`traditional_chinese`, `korean`) |

  For the USA and Asia/Hong Kong builds the list is read from the installed regional archive (`DRACRO_US.CPK`,
  `DRACRO_CK.CPK`): a language is offered only when the archive holds its set of message sheets and stage files, and a
  set whose `SystemMsg` sheet is Russian text is offered as Russian (`russian`), with the system language of that set.
  When the archive cannot be read or holds no set, the build's list stands. Korean is not yet tested in game. A saved
  language the installed game does not have is replaced by its first one, and the log says so
  (`Settings: saved language English unavailable for this build, using Japanese`). An explicit `DCP_LANGUAGE`
  environment variable (a PS4 language number) overrides it. Voices are chosen inside the game.
- **Japanese NPC Voices**: Off by default (`dcp_japanese_npc_voices`). CUSA10454 1.00 USA only; the option is hidden for
  the other builds. The Japanese and English NPC voice banks are both in the shared `DRACRO.CPK`; this routes the six
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
- **Story Skip Offer** and **Tutorial Skip Offer**: Off by default (`story_skip_offer`, `tutorial_skip_offer`;
  `DCP_STORY_SKIP` / `DCP_TUTORIAL_SKIP` override them for a run). The game's own questions, asked for every new
  character: whether to start from the second half of the story (normally offered only once a class on the file has
  finished the game) and "Begin the tutorial?" (normally skipped for a file's first character, which starts the tutorial).
  Existing characters are never asked. On, one branch each is patched in memory after its bytes are checked
  (`src/skip_offers.c`); Off leaves the code as it is. Shown for every build.
- **Allow overlay programs**: Off / On (`allow_overlay_programs`, Off by default; Windows only, hidden elsewhere). For
  recording, chat, store and performance overlays and other tools that inject into games. On, PLAY starts the game as a
  separate process from the launcher (see Architecture), so these tools attach to a clean game process. Off keeps the
  usual start in the launcher's process. Neither setting filters Vulkan layers or blocks any program; a
  `VK_LOADER_LAYERS_DISABLE` set in the environment reaches the game as it is.

No skip-intro.

## Cheats

Sidebar order: Play, Display, Graphics, Gameplay, Online, Cheats, Controls, Trophies, About. Subtitle "Optional gameplay modifiers", with the
line "Press F10 while playing to open the Cheats menu. Changes made there apply only to the current session. Meant for
offline and local play." and Off / On
rows in this order: **Infinite Health**, **Infinite Ally Health**, **Infinite Mana**, **Infinite Ally Mana**,
**Infinite Lives**, **Infinite Durability**, **Infinite Ally Durability**, **Infinite Arrows**, **Infinite Skill
Points**, **Infinite Skill Uses**, **Infinite Gold**, each with its one-line description (`src/dcp_cheats.h`), and
below them the **Movement Speed** combo
(Off, 2x, 4x) and the **Experience Multiplier** combo (Off, 2x, 5x, 10x, 100x).
Cheats work on the CUSA10454 1.00 (US) build only; on the USA 1.01, Japanese and Asia and Hong Kong builds the page and
the menu show them as unavailable. All are ordinary saved settings, Off by default: Save and Play keep them, Reset all settings puts
them back to Off. The saved values are the start of every game session; the F10 menu changes the running session only
and never writes them. The hooks, the menu and Player 1 targeting are in [CHEATS.md](CHEATS.md).

## Controls

Xbox controllers work without setup; the keyboard is always on. The page lists the layout implemented in
`runtime_pad.c` (W A S D and the arrow keys for the sticks, Space, Left Shift, E, Q, 1, 3, R, F, Z, C, I K J L, Enter,
Tab, Backspace) and that F10 opens the Cheats menu in the game (it also has the one-shot **Add 100 Skill Points**, **Add 1,000 Gold** and **Level Up (+1 Level)**, which are not settings). The dead zone is not a setting.

The page has two sections. **Keyboard controls** are the PS4 inputs (the sticks' four directions each, the face buttons, L1/R1/L2/R2/L3/R3, the D-pad, Options and the two touchpad sides); **Port hotkeys** are the Cheats menu and Window mode keys; the in-game menu stays on Insert. Click a row (or focus it and press Enter), then press the new key; Esc cancels and is never bound. **Reset to defaults** puts these keys back and changes no other setting.

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
in the installed game files: Xbox applies a block patch to `DRACRO_US.CPK` and `DRACRO.CPK`
(`launcher/controller_prompts.h`, `launcher/cpk_patch.h`), PlayStation reverts it. The Japanese and Asia and Hong Kong
builds have no validated Xbox patch: they always show the PlayStation prompts and their archives are not touched.
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

## Launcher input

Mouse, keyboard (arrows, Enter = Play, Alt+F4 = Quit, Ctrl+S = Save) and controller (D-pad or left stick, A, B, Start =
Play). The launcher closes before the game starts, so its input cannot reach the game.

## Window and display scaling

SDL3 runs the process per-monitor-v2 DPI aware (also declared in the manifest). The launcher is 1280 × 720 logical,
scaled by the display's content scale and capped to 90% of the usable area, so at 225% it is 2880 × 1620 pixels on a
4K display. Window sizes are real pixels. A saved launcher position that no display shows any more is ignored and the
window is centred on the primary display.

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

## Manual test

1. `tools\build.ps1 -Run`: the launcher opens on Play with "Ready to play", the gold accent and the controller icon.
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
