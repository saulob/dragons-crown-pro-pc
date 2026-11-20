# Reference projects

Local clones of the projects below were inspected before choosing a base. Everything here was read from those trees
(code, scripts, `git log`, diffs), not assumed. Folder to project:

| Folder | Project | HEAD (commits) |
|---|---|---|
| `uncharted-ndc-native` | Uncharted: NDC port by this project's author | 2026-10-14 (2) |
| `medievil_pc` | bc160201983/medievil_pc | 2026-10-13 (231) |
| `gow3_pc` | FelipePrado0/gow3_pc | 2026-10-13 (366) |
| `tlg_pc`, `gr2_pc`, `iss_pc` | HayatoG ports (code under `port/`) | 2026-10-13 (70 / 83 / 121) |
| `bloodborne_pc_windows` | Phroster/bloodborne_pc_windows | 2026-10-07 (149) |

All are GPL-2.0-or-later, share one lineage (`bloodborne_pc` → `gow3_pc` → the rest), the same split
(loader + `runtime_*.c` + `gpu/shadps4` video core + Python preparation scripts) and none does static recompilation.
Every `gpu/VENDOR.txt` pins the same shadPS4 commit `b4e7ae7`.

## Verdicts

| Question | Choice | Evidence |
|---|---|---|
| Overall base | **uncharted-ndc-native** | Already a de-GoW'd, renamed, CLANG64-proven derivative of medievil_pc, with the only PowerShell workflow, resumable PKG extraction, `image.json` import report and a log tee. Reached title screen/menus/audio/input on a non-GoW title. |
| Loader | uncharted `loader.c` (= medievil `probe.c` minus hooks/patch/restart code) | Diff of our `loader.c` against it: one string. gow3_pc/HayatoG `cpu_check`/`isa_rewrite` stay available if a CPU fault ever needs them. |
| Generic runtime | uncharted `runtime_*.c`, minus Ajm/MP3 | Same files as medievil's runtime without GoW hooks. Ajm/MP3/ATRAC9 dropped: Dragon's Crown imports no `libSceAjm`. |
| PKG extraction | medievil `pkg_extract.py` as carried in uncharted | Only references that unpack PKGs themselves; gow3 and the HayatoG ports expect an already unpacked folder. |
| Title profile | medievil `game_profiles.py` as carried in uncharted (keyed by `TITLE_ID`, per-version SHA-256) | HayatoG ports use per-game scripts instead. |
| Game-specific runtime file | medievil `runtime_extra.c`; gr2_pc `runtime_gr2_font.c`/`runtime_gr2_np.c`; tlg_pc `runtime_tlg.c` | Pattern for isolating per-title imports. See the inventory below for what maps onto Dragon's Crown. |
| PowerShell workflow | uncharted `tools\*.ps1` | The others only ship `build.sh`/`run.sh`/`run.py`. |
| Windows build | CMake + Ninja on CLANG64, `--disable-high-entropy-va`, 16 MiB committed stack, Win10 1903 API level | Same flags as medievil/gow3 `packaging/windows`, wrapped by `tools\build.ps1` (no bash driver needed). |
| Logging | uncharted `run_game.py` tee, numbered logs as in hard-corps-uprising-recompiled and the loader's Windows fault reporter | gow3/medievil `run.py` write no log file. The Windows loader also writes a minidump beside the session log after a fatal exception (the dump thread and the pairing of a dump with its log follow alienation-pc's crash guard); Linux has none. |
| Import discovery | uncharted/medievil `gen_import_names.py` + `prepare_image.py` `image.json` + loader `--check-imports` | Extended here by `scripts/import_inventory.py`. |
| GPU / video core | **uncharted `gpu/`** (decision below) | |

## GPU / video-core decision

All candidates are shadPS4 `b4e7ae7` plus the gow3 shim. Compared with `diff -rq --strip-trailing-cr`:

| Tree | Files (excl. third_party) | `gow3_*` shim files | vs uncharted `gpu/shadps4` | Proven on |
|---|---|---|---|---|
| uncharted `gpu/` | 439 | 16 | — | Uncharted (non-GoW): intro, title, menus at ~60 FPS |
| medievil_pc `gpu/` | 442 | 21 (adds `gow3_actors`, `gow3_orbs`) | identical except uncharted adds `videodec2*` (5 files) and one log line in `vk_instance.cpp` | MediEvil, playable |
| gow3_pc `gpu/` | 449 | 21 | 45 files differ | God of War III |
| tlg/gr2/iss `port/gpu` | 391–403 | — | 110–125 files differ | Older copies of gow3's core with game-specific fixes |

