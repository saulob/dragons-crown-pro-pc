# Graphics

## Correctness fixes

Three fixes made Dragon's Crown Pro render correctly. All three are code, not settings: neither `dcp.ini`, the launcher,
the in-game menu nor the environment can undo them (`tests/launcher_tests.cpp` checks this).

| Symptom | Cause | Fix |
|---|---|---|
| Ghost rectangles over the opening movie and the title smoke, a missing glyph (`Cre te save data?`) | Draw-preparation workers read a vertex stream's V# from guest memory before the command stream had written it | `Rasterizer::ResolveVertexBuffers` re-reads every V# on the GPU thread and uses prepared inputs only when they are identical; the workers are off (`vk_draw_prep.cpp`) |
| ATLUS logo and fairies flickering | Draws skipped while their new graphics pipeline compiled asynchronously | `PipelineCache::AsyncCompilation()` is always false: a new pipeline is built before its draw |

Also fixed: the GPU command thread stays at most one frame ahead of the GPU (`Presenter::PrepareFrame`), and the VideoOut
vblank is a steady 60 Hz with no frame limit, the game's own pacing (the Frame rate setting never changes it, see
docs/LAUNCHER.md).

The first one was found with window captures and frame-difference measurements on the title screen: disabling the workers
alone removed the rectangles, while the Vulkan recording thread, the draw pipeline and the present thread did not change
them. The same worker code exists in the other ports of this lineage (docs/REFERENCES.md).

## Presentation

Every frame, on the GPU command thread: the game's display buffer, at the render resolution, goes through **SMAA 1x**
(`host_passes/smaa_pass.cpp`, optional), then **FSR 1** when the window is larger than the picture and **RCAS** sharpening
(`host_passes/fsr_pass.cpp`), then the post-process pass into the presentation frame: the largest size with the picture's
aspect ratio that fits the window. The present thread blits that frame into the swapchain image and draws the in-game menu
and the frame counter over it. Presentation Letterbox (the default) fits it with its aspect ratio, bars included
(`FitImage`, `MakeImageBlitFit`); Presentation Stretch fills the whole swapchain (`MakeImageBlitStretch`). The value comes
from `dcp.ini` at game start and the presenter checks it every frame. FSR 1 does nothing when the window is the picture's size or smaller; RCAS alone runs only when the
window and the picture have the same size. All of them follow the settings live.

`Presenter::Present` blacks out the swapchain picture's ultrawide side areas, outside its centred 16:9, right after
blitting the finished frame into it and before the layout transition to present: the latest colour write before the
picture reaches the screen, so nothing later (the overlay draws over, never under, and the present itself copies
nothing new) can put a stale picture back into them. It runs whenever `src/ultrawide_hooks.c` says the real loading
or saving fade, or the fixed-screen transition fallback below, is active for the frame being built
(`dcpgpu_debug_saulo_ultrawide_mask`, a frame-scoped flag read in `PresentUltrawideSideMask`); the areas are
`gpu/shim/dcp_ultrawide_capture.h`'s `SidesOf`, empty at 16:9 or narrower. The centre, and the crest or scene behind
it, are untouched.

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

## True Ultrawide

The game builds an orthographic projection for each of its 43 scene passes once, in its pass setup loop, from fixed 16:9
canvases that ignore the render target: 812 × 456 for the world passes and 1280 × 720 for screen-space ones such as the
art panels, fades and manual pages. A wider target therefore stretches them. With True Ultrawide on and a 21:9 or 32:9 target,
the loader points the six `vmovss xmm0, [rip+disp32]` loads of those canvases' left and right extents (image offsets
0x10686, 0x10699, 0x106bb, 0x106d5, 0x106f7 and 0x10703, the same in every verified build) at a read-only page after the
image that holds the extents times the target's shape over 16:9. The top and bottom extents, the game's constants, the
viewports and the presenter are unchanged, so the picture is Hor+: the same vertical view, more of the world at the sides.
Both canvases widen together, which keeps world-anchored UI (health bubbles, signs) over its objects. Before writing, the
loader checks each load's opcode and that its constant holds -640, 640, -406 or 406; if one does not, nothing is written.

Guest hooks (`src/ultrawide_hooks.c`, one record per verified build) install in three groups; a group whose sites do not
match stays off on its own and the log line says which:

- Fill: the colour, textured and gradient quad builders all hand their quad to one 2D submit. At those three calls, a quad
  with no texture and no model matrix, drawn in a 1280 × 720 pass and spanning that whole canvas (fades, menu dims,
  cinematic bars), gets its x scaled by the same factor. Textured quads and everything narrower keep their place.
