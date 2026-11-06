# OpenGL renderer: origin and licensing

The OpenGL backend is experimental. It exists so that systems where Vulkan is missing or unreliable can still run the
game, and so that the video core is not tied to one graphics API. Vulkan remains the original primary renderer and the
default; OpenGL is chosen with the **Renderer** setting (launcher, or F1 in game; the choice applies at the next start).
How the backend works is in [GRAPHICS.md](GRAPHICS.md#opengl-renderer-experimental).

The OpenGL backend was implemented independently on top of the project's existing shadPS4-derived GPU architecture. The
Vulkan renderer (`gpu/shadps4/video_core/renderer_vulkan`) and shadPS4 define PS4 GPU behavior: GNM and GCN shader
semantics, textures, render targets, buffers, synchronization, presentation and resource handling.

## Sources

| Source | Used for |
|---|---|
| Dragon's Crown Pro Vulkan renderer | The primary implementation and semantic reference. The OpenGL classes follow its roles, and several are adapted from it (see below) |
| [shadPS4](https://github.com/shadps4-emu/shadPS4), commit `b4e7ae7` (GPL-2.0-or-later) | The PS4 runtime and GPU front end this project is built on: Liverpool, the shader recompiler, VideoOut, tiling |
| Historical Yuzu OpenGL renderer, tag `mainline-0-1734`, commit `537296095ab24eddcb196b5ef98004f91de9c8c2`, as referenced by the [Flathub packaging](https://github.com/flathub/org.yuzu_emu.yuzu) | Studied for generic, mature OpenGL backend design and performance ideas: buffer, texture and shader caches, state tracking, fences, queries, program management and stream buffers. No Yuzu source is included, and no GPU semantics come from it: PS4 behavior comes from shadPS4 and the Vulkan renderer only |

## Files

The `gl_*` files are in `gpu/shadps4/video_core/renderer_opengl`.

| Files | Origin |
|---|---|
| `gl_rasterizer.*`, `gl_shader_cache.*`, `gl_presenter.*` | Adapted from shadPS4's `vk_rasterizer`, `vk_pipeline_cache` and `vk_presenter`, with the Vulkan calls replaced; they keep the shadPS4 copyright notice |
| `gl_detile.*` | Port of shadPS4's `tiling.comp` to the CPU, because OpenGL 3.3 has no compute; keeps the shadPS4 notice |
| `shader_recompiler/backend/glsl/emit_glsl.*` | A GLSL 3.30 backend over the recompiler's IR, structured after shadPS4's SPIR-V backend; keeps the shadPS4 notice |
| `gl_state.h` (`GlState`), `gl_stream_ring.*`, `gl_guest_watch.*` (`GuestWriteWatch`), `gl_texture_cache.*`, `gl_format.*`, `gl_api.*`, `gl_capabilities.h` | Original to this project. State caching, a streaming buffer, write-protecting guest pages to detect changes and a format table are standard emulator and OpenGL practice; `GuestWriteWatch` plays the role shadPS4's `PageManager` plays for Vulkan |
| `renderer_backend.h`, `renderer_factory.cpp`, `gpu/shim/dcp_renderer.h` | Original to this project: the boundary between the GPU front end and a renderer, and the Renderer setting |

## Licensing

The project is GPL-2.0-or-later. Every OpenGL file carries `SPDX-License-Identifier: GPL-2.0-or-later`, and the files
adapted from shadPS4 also carry its `SPDX-FileCopyrightText`. Code that was only studied conceptually carries no
attribution because none of it was copied.
