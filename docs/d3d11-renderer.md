# Direct3D 11 renderer

Experimental and Windows only. It is meant mainly for systems where Vulkan or OpenGL has problems. It has not been
measured against Vulkan or OpenGL, and which hardware it runs on is not yet known from testing.

## Architecture

There is no separate Direct3D renderer. The OpenGL backend in `video_core/renderer_opengl` runs against OpenGL ES 3.0,
and [ANGLE](https://chromium.googlesource.com/angle/angle) translates that to Direct3D 11:

```
Liverpool -> renderer_backend.h -> OpenGL::Presenter / Rasterizer -> OpenGL ES 3.0 -> ANGLE -> Direct3D 11
```

The IR-to-GLSL emitter has one translation of the IR and two targets, `Desktop330` and `Gles300`, which differ in spelling
and resource declarations only. `RequestAngleDirect3D11` (`gl_angle.cpp`) asks for ANGLE's D3D11 platform by name and turns
off the fallbacks that could land elsewhere, and the presenter checks `GL_RENDERER` after the context exists. A failure
ends the game with the reason; no other renderer takes over. Auto never picks Direct3D 11. Desktop OpenGL does not load
ANGLE.

| Desktop OpenGL 3.3 | OpenGL ES 3.0 path | Why |
|---|---|---|
| One 64 MiB buffer for vertices, indices and shader data | Client memory for vertices and indices; shader data in a 4096 x 4096 `R32UI` texture | ANGLE copies a whole buffer between its internal copies when part of it changes, and ES 3.0 has no buffer textures. `EXT_texture_buffer` needs ES 3.1, which upstream ANGLE no longer offers on D3D11 |
| Geometry shader for rect lists | The vertex stage runs the guest code for all three vertices, one instance per rectangle | ES 3.0 has no geometry stage |
| Geometry shader for quad lists | Index expansion on the CPU | The same |
| Unchecked guest indices | Indices are read only as far as guest memory is mapped. A draw whose indices would reach past the vertex stream is dropped and logged | Client memory is copied by ANGLE from what the draw names |
| `BGRA` and `BGR` uploads | Rearranged on the CPU before upload | ES uploads only red-first layouts |
| Base vertex | Attribute pointers move, and `gl_VertexID` gets the same offset from a uniform | Guest code reads the vertex index, and Direct3D's does not include a base vertex: this keeps it the same in every path |
| `GL_TEXTURE_SWIZZLE_RGBA` | One parameter per channel | ES has no combined parameter |

## Feature level

OpenGL ES 3.0 and per-target blending need Direct3D feature level **10_1** in ANGLE: 10_0 gets ES 2.0 and the game stops
with a message. BC6 and BC7 need 11_0, and below that those textures draw black and are logged. This comes from ANGLE's
capability tables and from compiling representative shaders for shader models 4.1 and 5.0, not from running on such
hardware. On the pinned revision ANGLE gives 11_0 ES 3.1, but this renderer does not use it, because upstream ANGLE has
since removed ES 3.1 from D3D11.

## ANGLE revision and runtime files

| | |
|---|---|
| Upstream | https://chromium.googlesource.com/angle/angle (mirror https://github.com/google/angle) |
| Revision | `890b5d8fa2988e3719e0d80421bf3e927db9cd5c` (2025-07-16) |
| Binary source | MSYS2 `mingw-w64-clang-x86_64-angleproject` `2.1.r25748.890b5d8f-3`, downloaded from `repo.msys2.org`. ANGLE publishes no official binaries |
| Package SHA-256 | `71c0daec0503735a15b441bd45cb55296715fd03b89e1ad3a7dc460eabbad56a` |
| `libEGL.dll` SHA-256 | `f8fb45125931b5d5f5b7fec5c9c9dc08cf43120b0197cdd04a22341bf27ad2f6` |
| `libGLESv2.dll` SHA-256 | `ba7d72a365a9f83d270a2a2c5dd5c902f5f1a81a769f7f160d7d949378257cfe` |

`gpu/angle-runtime.txt` pins all of this, and `.\tools\build.ps1` does the rest; it only builds, and the game is started by
hand. On the first build it downloads the
package to `cache\deps\angle\<version>`, checks its SHA-256, extracts only `libEGL.dll`, `libGLESv2.dll` and `LICENSE` with
CMake and checks each of them. After that it needs no network: files that match their hashes are used as they are, and a
mismatch stops the build. Nothing links ANGLE, because SDL loads it at run time, so no headers or import libraries are
involved and CMake does not look for it.

`build.ps1` and `tools\package.ps1` put the same verified files beside `dragons_crown_pro.exe`. `libEGL.dll` and
`libGLESv2.dll` also need `libc++.dll` and `zlib1.dll`. Those come from the compiler toolchain, since the game is built
against them and a process loads one copy of each, and they are staged beside the game too. Both scripts walk the imports
of what they staged and stop if a DLL is neither there nor part of Windows, so nothing depends on `PATH`. The renderer loads
ANGLE by full path from the game's folder, and the log names the two files. `d3dcompiler_47.dll` is part of Windows 10 and
11, which the game already requires, and is not shipped.

## Licences

ANGLE is BSD-3-Clause, compatible with this project's GPL-2.0-or-later. No ANGLE source is in this repository. The MSYS2
build also links Chromium base code, xxHash, SPIRV-Tools, Vulkan-Loader with cJSON, and Vulkan Memory Allocator into
`libGLESv2.dll`. `packaging/licenses/THIRD-PARTY-LICENSES.txt` lists them and the package carries their licence texts. A
release must keep those notices.

## Known limitations

Everything [GRAPHICS.md](GRAPHICS.md) lists for OpenGL applies. In addition:

- Indexed rectangle lists are skipped and logged, including the one that opens each frame: drawing it blacks out the
  picture on Direct3D 11, for a reason not yet known. Instanced rectangle lists draw their first instance, and a rectangle
  list reading more than five vertex attributes is skipped. All are logged.
- 1D images are refused and their draws skipped, logged once.
- Logic operations, sampler LOD bias and point polygon mode are ignored, logged once.
- Two-sided stencil uses the front face's masks and references, and constant colour with constant alpha blending uses the
  colour: Direct3D has one of each.
- Block compressed images whose base level is not a multiple of 4 are refused and draw black.
- The Graphics card setting does not apply: ANGLE uses the default adapter.
