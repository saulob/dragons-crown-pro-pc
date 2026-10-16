# Linux port

The loader, runtime and video core run natively on Linux x86-64 exactly as on Windows: the game's own code executes on
the host CPU, the PS4 system libraries are replaced by `src/runtime_*.c`, and the renderer is the same Vulkan video
core. This page lists what differs from Windows, how to build and package it, and what has been verified.

## Building

Releases are built on Ubuntu 20.04. `packaging/linux/ci-deps.sh` prepares a clean 20.04 system as root: Clang 20 from
apt.llvm.org and the GCC 14 C++ library from the `ubuntu-toolchain-r/test` PPA (both signing keys checked by
fingerprint), CMake 3.31.8, and the distribution's development packages. What 20.04 lacks or ships too old is built into
`/usr/local` from pinned commits or SHA-256-checked archives: Vulkan-Headers, VMA, glslang, Boost 1.83 headers, fmt,
Zydis and Zycore, xxHash, robin-map, OpenSSL 3.5 (libcrypto), SDL3, Linux 6.8's `userfaultfd.h`, and the libdecor and
PipeWire headers SDL needs to build those backends (SDL loads both at run time; they are not bundled). Then:

| Command | Result |
| --- | --- |
| `tools/build.sh` | fetches `gpu/deps.txt`, builds `build/cmake/linux/bin/dcp-loader` |
| `tools/build.sh --test` | also runs `dcp-launcher-tests`, `dcp-cheats-tests`, `dcp-runtime-tests` and `dcp-text-tests` |
| `tools/build.sh --run` | starts the loader that was just built |
| `tools/package-linux.sh` | `dist/DragonsCrownProPC-v<version>-x86_64.AppImage` and `.sha256`; debug symbols in `dist/<name>-symbols/` (`--out-dir DIR` writes all three to DIR instead) |
| `tools\package-release.ps1` (Windows) | the Windows ZIP and this AppImage, in one run: the AppImage is built in the WSL2 distribution `DcpFocalBuild` (`tools/package-release-linux.sh` mirrors the sources into `~/.cache/dragons-crown-pro-pc-build`, runs `package-linux.sh` there and copies the result back) |

