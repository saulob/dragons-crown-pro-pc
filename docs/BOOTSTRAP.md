# Bootstrap (phase 1) and first-boot hand-off

Branch `feature/project-bootstrap`. Goal: a reproducible foundation, not a running game. Architecture and reference
choices: [REFERENCES.md](REFERENCES.md).

## Architecture

The PS4 x86-64 code of the game runs directly on the host CPU (not an emulator, not static recompilation).
`src/` replaces the PS4 system libraries; a shadPS4-derived video core will translate GNM to Vulkan (`src/dcpgpu.h`
is its interface; vendored in phase 2, see [FIRST_BOOT.md](FIRST_BOOT.md)). Pipeline:

```
game\*.pkg --pkg_extract--> game\extracted\base --hard links--> game\prepared\v1.03   (the /app0 tree)
                                                                  | prepare_image.py
                                                                  v
                                       build\image\v1.03\{boot.bin, content.bin, image.json}
                                                                  | dcp-loader.exe
                                                                  v   log: build\logs\dragons_crown_pro_NNN.log
```

`prepare_image.py` maps `eboot.bin` plus the bundled modules in profile order (`libc.prx`, `libSceFios2.prx`,
`libSceNpToolkit2.prx`), records relocations and binds each import either to an export of those modules (native) or to
a runtime name. The loader maps the image, applies relocations, runs module initialisers and enters the eboot; the first
unresolved import stops the process (exit 20) with its name and caller.

## Game metadata (read from the package, read-only)

