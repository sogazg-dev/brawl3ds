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
4. Lighting, fog, more TEV. Lighting first pass: GX per-vertex lighting of channel COLOR0A0 in the vertex
   shader (`gx_m4.v.pica`): normals packed into the 24-byte vertex (x127, beside the matrix row), up to 8 lights
   as uniforms, the GX spot/specular/no attenuation and none/sign/clamp diffuse models. The normal uses the
   position matrix, normalized. Then fog (per vertex, an alpha ramp on unit 2 and a last combiner stage), a second
   texture-coordinate set, coordinates from normals, the viewport, EFB copy clears, GX's dual texture transform and
   stretched (not padded) textures. Not yet: COLOR1A1 (drawn as COLOR0A0).
5. The GX side on a second core (design below).
6. EFB copies and the rest.

## Milestone 5: the GX side on core 2 (design, 2026-11-23)

Measured on the console with milestone 4: a match frame is 365.6 ms, of which 138 ms is spent inside
GXCallDisplayList (Aurora's command processor plus this renderer's back end, run inline on the game thread).

**Core 2 is available** to the application on the New 3DS with the exheader as it is (`AffinityMask: 1`,
`IdealProcessor: 0`): melee-3ds (`port/3ds/gx_thread.c`) and smash64-3ds (`3ds/src/render_thread.c`) create their
render threads with `threadCreate(..., 2, ...)`. Patterns worth taking from melee-3ds: a light event per side, the
worker spinning briefly before sleeping (only on a core of its own), an APT hook that drains recorded frames before
citro3d clears the GPU queue on HOME (`APTHOOK_ONSUSPEND`), and failures reported back to the game thread.

**Where to cut** (to be decided with a console profile.txt):
- *Front end* (as melee-3ds): the game thread only records, the worker runs Aurora and the back end; moves all
  138 ms. In Brawl the interface is wide: the gather-pipe stream (HleFifoWrite / GX_HLE_FIFO_WriteBurst), display
  lists (GXCallDisplayList), and about 150 HLE GX SDK entry points in runtime/src/hle/gx (gx_texture, gx_vertex,
  gx_transform, gx_tev, gx_pixel, gx_copy, ...) that call Aurora directly, many with guest pointers whose data must
  be copied when recorded (matrices, texture objects). Get* functions and sync points (GXDrawDone, draw sync tokens,
  bounding box, peeks) need the worker drained or a shadow state. Display-list recording (GXBegin/EndDisplayList)
  stays on the game thread (it writes guest memory and returns a size).
- *Back end*: Aurora stays on the game thread; push_draw_command / append / the copy clear / frame end are queued
  to the worker, which runs ApplyMaterial, texture binding, vertex decoding and citro3d. The back end reads ~35
  GXState fields (matrices, lights, TEV, texgens, vertex formats and arrays, textures, fog, viewport, blend), so each
  full draw (~555 a frame) carries a snapshot (~4-5 KB, ~2.5 MB a frame) unless only changed groups are copied.
  Vertex data stays valid: the staging arena is reset at frame end (after the worker drained) and guest arrays do
  not change within a frame. citro3d is then used only on the worker (captures and stats included).

Either way the frame end waits for the worker (GXDrawDone semantics) at first; overlapping the next frame's game
logic needs the staging arena double-buffered and is a later step.
