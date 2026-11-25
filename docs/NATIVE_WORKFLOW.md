# Running Brawl's code natively: what is possible and the workflow

Written 2026-11-24. Status: a plan plus the first working pieces; nothing here is a finished system.

## Where the time goes today

Translated code is PowerPC semantics expressed in C++ and compiled for the ARM11: every guest register lives in
`CpuContext`, every memory access swaps bytes, every call spills and reloads the registers in use, every
paired-single instruction is emulated lane by lane. Measured on the 4-player demo (emulator profile, 900 samples of
the game thread): ~46% of the time is inside translated functions and ~54% in runtime helpers they call
(indirect-call dispatch, the GX display-list path, FIFO writes, memory helpers). The profile is flat: no function
is worth more than ~3%.

So there are two different costs, and "native" attacks only the first:

1. **Per-instruction overhead of translation** (~2-5x a hand-compiled function). Fixed by native functions.
2. **The work itself** (Brawl's logic runs at 60 Hz on a 729 MHz PowerPC; the New 3DS's ARM11 is 804 MHz with a
   weaker FPU and caches). Even perfectly native code is not automatically full speed.

## Can the whole game run without translation?

Not in any near term, and the reason is data, not code:

- The game's data is big-endian and its pointers are 32-bit Wii addresses: every asset file (BRRES models,
  animations, parameter files, the relocatable modules) and every structure the game builds in memory. A true
  native build needs either all of that converted at load time (each of dozens of formats understood completely)
  or source written against big-endian accessor types everywhere.
- It needs the source. `BrawlRecompiled/extras/brawl-decomp` has 242 source files and a symbol map for RSBE01
  rev 2; the game has ~120,000 functions. nw4r (the library the hot rendering and animation code is in) is not in
  that tree, though other public decompilation projects cover parts of it.

What is realistic is the **hybrid**: the game keeps running translated, and functions are replaced one at a time
by native ARM code that works on the game's own memory. That is what `port/wiicompiled_arm32/natives_3ds.cpp`
already does for `PSMTXConcat`, `PSMTXConcatArray` and `__fill_mem`. Every replaced function is a real,
measurable gain, the game stays whole at every step, and the set can grow for years.

## The workflow (PC only until the last step)

### 1. Choose the function from a profile, not by guess

- `brawl3ds-work/scripts/gdb_sample.py` samples the game in the Azahar emulator (needs `use_gdbstub=true`; start
  sampling on a log line inside the match, e.g. `"frame   2792  scene scMelee"`). It gives native-level shares:
  which translated function, which runtime helper.
- `profile.txt` (the build's own sampler) gives guest-level self and inclusive time.
- Candidates worth it: leaf or near-leaf functions with heavy arithmetic (matrix, vector, quaternion, animation
  evaluation, collision tests). Functions that mostly call other functions gain little.

### 2. Get the function's meaning

- Name: `BrawlRecompiled/extras/brawl-decomp/config/RSBE01_02/symbols.txt` (address -> symbol, with sizes).
- Source, in order of preference: the decomp tree; another project's decompilation of the same library (nw4r,
  the Revolution SDK); the translated C++ itself (`generated/build_shards/base_common.orig`, one block per
  function: mechanical but exact, and good enough to read the arithmetic and its order).

### 3. Write it native

- One function per guest address in `natives_3ds.cpp`:
  `extern "C" void Brawl3dsNative_<ADDRESS>(CpuContext* ctx)`. Arguments are in `ctx->gpr[3..10]` and
  `ctx->fpr[1..8]`, results go to `ctx->gpr[3]` / `ctx->fpr[1]`. Volatile registers need not be reproduced.
- Guest memory through `MemoryInline::Brawl3dsLoadInline/StoreInline` (they swap bytes and resolve the page), or
  one page lookup for a whole structure (see `LoadMtx`).
- Keep the original's arithmetic order. A PowerPC `fmadds` rounds once; `(float)((double)a * b + c)` matches it
  except on exact ties.
- `python -I tools/apply_natives.py port/wiicompiled_arm32/natives_3ds.cpp <shards dir>` turns the translated
  body into a call to the native; every caller then reaches it.

### 4. Prove it on the PC

All of this runs in the emulator on this PC; none of it needs the console.

- **Bit-identical picture.** The demo match is deterministic (`NSMBW_DETERMINISTIC`). The headless CIA run with
  `azcycle.ps1 -StopAfter 3100` writes `shot_2873.bmp` inside the match; it must be byte-identical to
  `brawl3ds-work/az-results/ref-match/`. Every renderer change so far was checked this way.
- **Bit-identical game state.** The runtime can hash guest memory every N frames (`NSMBW_DET_HASH=<file>:<every>`,
  `det_hash.cpp`) and dump it at chosen frames (`NSMBW_DET_DUMP`). Run the demo with and without the new native
  and compare the hash files: the first differing frame is where the native diverged, and the two dumps of that
  frame, compared, give the addresses it wrote differently. Not wired into the 3DS build's launcher yet: the
  environment variables have to be set in `main_3ds.cpp` behind a debug flag (a small job, the next step here).
- **Same worker load.** `headless.log` prints the renderer worker's busy time; a native that changes the game's
  behaviour changes the draws, and that number moves. It stayed at 180.8 ms through every game-thread change.
- **Speed.** The same run prints `average game` (the game thread's time per frame).

### 5. Only then the console

A build whose picture, state hashes and full play loop (`play_endmatch.txt`: menus -> match -> results -> menus)
pass in the emulator goes to the console once, for the things the emulator cannot show: real timing, memory
limits, sound.

## What to build next for this workflow

1. `debug_flags` switches for `NSMBW_DET_HASH` / `NSMBW_DET_DUMP`, and a script that runs the demo twice and
   reports the first differing frame and addresses. This makes step 4 one command.
2. A header of guest types (`be<T>`, `gptr<T>`, a `GuestCall` helper) so decompiled C++ can be compiled almost as
   written instead of being re-typed against loads and stores.
3. Bring in a decompilation of nw4r g3d/math (licence permitting) as reference for the animation and matrix
   functions that lead the profile.
4. The PC build of the recompiler (`BrawlRecompiled/tools/build-brawl.ps1`) for faster iteration than the
   emulator: the same determinism checks run there in seconds. It was last built on another machine.

## Related: code packs

Fighter and stage code that does not fit in the executable is loaded from the SD card on demand
(`tools/build_packs.py`, `port/wiicompiled_arm32/code_packs_3ds.cpp`). That removes the interpreter from those
modules; it does not make translated code faster. The two efforts are independent.
