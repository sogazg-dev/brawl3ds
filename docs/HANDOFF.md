# Brawl 3DS port — handoff context

Everything a new session needs to continue the work. State as of 2026-11-22, commit `b8fe233` on branch
`feature/phase0-arm11-bringup` (repo `C:\Users\SOgaz\.t3\projects\brawl3ds`).

## 1. Goal and approach

- **Goal:** run Super Smash Bros. Brawl (the user's own disc, RSBE01 **rev 2**) on a **New 3DS**. VS mode comes
  first; later come controls, stereo 3D and settings like the melee-3ds and smash64-3ds ports (including damage %
  on the bottom screen).
- **Approach:** retarget **Brawl Recompiled** (the WiiCompiled static recompiler: PowerPC to C++) to the
  **ARM11**, borrowing 3DS techniques from melee-3ds.
- **Renderer:** option A, chosen by the user. GX commands are translated to the **PICA200**. It reuses Aurora's
  GX front end and command processor (`wiicompiled/aurora-main`, MIT licence) with a 3DS back end of our own.
  The plan is in `docs/RENDERER_3DS.md`.
- **Test method:** the "headless" timing build. It boots to the title screen; an A press there skips the opening
  movie; the idle attract demo then plays a VS match (Bowser, Mario, Olimar, Zelda; 1,141 frames, 2673 to 3814).
  The build logs frame times, a profile and screenshots to the SD card. The bottom screen shows "Step N of 9"
  with an ETA.

## 2. Ground rules from the user

- Commit on `feature/phase0-arm11-bringup`.
- **No AI attribution anywhere**, by the user's request (2026-11-22):
  - Do not add `Co-Authored-By:` trailers or "Generated with ..." lines to commits or PRs.
  - Do not credit an AI assistant as a contributor in any file.
  - The user's instruction overrides any default attribution guidance.
  - Before the first push, every earlier commit message was rewritten to drop such trailers. A backup of the old
    history is `C:\Users\SOgaz\brawl3ds-work\brawl3ds-before-rewrite.bundle`.
- **GitHub:** https://github.com/sogazg-dev/brawl3ds (remote `origin`), with branches
  `feature/phase0-arm11-bringup` and `master`.
- **Never commit or distribute game data** (disc files, saves, blobs).
- Keep the user's email private. **Never screen-capture the desktop.**
- Azahar uses the **software renderer**, which the user set by hand. It is needed for screenshots read back by
  the CPU.
- The user tests on Azahar and on a real **New 3DS XL** and sends back `headless.log`, `profile.txt` and the
  `shot_*.bmp` files.
- The user installs the **CIA** (`release-headless\brawl3ds-headless.cia`), not the .3dsx. **Always repackage
  the CIA after building** (section 4).
- Priorities: optimisation and running on hardware come first. The exit error and the black screen after START
  were deprioritised by the user.
- The user wants long waits explained and steady visible progress.
- The red fighter silhouettes are deferred to milestone 6; the user said "no pressure".

## 3. Paths

| What | Where |
|---|---|
| Project repo (docs, tools, port sources) | `C:\Users\SOgaz\.t3\projects\brawl3ds` |
| Port sources | `port\wiicompiled_arm32\` (main_3ds.cpp, guest memory, fibers, profiler...) |
| 3DS renderer | `port\wiicompiled_arm32\aurora3ds\` |
| Headless build CMake | `port\headless\CMakeLists.txt` |
| Tools | `tools\` (package_headless.py, patch_brawlrecomp_rev2.py, patch_wiicompiled_arm32.py, hot_shards.py, embed_binary.py...) |
| Working tree | `C:\Users\SOgaz\brawl3ds-work\` |
| Patched Brawl Recompiled (PC build works) | `brawl3ds-work\BrawlRecompiled\` (Aurora: `wiicompiled\aurora-main\`) |
| Build directory | `brawl3ds-work\headless-build\` (Ninja) |
| Release output | `brawl3ds-work\release-headless\` (CIA, 3dsx, sd\ folder) |
| Extracted disc | `brawl3ds-work\Disc\DATA` |
| Toolchains | devkitPro at `C:\devkitPro` (citro3d, picasso); CMake/Ninja at `BrawlRecompiled\toolchain\`; makerom at `brawl3ds-work\tools\makerom` |
| Reference clones (read-only) | `C:\Users\SOgaz\brawl3ds-research\` (BrawlRecompiled, melee-3ds, smash64-3ds, MarioKartWiiDS, brawl-decomp) |
| Rev 2 symbols | `BrawlRecompiled\extras\brawl-decomp\config\RSBE01_02\symbols.txt` |
| Azahar | `C:\Users\SOgaz\Games\Emulators\Azahar` |

## 4. Build and package

```powershell
# build (PowerShell)
Push-Location C:\Users\SOgaz\brawl3ds-work\headless-build
& C:\Users\SOgaz\brawl3ds-work\BrawlRecompiled\toolchain\Ninja\ninja.exe brawl3ds.elf
Pop-Location
# package the CIA (+ 3dsx, SD folder)
python C:\Users\SOgaz\.t3\projects\brawl3ds\tools\package_headless.py --build C:/Users/SOgaz/brawl3ds-work/headless-build --wiicompiled C:/Users/SOgaz/brawl3ds-work/BrawlRecompiled/wiicompiled --blobs C:/Users/SOgaz/brawl3ds-work/sd/3ds/brawl3ds/blobs --out C:/Users/SOgaz/brawl3ds-work/release-headless
```

- `scripts\cycle.ps1` in brawl3ds-work runs a cycle in Azahar, including the save copy.
- Script edits: write the Python file with the Write tool, then run it. Bash heredocs break on apostrophes, and
  `\n` inside Python strings becomes a real newline in C `fprintf` strings.

## 5. Hardware facts (measured, do not re-measure)

- **Memory modes:** 178 MB SystemModeExt works on the user's New 3DS. libctru reserves 112 MB by default, so the
  heap sizes are set explicitly.
- **Memory budget (the binding limit):**
  - Regular heap 87 MiB.
  - Linear heap **42 MiB**. Guest RAM takes ~37 MiB of it, leaving ~3.5 MiB. At 44 MiB start-up fails.
  - Code size ~44.5 MB. Builds with ≥46 MB of code did not boot.
  - The renderer's linear use: vertices 78K × 24 B, indices 160K × 2, citro3d command buffer 768 KiB, texture
    staging 256 KiB. About 325 KiB of linear memory is left.
- **SD card:**
  - `stat()` costs ~50 ms per file. Never stat per file at runtime: `fast_tree_3ds.cpp` lists the folder once
    with FSDIR_Read.
  - Avoid `std::filesystem` canonicalisation (`fs::relative`).
  - SD `rename()` won't overwrite an existing file, so it is wrapped.
- **Save:** a fresh NAND without a save stalls in scBoot at the "create save data?" prompt. The packager copies
  the PC save (`--save`).
- **Azahar differs from the console:**
  - It does not fault on unmapped or null reads; the console data-aborts. Treat any "unmapped Read" in
    Azahar's log as a crash on hardware.
  - It is ~2× slower than the console, so its timings are not representative.
- **Alignment:** clang turns `u32*` casts of byte pointers into `ldm`, which faults on hardware. Use memcpy
  loads. `-munaligned-access` is fine.
- **Video memory:** VRAM (0x1F000000) cannot be read by the CPU.
- **Screenshots:** they must not stall the game. A 3 s stall was once followed by a 248 s hang in
  VIWaitForRetrace. Captures are now copied in memory and written at the end of the run.
- **Audio:** ndsp crashed on hardware, so the build uses SDL dummy audio for now. Revisit in the audio phase.
- **Exit:** the HOME Menu shows "An error has occurred" after the game has run. Deprioritised.

## 6. Performance history (match average per frame, console)

| Point | ms/frame |
|---|---|
| First hardware run (no renderer) | 223.7 |
| Inline guest memory fast path (page-bias tables) | 207 |
| Hot shards -O2 | 192.8 |
| Inline fma + fast FP | **179.9** (5.6 fps) |
| + Aurora front end, counting back end | 262.5 |
| + PICA renderer, untextured | 273 |
| + Textures/TEV (M3) | ~326 (3.1 fps) |

**Where the time goes (no-renderer profile):**
- ~63 ms nw4r g3d drawing.
- ~10 ms AX audio (reverb; now 4.3 ms).
- ~90 ms game logic and animation.

**Renderer costs:**
- GXCallDisplayList ~67 ms/frame.
- Brawl emits ~11,500 GX draws/frame (555 batches after state merging) and ~69–74K vertices.

**Ideas for the big gains (30 fps needs ~6×):**
- The GX side of the renderer on the second core (milestone 5).
- Native (high-level) versions of the hot nw4r g3d functions.
- Better translator code for game logic.

## 7. Renderer state (aurora3ds/)

### Files

| File | Role |
|---|---|
| `backend_3ds.cpp` | Aurora's back end; draws go to the PICA path |
| `pica_renderer_3ds.cpp` | frame, CPU vertex decode, uniforms, lighting setup, debug dump, captures, stats |
| `pica_tev_3ds.cpp` | GX TEV to PICA combiners (≤6 stages); alpha test, blend, depth, cull |
| `pica_texture_3ds.cpp` | VRAM texture cache (LRU, ~5 MiB), GX_TextureCopy uploads (≤8/frame), halving above 128 texels |
| `gx_texture_decode.cpp` | our GX texture decoders (all formats incl. CMPR) into a 1 MiB scratch buffer |
| `gx_m4.v.pica` | vertex shader: transform, texgen for 3 units, GX lighting |

### Key decisions

- **Vertex layout (24 B):** pos float3, rgba u8 (material color), s8 row + s8 normal×127 ×3, uv s16×2 at 1/512.
- **Top screen:**
  - It is set to BGR8, single-buffered, at renderer start. consoleInit had left it RGB565, which showed the frame
    as smeared copies.
  - The projection folds the screen's quarter turn into the rows (row1, −row0). Writing `outpos` component-wise
    in the shader draws nothing.
- **Depth:** `C3D_DepthMap(true, 1, 1)` gives depth = z/w + 1, so GX's compare functions apply unchanged. The
  clear value is 0x00FFFFFF.
- **Culling:** GX_CULL_BACK maps to GPU_CULL_FRONT_CCW.
- **Textures:** rows are packed bottom-up.
- **Command buffer:** only changed state goes to citro3d (uniforms, texenv, fragment state, texture binds).
  Re-sending everything overflowed the command buffer (PANIC).
- **Frame start:** `C3D_FrameBegin(C3D_FRAME_NONBLOCK)` polling. SYNCDRAW froze Azahar. The frame is dropped
  after 2 s.
- **Diagnostics:**
  - Debug dump of suspect batches.
  - A STALL line from the heartbeat every 30 s when frames stop, with guest lr/sp and the renderer phase.
  - Captures at frames 700, 2000, 2873, 3373 and 3374.

### Milestones

1. ✅ Front end + counting back end.
2. ✅ Untextured geometry.
3. ✅ Textures, TEV, blend, depth, cull. The HUD, percentages, portraits and stage background show correctly on
   the console and in Azahar.
4. ⏳ **Lighting, fog, more TEV.** First pass committed in `b8fe233` (**not yet tested**):
   - GX per-vertex lighting of COLOR0A0 in the shader: ambient + up to 8 lights; spot/spec/none attenuation;
     none/sign/clamp diffuse.
   - The normal goes through the position matrix and is normalized.
   - Far lights are scaled to 1e6 because 24-bit floats overflow.
   - A `lighting:` line in the match summary counts lit batches, lights each, approximations and COLOR1A1 users.
   - Still to do in this milestone:
     - COLOR1A1 (drawn as COLOR0A0 now; the PICA has one vertex color, so fragment lighting may be needed).
     - Fog.
     - A second UV set.
     - Materials with more than 6 combiner stages or more than 3 textures.
5. The GX side on the second core.
6. EFB copies (GXCopyTex render-to-texture, and its clear) and the rest.
   - **The red fighter silhouettes** come from Brawl's shadow pass. It draws the fighters in one color, then an
     EFB copy turns them into a texture and clears that area. EFB copies are stubbed, so the mask stays on
     screen and there are no shadows.
   - A quick interim fix: honor the copy's clear.

## 8. Open items and next steps

1. **The user is testing `b8fe233`** (new CIA built 2026-11-22 22:08). Check:
   - Shading on the fighters and stage: not too dark or blown out.
   - The `lighting:` line in headless.log.
   - The frame time against ~326 ms.
   - The screenshots.
2. Then fog, COLOR1A1 handling (according to the stats), the second UV set and larger TEV setups.
3. A post-match STALL: at frame 3814 (scMemoryChange) the guest sat at lr 80023BB4 for 100+ s. The run finished.
   Investigate later.
4. Milestone 5 (second core) is the biggest performance lever on the renderer side.
5. Deprioritised:
   - The exit error.
   - The black screen when START is pressed during steps 2–4.
   - The scBoot SD/save stall seen in early runs (fixed by disabling the shadow copy).
