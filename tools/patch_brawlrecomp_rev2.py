"""Let Brawl Recompiled build and run from a USA *revision 2* disc (RSBE01, boot.bin byte 7 == 2).

Usage: python -I tools/patch_brawlrecomp_rev2.py <path to BrawlRecompiled checkout>
Run it after build_modules.py has written game/main_translate.dol (see README "Building from source").

Why this is safe: Brawl Recompiled targets revision 1, but its function map comes from the Brawl
decompilation's RSBE01_02 config. A rev 2 disc's main.dol and all 126 .rel modules match that config's
build.sha1 exactly (checked 2026-11-19), its data sections give the same SDA bases as recomp.yml
(0x805A4420 / 0x805A9320), and build_modules.py produces the same module layout. The translator works
from the disc's own code, so per-function byte differences between revisions are picked up automatically.

Changes:
  * wiicompiled/projects/brawl/recomp.yml          pin main_translate.dol's sha256 to this disc's build
  * wiicompiled/runtime/src/game_folder.cpp        accept revision 1 or 2
  * wiicompiled/runtime/src/system_bridge.cpp      report the disc's real revision (2) in low memory
"""
import hashlib
import re
import sys
from pathlib import Path

MARK = "brawl3ds-rev2"


def patch(path: Path, old: str, new: str):
    text = path.read_text(encoding="utf-8")
    if MARK in text:
        print(f"  already patched: {path.name}")
        return
    if text.count(old) != 1:
        sys.exit(f"{path}: expected exactly one occurrence of:\n{old}")
    path.write_text(text.replace(old, new), encoding="utf-8")
    print(f"  patched: {path.name}")


def main(root: Path):
    wc = root / "wiicompiled"
    dol = root / "game" / "main_translate.dol"
    if not dol.is_file():
        sys.exit(f"{dol} is missing: run build_modules.py first")
    boot = (root / "Disc" / "DATA" / "sys" / "boot.bin")
    sha = hashlib.sha256(dol.read_bytes()).hexdigest()

    yml = wc / "projects" / "brawl" / "recomp.yml"
    text = yml.read_text(encoding="utf-8")
    new, n = re.subn(r"(path: \.\./game/main_translate\.dol[^\n]*\n\s*sha256: )[0-9a-fA-F]{64}[^\n]*",
                     rf"\g<1>{sha}  # {MARK}: this disc's main_translate.dol", text)
    if n != 1:
        sys.exit(f"{yml}: could not find the main_translate.dol sha256 line")
    yml.write_text(new, encoding="utf-8")
    print(f"  recomp.yml: main_translate.dol sha256 = {sha}")

    patch(wc / "runtime" / "src" / "game_folder.cpp",
          "    if (kRevision >= 0 && revision != kRevision) {",
          f"    // {MARK}: revision 2 has the same layout (main.dol and every module match the decomp's RSBE01_02).\n"
          "    if (kRevision >= 0 && revision != kRevision && revision != 2) {")
    patch(wc / "runtime" / "src" / "system_bridge.cpp",
          '{0x80000004u, 0x30310001u, "Disc maker/id"});  // "01", disc 0, revision 1 (boot.bin)',
          f'{{0x80000004u, 0x30310002u, "Disc maker/id"}});  // "01", disc 0, revision 2 (boot.bin) - {MARK}')
    if boot.is_file() and boot.read_bytes()[7] != 2:
        print(f"  note: {boot} is revision {boot.read_bytes()[7]}, not 2")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(Path(sys.argv[1]))