- World windows: each frame the game copies every pass's matrices and viewport into that frame's set, which the
  executor draws from. Just after each copy, the hook may narrow a world pass's set: its viewport (left, top, right,
  bottom, as the executor programs it) and the inverse clip-space scale and offset on its projection and
  view-projection, so every world point keeps its pixel and only clipping changes. Stage layer i draws in pass i + 1
  with its own parallax (the camera's stage, +0xb8 map, layer pointers at +0x368, x parallax at layer +0x348). Layers
  that move with the playfield layer (stage layer 15, pass 0x10, where characters walk) or faster are clipped to the
  stage's limits (camera +0xe4/+0xe8, seen from where the game put the camera) and to where the playfield layer draws,
  never narrower than the centred 16:9; distant layers keep the whole width and are drawn first, so they show wherever
  a nearer layer stops. A one-screen stage without a playfield layer is an illustration (the guilds, the shop, the
  inn): every world pass keeps the centred 16:9, and each part that covers the whole 16:9 view (the room) is queued
  in the set of the world pass just before its own instead, in a priority bucket after everything already there. That
  set gets the room's pass matrices scaled evenly by the widening and its rows kept to the room's own height, so the
  room fills the frame behind the rest of its pass and still covers what it covered (the inn draws its tavern room in
  pass 0x03 over a hidden picture in pass 0x01). Only the first such part of a pass and the other full-view parts of
  its model are backdrop, and the rows of that first part bound the zoomed picture. A part whose polygons are all bands
  across the 16:9 at the top or bottom of the frame is the screen's black matte (drawn after the NPC). The matte with
  the parts of its pass queued after it (title plate, choice windows), and a full-view part of another model (the guild
  and shop title frame with its long lines, drawn last), go to the set of the next, unused world pass instead, which
  gets their pass's view unzoomed at the whole width: they still draw over the rest of their pass, and the matte's bars
  and the frame's lines reach the frame edges. The game's own pass data and camera are never changed. While the route
  map widget (`DcUi_WorldMap`) draws, the world and pass 0x26 are clipped to the centred 16:9 instead. Entering a
  guild or shop, the game's camera reaches the illustration's narrow stage a frame before any part of its room backs
  it, and leaving one, the room stops before the camera does: on such a frame (illustration, no backdrop found) every
  world pass still only gets the centred 16:9 above, with no part moved into another pass's set, and the final
  presenter blacks out the sides (above) instead of showing whatever was there before. Once a backdrop is found again
  the frame above resumes normally, and once the camera leaves the illustration's stage gameplay resumes; nothing
  is timed or guessed, only `committed.illustration` and whether a backdrop was found that same frame.
- Parts: at the model part draw's call of the render queue, a part's world matrix is adjusted before it is queued. Parts
  of the narration band (message widget type 0x26), and of the dialogue band (type 0x25) when anchored at the screen's
  edge, whose polygons reach past both 16:9 edges are stretched horizontally about their own origin by the widening
  times the share the stage allows, so they reach the frame edges with their ends off screen as at 16:9 while the text,
  drawn separately, is unchanged. Other message bands keep their 16:9 geometry. The town map's two models are scaled
  evenly about the canvas centre, map, markers and player face together, by the widening, capped so its highest marker
  stays on screen; at 32:9 the world beside it is clipped to the map's width.
- Horizontal bands: the town facility frame (the inn's top and bottom bars, lines, title plate and menus, one part in
  pass 0x26), the title frame and black matte bars of a fixed screen, the loading and saving fade's black backing and
  decorative line and, while a fixed screen's backdrop fills the frame, its bottom dialogue band are changed per polygon
  in the model data. A piece that reaches a 16:9 edge continues to the same place in the wider frame: bands and lines
  move only their outer corners, pieces taller than wide (end caps) move whole; everything clear of the edges (title
  plates, ornaments, menus, the fade's crest, text) and every y is left as it is. Each written polygon is remembered, so
  it is never extended twice whatever the animation's matrix, and a reloaded model is extended again.

## Texture filtering

Texture filtering 2x to 16x replaces the filter of the game's mipmapped bilinear samplers in graphics draws with anisotropic
filtering, unless the game already asks for at least as much; depth samplers and compute shaders keep theirs. Samplers are
cached by their final state and the device's `maxSamplerAnisotropy` and `samplerAnisotropy` feature limit what reaches
Vulkan.

## Removed

The video core came with God of War III's temporal upscaler (FSR 3.1, FSR 4, DLSS and TAA), its camera and object motion
vectors and their settings. Dragon's Crown Pro never ran them; they were removed, together with every inherited environment
switch. HDR and MSAA are not offered (docs/LAUNCHER.md).

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