| | |
|---|---|
| Package | A fake PKG of the game in `game\`, 5,839,257,600 B (header agrees), the only file |
| Title / title ID | Dragon's Crown Pro / **CUSA10454** (US build; the Japanese build is CUSA07001 and the Asia and Hong Kong build CUSA10209, see [LAUNCHER.md](LAUNCHER.md)) |
| Content ID | `UP2611-CUSA10454_00-DRAGONSCROWNPRO1` |
| Category | `gd` (base game); fake PKG (image key index 3), no delta info |
| `APP_VER` / `VERSION` | `01.00` / **`01.03`**; `SYSTEM_VER` 0x4700000 |
| Pubtool info | built 2020-12-25, SDK 4.508 (eboot procparam SDK `0x4508101`) |
| Update | None present, none needed to run. Whether `VERSION 01.03` is a patched base cannot be proven from the metadata; no overlay is done. |
| Contents | 9 files, 5.7 GB: `DRACRO.CPK` (5.26 GB), `DRACRO_US.CPK` (0.87 GB), `eboot.bin` (22.3 MB), `sce_discmap.plt`, `sce_module/{libc,libSceFios2,libSceNpToolkit2}.prx`, `sce_sys/` |
| Executables | `eboot.bin` only, plaintext SELF (no decryption step); sha256 `f9bff150…508a`, entry `0x54c690`, 12 segments, no static TLS, 379,352 relocations |
| Modules | `libc.prx` (2473 exports), `libSceFios2.prx` (168), `libSceNpToolkit2.prx` (2357); all plaintext SELF. No other PRX. |
| Libraries | 37 `NEEDED` by the eboot (Gnm/GnmDriver, VideoOut, Pad, AudioOut, Np* stack, Net/Http/Ssl/Rudp, SaveData, Font/FontFt, dialogs, Fiber, Ult, ...) |

The profile is `scripts/game_profiles.py`; only the facts above are in it.

## Game preparation model

| Path | Content | Tracked |
|---|---|---|
| `game\*.pkg` | User's package, never modified | no |
| `game\extracted\<folder>` | Unpacked package (resumable, `.pkg_extracted` marker). `core` scope = eboot, modules, `sce_sys`, `.plt` (25 MB); `full` = also both CPK archives (5.7 GB) | no |
| `game\prepared\v1.03` | Hard-link view that reproduces `/app0` | no |
| `build\image\v1.03` | Boot image and `image.json` | no |
| `user\` | Saves, caches at run time | no |

There is a single layer, so there is no overlay order to decide; `game_view.build_view` already takes a layer list if
an update ever appears. Core scope is enough for the loader now; the CPKs are only read by the game at run time
(`tools\prepare-game.ps1 -Scope full` before first boot).

## Tools (PowerShell 5.1 or 7, paths with spaces verified)

| Command | Does |
|---|---|
| `tools\setup.ps1 [-InstallPython]` | Reports Python, `cryptography`, MSYS2 CLANG64 (clang, CMake, Ninja, pkgconf, SDL3), video-core libraries (warn only), package, free disk. `-InstallPython` creates `.venv` with `cryptography` only. |
| `tools\inspect-game.ps1 [-List]` | PKG metadata, profile check, executables/modules report once extracted. Read-only. |
| `tools\prepare-game.ps1 [-Scope core\|full]` | Optional, development only: extract a fake PKG into `game\extracted\base` and build the view (Python). The launcher's Install game... unpacks the same `.pkg` natively. |
| `tools\build.ps1 [-Run] [-Clean]` | The normal command. Updates what is stale (GPU deps, CMake configuration, the boot image of an installed game, natively), builds incrementally; `-Run` launches that binary and logs to `build\logs\dragons_crown_pro_NNN.log` (next free number). `-CheckImports` (diagnostic) lists imports the runtime lacks. |
| `tools\run.ps1` | Wrapper for `tools\build.ps1 -Run`. |
| `tools\clean.ps1 [-Prepared] [-Logs] [-WhatIf]` | Removes `build\`, `dist\`; optionally the link views and logs. Never touches the `.pkg`, `game\extracted` or saves. |
| `scripts\import_inventory.py` | Import inventory, see below. |

## Toolchain

MSYS2 **CLANG64** only (found at `C:\msys64`, or `MSYS2_ROOT`): clang 22.1.7, CMake 4.3.3, Ninja, pkgconf, SDL3. Already
installed here; nothing was installed except `cryptography` in `.venv`. For first boot the video core also needs Vulkan
headers/loader, VMA, glslang, fmt, boost, robin-map, xxhash, libpng, zydis, spirv-* (`setup.ps1` prints the `pacman` line).
`CMakeLists.txt` refuses non-clang compilers. Win10 1903 APIs, `--disable-high-entropy-va` and a 16 MiB committed stack are
required by the loader.

## What is built

`dcp-loader.exe`: `loader.c` + runtime (libc contracts, mutex/cond/rwlock/semaphore, threads/TLS, direct and flexible
memory with the PS4 address reservation, kernel, files `/app0` and user dirs, time/RTC, system/user services, save data,
AppContent, fibers, AudioOut and Pad on SDL3) + GPU stub. 19 C sources (loader, runtime, compat, GPU stub), `-Wall -Wextra -Werror`, no warnings.
Dropped from the Uncharted copy: Ajm and MP3 (no `libSceAjm` import here).

Names: public identifiers are `DCP_*` / `dcp-loader` / `dcpgpu_*`. Imports in `src/import_names.inc` are regenerated for
this game (`scripts/gen_import_names.py --aerolib <medievil_pc\tools\aerolib.inl>`; `aerolib.inl` is not vendored).
Environment variables keep the runtime's names with the `DCP_` prefix (`DCP_USER_NAME`, `DCP_FILE_TRACE`, `DCP_DMEM_MB`,
`DCP_KEY_MAP`, `DCP_PAD_MAP`, ...).

## Logging and diagnostics

`build\logs\dragons_crown_pro_NNN.log` starts with the command line, prepared root, CUSA, `APP_VER`, eboot hash prefix, entry, image size,
SDK, module list and import counts, then everything the loader prints and `[run] exit code N`. The Windows loader already
carries a fault reporter (registers, stack words, rbp chain with guest offsets); no minidumps.

## Import inventory (`scripts\import_inventory.py`, planning only)

749 unique imports across the eboot and the three modules:

| Group | Count |
|---|---|
| Provided by the game's own modules (`libc` 62, `libSceNpToolkit2` 55, `libSceLibcInternal` 38, `libSceFios2` 11) | 166 |
| Implemented by the inherited runtime | 324 |
| Missing | 259 |

Missing by library: Gnm driver 97, Font 20, NpTus 18, NpMatching2 17, Rudp 17, libkernel 16, NpScore 7, VideoOut 7, NpTrophy 6,
ErrorDialog / GameCustomData / InvitationDialog 5 each, then 1–4 each in about 20 more. The 16 libkernel gaps are
`sceKernelCreateEqueue`, the event-flag family (6), `IsNeoMode`, `scePthreadCancel/Getname`, `_nanosleep`, two local-time
conversions and three diagnostic queries. Comparison with Uncharted, MediEvil and GoW3 is in [REFERENCES.md](REFERENCES.md);
in short Dragon's Crown needs fewer libraries than Uncharted and is closest to MediEvil.

Commands:

```powershell
tools\build.ps1 -Run -CheckImports   # runtime's own list of imports it cannot bind; nothing runs
.venv\Scripts\python.exe -I scripts\import_inventory.py --check-log build\logs\dragons_crown_pro_NNN.log `
  --reference medievil=<ref>\medievil_pc\src\import_names.inc --source medievil=<ref>\medievil_pc\src,<ref>\medievil_pc\gpu\shadps4,<ref>\medievil_pc\gpu\shim
```

