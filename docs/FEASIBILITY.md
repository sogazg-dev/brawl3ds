# Brawl3DS: feasibility study

Date: 2026-11-19. Reference repos cloned (read-only) to `C:\Users\SOgaz\brawl3ds-research\`.

## Verdict

**Possible, New 3DS only, and the hard parts are CPU time and memory, not graphics.**
A VS mode that boots and plays is realistic. A locked 60 FPS with four fighters is
unlikely without a lot of optimization. A realistic first target is 1v1 on light
stages, with game logic at 60 Hz and drawing at 30 or 60 (Melee's AUTO mode).
The Old 3DS is out of reach: 64–96 MB of RAM against Brawl's 88 MB of Wii RAM alone.

The plan is to **retarget Brawl Recompiled's PowerPC→C++ output to the 3DS's ARM11**,
borrowing melee-3ds's 3DS-specific techniques (big-endian ARM execution, a GX→PICA200
renderer on a second core, the bottom-screen dashboard, settings). We can't follow
the Melee or Smash 64 route because there is no Brawl source code: the Brawl
decompilation was about 1% done in April 2026. We can't follow the MKWiiDS route
either: rewriting Brawl's engine by hand is years of work.

## What each reference project does

| | Smash 64 3DS | Melee 3DS | Mario Kart WiiDS | Brawl Recompiled (PC) |
|---|---|---|---|---|
| Where the game code comes from | Full decomp (C source) | Full decomp (C source) | **Rewritten** natively (Kinoko physics reimplementation, ~45k lines of C++) | **Static recompilation**: PPC machine code → C++ (WiiCompiled) |
| Graphics | N64 RDP → citro3d (sm64-port 3DS backend) | GX calls recorded → translated to citro3d | Assets **pre-converted offline** (BRRES→own format, Python/TS cookers) | GX HLE → Aurora → WebGPU/D3D12 |
| Endianness | Converted assets | **ARM BE8**: engine built for `armeb-none-eabi`, runs under `setend be`, so GC data is read as-is | Converted offline | Byte-swap on every guest access |
| Threads/cores | Renderer on core 2 | GX translator + render worker on core 2 | Render thread + job worker | Frame worker thread |
| Memory mode | 124 MB, 804 MHz, L2 on | 124 MB, 804 MHz, L2 on | 80 MB (runs on Old 3DS too) | 4 GiB flat mapping (64-bit host) |
| Notable tricks | IO worker thread, perf reports on SD, tap-jump/C-stick options | Texture LRU cache in VRAM, command cache, stereo via vertex shader (8 PICA shader variants), Diet stage swaps, wrapped `linearAlloc` with a lock, AUTO 60/30 | 800px wide mode, quality settings (textures, shadows, bloom, opponent detail) | Function-level HLE (`native_registration_root`), PPC interpreter for patched code |

Settings to copy (all three 3DS ports have similar ones):
- FPS mode: AUTO (60, falling back to an even 30) / 30 / 60 (Melee).
- View: 4:3, or widescreen showing more of the scene without stretching (Smash 64, Melee).
- Resolution: 400x240, or 800px wide mode (MKWiiDS), which can be turned off for speed.
- 3D on/off. Stereo is generated in the vertex shader from one set of draws (Melee's `vertex-stereo*.v.pica`), so the 3D effect does not double CPU cost.
- Bottom screen: portraits, damage %, stocks, an FPS display, a CONTROLS/VIEW/FPS touch panel. Smash 64 reads the game's battle-state structs. We would read the same values from known addresses in guest RAM.

## Facts about Brawl Recompiled that decide the approach

- The game is `main.dol` (~4 MB of code) plus **126 `.rel` modules, 16.5 MB** in total. `sora_melee` (the battle engine) is 4.7 MB, and the 36 fighter modules are 6.1 MB.
  On PC everything is linked into a single exe and stays resident. On the 3DS that is the biggest memory risk.
- Memory access already has a **page-table path** (`runtime/include/memory_access.h`, 1 MiB pages)
  besides the 4 GiB flat mapping (`guest_flat_memory.h`, which `#error`s on unknown architectures).
  A 32-bit host has to use the paged path or a new compact one.
- The runtime already runs on **ARM64** (Linux, macOS, an Android app), so the C++ isn't x86-only.
  32-bit ARM (ARMv6K, no NEON, VFPv2) has not been tried.
- GX is emulated at the **function level** (`runtime/src/hle/gx/*`), and the result goes to Aurora. Aurora/WebGPU
  can't run on a 3DS. Its consumer has to be replaced by a citro3d backend, like Melee's `renderer.c`.
- Paired singles and double-precision FPRs are emulated in C++ (`isa/ppc_isa_*.h`). The ARM11's VFPv2 runs
  doubles slowly, so the hot paths should use single precision wherever the game only needs it.
