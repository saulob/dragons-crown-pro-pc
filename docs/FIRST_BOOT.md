# First boot (phase 2, `feature/first-boot`)

## Result

On this machine (RTX 4070, Vulkan 1.4, Xbox controller) `tools\build.ps1 -Run` runs Dragon's Crown Pro through the native
loader and the shadPS4-derived video core at a steady 60 FPS. Observed, with one scripted Cross press:

1. `Create save data? / This game handles autosave.` (game-drawn prompt; press Cross / Xbox A)
2. CRIWARE logo, the opening movie, then the **title screen** (`Press ✕ button`, `©ATLUS ©SEGA`), stable.

Not verified: anything past the title screen. The visual defects seen here (flicker, stale rectangles, a missing glyph) were
fixed in `feature/graphics-correctness`, see [GRAPHICS.md](GRAPHICS.md).

## Blockers in the order they were hit

| # | Stop | Resolution |
|---|---|---|
| 1 | `sceLibcHeapGetTraceInfo` (libc.prx initialiser) | `runtime_dcp.c`: fills the output struct with valid pointers (the caller clears `*mspace_id_mask`), no tracing. From tlg_pc `runtime_tlg.c`; medievil's not-found would leave null pointers. |
| 2 | `sceVideoOutOpen` | Video core vendored (`gpu/`, from uncharted-ndc-native, see below). Registers Gnm, VideoOut and the kernel event queues. |
| 3 | `sceKernelIsNeoMode`, event flags | `runtime_dcp.c` (base PS4), `runtime_eventflag.c` (gr2_pc implementation, checked with a standalone semantics test). |
| 4 | `sceGameLiveStreaming*`, `sceCoredump*` | No-ops in `runtime_dcp.c` (medievil contracts). |
| 5 | `scePthreadCancel` | `runtime_thread.c`: ESRCH for unknown or finished threads, stops on a running one. |
| 6 | libSceFont | `runtime_font.c`: FreeType implementation from gr2_pc trimmed to the 22 imports; `SupportSystemFonts`, `AttachDeviceCacheBuffer` and `OpenFontSet` added. The game opens its own embedded font (`FOT-Skip Std B`) through `OpenFontMemory`; the firmware font set it also asks for is answered with the built-in Noto Sans (`third_party/fonts`), or with the built-in Noto Sans TC for the Chinese set `0x180CAC44` the Asia and Hong Kong build opens, so no PS4 system fonts are needed. |
| 7 | `sceSaveDataGetMountInfo`, `sceCommonDialogIsUsed`, ErrorDialog | `runtime_savedata.c`, `runtime_services.c` (existing dialog model). |

Event queues come from the video core (`ndcgpu_register_kernel` equivalent), so no second event model exists.
NpToolkit2 initialises without any Np import being reached; Rudp, NpTus, NpMatching2 were never reached.
Audio (WASAPI via SDL3) and the pad (Xbox One S) initialise with the existing runtime. Imports still missing in the image
(`tools\build.ps1 -Run -CheckImports`): 114, none reached so far.

## Video core

`gpu/` is `uncharted-ndc-native/gpu` (itself `medievil_pc/gpu`; shadPS4 `b4e7ae7`): same tree minus `videodec2*` (the game
does not import it), `ndcgpu` renamed `dcpgpu`, `ndc_setjmp` → `dcp_setjmp`, `ndc.ini` → `dcp.ini`. `sirit`, `gcn`, `half`
and the font are vendored; `imgui`, `magic_enum`, `miniz`, `xbyak` are fetched at the pinned commits of `gpu/deps.txt`
by `tools\build.ps1` and git-ignored. The inherited `gow3_*` shim names and `GOW3_*` switches were renamed or removed in the
release review (docs/GRAPHICS.md). The first run compiles shaders on demand; later runs reuse them from `user\cache`.

## Saves

The first start creates `user\savedata\1\CUSA10454\SAVEDATA00` (the game writes `SAVE0.DAT`). Remove `user\savedata` to
see the first-run prompt again.

## Commands

```powershell
tools\build.ps1 -Run   # prepares the game on the first run; log: build\logs\dragons_crown_pro_NNN.log; Cross = A on an Xbox pad, Enter or the key map otherwise
```
