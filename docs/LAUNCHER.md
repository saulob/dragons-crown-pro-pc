# Launcher

The PC launcher is [TRG Launcher](https://github.com/TekRantGaming/trg-launcher) 1.1.0 (pinned in `gpu/deps.txt`, the
same revision as geometry-wars-retro-evolved-2-recompiled), in the same loader process as the game. Pages: Play, Display,
Graphics, Gameplay, Controls and About.

## Architecture

`dcp-loader.exe` calls `dcp_launcher_run()` (`launcher/dcp_launcher.h`) early in `main`, after the PS4 address range is
reserved and before the boot image is read. There is no second executable and no helper process. On Linux the loader
passes `dcp_check_vulkan` as the launcher's graphics check: Play runs it (the launcher itself needs no Vulkan) and, if
it fails, `launcher/problem_dialog.cpp` explains why and the launcher stays open.

The launcher cannot live in the game's own window: the Vulkan presenter and its Dear ImGui context are created by the
guest, and a missing game has no guest at all. So it opens a window of its own (SDL3, an SDL renderer, Dear ImGui) and
closes all of it on Play, before the loader creates the game window. Only one SDL window and one Dear ImGui context
exist at a time. The Dear ImGui copy is the one compiled into the video core library.

| Path | Purpose |
|---|---|
| `launcher/launcher.cpp` | pages, Play flow, startup decision, the C entry point |
| `launcher/host.cpp` | the launcher window: SDL3, renderer, Dear ImGui, F11, placement |
| `launcher/problem_dialog.cpp` | the graphics error dialog, in the launcher or in a window of its own |
| `launcher/game_files.cpp` | `CheckGameFiles`, SHA-256 (BCrypt), param.sfo and marker reading |
| `launcher/settings.cpp` | `LauncherSettings`, the view of `user\dcp.ini` |
| `launcher/options.cpp`, `placement.h`, `play_gate.h` | Display options, window placement, the Play gate |
| `src/dcp_displays.h` | the display list shared by the Monitor row and the game window |
| `tests/launcher_tests.cpp` | focused tests (`tools\test.ps1`) |

## Folders and defaults

`dcp-loader.exe` needs no arguments, Python or environment: explicit `boot.bin`, `--content-profile`, `--app0`, `--user`
and `--logs` win, and everything else is found next to the executable (`launcher/paths.cpp`).

| | Packaged (next to the executable) | Executable in `build\cmake\<toolchain>\bin` |
|---|---|---|
| Prepared game (`/app0`) | `game\prepared\v1.03` | `<repository>\game\prepared\v1.03` |
| Boot image | `game\image\v1.03` | `<repository>\build\image\v1.03` |
| User data, `dcp.ini` | `user` | `<repository>\user` |
| Logs | `logs` | `<repository>\build\logs` |

The settings file is always `<user>\dcp.ini`; the loader hands its path to the launcher and the video core. The
correctness defaults (draw preparation workers 0, asynchronous shaders off, vertex descriptor validation, linear image
readbacks, the 60 Hz vblank, the stick dead zone) are code, and the environment cannot change them (docs/GRAPHICS.md).
`tools\build.ps1 -Run` starts the executable with no folder arguments: it resolves the development folders itself.

The loader writes the one numbered log itself, `logs\dragons_crown_pro_NNN.log` (the next free number, never an
existing file) in the folder above, and echoes it to the console. `--logs <folder>` or `--log <file>` override it.

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

1. `game\prepared\v1.03\.dcp_view.json`: version `1.03` and scope `full` (written last by the installer, or by
   `prepare_game.py`).
2. `sce_sys\param.sfo`: `TITLE_ID CUSA10454`, `CONTENT_ID UP2611-CUSA10454_00-DRAGONSCROWNPRO1`, `APP_VER 01.00`,
   `VERSION 01.03`.
3. The eight game files are present and not empty (`eboot.bin`, `sce_discmap.plt`, `DRACRO.CPK`,
   `DRACRO_US.CPK`, the three `sce_module` libraries, `param.sfo`). The archives are not hashed: the marker records that
   their extraction finished.