Uncharted's tree is medievil's with the GoW actor/orb shims removed and no extra code, so it has the least
contamination (`gow3:` marker lines: 286 vs 318 in gow3_pc). Its `videodec2` addition is unused here (no
`libSceVideodec2` import) and can be left out. Build cost is shared by all: C++23 with clang, six pinned third-party
sources (`gpu/deps.txt`), about twenty MSYS2 packages. Vendored in phase 2 once the boot path reached `sceVideoOutOpen` (see FIRST_BOOT.md);
`src/dcpgpu.h` is the interface. Cherry-pick from tlg/gr2/iss only when a symptom matches (RDNA2 black lines, multisampling).

## What was taken

| From | Taken (renamed `ndc` → `dcp`) |
|---|---|
| uncharted-ndc-native | `tools\*.ps1` (reworked), `scripts/` (`pkg_extract`, `pkg_info`, `pkg_keys`, `ps4_module`, `prepare_image`, `game_view`, `gen_import_names`, `run_game`, reworked `game_profiles`/`prepare_game`), `src/loader.c`, `runtime*.c`, `compat_win.*`, `import_names.inc` (regenerated), CMake flags, `.gitignore`/`.gitattributes`, `LICENSE` |
| medievil_pc | (via uncharted) PKG/profile/import-name scripts; `tools/aerolib.inl` used as the NID→name source for `gen_import_names.py`; imported as evidence for NpToolkit2 handling (`runtime_extra.c`) |
| gow3_pc | (via uncharted) loader and runtime architecture; GPU lineage |
| gr2_pc / tlg_pc | Evidence only: `sceLibcHeapGetTraceInfo` (`runtime_tlg.c`), libSceFont/FontFt on FreeType (`runtime_gr2_font.c`) |
| bloodborne_pc_windows | The approach of its generic anisotropic sampler override, without its title hooks (texture filtering) |
| shadPS4 `d339b3f7` | The PKG extractor (`pkg.cpp`, `pfs.h`, `pkg_type.cpp`, `src/core/crypto`) as it was before `be22674f` removed it, adapted as `launcher/pkg_extract.cpp` with Windows CNG in place of Crypto++ |

Not taken: Uncharted specifics (CUSA02320, `big2`/`big3`, LoadExec switching, `u*data`, offsets, hashes, `NDC_`), GoW
cheats/patches/signatures/launcher/upscalers, MediEvil's UE4 override, HayatoG hooks and per-game runtimes.
The inherited `gow3_*` shim files and `GOW3_*` switches were later renamed to `dcp_*` or removed (gpu/VENDOR.txt).

## Import profile comparison

Computed by `scripts/import_inventory.py` from the prepared image, the loader's `--check-imports` and each
reference's `src/import_names.inc` (the imports of the game it was built for) and sources.

| | Dragon's Crown Pro | Uncharted (NDC) | MediEvil | GoW3 |
|---|---|---|---|---|
| Libraries imported | 48 (37 `NEEDED` by the eboot) | 35 in the name table (52 `NEEDED`) | 66 | 41 |
| Imports (eboot + bundled modules) | 749 | 577 named (479 eboot) | 1420 | 778 |
| Of our 583 runtime-side imports, also imported by it | — | 226 | 526 | 302 |

Dragon's Crown needs **fewer libraries than Uncharted (37 vs 52 `NEEDED`)** and none of Uncharted's Ajm, Videodec2, PlayGo,
Mouse, Remoteplay, Voice or image-decoder libraries, so the CPU side is simpler. It differs by importing the whole Np stack through the
game's own `libSceNpToolkit2.prx`, like MediEvil (which shares 526 of its 583 runtime-side imports). Of the 259 imports
the inherited runtime lacks, 104 are Gnm/VideoOut (video core), 22 Font/FontFt, 17 Rudp, and the rest Np/dialog/kernel.
Names present in a reference's code: medievil 158 of 259, uncharted 109, gow3 109, gr2 37, iss 39, tlg 12. The
**101 that no medievil/uncharted/gow3 code names**: Font 20, NpTus 18, NpMatching2 17, Rudp 17, NpScore 7, NpManager 4,
NpUtility 3, NpWebApi 3, Rtc 3, FontFt 2 and a few singles. libSceFont has a FreeType implementation in gr2_pc.
