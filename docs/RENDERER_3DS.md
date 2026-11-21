# 3DS renderer plan (option A: GX commands to the PICA200)

Decided 2026-11-21. The game keeps drawing through its own nw4r g3d code (translated), the runtime keeps parsing the
GX FIFO (runtime/src/hle/gx), and a 3DS renderer turns the resulting GX state and draws into PICA200 work through
citro3d. Later, the GX side can run on a second New 3DS core.

## Where the renderer plugs in

On PC the runtime parses the game's FIFO itself and hands Aurora (wiicompiled/aurora-main, MIT, Luke Street):

- the Dolphin-style GX API (145 functions the runtime calls: `GXSetTevOrder`, `GXLoadPosMtxImm`, `GXLoadTexObj`, ...),
  which Aurora's `lib/dolphin/gx/*.cpp` record into `g_gxState` (`lib/gx/gx.hpp`);
- raw display lists through `GXCallDisplayList` and packed draws through `aurora::gx::fifo::submit_raw_draw`, which
  Aurora's `lib/gx/command_processor.cpp` decodes (BP/CP/XF register writes and draw packets).

The headless build replaces all of that with no-ops (`port/wiicompiled_arm32/aurora_headless_3ds.cpp` and the
generated `link_stubs.cpp`).

## What is reusable from Aurora, and what is not

| Aurora part | WebGPU ties | Plan |
|---|---|---|
| `lib/dolphin/gx/*.cpp` (GX API → state) | 2 refs, one file | reuse |
| `lib/gx/command_processor.cpp` (register + packet decode) | none directly; draws go through `gfx::push_*` | reuse up to the draw |
| `lib/gx/shader_info.cpp` (what a TEV/channel config uses) | none | reuse |
| `lib/gx/gx.hpp`, `lib/gfx/common.hpp` | 6 + 19 refs | 3DS replacement for the gfx layer types |
| `lib/gx/gx.cpp`, `shader.cpp`, `pipeline.cpp`, `lib/webgpu/*` | the WebGPU back end | replaced by the PICA back end |
| `lib/gfx/texture_convert.cpp` | 5 refs | reference for the Wii texture formats |

Aurora uploads the game's vertex arrays as storage buffers and decodes them in the shader. The PICA200 has no
storage buffers, so the 3DS back end decodes each draw's vertices on the CPU into a fixed PICA layout.

## PICA200 mapping

- Vertices: position, normal, color0/1, up to 8 texcoords and the per-vertex matrix index (PNMTXIDX, skinning)
  decoded on the CPU; the vertex shader picks the position/normal matrix through the address register.
- Transform and lighting: GX position/normal/texture matrices and projection as vertex-shader uniforms; GX's
  per-vertex lighting (`GXSetChanCtrl`, up to 8 lights) in the vertex shader, simplified at first.
- TEV: up to 16 GX stages onto the PICA's 6 fixed combiner stages; exact where it fits, approximations otherwise.
- Textures: Wii formats (I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8/CMPR/CI4/CI8) converted to PICA formats in a cache keyed by
  address and size; VRAM is 6 MB.
- Framebuffer: 400x240 top screen; EFB copies (`GXCopyTex`) later.

## Memory

Code is 44.2 MB and the regular heap is 92 MiB; the linear heap (39 MiB) holds MEM1, the module homes and a 1 MiB
reserve. The renderer needs linear memory for its command buffer and per-frame vertices: the reserve goes back up
and the regular heap gives the room (the 4.7 MB per-link module copy is gone since 2a1294c).

## Milestones

1. Aurora's GX front end and command processor build for the 3DS with a draw sink that only counts draws and
   vertices. Measures the parse cost on hardware.
2. Untextured geometry on the top screen (vertex colors), checked in Azahar, which renders PICA.
3. Textures and the common TEV setups, alpha test/blend/depth, culling.
4. Lighting, fog, more TEV.
5. The GX side on a second core.
6. EFB copies and the rest.
