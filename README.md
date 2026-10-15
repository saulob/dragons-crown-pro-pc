<div align="center">

# Dragon's Crown Pro - PC Port

### Dragon's Crown Pro, Vanillaware's PS4 side-scrolling beat 'em up, running natively on Windows, and experimentally on Linux and Steam Deck, with the TRG launcher and modern graphics and display options

[![Latest release](https://img.shields.io/github/v/release/saulob/dragons-crown-pro-pc?style=for-the-badge&label=release&color=e0bb32&labelColor=07101f&include_prereleases&cacheSeconds=300)](https://github.com/saulob/dragons-crown-pro-pc/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/saulob/dragons-crown-pro-pc/total?style=for-the-badge&color=e0bb32&labelColor=07101f&cacheSeconds=300)](https://github.com/saulob/dragons-crown-pro-pc/releases)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20experimental%20%7C%20Steam%20Deck%20experimental-00acc1?style=for-the-badge&labelColor=07101f)

<sub>The original PS4 game code runs on the PC with a replacement for the PS4 system libraries and a Vulkan renderer derived from <a href="https://github.com/shadps4-emu/shadPS4">shadPS4</a>. <b>No game files included</b>: bring your own legally obtained Dragon's Crown Pro PS4 package.</sub>

</div>

<img width="1941" height="1156" alt="2026-10-14_23-43" src="https://github.com/user-attachments/assets/984433e5-1bd8-498a-949c-68815e73b901" />

## Features

- **Native PC port**: the game's own x86-64 code runs directly on the PC. It is not an emulator. Windows is the primary tested platform, and there is an experimental native Linux / Steam Deck AppImage with the same intended feature set. Linux needs no Proton or Wine.
- **TRG launcher** that installs the game directly from your own `.pkg` file, with no Python or other tools needed, and remembers its window position
- **Windowed** and **Borderless Fullscreen**, switched with **F11** while playing, with window size and monitor selection
- **Graphics card selection**: Automatic, or any Vulkan graphics card the renderer can use
- **VSync** on or off
- **Render resolution** from 480p to 4K (3840×2160), with the game's native 1080p as the default
- **SMAA** anti-aliasing and **anisotropic texture filtering** (Game, 2x, 4x, 8x, 16x)
- **AMD FidelityFX Super Resolution 1** upscaling and **RCAS sharpening** with a strength setting
- **Menu & Subtitles language**: English, French, German, Italian or Spanish
- Frame rate in the window title, and an in-game **frame counter** (frames per second and frame time)
- An in-game settings menu: **Insert** on the keyboard, or **R3 + L2** on the controller
- Xbox controllers without setup, and keyboard support, with **Xbox button prompts** as an option (PlayStation prompts are the default)
- Portable `user` and `logs` folders next to the program for settings, saves, the shader cache and logs

## What you need

- Windows 10 (version 1903 or newer) or Windows 11, 64-bit
- **Or, experimental:** 64-bit Linux on x86-64, including the Steam Deck, on an X11 or Wayland desktop, with glibc 2.31 or newer and a Vulkan 1.3 loader (`libvulkan.so.1`) and graphics driver (see [Linux and Steam Deck](#linux-and-steam-deck)). The game has been run on Ubuntu 24.04 only and its launcher started on Ubuntu 20.04; other distributions that are new enough may work
- A graphics card and driver with **Vulkan 1.3**, `VK_EXT_robustness2`, `VK_KHR_push_descriptor` and `VK_EXT_vertex_attribute_divisor` on either platform (tested on an NVIDIA GeForce RTX 4070 and Intel UHD Graphics 730 on Windows)
- **Your own legally obtained Dragon's Crown Pro package**: a decrypted PS4 `.pkg` file of this exact game version, and about 6 GB of free space for the installed game:

| | |
| --- | --- |
| Game | Dragon's Crown Pro |
| Platform | PlayStation 4 |
| Title ID | `CUSA10454` |
| Content ID | `UP2611-CUSA10454_00-DRAGONSCROWNPRO1` |
| APP_VER | `01.00` |
| Game version | `01.03` |
| `eboot.bin` | SHA-256 `f9bff150c4c28507096e7e143a07492063ed89bf05ac6cb995a55a0946e3508a` |

The launcher checks the Title ID, Content ID and versions in the package's `param.sfo` before it unpacks anything, and the `eboot.bin` hash before it installs the result. Any other version is refused, because the port is built for this exact game code. A package encrypted for a PS4 console cannot be unpacked on a PC.

The Linux package is the same game and launcher, so the package requirements are the same.

## Installing

1. Download **DragonsCrownProPC-v0.3.0-windows-x64.zip** from the [latest release](https://github.com/saulob/dragons-crown-pro-pc/releases/latest) and extract it anywhere you can write to.
2. Run **dcp-loader.exe**. The launcher opens.
3. On the **Play** page click **Install game...** and select your own Dragon's Crown Pro `.pkg` file.
4. Wait until **Ready to play** appears. The package is unpacked on your PC into the `game` folder next to the program (about 6 GB). The `.pkg` file is only read, and you can delete it afterwards.
5. Press **PLAY**.

**No game files are included** in the release, and the port does not download the game.

**Reinstall game...** replaces only the installed game files and keeps your settings, saves, shader cache and logs. If an installation fails or is cancelled, the previous one is left as it was.

### Saves and settings

Everything is kept next to the program, so copying the whole folder to another PC takes your data with it:

| Folder | Contents |
| --- | --- |
| `user\dcp.ini` | Launcher and game settings |
| `user\savedata\` | Your saves |
| `user\cache\CUSA10454\` | Shader cache, so later starts load faster |
| `logs\` | One `dragons_crown_pro_NNN.log` per run |
| `game\` | The installed game (unpacked from your own package) |

These folders are created the first time they are needed and are not part of the download. When updating the port, keep `user` (and `game`, to skip reinstalling).

### Linux and Steam Deck

**Experimental.** This is a native Linux build, not Proton or Wine, and it is intended to provide the same functionality as Windows: the same launcher, installer, display and graphics options, controls and in-game menu. "Experimental" is about testing: the maintainer has not been able to test it on a real Linux PC or Steam Deck yet. It was validated automatically under WSL2 with Ubuntu 24.04, in X11 and Wayland environments with a software Vulkan driver: the launcher, installing a package, the game's first screens, saves and the shader cache. Broader testing is still needed on AMD (RADV), NVIDIA and Intel Vulkan drivers, the Steam Deck's own controls, gamescope and Gaming Mode, performance, and long sessions. Please report Linux-specific problems with the log (see [Reporting problems](#reporting-problems)).

1. Download **DragonsCrownProPC-v&lt;version&gt;-x86_64.AppImage** from the [latest release](https://github.com/saulob/dragons-crown-pro-pc/releases/latest) and make it executable (right-click > **Properties** > **Permissions** > **Is executable**, or `chmod +x`).
2. Run it. The launcher opens.
3. On the **Play** page click **Install game...** and select your own Dragon's Crown Pro `.pkg` file, exactly as on Windows. The launcher uses the desktop's file chooser (`zenity` or `kdialog`) when there is one, and its own file chooser otherwise, which works with the mouse, the keyboard and a controller. Wait until **Ready to play** appears, then press **PLAY**.
4. `game`, `user` and `logs` are kept next to the AppImage file when that folder is writable (in `$XDG_DATA_HOME/DragonsCrownProPC` otherwise; `DCP_PORTABLE_DIR` chooses another folder). The AppImage itself is never written to.

An AppImage needs FUSE 2 to start. On a system without a working FUSE, run it as `./DragonsCrownProPC-*-x86_64.AppImage --appimage-extract-and-run`.

To bring the launcher back after turning **Show this launcher** off, hold **Shift** while starting (in an X11 session) or start the AppImage with `--launcher`. The AppImage's desktop entry also has a **Show launcher** action. On a Steam Deck, which has no keyboard in Gaming Mode, put `--launcher` in the shortcut's **Launch Options** for a run, then remove it.

**Steam Deck:** install the game once from Desktop Mode, then add the AppImage with **Games > Add a Non-Steam Game to My Library** and start it from Gaming Mode. Nothing needs Proton. Requirements, differences from Windows and known limits are in [docs/LINUX.md](docs/LINUX.md).

### Windows SmartScreen

Windows may warn the first time you run the program, because the release is not digitally signed. If you downloaded it from this repository's releases page, choose **More info**, then **Run anyway**.

## The launcher

The launcher opens before the game. Your choices are saved to `user\dcp.ini`, the same file the in-game menu writes. To turn the launcher off, set **Show this launcher** to **Off** on the Play page. Hold **Shift** while starting to bring it back (on Linux, also with `--launcher`, see [Linux and Steam Deck](#linux-and-steam-deck)). The launcher works the same on both platforms; on Linux the folder paths below use `/` instead of a backslash.

### Play
- **Install game...** from your `.pkg` file, with the Title ID, version and `eboot.bin` checks, and **Reinstall game...**
- The **Ready to play** status, and the **Show this launcher** setting

### Display
- **Display mode**: Windowed or Borderless Fullscreen. Press **F11** while playing to switch between them.
- **Window size** (1280×720 to 3840×2160) and **Monitor**
- **Graphics card**: Automatic or a specific Vulkan graphics card. A card that is no longer present falls back to Automatic.
- **VSync** on or off
- **Upscaling**: AMD FSR 1 enlarges the picture when the window is bigger than the render resolution
- **Sharpening** (AMD RCAS) and **Sharpening strength**

### Graphics
- **Render resolution**: Native (1080p) 1920×1080, 480p 854×480, 720p 1280×720, 1440p 2560×1440, 1800p 3200×1800 or 4K 3840×2160. 1440p and above need a powerful graphics card.
- **Anti-aliasing**: Off or SMAA
- **Texture filtering**: Game, 2x, 4x, 8x or 16x

### Gameplay
- **Menu & Subtitles**: the language of the game's menus and subtitles: English (default), French, German, Italian or Spanish. Voices are chosen inside the game.
- **Frame rate in title**: shows the frame rate in the window title, for example `Dragon's Crown Pro | FPS: 60.00`
- **Frame counter**: shows the frame rate and frame time in a corner of the game

The game runs at its own 60 frames per second.

### Controls
- Xbox controllers work without setup
- The **keyboard** is always on, together with the controller:

| Action | Keys |
| --- | --- |
| Left stick | W A S D |
| Right stick | Arrow keys |
| Cross (A) / Circle (B) | Space / Left Shift |
| Square (X) / Triangle (Y) | E / Q |
| L1 / R1 | 1 / 3 |
| L2 / R2 | R / F |
| L3 / R3 | Z / C |
| D-pad up / down / left / right | I / K / J / L |
| Options (Start) | Enter |
| Touchpad left / right (Back) | Tab / Backspace |

- **F11** switches Windowed and Borderless Fullscreen. **Insert** (or **R3 + L2**) opens the in-game menu, with the display mode, VSync, FSR 1, sharpening and the frame counter.
- One player: local multiplayer and online play are not supported.

### About
- The port version, game version, renderer and library versions
- Open the game folder, the saves and settings folder, or the logs folder
- **Show settings file**, and **Reset all settings**

## Screenshots

<img width="1962" height="1170" alt="2026-10-14_23-45" src="https://github.com/user-attachments/assets/91c30616-e700-478f-a56b-c286344e6502" />
<img width="1935" height="1161" alt="2026-10-14_23-44_1" src="https://github.com/user-attachments/assets/3f9fbc08-a505-4caa-8346-d5560b82d94d" />


## Reporting problems

If the game does not start or crashes, please open an [issue](https://github.com/saulob/dragons-crown-pro-pc/issues). Attach the newest `dragons_crown_pro_*.log` file from the `logs` folder next to the program, and say what you were doing when the problem happened.

For a Linux or Steam Deck problem, please also give your Linux distribution, your GPU and its driver (for example Mesa and RADV, or NVIDIA), and on a Steam Deck whether you were in Desktop Mode or Gaming Mode.

## Building it yourself

```powershell
git clone https://github.com/saulob/dragons-crown-pro-pc.git
```

1. Install [MSYS2](https://www.msys2.org) with the CLANG64 toolchain. `.\tools\setup.ps1` checks it and prints the `pacman` command for anything missing.
2. `.\tools\build.ps1` fetches the pinned dependencies (TRG Launcher, Dear ImGui and a few small libraries) and builds `build\cmake\clang64\bin\dcp-loader.exe`.
3. `.\tools\build.ps1 -Run` also starts it. Install the game from the launcher's Play page, as in a release; the development build keeps it in `game\` and the boot image in `build\image\` of the repository.
4. `.\tools\package.ps1` makes the release folder, ZIP and `.zip.sha256` file in `dist\` from the same build: the program without debug information (the symbols stay in `dist\`), the DLLs it needs and the licences. It is local only and never uploads, tags or publishes anything. The release version is `DCP_PORT_VERSION` in `CMakeLists.txt`.
5. `.\tools\package-release.ps1` prepares the whole release in `dist\`: that Windows ZIP, and the Linux AppImage built in WSL2 (`tools/package-release-linux.sh`, which runs `tools/package-linux.sh`), each with its `.sha256` file. It needs the Ubuntu 20.04 WSL2 distribution `DcpFocalBuild`, prepared once as [docs/LINUX.md](docs/LINUX.md#building) describes, installs nothing, fails if either platform fails, and never uploads anything.

`.\tools\test.ps1` runs the focused tests. Python is not needed to build, test or play. Only the optional development scripts in `scripts\` use it, such as the Python cross-checks of the package extractor and the boot image builder (`.\tools\setup.ps1 -InstallPython`).

### Linux

```sh
git clone https://github.com/saulob/dragons-crown-pro-pc.git
cd dragons-crown-pro-pc
sudo bash packaging/linux/ci-deps.sh   # Ubuntu 20.04: toolchain and pinned libraries (docs/LINUX.md)
bash tools/build.sh               # fetches the pinned dependencies, builds build/cmake/linux/bin/dcp-loader
bash tools/build.sh --test        # also builds and runs the focused launcher and runtime tests
bash tools/package-linux.sh       # dist/DragonsCrownProPC-v<version>-x86_64.AppImage and its .sha256 (--out-dir DIR: elsewhere)
```

`tools/build.sh --run` starts the loader that was built. Python is not needed. The AppImage is built on Ubuntu 20.04, whose glibc (2.31) is the oldest one it runs on, and `tools/package-linux.sh` checks every ELF file in it against that; `.github/workflows/linux-appimage.yml` does the same in GitHub Actions (started by hand, publishes nothing).

The technical notes are in [docs](docs): [launcher and settings](docs/LAUNCHER.md), [Linux port](docs/LINUX.md), [graphics](docs/GRAPHICS.md), [first boot](docs/FIRST_BOOT.md), [bootstrap](docs/BOOTSTRAP.md) and [references](docs/REFERENCES.md).

## Credits

- Dragon's Crown Pro by Vanillaware, published by Atlus (2018). This is an unofficial fan-made PC port. It is not affiliated with or endorsed by Vanillaware, Atlus, SEGA, Sony Interactive Entertainment or any other rights holder, and it contains none of their files.
- [shadPS4](https://github.com/shadps4-emu/shadPS4) for the Vulkan renderer, shader recompiler and GPU command processing the video core is built on, and for the PS4 package extractor the installer is adapted from, as it was before upstream commit `be22674f` removed it (GPL-2.0-or-later)
- [gow3_pc](https://github.com/FelipePrado0/gow3_pc) and its lineage, uncharted-ndc-native, MediEvil PC and the HayatoG ports, for the loader, runtime and video core this port grew from (see [docs/REFERENCES.md](docs/REFERENCES.md))
- [bloodborne_pc_windows](https://github.com/Phroster/bloodborne_pc_windows) for the anisotropic filtering approach
- [SMAA](https://github.com/iryoku/smaa) by Jorge Jimenez et al., and AMD FidelityFX Super Resolution 1
- [TRG Launcher](https://github.com/TekRantGaming/trg-launcher), the launcher interface, pinned at v1.1.0
- The launcher and installer follow the approach of the [Hard Corps: Uprising](https://github.com/saulob/hard-corps-uprising-recompiled) and [Geometry Wars: Retro Evolved 2](https://github.com/saulob/geometry-wars-retro-evolved-2-recompiled) PC ports
- The licenses of the libraries the release includes are in the `licenses` folder of the ZIP

## AI disclosure

AI tools were used to assist with code analysis, development, debugging and documentation. Changes were directed, tested and reviewed by the maintainer.

## License

Original code in this project is licensed under the GNU General Public License v2.0 or later (GPL-2.0-or-later). See [LICENSE](LICENSE).

Third-party components retain their respective licenses and notices. See [licenses/THIRD-PARTY-LICENSES.txt](packaging/licenses/THIRD-PARTY-LICENSES.txt).

No game files are included. Dragon's Crown Pro and its assets are not covered by this project's license.
