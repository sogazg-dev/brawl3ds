# Headless timing test (New 3DS)

A build of Brawl with **no graphics or sound output**. It boots the real game code, sits on the title
screen until Brawl starts its CPU demo match (Bowser, Olimar, Zelda and Mario on Smashville), measures
30 seconds of that match, and writes how long every frame took. The screen stays still apart from a
progress bar on the bottom screen.

## What goes on the SD card

```
SD:/3ds/dspfirm.cdc                    (your console's DSP dump: Luma3DS > Rosalina > Miscellaneous)
SD:/3ds/brawl3ds/
    game/            the extracted disc: the sys and files folders (about 7.2 GB)
    blobs/           from release-headless/sd/3ds/brawl3ds
    wii_bootstrap/   from release-headless/sd/3ds/brawl3ds
    dsp_coef.bin     from release-headless/sd/3ds/brawl3ds
    portable.txt     from release-headless/sd/3ds/brawl3ds
SD:/cias/brawl3ds-headless.cia
```

- `game/` is `C:\Users\SOgaz\brawl3ds-work\Disc\DATA` copied as a folder named `game`. It only has to be
  copied once; later builds only change the CIA (and sometimes `blobs/`).
- The rest comes from `C:\Users\SOgaz\brawl3ds-work\release-headless\sd\3ds\brawl3ds\`.

## Running it

1. Install `brawl3ds-headless.cia` with FBI.
2. Launch **Brawl3DS headless** from the HOME Menu. (Not from the Homebrew Launcher: a .3dsx only gets the
   launcher's memory, and this needs the 178 MB mode.)
3. Leave it alone. Boot takes a while; the bottom-screen bar is white while booting and on the title
   screen, then turns green while the match is measured. It closes itself after 30 seconds of the match,
   or press START to stop early.
4. Send back these files from `SD:/3ds/brawl3ds/`:
   - `headless.log` (the timing results)
   - `crash.txt` if it exists
   - `stderr.txt` and `stdout.txt` (the runtime's own log)

## Reading headless.log

Each line covers 60 frames: average frame time, the time spent in the game itself, the worst frame, and
an fps estimate. The summary at the end gives the whole match. 16.7 ms per frame is 60 fps; 33.3 ms is 30 fps.
This build only runs the game's logic (no drawing), so it is the CPU cost the real port starts from.