- License: WiiCompiled is GPLv3, and melee-3ds's 3DS code is GPLv3. Both are compatible with a GPLv3 port.
  As with every reference project, we can ship **no game code or data**: the build runs on the user's own disc.

## Budgets (New 3DS)

| Resource | Available | Brawl needs | Notes |
|---|---|---|---|
| App RAM | 124 MB (178 MB "dev" mode, untested on retail) | 24 MB MEM1 + 64 MB MEM2 + recompiled code + GPU buffers | **Over budget** unless code loads per module and heaps shrink |
| Recompiled ARM code | n/a | Estimated 2–4x the ~16 MB of PPC code, so **30–60 MB** | Measure in Phase 0. ARMv6K has no Thumb-2 for denser code |
| CPU | ARM11 804 MHz in-order, 2 usable cores (+ part of a 3rd) | Wii Broadway 729 MHz; recompiled code is slower than compiled source | Melee from source: 60 FPS 1v1, 30 FPS with 4 players. Brawl is heavier |
| VRAM / GPU | 6 MB VRAM, PICA200 with 6 TEV stages | Wii GX allows 16 TEV stages | Approximate the materials like Melee, and use a texture LRU |
| Pixels | 400x240 (800x240 wide) | 640x480 | About 3x fewer pixels, which helps |

Ways to fit memory:
1. **Load module code on demand**, as the Wii loads `.rel` files: compile each module into its own
   code blob, keep it on the SD card, and map it executable when the game loads that module (Luma's
   `svcControlProcessMemory`, the same mechanism 3DS JIT emulators use). Only the fighters in the match
   plus the stage would be resident.
2. **VS-only build**: skip translating Subspace (`sora_adv_*`, `sora_enemy`, ~1+ MB of PPC code) and other unused modes.
3. Shrink Brawl's fixed heaps (gfHeapManager) where the VS mode doesn't fill them.
4. Avoid keeping two copies of textures (guest RAM + linear memory). Melee's texture cache handles this.

## Phase 0 results (measured 2026-11-19)

**Disc.** The user's disc is RSBE01 **revision 2**. Brawl Recompiled only accepts rev 1, but rev 2's `main.dol`
and all 126 modules match the Brawl decompilation's `RSBE01_02` `build.sha1` exactly. The data-section SDA bases
equal `recomp.yml`'s, and `build_modules.py` produces the same module layout. `tools/patch_brawlrecomp_rev2.py`
makes the build and runtime accept it. The image stores its DATA partition already decrypted; the extraction
wrapper reads it directly (`brawl3ds-work/scripts/extract_iso.py`).

**PC build.** Brawl Recompiled builds from this disc in about 12 minutes (Ryzen 5 7600X, 31 GB RAM). The engine
DLL holds **126 MB of x86-64 code**, almost all translated game code.

**ARM11 build.** With `tools/patch_wiicompiled_arm32.py` (a scalar NEON stand-in, FPSCR, page-table-only memory),
**all 600 translated shards compile** for `arm-none-eabi -mcpu=mpcore -mfpu=vfp -mfloat-abi=hard` with clang
and devkitARM's headers. Exact per-function sizes (`-ffunction-sections`, llvm-nm):

| Part | ARM code at -O2 |
|---|---|
| main.dol | 68.0 MB |
| sora_melee (battle engine) | 67.9 MB |
| 36 fighters | 68.3 MB (largest 3.4 MB) |
| 49 stages | 28.2 MB |
| Subspace, enemies, minigames | 14.5 MB |
| Menus | 13.8 MB + 6.4 MB boot-resident |
| **Total** | **264.8 MB** (2.1x the x86-64 objects) |

- `-Os` saves almost nothing (-1.4%). **`-Oz` cuts code to 35.7%** on a 10-shard sample, so the bulk is ordinary
  inlined helper code. Speed at -Oz is still unmeasured.
- **Resident for a VS match** (boot-resident set + 2 biggest fighters + biggest stage): **152 MB at -O2,
  about 54 MB at -Oz** (56 MB with 4 fighters).
- Memory budget at -Oz: 124 MB − 54 MB code − 88 MB Wii RAM = **−18 MB**. It fits only with one or more of these:
  the 178 MB mode (to test on hardware), allocating guest RAM only as it's touched, trimming Brawl's heaps,
  or dropping main.dol code VS never runs. Mixed optimization (`-O2` for hot functions chosen by profiling,
  `-Oz` elsewhere) is the obvious trade-off.
- Load modules on demand (as the Wii does) is now **required**, not optional: fighters, stages and
  non-VS modes are 125 MB of the 265 MB.

**Getting into a match without input.** Left idle on the title screen, Brawl starts an attract-demo CPU match:
with `NSMBW_DETERMINISTIC=1` the PC build reaches `scMelee` at frame 9341 (scenes: scStrap → scBoot → scTitle →
scMelee), with no input script at all. The headless 3DS test uses this to time real match frames. For
user-chosen matches later, `input_script.txt` drives the GameCube pads per scene and frame
(`scene@<frames>f:BUTTONS:<duration>:<port>`, PLUS = START, MINUS = Z). Save states can't be used: they store
host stacks and addresses, so they only last one session on one machine.

