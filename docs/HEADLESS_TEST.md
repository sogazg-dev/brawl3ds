# Headless timing test (New 3DS)

A build of Brawl with **no graphics or sound output**. It boots the real game code, sits on the title
screen until Brawl starts its CPU demo match (Bowser, Olimar, Zelda and Mario on Smashville), measures
30 seconds of that match, and writes how long every frame took. The top screen shows instructions; the
bottom screen shows which step is running.

**This is a diagnostic tool, not the game.** Most of its running time is Brawl's own idle timers, counted in
frames: the safety screen (~1,435 frames, about 24 s at 60 fps) and the title screen before the attract demo
(~7,855 frames, about 131 s). A player pressing A skips both. Nothing is compiled or converted on the
console. The code is already ARM machine code, and the game files are read as they are.

## Steps (bottom screen: "Step N of 9")

| # | Step | What it is | Typical length |
|---|------|-----------|----------------|
| 1 | Checking game files | `game/` and the save are present | instant |
| 2 | Indexing the game folder | walks the 5,314 disc files on the SD | ~3 s |
| 3 | Reserving Wii memory | MEM1/MEM2/module homes | instant |
| 4 | Starting the runtime | game code, modules, virtual NAND | a few seconds |
| 5 | Safety screen (scStrap) | Brawl's idle timeout | 1,435 frames |
| 6 | Loading boot data + save | scBoot | 44 frames |
| 7 | Title screen idle (scTitle) | waits for the attract demo, then loads it | 7,855 frames |
| 8 | Measuring the VS demo match | scMelee | 1,800 frames |
| 9 | Writing results | headless.log | instant |

The screen shows the frame count against the PC build's count for that step, time in the step, total time,
an estimate of time left at the current speed, the fps, and memory. If a step runs far past its PC frame
count (2× + 600 frames), the build stops with "Stalled" instead of waiting forever. `headless.log` ends
with a time-per-step table.

## What goes on the SD card

```
SD:/3ds/dspfirm.cdc                    (your console's DSP dump: Luma3DS > Rosalina > Miscellaneous)
SD:/3ds/brawl3ds/
    game/            the extracted disc: the sys and files folders (about 7.2 GB)
    blobs/           from release-headless/sd/3ds/brawl3ds
    wii_bootstrap/   from release-headless/sd/3ds/brawl3ds
    dsp_coef.bin     from release-headless/sd/3ds/brawl3ds
    portable.txt     from release-headless/sd/3ds/brawl3ds
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
   itself after 30 seconds of the match, or press START to stop early.
4. Send back these files from `SD:/3ds/brawl3ds/`:
   - `headless.log` (the timing results)
   - `crash.txt` if it exists
   - `stderr.txt` and `stdout.txt` (the runtime's own log)

## Reading headless.log

Each line covers 60 frames: average frame time, the time spent in the game itself, the worst frame, and
an fps estimate. The summary at the end gives the whole match. 16.7 ms per frame is 60 fps; 33.3 ms is 30 fps.
This build only runs the game's logic (no drawing), so it is the CPU cost the real port starts from.