4. `eboot.bin` SHA-256 equals `f9bff150c4c28507096e7e143a07492063ed89bf05ac6cb995a55a0946e3508a` (about 21 MB).
5. `build\image\v1.03`: `boot.bin`, `content.bin`, `image.json`, and `image.json` names the same title and eboot hash.

Steps 2 to 4 are `CheckGameTree`, which the installer also runs on the folder the player selects.

States: missing, valid, different version, read error. The identity lives in `launcher/game_profile.h` and mirrors
`scripts/game_profiles.py`; a test fails if they drift.

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
  saved. Starting game..." for 0.4 s with the controls disabled, closes the launcher window and lets the loader go on.
  A failed save keeps the launcher open. No settings are written per frame.
- The window placement is saved about half a second after a move or resize, and when the launcher closes.

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
| `graphics_card` | `automatic` or an exact Vulkan device name | `automatic` | at start |
| `vsync` | 1, 0 | 1 (FIFO) | live |
| `fsr1` | 1, 0 | 0 | live |
| `rcas`, `rcas_strength` | 1, 0; 0-100 | 1; 75 | live |
| `smaa` | 1, 0 | 0 | live |
| `anisotropy` | 0 (game), 2, 4, 8, 16 | 0 | live |
| `render_resolution` | `native`, `480p`, `720p`, `1440p`, `1800p`, `4K` | `native` | at start |
| `show_fps_title` | 1, 0 | 0 | live |
| `show_fps` | 1, 0 | 0 | live |
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
  when they fit the selected monitor's usable area with the title bar; the current value is always listed. A size that
  does not fit when the game opens is reduced keeping its shape. The picture is always 16:9 and letterboxed, so there is
  no aspect option.
- **Monitor**: `Display N`, the primary display first, then SDL's order; the same list creates the game window.
- **Graphics card**: Automatic, then each Vulkan device the renderer can use (the launcher enumerates them without creating
  a device; a device must meet the renderer's Vulkan version, extension and robustness2 requirements), by its exact Vulkan
  name. Automatic is the renderer's own ranking (usable devices first, then API version, discrete before integrated, then
  device memory). A name
  that is not there any more, or not usable, falls back to Automatic. The game's log shows `GPU: selection Automatic` or
  `GPU: requested <name>`, then `GPU: Vulkan device <name>`. It is the game's device; the launcher's own window uses Direct3D 11.
- **VSync**: On is FIFO (the stable default). Off uses Mailbox, else Immediate.
- **F11** in the game switches Windowed and Borderless Fullscreen at once and saves the Display mode. In the launcher it
  switches the launcher's own fullscreen, saved in `launcher_fullscreen`, and never changes the Display mode.

Scaling and sharpening, below those on the Display page, are what the presenter already supports: **Upscaling** (AMD
FSR 1, off by default) enlarges the game's picture (the render resolution) in a bigger window; **Sharpening** (AMD
RCAS, on) with a
**strength** (75). They run on the finished frame at present time, the same live settings as the in-game menu.

## Graphics

**Render resolution** (a drop-down; `launcher/dcp_resolution.h`): Native (1080p) 1920 × 1080, 480p 854 × 480, 720p
1280 × 720, 1440p 2560 × 1440, 1800p 3200 × 1800, 4K 3840 × 2160. It is the game's own render size, separate from the window size: the picture is scaled to the window.

The game chooses its display render targets with `mov reg, imm32` of 1920 / 3840 and 1080 / 2160, picked by
`sceKernelIsNeoMode`. For anything but Native the loader rewrites those 23 immediates (`launcher/resolution.c`) to the
chosen width and height after checking that each still holds the original instruction; if one does not, the game does
not start. Native leaves the code untouched. `sceKernelIsNeoMode` answers "PS4 Pro" for 1440p, 1800p and 4K, which is the
game's larger graphics memory path (sized for 3840 × 2160); 480p, 720p and Native use the base PS4 path. The size
reported by `sceVideoOutGetResolutionStatus` plays no part. The run log prints `DCP: render resolution WxH (key)`.