**Memory probe.** `probes/memprobe` builds two CIAs (124 MB and 178 MB `SystemModeExt`) that report the memory
region, usable heap and linear memory, clock/L2, and float costs (float vs double madd, software `fmaf`).
Results go to `sdmc:/3ds/brawl3ds/probe-<mode>.txt`.

Hardware results (user's New 3DS, Luma, 2026-11-19):

| | 124 MB CIA | 178 MB CIA |
|---|---|---|
| APPLICATION region | 124 MB | **178 MB** |
| Reserved by libctru's default heap at start | 112 MB | 112 MB |
| Still free after that | 11 MB | **65 MB** |
| CPU | 804 MHz, L2 on | 804 MHz, L2 on |

- **The 178 MB mode works on retail hardware.** It takes a moment longer to launch, but no errors. libctru's
  default heap doesn't grow with the mode, so the port must set `__ctru_heap_size` / `__ctru_linear_heap_size`
  itself to reach about 170 MB. Budget: 54 MB VS code (-Oz) + 88 MB Wii RAM leaves about 30 MB for the runtime
  and GPU buffers, without on-demand module loading.
- Floats (latency-bound loops): float madd 16.2 ns, double madd 17.4 ns, software `fmaf` 37.3 ns (2.3x),
  double divide 51 ns. Doubles cost about the same as floats for add/multiply; fused madd in software is
  affordable but worth avoiding in hot paths.

**Runtime on ARM11.** With SDL3 3.4.4 built for the 3DS (its N3DS audio/joystick/thread/timer backends, via
`cmake/3ds-toolchain.cmake`), `-std=gnu++20 -DTARGET_PC` and the ARM32 patch, **94 of the 110 runtime sources
compile**. The 16 left are platform spots: fibers (`host_context.cpp`), guest memory reservation
(`guest_flat_memory.cpp`), FatFs include path, CryptoPP's OS RNG (4 NAND files), `POLLRDNORM` (5 network files),
the POSIX fault handler in `main.cpp`, and PC-only files (Discord, netplay socket, Windows loader).

## Plan

**Phase 0: measure before committing (about 1–2 weeks).** Everything else depends on these numbers.
1. Build Brawl Recompiled on PC from your own RSBE01 disc, and confirm a VS match works.
2. Cross-compile the generated C++ for `armv6k` (once little-endian, once `armeb`/BE8), and record `.text` size
   per module. That answers the memory question.
3. Time one VS frame's logic. Either run the recompiled code on a slow ARM32 board/QEMU with
   rendering disabled, or go straight to step 4 of Phase 1.

**Phase 1: headless boot on the 3DS.** Port the runtime: paged memory, libco fibers on ARM32, DVD→SD file
reads, NAND/save to SD, OS timing, no GX output. Goal: reach a VS match in memory (scripted inputs) and
print ms/frame on real hardware. **Go/no-go point.**

**Phase 2: graphics.** A citro3d backend for the GX HLE, on core 2 (Melee's record/replay design):
texture decode and LRU, TEV→6-stage approximation, EFB copies, the stereo vertex shader, frame-rate modes.

**Phase 3: playable VS.** Circle Pad/buttons→GameCube pad mapping (C-stick on the New 3DS), AX audio→`ndsp`,
a bottom-screen HUD (portraits, damage %, stocks, FPS), a settings page (FPS mode, 4:3/wide, 800px, 3D),
and native (hand-written) versions of the hottest recompiled functions, chosen by profiling.

**Phase 4: packaging.** A one-click Windows builder from the user's disc, `.3dsx` + CIA, and the SD layout.

## What you need

- **Hardware:** a New 3DS / New 3DS XL / New 2DS XL with Luma3DS + FBI, `dspfirm.cdc` dumped, and a fast SD card.
  The **real console is required** for performance numbers: Azahar results don't show console speed.
- **Game:** your own **Super Smash Bros. Brawl USA (RSBE01)** disc image (.iso/.wbfs). Brawl Recompiled checks the revision.
- **PC:** Windows 10/11, **16 GB RAM** (the translation needs it), ~30 GB free disk.
- **Toolchain:** Python 3.11+, Git, .NET SDK (the translator is C#), CMake + clang/LLVM (for the
  `armeb-none-eabi` target), **devkitPro `3ds-dev`** (devkitARM, libctru, citro3d, picasso, 3dsxtool),
  makerom/bannertool for CIAs, and the Azahar emulator for fast functional testing.
- **Skills that matter most:** C/C++, ARM assembly/performance, GPU fixed-function pipelines (TEV/PICA200), and patience with profiling.