## Validation done

- Original package untouched (size and mtime unchanged, only opened for reading); `game/` ignored, extracted, prepared
  and build trees ignored (`git check-ignore`), no game file tracked.
- Incremental build: second `tools\build.ps1` reports no work; clean build had no warnings.
- `loader.c` and the runtime are identical to the Uncharted sources apart from renames, the default window title and the
  removal of Ajm, so the mapping/relocation/protection code was not changed.
- One controlled loader run (`tools\run.ps1 -TimeoutSeconds 60`, an early diagnostic run): maps 18,841,696 bytes in 12 segments, applies 381,806
  relocations, enters the `libc.prx` initialiser and stops at the first unresolved import, as designed. Not a boot attempt.

## First-boot hand-off (`feature/first-boot`)

| Item | State |
|---|---|
| Title / version | CUSA10454, `VERSION 01.03` (`APP_VER 01.00`) |
| Entry | `eboot.bin`, entry `0x54c690`, SDK `0x4508101` |
| Prepared layout | `game\prepared\v1.03`, installed by the launcher (or `tools\prepare-game.ps1 -Scope full` from a PKG) |
| Modules | `libc.prx`, `libSceFios2.prx`, `libSceNpToolkit2.prx` linked at `0xed0000`, `0xfd0000`, `0x1160000` |
| Loader | Maps, relocates, enters module initialisers. Stops at unresolved imports. |
| Runtime | 324 of the 583 runtime-side imports implemented; 259 missing |
| GPU | Stub. Decision: take `uncharted-ndc-native\gpu` (see REFERENCES.md), rename `ndcgpu.cpp` → `dcpgpu.cpp`, drop `videodec2*`, wire into `CMakeLists.txt`, pins in `deps.txt`. |
| First missing import | `sceLibcHeapGetTraceInfo` (`NWtTN10cJzE#libSceLibcInternalExt`), called by the `libc.prx` initialiser at module offset `+0x64288`. Reference: tlg/gr2/iss `runtime_tlg.c` (`libc_heap_trace_info`, reports no tracing) and medievil `runtime_extra.c` (returns not-found). |
| Then likely | Event queues (`sceKernelCreateEqueue`) and event flags, which the references register from the video core / kernel; `sceKernelIsNeoMode`; libSceFont (gr2_pc has a FreeType implementation); NpToolkit2 module init needing Np answers as an offline console (medievil `runtime_extra.c`). |
| First graphics requirement | 97 Gnm driver + 7 VideoOut imports, i.e. the whole video core (Vulkan device, window, VideoOut buffers, flip/vblank events). |
| Risks | No `fs:` absolute accesses and no static TLS found in the eboot (heuristic scan), `libc.prx` has TLS. The Windows TCB redirect found 0 sites, so verify TLS behaviour early. Game data is two CRI CPK archives, presumably read through Fios2 (`sce_discmap.plt` is present). |

First controlled boot attempt:

```powershell
tools\build.ps1 -Run
```

Recommended first action: add `sceLibcHeapGetTraceInfo` in a new `src/runtime_dcp.c` (title-specific imports kept apart, as
medievil's `runtime_extra.c`), rerun, and only then vendor the video core.

## Deferred on purpose

Video core vendoring and its dependencies, any rendering, Font/Np/Rudp/dialog imports, input and audio behaviour, CPU
compatibility rewriting (gow3 `isa_rewrite`), save-data behaviour, launcher, patches, performance work.

## Canonical build

One configuration, `RelWithDebInfo` (`-O2 -g -DNDEBUG`, the CMake default in `CMakeLists.txt`), in `build\cmake\clang64`. It is what `tools\build.ps1 -Run` launches and what a package step must take as it is; there is no separate Release tree. Nothing in `CMakeLists.txt` or `gpu\CMakeLists.txt` branches on the build type, and the video core defines `NDEBUG` itself, so debug info is the only difference from Release.
