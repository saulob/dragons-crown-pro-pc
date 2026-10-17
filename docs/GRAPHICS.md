# Graphics

## Correctness fixes

Three fixes made Dragon's Crown Pro render correctly. All three are code, not settings: neither `dcp.ini`, the launcher,
the in-game menu nor the environment can undo them (`tests/launcher_tests.cpp` checks this).

| Symptom | Cause | Fix |
|---|---|---|
| Ghost rectangles over the opening movie and the title smoke, a missing glyph (`Cre te save data?`) | Draw-preparation workers read a vertex stream's V# from guest memory before the command stream had written it | `Rasterizer::ResolveVertexBuffers` re-reads every V# on the GPU thread and uses prepared inputs only when they are identical; the workers are off (`vk_draw_prep.cpp`) |
| ATLUS logo and fairies flickering | Draws skipped while their new graphics pipeline compiled asynchronously | `PipelineCache::AsyncCompilation()` is always false: a new pipeline is built before its draw |

Also fixed: the GPU command thread stays at most one frame ahead of the GPU (`Presenter::PrepareFrame`), and the VideoOut
vblank is a steady 60 Hz with no frame limit, the game's own pacing.

The first one was found with window captures and frame-difference measurements on the title screen: disabling the workers
alone removed the rectangles, while the Vulkan recording thread, the draw pipeline and the present thread did not change
them. The same worker code exists in the other ports of this lineage (docs/REFERENCES.md).

## Presentation

Every frame, on the GPU command thread: the game's display buffer, at the render resolution, goes through **SMAA 1x**
(`host_passes/smaa_pass.cpp`, optional), then **FSR 1** when the window is larger than the picture and **RCAS** sharpening
(`host_passes/fsr_pass.cpp`), then the post-process pass into the presentation frame: the largest size with the picture's
aspect ratio that fits the window. The present thread blits that frame, letterboxed, into the swapchain image and draws the
in-game menu and the frame counter over it. FSR 1 does nothing when the window is the picture's size or smaller; RCAS alone runs only when the
window and the picture have the same size. All of them follow the settings live.

Swapchain recreation (a window resize, F11, a display mode or VSync change) waits for the device under the queue submission
lock, because the GPU command thread keeps submitting meanwhile. The in-game menu's redraw of the last frame takes that frame
out of the free queue while it shows it, so the GPU thread can neither draw into it nor recreate it at a new window size.

## Render resolution

`launcher/resolution.c` rewrites the build's 23 display-target sizes (`mov reg, imm32` of 1920 / 3840 and 1080 / 2160;
the tables are in `launcher/game_profile.cpp`) after checking each still holds its original instruction; the game does not
start if one does not. Native in 16:9 leaves the code untouched; the Aspect Ratio setting changes the width of the target
(docs/LAUNCHER.md). `sceKernelIsNeoMode` answers PS4 Pro to the game for a target larger than 1920 × 1080 (1440p, 1800p,
4K and the wider shapes), which selects its larger graphics
memory path; the video core's own GnmDriver packets stay base PS4, which the emulated command processor accepts either
way. In that mode the game tiles textures with the PS4 Pro (alt tile) layout, which the detiler follows from the texture
descriptor: 16 pipes and the alternate bank parameters.

## Texture filtering

Texture filtering 2x to 16x replaces the filter of the game's mipmapped bilinear samplers in graphics draws with anisotropic
filtering, unless the game already asks for at least as much; depth samplers and compute shaders keep theirs. Samplers are
cached by their final state and the device's `maxSamplerAnisotropy` and `samplerAnisotropy` feature limit what reaches
Vulkan.

## Removed

The video core came with God of War III's temporal upscaler (FSR 3.1, FSR 4, DLSS and TAA), its camera and object motion
vectors and their settings. Dragon's Crown Pro never ran them; they were removed, together with every inherited environment
switch. HDR, MSAA and a frame rate limit are not offered (docs/LAUNCHER.md).

## Developer diagnostics

Off unless set to `1` (or a value), never written to `dcp.ini`, and none changes what is drawn:

| Variable | Effect |
|---|---|
| `DCP_VK_VALIDATION`, `DCP_VK_VALIDATION_SYNC` | Vulkan validation layers (core, synchronization) |
| `DCP_VK_MARKERS` | Vulkan debug labels for graphics debuggers |
| `DCP_DUMP_SHADERS` | Shader dumps under `user\shader\dumps` |
| `DCP_GPU_LOG` | Video core log level: `trace`, `debug`, `info`, `warning` (default), `error` |
| `DCP_FRAME_STATS` | Frame rate, pacing and stall causes every 5 seconds |
| `DCP_GPU_PROFILE` | GPU time per render pass and dispatch every 5 seconds |
| `DCP_PERF_DIAG`, `DCP_RECORDER_TRACE`, `DCP_DCB_STATS`, `DCP_WARN_META_TEXTURE` | Wait, recorder, command buffer and metadata diagnostics |
| `DCP_CAPTURE_TRIGGER=<file>`, `DCP_CAPTURE_DIR` | Creating the file records the passes of the next frame (default folder `user\captures`) |
| `DCP_WRITE_LOG=1` or `2` | The GPU's last writes to guest memory, printed with a fault report |

## Runtime switches

The PS4 runtime's own environment variables, for testing and diagnosis:

| Variable | Effect |
|---|---|
| `DCP_LANGUAGE` | System language number reported to the game (default 1, English); overrides the launcher's Menu & Subtitles |
| `DCP_USER_NAME` | Player name reported to the game (default `Player`) |
| `DCP_KEY_MAP`, `DCP_PAD_MAP` | Keyboard or controller button remap, e.g. `cross=Q,square=Left Shift` (`runtime_pad.c`) |
| `DCP_PAD_FILE`, `DCP_PAD_RECORD`, `DCP_PAD_REPLAY` | Scripted input, F9 recording and replay for automated runs |
| `DCP_IME_TEXT` | Answers the game's text dialog with this text |
| `DCP_AUDIO=none` | No audio device (a silent timer sink); `DCP_AUDIO_DUMP`, `DCP_AUDIO_STATS` dump or count audio |
| `DCP_FILE_TRACE`, `DCP_TRACE_SEMA` | Every file the game opens; blocking semaphore waits |
| `DCP_STRICT_HANDLES` | Invalid Windows handle use raises an exception (fault report) |
| `DCP_DMEM_MB` | Direct memory reported to the game, raised above the retail 5056 MiB (up to 16384) |
| `DCP_LAUNCHER_SHOTS` | Used by `tests/launcher_shots.cpp`: renders the launcher pages into PNGs and quits |
