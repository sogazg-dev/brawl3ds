# Headless timing test (New 3DS)

A build of Brawl with **no graphics or sound output**. It boots the real game code, sits on the title
screen until Brawl starts its CPU demo match (Bowser, Olimar, Zelda and Mario on Smashville), measures
the whole match (~1,141 frames, about 19 s at 60 fps), and writes how long every frame took. The top screen shows instructions; the
bottom screen shows which step is running.

**This is a diagnostic tool, not the game.** Most of its running time is Brawl's own idle timers, counted in
frames: the safety screen (~1,435 frames, about 24 s at 60 fps) and the title screen before the attract demo.
A player pressing A skips both. Nothing is compiled or converted on the console. The code is already ARM
machine code, and the game files are read as they are.

`input_script.txt` (written by `package_headless.py`) holds A from 60 frames into the title screen (for 120 pad polls). That
skips Brawl's opening movie, which the game decodes in software (about 1 s per video frame in Azahar), and the
demo match starts after ~1,186 title frames instead of ~7,855. The fighters are the same: Bowser,
Mario, Olimar and Zelda. The safety screen is left to time out, because skipping it too changes which demo match
plays, and this build only holds the code that the PC coverage run of this match used.

## Steps (bottom screen: "Step N of 9")

| # | Step | What it is | Typical length |
|---|------|-----------|----------------|
| 1 | Checking game files | `game/` and the save are present | instant |
| 2 | Indexing the game folder | walks the 5,314 disc files on the SD | ~3 s |
| 3 | Reserving Wii memory | MEM1/MEM2/module homes | instant |
| 4 | Starting the runtime | game code, modules, virtual NAND | ~30 s (Azahar) |
| 5 | Safety screen (scStrap) | Brawl's idle timeout | 1,435 frames |
| 6 | Loading boot data + save | scBoot | 44 frames |
| 7 | Title screen idle (scTitle) | A skips the movie; waits for the attract demo, then loads it | ~1,186 frames |
| 8 | Measuring the VS demo match | scMelee, the whole match | ~1,141 frames |
| 9 | Writing results | headless.log | instant |

The screen shows the frame count against the PC build's count for that step, time in the step, total time,
an estimate of time left at the current speed, the fps, and memory. If a step runs far past its PC frame
count (2× + 600 frames), the build stops with "Stalled" instead of waiting forever. `headless.log` ends
with a time-per-step table.

Steps 1–4 have no game frames, so a heartbeat redraws the screen every second during them. It shows the size
of the runtime's log, how long ago it last printed something, and its last line. The same information goes
into `headless.log` every 15 s. If 10 minutes pass with no new output, the screen says the run is probably
stuck. START closes the test during these steps too.

Before this fix, step 4 walked all 5,314 disc files twice with `fs::relative`. That call checks every folder in
a file's path on the SD card, one at a time. A real New 3DS spent more than 20 minutes in step 4 because of it.
The patch replaces it with a string-only `lexically_relative` (see `tools/patch_wiicompiled_arm32.py`).

## What goes on the SD card

```
SD:/3ds/dspfirm.cdc                    (your console's DSP dump: Luma3DS > Rosalina > Miscellaneous)
SD:/3ds/brawl3ds/
    game/            the extracted disc: the sys and files folders (about 7.2 GB)
    blobs/           from release-headless/sd/3ds/brawl3ds
    wii_bootstrap/   from release-headless/sd/3ds/brawl3ds
    dsp_coef.bin     from release-headless/sd/3ds/brawl3ds
    portable.txt     from release-headless/sd/3ds/brawl3ds
    input_script.txt from release-headless/sd/3ds/brawl3ds (the title-screen A press)
    UserData/NAND/title/00010000/52534245/data/   your Brawl save (from release-headless, see below)
SD:/cias/brawl3ds-headless.cia
```

- **The save is required.** On a first boot Brawl asks "create save data?" and waits for a button. This
  build has no input, so it would wait forever. `package_headless.py --save` copies the PC build's save
  (`BrawlRecompiled\Play\UserData\NAND\title\00010000\52534245\data`) into the SD folder. Without a save,
  the build stops at step 1 and says so.

- `game/` is `C:\Users\SOgaz\brawl3ds-work\Disc\DATA` copied as a folder named `game`. It only has to be
  copied once; later builds only change the CIA (and sometimes `blobs/`).
- The rest comes from `C:\Users\SOgaz\brawl3ds-work\release-headless\sd\3ds\brawl3ds\`.

## Running it

1. Install `brawl3ds-headless.cia` with FBI.
2. Launch **Brawl3DS headless** from the HOME Menu. (Not from the Homebrew Launcher: a .3dsx only gets the
   launcher's memory, and this needs the 178 MB mode.)
3. Leave it alone and watch the bottom screen for the step and the estimate of time left. It closes
   itself when the demo match ends, or press START to stop early.
4. Send back these files from `SD:/3ds/brawl3ds/`:
   - `headless.log` (the timing results)
   - `crash.txt` if it exists
   - `stderr.txt` and `stdout.txt` (the runtime's own log)

## Reading headless.log

Each line covers 60 frames: average frame time, the time spent in the game itself, the worst frame, and
an fps estimate. The summary at the end gives the whole match. 16.7 ms per frame is 60 fps; 33.3 ms is 30 fps.
This build only runs the game's logic (no drawing), so it is the CPU cost the real port starts from.