**Anti-aliasing** is SMAA 1x (tlg_pc's host pass, MIT): a post-process on the finished picture at the game's render
resolution, before FSR 1 and sharpening. **Texture filtering** forces 2x to 16x anisotropic filtering on the game's
mipmapped, bilinear samplers of graphics draws (Bloodborne's generic sampler override, without its title hooks); Game keeps
the game's own filters. MSAA is not offered: the references only respect the game's own multisampling or switch it off for
one title, and none lets the user choose a sample count.

Not offered: HDR (the video core's switch exists, but the game's `param.sfo` does not declare HDR support, so it would do
nothing) and a frame rate limit (the game is paced by a fixed 60 Hz vblank; a lower limit would slow the game itself).
There is no Performance page. Asynchronous shader compilation is fixed off:
`dcp.ini` and the environment cannot enable it.

## Gameplay

- **Menu & Subtitles**: the language of the game's menus and subtitle text (`text_language`: `english`, `french`,
  `german`, `italian`, `spanish`; English by default). The game ships text for these five. An explicit `DCP_LANGUAGE`
  environment variable (a PS4 language number) overrides it. Voices are chosen inside the game.
- **Frame rate in title**: `Dragon's Crown Pro | FPS: 60.00`, measured from the presented game frames, refreshed about once
  a second on the window thread.
- **Frame counter**: the video core's corner readout (`show_fps`).

No cheats, skip-intro or frame rate limit.

## Controls

Xbox controllers work without setup; the keyboard is always on. The page lists the layout implemented in
`runtime_pad.c` (W A S D and the arrow keys for the sticks, Space, Left Shift, E, Q, 1, 3, R, F, Z, C, I K J L, Enter,
Tab, Backspace). Nothing is remappable and the dead zone is not a setting.

## Launcher input

Mouse, keyboard (arrows, Enter = Play, Esc = Quit, Ctrl+S = Save) and controller (D-pad or left stick, A, B, Start =
Play). The launcher closes before the game starts, so its input cannot reach the game.

## Window and display scaling

SDL3 runs the process per-monitor-v2 DPI aware (also declared in the manifest). The launcher is 1280 × 720 logical,
scaled by the display's content scale and capped to 90% of the usable area, so at 225% it is 2880 × 1620 pixels on a
4K display. Window sizes are real pixels. A saved launcher position that no display shows any more is ignored and the
window is centred on the primary display.

## Logging

The launcher prints `Launcher: ...` lines (shown or skipped and why, game file state and hashes, settings path, settings
saved, install request, closing) into the run log, `build\logs\dragons_crown_pro_NNN.log`. The game window logs
`Window: display ... VSync ... frame rate in title ...`.

## Diagnostics

`ninja dcp-launcher-shots` builds a harness that renders every page into PNGs with a hidden window and quits: `dcp-launcher-shots.exe <app0> <image dir> <out dir>`. It never shows a window, starts the game or
touches `user\` (settings go to a temporary folder). `dcp-launcher-tests.exe --check-game <app0> <image dir>` prints the
game file state of real folders without writing anything.

## Manual test

1. `tools\build.ps1 -Run`: the launcher opens on Play with "Ready to play", the gold accent and the controller icon.
2. Change Display mode and Window size, press Save, quit, start again: the values are kept.
3. Press PLAY: "Settings saved. Starting game...", then the game window opens on the chosen monitor at the chosen size.
4. In the game press F11 twice: Windowed, Borderless Fullscreen, back; the picture keeps its size.
5. Turn Frame rate in title On and play: the title shows `| FPS: ...` after about a second.
6. Start with Show this launcher Off: the game starts directly; hold Shift while starting: the launcher opens.
7. Navigate the launcher with the keyboard and an Xbox controller.