The AppImage needs glibc 2.31 (Ubuntu 20.04) or newer: its newest glibc import is 2.30 (the loader's `gettid` and
`pthread_cond_clockwait`). `package-linux.sh` refuses to package on a system with a newer glibc, and before and after
writing the AppImage it reads the version needs of every ELF file in it (found by content, not by name) and stops if
any needs more than 2.31, or if the package carries a part of glibc. The loader also needs `GLIBCXX_3.4.32`; older
systems get the bundled fallback C++ library. `.github/workflows/linux-appimage.yml` builds it in an Ubuntu 20.04
container and opens its launcher on Debian 13, Fedora 42, Ubuntu 24.04, Arch and openSUSE, whose stock Vulkan loaders
support 1.3 (20.04's does not); it is started by hand, publishes nothing and has not been run yet.

To make the release environment in WSL2, import the Ubuntu 20.04.5 base image
(`ubuntu-base-20.04.5-base-amd64.tar.gz` from https://cdimage.ubuntu.com/ubuntu-base/releases/20.04/release/, SHA-256
`60e216b60947653dc8989be3821380268315b99b2959c29882781541bfe5a426`, listed in that folder's signed `SHA256SUMS`) and
prepare it once:

```powershell
wsl --import DcpFocalBuild <folder> ubuntu-base-20.04.5-base-amd64.tar.gz --version 2
wsl -d DcpFocalBuild -u root -- bash packaging/linux/ci-deps.sh
```

Run the second command from the repository folder. A Docker `ubuntu:20.04` container works the same way.

## What differs from Windows

| Area | Linux |
| --- | --- |
| Toolchain | Clang, non-PIE (`-no-pie`): the host heap handed to the guest (thread handles, TLS) must stay below 1 TiB, as guest code packs pointers into 40 bits. `-rdynamic` names host symbols in fault reports. |
| Guest address range | `runtime_memory_reserve` claims `[64 GiB, 1 TiB)` as `PROT_NONE` at startup with `MAP_FIXED_NOREPLACE` and stops with a message if the kernel cannot give it. Guest mappings then replace parts of that reservation with `MAP_FIXED`, which cannot touch host memory; a fixed request outside the range fails, and an unmap puts the reservation back. Host-owned guest-visible memory (image, stacks) lives in `[32 GiB, 64 GiB)` with `MAP_FIXED_NOREPLACE`. |
| Guest memory | One sparse `memfd` backs direct and flexible memory (as on Windows, a pagefile section plays that part). |
| Guest TCB | `arch_prctl(ARCH_SET_GS)` per thread: the boot image reads the TCB through `gs:[0]`. |
| Faults | `SIGSEGV` handler (GPU page tracking, speculative reads) in `loader.c`. |
| Threads and fibers | Stacks come from `runtime_stack_acquire` (below 1 TiB, guard page at the bottom). Released stacks are never unmapped: they are kept by size with their pages returned to the kernel, so the address range does not grow. A joined thread's stack is released after `pthread_join`; a detached thread's host thread stays joinable and is joined, then released, when the next thread is created. Fibers (`runtime_fiber.c`) switch with a small assembly routine and may continue on another thread. |
| File names | The PS4 file system ignores case, Linux does not: `runtime_file.c` matches a missing path component against the folder's entries without case (the game opens `DRACRO.cpk`, the package has `DRACRO.CPK`). |
| Hashes and ciphers | `launcher/crypto_openssl.cpp` (libcrypto) instead of CNG; the interface is `launcher/crypto.h`. Results are identical: the boot image built on Linux is byte-identical to the Windows one. |
| Logs | `runtime_log_posix.c`: stdout and stderr go to `logs/dragons_crown_pro_NNN.log` and are echoed to the console through a pipe. |
| Data folders | Next to the AppImage file (`$APPIMAGE`), else next to the executable; `DCP_PORTABLE_DIR` overrides; a read-only AppImage folder falls back to `$XDG_DATA_HOME/DragonsCrownProPC` (`launcher/paths.cpp`). |
| Launcher | The same TRG launcher and installer. File chooser: `zenity` or `kdialog` when installed, otherwise the launcher's own chooser (`launcher/package_browser.cpp`: mouse, keyboard and controller; it lists the program's folder, Downloads, Home and mounted drives such as the SD card). `DCP_FILE_CHOOSER=builtin` or `native` forces one. Open folder: `xdg-open`. Shift at start is read from the X11 server (`launcher/shift_key.cpp`); `--launcher` does the same in any session. |
| Windowing | SDL3 on X11 or Wayland; Vulkan surface, F11, resize and VSync are the existing SDL code. |

## Windows parity

Linux offers every Windows feature. Where the platforms differ, this is the Linux-native equivalent:

| Windows | Linux |
| --- | --- |
| Native file dialog for **Install game...** | `zenity` or `kdialog` when installed, else the launcher's own chooser |
| Hold Shift while starting | Shift is read from the X server in an X11 session; a Wayland desktop shows X11 programs no keys while another window has focus, so there, and in Gaming Mode, use `--launcher` (the Steam Deck's Launch Options) or the desktop entry's **Show launcher** action |
| **Show settings file** selects the file in Explorer | opens the settings folder with `xdg-open` |
| Launcher and game window positions are restored | restored on X11; a Wayland compositor decides where windows go, so the position may not be restored there |
| Data next to `dcp-loader.exe` | next to the AppImage file (the AppImage's own mount is read-only); `$XDG_DATA_HOME/DragonsCrownProPC` when that folder is not writable |
| `tools\package-release.ps1` | builds the AppImage in WSL2 with `tools/package-release-linux.sh`, or run `tools/package-linux.sh` on Linux |

Everything else (display mode, F11, window size, monitor and graphics card selection, VSync, render resolutions,
SMAA, texture filtering, FSR 1 and RCAS, frame rate readouts, the in-game menu, controller and keyboard input, saves,
shader cache, per-run logs, audio, reinstalling without losing user data) is the same code on both platforms.

## AppImage

`usr/bin/dcp-loader` with `DT_RPATH=$ORIGIN/../lib` finds the bundled libraries (SDL3, FreeType, libpng, OpenSSL's
libcrypto, fmt, Zydis, xxHash and their dependencies); no `LD_LIBRARY_PATH` is set, so programs the launcher starts
(`zenity`, `xdg-open`) are not affected. glibc, the Vulkan loader and drivers, X11/Wayland and the audio libraries come
from the system. The Vulkan loader must support Vulkan 1.3: Ubuntu 20.04's own `libvulkan1` (1.2.131) does not, and
the launcher still opens, and Play explains the problem in a dialog of the launcher's own (no `zenity` or other helper)
and stays open, so a driver can be installed and Play pressed again. When the launcher is not shown (**Show this
launcher** off, `--dcp-skip-launcher`) the same text appears in a small window and the program exits with status 1.
Without any `libvulkan.so.1` it cannot start at all.
`libstdc++` and `libgcc_s` are carried in `usr/lib/compat` and used by `AppRun` only when the system's are older than
the build needs. The package contains no game files, no drivers and no local paths (`package-linux.sh`
checks the staged folder before it writes the AppImage).

## Verification

Everything below ran on Ubuntu 24.04 (WSL2) with Xvfb and Mesa's software Vulkan driver (llvmpipe), against the real
package:

- configure, full build (no compiler warnings), `dcp-launcher-tests`, `dcp-runtime-tests` (reservation, mapping,
  protection, executable pages, aliasing, fibers across threads);
- installing the `.pkg` with the launcher's installer: the extracted `eboot.bin` matches the pinned SHA-256 and the boot
  image is byte-identical to the one the Windows build makes;
- the game running natively from the loader and from the AppImage: window, Vulkan device and swapchain, guest start-up,
  threads, audio output, keyboard input, the logo, the save-data dialog and the title menu, with save data and the shader
  cache written next to the AppImage;
- the AppImage's libraries (`ldd`, `readelf`), contents and licences, and its launcher smoke test, also on a clean
  Ubuntu 20.04 with a Vulkan 1.3 loader, where the bundled fallback C++ library is used;
- the launcher's own file chooser installing the package, Shift at start on an X11 server, and the launcher on WSLg's
  Wayland compositor.

Reported by community testers (issue #5): Bazzite with KDE Plasma on Wayland and an AMD Radeon RX 9070 XT (RADV), 4K
borderless fullscreen, and a Steam Deck LCD. Without a graphical session (`DISPLAY` and `WAYLAND_DISPLAY` both unset, or
the window cannot be created) the game prints the SDL error and exits with status 1 before Vulkan starts.

Not verified: NVIDIA and Intel GPUs, other distributions, the Steam Deck OLED, gamescope / Gaming Mode, longer play,
performance.

Known validation-layer findings, shared with Windows and upstream shadPS4: images of 4,194,304 texels or more (a
2048×2048 texture) are detiled with one compute dispatch of more than 65535 workgroups
(`video_core/texture_cache/tile_manager.cpp`), above the `maxComputeWorkGroupCount[0]` of drivers that report the
minimum 65535, such as lavapipe; `vkCmdDispatchBase` cannot split it within that limit, so a fix needs a change to the
tiling shader. Whether real drivers accept it is part of the hardware testing. `VUID-vkCmdDrawIndexed-None-07848`
(stencil state set only while the stencil test is on) appears on every draw.
